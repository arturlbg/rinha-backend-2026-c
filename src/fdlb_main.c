#include "fdlb.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t env_u32(const char *name, uint32_t fallback) {
    const char *value = getenv(name);
    if (value == NULL || *value == '\0') {
        return fallback;
    }

    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed > UINT32_MAX) {
        return fallback;
    }
    return (uint32_t)parsed;
}

static bool env_bool(const char *name, bool fallback) {
    const char *value = getenv(name);
    if (value == NULL || *value == '\0') {
        return fallback;
    }
    return strcmp(value, "1") == 0 ||
           strcmp(value, "true") == 0 ||
           strcmp(value, "TRUE") == 0 ||
           strcmp(value, "yes") == 0 ||
           strcmp(value, "YES") == 0;
}

int main(void) {
    const char *mode = getenv("RINHA_LB_MODE");
    if (mode == NULL || *mode == '\0') {
        mode = "fdpass";
    }

    const char *listen_addr = getenv("RINHA_LB_ADDR");
    if (listen_addr == NULL || *listen_addr == '\0') {
        listen_addr = ":9999";
    }

    const char *upstreams = strcmp(mode, "proxy") == 0 ?
        getenv("RINHA_PROXY_UPSTREAMS") : getenv("RINHA_FDPASS_UPSTREAMS");
    if (upstreams == NULL || *upstreams == '\0') {
        upstreams = strcmp(mode, "proxy") == 0 ?
            "/sockets/api1.sock,/sockets/api2.sock" :
            "/sockets/api1.ctrl,/sockets/api2.ctrl";
    }

    const char *backlog = getenv("RINHA_FDLB_BACKLOG");
    if (backlog == NULL || *backlog == '\0') {
        backlog = getenv("RINHA_FDLB_LISTEN_BACKLOG");
    }

    FdlbConfig config = {
        .listen_addr = listen_addr,
        .upstreams = upstreams,
        .mode = mode,
        .strategy = getenv("RINHA_FDLB_STRATEGY"),
        .lean = env_bool("RINHA_FDLB_LEAN", false),
        .metrics_enabled = env_bool("RINHA_FDLB_METRICS", false),
        .reuseport = env_bool("RINHA_FDLB_REUSEPORT", true),
        .tcp_defer_accept = env_bool("RINHA_FDLB_TCP_DEFER_ACCEPT", false),
        .tcp_fastopen = env_bool("RINHA_FDLB_TCP_FASTOPEN", false),
        .so_busy_poll_us = env_u32("RINHA_FDLB_SO_BUSY_POLL_US", 0U),
        .listen_backlog = fdlb_parse_u32_clamped(backlog, 4096U, 1U, 65535U),
        .accept_batch = fdlb_parse_u32_clamped(getenv("RINHA_FDLB_ACCEPT_BATCH"), 1U, 1U, 256U),
        .connect_retry_ms = env_u32("RINHA_LB_CONNECT_RETRY_MS", 25U),
        .startup_timeout_ms = env_u32("RINHA_LB_STARTUP_TIMEOUT_MS", 10000U),
    };

    return fdlb_run(&config);
}
