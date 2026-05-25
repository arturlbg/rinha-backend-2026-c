#define _GNU_SOURCE

#include "fdlb.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define FDLB_MAX_UPSTREAMS 16
#define FDLB_UPSTREAMS_BUFFER_SIZE 2048
#define FDLB_LISTEN_BACKLOG 4096
#define FDLB_FEEDBACK_BYTE 'C'
#define FDLB_METRICS_INTERVAL 100ULL

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

static int open_listen_socket(const char *listen_addr) {
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
#ifdef SO_REUSEPORT
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled));
#endif

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

    if (listen(fd, FDLB_LISTEN_BACKLOG) != 0) {
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

int fdlb_run(const FdlbConfig *config) {
    if (config == NULL || config->listen_addr == NULL || config->upstreams == NULL) {
        errno = EINVAL;
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

    FdlbStrategy strategy;
    if (!fdlb_parse_strategy(config->strategy, &strategy)) {
        fprintf(stderr, "invalid RINHA_FDLB_STRATEGY=%s\n", config->strategy);
        return 1;
    }

    FdlbMetrics metrics;
    memset(&metrics, 0, sizeof(metrics));
    metrics.enabled = config->metrics_enabled;
    fdlb_stop_requested = 0;
    install_signal_handlers();

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

    int listen_fd = open_listen_socket(config->listen_addr);
    if (listen_fd < 0) {
        perror("listen");
        for (size_t i = 0; i < upstream_count; i++) {
            close(upstreams[i].fd);
        }
        return 1;
    }

    fprintf(stderr, "fdlb listening on %s with %zu upstreams strategy=%u\n",
            config->listen_addr, upstream_count, (unsigned)strategy);

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
