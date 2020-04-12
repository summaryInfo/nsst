/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#ifndef TERM_H_
#define TERM_H_ 1

#include <sys/types.h>
#include "window.h"
#include "util.h"

typedef uint16_t nss_cid_t;
typedef uint32_t nss_color_t;

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

typedef struct nss_line {
    struct nss_line *next, *prev;
    int16_t width;
    int16_t wrap_at;
    uint16_t extra_size;
    uint16_t extra_caps;
    nss_color_t *extra;
    nss_cell_t cell[];
} nss_line_t;

typedef struct nss_input_mode nss_input_mode_t;
typedef struct nss_term nss_term_t;

nss_term_t *nss_create_term(nss_window_t *win, nss_input_mode_t *mode, int16_t width, int16_t height);
void nss_free_term(nss_term_t *term);
void nss_term_redraw_dirty(nss_term_t *term, _Bool cursor);
void nss_term_resize(nss_term_t *term, int16_t width, int16_t height);
void nss_term_visibility(nss_term_t *term, _Bool visible);
void nss_term_focus(nss_term_t *term, _Bool focused);
_Bool nss_term_mouse(nss_term_t *term, int16_t x, int16_t y, nss_mouse_state_t mask, nss_mouse_event_t event, uint8_t button);
void nss_term_sendkey(nss_term_t *term, const char *str, size_t size);
void nss_term_sendbreak(nss_term_t *term);
void nss_term_scroll_view(nss_term_t *term, int16_t amount);
ssize_t nss_term_read(nss_term_t *term);
int nss_term_fd(nss_term_t *term);
void nss_term_hang(nss_term_t *term);
_Bool nss_term_is_altscreen(nss_term_t *term);
_Bool nss_term_is_utf8(nss_term_t *term);
_Bool nss_term_is_nrcs_enabled(nss_term_t *term);
void nss_term_damage(nss_term_t *term, nss_rect_t damage);

#endif
