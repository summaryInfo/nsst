/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#include "feature.h"

#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "input.h"
#include "nrcs.h"
#include "util.h"
#include "window.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CN_BASE 16
#define CN_EXT (6*6*6)
#define CN_GRAY (NSS_PALETTE_SIZE - CN_BASE - CN_EXT)
#define SD28B(x) ((x) ? 0x37 + 0x28 * (x) : 0)

nss_optmap_item_t optmap[OPT_MAP_SIZE] = {
    {"allow-alternate", "\t(Enable alternate screen)", "allowAlternate", NSS_ICONFIG_ALLOW_ALTSCREEN},
    {"allow-charsets", "\t(Enable charsets support)", "allowCharsets", NSS_ICONFIG_ALLOW_CHARSETS},
    {"allow-nrcs", "\t\t(Enable NRCSs support)", "allowNrcs", NSS_ICONFIG_ALLOW_NRCS},
    {"allow-window-ops", "\t(Allow window manipulation with escape sequences)", "allowWindowOps", NSS_ICONFIG_ALLOW_WINDOW_OPS},
    {"alpha", "\t\t\t(Backround opacity, requires compositor to be running)", "alpha", NSS_ICONFIG_ALPHA},
    {"alternate-scroll", "\t(Scrolling sends arrow keys escapes in alternate screen)", "alternateScroll", NSS_ICONFIG_ALTERNATE_SCROLL},
    {"answerback-string", "\t(ENQ report)", "answerbackString", NSS_SCONFIG_ANSWERBACK_STRING},
    {"appcursor", "\t\t(Initial application cursor mode value)", "appcursor", NSS_ICONFIG_INPUT_APPCURSOR},
    {"appkey", "\t\t(Initial application keypad mode value)", "appkey", NSS_ICONFIG_INPUT_APPKEY},
    {"background", "\t\t(Default backround color)", "background", NSS_CCONFIG_BG},
    {"backspace-is-delete", "\t(Backspace sends DEL instead of BS)", "backspaceIsDelete", NSS_ICONFIG_INPUT_BACKSPACE_IS_DELETE},
    {"blink-time", "\t\t(Text blink interval in microseconds)","blinkTime", NSS_ICONFIG_BLINK_TIME},
    {"cursor-background", "\t(Default cursor backround color)", "cursorBackground", NSS_CCONFIG_CURSOR_BG},
    {"cursor-foreground", "\t(Default cursor foreround color)", "cursorForeground", NSS_CCONFIG_CURSOR_FG},
    {"cursor-shape", "\t\t(Shape of cursor)", "cursorShape", NSS_ICONFIG_CURSOR_SHAPE},
    {"cursor-width", "\t\t(Width of lines that forms cursor)", "cursorWidth", NSS_ICONFIG_CURSOR_WIDTH},
    {"delete-is-del", "\t\t(Delete sends DEL symbol instead of escape sequence)", "deleteIsDelete", NSS_ICONFIG_INPUT_DELETE_IS_DELETE},
    {"double-click-time", "\t(Time gap in milliseconds in witch two mouse presses will be considered double)", "doubleClickTime", NSS_ICONFIG_DOUBLE_CLICK_TIME},
    {"enable-autowrap", "\t(Initial autowrap setting)", "enableAutowrap", NSS_ICONFIG_INIT_WRAP},
    {"enable-reverse-video", "\t(Initial reverse video setting)", "enableReverseVideo", NSS_ICONFIG_REVERSE_VIDEO},
    {"fkey-increment", "\t(Step in numbering function keys)", "fkeyIncrement", NSS_ICONFIG_INPUT_FKEY_INCREMENT},
    {"font", ", -f<value>\t(Comma-separated list of fontconfig font patterns)", "font", NSS_SCONFIG_FONT_NAME},
    {"font-gamma", "\t\t(Factor of sharpenning\t(king of hack))", "fontGamma",NSS_ICONFIG_GAMMA},
    {"font-size", "\t\t(Font size in points)", "fontSize", NSS_ICONFIG_FONT_SIZE},
    {"font-size-step", "\t(Font size step in points)", "fontSizeStep", NSS_ICONFIG_FONT_SIZE_STEP},
    {"font-spacing", "\t\t(Additional spacing for individual symbols)", "fontSpacing", NSS_ICONFIG_FONT_SPACING},
    {"font-subpixel", "\t\t(Use subpixel rendering)", "fontSubpixel", NSS_ICONFIG_SUBPIXEL_FONTS},
    {"force-dpi", "\t\t(DPI value for fonts)", "dpi", NSS_ICONFIG_DPI},
    {"foreground", "\t\t(Default foreground color)", "foreground", NSS_CCONFIG_FG},
    {"fps", "\t\t\t(Window refresh rate)", "fps", NSS_ICONFIG_FPS},
    {"has-meta", "\t\t(Handle meta/alt)", "hasMeta", NSS_ICONFIG_INPUT_HAS_META},
    {"horizontal-border", "\t(Top and bottom botders)", "horizontalBorder", NSS_ICONFIG_TOP_BORDER},
    {"keep-clipboard", "\t(Reuse copied clipboard content instead of current selection data)", "keepClipboard", NSS_ICONFIG_KEEP_CLIPBOARD},
    {"keep-selection", "\t(Don't clear X11 selection when unhighlighted)", "keepSelection", NSS_ICONFIG_KEEP_SELECTION},
    {"keyboard-dialect", "\t(National replacement character set to be used in non-UTF-8 mode)", "keyboardDialect", NSS_ICONFIG_KEYBOARD_NRCS},
    {"keyboard-mapping", "\t(Initial keyboad mapping)", "keyboardMapping", NSS_ICONFIG_INPUT_MAPPING},
    {"line-spacing", "\t\t(Additional lines vertical spacing)", "lineSpacing", NSS_ICONFIG_LINE_SPACING},
    {"lock-keyboard", "\t\t(Disable keyboad input)", "lockKeyboard", NSS_ICONFIG_INPUT_LOCK},
    {"log-level","\t\t(Filering level of logged information)", "logLevel", NSS_ICONFIG_LOG_LEVEL},
    {"meta-sends-escape", "\t(Alt/Meta sends escape prefix instead of setting 8-th bit)", "metaSendsEscape", NSS_ICONFIG_INPUT_META_IS_ESC},
    {"modify-cursor", "\t\t(Enable encoding modifiers for cursor keys)", "modifyCursor", NSS_ICONFIG_INPUT_MODIFY_CURSOR},
    {"modify-function", "\t(Enable encoding modifiers for function keys)", "modifyFunction", NSS_ICONFIG_INPUT_MODIFY_FUNCTION},
    {"modify-keypad", "\t\t(Enable encoding modifiers keypad keys)", "modifyKeypad", NSS_ICONFIG_INPUT_MODIFY_KEYPAD},
    {"modify-other", "\t\t(Enable encoding modifiers for other keys)", "modifyOther", NSS_ICONFIG_INPUT_MODIFY_OTHER},
    {"modify-other-fmt", "\t(Format of encoding modifers)", "modifyOtherFmt", NSS_ICONFIG_INPUT_MODIFY_OTHER_FMT},
    {"modkey-allow-edit-keypad", " (Allow modifing edit keypad keys)", "modkeyAllowEditKeypad", NSS_ICONFIG_INPUT_MALLOW_EDIT},
    {"modkey-allow-function", "\t(Allow modifing function keys)", "modkeyAllowFunction", NSS_ICONFIG_INPUT_MALLOW_FUNCTION},
    {"modkey-allow-keypad", "\t(Allow modifing keypad keys)", "modkeyAllowKeypad", NSS_ICONFIG_INPUT_MALLOW_KEYPAD},
    {"modkey-allow-misc", "\t(Allow modifing miscelleneous keys)", "modkeyAllowMisc", NSS_ICONFIG_INPUT_MALLOW_MISC},
    {"numlock", "\t\t(Initial numlock state)", "numlock", NSS_ICONFIG_INPUT_NUMLOCK},
#if USE_BOXDRAWING
    {"override-boxdrawing", "\t(Use built-in box drawing characters)", "overrideBoxdrawing", NSS_ICONFIG_OVERRIDE_BOXDRAW},
#endif
    {"printer", ", -o<value>\t(File where CSI MC-line commands output to)", "printer", NSS_SCONFIG_PRINTER},
    {"resize-delay", "\t\t(Additional delay after resize in microseconds)", "resizeDelay", NSS_ICONFIG_RESIZE_DELAY},
    {"scroll-amount", "\t\t(Number of lines scrolled in a time)", "scrollAmout", NSS_ICONFIG_SCROLL_AMOUNT},
    {"scroll-delay", "\t\t(Additional delay after scroll in microseconds)", "scrollDelay", NSS_ICONFIG_SCROLL_DELAY},
    {"scroll-on-input", "\t(Scroll view to bottom on key press)", "scrollOnInput", NSS_ICONFIG_SCROLL_ON_INPUT},
    {"scroll-on-output", "\t(Scroll view to bottom when character in printed)", "scrollOnOutput", NSS_ICONFIG_SCROLL_ON_OUTPUT},
    {"scrollback-size", ", -H<value> (Number of saved lines)", "scrollbackSize", NSS_ICONFIG_HISTORY_LINES},
    {"select-to-clipboard", "\t(Use CLIPBOARD selection to store hightlighted data)", "selectToClipboard", NSS_ICONFIG_SELECT_TO_CLIPBOARD},
    {"shell", ", -s<value>\t(Shell to start in new instance)", "shell", NSS_SCONFIG_SHELL},
    {"tab-width", "\t\t(Initial width of tab character)", "tabWidth", NSS_ICONFIG_TAB_WIDTH},
    {"term-name", ", -D<value>\t(TERM value)", "termName", NSS_SCONFIG_TERM_NAME},
    {"title", ", -T<value>, -t<value> (Initial window title)", "title", NSS_SCONFIG_TITLE},
    {"triple-click-time", "\t(Time gap in milliseconds in witch tree mouse presses will be considered triple)", "trippleClickTime", NSS_ICONFIG_TRIPLE_CLICK_TIME},
    {"underline-width", "\t(Text underline width)", "underlineWidth", NSS_ICONFIG_UNDERLINE_WIDTH},
    {"use-utf8", "\t\t(Enable uft-8 i/o)", "useUTF8", NSS_ICONFIG_UTF8},
    {"vertical-border", "\t(Left and right borders)", "verticalBorder", NSS_ICONFIG_LEFT_BORDER},
    {"vt-version", ", -V<value>\t(Emulated VT version)", "vtVersion", NSS_ICONFIG_VT_VERION},
    {"window-class", ", -c<value> (X11 Window class)", "windowClass", NSS_SCONFIG_TERM_CLASS},
    {"word-break", "\t(Symbols treated as word separators when snapping mouse selection)", "wordBreak", NSS_SCONFIG_WORD_SEPARATORS},
};

static struct {
    int32_t val;
    int32_t dflt;
    int32_t min;
    int32_t max;
} ioptions[] = {
    [NSS_ICONFIG_LOG_LEVEL - NSS_ICONFIG_MIN] = {2, 2, 0, 4},
    [NSS_ICONFIG_WINDOW_X - NSS_ICONFIG_MIN] = {200, 200, -32768, 32767 },
    [NSS_ICONFIG_WINDOW_Y - NSS_ICONFIG_MIN] = {200, 200, -32768, 32767 },
    [NSS_ICONFIG_WINDOW_NEGATIVE_X - NSS_ICONFIG_MIN] = {0, 0, 0, 1},
    [NSS_ICONFIG_WINDOW_NEGATIVE_Y - NSS_ICONFIG_MIN] = {0, 0, 0, 1},
    [NSS_ICONFIG_WINDOW_WIDTH - NSS_ICONFIG_MIN] = {800, 800, 1, 32767},
    [NSS_ICONFIG_WINDOW_HEIGHT - NSS_ICONFIG_MIN] = {600, 600, 1, 32767},
    [NSS_ICONFIG_HISTORY_LINES - NSS_ICONFIG_MIN] = {1024, 1024, -1, 100000},
    [NSS_ICONFIG_UTF8 - NSS_ICONFIG_MIN] = {1, 1, 0, 1},
    [NSS_ICONFIG_VT_VERION - NSS_ICONFIG_MIN] = {320, 320, 0, 999},
    [NSS_ICONFIG_ALLOW_CHARSETS - NSS_ICONFIG_MIN] = {1, 1, 0, 1},
    [NSS_ICONFIG_TAB_WIDTH - NSS_ICONFIG_MIN] = {8, 8, 1, 100},
    [NSS_ICONFIG_INIT_WRAP - NSS_ICONFIG_MIN] = {1, 1, 0, 1},
    [NSS_ICONFIG_SCROLL_ON_INPUT - NSS_ICONFIG_MIN] = {1, 1, 0, 1},
    [NSS_ICONFIG_SCROLL_ON_OUTPUT - NSS_ICONFIG_MIN] = {0, 0, 0, 1},
    [NSS_ICONFIG_CURSOR_SHAPE - NSS_ICONFIG_MIN] = {nss_cursor_bar, nss_cursor_bar, 1, 6},
    [NSS_ICONFIG_UNDERLINE_WIDTH - NSS_ICONFIG_MIN] = {1, 1, 0, 16},
    [NSS_ICONFIG_CURSOR_WIDTH - NSS_ICONFIG_MIN] = {2, 2, 0, 16},
    [NSS_ICONFIG_SUBPIXEL_FONTS - NSS_ICONFIG_MIN] = {0, 0, 0, 1},
    [NSS_ICONFIG_REVERSE_VIDEO - NSS_ICONFIG_MIN] = {0, 0, 0, 1},
    [NSS_ICONFIG_ALLOW_ALTSCREEN - NSS_ICONFIG_MIN] = {1, 1, 0, 1},
    [NSS_ICONFIG_LEFT_BORDER - NSS_ICONFIG_MIN] = {8, 8, 0, 100},
    [NSS_ICONFIG_TOP_BORDER - NSS_ICONFIG_MIN] = {8, 8, 0 , 100},
    [NSS_ICONFIG_BLINK_TIME - NSS_ICONFIG_MIN] = {800000, 800000, 0, 10000000},
    [NSS_ICONFIG_FONT_SIZE - NSS_ICONFIG_MIN] = {0, 0, 1, 200},
    [NSS_ICONFIG_FONT_SPACING - NSS_ICONFIG_MIN] = {0, 0, -100, 100},
    [NSS_ICONFIG_LINE_SPACING - NSS_ICONFIG_MIN] = {0, 0, -100, 100},
    [NSS_ICONFIG_GAMMA - NSS_ICONFIG_MIN] = {10000, 10000, 2000, 200000},
    [NSS_ICONFIG_DPI - NSS_ICONFIG_MIN] = {96, 96, 10, 10000},
    [NSS_ICONFIG_KEYBOARD_NRCS - NSS_ICONFIG_MIN] = {nss_94cs_ascii, nss_94cs_ascii, 0, nss_nrcs_MAX},
    [NSS_ICONFIG_SKIP_CONFIG_FILE - NSS_ICONFIG_MIN] = {0, 0, 0, 1},
    [NSS_ICONFIG_ALLOW_NRCS - NSS_ICONFIG_MIN] = {1, 1, 0, 1},
    [NSS_ICONFIG_ALLOW_WINDOW_OPS - NSS_ICONFIG_MIN] = {1, 1, 0, 1},
#if USE_BOXDRAWING
    [NSS_ICONFIG_OVERRIDE_BOXDRAW - NSS_ICONFIG_MIN] = {0, 0, 0, 1},
#endif
    [NSS_ICONFIG_FPS - NSS_ICONFIG_MIN] = {60, 60, 1, 1000},
    [NSS_ICONFIG_SCROLL_DELAY - NSS_ICONFIG_MIN] = {SEC/180000, SEC/180000, 0, 10*SEC/1000},
    [NSS_ICONFIG_RESIZE_DELAY - NSS_ICONFIG_MIN] = {SEC/60000, SEC/60000, 0, 10*SEC/1000},
    [NSS_ICONFIG_SCROLL_AMOUNT - NSS_ICONFIG_MIN] = {2, 2, 1, 100},
    [NSS_ICONFIG_FONT_SIZE_STEP - NSS_ICONFIG_MIN] = {1, 1, 1, 250},
    [NSS_ICONFIG_ALTERNATE_SCROLL - NSS_ICONFIG_MIN] = {0, 0, 0, 1},
    [NSS_ICONFIG_DOUBLE_CLICK_TIME - NSS_ICONFIG_MIN] = {300, 300, 10, 1000000},
    [NSS_ICONFIG_TRIPLE_CLICK_TIME - NSS_ICONFIG_MIN] = {600, 600, 10, 1000000},
    [NSS_ICONFIG_KEEP_CLIPBOARD - NSS_ICONFIG_MIN] = {0, 0, 0, 1},
    [NSS_ICONFIG_KEEP_SELECTION - NSS_ICONFIG_MIN] = {0, 0, 0, 1},
    [NSS_ICONFIG_SELECT_TO_CLIPBOARD - NSS_ICONFIG_MIN] = {0, 0, 0, 1},
};

static struct {
    const char *dflt;
    char *val;
} soptions[] = {
        [NSS_SCONFIG_FONT_NAME - NSS_SCONFIG_MIN] = { "mono", NULL },
        [NSS_SCONFIG_ANSWERBACK_STRING - NSS_SCONFIG_MIN] = { "", NULL },
        [NSS_SCONFIG_SHELL - NSS_SCONFIG_MIN] = { "/bin/sh", NULL },
        [NSS_SCONFIG_TERM_NAME - NSS_SCONFIG_MIN] = { "xterm", NULL },
        [NSS_SCONFIG_TITLE - NSS_SCONFIG_MIN] = { "Not So Simple Terminal", NULL },
        [NSS_SCONFIG_PRINTER - NSS_SCONFIG_MIN] = { NULL, NULL },
        [NSS_SCONFIG_TERM_CLASS - NSS_SCONFIG_MIN] = { NULL, NULL },
        [NSS_SCONFIG_WORD_SEPARATORS - NSS_SCONFIG_MIN] = { " \t!#$%^&*()_+-={}[]\\\"'|/?,.<>~`", NULL },
};

static nss_input_mode_t input_mode = {
    .modkey_fn = 3,
    .modkey_cursor = 3,
    .modkey_keypad = 3,
    .modkey_other = 0,
    .modkey_other_fmt = 0,
    .modkey_legacy_allow_keypad = 0,
    .modkey_legacy_allow_edit_keypad = 0,
    .modkey_legacy_allow_function = 0,
    .modkey_legacy_allow_misc = 0,
    .appkey = 0,
    .appcursor = 0,
    .allow_numlock = 1,
    .keylock = 0,
    .has_meta = 1,
    .meta_escape = 1,
    .backspace_is_del = 1,
    .delete_is_del = 0,
    .fkey_inc_step = 10,
    .keyboad_vt52 = 0,
    .keyboard_mapping = nss_km_default
};

static nss_color_t coptions[NSS_PALETTE_SIZE];
static _Bool color_init;
static const char **argv = NULL;


/* Internal function, that calculates default palette
 *
 * base[CN_BASE] is default first 16 colors
 * next 6x6x6 colors are RGB cube
 * last 24 are gray scale
 *
 * default background and cursor background is color 0
 * default foreground and cursor foreground is color 15
 */
static nss_color_t color(uint32_t opt) {
    static nss_color_t base[CN_BASE] = {
    // That's gruvbox colors
            0xFF222222, 0xFFFF4433, 0xFFBBBB22, 0xFFFFBB22,
            0xFF88AA99, 0xFFDD8899, 0xFF88CC77, 0xFFDDCCAA,
            0xFF665555, 0xFFFF4433, 0xFFBBBB22, 0xFFFFBB22,
            0xFF88AA99, 0xFFDD8899, 0xFF88CC77, 0xFFFFFFCC,
    // Replace it with
    //        0xff000000, 0xff0000cd, 0xff00cd00, 0xff00cdcd,
    //        0xffee0000, 0xffcd00cd, 0xffcdcd00, 0xffe5e5e5,
    //        0xff7f7f7f, 0xff0000ff, 0xff00ff00, 0xff00ffff,
    //        0xffff5c5c, 0xffff00ff, 0xffffff00, 0xffffffff,
    // to get default xterm colors
    };

    switch(opt) {
    case NSS_CCONFIG_BG:
    case NSS_CCONFIG_CURSOR_BG:
        return base[0];
    case NSS_CCONFIG_FG:
    case NSS_CCONFIG_CURSOR_FG:
        return base[15];
    }

    opt -= NSS_CCONFIG_COLOR_0;

    if (opt < CN_BASE) return base[opt];
    else if (opt < CN_EXT + CN_BASE) {
        return 0xFF000000 | SD28B(((opt - CN_BASE) / 1) % 6) |
            (SD28B(((opt - CN_BASE) / 6) % 6) << 8) | (SD28B(((opt - CN_BASE) / 36) % 6) << 16);
    } else if (opt < CN_GRAY + CN_EXT + CN_BASE) {
        uint8_t val = MIN(0x08 + 0x0A * (opt - CN_BASE - CN_EXT), 0xFF);
        return 0xFF000000 + val * 0x10101;
    }

    return base[0];
}

int32_t nss_config_integer(uint32_t opt) {
    if (opt >= NSS_ICONFIG_INPUT_MIN) {
        warn("Unknown integer config option %d", opt);
        return 0;
    }
    return ioptions[opt].val;
}

void nss_config_set_integer(uint32_t opt, int32_t val) {
    if (opt < NSS_ICONFIG_INPUT_MIN) {
        if (val > ioptions[opt].max) val = ioptions[opt].max;
        else if (val < ioptions[opt].min) val = ioptions[opt].min;
        if(opt == NSS_ICONFIG_CURSOR_SHAPE)
            val = (val + 1) & ~1;
        ioptions[opt].val = val;
    } else if (opt < NSS_ICONFIG_MAX) {
        switch(opt) {
        case NSS_ICONFIG_INPUT_APPCURSOR: input_mode.appcursor = !!val; break;
        case NSS_ICONFIG_INPUT_APPKEY: input_mode.appkey = !!val; break;
        case NSS_ICONFIG_INPUT_BACKSPACE_IS_DELETE: input_mode.backspace_is_del = !!val; break;
        case NSS_ICONFIG_INPUT_DELETE_IS_DELETE: input_mode.delete_is_del = !!val; break;
        case NSS_ICONFIG_INPUT_FKEY_INCREMENT: input_mode.fkey_inc_step = val; break;
        case NSS_ICONFIG_INPUT_HAS_META: input_mode.has_meta = !!val; break;
        case NSS_ICONFIG_INPUT_MAPPING: input_mode.keyboard_mapping = val < nss_km_MAX ? val : nss_km_default; break;
        case NSS_ICONFIG_INPUT_LOCK: input_mode.keylock = !!val; break;
        case NSS_ICONFIG_INPUT_META_IS_ESC: input_mode.meta_escape = !!val; break;
        case NSS_ICONFIG_INPUT_MODIFY_CURSOR: input_mode.modkey_cursor = MIN(val, 4); break;
        case NSS_ICONFIG_INPUT_MODIFY_FUNCTION: input_mode.modkey_fn = MIN(val, 4); break;
        case NSS_ICONFIG_INPUT_MODIFY_KEYPAD: input_mode.modkey_keypad = MIN(val, 4); break;
        case NSS_ICONFIG_INPUT_MODIFY_OTHER: input_mode.modkey_other = MIN(val, 4); break;
        case NSS_ICONFIG_INPUT_MODIFY_OTHER_FMT: input_mode.modkey_other_fmt = !!val; break;
        case NSS_ICONFIG_INPUT_MALLOW_EDIT: input_mode.modkey_legacy_allow_edit_keypad = !!val; break;
        case NSS_ICONFIG_INPUT_MALLOW_FUNCTION: input_mode.modkey_legacy_allow_function = !!val; break;
        case NSS_ICONFIG_INPUT_MALLOW_KEYPAD: input_mode.modkey_legacy_allow_keypad = !!val; break;
        case NSS_ICONFIG_INPUT_MALLOW_MISC: input_mode.modkey_legacy_allow_misc = !!val; break;
        case NSS_ICONFIG_INPUT_NUMLOCK: input_mode.allow_numlock = !!val; break;
        case NSS_ICONFIG_ALPHA: {
            nss_color_t bg = nss_config_color(NSS_CCONFIG_BG);
            nss_config_set_color(NSS_CCONFIG_BG, (bg & 0xFFFFFF) | (MAX(0, MIN(val, 255)) << 24));
            break;
        }
        }
    } else {
        warn("Unknown integer option %d", opt);
    }
}

const char *nss_config_string(uint32_t opt) {
    if (NSS_SCONFIG_MIN > opt || opt >= NSS_SCONFIG_MAX) {
        warn("Unknown string option %d", opt);
        return NULL;
    }
    opt -= NSS_SCONFIG_MIN;
    return soptions[opt].val ? soptions[opt].val : soptions[opt].dflt;
}

void nss_config_set_string(uint32_t opt, const char *val) {
    if (NSS_SCONFIG_MIN <= opt && opt < NSS_SCONFIG_MAX) {
        opt -= NSS_SCONFIG_MIN;
        if(soptions[opt].val)
            free(soptions[opt].val);
        soptions[opt].val = strdup(val);
    } else if (opt < NSS_ICONFIG_MAX) {
        int32_t ival = 0;
        if (sscanf(val, "%"SCNd32, &ival) == 1)
            nss_config_set_integer(opt, ival);
        else
            warn("Unknown string option %d", opt);
    } else if (NSS_CCONFIG_MIN <= opt && opt < NSS_CCONFIG_MAX) {
            nss_color_t col = parse_color((uint8_t*)val, (uint8_t*)val + strlen(val));
            if (col) {
                nss_color_t old = nss_config_color(opt);
                nss_config_set_color(opt, (col & 0xFFFFFF) | (old & 0xFF000000));
            } else warn("Wrong color format: '%s'", val);
    } else {
        warn("Unknown string option %d", opt);
        return;
    }
}


void nss_config_set_color(uint32_t opt, nss_color_t val) {
    if (!color_init) {
        for (size_t i = 0; i < NSS_PALETTE_SIZE; i++)
            coptions[i] = color(i + NSS_CCONFIG_COLOR_0);
        color_init = 1;
    }
    if (opt < NSS_CCONFIG_MIN || opt >= NSS_CCONFIG_MAX) {
        warn("Unknown color option");
        return;
    }
    coptions[opt - NSS_CCONFIG_COLOR_0] = val ? val : color(opt);
}

nss_color_t nss_config_color(uint32_t opt) {
    if (!color_init) {
        for (size_t i = 0; i < NSS_PALETTE_SIZE; i++)
            coptions[i] = color(i + NSS_CCONFIG_COLOR_0);
        color_init = 1;
    }
    if (NSS_CCONFIG_MIN > opt || opt >= NSS_CCONFIG_MAX) {
        warn("Unknown option");
        return 0;
    }
    nss_color_t val = coptions[opt - NSS_CCONFIG_COLOR_0];
    return val ? val : color(opt);
}

nss_input_mode_t nss_config_input_mode(void) {
    return input_mode;
}

const char **nss_config_argv(void) {
    const char **res = argv;
    argv = NULL;
    return res;
}

void nss_config_set_argv(const char **val) {
    argv = val;
}
