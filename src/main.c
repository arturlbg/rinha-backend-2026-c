#include "config.h"
#include "fdpass.h"
#include "ivf8_index.h"
#include "kdtree.h"
#include "kdtree_repair.h"
#include "metrics.h"
#include "raw_http.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    INDEX_WARMUP_OFF = 0,
    INDEX_WARMUP_TOUCH = 1,
    INDEX_WARMUP_SEARCH = 2
} index_warmup_mode;

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
    if (strcmp(mode, "epoll") == 0) {
        return FDPASS_EXEC_EPOLL;
    }
    fprintf(stderr, "invalid RINHA_EXEC_MODE=%s\n", mode);
    exit(1);
}

static index_warmup_mode parse_index_warmup_mode(const char *mode) {
    if (mode == NULL || mode[0] == '\0' || strcmp(mode, "off") == 0) {
        return INDEX_WARMUP_OFF;
    }
    if (strcmp(mode, "touch") == 0) {
        return INDEX_WARMUP_TOUCH;
    }
    if (strcmp(mode, "search") == 0) {
        return INDEX_WARMUP_SEARCH;
    }
    fprintf(stderr, "invalid RINHA_INDEX_WARMUP=%s\n", mode);
    exit(1);
}

static bool env_bool(const char *name, bool fallback) {
    const char *raw = getenv(name);
    if (raw == NULL || raw[0] == '\0') {
        return fallback;
    }
    if (metrics_parse_enabled(raw)) {
        return true;
    }
    if (strcmp(raw, "0") == 0 ||
        strcmp(raw, "false") == 0 ||
        strcmp(raw, "FALSE") == 0 ||
        strcmp(raw, "no") == 0 ||
        strcmp(raw, "off") == 0) {
        return false;
    }
    fprintf(stderr, "invalid %s=%s\n", name, raw);
    exit(1);
}

static int16_t clamp_i16_index_value(int32_t value) {
    if (value < -10000) {
        return -10000;
    }
    if (value > 10000) {
        return 10000;
    }
    return (int16_t)value;
}

static void run_index_warmup(const Ivf8Index *index,
                             const Ivf8SearchConfig *search_config,
                             index_warmup_mode mode,
                             uint32_t queries,
                             bool use_madvise) {
    if (mode == INDEX_WARMUP_OFF) {
        return;
    }

    uint64_t start = metrics_now_ns();
    uint32_t advice = use_madvise ? ivf8_index_apply_memory_advice(index) : 0u;
    uint64_t sink = 0;
    if (mode == INDEX_WARMUP_TOUCH) {
        sink = ivf8_index_touch_pages(index);
    } else if (mode == INDEX_WARMUP_SEARCH) {
        if (queries == 0) {
            queries = RINHA_DEFAULT_INDEX_WARMUP_QUERIES;
        }
        for (uint32_t i = 0; i < queries; i++) {
            uint32_t block = (uint32_t)(((uint64_t)i * 2654435761ull) % index->blocks);
            uint32_t lane = i % IVF8_INDEX_LANES;
            uint32_t base = block * IVF8_INDEX_DIMS * IVF8_INDEX_LANES;
            int16_t query[IVF8_INDEX_DIMS];
            for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
                int32_t noise = (int32_t)((i + dim) % 3u) - 1;
                query[dim] = clamp_i16_index_value((int32_t)index->block_data[base + dim * IVF8_INDEX_LANES + lane] + noise);
            }
            sink += ivf8_search_fraud_count(index, query, search_config);
        }
    }
    uint64_t elapsed_ns = metrics_now_ns() - start;
    fprintf(stderr,
            "index_warmup mode=%s queries=%u advice=0x%x elapsed_ms=%.3f sink=%llu\n",
            mode == INDEX_WARMUP_TOUCH ? "touch" : "search",
            mode == INDEX_WARMUP_SEARCH ? queries : 0u,
            advice,
            (double)elapsed_ns / 1000000.0,
            (unsigned long long)sink);
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

    bool kdtree_repair_enabled = env_bool("RINHA_KDTREE_REPAIR_ENABLED", false);
    const char *kdtree_path = getenv("RINHA_KDTREE_PATH");
    if (kdtree_path == NULL || kdtree_path[0] == '\0') {
        kdtree_path = RINHA_DEFAULT_KDTREE_PATH;
    }
    const char *kdtree_policy_text = getenv("RINHA_KDTREE_REPAIR_POLICY");
    if (kdtree_policy_text == NULL || kdtree_policy_text[0] == '\0') {
        kdtree_policy_text = RINHA_DEFAULT_KDTREE_REPAIR_POLICY;
    }
    KdTreeRepairPolicy kdtree_policy;
    if (!kdtree_repair_policy_from_string(kdtree_policy_text, &kdtree_policy)) {
        fprintf(stderr, "invalid RINHA_KDTREE_REPAIR_POLICY=%s\n", kdtree_policy_text);
        ivf8_index_close(&index);
        return 1;
    }
    KdTree kdtree;
    memset(&kdtree, 0, sizeof(kdtree));
    kdtree.root = KDTREE_INVALID_NODE;
    if (kdtree_repair_enabled) {
        if (kdtree_mmap_nodes_for_ivf8(&kdtree, &index, kdtree_path) != 0) {
            fprintf(stderr, "load KD-tree %s: %s\n", kdtree_path, strerror(errno));
            ivf8_index_close(&index);
            return 1;
        }
        fprintf(stderr,
                "kdtree_repair enabled=1 policy=%s path=%s nodes=%u memory_mib=%.2f\n",
                kdtree_repair_policy_name(kdtree_policy),
                kdtree_path,
                kdtree.node_count,
                (double)kdtree_runtime_memory_bytes(&kdtree) / 1048576.0);
    }

    index_warmup_mode warmup_mode = parse_index_warmup_mode(getenv("RINHA_INDEX_WARMUP"));
    uint32_t warmup_queries = env_u32("RINHA_INDEX_WARMUP_QUERIES", RINHA_DEFAULT_INDEX_WARMUP_QUERIES);
    bool use_madvise = metrics_parse_enabled(getenv("RINHA_INDEX_MADVISE"));

    raw_http_app app = {
        .index = &index,
        .kdtree = kdtree_repair_enabled ? &kdtree : NULL,
        .search_config = {
            .max_candidates = env_u32("RINHA_IVF8_MAX_CANDIDATES", RINHA_DEFAULT_IVF8_MAX_CANDIDATES),
            .probes = env_u32("RINHA_IVF8_PROBES", RINHA_DEFAULT_IVF8_PROBES),
            .impl = search_impl,
        },
        .kdtree_repair_enabled = kdtree_repair_enabled,
        .kdtree_repair_policy = kdtree_policy,
        .metrics = &metrics,
        .listen_mode = listen_mode,
        .exec_mode = exec_mode_text,
        .workers = workers,
        .queue_size = queue_size,
    };

    run_index_warmup(&index, &app.search_config, warmup_mode, warmup_queries, use_madvise);

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
        kdtree_free(&kdtree);
        ivf8_index_close(&index);
        return 1;
    }

    if (serve_result != 0) {
        perror("serve");
        kdtree_free(&kdtree);
        ivf8_index_close(&index);
        return 1;
    }
    kdtree_free(&kdtree);
    ivf8_index_close(&index);
    return 0;
}
