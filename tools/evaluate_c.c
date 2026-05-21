#define _POSIX_C_SOURCE 200809L

#include "fastvector.h"
#include "ivf8_index.h"
#include "ivf8_search.h"

#include <errno.h>
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
    uint64_t candidates;
    uint64_t clusters;
    uint64_t bbox_pruned;
    uint64_t radius_pruned;
    long long micros;
} EvalStats;

static void usage(void) {
    fprintf(stderr,
            "usage: evaluate_c --index <index.bin> --test-data <test-data.json> [--limit N] "
            "[--max-candidates N] [--probes N] [--counts-output path]\n");
}

static int read_file(const char *path, char **out, size_t *out_len) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "evaluate_c: open %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "evaluate_c: seek %s failed\n", path);
        fclose(file);
        return 1;
    }
    long size = ftell(file);
    if (size < 0) {
        fprintf(stderr, "evaluate_c: tell %s failed\n", path);
        fclose(file);
        return 1;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "evaluate_c: seek %s failed\n", path);
        fclose(file);
        return 1;
    }
    char *buffer = (char *)malloc((size_t)size + 1u);
    if (buffer == NULL) {
        fprintf(stderr, "evaluate_c: out of memory\n");
        fclose(file);
        return 1;
    }
    size_t got = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        fprintf(stderr, "evaluate_c: short read %s\n", path);
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

static long long elapsed_micros(struct timespec start, struct timespec end) {
    long long seconds = (long long)(end.tv_sec - start.tv_sec);
    long long nanos = (long long)(end.tv_nsec - start.tv_nsec);
    return seconds * 1000000LL + nanos / 1000LL;
}

int main(int argc, char **argv) {
    const char *index_path = NULL;
    const char *test_data_path = NULL;
    const char *counts_output_path = NULL;
    int limit = 0;
    Ivf8SearchConfig cfg = {
        .max_candidates = IVF8_SEARCH_DEFAULT_MAX_CANDIDATES,
        .probes = IVF8_SEARCH_DEFAULT_PROBES,
    };

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
        } else if (strcmp(argv[i], "--counts-output") == 0 && i + 1 < argc) {
            counts_output_path = argv[++i];
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
        fprintf(stderr, "evaluate_c: %s\n", err);
        return 1;
    }

    char *data = NULL;
    size_t data_len = 0;
    if (read_file(test_data_path, &data, &data_len) != 0) {
        ivf8_index_close(&index);
        return 1;
    }

    FILE *counts_output = NULL;
    if (counts_output_path != NULL) {
        counts_output = fopen(counts_output_path, "wb");
        if (counts_output == NULL) {
            fprintf(stderr, "evaluate_c: open %s: %s\n", counts_output_path, strerror(errno));
            free(data);
            ivf8_index_close(&index);
            return 1;
        }
    }

    EvalStats stats;
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
            fprintf(stderr, "evaluate_c: failed to parse test-data near item %d\n", stats.total);
            stats.errors++;
            break;
        }

        int16_t query[FASTVECTOR_DIMENSIONS];
        if (!fastvector_vectorize(request.data, request.len, query)) {
            stats.errors++;
            stats.total++;
            continue;
        }

        struct timespec start;
        struct timespec finish;
        (void)clock_gettime(CLOCK_MONOTONIC, &start);
        Ivf8SearchResult result = ivf8_search(&index, query, &cfg);
        (void)clock_gettime(CLOCK_MONOTONIC, &finish);

        int approved = result.fraud_count < 3u;
        if (counts_output != NULL) {
            fprintf(counts_output, "%u\n", result.fraud_count);
        }
        if (result.fraud_count <= 5u) {
            stats.fraud_counts[result.fraud_count]++;
        }
        if (approved == expected_approved) {
            if (approved) {
                stats.tn++;
            } else {
                stats.tp++;
            }
        } else if (approved) {
            stats.fn++;
            if (stats.fp + stats.fn <= 20) {
                fprintf(stderr, "mismatch %d: FN c_fraud_count=%u expected_approved=false vector=",
                        stats.total, result.fraud_count);
                for (int i = 0; i < FASTVECTOR_DIMENSIONS; i++) {
                    fprintf(stderr, "%s%d", i == 0 ? "" : ",", query[i]);
                }
                fprintf(stderr, "\n");
            }
        } else {
            stats.fp++;
            if (stats.fp + stats.fn <= 20) {
                fprintf(stderr, "mismatch %d: FP c_fraud_count=%u expected_approved=true vector=",
                        stats.total, result.fraud_count);
                for (int i = 0; i < FASTVECTOR_DIMENSIONS; i++) {
                    fprintf(stderr, "%s%d", i == 0 ? "" : ",", query[i]);
                }
                fprintf(stderr, "\n");
            }
        }

        stats.total++;
        stats.candidates += result.stats.candidates_scanned;
        stats.clusters += result.stats.clusters_scanned;
        stats.bbox_pruned += result.stats.bbox_pruned;
        stats.radius_pruned += result.stats.radius_pruned;
        stats.micros += elapsed_micros(start, finish);
    }

    if (counts_output != NULL) {
        fclose(counts_output);
    }

    printf("evaluated=%d\n", stats.total);
    printf("TP=%d\n", stats.tp);
    printf("TN=%d\n", stats.tn);
    printf("FP=%d\n", stats.fp);
    printf("FN=%d\n", stats.fn);
    printf("Error=%d\n", stats.errors);
    printf("fraud_count_distribution=0:%d 1:%d 2:%d 3:%d 4:%d 5:%d\n",
           stats.fraud_counts[0], stats.fraud_counts[1], stats.fraud_counts[2],
           stats.fraud_counts[3], stats.fraud_counts[4], stats.fraud_counts[5]);
    if (stats.total > 0) {
        printf("avg_candidates=%.2f\n", (double)stats.candidates / (double)stats.total);
        printf("avg_clusters=%.2f\n", (double)stats.clusters / (double)stats.total);
        printf("avg_search_us=%.2f\n", (double)stats.micros / (double)stats.total);
    }
    printf("bbox_pruned=%llu\n", (unsigned long long)stats.bbox_pruned);
    printf("radius_pruned=%llu\n", (unsigned long long)stats.radius_pruned);

    free(data);
    ivf8_index_close(&index);
    return stats.errors == 0 ? 0 : 1;
}

