/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "input.h"
#include "util.h"
#include "window.h"
#include "nrcs.h"

#define CN_BASE 16
#define CN_EXT (6*6*6)
#define CN_GRAY (NSS_PALETTE_SIZE - CN_BASE - CN_EXT)
#define SD28B(x) ((x) ? 0x37 + 0x28 * (x) : 0)


static struct {
    int32_t val;
    int32_t dflt;
    int32_t min;
    int32_t max;
} ioptions[] = {
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
    [NSS_ICONFIG_KEYBOARD_NRCS] = {nss_94cs_ascii, nss_94cs_ascii, 0, nss_nrcs_MAX},
    [NSS_ICONFIG_SKIP_CONFIG_FILE] = {0, 0, 0, 1},
    [NSS_ICONFIG_ALLOW_NRCS] = {1, 1, 0, 1},
    [NSS_ICONFIG_OVERRIDE_BOXDRAW] = {0, 0, 0, 1},
};

static struct {
    const char *dflt;
    char *val;
} soptions[] = {
        [NSS_SCONFIG_FONT_NAME - NSS_SCONFIG_MIN] = { "mono" },
        [NSS_SCONFIG_ANSWERBACK_STRING - NSS_SCONFIG_MIN] = { "" },
        [NSS_SCONFIG_SHELL - NSS_SCONFIG_MIN] = { "/bin/sh" },
        [NSS_SCONFIG_TERM_NAME - NSS_SCONFIG_MIN] = { "xterm" },
        [NSS_SCONFIG_TITLE - NSS_SCONFIG_MIN] = { "Not So Simple Terminal" },
        [NSS_SCONFIG_PRINTER - NSS_SCONFIG_MIN] = { },
        [NSS_SCONFIG_TERM_CLASS - NSS_SCONFIG_MIN] = { },
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
