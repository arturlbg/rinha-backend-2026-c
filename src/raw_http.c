#define _GNU_SOURCE

#include "raw_http.h"

#include "config.h"
#include "responses.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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

static bool bytes_equal(const char *value, size_t value_len, const char *expected) {
    size_t expected_len = strlen(expected);
    return value_len == expected_len && memcmp(value, expected, expected_len) == 0;
}

static const http_response *route_request(const char *request, ssize_t request_len) {
    const char *first_space = memchr(request, ' ', (size_t)request_len);
    if (first_space == NULL || first_space == request) {
        return &RESPONSE_BAD_REQUEST;
    }

    const char *path_start = first_space + 1;
    const char *second_space = memchr(path_start, ' ', (size_t)(request + request_len - path_start));
    if (second_space == NULL || second_space == path_start) {
        return &RESPONSE_BAD_REQUEST;
    }

    size_t method_len = (size_t)(first_space - request);
    size_t path_len = (size_t)(second_space - path_start);

    bool is_get = bytes_equal(request, method_len, "GET");
    bool is_post = bytes_equal(request, method_len, "POST");

    if (bytes_equal(path_start, path_len, "/ready")) {
        return is_get ? &RESPONSE_READY : &RESPONSE_METHOD_NOT_ALLOWED;
    }
    if (bytes_equal(path_start, path_len, "/fraud-score")) {
        return is_post ? &RESPONSE_FRAUD_APPROVED : &RESPONSE_METHOD_NOT_ALLOWED;
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

static void handle_client(int client_fd) {
    char buffer[RINHA_MAX_REQUEST_BYTES];
    ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);
    if (n <= 0) {
        return;
    }

    const http_response *response = route_request(buffer, n);
    (void)write_all(client_fd, response->data, response->len);
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
        handle_client(client_fd);
        close(client_fd);
    }
}
