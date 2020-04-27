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

#define NSS_SPECIAL_COLORS 4
#define NSS_PALETTE_SIZE (256 + NSS_SPECIAL_COLORS)
#define NSS_SPECIAL_BG 256
#define NSS_SPECIAL_FG 257
#define NSS_SPECIAL_CURSOR_BG 258
#define NSS_SPECIAL_CURSOR_FG 259

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
    struct nss_line *next, *prev;
    nss_line_palette_t *pal;
    int16_t width;
    int16_t wrap_at;
    nss_cell_t cell[];
} nss_line_t;

/* "iterator" to traverse viewport (scrollback list part, then screen array part) */

typedef struct line_iter {
    nss_line_t *_line;
    nss_line_t *_last;
    nss_line_t **_screen;
    ssize_t _y;
    ssize_t _y_scr;
    ssize_t _y_max;
} nss_line_iter_t;

inline static nss_line_iter_t make_line_iter(nss_line_t *view, nss_line_t **screen, ssize_t y0, ssize_t y1) {
    nss_line_iter_t it = { view, view, screen, 0, 0, y1 };
    if (y0 >= 0)  {
        while (it._line && it._y < y0) it._y++, it._line = it._line->next;
        if (!it._line) it._y_scr = it._y;
        it._y = y0;
    } else if (it._line) {
        while (it._line->prev && it._y > y0) it._y--, it._line = it._line->prev;
    }
    return it;
}

inline static ssize_t line_iter_y(nss_line_iter_t *it) {
    return it->_y - 1;
}

inline static nss_line_t *line_iter_next(nss_line_iter_t *it) {
    if (it->_y >= it->_y_max ) return NULL;
    nss_line_t *ln;
    if (it->_line) {
        ln = it->_line;
        if (!it->_line->next) {
            it->_y_scr = it->_y + 1;
            it->_last = it->_line;
        }
        it->_line = it->_line->next;
    } else
        ln = it->_screen[it->_y - it->_y_scr];
    it->_y++;
    return ln;
}

inline static nss_line_t *line_iter_prev(nss_line_iter_t *it) {
    it->_y--;
    if (it->_line)
        return it->_line = it->_line->prev;
    else if (it->_y + 1 == it->_y_scr)
        return it->_line = it->_last;
    else
        return it->_screen[it->_y - it->_y_scr];
}

typedef struct nss_input_mode nss_input_mode_t;
typedef struct nss_term nss_term_t;

nss_term_t *nss_create_term(nss_window_t *win, nss_coord_t width, nss_coord_t height);
void nss_free_term(nss_term_t *term);
void nss_term_redraw_dirty(nss_term_t *term, _Bool cursor);
void nss_term_resize(nss_term_t *term, nss_coord_t width, nss_coord_t height);
void nss_term_visibility(nss_term_t *term, _Bool visible);
void nss_term_focus(nss_term_t *term, _Bool focused);
_Bool nss_term_mouse(nss_term_t *term, nss_coord_t x, nss_coord_t y, nss_mouse_state_t mask, nss_mouse_event_t event, uint8_t button);
void nss_term_sendkey(nss_term_t *term, const uint8_t *data, size_t size);
void nss_term_sendbreak(nss_term_t *term);
void nss_term_scroll_view(nss_term_t *term, nss_coord_t amount);
ssize_t nss_term_read(nss_term_t *term);
int nss_term_fd(nss_term_t *term);
void nss_term_hang(nss_term_t *term);
nss_input_mode_t *nss_term_inmode(nss_term_t *term);
_Bool nss_term_is_utf8(nss_term_t *term);
_Bool nss_term_is_nrcs_enabled(nss_term_t *term);
void nss_term_damage(nss_term_t *term, nss_rect_t damage);
_Bool nss_term_is_selected(nss_term_t *term, nss_coord_t x, nss_coord_t y);
uint8_t *nss_term_selection_data(nss_term_t *term);
void nss_term_clear_selection(nss_term_t *term);
void nss_term_paste_begin(nss_term_t *term);
void nss_term_paste_end(nss_term_t *term);

#endif
