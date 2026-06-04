#define _GNU_SOURCE

#include "fdlb.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define FDLB_MAX_UPSTREAMS 16
#define FDLB_UPSTREAMS_BUFFER_SIZE 2048
#define FDLB_DEFAULT_LISTEN_BACKLOG 4096U
#define FDLB_FEEDBACK_BYTE 'C'
#define FDLB_METRICS_INTERVAL 100ULL
#define FDLB_PROXY_BUFFER_SIZE 16384U
#define FDLB_PROXY_EPOLL_EVENTS 256U

#ifndef TCP_DEFER_ACCEPT
#define TCP_DEFER_ACCEPT 9
#endif

#ifndef TCP_FASTOPEN
#define TCP_FASTOPEN 23
#endif

#ifndef SO_BUSY_POLL
#define SO_BUSY_POLL 46
#endif

typedef struct {
    const char *path;
    int fd;
    uint32_t active;
    uint32_t max_active;
    uint64_t selected;
    uint64_t sent;
    uint64_t failures;
    uint64_t reconnects;
    uint64_t close_feedback;
} FdlbUpstream;

typedef struct {
    bool enabled;
    uint64_t accepted;
    uint64_t sent;
    uint64_t send_failures;
    uint64_t dropped;
} FdlbMetrics;

typedef struct FdlbProxyConn FdlbProxyConn;

typedef enum {
    FDLB_PROXY_SIDE_CLIENT = 1,
    FDLB_PROXY_SIDE_UPSTREAM = 2
} FdlbProxySideKind;

typedef struct {
    FdlbProxySideKind side;
    FdlbProxyConn *conn;
} FdlbProxySide;

typedef struct {
    char data[FDLB_PROXY_BUFFER_SIZE];
    size_t off;
    size_t len;
} FdlbProxyBuffer;

struct FdlbProxyConn {
    int client_fd;
    int upstream_fd;
    size_t upstream_index;
    bool client_eof;
    bool upstream_eof;
    bool client_shutdown_write;
    bool upstream_shutdown_write;
    FdlbProxyBuffer c2u;
    FdlbProxyBuffer u2c;
    FdlbProxySide *client_side;
    FdlbProxySide *upstream_side;
};

typedef struct {
    uint64_t accepted;
    uint64_t connected;
    uint64_t connect_failures;
    uint64_t closed;
    uint64_t bytes_client_to_upstream;
    uint64_t bytes_upstream_to_client;
    uint64_t sent_upstream[FDLB_MAX_UPSTREAMS];
} FdlbProxyMetrics;

static volatile sig_atomic_t fdlb_stop_requested = 0;

static void handle_stop_signal(int signum) {
    (void)signum;
    fdlb_stop_requested = 1;
}

static char *trim_ascii(char *s) {
    while (*s != '\0' && isspace((unsigned char)*s)) {
        s++;
    }

    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return s;
}

int fdlb_parse_upstreams(char *text, char **paths, size_t max_paths, size_t *count) {
    if (text == NULL || paths == NULL || count == NULL || max_paths == 0) {
        errno = EINVAL;
        return -1;
    }

    size_t n = 0;
    char *cursor = text;
    for (;;) {
        char *comma = strchr(cursor, ',');
        if (comma != NULL) {
            *comma = '\0';
        }

        char *path = trim_ascii(cursor);
        if (*path == '\0') {
            errno = EINVAL;
            return -1;
        }
        if (n >= max_paths) {
            errno = E2BIG;
            return -1;
        }
        paths[n++] = path;

        if (comma == NULL) {
            break;
        }
        cursor = comma + 1;
    }

    *count = n;
    return 0;
}

bool fdlb_parse_mode(const char *text, FdlbMode *mode) {
    if (mode == NULL) {
        return false;
    }
    if (text == NULL || *text == '\0' || strcmp(text, "fdpass") == 0) {
        *mode = FDLB_MODE_FDPASS;
        return true;
    }
    if (strcmp(text, "proxy") == 0) {
        *mode = FDLB_MODE_PROXY;
        return true;
    }
    return false;
}

bool fdlb_parse_strategy(const char *text, FdlbStrategy *strategy) {
    if (strategy == NULL) {
        return false;
    }
    if (text == NULL || *text == '\0' || strcmp(text, "round_robin") == 0) {
        *strategy = FDLB_STRATEGY_ROUND_ROBIN;
        return true;
    }
    if (strcmp(text, "least_active") == 0) {
        *strategy = FDLB_STRATEGY_LEAST_ACTIVE;
        return true;
    }
    if (strcmp(text, "power_of_two") == 0) {
        *strategy = FDLB_STRATEGY_POWER_OF_TWO;
        return true;
    }
    return false;
}

size_t fdlb_round_robin_next(size_t *cursor, size_t count) {
    if (cursor == NULL || count == 0) {
        return 0;
    }

    size_t selected = *cursor % count;
    *cursor = (*cursor + 1) % count;
    return selected;
}

static size_t select_least_active(size_t *cursor, const uint32_t *active, size_t count) {
    if (cursor == NULL || active == NULL || count == 0) {
        return 0;
    }

    size_t start = *cursor % count;
    size_t selected = start;
    uint32_t best = UINT32_MAX;
    for (size_t offset = 0; offset < count; offset++) {
        size_t i = (start + offset) % count;
        if (active[i] < best) {
            best = active[i];
            selected = i;
        }
    }
    *cursor = (selected + 1U) % count;
    return selected;
}

size_t fdlb_select_upstream(FdlbStrategy strategy, size_t *cursor, const uint32_t *active, size_t count) {
    if (count == 0) {
        return 0;
    }
    if (strategy == FDLB_STRATEGY_LEAST_ACTIVE) {
        return select_least_active(cursor, active, count);
    }
    if (strategy == FDLB_STRATEGY_POWER_OF_TWO && count > 1U && cursor != NULL && active != NULL) {
        size_t first = fdlb_round_robin_next(cursor, count);
        size_t second = fdlb_round_robin_next(cursor, count);
        if (active[second] < active[first]) {
            return second;
        }
        if (active[first] < active[second]) {
            return first;
        }
        return first < second ? first : second;
    }
    return fdlb_round_robin_next(cursor, count);
}

void fdlb_decrement_active(uint32_t *active) {
    if (active != NULL && *active > 0U) {
        (*active)--;
    }
}

int fdlb_send_one_fd(int control_fd, int fd) {
    char data[1] = {0};
    char control[CMSG_SPACE(sizeof(int))];
    struct iovec iov;
    struct msghdr msg;

    memset(control, 0, sizeof(control));
    memset(&iov, 0, sizeof(iov));
    memset(&msg, 0, sizeof(msg));

    iov.iov_base = data;
    iov.iov_len = sizeof(data);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));

    ssize_t n;
    do {
        n = sendmsg(control_fd, &msg, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);

    return n == (ssize_t)sizeof(data) ? 0 : -1;
}

static void install_signal_handlers(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_stop_signal;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)sigaction(SIGINT, &action, NULL);
}

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}

static void sleep_ms(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000U);
    ts.tv_nsec = (long)(ms % 1000U) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

static int connect_unix_once(const char *path) {
    if (path == NULL || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, strlen(path) + 1U);

    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

static int connect_unix_with_retry(const char *path, uint32_t retry_ms, uint32_t timeout_ms) {
    uint64_t start = monotonic_ms();
    uint32_t sleep_for = retry_ms == 0U ? 25U : retry_ms;

    for (;;) {
        int fd = connect_unix_once(path);
        if (fd >= 0) {
            return fd;
        }

        uint64_t now = monotonic_ms();
        if (timeout_ms != 0U && now >= start && now - start >= timeout_ms) {
            return -1;
        }
        sleep_ms(sleep_for);
    }
}

static int parse_listen_addr(const char *addr, uint16_t *port, struct in_addr *bind_addr) {
    if (addr == NULL || *addr == '\0' || port == NULL || bind_addr == NULL) {
        errno = EINVAL;
        return -1;
    }

    const char *port_text = addr;
    char host[64];
    host[0] = '\0';

    const char *colon = strrchr(addr, ':');
    if (colon != NULL) {
        size_t host_len = (size_t)(colon - addr);
        if (host_len >= sizeof(host)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(host, addr, host_len);
        host[host_len] = '\0';
        port_text = colon + 1;
    }

    if (*port_text == '\0') {
        errno = EINVAL;
        return -1;
    }

    char *end = NULL;
    unsigned long parsed = strtoul(port_text, &end, 10);
    if (end == port_text || *end != '\0' || parsed == 0UL || parsed > 65535UL) {
        errno = EINVAL;
        return -1;
    }

    if (host[0] == '\0' || strcmp(host, "0.0.0.0") == 0) {
        bind_addr->s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, host, bind_addr) != 1) {
        errno = EINVAL;
        return -1;
    }

    *port = (uint16_t)parsed;
    return 0;
}

static void apply_optional_socket_tuning(int fd, const FdlbConfig *config) {
    int enabled = 1;
    if (config == NULL) {
        return;
    }

#ifdef SO_REUSEPORT
    if (config->reuseport) {
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled));
    }
#endif
    if (config->tcp_defer_accept) {
        (void)setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &enabled, sizeof(enabled));
    }
    if (config->tcp_fastopen) {
        int backlog = config->listen_backlog == 0U ?
            (int)FDLB_DEFAULT_LISTEN_BACKLOG : (int)config->listen_backlog;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_FASTOPEN, &backlog, sizeof(backlog));
    }
    if (config->so_busy_poll_us > 0U) {
        int busy_poll_us = (int)config->so_busy_poll_us;
        (void)setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &busy_poll_us, sizeof(busy_poll_us));
    }
}

static int open_listen_socket(const FdlbConfig *config) {
    const char *listen_addr = config == NULL ? NULL : config->listen_addr;
    uint16_t port = 0;
    struct in_addr bind_addr;
    if (parse_listen_addr(listen_addr, &port, &bind_addr) != 0) {
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }

    int enabled = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    apply_optional_socket_tuning(fd, config);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr = bind_addr;
    addr.sin_port = htons(port);

    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    uint32_t backlog = config == NULL || config->listen_backlog == 0U ?
        FDLB_DEFAULT_LISTEN_BACKLOG : config->listen_backlog;
    if (listen(fd, (int)backlog) != 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    return fd;
}

static int accept_client(int listen_fd) {
    for (;;) {
        if (fdlb_stop_requested) {
            errno = EINTR;
            return -1;
        }
#ifdef __linux__
        int fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
#else
        int fd = accept(listen_fd, NULL, NULL);
#endif
        if (fd >= 0) {
            return fd;
        }
        if (errno == EINTR) {
            if (fdlb_stop_requested) {
                return -1;
            }
            continue;
        }
        return -1;
    }
}

static bool reconnect_upstream(FdlbUpstream *upstream, uint32_t retry_ms, uint32_t timeout_ms) {
    if (upstream->fd >= 0) {
        close(upstream->fd);
        upstream->fd = -1;
    }

    upstream->fd = connect_unix_with_retry(upstream->path, retry_ms, timeout_ms);
    if (upstream->fd >= 0) {
        upstream->reconnects++;
        upstream->active = 0;
        return true;
    }
    return false;
}

static void poll_feedback(FdlbUpstream *upstream) {
    char buf[256];
    for (;;) {
        ssize_t n = recv(upstream->fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] == FDLB_FEEDBACK_BYTE) {
                    fdlb_decrement_active(&upstream->active);
                    upstream->close_feedback++;
                }
            }
            continue;
        }
        if (n == 0) {
            close(upstream->fd);
            upstream->fd = -1;
            upstream->active = 0;
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return;
        }
        close(upstream->fd);
        upstream->fd = -1;
        upstream->active = 0;
        return;
    }
}

static void poll_all_feedback(FdlbUpstream *upstreams, size_t upstream_count) {
    for (size_t i = 0; i < upstream_count; i++) {
        if (upstreams[i].fd >= 0) {
            poll_feedback(&upstreams[i]);
        }
    }
}

static size_t select_with_attempts(FdlbStrategy strategy,
                                   size_t *cursor,
                                   const FdlbUpstream *upstreams,
                                   size_t upstream_count,
                                   const bool attempted[FDLB_MAX_UPSTREAMS]) {
    uint32_t active[FDLB_MAX_UPSTREAMS];
    for (size_t i = 0; i < upstream_count; i++) {
        active[i] = attempted[i] ? UINT32_MAX : upstreams[i].active;
    }
    size_t selected = fdlb_select_upstream(strategy, cursor, active, upstream_count);
    if (selected < upstream_count && !attempted[selected]) {
        return selected;
    }
    for (size_t i = 0; i < upstream_count; i++) {
        if (!attempted[i]) {
            return i;
        }
    }
    return 0;
}

static void note_sent(FdlbUpstream *upstream) {
    upstream->selected++;
    upstream->sent++;
    upstream->active++;
    if (upstream->active > upstream->max_active) {
        upstream->max_active = upstream->active;
    }
}

static bool deliver_fd(FdlbUpstream *upstreams,
                       size_t upstream_count,
                       size_t *cursor,
                       FdlbStrategy strategy,
                       int client_fd,
                       uint32_t retry_ms,
                       FdlbMetrics *metrics) {
    bool attempted[FDLB_MAX_UPSTREAMS];
    memset(attempted, 0, sizeof(attempted));

    for (size_t attempt = 0; attempt < upstream_count; attempt++) {
        size_t selected = select_with_attempts(strategy, cursor, upstreams, upstream_count, attempted);
        attempted[selected] = true;
        FdlbUpstream *upstream = &upstreams[selected];
        if (upstream->fd < 0 && !reconnect_upstream(upstream, retry_ms, retry_ms * 4U)) {
            upstream->failures++;
            if (metrics != NULL && metrics->enabled) {
                metrics->send_failures++;
            }
            continue;
        }

        if (fdlb_send_one_fd(upstream->fd, client_fd) == 0) {
            note_sent(upstream);
            if (metrics != NULL && metrics->enabled) {
                metrics->sent++;
            }
            return true;
        }

        upstream->failures++;
        if (metrics != NULL && metrics->enabled) {
            metrics->send_failures++;
        }
        if (reconnect_upstream(upstream, retry_ms, retry_ms * 4U) &&
            fdlb_send_one_fd(upstream->fd, client_fd) == 0) {
            note_sent(upstream);
            if (metrics != NULL && metrics->enabled) {
                metrics->sent++;
            }
            return true;
        }
    }

    return false;
}

static void print_metrics(const FdlbMetrics *metrics,
                          const FdlbUpstream *upstreams,
                          size_t upstream_count,
                          FdlbStrategy strategy) {
    if (metrics == NULL || !metrics->enabled) {
        return;
    }
    fprintf(stderr,
            "fdlb_metrics strategy=%u accepted=%llu sent=%llu send_failures=%llu dropped=%llu\n",
            (unsigned)strategy,
            (unsigned long long)metrics->accepted,
            (unsigned long long)metrics->sent,
            (unsigned long long)metrics->send_failures,
            (unsigned long long)metrics->dropped);
    for (size_t i = 0; i < upstream_count; i++) {
        fprintf(stderr,
                "fdlb_upstream index=%zu sent=%llu selected=%llu active=%u max_active=%u feedback=%llu failures=%llu reconnects=%llu path=%s\n",
                i,
                (unsigned long long)upstreams[i].sent,
                (unsigned long long)upstreams[i].selected,
                upstreams[i].active,
                upstreams[i].max_active,
                (unsigned long long)upstreams[i].close_feedback,
                (unsigned long long)upstreams[i].failures,
                (unsigned long long)upstreams[i].reconnects,
                upstreams[i].path);
    }
}

static bool lean_deliver_fd(FdlbUpstream *upstreams,
                            size_t upstream_count,
                            size_t *cursor,
                            int client_fd,
                            uint32_t retry_ms) {
    if (upstreams == NULL || upstream_count == 0U || cursor == NULL) {
        return false;
    }

    for (size_t attempt = 0; attempt < upstream_count; attempt++) {
        size_t selected = fdlb_round_robin_next(cursor, upstream_count);
        FdlbUpstream *upstream = &upstreams[selected];
        if (upstream->fd < 0) {
            upstream->fd = connect_unix_with_retry(upstream->path, retry_ms, retry_ms * 4U);
            if (upstream->fd < 0) {
                continue;
            }
        }

        if (fdlb_send_one_fd(upstream->fd, client_fd) == 0) {
            return true;
        }

        close(upstream->fd);
        upstream->fd = connect_unix_with_retry(upstream->path, retry_ms, retry_ms * 4U);
        if (upstream->fd >= 0 && fdlb_send_one_fd(upstream->fd, client_fd) == 0) {
            return true;
        }
    }

    return false;
}

static int fdlb_run_lean(const FdlbConfig *config, char **paths, size_t upstream_count) {
    FdlbUpstream upstreams[FDLB_MAX_UPSTREAMS];
    memset(upstreams, 0, sizeof(upstreams));
    for (size_t i = 0; i < upstream_count; i++) {
        upstreams[i].path = paths[i];
        upstreams[i].fd = connect_unix_with_retry(paths[i],
                                                  config->connect_retry_ms,
                                                  config->startup_timeout_ms);
        if (upstreams[i].fd < 0) {
            fprintf(stderr, "failed to connect upstream %s: %s\n", paths[i], strerror(errno));
            for (size_t j = 0; j < i; j++) {
                close(upstreams[j].fd);
            }
            return 1;
        }
    }

    int listen_fd = open_listen_socket(config);
    if (listen_fd < 0) {
        perror("listen");
        for (size_t i = 0; i < upstream_count; i++) {
            close(upstreams[i].fd);
        }
        return 1;
    }

    fprintf(stderr,
            "fdlb lean listening on %s with %zu upstreams backlog=%u defer_accept=%u fastopen=%u busy_poll_us=%u reuseport=%u\n",
            config->listen_addr,
            upstream_count,
            config->listen_backlog == 0U ? FDLB_DEFAULT_LISTEN_BACKLOG : config->listen_backlog,
            config->tcp_defer_accept ? 1U : 0U,
            config->tcp_fastopen ? 1U : 0U,
            config->so_busy_poll_us,
            config->reuseport ? 1U : 0U);

    size_t cursor = 0;
    uint32_t retry_ms = config->connect_retry_ms == 0U ? 25U : config->connect_retry_ms;
    for (; !fdlb_stop_requested;) {
        int client_fd = accept_client(listen_fd);
        if (client_fd < 0) {
            if (fdlb_stop_requested) {
                break;
            }
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
                sleep_ms(1);
            }
            continue;
        }

        if (!lean_deliver_fd(upstreams, upstream_count, &cursor, client_fd, retry_ms)) {
            close(client_fd);
            continue;
        }
        close(client_fd);
    }

    close(listen_fd);
    for (size_t i = 0; i < upstream_count; i++) {
        if (upstreams[i].fd >= 0) {
            close(upstreams[i].fd);
        }
    }
    return 0;
}

static int set_nonblocking_fd(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if ((flags & O_NONBLOCK) != 0) {
        return 0;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int accept_client_nonblocking(int listen_fd) {
    for (;;) {
#ifdef __linux__
        int fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
        int fd = accept(listen_fd, NULL, NULL);
#endif
        if (fd >= 0) {
#ifndef __linux__
            (void)set_nonblocking_fd(fd);
#endif
            return fd;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
}

static int connect_unix_nonblocking(const char *path) {
    if (path == NULL || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, strlen(path) + 1U);

    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0 && errno != EINPROGRESS) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

static void proxy_buffer_compact(FdlbProxyBuffer *buf) {
    if (buf == NULL || buf->off == 0U) {
        return;
    }
    if (buf->len > 0U) {
        memmove(buf->data, buf->data + buf->off, buf->len);
    }
    buf->off = 0U;
}

static size_t proxy_buffer_space(FdlbProxyBuffer *buf) {
    if (buf == NULL) {
        return 0U;
    }
    proxy_buffer_compact(buf);
    return sizeof(buf->data) - buf->len;
}

static bool proxy_flush_buffer(int fd, FdlbProxyBuffer *buf, uint64_t *bytes_written) {
    while (buf->len > 0U) {
        ssize_t n = send(fd,
                         buf->data + buf->off,
                         buf->len,
#ifdef MSG_NOSIGNAL
                         MSG_NOSIGNAL
#else
                         0
#endif
        );
        if (n > 0) {
            buf->off += (size_t)n;
            buf->len -= (size_t)n;
            if (bytes_written != NULL) {
                *bytes_written += (uint64_t)n;
            }
            if (buf->len == 0U) {
                buf->off = 0U;
            }
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            return true;
        }
        return false;
    }
    return true;
}

static bool proxy_read_into_buffer(int fd, FdlbProxyBuffer *buf, bool *eof) {
    for (;;) {
        size_t space = proxy_buffer_space(buf);
        if (space == 0U) {
            return true;
        }
        ssize_t n = recv(fd, buf->data + buf->len, space, 0);
        if (n > 0) {
            buf->len += (size_t)n;
            continue;
        }
        if (n == 0) {
            if (eof != NULL) {
                *eof = true;
            }
            return true;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return true;
        }
        return false;
    }
}

static void proxy_maybe_shutdown(FdlbProxyConn *conn) {
    if (conn == NULL) {
        return;
    }
    if (conn->client_eof && conn->c2u.len == 0U && !conn->upstream_shutdown_write) {
        (void)shutdown(conn->upstream_fd, SHUT_WR);
        conn->upstream_shutdown_write = true;
    }
    if (conn->upstream_eof && conn->u2c.len == 0U && !conn->client_shutdown_write) {
        (void)shutdown(conn->client_fd, SHUT_WR);
        conn->client_shutdown_write = true;
    }
}

static bool proxy_conn_done(const FdlbProxyConn *conn) {
    if (conn == NULL) {
        return true;
    }
    return conn->client_eof &&
           conn->upstream_eof &&
           conn->c2u.len == 0U &&
           conn->u2c.len == 0U;
}

static void proxy_close_conn(int epoll_fd, FdlbProxyConn *conn, FdlbProxyMetrics *metrics) {
    if (conn == NULL) {
        return;
    }
    if (conn->client_fd >= 0) {
        (void)epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->client_fd, NULL);
        close(conn->client_fd);
    }
    if (conn->upstream_fd >= 0) {
        (void)epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->upstream_fd, NULL);
        close(conn->upstream_fd);
    }
    if (metrics != NULL) {
        metrics->closed++;
    }
    if (conn->client_side != NULL) {
        conn->client_side->conn = NULL;
    }
    if (conn->upstream_side != NULL) {
        conn->upstream_side->conn = NULL;
    }
    free(conn);
}

static int proxy_update_side(int epoll_fd, int fd, FdlbProxySide *side, uint32_t events) {
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = events | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
    event.data.ptr = side;
    return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event);
}

static bool proxy_update_interests(int epoll_fd, FdlbProxyConn *conn) {
    uint32_t client_events = 0U;
    uint32_t upstream_events = 0U;

    if (!conn->client_eof && proxy_buffer_space(&conn->c2u) > 0U) {
        client_events |= EPOLLIN;
    }
    if (conn->u2c.len > 0U) {
        client_events |= EPOLLOUT;
    }
    if (!conn->upstream_eof && proxy_buffer_space(&conn->u2c) > 0U) {
        upstream_events |= EPOLLIN;
    }
    if (conn->c2u.len > 0U) {
        upstream_events |= EPOLLOUT;
    }

    if (proxy_update_side(epoll_fd, conn->client_fd, conn->client_side, client_events) != 0) {
        return false;
    }
    if (proxy_update_side(epoll_fd, conn->upstream_fd, conn->upstream_side, upstream_events) != 0) {
        return false;
    }
    return true;
}

static bool proxy_add_side(int epoll_fd, int fd, FdlbProxySide *side, uint32_t events) {
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = events | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
    event.data.ptr = side;
    return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) == 0;
}

static bool proxy_add_connection(int epoll_fd,
                                 int client_fd,
                                 int upstream_fd,
                                 size_t upstream_index,
                                 FdlbProxyMetrics *metrics) {
    FdlbProxyConn *conn = (FdlbProxyConn *)calloc(1, sizeof(*conn));
    if (conn == NULL) {
        close(client_fd);
        close(upstream_fd);
        return false;
    }
    conn->client_fd = client_fd;
    conn->upstream_fd = upstream_fd;
    conn->upstream_index = upstream_index;
    conn->client_side = (FdlbProxySide *)calloc(1, sizeof(*conn->client_side));
    conn->upstream_side = (FdlbProxySide *)calloc(1, sizeof(*conn->upstream_side));
    if (conn->client_side == NULL || conn->upstream_side == NULL) {
        free(conn->client_side);
        free(conn->upstream_side);
        free(conn);
        close(client_fd);
        close(upstream_fd);
        return false;
    }
    conn->client_side->side = FDLB_PROXY_SIDE_CLIENT;
    conn->client_side->conn = conn;
    conn->upstream_side->side = FDLB_PROXY_SIDE_UPSTREAM;
    conn->upstream_side->conn = conn;

    if (!proxy_add_side(epoll_fd, client_fd, conn->client_side, EPOLLIN) ||
        !proxy_add_side(epoll_fd, upstream_fd, conn->upstream_side, EPOLLIN)) {
        proxy_close_conn(epoll_fd, conn, NULL);
        return false;
    }
    if (metrics != NULL) {
        metrics->connected++;
        if (upstream_index < FDLB_MAX_UPSTREAMS) {
            metrics->sent_upstream[upstream_index]++;
        }
    }
    return true;
}

static bool proxy_handle_side_event(int epoll_fd,
                                    FdlbProxySide *side,
                                    uint32_t events,
                                    FdlbProxyMetrics *metrics) {
    FdlbProxyConn *conn = side == NULL ? NULL : side->conn;
    if (conn == NULL) {
        return false;
    }

    bool ok = true;
    if (side->side == FDLB_PROXY_SIDE_CLIENT) {
        if ((events & EPOLLOUT) != 0) {
            ok = proxy_flush_buffer(conn->client_fd,
                                    &conn->u2c,
                                    metrics == NULL ? NULL : &metrics->bytes_upstream_to_client);
        }
        if (ok && (events & EPOLLIN) != 0) {
            ok = proxy_read_into_buffer(conn->client_fd, &conn->c2u, &conn->client_eof);
        }
        if ((events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
            conn->client_eof = true;
        }
    } else {
        if ((events & EPOLLOUT) != 0) {
            ok = proxy_flush_buffer(conn->upstream_fd,
                                    &conn->c2u,
                                    metrics == NULL ? NULL : &metrics->bytes_client_to_upstream);
        }
        if (ok && (events & EPOLLIN) != 0) {
            ok = proxy_read_into_buffer(conn->upstream_fd, &conn->u2c, &conn->upstream_eof);
        }
        if ((events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
            conn->upstream_eof = true;
        }
    }

    if (!ok) {
        proxy_close_conn(epoll_fd, conn, metrics);
        return true;
    }
    proxy_maybe_shutdown(conn);
    if (proxy_conn_done(conn)) {
        proxy_close_conn(epoll_fd, conn, metrics);
        return true;
    }
    if (!proxy_update_interests(epoll_fd, conn)) {
        proxy_close_conn(epoll_fd, conn, metrics);
    }
    return true;
}

static void proxy_print_metrics(const FdlbProxyMetrics *metrics, char **paths, size_t upstream_count) {
    if (metrics == NULL) {
        return;
    }
    fprintf(stderr,
            "proxy_metrics accepted=%llu connected=%llu connect_failures=%llu closed=%llu c2u_bytes=%llu u2c_bytes=%llu\n",
            (unsigned long long)metrics->accepted,
            (unsigned long long)metrics->connected,
            (unsigned long long)metrics->connect_failures,
            (unsigned long long)metrics->closed,
            (unsigned long long)metrics->bytes_client_to_upstream,
            (unsigned long long)metrics->bytes_upstream_to_client);
    for (size_t i = 0; i < upstream_count; i++) {
        fprintf(stderr,
                "proxy_upstream index=%zu sent=%llu path=%s\n",
                i,
                (unsigned long long)metrics->sent_upstream[i],
                paths[i]);
    }
}

static int fdlb_run_proxy(const FdlbConfig *config, char **paths, size_t upstream_count) {
    uint32_t retry_ms = config->connect_retry_ms == 0U ? 25U : config->connect_retry_ms;
    for (size_t i = 0; i < upstream_count; i++) {
        int probe_fd = connect_unix_with_retry(paths[i], retry_ms, config->startup_timeout_ms);
        if (probe_fd < 0) {
            fprintf(stderr, "failed to connect proxy upstream %s: %s\n", paths[i], strerror(errno));
            return 1;
        }
        close(probe_fd);
    }

    int listen_fd = open_listen_socket(config);
    if (listen_fd < 0) {
        perror("listen");
        return 1;
    }
    if (set_nonblocking_fd(listen_fd) != 0) {
        perror("nonblocking listen");
        close(listen_fd);
        return 1;
    }

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        close(listen_fd);
        return 1;
    }

    FdlbProxySide listener_side = {
        .side = 0,
        .conn = NULL,
    };
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLERR | EPOLLHUP;
    event.data.ptr = &listener_side;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event) != 0) {
        perror("epoll_ctl listen");
        close(epoll_fd);
        close(listen_fd);
        return 1;
    }

    fprintf(stderr,
            "proxy_lb listening on %s with %zu unix upstreams backlog=%u reuseport=%u\n",
            config->listen_addr,
            upstream_count,
            config->listen_backlog == 0U ? FDLB_DEFAULT_LISTEN_BACKLOG : config->listen_backlog,
            config->reuseport ? 1U : 0U);

    FdlbProxyMetrics metrics;
    memset(&metrics, 0, sizeof(metrics));
    size_t cursor = 0;
    struct epoll_event events[FDLB_PROXY_EPOLL_EVENTS];
    while (!fdlb_stop_requested) {
        int n = epoll_wait(epoll_fd, events, (int)FDLB_PROXY_EPOLL_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n; i++) {
            FdlbProxySide *side = (FdlbProxySide *)events[i].data.ptr;
            if (side == &listener_side) {
                if ((events[i].events & (EPOLLERR | EPOLLHUP)) != 0) {
                    fdlb_stop_requested = 1;
                    break;
                }
                for (;;) {
                    int client_fd = accept_client_nonblocking(listen_fd);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        if (errno == EINTR) {
                            continue;
                        }
                        break;
                    }
                    metrics.accepted++;
                    size_t selected = fdlb_round_robin_next(&cursor, upstream_count);
                    int upstream_fd = connect_unix_nonblocking(paths[selected]);
                    if (upstream_fd < 0) {
                        metrics.connect_failures++;
                        close(client_fd);
                        continue;
                    }
                    if (!proxy_add_connection(epoll_fd, client_fd, upstream_fd, selected, &metrics)) {
                        metrics.connect_failures++;
                    }
                }
                continue;
            }
            (void)proxy_handle_side_event(epoll_fd, side, events[i].events, &metrics);
        }
    }

    proxy_print_metrics(&metrics, paths, upstream_count);
    close(epoll_fd);
    close(listen_fd);
    return 0;
}

int fdlb_run(const FdlbConfig *config) {
    if (config == NULL || config->listen_addr == NULL || config->upstreams == NULL) {
        errno = EINVAL;
        return 1;
    }

    FdlbMode mode;
    if (!fdlb_parse_mode(config->mode, &mode)) {
        fprintf(stderr, "invalid RINHA_LB_MODE=%s\n", config->mode == NULL ? "" : config->mode);
        return 1;
    }

    char upstream_buffer[FDLB_UPSTREAMS_BUFFER_SIZE];
    size_t upstreams_len = strlen(config->upstreams);
    if (upstreams_len == 0U || upstreams_len >= sizeof(upstream_buffer)) {
        fprintf(stderr, "invalid RINHA_FDPASS_UPSTREAMS\n");
        return 1;
    }
    memcpy(upstream_buffer, config->upstreams, upstreams_len + 1U);

    char *paths[FDLB_MAX_UPSTREAMS];
    size_t upstream_count = 0;
    if (fdlb_parse_upstreams(upstream_buffer, paths, FDLB_MAX_UPSTREAMS, &upstream_count) != 0) {
        perror("parse upstreams");
        return 1;
    }

    fdlb_stop_requested = 0;
    install_signal_handlers();
    if (mode == FDLB_MODE_PROXY) {
        return fdlb_run_proxy(config, paths, upstream_count);
    }
    if (config->lean) {
        return fdlb_run_lean(config, paths, upstream_count);
    }

    FdlbStrategy strategy;
    if (!fdlb_parse_strategy(config->strategy, &strategy)) {
        fprintf(stderr, "invalid RINHA_FDLB_STRATEGY=%s\n", config->strategy);
        return 1;
    }

    FdlbMetrics metrics;
    memset(&metrics, 0, sizeof(metrics));
    metrics.enabled = config->metrics_enabled;

    FdlbUpstream upstreams[FDLB_MAX_UPSTREAMS];
    memset(upstreams, 0, sizeof(upstreams));
    for (size_t i = 0; i < upstream_count; i++) {
        upstreams[i].path = paths[i];
        upstreams[i].fd = -1;
        upstreams[i].fd = connect_unix_with_retry(paths[i],
                                                  config->connect_retry_ms,
                                                  config->startup_timeout_ms);
        if (upstreams[i].fd < 0) {
            fprintf(stderr, "failed to connect upstream %s: %s\n", paths[i], strerror(errno));
            for (size_t j = 0; j < i; j++) {
                close(upstreams[j].fd);
            }
            return 1;
        }
    }

    int listen_fd = open_listen_socket(config);
    if (listen_fd < 0) {
        perror("listen");
        for (size_t i = 0; i < upstream_count; i++) {
            close(upstreams[i].fd);
        }
        return 1;
    }

    fprintf(stderr,
            "fdlb listening on %s with %zu upstreams strategy=%u backlog=%u defer_accept=%u fastopen=%u busy_poll_us=%u reuseport=%u\n",
            config->listen_addr,
            upstream_count,
            (unsigned)strategy,
            config->listen_backlog == 0U ? FDLB_DEFAULT_LISTEN_BACKLOG : config->listen_backlog,
            config->tcp_defer_accept ? 1U : 0U,
            config->tcp_fastopen ? 1U : 0U,
            config->so_busy_poll_us,
            config->reuseport ? 1U : 0U);

    size_t cursor = 0;
    for (; !fdlb_stop_requested;) {
        poll_all_feedback(upstreams, upstream_count);
        int client_fd = accept_client(listen_fd);
        if (client_fd < 0) {
            if (fdlb_stop_requested) {
                break;
            }
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
                sleep_ms(1);
                continue;
            }
            continue;
        }
        if (metrics.enabled) {
            metrics.accepted++;
        }

        if (!deliver_fd(upstreams,
                        upstream_count,
                        &cursor,
                        strategy,
                        client_fd,
                        config->connect_retry_ms == 0U ? 25U : config->connect_retry_ms,
                        &metrics)) {
            if (metrics.enabled) {
                metrics.dropped++;
            }
            close(client_fd);
            continue;
        }
        close(client_fd);
        if (metrics.enabled && metrics.accepted % FDLB_METRICS_INTERVAL == 0ULL) {
            print_metrics(&metrics, upstreams, upstream_count, strategy);
        }
    }
    poll_all_feedback(upstreams, upstream_count);
    print_metrics(&metrics, upstreams, upstream_count, strategy);
    close(listen_fd);
    for (size_t i = 0; i < upstream_count; i++) {
        if (upstreams[i].fd >= 0) {
            close(upstreams[i].fd);
        }
    }
    return 0;
}
