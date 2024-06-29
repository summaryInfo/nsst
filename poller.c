/* Copyright (c) 2019-2024, Evgeniy Baskov. All rights reserved */

#include "feature.h"

#if USE_PPOLL
#   define _GNU_SOURCE
#ifndef __linux__
/* GLIBC insists on _DEFAULT_SOURCE instead of _BSD_SOURCE,
 * so define it if only we are not on linux */
#   define _BSD_SOURCE
#endif
#endif

#include "config.h"
#include "poller.h"
#include "util.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INIT_PFD_NUM 16
#define INIT_HEAP_NUM 64
#define EVENT_FREE_MAX 128
#define FREE_SLOT INT_MIN

struct event {
    enum event_state {
        evt_state_free,
        evt_state_timer,
        evt_state_fd,
    } state;
    enum event_flags {
        evt_flag_disabled = (1 << 0),
    } flags;
    struct event **pptr;
    union {
        struct {
            struct event *next;
        } free;
        struct {
            int index;
            poller_timer_cb_t cb;
            void *arg;
            int64_t period;
            struct timespec current;
        } timer;
        struct {
            int index;
            poller_fd_cb_t cb;
            void *arg;
        } fd;
    };
};

struct poller {
    struct pollfd *pollfd_array;
    struct event **pollfd_array_events;
    ssize_t pollfd_array_caps;
    ssize_t pollfd_array_size;
    ssize_t pollfd_first_free;

    struct event **timer_heap;
    ssize_t timer_heap_caps;
    ssize_t timer_heap_size;

    struct event *event_free_list;
    ssize_t event_free_count;
    ssize_t event_free_max;

    bool should_stop;
    poller_tick_cb_t tick_cb;
    void *tick_arg;
};

struct poller poller;

static void empty_tick(void *arg, const struct timespec *now) {
    (void)arg, (void)now;
}

void init_poller(void) {
    /* Initialize fd event array. For now it just uses poll, later we can support to
     * epoll or event io_uring where available. */
    poller.pollfd_array = xzalloc(INIT_PFD_NUM * sizeof(*poller.pollfd_array));
    poller.pollfd_array_events = xzalloc(INIT_PFD_NUM * sizeof(*poller.pollfd_array_events));
    poller.pollfd_array_caps = INIT_PFD_NUM;
    poller.pollfd_array_size = 0;
    poller.pollfd_first_free = 0;
    poller.pollfd_array[0].fd = FREE_SLOT;
    for (ssize_t i = 1; i < INIT_PFD_NUM; i++) {
        poller.pollfd_array[i].fd = FREE_SLOT;
        /* This field is used as next index in a free list for faster allocation,
         * this is valid, since events field is ignored when fd < 0. */
        poller.pollfd_array[i-1].events = i;
    }
    poller.pollfd_array[INIT_PFD_NUM-1].events = -1;

    /* Initialize timer heap. It is maintained in userspace to allow
     * high frequency timer changes. */

    poller.timer_heap = xzalloc(INIT_HEAP_NUM * sizeof(*poller.timer_heap));
    poller.timer_heap_caps = INIT_HEAP_NUM;
    poller.timer_heap_size = 0;

    /* Initalize cached free 'struct event' list.
     * It allows reducing malloc calls by caching free events */

    poller.event_free_max = EVENT_FREE_MAX;
    poller.tick_cb = empty_tick;
}

void poller_add_tick(poller_tick_cb_t tick, void *arg) {
    poller.tick_cb = tick ? tick : empty_tick;
    poller.tick_arg = arg;
}

void free_poller(void) {
    assert(!poller.pollfd_array_size);
    assert(!poller.timer_heap_size);

    free(poller.pollfd_array);
    free(poller.pollfd_array_events);
    free(poller.timer_heap);

    while (poller.event_free_list) {
        struct event *evt = poller.event_free_list;
        poller.event_free_list = evt->free.next;
        free(evt);
    }

    memset(&poller, 0, sizeof poller);
}

void poller_stop(void) {
    poller.should_stop = true;
}

static struct event *alloc_event(void) {
    struct event *evt =  poller.event_free_list;
    if (!evt) {
        evt = xzalloc(sizeof (struct event));
        if (gconfig.trace_misc)
            info("Event[%p] alloc", (void *)evt);
        return evt;
    }

    if (gconfig.trace_misc)
        info("Event[%p] alloc cached", (void *)evt);

    assert(poller.event_free_count);
    assert(evt->state == evt_state_free);

    poller.event_free_list = evt->free.next;
    poller.event_free_count--;
    return evt;
}

static void free_event(struct event *evt) {
    if (gconfig.trace_misc)
        info("Event[%p] free", (void *)evt);
    if (evt->pptr)
        *evt->pptr = NULL;
    if (poller.event_free_count + 1 > poller.event_free_max) {
        free(evt);
    } else {
        evt->state = evt_state_free;
        evt->free.next = poller.event_free_list;
        poller.event_free_list = evt;
        poller.event_free_count++;
    }
}

static void grow_pollfd_array(void) {
    ssize_t old_size2 = poller.pollfd_array_caps*sizeof(*poller.pollfd_array_events);
    ssize_t new_size2 = (poller.pollfd_array_caps + INIT_PFD_NUM)*sizeof(*poller.pollfd_array_events);
    poller.pollfd_array_events = xrezalloc(poller.pollfd_array_events, old_size2, new_size2);

    ssize_t old_size = poller.pollfd_array_caps*sizeof(*poller.pollfd_array);
    ssize_t new_size = (poller.pollfd_array_caps + INIT_PFD_NUM)*sizeof(*poller.pollfd_array);
    struct pollfd *new = xrealloc(poller.pollfd_array, old_size, new_size);

    for (ssize_t i = 0; i < INIT_PFD_NUM; i++) {
        new[i + poller.pollfd_array_caps] = (struct pollfd) {
            .fd = FREE_SLOT,
            .events = poller.pollfd_first_free
        };
        poller.pollfd_first_free = i;
    }

    poller.pollfd_array_caps += INIT_PFD_NUM;
    poller.pollfd_array = new;
}

void poller_set_autoreset(struct event *evt, struct event **pevt) {
    evt->pptr = pevt;
}

struct event *poller_add_fd(poller_fd_cb_t cb, void *arg, int fd, uint32_t mask) {
    if (poller.pollfd_array_size + 1 > poller.pollfd_array_caps)
        grow_pollfd_array();

    int index = poller.pollfd_first_free;
    assert(poller.pollfd_first_free != -1);

    struct pollfd *pfd = &poller.pollfd_array[index];
    poller.pollfd_first_free = pfd->events;

    pfd->fd = fd;
    pfd->events = mask;
    pfd->revents = 0;

    struct event *evt = alloc_event();
    poller.pollfd_array_events[index] = evt;
    *evt = (struct event) {
        .state = evt_state_fd,
        .flags = 0,
        .fd = {
            .cb = cb,
            .arg = arg,
            .index = index,
        }
    };

    return evt;
}

static inline bool heap_leq(int a, int b) {
    return ts_leq(&poller.timer_heap[a]->timer.current,
                  &poller.timer_heap[b]->timer.current);
}

static int heap_sift_down(int index) {
    for (int len = poller.timer_heap_size, next; (next = 2*index) < len; ) {
        next += next + 1 < len && !heap_leq(next, next + 1);
        if (heap_leq(index, next)) break;

        poller.timer_heap[next]->timer.index = index;
        SWAP(poller.timer_heap[next], poller.timer_heap[index]);
        index = next;
    }
    return index;
}

static int heap_sift_up(int index) {
    while (index) {
        int parent = index/2;
        if (heap_leq(parent, index)) break;

        poller.timer_heap[parent]->timer.index = index;
        SWAP(poller.timer_heap[parent], poller.timer_heap[index]);
        index = parent;
    }
    return index;
}

static void heap_remove(struct event *evt) {
    int index = evt->timer.index;
    if (index == -1) return;

    assert(poller.timer_heap_size);
    int last = --poller.timer_heap_size;

    SWAP(poller.timer_heap[last], poller.timer_heap[index]);
    heap_sift_up(index);
    index = heap_sift_down(index);
    poller.timer_heap[index]->timer.index = index;

    evt->timer.index = -1;
}

static void heap_insert_(struct event *evt, struct timespec *now) {
    evt->timer.current = ts_add(now, evt->timer.period);

    int index = poller.timer_heap_size++;
    if (poller.timer_heap_size > poller.timer_heap_caps)
        adjust_buffer((void **)&poller.timer_heap, (size_t *)&poller.timer_heap_caps,
                      poller.timer_heap_size, sizeof *poller.timer_heap);

    poller.timer_heap[index] = evt;

    index = heap_sift_up(index);
    poller.timer_heap[index]->timer.index = index;
}

static inline void heap_insert(struct event *evt) {
    struct timespec now;
    clock_gettime(CLOCK_TYPE, &now);
    heap_insert_(evt, &now);
}

struct event *poller_add_timer(poller_timer_cb_t cb, void *arg, int64_t periodns) {
    struct event *evt = alloc_event();
    struct timespec now;
    clock_gettime(CLOCK_TYPE, &now);

    *evt = (struct event) {
        .state = evt_state_timer,
        .timer = {
            .cb = cb,
            .arg = arg,
            .period = periodns,
        }
    };

    heap_insert(evt);

    return evt;
}

static void remove_timer(struct event *evt) {
    heap_remove(evt);
    free_event(evt);
}

static void remove_fd(struct event *evt) {
    struct pollfd *pfd = &poller.pollfd_array[evt->fd.index];
    poller.pollfd_array_events[evt->fd.index] = NULL;
    pfd->fd = FREE_SLOT;
    pfd->events = poller.pollfd_first_free;
    poller.pollfd_first_free = evt->fd.index;
    free_event(evt);
}

void poller_remove(struct event *evt) {
    switch (evt->state) {
    case evt_state_fd:
        remove_fd(evt);
        break;
    case evt_state_timer:
        remove_timer(evt);
        break;
    case evt_state_free:
        die("Event use after free");
    default:
        __builtin_unreachable();
    }
}

bool poller_is_enabled(struct event *evt) {
    return !(evt->flags & evt_flag_disabled);
}

static void toggle_timer(struct event *evt, bool enable) {
    if (poller_is_enabled(evt) == enable) return;

    if (enable) heap_insert(evt);
    else heap_remove(evt);
}

static void toggle_fd(struct event *evt, bool enable) {
    if (poller_is_enabled(evt) == enable) return;

    evt->flags ^= evt_flag_disabled;
    poller.pollfd_array[evt->fd.index].fd *= -1;
}

void poller_toggle(struct event *evt, bool enable) {
    switch (evt->state) {
    case evt_state_fd:
        toggle_fd(evt, enable);
        break;
    case evt_state_timer:
        toggle_timer(evt, enable);
        break;
    case evt_state_free:
        die("Event use after free");
        assert(0);
    default:
        __builtin_unreachable();
    }
}

void poller_run(void) {
    while (!poller.should_stop) {
        struct timespec now;
        clock_gettime(CLOCK_TYPE, &now);

#if USE_PPOLL
        struct timespec top, *timeout = NULL;
        if (poller.timer_heap_size)
            timeout = &top, top = ts_sub_sat(&poller.timer_heap[0]->timer.current, &now);
        if (ppoll(poller.pollfd_array, poller.pollfd_array_caps, timeout, NULL) < 0 && errno != EINTR)
            warn("Poll error: %s", strerror(errno));
#else
        int timeout = -1;
        if (poller.timer_heap_size)
            timeout = MAX(0, ts_diff(&now, &poller.timer_heap[0]->timer.current)/(SEC/1000));
        if (poll(poller.pollfd_array, poller.pollfd_array_caps, timeout) < 0 && errno != EINTR)
            warn("Poll error: %s", strerror(errno));
#endif
        for (ssize_t i = 0; i < poller.pollfd_array_caps; i++) {
            if (poller.pollfd_array[i].fd >= 0 && poller.pollfd_array[i].revents) {
                struct event *evt = poller.pollfd_array_events[i];
                if (gconfig.trace_misc) {
                    info("File[%p, %d] %p(%p, %x)", (void *)evt, poller.pollfd_array[evt->fd.index].fd,
                         (void *)(uintptr_t)evt->fd.cb, evt->fd.arg, poller.pollfd_array[i].revents);
                }
                evt->fd.cb(evt->fd.arg, poller.pollfd_array[i].revents);
                poller.pollfd_array[i].revents = 0;
            }
        }

        clock_gettime(CLOCK_TYPE, &now);
        struct timespec now2 = ts_add(&now, 10000);

        while (poller.timer_heap_size) {
            struct event *evt = poller.timer_heap[0];
            if (ts_leq(&now2, &evt->timer.current)) break;

            if (gconfig.trace_misc) {
                info("Timer[%p, %f] %p(%p)", (void *)evt, now.tv_sec + now.tv_nsec*1e-9,
                     (void *)(uintptr_t)evt->timer.cb, evt->timer.arg);
            }

            assert(evt->state == evt_state_timer);
            heap_remove(evt);

            if (evt->timer.cb(evt->timer.arg, &now))
                heap_insert_(evt, &now);
            else
                free_event(evt);
        }

        poller.tick_cb(poller.tick_arg, &now);
        if (gconfig.trace_misc)
            info("Tick");
    }
}
