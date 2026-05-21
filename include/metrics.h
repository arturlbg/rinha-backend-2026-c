#ifndef RINHA_METRICS_H
#define RINHA_METRICS_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RINHA_METRIC_BUCKETS 15u

typedef struct {
    atomic_uint_fast64_t buckets[RINHA_METRIC_BUCKETS];
} RinhaMetricHistogram;

typedef struct {
    bool enabled;

    atomic_uint_fast64_t accepted_connections;
    atomic_uint_fast64_t adopted_fds;
    atomic_uint_fast64_t closed_connections;
    atomic_uint_fast64_t request_count;
    atomic_uint_fast64_t ready_count;
    atomic_uint_fast64_t fraud_count;
    atomic_uint_fast64_t debug_count;
    atomic_uint_fast64_t vectorize_failures;
    atomic_uint_fast64_t read_errors;
    atomic_uint_fast64_t write_errors;
    atomic_uint_fast64_t malformed_requests;
    atomic_uint_fast64_t fdpass_receive_errors;
    atomic_uint_fast64_t fd_queue_enqueued;
    atomic_uint_fast64_t fd_queue_dequeued;
    atomic_uint_fast64_t fd_queue_dropped;

    RinhaMetricHistogram request_total;
    RinhaMetricHistogram vectorize;
    RinhaMetricHistogram search;
    RinhaMetricHistogram write_response;
    RinhaMetricHistogram connection_lifetime;
    RinhaMetricHistogram fdpass_receive;
    RinhaMetricHistogram queue_wait;
} RinhaMetrics;

void metrics_init(RinhaMetrics *metrics, bool enabled);
bool metrics_parse_enabled(const char *value);
uint64_t metrics_now_ns(void);
void metrics_inc(atomic_uint_fast64_t *counter);
void metrics_observe(RinhaMetricHistogram *histogram, uint64_t ns);
size_t metrics_write_text(const RinhaMetrics *metrics,
                          char *out,
                          size_t cap,
                          const char *listen_mode,
                          const char *exec_mode,
                          uint32_t workers,
                          uint32_t queue_size);

#endif
