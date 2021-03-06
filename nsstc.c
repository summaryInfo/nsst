/* Copyright (c) 2019-2021, Evgeny Baskov. All rights reserved */

#define _XOPEN_SOURCE 500
#define _POSIX_C_SOURCE 200809L

#include "feature.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#define SKIP_OPT ((void*)-1)
#define MAX(x, y) ((x) > (y) ? (x) : (y))

static char buffer[MAX(MAX_OPTION_DESC, PATH_MAX) + 1];

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

static void parse_client_args(char **argv, const char **cpath, const char **spath, _Bool *need_daemon, const char **cwd) {
    size_t ind = 1;
    char *arg, *opt;

    while (argv[ind] && argv[ind][0] == '-') {
        size_t cind = 0;
        if (!argv[ind][1]) exit(EXIT_FAILURE);
        if (argv[ind][1] == '-') {
            if (!argv[ind][2]) {
                ind++;
                break;
            }

            //Long options
            opt = argv[ind] + 2;
            if ((arg = strchr(opt, '='))) {
                if (!*++arg) arg = argv[++ind];
                if (!strncmp(opt, "config=" , 7)) *cpath = arg;
                else if (!strncmp(opt, "socket=", 7)) *spath = arg;
                else if (!strncmp(opt, "daemon", 7)) *need_daemon = 1;
                else if (!strncmp(opt, "cwd=", 4)) {
                    if (!(*cwd = realpath(arg, buffer)))
                        fprintf(stderr, "[\033[33;1mWARN\033[m] realpath(): %s\n", strerror(errno));
                }
            }
        } else while (argv[ind] && argv[ind][++cind]) {
            char letter = argv[ind][cind];
            // One letter options
            switch (letter) {
            case 'd':
                *need_daemon = 1;
                break;
            case 'e':
                return;
            case 'f': case 's': case 'C':
            case 'D': case 'o': case 'c':
            case 't': case 'T': case 'V':
            case 'H': case 'g':
                // Has arguments
                if (!argv[ind][++cind]) ind++, cind = 0;
                if (!argv[ind]) exit(EXIT_FAILURE);
                arg = argv[ind] + cind;

                // Ignore all options with arguments besides -C and -s here
                if (letter == 'C') *cpath = arg;
                else if (letter == 's') *spath = arg;

                goto next;
            }
        }
next:
        if (argv[ind]) ind++;
    }
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

static void parse_server_args(char **argv, int fd) {
    size_t ind = 1;
    char *arg;
    const char *opt;
    while (argv[ind] && argv[ind][0] == '-') {
        size_t cind = 0;
        if (!argv[ind][1]) usage(fd, argv[0], EXIT_FAILURE);
        if (argv[ind][1] == '-') {
            if (!argv[ind][2]) {
                ind++;
                break;
            }

            //Long options
            opt = argv[ind] + 2;

            if ((arg = strchr(opt, '='))) {
                *arg++ = '\0';
                if (!*arg) arg = argv[++ind];

                if (strcmp(opt, "config") && strcmp(opt, "socket") && strcmp(opt, "cwd")) {
                    send_opt(fd, opt, arg);
                }
            } else if (!strcmp(opt, "help")) {
                usage(fd, argv[0], EXIT_SUCCESS);
            } else if (!strcmp(opt, "version")) {
                version(fd);
            } else {
                const char *val = "true";
                if (!strncmp(opt, "no-", 3))
                    opt += 3, val = "false";
                send_opt(fd, opt, val);
            }
        } else while (argv[ind] && argv[ind][++cind]) {
            char letter = argv[ind][cind];
            opt = NULL;
            switch (letter) {
            case 'd':
                /* ignore */
                continue;
            case 'e':
                if (!argv[++ind]) usage(fd, argv[0], EXIT_FAILURE);
                goto end;
            case 'h':
                usage(fd, argv[0], EXIT_SUCCESS);
            case 'v':
                version(fd);
            case 'C':
            case 's': opt = SKIP_OPT; break;
            case 'f': opt = "font"; break;
            case 'D': opt = "term-name"; break;
            case 'o': opt = "printer-file"; break;
            case 'c': opt = "window-class"; break;
            case 't':
            case 'T': opt = "title"; break;
            case 'V': opt = "vt-version"; break;
            case 'H': opt = "scrollback-size"; break;
            case 'g': opt = "geometry"; break;
            }

            if (!opt) {
                fprintf(stderr, "[\033[33;1mWARN\033[m] Unknown option -%c\n", letter);
                break;
            }

            if (!argv[ind][++cind]) ind++, cind = 0;
            if (!argv[ind]) usage(fd, argv[0], EXIT_FAILURE);

            arg = argv[ind] + cind;

            if (opt != SKIP_OPT)
                send_opt(fd, opt, arg);

            break;
        }
        if (argv[ind]) ind++;
    }

end:
    while (argv[ind])
        send_arg(fd, argv[ind++]);
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
        for (int i = 0; stat(spath, &stt) < 0 && i < MAX_WAIT_LOOP; i++)
            clock_nanosleep(CLOCK_MONOTONIC, 0, &(struct timespec){.tv_nsec = STARTUP_DELAY}, NULL);
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
    const char *cpath = NULL, *spath = "/tmp/nsst-sock0";
    _Bool need_daemon = 0;

    (void)argc;

    const char *pcwd = getcwd(buffer, sizeof(buffer));
    parse_client_args(argv, &cpath, &spath, &need_daemon, &pcwd);

    int fd = try_connect(spath);
    if (fd < 0 && need_daemon) {
        do_fork(spath);
        fd = try_connect(spath);
    }

    if (fd < 0) {
        perror(fd == -2 ? "connect()" : "socket()");
        return fd;
    }

    send_header(fd, cpath);
    if (pcwd) send_opt(fd, "cwd", pcwd);

    parse_server_args(argv, fd);

    send_char(fd, '\003' /* ETX */);

    return 0;
}
