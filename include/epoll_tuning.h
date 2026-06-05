#ifndef RINHA_EPOLL_TUNING_H
#define RINHA_EPOLL_TUNING_H

#include "config.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <time.h>

typedef struct {
    uint64_t deadline_ns;
    unsigned int remaining;
    bool busy;
    bool last_wait_busy;
} RinhaEpollPollState;

static inline uint64_t rinha_epoll_now_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0u;
    }
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static inline bool rinha_epoll_busy_enabled(const RinhaEpollTuning *tuning) {
    return tuning != NULL &&
        tuning->busy_poll_us != 0u &&
        tuning->busy_poll_budget != 0u;
}

static inline void rinha_epoll_start_busy_poll(RinhaEpollPollState *state,
                                               const RinhaEpollTuning *tuning) {
    if (state == NULL || !rinha_epoll_busy_enabled(tuning)) {
        return;
    }
    state->busy = true;
    state->remaining = tuning->busy_poll_budget;
    state->deadline_ns =
        rinha_epoll_now_ns() + (uint64_t)tuning->busy_poll_us * 1000ull;
}

static inline void rinha_epoll_poll_state_init(RinhaEpollPollState *state,
                                               const RinhaEpollTuning *tuning) {
    if (state == NULL) {
        return;
    }
    state->deadline_ns = 0u;
    state->remaining = 0u;
    state->busy = false;
    state->last_wait_busy = false;
    if (tuning != NULL && tuning->prefer_busy_poll) {
        rinha_epoll_start_busy_poll(state, tuning);
    }
}

static inline int rinha_epoll_wait_tuned(int epoll_fd,
                                         struct epoll_event *events,
                                         int max_events,
                                         const RinhaEpollTuning *tuning,
                                         RinhaEpollPollState *state) {
    if (state != NULL && state->busy) {
        uint64_t now = rinha_epoll_now_ns();
        if (state->remaining != 0u && now < state->deadline_ns) {
            state->remaining--;
            state->last_wait_busy = true;
            return epoll_wait(epoll_fd, events, max_events, 0);
        }
        state->busy = false;
        state->remaining = 0u;
    }

    if (state != NULL) {
        state->last_wait_busy = false;
    }
    if (tuning == NULL || tuning->idle_us == 0u) {
        return epoll_wait(epoll_fd, events, max_events, -1);
    }

    struct timespec timeout = {
        .tv_sec = 0,
        .tv_nsec = (long)tuning->idle_us * 1000L,
    };
    int n = epoll_pwait2(epoll_fd, events, max_events, &timeout, NULL);
    if (n < 0 && errno == ENOSYS) {
        int timeout_ms = (int)((tuning->idle_us + 999u) / 1000u);
        return epoll_wait(epoll_fd, events, max_events, timeout_ms);
    }
    return n;
}

static inline void rinha_epoll_after_wait(RinhaEpollPollState *state,
                                          const RinhaEpollTuning *tuning,
                                          int event_count) {
    if (state == NULL || tuning == NULL || event_count < 0) {
        return;
    }
    if (event_count > 0) {
        if (!state->last_wait_busy) {
            rinha_epoll_start_busy_poll(state, tuning);
        }
        return;
    }

    if (state->last_wait_busy) {
        state->busy = false;
        state->remaining = 0u;
    } else if (tuning->prefer_busy_poll) {
        rinha_epoll_start_busy_poll(state, tuning);
    }
}

#endif
