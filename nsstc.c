/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#define _POSIX_C_SOURCE 200809L

#include "feature.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

_Noreturn void usage(const char *argv0, int code) {
    // TODO
    exit(code);
}

_Noreturn void version(void) {
    // TODO
    exit(0);
}

static void parse_client_args(char **argv, char **cpath, char **spath) {
    size_t ind = 1;
    char *arg, *opt;

    while (argv[ind] && argv[ind][0] == '-') {
        size_t cind = 0;
        if (!argv[ind][1]) usage(argv[0], EXIT_FAILURE);
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
            } else if (!strcmp(opt, "help")) {
                usage(argv[0], EXIT_SUCCESS);
            } else if (!strcmp(opt, "version")) {
                version();
            }
        } else while (argv[ind] && argv[ind][++cind]) {
            char letter = argv[ind][cind];
            // One letter options
            switch (letter) {
            case 'd':
                /* ignore */
                break;
            case 'e':
                return;
            case 'h':
                usage(argv[0], EXIT_SUCCESS);
            case 'v':
                version();
            default:
                // Has arguments
                if (!argv[ind][++cind]) ind++, cind = 0;
                if (!argv[ind]) usage(argv[0], EXIT_FAILURE);
                arg = argv[ind] + cind;

                switch (letter) {
                case 'C': *cpath = arg; goto next;
                case 's': *spath = arg; goto next;
                }

                // Treat all unknown options not having arguments
                if (cind) cind--;
            }
        }
next:
        if (argv[ind]) ind++;
    }
}

#define MAX_ARG_LEN 512

static void send_opt(int fd, char *opt, char *value) {
    struct iovec dvec[4] = {
        { .iov_base = "\035", .iov_len = 1 },
        { .iov_base = opt, .iov_len = strlen(opt) },
        { .iov_base = "=", .iov_len = 1 },
        { .iov_base = value, .iov_len = strlen(value) },
    };

    struct msghdr hdr = {
        .msg_iov = dvec,
        .msg_iovlen = sizeof dvec / sizeof *dvec
    };

    for (int res; (res = sendmsg(fd, &hdr, 0)) < 0 && errno == EAGAIN;);
}

static void send_arg(int fd, char *arg) {
    struct iovec dvec[4] = {
        { .iov_base = "\036", .iov_len = 1 },
        { .iov_base = arg, .iov_len = strlen(arg) },
    };

    struct msghdr hdr = {
        .msg_iov = dvec,
        .msg_iovlen = sizeof dvec / sizeof *dvec
    };
    
    for (int res; (res = sendmsg(fd, &hdr, 0)) < 0 && errno == EAGAIN;);
}

static void send_header(int fd, char *cpath) {
    struct iovec dvec[4] = {
        { .iov_base = "\001", .iov_len = 1 },
        { .iov_base = cpath, .iov_len = cpath ? strlen(cpath) : 0 },
    };

    struct msghdr hdr = {
        .msg_iov = dvec,
        .msg_iovlen = 1 + !!cpath,
    };
    
    for (int res; (res = sendmsg(fd, &hdr, 0)) < 0 && errno == EAGAIN;);
}

static void send_footer(int fd) {
    for (int res; (res = send(fd, "\003", 1, 0)) < 0 && errno == EAGAIN;);
}

static void parse_server_args(char **argv, int fd) {
    size_t ind = 1;
    char *arg, *opt;
    while (argv[ind] && argv[ind][0] == '-') {
        size_t cind = 0;
        if (!argv[ind][1]) usage(argv[0], EXIT_FAILURE);
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

                if (strcmp(opt, "config") && strcmp(opt, "socket"))
                    send_opt(fd, opt, arg);
            } else {
                char *val = "true";
                if (!strncmp(opt, "no-", 3))
                    opt += 3, val = "false";
                send_opt(fd, opt, val);
            }
        } else while (argv[ind] && argv[ind][++cind]) {
            char letter = argv[ind][cind];
            // One letter options
            switch (letter) {
            case 'd':
                /* ignore */
                break;
            case 'e':
                if (!argv[++ind]) usage(argv[0], EXIT_FAILURE);
                goto end;
            default:
                // Has arguments
                if (!argv[ind][++cind]) ind++, cind = 0;
                if (!argv[ind]) usage(argv[0], EXIT_FAILURE);
                arg = argv[ind] + cind;

                char *opt = NULL;
                switch (letter) {
                case 'C':
                case 's': goto next;
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
                if (opt) {
                    send_opt(fd, opt, arg);
                    goto next;
                }
                // Treat all unknown options not having arguments
                if (cind) cind--;
                printf("Unknown option -%c", letter);
                // Next option, same argv element
            }
        }
    next:
        if (argv[ind]) ind++;
    }

end:
    while (argv[ind])
        send_arg(fd, argv[ind++]);
}

int main(int argc, char **argv) {
    char *cpath = NULL, *spath = "/tmp/nsst-sock0";

    parse_client_args(argv, &cpath, &spath);
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, spath, sizeof addr.sun_path - 1);

    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) return 1;

    if (connect(fd, (struct sockaddr *)&addr,
            offsetof(struct sockaddr_un, sun_path) + strlen(addr.sun_path)) < 0) {
        perror("connect()");
        return 2;
    }

    send_header(fd, cpath);

    parse_server_args(argv, fd);

    send_footer(fd);

    return 0;

}
