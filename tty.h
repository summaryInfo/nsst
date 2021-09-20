/* Copyright (c) 2019-2021, Evgeny Baskov. All rights reserved */

#ifndef TTY_H_
#define TTY_H_ 1

#define FD_BUF_SIZE 16384

#include "config.h"

#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

struct tty {
    struct tty *next;
    struct tty *prev;

    pid_t child;
    int fd;
    pid_t printer;
    int printerfd;

    uint8_t fd_buf[FD_BUF_SIZE];

    uint8_t *start;
    uint8_t *end;
};

void init_default_termios(void);

int tty_open(struct tty *tty, struct instance_config *cfg);
void tty_break(struct tty *tty);
void tty_set_winsz(struct tty *tty, int16_t width, int16_t height, int16_t wwidth, int16_t wheight);
void tty_print_string(struct tty *tty, const uint8_t *str, ssize_t size);
ssize_t tty_refill(struct tty *tty);
void tty_write(struct tty *tty, const uint8_t *buf, size_t len, bool crlf);
void tty_hang(struct tty *tty);
bool tty_has_printer(struct tty *tty);


#endif
