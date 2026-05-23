#define _POSIX_C_SOURCE 200809L

#include "fastvector.h"
#include "ivf8_index.h"
#include "ivf8_search.h"
#include "kdtree.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_ATOMS 12000u
#define MAX_RESULTS 24u
#define MAX_PAIR_RESULTS 512u
#define TOP_ATOMS_FOR_COMBOS 512u
#define TOP_PAIRS_FOR_TRIPLES 256u
#define MAX_PRINTED_WRONG 32u
#define MAX_THRESHOLDS 96u

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
    FEATURE_COUNT
} FeatureKind;

typedef enum {
    OP_GE = 0,
    OP_LE = 1
} FeatureOp;

typedef struct {
    int query_index;
    int expected_approved;
    uint8_t ivf8_fraud_count;
    uint8_t kd_fraud_count;
    uint8_t pattern;
    uint8_t labels[IVF8_SEARCH_TOP_K];
    uint64_t distances[IVF8_SEARCH_TOP_K];
    uint64_t features[FEATURE_COUNT];
    uint64_t ivf8_ns;
    uint64_t kd_ns;
    uint32_t candidates;
    uint32_t clusters;
    uint32_t largest_cluster;
    bool ivf8_wrong;
    bool kd_wrong;
} RepairRecord;

typedef struct {
    char name[160];
    uint64_t *bits;
    int repair_count;
    int wrong_covered;
} Atom;

typedef struct {
    int atom_count;
    int atoms[3];
    int repair_count;
    int wrong_covered;
} PolicyCandidate;

typedef struct {
    PolicyCandidate candidates[MAX_RESULTS];
    int count;
    int wrong_total;
} ResultSet;

typedef struct {
    uint64_t *words;
    size_t word_count;
} Bitset;

static void usage(void) {
    fprintf(stderr,
            "usage: analyze_repair_policy --index <index.bin> --tree <tree.bin> "
            "--test-data <test-data.json> [--limit N] [--output-csv path]\n");
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
        fprintf(stderr, "analyze_repair_policy: open %s: %s\n", path, strerror(errno));
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

static size_t percentile_index(size_t n, double percentile) {
    if (n == 0) {
        return 0;
    }
    size_t idx = (size_t)((percentile / 100.0) * (double)(n - 1u) + 0.5);
    return idx < n ? idx : n - 1u;
}

static uint64_t mixed_label_margin(const Ivf8Neighbor top[IVF8_SEARCH_TOP_K]) {
    uint64_t nearest_legit = UINT64_MAX;
    uint64_t nearest_fraud = UINT64_MAX;
    for (uint32_t i = 0; i < IVF8_SEARCH_TOP_K; i++) {
        if (top[i].fraud != 0u) {
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

static uint8_t top5_pattern(const Ivf8Neighbor top[IVF8_SEARCH_TOP_K]) {
    uint8_t pattern = 0;
    for (uint32_t i = 0; i < IVF8_SEARCH_TOP_K; i++) {
        if (top[i].fraud != 0u) {
            pattern |= (uint8_t)(1u << i);
        }
    }
    return pattern;
}

static void fill_features(RepairRecord *record, const Ivf8SearchTraceResult *trace) {
    for (uint32_t i = 0; i < IVF8_SEARCH_TOP_K; i++) {
        record->labels[i] = trace->top[i].fraud != 0u ? 1u : 0u;
        record->distances[i] = trace->top[i].distance;
    }
    uint64_t d0 = record->distances[0];
    uint64_t d1 = record->distances[1];
    uint64_t d2 = record->distances[2];
    uint64_t d3 = record->distances[3];
    uint64_t d4 = record->distances[4];
    record->features[FEATURE_BEST] = d0;
    record->features[FEATURE_WORST] = d4;
    record->features[FEATURE_SPREAD] = d4 >= d0 ? d4 - d0 : 0u;
    record->features[FEATURE_GAP10] = d1 >= d0 ? d1 - d0 : 0u;
    record->features[FEATURE_GAP21] = d2 >= d1 ? d2 - d1 : 0u;
    record->features[FEATURE_GAP32] = d3 >= d2 ? d3 - d2 : 0u;
    record->features[FEATURE_GAP43] = d4 >= d3 ? d4 - d3 : 0u;
    record->features[FEATURE_MIXED_MARGIN] = mixed_label_margin(trace->top);
    record->features[FEATURE_CANDIDATES] = trace->result.stats.candidates_scanned;
    record->features[FEATURE_CLUSTERS] = trace->result.stats.clusters_scanned;
    record->features[FEATURE_LARGEST_CLUSTER] = trace->result.stats.largest_scanned_cluster_candidates;
}

static const char *feature_name(FeatureKind feature) {
    switch (feature) {
        case FEATURE_WORST:
            return "worst";
        case FEATURE_BEST:
            return "best";
        case FEATURE_SPREAD:
            return "spread";
        case FEATURE_GAP10:
            return "gap10";
        case FEATURE_GAP21:
            return "gap21";
        case FEATURE_GAP32:
            return "gap32";
        case FEATURE_GAP43:
            return "gap43";
        case FEATURE_MIXED_MARGIN:
            return "mixed_margin";
        case FEATURE_CANDIDATES:
            return "candidates";
        case FEATURE_CLUSTERS:
            return "clusters";
        case FEATURE_LARGEST_CLUSTER:
            return "largest_cluster";
        case FEATURE_COUNT:
            break;
    }
    return "unknown";
}

static bool bitset_alloc(Bitset *bitset, size_t bit_count) {
    bitset->word_count = (bit_count + 63u) / 64u;
    bitset->words = (uint64_t *)calloc(bitset->word_count, sizeof(uint64_t));
    return bitset->words != NULL;
}

static void bitset_free(Bitset *bitset) {
    free(bitset->words);
    bitset->words = NULL;
    bitset->word_count = 0;
}

static void bitset_set(Bitset *bitset, size_t index) {
    bitset->words[index / 64u] |= 1ull << (index % 64u);
}

static bool bitset_get(const uint64_t *words, size_t index) {
    return (words[index / 64u] & (1ull << (index % 64u))) != 0u;
}

static int popcount64(uint64_t value) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(value);
#else
    int count = 0;
    while (value != 0u) {
        count += (int)(value & 1u);
        value >>= 1u;
    }
    return count;
#endif
}

static int atom_score(const Atom *atom, const Bitset *wrong_bits) {
    int covered = 0;
    for (size_t i = 0; i < wrong_bits->word_count; i++) {
        covered += popcount64(atom->bits[i] & wrong_bits->words[i]);
    }
    return covered;
}

static int atom_repair_count(const Atom *atom, size_t word_count) {
    int count = 0;
    for (size_t i = 0; i < word_count; i++) {
        count += popcount64(atom->bits[i]);
    }
    return count;
}

static bool fc_mask_has(uint8_t mask, uint8_t fraud_count) {
    return fraud_count < 8u && (mask & (uint8_t)(1u << fraud_count)) != 0u;
}

static const char *fc_mask_name(uint8_t mask, char *buf, size_t cap) {
    size_t used = 0;
    used += (size_t)snprintf(buf + used, cap - used, "fc{");
    bool first = true;
    for (uint8_t i = 0; i <= 5u; i++) {
        if (fc_mask_has(mask, i)) {
            used += (size_t)snprintf(buf + used, cap - used, "%s%u", first ? "" : "_", i);
            first = false;
        }
    }
    (void)snprintf(buf + used, cap - used, "}");
    return buf;
}

static int add_atom(Atom *atoms,
                    int *atom_count,
                    size_t record_count,
                    const RepairRecord *records,
                    const Bitset *wrong_bits,
                    const char *name,
                    bool (*predicate)(const RepairRecord *record, void *ctx),
                    void *ctx) {
    if (*atom_count >= (int)MAX_ATOMS) {
        return 0;
    }
    Atom *atom = &atoms[*atom_count];
    memset(atom, 0, sizeof(*atom));
    (void)snprintf(atom->name, sizeof(atom->name), "%s", name);
    size_t word_count = (record_count + 63u) / 64u;
    atom->bits = (uint64_t *)calloc(word_count, sizeof(uint64_t));
    if (atom->bits == NULL) {
        return -1;
    }
    for (size_t i = 0; i < record_count; i++) {
        if (predicate(&records[i], ctx)) {
            atom->bits[i / 64u] |= 1ull << (i % 64u);
        }
    }
    atom->repair_count = atom_repair_count(atom, word_count);
    atom->wrong_covered = atom_score(atom, wrong_bits);
    if (atom->repair_count == 0 || atom->wrong_covered == 0) {
        free(atom->bits);
        atom->bits = NULL;
        return 0;
    }
    (*atom_count)++;
    return 0;
}

typedef struct {
    uint8_t mask;
} CountCtx;

static bool pred_count(const RepairRecord *record, void *ctx) {
    CountCtx *c = (CountCtx *)ctx;
    return fc_mask_has(c->mask, record->ivf8_fraud_count);
}

typedef struct {
    uint8_t mask;
    FeatureKind feature;
    FeatureOp op;
    uint64_t threshold;
} FeatureCtx;

static bool pred_count_feature(const RepairRecord *record, void *ctx) {
    FeatureCtx *c = (FeatureCtx *)ctx;
    if (!fc_mask_has(c->mask, record->ivf8_fraud_count)) {
        return false;
    }
    uint64_t value = record->features[c->feature];
    return c->op == OP_GE ? value >= c->threshold : value <= c->threshold;
}

typedef struct {
    uint8_t pattern;
} PatternCtx;

static bool pred_pattern(const RepairRecord *record, void *ctx) {
    PatternCtx *c = (PatternCtx *)ctx;
    return record->pattern == c->pattern;
}

typedef struct {
    uint8_t pattern;
    FeatureKind feature;
    FeatureOp op;
    uint64_t threshold;
} PatternFeatureCtx;

static bool pred_pattern_feature(const RepairRecord *record, void *ctx) {
    PatternFeatureCtx *c = (PatternFeatureCtx *)ctx;
    if (record->pattern != c->pattern) {
        return false;
    }
    uint64_t value = record->features[c->feature];
    return c->op == OP_GE ? value >= c->threshold : value <= c->threshold;
}

static int candidate_errors(const PolicyCandidate *candidate, int wrong_total) {
    return wrong_total - candidate->wrong_covered;
}

static bool result_better(const PolicyCandidate *a, const PolicyCandidate *b, int wrong_total) {
    int ae = candidate_errors(a, wrong_total);
    int be = candidate_errors(b, wrong_total);
    if (ae != be) {
        return ae < be;
    }
    if (a->repair_count != b->repair_count) {
        return a->repair_count < b->repair_count;
    }
    return a->atom_count < b->atom_count;
}

static bool same_candidate(const PolicyCandidate *a, const PolicyCandidate *b) {
    if (a->atom_count != b->atom_count) {
        return false;
    }
    for (int i = 0; i < a->atom_count; i++) {
        if (a->atoms[i] != b->atoms[i]) {
            return false;
        }
    }
    return true;
}

static void result_set_add(ResultSet *set, PolicyCandidate candidate) {
    for (int i = 0; i < set->count; i++) {
        if (same_candidate(&set->candidates[i], &candidate)) {
            return;
        }
    }
    int pos = set->count;
    if (pos < (int)MAX_RESULTS) {
        set->count++;
    } else if (!result_better(&candidate, &set->candidates[set->count - 1], set->wrong_total)) {
        return;
    } else {
        pos = set->count - 1;
    }
    while (pos > 0 && result_better(&candidate, &set->candidates[pos - 1], set->wrong_total)) {
        set->candidates[pos] = set->candidates[pos - 1];
        pos--;
    }
    set->candidates[pos] = candidate;
}

static void record_candidate(ResultSet *all,
                             ResultSet *under1,
                             ResultSet *under05,
                             int under1_limit,
                             int under05_limit,
                             PolicyCandidate candidate) {
    result_set_add(all, candidate);
    if (candidate.repair_count <= under1_limit) {
        result_set_add(under1, candidate);
    }
    if (candidate.repair_count <= under05_limit) {
        result_set_add(under05, candidate);
    }
}

static int compare_atoms(const void *a, const void *b) {
    const Atom *aa = (const Atom *)a;
    const Atom *bb = (const Atom *)b;
    if (aa->wrong_covered != bb->wrong_covered) {
        return bb->wrong_covered - aa->wrong_covered;
    }
    if (aa->repair_count != bb->repair_count) {
        return aa->repair_count - bb->repair_count;
    }
    return strcmp(aa->name, bb->name);
}

static void union_score_two(const Atom *a,
                            const Atom *b,
                            size_t word_count,
                            const Bitset *wrong_bits,
                            int *repair_count,
                            int *wrong_covered) {
    int repairs = 0;
    int covered = 0;
    for (size_t w = 0; w < word_count; w++) {
        uint64_t bits = a->bits[w] | b->bits[w];
        repairs += popcount64(bits);
        covered += popcount64(bits & wrong_bits->words[w]);
    }
    *repair_count = repairs;
    *wrong_covered = covered;
}

static void union_score_three(const Atom *a,
                              const Atom *b,
                              const Atom *c,
                              size_t word_count,
                              const Bitset *wrong_bits,
                              int *repair_count,
                              int *wrong_covered) {
    int repairs = 0;
    int covered = 0;
    for (size_t w = 0; w < word_count; w++) {
        uint64_t bits = a->bits[w] | b->bits[w] | c->bits[w];
        repairs += popcount64(bits);
        covered += popcount64(bits & wrong_bits->words[w]);
    }
    *repair_count = repairs;
    *wrong_covered = covered;
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

static void add_threshold(uint64_t *thresholds, size_t *count, uint64_t value) {
    for (size_t i = 0; i < *count; i++) {
        if (thresholds[i] == value) {
            return;
        }
    }
    if (*count < MAX_THRESHOLDS) {
        thresholds[*count] = value;
        (*count)++;
    }
}

static size_t collect_thresholds(FeatureKind feature,
                                 const RepairRecord *records,
                                 size_t record_count,
                                 const uint64_t *base,
                                 size_t base_count,
                                 uint64_t out[MAX_THRESHOLDS]) {
    size_t count = 0;
    for (size_t i = 0; i < base_count; i++) {
        add_threshold(out, &count, base[i]);
    }
    for (size_t i = 0; i < record_count; i++) {
        if (!records[i].ivf8_wrong) {
            continue;
        }
        uint64_t value = records[i].features[feature];
        add_threshold(out, &count, value);
        if (value > 0u) {
            add_threshold(out, &count, value - 1u);
        }
        if (value != UINT64_MAX) {
            add_threshold(out, &count, value + 1u);
        }
    }
    return count;
}

static void write_csv_header(FILE *file) {
    fprintf(file,
            "row_index,expected_approved,ivf8_fraud_count,kd_fraud_count,ivf8_approved,kd_approved,"
            "is_fp,is_fn,pattern,label0,label1,label2,label3,label4,d0,d1,d2,d3,d4,"
            "best_distance,worst_distance,gap10,gap21,gap32,gap43,spread,mixed_margin,"
            "candidate_count,cluster_count,largest_cluster,ivf8_us,kd_us\n");
}

static void write_csv_row(FILE *file, const RepairRecord *r) {
    int ivf8_approved = r->ivf8_fraud_count < 3u ? 1 : 0;
    int kd_approved = r->kd_fraud_count < 3u ? 1 : 0;
    int is_fp = r->expected_approved && !ivf8_approved;
    int is_fn = !r->expected_approved && ivf8_approved;
    fprintf(file,
            "%d,%d,%u,%u,%d,%d,%d,%d,%u,%u,%u,%u,%u,%u,"
            "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
            "%u,%u,%u,%.3f,%.3f\n",
            r->query_index,
            r->expected_approved,
            r->ivf8_fraud_count,
            r->kd_fraud_count,
            ivf8_approved,
            kd_approved,
            is_fp,
            is_fn,
            r->pattern,
            r->labels[0],
            r->labels[1],
            r->labels[2],
            r->labels[3],
            r->labels[4],
            (unsigned long long)r->distances[0],
            (unsigned long long)r->distances[1],
            (unsigned long long)r->distances[2],
            (unsigned long long)r->distances[3],
            (unsigned long long)r->distances[4],
            (unsigned long long)r->features[FEATURE_BEST],
            (unsigned long long)r->features[FEATURE_WORST],
            (unsigned long long)r->features[FEATURE_GAP10],
            (unsigned long long)r->features[FEATURE_GAP21],
            (unsigned long long)r->features[FEATURE_GAP32],
            (unsigned long long)r->features[FEATURE_GAP43],
            (unsigned long long)r->features[FEATURE_SPREAD],
            (unsigned long long)r->features[FEATURE_MIXED_MARGIN],
            r->candidates,
            r->clusters,
            r->largest_cluster,
            ns_to_us(r->ivf8_ns),
            ns_to_us(r->kd_ns));
}

static void print_feature_summary(const RepairRecord *records, size_t count, bool wrong_only) {
    printf("%s_feature_summary_begin\n", wrong_only ? "wrong" : "correct");
    for (FeatureKind f = 0; f < FEATURE_COUNT; f++) {
        uint64_t *values = (uint64_t *)malloc(count * sizeof(uint64_t));
        if (values == NULL) {
            return;
        }
        size_t n = 0;
        for (size_t i = 0; i < count; i++) {
            if (records[i].ivf8_wrong == wrong_only) {
                values[n++] = records[i].features[f];
            }
        }
        if (n > 0) {
            qsort(values, n, sizeof(uint64_t), compare_u64);
            size_t p50 = percentile_index(n, 50.0);
            size_t p95 = percentile_index(n, 95.0);
            printf("%s_min=%llu %s_p50=%llu %s_p95=%llu %s_max=%llu\n",
                   feature_name(f), (unsigned long long)values[0],
                   feature_name(f), (unsigned long long)values[p50],
                   feature_name(f), (unsigned long long)values[p95],
                   feature_name(f), (unsigned long long)values[n - 1u]);
        }
        free(values);
    }
    printf("%s_feature_summary_end\n", wrong_only ? "wrong" : "correct");
}

static void evaluate_policy_exact(const PolicyCandidate *candidate,
                                  const Atom *atoms,
                                  const RepairRecord *records,
                                  size_t count,
                                  const char *label) {
    int tp = 0;
    int tn = 0;
    int fp = 0;
    int fn = 0;
    int repairs = 0;
    uint64_t *effective = (uint64_t *)malloc(count * sizeof(uint64_t));
    if (effective == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        bool repair = false;
        for (int a = 0; a < candidate->atom_count; a++) {
            if (bitset_get(atoms[candidate->atoms[a]].bits, i)) {
                repair = true;
                break;
            }
        }
        uint8_t final_fc = repair ? records[i].kd_fraud_count : records[i].ivf8_fraud_count;
        int approved = final_fc < 3u ? 1 : 0;
        if (repair) {
            repairs++;
        }
        effective[i] = records[i].ivf8_ns + (repair ? records[i].kd_ns : 0u);
        if (approved == records[i].expected_approved) {
            if (approved) {
                tn++;
            } else {
                tp++;
            }
        } else if (approved) {
            fn++;
        } else {
            fp++;
        }
    }
    qsort(effective, count, sizeof(uint64_t), compare_u64);
    size_t p99 = percentile_index(count, 99.0);
    size_t p95 = percentile_index(count, 95.0);
    int weighted = fp + fn * 3;
    printf("policy_result name=\"%s\" repairs=%d repair_rate=%.6f TP=%d TN=%d FP=%d FN=%d weighted=%d detection=%.2f p95_effective_us=%.3f p99_effective_us=%.3f atoms=\"",
           label,
           repairs,
           count > 0 ? (double)repairs / (double)count : 0.0,
           tp,
           tn,
           fp,
           fn,
           weighted,
           detection_score((int)count, fp, fn, 0),
           ns_to_us(effective[p95]),
           ns_to_us(effective[p99]));
    for (int i = 0; i < candidate->atom_count; i++) {
        printf("%s%s", i == 0 ? "" : " OR ", atoms[candidate->atoms[i]].name);
    }
    printf("\"\n");
    free(effective);
}

static void print_result_set(const char *title,
                             const ResultSet *set,
                             const Atom *atoms,
                             const RepairRecord *records,
                             size_t count) {
    printf("%s_begin\n", title);
    for (int i = 0; i < set->count; i++) {
        char label[512];
        size_t used = 0;
        for (int a = 0; a < set->candidates[i].atom_count; a++) {
            used += (size_t)snprintf(label + used,
                                     sizeof(label) - used,
                                     "%s%s",
                                     a == 0 ? "" : " OR ",
                                     atoms[set->candidates[i].atoms[a]].name);
        }
        evaluate_policy_exact(&set->candidates[i], atoms, records, count, label);
    }
    printf("%s_end\n", title);
}

int main(int argc, char **argv) {
    const char *index_path = NULL;
    const char *tree_path = NULL;
    const char *test_data_path = NULL;
    const char *csv_path = NULL;
    int limit = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--index") == 0 && i + 1 < argc) {
            index_path = argv[++i];
        } else if (strcmp(argv[i], "--tree") == 0 && i + 1 < argc) {
            tree_path = argv[++i];
        } else if (strcmp(argv[i], "--test-data") == 0 && i + 1 < argc) {
            test_data_path = argv[++i];
        } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            limit = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--output-csv") == 0 && i + 1 < argc) {
            csv_path = argv[++i];
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
        fprintf(stderr, "analyze_repair_policy: %s\n", err);
        return 1;
    }
    KdTree tree;
    if (kdtree_load_nodes_for_ivf8(&tree, &index, tree_path) != 0) {
        fprintf(stderr, "analyze_repair_policy: load tree %s failed: %s\n", tree_path, strerror(errno));
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

    size_t capacity = 65536u;
    RepairRecord *records = (RepairRecord *)calloc(capacity, sizeof(RepairRecord));
    if (records == NULL) {
        free(data);
        kdtree_free(&tree);
        ivf8_index_close(&index);
        return 1;
    }

    Ivf8SearchConfig ivf8_cfg = {
        .max_candidates = IVF8_SEARCH_DEFAULT_MAX_CANDIDATES,
        .probes = IVF8_SEARCH_DEFAULT_PROBES,
        .impl = ivf8_cpu_supports_avx2() ? IVF8_SEARCH_IMPL_AVX2 : IVF8_SEARCH_IMPL_SCALAR,
    };
    KdTreeSearchConfig kd_cfg = {.max_visited = 0};

    const char *cursor = data;
    const char *end = data + data_len;
    size_t count = 0;
    int parse_errors = 0;
    int kd_wrong = 0;
    int ivf_wrong = 0;
    int ivf_wrong_by_fc[6] = {0};
    int correct_by_fc[6] = {0};
    int wrong_by_pattern[32] = {0};
    int correct_by_pattern[32] = {0};

    for (;;) {
        if (limit > 0 && (int)count >= limit) {
            break;
        }
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
        if (count == capacity) {
            size_t next_capacity = capacity * 2u;
            RepairRecord *next_records = (RepairRecord *)realloc(records, next_capacity * sizeof(RepairRecord));
            if (next_records == NULL) {
                fprintf(stderr, "analyze_repair_policy: out of memory\n");
                parse_errors++;
                break;
            }
            records = next_records;
            memset(records + capacity, 0, (next_capacity - capacity) * sizeof(RepairRecord));
            capacity = next_capacity;
        }
        int16_t query[IVF8_INDEX_DIMS];
        if (!fastvector_vectorize(request.data, request.len, query)) {
            parse_errors++;
            continue;
        }

        uint64_t ivf_start = now_ns();
        Ivf8SearchTraceResult trace = ivf8_search_trace(&index, query, &ivf8_cfg);
        uint64_t ivf_ns = now_ns() - ivf_start;

        uint64_t kd_start = now_ns();
        KdTreeSearchResult kd = kdtree_search_top5(&tree, query, &kd_cfg);
        uint64_t kd_ns = now_ns() - kd_start;

        RepairRecord *r = &records[count];
        r->query_index = (int)count;
        r->expected_approved = expected_approved;
        r->ivf8_fraud_count = trace.result.fraud_count;
        r->kd_fraud_count = kd.fraud_count;
        r->pattern = top5_pattern(trace.top);
        r->ivf8_ns = ivf_ns;
        r->kd_ns = kd_ns;
        r->candidates = trace.result.stats.candidates_scanned;
        r->clusters = trace.result.stats.clusters_scanned;
        r->largest_cluster = trace.result.stats.largest_scanned_cluster_candidates;
        fill_features(r, &trace);

        int ivf_approved = r->ivf8_fraud_count < 3u ? 1 : 0;
        int kd_approved = r->kd_fraud_count < 3u ? 1 : 0;
        r->ivf8_wrong = ivf_approved != expected_approved;
        r->kd_wrong = kd_approved != expected_approved;
        if (r->ivf8_wrong) {
            ivf_wrong++;
            if (r->ivf8_fraud_count < 6u) {
                ivf_wrong_by_fc[r->ivf8_fraud_count]++;
            }
            wrong_by_pattern[r->pattern]++;
        } else {
            if (r->ivf8_fraud_count < 6u) {
                correct_by_fc[r->ivf8_fraud_count]++;
            }
            correct_by_pattern[r->pattern]++;
        }
        if (r->kd_wrong) {
            kd_wrong++;
        }
        count++;
    }

    FILE *csv = NULL;
    if (csv_path != NULL) {
        csv = fopen(csv_path, "wb");
        if (csv == NULL) {
            fprintf(stderr, "analyze_repair_policy: open %s: %s\n", csv_path, strerror(errno));
        } else {
            write_csv_header(csv);
            for (size_t i = 0; i < count; i++) {
                write_csv_row(csv, &records[i]);
            }
            fclose(csv);
        }
    }

    printf("evaluated=%zu\n", count);
    printf("parse_errors=%d\n", parse_errors);
    printf("ivf8_impl=%s\n", ivf8_search_impl_name(ivf8_cfg.impl));
    printf("ivf8_wrong_total=%d\n", ivf_wrong);
    printf("kd_wrong_total=%d\n", kd_wrong);
    for (uint8_t fc = 0; fc <= 5u; fc++) {
        printf("wrong_by_fraud_count_%u=%d correct_by_fraud_count_%u=%d\n",
               fc, ivf_wrong_by_fc[fc], fc, correct_by_fc[fc]);
    }
    printf("wrong_patterns_begin\n");
    for (int p = 0; p < 32; p++) {
        if (wrong_by_pattern[p] > 0) {
            printf("pattern=%d wrong=%d correct=%d\n", p, wrong_by_pattern[p], correct_by_pattern[p]);
        }
    }
    printf("wrong_patterns_end\n");
    printf("first_wrong_cases_begin\n");
    int printed = 0;
    for (size_t i = 0; i < count && printed < (int)MAX_PRINTED_WRONG; i++) {
        if (records[i].ivf8_wrong) {
            printf("wrong idx=%d expected=%d ivf8_fc=%u kd_fc=%u pattern=%u distances=%llu/%llu/%llu/%llu/%llu worst=%llu spread=%llu margin=%llu candidates=%u clusters=%u largest=%u\n",
                   records[i].query_index,
                   records[i].expected_approved,
                   records[i].ivf8_fraud_count,
                   records[i].kd_fraud_count,
                   records[i].pattern,
                   (unsigned long long)records[i].distances[0],
                   (unsigned long long)records[i].distances[1],
                   (unsigned long long)records[i].distances[2],
                   (unsigned long long)records[i].distances[3],
                   (unsigned long long)records[i].distances[4],
                   (unsigned long long)records[i].features[FEATURE_WORST],
                   (unsigned long long)records[i].features[FEATURE_SPREAD],
                   (unsigned long long)records[i].features[FEATURE_MIXED_MARGIN],
                   records[i].candidates,
                   records[i].clusters,
                   records[i].largest_cluster);
            printed++;
        }
    }
    printf("first_wrong_cases_end\n");
    print_feature_summary(records, count, true);
    print_feature_summary(records, count, false);

    Bitset wrong_bits;
    if (!bitset_alloc(&wrong_bits, count)) {
        free(records);
        free(data);
        kdtree_free(&tree);
        ivf8_index_close(&index);
        return 1;
    }
    for (size_t i = 0; i < count; i++) {
        if (records[i].ivf8_wrong) {
            bitset_set(&wrong_bits, i);
        }
    }

    Atom *atoms = (Atom *)calloc(MAX_ATOMS, sizeof(Atom));
    if (atoms == NULL) {
        bitset_free(&wrong_bits);
        free(records);
        free(data);
        kdtree_free(&tree);
        ivf8_index_close(&index);
        return 1;
    }
    int atom_count = 0;
    const uint8_t masks[] = {
        (uint8_t)(1u << 0), (uint8_t)(1u << 1), (uint8_t)(1u << 2),
        (uint8_t)(1u << 3), (uint8_t)(1u << 4), (uint8_t)(1u << 5),
        (uint8_t)((1u << 2) | (1u << 3)),
        (uint8_t)((1u << 4) | (1u << 5)),
        (uint8_t)((1u << 2) | (1u << 3) | (1u << 4)),
        (uint8_t)((1u << 2) | (1u << 3) | (1u << 4) | (1u << 5)),
        (uint8_t)((1u << 1) | (1u << 2) | (1u << 3) | (1u << 4)),
        (uint8_t)((1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5)),
    };
    const uint64_t distance_thresholds[] = {
        50000ull, 100000ull, 250000ull, 500000ull, 750000ull, 1000000ull,
        1500000ull, 2000000ull, 2500000ull, 3000000ull, 3500000ull,
        4000000ull, 4500000ull, 5000000ull, 6000000ull, 8000000ull,
        10000000ull, 15000000ull, 20000000ull
    };
    const uint64_t count_thresholds[] = {1ull, 2ull, 3ull, 4ull, 5ull, 6ull, 7ull, 8ull, 512ull, 1024ull, 1536ull, 2048ull, 2560ull, 3072ull, 3584ull, 4096ull};

    for (size_t m = 0; m < sizeof(masks) / sizeof(masks[0]); m++) {
        char mask_buf[64];
        CountCtx ctx = {.mask = masks[m]};
        char name[160];
        (void)snprintf(name, sizeof(name), "%s", fc_mask_name(masks[m], mask_buf, sizeof(mask_buf)));
        if (add_atom(atoms, &atom_count, count, records, &wrong_bits, name, pred_count, &ctx) != 0) {
            parse_errors++;
            break;
        }
        for (FeatureKind f = 0; f < FEATURE_COUNT; f++) {
            const uint64_t *base_thresholds = distance_thresholds;
            size_t base_threshold_count = sizeof(distance_thresholds) / sizeof(distance_thresholds[0]);
            if (f == FEATURE_CANDIDATES || f == FEATURE_CLUSTERS || f == FEATURE_LARGEST_CLUSTER) {
                base_thresholds = count_thresholds;
                base_threshold_count = sizeof(count_thresholds) / sizeof(count_thresholds[0]);
            }
            uint64_t thresholds[MAX_THRESHOLDS];
            size_t threshold_count = collect_thresholds(f, records, count, base_thresholds, base_threshold_count, thresholds);
            for (size_t t = 0; t < threshold_count; t++) {
                for (int op = 0; op < 2; op++) {
                    FeatureCtx fctx = {.mask = masks[m], .feature = f, .op = op == 0 ? OP_GE : OP_LE, .threshold = thresholds[t]};
                    (void)snprintf(name, sizeof(name), "%s_%s_%s_%llu",
                                   fc_mask_name(masks[m], mask_buf, sizeof(mask_buf)),
                                   feature_name(f),
                                   op == 0 ? "ge" : "le",
                                   (unsigned long long)thresholds[t]);
                    if (add_atom(atoms, &atom_count, count, records, &wrong_bits, name, pred_count_feature, &fctx) != 0) {
                        parse_errors++;
                        break;
                    }
                }
            }
        }
    }

    for (uint8_t pattern = 0; pattern < 32u && atom_count < (int)MAX_ATOMS; pattern++) {
        if (wrong_by_pattern[pattern] == 0) {
            continue;
        }
        PatternCtx pctx = {.pattern = pattern};
        char name[160];
        (void)snprintf(name, sizeof(name), "pattern_%u", pattern);
        if (add_atom(atoms, &atom_count, count, records, &wrong_bits, name, pred_pattern, &pctx) != 0) {
            parse_errors++;
            break;
        }
        for (FeatureKind f = 0; f < FEATURE_COUNT && atom_count < (int)MAX_ATOMS; f++) {
            const uint64_t *base_thresholds = distance_thresholds;
            size_t base_threshold_count = sizeof(distance_thresholds) / sizeof(distance_thresholds[0]);
            if (f == FEATURE_CANDIDATES || f == FEATURE_CLUSTERS || f == FEATURE_LARGEST_CLUSTER) {
                base_thresholds = count_thresholds;
                base_threshold_count = sizeof(count_thresholds) / sizeof(count_thresholds[0]);
            }
            uint64_t thresholds[MAX_THRESHOLDS];
            size_t threshold_count = collect_thresholds(f, records, count, base_thresholds, base_threshold_count, thresholds);
            for (size_t t = 0; t < threshold_count && atom_count < (int)MAX_ATOMS; t++) {
                for (int op = 0; op < 2 && atom_count < (int)MAX_ATOMS; op++) {
                    PatternFeatureCtx pfctx = {.pattern = pattern, .feature = f, .op = op == 0 ? OP_GE : OP_LE, .threshold = thresholds[t]};
                    (void)snprintf(name, sizeof(name), "pattern_%u_%s_%s_%llu",
                                   pattern,
                                   feature_name(f),
                                   op == 0 ? "ge" : "le",
                                   (unsigned long long)thresholds[t]);
                    if (add_atom(atoms, &atom_count, count, records, &wrong_bits, name, pred_pattern_feature, &pfctx) != 0) {
                        parse_errors++;
                        break;
                    }
                }
            }
        }
    }

    qsort(atoms, (size_t)atom_count, sizeof(Atom), compare_atoms);
    printf("atom_count=%d\n", atom_count);
    printf("top_atoms_begin\n");
    int top_atom_print = atom_count < 20 ? atom_count : 20;
    for (int i = 0; i < top_atom_print; i++) {
        printf("atom rank=%d name=\"%s\" repairs=%d repair_rate=%.6f wrong_covered=%d\n",
               i + 1,
               atoms[i].name,
               atoms[i].repair_count,
               count > 0 ? (double)atoms[i].repair_count / (double)count : 0.0,
               atoms[i].wrong_covered);
    }
    printf("top_atoms_end\n");

    ResultSet results;
    memset(&results, 0, sizeof(results));
    results.wrong_total = ivf_wrong;
    ResultSet under1;
    memset(&under1, 0, sizeof(under1));
    under1.wrong_total = ivf_wrong;
    ResultSet under05;
    memset(&under05, 0, sizeof(under05));
    under05.wrong_total = ivf_wrong;
    int under1_limit = (int)(count / 100u);
    int under05_limit = (int)(count / 200u);

    int combo_atoms = atom_count < (int)TOP_ATOMS_FOR_COMBOS ? atom_count : (int)TOP_ATOMS_FOR_COMBOS;
    for (int i = 0; i < atom_count; i++) {
        PolicyCandidate c;
        memset(&c, 0, sizeof(c));
        c.atom_count = 1;
        c.atoms[0] = i;
        c.repair_count = atoms[i].repair_count;
        c.wrong_covered = atoms[i].wrong_covered;
        record_candidate(&results, &under1, &under05, under1_limit, under05_limit, c);
    }

    PolicyCandidate pair_results[MAX_PAIR_RESULTS];
    int pair_result_count = 0;
    for (int i = 0; i < combo_atoms; i++) {
        for (int j = i + 1; j < combo_atoms; j++) {
            PolicyCandidate c;
            memset(&c, 0, sizeof(c));
            c.atom_count = 2;
            c.atoms[0] = i;
            c.atoms[1] = j;
            union_score_two(&atoms[i], &atoms[j], wrong_bits.word_count, &wrong_bits, &c.repair_count, &c.wrong_covered);
            record_candidate(&results, &under1, &under05, under1_limit, under05_limit, c);
            int pos = pair_result_count;
            if (pos < (int)MAX_PAIR_RESULTS) {
                pair_result_count++;
            } else if (!result_better(&c, &pair_results[pair_result_count - 1], ivf_wrong)) {
                continue;
            } else {
                pos = pair_result_count - 1;
            }
            while (pos > 0 && result_better(&c, &pair_results[pos - 1], ivf_wrong)) {
                pair_results[pos] = pair_results[pos - 1];
                pos--;
            }
            pair_results[pos] = c;
        }
    }

    int triple_pairs = pair_result_count < (int)TOP_PAIRS_FOR_TRIPLES ? pair_result_count : (int)TOP_PAIRS_FOR_TRIPLES;
    int triple_atoms = combo_atoms < 256 ? combo_atoms : 256;
    for (int p = 0; p < triple_pairs; p++) {
        for (int k = 0; k < triple_atoms; k++) {
            if (k == pair_results[p].atoms[0] || k == pair_results[p].atoms[1]) {
                continue;
            }
            PolicyCandidate c;
            memset(&c, 0, sizeof(c));
            c.atom_count = 3;
            c.atoms[0] = pair_results[p].atoms[0];
            c.atoms[1] = pair_results[p].atoms[1];
            c.atoms[2] = k;
            union_score_three(&atoms[c.atoms[0]], &atoms[c.atoms[1]], &atoms[c.atoms[2]],
                              wrong_bits.word_count, &wrong_bits, &c.repair_count, &c.wrong_covered);
            record_candidate(&results, &under1, &under05, under1_limit, under05_limit, c);
        }
    }

    print_result_set("policy_search_top", &results, atoms, records, count);
    print_result_set("policy_search_under1pct", &under1, atoms, records, count);
    print_result_set("policy_search_under05pct", &under05, atoms, records, count);

    for (int i = 0; i < atom_count; i++) {
        free(atoms[i].bits);
    }
    free(atoms);
    bitset_free(&wrong_bits);
    free(records);
    free(data);
    kdtree_free(&tree);
    ivf8_index_close(&index);
    return parse_errors == 0 ? 0 : 1;
}
