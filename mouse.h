/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

#ifndef MOUSE_H_
#define MOUSE_H_ 1

#include "term.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

struct screen;

struct segments {
    struct line *line;
    bool new_line_flag;
    uint16_t size;
    uint16_t caps;
    struct segment {
        uint32_t offset;
        uint32_t length;
    } segs[];
};

#define SELECTION_EMPTY 0

struct mouse_state {
    int16_t reported_x;
    int16_t reported_y;
    uint8_t reported_button;

    bool locator_enabled : 1;
    bool locator_oneshot : 1;
    bool locator_filter : 1;
    bool locator_pixels : 1;
    bool locator_report_press : 1;
    bool locator_report_release : 1;

    struct rect filter;

    enum mouse_mode {
        mouse_mode_none,
        mouse_mode_x10,
        mouse_mode_button,
        mouse_mode_drag,
        mouse_mode_motion,
    } mouse_mode;

    enum mouse_format {
        mouse_format_default,
        mouse_format_sgr,
        mouse_format_utf8,
        mouse_format_uxvt,
        mouse_format_pixel
    } mouse_format;
};

struct mouse_selection_iterator {
    struct segment *seg;
    ssize_t idx;
};

void mouse_handle_input(struct term *term, struct mouse_event ev);
void mouse_report_locator(struct term *term, uint8_t evt, int16_t x, int16_t y, uint32_t mask);
void mouse_set_filter(struct term *term, iparam_t xs, iparam_t xe, iparam_t ys, iparam_t ye);

struct selection_state {
    struct window *win;

    size_t seg_caps;
    size_t seg_size;
    struct segments **seg;

    struct line_handle start;
    struct line_handle end;
    bool rectangular;

    enum {
        snap_none,
        snap_word,
        snap_line,
    } snap;

    enum {
        state_sel_none,
        state_sel_pressed = mouse_event_press + 1,
        state_sel_released = mouse_event_release + 1,
        state_sel_progress = mouse_event_motion + 1,
    } state;

    struct timespec click0;
    struct timespec click1;
    struct timespec last_scroll;

    int32_t pending_scroll;
    int16_t pointer_x;
    int16_t pointer_y;

    enum clip_target targ;

    bool keep_selection;
    bool select_to_clipboard;
};


void free_selection(struct selection_state *sel);
bool init_selection(struct selection_state *sel, struct window *win);

void selection_view_scrolled(struct selection_state *sel, struct screen *scr);

/* Starts from the last character */
struct mouse_selection_iterator selection_begin_iteration(struct selection_state *sel, struct line_view *view);
bool is_selected_prev(struct mouse_selection_iterator *it, struct line_view *view, int16_t x);

void selection_clear(struct selection_state *sel);
void selection_damage(struct selection_state *sel, struct line *line);
void selection_concat(struct selection_state *sel, struct line *dst, struct line *src);
void selection_split(struct selection_state *sel, struct line *line, struct line *tail);
void selection_relocated(struct selection_state *sel, struct line *line);
void selection_free(struct selection_state *sel, struct line *line);
void selection_scrolled(struct selection_state *sel, struct screen *scr, int16_t x, ssize_t top, ssize_t bottom);
bool selection_intersects(struct selection_state *sel, struct line *line, int16_t x0, int16_t x1);
bool selection_pending_scroll(struct selection_state *sel, struct screen *scr);

inline static bool selection_active(struct selection_state *sel) {
    return sel->state != state_sel_none &&
           sel->state != state_sel_pressed;
}

inline static bool view_selection_intersects(struct selection_state *sel, struct line_view *line, int16_t x0, int16_t x1) {
    ssize_t offset = line->h.offset;
    return selection_intersects(sel, line->h.line, x0 + offset, x1 + offset);
}

#endif
