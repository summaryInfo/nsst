/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

#include "feature.h"


#include "config.h"
#include "util.h"
#include "window.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>

#define MAX_ARG_LEN 512
#define INIT_ARGN 4
#define ARGN_STEP(x) (MAX(INIT_ARGN, 3*(x)/2))
#define NUM_PENDING 8

struct pending_launch {
    struct list_head link;
    ssize_t argn;
    ssize_t argcap;
    ssize_t poll_index;
    int fd;
    char **args;
    struct instance_config cfg;
};

static struct list_head pending_launches;

static int socket_index = 1;
static int socket_fd = -1;
static bool need_exit = 0;

static void daemonize(void) {
    pid_t pid = fork();
    if (pid > 0)
        _exit(0);
    else if (pid < 0)
        die("Can't fork() daemon: %s", strerror(errno));

    if (setsid() < 0)
        die("Can't setsid(): %s", strerror(errno));

    pid = fork();
    if (pid > 0)
        _exit(0);
    else if (pid < 0)
        die("Can't fork() daemon: %s", strerror(errno));

    int devnull = open("/dev/null", O_RDONLY);
    dup2(STDERR_FILENO, STDOUT_FILENO);
    dup2(devnull, STDIN_FILENO);
    close(devnull);

    if (chdir("/") < 0)
        warn("chdir(): %s", strerror(errno));
}


bool init_daemon(void) {
    list_init(&pending_launches);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, gconfig.sockpath, sizeof addr.sun_path - 1);

    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) {
        warn("Can't create daemon socket: %s", strerror(errno));
        return false;
    }

    set_cloexec(fd);

    if (bind(fd, (struct sockaddr*)&addr,
            offsetof(struct sockaddr_un, sun_path) + strlen(addr.sun_path)) < 0) {
        warn("Can't bind daemon socket: %s", strerror(errno));
        close(fd);
        return false;
    }
    if (listen(fd, NUM_PENDING) < 0) {
        warn("Can't listen to daemon socket: %s", strerror(errno));
        close(fd);
        unlink(gconfig.sockpath);
        return false;
    }

    socket_fd = fd;
    socket_index = poller_alloc_index(fd, POLLIN | POLLHUP);

    if (gconfig.fork) daemonize();
    return true;
}

void free_daemon(void) {
    if (!gconfig.daemon_mode)
        return;
    if (socket_index >= 0) {
        poller_free_index(socket_index);
        socket_index = -1;
    }
    if (socket_fd > 0) {
        close(socket_fd);
        socket_fd = -1;
    }
    unlink(gconfig.sockpath);
    gconfig.daemon_mode = 0;
}

static void free_pending_launch(struct pending_launch *lnch) {
    list_remove(&lnch->link);

    close(lnch->fd);
    poller_free_index(lnch->poll_index);

    for (ssize_t i = 0; i < lnch->argn; i++)
        free(lnch->args[i]);
    free(lnch->args);
    free_config(&lnch->cfg);
    free(lnch);
}

static bool send_pending_launch_resp(struct pending_launch *lnch, const char *str) {
    if (send(lnch->fd, str, strlen(str), 0) < 0) {
        warn("Can't send responce to client, dropping");
        free_pending_launch(lnch);
        return 0;
    }
    return 1;
}

static void append_pending_launch(struct pending_launch *lnch) {
    char buffer[MAX_ARG_LEN + 1];

    ssize_t len = recv(lnch->fd, buffer, MAX_ARG_LEN, 0);
    if (len < 0) {
        warn("Can't recv argument: %s", strerror(errno));
        free_pending_launch(lnch);
        return;
    }

    buffer[len] = '\0';

    if (buffer[0] == '\001' /* SOH */) /* Header */ {
        char *cpath = len > 1 ? buffer + 1 : NULL;
        init_instance_config(&lnch->cfg, cpath, 0);
    } else if (buffer[0] == '\003' /* ETX */ && len == 1) /* End of configuration */ {
        if (lnch->args) lnch->args[lnch->argn] = NULL;
        lnch->cfg.argv = lnch->args;
        create_window(&lnch->cfg);
        free_pending_launch(lnch);
    } else if (buffer[0] == '\034' /* FS */ && len > 1) /* Short option */ {
        char *name = buffer + 1, *value = memchr(buffer + 1, '=', len);
        if (!value || name + 1 != value) {
            warn("Wrong option format: '%s'", name);
            return;
        }

        struct option *opt = find_short_option_entry(*name);
        if (opt) set_option_entry(&lnch->cfg, opt, value, true);
    } else if (buffer[0] == '\035' /* GS */ && len > 1) /* Option */ {
        char *name = buffer + 1, *value = memchr(buffer + 1, '=', len);
        if (!value) {
            warn("Wrong option format: '%s'", name);
            return;
        }

        *value++ = '\0';
        struct option *opt = find_option_entry(name, true);
        if (opt) set_option_entry(&lnch->cfg, opt, value, true);
    } else if (buffer[0] == '\036' /* RS */ && len > 1) /* Argument */ {
        if (lnch->argn + 2 > lnch->argcap) {
            ssize_t newsz = ARGN_STEP(lnch->argcap);
            lnch->args = xrealloc(lnch->args, lnch->argcap*sizeof(*lnch->args), newsz*sizeof(*lnch->args));
            lnch->argcap = newsz;
        }

        lnch->args[lnch->argn++] = strdup(buffer + 1);
    } else if (buffer[0] == '\005' /* ENQ */ && len == 1) /* Version text request */ {
        const char *resps[] = {version_string(), "Features: ", features_string()};

        for (size_t i = 0; i < LEN(resps); i++)
            if (!send_pending_launch_resp(lnch, resps[i])) return; /* Don't free pending_launch twice */

        free_pending_launch(lnch);
    } else if (buffer[0] == '\025' /* NAK */ && len == 1) /* Usage text request */ {
        ssize_t i = 0;

        for (const char *part = usage_string(i++); part; part = usage_string(i++))
            if (!send_pending_launch_resp(lnch, part)) return; /* Don't free pending_launch twice */

        free_pending_launch(lnch);
    } else if (buffer[0] == '\031' /* EM */ && len == 1) /* Exit daemon*/ {
        need_exit = 1;
        free_pending_launch(lnch);
    }
}

static void accept_pending_launch(void) {
    int fd = accept(socket_fd, NULL, NULL);

    set_cloexec(fd);

    struct pending_launch *lnch = xzalloc(sizeof(struct pending_launch));
    if (fd < 0 || (lnch->poll_index = poller_alloc_index(fd, POLLIN | POLLHUP)) < 0) {
        close(fd);
        free(lnch);
        warn("Can't create pending launch: %s", strerror(errno));
    } else {
        lnch->fd = fd;
        list_insert_after(&pending_launches, &lnch->link);
    }
}

bool daemon_process_clients(void) {
    if (socket_fd < 0) return 0;
    /* Handle daemon requests */

    int rev = poller_index_events(socket_index);
    if (rev & POLLIN) accept_pending_launch();
    else if (rev & (POLLERR | POLLNVAL | POLLHUP)) free_daemon();

    /* Handle pending launches */
    LIST_FOREACH_SAFE(it, &pending_launches) {
        struct pending_launch *holder = CONTAINEROF(it, struct pending_launch, link);
        rev = poller_index_events(holder->poll_index);
        if (rev & POLLIN) append_pending_launch(holder);
        else if (rev & (POLLERR | POLLHUP | POLLNVAL)) free_pending_launch(holder);
    }

    return need_exit;
}
