#define _GNU_SOURCE

#include "fdpass.h"

#include "config.h"
#include "fd_queue.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

typedef struct {
    int client_fd;
    const raw_http_app *app;
} fdpass_client_arg;

typedef struct {
    int control_fd;
    struct fdpass_runtime *runtime;
} fdpass_control_arg;

typedef struct fdpass_runtime {
    const raw_http_app *app;
    fdpass_options options;
    FdQueue queue;
    bool queue_ready;
} fdpass_runtime;

static int mkdir_parent(const char *path) {
    const char *slash = strrchr(path, '/');
    if (slash == NULL || slash == path) {
        return 0;
    }

    char dir[sizeof(((struct sockaddr_un *)0)->sun_path)];
    size_t len = (size_t)(slash - path);
    if (len >= sizeof(dir)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(dir, path, len);
    dir[len] = '\0';

    if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static void tune_client_fd(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }

    int enabled = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
#ifdef TCP_QUICKACK
    (void)setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &enabled, sizeof(enabled));
#endif
}

int fdpass_recv_one_fd(int control_fd) {
    char data[16];
    char control[CMSG_SPACE(sizeof(int) * 4)];
    struct iovec iov;
    struct msghdr msg;

    memset(&iov, 0, sizeof(iov));
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = data;
    iov.iov_len = sizeof(data);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    ssize_t n;
    do {
        n = recvmsg(control_fd, &msg, 0);
    } while (n < 0 && errno == EINTR);

    if (n == 0) {
        return -2;
    }
    if (n < 0) {
        return -1;
    }
    if ((msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) != 0) {
        errno = EBADMSG;
        return -1;
    }

    int received_fd = -1;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
         cmsg != NULL;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
            continue;
        }
        if (cmsg->cmsg_len < CMSG_LEN(0)) {
            continue;
        }

        size_t payload_len = cmsg->cmsg_len - CMSG_LEN(0);
        size_t fd_count = payload_len / sizeof(int);
        const int *fds = (const int *)CMSG_DATA(cmsg);
        for (size_t i = 0; i < fd_count; i++) {
            if (received_fd < 0) {
                received_fd = fds[i];
            } else {
                close(fds[i]);
            }
        }
    }

    if (received_fd < 0) {
        errno = EBADMSG;
        return -1;
    }
    return received_fd;
}

int fdpass_send_one_fd(int control_fd, int fd) {
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

static void *fdpass_client_thread(void *arg) {
    fdpass_client_arg *client = (fdpass_client_arg *)arg;
    int client_fd = client->client_fd;
    const raw_http_app *app = client->app;
    free(client);

    tune_client_fd(client_fd);
    (void)raw_http_handle_connection(client_fd, app);
    close(client_fd);
    return NULL;
}

static RinhaMetrics *runtime_metrics(const fdpass_runtime *runtime) {
    if (runtime == NULL || runtime->app == NULL ||
        runtime->app->metrics == NULL || !runtime->app->metrics->enabled) {
        return NULL;
    }
    return runtime->app->metrics;
}

static void start_client_thread(int client_fd, const raw_http_app *app) {
    fdpass_client_arg *arg = (fdpass_client_arg *)malloc(sizeof(*arg));
    if (arg == NULL) {
        close(client_fd);
        return;
    }
    arg->client_fd = client_fd;
    arg->app = app;

    pthread_t thread;
    if (pthread_create(&thread, NULL, fdpass_client_thread, arg) != 0) {
        free(arg);
        close(client_fd);
        return;
    }
    (void)pthread_detach(thread);
}

static void handle_client_fd(int client_fd, const raw_http_app *app) {
    tune_client_fd(client_fd);
    (void)raw_http_handle_connection(client_fd, app);
    close(client_fd);
}

static void *fdpass_worker_thread(void *arg) {
    fdpass_runtime *runtime = (fdpass_runtime *)arg;
    RinhaMetrics *metrics = runtime_metrics(runtime);
    for (;;) {
        FdQueueItem item;
        if (!fd_queue_pop(&runtime->queue, &item)) {
            break;
        }
        if (metrics != NULL) {
            metrics_inc(&metrics->fd_queue_dequeued);
            metrics_observe(&metrics->queue_wait, metrics_now_ns() - item.enqueued_ns);
        }
        handle_client_fd(item.fd, runtime->app);
    }
    return NULL;
}

static int start_worker_threads(fdpass_runtime *runtime) {
    if (runtime->options.workers == 0) {
        errno = EINVAL;
        return -1;
    }
    for (uint32_t i = 0; i < runtime->options.workers; i++) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, fdpass_worker_thread, runtime) != 0) {
            return -1;
        }
        (void)pthread_detach(thread);
    }
    return 0;
}

static void dispatch_client_fd(fdpass_runtime *runtime, int client_fd) {
    RinhaMetrics *metrics = runtime_metrics(runtime);
    if (metrics != NULL) {
        metrics_inc(&metrics->adopted_fds);
    }

    if (runtime->options.exec_mode == FDPASS_EXEC_WORKER_POOL) {
        uint64_t now = metrics != NULL ? metrics_now_ns() : 0u;
        if (fd_queue_push(&runtime->queue, client_fd, now)) {
            if (metrics != NULL) {
                metrics_inc(&metrics->fd_queue_enqueued);
            }
            return;
        }
        if (metrics != NULL) {
            metrics_inc(&metrics->fd_queue_dropped);
        }
        close(client_fd);
        return;
    }

    start_client_thread(client_fd, runtime->app);
}

static void *fdpass_control_thread(void *arg) {
    fdpass_control_arg *control = (fdpass_control_arg *)arg;
    int control_fd = control->control_fd;
    fdpass_runtime *runtime = control->runtime;
    RinhaMetrics *metrics = runtime_metrics(runtime);
    free(control);

    for (;;) {
        uint64_t start = metrics != NULL ? metrics_now_ns() : 0u;
        int client_fd = fdpass_recv_one_fd(control_fd);
        if (metrics != NULL) {
            metrics_observe(&metrics->fdpass_receive, metrics_now_ns() - start);
        }
        if (client_fd >= 0) {
            dispatch_client_fd(runtime, client_fd);
            continue;
        }
        if (metrics != NULL && client_fd != -2) {
            metrics_inc(&metrics->fdpass_receive_errors);
        }
        break;
    }

    close(control_fd);
    return NULL;
}

static void start_control_thread(int control_fd, fdpass_runtime *runtime) {
    fdpass_control_arg *arg = (fdpass_control_arg *)malloc(sizeof(*arg));
    if (arg == NULL) {
        close(control_fd);
        return;
    }
    arg->control_fd = control_fd;
    arg->runtime = runtime;

    pthread_t thread;
    if (pthread_create(&thread, NULL, fdpass_control_thread, arg) != 0) {
        free(arg);
        close(control_fd);
        return;
    }
    (void)pthread_detach(thread);
}

static fdpass_options normalize_options(const fdpass_options *options) {
    fdpass_options normalized = {
        .exec_mode = FDPASS_EXEC_PER_CONNECTION,
        .workers = RINHA_DEFAULT_WORKERS,
        .queue_size = RINHA_DEFAULT_FD_QUEUE_SIZE,
    };
    if (options != NULL) {
        normalized = *options;
    }
    if (normalized.workers == 0) {
        normalized.workers = RINHA_DEFAULT_WORKERS;
    }
    if (normalized.queue_size == 0) {
        normalized.queue_size = RINHA_DEFAULT_FD_QUEUE_SIZE;
    }
    return normalized;
}

int fdpass_serve(const char *control_path, const raw_http_app *app, const fdpass_options *options) {
    if (control_path == NULL || control_path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (strlen(control_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (mkdir_parent(control_path) != 0) {
        return -1;
    }

    fdpass_runtime runtime;
    memset(&runtime, 0, sizeof(runtime));
    runtime.app = app;
    runtime.options = normalize_options(options);
    if (runtime.options.exec_mode == FDPASS_EXEC_WORKER_POOL) {
        if (fd_queue_init(&runtime.queue, runtime.options.queue_size) != 0) {
            return -1;
        }
        runtime.queue_ready = true;
        if (start_worker_threads(&runtime) != 0) {
            fd_queue_close(&runtime.queue);
            fd_queue_destroy(&runtime.queue);
            return -1;
        }
    }

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        if (runtime.queue_ready) {
            fd_queue_close(&runtime.queue);
            fd_queue_destroy(&runtime.queue);
        }
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, control_path);

    (void)unlink(control_path);
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(server_fd);
        if (runtime.queue_ready) {
            fd_queue_close(&runtime.queue);
            fd_queue_destroy(&runtime.queue);
        }
        return -1;
    }
    (void)chmod(control_path, 0666);

    if (listen(server_fd, RINHA_LISTEN_BACKLOG) != 0) {
        close(server_fd);
        (void)unlink(control_path);
        if (runtime.queue_ready) {
            fd_queue_close(&runtime.queue);
            fd_queue_destroy(&runtime.queue);
        }
        return -1;
    }

    for (;;) {
        int control_fd = accept(server_fd, NULL, NULL);
        if (control_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(server_fd);
            if (runtime.queue_ready) {
                fd_queue_close(&runtime.queue);
                fd_queue_destroy(&runtime.queue);
            }
            return -1;
        }
        start_control_thread(control_fd, &runtime);
    }
}
