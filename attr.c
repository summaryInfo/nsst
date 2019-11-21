#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include "attr.h"
#include "util.h"
#include "window.h"
#include "input.h"

#define CN_BASE 16
#define CN_EXT (6*6*6)
#define CN_GRAY (NSS_PALETTE_SIZE - CN_BASE - CN_EXT)
#define SD28B(x) ((x) ? 0 : 0x37 + 0x28 * (x))


static struct {
    int32_t val;
    int32_t dflt;
    int32_t min;
    int32_t max;
} ioptions[] = {
    [NSS_ICONFIG_WINDOW_X] = {200, 200, -32768, 32767 },
    [NSS_ICONFIG_WINDOW_Y] = {200, 200, -32768, 32767 },
    [NSS_ICONFIG_WINDOW_WIDTH] = {800, 800, 1, 32767},
    [NSS_ICONFIG_WINDOW_HEIGHT] = {600, 600, 1, 32767},
    [NSS_ICONFIG_HISTORY_LINES] = {1024, 1024, -1, 100000},
    [NSS_ICONFIG_UTF8] = {1, 1, 0, 1},
    [NSS_ICONFIG_VT_VERION] = {420, 420, 0, 999},
    [NSS_ICONFIG_ALLOW_NRCS] = {1, 1, 0, 1},
    [NSS_ICONFIG_TAB_WIDTH] = {8, 8, 1, 100},
    [NSS_ICONFIG_INIT_WRAP] = {1, 1, 0, 1},
    [NSS_ICONFIG_SCROLL_ON_INPUT] = {1, 1, 0, 1},
    [NSS_ICONFIG_SCROLL_ON_OUTPUT] = {0, 0, 0, 1},
    [NSS_ICONFIG_CURSOR_SHAPE] = {nss_cursor_bar, nss_cursor_bar, 0, 6},
    [NSS_ICONFIG_UNDERLINE_WIDTH] = {1, 1, 0, 16},
    [NSS_ICONFIG_CURSOR_WIDTH] = {2, 2, 0, 16},
    [NSS_ICONFIG_SUBPIXEL_FONTS] = {0, 0, 0, 1},
    [NSS_ICONFIG_REVERSE_VIDEO] = {0, 0, 0, 1},
    [NSS_ICONFIG_ALLOW_ALTSCREEN] = {1, 1, 0, 1},
    [NSS_ICONFIG_LEFT_BORDER] = {8, 8, 0, 100},
    [NSS_ICONFIG_TOP_BORDER] = {8, 8, 0 , 100},
    [NSS_ICONFIG_BLINK_TIME] = {800000, 800000, 0, 10000000},
    [NSS_ICONFIG_FONT_SIZE] = {13, 13, 1, 200}
};

static struct {
    const char *dflt;
    char *val;
} soptions[] = {
        [NSS_SCONFIG_FONT_NAME - NSS_ICONFIG_MAX] = { "Iosevka-13,MaterialDesignIcons-13" },
        [NSS_SCONFIG_ANSWERBACK_STRING - NSS_ICONFIG_MAX] = { "" },
        [NSS_SCONFIG_SHELL - NSS_ICONFIG_MAX] = { "/bin/sh" },
        [NSS_SCONFIG_TERM_NAME - NSS_ICONFIG_MAX] = { "xterm" },
};

static nss_color_t coptions[NSS_PALETTE_SIZE];
static _Bool color_init;

int32_t nss_config_integer(uint32_t opt) {
    if (opt >= NSS_ICONFIG_MAX) {
        warn("Unknown config option");
        return 0;
    }
    return ioptions[opt].val;
}

void nss_config_set_integer(uint32_t opt, int32_t val) {
    if (opt >= NSS_ICONFIG_MAX) {
        warn("Unknown config option");
        return;
    }
    if (val > ioptions[opt].max) val = ioptions[opt].max;
    else if (val < ioptions[opt].min) val = ioptions[opt].min;
    ioptions[opt].val = val;
}

const char *nss_config_string(uint32_t opt) {
    if (opt <= NSS_ICONFIG_MAX || opt >= NSS_SCONFIG_MAX) {
        warn("Unknown config option");
        return NULL;
    }
    opt -= NSS_ICONFIG_MAX;
    return soptions[opt].val ? soptions[opt].val : soptions[opt].dflt;
}

void nss_config_set_string(uint32_t opt, const char *val) {
    if (opt <= NSS_ICONFIG_MAX || opt >= NSS_SCONFIG_MAX) {
        warn("Unknown config option");
        return;
    }
    opt -= NSS_ICONFIG_MAX;
    if(soptions[opt].val)
        free(soptions[opt].val);
    soptions[opt].val = strdup(val);
}

static nss_color_t color(uint32_t opt) {
    static nss_color_t base[CN_BASE] = {
            0xFF222222, 0xFFFF4433, 0xFFBBBB22, 0xFFFFBB22,
            0xFF88AA99, 0xFFDD8899, 0xFF88CC77, 0xFFDDCCAA,
            0xFF665555, 0xFFFF4433, 0xFFBBBB22, 0xFFFFBB22,
            0xFF88AA99, 0xFFDD8899, 0xFF88CC77, 0xFFFFFFCC,
    //        0xff000000, 0xff0000cd, 0xff00cd00, 0xff00cdcd,
    //        0xffee0000, 0xffcd00cd, 0xffcdcd00, 0xffe5e5e5,
    //        0xff7f7f7f, 0xff0000ff, 0xff00ff00, 0xff00ffff,
    //        0xffff5c5c, 0xffff00ff, 0xffffff00, 0xffffffff,
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
        return 0xFF000000 + SD28B(((opt - CN_BASE) / 36) % 6) +
                (SD28B(((opt - CN_BASE) / 36) % 6) << 8) + (SD28B(((opt - CN_BASE) / 36) % 6) << 16);
    } else if (opt < CN_GRAY + CN_EXT + CN_BASE) {
        uint8_t val = MIN(0x08 + 0x0A * (opt - CN_BASE - CN_EXT), 0xFF);
        return 0xFF000000 + val * 0x10101;
    }

    return base[0];
}

void nss_config_set_color(uint32_t opt, nss_color_t val) {
    if (!color_init) {
        for (size_t i = 0; i < NSS_PALETTE_SIZE; i++)
            coptions[i] = color(i + NSS_CCONFIG_COLOR_0);
        color_init = 1;
    }
    if (opt < NSS_CCONFIG_COLOR_0 || opt >= NSS_CCONFIG_MAX) {
        warn("Unknown option");
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
    if (opt < NSS_CCONFIG_COLOR_0 || opt >= NSS_CCONFIG_MAX) {
        warn("Unknown option");
        return 0;
    }
    nss_color_t val = coptions[opt - NSS_CCONFIG_COLOR_0];
    return val ? val : color(opt);
}

nss_color_t *nss_create_palette(void) {
    if (!color_init) {
        for (size_t i = 0; i < NSS_PALETTE_SIZE; i++)
            coptions[i] = color(i + NSS_CCONFIG_COLOR_0);
        color_init = 1;
    }
    nss_color_t *palette = malloc(NSS_PALETTE_SIZE * sizeof(nss_color_t));
    memcpy(palette, coptions, sizeof(coptions));
    return palette;
}

static nss_input_mode_t input_mode = {
    .modkey_fn = 3,
    .modkey_cursor = 3,
    .modkey_keypad = 3,
    .modkey_other = 1,
    .modkey_other_fmt = 0,
    .modkey_legacy_allow_keypad = 0,
    .modkey_legacy_allow_edit_keypad = 0,
    .modkey_legacy_allow_function = 0,
    .modkey_legacy_allow_misc = 0,
    .appkey = 0,
    .appcursor = 0,
    .numlock = 1,
    .keylock = 0,
    .has_meta = 1,
    .meta_escape = 1,
    .backspace_is_del = 1,
    .delete_is_del = 0,
    .fkey_inc_step = 10,
    .keyboad_vt52 = 0,
    .keyboard_mapping = nss_km_default
};

nss_input_mode_t nss_config_input_mode(void) {
    return input_mode;
}
void nss_config_set_input_mode(nss_input_mode_t mode) {
    input_mode = mode;
}
