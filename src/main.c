#include "config.h"
#include "fdpass.h"
#include "ivf8_index.h"
#include "raw_http.h"

#include <errno.h>
#include <limits.h>
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

    const char *index_path = getenv("RINHA_INDEX_PATH");
    if (index_path == NULL || index_path[0] == '\0') {
        index_path = RINHA_DEFAULT_INDEX_PATH;
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
        },
    };

    int serve_result;
    if (strcmp(listen_mode, "tcp") == 0) {
        serve_result = raw_http_serve(addr, &app);
    } else if (strcmp(listen_mode, "fdpass") == 0) {
        serve_result = fdpass_serve(unix_socket, &app);
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
