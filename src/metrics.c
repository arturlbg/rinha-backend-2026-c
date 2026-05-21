#define _POSIX_C_SOURCE 200809L

#include "metrics.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const uint64_t metric_thresholds[RINHA_METRIC_BUCKETS - 1u] = {
    10000ull,
    25000ull,
    50000ull,
    100000ull,
    250000ull,
    500000ull,
    1000000ull,
    2000000ull,
    5000000ull,
    10000000ull,
    25000000ull,
    50000000ull,
    100000000ull,
    250000000ull,
};

static const char *metric_bucket_labels[RINHA_METRIC_BUCKETS] = {
    "lt_10us",
    "lt_25us",
    "lt_50us",
    "lt_100us",
    "lt_250us",
    "lt_500us",
    "lt_1ms",
    "lt_2ms",
    "lt_5ms",
    "lt_10ms",
    "lt_25ms",
    "lt_50ms",
    "lt_100ms",
    "lt_250ms",
    "gte_250ms",
};

void metrics_init(RinhaMetrics *metrics, bool enabled) {
    if (metrics == NULL) {
        return;
    }
    memset(metrics, 0, sizeof(*metrics));
    metrics->enabled = enabled;
}

bool metrics_parse_enabled(const char *value) {
    if (value == NULL || value[0] == '\0') {
        return false;
    }
    return strcmp(value, "1") == 0 ||
           strcmp(value, "true") == 0 ||
           strcmp(value, "TRUE") == 0 ||
           strcmp(value, "yes") == 0 ||
           strcmp(value, "on") == 0;
}

uint64_t metrics_now_ns(void) {
    struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
    (void)clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void metrics_inc(atomic_uint_fast64_t *counter) {
    atomic_fetch_add_explicit(counter, 1u, memory_order_relaxed);
}

void metrics_observe(RinhaMetricHistogram *histogram, uint64_t ns) {
    uint32_t bucket = RINHA_METRIC_BUCKETS - 1u;
    for (uint32_t i = 0; i < RINHA_METRIC_BUCKETS - 1u; i++) {
        if (ns < metric_thresholds[i]) {
            bucket = i;
            break;
        }
    }
    atomic_fetch_add_explicit(&histogram->buckets[bucket], 1u, memory_order_relaxed);
}

static size_t appendf(char *out, size_t cap, size_t used, const char *fmt, ...) {
    if (used >= cap) {
        return used;
    }
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(out + used, cap - used, fmt, args);
    va_end(args);
    if (n < 0) {
        return used;
    }
    size_t added = (size_t)n;
    if (added >= cap - used) {
        return cap;
    }
    return used + added;
}

static uint64_t load_counter(const atomic_uint_fast64_t *counter) {
    return atomic_load_explicit(counter, memory_order_relaxed);
}

static size_t append_histogram(char *out,
                               size_t cap,
                               size_t used,
                               const char *name,
                               const RinhaMetricHistogram *histogram) {
    for (uint32_t i = 0; i < RINHA_METRIC_BUCKETS; i++) {
        used = appendf(out, cap, used, "%s.%s=%llu\n",
                       name,
                       metric_bucket_labels[i],
                       (unsigned long long)load_counter(&histogram->buckets[i]));
    }
    return used;
}

size_t metrics_write_text(const RinhaMetrics *metrics,
                          char *out,
                          size_t cap,
                          const char *listen_mode,
                          const char *exec_mode,
                          uint32_t workers,
                          uint32_t queue_size) {
    if (out == NULL || cap == 0) {
        return 0;
    }
    if (metrics == NULL || !metrics->enabled) {
        const char disabled[] = "metrics_enabled=0\n";
        size_t len = sizeof(disabled) - 1u;
        if (len > cap) {
            len = cap;
        }
        memcpy(out, disabled, len);
        return len;
    }

    size_t used = 0;
    used = appendf(out, cap, used, "metrics_enabled=1\n");
    used = appendf(out, cap, used, "listen_mode=%s\n", listen_mode == NULL ? "" : listen_mode);
    used = appendf(out, cap, used, "exec_mode=%s\n", exec_mode == NULL ? "" : exec_mode);
    used = appendf(out, cap, used, "workers=%u\n", workers);
    used = appendf(out, cap, used, "queue_size=%u\n", queue_size);

    used = appendf(out, cap, used, "accepted_connections=%llu\n", (unsigned long long)load_counter(&metrics->accepted_connections));
    used = appendf(out, cap, used, "adopted_fds=%llu\n", (unsigned long long)load_counter(&metrics->adopted_fds));
    used = appendf(out, cap, used, "closed_connections=%llu\n", (unsigned long long)load_counter(&metrics->closed_connections));
    used = appendf(out, cap, used, "request_count=%llu\n", (unsigned long long)load_counter(&metrics->request_count));
    used = appendf(out, cap, used, "ready_count=%llu\n", (unsigned long long)load_counter(&metrics->ready_count));
    used = appendf(out, cap, used, "fraud_count=%llu\n", (unsigned long long)load_counter(&metrics->fraud_count));
    used = appendf(out, cap, used, "debug_count=%llu\n", (unsigned long long)load_counter(&metrics->debug_count));
    used = appendf(out, cap, used, "vectorize_failures=%llu\n", (unsigned long long)load_counter(&metrics->vectorize_failures));
    used = appendf(out, cap, used, "read_errors=%llu\n", (unsigned long long)load_counter(&metrics->read_errors));
    used = appendf(out, cap, used, "write_errors=%llu\n", (unsigned long long)load_counter(&metrics->write_errors));
    used = appendf(out, cap, used, "malformed_requests=%llu\n", (unsigned long long)load_counter(&metrics->malformed_requests));
    used = appendf(out, cap, used, "fdpass_receive_errors=%llu\n", (unsigned long long)load_counter(&metrics->fdpass_receive_errors));
    used = appendf(out, cap, used, "fd_queue_enqueued=%llu\n", (unsigned long long)load_counter(&metrics->fd_queue_enqueued));
    used = appendf(out, cap, used, "fd_queue_dequeued=%llu\n", (unsigned long long)load_counter(&metrics->fd_queue_dequeued));
    used = appendf(out, cap, used, "fd_queue_dropped=%llu\n", (unsigned long long)load_counter(&metrics->fd_queue_dropped));

    used = append_histogram(out, cap, used, "request_total", &metrics->request_total);
    used = append_histogram(out, cap, used, "vectorize", &metrics->vectorize);
    used = append_histogram(out, cap, used, "search", &metrics->search);
    used = append_histogram(out, cap, used, "write_response", &metrics->write_response);
    used = append_histogram(out, cap, used, "connection_lifetime", &metrics->connection_lifetime);
    used = append_histogram(out, cap, used, "fdpass_receive", &metrics->fdpass_receive);
    used = append_histogram(out, cap, used, "queue_wait", &metrics->queue_wait);

    if (used > cap) {
        used = cap;
    }
    return used;
}
