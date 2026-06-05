#define _POSIX_C_SOURCE 200809L

#include "fastvector.h"
#include "kdclass3_opt.h"

#include <errno.h>
#include <limits.h>
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
    double avg;
    double p50;
    double p95;
    double p99;
} Summary;

typedef struct {
    uint64_t *baseline_ns;
    uint64_t *optimized_ns;
    uint32_t *baseline_nodes;
    uint32_t *optimized_nodes;
    uint32_t *baseline_leaves;
    uint32_t *optimized_leaves;
    uint32_t *baseline_points;
    uint32_t *optimized_points;
    uint32_t *baseline_pruned;
    uint32_t *optimized_pruned;
    uint32_t *optimized_bbox_dims;
    uint32_t *optimized_blocks;
    uint32_t *optimized_checkpoint_pruned;
    uint32_t *optimized_lanes_pruned;
    size_t capacity;
} Samples;

static void usage(void) {
    fprintf(stderr,
            "usage: evaluate_kdclass3_opt --tree <kdclass3.bin> "
            "(--test-data <test-data.json> | --vectors-csv <rows.csv>) "
            "[--variant bbox|checkpoint|combined|simd-bbox|simd-combined|simd-full] "
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
        fprintf(stderr, "evaluate_kdclass3_opt: open %s: %s\n",
                path, strerror(errno));
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

static const char *find_bytes(const char *haystack,
                              size_t haystack_len,
                              const char *needle,
                              size_t needle_len) {
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
    while (p < end &&
           (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) {
        p++;
    }
    return p;
}

static const char *matching_object_end(const char *start, const char *end) {
    int depth = 0;
    int in_string = 0;
    for (const char *p = start; p < end; p++) {
        char c = *p;
        if (in_string != 0) {
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

static int next_entry(const char **cursor,
                      const char *end,
                      Slice *request,
                      int *expected_approved) {
    static const char request_key[] = "\"request\"";
    static const char expected_key[] = "\"expected_approved\"";
    const char *request_pos = find_bytes(
        *cursor, (size_t)(end - *cursor), request_key, sizeof(request_key) - 1u);
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
    const char *expected_pos = find_bytes(
        request_end, (size_t)(end - request_end),
        expected_key, sizeof(expected_key) - 1u);
    if (expected_pos == NULL) {
        return -1;
    }
    const char *value = skip_spaces(
        expected_pos + sizeof(expected_key) - 1u, end);
    if (value >= end || *value != ':') {
        return -1;
    }
    value = skip_spaces(value + 1, end);
    if ((size_t)(end - value) >= 4u && memcmp(value, "true", 4u) == 0) {
        *expected_approved = 1;
    } else if ((size_t)(end - value) >= 5u &&
               memcmp(value, "false", 5u) == 0) {
        *expected_approved = 0;
    } else {
        return -1;
    }
    request->data = p;
    request->len = (size_t)(request_end - p);
    *cursor = request_end;
    return 1;
}

static int next_csv_vector(char **cursor,
                           const char *end,
                           int16_t query[IVF8_INDEX_DIMS],
                           int *expected_approved) {
    while (*cursor < end) {
        char *line = *cursor;
        char *line_end = line;
        while (line_end < end && *line_end != '\n' && *line_end != '\r') {
            line_end++;
        }
        char *next = line_end;
        while (next < end && (*next == '\n' || *next == '\r')) {
            next++;
        }
        *cursor = next;
        if (line == line_end ||
            ((size_t)(line_end - line) >= 6u &&
             memcmp(line, "source", 6u) == 0)) {
            continue;
        }

        char saved = *line_end;
        *line_end = '\0';
        char *p = line;
        long long values[20];
        bool ok = true;
        for (size_t col = 0; col < 20u; col++) {
            char *value_end = NULL;
            errno = 0;
            values[col] = strtoll(p, &value_end, 10);
            if (errno != 0 || value_end == p ||
                (col < 19u && *value_end != ',')) {
                ok = false;
                break;
            }
            p = value_end + (col < 19u ? 1 : 0);
        }
        *line_end = saved;
        if (!ok || (values[19] != 0 && values[19] != 1)) {
            return -1;
        }
        for (size_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
            long long value = values[4u + dim];
            if (value < INT16_MIN || value > INT16_MAX) {
                return -1;
            }
            query[dim] = (int16_t)value;
        }
        *expected_approved = (int)values[19];
        return 1;
    }
    return 0;
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
    size_t idx = (size_t)((percentile / 100.0) * (double)(n - 1u) + 0.5);
    return idx < n ? idx : n - 1u;
}

static Summary summarize_u64(const uint64_t *values, size_t count) {
    Summary summary = {0};
    uint64_t *copy = (uint64_t *)malloc(count * sizeof(uint64_t));
    if (copy == NULL || count == 0) {
        free(copy);
        return summary;
    }
    memcpy(copy, values, count * sizeof(uint64_t));
    qsort(copy, count, sizeof(uint64_t), compare_u64);
    long double sum = 0;
    for (size_t i = 0; i < count; i++) {
        sum += (long double)values[i];
    }
    summary.avg = (double)(sum / (long double)count) / 1000.0;
    summary.p50 = (double)copy[percentile_index(count, 50)] / 1000.0;
    summary.p95 = (double)copy[percentile_index(count, 95)] / 1000.0;
    summary.p99 = (double)copy[percentile_index(count, 99)] / 1000.0;
    free(copy);
    return summary;
}

static void print_summary(const char *name, Summary summary) {
    printf("avg_%s_us=%.3f\n", name, summary.avg);
    printf("p50_%s_us=%.3f\n", name, summary.p50);
    printf("p95_%s_us=%.3f\n", name, summary.p95);
    printf("p99_%s_us=%.3f\n", name, summary.p99);
}

static void print_u32_summary(const char *name,
                              const uint32_t *values,
                              size_t count) {
    uint32_t *copy = (uint32_t *)malloc(count * sizeof(uint32_t));
    if (copy == NULL || count == 0) {
        free(copy);
        return;
    }
    memcpy(copy, values, count * sizeof(uint32_t));
    qsort(copy, count, sizeof(uint32_t), compare_u32);
    long double sum = 0;
    for (size_t i = 0; i < count; i++) {
        sum += values[i];
    }
    printf("avg_%s=%.3f\n", name, (double)(sum / (long double)count));
    printf("p95_%s=%u\n", name, copy[percentile_index(count, 95)]);
    printf("p99_%s=%u\n", name, copy[percentile_index(count, 99)]);
    free(copy);
}

static int resize_u64(uint64_t **ptr, size_t next) {
    uint64_t *value = (uint64_t *)realloc(*ptr, next * sizeof(uint64_t));
    if (value == NULL) {
        return -1;
    }
    *ptr = value;
    return 0;
}

static int resize_u32(uint32_t **ptr, size_t next) {
    uint32_t *value = (uint32_t *)realloc(*ptr, next * sizeof(uint32_t));
    if (value == NULL) {
        return -1;
    }
    *ptr = value;
    return 0;
}

static int samples_reserve(Samples *samples, size_t needed) {
    if (needed <= samples->capacity) {
        return 0;
    }
    size_t next = samples->capacity == 0 ? 1024u : samples->capacity * 2u;
    while (next < needed) {
        next *= 2u;
    }
#define RESIZE_U64(name) if (resize_u64(&samples->name, next) != 0) return -1
#define RESIZE_U32(name) if (resize_u32(&samples->name, next) != 0) return -1
    RESIZE_U64(baseline_ns);
    RESIZE_U64(optimized_ns);
    RESIZE_U32(baseline_nodes);
    RESIZE_U32(optimized_nodes);
    RESIZE_U32(baseline_leaves);
    RESIZE_U32(optimized_leaves);
    RESIZE_U32(baseline_points);
    RESIZE_U32(optimized_points);
    RESIZE_U32(baseline_pruned);
    RESIZE_U32(optimized_pruned);
    RESIZE_U32(optimized_bbox_dims);
    RESIZE_U32(optimized_blocks);
    RESIZE_U32(optimized_checkpoint_pruned);
    RESIZE_U32(optimized_lanes_pruned);
#undef RESIZE_U64
#undef RESIZE_U32
    samples->capacity = next;
    return 0;
}

static void samples_free(Samples *samples) {
    free(samples->baseline_ns);
    free(samples->optimized_ns);
    free(samples->baseline_nodes);
    free(samples->optimized_nodes);
    free(samples->baseline_leaves);
    free(samples->optimized_leaves);
    free(samples->baseline_points);
    free(samples->optimized_points);
    free(samples->baseline_pruned);
    free(samples->optimized_pruned);
    free(samples->optimized_bbox_dims);
    free(samples->optimized_blocks);
    free(samples->optimized_checkpoint_pruned);
    free(samples->optimized_lanes_pruned);
}

static uint32_t baseline_stat_sum(KdClass3SearchStats fraud,
                                  KdClass3SearchStats legit,
                                  int field) {
    switch (field) {
        case 0: return fraud.nodes_visited + legit.nodes_visited;
        case 1: return fraud.leaves_visited + legit.leaves_visited;
        case 2: return fraud.points_evaluated + legit.points_evaluated;
        default: return fraud.pruned_branches + legit.pruned_branches;
    }
}

static uint32_t optimized_stat_sum(KdClass3OptStats fraud,
                                   KdClass3OptStats legit,
                                   int field) {
    switch (field) {
        case 0: return fraud.nodes_visited + legit.nodes_visited;
        case 1: return fraud.leaves_visited + legit.leaves_visited;
        case 2: return fraud.points_evaluated + legit.points_evaluated;
        case 3: return fraud.pruned_branches + legit.pruned_branches;
        case 4: return fraud.bbox_dimensions_evaluated +
                       legit.bbox_dimensions_evaluated;
        case 5: return fraud.blocks_evaluated + legit.blocks_evaluated;
        case 6: return fraud.blocks_checkpoint_pruned +
                       legit.blocks_checkpoint_pruned;
        default: return fraud.lanes_limit_pruned + legit.lanes_limit_pruned;
    }
}

int main(int argc, char **argv) {
    const char *tree_path = NULL;
    const char *test_data_path = NULL;
    const char *vectors_csv_path = NULL;
    int limit = 0;
    bool touch = false;
    KdClass3OptMode mode = KDCLASS3_OPT_COMBINED;
    const char *variant = "combined";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tree") == 0 && i + 1 < argc) {
            tree_path = argv[++i];
        } else if (strcmp(argv[i], "--test-data") == 0 && i + 1 < argc) {
            test_data_path = argv[++i];
        } else if (strcmp(argv[i], "--vectors-csv") == 0 && i + 1 < argc) {
            vectors_csv_path = argv[++i];
        } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            limit = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--touch") == 0) {
            touch = true;
        } else if (strcmp(argv[i], "--variant") == 0 && i + 1 < argc) {
            variant = argv[++i];
            if (strcmp(variant, "bbox") == 0) {
                mode = KDCLASS3_OPT_BBOX_ONLY;
            } else if (strcmp(variant, "checkpoint") == 0) {
                mode = KDCLASS3_OPT_CHECKPOINT_ONLY;
            } else if (strcmp(variant, "combined") == 0) {
                mode = KDCLASS3_OPT_COMBINED;
            } else if (strcmp(variant, "simd-bbox") == 0) {
                mode = KDCLASS3_OPT_SIMD_BBOX_ONLY;
            } else if (strcmp(variant, "simd-combined") == 0) {
                mode = KDCLASS3_OPT_SIMD_COMBINED;
            } else if (strcmp(variant, "simd-full") == 0) {
                mode = KDCLASS3_OPT_SIMD_BBOX_FULL;
            } else {
                usage();
                return 2;
            }
        } else {
            usage();
            return 2;
        }
    }
    if (tree_path == NULL ||
        ((test_data_path == NULL) == (vectors_csv_path == NULL))) {
        usage();
        return 2;
    }

    char err[256];
    KdClass3Index index;
    if (kdclass3_open(tree_path, &index, err, sizeof(err)) != 0) {
        fprintf(stderr, "evaluate_kdclass3_opt: %s\n", err);
        return 1;
    }
    uint64_t touch_sum = touch ? kdclass3_touch_pages(&index) : 0;

    const char *input_path =
        test_data_path != NULL ? test_data_path : vectors_csv_path;
    char *input = NULL;
    size_t input_len = 0;
    if (read_file(input_path, &input, &input_len) != 0) {
        kdclass3_close(&index);
        return 1;
    }

    Samples samples;
    memset(&samples, 0, sizeof(samples));
    size_t total = 0;
    uint64_t tp = 0;
    uint64_t tn = 0;
    uint64_t fp = 0;
    uint64_t fn = 0;
    uint64_t errors = 0;
    uint64_t result_mismatches = 0;
    uint64_t runtime_simd_mismatches = 0;
    uint64_t fallback_count = 0;
    unsigned printed_mismatches = 0;
    const char *json_cursor = input;
    char *csv_cursor = input;
    const char *end = input + input_len;

    while (limit <= 0 || total < (size_t)limit) {
        int16_t query[IVF8_INDEX_DIMS];
        int expected_approved = 0;
        int next = 0;
        if (test_data_path != NULL) {
            Slice request;
            next = next_entry(&json_cursor, end, &request, &expected_approved);
            if (next > 0 &&
                !fastvector_vectorize(request.data, request.len, query)) {
                next = -1;
            }
        } else {
            next = next_csv_vector(&csv_cursor, end, query, &expected_approved);
        }
        if (next == 0) {
            break;
        }
        if (next < 0 || samples_reserve(&samples, total + 1u) != 0) {
            errors++;
            break;
        }

        KdClass3SearchResult baseline;
        KdClass3OptSearchResult optimized;
        uint64_t baseline_elapsed;
        uint64_t optimized_elapsed;
        if ((total & 1u) == 0u) {
            uint64_t start = now_ns();
            baseline = kdclass3_search(&index, query);
            baseline_elapsed = now_ns() - start;
            start = now_ns();
            optimized = kdclass3_opt_search_mode(&index, query, mode);
            optimized_elapsed = now_ns() - start;
        } else {
            uint64_t start = now_ns();
            optimized = kdclass3_opt_search_mode(&index, query, mode);
            optimized_elapsed = now_ns() - start;
            start = now_ns();
            baseline = kdclass3_search(&index, query);
            baseline_elapsed = now_ns() - start;
        }

        bool mismatch =
            baseline.fraud_count != optimized.fraud_count ||
            baseline.fallback_required != optimized.fallback_required ||
            baseline.fraud_distance3 != optimized.fraud_distance3 ||
            baseline.legit_distance3 != optimized.legit_distance3;
        if (mismatch) {
            result_mismatches++;
            if (printed_mismatches < 10u) {
                fprintf(stderr,
                        "mismatch row=%zu base=(%u,%d,%llu,%llu) "
                        "opt=(%u,%d,%llu,%llu)\n",
                        total,
                        (unsigned)baseline.fraud_count,
                        baseline.fallback_required ? 1 : 0,
                        (unsigned long long)baseline.fraud_distance3,
                        (unsigned long long)baseline.legit_distance3,
                        (unsigned)optimized.fraud_count,
                        optimized.fallback_required ? 1 : 0,
                        (unsigned long long)optimized.fraud_distance3,
                        (unsigned long long)optimized.legit_distance3);
                printed_mismatches++;
            }
        }
        if (mode == KDCLASS3_OPT_SIMD_BBOX_FULL) {
            KdClass3SearchResult runtime_simd =
                kdclass3_search_simd_full(&index, query);
            if (baseline.fraud_count != runtime_simd.fraud_count ||
                baseline.fallback_required != runtime_simd.fallback_required ||
                baseline.fraud_distance3 != runtime_simd.fraud_distance3 ||
                baseline.legit_distance3 != runtime_simd.legit_distance3) {
                runtime_simd_mismatches++;
            }
        }
        if (optimized.fallback_required) {
            fallback_count++;
            errors++;
        } else {
            int approved = optimized.fraud_count < 3u ? 1 : 0;
            if (approved == expected_approved) {
                if (approved != 0) {
                    tn++;
                } else {
                    tp++;
                }
            } else if (approved != 0) {
                fn++;
            } else {
                fp++;
            }
        }

        samples.baseline_ns[total] = baseline_elapsed;
        samples.optimized_ns[total] = optimized_elapsed;
        samples.baseline_nodes[total] =
            baseline_stat_sum(baseline.fraud_stats, baseline.legit_stats, 0);
        samples.baseline_leaves[total] =
            baseline_stat_sum(baseline.fraud_stats, baseline.legit_stats, 1);
        samples.baseline_points[total] =
            baseline_stat_sum(baseline.fraud_stats, baseline.legit_stats, 2);
        samples.baseline_pruned[total] =
            baseline_stat_sum(baseline.fraud_stats, baseline.legit_stats, 3);
        samples.optimized_nodes[total] =
            optimized_stat_sum(optimized.fraud_stats, optimized.legit_stats, 0);
        samples.optimized_leaves[total] =
            optimized_stat_sum(optimized.fraud_stats, optimized.legit_stats, 1);
        samples.optimized_points[total] =
            optimized_stat_sum(optimized.fraud_stats, optimized.legit_stats, 2);
        samples.optimized_pruned[total] =
            optimized_stat_sum(optimized.fraud_stats, optimized.legit_stats, 3);
        samples.optimized_bbox_dims[total] =
            optimized_stat_sum(optimized.fraud_stats, optimized.legit_stats, 4);
        samples.optimized_blocks[total] =
            optimized_stat_sum(optimized.fraud_stats, optimized.legit_stats, 5);
        samples.optimized_checkpoint_pruned[total] =
            optimized_stat_sum(optimized.fraud_stats, optimized.legit_stats, 6);
        samples.optimized_lanes_pruned[total] =
            optimized_stat_sum(optimized.fraud_stats, optimized.legit_stats, 7);
        total++;
    }

    Summary baseline_summary = summarize_u64(samples.baseline_ns, total);
    Summary optimized_summary = summarize_u64(samples.optimized_ns, total);
    printf("tree=%s\ninput=%s\nvariant=%s\n", tree_path, input_path, variant);
    printf("touch=%s\ntouch_sum=%llu\n",
           touch ? "true" : "false", (unsigned long long)touch_sum);
    printf("evaluated=%zu\n", total);
    printf("TP=%llu\nTN=%llu\nFP=%llu\nFN=%llu\nError=%llu\n",
           (unsigned long long)tp, (unsigned long long)tn,
           (unsigned long long)fp, (unsigned long long)fn,
           (unsigned long long)errors);
    printf("fallback_count=%llu\n", (unsigned long long)fallback_count);
    printf("result_mismatches=%llu\n", (unsigned long long)result_mismatches);
    printf("runtime_simd_mismatches=%llu\n",
           (unsigned long long)runtime_simd_mismatches);
    printf("runtime_memory_mib=%.2f\n",
           (double)kdclass3_runtime_memory_bytes(&index) / 1048576.0);
    print_summary("baseline", baseline_summary);
    print_summary("optimized", optimized_summary);
    printf("avg_improvement_percent=%.3f\n",
           baseline_summary.avg > 0
               ? (baseline_summary.avg - optimized_summary.avg) * 100.0 /
                     baseline_summary.avg
               : 0.0);
    printf("p99_improvement_percent=%.3f\n",
           baseline_summary.p99 > 0
               ? (baseline_summary.p99 - optimized_summary.p99) * 100.0 /
                     baseline_summary.p99
               : 0.0);

    print_u32_summary("baseline_nodes", samples.baseline_nodes, total);
    print_u32_summary("optimized_nodes", samples.optimized_nodes, total);
    print_u32_summary("baseline_leaves", samples.baseline_leaves, total);
    print_u32_summary("optimized_leaves", samples.optimized_leaves, total);
    print_u32_summary("baseline_points", samples.baseline_points, total);
    print_u32_summary("optimized_points", samples.optimized_points, total);
    print_u32_summary("baseline_pruned", samples.baseline_pruned, total);
    print_u32_summary("optimized_pruned", samples.optimized_pruned, total);
    print_u32_summary("optimized_bbox_dimensions",
                      samples.optimized_bbox_dims, total);
    print_u32_summary("optimized_blocks", samples.optimized_blocks, total);
    print_u32_summary("optimized_checkpoint_pruned",
                      samples.optimized_checkpoint_pruned, total);
    print_u32_summary("optimized_lanes_limit_pruned",
                      samples.optimized_lanes_pruned, total);

    samples_free(&samples);
    free(input);
    kdclass3_close(&index);
    return errors == 0 && fp == 0 && fn == 0 && result_mismatches == 0 &&
                   runtime_simd_mismatches == 0
               ? 0
               : 1;
}
