#define _POSIX_C_SOURCE 200809L

#include "fastvector.h"
#include "ivf8_index.h"
#include "kdclass3.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
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
    SOURCE_REFERENCE_PERTURB = 4
} DatasetSource;

typedef struct {
    uint64_t emitted;
    uint64_t official;
    uint64_t official_perturb;
    uint64_t edge_perturb;
    uint64_t reference;
    uint64_t reference_perturb;
    uint64_t kdclass3_fallbacks;
    uint64_t expected_mismatches;
} DatasetStats;

static void usage(void) {
    fprintf(stderr,
            "usage: build_rf_dataset --tree <kdclass3.bin> --index <index.bin> "
            "--test-data <test-data.json> --output <rows.csv> [--official-limit N] "
            "[--official-perturb N] [--edge-perturb N] [--ref-samples N] "
            "[--ref-perturb N] [--edge-margin N] [--touch]\n");
}

static int read_file(const char *path, char **out, size_t *out_len) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "build_rf_dataset: open %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "build_rf_dataset: seek %s failed\n", path);
        fclose(file);
        return 1;
    }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "build_rf_dataset: tell/seek %s failed\n", path);
        fclose(file);
        return 1;
    }
    char *buffer = (char *)malloc((size_t)size + 1u);
    if (buffer == NULL) {
        fprintf(stderr, "build_rf_dataset: out of memory\n");
        fclose(file);
        return 1;
    }
    size_t got = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        fprintf(stderr, "build_rf_dataset: short read %s\n", path);
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

static uint64_t mix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30u)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27u)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31u);
}

static int16_t clamp_i16(int32_t value) {
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)value;
}

static void perturb_vector(const int16_t in[IVF8_INDEX_DIMS],
                           int16_t out[IVF8_INDEX_DIMS],
                           uint64_t seed,
                           uint32_t strength) {
    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        uint64_t r = mix64(seed + dim * 0x632be59bd9b4e019ull);
        int32_t delta = (int32_t)(r % (2u * strength + 1u)) - (int32_t)strength;
        out[dim] = clamp_i16((int32_t)in[dim] + delta);
    }
}

static bool copy_reference_vector(const Ivf8Index *index, uint32_t logical_id, int16_t out[IVF8_INDEX_DIMS]) {
    if (index == NULL || logical_id >= index->n) {
        return false;
    }
    uint32_t block = logical_id / IVF8_INDEX_LANES;
    uint32_t lane = logical_id % IVF8_INDEX_LANES;
    if (block >= index->blocks) {
        return false;
    }
    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        size_t pos = ((size_t)block * IVF8_INDEX_DIMS + dim) * IVF8_INDEX_LANES + lane;
        out[dim] = index->block_data[pos];
    }
    return true;
}

static int approved_from_fraud_count(uint8_t fraud_count) {
    return fraud_count < 3u ? 1 : 0;
}

static uint64_t distance_margin(uint64_t a, uint64_t b) {
    return a > b ? a - b : b - a;
}

static void write_header(FILE *out) {
    fprintf(out, "source,row_index,parent_id,expected_approved");
    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        fprintf(out, ",v%u", dim);
    }
    fprintf(out,
            ",kd_fraud_count,kd_approved,kd_fraud_distance3,kd_legit_distance3,kd_margin,"
            "kd_predicted_class,kd_fallback_required,"
            "fraud_nodes,fraud_leaves,fraud_points,fraud_pruned,"
            "legit_nodes,legit_leaves,legit_points,legit_pruned\n");
}

static int emit_row(FILE *out,
                    const KdClass3Index *tree,
                    DatasetStats *stats,
                    DatasetSource source,
                    uint32_t row_index,
                    uint32_t parent_id,
                    int expected_approved,
                    const int16_t vector[IVF8_INDEX_DIMS],
                    uint64_t *out_margin,
                    int *out_kd_approved) {
    KdClass3SearchResult result = kdclass3_search(tree, vector);
    uint64_t margin = distance_margin(result.fraud_distance3, result.legit_distance3);
    uint8_t fraud_count = result.fraud_count;
    int kd_approved = result.fallback_required ? -1 : approved_from_fraud_count(fraud_count);
    int predicted_class = result.fallback_required ? -1 : (kd_approved ? 0 : 1);

    if (result.fallback_required) {
        stats->kdclass3_fallbacks++;
        fraud_count = UINT8_MAX;
    } else if (source == SOURCE_OFFICIAL && kd_approved != expected_approved) {
        stats->expected_mismatches++;
    }

    fprintf(out, "%d,%u,%u,%d", (int)source, row_index, parent_id, expected_approved);
    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        fprintf(out, ",%d", (int)vector[dim]);
    }
    fprintf(out,
            ",%u,%d,%llu,%llu,%llu,%d,%d,%u,%u,%u,%u,%u,%u,%u,%u\n",
            (unsigned)fraud_count,
            kd_approved,
            (unsigned long long)result.fraud_distance3,
            (unsigned long long)result.legit_distance3,
            (unsigned long long)margin,
            predicted_class,
            result.fallback_required ? 1 : 0,
            result.fraud_stats.nodes_visited,
            result.fraud_stats.leaves_visited,
            result.fraud_stats.points_evaluated,
            result.fraud_stats.pruned_branches,
            result.legit_stats.nodes_visited,
            result.legit_stats.leaves_visited,
            result.legit_stats.points_evaluated,
            result.legit_stats.pruned_branches);

    stats->emitted++;
    switch (source) {
    case SOURCE_OFFICIAL:
        stats->official++;
        break;
    case SOURCE_OFFICIAL_PERTURB:
        stats->official_perturb++;
        break;
    case SOURCE_EDGE_PERTURB:
        stats->edge_perturb++;
        break;
    case SOURCE_REFERENCE:
        stats->reference++;
        break;
    case SOURCE_REFERENCE_PERTURB:
        stats->reference_perturb++;
        break;
    }
    if (out_margin != NULL) {
        *out_margin = margin;
    }
    if (out_kd_approved != NULL) {
        *out_kd_approved = kd_approved;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *tree_path = NULL;
    const char *index_path = NULL;
    const char *test_data_path = NULL;
    const char *output_path = NULL;
    uint32_t official_limit = 0;
    uint32_t official_perturb = 0;
    uint32_t edge_perturb = 0;
    uint32_t ref_samples = 0;
    uint32_t ref_perturb = 0;
    uint64_t edge_margin = 2500000ull;
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
            official_limit = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--official-perturb") == 0 && i + 1 < argc) {
            official_perturb = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--edge-perturb") == 0 && i + 1 < argc) {
            edge_perturb = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--ref-samples") == 0 && i + 1 < argc) {
            ref_samples = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--ref-perturb") == 0 && i + 1 < argc) {
            ref_perturb = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--edge-margin") == 0 && i + 1 < argc) {
            edge_margin = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--touch") == 0) {
            touch = true;
        } else {
            usage();
            return 2;
        }
    }

    if (tree_path == NULL || index_path == NULL || test_data_path == NULL || output_path == NULL) {
        usage();
        return 2;
    }

    char err[256];
    KdClass3Index tree;
    if (kdclass3_open(tree_path, &tree, err, sizeof(err)) != 0) {
        fprintf(stderr, "build_rf_dataset: %s\n", err);
        return 1;
    }
    if (touch) {
        uint64_t sink = kdclass3_touch_pages(&tree);
        fprintf(stderr, "build_rf_dataset: touched kdclass3 sink=%llu\n", (unsigned long long)sink);
    }

    Ivf8Index index;
    if (ivf8_index_open(index_path, &index, err, sizeof(err)) != 0) {
        fprintf(stderr, "build_rf_dataset: %s\n", err);
        kdclass3_close(&tree);
        return 1;
    }

    char *json = NULL;
    size_t json_len = 0;
    if (read_file(test_data_path, &json, &json_len) != 0) {
        ivf8_index_close(&index);
        kdclass3_close(&tree);
        return 1;
    }

    FILE *out = fopen(output_path, "wb");
    if (out == NULL) {
        fprintf(stderr, "build_rf_dataset: open %s: %s\n", output_path, strerror(errno));
        free(json);
        ivf8_index_close(&index);
        kdclass3_close(&tree);
        return 1;
    }
    write_header(out);

    DatasetStats stats;
    memset(&stats, 0, sizeof(stats));

    const char *cursor = json;
    const char *end = json + json_len;
    Slice request;
    int expected_approved = 0;
    uint32_t row = 0;
    while (official_limit == 0 || row < official_limit) {
        int next = next_entry(&cursor, end, &request, &expected_approved);
        if (next == 0) {
            break;
        }
        if (next < 0) {
            fprintf(stderr, "build_rf_dataset: malformed test-data near official row %u\n", row);
            fclose(out);
            free(json);
            ivf8_index_close(&index);
            kdclass3_close(&tree);
            return 1;
        }

        int16_t vector[IVF8_INDEX_DIMS];
        if (!fastvector_vectorize(request.data, request.len, vector)) {
            fprintf(stderr, "build_rf_dataset: vectorize failed row %u\n", row);
            fclose(out);
            free(json);
            ivf8_index_close(&index);
            kdclass3_close(&tree);
            return 1;
        }

        uint64_t margin = 0;
        int kd_approved = -1;
        emit_row(out, &tree, &stats, SOURCE_OFFICIAL, row, row, expected_approved, vector, &margin, &kd_approved);

        for (uint32_t j = 0; j < official_perturb; j++) {
            int16_t perturbed[IVF8_INDEX_DIMS];
            perturb_vector(vector, perturbed, mix64(((uint64_t)row << 16u) | j), 192u);
            emit_row(out,
                     &tree,
                     &stats,
                     SOURCE_OFFICIAL_PERTURB,
                     (uint32_t)stats.emitted,
                     row,
                     kd_approved,
                     perturbed,
                     NULL,
                     NULL);
        }
        if (margin <= edge_margin) {
            for (uint32_t j = 0; j < edge_perturb; j++) {
                int16_t perturbed[IVF8_INDEX_DIMS];
                perturb_vector(vector, perturbed, mix64(0xed9e9d5ull ^ ((uint64_t)row << 16u) ^ j), 320u);
                emit_row(out,
                         &tree,
                         &stats,
                         SOURCE_EDGE_PERTURB,
                         (uint32_t)stats.emitted,
                         row,
                         kd_approved,
                         perturbed,
                         NULL,
                         NULL);
            }
        }

        row++;
    }

    for (uint32_t i = 0; i < ref_samples; i++) {
        uint32_t logical_id = (uint32_t)(mix64(0x123456789abcdef0ull + i) % index.n);
        int16_t vector[IVF8_INDEX_DIMS];
        if (!copy_reference_vector(&index, logical_id, vector)) {
            continue;
        }
        int kd_approved = -1;
        emit_row(out,
                 &tree,
                 &stats,
                 SOURCE_REFERENCE,
                 (uint32_t)stats.emitted,
                 logical_id,
                 -1,
                 vector,
                 NULL,
                 &kd_approved);
        for (uint32_t j = 0; j < ref_perturb; j++) {
            int16_t perturbed[IVF8_INDEX_DIMS];
            perturb_vector(vector, perturbed, mix64(0xfeedfacedeadbeefull ^ ((uint64_t)i << 16u) ^ j), 256u);
            emit_row(out,
                     &tree,
                     &stats,
                     SOURCE_REFERENCE_PERTURB,
                     (uint32_t)stats.emitted,
                     logical_id,
                     kd_approved,
                     perturbed,
                     NULL,
                     NULL);
        }
    }

    fclose(out);
    free(json);
    ivf8_index_close(&index);
    kdclass3_close(&tree);

    fprintf(stderr,
            "build_rf_dataset: emitted=%llu official=%llu official_perturb=%llu edge_perturb=%llu "
            "reference=%llu reference_perturb=%llu kdclass3_fallbacks=%llu expected_mismatches=%llu\n",
            (unsigned long long)stats.emitted,
            (unsigned long long)stats.official,
            (unsigned long long)stats.official_perturb,
            (unsigned long long)stats.edge_perturb,
            (unsigned long long)stats.reference,
            (unsigned long long)stats.reference_perturb,
            (unsigned long long)stats.kdclass3_fallbacks,
            (unsigned long long)stats.expected_mismatches);

    return 0;
}
