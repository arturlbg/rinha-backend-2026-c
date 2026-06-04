#define _GNU_SOURCE

#include "config.h"
#include "fdpass.h"
#include "ivf8_index.h"
#include "kdclass3.h"
#include "kdprimary.h"
#include "kdprimary2.h"
#include "kdtree.h"
#include "kdtree_repair.h"
#include "metrics.h"
#include "raw_http.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum {
    INDEX_WARMUP_OFF = 0,
    INDEX_WARMUP_TOUCH = 1,
    INDEX_WARMUP_SEARCH = 2
} index_warmup_mode;

typedef struct {
    const char *addr;
    const char *listen_mode;
    const char *exec_mode_text;
    fdpass_exec_mode exec_mode;
    uint32_t workers;
    uint32_t queue_size;
    raw_http_process_mode process_mode;
    uint32_t api_workers;
} api_runtime_options;

static volatile sig_atomic_t api_supervisor_stop_requested = 0;

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

static void handle_supervisor_signal(int signum) {
    (void)signum;
    api_supervisor_stop_requested = 1;
}

static int install_supervisor_signal_handlers(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_supervisor_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) != 0) {
        return -1;
    }
    if (sigaction(SIGINT, &action, NULL) != 0) {
        return -1;
    }
    return 0;
}

static void restore_default_signal_handlers(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)sigaction(SIGINT, &action, NULL);
}

static int run_api_server(const api_runtime_options *options,
                          const char *unix_socket,
                          const raw_http_app *app_template) {
    if (options == NULL || app_template == NULL) {
        errno = EINVAL;
        return 1;
    }

    raw_http_async_runtime *async_runtime = NULL;
    raw_http_app app = *app_template;
    if (options->process_mode == RAW_HTTP_PROCESS_ASYNC_WORKER) {
        if (raw_http_async_runtime_create(&async_runtime,
                                          options->api_workers,
                                          options->queue_size) != 0) {
            fprintf(stderr, "failed to start async request workers\n");
            return 1;
        }
#if RINHA_ENABLE_METRICS
        if (app.metrics != NULL) {
            metrics_add(&app.metrics->async_worker_count, options->api_workers);
        }
#endif
        fprintf(stderr,
                "api_process mode=async_worker workers=%u queue_size=%u\n",
                options->api_workers,
                options->queue_size);
    } else {
        fprintf(stderr, "api_process mode=sync\n");
    }
    app.async_runtime = async_runtime;
    app.workers = options->process_mode == RAW_HTTP_PROCESS_ASYNC_WORKER ?
        options->api_workers : options->workers;
    app.queue_size = options->queue_size;

    fdpass_options fdpass_opts = {
        .exec_mode = options->exec_mode,
        .workers = options->workers,
        .queue_size = options->queue_size,
    };

    int serve_result = 0;
    if (strcmp(options->listen_mode, "tcp") == 0) {
        if (options->exec_mode == FDPASS_EXEC_EPOLL) {
            serve_result = raw_http_serve_epoll(options->addr, &app);
        } else {
            serve_result = raw_http_serve(options->addr, &app);
        }
    } else if (strcmp(options->listen_mode, "unix_http") == 0) {
        if (options->exec_mode == FDPASS_EXEC_EPOLL) {
            serve_result = raw_http_serve_unix_epoll(unix_socket, &app);
        } else {
            serve_result = raw_http_serve_unix(unix_socket, &app);
        }
    } else if (strcmp(options->listen_mode, "fdpass") == 0) {
        serve_result = fdpass_serve(unix_socket, &app, &fdpass_opts);
    } else {
        fprintf(stderr, "invalid RINHA_LISTEN_MODE=%s\n", options->listen_mode);
        raw_http_async_runtime_destroy(async_runtime);
        return 1;
    }

    if (serve_result != 0) {
        perror("serve");
        raw_http_async_runtime_destroy(async_runtime);
        return 1;
    }
    raw_http_async_runtime_destroy(async_runtime);
    return 0;
}

static void terminate_api_children(pid_t *children, unsigned int count) {
    for (unsigned int i = 0; i < count; i++) {
        if (children[i] > 0) {
            (void)kill(children[i], SIGTERM);
        }
    }
}

static void mark_child_exited(pid_t *children, unsigned int count, pid_t pid) {
    for (unsigned int i = 0; i < count; i++) {
        if (children[i] == pid) {
            children[i] = 0;
            return;
        }
    }
}

static int wait_for_api_children(pid_t *children, unsigned int count) {
    unsigned int live = count;
    int exit_code = 0;
    while (live > 0) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, 0);
        if (pid < 0) {
            if (errno == EINTR) {
                if (api_supervisor_stop_requested) {
                    terminate_api_children(children, count);
                }
                continue;
            }
            if (errno == ECHILD) {
                break;
            }
            perror("waitpid");
            terminate_api_children(children, count);
            return 1;
        }

        mark_child_exited(children, count, pid);
        live--;

        if (api_supervisor_stop_requested) {
            continue;
        }
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            fprintf(stderr, "api child pid=%ld exited cleanly; stopping supervisor\n", (long)pid);
        } else if (WIFEXITED(status)) {
            fprintf(stderr,
                    "api child pid=%ld exited status=%d; terminating siblings\n",
                    (long)pid,
                    WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            fprintf(stderr,
                    "api child pid=%ld terminated by signal=%d; terminating siblings\n",
                    (long)pid,
                    WTERMSIG(status));
        } else {
            fprintf(stderr, "api child pid=%ld stopped unexpectedly; terminating siblings\n", (long)pid);
        }
        exit_code = 1;
        api_supervisor_stop_requested = 1;
        terminate_api_children(children, count);
    }
    return exit_code;
}

static int run_api_process_group(const api_runtime_options *options,
                                 const char *base_unix_socket,
                                 const raw_http_app *app_template,
                                 unsigned int process_count) {
    if (process_count <= 1u) {
        return run_api_server(options, base_unix_socket, app_template);
    }
    if (strcmp(options->listen_mode, "fdpass") != 0) {
        fprintf(stderr,
                "RINHA_API_PROCESSES=%u requires RINHA_LISTEN_MODE=fdpass; direct TCP/unix_http supports one API process\n",
                process_count);
        return 1;
    }
    if (install_supervisor_signal_handlers() != 0) {
        perror("sigaction");
        return 1;
    }

    pid_t children[RINHA_MAX_API_PROCESSES];
    memset(children, 0, sizeof(children));

    for (unsigned int i = 0; i < process_count; i++) {
        char child_socket[RINHA_UNIX_SOCKET_PATH_MAX];
        if (rinha_child_unix_socket_path(base_unix_socket,
                                         i,
                                         process_count,
                                         child_socket,
                                         sizeof(child_socket)) != 0) {
            fprintf(stderr,
                    "derived Unix socket path is too long for child=%u base=%s\n",
                    i,
                    base_unix_socket);
            terminate_api_children(children, i);
            (void)wait_for_api_children(children, i);
            return 1;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            terminate_api_children(children, i);
            (void)wait_for_api_children(children, i);
            return 1;
        }
        if (pid == 0) {
            restore_default_signal_handlers();
            int child_result = run_api_server(options, child_socket, app_template);
            _exit(child_result == 0 ? 0 : 1);
        }

        children[i] = pid;
        fprintf(stderr,
                "api_process child=%u pid=%ld unix_socket=%s\n",
                i,
                (long)pid,
                child_socket);
    }

    return wait_for_api_children(children, process_count);
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
    const char *process_mode_text = getenv("RINHA_API_PROCESS_MODE");
    if (process_mode_text == NULL || process_mode_text[0] == '\0') {
        process_mode_text = RINHA_DEFAULT_API_PROCESS_MODE;
    }
    raw_http_process_mode process_mode;
    if (!raw_http_process_mode_from_string(process_mode_text, &process_mode)) {
        fprintf(stderr, "invalid RINHA_API_PROCESS_MODE=%s\n", process_mode_text);
        return 1;
    }
    uint32_t api_workers = env_u32("RINHA_API_WORKERS", RINHA_DEFAULT_WORKERS);
    unsigned int api_processes = RINHA_DEFAULT_API_PROCESSES;
    if (!rinha_parse_api_processes(getenv("RINHA_API_PROCESSES"), &api_processes)) {
        fprintf(stderr,
                "invalid RINHA_API_PROCESSES=%s (expected 1..%u)\n",
                getenv("RINHA_API_PROCESSES") == NULL ? "" : getenv("RINHA_API_PROCESSES"),
                RINHA_MAX_API_PROCESSES);
        return 1;
    }
    if (api_processes > 1u && strcmp(listen_mode, "fdpass") != 0) {
        fprintf(stderr,
                "RINHA_API_PROCESSES=%u requires RINHA_LISTEN_MODE=fdpass; direct TCP/unix_http supports one API process\n",
                api_processes);
        return 1;
    }

#if RINHA_ENABLE_METRICS
    RinhaMetrics metrics;
    bool debug_timing_enabled = metrics_parse_enabled(getenv("RINHA_DEBUG_TIMING"));
    metrics_init(&metrics, metrics_parse_enabled(getenv("RINHA_METRICS_ENABLED")) || debug_timing_enabled);
    metrics.debug_timing_enabled = debug_timing_enabled;
#endif

    const char *index_path = getenv("RINHA_INDEX_PATH");
    if (index_path == NULL || index_path[0] == '\0') {
        index_path = RINHA_DEFAULT_INDEX_PATH;
    }

    const char *search_impl_text = getenv("RINHA_SEARCH_IMPL");
    if (search_impl_text == NULL || search_impl_text[0] == '\0') {
        search_impl_text = RINHA_DEFAULT_SEARCH_IMPL;
    }
    raw_http_search_mode search_mode;
    Ivf8SearchImpl search_impl;
    if (!raw_http_search_mode_from_string(search_impl_text, &search_mode, &search_impl)) {
        fprintf(stderr, "invalid RINHA_SEARCH_IMPL=%s\n", search_impl_text);
        return 1;
    }
    if (search_mode == RAW_HTTP_SEARCH_IVF8 &&
        search_impl == IVF8_SEARCH_IMPL_AVX2 &&
        !ivf8_cpu_supports_avx2()) {
        fprintf(stderr, "RINHA_SEARCH_IMPL=avx2 requested but AVX2 is unavailable; falling back to scalar\n");
        search_impl = IVF8_SEARCH_IMPL_SCALAR;
        search_impl_text = "scalar";
    }

    Ivf8Index index;
    memset(&index, 0, sizeof(index));
    index.fd = -1;
    char err[256];
    KdPrimaryIndex kdprimary;
    memset(&kdprimary, 0, sizeof(kdprimary));
    kdprimary.fd = -1;
    KdPrimary2Index kdprimary2;
    memset(&kdprimary2, 0, sizeof(kdprimary2));
    kdprimary2.fd = -1;
    KdClass3Index kdclass3;
    memset(&kdclass3, 0, sizeof(kdclass3));
    kdclass3.fd = -1;

    bool kdclass3_fallback_kdprimary2 = false;
    if (search_mode == RAW_HTTP_SEARCH_KDCLASS3) {
        const char *fallback = getenv("RINHA_KDCLASS3_FALLBACK");
        if (fallback == NULL || fallback[0] == '\0') {
            fallback = RINHA_DEFAULT_KDCLASS3_FALLBACK;
        }
        if (strcmp(fallback, "none") == 0) {
            kdclass3_fallback_kdprimary2 = false;
        } else if (strcmp(fallback, "kdprimary2") == 0) {
            kdclass3_fallback_kdprimary2 = true;
        } else {
            fprintf(stderr, "invalid RINHA_KDCLASS3_FALLBACK=%s\n", fallback);
            return 1;
        }
    }

    bool kdtree_repair_enabled = env_bool("RINHA_KDTREE_REPAIR_ENABLED", false);
    if (search_mode != RAW_HTTP_SEARCH_IVF8 && kdtree_repair_enabled) {
        fprintf(stderr, "RINHA_KDTREE_REPAIR_ENABLED cannot be combined with KD-primary/KD-class search modes\n");
        return 1;
    }

    if (search_mode == RAW_HTTP_SEARCH_KDPRIMARY) {
        const char *kdprimary_path = getenv("RINHA_KDPRIMARY_PATH");
        if (kdprimary_path == NULL || kdprimary_path[0] == '\0') {
            kdprimary_path = RINHA_DEFAULT_KDPRIMARY_PATH;
        }
        if (kdprimary_open(kdprimary_path, &kdprimary, err, sizeof(err)) != 0) {
            fprintf(stderr, "load KD-primary %s: %s\n", kdprimary_path, err);
            return 1;
        }
        bool kdprimary_touch = env_bool("RINHA_KDPRIMARY_TOUCH", false);
        uint64_t touch_sum = 0;
        uint64_t touch_elapsed_ns = 0;
        if (kdprimary_touch) {
            uint64_t start = metrics_now_ns();
            touch_sum = kdprimary_touch_pages(&kdprimary);
            touch_elapsed_ns = metrics_now_ns() - start;
        }
        fprintf(stderr,
                "kdprimary enabled=1 path=%s points=%u nodes=%u leaf_size=%u memory_mib=%.2f touch=%s touch_ms=%.3f sink=%llu\n",
                kdprimary_path,
                kdprimary.count,
                kdprimary.node_count,
                kdprimary.leaf_size,
                (double)kdprimary_runtime_memory_bytes(&kdprimary) / 1048576.0,
                kdprimary_touch ? "true" : "false",
                (double)touch_elapsed_ns / 1000000.0,
                (unsigned long long)touch_sum);
    }

    if (search_mode == RAW_HTTP_SEARCH_KDPRIMARY2 || kdclass3_fallback_kdprimary2) {
        const char *kdprimary2_path = getenv("RINHA_KDPRIMARY2_PATH");
        if (kdprimary2_path == NULL || kdprimary2_path[0] == '\0') {
            kdprimary2_path = RINHA_DEFAULT_KDPRIMARY2_PATH;
        }
        if (kdprimary2_open(kdprimary2_path, &kdprimary2, err, sizeof(err)) != 0) {
            fprintf(stderr, "load KD-primary2 %s: %s\n", kdprimary2_path, err);
            kdprimary_close(&kdprimary);
            return 1;
        }
        bool kdprimary2_touch = env_bool("RINHA_KDPRIMARY2_TOUCH", false);
        uint64_t touch_sum = 0;
        uint64_t touch_elapsed_ns = 0;
        if (kdprimary2_touch) {
            uint64_t start = metrics_now_ns();
            touch_sum = kdprimary2_touch_pages(&kdprimary2);
            touch_elapsed_ns = metrics_now_ns() - start;
        }
        fprintf(stderr,
                "kdprimary2 enabled=1 path=%s points=%u nodes=%u blocks=%u leaf_size=%u memory_mib=%.2f touch=%s touch_ms=%.3f sink=%llu\n",
                kdprimary2_path,
                kdprimary2.count,
                kdprimary2.node_count,
                kdprimary2.block_count,
                kdprimary2.leaf_size,
                (double)kdprimary2_runtime_memory_bytes(&kdprimary2) / 1048576.0,
                kdprimary2_touch ? "true" : "false",
                (double)touch_elapsed_ns / 1000000.0,
                (unsigned long long)touch_sum);
    }

    if (search_mode == RAW_HTTP_SEARCH_KDCLASS3 || search_mode == RAW_HTTP_SEARCH_RF_KDCLASS3) {
        const char *kdclass3_path = getenv("RINHA_KDCLASS3_PATH");
        if (kdclass3_path == NULL || kdclass3_path[0] == '\0') {
            kdclass3_path = RINHA_DEFAULT_KDCLASS3_PATH;
        }
        if (kdclass3_open(kdclass3_path, &kdclass3, err, sizeof(err)) != 0) {
            fprintf(stderr, "load KD-class3 %s: %s\n", kdclass3_path, err);
            kdprimary2_close(&kdprimary2);
            kdprimary_close(&kdprimary);
            return 1;
        }
        bool kdclass3_touch = env_bool("RINHA_KDCLASS3_TOUCH", false);
        uint64_t touch_sum = 0;
        uint64_t touch_elapsed_ns = 0;
        if (kdclass3_touch) {
            uint64_t start = metrics_now_ns();
            touch_sum = kdclass3_touch_pages(&kdclass3);
            touch_elapsed_ns = metrics_now_ns() - start;
        }
        fprintf(stderr,
                "kdclass3 enabled=1 path=%s fraud_points=%u legit_points=%u fraud_nodes=%u legit_nodes=%u leaf_size=%u memory_mib=%.2f touch=%s touch_ms=%.3f fallback=%s sink=%llu\n",
                kdclass3_path,
                kdclass3.fraud.count,
                kdclass3.legit.count,
                kdclass3.fraud.node_count,
                kdclass3.legit.node_count,
                kdclass3.leaf_size,
                (double)kdclass3_runtime_memory_bytes(&kdclass3) / 1048576.0,
                kdclass3_touch ? "true" : "false",
                (double)touch_elapsed_ns / 1000000.0,
                kdclass3_fallback_kdprimary2 ? "kdprimary2" : "none",
                (unsigned long long)touch_sum);
        if (search_mode == RAW_HTTP_SEARCH_RF_KDCLASS3) {
            fprintf(stderr,
                    "rf_kdclass3 enabled=1 model_id=%s threshold=%s low=%.8f high=%.8f "
                    "trees=%u nodes=%u features=%u fallback=kdclass3\n",
                    RF_GATE_MODEL_ID,
                    RF_GATE_THRESHOLD_NAME,
                    RF_GATE_LOW_THRESHOLD,
                    RF_GATE_HIGH_THRESHOLD,
                    RF_GATE_TREE_COUNT,
                    RF_GATE_NODE_COUNT,
                    RF_GATE_FEATURE_COUNT);
        }
    }

    if (search_mode == RAW_HTTP_SEARCH_IVF8 &&
        ivf8_index_open(index_path, &index, err, sizeof(err)) != 0) {
        fprintf(stderr, "load IVF8 index %s: %s\n", index_path, err);
        kdclass3_close(&kdclass3);
        kdprimary2_close(&kdprimary2);
        kdprimary_close(&kdprimary);
        return 1;
    }

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
        kdclass3_close(&kdclass3);
        kdprimary2_close(&kdprimary2);
        kdprimary_close(&kdprimary);
        ivf8_index_close(&index);
        return 1;
    }
    KdTree kdtree;
    memset(&kdtree, 0, sizeof(kdtree));
    kdtree.root = KDTREE_INVALID_NODE;
    if (kdtree_repair_enabled) {
        if (kdtree_mmap_nodes_for_ivf8(&kdtree, &index, kdtree_path) != 0) {
            fprintf(stderr, "load KD-tree %s: %s\n", kdtree_path, strerror(errno));
            kdclass3_close(&kdclass3);
            kdprimary2_close(&kdprimary2);
            kdprimary_close(&kdprimary);
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
        .index = search_mode == RAW_HTTP_SEARCH_IVF8 ? &index : NULL,
        .kdprimary = search_mode == RAW_HTTP_SEARCH_KDPRIMARY ? &kdprimary : NULL,
        .kdprimary2 = (search_mode == RAW_HTTP_SEARCH_KDPRIMARY2 || kdclass3_fallback_kdprimary2) ? &kdprimary2 : NULL,
        .kdclass3 = (search_mode == RAW_HTTP_SEARCH_KDCLASS3 ||
                     search_mode == RAW_HTTP_SEARCH_RF_KDCLASS3) ? &kdclass3 : NULL,
        .kdtree = kdtree_repair_enabled ? &kdtree : NULL,
        .search_mode = search_mode,
        .search_config = {
            .max_candidates = env_u32("RINHA_IVF8_MAX_CANDIDATES", RINHA_DEFAULT_IVF8_MAX_CANDIDATES),
            .probes = env_u32("RINHA_IVF8_PROBES", RINHA_DEFAULT_IVF8_PROBES),
            .impl = search_impl,
        },
        .kdtree_repair_enabled = kdtree_repair_enabled,
        .kdclass3_fallback_kdprimary2 = kdclass3_fallback_kdprimary2,
        .kdtree_repair_policy = kdtree_policy,
#if RINHA_ENABLE_METRICS
        .metrics = &metrics,
#else
        .metrics = NULL,
#endif
        .listen_mode = listen_mode,
        .exec_mode = exec_mode_text,
        .debug_instance = getenv("RINHA_DEBUG_INSTANCE"),
        .process_mode = process_mode,
        .async_runtime = NULL,
        .fast_fraud_parser = env_bool("RINHA_FAST_FRAUD_PARSER", false),
        .workers = process_mode == RAW_HTTP_PROCESS_ASYNC_WORKER ? api_workers : workers,
        .queue_size = queue_size,
    };

    if (search_mode == RAW_HTTP_SEARCH_IVF8) {
        run_index_warmup(&index, &app.search_config, warmup_mode, warmup_queries, use_madvise);
    }

    api_runtime_options runtime_options = {
        .addr = addr,
        .listen_mode = listen_mode,
        .exec_mode_text = exec_mode_text,
        .exec_mode = exec_mode,
        .workers = workers,
        .queue_size = queue_size,
        .process_mode = process_mode,
        .api_workers = api_workers,
    };

    int serve_result = run_api_process_group(&runtime_options,
                                             unix_socket,
                                             &app,
                                             api_processes);

    if (serve_result != 0) {
        kdtree_free(&kdtree);
        kdclass3_close(&kdclass3);
        kdprimary2_close(&kdprimary2);
        kdprimary_close(&kdprimary);
        ivf8_index_close(&index);
        return 1;
    }

    kdtree_free(&kdtree);
    kdclass3_close(&kdclass3);
    kdprimary2_close(&kdprimary2);
    kdprimary_close(&kdprimary);
    ivf8_index_close(&index);
    return 0;
}
