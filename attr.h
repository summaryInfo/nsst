#ifndef NSS_ATTR_H_
#define NSS_ATTR_H_ 1

#include <stdint.h>

typedef uint16_t nss_cid_t;
typedef uint32_t nss_color_t;

#define NSS_SPECIAL_COLORS 4
#define NSS_PALETTE_SIZE (256 + NSS_SPECIAL_COLORS)
#define NSS_SPECIAL_BG 256
#define NSS_SPECIAL_FG 257
#define NSS_SPECIAL_CURSOR_BG 258
#define NSS_SPECIAL_CURSOR_FG 259

enum nss_config_opt {
    nss_config_window_x,
    nss_config_window_y,
    nss_config_window_width,
    nss_config_window_height,
    nss_config_history_lines,
    nss_config_utf8,
    nss_config_allow_nrcs,
    nss_config_tab_width,
    nss_config_init_wrap,
    nss_config_scroll_on_input,
    nss_config_scroll_on_output,
    nss_config_appkey,
    nss_config_appcursor,
    nss_config_numlock,
    nss_config_has_meta,
    nss_config_meta_escape,
    nss_config_backspace_is_delete,
    nss_config_delete_is_delete,
    nss_config_cursor_shape,
    nss_config_underline_width,
    nss_config_cursor_width,
    nss_config_subpixel_fonts,
    nss_config_reverse_video,
    nss_config_allow_altscreen,
    nss_config_left_border,
    nss_config_top_border,
    nss_config_blink_time,
    nss_config_font_size,
    nss_config_font_name, // string
    nss_config_answerback_string, // string
    nss_config_shell, // string
    nss_config_term_name, //string
    nss_config_color_0,
    nss_config_bg = nss_config_color_0 + NSS_PALETTE_SIZE - NSS_SPECIAL_COLORS,
    nss_config_fg,
    nss_config_cursor_bg,
    nss_config_cursor_fg,
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
    nss_attrib_wdummy = 1 << 9,
    nss_attrib_protected = 1 << 10
    // 11 bits total, sice unicode codepoint is 21 bit
} nss_attrs_t;

#define CELL_CHAR_BITS 21
#define CELL_CHAR_MASK ((1 << CELL_CHAR_BITS) - 1)
#define CELL_CHAR(s) ((s).ch & CELL_CHAR_MASK)
#define CELL_CHAR_SET(s, c) ((s).ch = ((s).ch & ~CELL_CHAR_MASK) | ((c) & CELL_CHAR_MASK))
#define CELL_FG(s, b) (nss_color_get((s).fg + (b)))
#define CELL_BG(s, b) (nss_color_get((s).bg + (b)))
#define CELL_ATTR_ZERO(s) ((s).ch &= CELL_CHAR_MASK)
#define CELL_ATTR(s) ((s).ch >> CELL_CHAR_BITS)
#define CELL_ATTR_SET(s, l) ((s).ch |= (l) << CELL_CHAR_BITS)
#define CELL_ATTR_CLR(s, l) ((s).ch &= ~((l) << CELL_CHAR_BITS))
#define CELL_ATTR_INVERT(s, l) ((s).ch ^= (l) << CELL_CHAR_BITS)
#define MKCELLWITH(s, c) MKCELL((s).fg, (s).bg, CELL_ATTR(s), (c))
#define MKCELL(f, b, l, c) ((nss_cell_t) { .bg = (b), .fg = (f), .ch = ((c) & CELL_CHAR_MASK) | ((l) << CELL_CHAR_BITS)})

typedef struct nss_cell {
        uint32_t ch; /* not really char but char + attributes */
        nss_cid_t fg;
        nss_cid_t bg;
} nss_cell_t;

nss_color_t *nss_create_palette(void);
nss_color_t nss_config_color(uint32_t opt);
int32_t nss_config_integer(uint32_t opt, int32_t min, int32_t max);
const char *nss_config_string(uint32_t opt, const char *alt);

#endif

