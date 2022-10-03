/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

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

struct term;
struct screen;

typedef uint32_t uparam_t;
typedef int32_t iparam_t;

#define SCNparam SCNu32

struct line_view {
    struct line_handle h;
    int32_t width;
    bool wrapped;
};

inline static struct cell *view_cell(struct line_view *view, ssize_t x) {
    return view->h.line->cell + view->h.offset + x;
}

inline static struct attr view_attr_at(struct line_view *view, ssize_t x) {
    return attr_at(view->h.line, view->h.offset + x);
}

inline static struct attr view_attr(struct line_view *view, uint32_t attrid) {
    return attrid ? view->h.line->attrs->data[attrid - 1] : ATTR_DEFAULT;
}

inline static void view_adjust_wide_right(struct line_view *view, ssize_t x) {
    adjust_wide_right(view->h.line, view->h.offset + x);
}

inline static void view_adjust_wide_left(struct line_view *view, ssize_t x) {
    adjust_wide_left(view->h.line, view->h.offset + x);
}

/* Returns true if next line will be next physical line
 * and not continuation part of current physical line */
inline static bool is_last_line(struct line_view *view, bool rewrap) {
    return !rewrap || !view->wrapped;
}

struct term *create_term(struct window *win, int16_t width, int16_t height);
void free_term(struct term *term);
void term_resize(struct term *term, int16_t width, int16_t height);
void term_handle_focus(struct term *term, bool focused);
bool term_read(struct term *term);
void term_scroll_view(struct term *term, int16_t amount);
void term_reload_config(struct term *term);
void term_toggle_numlock(struct term *term);
struct keyboard_state *term_get_kstate(struct term *term);
struct mouse_state *term_get_mstate(struct term *term);
struct selection_state *term_get_sstate(struct term *term);
struct window *term_window(struct term *term);
color_t *term_palette(struct term *term);
int term_fd(struct term *term);
void term_paste(struct term *term, uint8_t *data, ssize_t size, bool utf8, bool is_first, bool is_last);
void term_sendkey(struct term *term, const uint8_t *data, size_t size);
void term_answerback(struct term *term, const char *str, ...) __attribute__ ((format (printf, 2, 3)));
void term_reset(struct term *term);
void term_set_reverse(struct term *term, bool set);
void term_break(struct term *term);
void term_hang(struct term *term);

struct screen *term_screen(struct term *term);

bool screen_redraw(struct screen *scr, bool blink_commited);
void screen_damage_lines(struct screen *scr, ssize_t ys, ssize_t yd);
void screen_scroll_view(struct screen *scr, int16_t amount);
struct line_view screen_view_at(struct screen *scr, struct line_handle *pos);
struct line_handle screen_view(struct screen *scr); /* NOTE: It does not register handle */
struct line_handle screen_line_iter(struct screen *scr, ssize_t y); /* NOTE: It does not register handle */
ssize_t screen_advance_iter(struct screen *scr, struct line_handle *pos, ssize_t amount);
ssize_t screen_inc_iter(struct screen *scr, struct line_handle *pos);
void screen_damage_selection(struct screen *scr);
#if USE_URI
void screen_damage_uri(struct screen *scr, uint32_t uri);
#endif

/* Needs to be multiple of 4 */
#define PASTE_BLOCK_SIZE 1024

bool term_is_keep_clipboard_enabled(struct term *term);
bool term_is_bell_urgent_enabled(struct term *term);
bool term_is_bell_raise_enabled(struct term *term);
bool term_is_utf8_enabled(struct term *term);
bool term_is_nrcs_enabled(struct term *term);
bool term_is_reverse(struct term *term);

#endif
