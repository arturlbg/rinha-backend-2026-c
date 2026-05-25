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

static const uint64_t value_thresholds[RINHA_VALUE_BUCKETS - 1u] = {
    16ull,
    32ull,
    64ull,
    128ull,
    256ull,
    512ull,
    1024ull,
    2048ull,
    4096ull,
    8192ull,
    16384ull,
    32768ull,
    65536ull,
    131072ull,
};

static const char *value_bucket_labels[RINHA_VALUE_BUCKETS] = {
    "lt_16",
    "lt_32",
    "lt_64",
    "lt_128",
    "lt_256",
    "lt_512",
    "lt_1024",
    "lt_2048",
    "lt_4096",
    "lt_8192",
    "lt_16384",
    "lt_32768",
    "lt_65536",
    "lt_131072",
    "gte_131072",
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

void metrics_dec(atomic_uint_fast64_t *counter) {
    atomic_fetch_sub_explicit(counter, 1u, memory_order_relaxed);
}

void metrics_add(atomic_uint_fast64_t *counter, uint64_t value) {
    atomic_fetch_add_explicit(counter, value, memory_order_relaxed);
}

void metrics_update_max(atomic_uint_fast64_t *counter, uint64_t value) {
    uint_fast64_t current = atomic_load_explicit(counter, memory_order_relaxed);
    while (current < value &&
           !atomic_compare_exchange_weak_explicit(counter,
                                                  &current,
                                                  value,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
    }
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

void metrics_observe_value(RinhaValueHistogram *histogram, uint64_t value) {
    uint32_t bucket = RINHA_VALUE_BUCKETS - 1u;
    for (uint32_t i = 0; i < RINHA_VALUE_BUCKETS - 1u; i++) {
        if (value < value_thresholds[i]) {
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

static size_t append_value_histogram(char *out,
                                     size_t cap,
                                     size_t used,
                                     const char *name,
                                     const RinhaValueHistogram *histogram) {
    for (uint32_t i = 0; i < RINHA_VALUE_BUCKETS; i++) {
        used = appendf(out, cap, used, "%s.%s=%llu\n",
                       name,
                       value_bucket_labels[i],
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

    used = appendf(out, cap, used, "open_connections=%llu\n", (unsigned long long)load_counter(&metrics->open_connections));
    used = appendf(out, cap, used, "max_open_connections=%llu\n", (unsigned long long)load_counter(&metrics->max_open_connections));
    used = appendf(out, cap, used, "accepted_connections=%llu\n", (unsigned long long)load_counter(&metrics->accepted_connections));
    used = appendf(out, cap, used, "tcp_accept_count=%llu\n", (unsigned long long)load_counter(&metrics->tcp_accept_count));
    used = appendf(out, cap, used, "adopted_fds=%llu\n", (unsigned long long)load_counter(&metrics->adopted_fds));
    used = appendf(out, cap, used, "fdpass_adopt_count=%llu\n", (unsigned long long)load_counter(&metrics->adopted_fds));
    used = appendf(out, cap, used, "closed_connections=%llu\n", (unsigned long long)load_counter(&metrics->closed_connections));
    used = appendf(out, cap, used, "request_count=%llu\n", (unsigned long long)load_counter(&metrics->request_count));
    used = appendf(out, cap, used, "ready_count=%llu\n", (unsigned long long)load_counter(&metrics->ready_count));
    used = appendf(out, cap, used, "fraud_count=%llu\n", (unsigned long long)load_counter(&metrics->fraud_count));
    used = appendf(out, cap, used, "debug_count=%llu\n", (unsigned long long)load_counter(&metrics->debug_count));
    used = appendf(out, cap, used, "vectorize_failures=%llu\n", (unsigned long long)load_counter(&metrics->vectorize_failures));
    used = appendf(out, cap, used, "kdtree_repair_count=%llu\n", (unsigned long long)load_counter(&metrics->kdtree_repair_count));
    used = appendf(out, cap, used, "kdtree_repair_skipped=%llu\n", (unsigned long long)load_counter(&metrics->kdtree_repair_skipped));
    used = appendf(out, cap, used, "read_errors=%llu\n", (unsigned long long)load_counter(&metrics->read_errors));
    used = appendf(out, cap, used, "write_errors=%llu\n", (unsigned long long)load_counter(&metrics->write_errors));
    used = appendf(out, cap, used, "malformed_requests=%llu\n", (unsigned long long)load_counter(&metrics->malformed_requests));
    used = appendf(out, cap, used, "listener_accept_errors=%llu\n", (unsigned long long)load_counter(&metrics->listener_accept_errors));
    used = appendf(out, cap, used, "epoll_add_errors=%llu\n", (unsigned long long)load_counter(&metrics->epoll_add_errors));
    used = appendf(out, cap, used, "fdpass_receive_errors=%llu\n", (unsigned long long)load_counter(&metrics->fdpass_receive_errors));
    used = appendf(out, cap, used, "fd_queue_enqueued=%llu\n", (unsigned long long)load_counter(&metrics->fd_queue_enqueued));
    used = appendf(out, cap, used, "fd_queue_dequeued=%llu\n", (unsigned long long)load_counter(&metrics->fd_queue_dequeued));
    used = appendf(out, cap, used, "fd_queue_dropped=%llu\n", (unsigned long long)load_counter(&metrics->fd_queue_dropped));
    used = appendf(out, cap, used, "epoll_registered_connections=%llu\n", (unsigned long long)load_counter(&metrics->epoll_registered_connections));
    used = appendf(out, cap, used, "epoll_closed_connections=%llu\n", (unsigned long long)load_counter(&metrics->epoll_closed_connections));
    used = appendf(out, cap, used, "epoll_read_events=%llu\n", (unsigned long long)load_counter(&metrics->epoll_read_events));
    used = appendf(out, cap, used, "epoll_write_events=%llu\n", (unsigned long long)load_counter(&metrics->epoll_write_events));
    used = appendf(out, cap, used, "epoll_partial_writes=%llu\n", (unsigned long long)load_counter(&metrics->epoll_partial_writes));
    used = appendf(out, cap, used, "epoll_parser_errors=%llu\n", (unsigned long long)load_counter(&metrics->epoll_parser_errors));
    used = appendf(out, cap, used, "epoll_open_connections=%llu\n", (unsigned long long)load_counter(&metrics->epoll_open_connections));
    used = appendf(out, cap, used, "epoll_max_open_connections=%llu\n", (unsigned long long)load_counter(&metrics->epoll_max_open_connections));
    used = appendf(out, cap, used, "kdprimary2_search_count=%llu\n", (unsigned long long)load_counter(&metrics->kdprimary2_search_count));
    used = appendf(out, cap, used, "kdprimary2_nodes_visited_total=%llu\n", (unsigned long long)load_counter(&metrics->kdprimary2_nodes_visited_total));
    used = appendf(out, cap, used, "kdprimary2_leaves_visited_total=%llu\n", (unsigned long long)load_counter(&metrics->kdprimary2_leaves_visited_total));
    used = appendf(out, cap, used, "kdprimary2_points_evaluated_total=%llu\n", (unsigned long long)load_counter(&metrics->kdprimary2_points_evaluated_total));
    used = appendf(out, cap, used, "kdprimary2_pruned_branches_total=%llu\n", (unsigned long long)load_counter(&metrics->kdprimary2_pruned_branches_total));
    used = appendf(out, cap, used, "kdprimary2_nodes_visited_max=%llu\n", (unsigned long long)load_counter(&metrics->kdprimary2_nodes_visited_max));
    used = appendf(out, cap, used, "kdprimary2_leaves_visited_max=%llu\n", (unsigned long long)load_counter(&metrics->kdprimary2_leaves_visited_max));
    used = appendf(out, cap, used, "kdprimary2_points_evaluated_max=%llu\n", (unsigned long long)load_counter(&metrics->kdprimary2_points_evaluated_max));
    used = appendf(out, cap, used, "kdprimary2_pruned_branches_max=%llu\n", (unsigned long long)load_counter(&metrics->kdprimary2_pruned_branches_max));
    used = appendf(out, cap, used, "requests_per_connection_1=%llu\n", (unsigned long long)load_counter(&metrics->requests_per_connection_1));
    used = appendf(out, cap, used, "requests_per_connection_2_5=%llu\n", (unsigned long long)load_counter(&metrics->requests_per_connection_2_5));
    used = appendf(out, cap, used, "requests_per_connection_6_20=%llu\n", (unsigned long long)load_counter(&metrics->requests_per_connection_6_20));
    used = appendf(out, cap, used, "requests_per_connection_gt20=%llu\n", (unsigned long long)load_counter(&metrics->requests_per_connection_gt20));
    used = appendf(out, cap, used, "async_jobs_enqueued=%llu\n", (unsigned long long)load_counter(&metrics->async_jobs_enqueued));
    used = appendf(out, cap, used, "async_jobs_completed=%llu\n", (unsigned long long)load_counter(&metrics->async_jobs_completed));
    used = appendf(out, cap, used, "async_job_queue_full=%llu\n", (unsigned long long)load_counter(&metrics->async_job_queue_full));
    used = appendf(out, cap, used, "async_sync_fallback=%llu\n", (unsigned long long)load_counter(&metrics->async_sync_fallback));
    used = appendf(out, cap, used, "async_worker_count=%llu\n", (unsigned long long)load_counter(&metrics->async_worker_count));

    used = append_histogram(out, cap, used, "request_total", &metrics->request_total);
    used = append_histogram(out, cap, used, "accepted_to_first_epollin", &metrics->accepted_to_first_epollin);
    used = append_histogram(out, cap, used, "accepted_to_first_read", &metrics->accepted_to_first_read);
    used = append_histogram(out, cap, used, "adopted_to_first_epollin", &metrics->adopted_to_first_epollin);
    used = append_histogram(out, cap, used, "adopted_to_first_read", &metrics->adopted_to_first_read);
    used = append_histogram(out, cap, used, "first_read_to_header_complete", &metrics->first_read_to_header_complete);
    used = append_histogram(out, cap, used, "header_complete_to_body_complete", &metrics->header_complete_to_body_complete);
    used = append_histogram(out, cap, used, "request_complete_to_response_done", &metrics->request_complete_to_response_done);
    used = append_histogram(out, cap, used, "vectorize", &metrics->vectorize);
    used = append_histogram(out, cap, used, "search", &metrics->search);
    used = append_histogram(out, cap, used, "kdprimary2_search", &metrics->kdprimary2_search);
    used = append_histogram(out, cap, used, "write_response", &metrics->write_response);
    used = append_histogram(out, cap, used, "connection_lifetime", &metrics->connection_lifetime);
    used = append_histogram(out, cap, used, "fdpass_receive", &metrics->fdpass_receive);
    used = append_histogram(out, cap, used, "queue_wait", &metrics->queue_wait);
    used = append_histogram(out, cap, used, "async_job_wait", &metrics->async_job_wait);
    used = append_histogram(out, cap, used, "async_job_compute", &metrics->async_job_compute);
    used = append_histogram(out, cap, used, "async_completion_to_write", &metrics->async_completion_to_write);
    used = append_value_histogram(out, cap, used, "kdprimary2_nodes_visited", &metrics->kdprimary2_nodes_visited);
    used = append_value_histogram(out, cap, used, "kdprimary2_leaves_visited", &metrics->kdprimary2_leaves_visited);
    used = append_value_histogram(out, cap, used, "kdprimary2_points_evaluated", &metrics->kdprimary2_points_evaluated);
    used = append_value_histogram(out, cap, used, "kdprimary2_pruned_branches", &metrics->kdprimary2_pruned_branches);
    used = append_value_histogram(out, cap, used, "requests_per_connection", &metrics->requests_per_connection);

    if (used > cap) {
        used = cap;
    }
    return used;
}
