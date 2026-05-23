#define _GNU_SOURCE

#include "raw_http.h"

#include "config.h"
#include "fastvector.h"
#include "responses.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    raw_http_method method;
    raw_http_path path;
    size_t content_length;
    bool content_length_present;
} parsed_request;

typedef struct {
    int client_fd;
    const raw_http_app *app;
} connection_arg;

static RinhaMetrics *app_metrics(const raw_http_app *app) {
    if (app == NULL || app->metrics == NULL || !app->metrics->enabled) {
        return NULL;
    }
    return app->metrics;
}

static int parse_port(const char *addr) {
    const char *port_text = addr;
    const char *colon = strrchr(addr, ':');
    if (colon != NULL) {
        port_text = colon + 1;
    }
    if (*port_text == '\0') {
        return -1;
    }

    char *end = NULL;
    long port = strtol(port_text, &end, 10);
    if (end == port_text || *end != '\0' || port <= 0 || port > 65535) {
        return -1;
    }
    return (int)port;
}

static bool bytes_equal_literal(const char *value, size_t value_len, const char *expected) {
    size_t expected_len = strlen(expected);
    return value_len == expected_len && memcmp(value, expected, expected_len) == 0;
}

static bool ascii_equal_fold_literal(const char *value, size_t value_len, const char *expected) {
    size_t expected_len = strlen(expected);
    if (value_len != expected_len) {
        return false;
    }
    for (size_t i = 0; i < value_len; i++) {
        unsigned char a = (unsigned char)value[i];
        unsigned char b = (unsigned char)expected[i];
        if (a >= 'A' && a <= 'Z') {
            a = (unsigned char)(a + ('a' - 'A'));
        }
        if (b >= 'A' && b <= 'Z') {
            b = (unsigned char)(b + ('a' - 'A'));
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

bool raw_http_search_mode_from_string(const char *value,
                                      raw_http_search_mode *mode,
                                      Ivf8SearchImpl *ivf8_impl) {
    if (mode == NULL || ivf8_impl == NULL) {
        return false;
    }
    if (value != NULL && strcmp(value, "kdprimary") == 0) {
        *mode = RAW_HTTP_SEARCH_KDPRIMARY;
        *ivf8_impl = IVF8_SEARCH_IMPL_SCALAR;
        return true;
    }

    bool ok = false;
    Ivf8SearchImpl parsed = ivf8_search_impl_from_string(value, &ok);
    if (!ok) {
        return false;
    }
    *mode = RAW_HTTP_SEARCH_IVF8;
    *ivf8_impl = parsed;
    return true;
}

ssize_t raw_http_index_header_end(const char *buffer, size_t len) {
    if (len < 4) {
        return -1;
    }
    for (size_t i = 0; i + 3 < len; i++) {
        if (buffer[i] == '\r' && buffer[i + 1] == '\n' &&
            buffer[i + 2] == '\r' && buffer[i + 3] == '\n') {
            return (ssize_t)(i + 4);
        }
    }
    return -1;
}

int raw_http_parse_request_line(const char *header, size_t len, raw_http_request_line *out) {
    if (out == NULL) {
        return -1;
    }
    const char *line_end = memmem(header, len, "\r\n", 2);
    if (line_end == NULL || line_end == header) {
        return -1;
    }

    const char *first_space = memchr(header, ' ', (size_t)(line_end - header));
    if (first_space == NULL || first_space == header) {
        return -1;
    }

    const char *path_start = first_space + 1;
    const char *second_space = memchr(path_start, ' ', (size_t)(line_end - path_start));
    if (second_space == NULL || second_space == path_start) {
        return -1;
    }

    size_t method_len = (size_t)(first_space - header);
    size_t path_len = (size_t)(second_space - path_start);

    out->method = RAW_HTTP_METHOD_OTHER;
    if (bytes_equal_literal(header, method_len, "GET")) {
        out->method = RAW_HTTP_METHOD_GET;
    } else if (bytes_equal_literal(header, method_len, "POST")) {
        out->method = RAW_HTTP_METHOD_POST;
    }

    out->path = RAW_HTTP_PATH_OTHER;
    if (bytes_equal_literal(path_start, path_len, "/ready")) {
        out->path = RAW_HTTP_PATH_READY;
    } else if (bytes_equal_literal(path_start, path_len, "/fraud-score")) {
        out->path = RAW_HTTP_PATH_FRAUD_SCORE;
    } else if (bytes_equal_literal(path_start, path_len, "/debug/info")) {
        out->path = RAW_HTTP_PATH_DEBUG_INFO;
    }
    return 0;
}

int raw_http_parse_content_length(const char *header, size_t len, size_t *content_length, bool *present) {
    if (content_length == NULL || present == NULL) {
        return -1;
    }
    *content_length = 0;
    *present = false;

    size_t pos = 0;
    while (pos < len) {
        const char *line_end = memmem(header + pos, len - pos, "\r\n", 2);
        if (line_end == NULL) {
            return -1;
        }
        size_t line_len = (size_t)(line_end - (header + pos));
        if (line_len == 0) {
            return 0;
        }

        const char *line = header + pos;
        const char *colon = memchr(line, ':', line_len);
        if (colon != NULL) {
            size_t name_len = (size_t)(colon - line);
            if (ascii_equal_fold_literal(line, name_len, "Content-Length")) {
                const char *value = colon + 1;
                const char *value_end = line + line_len;
                while (value < value_end && (*value == ' ' || *value == '\t')) {
                    value++;
                }
                if (value == value_end || !isdigit((unsigned char)*value)) {
                    return -1;
                }
                size_t parsed = 0;
                while (value < value_end && isdigit((unsigned char)*value)) {
                    size_t digit = (size_t)(*value - '0');
                    if (parsed > (SIZE_MAX - digit) / 10) {
                        return -1;
                    }
                    parsed = parsed * 10 + digit;
                    value++;
                }
                while (value < value_end && (*value == ' ' || *value == '\t')) {
                    value++;
                }
                if (value != value_end) {
                    return -1;
                }
                *content_length = parsed;
                *present = true;
                return 0;
            }
        }
        pos += line_len + 2;
    }
    return -1;
}

static int parse_request_header(const char *header, size_t len, parsed_request *out) {
    raw_http_request_line line;
    if (raw_http_parse_request_line(header, len, &line) != 0) {
        return -1;
    }

    size_t content_length = 0;
    bool present = false;
    if (raw_http_parse_content_length(header, len, &content_length, &present) != 0) {
        return -1;
    }
    if (content_length > RINHA_MAX_REQUEST_BYTES) {
        return -1;
    }

    out->method = line.method;
    out->path = line.path;
    out->content_length = content_length;
    out->content_length_present = present;
    return 0;
}

static const http_response *route_request(const parsed_request *request, const char *body, const raw_http_app *app) {
    RinhaMetrics *metrics = app_metrics(app);
    if (request->path == RAW_HTTP_PATH_READY) {
        if (metrics != NULL && request->method == RAW_HTTP_METHOD_GET) {
            metrics_inc(&metrics->ready_count);
        }
        return request->method == RAW_HTTP_METHOD_GET ? &RESPONSE_READY : &RESPONSE_METHOD_NOT_ALLOWED;
    }
    if (request->path == RAW_HTTP_PATH_FRAUD_SCORE) {
        if (metrics != NULL) {
            metrics_inc(&metrics->fraud_count);
        }
        if (request->method != RAW_HTTP_METHOD_POST) {
            return &RESPONSE_METHOD_NOT_ALLOWED;
        }
        if (!request->content_length_present) {
            return &RESPONSE_BAD_REQUEST;
        }
        if (app == NULL || body == NULL ||
            (app->search_mode == RAW_HTTP_SEARCH_KDPRIMARY && app->kdprimary == NULL) ||
            (app->search_mode == RAW_HTTP_SEARCH_IVF8 && app->index == NULL)) {
            return &RESPONSE_FRAUD_APPROVED;
        }
        int16_t query[FASTVECTOR_DIMENSIONS];
        uint64_t start = metrics != NULL ? metrics_now_ns() : 0u;
        if (!fastvector_vectorize(body, request->content_length, query)) {
            if (metrics != NULL) {
                metrics_observe(&metrics->vectorize, metrics_now_ns() - start);
                metrics_inc(&metrics->vectorize_failures);
            }
            return &RESPONSE_FRAUD_APPROVED;
        }
        if (metrics != NULL) {
            metrics_observe(&metrics->vectorize, metrics_now_ns() - start);
        }
        start = metrics != NULL ? metrics_now_ns() : 0u;
        uint8_t fraud_count;
        if (app->search_mode == RAW_HTTP_SEARCH_KDPRIMARY) {
            fraud_count = kdprimary_search_fraud_count(app->kdprimary, query);
        } else if (app->kdtree_repair_enabled && app->kdtree != NULL) {
            Ivf8SearchTraceResult trace = ivf8_search_trace(app->index, query, &app->search_config);
            fraud_count = trace.result.fraud_count;
            if (kdtree_repair_should_run(app->kdtree_repair_policy, &trace)) {
                if (metrics != NULL) {
                    metrics_inc(&metrics->kdtree_repair_count);
                }
                fraud_count = kdtree_search_fraud_count(app->kdtree, query, NULL);
            } else if (metrics != NULL) {
                metrics_inc(&metrics->kdtree_repair_skipped);
            }
        } else {
            fraud_count = ivf8_search_fraud_count(app->index, query, &app->search_config);
        }
        if (metrics != NULL) {
            metrics_observe(&metrics->search, metrics_now_ns() - start);
        }
        return response_for_fraud_count(fraud_count);
    }
    return &RESPONSE_NOT_FOUND;
}

static int write_all(int fd, const char *data, size_t len, RinhaMetrics *metrics) {
    size_t written = 0;
    while (written < len) {
        ssize_t n = send(fd, data + written, len - written, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (metrics != NULL) {
                metrics_inc(&metrics->write_errors);
            }
            return -1;
        }
        if (n == 0) {
            if (metrics != NULL) {
                metrics_inc(&metrics->write_errors);
            }
            return -1;
        }
        written += (size_t)n;
    }
    return 0;
}

static size_t build_debug_response(const raw_http_app *app,
                                   RinhaMetrics *metrics,
                                   char *out,
                                   size_t cap) {
    if (metrics == NULL || out == NULL || cap < 256) {
        return 0;
    }

    const size_t body_offset = 192;
    size_t body_len = metrics_write_text(metrics,
                                         out + body_offset,
                                         cap - body_offset,
                                         app == NULL ? "" : app->listen_mode,
                                         app == NULL ? "" : app->exec_mode,
                                         app == NULL ? 0u : app->workers,
                                         app == NULL ? 0u : app->queue_size);
    int header_len = snprintf(out,
                              body_offset,
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: text/plain\r\n"
                              "Content-Length: %zu\r\n"
                              "\r\n",
                              body_len);
    if (header_len <= 0 || (size_t)header_len >= body_offset) {
        return 0;
    }
    memmove(out + (size_t)header_len, out + body_offset, body_len);
    return (size_t)header_len + body_len;
}

static void prepare_response(const parsed_request *request,
                             const char *body,
                             const raw_http_app *app,
                             char *dynamic_response,
                             size_t dynamic_response_cap,
                             const char **out_data,
                             size_t *out_len) {
    RinhaMetrics *metrics = app_metrics(app);
    if (request->path == RAW_HTTP_PATH_DEBUG_INFO &&
        request->method == RAW_HTTP_METHOD_GET &&
        metrics != NULL) {
        metrics_inc(&metrics->debug_count);
        size_t response_len = build_debug_response(app, metrics, dynamic_response, dynamic_response_cap);
        if (response_len > 0) {
            *out_data = dynamic_response;
            *out_len = response_len;
            return;
        }
        *out_data = RESPONSE_BAD_REQUEST.data;
        *out_len = RESPONSE_BAD_REQUEST.len;
        return;
    }

    const http_response *response = route_request(request, body, app);
    *out_data = response->data;
    *out_len = response->len;
}

static bool compact_or_expand_buffer(char *buffer, size_t *used, size_t *pos, size_t *capacity, size_t needed) {
    if (needed <= *capacity && *used < *capacity) {
        return true;
    }
    if (*pos > 0) {
        memmove(buffer, buffer + *pos, *used - *pos);
        *used -= *pos;
        if (needed >= *pos) {
            needed -= *pos;
        } else {
            needed = 0;
        }
        *pos = 0;
        if (needed <= *capacity && *used < *capacity) {
            return true;
        }
    }
    if (*capacity < RINHA_MAX_REQUEST_BYTES) {
        size_t next = *capacity * 2;
        if (next > RINHA_MAX_REQUEST_BYTES) {
            next = RINHA_MAX_REQUEST_BYTES;
        }
        if (needed > next) {
            next = needed;
        }
        if (next > RINHA_MAX_REQUEST_BYTES) {
            return false;
        }
        *capacity = next;
        return *used < *capacity;
    }
    return false;
}

static int write_debug_response(int client_fd, const raw_http_app *app, RinhaMetrics *metrics) {
    if (metrics == NULL) {
        return write_all(client_fd, RESPONSE_NOT_FOUND.data, RESPONSE_NOT_FOUND.len, NULL);
    }

    char response[RAW_HTTP_DEBUG_RESPONSE_BYTES];
    size_t response_len = build_debug_response(app, metrics, response, sizeof(response));
    if (response_len == 0) {
        return -1;
    }
    return write_all(client_fd, response, response_len, metrics);
}

static int raw_http_handle_connection_loop(int client_fd, const raw_http_app *app) {
    char buffer[RINHA_MAX_REQUEST_BYTES];
    size_t used = 0;
    size_t pos = 0;
    size_t capacity = RINHA_READ_BUFFER_BYTES;
    RinhaMetrics *metrics = app_metrics(app);

    for (;;) {
        ssize_t relative_header_end = raw_http_index_header_end(buffer + pos, used - pos);
        while (relative_header_end < 0) {
            if (!compact_or_expand_buffer(buffer, &used, &pos, &capacity, pos)) {
                if (metrics != NULL) {
                    metrics_inc(&metrics->malformed_requests);
                }
                (void)write_all(client_fd, RESPONSE_BAD_REQUEST.data, RESPONSE_BAD_REQUEST.len, metrics);
                return -1;
            }
            relative_header_end = raw_http_index_header_end(buffer + pos, used - pos);
            if (relative_header_end >= 0) {
                break;
            }
            ssize_t n = recv(client_fd, buffer + used, capacity - used, 0);
            if (n > 0) {
                used += (size_t)n;
                relative_header_end = raw_http_index_header_end(buffer + pos, used - pos);
                continue;
            }
            if (n == 0) {
                if (used == pos) {
                    return 0;
                }
                if (metrics != NULL) {
                    metrics_inc(&metrics->malformed_requests);
                }
                (void)write_all(client_fd, RESPONSE_BAD_REQUEST.data, RESPONSE_BAD_REQUEST.len, metrics);
                return -1;
            }
            if (errno == EINTR) {
                continue;
            }
            if (metrics != NULL) {
                metrics_inc(&metrics->read_errors);
            }
            return -1;
        }

        size_t header_start = pos;
        size_t header_end = pos + (size_t)relative_header_end;
        parsed_request request;
        if (parse_request_header(buffer + header_start, header_end - header_start, &request) != 0) {
            if (metrics != NULL) {
                metrics_inc(&metrics->malformed_requests);
            }
            (void)write_all(client_fd, RESPONSE_BAD_REQUEST.data, RESPONSE_BAD_REQUEST.len, metrics);
            return -1;
        }

        size_t header_len = header_end - header_start;
        if (request.content_length > RINHA_MAX_REQUEST_BYTES ||
            header_len > RINHA_MAX_REQUEST_BYTES - request.content_length) {
            if (metrics != NULL) {
                metrics_inc(&metrics->malformed_requests);
            }
            (void)write_all(client_fd, RESPONSE_BAD_REQUEST.data, RESPONSE_BAD_REQUEST.len, metrics);
            return -1;
        }
        size_t body_end = header_end + request.content_length;

        while (used < body_end) {
            if (!compact_or_expand_buffer(buffer, &used, &pos, &capacity, body_end)) {
                if (metrics != NULL) {
                    metrics_inc(&metrics->malformed_requests);
                }
                (void)write_all(client_fd, RESPONSE_BAD_REQUEST.data, RESPONSE_BAD_REQUEST.len, metrics);
                return -1;
            }
            if (pos == 0 && body_end > capacity) {
                if (!compact_or_expand_buffer(buffer, &used, &pos, &capacity, body_end)) {
                    if (metrics != NULL) {
                        metrics_inc(&metrics->malformed_requests);
                    }
                    (void)write_all(client_fd, RESPONSE_BAD_REQUEST.data, RESPONSE_BAD_REQUEST.len, metrics);
                    return -1;
                }
            }
            if (pos == 0) {
                header_end = header_len;
                body_end = header_end + request.content_length;
            }
            if (used >= body_end) {
                break;
            }

            ssize_t n = recv(client_fd, buffer + used, capacity - used, 0);
            if (n > 0) {
                used += (size_t)n;
                continue;
            }
            if (n == 0) {
                if (metrics != NULL) {
                    metrics_inc(&metrics->malformed_requests);
                }
                (void)write_all(client_fd, RESPONSE_BAD_REQUEST.data, RESPONSE_BAD_REQUEST.len, metrics);
                return -1;
            }
            if (errno == EINTR) {
                continue;
            }
            if (metrics != NULL) {
                metrics_inc(&metrics->read_errors);
            }
            return -1;
        }

        uint64_t request_start = metrics != NULL ? metrics_now_ns() : 0u;
        if (metrics != NULL) {
            metrics_inc(&metrics->request_count);
        }
        const char *body = buffer + header_end;
        uint64_t write_start;
        int write_status;
        if (request.path == RAW_HTTP_PATH_DEBUG_INFO && request.method == RAW_HTTP_METHOD_GET && metrics != NULL) {
            metrics_inc(&metrics->debug_count);
            write_start = metrics_now_ns();
            write_status = write_debug_response(client_fd, app, metrics);
        } else {
            const http_response *response = route_request(&request, body, app);
            write_start = metrics != NULL ? metrics_now_ns() : 0u;
            write_status = write_all(client_fd, response->data, response->len, metrics);
        }
        if (metrics != NULL) {
            metrics_observe(&metrics->write_response, metrics_now_ns() - write_start);
            metrics_observe(&metrics->request_total, metrics_now_ns() - request_start);
        }
        if (write_status != 0) {
            return -1;
        }

        pos = body_end;
        if (pos == used) {
            pos = 0;
            used = 0;
            capacity = RINHA_READ_BUFFER_BYTES;
        }
    }
}

int raw_http_handle_connection(int client_fd, const raw_http_app *app) {
    RinhaMetrics *metrics = app_metrics(app);
    uint64_t start = metrics != NULL ? metrics_now_ns() : 0u;
    int result = raw_http_handle_connection_loop(client_fd, app);
    if (metrics != NULL) {
        metrics_inc(&metrics->closed_connections);
        metrics_observe(&metrics->connection_lifetime, metrics_now_ns() - start);
    }
    return result;
}

static void raw_http_conn_compact(raw_http_conn *conn) {
    if (conn->pos == 0) {
        return;
    }
    if (conn->pos == conn->used) {
        conn->pos = 0;
        conn->used = 0;
        return;
    }
    memmove(conn->buffer, conn->buffer + conn->pos, conn->used - conn->pos);
    conn->used -= conn->pos;
    conn->pos = 0;
}

void raw_http_conn_init(raw_http_conn *conn, int client_fd, const raw_http_app *app) {
    memset(conn, 0, sizeof(*conn));
    conn->fd = client_fd;
    conn->app = app;
    RinhaMetrics *metrics = app_metrics(app);
    conn->connection_start_ns = metrics != NULL ? metrics_now_ns() : 0u;
}

bool raw_http_conn_wants_write(const raw_http_conn *conn) {
    return conn != NULL && !conn->closed && conn->out_data != NULL && conn->out_pos < conn->out_len;
}

void raw_http_conn_close(raw_http_conn *conn) {
    if (conn == NULL || conn->closed) {
        return;
    }
    RinhaMetrics *metrics = app_metrics(conn->app);
    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
    }
    conn->closed = true;
    if (metrics != NULL) {
        metrics_inc(&metrics->closed_connections);
        metrics_observe(&metrics->connection_lifetime, metrics_now_ns() - conn->connection_start_ns);
    }
}

static void raw_http_conn_set_static_response(raw_http_conn *conn,
                                              const http_response *response,
                                              bool close_after_write) {
    conn->out_data = response->data;
    conn->out_len = response->len;
    conn->out_pos = 0;
    conn->close_after_write = close_after_write;
    RinhaMetrics *metrics = app_metrics(conn->app);
    if (metrics != NULL && conn->request_start_ns == 0) {
        conn->request_start_ns = metrics_now_ns();
    }
    conn->write_start_ns = metrics != NULL ? metrics_now_ns() : 0u;
}

static uint32_t raw_http_conn_flush(raw_http_conn *conn) {
    RinhaMetrics *metrics = app_metrics(conn->app);
    while (raw_http_conn_wants_write(conn)) {
        ssize_t n = send(conn->fd,
                         conn->out_data + conn->out_pos,
                         conn->out_len - conn->out_pos,
                         MSG_NOSIGNAL);
        if (n > 0) {
            conn->out_pos += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (metrics != NULL) {
                metrics_inc(&metrics->epoll_partial_writes);
            }
            return RAW_HTTP_CONN_WANT_WRITE;
        }
        if (metrics != NULL) {
            metrics_inc(&metrics->write_errors);
        }
        raw_http_conn_close(conn);
        return RAW_HTTP_CONN_CLOSED;
    }

    if (conn->out_data != NULL) {
        if (metrics != NULL) {
            uint64_t now = metrics_now_ns();
            metrics_observe(&metrics->write_response, now - conn->write_start_ns);
            metrics_observe(&metrics->request_total, now - conn->request_start_ns);
        }
        conn->out_data = NULL;
        conn->out_len = 0;
        conn->out_pos = 0;
        conn->write_start_ns = 0;
        conn->request_start_ns = 0;
    }

    if (conn->close_after_write) {
        raw_http_conn_close(conn);
        return RAW_HTTP_CONN_CLOSED;
    }
    return RAW_HTTP_CONN_WANT_READ;
}

static uint32_t raw_http_conn_process_buffer(raw_http_conn *conn) {
    RinhaMetrics *metrics = app_metrics(conn->app);

    for (;;) {
        if (conn->closed) {
            return RAW_HTTP_CONN_CLOSED;
        }
        if (raw_http_conn_wants_write(conn)) {
            return RAW_HTTP_CONN_WANT_WRITE;
        }
        if (conn->pos == conn->used) {
            conn->pos = 0;
            conn->used = 0;
            return RAW_HTTP_CONN_WANT_READ;
        }

        ssize_t relative_header_end = raw_http_index_header_end(conn->buffer + conn->pos,
                                                                conn->used - conn->pos);
        if (relative_header_end < 0) {
            if (conn->used == sizeof(conn->buffer)) {
                if (conn->pos > 0) {
                    raw_http_conn_compact(conn);
                    relative_header_end = raw_http_index_header_end(conn->buffer, conn->used);
                    if (relative_header_end >= 0) {
                        continue;
                    }
                }
                if (conn->used == sizeof(conn->buffer)) {
                    if (metrics != NULL) {
                        metrics_inc(&metrics->malformed_requests);
                        metrics_inc(&metrics->epoll_parser_errors);
                    }
                    raw_http_conn_set_static_response(conn, &RESPONSE_BAD_REQUEST, true);
                    return raw_http_conn_flush(conn);
                }
            }
            return RAW_HTTP_CONN_WANT_READ;
        }

        size_t header_start = conn->pos;
        size_t header_end = conn->pos + (size_t)relative_header_end;
        parsed_request request;
        if (parse_request_header(conn->buffer + header_start, header_end - header_start, &request) != 0) {
            if (metrics != NULL) {
                metrics_inc(&metrics->malformed_requests);
                metrics_inc(&metrics->epoll_parser_errors);
            }
            raw_http_conn_set_static_response(conn, &RESPONSE_BAD_REQUEST, true);
            return raw_http_conn_flush(conn);
        }

        size_t header_len = header_end - header_start;
        if (request.content_length > RINHA_MAX_REQUEST_BYTES ||
            header_len > RINHA_MAX_REQUEST_BYTES - request.content_length) {
            if (metrics != NULL) {
                metrics_inc(&metrics->malformed_requests);
                metrics_inc(&metrics->epoll_parser_errors);
            }
            raw_http_conn_set_static_response(conn, &RESPONSE_BAD_REQUEST, true);
            return raw_http_conn_flush(conn);
        }

        size_t body_end = header_end + request.content_length;
        if (conn->used < body_end) {
            if (body_end > sizeof(conn->buffer)) {
                if (metrics != NULL) {
                    metrics_inc(&metrics->malformed_requests);
                    metrics_inc(&metrics->epoll_parser_errors);
                }
                raw_http_conn_set_static_response(conn, &RESPONSE_BAD_REQUEST, true);
                return raw_http_conn_flush(conn);
            }
            return RAW_HTTP_CONN_WANT_READ;
        }

        if (metrics != NULL) {
            metrics_inc(&metrics->request_count);
            conn->request_start_ns = metrics_now_ns();
        }

        const char *response_data = NULL;
        size_t response_len = 0;
        prepare_response(&request,
                         conn->buffer + header_end,
                         conn->app,
                         conn->dynamic_response,
                         sizeof(conn->dynamic_response),
                         &response_data,
                         &response_len);
        conn->out_data = response_data;
        conn->out_len = response_len;
        conn->out_pos = 0;
        conn->close_after_write = false;
        conn->write_start_ns = metrics != NULL ? metrics_now_ns() : 0u;
        conn->pos = body_end;
        if (conn->pos == conn->used) {
            conn->pos = 0;
            conn->used = 0;
        }

        uint32_t status = raw_http_conn_flush(conn);
        if (status != RAW_HTTP_CONN_WANT_READ) {
            return status;
        }
    }
}

uint32_t raw_http_conn_on_writable(raw_http_conn *conn) {
    uint32_t status = raw_http_conn_flush(conn);
    if (status != RAW_HTTP_CONN_WANT_READ) {
        return status;
    }
    return raw_http_conn_process_buffer(conn);
}

uint32_t raw_http_conn_on_readable(raw_http_conn *conn) {
    RinhaMetrics *metrics = app_metrics(conn->app);
    if (raw_http_conn_wants_write(conn)) {
        return RAW_HTTP_CONN_WANT_WRITE;
    }

    for (;;) {
        if (conn->used == sizeof(conn->buffer)) {
            raw_http_conn_compact(conn);
            if (conn->used == sizeof(conn->buffer)) {
                if (metrics != NULL) {
                    metrics_inc(&metrics->malformed_requests);
                    metrics_inc(&metrics->epoll_parser_errors);
                }
                raw_http_conn_set_static_response(conn, &RESPONSE_BAD_REQUEST, true);
                return raw_http_conn_flush(conn);
            }
        }

        ssize_t n = recv(conn->fd, conn->buffer + conn->used, sizeof(conn->buffer) - conn->used, 0);
        if (n > 0) {
            conn->used += (size_t)n;
            uint32_t status = raw_http_conn_process_buffer(conn);
            if (status != RAW_HTTP_CONN_WANT_READ) {
                return status;
            }
            continue;
        }
        if (n == 0) {
            if (conn->used != conn->pos) {
                if (metrics != NULL) {
                    metrics_inc(&metrics->malformed_requests);
                    metrics_inc(&metrics->epoll_parser_errors);
                }
                raw_http_conn_set_static_response(conn, &RESPONSE_BAD_REQUEST, true);
                return raw_http_conn_flush(conn);
            }
            raw_http_conn_close(conn);
            return RAW_HTTP_CONN_CLOSED;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return raw_http_conn_process_buffer(conn);
        }
        if (metrics != NULL) {
            metrics_inc(&metrics->read_errors);
        }
        raw_http_conn_close(conn);
        return RAW_HTTP_CONN_CLOSED;
    }
}

static void *connection_thread(void *arg) {
    connection_arg *conn = (connection_arg *)arg;
    int client_fd = conn->client_fd;
    const raw_http_app *app = conn->app;
    free(conn);
    (void)raw_http_handle_connection(client_fd, app);
    close(client_fd);
    return NULL;
}

int raw_http_serve(const char *addr, const raw_http_app *app) {
    int port = parse_port(addr);
    if (port < 0) {
        errno = EINVAL;
        return -1;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        return -1;
    }

    int enabled = 1;
    (void)setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

    struct sockaddr_in listen_addr;
    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    listen_addr.sin_port = htons((uint16_t)port);

    if (bind(server_fd, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        close(server_fd);
        return -1;
    }
    if (listen(server_fd, RINHA_LISTEN_BACKLOG) < 0) {
        close(server_fd);
        return -1;
    }

    for (;;) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(server_fd);
            return -1;
        }

        RinhaMetrics *metrics = app_metrics(app);
        if (metrics != NULL) {
            metrics_inc(&metrics->accepted_connections);
        }

        connection_arg *arg = (connection_arg *)malloc(sizeof(*arg));
        if (arg == NULL) {
            close(client_fd);
            continue;
        }
        arg->client_fd = client_fd;
        arg->app = app;
        pthread_t thread;
        if (pthread_create(&thread, NULL, connection_thread, arg) != 0) {
            free(arg);
            close(client_fd);
            continue;
        }
        (void)pthread_detach(thread);
    }
}
