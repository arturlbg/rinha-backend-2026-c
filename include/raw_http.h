#ifndef RINHA_RAW_HTTP_H
#define RINHA_RAW_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "ivf8_index.h"
#include "ivf8_search.h"
#include "metrics.h"

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

typedef struct {
    raw_http_method method;
    raw_http_path path;
} raw_http_request_line;

typedef struct {
    const Ivf8Index *index;
    Ivf8SearchConfig search_config;
    RinhaMetrics *metrics;
    const char *listen_mode;
    const char *exec_mode;
    uint32_t workers;
    uint32_t queue_size;
} raw_http_app;

int raw_http_serve(const char *addr, const raw_http_app *app);
int raw_http_handle_connection(int client_fd, const raw_http_app *app);

ssize_t raw_http_index_header_end(const char *buffer, size_t len);
int raw_http_parse_content_length(const char *header, size_t len, size_t *content_length, bool *present);
int raw_http_parse_request_line(const char *header, size_t len, raw_http_request_line *out);

#endif
