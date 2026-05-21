#define _GNU_SOURCE

#include "raw_http.h"

#include "config.h"
#include "responses.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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

static const http_response *route_request(const parsed_request *request) {
    if (request->path == RAW_HTTP_PATH_READY) {
        return request->method == RAW_HTTP_METHOD_GET ? &RESPONSE_READY : &RESPONSE_METHOD_NOT_ALLOWED;
    }
    if (request->path == RAW_HTTP_PATH_FRAUD_SCORE) {
        if (request->method != RAW_HTTP_METHOD_POST) {
            return &RESPONSE_METHOD_NOT_ALLOWED;
        }
        if (!request->content_length_present) {
            return &RESPONSE_BAD_REQUEST;
        }
        return &RESPONSE_FRAUD_APPROVED;
    }
    return &RESPONSE_NOT_FOUND;
}

static int write_all(int fd, const char *data, size_t len) {
    size_t written = 0;
    while (written < len) {
        ssize_t n = send(fd, data + written, len - written, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
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

int raw_http_handle_connection(int client_fd) {
    char buffer[RINHA_MAX_REQUEST_BYTES];
    size_t used = 0;
    size_t pos = 0;
    size_t capacity = RINHA_READ_BUFFER_BYTES;

    for (;;) {
        ssize_t relative_header_end = raw_http_index_header_end(buffer + pos, used - pos);
        while (relative_header_end < 0) {
            if (!compact_or_expand_buffer(buffer, &used, &pos, &capacity, pos)) {
                (void)write_all(client_fd, RESPONSE_BAD_REQUEST.data, RESPONSE_BAD_REQUEST.len);
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
                (void)write_all(client_fd, RESPONSE_BAD_REQUEST.data, RESPONSE_BAD_REQUEST.len);
                return -1;
            }
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        size_t header_start = pos;
        size_t header_end = pos + (size_t)relative_header_end;
        parsed_request request;
        if (parse_request_header(buffer + header_start, header_end - header_start, &request) != 0) {
            (void)write_all(client_fd, RESPONSE_BAD_REQUEST.data, RESPONSE_BAD_REQUEST.len);
            return -1;
        }

        size_t header_len = header_end - header_start;
        if (request.content_length > RINHA_MAX_REQUEST_BYTES ||
            header_len > RINHA_MAX_REQUEST_BYTES - request.content_length) {
            (void)write_all(client_fd, RESPONSE_BAD_REQUEST.data, RESPONSE_BAD_REQUEST.len);
            return -1;
        }
        size_t body_end = header_end + request.content_length;

        while (used < body_end) {
            if (!compact_or_expand_buffer(buffer, &used, &pos, &capacity, body_end)) {
                (void)write_all(client_fd, RESPONSE_BAD_REQUEST.data, RESPONSE_BAD_REQUEST.len);
                return -1;
            }
            if (pos == 0 && body_end > capacity) {
                if (!compact_or_expand_buffer(buffer, &used, &pos, &capacity, body_end)) {
                    (void)write_all(client_fd, RESPONSE_BAD_REQUEST.data, RESPONSE_BAD_REQUEST.len);
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
                (void)write_all(client_fd, RESPONSE_BAD_REQUEST.data, RESPONSE_BAD_REQUEST.len);
                return -1;
            }
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        const http_response *response = route_request(&request);
        if (write_all(client_fd, response->data, response->len) != 0) {
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

int raw_http_serve(const char *addr) {
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
        (void)raw_http_handle_connection(client_fd);
        close(client_fd);
    }
}
