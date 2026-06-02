#define _POSIX_C_SOURCE 200809L

#include "fastvector.h"
#include "ivf8_index.h"
#include "ivf8_search.h"
#include "kdprimary2.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *data;
    size_t len;
} Slice;

typedef enum {
    SOURCE_OFFICIAL = 0,
    SOURCE_OFFICIAL_PERTURB = 1,
    SOURCE_EDGE_PERTURB = 2,
    SOURCE_REFERENCE = 3,
    SOURCE_REFERENCE_PERTURB = 4,
} DatasetSource;

static void usage(void) {
    fprintf(stderr,
            "usage: build_model_dataset --tree <kdprimary2.bin> --index <index.bin> "
            "--test-data <test-data.json> --output <rows.csv> [--official-limit N] "
            "[--ref-samples N] [--official-perturb N] [--edge-perturb N] "
            "[--ref-perturb N] [--touch]\n");
}

static int read_file(const char *path, char **out, size_t *out_len) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "build_model_dataset: open %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 1;
    }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 1;
    }
    char *buffer = (char *)malloc((size_t)size + 1u);
    if (buffer == NULL) {
        fclose(file);
        return 1;
    }
    size_t got = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        free(buffer);
        return 1;
    }
    buffer[got] = '\0';
    *out = buffer;
    *out_len = got;
    return 0;
}

static const char *find_bytes(const char *haystack, size_t haystack_len, const char *needle, size_t needle_len) {
    if (needle_len == 0 || haystack_len < needle_len) {
        return NULL;
    }
    for (size_t i = 0; i + needle_len <= haystack_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return haystack + i;
        }
    }
    return NULL;
}

static const char *skip_spaces(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) {
        p++;
    }
    return p;
}

static const char *matching_object_end(const char *start, const char *end) {
    int depth = 0;
    int in_string = 0;
    for (const char *p = start; p < end; p++) {
        char c = *p;
        if (in_string) {
            if (c == '\\') {
                p++;
                continue;
            }
            if (c == '"') {
                in_string = 0;
            }
            continue;
        }
        if (c == '"') {
            in_string = 1;
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) {
                return p + 1;
            }
        }
    }
    return NULL;
}

static int next_entry(const char **cursor, const char *end, Slice *request, int *expected_approved) {
    static const char request_key[] = "\"request\"";
    static const char expected_key[] = "\"expected_approved\"";

    const char *request_pos = find_bytes(*cursor, (size_t)(end - *cursor), request_key, sizeof(request_key) - 1u);
    if (request_pos == NULL) {
        return 0;
    }
    const char *p = skip_spaces(request_pos + sizeof(request_key) - 1u, end);
    if (p >= end || *p != ':') {
        return -1;
    }
    p = skip_spaces(p + 1, end);
    if (p >= end || *p != '{') {
        return -1;
    }
    const char *request_end = matching_object_end(p, end);
    if (request_end == NULL) {
        return -1;
    }

    const char *expected_pos = find_bytes(request_end, (size_t)(end - request_end),
                                          expected_key, sizeof(expected_key) - 1u);
    if (expected_pos == NULL) {
        return -1;
    }
    const char *value = skip_spaces(expected_pos + sizeof(expected_key) - 1u, end);
    if (value >= end || *value != ':') {
        return -1;
    }
    value = skip_spaces(value + 1, end);
    if ((size_t)(end - value) >= 4u && memcmp(value, "true", 4) == 0) {
        *expected_approved = 1;
    } else if ((size_t)(end - value) >= 5u && memcmp(value, "false", 5) == 0) {
        *expected_approved = 0;
    } else {
        return -1;
    }

    request->data = p;
    request->len = (size_t)(request_end - p);
    *cursor = request_end;
    return 1;
}

static int approved_from_count(uint8_t fraud_count) {
    return fraud_count < 3u ? 1 : 0;
}

static uint64_t mix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

static int16_t clamp_i16(int32_t value) {
    if (value < -32768) {
        return -32768;
    }
    if (value > 32767) {
        return 32767;
    }
    return (int16_t)value;
}

static void perturb_vector(const int16_t src[FASTVECTOR_DIMENSIONS],
                           int16_t dst[FASTVECTOR_DIMENSIONS],
                           uint64_t seed,
                           int scale) {
    for (uint32_t dim = 0; dim < FASTVECTOR_DIMENSIONS; dim++) {
        uint64_t r = mix64(seed + dim * 0x9e3779b97f4a7c15ull);
        int32_t delta = (int32_t)((r % 17u) - 8u) * scale;
        dst[dim] = clamp_i16((int32_t)src[dim] + delta);
    }
}

static int copy_reference_vector(const Ivf8Index *index, uint32_t logical_id, int16_t out[FASTVECTOR_DIMENSIONS]) {
    if (index == NULL || logical_id >= index->n) {
        return -1;
    }
    uint32_t remaining = logical_id;
    uint32_t cluster = 0;
    for (; cluster < index->k; cluster++) {
        uint32_t count = index->counts[cluster];
        if (remaining < count) {
            break;
        }
        remaining -= count;
    }
    if (cluster >= index->k) {
        return -1;
    }
    uint32_t block = index->offsets[cluster] + remaining / IVF8_INDEX_LANES;
    uint32_t lane = remaining % IVF8_INDEX_LANES;
    uint32_t block_base = block * IVF8_INDEX_DIMS * IVF8_INDEX_LANES;
    for (uint32_t dim = 0; dim < FASTVECTOR_DIMENSIONS; dim++) {
        out[dim] = index->block_data[block_base + dim * IVF8_INDEX_LANES + lane];
    }
    return 0;
}

static void write_header(FILE *out) {
    fprintf(out, "source,row_index,parent_id,expected_approved");
    for (uint32_t dim = 0; dim < FASTVECTOR_DIMENSIONS; dim++) {
        fprintf(out, ",v%u", dim);
    }
    fprintf(out, ",kd_fraud_count,kd_approved");
    for (uint32_t i = 0; i < KDPRIMARY2_TOP_K; i++) {
        fprintf(out, ",kd_d%u", i);
    }
    for (uint32_t i = 0; i < KDPRIMARY2_TOP_K; i++) {
        fprintf(out, ",kd_l%u", i);
    }
    fprintf(out, ",kd_nodes,kd_leaves,kd_points,kd_pruned");
    fprintf(out, ",ivf8_fraud_count,ivf8_approved");
    for (uint32_t i = 0; i < IVF8_SEARCH_TOP_K; i++) {
        fprintf(out, ",ivf8_d%u", i);
    }
    for (uint32_t i = 0; i < IVF8_SEARCH_TOP_K; i++) {
        fprintf(out, ",ivf8_l%u", i);
    }
    fprintf(out, ",ivf8_best,ivf8_worst,ivf8_spread,ivf8_gap10,ivf8_gap21,ivf8_gap32,ivf8_gap43");
    fprintf(out, ",ivf8_candidates,ivf8_clusters,ivf8_largest_cluster\n");
}

static uint8_t emit_row(FILE *out,
                        DatasetSource source,
                        int row_index,
                        int parent_id,
                        int expected_approved,
                        const int16_t vector[FASTVECTOR_DIMENSIONS],
                        const KdPrimary2Index *tree,
                        const Ivf8Index *index,
                        const Ivf8SearchConfig *ivf8_cfg) {
    KdPrimary2SearchResult kd = kdprimary2_search_top5(tree, vector);
    Ivf8SearchTraceResult ivf8 = ivf8_search_trace(index, vector, ivf8_cfg);
    int kd_approved = approved_from_count(kd.fraud_count);
    int ivf8_approved = approved_from_count(ivf8.result.fraud_count);

    fprintf(out, "%u,%d,%d,%d", (unsigned)source, row_index, parent_id, expected_approved);
    for (uint32_t dim = 0; dim < FASTVECTOR_DIMENSIONS; dim++) {
        fprintf(out, ",%d", (int)vector[dim]);
    }
    fprintf(out, ",%u,%d", kd.fraud_count, kd_approved);
    for (uint32_t i = 0; i < KDPRIMARY2_TOP_K; i++) {
        fprintf(out, ",%llu", (unsigned long long)kd.top[i].distance);
    }
    for (uint32_t i = 0; i < KDPRIMARY2_TOP_K; i++) {
        fprintf(out, ",%u", kd.top[i].fraud != 0 ? 1u : 0u);
    }
    fprintf(out, ",%u,%u,%u,%u",
            kd.stats.nodes_visited,
            kd.stats.leaves_visited,
            kd.stats.points_evaluated,
            kd.stats.pruned_branches);

    uint64_t best = ivf8.top[0].distance;
    uint64_t worst = ivf8.top[IVF8_SEARCH_TOP_K - 1u].distance;
    uint64_t spread = worst >= best ? worst - best : 0u;
    fprintf(out, ",%u,%d", ivf8.result.fraud_count, ivf8_approved);
    for (uint32_t i = 0; i < IVF8_SEARCH_TOP_K; i++) {
        fprintf(out, ",%llu", (unsigned long long)ivf8.top[i].distance);
    }
    for (uint32_t i = 0; i < IVF8_SEARCH_TOP_K; i++) {
        fprintf(out, ",%u", ivf8.top[i].fraud != 0 ? 1u : 0u);
    }
    fprintf(out, ",%llu,%llu,%llu",
            (unsigned long long)best,
            (unsigned long long)worst,
            (unsigned long long)spread);
    for (uint32_t i = 1; i < IVF8_SEARCH_TOP_K; i++) {
        uint64_t gap = ivf8.top[i].distance >= ivf8.top[i - 1u].distance
                           ? ivf8.top[i].distance - ivf8.top[i - 1u].distance
                           : 0u;
        fprintf(out, ",%llu", (unsigned long long)gap);
    }
    fprintf(out, ",%u,%u,%u\n",
            ivf8.result.stats.candidates_scanned,
            ivf8.result.stats.clusters_scanned,
            ivf8.result.stats.largest_scanned_cluster_candidates);
    return kd.fraud_count;
}

int main(int argc, char **argv) {
    const char *tree_path = NULL;
    const char *index_path = NULL;
    const char *test_data_path = NULL;
    const char *output_path = NULL;
    int official_limit = 0;
    int ref_samples = 0;
    int official_perturb = 0;
    int edge_perturb = 0;
    int ref_perturb = 0;
    bool touch = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tree") == 0 && i + 1 < argc) {
            tree_path = argv[++i];
        } else if (strcmp(argv[i], "--index") == 0 && i + 1 < argc) {
            index_path = argv[++i];
        } else if (strcmp(argv[i], "--test-data") == 0 && i + 1 < argc) {
            test_data_path = argv[++i];
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (strcmp(argv[i], "--official-limit") == 0 && i + 1 < argc) {
            official_limit = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--ref-samples") == 0 && i + 1 < argc) {
            ref_samples = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--official-perturb") == 0 && i + 1 < argc) {
            official_perturb = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--edge-perturb") == 0 && i + 1 < argc) {
            edge_perturb = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--ref-perturb") == 0 && i + 1 < argc) {
            ref_perturb = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--touch") == 0) {
            touch = true;
        } else {
            usage();
            return 1;
        }
    }
    if (tree_path == NULL || index_path == NULL || test_data_path == NULL || output_path == NULL) {
        usage();
        return 1;
    }

    char err[256];
    KdPrimary2Index tree;
    memset(&tree, 0, sizeof(tree));
    tree.fd = -1;
    if (kdprimary2_open(tree_path, &tree, err, sizeof(err)) != 0) {
        fprintf(stderr, "build_model_dataset: %s\n", err);
        return 1;
    }
    if (touch) {
        (void)kdprimary2_touch_pages(&tree);
    }

    Ivf8Index index;
    memset(&index, 0, sizeof(index));
    index.fd = -1;
    if (ivf8_index_open(index_path, &index, err, sizeof(err)) != 0) {
        fprintf(stderr, "build_model_dataset: %s\n", err);
        kdprimary2_close(&tree);
        return 1;
    }
    Ivf8SearchConfig ivf8_cfg = {
        .max_candidates = IVF8_SEARCH_DEFAULT_MAX_CANDIDATES,
        .probes = IVF8_SEARCH_DEFAULT_PROBES,
        .impl = ivf8_cpu_supports_avx2() ? IVF8_SEARCH_IMPL_AVX2 : IVF8_SEARCH_IMPL_SCALAR,
    };

    char *data = NULL;
    size_t data_len = 0;
    if (read_file(test_data_path, &data, &data_len) != 0) {
        ivf8_index_close(&index);
        kdprimary2_close(&tree);
        return 1;
    }

    FILE *out = fopen(output_path, "wb");
    if (out == NULL) {
        fprintf(stderr, "build_model_dataset: open %s: %s\n", output_path, strerror(errno));
        free(data);
        ivf8_index_close(&index);
        kdprimary2_close(&tree);
        return 1;
    }
    write_header(out);

    const char *cursor = data;
    const char *end = data + data_len;
    int official_rows = 0;
    int parse_errors = 0;
    int emitted = 0;
    int official_emitted = 0;
    int official_perturbed = 0;
    int edge_perturbed = 0;
    int ref_emitted = 0;
    int ref_perturbed = 0;

    for (;;) {
        Slice request;
        int expected_approved = 0;
        int next = next_entry(&cursor, end, &request, &expected_approved);
        if (next == 0) {
            break;
        }
        if (next < 0) {
            parse_errors++;
            break;
        }
        if (official_limit > 0 && official_rows >= official_limit) {
            break;
        }

        int16_t vector[FASTVECTOR_DIMENSIONS];
        if (!fastvector_vectorize(request.data, request.len, vector)) {
            parse_errors++;
            official_rows++;
            continue;
        }
        uint8_t kd_fc = emit_row(out, SOURCE_OFFICIAL, emitted++, official_rows, expected_approved,
                                 vector, &tree, &index, &ivf8_cfg);
        official_rows++;
        official_emitted++;

        for (int p = 0; p < official_perturb; p++) {
            int16_t perturbed[FASTVECTOR_DIMENSIONS];
            perturb_vector(vector, perturbed, 0x100000001b3ull + (uint64_t)official_rows * 97u + (uint64_t)p, 64);
            uint8_t pert_fc = emit_row(out, SOURCE_OFFICIAL_PERTURB, emitted++, official_rows - 1,
                                       approved_from_count(kd_fc), perturbed, &tree, &index, &ivf8_cfg);
            (void)pert_fc;
            official_perturbed++;
        }

        if (kd_fc >= 1u && kd_fc <= 4u) {
            for (int p = 0; p < edge_perturb; p++) {
                int16_t perturbed[FASTVECTOR_DIMENSIONS];
                perturb_vector(vector, perturbed, 0x9e3779b97f4a7c15ull + (uint64_t)official_rows * 131u + (uint64_t)p, 96);
                uint8_t pert_fc = emit_row(out, SOURCE_EDGE_PERTURB, emitted++, official_rows - 1,
                                           approved_from_count(kd_fc), perturbed, &tree, &index, &ivf8_cfg);
                (void)pert_fc;
                edge_perturbed++;
            }
        }
    }

    for (int i = 0; i < ref_samples; i++) {
        uint32_t logical_id = (uint32_t)(mix64((uint64_t)i * 0x5851f42d4c957f2dull + 0x14057b7ef767814full) % index.n);
        int16_t vector[FASTVECTOR_DIMENSIONS];
        if (copy_reference_vector(&index, logical_id, vector) != 0) {
            continue;
        }
        uint8_t kd_fc = emit_row(out, SOURCE_REFERENCE, emitted++, (int)logical_id,
                                 -1, vector, &tree, &index, &ivf8_cfg);
        (void)kd_fc;
        ref_emitted++;
        for (int p = 0; p < ref_perturb; p++) {
            int16_t perturbed[FASTVECTOR_DIMENSIONS];
            perturb_vector(vector, perturbed, 0xd1342543de82ef95ull + (uint64_t)i * 257u + (uint64_t)p, 48);
            uint8_t pert_fc = emit_row(out, SOURCE_REFERENCE_PERTURB, emitted++, (int)logical_id,
                                       approved_from_count(kd_fc), perturbed, &tree, &index, &ivf8_cfg);
            (void)pert_fc;
            ref_perturbed++;
        }
    }

    fclose(out);
    free(data);
    ivf8_index_close(&index);
    kdprimary2_close(&tree);

    fprintf(stderr,
            "build_model_dataset: emitted=%d official=%d official_perturb=%d edge_perturb=%d "
            "reference=%d reference_perturb=%d parse_errors=%d ivf8_impl=%s\n",
            emitted,
            official_emitted,
            official_perturbed,
            edge_perturbed,
            ref_emitted,
            ref_perturbed,
            parse_errors,
            ivf8_search_impl_name(ivf8_cfg.impl));
    return parse_errors == 0 ? 0 : 1;
}
