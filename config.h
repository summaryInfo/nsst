/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#ifndef ATTR_H_
#define ATTR_H_ 1

#include <stdint.h>
#include "input.h"
#include "term.h"

enum nss_config_opt {
    NSS_ICONFIG_WINDOW_X,
    NSS_ICONFIG_WINDOW_Y,
    NSS_ICONFIG_WINDOW_WIDTH,
    NSS_ICONFIG_WINDOW_HEIGHT,
    NSS_ICONFIG_HISTORY_LINES,
    NSS_ICONFIG_UTF8,
    NSS_ICONFIG_VT_VERION,
    NSS_ICONFIG_ALLOW_CHARSETS,
    NSS_ICONFIG_TAB_WIDTH,
    NSS_ICONFIG_INIT_WRAP,
    NSS_ICONFIG_SCROLL_ON_INPUT,
    NSS_ICONFIG_SCROLL_ON_OUTPUT,
    NSS_ICONFIG_CURSOR_SHAPE,
    NSS_ICONFIG_UNDERLINE_WIDTH,
    NSS_ICONFIG_CURSOR_WIDTH,
    NSS_ICONFIG_SUBPIXEL_FONTS,
    NSS_ICONFIG_REVERSE_VIDEO,
    NSS_ICONFIG_ALLOW_ALTSCREEN,
    NSS_ICONFIG_LEFT_BORDER,
    NSS_ICONFIG_TOP_BORDER,
    NSS_ICONFIG_BLINK_TIME,
    NSS_ICONFIG_FONT_SIZE,
    NSS_ICONFIG_FONT_SPACING,
    NSS_ICONFIG_GAMMA,
    NSS_ICONFIG_DPI,
    NSS_ICONFIG_MAX,

    NSS_SCONFIG_FONT_NAME,
    NSS_SCONFIG_ANSWERBACK_STRING,
    NSS_SCONFIG_SHELL,
    NSS_SCONFIG_TERM_NAME,
    NSS_SCONFIG_TERM_CLASS,
    NSS_SCONFIG_TITLE,
    NSS_SCONFIG_PRINTER,
    NSS_SCONFIG_MAX,

    NSS_CCONFIG_COLOR_0,
    NSS_CCONFIG_BG = NSS_CCONFIG_COLOR_0 + NSS_PALETTE_SIZE - NSS_SPECIAL_COLORS,
    NSS_CCONFIG_FG,
    NSS_CCONFIG_CURSOR_BG,
    NSS_CCONFIG_CURSOR_FG,
    NSS_CCONFIG_MAX,
};

nss_color_t *nss_create_palette(void);
nss_color_t nss_config_color(uint32_t opt);
int32_t nss_config_integer(uint32_t opt);
const char *nss_config_string(uint32_t opt);
void nss_config_set_color(uint32_t opt, nss_color_t val);
void nss_config_set_integer(uint32_t opt, int32_t val);
void nss_config_set_string(uint32_t opt, const char *val);
nss_input_mode_t nss_config_input_mode(void);
const char **nss_config_argv(void);
void nss_config_set_argv(const char **argv);
void nss_config_set_input_mode(nss_input_mode_t val);

#endif
