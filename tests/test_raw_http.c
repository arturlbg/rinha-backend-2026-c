#include "raw_http.h"

#include "fastvector.h"
#include "metrics.h"
#include "responses.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
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
    const raw_http_app *app;
} server_arg;

static void *server_thread(void *arg) {
    server_arg *server = (server_arg *)arg;
    (void)raw_http_handle_connection(server->fd, server->app);
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

static void set_nonblocking_test(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    CHECK(flags >= 0);
    CHECK(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

static void tiny_pause(void) {
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000;
    (void)nanosleep(&ts, NULL);
}

static size_t run_connection_with_app(const test_chunk *chunks,
                                      size_t chunk_count,
                                      char *out,
                                      size_t out_cap,
                                      const raw_http_app *app) {
    int sockets[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    if (failures != 0 && sockets[0] < 0) {
        return 0;
    }

    server_arg arg = {.fd = sockets[1], .app = app};
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

static size_t run_connection(const test_chunk *chunks, size_t chunk_count, char *out, size_t out_cap) {
    return run_connection_with_app(chunks, chunk_count, out, out_cap, NULL);
}

static size_t run_nonblocking_connection_with_app(const test_chunk *chunks,
                                                  size_t chunk_count,
                                                  char *out,
                                                  size_t out_cap,
                                                  const raw_http_app *app) {
    int sockets[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    if (failures != 0 && sockets[0] < 0) {
        return 0;
    }
    set_nonblocking_test(sockets[1]);

    raw_http_conn conn;
    raw_http_conn_init(&conn, sockets[1], app);

    for (size_t i = 0; i < chunk_count; i++) {
        CHECK(write_all_test(sockets[0], chunks[i].data, chunks[i].len) == 0);
        uint32_t status = raw_http_conn_on_readable(&conn);
        for (int spin = 0; spin < 16 && status == RAW_HTTP_CONN_WANT_WRITE; spin++) {
            status = raw_http_conn_on_writable(&conn);
        }
        CHECK(status == RAW_HTTP_CONN_WANT_READ || status == RAW_HTTP_CONN_WANT_WRITE || status == RAW_HTTP_CONN_CLOSED);
    }

    uint32_t status = raw_http_conn_on_readable(&conn);
    for (int spin = 0; spin < 16 && status == RAW_HTTP_CONN_WANT_WRITE; spin++) {
        status = raw_http_conn_on_writable(&conn);
    }

    size_t used = 0;
    for (;;) {
        ssize_t n = recv(sockets[0], out + used, out_cap - used, MSG_DONTWAIT);
        if (n > 0) {
            used += (size_t)n;
            if (used == out_cap) {
                break;
            }
            continue;
        }
        if (n == 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        CHECK(false);
        break;
    }

    raw_http_conn_close(&conn);
    close(sockets[0]);
    return used;
}

static size_t run_nonblocking_connection(const test_chunk *chunks,
                                         size_t chunk_count,
                                         char *out,
                                         size_t out_cap) {
    return run_nonblocking_connection_with_app(chunks, chunk_count, out, out_cap, NULL);
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

    const char *debug = "GET /debug/info HTTP/1.1\r\n\r\n";
    CHECK(raw_http_parse_request_line(debug, strlen(debug), &line) == 0);
    CHECK(line.method == RAW_HTTP_METHOD_GET);
    CHECK(line.path == RAW_HTTP_PATH_DEBUG_INFO);
}

static void test_search_mode_parse(void) {
    raw_http_search_mode mode;
    Ivf8SearchImpl impl;

    CHECK(raw_http_search_mode_from_string("scalar", &mode, &impl));
    CHECK(mode == RAW_HTTP_SEARCH_IVF8);
    CHECK(impl == IVF8_SEARCH_IMPL_SCALAR);

    CHECK(raw_http_search_mode_from_string("avx2", &mode, &impl));
    CHECK(mode == RAW_HTTP_SEARCH_IVF8);
    CHECK(impl == IVF8_SEARCH_IMPL_AVX2);

    CHECK(raw_http_search_mode_from_string("kdprimary", &mode, &impl));
    CHECK(mode == RAW_HTTP_SEARCH_KDPRIMARY);

    CHECK(raw_http_search_mode_from_string("kdprimary2", &mode, &impl));
    CHECK(mode == RAW_HTTP_SEARCH_KDPRIMARY2);

    CHECK(!raw_http_search_mode_from_string("bogus", &mode, &impl));
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

static void test_nonblocking_single_request(void) {
    char req[256];
    size_t req_len = make_request(req, sizeof(req), "GET", "/ready", "");
    test_chunk chunks[] = {{req, req_len}};
    char out[1024];
    size_t out_len = run_nonblocking_connection(chunks, 1, out, sizeof(out));
    CHECK(count_responses(out, out_len) == 1);
    CHECK(contains_text(out, out_len, "\r\n\r\nok"));
}

static void test_nonblocking_pipelined_requests(void) {
    char req1[256];
    char req2[256];
    size_t req1_len = make_request(req1, sizeof(req1), "GET", "/ready", "");
    size_t req2_len = make_request(req2, sizeof(req2), "POST", "/fraud-score", "{}");
    char combined[512];
    memcpy(combined, req1, req1_len);
    memcpy(combined + req1_len, req2, req2_len);
    test_chunk chunks[] = {{combined, req1_len + req2_len}};
    char out[2048];
    size_t out_len = run_nonblocking_connection(chunks, 1, out, sizeof(out));
    CHECK(count_responses(out, out_len) == 2);
    CHECK(contains_text(out, out_len, "{\"approved\":true,\"fraud_score\":0}"));
}

static void test_nonblocking_fragmented_header_and_body(void) {
    char req[512];
    size_t req_len = make_request(req, sizeof(req), "POST", "/fraud-score", "{}");
    test_chunk chunks[] = {
        {req, 8},
        {req + 8, req_len - 9},
        {req + req_len - 1, 1},
    };
    char out[1024];
    size_t out_len = run_nonblocking_connection(chunks, 3, out, sizeof(out));
    CHECK(count_responses(out, out_len) == 1);
    CHECK(contains_text(out, out_len, "{\"approved\":true,\"fraud_score\":0}"));
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

typedef struct {
    Ivf8Index index;
    int16_t centroids[IVF8_INDEX_DIMS];
    uint32_t offsets[2];
    uint32_t counts[1];
    int16_t bbox_min[IVF8_INDEX_DIMS];
    int16_t bbox_max[IVF8_INDEX_DIMS];
    uint64_t radii[1];
    uint8_t labels[IVF8_INDEX_LANES];
    int16_t block_data[IVF8_INDEX_DIMS * IVF8_INDEX_LANES];
} FraudPathIndex;

static void make_fraud_path_index(FraudPathIndex *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->index.n = 5;
    fixture->index.k = 1;
    fixture->index.dims = IVF8_INDEX_DIMS;
    fixture->index.lanes = IVF8_INDEX_LANES;
    fixture->index.blocks = 1;
    fixture->index.centroids = fixture->centroids;
    fixture->index.offsets = fixture->offsets;
    fixture->index.counts = fixture->counts;
    fixture->index.bbox_min = fixture->bbox_min;
    fixture->index.bbox_max = fixture->bbox_max;
    fixture->index.radii = fixture->radii;
    fixture->index.labels = fixture->labels;
    fixture->index.block_data = fixture->block_data;
    fixture->offsets[0] = 0;
    fixture->offsets[1] = 1;
    fixture->counts[0] = 5;
    fixture->labels[0] = 1;
    fixture->labels[1] = 1;
    fixture->labels[2] = 1;
    fixture->labels[3] = 0;
    fixture->labels[4] = 0;
    fixture->radii[0] = UINT64_MAX;
    for (int dim = 0; dim < FASTVECTOR_DIMENSIONS; dim++) {
        fixture->bbox_min[dim] = -10000;
        fixture->bbox_max[dim] = 10000;
    }
}

static const char *valid_payload(void) {
    return
        "{\"id\":\"tx-1\","
        "\"transaction\":{\"amount\":100,\"installments\":1,\"requested_at\":\"2026-03-16T12:00:00Z\"},"
        "\"customer\":{\"avg_amount\":100,\"tx_count_24h\":1,\"known_merchants\":[\"M1\"]},"
        "\"merchant\":{\"id\":\"M1\",\"mcc\":\"5411\",\"avg_amount\":100},"
        "\"terminal\":{\"is_online\":false,\"card_present\":true,\"km_from_home\":1},"
        "\"last_transaction\":null}";
}

static void test_fraud_response_mapping(void) {
    CHECK(contains_text(RESPONSE_FRAUD[0].data, RESPONSE_FRAUD[0].len, "{\"approved\":true,\"fraud_score\":0}"));
    CHECK(contains_text(RESPONSE_FRAUD[1].data, RESPONSE_FRAUD[1].len, "{\"approved\":true,\"fraud_score\":0.2}"));
    CHECK(contains_text(RESPONSE_FRAUD[2].data, RESPONSE_FRAUD[2].len, "{\"approved\":true,\"fraud_score\":0.4}"));
    CHECK(contains_text(RESPONSE_FRAUD[3].data, RESPONSE_FRAUD[3].len, "{\"approved\":false,\"fraud_score\":0.6}"));
    CHECK(contains_text(RESPONSE_FRAUD[4].data, RESPONSE_FRAUD[4].len, "{\"approved\":false,\"fraud_score\":0.8}"));
    CHECK(contains_text(RESPONSE_FRAUD[5].data, RESPONSE_FRAUD[5].len, "{\"approved\":false,\"fraud_score\":1}"));
}

static void test_invalid_body_returns_safe_approved(void) {
    char req[256];
    size_t req_len = make_request(req, sizeof(req), "POST", "/fraud-score", "{}");
    raw_http_app app = {.index = (const Ivf8Index *)(const void *)1};
    test_chunk chunks[] = {{req, req_len}};
    char out[1024];
    size_t out_len = run_connection_with_app(chunks, 1, out, sizeof(out), &app);
    CHECK(contains_text(out, out_len, "{\"approved\":true,\"fraud_score\":0}"));
}

static void test_valid_body_uses_search_response(void) {
    FraudPathIndex fixture;
    make_fraud_path_index(&fixture);
    raw_http_app app = {
        .index = &fixture.index,
        .search_config = {.max_candidates = 5, .probes = 1},
    };
    char req[1024];
    size_t req_len = make_request(req, sizeof(req), "POST", "/fraud-score", valid_payload());
    test_chunk chunks[] = {{req, req_len}};
    char out[2048];
    size_t out_len = run_connection_with_app(chunks, 1, out, sizeof(out), &app);
    CHECK(contains_text(out, out_len, "{\"approved\":false,\"fraud_score\":0.6}"));
}

static void test_valid_body_uses_kdprimary_response(void) {
    int16_t query[FASTVECTOR_DIMENSIONS];
    CHECK(fastvector_vectorize(valid_payload(), strlen(valid_payload()), query));

    int16_t vectors[5 * IVF8_INDEX_DIMS];
    uint8_t labels[5] = {1, 1, 1, 0, 0};
    for (uint32_t point = 0; point < 5; point++) {
        memcpy(vectors + (size_t)point * IVF8_INDEX_DIMS, query, IVF8_INDEX_DIMS * sizeof(int16_t));
        vectors[(size_t)point * IVF8_INDEX_DIMS] += (int16_t)point;
    }

    char err[256];
    KdPrimaryBuild build;
    CHECK(kdprimary_build_from_points(&build, vectors, labels, 5, 4, err, sizeof(err)) == 0);
    KdPrimaryIndex kdprimary = {
        .count = build.count,
        .node_count = build.node_count,
        .root = build.root,
        .leaf_size = build.leaf_size,
        .nodes = build.nodes,
        .vectors = build.vectors,
        .labels = build.labels,
    };
    raw_http_app app = {
        .kdprimary = &kdprimary,
        .search_mode = RAW_HTTP_SEARCH_KDPRIMARY,
    };

    char req[1024];
    size_t req_len = make_request(req, sizeof(req), "POST", "/fraud-score", valid_payload());
    test_chunk chunks[] = {{req, req_len}};
    char out[2048];
    size_t out_len = run_connection_with_app(chunks, 1, out, sizeof(out), &app);
    CHECK(contains_text(out, out_len, "{\"approved\":false,\"fraud_score\":0.6}"));
    kdprimary_build_free(&build);
}

static void test_valid_body_uses_kdprimary2_response(void) {
    int16_t query[FASTVECTOR_DIMENSIONS];
    CHECK(fastvector_vectorize(valid_payload(), strlen(valid_payload()), query));

    int16_t vectors[5 * IVF8_INDEX_DIMS];
    uint8_t labels[5] = {1, 1, 1, 0, 0};
    for (uint32_t point = 0; point < 5; point++) {
        memcpy(vectors + (size_t)point * IVF8_INDEX_DIMS, query, IVF8_INDEX_DIMS * sizeof(int16_t));
        vectors[(size_t)point * IVF8_INDEX_DIMS] += (int16_t)point;
    }

    char err[256];
    KdPrimary2Build build;
    CHECK(kdprimary2_build_from_points(&build, vectors, labels, 5, 4, err, sizeof(err)) == 0);
    KdPrimary2Index kdprimary2 = {
        .count = build.count,
        .node_count = build.node_count,
        .block_count = build.block_count,
        .root = build.root,
        .leaf_size = build.leaf_size,
        .nodes = build.nodes,
        .block_data = build.block_data,
        .labels = build.labels,
    };
    RinhaMetrics metrics;
    metrics_init(&metrics, true);
    raw_http_app app = {
        .kdprimary2 = &kdprimary2,
        .search_mode = RAW_HTTP_SEARCH_KDPRIMARY2,
        .metrics = &metrics,
        .fast_fraud_parser = true,
    };

    char req[1024];
    size_t req_len = make_request(req, sizeof(req), "POST", "/fraud-score", valid_payload());
    test_chunk chunks[] = {{req, req_len}};
    char out[2048];
    size_t out_len = run_connection_with_app(chunks, 1, out, sizeof(out), &app);
    CHECK(contains_text(out, out_len, "{\"approved\":false,\"fraud_score\":0.6}"));
    CHECK(atomic_load_explicit(&metrics.kdprimary2_search_count, memory_order_relaxed) == 1);
    CHECK(atomic_load_explicit(&metrics.kdprimary2_points_evaluated_total, memory_order_relaxed) == 5);
    CHECK(atomic_load_explicit(&metrics.kdprimary2_nodes_visited_total, memory_order_relaxed) > 0);
    kdprimary2_build_free(&build);
}

static void test_metrics_counts_and_debug(void) {
    RinhaMetrics metrics;
    metrics_init(&metrics, true);
    raw_http_app app = {
        .index = (const Ivf8Index *)(const void *)1,
        .metrics = &metrics,
        .listen_mode = "tcp",
        .exec_mode = "per_connection",
        .workers = 1,
        .queue_size = 1024,
    };

    char ready[256];
    char fraud[256];
    char debug[256];
    size_t ready_len = make_request(ready, sizeof(ready), "GET", "/ready", "");
    size_t fraud_len = make_request(fraud, sizeof(fraud), "POST", "/fraud-score", "{}");
    size_t debug_len = make_request(debug, sizeof(debug), "GET", "/debug/info", "");

    char combined[768];
    size_t offset = 0;
    memcpy(combined + offset, ready, ready_len);
    offset += ready_len;
    memcpy(combined + offset, fraud, fraud_len);
    offset += fraud_len;
    memcpy(combined + offset, debug, debug_len);
    offset += debug_len;

    test_chunk chunks[] = {{combined, offset}};
    char out[8192];
    size_t out_len = run_connection_with_app(chunks, 1, out, sizeof(out), &app);
    CHECK(count_responses(out, out_len) == 3);
    CHECK(contains_text(out, out_len, "metrics_enabled=1"));
    CHECK(contains_text(out, out_len, "request_count="));
    CHECK(atomic_load_explicit(&metrics.request_count, memory_order_relaxed) == 3);
    CHECK(atomic_load_explicit(&metrics.ready_count, memory_order_relaxed) == 1);
    CHECK(atomic_load_explicit(&metrics.fraud_count, memory_order_relaxed) == 1);
    CHECK(atomic_load_explicit(&metrics.debug_count, memory_order_relaxed) == 1);
    CHECK(atomic_load_explicit(&metrics.vectorize_failures, memory_order_relaxed) == 1);
    CHECK(atomic_load_explicit(&metrics.closed_connections, memory_order_relaxed) == 1);
    CHECK(atomic_load_explicit(&metrics.open_connections, memory_order_relaxed) == 0);
    CHECK(atomic_load_explicit(&metrics.max_open_connections, memory_order_relaxed) == 1);
}

int main(void) {
    test_header_end();
    test_content_length();
    test_request_line();
    test_search_mode_parse();
    test_single_request();
    test_pipelined_requests();
    test_fragmented_header();
    test_fragmented_body();
    test_mixed_pipeline_fragment();
    test_nonblocking_single_request();
    test_nonblocking_pipelined_requests();
    test_nonblocking_fragmented_header_and_body();
    test_unknown_and_wrong_method();
    test_fraud_response_mapping();
    test_invalid_body_returns_safe_approved();
    test_valid_body_uses_search_response();
    test_valid_body_uses_kdprimary_response();
    test_valid_body_uses_kdprimary2_response();
    test_metrics_counts_and_debug();

    if (failures != 0) {
        fprintf(stderr, "%d test failure(s)\n", failures);
        return 1;
    }
    puts("raw_http tests passed");
    return 0;
}
