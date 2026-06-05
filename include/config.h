#ifndef RINHA_CONFIG_H
#define RINHA_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RINHA_DEFAULT_ADDR ":8080"
#define RINHA_DEFAULT_LISTEN_MODE "tcp"
#define RINHA_DEFAULT_UNIX_SOCKET "/sockets/api.ctrl"
#define RINHA_DEFAULT_EXEC_MODE "per_connection"
#define RINHA_DEFAULT_API_PROCESS_MODE "sync"
#define RINHA_DEFAULT_API_PROCESSES 1u
#define RINHA_MAX_API_PROCESSES 8u
#define RINHA_DEFAULT_WORKERS 1u
#define RINHA_DEFAULT_FD_QUEUE_SIZE 1024u
#define RINHA_DEFAULT_EPOLL_IDLE_US 0u
#define RINHA_DEFAULT_EPOLL_BUSY_POLL_US 0u
#define RINHA_DEFAULT_EPOLL_BUSY_POLL_BUDGET 0u
#define RINHA_DEFAULT_EPOLL_PREFER_BUSY_POLL false
#define RINHA_MAX_EPOLL_IDLE_US 1000u
#define RINHA_MAX_EPOLL_BUSY_POLL_US 1000u
#define RINHA_MAX_EPOLL_BUSY_POLL_BUDGET 256u
#define RINHA_DEFAULT_INDEX_PATH "/app/resources/index.bin"
#define RINHA_DEFAULT_INDEX_WARMUP "off"
#define RINHA_DEFAULT_INDEX_WARMUP_QUERIES 2048u
#define RINHA_DEFAULT_INDEX_MADVISE "false"
#define RINHA_DEFAULT_SEARCH_IMPL "scalar"
#define RINHA_DEFAULT_KDPRIMARY_PATH "/app/resources/kdprimary.bin"
#define RINHA_DEFAULT_KDPRIMARY_TOUCH "false"
#define RINHA_DEFAULT_KDPRIMARY2_PATH "/app/resources/kdprimary2.bin"
#define RINHA_DEFAULT_KDPRIMARY2_TOUCH "false"
#define RINHA_DEFAULT_KDCLASS3_PATH "/app/resources/kdclass3.bin"
#define RINHA_DEFAULT_KDCLASS3_TOUCH "false"
#define RINHA_DEFAULT_KDCLASS3_FALLBACK "none"
#define RINHA_DEFAULT_KDCLASS3_IMPL "baseline"
#define RINHA_DEFAULT_KDTREE_PATH "/app/resources/kdtree.bin"
#define RINHA_DEFAULT_KDTREE_REPAIR_POLICY "boundary23_far45"
#define RINHA_DEFAULT_IVF8_MAX_CANDIDATES 4096u
#define RINHA_DEFAULT_IVF8_PROBES 8u
#define RINHA_READ_BUFFER_BYTES 4096
#define RINHA_MAX_REQUEST_BYTES 8192
#define RINHA_LISTEN_BACKLOG 128
#define RINHA_UNIX_SOCKET_PATH_MAX 108u

#ifndef RINHA_ENABLE_METRICS
#define RINHA_ENABLE_METRICS 1
#endif

typedef struct {
    unsigned int idle_us;
    unsigned int busy_poll_us;
    unsigned int busy_poll_budget;
    bool prefer_busy_poll;
} RinhaEpollTuning;

static inline bool rinha_parse_clamped_u32(const char *raw,
                                           unsigned int fallback,
                                           unsigned int maximum,
                                           unsigned int *out) {
    if (out == NULL) {
        return false;
    }
    if (raw == NULL || raw[0] == '\0') {
        *out = fallback;
        return true;
    }
    if (raw[0] == '-') {
        return false;
    }

    char *end = NULL;
    unsigned long long parsed = strtoull(raw, &end, 10);
    if (end == raw || *end != '\0') {
        return false;
    }
    *out = parsed > maximum ? maximum : (unsigned int)parsed;
    return true;
}

static inline bool rinha_parse_bool(const char *raw, bool fallback, bool *out) {
    if (out == NULL) {
        return false;
    }
    if (raw == NULL || raw[0] == '\0') {
        *out = fallback;
        return true;
    }
    if (strcmp(raw, "1") == 0 ||
        strcmp(raw, "true") == 0 ||
        strcmp(raw, "TRUE") == 0 ||
        strcmp(raw, "yes") == 0 ||
        strcmp(raw, "YES") == 0 ||
        strcmp(raw, "on") == 0 ||
        strcmp(raw, "ON") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(raw, "0") == 0 ||
        strcmp(raw, "false") == 0 ||
        strcmp(raw, "FALSE") == 0 ||
        strcmp(raw, "no") == 0 ||
        strcmp(raw, "NO") == 0 ||
        strcmp(raw, "off") == 0 ||
        strcmp(raw, "OFF") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static inline bool rinha_parse_epoll_tuning(const char *idle_us,
                                            const char *busy_poll_us,
                                            const char *busy_poll_budget,
                                            const char *prefer_busy_poll,
                                            RinhaEpollTuning *out) {
    return out != NULL &&
        rinha_parse_clamped_u32(idle_us,
                                RINHA_DEFAULT_EPOLL_IDLE_US,
                                RINHA_MAX_EPOLL_IDLE_US,
                                &out->idle_us) &&
        rinha_parse_clamped_u32(busy_poll_us,
                                RINHA_DEFAULT_EPOLL_BUSY_POLL_US,
                                RINHA_MAX_EPOLL_BUSY_POLL_US,
                                &out->busy_poll_us) &&
        rinha_parse_clamped_u32(busy_poll_budget,
                                RINHA_DEFAULT_EPOLL_BUSY_POLL_BUDGET,
                                RINHA_MAX_EPOLL_BUSY_POLL_BUDGET,
                                &out->busy_poll_budget) &&
        rinha_parse_bool(prefer_busy_poll,
                         RINHA_DEFAULT_EPOLL_PREFER_BUSY_POLL,
                         &out->prefer_busy_poll);
}

static inline bool rinha_epoll_tuning_enabled(const RinhaEpollTuning *tuning) {
    return tuning != NULL &&
        (tuning->idle_us != 0u ||
         tuning->busy_poll_us != 0u ||
         tuning->busy_poll_budget != 0u ||
         tuning->prefer_busy_poll);
}

static inline bool rinha_parse_api_processes(const char *raw, unsigned int *out) {
    if (out == NULL) {
        return false;
    }
    if (raw == NULL || raw[0] == '\0') {
        *out = RINHA_DEFAULT_API_PROCESSES;
        return true;
    }

    char *end = NULL;
    unsigned long parsed = strtoul(raw, &end, 10);
    if (end == raw || *end != '\0' ||
        parsed == 0UL || parsed > RINHA_MAX_API_PROCESSES) {
        return false;
    }
    *out = (unsigned int)parsed;
    return true;
}

static inline int rinha_child_unix_socket_path(const char *base,
                                               unsigned int child_index,
                                               unsigned int process_count,
                                               char *out,
                                               size_t out_len) {
    if (base == NULL || base[0] == '\0' || out == NULL || out_len == 0) {
        return -1;
    }

    if (process_count <= 1u) {
        int copied = snprintf(out, out_len, "%s", base);
        return copied >= 0 && (size_t)copied < out_len ? 0 : -1;
    }

    const char *slash = strrchr(base, '/');
    const char *dot = strrchr(base, '.');
    if (dot == NULL || (slash != NULL && dot < slash)) {
        dot = base + strlen(base);
    }

    size_t prefix_len = (size_t)(dot - base);
    int written = snprintf(out,
                           out_len,
                           "%.*s-%u%s",
                           (int)prefix_len,
                           base,
                           child_index,
                           dot);
    return written >= 0 && (size_t)written < out_len ? 0 : -1;
}

#endif
