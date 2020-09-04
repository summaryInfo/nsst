/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

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

static _Noreturn void usage(char *argv0, int code) {
    if (gconfig.log_level > 0 || code == EXIT_SUCCESS) {
        printf("%s%s", argv0, " [-options] [-e] [command [args]]\n"
            "Where options are:\n"
                "\t--help, -h\t\t\t(Print this message and exit)\n"
                "\t--version, -v\t\t\t(Print version and exit)\n"
                "\t--color<N>=<color>, \t\t(Set palette color <N>, <N> is from 0 to 255)\n"
                "\t--geometry=<value>, -g<value> \t(Window geometry, format is [=][<width>{xX}<height>][{+-}<xoffset>{+-}<yoffset>])\n"
        );
        for (size_t i = 0; i < o_MAX; i++)
            printf("\t--%s=<value>%s\n", optmap[i].opt, optmap[i].descr);
        printf("%s",
            "For every boolean option --<X>=<Y>\n"
                "\t--<X>, --enable-<X>, --with-<X>,\n"
                "\t--<X>=yes, --<X>=y,  --<X>=true\n"
            "are equivalent to --<X>=1, and\n"
                "\t--no-<X>, --disable-<X>, --without-<X>\n"
                "\t--<X>=no, --<X>=n, --<X>=false\n"
            "are equivalent to --<X>=0,\n"
            "where 'yes', 'y', 'true', 'no', 'n' and 'false' are case independet\n"
            "All options are also accept special value 'default' to reset to built-in default\n"
        );
    }
    free_context();
    exit(code);
}

static _Noreturn void version(void) {
    printf("%s"
            "Features: nsst"
#if USE_PPOLL
            "+ppoll"
#endif
#if USE_BOXDRAWING
            "+boxdrawing"
#endif
#if USE_X11SHM
            "+mitshm"
#endif
#if USE_POSIX_SHM
            "+posixshm"
#endif
#if USE_PRECOMPOSE
            "+precompose"
#endif
            "\n", version_string());
    free_context();
    exit(EXIT_SUCCESS);
}

static void parse_options(struct instance_config *cfg, char **argv) {
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
                usage(argv[0], EXIT_FAILURE);
                break;
            case 'v':
                version();
            default:
                // Has arguments
                if (!argv[ind][++cind]) ind++, cind = 0;
                if (!argv[ind]) usage(argv[0], EXIT_FAILURE);
                arg = argv[ind] + cind;

                const char *opt = NULL;
                switch (letter) {
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
                if (opt && !set_option(cfg, opt, arg, 1)) {
                } else {
                    // Treat all unknown options not having arguments
                    if (cind) cind--;
                    warn("Unknown option -%c", letter);
                    // Next option, same argv element
                    continue;
                }
                // Next argv element
                goto next;
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
        // TODO: Check for supported NRCSs and prefere them to luit
        gconfig.utf8 = !strncasecmp(charset, "UTF", 3) && (charset[3] == '8' || charset[4] == '8');
        bool supported = !strcasecmp(charset, "ISO-8859-1") || !strcasecmp(charset, "ASCII");
        gconfig.want_luit = !supported && !gconfig.utf8;
    }

    init_context();
    init_default_termios();

    // Parse --config/-C argument before parsing config file
    for (int i = 1; i < argc; i++) {
        char *arg = NULL;
        if (!strncmp(argv[i], "--config=", sizeof "--config=" - 1)) {
            arg = argv[i] + sizeof "--config=" - 1;
            if (!*arg) arg = argv[++i];
            if (!arg) usage(argv[0], EXIT_FAILURE);
        } else if (!strncmp(argv[i], "-C", sizeof "-C" - 1)) {
            arg = argv[i] + sizeof "-C" - 1;
            if (!*arg) arg = argv[++i];
            if (!arg) usage(argv[0], EXIT_FAILURE);
        }
        if (arg) set_option(&cfg, "config", arg, 1);
    }

    init_instance_config(&cfg, 1);
    parse_options(&cfg, argv);

    if (!gconfig.daemon_mode) create_window(&cfg);

    free_config(&cfg);

    run();

    free_context();

    return EXIT_SUCCESS;
}
