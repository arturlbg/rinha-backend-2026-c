#define _POSIX_C_SOURCE 200809L

#include "fastvector.h"
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
            "usage: evaluate_kdprimary2 --tree <kdprimary2.bin> --test-data <test-data.json> "
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
        fprintf(stderr, "evaluate_kdprimary2: open %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "evaluate_kdprimary2: seek %s failed\n", path);
        fclose(file);
        return 1;
    }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "evaluate_kdprimary2: tell/seek %s failed\n", path);
        fclose(file);
        return 1;
    }
    char *buffer = (char *)malloc((size_t)size + 1u);
    if (buffer == NULL) {
        fprintf(stderr, "evaluate_kdprimary2: out of memory\n");
        fclose(file);
        return 1;
    }
    size_t got = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        fprintf(stderr, "evaluate_kdprimary2: short read %s\n", path);
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
    const char *test_data_path = NULL;
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
    KdPrimary2Index index;
    if (kdprimary2_open(tree_path, &index, err, sizeof(err)) != 0) {
        fprintf(stderr, "evaluate_kdprimary2: %s\n", err);
        return 1;
    }
    uint64_t touch_sum = 0;
    if (touch) {
        touch_sum = kdprimary2_touch_pages(&index);
    }

    char *json = NULL;
    size_t json_len = 0;
    if (read_file(test_data_path, &json, &json_len) != 0) {
        kdprimary2_close(&index);
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
            fprintf(stderr, "evaluate_kdprimary2: failed parsing test data near entry %d\n", stats.total);
            stats.errors++;
            break;
        }
        if (stats_reserve(&stats, (size_t)stats.total + 1u) != 0) {
            fprintf(stderr, "evaluate_kdprimary2: out of memory\n");
            stats.errors++;
            break;
        }

        int16_t query[IVF8_INDEX_DIMS];
        if (!fastvector_vectorize(request.data, request.len, query)) {
            stats.errors++;
            stats.total++;
            continue;
        }

        uint64_t start = now_ns();
        KdPrimary2SearchResult result = kdprimary2_search_top5(&index, query);
        uint64_t elapsed = now_ns() - start;

        uint8_t fraud_count = result.fraud_count;
        if (fraud_count > 5u) {
            stats.errors++;
            stats.total++;
            continue;
        }
        int approved = fraud_count < 3u;
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
        stats.fraud_counts[fraud_count]++;
        stats.search_ns[pos] = elapsed;
        stats.nodes_visited[pos] = result.stats.nodes_visited;
        stats.leaves_visited[pos] = result.stats.leaves_visited;
        stats.points_evaluated[pos] = result.stats.points_evaluated;
        stats.pruned_branches[pos] = result.stats.pruned_branches;
        stats.search_ns_sum += elapsed;
        stats.nodes_sum += result.stats.nodes_visited;
        stats.leaves_sum += result.stats.leaves_visited;
        stats.points_sum += result.stats.points_evaluated;
        stats.pruned_sum += result.stats.pruned_branches;
        stats.total++;
    }

    printf("tree=%s\n", tree_path);
    printf("test_data=%s\n", test_data_path);
    printf("touch=%s\n", touch ? "true" : "false");
    printf("touch_sum=%llu\n", (unsigned long long)touch_sum);
    printf("evaluated=%d\n", stats.total);
    printf("TP=%d\nTN=%d\nFP=%d\nFN=%d\nError=%d\n", stats.tp, stats.tn, stats.fp, stats.fn, stats.errors);
    for (int i = 0; i <= 5; i++) {
        printf("fraud_count_%d=%d\n", i, stats.fraud_counts[i]);
    }
    printf("kdprimary2_points=%u\n", index.count);
    printf("kdprimary2_nodes=%u\n", index.node_count);
    printf("kdprimary2_blocks=%u\n", index.block_count);
    printf("kdprimary2_leaf_size=%u\n", index.leaf_size);
    printf("kdprimary2_runtime_memory_bytes=%zu\n", kdprimary2_runtime_memory_bytes(&index));
    printf("kdprimary2_runtime_memory_mib=%.2f\n", (double)kdprimary2_runtime_memory_bytes(&index) / 1048576.0);

    print_u64_stats("search_us", stats.search_ns, stats.total, 1000.0);
    print_u32_stats("nodes_visited", stats.nodes_visited, stats.total);
    print_u32_stats("leaves_visited", stats.leaves_visited, stats.total);
    print_u32_stats("points_evaluated", stats.points_evaluated, stats.total);
    print_u32_stats("pruned_branches", stats.pruned_branches, stats.total);

    free(stats.search_ns);
    free(stats.nodes_visited);
    free(stats.leaves_visited);
    free(stats.points_evaluated);
    free(stats.pruned_branches);
    free(json);
    kdprimary2_close(&index);
    return stats.errors == 0 ? 0 : 1;
}
