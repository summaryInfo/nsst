/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#include "feature.h"

#define _XOPEN_SOURCE 700

#include "config.h"
#include "tty.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

//For opentty() function
#if   defined(__linux)
#   include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
#   include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#   include <libutil.h>
#endif


#define TTY_MAX_WRITE 256

/* Default termios, initialized from main */
static struct termios dtio;

static void handle_chld(int arg) {
    int status;
    char str[128];
    ssize_t len = 0;

    pid_t pid = waitpid(-1, &status, WNOHANG);
    uint32_t loglevel = iconf(ICONF_LOG_LEVEL);

    if (pid < 0) {
        if (loglevel > 1) len = snprintf(str, sizeof str,
                "[\033[33;1mWARN\033[m] Child wait failed");
    } else if (WIFEXITED(status) && WEXITSTATUS(status)) {
        if (loglevel > 2) len = snprintf(str, sizeof str,
                "[\033[32;1mINFO\033[m] Child exited with status: %d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        if (loglevel > 2) len = snprintf(str, sizeof str,
                "[\033[32;1mINFO\033[m] Child terminated due to the signal: %d\n", WTERMSIG(status));
    }

    if (len) write(STDERR_FILENO, str, len);

    (void)arg;
}

void exec_shell(const char *cmd, const char **args) {

    const struct passwd *pw;
    errno = 0;
    if (!(pw = getpwuid(getuid()))) {
        if (errno) die("getpwuid(): %s", strerror(errno));
        else die("I don't know you");
     }

    const char *sh = cmd;
    if (!(sh = getenv("SHELL")))
        sh = pw->pw_shell[0] ? pw->pw_shell : cmd;

    if (args) cmd = args[0];
    else cmd = sh;

    const char *def[] = {cmd, NULL};
    if (!args) args = def;

    unsetenv("COLUMNS");
    unsetenv("LINES");
    unsetenv("TERMCAP");

    setenv("LOGNAME", pw->pw_name, 1);
    setenv("USER", pw->pw_name, 1);
    setenv("SHELL", sh, 1);
    setenv("HOME", pw->pw_dir, 1);
    setenv("TERM", sconf(SCONF_TERM_NAME), 1);

    signal(SIGCHLD, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGALRM, SIG_DFL);

    // Disable job control signals by default
    // like login does
#ifdef SIGTSTP
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
#endif

    execvp(cmd, (char *const *)args);
    _exit(1);
}

void init_default_termios(void) {
    /* Use stdin as base configuration */
    if (tcgetattr(STDIN_FILENO, &dtio) < 0)
        memset(&dtio, 0, sizeof(dtio));

    /* Setup keys */

    /* Disable everything */
    for (size_t i = 0; i < NCCS; i++)
#ifdef _POSIX_VDISABLE
        dtio.c_cc[i] = _POSIX_VDISABLE;
#else
        dtio.c_cc[i] = 255;
#endif

    /* Then enable all known */

#ifdef CINTR
    dtio.c_cc[VINTR] = CINTR;
#else
    dtio.c_cc[VINTR] = '\003';
#endif

#ifdef CQUIT
    dtio.c_cc[VQUIT] = CQUIT;
#else
    dtio.c_cc[VQUIT] = '\034';
#endif

#ifdef CERASE
    dtio.c_cc[VERASE] = CERASE;
#elif defined(__linux__)
    dtio.c_cc[VERASE] = '\177';
#else
    dtio.c_cc[VERASE] = '\010';
#endif

#ifdef CKILL
    dtio.c_cc[VKILL] = CKILL;
#else
    dtio.c_cc[VKILL] = '\025';
#endif

#ifdef CEOF
    dtio.c_cc[VEOF] = CEOF;
#else
    dtio.c_cc[VEOF] = '\004';
#endif

#ifdef CSTART
    dtio.c_cc[VSTART] = CSTART;
#else
    dtio.c_cc[VSTART] = '\021';
#endif

#ifdef CSTOP
    dtio.c_cc[VSTOP] = CSTOP;
#else
    dtio.c_cc[VSTOP] = '\023';
#endif

#ifdef CSUSP
    dtio.c_cc[VSUSP] = CSUSP;
#else
    dtio.c_cc[VSUSP] = '\032';
#endif

#ifdef VERASE2
    dtio.c_cc[VERASE2] = CERASE2;
#endif

#ifdef VDSUSP
#   ifdef CDSUSP
    dtio.c_cc[VDSUSP] = CDSUSP;
#   else
    dtio.c_cc[VDSUSP] = '\031';
#   endif
#endif

#ifdef VREPRINT
#   ifdef CRPRNT
    dtio.c_cc[VREPRINT] = CRPRNT;
#   else
    dtio.c_cc[VREPRINT] = '\022';
#   endif
#endif

#ifdef VDISCRD
#   ifdef CFLUSH
    dtio.c_cc[VDISCRD] = CFLUSH;
#   else
    dtio.c_cc[VDISCRD] = '\017';
#   endif
#elif defined(VDISCARD)
#   ifdef CFLUSH
    dtio.c_cc[VDISCARD] = CFLUSH;
#   else
    dtio.c_cc[VDISCARD] = '\017';
#   endif
#endif

#ifdef VWERSE
    dtio.c_cc[VWERSE] = CWERASE;
#elif defined(VWERASE)
    dtio.c_cc[VWERASE] = '\027';
#endif

#ifdef VLNEXT
#   ifdef CLNEXT
    dtio.c_cc[VLNEXT] = CLNEXT;
#   else
    dtio.c_cc[VLNEXT] = '\026';
#   endif
#endif

#ifdef VSTATUS
    dtio.c_cc[VSTATUS] = CSTATUS;
#endif

#if VMIN != VEOF
    dtio.c_cc[VMIN] = 1;
#endif

#if VTIME != VEOL
    dtio.c_cc[VTIME] = 0;
#endif

    /* Input modes */
#ifdef IMAXBEL
    dtio.c_iflag = BRKINT | IGNPAR | ICRNL | IMAXBEL | IXON;
#else
    dtio.c_iflag = BRKINT | IGNPAR | ICRNL | IXON;
#endif

    /* Output modes */
#ifdef ONLCR
    dtio.c_oflag = OPOST | ONLCR;
#else
    dtio.c_oflag = OPOST;
#endif

    /* Control modes */
    dtio.c_cflag = CS8 | CREAD;

    /* Local modes */
#if defined (ECHOCTL) && defined (ECHOKE)
    dtio.c_lflag = ISIG | ICANON | IEXTEN | ECHO | ECHOCTL | ECHOKE | ECHOE | ECHOK;
#else
    dtio.c_lflag = ISIG | ICANON | IEXTEN | ECHO | ECHOE | ECHOK;
#endif

    /* Find and set max I/O baud rate */
    int rate =
#if defined(B230400)
            B230400;
#elif defined(B115200)
            B115200;
#elif defined(B57600)
            B57600;
#elif defined(B38400)
            B38400;
#elif defined(B19200)
            B19200;
#else
            B9600;
#endif
    cfsetispeed(&dtio, rate);
    cfsetospeed(&dtio, rate);

}

int tty_open(struct tty *tty, const char *cmd, const char **args) {
    /* Configure PTY */

    struct termios tio = dtio;

    tio.c_cc[VERASE] = iconf(ICONF_BACKSPACE_IS_DELETE) ? '\177' : '\010';

    /* If IUTF8 is defined, enable it by default,
     * when terminal itself is in UTF-8 mode */
#ifdef IUTF8
    if (iconf(ICONF_UTF8))
        tio.c_iflag |= IUTF8;
#endif

    int slave;
    if (openpty(&tty->fd, &slave, NULL, &tio, NULL) < 0) {
        warn("Can't create pseudo terminal");
        tty->fd = -1;
        return -1;
    }

    int fl = fcntl(tty->fd, F_GETFL);
    if (fl >= 0) fcntl(tty->fd, F_SETFL, fl | O_NONBLOCK | O_CLOEXEC);

    switch ((tty->child = fork())) {
    case -1:
        close(slave);
        close(tty->fd);
        warn("Can't fork");
        tty->fd = -1;
        return -1;
    case 0:
        setsid();
        errno = 0;
        if (ioctl(slave, TIOCSCTTY, NULL) < 0)
            die("Can't make tty controlling");
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        close(slave);
        exec_shell(cmd, args);
        break;
    default:
        close(slave);
        sigaction(SIGCHLD, &(struct sigaction){
                .sa_handler = handle_chld, .sa_flags = SA_RESTART}, NULL);
    }

    // Open printer file/pipe
    if (tty->fd >= 0) {
        const char *print_cmd = sconf(SCONF_PRINT_CMD);
        const char *printer_path = sconf(SCONF_PRINTER);
        tty->printerfd = -1;


        if (print_cmd) {
            int pip[2];
            if (pipe(pip) < 0)  goto n_printer;

            switch (fork()) {
            case -1:
                goto n_printer;
            default:
                dup2(pip[0], 0);
                close(pip[1]);
                close(pip[0]);
                tty->printerfd = pip[0];
                execl("/bin/sh", "/bin/sh", "-c", print_cmd, NULL);
                warn("Can't run print command: '%s'", print_cmd);
                return 127;
            case 0:
                signal(SIGPIPE, SIG_IGN);
                close(pip[0]);
                tty->printerfd = pip[1];
            }
            if (0) {
n_printer:
                warn("Can't run print command: '%s'", print_cmd);
            }
        }

        if (tty->printerfd < 0 && printer_path) {
            if (printer_path[0] == '-' && !printer_path[1])
                tty->printerfd = STDOUT_FILENO;
            else
                tty->printerfd = open(printer_path, O_WRONLY | O_CREAT, 0660);
        }

        if (tty->printerfd >= 0) {
            int fl = fcntl(tty->printerfd, F_GETFL);
            if (fl >= 0) fcntl(tty->printerfd, F_SETFL, fl | O_CLOEXEC);
        }
    }

    return tty->fd;
}

void tty_hang(struct tty *tty) {
    if (tty->fd >= 0) {
        close(tty->fd);
        if (tty->printerfd != STDOUT_FILENO)
            close(tty->printerfd);
        tty->fd = -1;
    }
    kill(tty->child, SIGHUP);
}

ssize_t tty_refill(struct tty *tty) {
    if (tty->fd == -1) return -1;

    ssize_t inc, sz = tty->end - tty->start;

    if (tty->start != tty->fd_buf) {
        memmove(tty->fd_buf, tty->start, sz);
        tty->end = tty->fd_buf + sz;
        tty->start = tty->fd_buf;
    }

    if ((inc = read(tty->fd, tty->end, sizeof(tty->fd_buf) - sz)) < 0) {
        if (errno != EAGAIN) {
            warn("Can't read from tty");
            tty_hang(tty);
            return -1;
        }
        inc = 0;
    }

    tty->end += inc;
    return inc;
}


inline static void tty_write_raw(struct tty *tty, const uint8_t *buf, ssize_t len) {
    ssize_t res, lim = TTY_MAX_WRITE;
    struct pollfd pfd = {
        .events = POLLIN | POLLOUT,
        .fd = tty->fd
    };
    while (len) {
        if (poll(&pfd, 1, -1) < 0 && errno != EINTR) {
            warn("Can't poll tty");
            tty_hang(tty);
            return;
        }
        if (pfd.revents & POLLOUT) {
            if ((res = write(tty->fd, buf, MIN(lim, len))) < 0) {
                warn("Can't write to from tty");
                tty_hang(tty);
                return;
            }

            if (res < (ssize_t)len) {
                if (len < lim)
                    lim = tty_refill(tty);
                len -= res;
                buf += res;
            } else break;
        }
        if (pfd.revents & POLLIN)
            lim = tty_refill(tty);
    }
}

void tty_write(struct tty *tty, const uint8_t *buf, size_t len, bool crlf) {
    if (tty->fd == -1) return;

    const uint8_t *next;

    if (!crlf) tty_write_raw(tty, buf, len);
    else while (len) {
        if (*buf == '\r') {
            next = buf + 1;
            tty_write_raw(tty, (const uint8_t *)"\r\n", 2);
        } else {
            next = memchr(buf , '\r', len);
            if (!next) next = buf + len;
            tty_write_raw(tty, buf, next - buf);
        }
        len -= next - buf;
        buf = next;
    }
}

void tty_break(struct tty *tty) {
    if (tcsendbreak(tty->fd, 0))
        warn("Can't send break");
}

void tty_set_winsz(struct tty *tty, int16_t width, int16_t height, int16_t wwidth, int16_t wheight) {
    struct winsize wsz = {
        .ws_col = width,
        .ws_row = height,
        .ws_xpixel = wwidth,
        .ws_ypixel = wheight
    };

    if (ioctl(tty->fd, TIOCSWINSZ, &wsz) < 0) {
        warn("Can't change tty size");
        tty_hang(tty);
    }
}

void tty_print_string(struct tty *tty, const uint8_t *str, ssize_t size) {
    ssize_t wri = 0, res;
    do {
        res = write(tty->printerfd, str, size);
        if (res < 0) {
            warn("Printer error");
            if (tty->printerfd != STDOUT_FILENO)
                close(tty->printerfd);
            tty->printerfd = -1;
            break;
        }
        wri += res;
    } while(wri < size);
}

bool tty_has_printer(struct tty *tty) {
    return tty->printerfd >= 0;
}
