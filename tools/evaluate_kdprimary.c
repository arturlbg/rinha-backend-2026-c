#define _POSIX_C_SOURCE 200809L

#include "fastvector.h"
#include "kdprimary.h"

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
    int fraud_counts[6];
    uint64_t search_ns_sum;
    uint64_t nodes_sum;
    uint64_t leaves_sum;
    uint64_t points_sum;
    uint64_t pruned_sum;
    uint64_t *search_ns;
    uint32_t *nodes_visited;
    uint32_t *leaves_visited;
    uint32_t *points_evaluated;
    uint32_t *pruned_branches;
    size_t capacity;
} EvalStats;

static void usage(void) {
    fprintf(stderr,
            "usage: evaluate_kdprimary --tree <kdprimary.bin> --test-data <test-data.json> "
            "[--limit N] [--touch] [--output-errors path]\n");
}

static uint64_t now_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static double ns_to_us(uint64_t ns) {
    return (double)ns / 1000.0;
}

static int read_file(const char *path, char **out, size_t *out_len) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "evaluate_kdprimary: open %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "evaluate_kdprimary: seek %s failed\n", path);
        fclose(file);
        return 1;
    }
    long size = ftell(file);
    if (size < 0) {
        fprintf(stderr, "evaluate_kdprimary: tell %s failed\n", path);
        fclose(file);
        return 1;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "evaluate_kdprimary: seek %s failed\n", path);
        fclose(file);
        return 1;
    }
    char *buffer = (char *)malloc((size_t)size + 1u);
    if (buffer == NULL) {
        fprintf(stderr, "evaluate_kdprimary: out of memory\n");
        fclose(file);
        return 1;
    }
    size_t got = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        fprintf(stderr, "evaluate_kdprimary: short read %s\n", path);
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
    uint32_t *leaves = (uint32_t *)malloc(next * sizeof(uint32_t));
    uint32_t *points = (uint32_t *)malloc(next * sizeof(uint32_t));
    uint32_t *pruned = (uint32_t *)malloc(next * sizeof(uint32_t));
    if (search_ns == NULL || nodes == NULL || leaves == NULL || points == NULL || pruned == NULL) {
        free(search_ns);
        free(nodes);
        free(leaves);
        free(points);
        free(pruned);
        return -1;
    }
    if (stats->capacity > 0) {
        memcpy(search_ns, stats->search_ns, stats->capacity * sizeof(uint64_t));
        memcpy(nodes, stats->nodes_visited, stats->capacity * sizeof(uint32_t));
        memcpy(leaves, stats->leaves_visited, stats->capacity * sizeof(uint32_t));
        memcpy(points, stats->points_evaluated, stats->capacity * sizeof(uint32_t));
        memcpy(pruned, stats->pruned_branches, stats->capacity * sizeof(uint32_t));
    }

    free(stats->search_ns);
    free(stats->nodes_visited);
    free(stats->leaves_visited);
    free(stats->points_evaluated);
    free(stats->pruned_branches);
    stats->search_ns = search_ns;
    stats->nodes_visited = nodes;
    stats->leaves_visited = leaves;
    stats->points_evaluated = points;
    stats->pruned_branches = pruned;
    stats->capacity = next;
    return 0;
}

static void record_confusion(EvalStats *stats, int expected_approved, int approved) {
    if (approved == expected_approved) {
        if (approved) {
            stats->tn++;
        } else {
            stats->tp++;
        }
    } else if (approved) {
        stats->fn++;
    } else {
        stats->fp++;
    }
}

static void write_error_row(FILE *file,
                            int query_index,
                            const char *kind,
                            int expected_approved,
                            uint8_t fraud_count,
                            const KdPrimarySearchResult *result,
                            uint64_t search_ns) {
    if (file == NULL) {
        return;
    }
    fprintf(file,
            "%d,%s,%d,%u,%u,%u,%u,%u,%.3f",
            query_index,
            kind,
            expected_approved,
            fraud_count,
            result->stats.nodes_visited,
            result->stats.leaves_visited,
            result->stats.points_evaluated,
            result->stats.pruned_branches,
            ns_to_us(search_ns));
    for (uint32_t i = 0; i < KDPRIMARY_TOP_K; i++) {
        fprintf(file,
                ",%u,%u,%llu",
                result->top[i].seq,
                result->top[i].fraud,
                (unsigned long long)result->top[i].distance);
    }
    fputc('\n', file);
}

int main(int argc, char **argv) {
    const char *tree_path = NULL;
    const char *test_data_path = NULL;
    const char *errors_path = NULL;
    int limit = 0;
    bool touch = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tree") == 0 && i + 1 < argc) {
            tree_path = argv[++i];
        } else if (strcmp(argv[i], "--test-data") == 0 && i + 1 < argc) {
            test_data_path = argv[++i];
        } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            limit = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--touch") == 0) {
            touch = true;
        } else if (strcmp(argv[i], "--output-errors") == 0 && i + 1 < argc) {
            errors_path = argv[++i];
        } else {
            usage();
            return 2;
        }
    }
    if (tree_path == NULL || test_data_path == NULL) {
        usage();
        return 2;
    }

    char err[256];
    uint64_t load_start = now_ns();
    KdPrimaryIndex index;
    if (kdprimary_open(tree_path, &index, err, sizeof(err)) != 0) {
        fprintf(stderr, "evaluate_kdprimary: %s\n", err);
        return 1;
    }
    uint64_t load_elapsed = now_ns() - load_start;
    uint64_t touch_elapsed = 0;
    uint64_t touch_sum = 0;
    if (touch) {
        uint64_t touch_start = now_ns();
        touch_sum = kdprimary_touch_pages(&index);
        touch_elapsed = now_ns() - touch_start;
    }

    char *data = NULL;
    size_t data_len = 0;
    if (read_file(test_data_path, &data, &data_len) != 0) {
        kdprimary_close(&index);
        return 1;
    }

    FILE *errors_file = NULL;
    if (errors_path != NULL) {
        errors_file = fopen(errors_path, "wb");
        if (errors_file == NULL) {
            fprintf(stderr, "evaluate_kdprimary: open %s: %s\n", errors_path, strerror(errno));
            free(data);
            kdprimary_close(&index);
            return 1;
        }
        fprintf(errors_file,
                "query_index,kind,expected_approved,fraud_count,nodes_visited,leaves_visited,points_evaluated,pruned_branches,search_us");
        for (uint32_t i = 0; i < KDPRIMARY_TOP_K; i++) {
            fprintf(errors_file, ",top%u_seq,top%u_fraud,top%u_distance", i + 1u, i + 1u, i + 1u);
        }
        fputc('\n', errors_file);
    }

    const char *cursor = data;
    const char *end = data + data_len;
    EvalStats stats;
    memset(&stats, 0, sizeof(stats));

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
            fprintf(stderr, "evaluate_kdprimary: failed parsing test data near entry %d\n", stats.total);
            stats.errors++;
            break;
        }
        if (stats_reserve(&stats, (size_t)stats.total + 1u) != 0) {
            fprintf(stderr, "evaluate_kdprimary: out of memory\n");
            stats.errors++;
            break;
        }

        int16_t query[IVF8_INDEX_DIMS];
        if (!fastvector_vectorize(request.data, request.len, query)) {
            stats.errors++;
            stats.search_ns[stats.total] = 0;
            stats.nodes_visited[stats.total] = 0;
            stats.leaves_visited[stats.total] = 0;
            stats.points_evaluated[stats.total] = 0;
            stats.pruned_branches[stats.total] = 0;
            stats.total++;
            continue;
        }

        uint64_t start = now_ns();
        KdPrimarySearchResult result = kdprimary_search_top5(&index, query);
        uint64_t search_ns = now_ns() - start;
        uint8_t fraud_count = result.fraud_count;
        int approved = fraud_count < 3u ? 1 : 0;

        if (fraud_count < 6u) {
            stats.fraud_counts[fraud_count]++;
        }
        record_confusion(&stats, expected_approved, approved);
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
        stats.leaves_visited[stats.total] = result.stats.leaves_visited;
        stats.points_evaluated[stats.total] = result.stats.points_evaluated;
        stats.pruned_branches[stats.total] = result.stats.pruned_branches;
        stats.search_ns_sum += search_ns;
        stats.nodes_sum += result.stats.nodes_visited;
        stats.leaves_sum += result.stats.leaves_visited;
        stats.points_sum += result.stats.points_evaluated;
        stats.pruned_sum += result.stats.pruned_branches;
        stats.total++;
    }

    if (stats.total > 0) {
        qsort(stats.search_ns, (size_t)stats.total, sizeof(uint64_t), compare_u64);
        qsort(stats.nodes_visited, (size_t)stats.total, sizeof(uint32_t), compare_u32);
        qsort(stats.leaves_visited, (size_t)stats.total, sizeof(uint32_t), compare_u32);
        qsort(stats.points_evaluated, (size_t)stats.total, sizeof(uint32_t), compare_u32);
        qsort(stats.pruned_branches, (size_t)stats.total, sizeof(uint32_t), compare_u32);
    }

    printf("evaluated=%d\n", stats.total);
    printf("tree=%s\n", tree_path);
    printf("tree_load_seconds=%.3f\n", (double)load_elapsed / 1000000000.0);
    printf("tree_touch_enabled=%s\n", touch ? "true" : "false");
    if (touch) {
        printf("tree_touch_seconds=%.3f\n", (double)touch_elapsed / 1000000000.0);
        printf("tree_touch_sum=%llu\n", (unsigned long long)touch_sum);
    }
    printf("points=%u\n", index.count);
    printf("nodes=%u\n", index.node_count);
    printf("leaf_size=%u\n", index.leaf_size);
    printf("node_size=%zu\n", sizeof(KdPrimaryNode));
    printf("kdprimary_runtime_memory_bytes=%zu\n", kdprimary_runtime_memory_bytes(&index));
    printf("kdprimary_runtime_memory_mib=%.2f\n", (double)kdprimary_runtime_memory_bytes(&index) / 1048576.0);
    printf("TP=%d\nTN=%d\nFP=%d\nFN=%d\nError=%d\n", stats.tp, stats.tn, stats.fp, stats.fn, stats.errors);
    printf("fraud_count_0=%d\nfraud_count_1=%d\nfraud_count_2=%d\n",
           stats.fraud_counts[0], stats.fraud_counts[1], stats.fraud_counts[2]);
    printf("fraud_count_3=%d\nfraud_count_4=%d\nfraud_count_5=%d\n",
           stats.fraud_counts[3], stats.fraud_counts[4], stats.fraud_counts[5]);
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
        printf("avg_leaves_visited=%.2f\n", (double)stats.leaves_sum / (double)stats.total);
        printf("p50_leaves_visited=%u\n", stats.leaves_visited[p50]);
        printf("p95_leaves_visited=%u\n", stats.leaves_visited[p95]);
        printf("p99_leaves_visited=%u\n", stats.leaves_visited[p99]);
        printf("avg_points_evaluated=%.2f\n", (double)stats.points_sum / (double)stats.total);
        printf("p50_points_evaluated=%u\n", stats.points_evaluated[p50]);
        printf("p95_points_evaluated=%u\n", stats.points_evaluated[p95]);
        printf("p99_points_evaluated=%u\n", stats.points_evaluated[p99]);
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
    free(stats.leaves_visited);
    free(stats.points_evaluated);
    free(stats.pruned_branches);
    free(data);
    kdprimary_close(&index);
    return stats.errors == 0 ? 0 : 1;
}
