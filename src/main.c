#include "config.h"
#include "fdpass.h"
#include "ivf8_index.h"
#include "metrics.h"
#include "raw_http.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int env_u32(const char *name, unsigned int fallback) {
    const char *raw = getenv(name);
    if (raw == NULL || raw[0] == '\0') {
        return fallback;
    }

    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0' || parsed > UINT_MAX) {
        fprintf(stderr, "invalid %s=%s\n", name, raw);
        exit(1);
    }
    return (unsigned int)parsed;
}

static fdpass_exec_mode parse_exec_mode(const char *mode) {
    if (strcmp(mode, "per_connection") == 0) {
        return FDPASS_EXEC_PER_CONNECTION;
    }
    if (strcmp(mode, "worker_pool") == 0) {
        return FDPASS_EXEC_WORKER_POOL;
    }
    fprintf(stderr, "invalid RINHA_EXEC_MODE=%s\n", mode);
    exit(1);
}

int main(void) {
    const char *addr = getenv("RINHA_ADDR");
    if (addr == NULL || addr[0] == '\0') {
        addr = RINHA_DEFAULT_ADDR;
    }

    const char *listen_mode = getenv("RINHA_LISTEN_MODE");
    if (listen_mode == NULL || listen_mode[0] == '\0') {
        listen_mode = RINHA_DEFAULT_LISTEN_MODE;
    }

    const char *unix_socket = getenv("RINHA_UNIX_SOCKET");
    if (unix_socket == NULL || unix_socket[0] == '\0') {
        unix_socket = RINHA_DEFAULT_UNIX_SOCKET;
    }

    const char *exec_mode_text = getenv("RINHA_EXEC_MODE");
    if (exec_mode_text == NULL || exec_mode_text[0] == '\0') {
        exec_mode_text = RINHA_DEFAULT_EXEC_MODE;
    }
    fdpass_exec_mode exec_mode = parse_exec_mode(exec_mode_text);
    uint32_t workers = env_u32("RINHA_WORKERS", RINHA_DEFAULT_WORKERS);
    uint32_t queue_size = env_u32("RINHA_FD_QUEUE_SIZE", RINHA_DEFAULT_FD_QUEUE_SIZE);

    RinhaMetrics metrics;
    metrics_init(&metrics, metrics_parse_enabled(getenv("RINHA_METRICS_ENABLED")));

    const char *index_path = getenv("RINHA_INDEX_PATH");
    if (index_path == NULL || index_path[0] == '\0') {
        index_path = RINHA_DEFAULT_INDEX_PATH;
    }

    const char *search_impl_text = getenv("RINHA_SEARCH_IMPL");
    if (search_impl_text == NULL || search_impl_text[0] == '\0') {
        search_impl_text = RINHA_DEFAULT_SEARCH_IMPL;
    }
    bool search_impl_ok = false;
    Ivf8SearchImpl search_impl = ivf8_search_impl_from_string(search_impl_text, &search_impl_ok);
    if (!search_impl_ok) {
        fprintf(stderr, "invalid RINHA_SEARCH_IMPL=%s\n", search_impl_text);
        return 1;
    }
    if (search_impl == IVF8_SEARCH_IMPL_AVX2 && !ivf8_cpu_supports_avx2()) {
        fprintf(stderr, "RINHA_SEARCH_IMPL=avx2 requested but AVX2 is unavailable; falling back to scalar\n");
        search_impl = IVF8_SEARCH_IMPL_SCALAR;
        search_impl_text = "scalar";
    }

    Ivf8Index index;
    char err[256];
    if (ivf8_index_open(index_path, &index, err, sizeof(err)) != 0) {
        fprintf(stderr, "load IVF8 index %s: %s\n", index_path, err);
        return 1;
    }

    raw_http_app app = {
        .index = &index,
        .search_config = {
            .max_candidates = env_u32("RINHA_IVF8_MAX_CANDIDATES", RINHA_DEFAULT_IVF8_MAX_CANDIDATES),
            .probes = env_u32("RINHA_IVF8_PROBES", RINHA_DEFAULT_IVF8_PROBES),
            .impl = search_impl,
        },
        .metrics = &metrics,
        .listen_mode = listen_mode,
        .exec_mode = exec_mode_text,
        .workers = workers,
        .queue_size = queue_size,
    };

    fdpass_options fdpass_opts = {
        .exec_mode = exec_mode,
        .workers = workers,
        .queue_size = queue_size,
    };

    int serve_result;
    if (strcmp(listen_mode, "tcp") == 0) {
        serve_result = raw_http_serve(addr, &app);
    } else if (strcmp(listen_mode, "fdpass") == 0) {
        serve_result = fdpass_serve(unix_socket, &app, &fdpass_opts);
    } else {
        fprintf(stderr, "invalid RINHA_LISTEN_MODE=%s\n", listen_mode);
        ivf8_index_close(&index);
        return 1;
    }

    if (serve_result != 0) {
        perror("serve");
        ivf8_index_close(&index);
        return 1;
    }
    ivf8_index_close(&index);
    return 0;
}
