#ifndef RINHA_RAW_HTTP_H
#define RINHA_RAW_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "config.h"
#include "ivf8_index.h"
#include "ivf8_search.h"
#include "kdclass3.h"
#include "kdprimary.h"
#include "kdprimary2.h"
#include "kdtree.h"
#include "kdtree_repair.h"
#include "metrics.h"
#include "rf_gate_model.h"

#define RAW_HTTP_CONN_WANT_READ 1u
#define RAW_HTTP_CONN_WANT_WRITE 2u
#define RAW_HTTP_CONN_CLOSED 4u
#define RAW_HTTP_DEBUG_RESPONSE_BYTES 49152u

typedef enum {
    RAW_HTTP_METHOD_OTHER = 0,
    RAW_HTTP_METHOD_GET = 1,
    RAW_HTTP_METHOD_POST = 2
} raw_http_method;

typedef enum {
    RAW_HTTP_PATH_OTHER = 0,
    RAW_HTTP_PATH_READY = 1,
    RAW_HTTP_PATH_FRAUD_SCORE = 2,
    RAW_HTTP_PATH_DEBUG_INFO = 3
} raw_http_path;

typedef enum {
    RAW_HTTP_SEARCH_IVF8 = 0,
    RAW_HTTP_SEARCH_KDPRIMARY = 1,
    RAW_HTTP_SEARCH_KDPRIMARY2 = 2,
    RAW_HTTP_SEARCH_KDCLASS3 = 3,
    RAW_HTTP_SEARCH_RF_KDCLASS3 = 4
} raw_http_search_mode;

typedef enum {
    RAW_HTTP_PROCESS_SYNC = 0,
    RAW_HTTP_PROCESS_ASYNC_WORKER = 1
} raw_http_process_mode;

typedef struct raw_http_async_runtime raw_http_async_runtime;
typedef struct raw_http_conn raw_http_conn;

typedef struct {
    raw_http_method method;
    raw_http_path path;
} raw_http_request_line;

typedef struct {
    const Ivf8Index *index;
    const KdPrimaryIndex *kdprimary;
    const KdPrimary2Index *kdprimary2;
    const KdClass3Index *kdclass3;
    const KdTree *kdtree;
    raw_http_search_mode search_mode;
    Ivf8SearchConfig search_config;
    bool kdtree_repair_enabled;
    bool kdclass3_fallback_kdprimary2;
    KdTreeRepairPolicy kdtree_repair_policy;
    RinhaMetrics *metrics;
    const char *listen_mode;
    const char *exec_mode;
    const char *debug_instance;
    raw_http_process_mode process_mode;
    raw_http_async_runtime *async_runtime;
    bool fast_fraud_parser;
    uint32_t workers;
    uint32_t queue_size;
} raw_http_app;

struct raw_http_conn {
    int fd;
    int close_feedback_fd;
    const raw_http_app *app;
    char buffer[RINHA_MAX_REQUEST_BYTES];
    size_t used;
    size_t pos;
    const char *out_data;
    size_t out_len;
    size_t out_pos;
    char dynamic_response[RAW_HTTP_DEBUG_RESPONSE_BYTES];
    bool close_after_write;
    bool closed;
    uint64_t connection_start_ns;
    uint64_t first_epollin_ns;
    uint64_t first_read_ns;
    uint64_t request_read_start_ns;
    uint64_t request_header_complete_ns;
    uint64_t request_complete_ns;
    uint64_t request_start_ns;
    uint64_t write_start_ns;
    uint32_t requests_seen;
    bool async_pending;
    uint64_t async_generation;
    uint64_t async_completed_ns;
};

typedef struct {
    raw_http_conn *conn;
    uint64_t generation;
    const char *response_data;
    size_t response_len;
    uint64_t completed_ns;
} raw_http_async_completion;

int raw_http_serve(const char *addr, const raw_http_app *app);
int raw_http_serve_epoll(const char *addr, const raw_http_app *app);
int raw_http_serve_unix(const char *path, const raw_http_app *app);
int raw_http_serve_unix_epoll(const char *path, const raw_http_app *app);
int raw_http_handle_connection(int client_fd, const raw_http_app *app);
void raw_http_conn_init(raw_http_conn *conn, int client_fd, const raw_http_app *app);
void raw_http_conn_note_read_event(raw_http_conn *conn);
uint32_t raw_http_conn_on_readable(raw_http_conn *conn);
uint32_t raw_http_conn_on_writable(raw_http_conn *conn);
bool raw_http_conn_wants_write(const raw_http_conn *conn);
void raw_http_conn_close(raw_http_conn *conn);
bool raw_http_conn_has_pending_async(const raw_http_conn *conn);
bool raw_http_conn_complete_async(raw_http_conn *conn, const raw_http_async_completion *completion);

bool raw_http_process_mode_from_string(const char *value, raw_http_process_mode *mode);
int raw_http_async_runtime_create(raw_http_async_runtime **out, uint32_t workers, uint32_t queue_size);
void raw_http_async_runtime_destroy(raw_http_async_runtime *runtime);
int raw_http_async_runtime_event_fd(const raw_http_async_runtime *runtime);
void raw_http_async_runtime_drain_event(raw_http_async_runtime *runtime);
bool raw_http_async_runtime_pop_completion(raw_http_async_runtime *runtime,
                                           raw_http_async_completion *completion);

bool raw_http_search_mode_from_string(const char *value,
                                      raw_http_search_mode *mode,
                                      Ivf8SearchImpl *ivf8_impl);
ssize_t raw_http_index_header_end(const char *buffer, size_t len);
int raw_http_parse_content_length(const char *header, size_t len, size_t *content_length, bool *present);
int raw_http_parse_request_line(const char *header, size_t len, raw_http_request_line *out);

#endif
