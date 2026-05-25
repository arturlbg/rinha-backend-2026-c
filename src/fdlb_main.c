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
    const char *listen_addr = getenv("RINHA_LB_ADDR");
    if (listen_addr == NULL || *listen_addr == '\0') {
        listen_addr = ":9999";
    }

    const char *upstreams = getenv("RINHA_FDPASS_UPSTREAMS");
    if (upstreams == NULL || *upstreams == '\0') {
        upstreams = "/sockets/api1.ctrl,/sockets/api2.ctrl";
    }

    FdlbConfig config = {
        .listen_addr = listen_addr,
        .upstreams = upstreams,
        .strategy = getenv("RINHA_FDLB_STRATEGY"),
        .metrics_enabled = env_bool("RINHA_FDLB_METRICS", false),
        .connect_retry_ms = env_u32("RINHA_LB_CONNECT_RETRY_MS", 25U),
        .startup_timeout_ms = env_u32("RINHA_LB_STARTUP_TIMEOUT_MS", 10000U),
    };

    return fdlb_run(&config);
}
