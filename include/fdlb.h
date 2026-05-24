#ifndef RINHA_FDLB_H
#define RINHA_FDLB_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *listen_addr;
    const char *upstreams;
    uint32_t connect_retry_ms;
    uint32_t startup_timeout_ms;
} FdlbConfig;

int fdlb_parse_upstreams(char *text, char **paths, size_t max_paths, size_t *count);
size_t fdlb_round_robin_next(size_t *cursor, size_t count);
int fdlb_send_one_fd(int control_fd, int fd);
int fdlb_run(const FdlbConfig *config);

#endif
