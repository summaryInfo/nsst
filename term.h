/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#ifndef TERM_H_
#define TERM_H_ 1

#include "feature.h"

#include "util.h"
#include "window.h"

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>

typedef uint16_t color_id_t;
typedef uint32_t color_t;
typedef uint32_t term_char_t;
typedef int16_t nss_coord_t;
typedef uint32_t uparam_t;
typedef int32_t iparam_t;

#define PRIparam PRId32
#define SCNparam SCNd32

#define SPECIAL_PALETTE_SIZE 11
#define PALETTE_SIZE (256 + SPECIAL_PALETTE_SIZE)
#define SPECIAL_BOLD 256
#define SPECIAL_UNDERLINE 257
#define SPECIAL_BLINK 258
#define SPECIAL_REVERSE 259
#define SPECIAL_ITALIC 260
#define SPECIAL_BG 261
#define SPECIAL_FG 262
#define SPECIAL_CURSOR_BG 263
#define SPECIAL_CURSOR_FG 264
#define SPECIAL_SELECTED_BG 265
#define SPECIAL_SELECTED_FG 266

enum cell_attr {
    attr_italic = 1 << 0,
    attr_bold = 1 << 1,
    attr_faint = 1 << 2,
    attr_underlined = 1 << 3,
    attr_strikethrough = 1 << 4,
    attr_invisible = 1 << 5,
    attr_inverse = 1 << 6,
    attr_blink = 1 << 7,
    attr_wide = 1 << 8,
    attr_protected = 1 << 9,
    attr_drawn = 1 << 10
    // 11 bits total, sice unicode codepoint is 21 bit
};

#define MKCELLWITH(s, c) MKCELL((s).fg, (s).bg, (s).attr, (c))
#define MKCELL(f, b, l, c) ((struct cell) { .bg = (b), .fg = (f), .ch = (c), .attr = (l) & ~attr_drawn})

struct cell {
    uint32_t ch : 21;
    uint32_t attr : 11;
    color_id_t fg;
    color_id_t bg;
};

struct line_palette {
    color_id_t size;
    color_id_t caps;
    color_t data[];
};

struct line {
    struct line_palette *pal;
    int16_t width;
    _Bool force_damage;
    _Bool wrapped;
    struct cell cell[];
};

struct line_view {
    struct line *line;
    uint16_t width;
    _Bool wrapped;
    struct cell *cell;
};

struct line_offset {
    ssize_t line;
    ssize_t offset;
};

struct term *create_term(struct window *win, nss_coord_t width, nss_coord_t height);
void free_term(struct term *term);
_Bool term_redraw(struct term *term);
void term_resize(struct term *term, nss_coord_t width, nss_coord_t height);
void term_handle_focus(struct term *term, _Bool focused);
void term_sendkey(struct term *term, const uint8_t *data, size_t size);
void term_answerback(struct term *term, const char *str, ...);
void term_break(struct term *term);
void term_scroll_view(struct term *term, nss_coord_t amount);
void term_read(struct term *term);
int term_fd(struct term *term);
void term_hang(struct term *term);
void term_toggle_numlock(struct term *term);
struct keyboard_state *term_get_kstate(struct term *term);
void term_damage_lines(struct term *term, nss_coord_t ys, nss_coord_t yd);
void term_damage(struct term *term, struct rect damage);
void term_reset(struct term *term);
void term_set_reverse(struct term *term, _Bool set);
struct window *term_window(struct term *term);

struct line_view term_line_at(struct term *term, struct line_offset pos);
struct line_offset term_get_view(struct term *term);
struct line_offset term_get_line_pos(struct term *term, ssize_t y);
ssize_t term_line_next(struct term *term, struct line_offset *pos, ssize_t amount);
_Bool is_last_line(struct line_view line);

_Bool term_is_paste_requested(struct term *term);
void term_paste_begin(struct term *term);
void term_paste_end(struct term *term);

_Bool term_is_keep_clipboard_enabled(struct term *term);
_Bool term_is_keep_selection_enabled(struct term *term);
_Bool term_is_select_to_clipboard_enabled(struct term *term);
_Bool term_is_bell_urgent_enabled(struct term *term);
_Bool term_is_bell_raise_enabled(struct term *term);
_Bool term_is_utf8_enabled(struct term *term);
_Bool term_is_nrcs_enabled(struct term *term);
_Bool term_is_paste_nl_enabled(struct term *term);
_Bool term_is_paste_quote_enabled(struct term *term);
_Bool term_is_cursor_enabled(struct term *term);
_Bool term_is_reverse(struct term *term);

void init_default_termios(void);

#endif
