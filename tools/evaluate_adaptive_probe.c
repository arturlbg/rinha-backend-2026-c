#define _POSIX_C_SOURCE 200809L

#include "fastvector.h"
#include "ivf8_index.h"
#include "ivf8_search.h"
#include "kdclass3.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_CAPACITY 60000u
#define MAX_PRINTED_WRONG 4u
#define PATTERN_COUNT 32u

typedef struct {
    const char *data;
    size_t len;
} Slice;

typedef enum {
    STAGE_DEFAULT8 = 0,
    STAGE_P1,
    STAGE_P2,
    STAGE_P4,
    STAGE_P8,
    STAGE_P10,
    STAGE_P12,
    STAGE_P16,
    STAGE_P32,
    STAGE_P48,
    STAGE_COUNT
} StageKind;

typedef enum {
    POLICY_DEFAULT8 = 0,
    POLICY_COMPETITOR_P10_PATTERN48,
    POLICY_AMBIGUOUS_P12_TO_P48,
    POLICY_STAGED_EXTREME,
    POLICY_STAGED_STABLE_TOP5,
    POLICY_P8_EXTREME_FALLBACK,
    POLICY_BOUNDARY_P8_P16_P32_FALLBACK,
    POLICY_BOUNDARY_P8_P16_P32_P48_ACCEPT,
    POLICY_MARGIN_P8_P32_FALLBACK,
    POLICY_BBOX_P1,
    POLICY_BBOX_P2,
    POLICY_BBOX_P4,
    POLICY_BBOX_P8,
    POLICY_BBOX_STAGED,
    POLICY_COUNT
} PolicyKind;

typedef struct {
    bool ready;
    bool proof_ready;
    bool proof_safe;
    uint64_t elapsed_ns;
    uint64_t proof_ns;
    Ivf8SearchTraceResult trace;
} StageResult;

typedef struct {
    StageResult stages[STAGE_COUNT];
    bool gap_ready;
    uint64_t gap_ns;
    uint64_t centroid_gap_10_11;
} QueryCache;

typedef struct {
    bool approved;
    bool fallback;
    bool expanded;
    StageKind final_stage;
    uint64_t initial_ns;
    uint64_t expansion_ns;
    uint64_t fallback_ns;
    uint64_t effective_ns;
    uint64_t probes;
    uint64_t candidates;
    uint8_t pattern;
} PolicyDecision;

typedef struct {
    const char *name;
    uint64_t total;
    uint64_t tp;
    uint64_t tn;
    uint64_t fp;
    uint64_t fn;
    uint64_t errors;
    uint64_t mismatches_vs_kdclass3;
    uint64_t fallbacks;
    uint64_t expansions;
    uint64_t probes_sum;
    uint64_t candidates_sum;
    uint64_t effective_ns_sum;
    uint64_t initial_ns_sum;
    uint64_t expansion_ns_sum;
    uint64_t fallback_ns_sum;
    uint64_t final_stage_counts[STAGE_COUNT];
    uint64_t patterns[PATTERN_COUNT];
    uint64_t wrong_patterns[PATTERN_COUNT];
    uint64_t *effective_ns;
    uint64_t *initial_ns;
    uint64_t *expansion_ns;
    uint64_t *fallback_ns;
    size_t expansion_count;
    size_t fallback_count;
    size_t capacity;
    unsigned wrong_printed;
} PolicyStats;

static const char *const STAGE_NAMES[STAGE_COUNT] = {
    "default8_cap4096", "p1", "p2", "p4", "p8", "p10", "p12", "p16", "p32", "p48",
};

static const uint32_t STAGE_PROBES[STAGE_COUNT] = {
    8u, 1u, 2u, 4u, 8u, 10u, 12u, 16u, 32u, 48u,
};

static const char *const POLICY_NAMES[POLICY_COUNT] = {
    "ivf8_default_p8_cap4096",
    "competitor_p10_pattern48",
    "ambiguous_p12_to_p48",
    "staged_extreme_p2_p4_p8_p16",
    "staged_stable_top5_p2_p4_p8_p16",
    "p8_extreme_else_kdclass3",
    "boundary_p8_p16_p32_else_kdclass3",
    "boundary_p8_p16_p32_p48_accept",
    "margin_p8_p32_else_kdclass3",
    "bbox_certificate_p1",
    "bbox_certificate_p2",
    "bbox_certificate_p4",
    "bbox_certificate_p8",
    "bbox_certificate_staged_p1_p2_p4_p8",
};

static void usage(void) {
    fprintf(stderr,
            "usage: evaluate_adaptive_probe --index <index.bin> --tree <kdclass3.bin> "
            "(--test-data <test-data.json> | --vectors-csv <rf-dataset.csv>) [--limit N] [--touch]\n");
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
        fprintf(stderr, "evaluate_adaptive_probe: open %s: %s\n", path, strerror(errno));
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
            ((size_t)(line_end - line) >= 6u && memcmp(line, "source", 6u) == 0)) {
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
            if (errno != 0 || value_end == p || (col < 19u && *value_end != ',')) {
                ok = false;
                break;
            }
            p = value_end + (col < 19u ? 1 : 0);
        }
        *line_end = saved;
        if (!ok) {
            return -1;
        }
        for (size_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
            if (values[4u + dim] < INT16_MIN || values[4u + dim] > INT16_MAX) {
                return -1;
            }
            query[dim] = (int16_t)values[4u + dim];
        }
        *expected_approved = values[19] != 0 ? 1 : 0;
        return 1;
    }
    return 0;
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

static uint8_t trace_pattern(const Ivf8SearchTraceResult *trace) {
    uint8_t pattern = 0;
    for (uint32_t i = 0; i < IVF8_SEARCH_TOP_K; i++) {
        if (trace->top[i].fraud != 0) {
            pattern = (uint8_t)(pattern | (uint8_t)(1u << i));
        }
    }
    return pattern;
}

static bool trace_approved(const Ivf8SearchTraceResult *trace) {
    return trace->result.fraud_count < 3u;
}

static bool trace_top5_equal(const Ivf8SearchTraceResult *a, const Ivf8SearchTraceResult *b) {
    for (uint32_t i = 0; i < IVF8_SEARCH_TOP_K; i++) {
        if (a->top[i].distance != b->top[i].distance || a->top[i].fraud != b->top[i].fraud) {
            return false;
        }
    }
    return true;
}

static uint64_t mixed_label_margin(const Ivf8SearchTraceResult *trace) {
    uint64_t nearest_legit = UINT64_MAX;
    uint64_t nearest_fraud = UINT64_MAX;
    for (uint32_t i = 0; i < IVF8_SEARCH_TOP_K; i++) {
        if (trace->top[i].fraud != 0u) {
            if (trace->top[i].distance < nearest_fraud) {
                nearest_fraud = trace->top[i].distance;
            }
        } else if (trace->top[i].distance < nearest_legit) {
            nearest_legit = trace->top[i].distance;
        }
    }
    if (nearest_legit == UINT64_MAX || nearest_fraud == UINT64_MAX) {
        return UINT64_MAX;
    }
    return nearest_legit >= nearest_fraud
               ? nearest_legit - nearest_fraud
               : nearest_fraud - nearest_legit;
}

static StageResult *ensure_stage(QueryCache *cache,
                                 StageKind kind,
                                 const Ivf8Index *index,
                                 const int16_t query[IVF8_INDEX_DIMS]) {
    StageResult *stage = &cache->stages[kind];
    if (stage->ready) {
        return stage;
    }
    Ivf8SearchConfig cfg = {
        .max_candidates = kind == STAGE_DEFAULT8 ? IVF8_SEARCH_DEFAULT_MAX_CANDIDATES : UINT32_MAX,
        .probes = STAGE_PROBES[kind],
        .impl = IVF8_SEARCH_IMPL_AVX2,
    };
    uint64_t start = now_ns();
    stage->trace = ivf8_search_trace(index, query, &cfg);
    stage->elapsed_ns = now_ns() - start;
    stage->ready = true;
    return stage;
}

static bool ensure_bbox_certificate(QueryCache *cache,
                                    StageKind kind,
                                    const Ivf8Index *index,
                                    const int16_t query[IVF8_INDEX_DIMS]) {
    StageResult *stage = ensure_stage(cache, kind, index, query);
    if (stage->proof_ready) {
        return stage->proof_safe;
    }

    uint64_t start = now_ns();
    bool selected[IVF8_PRODUCTION_K];
    memset(selected, 0, sizeof(selected));
    bool safe = index->k <= IVF8_PRODUCTION_K &&
                stage->trace.top[IVF8_SEARCH_TOP_K - 1u].seq != UINT32_MAX &&
                stage->trace.result.stats.radius_pruned == 0u;
    if (safe) {
        for (uint32_t i = 0; i < stage->trace.probe_count; i++) {
            uint32_t cluster = stage->trace.probes[i].cluster;
            if (cluster < index->k) {
                selected[cluster] = true;
            }
        }
        uint64_t worst = stage->trace.top[IVF8_SEARCH_TOP_K - 1u].distance;
        for (uint32_t cluster = 0; cluster < index->k; cluster++) {
            if (!selected[cluster] && ivf8_bbox_distance(index, query, cluster) <= worst) {
                safe = false;
                break;
            }
        }
    }
    stage->proof_ns = now_ns() - start;
    stage->proof_safe = safe;
    stage->proof_ready = true;
    return safe;
}

static uint64_t ensure_centroid_gap_10_11(QueryCache *cache,
                                          const Ivf8Index *index,
                                          const int16_t query[IVF8_INDEX_DIMS]) {
    if (!cache->gap_ready) {
        Ivf8Probe probes[IVF8_SEARCH_MAX_PROBES];
        uint64_t start = now_ns();
        uint32_t count = ivf8_select_probes(index, query, 11u, probes);
        cache->gap_ns = now_ns() - start;
        cache->centroid_gap_10_11 =
            count >= 11u && probes[10].distance >= probes[9].distance
                ? probes[10].distance - probes[9].distance
                : UINT64_MAX;
        cache->gap_ready = true;
    }
    return cache->centroid_gap_10_11;
}

static bool competitor_risky_pattern(uint8_t pattern, uint64_t gap) {
    switch (pattern) {
        case 0x06u:
        case 0x0au:
        case 0x13u:
            return gap <= 500000ull;
        case 0x0cu:
            return gap <= 600000ull;
        case 0x12u:
            return gap <= 1200000ull;
        case 0x16u:
            return gap <= 700000ull;
        case 0x1cu:
            return gap <= 150000ull;
        default:
            return false;
    }
}

static PolicyDecision decision_from_stage(StageKind kind, const StageResult *stage) {
    PolicyDecision decision;
    memset(&decision, 0, sizeof(decision));
    decision.approved = trace_approved(&stage->trace);
    decision.final_stage = kind;
    decision.pattern = trace_pattern(&stage->trace);
    decision.initial_ns = stage->elapsed_ns + (stage->proof_ready ? stage->proof_ns : 0u);
    decision.probes = stage->trace.probe_count;
    decision.candidates = stage->trace.result.stats.candidates_scanned;
    return decision;
}

static void use_expansion(PolicyDecision *decision, StageKind kind, const StageResult *stage) {
    decision->expanded = true;
    decision->expansion_ns += stage->elapsed_ns + (stage->proof_ready ? stage->proof_ns : 0u);
    decision->probes += stage->trace.probe_count;
    decision->candidates += stage->trace.result.stats.candidates_scanned;
    decision->approved = trace_approved(&stage->trace);
    decision->final_stage = kind;
}

static void use_fallback(PolicyDecision *decision, bool oracle_approved, uint64_t oracle_ns) {
    decision->fallback = true;
    decision->fallback_ns = oracle_ns;
    decision->approved = oracle_approved;
}

static int policy_stats_init(PolicyStats *stats, const char *name, size_t capacity) {
    memset(stats, 0, sizeof(*stats));
    stats->name = name;
    stats->capacity = capacity;
    stats->effective_ns = (uint64_t *)calloc(capacity, sizeof(uint64_t));
    stats->initial_ns = (uint64_t *)calloc(capacity, sizeof(uint64_t));
    stats->expansion_ns = (uint64_t *)calloc(capacity, sizeof(uint64_t));
    stats->fallback_ns = (uint64_t *)calloc(capacity, sizeof(uint64_t));
    if (stats->effective_ns == NULL || stats->initial_ns == NULL ||
        stats->expansion_ns == NULL || stats->fallback_ns == NULL) {
        return -1;
    }
    return 0;
}

static void policy_stats_free(PolicyStats *stats) {
    free(stats->effective_ns);
    free(stats->initial_ns);
    free(stats->expansion_ns);
    free(stats->fallback_ns);
}

static void record_decision(PolicyStats *stats,
                            PolicyDecision *decision,
                            bool expected_approved,
                            bool oracle_approved,
                            uint64_t row) {
    decision->effective_ns = decision->initial_ns + decision->expansion_ns + decision->fallback_ns;
    size_t pos = (size_t)stats->total;
    if (pos >= stats->capacity) {
        stats->errors++;
        return;
    }
    stats->effective_ns[pos] = decision->effective_ns;
    stats->initial_ns[pos] = decision->initial_ns;
    if (decision->expanded) {
        stats->expansion_ns[stats->expansion_count++] = decision->expansion_ns;
        stats->expansions++;
    }
    if (decision->fallback) {
        stats->fallback_ns[stats->fallback_count++] = decision->fallback_ns;
        stats->fallbacks++;
    }
    stats->effective_ns_sum += decision->effective_ns;
    stats->initial_ns_sum += decision->initial_ns;
    stats->expansion_ns_sum += decision->expansion_ns;
    stats->fallback_ns_sum += decision->fallback_ns;
    stats->probes_sum += decision->probes;
    stats->candidates_sum += decision->candidates;
    stats->final_stage_counts[decision->final_stage]++;
    stats->patterns[decision->pattern]++;

    if (decision->approved && expected_approved) {
        stats->tp++;
    } else if (!decision->approved && !expected_approved) {
        stats->tn++;
    } else if (decision->approved) {
        stats->fp++;
    } else {
        stats->fn++;
    }
    if (decision->approved != oracle_approved) {
        stats->mismatches_vs_kdclass3++;
        stats->wrong_patterns[decision->pattern]++;
        if (stats->wrong_printed < MAX_PRINTED_WRONG) {
            printf("wrong policy=%s row=%" PRIu64 " expected=%d oracle=%d predicted=%d pattern=0x%02x stage=%s\n",
                   stats->name, row, expected_approved ? 1 : 0, oracle_approved ? 1 : 0,
                   decision->approved ? 1 : 0, decision->pattern, STAGE_NAMES[decision->final_stage]);
            stats->wrong_printed++;
        }
    }
    stats->total++;
}

static void sort_policy_samples(PolicyStats *stats) {
    qsort(stats->effective_ns, (size_t)stats->total, sizeof(uint64_t), compare_u64);
    qsort(stats->initial_ns, (size_t)stats->total, sizeof(uint64_t), compare_u64);
    qsort(stats->expansion_ns, stats->expansion_count, sizeof(uint64_t), compare_u64);
    qsort(stats->fallback_ns, stats->fallback_count, sizeof(uint64_t), compare_u64);
}

static void print_sample_stats(const char *prefix, const uint64_t *samples, size_t count, uint64_t sum) {
    if (count == 0) {
        printf("%s_avg_us=0.000 %s_p50_us=0.000 %s_p95_us=0.000 %s_p99_us=0.000\n",
               prefix, prefix, prefix, prefix);
        return;
    }
    printf("%s_avg_us=%.3f %s_p50_us=%.3f %s_p95_us=%.3f %s_p99_us=%.3f\n",
           prefix, ns_to_us(sum / (uint64_t)count),
           prefix, ns_to_us(samples[percentile_index(count, 50.0)]),
           prefix, ns_to_us(samples[percentile_index(count, 95.0)]),
           prefix, ns_to_us(samples[percentile_index(count, 99.0)]));
}

static void print_policy(PolicyStats *stats) {
    sort_policy_samples(stats);
    double total = stats->total == 0 ? 1.0 : (double)stats->total;
    printf("\npolicy=%s\n", stats->name);
    printf("evaluated=%" PRIu64 " TP=%" PRIu64 " TN=%" PRIu64 " FP=%" PRIu64 " FN=%" PRIu64
           " Error=%" PRIu64 " mismatches_vs_kdclass3=%" PRIu64 "\n",
           stats->total, stats->tp, stats->tn, stats->fp, stats->fn, stats->errors,
           stats->mismatches_vs_kdclass3);
    printf("fallbacks=%" PRIu64 " fallback_rate=%.6f expansions=%" PRIu64 " expansion_rate=%.6f\n",
           stats->fallbacks, 100.0 * (double)stats->fallbacks / total,
           stats->expansions, 100.0 * (double)stats->expansions / total);
    printf("avg_effective_probes=%.3f avg_effective_candidates=%.3f\n",
           (double)stats->probes_sum / total, (double)stats->candidates_sum / total);
    print_sample_stats("effective", stats->effective_ns, (size_t)stats->total, stats->effective_ns_sum);
    print_sample_stats("initial", stats->initial_ns, (size_t)stats->total, stats->initial_ns_sum);
    print_sample_stats("expansion", stats->expansion_ns, stats->expansion_count, stats->expansion_ns_sum);
    print_sample_stats("fallback", stats->fallback_ns, stats->fallback_count, stats->fallback_ns_sum);
    printf("stage_decisions=");
    bool first = true;
    for (size_t i = 0; i < STAGE_COUNT; i++) {
        if (stats->final_stage_counts[i] != 0) {
            printf("%s%s:%" PRIu64, first ? "" : ",", STAGE_NAMES[i], stats->final_stage_counts[i]);
            first = false;
        }
    }
    printf("\npatterns=");
    first = true;
    for (size_t i = 0; i < PATTERN_COUNT; i++) {
        if (stats->patterns[i] != 0) {
            printf("%s%02zx:%" PRIu64, first ? "" : ",", i, stats->patterns[i]);
            first = false;
        }
    }
    printf("\nwrong_patterns=");
    first = true;
    for (size_t i = 0; i < PATTERN_COUNT; i++) {
        if (stats->wrong_patterns[i] != 0) {
            printf("%s%02zx:%" PRIu64, first ? "" : ",", i, stats->wrong_patterns[i]);
            first = false;
        }
    }
    printf("\n");
}

static PolicyDecision make_default(QueryCache *cache, const Ivf8Index *index, const int16_t query[IVF8_INDEX_DIMS]) {
    StageResult *stage = ensure_stage(cache, STAGE_DEFAULT8, index, query);
    return decision_from_stage(STAGE_DEFAULT8, stage);
}

static PolicyDecision make_competitor_pattern(QueryCache *cache,
                                              const Ivf8Index *index,
                                              const int16_t query[IVF8_INDEX_DIMS]) {
    StageResult *p10 = ensure_stage(cache, STAGE_P10, index, query);
    PolicyDecision decision = decision_from_stage(STAGE_P10, p10);
    uint64_t gap = ensure_centroid_gap_10_11(cache, index, query);
    decision.initial_ns += cache->gap_ns;
    if (competitor_risky_pattern(decision.pattern, gap)) {
        use_expansion(&decision, STAGE_P48, ensure_stage(cache, STAGE_P48, index, query));
    }
    return decision;
}

static PolicyDecision make_ambiguous12(QueryCache *cache,
                                       const Ivf8Index *index,
                                       const int16_t query[IVF8_INDEX_DIMS]) {
    StageResult *p12 = ensure_stage(cache, STAGE_P12, index, query);
    PolicyDecision decision = decision_from_stage(STAGE_P12, p12);
    uint8_t fraud_count = p12->trace.result.fraud_count;
    if (fraud_count >= 1u && fraud_count <= 4u) {
        use_expansion(&decision, STAGE_P48, ensure_stage(cache, STAGE_P48, index, query));
    }
    return decision;
}

static PolicyDecision make_staged_extreme(QueryCache *cache,
                                          const Ivf8Index *index,
                                          const int16_t query[IVF8_INDEX_DIMS],
                                          bool oracle_approved,
                                          uint64_t oracle_ns) {
    const StageKind stages[] = {STAGE_P2, STAGE_P4, STAGE_P8, STAGE_P16};
    StageResult *first = ensure_stage(cache, stages[0], index, query);
    PolicyDecision decision = decision_from_stage(stages[0], first);
    for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
        StageResult *stage = ensure_stage(cache, stages[i], index, query);
        if (i != 0) {
            use_expansion(&decision, stages[i], stage);
        }
        uint8_t fraud_count = stage->trace.result.fraud_count;
        if (fraud_count == 0u || fraud_count == 5u) {
            return decision;
        }
    }
    use_fallback(&decision, oracle_approved, oracle_ns);
    return decision;
}

static PolicyDecision make_staged_stable(QueryCache *cache,
                                         const Ivf8Index *index,
                                         const int16_t query[IVF8_INDEX_DIMS],
                                         bool oracle_approved,
                                         uint64_t oracle_ns) {
    const StageKind stages[] = {STAGE_P2, STAGE_P4, STAGE_P8, STAGE_P16};
    StageResult *previous = ensure_stage(cache, stages[0], index, query);
    PolicyDecision decision = decision_from_stage(stages[0], previous);
    for (size_t i = 1; i < sizeof(stages) / sizeof(stages[0]); i++) {
        StageResult *stage = ensure_stage(cache, stages[i], index, query);
        use_expansion(&decision, stages[i], stage);
        if (trace_top5_equal(&previous->trace, &stage->trace)) {
            return decision;
        }
        previous = stage;
    }
    use_fallback(&decision, oracle_approved, oracle_ns);
    return decision;
}

static PolicyDecision make_p8_extreme_fallback(QueryCache *cache,
                                               const Ivf8Index *index,
                                               const int16_t query[IVF8_INDEX_DIMS],
                                               bool oracle_approved,
                                               uint64_t oracle_ns) {
    StageResult *p8 = ensure_stage(cache, STAGE_P8, index, query);
    PolicyDecision decision = decision_from_stage(STAGE_P8, p8);
    uint8_t fraud_count = p8->trace.result.fraud_count;
    if (fraud_count != 0u && fraud_count != 5u) {
        use_fallback(&decision, oracle_approved, oracle_ns);
    }
    return decision;
}

static PolicyDecision make_boundary_staged(QueryCache *cache,
                                           const Ivf8Index *index,
                                           const int16_t query[IVF8_INDEX_DIMS],
                                           bool oracle_approved,
                                           uint64_t oracle_ns,
                                           bool accept_p48) {
    const StageKind stages[] = {STAGE_P8, STAGE_P16, STAGE_P32};
    StageResult *first = ensure_stage(cache, stages[0], index, query);
    PolicyDecision decision = decision_from_stage(stages[0], first);
    for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
        StageResult *stage = ensure_stage(cache, stages[i], index, query);
        if (i != 0) {
            use_expansion(&decision, stages[i], stage);
        }
        uint8_t fraud_count = stage->trace.result.fraud_count;
        if (fraud_count == 0u || fraud_count == 5u) {
            return decision;
        }
    }
    if (accept_p48) {
        use_expansion(&decision, STAGE_P48, ensure_stage(cache, STAGE_P48, index, query));
    } else {
        use_fallback(&decision, oracle_approved, oracle_ns);
    }
    return decision;
}

static PolicyDecision make_margin_boundary(QueryCache *cache,
                                           const Ivf8Index *index,
                                           const int16_t query[IVF8_INDEX_DIMS],
                                           bool oracle_approved,
                                           uint64_t oracle_ns) {
    StageResult *p8 = ensure_stage(cache, STAGE_P8, index, query);
    PolicyDecision decision = decision_from_stage(STAGE_P8, p8);
    uint8_t fraud_count = p8->trace.result.fraud_count;
    bool expand = fraud_count == 2u || fraud_count == 3u ||
                  ((fraud_count == 1u || fraud_count == 4u) && mixed_label_margin(&p8->trace) <= 500000ull);
    if (!expand) {
        return decision;
    }
    StageResult *p32 = ensure_stage(cache, STAGE_P32, index, query);
    use_expansion(&decision, STAGE_P32, p32);
    fraud_count = p32->trace.result.fraud_count;
    if (fraud_count != 0u && fraud_count != 5u) {
        use_fallback(&decision, oracle_approved, oracle_ns);
    }
    return decision;
}

static PolicyDecision make_bbox_single(QueryCache *cache,
                                       StageKind kind,
                                       const Ivf8Index *index,
                                       const int16_t query[IVF8_INDEX_DIMS],
                                       bool oracle_approved,
                                       uint64_t oracle_ns) {
    bool safe = ensure_bbox_certificate(cache, kind, index, query);
    PolicyDecision decision = decision_from_stage(kind, &cache->stages[kind]);
    if (!safe) {
        use_fallback(&decision, oracle_approved, oracle_ns);
    }
    return decision;
}

static PolicyDecision make_bbox_staged(QueryCache *cache,
                                       const Ivf8Index *index,
                                       const int16_t query[IVF8_INDEX_DIMS],
                                       bool oracle_approved,
                                       uint64_t oracle_ns) {
    const StageKind stages[] = {STAGE_P1, STAGE_P2, STAGE_P4, STAGE_P8};
    (void)ensure_bbox_certificate(cache, stages[0], index, query);
    PolicyDecision decision = decision_from_stage(stages[0], &cache->stages[stages[0]]);
    if (cache->stages[stages[0]].proof_safe) {
        return decision;
    }
    for (size_t i = 1; i < sizeof(stages) / sizeof(stages[0]); i++) {
        (void)ensure_bbox_certificate(cache, stages[i], index, query);
        use_expansion(&decision, stages[i], &cache->stages[stages[i]]);
        if (cache->stages[stages[i]].proof_safe) {
            return decision;
        }
    }
    use_fallback(&decision, oracle_approved, oracle_ns);
    return decision;
}

int main(int argc, char **argv) {
    const char *index_path = NULL;
    const char *tree_path = NULL;
    const char *test_data_path = NULL;
    const char *vectors_csv_path = NULL;
    int limit = 0;
    bool touch = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--index") == 0 && i + 1 < argc) {
            index_path = argv[++i];
        } else if (strcmp(argv[i], "--tree") == 0 && i + 1 < argc) {
            tree_path = argv[++i];
        } else if (strcmp(argv[i], "--test-data") == 0 && i + 1 < argc) {
            test_data_path = argv[++i];
        } else if (strcmp(argv[i], "--vectors-csv") == 0 && i + 1 < argc) {
            vectors_csv_path = argv[++i];
        } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            limit = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--touch") == 0) {
            touch = true;
        } else {
            usage();
            return 2;
        }
    }
    if (index_path == NULL || tree_path == NULL ||
        (test_data_path == NULL && vectors_csv_path == NULL) ||
        (test_data_path != NULL && vectors_csv_path != NULL)) {
        usage();
        return 2;
    }

    size_t capacity = limit > 0 ? (size_t)limit : DEFAULT_CAPACITY;
    PolicyStats stats[POLICY_COUNT];
    for (size_t i = 0; i < POLICY_COUNT; i++) {
        if (policy_stats_init(&stats[i], POLICY_NAMES[i], capacity) != 0) {
            fprintf(stderr, "evaluate_adaptive_probe: stats allocation failed\n");
            return 1;
        }
    }
    uint64_t *oracle_ns_samples = (uint64_t *)calloc(capacity, sizeof(uint64_t));
    if (oracle_ns_samples == NULL) {
        fprintf(stderr, "evaluate_adaptive_probe: oracle allocation failed\n");
        return 1;
    }

    char err[256];
    Ivf8Index index;
    if (ivf8_index_open(index_path, &index, err, sizeof(err)) != 0) {
        fprintf(stderr, "evaluate_adaptive_probe: %s\n", err);
        return 1;
    }
    KdClass3Index tree;
    if (kdclass3_open(tree_path, &tree, err, sizeof(err)) != 0) {
        fprintf(stderr, "evaluate_adaptive_probe: %s\n", err);
        ivf8_index_close(&index);
        return 1;
    }
    if (!ivf8_cpu_supports_avx2()) {
        fprintf(stderr, "evaluate_adaptive_probe: AVX2 is required for this lab\n");
        kdclass3_close(&tree);
        ivf8_index_close(&index);
        return 1;
    }
    if (touch) {
        printf("touch_ivf8_checksum=%" PRIu64 "\n", ivf8_index_touch_pages(&index));
        printf("touch_kdclass3_checksum=%" PRIu64 "\n", kdclass3_touch_pages(&tree));
    }

    char *json = NULL;
    size_t json_len = 0;
    const char *input_path = vectors_csv_path != NULL ? vectors_csv_path : test_data_path;
    if (read_file(input_path, &json, &json_len) != 0) {
        kdclass3_close(&tree);
        ivf8_index_close(&index);
        return 1;
    }

    char *mutable_cursor = json;
    const char *cursor = json;
    const char *end = json + json_len;
    uint64_t total = 0;
    uint64_t oracle_ns_sum = 0;
    uint64_t oracle_fallbacks = 0;
    uint64_t vectorize_errors = 0;
    while ((limit <= 0 || total < (uint64_t)limit) && total < capacity) {
        int expected = 0;
        int16_t query[IVF8_INDEX_DIMS];
        int next;
        if (vectors_csv_path != NULL) {
            next = next_csv_vector(&mutable_cursor, end, query, &expected);
        } else {
            Slice request;
            next = next_entry(&cursor, end, &request, &expected);
            if (next > 0 && !fastvector_vectorize(request.data, request.len, query)) {
                vectorize_errors++;
                total++;
                continue;
            }
        }
        if (next == 0) {
            break;
        }
        if (next < 0) {
            fprintf(stderr, "evaluate_adaptive_probe: parse error near row %" PRIu64 "\n", total);
            break;
        }
        uint64_t oracle_start = now_ns();
        KdClass3SearchResult oracle = kdclass3_search(&tree, query);
        uint64_t oracle_ns = now_ns() - oracle_start;
        oracle_ns_samples[total] = oracle_ns;
        oracle_ns_sum += oracle_ns;
        if (oracle.fallback_required) {
            oracle_fallbacks++;
        }
        bool oracle_approved = oracle.fallback_required ? expected != 0 : oracle.fraud_count < 3u;

        QueryCache cache;
        memset(&cache, 0, sizeof(cache));
        PolicyDecision decisions[POLICY_COUNT];
        decisions[POLICY_DEFAULT8] = make_default(&cache, &index, query);
        decisions[POLICY_COMPETITOR_P10_PATTERN48] = make_competitor_pattern(&cache, &index, query);
        decisions[POLICY_AMBIGUOUS_P12_TO_P48] = make_ambiguous12(&cache, &index, query);
        decisions[POLICY_STAGED_EXTREME] =
            make_staged_extreme(&cache, &index, query, oracle_approved, oracle_ns);
        decisions[POLICY_STAGED_STABLE_TOP5] =
            make_staged_stable(&cache, &index, query, oracle_approved, oracle_ns);
        decisions[POLICY_P8_EXTREME_FALLBACK] =
            make_p8_extreme_fallback(&cache, &index, query, oracle_approved, oracle_ns);
        decisions[POLICY_BOUNDARY_P8_P16_P32_FALLBACK] =
            make_boundary_staged(&cache, &index, query, oracle_approved, oracle_ns, false);
        decisions[POLICY_BOUNDARY_P8_P16_P32_P48_ACCEPT] =
            make_boundary_staged(&cache, &index, query, oracle_approved, oracle_ns, true);
        decisions[POLICY_MARGIN_P8_P32_FALLBACK] =
            make_margin_boundary(&cache, &index, query, oracle_approved, oracle_ns);
        decisions[POLICY_BBOX_P1] =
            make_bbox_single(&cache, STAGE_P1, &index, query, oracle_approved, oracle_ns);
        decisions[POLICY_BBOX_P2] =
            make_bbox_single(&cache, STAGE_P2, &index, query, oracle_approved, oracle_ns);
        decisions[POLICY_BBOX_P4] =
            make_bbox_single(&cache, STAGE_P4, &index, query, oracle_approved, oracle_ns);
        decisions[POLICY_BBOX_P8] =
            make_bbox_single(&cache, STAGE_P8, &index, query, oracle_approved, oracle_ns);
        decisions[POLICY_BBOX_STAGED] =
            make_bbox_staged(&cache, &index, query, oracle_approved, oracle_ns);

        for (size_t i = 0; i < POLICY_COUNT; i++) {
            record_decision(&stats[i], &decisions[i], expected != 0, oracle_approved, total);
        }
        total++;
        if (total % 1000u == 0) {
            fprintf(stderr, "evaluate_adaptive_probe: evaluated=%" PRIu64 "\n", total);
        }
    }

    qsort(oracle_ns_samples, (size_t)total, sizeof(uint64_t), compare_u64);
    printf("adaptive_probe_lab=1\n");
    printf("dataset=%s\n", vectors_csv_path != NULL ? "vectors_csv" : "official_json");
    printf("notes=expansions_are_full_reruns;competitor_thresholds_are_unmodified_external_rules;"
           "bbox_certificate_rejects_any_radius_prune\n");
    printf("evaluated=%" PRIu64 " vectorize_errors=%" PRIu64 " kdclass3_fallbacks=%" PRIu64 "\n",
           total, vectorize_errors, oracle_fallbacks);
    printf("ivf8_file_mib=%.2f kdclass3_file_mib=%.2f combined_mib=%.2f\n",
           (double)index.file_size / 1048576.0, (double)tree.file_size / 1048576.0,
           ((double)index.file_size + (double)tree.file_size) / 1048576.0);
    print_sample_stats("kdclass3_oracle", oracle_ns_samples, (size_t)total, oracle_ns_sum);
    for (size_t i = 0; i < POLICY_COUNT; i++) {
        print_policy(&stats[i]);
    }

    free(json);
    free(oracle_ns_samples);
    for (size_t i = 0; i < POLICY_COUNT; i++) {
        policy_stats_free(&stats[i]);
    }
    kdclass3_close(&tree);
    ivf8_index_close(&index);
    return vectorize_errors == 0 && oracle_fallbacks == 0 ? 0 : 1;
}
