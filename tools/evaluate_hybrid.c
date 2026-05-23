#define _POSIX_C_SOURCE 200809L

#include "fastvector.h"
#include "ivf8_index.h"
#include "ivf8_search.h"
#include "kdtree.h"
#include "kdtree_repair.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_WORST_THRESHOLD KDTREE_REPAIR_BOUNDARY23_FAR45_THRESHOLD
#define DEFAULT_MARGIN_THRESHOLD 250000ull
#define DEFAULT_ASSUMED_P99_MS 13.64
#define MAX_PRINTED_WRONG 20

typedef struct {
    const char *data;
    size_t len;
} Slice;

typedef enum {
    POLICY_BOUNDARY23 = 0,
    POLICY_BOUNDARY234,
    POLICY_BOUNDARY23_FAR45,
    POLICY_BOUNDARY23_FAR,
    POLICY_UNCERTAIN,
    POLICY_NON_EXTREME,
    POLICY_ALL
} HybridPolicy;

typedef struct {
    int total;
    int tp;
    int tn;
    int fp;
    int fn;
    int errors;
    int repairs;
    int fraud_count_mismatches_vs_ivf8;
    int approved_mismatches_vs_ivf8;
    int fraud_count_mismatches_vs_kd;
    int approved_mismatches_vs_kd;
    int wrong_printed;
    int fraud_counts[6];
    uint64_t effective_ns_sum;
    uint64_t ivf8_ns_sum;
    uint64_t kd_repair_ns_sum;
    uint64_t kd_oracle_ns_sum;
    uint64_t ivf8_candidates_sum;
    uint64_t *effective_ns;
    uint64_t *ivf8_ns;
    uint64_t *kd_repair_ns;
    uint64_t *kd_oracle_ns;
    uint32_t *ivf8_candidates;
    size_t kd_repair_count;
    size_t capacity;
    size_t repair_capacity;
} HybridStats;

static void usage(void) {
    fprintf(stderr,
            "usage: evaluate_hybrid --index <index.bin> --tree <tree.bin> --test-data <test-data.json> "
            "[--limit N] [--policy boundary23|boundary234|boundary23_far45|boundary23_far|uncertain|non_extreme|all] "
            "[--worst-threshold N] [--margin-threshold N] [--assume-p99-ms N] "
            "[--output-errors path]\n");
}

static uint64_t now_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static const char *policy_name(HybridPolicy policy) {
    switch (policy) {
        case POLICY_BOUNDARY23:
            return "boundary23";
        case POLICY_BOUNDARY234:
            return "boundary234";
        case POLICY_BOUNDARY23_FAR45:
            return "boundary23_far45";
        case POLICY_BOUNDARY23_FAR:
            return "boundary23_far";
        case POLICY_UNCERTAIN:
            return "uncertain";
        case POLICY_NON_EXTREME:
            return "non_extreme";
        case POLICY_ALL:
            return "all";
    }
    return "unknown";
}

static bool parse_policy(const char *value, HybridPolicy *out) {
    if (strcmp(value, "boundary23") == 0) {
        *out = POLICY_BOUNDARY23;
    } else if (strcmp(value, "boundary234") == 0) {
        *out = POLICY_BOUNDARY234;
    } else if (strcmp(value, "boundary23_far45") == 0) {
        *out = POLICY_BOUNDARY23_FAR45;
    } else if (strcmp(value, "boundary23_far") == 0 || strcmp(value, "distance_gate") == 0) {
        *out = POLICY_BOUNDARY23_FAR;
    } else if (strcmp(value, "uncertain") == 0) {
        *out = POLICY_UNCERTAIN;
    } else if (strcmp(value, "non_extreme") == 0) {
        *out = POLICY_NON_EXTREME;
    } else if (strcmp(value, "all") == 0) {
        *out = POLICY_ALL;
    } else {
        return false;
    }
    return true;
}

static int read_file(const char *path, char **out, size_t *out_len) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "evaluate_hybrid: open %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 1;
    }
    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return 1;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
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

static double ns_to_us(uint64_t ns) {
    return (double)ns / 1000.0;
}

static int stats_reserve(HybridStats *stats, size_t needed) {
    if (needed <= stats->capacity) {
        return 0;
    }
    size_t next = stats->capacity == 0 ? 1024u : stats->capacity * 2u;
    while (next < needed) {
        next *= 2u;
    }

    uint64_t *effective = (uint64_t *)malloc(next * sizeof(uint64_t));
    uint64_t *ivf8 = (uint64_t *)malloc(next * sizeof(uint64_t));
    uint64_t *kd_oracle = (uint64_t *)malloc(next * sizeof(uint64_t));
    uint32_t *candidates = (uint32_t *)malloc(next * sizeof(uint32_t));
    if (effective == NULL || ivf8 == NULL || kd_oracle == NULL || candidates == NULL) {
        free(effective);
        free(ivf8);
        free(kd_oracle);
        free(candidates);
        return -1;
    }
    if (stats->capacity > 0) {
        memcpy(effective, stats->effective_ns, stats->capacity * sizeof(uint64_t));
        memcpy(ivf8, stats->ivf8_ns, stats->capacity * sizeof(uint64_t));
        memcpy(kd_oracle, stats->kd_oracle_ns, stats->capacity * sizeof(uint64_t));
        memcpy(candidates, stats->ivf8_candidates, stats->capacity * sizeof(uint32_t));
    }
    free(stats->effective_ns);
    free(stats->ivf8_ns);
    free(stats->kd_oracle_ns);
    free(stats->ivf8_candidates);
    stats->effective_ns = effective;
    stats->ivf8_ns = ivf8;
    stats->kd_oracle_ns = kd_oracle;
    stats->ivf8_candidates = candidates;
    stats->capacity = next;
    return 0;
}

static int repair_stats_reserve(HybridStats *stats, size_t needed) {
    if (needed <= stats->repair_capacity) {
        return 0;
    }
    size_t next = stats->repair_capacity == 0 ? 128u : stats->repair_capacity * 2u;
    while (next < needed) {
        next *= 2u;
    }
    uint64_t *kd_repair = (uint64_t *)malloc(next * sizeof(uint64_t));
    if (kd_repair == NULL) {
        return -1;
    }
    if (stats->repair_capacity > 0) {
        memcpy(kd_repair, stats->kd_repair_ns, stats->repair_capacity * sizeof(uint64_t));
    }
    free(stats->kd_repair_ns);
    stats->kd_repair_ns = kd_repair;
    stats->repair_capacity = next;
    return 0;
}

static void record_confusion(HybridStats *stats, int expected_approved, int approved) {
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

static uint64_t top5_worst_distance(const Ivf8Neighbor top[IVF8_SEARCH_TOP_K]) {
    return top[IVF8_SEARCH_TOP_K - 1u].distance;
}

static uint64_t top5_mixed_label_margin(const Ivf8Neighbor top[IVF8_SEARCH_TOP_K]) {
    uint64_t nearest_legit = UINT64_MAX;
    uint64_t nearest_fraud = UINT64_MAX;
    for (uint32_t i = 0; i < IVF8_SEARCH_TOP_K; i++) {
        if (top[i].fraud != 0) {
            if (top[i].distance < nearest_fraud) {
                nearest_fraud = top[i].distance;
            }
        } else if (top[i].distance < nearest_legit) {
            nearest_legit = top[i].distance;
        }
    }
    if (nearest_legit == UINT64_MAX || nearest_fraud == UINT64_MAX) {
        return UINT64_MAX;
    }
    return nearest_legit > nearest_fraud ? nearest_legit - nearest_fraud : nearest_fraud - nearest_legit;
}

static bool should_repair(HybridPolicy policy,
                          const Ivf8SearchTraceResult *trace,
                          uint64_t worst_threshold,
                          uint64_t margin_threshold) {
    uint8_t fc = trace->result.fraud_count;
    uint64_t worst = top5_worst_distance(trace->top);
    uint64_t margin = top5_mixed_label_margin(trace->top);
    switch (policy) {
        case POLICY_BOUNDARY23:
            return fc == 2u || fc == 3u;
        case POLICY_BOUNDARY234:
            return fc == 2u || fc == 3u || fc == 4u;
        case POLICY_BOUNDARY23_FAR45:
            return fc == 2u || fc == 3u || ((fc == 4u || fc == 5u) && worst >= worst_threshold);
        case POLICY_BOUNDARY23_FAR:
            return fc == 2u || fc == 3u || worst >= worst_threshold;
        case POLICY_UNCERTAIN:
            return fc == 2u || fc == 3u ||
                   ((fc == 1u || fc == 4u) &&
                    (worst >= worst_threshold || margin <= margin_threshold));
        case POLICY_NON_EXTREME:
            return fc != 0u && fc != 5u;
        case POLICY_ALL:
            return true;
    }
    return false;
}

static double p99_score(double p99_ms) {
    const double k = 1000.0;
    const double t_max_ms = 1000.0;
    const double p99_min_ms = 1.0;
    const double p99_max_ms = 2000.0;
    if (p99_ms <= 0.0) {
        return 0.0;
    }
    if (p99_ms > p99_max_ms) {
        return -3000.0;
    }
    double clamped = p99_ms < p99_min_ms ? p99_min_ms : p99_ms;
    return k * log10(t_max_ms / clamped);
}

static double detection_score(int total, int fp, int fn, int errors) {
    const double k = 1000.0;
    const double epsilon_min = 0.001;
    const double beta = 300.0;
    const double failure_cut = 0.15;
    int weighted = fp + fn * 3 + errors * 5;
    int failures = fp + fn + errors;
    double failure_rate = total > 0 ? (double)failures / (double)total : 0.0;
    if (failure_rate > failure_cut) {
        return -3000.0;
    }
    double epsilon = total > 0 ? (double)weighted / (double)total : 0.0;
    if (epsilon < epsilon_min) {
        epsilon = epsilon_min;
    }
    return k * log10(1.0 / epsilon) - beta * log10(1.0 + (double)weighted);
}

static void write_error_header(FILE *file) {
    fprintf(file,
            "query_index,kind,expected_approved,final_fraud_count,ivf8_fraud_count,kd_fraud_count,repaired,"
            "worst_top5_distance,mixed_margin,ivf8_candidates,ivf8_us,kd_us,effective_us");
    for (uint32_t i = 0; i < IVF8_SEARCH_TOP_K; i++) {
        fprintf(file, ",ivf8_top%u_fraud,ivf8_top%u_distance", i + 1u, i + 1u);
    }
    fputc('\n', file);
}

static void write_error_row(FILE *file,
                            int query_index,
                            const char *kind,
                            int expected_approved,
                            uint8_t final_fc,
                            uint8_t ivf8_fc,
                            uint8_t kd_fc,
                            bool repaired,
                            const Ivf8SearchTraceResult *trace,
                            uint64_t ivf8_ns,
                            uint64_t kd_ns,
                            uint64_t effective_ns) {
    if (file == NULL) {
        return;
    }
    uint64_t worst = top5_worst_distance(trace->top);
    uint64_t margin = top5_mixed_label_margin(trace->top);
    fprintf(file,
            "%d,%s,%d,%u,%u,%u,%d,%llu,%llu,%u,%.3f,%.3f,%.3f",
            query_index,
            kind,
            expected_approved,
            final_fc,
            ivf8_fc,
            kd_fc,
            repaired ? 1 : 0,
            (unsigned long long)worst,
            (unsigned long long)margin,
            trace->result.stats.candidates_scanned,
            ns_to_us(ivf8_ns),
            ns_to_us(kd_ns),
            ns_to_us(effective_ns));
    for (uint32_t i = 0; i < IVF8_SEARCH_TOP_K; i++) {
        fprintf(file, ",%u,%llu", trace->top[i].fraud, (unsigned long long)trace->top[i].distance);
    }
    fputc('\n', file);
}

int main(int argc, char **argv) {
    const char *index_path = NULL;
    const char *tree_path = NULL;
    const char *test_data_path = NULL;
    const char *errors_path = NULL;
    int limit = 0;
    HybridPolicy policy = POLICY_BOUNDARY23;
    uint64_t worst_threshold = DEFAULT_WORST_THRESHOLD;
    uint64_t margin_threshold = DEFAULT_MARGIN_THRESHOLD;
    double assumed_p99_ms = DEFAULT_ASSUMED_P99_MS;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--index") == 0 && i + 1 < argc) {
            index_path = argv[++i];
        } else if (strcmp(argv[i], "--tree") == 0 && i + 1 < argc) {
            tree_path = argv[++i];
        } else if (strcmp(argv[i], "--test-data") == 0 && i + 1 < argc) {
            test_data_path = argv[++i];
        } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            limit = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--policy") == 0 && i + 1 < argc) {
            if (!parse_policy(argv[++i], &policy)) {
                usage();
                return 2;
            }
        } else if (strcmp(argv[i], "--compare-ivf8") == 0) {
            /* IVF8 is always run as the fast path; keep this accepted for script compatibility. */
        } else if (strcmp(argv[i], "--worst-threshold") == 0 && i + 1 < argc) {
            worst_threshold = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--margin-threshold") == 0 && i + 1 < argc) {
            margin_threshold = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--assume-p99-ms") == 0 && i + 1 < argc) {
            assumed_p99_ms = strtod(argv[++i], NULL);
        } else if (strcmp(argv[i], "--output-errors") == 0 && i + 1 < argc) {
            errors_path = argv[++i];
        } else {
            usage();
            return 2;
        }
    }
    if (index_path == NULL || tree_path == NULL || test_data_path == NULL) {
        usage();
        return 2;
    }

    char err[256];
    Ivf8Index index;
    if (ivf8_index_open(index_path, &index, err, sizeof(err)) != 0) {
        fprintf(stderr, "evaluate_hybrid: %s\n", err);
        return 1;
    }

    KdTree tree;
    if (kdtree_load_nodes_for_ivf8(&tree, &index, tree_path) != 0) {
        fprintf(stderr, "evaluate_hybrid: load tree %s failed: %s\n", tree_path, strerror(errno));
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
            fprintf(stderr, "evaluate_hybrid: open %s: %s\n", errors_path, strerror(errno));
            free(data);
            kdtree_free(&tree);
            ivf8_index_close(&index);
            return 1;
        }
        write_error_header(errors_file);
    }

    Ivf8SearchConfig ivf8_cfg = {
        .max_candidates = IVF8_SEARCH_DEFAULT_MAX_CANDIDATES,
        .probes = IVF8_SEARCH_DEFAULT_PROBES,
        .impl = IVF8_SEARCH_IMPL_AVX2,
    };
    if (!ivf8_cpu_supports_avx2()) {
        ivf8_cfg.impl = IVF8_SEARCH_IMPL_SCALAR;
    }
    KdTreeSearchConfig kd_cfg = {.max_visited = 0};

    HybridStats stats;
    memset(&stats, 0, sizeof(stats));
    const char *cursor = data;
    const char *end = data + data_len;
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
            fprintf(stderr, "evaluate_hybrid: failed parsing test data near entry %d\n", stats.total);
            stats.errors++;
            break;
        }
        if (stats_reserve(&stats, (size_t)stats.total + 1u) != 0) {
            fprintf(stderr, "evaluate_hybrid: out of memory\n");
            stats.errors++;
            break;
        }

        int16_t query[IVF8_INDEX_DIMS];
        if (!fastvector_vectorize(request.data, request.len, query)) {
            stats.errors++;
            stats.effective_ns[stats.total] = 0;
            stats.ivf8_ns[stats.total] = 0;
            stats.kd_oracle_ns[stats.total] = 0;
            stats.ivf8_candidates[stats.total] = 0;
            stats.total++;
            continue;
        }

        uint64_t ivf8_start = now_ns();
        Ivf8SearchTraceResult ivf8 = ivf8_search_trace_profiled(&index, query, &ivf8_cfg);
        uint64_t ivf8_ns = now_ns() - ivf8_start;

        uint64_t kd_start = now_ns();
        KdTreeSearchResult kd = kdtree_search_top5(&tree, query, &kd_cfg);
        uint64_t kd_ns = now_ns() - kd_start;

        bool repair = should_repair(policy, &ivf8, worst_threshold, margin_threshold);
        uint8_t final_fc = repair ? kd.fraud_count : ivf8.result.fraud_count;
        uint8_t ivf8_fc = ivf8.result.fraud_count;
        uint8_t kd_fc = kd.fraud_count;
        int approved = final_fc < 3u ? 1 : 0;
        int kd_approved = kd_fc < 3u ? 1 : 0;
        int ivf8_approved = ivf8_fc < 3u ? 1 : 0;
        uint64_t effective_ns = ivf8_ns + (repair ? kd_ns : 0u);

        if (final_fc < 6u) {
            stats.fraud_counts[final_fc]++;
        }
        if (ivf8_fc != final_fc) {
            stats.fraud_count_mismatches_vs_ivf8++;
        }
        if (ivf8_approved != approved) {
            stats.approved_mismatches_vs_ivf8++;
        }
        if (kd_fc != final_fc) {
            stats.fraud_count_mismatches_vs_kd++;
        }
        if (kd_approved != approved) {
            stats.approved_mismatches_vs_kd++;
        }
        if (repair) {
            if (repair_stats_reserve(&stats, stats.kd_repair_count + 1u) != 0) {
                fprintf(stderr, "evaluate_hybrid: out of memory\n");
                stats.errors++;
                break;
            }
            stats.kd_repair_ns[stats.kd_repair_count++] = kd_ns;
            stats.kd_repair_ns_sum += kd_ns;
            stats.repairs++;
        }

        record_confusion(&stats, expected_approved, approved);
        if (expected_approved != approved && stats.wrong_printed < MAX_PRINTED_WRONG) {
            fprintf(stderr,
                    "wrong[%d] idx=%d expected=%d final=%u ivf8=%u kd=%u repaired=%d worst=%llu margin=%llu candidates=%u\n",
                    stats.wrong_printed,
                    stats.total,
                    expected_approved,
                    final_fc,
                    ivf8_fc,
                    kd_fc,
                    repair ? 1 : 0,
                    (unsigned long long)top5_worst_distance(ivf8.top),
                    (unsigned long long)top5_mixed_label_margin(ivf8.top),
                    ivf8.result.stats.candidates_scanned);
            stats.wrong_printed++;
        }
        if (expected_approved != approved) {
            write_error_row(errors_file,
                            stats.total,
                            expected_approved ? "FN" : "FP",
                            expected_approved,
                            final_fc,
                            ivf8_fc,
                            kd_fc,
                            repair,
                            &ivf8,
                            ivf8_ns,
                            kd_ns,
                            effective_ns);
        }

        stats.effective_ns[stats.total] = effective_ns;
        stats.ivf8_ns[stats.total] = ivf8_ns;
        stats.kd_oracle_ns[stats.total] = kd_ns;
        stats.ivf8_candidates[stats.total] = ivf8.result.stats.candidates_scanned;
        stats.effective_ns_sum += effective_ns;
        stats.ivf8_ns_sum += ivf8_ns;
        stats.kd_oracle_ns_sum += kd_ns;
        stats.ivf8_candidates_sum += ivf8.result.stats.candidates_scanned;
        stats.total++;
    }

    if (stats.total > 0) {
        qsort(stats.effective_ns, (size_t)stats.total, sizeof(uint64_t), compare_u64);
        qsort(stats.ivf8_ns, (size_t)stats.total, sizeof(uint64_t), compare_u64);
        qsort(stats.kd_oracle_ns, (size_t)stats.total, sizeof(uint64_t), compare_u64);
        qsort(stats.ivf8_candidates, (size_t)stats.total, sizeof(uint32_t), compare_u32);
    }
    if (stats.kd_repair_count > 0) {
        qsort(stats.kd_repair_ns, stats.kd_repair_count, sizeof(uint64_t), compare_u64);
    }

    int weighted_errors = stats.fp + stats.fn * 3 + stats.errors * 5;
    double det_score = detection_score(stats.total, stats.fp, stats.fn, stats.errors);
    double latency_score = p99_score(assumed_p99_ms);
    printf("evaluated=%d\n", stats.total);
    printf("policy=%s\n", policy_name(policy));
    printf("worst_threshold=%llu\n", (unsigned long long)worst_threshold);
    printf("margin_threshold=%llu\n", (unsigned long long)margin_threshold);
    printf("ivf8_impl=%s\n", ivf8_search_impl_name(ivf8_cfg.impl));
    printf("TP=%d\nTN=%d\nFP=%d\nFN=%d\nError=%d\n", stats.tp, stats.tn, stats.fp, stats.fn, stats.errors);
    printf("weighted_errors=%d\n", weighted_errors);
    printf("repair_count=%d\n", stats.repairs);
    printf("repair_rate=%.6f\n", stats.total > 0 ? (double)stats.repairs / (double)stats.total : 0.0);
    printf("fraud_count_mismatches_vs_ivf8=%d\n", stats.fraud_count_mismatches_vs_ivf8);
    printf("approved_mismatches_vs_ivf8=%d\n", stats.approved_mismatches_vs_ivf8);
    printf("fraud_count_mismatches_vs_kd=%d\n", stats.fraud_count_mismatches_vs_kd);
    printf("approved_mismatches_vs_kd=%d\n", stats.approved_mismatches_vs_kd);
    printf("fraud_count_0=%d\nfraud_count_1=%d\nfraud_count_2=%d\n",
           stats.fraud_counts[0], stats.fraud_counts[1], stats.fraud_counts[2]);
    printf("fraud_count_3=%d\nfraud_count_4=%d\nfraud_count_5=%d\n",
           stats.fraud_counts[3], stats.fraud_counts[4], stats.fraud_counts[5]);
    printf("assumed_p99_ms=%.3f\n", assumed_p99_ms);
    printf("p99_score_estimate=%.2f\n", latency_score);
    printf("detection_score_estimate=%.2f\n", det_score);
    printf("final_score_estimate=%.2f\n", latency_score + det_score);
    printf("kdtree_runtime_memory_mib=%.2f\n", (double)kdtree_runtime_memory_bytes(&tree) / 1048576.0);
    printf("ivf8_plus_kdtree_mib=%.2f\n",
           ((double)index.file_size + (double)kdtree_runtime_memory_bytes(&tree)) / 1048576.0);
    if (stats.total > 0) {
        size_t p50 = percentile_index((size_t)stats.total, 50.0);
        size_t p95 = percentile_index((size_t)stats.total, 95.0);
        size_t p99 = percentile_index((size_t)stats.total, 99.0);
        printf("avg_effective_search_us=%.3f\n", ns_to_us(stats.effective_ns_sum / (uint64_t)stats.total));
        printf("p50_effective_search_us=%.3f\n", ns_to_us(stats.effective_ns[p50]));
        printf("p95_effective_search_us=%.3f\n", ns_to_us(stats.effective_ns[p95]));
        printf("p99_effective_search_us=%.3f\n", ns_to_us(stats.effective_ns[p99]));
        printf("avg_ivf8_us=%.3f\n", ns_to_us(stats.ivf8_ns_sum / (uint64_t)stats.total));
        printf("p50_ivf8_us=%.3f\n", ns_to_us(stats.ivf8_ns[p50]));
        printf("p95_ivf8_us=%.3f\n", ns_to_us(stats.ivf8_ns[p95]));
        printf("p99_ivf8_us=%.3f\n", ns_to_us(stats.ivf8_ns[p99]));
        printf("avg_kd_oracle_us=%.3f\n", ns_to_us(stats.kd_oracle_ns_sum / (uint64_t)stats.total));
        printf("p50_kd_oracle_us=%.3f\n", ns_to_us(stats.kd_oracle_ns[p50]));
        printf("p95_kd_oracle_us=%.3f\n", ns_to_us(stats.kd_oracle_ns[p95]));
        printf("p99_kd_oracle_us=%.3f\n", ns_to_us(stats.kd_oracle_ns[p99]));
        printf("avg_ivf8_candidates=%.2f\n", (double)stats.ivf8_candidates_sum / (double)stats.total);
        printf("p50_ivf8_candidates=%u\n", stats.ivf8_candidates[p50]);
        printf("p95_ivf8_candidates=%u\n", stats.ivf8_candidates[p95]);
        printf("p99_ivf8_candidates=%u\n", stats.ivf8_candidates[p99]);
    }
    if (stats.kd_repair_count > 0) {
        size_t p50 = percentile_index(stats.kd_repair_count, 50.0);
        size_t p95 = percentile_index(stats.kd_repair_count, 95.0);
        size_t p99 = percentile_index(stats.kd_repair_count, 99.0);
        printf("avg_kd_repair_us=%.3f\n", ns_to_us(stats.kd_repair_ns_sum / (uint64_t)stats.kd_repair_count));
        printf("p50_kd_repair_us=%.3f\n", ns_to_us(stats.kd_repair_ns[p50]));
        printf("p95_kd_repair_us=%.3f\n", ns_to_us(stats.kd_repair_ns[p95]));
        printf("p99_kd_repair_us=%.3f\n", ns_to_us(stats.kd_repair_ns[p99]));
    } else {
        printf("avg_kd_repair_us=0.000\np50_kd_repair_us=0.000\np95_kd_repair_us=0.000\np99_kd_repair_us=0.000\n");
    }

    if (errors_file != NULL) {
        fclose(errors_file);
    }
    free(stats.effective_ns);
    free(stats.ivf8_ns);
    free(stats.kd_repair_ns);
    free(stats.kd_oracle_ns);
    free(stats.ivf8_candidates);
    free(data);
    kdtree_free(&tree);
    ivf8_index_close(&index);
    return stats.errors == 0 ? 0 : 1;
}
