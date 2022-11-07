/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

#include "feature.h"

#define _DEFAULT_SOURCE
#define _BSD_SOURCE

#include "config.h"
#include "tty.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <locale.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* For opentty() function */
#if   defined(__linux)
#   include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
#   include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#   include <libutil.h>
#endif


#define TTY_MAX_WRITE 256

/* Head of TTYs list with alive child process */
static struct watcher *first_child;

/* Default termios, initialized from main */
static struct termios dtio;

static void handle_chld(int arg) {
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid < 0) {
            warn("Child wait failed");
            break;
        }

        if (WIFEXITED(status) && WEXITSTATUS(status))
            info("Child exited with status: %d", WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            info("Child terminated due to the signal: %d\n", WTERMSIG(status));

        for (struct watcher *w = first_child; w; w = w->next) {
            if (w->child == pid) {
                close(w->fd);
                w->fd = -1;
                break;
            }
        }
    }

    (void)arg;
}

static _Noreturn void exec_shell(char **args, char *sh, char *termname, char *luit) {

    const struct passwd *pw;
    errno = 0;
    if (!(pw = getpwuid(getuid()))) {
        if (errno) die("getpwuid(): %s", strerror(errno));
        else die("I don't know you");
     }

    if (!(sh = getenv("SHELL")))
        sh = pw->pw_shell[0] ? pw->pw_shell : sh;

    assert(sh);

    unsetenv("COLUMNS");
    unsetenv("LINES");
    unsetenv("TERMCAP");

    setenv("LOGNAME", pw->pw_name, 1);
    setenv("USER", pw->pw_name, 1);
    setenv("SHELL", sh, 1);
    setenv("HOME", pw->pw_dir, 1);
    setenv("TERM", termname, 1);

    if (args) sh = args[0];

    char *def[] = {sh, NULL};
    if (!args) args = def;

    signal(SIGCHLD, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGALRM, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);

    /* Disable job control signals by default
     * like login does */
#ifdef SIGTSTP
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
#endif

    /* Reformat argv to run luit
     * If luit cannon be executed, just exit
     * if user would want to run nsst with unsupported
     * encoding without luit, it is possible
     * to set allow_luit to zero */
    if (luit) {
        ssize_t narg = 0;
        for (char **arg = args; *arg; arg++) narg++;
        char **new_args = calloc(narg + 2, sizeof *new_args);
        if (!new_args) _exit(2);
        new_args[0] = luit;
        for (ssize_t i = 1; i <= narg; i++)
            new_args[i] = args[i - 1];
        args = new_args;
        sh = new_args[0];
    }

    execvp(sh, (char *const *)args);

    if (luit && (errno == ENOENT || errno == EACCES))
        fatal("Can't run luit at '%s'", luit);

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

    sigaction(SIGCHLD, &(struct sigaction) {
            .sa_handler = handle_chld, .sa_flags = SA_RESTART}, NULL);
}

static void add_watcher(struct watcher *w) {
    w->next = first_child;
    w->prev = NULL;

    if (first_child)
        first_child->prev = w;
    first_child = w;
}

static void remove_watcher(struct watcher *w) {
    if (w->next) w->next->prev = w->prev;
    if (w->prev) w->prev->next = w->next;
    else if (first_child == w) first_child = w->next;
    w->next = w->prev = NULL;

    if (w->fd >= 0) {
        close(w->fd);
        w->fd = -1;
    }

    if (w->child > 0)
        kill(w->child, SIGHUP);
}

void hang_watched_children(void) {
    while (first_child)
        remove_watcher(first_child);
}

int tty_open(struct tty *tty, struct instance_config *cfg) {
    /* Configure PTY */

    struct termios tio = dtio;

    tio.c_cc[VERASE] = cfg->backspace_is_delete ? '\177' : '\010';

    /* We will run luit if it is allowed and required */
    bool luit = cfg->allow_luit && gconfig.want_luit;

    /* If we can and want to run luit we need to enable UTF-8 */
    cfg->utf8 |= luit;

    /* If IUTF8 is defined, enable it by default,
     * when terminal itself is in UTF-8 mode */
#ifdef IUTF8
    if (cfg->utf8)
        tio.c_iflag |= IUTF8;
#endif

    int slave;
    if (openpty(&tty->w.fd, &slave, NULL, &tio, NULL) < 0) {
        warn("Can't create pseudo terminal");
        tty->w.fd = -1;
        return -1;
    }

    int fld = fcntl(tty->w.fd, F_GETFD);
    if (fld >= 0) fcntl(tty->w.fd, F_SETFD, fld | FD_CLOEXEC);
    int fl = fcntl(tty->w.fd, F_GETFL);
    if (fl >= 0) fcntl(tty->w.fd, F_SETFL, fl | O_NONBLOCK);

    switch ((tty->w.child = fork())) {
    case -1:
        close(slave);
        close(tty->w.fd);
        warn("Can't fork");
        tty->w.fd = -1;
        return -1;
    case 0:
        setsid();
        errno = 0;
        if (ioctl(slave, TIOCSCTTY, NULL) < 0)
            die("Can't make tty controlling");
        if (cfg->cwd && chdir(cfg->cwd) < 0)
            warn("Can't change current directory");
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        close(slave);
        exec_shell(cfg->argv, cfg->shell, cfg->terminfo, luit ? cfg->luit : NULL);
    default:
        /* Reset argv to not use it twice */
        cfg->argv = NULL;
        close(slave);
    }

    tty->start = tty->end = tty->fd_buf;

    if (tty->w.child > 0)
        add_watcher(&tty->w);

    return tty->w.fd;
}

void tty_hang(struct tty *tty) {
    remove_watcher(&tty->w);
}

ssize_t tty_refill(struct tty *tty) {
    if (UNLIKELY(tty->w.fd == -1)) return -1;

    ssize_t inc = 0, sz = tty->end - tty->start, inctotal = 0;

    if (tty->start != tty->fd_buf) {
        /* Always keep last MAX_PROTOCOL_LEN bytes in buffer for URL parsing matching */
        ssize_t tail = MAX(sz, MAX_PROTOCOL_LEN);
        memmove(tty->fd_buf, tty->start, tail);
        tty->start = tty->fd_buf + tail - sz;
        tty->end = tty->fd_buf + tail;
        sz = tail;
    }

    ssize_t space = sizeof tty->fd_buf - sz;
    ssize_t n_read = 0;

    while (space > 0 && (inc = read(tty->w.fd, tty->end, space)) > 0) {
        space -= inc;
        tty->end += inc;
        n_read++;
    }

    if (UNLIKELY(gconfig.trace_misc))
        info("Read TTY (size=%zd n_read=%zd)", inctotal, n_read);

    inctotal = (sizeof tty->fd_buf - sz) - space;

    if (inc < 0 && errno != EAGAIN) {
        warn("Can't read from tty");
        tty_hang(tty);
        return -1;
    }

    return inctotal;
}


inline static void tty_write_raw(struct tty *tty, const uint8_t *buf, ssize_t len) {
    ssize_t res, lim = TTY_MAX_WRITE;
    struct pollfd pfd = {
        .events = POLLIN | POLLOUT,
        .fd = tty->w.fd
    };
    while (len) {
        if (poll(&pfd, 1, -1) < 0 && errno != EINTR) {
            warn("Can't poll tty");
            tty_hang(tty);
            return;
        }
        if (pfd.revents & POLLOUT) {
            if ((res = write(tty->w.fd, buf, MIN(lim, len))) < 0) {
                warn("Can't write to from tty");
                tty_hang(tty);
                return;
            }

            if (res < (ssize_t)len) {
                len -= res;
                buf += res;
            } else break;
        }

        if (pfd.revents & POLLIN) {
            if (tty->end - tty->start == sizeof tty->fd_buf) {
                /* Since the parser cannot be called recursively
                 * called recursively we cannot empty the input buffer
                 * and tty input queue, so we cannot write the data.
                 * This situation is really rare,
                 * so we can just not write the output for now. */
                // FIXME: Add option to defer answerback messages to be written outside the parser.
                warn("TTY buffer is overfilled, current output buffer is discarded");
                return;
            }

            lim = tty_refill(tty);
        }
    }
}

void tty_write(struct tty *tty, const uint8_t *buf, size_t len, bool crlf) {
    if (tty->w.fd == -1) return;

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
    if (tcsendbreak(tty->w.fd, 0))
        warn("Can't send break");
}

void tty_set_winsz(struct tty *tty, int16_t width, int16_t height, int16_t wwidth, int16_t wheight) {
    struct winsize wsz = {
        .ws_col = width,
        .ws_row = height,
        .ws_xpixel = wwidth,
        .ws_ypixel = wheight
    };

    if (ioctl(tty->w.fd, TIOCSWINSZ, &wsz) < 0) {
        warn("Can't change tty size");
        tty_hang(tty);
    }
}

void printer_print_string(struct printer *pr, const uint8_t *str, ssize_t size) {
    ssize_t wri = 0, res;
    do {
        res = write(pr->w.fd, str, size);
        if (res < 0) {
            warn("Printer error");
            if (pr->w.fd != STDOUT_FILENO)
                close(pr->w.fd);
            pr->w.fd = -1;
            break;
        }
        wri += res;
    } while(wri < size);
}

bool printer_is_available(struct printer *pr) {
    return pr->w.fd >= 0;
}

void free_printer(struct printer *pr) {
    remove_watcher(&pr->w);
}

void init_printer(struct printer *pr, struct instance_config *cfg) {
    pr->w.fd = -1;

    /* Printer command is more prioritized that file */
    if (cfg->printer_cmd) {
        int pip[2];
        if (pipe(pip) >= 0 && (pr->w.child = fork()) >= 0) {
            if (!pr->w.child) {
                dup2(pip[0], 0);
                close(pip[1]);
                close(pip[0]);
                pr->w.fd = pip[0];
                execl("/bin/sh", "/bin/sh", "-c", cfg->printer_cmd, NULL);
                warn("Can't run print command: '%s'", cfg->printer_cmd);
                _exit(127);
            } else {
                signal(SIGPIPE, SIG_IGN);
                close(pip[0]);
                pr->w.fd = pip[1];
            }
        } else {
            if (pr->w.child) {
                close(pip[0]);
                close(pip[1]);
            }
            warn("Can't run print command: '%s'", cfg->printer_cmd);
        }
    }

    if (pr->w.fd < 0 && cfg->printer_file) {
        if (cfg->printer_file[0] == '-' && !cfg->printer_file[1])
            pr->w.fd = STDOUT_FILENO;
        else
            pr->w.fd = open(cfg->printer_file, O_WRONLY | O_CREAT, 0660);
    }

    if (pr->w.fd >= 0) {
        int fl = fcntl(pr->w.fd, F_GETFL);
        if (fl >= 0) fcntl(pr->w.fd, F_SETFL, fl | O_CLOEXEC);
    }

    if (pr->w.child > 0)
        add_watcher(&pr->w);
}

void printer_intercept(struct printer *pr, const uint8_t **start, const uint8_t *end) {
    if (!pr->print_controller) return;

    /* If terminal is in controller mode
     * all input is redirected to printer except for
     * NUL, XON, XOFF (that are eaten) and 4 sequences:
     * CSI"5i", ESC"[5i", CSI"[4i", ESC"[4i",
     * first two of which are no-ops,
     * and second two disabled printer controller */

    const uint8_t *blk_start = *start;
    while (*start < end) {
        uint8_t ch = *(*start)++;
        switch (ch) {
        case 0x11: /* XON */
        case 0x13: /* XOFF */
        case 0x00: /* NUL */
            if (blk_start < *start - 1)
                printer_print_string(pr, blk_start, *start - 1 - blk_start);
            blk_start = *start;
            break;
        case 0x9B: /* CSI */
        case 0x1B: /* ESC */
            if (blk_start < *start - 1)
                printer_print_string(pr, blk_start, *start - 1 - blk_start);
            blk_start = *start - 1;
            if (ch == 0x1B) pr->state = pr_esc;
            else pr->state = pr_csi;
            break;
        case '[':
            if (pr->state == pr_esc) pr->state = pr_bracket;
            else pr->state = pr_ground;
            break;
        case '4':
        case '5':
            if (pr->state == pr_bracket || pr->state == pr_csi) {
                pr->state = ch == '4' ? pr_4 : pr_5;
            } else pr->state = pr_ground;
            break;
        case 'i':
            if (pr->state == pr_4) {
                /* Disable printer controller mode */
                pr->print_controller = 0;
                return;
            } else if (pr->state == pr_5) {
                /* Eat sequence */
                blk_start = *start;
            }
            /* fallthrough */
        default:
            pr->state = pr_ground;
        }
    }

    if (blk_start < end && pr->state == pr_ground)
        printer_print_string(pr, blk_start, end - blk_start);
}
