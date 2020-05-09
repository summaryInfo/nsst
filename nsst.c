/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#include "feature.h"

#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "feature.h"
#include "util.h"
#include "window.h"

#include <inttypes.h>
#include <langinfo.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdio.h>

static int optmap_cmp(const void *a, const void *b) {
    const char *a_arg_name = ((const struct nss_optmap_item *)a)->arg_name;
    const char *b_arg_name = ((const struct nss_optmap_item *)b)->arg_name;
    return strcmp(a_arg_name, b_arg_name);
}

static _Noreturn void usage(char *argv0, int code) {
    if (nss_config_integer(NSS_ICONFIG_LOG_LEVEL) > 0 || code == EXIT_SUCCESS) {
        printf("%s%s", argv0, " [-options] [-e] [command [args]]\n"
            "\nWhere options are:\n"
                "\t--help, -h\t\t\t(Print this message and exit)\n"
                "\t--version, -v\t\t\t(Print version and exit)\n"
                "\t--color<N>=<color>, \t\t(Set palette color <N>, <N> is from 0 to 255)\n"
                "\t--geometry=<value>, -g<value> \t(Window geometry, format is [=][<width>{xX}<height>][{+-}<xoffset>{+-}<yoffset>])\n"
        );
        for (size_t i = 0; i < sizeof(optmap)/sizeof(optmap[0]); i++)
            printf("\t--%s=<value>%s\n", optmap[i].arg_name, optmap[i].arg_desc);
    }
    nss_free_context();
    exit(code);
}

static _Noreturn void version(void) {
    fprintf(stderr, "Not So Simple Terminal v1.0.0\n"
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
            "\n"
    );
    nss_free_context();
    exit(EXIT_SUCCESS);
}

static void parse_geometry(char *arg, char *argv0) {
    int16_t x = 0, y = 0, w = 0, h = 0;
    char xsgn = '+', ysgn = '+';
    if (arg[0] == '=') arg++;
    if (arg[0] == '+' || arg[0] == '-') {
        _Bool scanned = sscanf(arg, "%c%"SCNd16"%c%"SCNd16, &xsgn, &x, &ysgn, &y) == 4;
        if (!scanned || (xsgn != '+' && xsgn != '-') || (ysgn != '+' && ysgn != '-'))
            usage(argv0, EXIT_FAILURE);
        if (xsgn == '-') x = -x;
        if (ysgn == '-') y = -y;
    } else {
        int res = sscanf(arg, "%"SCNd16"%*[xX]%"SCNd16"%c%"SCNd16"%c%"SCNd16,
                &w, &h, &xsgn, &x, &ysgn, &y);
        if (res == 6) {
            if ((xsgn != '+' && xsgn != '-') || (ysgn != '+' && ysgn != '-'))
                usage(argv0, EXIT_FAILURE);
            if (xsgn == '-') x = -x;
            if (ysgn == '-') y = -y;
        } else if (res != 2) usage(argv0, EXIT_FAILURE);
        nss_config_set_integer(NSS_ICONFIG_WINDOW_WIDTH, w);
        nss_config_set_integer(NSS_ICONFIG_WINDOW_HEIGHT, h);
    }
    nss_config_set_integer(NSS_ICONFIG_HAS_GEOMETRY, 1);
    nss_config_set_integer(NSS_ICONFIG_WINDOW_X, x);
    nss_config_set_integer(NSS_ICONFIG_WINDOW_Y, y);
    nss_config_set_integer(NSS_ICONFIG_WINDOW_NEGATIVE_X, xsgn == '-');
    nss_config_set_integer(NSS_ICONFIG_WINDOW_NEGATIVE_Y, ysgn == '-');
}

static char **parse_options(char **argv) {
    size_t ind = 1;

    char *arg, *opt;
    while (argv[ind] && argv[ind][0] == '-') {
        size_t cind = 0, n;
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

                nss_optmap_item_t *res = bsearch(&(nss_optmap_item_t){opt, NULL, NULL, 0},
                        optmap, OPT_MAP_SIZE, sizeof(*optmap), optmap_cmp);
                if (res && arg)
                    nss_config_set_string(res->opt, arg);
                else if (arg && !strcmp(opt, "geometry"))
                    parse_geometry(arg, argv[0]);
                else if (arg && !strncmp(opt, "color", 5) &&
                        sscanf(opt, "color%zu", &n) == 1)
                    nss_config_set_string(NSS_CCONFIG_COLOR_0 + n, arg);
                else  usage(argv[0], EXIT_FAILURE);
            } else {
                if (!strcmp(opt, "help"))
                    usage(argv[0], EXIT_SUCCESS);
                else if (!strcmp(opt, "version"))
                    version();
                else if (!strcmp(opt, "no-config-file")) /* NOTHING */;
                else {
                    _Bool val = 1;
                    if (!strncmp(opt, "no-", 3)) opt += 3, val = 0;
                    else if (!strncmp(opt, "enable-", 7)) opt += 7, val = 1;
                    else if (!strncmp(opt, "disable-", 8)) opt += 8, val = 0;
                    else if (!strncmp(opt, "with-", 5)) opt += 5, val = 1;
                    else if (!strncmp(opt, "without-", 8)) opt += 8, val = 0;
                    nss_optmap_item_t *res = bsearch(&(nss_optmap_item_t){opt, NULL, NULL, 0},
                            optmap, OPT_MAP_SIZE, sizeof(*optmap), optmap_cmp);
                    if (!res || !nss_config_bool(res->opt, val))
                        usage(argv[0], EXIT_FAILURE);
                }
            }
        } else while (argv[ind] && argv[ind][++cind]) {
            char letter = argv[ind][cind];
            // One letter options
            switch (letter) {
            case 'e':
                if (!argv[++ind]) usage(argv[0], EXIT_FAILURE);
                return &argv[ind];
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

                enum nss_config_opt opt = 0;
                switch (letter) {
                case 'f': opt = NSS_SCONFIG_FONT_NAME; break;
                case 's': opt = NSS_SCONFIG_SHELL; break;
                case 'D': opt = NSS_SCONFIG_TERM_NAME; break;
                case 'o': opt = NSS_SCONFIG_PRINTER; break;
                case 'c': opt = NSS_SCONFIG_TERM_CLASS; break;
                case 't':
                case 'T': opt = NSS_SCONFIG_TITLE; break;
                case 'V': opt = NSS_ICONFIG_VT_VERION; break;
                case 'H': opt = NSS_ICONFIG_HISTORY_LINES; break;
                }
                if (opt) nss_config_set_string(opt, arg);
                else if (letter == 'g') {
                    // Still need to parse geometry
                    parse_geometry(arg, argv[0]);
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
        if(argv[ind]) ind++;
    }
    return argv[ind] ? &argv[ind] : NULL;
}

int main(int argc, char **argv) {

    // Load locale
    setlocale(LC_ALL, "");
    char *charset = nl_langinfo(CODESET);
    _Bool bset = charset && (charset[0] & ~0x20) == 'U' &&
            (charset[1] & ~0x20) == 'T' && (charset[2] & ~0x20) == 'F' &&
            (charset[3] == '8' || charset[4] == '8');
    // Enable UTF-8 support if it is UTF-8
    nss_config_set_integer(NSS_ICONFIG_UTF8, bset);

    //Initialize graphical context
    for (char **opt = argv; *opt; opt++)
        if (!strcmp("--no-config-file", *opt))
            nss_config_set_integer(NSS_ICONFIG_SKIP_CONFIG_FILE, 1);
    nss_init_context();

    (void)argc;

    const char **res = (const char **)parse_options(argv);
    if (res) nss_config_set_argv(res);

    nss_create_window();
    nss_context_run();
    nss_free_context();

    return EXIT_SUCCESS;
}
