/* Copyright (c) 2024, Evgeniy Baskov. All rights reserved */

#ifndef POLLER_H_
#define POLLER_H_ 1

#include "feature.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/poll.h>
#include <time.h>

struct event;

typedef bool (*poller_timer_cb_t)(void *arg);
typedef void (*poller_fd_cb_t)(void *arg, uint32_t mask);
typedef void (*poller_tick_cb_t)(void *arg);

struct event *poller_add_fd(poller_fd_cb_t cb, void *arg, int fd, uint32_t mask);
struct event *poller_add_timer(poller_timer_cb_t cb, void *arg, int64_t periodns);
void poller_add_tick(poller_tick_cb_t tick, void *arg);
void poller_remove(struct event *evt);
void poller_toggle(struct event *evt, bool enable);
bool poller_is_enabled(struct event *evt);
void poller_stop(void);
void poller_run(void);
void poller_set_autoreset(struct event *evt, struct event **pevt);
void poller_skip_wait(void);

static inline bool poller_unset(struct event **evt) {
    if (!*evt) return false;
    poller_remove(*evt);
    return true;
}

static inline bool poller_set_timer(struct event **evt, poller_timer_cb_t cb, void *arg, int64_t periodns) {
    bool result = poller_unset(evt);
    *evt = poller_add_timer(cb, arg, periodns);
    poller_set_autoreset(*evt, evt);
    return result;
}

void init_poller(void);
void free_poller(void);

#endif
