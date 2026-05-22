#define _POSIX_C_SOURCE 200809L

#include "fastvector.h"
#include "ivf8_index.h"
#include "ivf8_search.h"
#include "kdtree.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    const char *data;
    size_t len;
} Slice;

typedef struct {
    int total;
    int tp;
    int tn;
    int fp;
    int fn;
    int errors;
    int compare_ivf8;
    int fraud_count_mismatches_vs_ivf8;
    int approved_mismatches_vs_ivf8;
    int fraud_counts[6];
    uint64_t search_ns_sum;
    uint64_t nodes_sum;
    uint64_t distances_sum;
    uint64_t pruned_sum;
    uint64_t *search_ns;
    uint32_t *nodes_visited;
    uint32_t *distance_evals;
    uint32_t *pruned_branches;
    size_t capacity;
} EvalStats;

static void usage(void) {
    fprintf(stderr,
            "usage: evaluate_kdtree --index <index.bin> --test-data <test-data.json> "
            "[--tree <tree.bin>] [--limit N] [--mode exact|approx] [--max-visited N] "
            "[--compare-ivf8] [--output-errors path]\n");
}

static uint64_t now_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int read_file(const char *path, char **out, size_t *out_len) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "evaluate_kdtree: open %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "evaluate_kdtree: seek %s failed\n", path);
        fclose(file);
        return 1;
    }
    long size = ftell(file);
    if (size < 0) {
        fprintf(stderr, "evaluate_kdtree: tell %s failed\n", path);
        fclose(file);
        return 1;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "evaluate_kdtree: seek %s failed\n", path);
        fclose(file);
        return 1;
    }
    char *buffer = (char *)malloc((size_t)size + 1u);
    if (buffer == NULL) {
        fprintf(stderr, "evaluate_kdtree: out of memory\n");
        fclose(file);
        return 1;
    }
    size_t got = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        fprintf(stderr, "evaluate_kdtree: short read %s\n", path);
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
            continue;
        }
        if (c == '{') {
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
    const char *p = request_pos + sizeof(request_key) - 1u;
    p = skip_spaces(p, end);
    if (p >= end || *p != ':') {
        return -1;
    }
    p++;
    p = skip_spaces(p, end);
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
    const char *value = expected_pos + sizeof(expected_key) - 1u;
    value = skip_spaces(value, end);
    if (value >= end || *value != ':') {
        return -1;
    }
    value++;
    value = skip_spaces(value, end);
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

static int compare_u64(const void *a, const void *b) {
    uint64_t av = *(const uint64_t *)a;
    uint64_t bv = *(const uint64_t *)b;
    return (av > bv) - (av < bv);
}

static int compare_u32(const void *a, const void *b) {
    uint32_t av = *(const uint32_t *)a;
    uint32_t bv = *(const uint32_t *)b;
    return (av > bv) - (av < bv);
}

static size_t percentile_index(size_t n, double percentile) {
    if (n == 0) {
        return 0;
    }
    size_t idx = (size_t)((percentile / 100.0) * (double)(n - 1u) + 0.5);
    if (idx >= n) {
        idx = n - 1u;
    }
    return idx;
}

static int stats_reserve(EvalStats *stats, size_t needed) {
    if (needed <= stats->capacity) {
        return 0;
    }
    size_t next = stats->capacity == 0 ? 1024u : stats->capacity * 2u;
    while (next < needed) {
        next *= 2u;
    }

    uint64_t *search_ns = (uint64_t *)malloc(next * sizeof(uint64_t));
    uint32_t *nodes = (uint32_t *)malloc(next * sizeof(uint32_t));
    uint32_t *distances = (uint32_t *)malloc(next * sizeof(uint32_t));
    uint32_t *pruned = (uint32_t *)malloc(next * sizeof(uint32_t));
    if (search_ns == NULL || nodes == NULL || distances == NULL || pruned == NULL) {
        free(search_ns);
        free(nodes);
        free(distances);
        free(pruned);
        return -1;
    }
    if (stats->capacity > 0) {
        memcpy(search_ns, stats->search_ns, stats->capacity * sizeof(uint64_t));
        memcpy(nodes, stats->nodes_visited, stats->capacity * sizeof(uint32_t));
        memcpy(distances, stats->distance_evals, stats->capacity * sizeof(uint32_t));
        memcpy(pruned, stats->pruned_branches, stats->capacity * sizeof(uint32_t));
    }

    free(stats->search_ns);
    free(stats->nodes_visited);
    free(stats->distance_evals);
    free(stats->pruned_branches);
    stats->search_ns = search_ns;
    stats->nodes_visited = nodes;
    stats->distance_evals = distances;
    stats->pruned_branches = pruned;
    stats->capacity = next;
    return 0;
}

static double ns_to_us(uint64_t ns) {
    return (double)ns / 1000.0;
}

static void record_confusion(EvalStats *stats, int expected_approved, int approved) {
    if (expected_approved && approved) {
        stats->tp++;
    } else if (!expected_approved && !approved) {
        stats->tn++;
    } else if (!expected_approved && approved) {
        stats->fp++;
    } else {
        stats->fn++;
    }
}

static void write_error_row(FILE *file,
                            int query_index,
                            const char *kind,
                            int expected_approved,
                            uint8_t fraud_count,
                            const KdTreeSearchResult *result,
                            uint64_t search_ns) {
    if (file == NULL) {
        return;
    }
    fprintf(file,
            "%d,%s,%d,%u,%u,%u,%u,%.3f",
            query_index,
            kind,
            expected_approved,
            fraud_count,
            result->stats.nodes_visited,
            result->stats.distance_evaluations,
            result->stats.pruned_branches,
            ns_to_us(search_ns));
    for (uint32_t i = 0; i < KDTREE_TOP_K; i++) {
        fprintf(file,
                ",%u,%u,%llu",
                result->top[i].seq,
                result->top[i].fraud,
                (unsigned long long)result->top[i].distance);
    }
    fputc('\n', file);
}

int main(int argc, char **argv) {
    const char *index_path = NULL;
    const char *test_data_path = NULL;
    const char *tree_path = NULL;
    const char *errors_path = NULL;
    int limit = 0;
    bool compare_ivf8 = false;
    KdTreeSearchConfig kd_cfg = {.max_visited = 0};

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--index") == 0 && i + 1 < argc) {
            index_path = argv[++i];
        } else if (strcmp(argv[i], "--test-data") == 0 && i + 1 < argc) {
            test_data_path = argv[++i];
        } else if (strcmp(argv[i], "--tree") == 0 && i + 1 < argc) {
            tree_path = argv[++i];
        } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            limit = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];
            if (strcmp(mode, "exact") == 0) {
                kd_cfg.max_visited = 0;
            } else if (strcmp(mode, "approx") == 0) {
                if (kd_cfg.max_visited == 0) {
                    kd_cfg.max_visited = 20000u;
                }
            } else {
                usage();
                return 2;
            }
        } else if (strcmp(argv[i], "--max-visited") == 0 && i + 1 < argc) {
            kd_cfg.max_visited = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--compare-ivf8") == 0) {
            compare_ivf8 = true;
        } else if (strcmp(argv[i], "--output-errors") == 0 && i + 1 < argc) {
            errors_path = argv[++i];
        } else {
            usage();
            return 2;
        }
    }
    if (index_path == NULL || test_data_path == NULL) {
        usage();
        return 2;
    }

    char err[256];
    Ivf8Index index;
    if (ivf8_index_open(index_path, &index, err, sizeof(err)) != 0) {
        fprintf(stderr, "evaluate_kdtree: %s\n", err);
        return 1;
    }

    uint64_t load_start = now_ns();
    KdTree tree;
    int tree_status;
    if (tree_path != NULL) {
        tree_status = kdtree_load_nodes_for_ivf8(&tree, &index, tree_path);
    } else {
        tree_status = kdtree_build_from_ivf8(&tree, &index);
    }
    uint64_t load_elapsed = now_ns() - load_start;
    if (tree_status != 0) {
        fprintf(stderr, "evaluate_kdtree: tree %s failed: %s\n",
                tree_path == NULL ? "build" : "load",
                strerror(errno));
        ivf8_index_close(&index);
        return 1;
    }

    char *data = NULL;
    size_t data_len = 0;
    if (read_file(test_data_path, &data, &data_len) != 0) {
        kdtree_free(&tree);
        ivf8_index_close(&index);
        return 1;
    }

    FILE *errors_file = NULL;
    if (errors_path != NULL) {
        errors_file = fopen(errors_path, "wb");
        if (errors_file == NULL) {
            fprintf(stderr, "evaluate_kdtree: open %s: %s\n", errors_path, strerror(errno));
            free(data);
            kdtree_free(&tree);
            ivf8_index_close(&index);
            return 1;
        }
        fprintf(errors_file,
                "query_index,kind,expected_approved,fraud_count,nodes_visited,distance_evaluations,pruned_branches,search_us");
        for (uint32_t i = 0; i < KDTREE_TOP_K; i++) {
            fprintf(errors_file, ",top%u_seq,top%u_fraud,top%u_distance", i + 1u, i + 1u, i + 1u);
        }
        fputc('\n', errors_file);
    }

    const char *cursor = data;
    const char *end = data + data_len;
    EvalStats stats;
    memset(&stats, 0, sizeof(stats));
    stats.compare_ivf8 = compare_ivf8 ? 1 : 0;

    Ivf8SearchConfig ivf8_cfg = {
        .max_candidates = IVF8_SEARCH_DEFAULT_MAX_CANDIDATES,
        .probes = IVF8_SEARCH_DEFAULT_PROBES,
        .impl = IVF8_SEARCH_IMPL_AVX2,
    };
    if (!ivf8_cpu_supports_avx2()) {
        ivf8_cfg.impl = IVF8_SEARCH_IMPL_SCALAR;
    }

    for (;;) {
        if (limit > 0 && stats.total >= limit) {
            break;
        }
        Slice request;
        int expected_approved = 0;
        int next = next_entry(&cursor, end, &request, &expected_approved);
        if (next == 0) {
            break;
        }
        if (next < 0) {
            fprintf(stderr, "evaluate_kdtree: failed parsing test data near entry %d\n", stats.total);
            stats.errors++;
            break;
        }
        if (stats_reserve(&stats, (size_t)stats.total + 1u) != 0) {
            fprintf(stderr, "evaluate_kdtree: out of memory\n");
            stats.errors++;
            break;
        }

        int16_t query[IVF8_INDEX_DIMS];
        if (!fastvector_vectorize(request.data, request.len, query)) {
            stats.errors++;
            stats.search_ns[stats.total] = 0;
            stats.nodes_visited[stats.total] = 0;
            stats.distance_evals[stats.total] = 0;
            stats.pruned_branches[stats.total] = 0;
            stats.total++;
            continue;
        }

        uint64_t start = now_ns();
        KdTreeSearchResult result = kdtree_search_top5(&tree, query, &kd_cfg);
        uint64_t search_ns = now_ns() - start;
        uint8_t fraud_count = result.fraud_count;
        int approved = fraud_count < 3u ? 1 : 0;

        if (fraud_count < 6u) {
            stats.fraud_counts[fraud_count]++;
        }
        record_confusion(&stats, expected_approved, approved);

        if (compare_ivf8) {
            uint8_t ivf8_fraud_count = ivf8_search_fraud_count(&index, query, &ivf8_cfg);
            int ivf8_approved = ivf8_fraud_count < 3u ? 1 : 0;
            if (ivf8_fraud_count != fraud_count) {
                stats.fraud_count_mismatches_vs_ivf8++;
            }
            if (ivf8_approved != approved) {
                stats.approved_mismatches_vs_ivf8++;
            }
        }

        if (expected_approved != approved) {
            write_error_row(errors_file,
                            stats.total,
                            expected_approved ? "FN" : "FP",
                            expected_approved,
                            fraud_count,
                            &result,
                            search_ns);
        }

        stats.search_ns[stats.total] = search_ns;
        stats.nodes_visited[stats.total] = result.stats.nodes_visited;
        stats.distance_evals[stats.total] = result.stats.distance_evaluations;
        stats.pruned_branches[stats.total] = result.stats.pruned_branches;
        stats.search_ns_sum += search_ns;
        stats.nodes_sum += result.stats.nodes_visited;
        stats.distances_sum += result.stats.distance_evaluations;
        stats.pruned_sum += result.stats.pruned_branches;
        stats.total++;
    }

    if (stats.total > 0) {
        qsort(stats.search_ns, (size_t)stats.total, sizeof(uint64_t), compare_u64);
        qsort(stats.nodes_visited, (size_t)stats.total, sizeof(uint32_t), compare_u32);
        qsort(stats.distance_evals, (size_t)stats.total, sizeof(uint32_t), compare_u32);
        qsort(stats.pruned_branches, (size_t)stats.total, sizeof(uint32_t), compare_u32);
    }

    printf("evaluated=%d\n", stats.total);
    printf("mode=%s\n", kd_cfg.max_visited == 0 ? "exact" : "approx");
    printf("max_visited=%u\n", kd_cfg.max_visited);
    printf("tree_source=%s\n", tree_path == NULL ? "built" : "loaded");
    printf("tree_load_or_build_seconds=%.3f\n", (double)load_elapsed / 1000000000.0);
    printf("source_records=%u\n", kdtree_count_ivf8_records(&index));
    printf("nodes=%u\n", tree.node_count);
    printf("node_size=%zu\n", sizeof(KdTreeNode));
    printf("kdtree_runtime_memory_bytes=%zu\n", kdtree_runtime_memory_bytes(&tree));
    printf("kdtree_runtime_memory_mib=%.2f\n", (double)kdtree_runtime_memory_bytes(&tree) / 1048576.0);
    printf("ivf8_mmap_bytes=%zu\n", index.file_size);
    printf("ivf8_plus_kdtree_mib=%.2f\n",
           ((double)index.file_size + (double)kdtree_runtime_memory_bytes(&tree)) / 1048576.0);
    printf("TP=%d\nTN=%d\nFP=%d\nFN=%d\nError=%d\n", stats.tp, stats.tn, stats.fp, stats.fn, stats.errors);
    printf("fraud_count_0=%d\nfraud_count_1=%d\nfraud_count_2=%d\n",
           stats.fraud_counts[0], stats.fraud_counts[1], stats.fraud_counts[2]);
    printf("fraud_count_3=%d\nfraud_count_4=%d\nfraud_count_5=%d\n",
           stats.fraud_counts[3], stats.fraud_counts[4], stats.fraud_counts[5]);
    if (compare_ivf8) {
        printf("fraud_count_mismatches_vs_ivf8=%d\n", stats.fraud_count_mismatches_vs_ivf8);
        printf("approved_mismatches_vs_ivf8=%d\n", stats.approved_mismatches_vs_ivf8);
        printf("ivf8_compare_impl=%s\n", ivf8_search_impl_name(ivf8_cfg.impl));
    }
    if (stats.total > 0) {
        size_t p50 = percentile_index((size_t)stats.total, 50.0);
        size_t p95 = percentile_index((size_t)stats.total, 95.0);
        size_t p99 = percentile_index((size_t)stats.total, 99.0);
        printf("avg_search_us=%.3f\n", ns_to_us(stats.search_ns_sum / (uint64_t)stats.total));
        printf("p50_search_us=%.3f\n", ns_to_us(stats.search_ns[p50]));
        printf("p95_search_us=%.3f\n", ns_to_us(stats.search_ns[p95]));
        printf("p99_search_us=%.3f\n", ns_to_us(stats.search_ns[p99]));
        printf("avg_nodes_visited=%.2f\n", (double)stats.nodes_sum / (double)stats.total);
        printf("p50_nodes_visited=%u\n", stats.nodes_visited[p50]);
        printf("p95_nodes_visited=%u\n", stats.nodes_visited[p95]);
        printf("p99_nodes_visited=%u\n", stats.nodes_visited[p99]);
        printf("avg_distance_evaluations=%.2f\n", (double)stats.distances_sum / (double)stats.total);
        printf("p50_distance_evaluations=%u\n", stats.distance_evals[p50]);
        printf("p95_distance_evaluations=%u\n", stats.distance_evals[p95]);
        printf("p99_distance_evaluations=%u\n", stats.distance_evals[p99]);
        printf("avg_pruned_branches=%.2f\n", (double)stats.pruned_sum / (double)stats.total);
        printf("p50_pruned_branches=%u\n", stats.pruned_branches[p50]);
        printf("p95_pruned_branches=%u\n", stats.pruned_branches[p95]);
        printf("p99_pruned_branches=%u\n", stats.pruned_branches[p99]);
    }

    if (errors_file != NULL) {
        fclose(errors_file);
    }
    free(stats.search_ns);
    free(stats.nodes_visited);
    free(stats.distance_evals);
    free(stats.pruned_branches);
    free(data);
    kdtree_free(&tree);
    ivf8_index_close(&index);
    return stats.errors == 0 ? 0 : 1;
}
