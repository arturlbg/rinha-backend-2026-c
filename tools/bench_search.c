#define _POSIX_C_SOURCE 200809L

#include "fastvector.h"
#include "ivf8_index.h"
#include "ivf8_search.h"
#include "metrics.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    int fraud_count_mismatches;
    int approved_mismatches;
    uint64_t vectorize_ns_sum;
    uint64_t search_ns_sum;
    uint64_t candidates_sum;
    uint64_t *vectorize_ns;
    uint64_t *search_ns;
    uint32_t *candidates;
    size_t capacity;
} BenchStats;

static void usage(void) {
    fprintf(stderr,
            "usage: bench_search --index <index.bin> --test-data <test-data.json> [--limit N] "
            "[--max-candidates N] [--probes N] [--impl scalar|avx2] [--compare-scalar]\n");
}

static int read_file(const char *path, char **out, size_t *out_len) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "bench_search: open %s: %s\n", path, strerror(errno));
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

static int stats_reserve(BenchStats *stats, size_t needed) {
    if (needed <= stats->capacity) {
        return 0;
    }
    size_t next = stats->capacity == 0 ? 1024u : stats->capacity * 2u;
    while (next < needed) {
        next *= 2u;
    }
    uint64_t *vec = (uint64_t *)realloc(stats->vectorize_ns, next * sizeof(uint64_t));
    uint64_t *search = (uint64_t *)realloc(stats->search_ns, next * sizeof(uint64_t));
    uint32_t *candidates = (uint32_t *)realloc(stats->candidates, next * sizeof(uint32_t));
    if (vec == NULL || search == NULL || candidates == NULL) {
        free(vec);
        free(search);
        free(candidates);
        return -1;
    }
    stats->vectorize_ns = vec;
    stats->search_ns = search;
    stats->candidates = candidates;
    stats->capacity = next;
    return 0;
}

static double ns_to_us(uint64_t ns) {
    return (double)ns / 1000.0;
}

int main(int argc, char **argv) {
    const char *index_path = NULL;
    const char *test_data_path = NULL;
    int limit = 0;
    Ivf8SearchConfig cfg = {
        .max_candidates = IVF8_SEARCH_DEFAULT_MAX_CANDIDATES,
        .probes = IVF8_SEARCH_DEFAULT_PROBES,
        .impl = IVF8_SEARCH_IMPL_SCALAR,
    };
    bool compare_scalar = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--index") == 0 && i + 1 < argc) {
            index_path = argv[++i];
        } else if (strcmp(argv[i], "--test-data") == 0 && i + 1 < argc) {
            test_data_path = argv[++i];
        } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            limit = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--max-candidates") == 0 && i + 1 < argc) {
            cfg.max_candidates = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--probes") == 0 && i + 1 < argc) {
            cfg.probes = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--impl") == 0 && i + 1 < argc) {
            bool ok = false;
            cfg.impl = ivf8_search_impl_from_string(argv[++i], &ok);
            if (!ok) {
                usage();
                return 2;
            }
        } else if (strcmp(argv[i], "--compare-scalar") == 0) {
            compare_scalar = true;
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
        fprintf(stderr, "bench_search: %s\n", err);
        return 1;
    }

    char *data = NULL;
    size_t data_len = 0;
    if (read_file(test_data_path, &data, &data_len) != 0) {
        ivf8_index_close(&index);
        return 1;
    }

    BenchStats stats;
    memset(&stats, 0, sizeof(stats));

    const char *cursor = data;
    const char *end = data + data_len;
    for (;;) {
        if (limit > 0 && stats.total >= limit) {
            break;
        }
        Slice request;
        int expected_approved = 0;
        int status = next_entry(&cursor, end, &request, &expected_approved);
        if (status == 0) {
            break;
        }
        if (status < 0) {
            stats.errors++;
            break;
        }
        if (stats_reserve(&stats, (size_t)stats.total + 1u) != 0) {
            stats.errors++;
            break;
        }

        int16_t query[FASTVECTOR_DIMENSIONS];
        uint64_t start = metrics_now_ns();
        bool vectorized = fastvector_vectorize(request.data, request.len, query);
        uint64_t finish = metrics_now_ns();
        uint64_t vectorize_ns = finish - start;
        if (!vectorized) {
            stats.errors++;
            stats.total++;
            continue;
        }

        Ivf8SearchResult scalar_result;
        memset(&scalar_result, 0, sizeof(scalar_result));
        if (compare_scalar) {
            Ivf8SearchConfig scalar_cfg = cfg;
            scalar_cfg.impl = IVF8_SEARCH_IMPL_SCALAR;
            scalar_result = ivf8_search(&index, query, &scalar_cfg);
        }

        start = metrics_now_ns();
        Ivf8SearchResult result = ivf8_search(&index, query, &cfg);
        finish = metrics_now_ns();
        uint64_t search_ns = finish - start;

        if (compare_scalar) {
            if (scalar_result.fraud_count != result.fraud_count) {
                stats.fraud_count_mismatches++;
            }
            if ((scalar_result.fraud_count < 3u) != (result.fraud_count < 3u)) {
                stats.approved_mismatches++;
            }
        }

        int approved = result.fraud_count < 3u;
        if (approved == expected_approved) {
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

        stats.vectorize_ns[stats.total] = vectorize_ns;
        stats.search_ns[stats.total] = search_ns;
        stats.candidates[stats.total] = result.stats.candidates_scanned;
        stats.vectorize_ns_sum += vectorize_ns;
        stats.search_ns_sum += search_ns;
        stats.candidates_sum += result.stats.candidates_scanned;
        stats.total++;
    }

    qsort(stats.vectorize_ns, (size_t)stats.total, sizeof(uint64_t), compare_u64);
    qsort(stats.search_ns, (size_t)stats.total, sizeof(uint64_t), compare_u64);
    qsort(stats.candidates, (size_t)stats.total, sizeof(uint32_t), compare_u32);

    printf("evaluated=%d\n", stats.total);
    printf("search_impl=%s\n", ivf8_search_impl_name(cfg.impl));
    printf("avx2_supported=%s\n", ivf8_cpu_supports_avx2() ? "true" : "false");
    printf("TP=%d\nTN=%d\nFP=%d\nFN=%d\nError=%d\n", stats.tp, stats.tn, stats.fp, stats.fn, stats.errors);
    if (compare_scalar) {
        printf("fraud_count_mismatches_vs_scalar=%d\n", stats.fraud_count_mismatches);
        printf("approved_mismatches_vs_scalar=%d\n", stats.approved_mismatches);
    }
    if (stats.total > 0) {
        size_t p50 = percentile_index((size_t)stats.total, 50.0);
        size_t p95 = percentile_index((size_t)stats.total, 95.0);
        size_t p99 = percentile_index((size_t)stats.total, 99.0);
        printf("avg_vectorize_us=%.3f\n", ns_to_us(stats.vectorize_ns_sum / (uint64_t)stats.total));
        printf("p50_vectorize_us=%.3f\n", ns_to_us(stats.vectorize_ns[p50]));
        printf("p95_vectorize_us=%.3f\n", ns_to_us(stats.vectorize_ns[p95]));
        printf("p99_vectorize_us=%.3f\n", ns_to_us(stats.vectorize_ns[p99]));
        printf("avg_search_us=%.3f\n", ns_to_us(stats.search_ns_sum / (uint64_t)stats.total));
        printf("p50_search_us=%.3f\n", ns_to_us(stats.search_ns[p50]));
        printf("p95_search_us=%.3f\n", ns_to_us(stats.search_ns[p95]));
        printf("p99_search_us=%.3f\n", ns_to_us(stats.search_ns[p99]));
        printf("avg_candidates=%.2f\n", (double)stats.candidates_sum / (double)stats.total);
        printf("p50_candidates=%u\n", stats.candidates[p50]);
        printf("p95_candidates=%u\n", stats.candidates[p95]);
        printf("p99_candidates=%u\n", stats.candidates[p99]);
    }

    free(stats.vectorize_ns);
    free(stats.search_ns);
    free(stats.candidates);
    free(data);
    ivf8_index_close(&index);
    return stats.errors == 0 ? 0 : 1;
}
