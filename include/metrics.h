#ifndef RINHA_METRICS_H
#define RINHA_METRICS_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RINHA_METRIC_BUCKETS 15u
#define RINHA_VALUE_BUCKETS 15u

typedef struct {
    atomic_uint_fast64_t buckets[RINHA_METRIC_BUCKETS];
} RinhaMetricHistogram;

typedef struct {
    atomic_uint_fast64_t buckets[RINHA_VALUE_BUCKETS];
} RinhaValueHistogram;

typedef struct {
    bool enabled;

    atomic_uint_fast64_t open_connections;
    atomic_uint_fast64_t max_open_connections;
    atomic_uint_fast64_t accepted_connections;
    atomic_uint_fast64_t tcp_accept_count;
    atomic_uint_fast64_t adopted_fds;
    atomic_uint_fast64_t closed_connections;
    atomic_uint_fast64_t request_count;
    atomic_uint_fast64_t ready_count;
    atomic_uint_fast64_t fraud_count;
    atomic_uint_fast64_t debug_count;
    atomic_uint_fast64_t vectorize_failures;
    atomic_uint_fast64_t kdtree_repair_count;
    atomic_uint_fast64_t kdtree_repair_skipped;
    atomic_uint_fast64_t read_errors;
    atomic_uint_fast64_t write_errors;
    atomic_uint_fast64_t malformed_requests;
    atomic_uint_fast64_t listener_accept_errors;
    atomic_uint_fast64_t epoll_add_errors;
    atomic_uint_fast64_t fdpass_receive_errors;
    atomic_uint_fast64_t fd_queue_enqueued;
    atomic_uint_fast64_t fd_queue_dequeued;
    atomic_uint_fast64_t fd_queue_dropped;
    atomic_uint_fast64_t epoll_registered_connections;
    atomic_uint_fast64_t epoll_closed_connections;
    atomic_uint_fast64_t epoll_read_events;
    atomic_uint_fast64_t epoll_write_events;
    atomic_uint_fast64_t epoll_partial_writes;
    atomic_uint_fast64_t epoll_parser_errors;
    atomic_uint_fast64_t epoll_open_connections;
    atomic_uint_fast64_t epoll_max_open_connections;
    atomic_uint_fast64_t kdprimary2_search_count;
    atomic_uint_fast64_t kdprimary2_nodes_visited_total;
    atomic_uint_fast64_t kdprimary2_leaves_visited_total;
    atomic_uint_fast64_t kdprimary2_points_evaluated_total;
    atomic_uint_fast64_t kdprimary2_pruned_branches_total;
    atomic_uint_fast64_t kdprimary2_nodes_visited_max;
    atomic_uint_fast64_t kdprimary2_leaves_visited_max;
    atomic_uint_fast64_t kdprimary2_points_evaluated_max;
    atomic_uint_fast64_t kdprimary2_pruned_branches_max;
    atomic_uint_fast64_t requests_per_connection_1;
    atomic_uint_fast64_t requests_per_connection_2_5;
    atomic_uint_fast64_t requests_per_connection_6_20;
    atomic_uint_fast64_t requests_per_connection_gt20;

    RinhaMetricHistogram request_total;
    RinhaMetricHistogram accepted_to_first_epollin;
    RinhaMetricHistogram accepted_to_first_read;
    RinhaMetricHistogram adopted_to_first_epollin;
    RinhaMetricHistogram adopted_to_first_read;
    RinhaMetricHistogram first_read_to_header_complete;
    RinhaMetricHistogram header_complete_to_body_complete;
    RinhaMetricHistogram request_complete_to_response_done;
    RinhaMetricHistogram vectorize;
    RinhaMetricHistogram search;
    RinhaMetricHistogram kdprimary2_search;
    RinhaMetricHistogram write_response;
    RinhaMetricHistogram connection_lifetime;
    RinhaMetricHistogram fdpass_receive;
    RinhaMetricHistogram queue_wait;
    RinhaValueHistogram kdprimary2_nodes_visited;
    RinhaValueHistogram kdprimary2_leaves_visited;
    RinhaValueHistogram kdprimary2_points_evaluated;
    RinhaValueHistogram kdprimary2_pruned_branches;
    RinhaValueHistogram requests_per_connection;
} RinhaMetrics;

void metrics_init(RinhaMetrics *metrics, bool enabled);
bool metrics_parse_enabled(const char *value);
uint64_t metrics_now_ns(void);
void metrics_inc(atomic_uint_fast64_t *counter);
void metrics_dec(atomic_uint_fast64_t *counter);
void metrics_add(atomic_uint_fast64_t *counter, uint64_t value);
void metrics_update_max(atomic_uint_fast64_t *counter, uint64_t value);
void metrics_observe(RinhaMetricHistogram *histogram, uint64_t ns);
void metrics_observe_value(RinhaValueHistogram *histogram, uint64_t value);
size_t metrics_write_text(const RinhaMetrics *metrics,
                          char *out,
                          size_t cap,
                          const char *listen_mode,
                          const char *exec_mode,
                          uint32_t workers,
                          uint32_t queue_size);

#endif
