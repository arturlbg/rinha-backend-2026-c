#define _POSIX_C_SOURCE 200809L

#include "fastvector.h"
#include "kdclass3.h"
#include "kdprimary2.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
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
    int fallback_count;
    int class_fraud_count;
    int class_legit_count;
    int approved_mismatches_vs_kdprimary2;
    int result_mismatches_vs_baseline;
    uint64_t *fraud_ns;
    uint64_t *legit_ns;
    uint64_t *class_ns;
    uint64_t *effective_ns;
    uint64_t *kdprimary2_ns;
    uint32_t *fraud_nodes;
    uint32_t *legit_nodes;
    uint32_t *fraud_leaves;
    uint32_t *legit_leaves;
    uint32_t *fraud_points;
    uint32_t *legit_points;
    uint32_t *fraud_pruned;
    uint32_t *legit_pruned;
    size_t capacity;
} EvalStats;

static void usage(void) {
    fprintf(stderr,
            "usage: evaluate_kdclass3 --tree <kdclass3.bin> --kdprimary2 <kdprimary2.bin> "
            "--test-data <test-data.json> [--impl baseline|simd_full] "
            "[--limit N] [--touch]\n");
}

static uint64_t now_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int read_file(const char *path, char **out, size_t *out_len) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "evaluate_kdclass3: open %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "evaluate_kdclass3: seek %s failed\n", path);
        fclose(file);
        return 1;
    }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "evaluate_kdclass3: tell/seek %s failed\n", path);
        fclose(file);
        return 1;
    }
    char *buffer = (char *)malloc((size_t)size + 1u);
    if (buffer == NULL) {
        fprintf(stderr, "evaluate_kdclass3: out of memory\n");
        fclose(file);
        return 1;
    }
    size_t got = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        fprintf(stderr, "evaluate_kdclass3: short read %s\n", path);
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
    return idx < n ? idx : n - 1u;
}

static int resize_u64(uint64_t **ptr, size_t next) {
    uint64_t *tmp = (uint64_t *)realloc(*ptr, next * sizeof(uint64_t));
    if (tmp == NULL) {
        return -1;
    }
    *ptr = tmp;
    return 0;
}

static int resize_u32(uint32_t **ptr, size_t next) {
    uint32_t *tmp = (uint32_t *)realloc(*ptr, next * sizeof(uint32_t));
    if (tmp == NULL) {
        return -1;
    }
    *ptr = tmp;
    return 0;
}

static int stats_reserve(EvalStats *stats, size_t needed) {
    if (needed <= stats->capacity) {
        return 0;
    }
    size_t next = stats->capacity == 0 ? 1024u : stats->capacity * 2u;
    while (next < needed) {
        next *= 2u;
    }
    if (resize_u64(&stats->fraud_ns, next) != 0 ||
        resize_u64(&stats->legit_ns, next) != 0 ||
        resize_u64(&stats->class_ns, next) != 0 ||
        resize_u64(&stats->effective_ns, next) != 0 ||
        resize_u64(&stats->kdprimary2_ns, next) != 0 ||
        resize_u32(&stats->fraud_nodes, next) != 0 ||
        resize_u32(&stats->legit_nodes, next) != 0 ||
        resize_u32(&stats->fraud_leaves, next) != 0 ||
        resize_u32(&stats->legit_leaves, next) != 0 ||
        resize_u32(&stats->fraud_points, next) != 0 ||
        resize_u32(&stats->legit_points, next) != 0 ||
        resize_u32(&stats->fraud_pruned, next) != 0 ||
        resize_u32(&stats->legit_pruned, next) != 0) {
        return -1;
    }
    stats->capacity = next;
    return 0;
}

static void stats_free(EvalStats *stats) {
    free(stats->fraud_ns);
    free(stats->legit_ns);
    free(stats->class_ns);
    free(stats->effective_ns);
    free(stats->kdprimary2_ns);
    free(stats->fraud_nodes);
    free(stats->legit_nodes);
    free(stats->fraud_leaves);
    free(stats->legit_leaves);
    free(stats->fraud_points);
    free(stats->legit_points);
    free(stats->fraud_pruned);
    free(stats->legit_pruned);
}

static void print_u64_stats(const char *name, const uint64_t *values, int count, double divisor) {
    if (count <= 0) {
        printf("avg_%s=0\np50_%s=0\np95_%s=0\np99_%s=0\n", name, name, name, name);
        return;
    }
    uint64_t *copy = (uint64_t *)malloc((size_t)count * sizeof(uint64_t));
    if (copy == NULL) {
        return;
    }
    memcpy(copy, values, (size_t)count * sizeof(uint64_t));
    qsort(copy, (size_t)count, sizeof(uint64_t), compare_u64);
    long double sum = 0;
    for (int i = 0; i < count; i++) {
        sum += (long double)values[i];
    }
    printf("avg_%s=%.3f\n", name, (double)(sum / (long double)count / divisor));
    printf("p50_%s=%.3f\n", name, (double)copy[percentile_index((size_t)count, 50)] / divisor);
    printf("p95_%s=%.3f\n", name, (double)copy[percentile_index((size_t)count, 95)] / divisor);
    printf("p99_%s=%.3f\n", name, (double)copy[percentile_index((size_t)count, 99)] / divisor);
    free(copy);
}

static void print_u32_stats(const char *name, const uint32_t *values, int count) {
    if (count <= 0) {
        printf("avg_%s=0\np50_%s=0\np95_%s=0\np99_%s=0\n", name, name, name, name);
        return;
    }
    uint32_t *copy = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));
    if (copy == NULL) {
        return;
    }
    memcpy(copy, values, (size_t)count * sizeof(uint32_t));
    qsort(copy, (size_t)count, sizeof(uint32_t), compare_u32);
    long double sum = 0;
    for (int i = 0; i < count; i++) {
        sum += (long double)values[i];
    }
    printf("avg_%s=%.2f\n", name, (double)(sum / (long double)count));
    printf("p50_%s=%u\n", name, copy[percentile_index((size_t)count, 50)]);
    printf("p95_%s=%u\n", name, copy[percentile_index((size_t)count, 95)]);
    printf("p99_%s=%u\n", name, copy[percentile_index((size_t)count, 99)]);
    free(copy);
}

int main(int argc, char **argv) {
    const char *tree_path = NULL;
    const char *kdprimary2_path = NULL;
    const char *test_data_path = NULL;
    int limit = 0;
    bool touch = false;
    KdClass3Impl impl = KDCLASS3_IMPL_BASELINE;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tree") == 0 && i + 1 < argc) {
            tree_path = argv[++i];
        } else if (strcmp(argv[i], "--kdprimary2") == 0 && i + 1 < argc) {
            kdprimary2_path = argv[++i];
        } else if (strcmp(argv[i], "--test-data") == 0 && i + 1 < argc) {
            test_data_path = argv[++i];
        } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            limit = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--touch") == 0) {
            touch = true;
        } else if (strcmp(argv[i], "--impl") == 0 && i + 1 < argc) {
            if (!kdclass3_impl_from_string(argv[++i], &impl)) {
                usage();
                return 2;
            }
        } else {
            usage();
            return 2;
        }
    }

    if (tree_path == NULL || kdprimary2_path == NULL || test_data_path == NULL) {
        usage();
        return 2;
    }
    if (impl == KDCLASS3_IMPL_SIMD_FULL && !ivf8_cpu_supports_avx2()) {
        fprintf(stderr, "evaluate_kdclass3: simd_full requires AVX2\n");
        return 2;
    }

    char err[256];
    KdClass3Index index;
    if (kdclass3_open(tree_path, &index, err, sizeof(err)) != 0) {
        fprintf(stderr, "evaluate_kdclass3: %s\n", err);
        return 1;
    }
    KdPrimary2Index kdprimary2;
    if (kdprimary2_open(kdprimary2_path, &kdprimary2, err, sizeof(err)) != 0) {
        fprintf(stderr, "evaluate_kdclass3: %s\n", err);
        kdclass3_close(&index);
        return 1;
    }

    uint64_t touch_sum = 0;
    uint64_t kd_touch_sum = 0;
    if (touch) {
        touch_sum = kdclass3_touch_pages(&index);
        kd_touch_sum = kdprimary2_touch_pages(&kdprimary2);
    }

    char *json = NULL;
    size_t json_len = 0;
    if (read_file(test_data_path, &json, &json_len) != 0) {
        kdprimary2_close(&kdprimary2);
        kdclass3_close(&index);
        return 1;
    }

    EvalStats stats;
    memset(&stats, 0, sizeof(stats));
    const char *cursor = json;
    const char *end = json + json_len;

    while (limit <= 0 || stats.total < limit) {
        Slice request;
        int expected_approved = 0;
        int next = next_entry(&cursor, end, &request, &expected_approved);
        if (next == 0) {
            break;
        }
        if (next < 0) {
            fprintf(stderr, "evaluate_kdclass3: failed parsing test data near entry %d\n", stats.total);
            stats.errors++;
            break;
        }
        if (stats_reserve(&stats, (size_t)stats.total + 1u) != 0) {
            fprintf(stderr, "evaluate_kdclass3: out of memory\n");
            stats.errors++;
            break;
        }

        int16_t query[IVF8_INDEX_DIMS];
        if (!fastvector_vectorize(request.data, request.len, query)) {
            stats.errors++;
            stats.total++;
            continue;
        }

        uint64_t class_start = now_ns();
        uint64_t fraud_start = class_start;
        KdClass3ClassSearchResult fraud = kdclass3_search_class3(&index.fraud, query);
        (void)fraud;
        uint64_t fraud_elapsed = now_ns() - fraud_start;
        uint64_t legit_start = now_ns();
        KdClass3ClassSearchResult legit = kdclass3_search_class3(&index.legit, query);
        (void)legit;
        uint64_t legit_elapsed = now_ns() - legit_start;
        uint64_t class_elapsed = now_ns() - class_start;

        uint64_t optimized_start = now_ns();
        KdClass3SearchResult optimized =
            impl == KDCLASS3_IMPL_SIMD_FULL
                ? kdclass3_search_simd_full(&index, query)
                : kdclass3_search(&index, query);
        uint64_t optimized_elapsed = now_ns() - optimized_start;
        if (impl == KDCLASS3_IMPL_SIMD_FULL) {
            KdClass3SearchResult baseline = kdclass3_search(&index, query);
            if (baseline.fraud_count != optimized.fraud_count ||
                baseline.fallback_required != optimized.fallback_required ||
                baseline.fraud_distance3 != optimized.fraud_distance3 ||
                baseline.legit_distance3 != optimized.legit_distance3) {
                stats.result_mismatches_vs_baseline++;
            }
        }

        bool fallback = optimized.fallback_required;
        uint8_t class_fraud_count = optimized.fraud_count;
        if (!fallback && class_fraud_count >= 3u) {
            stats.class_fraud_count++;
        } else if (!fallback) {
            stats.class_legit_count++;
        } else {
            stats.fallback_count++;
        }

        uint64_t kd_start = now_ns();
        KdPrimary2SearchResult kd = kdprimary2_search_top5(&kdprimary2, query);
        uint64_t kd_elapsed = now_ns() - kd_start;
        uint8_t final_fraud_count = fallback ? kd.fraud_count : class_fraud_count;
        int approved = final_fraud_count < 3u ? 1 : 0;
        int kd_approved = kd.fraud_count < 3u ? 1 : 0;
        if (approved != kd_approved) {
            stats.approved_mismatches_vs_kdprimary2++;
        }

        if (expected_approved == approved) {
            if (approved) {
                stats.tn++;
            } else {
                stats.tp++;
            }
        } else if (approved) {
            stats.fn++;
        } else {
            stats.fp++;
        }

        int pos = stats.total;
        stats.fraud_ns[pos] = fraud_elapsed;
        stats.legit_ns[pos] = legit_elapsed;
        stats.class_ns[pos] = class_elapsed;
        stats.kdprimary2_ns[pos] = kd_elapsed;
        stats.effective_ns[pos] = optimized_elapsed + (fallback ? kd_elapsed : 0u);
        stats.fraud_nodes[pos] = optimized.fraud_stats.nodes_visited;
        stats.legit_nodes[pos] = optimized.legit_stats.nodes_visited;
        stats.fraud_leaves[pos] = optimized.fraud_stats.leaves_visited;
        stats.legit_leaves[pos] = optimized.legit_stats.leaves_visited;
        stats.fraud_points[pos] = optimized.fraud_stats.points_evaluated;
        stats.legit_points[pos] = optimized.legit_stats.points_evaluated;
        stats.fraud_pruned[pos] = optimized.fraud_stats.pruned_branches;
        stats.legit_pruned[pos] = optimized.legit_stats.pruned_branches;
        stats.total++;
    }

    printf("tree=%s\n", tree_path);
    printf("kdprimary2=%s\n", kdprimary2_path);
    printf("test_data=%s\n", test_data_path);
    printf("impl=%s\n", kdclass3_impl_name(impl));
    printf("touch=%s\n", touch ? "true" : "false");
    printf("touch_sum=%llu\n", (unsigned long long)touch_sum);
    printf("kdprimary2_touch_sum=%llu\n", (unsigned long long)kd_touch_sum);
    printf("evaluated=%d\n", stats.total);
    printf("TP=%d\nTN=%d\nFP=%d\nFN=%d\nError=%d\n", stats.tp, stats.tn, stats.fp, stats.fn, stats.errors);
    printf("approved_mismatches_vs_kdprimary2=%d\n", stats.approved_mismatches_vs_kdprimary2);
    printf("result_mismatches_vs_baseline=%d\n", stats.result_mismatches_vs_baseline);
    printf("fallback_count=%d\n", stats.fallback_count);
    printf("fallback_rate=%.6f\n", stats.total > 0 ? (double)stats.fallback_count / (double)stats.total : 0.0);
    printf("class_fraud_decisions=%d\n", stats.class_fraud_count);
    printf("class_legit_decisions=%d\n", stats.class_legit_count);
    printf("kdclass3_fraud_points=%u\n", index.fraud.count);
    printf("kdclass3_legit_points=%u\n", index.legit.count);
    printf("kdclass3_fraud_nodes=%u\n", index.fraud.node_count);
    printf("kdclass3_legit_nodes=%u\n", index.legit.node_count);
    printf("kdclass3_fraud_blocks=%u\n", index.fraud.block_count);
    printf("kdclass3_legit_blocks=%u\n", index.legit.block_count);
    printf("kdclass3_leaf_size=%u\n", index.leaf_size);
    printf("kdclass3_runtime_memory_bytes=%zu\n", kdclass3_runtime_memory_bytes(&index));
    printf("kdclass3_runtime_memory_mib=%.2f\n", (double)kdclass3_runtime_memory_bytes(&index) / 1048576.0);
    printf("kdprimary2_runtime_memory_bytes=%zu\n", kdprimary2_runtime_memory_bytes(&kdprimary2));
    printf("kdprimary2_runtime_memory_mib=%.2f\n", (double)kdprimary2_runtime_memory_bytes(&kdprimary2) / 1048576.0);

    print_u64_stats("kdprimary2_search_us", stats.kdprimary2_ns, stats.total, 1000.0);
    print_u64_stats("kdclass3_fraud_search_us", stats.fraud_ns, stats.total, 1000.0);
    print_u64_stats("kdclass3_legit_search_us", stats.legit_ns, stats.total, 1000.0);
    print_u64_stats("kdclass3_combined_search_us", stats.class_ns, stats.total, 1000.0);
    print_u64_stats("kdclass3_effective_search_us", stats.effective_ns, stats.total, 1000.0);
    print_u32_stats("kdclass3_fraud_nodes_visited", stats.fraud_nodes, stats.total);
    print_u32_stats("kdclass3_legit_nodes_visited", stats.legit_nodes, stats.total);
    print_u32_stats("kdclass3_fraud_leaves_visited", stats.fraud_leaves, stats.total);
    print_u32_stats("kdclass3_legit_leaves_visited", stats.legit_leaves, stats.total);
    print_u32_stats("kdclass3_fraud_points_evaluated", stats.fraud_points, stats.total);
    print_u32_stats("kdclass3_legit_points_evaluated", stats.legit_points, stats.total);
    print_u32_stats("kdclass3_fraud_pruned", stats.fraud_pruned, stats.total);
    print_u32_stats("kdclass3_legit_pruned", stats.legit_pruned, stats.total);

    stats_free(&stats);
    free(json);
    kdprimary2_close(&kdprimary2);
    kdclass3_close(&index);
    return stats.errors == 0 && stats.fp == 0 && stats.fn == 0 &&
                   stats.approved_mismatches_vs_kdprimary2 == 0 &&
                   stats.result_mismatches_vs_baseline == 0
               ? 0
               : 1;
}
