#ifndef RINHA_FDPASS_H
#define RINHA_FDPASS_H

#include "raw_http.h"

#include <stdint.h>

typedef enum {
    FDPASS_EXEC_PER_CONNECTION = 0,
    FDPASS_EXEC_WORKER_POOL = 1,
    FDPASS_EXEC_EPOLL = 2
} fdpass_exec_mode;

typedef struct {
    fdpass_exec_mode exec_mode;
    uint32_t workers;
    uint32_t queue_size;
} fdpass_options;

int fdpass_serve(const char *control_path, const raw_http_app *app, const fdpass_options *options);
int fdpass_recv_one_fd(int control_fd);
int fdpass_send_one_fd(int control_fd, int fd);

#endif
