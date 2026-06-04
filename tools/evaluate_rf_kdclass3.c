#define _POSIX_C_SOURCE 200809L

#include "fastvector.h"
#include "kdclass3.h"
#include "rf_gate_model.h"

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
    uint64_t *values;
    size_t len;
    size_t cap;
} TimingSeries;

typedef struct {
    uint32_t total;
    uint32_t tp;
    uint32_t tn;
    uint32_t fp;
    uint32_t fn;
    uint32_t errors;
    uint32_t rf_accept_legit;
    uint32_t rf_accept_fraud;
    uint32_t fallback_count;
    uint32_t accepted_errors;
    uint32_t fallback_errors;
    uint32_t kdclass3_ties;
    TimingSeries vectorize_ns;
    TimingSeries rf_ns;
    TimingSeries fallback_ns;
    TimingSeries total_ns;
} EvalStats;

static void usage(void) {
    fprintf(stderr,
            "usage: evaluate_rf_kdclass3 --tree <kdclass3.bin> --test-data <test-data.json> "
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
        fprintf(stderr, "evaluate_rf_kdclass3: open %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "evaluate_rf_kdclass3: seek %s failed\n", path);
        fclose(file);
        return 1;
    }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "evaluate_rf_kdclass3: tell/seek %s failed\n", path);
        fclose(file);
        return 1;
    }
    char *buffer = (char *)malloc((size_t)size + 1u);
    if (buffer == NULL) {
        fprintf(stderr, "evaluate_rf_kdclass3: out of memory\n");
        fclose(file);
        return 1;
    }
    size_t got = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        fprintf(stderr, "evaluate_rf_kdclass3: short read %s\n", path);
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

static int timing_push(TimingSeries *series, uint64_t value) {
    if (series->len == series->cap) {
        size_t next = series->cap == 0 ? 1024u : series->cap * 2u;
        uint64_t *tmp = (uint64_t *)realloc(series->values, next * sizeof(uint64_t));
        if (tmp == NULL) {
            return -1;
        }
        series->values = tmp;
        series->cap = next;
    }
    series->values[series->len++] = value;
    return 0;
}

static int cmp_u64(const void *a, const void *b) {
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

static void print_timing(const char *name, TimingSeries *series) {
    if (series->len == 0) {
        printf("%s avg_us=0.000 p95_us=0.000 p99_us=0.000 count=0\n", name);
        return;
    }
    uint64_t sum = 0;
    for (size_t i = 0; i < series->len; i++) {
        sum += series->values[i];
    }
    qsort(series->values, series->len, sizeof(uint64_t), cmp_u64);
    double avg = (double)sum / (double)series->len / 1000.0;
    double p50 = (double)series->values[percentile_index(series->len, 50.0)] / 1000.0;
    double p95 = (double)series->values[percentile_index(series->len, 95.0)] / 1000.0;
    double p99 = (double)series->values[percentile_index(series->len, 99.0)] / 1000.0;
    printf("%s avg_us=%.3f p50_us=%.3f p95_us=%.3f p99_us=%.3f count=%zu\n",
           name,
           avg,
           p50,
           p95,
           p99,
           series->len);
}

static int approved_from_fraud_count(uint8_t fraud_count) {
    return fraud_count < 3u ? 1 : 0;
}

static void stats_add_decision(EvalStats *stats, int approved, int expected_approved) {
    if (approved && expected_approved) {
        stats->tp++;
    } else if (!approved && !expected_approved) {
        stats->tn++;
    } else if (!approved && expected_approved) {
        stats->fp++;
        stats->errors++;
    } else {
        stats->fn++;
        stats->errors++;
    }
}

int main(int argc, char **argv) {
    const char *tree_path = NULL;
    const char *test_data_path = NULL;
    uint32_t limit = 0;
    bool touch = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tree") == 0 && i + 1 < argc) {
            tree_path = argv[++i];
        } else if (strcmp(argv[i], "--test-data") == 0 && i + 1 < argc) {
            test_data_path = argv[++i];
        } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            limit = (uint32_t)strtoul(argv[++i], NULL, 10);
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
    KdClass3Index tree;
    if (kdclass3_open(tree_path, &tree, err, sizeof(err)) != 0) {
        fprintf(stderr, "evaluate_rf_kdclass3: %s\n", err);
        return 1;
    }
    if (touch) {
        uint64_t sink = kdclass3_touch_pages(&tree);
        fprintf(stderr, "evaluate_rf_kdclass3: touched kdclass3 sink=%llu\n", (unsigned long long)sink);
    }

    char *json = NULL;
    size_t json_len = 0;
    if (read_file(test_data_path, &json, &json_len) != 0) {
        kdclass3_close(&tree);
        return 1;
    }

    EvalStats stats;
    memset(&stats, 0, sizeof(stats));
    const char *cursor = json;
    const char *end = json + json_len;
    Slice request;
    int expected_approved = 0;
    while (limit == 0 || stats.total < limit) {
        int next = next_entry(&cursor, end, &request, &expected_approved);
        if (next == 0) {
            break;
        }
        if (next < 0) {
            fprintf(stderr, "evaluate_rf_kdclass3: malformed test-data near row %u\n", stats.total);
            free(json);
            kdclass3_close(&tree);
            return 1;
        }

        uint64_t total_start = now_ns();
        int16_t query[FASTVECTOR_DIMENSIONS];
        uint64_t vectorize_start = now_ns();
        if (!fastvector_vectorize(request.data, request.len, query)) {
            fprintf(stderr, "evaluate_rf_kdclass3: vectorize failed row %u\n", stats.total);
            free(json);
            kdclass3_close(&tree);
            return 1;
        }
        uint64_t vectorize_elapsed = now_ns() - vectorize_start;

        uint64_t rf_start = now_ns();
        double probability = 0.0;
        RfGateDecision decision = rf_gate_decide(query, &probability);
        (void)probability;
        uint64_t rf_elapsed = now_ns() - rf_start;

        bool accepted_by_rf = true;
        uint8_t fraud_count = 0;
        uint64_t fallback_elapsed = 0;
        if (decision == RF_GATE_DECISION_LEGIT) {
            stats.rf_accept_legit++;
            fraud_count = 0;
        } else if (decision == RF_GATE_DECISION_FRAUD) {
            stats.rf_accept_fraud++;
            fraud_count = 3;
        } else {
            accepted_by_rf = false;
            stats.fallback_count++;
            uint64_t fallback_start = now_ns();
            KdClass3SearchResult result = kdclass3_search(&tree, query);
            fallback_elapsed = now_ns() - fallback_start;
            if (result.fallback_required) {
                stats.kdclass3_ties++;
                fraud_count = 3;
            } else {
                fraud_count = result.fraud_count;
            }
        }
        int approved = approved_from_fraud_count(fraud_count);
        uint32_t before_errors = stats.errors;
        stats_add_decision(&stats, approved, expected_approved);
        if (stats.errors != before_errors) {
            if (accepted_by_rf) {
                stats.accepted_errors++;
            } else {
                stats.fallback_errors++;
            }
        }
        stats.total++;

        if (timing_push(&stats.vectorize_ns, vectorize_elapsed) != 0 ||
            timing_push(&stats.rf_ns, rf_elapsed) != 0 ||
            timing_push(&stats.total_ns, now_ns() - total_start) != 0 ||
            (!accepted_by_rf && timing_push(&stats.fallback_ns, fallback_elapsed) != 0)) {
            fprintf(stderr, "evaluate_rf_kdclass3: out of memory\n");
            free(json);
            kdclass3_close(&tree);
            return 1;
        }
    }

    printf("model_id=%s threshold=%s low=%.8f high=%.8f trees=%u nodes=%u features=%u\n",
           RF_GATE_MODEL_ID,
           RF_GATE_THRESHOLD_NAME,
           RF_GATE_LOW_THRESHOLD,
           RF_GATE_HIGH_THRESHOLD,
           RF_GATE_TREE_COUNT,
           RF_GATE_NODE_COUNT,
           RF_GATE_FEATURE_COUNT);
    printf("total=%u TP=%u TN=%u FP=%u FN=%u Error=%u\n",
           stats.total,
           stats.tp,
           stats.tn,
           stats.fp,
           stats.fn,
           stats.errors);
    printf("rf_accept_legit=%u rf_accept_fraud=%u fallback_count=%u fallback_rate=%.9f accepted_errors=%u fallback_errors=%u kdclass3_ties=%u\n",
           stats.rf_accept_legit,
           stats.rf_accept_fraud,
           stats.fallback_count,
           stats.total == 0 ? 0.0 : (double)stats.fallback_count / (double)stats.total,
           stats.accepted_errors,
           stats.fallback_errors,
           stats.kdclass3_ties);
    print_timing("vectorize", &stats.vectorize_ns);
    print_timing("rf_inference", &stats.rf_ns);
    print_timing("kdclass3_fallback", &stats.fallback_ns);
    print_timing("effective_total", &stats.total_ns);

    free(stats.vectorize_ns.values);
    free(stats.rf_ns.values);
    free(stats.fallback_ns.values);
    free(stats.total_ns.values);
    free(json);
    kdclass3_close(&tree);
    return stats.errors == 0 && stats.accepted_errors == 0 ? 0 : 1;
}
