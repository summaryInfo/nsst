/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#ifndef ATTR_H_
#define ATTR_H_ 1

#include <stdint.h>

#include "input.h"
#include "term.h"

/* Configuration options
 *
 * They are initialized from X11 database
 * and then from command line flags
 *
 * *_MIN and *_MAX values are
 * not real options but boundaries
 * for internal use
 *
 * NSS_ICONFIG_* are accessed through nss_config_integer/nss_config_set_integer
 *      NSS_ICONFIG_INPUT are write only but can be read with nss_config_input_mode
 * 		nss_config_string can set NSS_ICONFIG_* by parsing string
 * NSS_SCONFIG_* are accessed through nss_config_string/nss_config_set_string
 * NSS_CCONFIG_* are accessed through nss_config_color/nss_config_set_color
 *      and also though nss_create_palette
 */
enum nss_config_opt {
    NSS_ICONFIG_MIN,
    NSS_ICONFIG_WINDOW_X = NSS_ICONFIG_MIN,
    NSS_ICONFIG_WINDOW_Y,
    NSS_ICONFIG_WINDOW_NEGATIVE_X,
    NSS_ICONFIG_WINDOW_NEGATIVE_Y,
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
    NSS_ICONFIG_LINE_SPACING,
    NSS_ICONFIG_GAMMA,
    NSS_ICONFIG_DPI,
    NSS_ICONFIG_SKIP_CONFIG_FILE,

    // These can't be read with nss_config_integer
    // Use nss_config_input_mode to get the copy of the whole structure
    NSS_ICONFIG_INPUT_MIN,
    NSS_ICONFIG_INPUT_APPCURSOR = NSS_ICONFIG_INPUT_MIN,
    NSS_ICONFIG_INPUT_APPKEY,
    NSS_ICONFIG_INPUT_BACKSPACE_IS_DELETE,
    NSS_ICONFIG_INPUT_DELETE_IS_DELETE,
    NSS_ICONFIG_INPUT_FKEY_INCREMENT,
    NSS_ICONFIG_INPUT_HAS_META,
    NSS_ICONFIG_INPUT_MAPPING,
    NSS_ICONFIG_INPUT_LOCK,
    NSS_ICONFIG_INPUT_META_IS_ESC,
    NSS_ICONFIG_INPUT_MODIFY_CURSOR,
    NSS_ICONFIG_INPUT_MODIFY_FUNCTION,
    NSS_ICONFIG_INPUT_MODIFY_KEYPAD,
    NSS_ICONFIG_INPUT_MODIFY_OTHER,
    NSS_ICONFIG_INPUT_MODIFY_OTHER_FMT,
    NSS_ICONFIG_INPUT_MALLOW_EDIT,
    NSS_ICONFIG_INPUT_MALLOW_FUNCTION,
    NSS_ICONFIG_INPUT_MALLOW_KEYPAD,
    NSS_ICONFIG_INPUT_MALLOW_MISC,
    NSS_ICONFIG_INPUT_NUMLOCK,
    NSS_ICONFIG_MAX,

    NSS_SCONFIG_MIN = NSS_ICONFIG_MAX,
    NSS_SCONFIG_FONT_NAME = NSS_SCONFIG_MIN,
    NSS_SCONFIG_ANSWERBACK_STRING,
    NSS_SCONFIG_SHELL,
    NSS_SCONFIG_TERM_NAME,
    NSS_SCONFIG_TERM_CLASS,
    NSS_SCONFIG_TITLE,
    NSS_SCONFIG_PRINTER,
    NSS_SCONFIG_MAX,

    NSS_CCONFIG_MIN = NSS_SCONFIG_MAX,
    NSS_CCONFIG_COLOR_0 = NSS_CCONFIG_MIN,
    //Here NSS_CCONFIG_COLOR_1 - NSS_CCONFIG_COLOR_{NSS_PALETTE_SIZE}
    //NSS_CONFIG_COLOR_{N} is NSS_CCONFIG_COLOR_0 + N
    NSS_CCONFIG_BG = NSS_CCONFIG_COLOR_0 + NSS_PALETTE_SIZE - NSS_SPECIAL_COLORS,
    NSS_CCONFIG_FG,
    NSS_CCONFIG_CURSOR_BG,
    NSS_CCONFIG_CURSOR_FG,
    NSS_CCONFIG_MAX,

    NSS_CONFIG_MAX = NSS_CCONFIG_MAX,
};

/* Allocates an array of NSS_PALETTE_SIZE colors with all configured values */
nss_color_t *nss_create_palette(void);

/* Getters for options */
nss_color_t nss_config_color(uint32_t opt);
int32_t nss_config_integer(uint32_t opt);
const char *nss_config_string(uint32_t opt);

/* Setters for options */
void nss_config_set_color(uint32_t opt, nss_color_t val);
void nss_config_set_integer(uint32_t opt, int32_t val);
void nss_config_set_string(uint32_t opt, const char *val);

nss_input_mode_t nss_config_input_mode(void);

/* Get argv from -e flag
 * Resets to NULL when called */
const char **nss_config_argv(void);

/* Get argv for -e flag */
void nss_config_set_argv(const char **argv);

#endif
