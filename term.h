/* Copyright (c) 2019-2021, Evgeny Baskov. All rights reserved */

#ifndef TERM_H_
#define TERM_H_ 1

#include "feature.h"

#include "util.h"
#include "window.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

typedef uint32_t uparam_t;
typedef int32_t iparam_t;

#define SCNparam SCNu32

struct line_view {
    struct line *line;
    uint16_t width;
    bool wrapped;
    struct cell *cell;
};

struct line_offset {
    ssize_t line;
    ssize_t offset;
};

struct term *create_term(struct window *win, int16_t width, int16_t height);
void free_term(struct term *term);
bool term_redraw(struct term *term);
void term_resize(struct term *term, int16_t width, int16_t height);
void term_handle_focus(struct term *term, bool focused);
void term_scroll_view(struct term *term, int16_t amount);
bool term_read(struct term *term);
void term_toggle_numlock(struct term *term);
struct keyboard_state *term_get_kstate(struct term *term);
struct mouse_state *term_get_mstate(struct term *term);
struct window *term_window(struct term *term);
color_t *term_palette(struct term *term);
int term_fd(struct term *term);
void term_sendkey(struct term *term, const uint8_t *data, size_t size);
void term_answerback(struct term *term, const char *str, ...) __attribute__ ((format (printf, 2, 3)));
void term_damage_lines(struct term *term, ssize_t ys, ssize_t yd);
void term_damage(struct term *term, struct rect damage);
void term_reset(struct term *term);
void term_set_reverse(struct term *term, bool set);
void term_break(struct term *term);
void term_hang(struct term *term);

struct line_view term_line_at(struct term *term, struct line_offset pos);
struct line_offset term_get_view(struct term *term);
struct line_offset term_get_line_pos(struct term *term, ssize_t y);
ssize_t term_line_next(struct term *term, struct line_offset *pos, ssize_t amount);
bool is_last_line(struct line_view line, bool rewrap);

bool term_is_paste_requested(struct term *term);
void term_paste_begin(struct term *term);
void term_paste_end(struct term *term);

bool term_is_keep_clipboard_enabled(struct term *term);
bool term_is_keep_selection_enabled(struct term *term);
bool term_is_select_to_clipboard_enabled(struct term *term);
bool term_is_bell_urgent_enabled(struct term *term);
bool term_is_bell_raise_enabled(struct term *term);
bool term_is_utf8_enabled(struct term *term);
bool term_is_nrcs_enabled(struct term *term);
bool term_is_paste_nl_enabled(struct term *term);
bool term_is_paste_quote_enabled(struct term *term);
bool term_is_cursor_enabled(struct term *term);
bool term_is_reverse(struct term *term);

#endif
