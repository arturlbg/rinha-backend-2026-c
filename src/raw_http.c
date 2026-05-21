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
        if (app == NULL || app->index == NULL || body == NULL) {
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
        uint8_t fraud_count = ivf8_search_fraud_count(app->index, query, &app->search_config);
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

    char body[8192];
    size_t body_len = metrics_write_text(metrics,
                                         body,
                                         sizeof(body),
                                         app == NULL ? "" : app->listen_mode,
                                         app == NULL ? "" : app->exec_mode,
                                         app == NULL ? 0u : app->workers,
                                         app == NULL ? 0u : app->queue_size);
    char header[160];
    int header_len = snprintf(header,
                              sizeof(header),
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: text/plain\r\n"
                              "Content-Length: %zu\r\n"
                              "\r\n",
                              body_len);
    if (header_len <= 0 || (size_t)header_len >= sizeof(header)) {
        return -1;
    }
    if (write_all(client_fd, header, (size_t)header_len, metrics) != 0) {
        return -1;
    }
    return write_all(client_fd, body, body_len, metrics);
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
