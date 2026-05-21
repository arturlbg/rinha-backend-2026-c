#include "raw_http.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

typedef struct {
    const char *data;
    size_t len;
} test_chunk;

typedef struct {
    int fd;
} server_arg;

static void *server_thread(void *arg) {
    server_arg *server = (server_arg *)arg;
    (void)raw_http_handle_connection(server->fd);
    close(server->fd);
    return NULL;
}

static int write_all_test(int fd, const char *data, size_t len) {
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        written += (size_t)n;
    }
    return 0;
}

static void tiny_pause(void) {
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000;
    (void)nanosleep(&ts, NULL);
}

static size_t run_connection(const test_chunk *chunks, size_t chunk_count, char *out, size_t out_cap) {
    int sockets[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    if (failures != 0 && sockets[0] < 0) {
        return 0;
    }

    server_arg arg = {.fd = sockets[1]};
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, server_thread, &arg) == 0);

    for (size_t i = 0; i < chunk_count; i++) {
        CHECK(write_all_test(sockets[0], chunks[i].data, chunks[i].len) == 0);
        tiny_pause();
    }
    CHECK(shutdown(sockets[0], SHUT_WR) == 0);

    size_t used = 0;
    for (;;) {
        ssize_t n = read(sockets[0], out + used, out_cap - used);
        if (n > 0) {
            used += (size_t)n;
            if (used == out_cap) {
                break;
            }
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        CHECK(false);
        break;
    }
    close(sockets[0]);
    CHECK(pthread_join(thread, NULL) == 0);
    return used;
}

static size_t make_request(char *out, size_t cap, const char *method, const char *path, const char *body) {
    int n = snprintf(out, cap,
                     "%s %s HTTP/1.1\r\n"
                     "Host: localhost\r\n"
                     "Content-Length: %zu\r\n"
                     "\r\n"
                     "%s",
                     method, path, strlen(body), body);
    CHECK(n > 0 && (size_t)n < cap);
    return (size_t)n;
}

static int count_responses(const char *data, size_t len) {
    const char *needle = "HTTP/1.1";
    size_t needle_len = strlen(needle);
    int count = 0;
    for (size_t i = 0; i + needle_len <= len; i++) {
        if (memcmp(data + i, needle, needle_len) == 0) {
            count++;
        }
    }
    return count;
}

static bool contains_text(const char *data, size_t len, const char *needle) {
    size_t needle_len = strlen(needle);
    for (size_t i = 0; i + needle_len <= len; i++) {
        if (memcmp(data + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static void test_header_end(void) {
    const char *complete = "GET /ready HTTP/1.1\r\nHost: x\r\n\r\nbody";
    const char *marker = strstr(complete, "\r\n\r\n");
    CHECK(raw_http_index_header_end(complete, strlen(complete)) == (ssize_t)(marker - complete + 4));

    const char *incomplete = "GET /ready HTTP/1.1\r\nHost: x\r\n";
    CHECK(raw_http_index_header_end(incomplete, strlen(incomplete)) == -1);
}

static void test_content_length(void) {
    size_t value = 0;
    bool present = false;

    const char *a = "POST /x HTTP/1.1\r\nContent-Length: 42\r\n\r\n";
    CHECK(raw_http_parse_content_length(a, strlen(a), &value, &present) == 0);
    CHECK(present && value == 42);

    const char *b = "POST /x HTTP/1.1\r\ncontent-length: 42\r\n\r\n";
    CHECK(raw_http_parse_content_length(b, strlen(b), &value, &present) == 0);
    CHECK(present && value == 42);

    const char *c = "POST /x HTTP/1.1\r\nCoNtEnT-LeNgTh:    42\r\n\r\n";
    CHECK(raw_http_parse_content_length(c, strlen(c), &value, &present) == 0);
    CHECK(present && value == 42);

    const char *d = "GET /ready HTTP/1.1\r\nHost: x\r\n\r\n";
    CHECK(raw_http_parse_content_length(d, strlen(d), &value, &present) == 0);
    CHECK(!present && value == 0);
}

static void test_request_line(void) {
    raw_http_request_line line;

    const char *ready = "GET /ready HTTP/1.1\r\n\r\n";
    CHECK(raw_http_parse_request_line(ready, strlen(ready), &line) == 0);
    CHECK(line.method == RAW_HTTP_METHOD_GET);
    CHECK(line.path == RAW_HTTP_PATH_READY);

    const char *fraud = "POST /fraud-score HTTP/1.1\r\n\r\n";
    CHECK(raw_http_parse_request_line(fraud, strlen(fraud), &line) == 0);
    CHECK(line.method == RAW_HTTP_METHOD_POST);
    CHECK(line.path == RAW_HTTP_PATH_FRAUD_SCORE);
}

static void test_single_request(void) {
    char req[256];
    size_t req_len = make_request(req, sizeof(req), "GET", "/ready", "");
    test_chunk chunks[] = {{req, req_len}};
    char out[1024];
    size_t out_len = run_connection(chunks, 1, out, sizeof(out));
    CHECK(count_responses(out, out_len) == 1);
    CHECK(contains_text(out, out_len, "HTTP/1.1 200 OK"));
    CHECK(contains_text(out, out_len, "\r\n\r\nok"));
}

static void test_pipelined_requests(void) {
    char req1[256];
    char req2[256];
    size_t req1_len = make_request(req1, sizeof(req1), "GET", "/ready", "");
    size_t req2_len = make_request(req2, sizeof(req2), "POST", "/fraud-score", "{}");
    char combined[512];
    memcpy(combined, req1, req1_len);
    memcpy(combined + req1_len, req2, req2_len);
    test_chunk chunks[] = {{combined, req1_len + req2_len}};
    char out[2048];
    size_t out_len = run_connection(chunks, 1, out, sizeof(out));
    CHECK(count_responses(out, out_len) == 2);
    CHECK(contains_text(out, out_len, "{\"approved\":true,\"fraud_score\":0}"));
}

static void test_fragmented_header(void) {
    char req[256];
    size_t req_len = make_request(req, sizeof(req), "GET", "/ready", "");
    test_chunk chunks[] = {
        {req, 12},
        {req + 12, 11},
        {req + 23, req_len - 23},
    };
    char out[1024];
    size_t out_len = run_connection(chunks, 3, out, sizeof(out));
    CHECK(count_responses(out, out_len) == 1);
    CHECK(contains_text(out, out_len, "\r\n\r\nok"));
}

static void test_fragmented_body(void) {
    char req[256];
    size_t req_len = make_request(req, sizeof(req), "POST", "/fraud-score", "{}");
    test_chunk chunks[] = {
        {req, req_len - 1},
        {req + req_len - 1, 1},
    };
    char out[1024];
    size_t out_len = run_connection(chunks, 2, out, sizeof(out));
    CHECK(count_responses(out, out_len) == 1);
    CHECK(contains_text(out, out_len, "{\"approved\":true,\"fraud_score\":0}"));
}

static void test_mixed_pipeline_fragment(void) {
    char req1[256];
    char req2[256];
    char req3[256];
    size_t req1_len = make_request(req1, sizeof(req1), "GET", "/ready", "");
    size_t req2_len = make_request(req2, sizeof(req2), "POST", "/fraud-score", "{}");
    size_t req3_len = make_request(req3, sizeof(req3), "GET", "/ready", "");

    char combined[768];
    size_t offset = 0;
    memcpy(combined + offset, req1, req1_len);
    offset += req1_len;
    memcpy(combined + offset, req2, req2_len);
    offset += req2_len;
    memcpy(combined + offset, req3, req3_len);
    offset += req3_len;

    size_t split = req1_len + req2_len + 20;
    test_chunk chunks[] = {
        {combined, split},
        {combined + split, offset - split},
    };
    char out[2048];
    size_t out_len = run_connection(chunks, 2, out, sizeof(out));
    CHECK(count_responses(out, out_len) == 3);
}

static void test_unknown_and_wrong_method(void) {
    char unknown[256];
    char wrong[256];
    size_t unknown_len = make_request(unknown, sizeof(unknown), "GET", "/missing", "");
    size_t wrong_len = make_request(wrong, sizeof(wrong), "GET", "/fraud-score", "");

    test_chunk chunks1[] = {{unknown, unknown_len}};
    char out1[1024];
    size_t out1_len = run_connection(chunks1, 1, out1, sizeof(out1));
    CHECK(contains_text(out1, out1_len, "HTTP/1.1 404 Not Found"));

    test_chunk chunks2[] = {{wrong, wrong_len}};
    char out2[1024];
    size_t out2_len = run_connection(chunks2, 1, out2, sizeof(out2));
    CHECK(contains_text(out2, out2_len, "HTTP/1.1 405 Method Not Allowed"));
}

int main(void) {
    test_header_end();
    test_content_length();
    test_request_line();
    test_single_request();
    test_pipelined_requests();
    test_fragmented_header();
    test_fragmented_body();
    test_mixed_pipeline_fragment();
    test_unknown_and_wrong_method();

    if (failures != 0) {
        fprintf(stderr, "%d test failure(s)\n", failures);
        return 1;
    }
    puts("raw_http tests passed");
    return 0;
}
