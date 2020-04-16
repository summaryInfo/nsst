/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <langinfo.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "util.h"
#include "window.h"


static struct optmap_item {
    const char *name;
    const char *descr;
    enum nss_config_opt opt;
} map[] = {
    {"allow-alternate", "\t(Enable alternate screen)", NSS_ICONFIG_ALLOW_ALTSCREEN},
    {"allow-charsets", "\t(Enable charsets support)", NSS_ICONFIG_ALLOW_CHARSETS},
    {"allow-nrcs", "\t(Enable NRCSs support)", NSS_ICONFIG_ALLOW_NRCS},
    {"answerback-string", "\t(ENQ report)", NSS_SCONFIG_ANSWERBACK_STRING},
    {"appcursor", "\t\t(Initial application cursor mode value)", NSS_ICONFIG_INPUT_APPCURSOR},
    {"appkey", "\t\t(Initial application keypad mode value)", NSS_ICONFIG_INPUT_APPKEY},
    {"backspace-is-delete", "\t(Backspace sends DEL instead of BS)", NSS_ICONFIG_INPUT_BACKSPACE_IS_DELETE},
    {"blink-time", "\t\t(Text blink interval)",NSS_ICONFIG_BLINK_TIME},
    {"cursor-shape", "\t\t(Shape of cursor)", NSS_ICONFIG_CURSOR_SHAPE},
    {"cursor-width", "\t\t(Width of lines that forms cursor)",NSS_ICONFIG_CURSOR_WIDTH},
    {"delete-is-del", "\t\t(Delete sends DEL symbol instead of escape sequence)", NSS_ICONFIG_INPUT_DELETE_IS_DELETE},
    {"enable-autowrap", "\t(Initial autowrap setting)", NSS_ICONFIG_INIT_WRAP},
    {"enable-reverse-video", "\t(Initial reverse video setting)", NSS_ICONFIG_REVERSE_VIDEO},
    {"fkey-increment", "\t(Step in numbering function keys)", NSS_ICONFIG_INPUT_FKEY_INCREMENT},
    {"font", ", -f<value>\t(Comma-separated list of fontconfig font patterns)", NSS_SCONFIG_FONT_NAME},
    {"font-gamma", "\t\t(Factor of sharpenning\t(king of hack))",NSS_ICONFIG_GAMMA},
    {"font-size", "\t\t(Font size in points)",NSS_ICONFIG_FONT_SIZE},
    {"font-spacing", "\t\t(Additional spacing for individual symbols)", NSS_ICONFIG_FONT_SPACING},
    {"font-subpixel", "\t\t(Use subpixel rendering)",NSS_ICONFIG_SUBPIXEL_FONTS},
    {"force-dpi", "\t\t(DPI value for fonts)",NSS_ICONFIG_DPI},
    {"has-meta", "\t\t(Handle meta/alt)", NSS_ICONFIG_INPUT_HAS_META},
    {"horizontal-border", "\t(Top and bottom botders)",NSS_ICONFIG_TOP_BORDER},
    {"keyboard-dialect", "\t(National replacement character set to be used in non-UTF-8 mode)", NSS_ICONFIG_KEYBOARD_NRCS},
    {"keyboard-mapping", "\t(Initial keyboad mapping)", NSS_ICONFIG_INPUT_MAPPING},
    {"line-spacing", "\t\t(Additional lines vertical spacing)", NSS_ICONFIG_LINE_SPACING},
    {"lock-keyboard", "\t\t(Disable keyboad input)", NSS_ICONFIG_INPUT_LOCK},
    {"meta-sends-escape", "\t(Alt/Meta sends escape prefix instead of setting 8-th bit)", NSS_ICONFIG_INPUT_META_IS_ESC},
    {"modify-cursor", "\t\t(Enable encoding modifiers for cursor keys)", NSS_ICONFIG_INPUT_MODIFY_CURSOR},
    {"modify-function", "\t(Enable encoding modifiers for function keys)", NSS_ICONFIG_INPUT_MODIFY_FUNCTION},
    {"modify-keypad", "\t\t(Enable encoding modifiers keypad keys)", NSS_ICONFIG_INPUT_MODIFY_KEYPAD},
    {"modify-other", "\t\t(Enable encoding modifiers for other keys)", NSS_ICONFIG_INPUT_MODIFY_OTHER},
    {"modify-other-fmt", "\t(Format of encoding modifers)", NSS_ICONFIG_INPUT_MODIFY_OTHER_FMT},
    {"modkey-allow-edit-keypad", " (Allow modifing edit keypad keys)", NSS_ICONFIG_INPUT_MALLOW_EDIT},
    {"modkey-allow-function", "\t(Allow modifing function keys)", NSS_ICONFIG_INPUT_MALLOW_FUNCTION},
    {"modkey-allow-keypad", "\t(Allow modifing keypad keys)", NSS_ICONFIG_INPUT_MALLOW_KEYPAD},
    {"modkey-allow-misc", "\t(Allow modifing miscelleneous keys)", NSS_ICONFIG_INPUT_MALLOW_MISC},
    {"numlock", "\t\t(Initial numlock state)", NSS_ICONFIG_INPUT_NUMLOCK},
    {"override-boxdraw", "\t(Use built-in box drawing characters)", NSS_ICONFIG_OVERRIDE_BOXDRAW},
    {"printer", ", -o<value>\t(File where CSI MC-line commands output to)", NSS_SCONFIG_PRINTER},
    {"scroll-on-input", "\t(Scroll view to bottom on key press)", NSS_ICONFIG_SCROLL_ON_INPUT},
    {"scroll-on-output", "\t(Scroll view to bottom when character in printed)", NSS_ICONFIG_SCROLL_ON_OUTPUT},
    {"scrollback-size", ", -H<value> (Number of saved lines)", NSS_ICONFIG_HISTORY_LINES},
    {"shell", ", -s<value>\t(Shell to start in new instance)", NSS_SCONFIG_SHELL},
    {"tab-width", "\t\t(Initial width of tab character)", NSS_ICONFIG_TAB_WIDTH},
    {"term-name", ", -D<value>\t(TERM value)", NSS_SCONFIG_TERM_NAME},
    {"title", ", -T<value>, -t<value> (Initial window title)", NSS_SCONFIG_TITLE},
    {"underline-width", "\t(Text underline width)",NSS_ICONFIG_UNDERLINE_WIDTH},
    {"use-utf8", "\t\t(Enable uft-8 i/o)", NSS_ICONFIG_UTF8},
    {"vertical-border", "\t(Left and right borders)",NSS_ICONFIG_LEFT_BORDER},
    {"vt-version", ", -V<value>\t(Emulated VT version)", NSS_ICONFIG_VT_VERION},
    {"window-class", ", -c<value> (X11 Window class)", NSS_SCONFIG_TERM_CLASS},
};

static int optmap_cmp(const void *a, const void *b) {
    const char *a_name = ((const struct optmap_item *)a)->name;
    const char *b_name = ((const struct optmap_item *)b)->name;
    return strcmp(a_name, b_name);
}


static _Noreturn void usage(char *argv0, int code) {
    fprintf(stderr, "%s%s", argv0, " [-options] [-e] [command [args]]\n"
        "Where options are:\n"
            "\t--geometry=<value>, -g[=][<width>{xX}<height>][{+-}<xoffset>{+-}<yoffset>] (Window geometry)\n"
            "\t--help, -h\t\t\t(Print this message and exit)\n"
            "\t--version, -v\t\t\t(Print version and exit)\n");
    for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); i++)
        fprintf(stderr, "\t--%s=<value>%s\n", map[i].name, map[i].descr);
    nss_free_context();
    exit(code);
}

static _Noreturn void version(void) {
    fprintf(stderr, "Not So Simple Terminal v1.0.0\n");
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
        if (!argv[ind][1]) usage(argv[0], EXIT_FAILURE);
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

            struct optmap_item *res = bsearch(&(struct optmap_item){argv[ind] + 2},
                    map, sizeof(map)/sizeof(*map), sizeof(*map), optmap_cmp);
            if (res && arg)
                nss_config_set_string(res->opt, arg);
            else if (!strcmp(argv[ind] + 2, "geometry"))
                parse_geometry(arg, argv[0]);
            else if (!strcmp(argv[ind] + 2, "help"))
                usage(argv[0], EXIT_SUCCESS);
            else if (!strcmp(argv[ind] + 2, "version"))
                version();
            else if (strcmp(argv[ind] + 2, "no-config-file"))
                usage(argv[0], EXIT_FAILURE);
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

    // Load locale
    setlocale(LC_ALL, "");
    char *charset = nl_langinfo(CODESET);
    _Bool bset = charset && !strcmp(charset, "UTF-8");
    // Enable UTF-8 support if it is UTF-8
    nss_config_set_integer(NSS_ICONFIG_UTF8, bset);

	//Initialize graphical context
    for (char **opt = argv; *opt; opt++)
        if (!strcmp("--no-config-file", *opt))
            nss_config_set_integer(NSS_ICONFIG_SKIP_CONFIG_FILE, 1);
    nss_init_context();

    const char **res = (const char **)parse_options(argc, argv);
    if (res) nss_config_set_argv(res);

    nss_create_window(NULL, 0, NULL);
    nss_context_run();
    nss_free_context();
    return EXIT_SUCCESS;
}
