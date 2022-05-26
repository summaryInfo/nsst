/* Copyright (c) 2019-2022, Evgeny Baskov. All rights reserved */

#ifndef TTY_H_
#define TTY_H_ 1

#define FD_BUF_SIZE 16384

#include "config.h"

#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

struct watcher {
    struct watcher *next;
    struct watcher *prev;
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

struct tty {
    struct watcher w;

    uint8_t fd_buf[FD_BUF_SIZE];
    uint8_t *start;
    uint8_t *end;
};

inline static bool tty_has_data(struct tty *tty) {
    return tty->start < tty->end;
}

void init_default_termios(void);
void hang_watched_children(void);

int tty_open(struct tty *tty, struct instance_config *cfg);
void tty_break(struct tty *tty);
void tty_set_winsz(struct tty *tty, int16_t width, int16_t height, int16_t wwidth, int16_t wheight);
ssize_t tty_refill(struct tty *tty);
void tty_write(struct tty *tty, const uint8_t *buf, size_t len, bool crlf);
void tty_hang(struct tty *tty);

void printer_intercept(struct printer *pr, const uint8_t **start, const uint8_t *end);
void printer_print_string(struct printer *pr, const uint8_t *str, ssize_t size);
bool printer_is_available(struct printer *pr);
void free_printer(struct printer *pr);
void init_printer(struct printer *pr, struct instance_config *cfg);

#endif
