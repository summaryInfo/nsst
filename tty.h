#ifndef TTY_H_
#define TTY_H_ 1

#define FD_BUF_SIZE 4096

#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

struct tty {
    pid_t child;
    int fd;
    int printerfd;

    uint8_t fd_buf[FD_BUF_SIZE];

    uint8_t *start;
    uint8_t *end;
};

void init_default_termios(void);

void exec_shell(const char *cmd, const char **args);
int tty_open(struct tty *tty, const char *cmd, const char **args);
void tty_break(struct tty *tty);
void tty_set_winsz(struct tty *tty, int16_t width, int16_t height, int16_t wwidth, int16_t wheight);
void tty_print_string(struct tty *tty, const uint8_t *str, ssize_t size);
ssize_t tty_refill(struct tty *tty);
void tty_write(struct tty *tty, const uint8_t *buf, size_t len, bool crlf);
void tty_hang(struct tty *tty);


#endif
