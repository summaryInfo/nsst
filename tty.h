/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

#ifndef TTY_H_
#define TTY_H_ 1

#define FD_BUF_SIZE 16384

#include "config.h"
#include "list.h"

#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

struct watcher {
    struct list_head link;
    pid_t child;
    int fd;
};

/* Printer controller mode
 * parser state */
enum pr_state {
    pr_ground,
    pr_csi,
    pr_esc,
    pr_bracket,
    pr_5,
    pr_4,
    pr_ignore,
};

struct printer {
    struct watcher w;
    enum pr_state state;
    bool print_controller;
};

struct deferred_write {
    struct list_head link;
    ssize_t offset;
    ssize_t size;
    char data[];
};

struct tty {
    struct watcher w;
    struct list_head deferred;
    struct event *evt;
    ssize_t deferred_count;

    uint8_t fd_buf[FD_BUF_SIZE];
    uint8_t *start;
    uint8_t *end;
};

static inline bool tty_has_data(struct tty *tty) {
    return tty->start < tty->end;
}

void init_default_termios(void);
void hang_watched_children(void);

struct window;

int tty_open(struct tty *tty, struct instance_config *cfg, struct window *win);
void tty_break(struct tty *tty);
void tty_set_winsz(struct tty *tty, int16_t width, int16_t height, int16_t wwidth, int16_t wheight);
ssize_t tty_refill(struct tty *tty);
void tty_write(struct tty *tty, const uint8_t *buf, size_t len, bool crlf);
void tty_hang(struct tty *tty);
void tty_toggle_read(struct tty *tty, bool enable);

void printer_intercept(struct printer *pr, const uint8_t **start, const uint8_t *end);
void printer_print_string(struct printer *pr, const uint8_t *str, ssize_t size);
bool printer_is_available(struct printer *pr);
void free_printer(struct printer *pr);
void init_printer(struct printer *pr, struct instance_config *cfg);
void handle_term_read(void *win_, uint32_t mask);

#endif
