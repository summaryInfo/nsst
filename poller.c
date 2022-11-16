/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

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
#include "util.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INIT_PFD_NUM 16
#define FREE_SLOT INT_MIN

struct poller_item {
    uint8_t **start;
    uint8_t *end;
};

struct poller {
    struct pollfd *pfds;
    struct poller_item *data;
    ssize_t pfdn;
    ssize_t pfdcap;
};

struct poller poller;

void init_poller(void) {
    poller.pfds = xzalloc(INIT_PFD_NUM * sizeof(struct pollfd));
    poller.data = xzalloc(INIT_PFD_NUM * sizeof(struct poller_item));
    poller.pfdn = 2;
    poller.pfdcap = INIT_PFD_NUM;
    for (ssize_t i = 1; i < INIT_PFD_NUM; i++)
        poller.pfds[i].fd = FREE_SLOT;
}

void free_poller(void) {
    free(poller.pfds);
    free(poller.data);
    memset(&poller, 0, sizeof poller);
}

int poller_alloc_index(int fd, int events) {
    if (poller.pfdn + 1 > poller.pfdcap) {
        struct pollfd *new = xrealloc(poller.pfds, poller.pfdcap*sizeof(*poller.pfds),
                                     (poller.pfdcap + INIT_PFD_NUM)*sizeof(*poller.pfds));
        struct poller_item *newdata = xrealloc(poller.data, poller.pfdcap*sizeof(*poller.data),
                                               (poller.pfdcap + INIT_PFD_NUM)*sizeof(*poller.data));
        for (ssize_t i = 0; i < INIT_PFD_NUM; i++) {
            new[i + poller.pfdcap].fd = FREE_SLOT;
            new[i + poller.pfdcap].events = 0;
        }
        poller.pfdcap += INIT_PFD_NUM;
        poller.pfds = new;
        poller.data = newdata;
    }

    poller.pfdn++;
    ssize_t i = 0;
    while (poller.pfds[i].fd != FREE_SLOT) i++;

    poller.pfds[i].fd = fd;
    poller.pfds[i].events = events;
    poller.data[i].start = NULL;
    poller.data[i].end = NULL;

    return i;
}

int poller_add_reader(int fd, uint8_t **buffer_start, uint8_t *buffer_end) {
    int i =  poller_alloc_index(fd, POLLIN | POLLHUP);
    poller.data[i].start = buffer_start;
    poller.data[i].end = buffer_end;
    return i;
}

int poller_enable(int i, bool toggle) {
    int old = poller.pfds[i].fd;
    poller.pfds[i].fd = (2*toggle-1)*abs(poller.pfds[i].fd);
    return old >= 0;
}

void poller_free_index(int i) {
    poller.pfds[i].fd = FREE_SLOT;
    poller.pfdn--;
}

bool poller_is_enabled(int i) {
    return i < poller.pfdn && poller.pfds[i].fd >= 0;
}

void poller_poll(int64_t timeout) {
#if USE_PPOLL
    if (ppoll(poller.pfds, poller.pfdcap, &(struct timespec){timeout / SEC, timeout % SEC}, NULL) < 0 && errno != EINTR)
#else
    if (poll(poller.pfds, poller.pfdcap, timeout/(SEC/1000)) < 0 && errno != EINTR)
#endif
        warn("Poll error: %s", strerror(errno));

    for (ssize_t i = 0; i < poller.pfdcap; i++) {
        if (poller.pfds[i].fd >= 0 && poller.data[i].start && poller.pfds[i].revents & POLLIN) {
            struct poller_item *it = &poller.data[i];
            ssize_t n_read = 0, inc, inc_total = 0;
            while (it->end > *it->start && (inc = read(poller.pfds[i].fd, *it->start, it->end - *it->start)) > 0)
                   *it->start += inc, n_read++, inc_total += inc;
            if (UNLIKELY(gconfig.trace_misc))
                info("Read from poller (size=%zd n_read=%zd)", inc_total, n_read);
        }
    }
}

int poller_index_events(int i) {
    return poller.pfds[i].revents;
}
