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

static void usage(void) {
    fprintf(stderr,
            "usage: build_gate_dataset --tree <kdprimary2.bin> --test-data <test-data.json> "
            "[--index <index.bin>] [--output <rows.csv>] [--limit N] [--touch]\n");
}

static int read_file(const char *path, char **out, size_t *out_len) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "build_gate_dataset: open %s: %s\n", path, strerror(errno));
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

static void write_header(FILE *out, bool include_ivf8) {
    fprintf(out, "row_index,expected_approved");
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
    if (include_ivf8) {
        fprintf(out, ",ivf8_fraud_count,ivf8_approved");
        for (uint32_t i = 0; i < IVF8_SEARCH_TOP_K; i++) {
            fprintf(out, ",ivf8_d%u", i);
        }
        for (uint32_t i = 0; i < IVF8_SEARCH_TOP_K; i++) {
            fprintf(out, ",ivf8_l%u", i);
        }
        fprintf(out, ",ivf8_best,ivf8_worst,ivf8_spread,ivf8_gap10,ivf8_gap21,ivf8_gap32,ivf8_gap43");
        fprintf(out, ",ivf8_candidates,ivf8_clusters,ivf8_largest_cluster");
    }
    fputc('\n', out);
}

int main(int argc, char **argv) {
    const char *tree_path = NULL;
    const char *index_path = NULL;
    const char *test_data_path = NULL;
    const char *output_path = NULL;
    int limit = 0;
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
        } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            limit = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--touch") == 0) {
            touch = true;
        } else {
            usage();
            return 1;
        }
    }
    if (tree_path == NULL || test_data_path == NULL) {
        usage();
        return 1;
    }

    char err[256];
    KdPrimary2Index tree;
    memset(&tree, 0, sizeof(tree));
    tree.fd = -1;
    if (kdprimary2_open(tree_path, &tree, err, sizeof(err)) != 0) {
        fprintf(stderr, "build_gate_dataset: %s\n", err);
        return 1;
    }
    if (touch) {
        (void)kdprimary2_touch_pages(&tree);
    }

    Ivf8Index index;
    memset(&index, 0, sizeof(index));
    index.fd = -1;
    bool include_ivf8 = index_path != NULL;
    if (include_ivf8 && ivf8_index_open(index_path, &index, err, sizeof(err)) != 0) {
        fprintf(stderr, "build_gate_dataset: %s\n", err);
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

    FILE *out = stdout;
    if (output_path != NULL) {
        out = fopen(output_path, "wb");
        if (out == NULL) {
            fprintf(stderr, "build_gate_dataset: open %s: %s\n", output_path, strerror(errno));
            free(data);
            ivf8_index_close(&index);
            kdprimary2_close(&tree);
            return 1;
        }
    }

    write_header(out, include_ivf8);
    const char *cursor = data;
    const char *end = data + data_len;
    int row = 0;
    int emitted = 0;
    int parse_errors = 0;
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
        if (limit > 0 && emitted >= limit) {
            break;
        }

        int16_t query[FASTVECTOR_DIMENSIONS];
        if (!fastvector_vectorize(request.data, request.len, query)) {
            parse_errors++;
            row++;
            continue;
        }

        KdPrimary2SearchResult kd = kdprimary2_search_top5(&tree, query);
        fprintf(out, "%d,%d", row, expected_approved);
        for (uint32_t dim = 0; dim < FASTVECTOR_DIMENSIONS; dim++) {
            fprintf(out, ",%d", (int)query[dim]);
        }
        fprintf(out, ",%u,%d", kd.fraud_count, approved_from_count(kd.fraud_count));
        for (uint32_t i = 0; i < KDPRIMARY2_TOP_K; i++) {
            fprintf(out, ",%llu", (unsigned long long)kd.top[i].distance);
        }
        for (uint32_t i = 0; i < KDPRIMARY2_TOP_K; i++) {
            fprintf(out, ",%u", kd.top[i].fraud);
        }
        fprintf(out, ",%u,%u,%u,%u",
                kd.stats.nodes_visited,
                kd.stats.leaves_visited,
                kd.stats.points_evaluated,
                kd.stats.pruned_branches);

        if (include_ivf8) {
            Ivf8SearchTraceResult trace = ivf8_search_trace(&index, query, &ivf8_cfg);
            uint64_t best = trace.top[0].distance;
            uint64_t worst = trace.top[IVF8_SEARCH_TOP_K - 1u].distance;
            fprintf(out, ",%u,%d", trace.result.fraud_count, approved_from_count(trace.result.fraud_count));
            for (uint32_t i = 0; i < IVF8_SEARCH_TOP_K; i++) {
                fprintf(out, ",%llu", (unsigned long long)trace.top[i].distance);
            }
            for (uint32_t i = 0; i < IVF8_SEARCH_TOP_K; i++) {
                fprintf(out, ",%u", trace.top[i].fraud);
            }
            fprintf(out, ",%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,%u",
                    (unsigned long long)best,
                    (unsigned long long)worst,
                    (unsigned long long)(worst >= best ? worst - best : 0u),
                    (unsigned long long)(trace.top[1].distance - trace.top[0].distance),
                    (unsigned long long)(trace.top[2].distance - trace.top[1].distance),
                    (unsigned long long)(trace.top[3].distance - trace.top[2].distance),
                    (unsigned long long)(trace.top[4].distance - trace.top[3].distance),
                    trace.result.stats.candidates_scanned,
                    trace.result.stats.clusters_scanned,
                    trace.result.stats.largest_scanned_cluster_candidates);
        }
        fputc('\n', out);
        emitted++;
        row++;
    }

    if (out != stdout) {
        fclose(out);
    }
    fprintf(stderr,
            "build_gate_dataset: emitted=%d parse_errors=%d include_ivf8=%s ivf8_impl=%s\n",
            emitted,
            parse_errors,
            include_ivf8 ? "true" : "false",
            include_ivf8 ? ivf8_search_impl_name(ivf8_cfg.impl) : "none");
    free(data);
    ivf8_index_close(&index);
    kdprimary2_close(&tree);
    return parse_errors == 0 ? 0 : 1;
}
