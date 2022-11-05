/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

#include "feature.h"

#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_OPTION_DESC 1024
#define MAX_WAIT_LOOP 8
#define STARTUP_DELAY 10000000LL
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#define WARN_PREFIX "[\033[33;1mWARN\033[m] "

static char buffer[MAX(MAX_OPTION_DESC, PATH_MAX) + 1];
static const char *config_path;
static const char *socket_path = "/tmp/nsst-sock0";
static const char *cwd;
static bool need_daemon;
static bool need_exit;

inline static void send_char(int fd, char c) {
    while (send(fd, (char[1]){c}, 1, 0) < 0 && errno == EAGAIN);
}

inline static void recv_response(int fd) {
    ssize_t res = 0;
    while ((res = recv(fd, buffer, sizeof buffer - 1, 0)) > 0) {
        buffer[res] = '\0';
        fputs(buffer, stdout);
    }
}

static _Noreturn void usage(int fd, const char *argv0, int code) {
    send_char(fd, '\025' /* NAK */);

    fputs(argv0, stdout);
    recv_response(fd);

    exit(code);
}

static _Noreturn void version(int fd) {
    send_char(fd, '\005' /* ENQ */);

    recv_response(fd);

    exit(0);
}

static void send_opt(int fd, const char *opt, const char *value) {
    // This API lacks const's but doesn't change values...
    struct iovec dvec[4] = {
        { .iov_base = (char *)"\035" /* GS */, .iov_len = 1 },
        { .iov_base = (char *)opt, .iov_len = strlen(opt) },
        { .iov_base = (char *)"=", .iov_len = 1 },
        { .iov_base = (char *)value, .iov_len = strlen(value) },
    };

    struct msghdr hdr = {
        .msg_iov = dvec,
        .msg_iovlen = sizeof dvec / sizeof *dvec
    };

    while (sendmsg(fd, &hdr, 0) < 0 && errno == EAGAIN);
}

static void send_short_opt(int fd, char opt, const char *value) {
    struct iovec dvec[4] = {
        { .iov_base = (char *)"\034" /* FS */, .iov_len = 1 },
        { .iov_base = &opt, .iov_len = 1 },
        { .iov_base = (char *)"=", .iov_len = 1 },
        { .iov_base = (char *)value, .iov_len = strlen(value) },
    };

    struct msghdr hdr = {
        .msg_iov = dvec,
        .msg_iovlen = sizeof dvec / sizeof *dvec
    };

    while (sendmsg(fd, &hdr, 0) < 0 && errno == EAGAIN);
}

static void send_arg(int fd, char *arg) {
    struct iovec dvec[4] = {
        { .iov_base = (char *)"\036" /* RS */, .iov_len = 1 },
        { .iov_base = arg, .iov_len = strlen(arg) },
    };

    struct msghdr hdr = {
        .msg_iov = dvec,
        .msg_iovlen = sizeof dvec / sizeof *dvec
    };

    while (sendmsg(fd, &hdr, 0) < 0 && errno == EAGAIN);
}

static void send_header(int fd, const char *cpath) {
    struct iovec dvec[4] = {
        { .iov_base = (char *)"\001" /* SOH */, .iov_len = 1 },
        { .iov_base = (char *)cpath, .iov_len = cpath ? strlen(cpath) : 0 },
    };

    struct msghdr hdr = {
        .msg_iov = dvec,
        .msg_iovlen = 1 + !!cpath,
    };

    while (sendmsg(fd, &hdr, 0) < 0 && errno == EAGAIN);
}

static bool is_boolean_option(const char *opt) {
    // TODO Use hash table?
    const char *bool_opts[] = {
        "autorepeat", "allow-uris", "allow-alternate", "allow-blinking",
        "allow-modify-edit-keypad", "allow-modify-function", "allow-modify-keypad",
        "allow-modify-misc", "alternate-scroll", "appcursor", "appkey",
        "autowrap", "backspace-is-del", "blend-all-background", "blend-foreground",
        "daemon", "delete-is-del", "erase-scrollback", "extended-cir",
        "fixed", "force-nrcs", "force-scalable", "fork", "has-meta",
        "keep-clipboard", "keep-selection", "lock-keyboard", "luit",
        "meta-sends-escape", "nrcs", "numlock",
        "override-boxdrawing", "unique-uris", "print-attributes", "raise-on-bell",
        "reverse-video", "scroll-on-input", "scroll-on-output", "select-to-clipboard",
        "smooth-scroll", "special-blink", "special-bold", "special-italic",
        "special-reverse", "special-underlined", "substitute-fonts", "trace-characters",
        "trace-controls", "trace-events", "trace-fonts", "trace-input", "trace-misc",
        "urgent-on-bell", "use-utf8", "visual-bell", "window-ops",
    };
    for (size_t i = 0; i < sizeof bool_opts/sizeof *bool_opts; i++)
        if (!strcmp(bool_opts[i], opt)) return true;
    return false;
}

static bool is_boolean_short_option(char opt) {
    return opt == 'q' || opt == 'd' || opt == 'h' || opt == 'v';
}

inline static bool is_client_only_option(const char *opt) {
    if (!strcmp(opt, "config")) return true;
    if (!strcmp(opt, "socket")) return true;
    if (!strcmp(opt, "cwd")) return true;
    if (!strcmp(opt, "exit")) return true;
    return false;
}

inline static bool is_client_only_short_option(char opt) {
    return opt == 'q' || opt == 's' || opt == 'C' || opt == 'd';
}

bool is_true(const char *value) {
    if (!strcasecmp(value, "true")) return 1;
    if (!strcasecmp(value, "yes")) return 1;
    if (!strcmp(value, "1")) return 1;
    return 0;
}

static void parse_args(char **argv, int fd, bool client) {
    size_t arg_i = 1;
    char *name_end;

    for (const char *arg, *name; argv[arg_i] && argv[arg_i][0] == '-'; arg_i += !!argv[arg_i]) {
        switch (argv[arg_i][1]) {
        case '\0': /* Invalid syntax */;
            if (client) continue;
            usage(fd, argv[0], EXIT_FAILURE);
        case '-': /* Long options */;
            /* End of flags mark */
            if (!*(name = argv[arg_i] + 2)) {
                arg_i++;
                goto finish;
            }

            /* Options without arguments */
            if (!strcmp(name, "help")) {
                if (client) continue;
                usage(fd, argv[0], EXIT_SUCCESS);
            }
            if (!strcmp(name, "version")) {
                if (client) continue;
                version(fd);
            }

            /* Options with arguments */
            if ((arg = name_end = strchr(name, '=')))
                *name_end = '\0', arg++;

            if (!strncmp(name, "no-", 3) && is_boolean_option(name + 3))
                arg = "false", name += 3;

            if (is_boolean_option(name)) {
                if (!arg) arg = "true";
            } else {
                if (!arg || !*arg)
                    arg = argv[++arg_i];
            }

            if (!client) {
                if (!arg)
                    usage(fd, argv[0], EXIT_FAILURE);
                if (!is_client_only_option(name))
                    send_opt(fd, name, arg);
            } else {
                if (!arg) continue;
                if (!strcmp(name, "config"))
                    config_path = arg;
                else if (!strcmp(name, "socket"))
                    socket_path = arg;
                else if (!strcmp(name, "cwd"))
                    cwd = arg;
                else if (!strcmp(name, "daemon"))
                    need_daemon = is_true(arg);
                else if (!strcmp(name, "quit"))
                    need_exit = is_true(arg);
            }

            // We need to restore the original
            // argv value since we parse options twice.
            if (name_end)
                *name_end = '=';
            continue;
        }

        /* Short options, may be clustered  */
        for (size_t char_i = 1; argv[arg_i] && argv[arg_i][char_i]; char_i++) {
            char letter = argv[arg_i][char_i];
            /* Handle options without arguments */
            switch (letter) {
            case 'e':
                /* Works the same way as -- */
                if (!client && argv[arg_i++][char_i + 1])
                    usage(fd, argv[0], EXIT_FAILURE);
                goto finish;
            case 'h':
                if (client) break;
                usage(fd, argv[0], EXIT_SUCCESS);
            case 'v':
                if (client) break;
                version(fd);
            }

            /* Handle options with arguments (including implicit arguments) */
            if (!is_boolean_short_option(letter)) {
                if (!argv[arg_i][++char_i]) arg_i++, char_i = 0;
                if (!argv[arg_i]) {
                    if (client) break;
                    usage(fd, argv[0], EXIT_FAILURE);
                }
                arg = argv[arg_i] + char_i;
            } else {
                arg = "true";
            }

            if (!client && !is_client_only_short_option(letter))
                send_short_opt(fd, letter, arg);
            else if (client) {
                switch (letter) {
                case 'q':
                    need_exit = 1;
                    break;
                case 'd':
                    need_daemon = 1;
                    break;
                case 'C':
                    config_path = arg;
                    break;
                case 's':
                    socket_path = arg;
                    break;
                }
            }

            if (!is_boolean_short_option(letter)) break;
            else continue;
        }
    }

    if (argv[arg_i]) {
finish:
        if (!client) {
            if (!argv[arg_i])
                usage(fd, argv[0], EXIT_FAILURE);
            while (argv[arg_i])
                send_arg(fd, argv[arg_i++]);
        }
    }

    if (client && cwd && !(cwd = realpath(cwd, buffer)))
        fprintf(stderr, WARN_PREFIX"realpath(): %s\n", strerror(errno));
}

static void do_fork(const char *spath) {
    int res;
    struct stat stt;
    switch (fork()) {
    case -1:
        exit(1);
    case 0:
        switch((res = fork())) {
        case 0:
            setsid();
            execlp("nsst", "nsst", "-d", NULL);
            /* fallthrough */
        default:
            _exit(res <= 0);
        }
    default:
        while(wait(NULL) < 0 && errno == EINTR);
        /* Wait for socket */
        struct timespec ts = {.tv_nsec = STARTUP_DELAY};
        for (int i = 0; stat(spath, &stt) < 0 && i < MAX_WAIT_LOOP; i++)
#if USE_CLOCK_NANOSLEEP
            clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
#else
            nanosleep(&ts, NULL);
#endif
    }
}

static int try_connect(const char *spath) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, spath, sizeof addr.sun_path - 1);

    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) return -1;

    if (connect(fd, (struct sockaddr *)&addr,
            offsetof(struct sockaddr_un, sun_path) + strlen(addr.sun_path)) < 0) {
        close(fd);
        return -2;
    }
    return fd;
}

int main(int argc, char **argv) {

    (void)argc;

    cwd = getcwd(buffer, sizeof(buffer));
    parse_args(argv, -1, true);

    int fd = try_connect(socket_path);
    if (fd < 0 && need_daemon) {
        do_fork(socket_path);
        fd = try_connect(socket_path);
    }

    if (fd < 0) {
        perror(fd == -2 ? "connect()" : "socket()");
        return fd;
    }

    if (need_exit) {
        send_char(fd, '\031');
        return 0;
    }

    send_header(fd, config_path);
    if (cwd) send_opt(fd, "cwd", cwd);

    parse_args(argv, fd, false);

    send_char(fd, '\003' /* ETX */);

    return 0;
}
