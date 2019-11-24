#ifndef NSS_ATTR_H_
#define NSS_ATTR_H_ 1

#include <stdint.h>
#include "input.h"

typedef uint16_t nss_cid_t;
typedef uint32_t nss_color_t;

#define NSS_SPECIAL_COLORS 4
#define NSS_PALETTE_SIZE (256 + NSS_SPECIAL_COLORS)
#define NSS_SPECIAL_BG 256
#define NSS_SPECIAL_FG 257
#define NSS_SPECIAL_CURSOR_BG 258
#define NSS_SPECIAL_CURSOR_FG 259

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
    NSS_ICONFIG_MAX,

    NSS_SCONFIG_FONT_NAME,
    NSS_SCONFIG_ANSWERBACK_STRING,
    NSS_SCONFIG_SHELL,
    NSS_SCONFIG_TERM_NAME,
    NSS_SCONFIG_MAX,

    NSS_CCONFIG_COLOR_0,
    NSS_CCONFIG_BG = NSS_CCONFIG_COLOR_0 + NSS_PALETTE_SIZE - NSS_SPECIAL_COLORS,
    NSS_CCONFIG_FG,
    NSS_CCONFIG_CURSOR_BG,
    NSS_CCONFIG_CURSOR_FG,
    NSS_CCONFIG_MAX,
};

typedef enum nss_attrs {
    nss_attrib_italic = 1 << 0,
    nss_attrib_bold = 1 << 1,
    nss_attrib_faint = 1 << 2,
    nss_attrib_underlined = 1 << 3,
    nss_attrib_strikethrough = 1 << 4,
    nss_attrib_invisible = 1 << 5,
    nss_attrib_inverse = 1 << 6,
    nss_attrib_blink = 1 << 7,
    nss_attrib_wide = 1 << 8,
    nss_attrib_protected = 1 << 9,
    nss_attrib_drawn = 1 << 10
    // 11 bits total, sice unicode codepoint is 21 bit
} nss_attrs_t;

#define MKCELLWITH(s, c) MKCELL((s).fg, (s).bg, (s).attr, (c))
#define MKCELL(f, b, l, c) ((nss_cell_t) { .bg = (b), .fg = (f), .ch = (c), .attr = (l) & ~nss_attrib_drawn})

typedef struct nss_cell {
        uint32_t ch : 21;
        uint32_t attr : 11;
        nss_cid_t fg;
        nss_cid_t bg;
} nss_cell_t;

nss_color_t *nss_create_palette(void);
nss_color_t nss_config_color(uint32_t opt);
int32_t nss_config_integer(uint32_t opt);
const char *nss_config_string(uint32_t opt);
void nss_config_set__color(uint32_t opt, nss_color_t val);
void nss_config_set_integer(uint32_t opt, int32_t val);
void nss_config_set_string(uint32_t opt, const char *val);
nss_input_mode_t nss_config_input_mode(void);
void nss_config_set_input_mode(nss_input_mode_t val);

#endif

