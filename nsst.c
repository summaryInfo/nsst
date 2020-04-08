/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#define _POSIX_C_SOURCE 200809L

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include "window.h"
#include "util.h"
#include "config.h"

void usage(char *argv0) {
    die("%s [-c class ] [-a] [-f font] [-o dump_file] [-[tT] title] [-v] [-e] [command [args]]", argv0);
/*
  -g[=][<width>{xX}<height>][{+-}<xoffset>{+-}<yoffset>]

  --scrollback-size=10000
        -H<lines>
  --vt-version=220
        -V<version>
  --tab-width=8
  --cursor-shape=
  --underline-width=
  --cursor-width=
  --vertical-border=
  --horizontal-border=
  --blink-time=
  --font-size=
  --font-spacing=
  --font-subpixel=
  --font-gamma=
  --display-dpi=96
  --use-utf8=1
  --allow-charsets=1
  --enable-autowrap=1
  --scroll-on-input=1
  --scroll-on-output=0
  --enable-reverse-video=0
  --allow-alternate=1

    //Input options
  --modify-function=3
  --modify-cursor=3
  --modify-keypad=3
  --modify-other=0
  --modify-other-fmt=0
  --modkey-allow-keypad=0
  --modkey-allow-edit-keypad=0
  --modkey-allow-function=0
  --modkey-allow-misc=0
  --appkey=0
  --appcursor=0
  --numlock=1
  --lock-keyboard=0
  --has-meta=1
  --meta-sends-escape=1
  --backspace-is-delete=1
  --delete-is-delete=0
  --fkey-increment=10
  --keyboard-mapping=default
*/

}

void version(void) {
    fprintf(stderr, "Not So Simple Terminal v1.0.0\n");
    exit(EXIT_SUCCESS);
}

struct optmap_item {
    const char *name;
    enum nss_config_opt opt;
};

static int optmap_cmp(const void *a, const void *b) {
    const char *a_name = ((const struct optmap_item *)a)->name;
    const char *b_name = ((const struct optmap_item *)b)->name;
    return strcmp(a_name, b_name);
}

static void parse_geometry(char *arg, char *argv0) {
    int16_t x = 0, y = 0, w = 0, h = 0;
    char xsgn = '+', ysgn = '+';
    if (arg[0] == '=') arg++;
    if (arg[0] == '+' || arg[0] == '-') {
        _Bool scanned = sscanf(arg, "%c%"SCNd16"%c%"SCNd16, &xsgn, &x, &ysgn, &y) == 4;
        if (!scanned || (xsgn != '+' && xsgn != '-') || (ysgn != '+' && ysgn != '-'))
            usage(argv0);
        if (xsgn == '-') x = -x;
        if (ysgn == '-') y = -y;
    } else {
        int res = sscanf(arg, "%"SCNd16"%*[xX]%"SCNd16"%c%"SCNd16"%c%"SCNd16,
                &w, &h, &xsgn, &x, &ysgn, &y);
        if (res == 6) {
            if ((xsgn != '+' && xsgn != '-') || (ysgn != '+' && ysgn != '-'))
                usage(argv0);
            if (xsgn == '-') x = -x;
            if (ysgn == '-') y = -y;
        } else if (res != 2) usage(argv0);
        nss_config_set_integer(NSS_ICONFIG_WINDOW_WIDTH, w);
        nss_config_set_integer(NSS_ICONFIG_WINDOW_HEIGHT, h);
    }
    nss_config_set_integer(NSS_ICONFIG_WINDOW_X, x);
    nss_config_set_integer(NSS_ICONFIG_WINDOW_Y, y);
    nss_config_set_integer(NSS_ICONFIG_WINDOW_NEGATIVE_X, xsgn == '-');
    nss_config_set_integer(NSS_ICONFIG_WINDOW_NEGATIVE_Y, ysgn == '-');
}

static char **parse_options(int argc, char **argv) {
    size_t ind = 1;

    char *arg;
    while (argv[ind] && argv[ind][0] == '-') {
        size_t cind = 0;
        if (!argv[ind][1]) usage(argv[0]);
        if (argv[ind][1] == '-') {
            if (!argv[ind][2]) {
                ind++;
                break;
            }
            //Long options

            if ((arg = strchr(argv[ind], '='))) {
                if (arg[1]) *arg++ = '\0';
                else arg = argv[++ind];
            }
            if (!arg) usage(argv[0]);

            static struct optmap_item *res, map[] = {
                {"allow-alternate", NSS_ICONFIG_ALLOW_ALTSCREEN},
                {"allow-charsets", NSS_ICONFIG_ALLOW_CHARSETS},
                {"answerback-string", NSS_SCONFIG_ANSWERBACK_STRING},
                {"appcursor", NSS_ICONFIG_INPUT_APPCURSOR},
                {"appkey", NSS_ICONFIG_INPUT_APPKEY},
                {"backspace-is-delete", NSS_ICONFIG_INPUT_BACKSPACE_IS_DELETE},
                {"blink-time",NSS_ICONFIG_BLINK_TIME},
                {"cursor-shape", NSS_ICONFIG_CURSOR_SHAPE},
                {"cursor-width",NSS_ICONFIG_CURSOR_WIDTH},
                {"delete-is-delete", NSS_ICONFIG_INPUT_DELETE_IS_DELETE},
                {"enable-autowrap", NSS_ICONFIG_INIT_WRAP},
                {"enable-reverse-video", NSS_ICONFIG_REVERSE_VIDEO},
                {"fkey-increment", NSS_ICONFIG_INPUT_FKEY_INCREMENT},
                {"font", NSS_SCONFIG_FONT_NAME},
                {"font-gamma",NSS_ICONFIG_GAMMA},
                {"font-size",NSS_ICONFIG_FONT_SIZE},
                {"font-spacing", NSS_ICONFIG_FONT_SPACING},
                {"font-subpixel",NSS_ICONFIG_SUBPIXEL_FONTS},
                {"force-dpi",NSS_ICONFIG_DPI},
                {"has-meta", NSS_ICONFIG_INPUT_HAS_META},
                {"horizontal-border",NSS_ICONFIG_TOP_BORDER},
                {"keyboard-mapping", NSS_ICONFIG_INPUT_MAPPING},
                {"lock-keyboard", NSS_ICONFIG_INPUT_LOCK},
                {"meta-sends-escape", NSS_ICONFIG_INPUT_META_IS_ESC},
                {"modify-cursor", NSS_ICONFIG_INPUT_MODIFY_CURSOR},
                {"modify-function", NSS_ICONFIG_INPUT_MODIFY_FUNCTION},
                {"modify-keypad", NSS_ICONFIG_INPUT_MODIFY_KEYPAD},
                {"modify-other", NSS_ICONFIG_INPUT_MODIFY_OTHER},
                {"modify-other-fmt", NSS_ICONFIG_INPUT_MODIFY_OTHER_FMT},
                {"modkey-allow-edit-keypad", NSS_ICONFIG_INPUT_MALLOW_EDIT},
                {"modkey-allow-function", NSS_ICONFIG_INPUT_MALLOW_FUNCTION},
                {"modkey-allow-keypad", NSS_ICONFIG_INPUT_MALLOW_KEYPAD},
                {"modkey-allow-misc", NSS_ICONFIG_INPUT_MALLOW_MISC},
                {"numlock", NSS_ICONFIG_INPUT_NUMLOCK},
                {"printer", NSS_SCONFIG_PRINTER},
                {"scroll-on-input", NSS_ICONFIG_SCROLL_ON_INPUT},
                {"scroll-on-output", NSS_ICONFIG_SCROLL_ON_OUTPUT},
                {"scrollback-size", NSS_ICONFIG_HISTORY_LINES},
                {"shell", NSS_SCONFIG_SHELL},
                {"tab-width", NSS_ICONFIG_TAB_WIDTH},
                {"term-name", NSS_SCONFIG_TERM_NAME},
                {"title", NSS_SCONFIG_TITLE},
                {"underline-width",NSS_ICONFIG_UNDERLINE_WIDTH},
                {"use-utf8", NSS_ICONFIG_UTF8},
                {"vertical-border",NSS_ICONFIG_LEFT_BORDER},
                {"vt-version", NSS_ICONFIG_VT_VERION},
                {"window-class", NSS_SCONFIG_TERM_CLASS},
            };
            res = bsearch(&(struct optmap_item){argv[ind] + 2}, map,
                    sizeof(map)/sizeof(*map), sizeof(*map), optmap_cmp);
            if (res) nss_config_set_string(res->opt, arg);
            else if (!strcmp(argv[ind] + 2, "geometry"))
				parse_geometry(arg, argv[0]);
            else usage(argv[0]);
        } else while (argv[ind] && argv[ind][++cind]) {
            char letter = argv[ind][cind];
            // One letter options
            switch (letter) {
            case 'e':
                if (!argv[++ind]) usage(argv[0]);
                return &argv[ind];
            case 'h':
                usage(argv[0]);
                break;
            case 'v':
                version();
            default:
                // Has arguments
                if (!argv[ind][++cind]) ind++, cind = 0;
                if (!argv[ind]) usage(argv[0]);
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
                if (opt > 0) nss_config_set_string(opt, arg);
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
    nss_init_context();

    const char **res = (const char **)parse_options(argc, argv);
    if (res) nss_config_set_argv(res);

    nss_create_window(NULL, 0, NULL);
    nss_context_run();
    nss_free_context();
    return 0;
}
