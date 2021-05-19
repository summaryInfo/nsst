/* Copyright (c) 2019-2021, Evgeny Baskov. All rights reserved */

#include "feature.h"

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "feature.h"
#include "input.h"
#include "tty.h"
#include "util.h"
#include "window.h"

#include <fcntl.h>
#include <inttypes.h>
#include <langinfo.h>
#include <locale.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SKIP_OPT ((void*)-1)

static _Noreturn void usage(const char *argv0, int code) {
    if (gconfig.log_level > 0 || code == EXIT_SUCCESS) {
        ssize_t i = 0;
        do fputs(argv0, stdout);
        while((argv0 = usage_string(i++)));
    }
    free_context();
    exit(code);
}

static _Noreturn void version(void) {
    printf("%sFeatures: %s", version_string(), features_string());
    free_context();
    exit(EXIT_SUCCESS);
}

static void parse_options(struct instance_config *cfg, char **argv) {
    size_t ind = 1;

    char *arg;
    const char *opt;
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

            if ((arg = strchr(argv[ind], '='))) {
                *arg++ = '\0';
                if (!*arg) arg = argv[++ind];

                if (strcmp(opt, "config") && !set_option(cfg, opt, arg, 1))
                    usage(argv[0], EXIT_FAILURE);
            } else {
                if (!strcmp(opt, "help"))
                    usage(argv[0], EXIT_SUCCESS);
                else if (!strcmp(opt, "version"))
                    version();
                else {
                    const char *val = "true";
                    if (!strncmp(opt, "no-", 3)) opt += 3, val = "false";
                    if (!set_option(cfg, opt, val, 1))
                        usage(argv[0], EXIT_FAILURE);
                }
            }
        } else while (argv[ind] && argv[ind][++cind]) {
            char letter = argv[ind][cind];
            // One letter options
            switch (letter) {
            case 'd':
                gconfig.daemon_mode = 1;
                break;
            case 'e':
                if (!argv[++ind]) usage(argv[0], EXIT_FAILURE);
                cfg->argv = &argv[ind];
                return;
            case 'h':
                usage(argv[0], EXIT_SUCCESS);
            case 'v':
                version();
            default:;
                opt = NULL;
                switch (letter) {
                case 'C': opt = SKIP_OPT; break;
                case 'f': opt = "font"; break;
                case 'D': opt = "term-name"; break;
                case 'o': opt = "printer-file"; break;
                case 'c': opt = "window-class"; break;
                case 't':
                case 'T': opt = "title"; break;
                case 'V': opt = "vt-version"; break;
                case 'H': opt = "scrollback-size"; break;
                case 'g': opt = "geometry"; break;
                case 's': opt = "socket"; break;
                }

                if (opt) {
                    // Has arguments
                    if (!argv[ind][++cind]) ind++, cind = 0;
                    if (!argv[ind]) usage(argv[0], EXIT_FAILURE);
                    arg = argv[ind] + cind;

                    if (opt != SKIP_OPT)
                        set_option(cfg, opt, arg, 1);

                    goto next;
                }

                warn("Unknown option -%c", letter);
            }
        }
    next:
        if (argv[ind]) ind++;
    }

    if (argv[ind]) cfg->argv = &argv[ind];

    // Parse all shortcuts
    keyboard_parse_config(cfg);
}

static struct instance_config cfg;

int main(int argc, char **argv) {

    // Load locale
    setlocale(LC_CTYPE, "");

    char *charset = nl_langinfo(CODESET);
    if (charset) {
        // Builtin support for locales only include UTF-8, Latin-1 and ASCII
        // TODO: Check for supported NRCSs and prefer them to luit
        bool utf8 = !strncasecmp(charset, "UTF", 3) && (charset[3] == '8' || charset[4] == '8');
        bool supported = 0;
        const char *lc_supported[] = {
            "C",
            "POSIX",
            "ASCII",
            "US-ASCII",
            "ANSI_X3.4-1968",
            "ISO-8869-1",
            "ISO8869-1",
        };
        for (size_t i = 0; !supported && i < sizeof(lc_supported)/sizeof(*lc_supported); i++)
            supported |= !strcasecmp(charset, lc_supported[i]);

        set_default_utf8(utf8);
        gconfig.want_luit = !supported && !utf8;
    }

    /* Initialize cached hostname
     * (let's assume that it won't change when terminal is running) */
    gethostname(gconfig.hostname, MAX_DOMAIN_NAME - 1);

    init_context();
    init_default_termios();

    // Parse --config/-C argument before parsing config file
    char *cpath = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--config=", sizeof "--config=" - 1) ||
                !strncmp(argv[i], "-C", sizeof "-C" - 1)) {
            char *arg = argv[i] + (argv[i][1] == '-' ? sizeof "--config=" : sizeof "-C") - 1;
            if (!*arg) arg = argv[++i];
            if (!arg) usage(argv[0], EXIT_FAILURE);
            cpath = arg;
        }
    }

    init_instance_config(&cfg, cpath, 1);
    parse_options(&cfg, argv);

    if (!gconfig.daemon_mode) create_window(&cfg);

    free_config(&cfg);

    run();

    free_context();

    return EXIT_SUCCESS;
}
