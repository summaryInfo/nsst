/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#ifndef CONFIG_H_
#define CONFIG_H_ 1

#include <stdbool.h>
#include <stdint.h>

#include "feature.h"
#include "input.h"
#include "line.h"

/* Configuration options
 *
 * They are initialized from X11 database
 * and then from command line flags
 *
 * *_MIN and *_MAX values are
 * not real options but boundaries
 * for internal use
 *
 * ICONF_* are accessed through iconf()/iconf_set()
 *      ICONF_ALPHA
 *      sconf() can set all options by parsing string
 * SCONF_* are accessed through sconf()/sconf_set()
 * CCONF_* are accessed through cconf()/cconf_set()
 */
enum config_option {
    ICONF_MIN,
    ICONF_LOG_LEVEL = ICONF_MIN,
    ICONF_WINDOW_X,
    ICONF_WINDOW_Y,
    ICONF_WINDOW_NEGATIVE_X,
    ICONF_WINDOW_NEGATIVE_Y,
    ICONF_WINDOW_WIDTH,
    ICONF_WINDOW_HEIGHT,
    ICONF_FIXED_SIZE,
    ICONF_HAS_GEOMETRY,
    ICONF_HISTORY_LINES,
    ICONF_UTF8,
    ICONF_VT_VERION,
    ICONF_FORCE_UTF8_NRCS,
    ICONF_TAB_WIDTH,
    ICONF_INIT_WRAP,
    ICONF_SCROLL_ON_INPUT,
    ICONF_SCROLL_ON_OUTPUT,
    ICONF_CURSOR_SHAPE,
    ICONF_UNDERLINE_WIDTH,
    ICONF_CURSOR_WIDTH,
    ICONF_PIXEL_MODE,
    ICONF_REVERSE_VIDEO,
    ICONF_ALLOW_ALTSCREEN,
    ICONF_LEFT_BORDER,
    ICONF_TOP_BORDER,
    ICONF_BLINK_TIME,
    ICONF_FONT_SIZE,
    ICONF_FONT_SPACING,
    ICONF_LINE_SPACING,
    ICONF_GAMMA,
    ICONF_DPI,
    ICONF_KEYBOARD_NRCS,
    ICONF_ALLOW_NRCS,
    ICONF_ALLOW_WINDOW_OPS,
#if USE_BOXDRAWING
    ICONF_OVERRIDE_BOXDRAW,
#endif
    ICONF_FPS,
    ICONF_SCROLL_DELAY,
    ICONF_RESIZE_DELAY,
    ICONF_SYNC_TIME,
    ICONF_SCROLL_AMOUNT,
    ICONF_FONT_SIZE_STEP,
    ICONF_DOUBLE_CLICK_TIME,
    ICONF_TRIPLE_CLICK_TIME,
    ICONF_KEEP_CLIPBOARD,
    ICONF_KEEP_SELECTION,
    ICONF_SELECT_TO_CLIPBOARD,
    ICONF_ALLOW_BLINKING,
    ICONF_EXTENDED_CIR,
    ICONF_SPEICAL_BOLD,
    ICONF_SPEICAL_UNDERLINE,
    ICONF_SPEICAL_BLINK,
    ICONF_SPEICAL_REVERSE,
    ICONF_SPEICAL_ITALIC,
    ICONF_BELL_VOLUME,
    ICONF_MARGIN_BELL_VOLUME,
    ICONF_MARGIN_BELL_COLUMN,
    ICONF_VISUAL_BELL,
    ICONF_RAISE_ON_BELL,
    ICONF_URGENT_ON_BELL,
    ICONF_BELL_LOW_VOLUME,
    ICONF_MARGIN_BELL_LOW_VOLUME,
    ICONF_BELL_HIGH_VOLUME,
    ICONF_MARGIN_BELL_HIGH_VOLUME,
    ICONF_VISUAL_BELL_TIME,
    ICONF_ALLOW_ERASE_SCROLLBACK,
    ICONF_TRACE_FONTS,
    ICONF_TRACE_CONTROLS,
    ICONF_TRACE_CHARACTERS,
    ICONF_TRACE_EVENTS,
    ICONF_TRACE_INPUT,
    ICONF_TRACE_MISC,
    ICONF_ALTERNATE_SCROLL,
    ICONF_APPCURSOR,
    ICONF_APPKEY,
    ICONF_BACKSPACE_IS_DELETE,
    ICONF_DELETE_IS_DELETE,
    ICONF_FKEY_INCREMENT,
    ICONF_HAS_META,
    ICONF_MAPPING,
    ICONF_LOCK,
    ICONF_META_IS_ESC,
    ICONF_MODIFY_CURSOR,
    ICONF_MODIFY_FUNCTION,
    ICONF_MODIFY_KEYPAD,
    ICONF_MODIFY_OTHER,
    ICONF_MODIFY_OTHER_FMT,
    ICONF_MALLOW_EDIT,
    ICONF_MALLOW_FUNCTION,
    ICONF_MALLOW_KEYPAD,
    ICONF_MALLOW_MISC,
    ICONF_NUMLOCK,
    ICONF_REWRAP,
    ICONF_CUT_LINES,
    ICONF_MINIMIZE_SCROLLBACK,
    ICONF_PRINT_ATTR,
    ICONF_ALLOW_SUBST_FONTS,
    ICONF_FORCE_SCALABLE,
    ICONF_ALPHA,
    ICONF_BLEND_ALL_BG,
    ICONF_BLEND_FG,
    ICONF_SELECT_SCROLL_TIME,
    ICONF_MAX,

    SCONF_MIN = ICONF_MAX,
    SCONF_FONT_NAME = SCONF_MIN,
    SCONF_ANSWERBACK_STRING,
    SCONF_SHELL,
    SCONF_TERM_NAME,
    SCONF_TERM_CLASS,
    SCONF_TITLE,
    SCONF_PRINTER,
    SCONF_PRINT_CMD,
    SCONF_WORD_SEPARATORS,
    SCONF_TERM_MOD,
    SCONF_FORCE_MOUSE_MOD,
    SCONF_CONFIG_PATH,
    SCONF_CWD,
    SCONF_MAX,

    KCONF_MIN = SCONF_MAX,
    KCONF_BREAK = KCONF_MIN,
    KCONF_NUMLOCK,
    KCONF_SCROLL_UP,
    KCONF_SCROLL_DOWN,
    KCONF_FONT_INC,
    KCONF_FONT_DEC,
    KCONF_FONT_RESET,
    KCONF_NEW_WINDOW,
    KCONF_RESET,
    KCONF_RELOAD_CONFIG,
    KCONF_COPY,
    KCONF_PASTE,
    KCONF_REVERSE_VIDEO,
    KCONF_MAX,

    CCONF_MIN = KCONF_MAX,
    CCONF_COLOR_0 = CCONF_MIN,
    //Here CCONF_COLOR_1 - CCONF_COLOR_{PALETTE_SIZE}
    //CCONF_COLOR_{N} is CCONF_COLOR_0 + N
    CCONF_BOLD = CCONF_COLOR_0 + PALETTE_SIZE - SPECIAL_PALETTE_SIZE,
    CCONF_UNDERLINE,
    CCONF_BLINK,
    CCONF_REVERSE,
    CCONF_ITALIC,
    CCONF_BG,
    CCONF_FG,
    CCONF_CURSOR_BG,
    CCONF_CURSOR_FG,
    CCONF_SELECTED_BG,
    CCONF_SELECTED_FG,
    CCONF_MAX,

    CONF_MAX = CCONF_MAX,
};

struct optmap_item {
    const char *arg_name;
    const char *arg_desc;
    enum config_option opt;
};

#if USE_BOXDRAWING
#    define OPT_MAP_SIZE 131
#else
#    define OPT_MAP_SIZE 132
#endif

extern struct optmap_item optmap[OPT_MAP_SIZE];

/* Getters for options */
color_t cconf(uint32_t opt);
int32_t iconf(uint32_t opt);
const char *sconf(uint32_t opt);

/* Setters for options */
void cconf_set(uint32_t opt, color_t val);
void iconf_set(uint32_t opt, int32_t val);
void sconf_set(uint32_t opt, const char *val);
bool bconf_set(uint32_t opt, bool val);

/* Get argv from -e flag
 * Resets to NULL when called */
const char **sconf_argv(void);

/* Get argv for -e flag */
void sconf_set_argv(const char **argv);

void parse_config(void);

#endif
