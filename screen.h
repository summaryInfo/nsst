/* Copyright (c) 2019-2022,2025, Evgeniy Baskov. All rights reserved */

#ifndef SCREEN_H_
#define SCREEN_H_ 1

#include "feature.h"

#include "mouse.h"
#include "nrcs.h"
#include "term.h"
#include "tty.h"

#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>

#define IS_C1(c) ((uint32_t)(c) - 0x80U < 0x20U)
#define IS_C0(c) ((c) < 0x20U)
#define IS_CBYTE(c) (!((uint32_t)(c) & 0x60))
#define IS_DEL(c) ((c) == 0x7FU)
#define IS_STREND(c) ((c) == 0x1B || (c) == 0x1A || (c) == 0x18 || (c) == 0x07)

struct cursor {
    /* Curosr position */
    ssize_t x;
    ssize_t y;

    /* Character sets */
    uint32_t gl;
    uint32_t gr;
    uint32_t gl_ss;
    enum charset gn[4];

    bool origin : 1;

    /* Pending wrap flag
     * is used to match DEC VT terminals
     * wrapping behavior */
    bool pending : 1;
};

struct screen_mode {
    bool altscreen : 1;
    bool lr_margins : 1;
    bool disable_altscreen : 1;
    bool hide_cursor : 1;
    bool attr_ext_rectangle : 1;
    bool smooth_scroll : 1;
    bool wrap : 1;
    bool insert : 1;
    bool reverse_wrap : 1;
    bool xterm_more_hack : 1;
    bool print_extend : 1;
    bool print_auto : 1;
    bool print_form_feed : 1;
    bool margin_bell : 1;
};

struct checksum_mode {
    bool positive : 1;
    bool no_attr : 1;
    bool no_trim : 1;
    bool no_implicit : 1;
    bool wide : 1;
    bool eight_bit : 1;
};

/* There are two screens, and corresponding
 * saved cursor (saved_c, back_saved_c) and SGR states (saved_sgr, back_saved_sgr),
 * if term->scr.mode.altscreen is set
 * scr.screen points to alternate screen and scr.back_screen points to main screen,
 * in opposite case screen points to main screen and back_screen points to
 * alternate screen; same with saved_c/back_saved_c and saved_sgr/saved_back_sgr */

struct screen {
    struct screen_storage main_screen;
    struct screen_storage alt_screen;

    struct line_span *screen; /* either main_screen.screen or alt_screen.screen */
    struct line_span *temp_screen;

    /* History topmost line */
    struct line_handle top_line;
    /* Number of lines */
    ssize_t sb_limit;
    /* Maximal capacity */
    ssize_t sb_max_caps;

    /* Viewport start position */
    struct line_handle view_pos;

    /* Margins */
    ssize_t top;
    ssize_t bottom;
    ssize_t left;
    ssize_t right;

    /* Terminal screen dimensions */
    ssize_t width;
    ssize_t height;

    /* Smooth scroll accumulator */
    ssize_t scrolled;

    /* Flags specific to screen */
    struct screen_mode mode;

    /* Previous cursor state
     * Used for effective cursor invalidation */
    ssize_t prev_c_x;
    ssize_t prev_c_y;
    bool prev_c_hidden;
    bool prev_c_view_changed;

    /* Selection state */
    struct selection_state sstate;

    /* Cursor state */
    struct cursor back_saved_c;
    struct cursor saved_c;
    struct cursor last_scr_c;
    struct cursor c;

    /* Graphics rendition state */
    struct attr back_saved_sgr;
    struct attr saved_sgr;
    struct attr sgr;

    /* Tabstop positions */
    bool *tabs;

    /* Terminal window pointer */
    struct window *win;
    /* Printer state */
    struct printer printer;

    bool scroll_damage;

    /* Sequential graphical characters are decoded
     * at once for faster parsing, and stored to this buffer
     * size of this buffer is equal to terminal width */
    uint32_t *predec_buf;

    /* Margin bell volume */
    uint8_t mbvol;

    /* User preferred charset */
    enum charset upcs;

    /* Last written character
     * Used for REP */
    uint32_t prev_ch;

    const uint8_t *save_handle_at_print;
    struct line_handle saved_handle;
};

bool init_screen(struct screen *scr, struct window *win);
void free_screen(struct screen *scr);
struct line_span screen_view(struct screen *scr);
struct line_span screen_top(struct screen *scr);
void screen_damage_lines(struct screen *scr, ssize_t ys, ssize_t yd);
void screen_damage_selection(struct screen *scr);
void screen_damage_uri(struct screen *scr, uint32_t uri);
void screen_span_width(struct screen *scr, struct line_span *pos);
ssize_t screen_span_shift_n(struct screen *scr, struct line_span *pos, ssize_t amount);
struct line_span screen_span(struct screen *scr, ssize_t y);
void screen_reset_view(struct screen *scr, bool damage);
void screen_free_scrollback(struct screen *scr, ssize_t max_size);
void screen_scroll_view(struct screen *scr, int16_t amount);
void screen_scroll_view_to_cmd(struct screen *scr, int16_t amount);
void screen_resize(struct screen *scr, int16_t width, int16_t height);
bool screen_redraw(struct screen *scr, bool blink_commited);
void screen_set_tb_margins(struct screen *scr, int16_t top, int16_t bottom);
bool screen_set_lr_margins(struct screen *scr, int16_t left, int16_t right);
void screen_reset_margins(struct screen *scr);
uint16_t screen_checksum(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye, struct checksum_mode mode, bool nrcs);
void screen_reverse_sgr(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye, struct attr *attr);
void screen_apply_sgr(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye, struct attr *mask, struct attr *attr);
struct attr screen_common_sgr(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye);
void screen_copy(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye, int16_t xd, int16_t yd, bool origin);
void screen_fill(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye, bool origin, uint32_t ch);
void screen_erase(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye, bool origin);
void screen_protective_erase(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye, bool origin);
void screen_selective_erase(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye, bool origin);
void screen_move_to(struct screen *scr, int16_t x, int16_t y);
void screen_bounded_move_to(struct screen *scr, int16_t x, int16_t y);
void screen_move_left(struct screen *scr, int16_t amount);
void screen_save_cursor(struct screen *scr, bool mode);
void screen_set_altscreen(struct screen *scr, bool set, bool clear, bool save);
void screen_scroll_horizontal(struct screen *scr, int16_t left, int16_t amount);
void screen_scroll(struct screen *scr, int16_t top, int16_t amount, bool save);
void screen_insert_cells(struct screen *scr, int16_t n);
void screen_delete_cells(struct screen *scr, int16_t n);
void screen_insert_lines(struct screen *scr, int16_t n);
void screen_delete_lines(struct screen *scr, int16_t n);
void screen_insert_columns(struct screen *scr, int16_t n);
void screen_delete_columns(struct screen *scr, int16_t n);
void screen_index_horizonal(struct screen *scr);
void screen_rindex_horizonal(struct screen *scr);
bool screen_index(struct screen *scr);
void screen_rindex(struct screen *scr);
void screen_cr(struct screen *scr);
bool screen_load_config(struct screen *scr, bool reset);
void screen_tabs(struct screen *scr, int16_t n);
void screen_reset_tabs(struct screen *scr);
void screen_print_screen(struct screen *scr, bool force_ext);
void screen_print_line(struct screen *scr, struct line_span *line);
void screen_set_margin_bell_volume(struct screen *scr, uint8_t vol);
uint8_t screen_get_margin_bell_volume(struct screen *scr);
bool screen_dispatch_print(struct screen *scr, const uint8_t **start, const uint8_t *end, bool utf8, bool nrcs);
ssize_t screen_dispatch_rep(struct screen *scr, int32_t rune, ssize_t rep);
void screen_ensure_new_paragaph(struct screen *scr);
void screen_cursor_line_set_prompt(struct screen *scr);
void screen_cursor_line_set_cmd_start(struct screen *scr);
void screen_print_all(struct screen *scr);
void screen_drain_scrolled(struct screen *scr);

char *encode_sgr(char *dst, char *end, const struct attr *attr);

static inline void screen_set_bookmark(struct screen *scr, const uint8_t *pin) {
    scr->save_handle_at_print = pin;
}

static inline struct line_span *screen_get_bookmark(struct screen *scr) {
    return &scr->saved_handle.s;
}

static inline ssize_t screen_width(struct screen *scr) {
    return scr->width;
}

static inline ssize_t screen_height(struct screen *scr) {
    return scr->height;
}

static inline ssize_t screen_max_y(struct screen *scr) {
    return scr->bottom + 1;
}

static inline ssize_t screen_min_y(struct screen *scr) {
    return scr->top;
}

static inline ssize_t screen_max_x(struct screen *scr) {
    return scr->mode.lr_margins ? scr->right + 1 : scr->width;
}

static inline ssize_t screen_min_x(struct screen *scr) {
    return scr->mode.lr_margins ? scr->left : 0;
}

static inline ssize_t screen_max_ox(struct screen *scr) {
    return (scr->mode.lr_margins && scr->c.origin) ? scr->right + 1: scr->width;
}

static inline ssize_t screen_min_ox(struct screen *scr) {
    return (scr->mode.lr_margins && scr->c.origin) ? scr->left : 0;
}

static inline ssize_t screen_max_oy(struct screen *scr) {
    return scr->c.origin ? scr->bottom + 1: scr->height;
}

static inline ssize_t screen_min_oy(struct screen *scr) {
    return scr->c.origin ? scr->top : 0;
}

static inline ssize_t screen_cursor_x(struct screen *scr) {
    return scr->c.x;
}

static inline ssize_t screen_cursor_y(struct screen *scr) {
    return scr->c.y;
}

static inline void screen_reset_pending(struct screen *scr) {
    scr->c.pending = 0;
}

static inline bool screen_cursor_in_region(struct screen *scr) {
    return scr->c.x >= screen_min_x(scr) && scr->c.x < screen_max_x(scr) &&
            scr->c.y >= screen_min_y(scr) && scr->c.y < screen_max_y(scr);
}

static inline void screen_cursor_adjust_wide_left(struct screen *scr) {
    view_adjust_wide_left(&scr->screen[scr->c.y], scr->c.x);
}

static inline void screen_cursor_adjust_wide_right(struct screen *scr) {
    view_adjust_wide_right(&scr->screen[scr->c.y], scr->c.x);
}

static inline void screen_damage_cursor(struct screen *scr) {
    struct line_span *cview = &scr->screen[scr->c.y];
    if (cview->width <= scr->c.x) cview->line->force_damage = 1;
    else view_cell(cview, scr->c.x)->drawn = 0;
}

static inline void screen_move_width_origin(struct screen *scr, int16_t x, int16_t y) {
    (scr->c.origin ? screen_bounded_move_to : screen_move_to)(scr, x, y);
}

static inline struct attr *screen_sgr(struct screen *scr) {
    return &scr->sgr;
}

static inline struct window *screen_window(struct screen *scr) {
    return scr->win;
}

static inline struct selection_state *screen_selection(struct screen *scr) {
    return &scr->sstate;
}

static inline ssize_t screen_scrollback_top(struct screen *scr) {
    return -scr->sb_limit;
}

static inline bool screen_has_tab(struct screen *scr, int16_t i) {
    return scr->tabs[i];
}

static inline struct printer *screen_printer(struct screen *scr) {
    return &scr->printer;
}

static inline void screen_set_tab(struct screen *scr, ssize_t i, bool set) {
    scr->tabs[i] = set;
}

static inline void screen_clear_tabs(struct screen *scr) {
    memset(scr->tabs, 0, scr->width*sizeof *scr->tabs);
}

static inline void screen_store_cursor_position(struct screen *scr, ssize_t *cx,
                                                ssize_t *cy, bool *cpending) {
    *cx = scr->c.x;
    *cy = scr->c.y;
    *cpending = scr->c.pending;
}

static inline void screen_load_cursor_position(struct screen *scr, ssize_t cx,
                                               ssize_t cy, bool cpending) {
    scr->c.x = cx;
    scr->c.y = cy;
    scr->c.pending = cpending;
}

static inline void screen_print_cursor_line(struct screen *scr) {
    screen_print_line(scr, &scr->screen[scr->c.y]);
}

static inline void screen_precompose_at_cursor(struct screen *scr, uint32_t ch) {
    struct line_span *cview = &scr->screen[scr->c.y];
    if (cview->width <= scr->c.x) return;

    struct cell *cel = view_cell(cview, scr->c.x);

    /* Step back to previous cell */
    if (scr->c.x) cel--;
    if (!cel->ch && scr->c.x > 1 && cell_wide(cel - 1)) cel--;

    ch = try_precompose(cell_get(cel), ch);

    /* Only make cell dirty if precomposition happened */
    if (cell_get(cel) != ch)
        cell_set(cel, ch);
}

static inline enum charset screen_get_upcs(struct screen *scr) {
    return scr->upcs;
}

static inline void screen_set_upcs(struct screen *scr, enum charset upcs) {
    scr->upcs = upcs;
}

static inline void screen_set_gl(struct screen *scr, ssize_t gl, bool once) {
    if (!once) scr->c.gl = gl;
    scr->c.gl_ss = gl;
}

static inline void screen_set_gr(struct screen *scr, ssize_t gr) {
    scr->c.gr = gr;
}

static inline void screen_set_charset(struct screen *scr, ssize_t idx, enum charset cs) {
    scr->c.gn[idx] = cs;
}

static inline struct cursor *screen_cursor(struct screen *scr) {
    return &scr->c;
}

static inline void screen_rep(struct screen *scr, ssize_t rep) {
    if (scr->prev_ch == -1U) return;
    while (rep > 0)
        rep = screen_dispatch_rep(scr, scr->prev_ch, rep);
}

static inline void screen_putchar(struct screen *scr, uint32_t ch) {
    screen_dispatch_rep(scr, ch, 1);
}

static inline bool screen_altscreen(struct screen *scr) {
    return scr->mode.altscreen;
}

static inline void screen_set_origin(struct screen *scr, bool set) {
    scr->c.origin = set;
    screen_move_to(scr, screen_min_ox(scr), screen_min_oy(scr));
}

static inline void screen_autoprint(struct screen *scr) {
    if (scr->mode.print_auto)
        screen_print_line(scr, &scr->screen[scr->c.y]);
}

#endif
