#define _POSIX_C_SOURCE 200809L

#include "fastvector.h"
#include "ivf8_index.h"
#include "ivf8_search.h"
#include "kdprimary2.h"
#include "kdtree_repair.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_RULES 4u
#define MAX_CANDIDATES 8192u

typedef struct {
    const char *data;
    size_t len;
} Slice;

typedef enum {
    FEATURE_WORST = 0,
    FEATURE_BEST,
    FEATURE_SPREAD,
    FEATURE_GAP10,
    FEATURE_GAP21,
    FEATURE_GAP32,
    FEATURE_GAP43,
    FEATURE_MIXED_MARGIN,
    FEATURE_CANDIDATES,
    FEATURE_CLUSTERS,
    FEATURE_LARGEST_CLUSTER,
    FEATURE_V0,
    FEATURE_COUNT = FEATURE_V0 + FASTVECTOR_DIMENSIONS
} FeatureKind;

typedef enum {
    OP_GE = 0,
    OP_LE = 1
} FeatureOp;

typedef struct {
    int row_index;
    int expected_approved;
    int16_t vector[FASTVECTOR_DIMENSIONS];
    uint8_t kd_fraud_count;
    uint8_t ivf8_fraud_count;
    uint8_t pattern;
    uint8_t labels[IVF8_SEARCH_TOP_K];
    uint64_t distances[IVF8_SEARCH_TOP_K];
    int64_t features[FEATURE_COUNT];
    uint64_t kd_ns;
    uint64_t ivf8_ns;
    Ivf8SearchTraceResult trace;
} GateRecord;

typedef struct {
    char name[192];
    uint8_t fc_mask;
    int pattern;
    FeatureKind feature;
    FeatureOp op;
    int64_t threshold;
    bool has_feature;
} GateRule;

typedef struct {
    const char *name;
    int total;
    int train_count;
    int validation_count;
    int fallback;
    int fast_accept;
    int tp;
    int tn;
    int fp;
    int fn;
    int fraud_count_mismatches_vs_kd;
    int approved_mismatches_vs_kd;
    uint64_t *effective_ns;
    uint64_t effective_sum_ns;
} GateEval;

static void usage(void) {
    fprintf(stderr,
            "usage: evaluate_gate --index <index.bin> --tree <kdprimary2.bin> --test-data <test-data.json> "
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
        fprintf(stderr, "evaluate_gate: open %s: %s\n", path, strerror(errno));
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

static int compare_u64(const void *a, const void *b) {
    uint64_t av = *(const uint64_t *)a;
    uint64_t bv = *(const uint64_t *)b;
    return (av > bv) - (av < bv);
}

static size_t percentile_index(size_t n, double percentile) {
    if (n == 0) {
        return 0;
    }
    size_t idx = (size_t)((percentile / 100.0) * (double)(n - 1u) + 0.5);
    return idx < n ? idx : n - 1u;
}

static int approved_from_count(uint8_t fraud_count) {
    return fraud_count < 3u ? 1 : 0;
}

static uint64_t mixed_label_margin(const Ivf8Neighbor top[IVF8_SEARCH_TOP_K]) {
    uint64_t nearest_legit = UINT64_MAX;
    uint64_t nearest_fraud = UINT64_MAX;
    for (uint32_t i = 0; i < IVF8_SEARCH_TOP_K; i++) {
        if (top[i].fraud) {
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

static void fill_features(GateRecord *r) {
    uint64_t d0 = r->distances[0];
    uint64_t d1 = r->distances[1];
    uint64_t d2 = r->distances[2];
    uint64_t d3 = r->distances[3];
    uint64_t d4 = r->distances[4];
    r->features[FEATURE_BEST] = (int64_t)d0;
    r->features[FEATURE_WORST] = (int64_t)d4;
    r->features[FEATURE_SPREAD] = (int64_t)(d4 >= d0 ? d4 - d0 : 0u);
    r->features[FEATURE_GAP10] = (int64_t)(d1 >= d0 ? d1 - d0 : 0u);
    r->features[FEATURE_GAP21] = (int64_t)(d2 >= d1 ? d2 - d1 : 0u);
    r->features[FEATURE_GAP32] = (int64_t)(d3 >= d2 ? d3 - d2 : 0u);
    r->features[FEATURE_GAP43] = (int64_t)(d4 >= d3 ? d4 - d3 : 0u);
    uint64_t margin = mixed_label_margin(r->trace.top);
    r->features[FEATURE_MIXED_MARGIN] = margin == UINT64_MAX ? INT64_MAX : (int64_t)margin;
    r->features[FEATURE_CANDIDATES] = (int64_t)r->trace.result.stats.candidates_scanned;
    r->features[FEATURE_CLUSTERS] = (int64_t)r->trace.result.stats.clusters_scanned;
    r->features[FEATURE_LARGEST_CLUSTER] = (int64_t)r->trace.result.stats.largest_scanned_cluster_candidates;
    for (uint32_t dim = 0; dim < FASTVECTOR_DIMENSIONS; dim++) {
        r->features[FEATURE_V0 + dim] = r->vector[dim];
    }
}

static const char *feature_name(FeatureKind f) {
    switch (f) {
        case FEATURE_WORST: return "worst";
        case FEATURE_BEST: return "best";
        case FEATURE_SPREAD: return "spread";
        case FEATURE_GAP10: return "gap10";
        case FEATURE_GAP21: return "gap21";
        case FEATURE_GAP32: return "gap32";
        case FEATURE_GAP43: return "gap43";
        case FEATURE_MIXED_MARGIN: return "mixed_margin";
        case FEATURE_CANDIDATES: return "candidates";
        case FEATURE_CLUSTERS: return "clusters";
        case FEATURE_LARGEST_CLUSTER: return "largest_cluster";
        default:
            break;
    }
    static char names[FASTVECTOR_DIMENSIONS][8];
    if (f >= FEATURE_V0 && f < FEATURE_COUNT) {
        uint32_t dim = (uint32_t)(f - FEATURE_V0);
        (void)snprintf(names[dim], sizeof(names[dim]), "v%u", dim);
        return names[dim];
    }
    return "unknown";
}

static bool is_train_row(const GateRecord *r) {
    return (r->row_index % 10) < 7;
}

static bool rule_matches(const GateRule *rule, const GateRecord *r) {
    if (rule->fc_mask != 0 && (rule->fc_mask & (uint8_t)(1u << r->ivf8_fraud_count)) == 0) {
        return false;
    }
    if (rule->pattern >= 0 && r->pattern != (uint8_t)rule->pattern) {
        return false;
    }
    if (!rule->has_feature) {
        return true;
    }
    int64_t value = r->features[rule->feature];
    return rule->op == OP_GE ? value >= rule->threshold : value <= rule->threshold;
}

static bool selected_rules_match(const GateRule *rules, uint32_t rule_count, const GateRecord *r) {
    for (uint32_t i = 0; i < rule_count; i++) {
        if (rule_matches(&rules[i], r)) {
            return true;
        }
    }
    return false;
}

static bool repair_policy_matches(KdTreeRepairPolicy policy, const GateRecord *r) {
    return kdtree_repair_should_run(policy, &r->trace);
}

static void eval_init(GateEval *eval, const char *name, int total) {
    memset(eval, 0, sizeof(*eval));
    eval->name = name;
    eval->effective_ns = (uint64_t *)calloc((size_t)total, sizeof(uint64_t));
}

static void eval_free(GateEval *eval) {
    free(eval->effective_ns);
}

static void eval_record(GateEval *eval,
                        const GateRecord *r,
                        bool fallback,
                        uint8_t predicted_fraud_count,
                        bool include) {
    if (!include) {
        return;
    }
    int predicted_approved = approved_from_count(predicted_fraud_count);
    if (r->expected_approved) {
        if (predicted_approved) {
            eval->tn++;
        } else {
            eval->fp++;
        }
    } else {
        if (predicted_approved) {
            eval->fn++;
        } else {
            eval->tp++;
        }
    }
    if (predicted_fraud_count != r->kd_fraud_count) {
        eval->fraud_count_mismatches_vs_kd++;
    }
    if (predicted_approved != approved_from_count(r->kd_fraud_count)) {
        eval->approved_mismatches_vs_kd++;
    }
    if (fallback) {
        eval->fallback++;
        eval->effective_ns[eval->total] = r->ivf8_ns + r->kd_ns;
    } else {
        eval->fast_accept++;
        eval->effective_ns[eval->total] = r->ivf8_ns;
    }
    eval->effective_sum_ns += eval->effective_ns[eval->total];
    eval->total++;
    if (is_train_row(r)) {
        eval->train_count++;
    } else {
        eval->validation_count++;
    }
}

static void print_eval(GateEval *eval, const char *dataset) {
    if (eval->total <= 0) {
        printf("gate_result dataset=%s name=%s total=0\n", dataset, eval->name);
        return;
    }
    qsort(eval->effective_ns, (size_t)eval->total, sizeof(uint64_t), compare_u64);
    size_t p95 = percentile_index((size_t)eval->total, 95.0);
    size_t p99 = percentile_index((size_t)eval->total, 99.0);
    int weighted = eval->fp + eval->fn * 3;
    double detection = 3000.0 - (double)weighted * (3000.0 / 242.0);
    if (detection < 0.0) {
        detection = 0.0;
    }
    printf("gate_result dataset=%s name=%s total=%d fast_accept=%d fast_accept_rate=%.6f fallback=%d fallback_rate=%.6f TP=%d TN=%d FP=%d FN=%d weighted=%d detection=%.2f fraud_mismatch_vs_kd=%d approved_mismatch_vs_kd=%d avg_effective_us=%.3f p95_effective_us=%.3f p99_effective_us=%.3f\n",
           dataset,
           eval->name,
           eval->total,
           eval->fast_accept,
           (double)eval->fast_accept / (double)eval->total,
           eval->fallback,
           (double)eval->fallback / (double)eval->total,
           eval->tp,
           eval->tn,
           eval->fp,
           eval->fn,
           weighted,
           detection,
           eval->fraud_count_mismatches_vs_kd,
           eval->approved_mismatches_vs_kd,
           (double)eval->effective_sum_ns / (double)eval->total / 1000.0,
           (double)eval->effective_ns[p95] / 1000.0,
           (double)eval->effective_ns[p99] / 1000.0);
}

typedef enum {
    POLICY_KD_EXACT = 0,
    POLICY_IVF8_ALL,
    POLICY_ACCEPT_EXTREME,
    POLICY_ACCEPT_015,
    POLICY_REPAIR_BOUNDARY23,
    POLICY_REPAIR_MINIMAL_V1,
    POLICY_REPAIR_PERFECT_V1,
    POLICY_REPAIR_BOUNDARY23_FAR45,
    POLICY_ACCEPT_EXTREME_WORST_LE
} BuiltinPolicy;

static void evaluate_builtin(const GateRecord *records,
                             int count,
                             BuiltinPolicy policy,
                             uint64_t threshold,
                             const char *name) {
    GateEval full;
    GateEval train;
    GateEval validation;
    eval_init(&full, name, count);
    eval_init(&train, name, count);
    eval_init(&validation, name, count);
    for (int i = 0; i < count; i++) {
        const GateRecord *r = &records[i];
        bool fallback = false;
        uint8_t predicted = r->ivf8_fraud_count;
        switch (policy) {
            case POLICY_KD_EXACT:
                fallback = true;
                predicted = r->kd_fraud_count;
                break;
            case POLICY_IVF8_ALL:
                fallback = false;
                break;
            case POLICY_ACCEPT_EXTREME:
                fallback = !(r->ivf8_fraud_count == 0 || r->ivf8_fraud_count == 5);
                break;
            case POLICY_ACCEPT_015:
                fallback = !(r->ivf8_fraud_count == 0 || r->ivf8_fraud_count == 1 || r->ivf8_fraud_count == 5);
                break;
            case POLICY_REPAIR_BOUNDARY23:
                fallback = r->ivf8_fraud_count == 2 || r->ivf8_fraud_count == 3;
                break;
            case POLICY_REPAIR_MINIMAL_V1:
                fallback = repair_policy_matches(KDTREE_REPAIR_POLICY_MINIMAL_V1, r);
                break;
            case POLICY_REPAIR_PERFECT_V1:
                fallback = repair_policy_matches(KDTREE_REPAIR_POLICY_PERFECT_V1, r);
                break;
            case POLICY_REPAIR_BOUNDARY23_FAR45:
                fallback = repair_policy_matches(KDTREE_REPAIR_POLICY_BOUNDARY23_FAR45, r);
                break;
            case POLICY_ACCEPT_EXTREME_WORST_LE:
                fallback = !((r->ivf8_fraud_count == 0 || r->ivf8_fraud_count == 5) &&
                             r->distances[IVF8_SEARCH_TOP_K - 1u] <= threshold);
                break;
        }
        if (fallback) {
            predicted = r->kd_fraud_count;
        }
        eval_record(&full, r, fallback, predicted, true);
        eval_record(&train, r, fallback, predicted, is_train_row(r));
        eval_record(&validation, r, fallback, predicted, !is_train_row(r));
    }
    print_eval(&full, "full");
    print_eval(&train, "train70_mod10");
    print_eval(&validation, "validation30_mod10");
    eval_free(&full);
    eval_free(&train);
    eval_free(&validation);
}

static void add_rule(GateRule *rules,
                     uint32_t *count,
                     uint8_t fc_mask,
                     int pattern,
                     FeatureKind feature,
                     FeatureOp op,
                     int64_t threshold,
                     bool has_feature) {
    if (*count >= MAX_CANDIDATES) {
        return;
    }
    GateRule *r = &rules[*count];
    memset(r, 0, sizeof(*r));
    r->fc_mask = fc_mask;
    r->pattern = pattern;
    r->feature = feature;
    r->op = op;
    r->threshold = threshold;
    r->has_feature = has_feature;
    if (has_feature) {
        (void)snprintf(r->name, sizeof(r->name), "%s%s%s_%s_%lld",
                       fc_mask != 0 ? "fc_mask_" : "",
                       pattern >= 0 ? "pattern_" : "",
                       feature_name(feature),
                       op == OP_GE ? "ge" : "le",
                       (long long)threshold);
    } else if (fc_mask != 0) {
        (void)snprintf(r->name, sizeof(r->name), "fc_mask_0x%02x", fc_mask);
    } else if (pattern >= 0) {
        (void)snprintf(r->name, sizeof(r->name), "pattern_%d", pattern);
    } else {
        (void)snprintf(r->name, sizeof(r->name), "all");
    }
    (*count)++;
}

static void build_candidate_rules(GateRule *rules, uint32_t *rule_count) {
    static const uint8_t masks[] = {
        (uint8_t)(1u << 0), (uint8_t)(1u << 1), (uint8_t)(1u << 2),
        (uint8_t)(1u << 3), (uint8_t)(1u << 4), (uint8_t)(1u << 5),
        (uint8_t)((1u << 2) | (1u << 3)),
        (uint8_t)((1u << 4) | (1u << 5)),
        (uint8_t)((1u << 0) | (1u << 5)),
        (uint8_t)((1u << 1) | (1u << 2) | (1u << 3) | (1u << 4)),
    };
    static const int64_t distance_thresholds[] = {
        50000, 100000, 250000, 500000, 750000, 1000000, 1500000, 2000000,
        2500000, 3000000, 3500000, 4000000, 4500000, 5000000, 6000000,
        8000000, 10000000, 15000000, 20000000
    };
    static const int64_t count_thresholds[] = {1, 2, 4, 8, 16, 512, 1024, 2048, 3072, 4096};
    static const int64_t vector_thresholds[] = {-9000, -7000, -5000, -3000, -2000, -1000, -500, 0,
                                                500, 1000, 2000, 3000, 5000, 7000, 9000};

    for (size_t i = 0; i < sizeof(masks) / sizeof(masks[0]); i++) {
        add_rule(rules, rule_count, masks[i], -1, FEATURE_WORST, OP_GE, 0, false);
    }
    for (uint8_t pattern = 0; pattern < 32u; pattern++) {
        add_rule(rules, rule_count, 0, pattern, FEATURE_WORST, OP_GE, 0, false);
    }
    for (size_t m = 0; m < sizeof(masks) / sizeof(masks[0]); m++) {
        for (FeatureKind f = 0; f < FEATURE_COUNT; f++) {
            const int64_t *thresholds = distance_thresholds;
            size_t threshold_count = sizeof(distance_thresholds) / sizeof(distance_thresholds[0]);
            if (f == FEATURE_CANDIDATES || f == FEATURE_CLUSTERS || f == FEATURE_LARGEST_CLUSTER) {
                thresholds = count_thresholds;
                threshold_count = sizeof(count_thresholds) / sizeof(count_thresholds[0]);
            } else if (f >= FEATURE_V0) {
                thresholds = vector_thresholds;
                threshold_count = sizeof(vector_thresholds) / sizeof(vector_thresholds[0]);
            }
            for (size_t t = 0; t < threshold_count; t++) {
                add_rule(rules, rule_count, masks[m], -1, f, OP_GE, thresholds[t], true);
                add_rule(rules, rule_count, masks[m], -1, f, OP_LE, thresholds[t], true);
            }
        }
    }
}

static void greedy_model(const GateRecord *records, int count) {
    GateRule *candidates = (GateRule *)calloc(MAX_CANDIDATES, sizeof(GateRule));
    GateRule selected[MAX_RULES];
    bool *covered = (bool *)calloc((size_t)count, sizeof(bool));
    if (candidates == NULL || covered == NULL) {
        free(candidates);
        free(covered);
        return;
    }
    uint32_t candidate_count = 0;
    build_candidate_rules(candidates, &candidate_count);

    int train_wrong_total = 0;
    int validation_wrong_total = 0;
    int full_wrong_total = 0;
    for (int i = 0; i < count; i++) {
        bool wrong = approved_from_count(records[i].ivf8_fraud_count) != approved_from_count(records[i].kd_fraud_count);
        if (wrong) {
            full_wrong_total++;
            if (is_train_row(&records[i])) {
                train_wrong_total++;
            } else {
                validation_wrong_total++;
            }
        }
    }

    uint32_t selected_count = 0;
    for (; selected_count < MAX_RULES; selected_count++) {
        int best = -1;
        int best_cover = 0;
        int best_repairs = 1;
        for (uint32_t c = 0; c < candidate_count; c++) {
            int cover = 0;
            int repairs = 0;
            for (int i = 0; i < count; i++) {
                if (!is_train_row(&records[i]) || !rule_matches(&candidates[c], &records[i])) {
                    continue;
                }
                repairs++;
                bool wrong = approved_from_count(records[i].ivf8_fraud_count) != approved_from_count(records[i].kd_fraud_count);
                if (wrong && !covered[i]) {
                    cover++;
                }
            }
            if (cover == 0 || repairs == 0) {
                continue;
            }
            if (best < 0 ||
                cover * best_repairs > best_cover * repairs ||
                (cover * best_repairs == best_cover * repairs && repairs < best_repairs)) {
                best = (int)c;
                best_cover = cover;
                best_repairs = repairs;
            }
        }
        if (best < 0) {
            break;
        }
        selected[selected_count] = candidates[best];
        for (int i = 0; i < count; i++) {
            bool wrong = approved_from_count(records[i].ivf8_fraud_count) != approved_from_count(records[i].kd_fraud_count);
            if (is_train_row(&records[i]) && wrong && rule_matches(&candidates[best], &records[i])) {
                covered[i] = true;
            }
        }
    }

    printf("model_greedy_summary candidates=%u selected=%u train_wrong=%d validation_wrong=%d full_wrong=%d rules=\"",
           candidate_count,
           selected_count,
           train_wrong_total,
           validation_wrong_total,
           full_wrong_total);
    for (uint32_t i = 0; i < selected_count; i++) {
        printf("%s%s", i == 0 ? "" : " OR ", selected[i].name);
    }
    printf("\"\n");

    GateEval full;
    GateEval train;
    GateEval validation;
    eval_init(&full, "greedy_uncertainty_v1", count);
    eval_init(&train, "greedy_uncertainty_v1", count);
    eval_init(&validation, "greedy_uncertainty_v1", count);
    for (int i = 0; i < count; i++) {
        const GateRecord *r = &records[i];
        bool fallback = selected_rules_match(selected, selected_count, r);
        uint8_t predicted = fallback ? r->kd_fraud_count : r->ivf8_fraud_count;
        eval_record(&full, r, fallback, predicted, true);
        eval_record(&train, r, fallback, predicted, is_train_row(r));
        eval_record(&validation, r, fallback, predicted, !is_train_row(r));
    }
    print_eval(&full, "full");
    print_eval(&train, "train70_mod10");
    print_eval(&validation, "validation30_mod10");
    eval_free(&full);
    eval_free(&train);
    eval_free(&validation);
    free(covered);
    free(candidates);
}

int main(int argc, char **argv) {
    const char *index_path = NULL;
    const char *tree_path = NULL;
    const char *test_data_path = NULL;
    int limit = 0;
    bool touch = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--index") == 0 && i + 1 < argc) {
            index_path = argv[++i];
        } else if (strcmp(argv[i], "--tree") == 0 && i + 1 < argc) {
            tree_path = argv[++i];
        } else if (strcmp(argv[i], "--test-data") == 0 && i + 1 < argc) {
            test_data_path = argv[++i];
        } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            limit = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--touch") == 0) {
            touch = true;
        } else {
            usage();
            return 1;
        }
    }
    if (index_path == NULL || tree_path == NULL || test_data_path == NULL) {
        usage();
        return 1;
    }

    char err[256];
    Ivf8Index index;
    memset(&index, 0, sizeof(index));
    index.fd = -1;
    if (ivf8_index_open(index_path, &index, err, sizeof(err)) != 0) {
        fprintf(stderr, "evaluate_gate: %s\n", err);
        return 1;
    }
    KdPrimary2Index tree;
    memset(&tree, 0, sizeof(tree));
    tree.fd = -1;
    if (kdprimary2_open(tree_path, &tree, err, sizeof(err)) != 0) {
        fprintf(stderr, "evaluate_gate: %s\n", err);
        ivf8_index_close(&index);
        return 1;
    }
    if (touch) {
        (void)kdprimary2_touch_pages(&tree);
    }
    Ivf8SearchConfig ivf8_cfg = {
        .max_candidates = IVF8_SEARCH_DEFAULT_MAX_CANDIDATES,
        .probes = IVF8_SEARCH_DEFAULT_PROBES,
        .impl = ivf8_cpu_supports_avx2() ? IVF8_SEARCH_IMPL_AVX2 : IVF8_SEARCH_IMPL_SCALAR,
    };

    char *data = NULL;
    size_t data_len = 0;
    if (read_file(test_data_path, &data, &data_len) != 0) {
        kdprimary2_close(&tree);
        ivf8_index_close(&index);
        return 1;
    }

    size_t capacity = 65536u;
    GateRecord *records = (GateRecord *)calloc(capacity, sizeof(GateRecord));
    if (records == NULL) {
        free(data);
        kdprimary2_close(&tree);
        ivf8_index_close(&index);
        return 1;
    }

    const char *cursor = data;
    const char *end = data + data_len;
    int count = 0;
    int row_index = 0;
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
        if (limit > 0 && count >= limit) {
            break;
        }
        if ((size_t)count == capacity) {
            capacity *= 2u;
            GateRecord *next_records = (GateRecord *)realloc(records, capacity * sizeof(GateRecord));
            if (next_records == NULL) {
                parse_errors++;
                break;
            }
            records = next_records;
        }

        GateRecord *r = &records[count];
        memset(r, 0, sizeof(*r));
        r->row_index = row_index;
        r->expected_approved = expected_approved;
        if (!fastvector_vectorize(request.data, request.len, r->vector)) {
            parse_errors++;
            row_index++;
            continue;
        }
        uint64_t start = now_ns();
        KdPrimary2SearchResult kd = kdprimary2_search_top5(&tree, r->vector);
        r->kd_ns = now_ns() - start;
        r->kd_fraud_count = kd.fraud_count;

        start = now_ns();
        r->trace = ivf8_search_trace(&index, r->vector, &ivf8_cfg);
        r->ivf8_ns = now_ns() - start;
        r->ivf8_fraud_count = r->trace.result.fraud_count;
        for (uint32_t i = 0; i < IVF8_SEARCH_TOP_K; i++) {
            r->labels[i] = r->trace.top[i].fraud;
            r->distances[i] = r->trace.top[i].distance;
            if (r->trace.top[i].fraud) {
                r->pattern |= (uint8_t)(1u << i);
            }
        }
        fill_features(r);
        count++;
        row_index++;
    }

    int kd_wrong = 0;
    int ivf8_wrong = 0;
    int ivf8_fraud_mismatch = 0;
    int ivf8_approved_mismatch = 0;
    for (int i = 0; i < count; i++) {
        int kd_approved = approved_from_count(records[i].kd_fraud_count);
        int ivf8_approved = approved_from_count(records[i].ivf8_fraud_count);
        if (kd_approved != records[i].expected_approved) {
            kd_wrong++;
        }
        if (ivf8_approved != records[i].expected_approved) {
            ivf8_wrong++;
        }
        if (records[i].ivf8_fraud_count != records[i].kd_fraud_count) {
            ivf8_fraud_mismatch++;
        }
        if (ivf8_approved != kd_approved) {
            ivf8_approved_mismatch++;
        }
    }

    printf("gate_dataset total=%d parse_errors=%d ivf8_impl=%s kd_wrong=%d ivf8_wrong=%d ivf8_fraud_mismatch_vs_kd=%d ivf8_approved_mismatch_vs_kd=%d train70_mod10=%d validation30_mod10=%d\n",
           count,
           parse_errors,
           ivf8_search_impl_name(ivf8_cfg.impl),
           kd_wrong,
           ivf8_wrong,
           ivf8_fraud_mismatch,
           ivf8_approved_mismatch,
           (count * 7 + 9) / 10,
           count - ((count * 7 + 9) / 10));

    evaluate_builtin(records, count, POLICY_KD_EXACT, 0, "kd_exact_baseline");
    evaluate_builtin(records, count, POLICY_IVF8_ALL, 0, "ivf8_all_no_fallback");
    evaluate_builtin(records, count, POLICY_ACCEPT_EXTREME, 0, "ivf8_accept_fc0_or_fc5");
    evaluate_builtin(records, count, POLICY_ACCEPT_015, 0, "ivf8_accept_fc0_fc1_fc5");
    evaluate_builtin(records, count, POLICY_REPAIR_BOUNDARY23, 0, "ivf8_repair_fc2_or_fc3");
    evaluate_builtin(records, count, POLICY_REPAIR_MINIMAL_V1, 0, "ivf8_repair_minimal_v1");
    evaluate_builtin(records, count, POLICY_REPAIR_PERFECT_V1, 0, "ivf8_repair_perfect_v1");
    evaluate_builtin(records, count, POLICY_REPAIR_BOUNDARY23_FAR45, 0, "ivf8_repair_boundary23_far45");

    static const uint64_t worst_thresholds[] = {1000000ull, 2000000ull, 3000000ull, 4000000ull,
                                                4500000ull, 5000000ull, 6000000ull, 8000000ull};
    for (size_t i = 0; i < sizeof(worst_thresholds) / sizeof(worst_thresholds[0]); i++) {
        char name[80];
        (void)snprintf(name, sizeof(name), "ivf8_accept_extreme_worst_le_%llu",
                       (unsigned long long)worst_thresholds[i]);
        evaluate_builtin(records, count, POLICY_ACCEPT_EXTREME_WORST_LE, worst_thresholds[i], name);
    }

    greedy_model(records, count);

    free(records);
    free(data);
    kdprimary2_close(&tree);
    ivf8_index_close(&index);
    return parse_errors == 0 ? 0 : 1;
}
