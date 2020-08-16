/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#ifndef TERM_H_
#define TERM_H_ 1

#include "feature.h"

#include "util.h"
#include "window.h"

#include <stdint.h>
#include <sys/types.h>

typedef uint16_t nss_cid_t;
typedef uint32_t nss_color_t;
typedef uint32_t nss_char_t;
typedef int16_t nss_coord_t;
typedef uint32_t param_t;

#define NSS_SPECIAL_COLORS 11
#define NSS_PALETTE_SIZE (256 + NSS_SPECIAL_COLORS)
#define NSS_SPECIAL_BOLD 256
#define NSS_SPECIAL_UNDERLINE 257
#define NSS_SPECIAL_BLINK 258
#define NSS_SPECIAL_REVERSE 259
#define NSS_SPECIAL_ITALIC 260
#define NSS_SPECIAL_BG 261
#define NSS_SPECIAL_FG 262
#define NSS_SPECIAL_CURSOR_BG 263
#define NSS_SPECIAL_CURSOR_FG 264
#define NSS_SPECIAL_SELECTED_BG 265
#define NSS_SPECIAL_SELECTED_FG 266

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

typedef struct nss_line_palette {
    uint16_t size;
    uint16_t caps;
    nss_color_t data[];
} nss_line_palette_t;

typedef struct nss_line {
    nss_line_palette_t *pal;
    int16_t width;
    int16_t wrap_at;
    uint32_t force_damage : 1;
    nss_cell_t cell[];
} nss_line_t;

/* iterator for iterating both scrollback and screen */

typedef struct nss_line_iter {
    nss_line_t **_screen;
    nss_line_t **_scrollback;
    ssize_t _sb_0;
    ssize_t _sb_limit;
    ssize_t _y_min;
    ssize_t _y_max;
    ssize_t _y;
} nss_line_iter_t;

inline static ssize_t line_iter_y(nss_line_iter_t *it) {
    return it->_y - 1 - it->_y_min;
}

inline static nss_line_t *line_iter_next(nss_line_iter_t *it) {
    if (it->_y >= it->_y_max || it->_y < it->_y_min) return NULL;
    return (it->_y >= 0) ? it->_screen[it->_y++] :
        it->_scrollback[(it->_sb_0 + ++it->_y + it->_sb_limit) % it->_sb_limit];
}

typedef struct nss_input_mode nss_input_mode_t;
typedef struct nss_term nss_term_t;

nss_term_t *nss_create_term(nss_window_t *win, nss_coord_t width, nss_coord_t height);
void nss_free_term(nss_term_t *term);
_Bool nss_term_redraw_dirty(nss_term_t *term);
void nss_term_resize(nss_term_t *term, nss_coord_t width, nss_coord_t height);
void nss_term_focus(nss_term_t *term, _Bool focused);
void nss_term_mouse(nss_term_t *term, nss_coord_t x, nss_coord_t y, nss_mouse_state_t mask, nss_mouse_event_t event, uint8_t button);
void nss_term_sendkey(nss_term_t *term, const uint8_t *data, size_t size);
void nss_term_sendbreak(nss_term_t *term);
void nss_term_scroll_view(nss_term_t *term, nss_coord_t amount);
void nss_term_read(nss_term_t *term);
int nss_term_fd(nss_term_t *term);
void nss_term_hang(nss_term_t *term);
void nss_term_toggle_numlock(nss_term_t *term);
nss_input_mode_t *nss_term_inmode(nss_term_t *term);
_Bool nss_term_is_utf8(nss_term_t *term);
_Bool nss_term_is_nrcs_enabled(nss_term_t *term);
_Bool nss_term_is_paste_nl_enabled(nss_term_t *term);
_Bool nss_term_is_paste_quote_enabled(nss_term_t *term);
_Bool nss_term_is_cursor_enabled(nss_term_t *term);
void nss_term_damage_lines(nss_term_t *term, nss_coord_t ys, nss_coord_t yd);
void nss_term_reset(nss_term_t *term);

/* Selection related functions */
_Bool nss_term_is_selected(nss_term_t *term, nss_coord_t x, nss_coord_t y);
void nss_term_clear_selection(nss_term_t *term);
_Bool nss_term_paste_need_encode(nss_term_t *term);
void nss_term_paste_begin(nss_term_t *term);
void nss_term_paste_end(nss_term_t *term);
_Bool nss_term_keep_clipboard(nss_term_t *term);

void nss_setup_default_termios(void);

#endif
