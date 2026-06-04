#ifndef RINHA_FDLB_H
#define RINHA_FDLB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *listen_addr;
    const char *upstreams;
    const char *mode;
    const char *strategy;
    bool lean;
    bool metrics_enabled;
    bool reuseport;
    bool tcp_defer_accept;
    bool tcp_fastopen;
    uint32_t so_busy_poll_us;
    uint32_t listen_backlog;
    uint32_t accept_batch;
    uint32_t connect_retry_ms;
    uint32_t startup_timeout_ms;
} FdlbConfig;

typedef enum {
    FDLB_MODE_FDPASS = 0,
    FDLB_MODE_PROXY = 1
} FdlbMode;

typedef enum {
    FDLB_STRATEGY_ROUND_ROBIN = 0,
    FDLB_STRATEGY_LEAST_ACTIVE = 1,
    FDLB_STRATEGY_POWER_OF_TWO = 2
} FdlbStrategy;

int fdlb_parse_upstreams(char *text, char **paths, size_t max_paths, size_t *count);
uint32_t fdlb_parse_u32_clamped(const char *text, uint32_t fallback, uint32_t minimum, uint32_t maximum);
bool fdlb_parse_mode(const char *text, FdlbMode *mode);
bool fdlb_parse_strategy(const char *text, FdlbStrategy *strategy);
size_t fdlb_round_robin_next(size_t *cursor, size_t count);
size_t fdlb_select_upstream(FdlbStrategy strategy, size_t *cursor, const uint32_t *active, size_t count);
void fdlb_decrement_active(uint32_t *active);
int fdlb_send_one_fd(int control_fd, int fd);
int fdlb_run(const FdlbConfig *config);

#endif
