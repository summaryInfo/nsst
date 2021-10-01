/* Copyright (c) 2019-2021, Evgeny Baskov. All rights reserved */

#ifndef MOUSE_H_
#define MOUSE_H_ 1

#include "term.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

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
    size_t seg_caps;
    size_t seg_size;
    struct segments **seg;

    struct line_offset start;
    struct line_offset end;
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

    enum clip_target targ;

    int16_t pointer_x;
    int16_t pointer_y;

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

void free_mouse(struct term *term);
bool init_mouse(struct term *term);

void mouse_handle_input(struct term *term, struct mouse_event ev);
void mouse_view_scrolled(struct term *term);
bool mouse_is_selected(struct term *term, struct line_view *view, int16_t x);
void mouse_clear_selection(struct term *term, bool damage);
void mouse_damage_selected(struct term *term, struct line *line);
void mouse_report_locator(struct term *term, uint8_t evt, int16_t x, int16_t y, uint32_t mask);
void mouse_set_filter(struct term *term, iparam_t xs, iparam_t xe, iparam_t ys, iparam_t ye);
bool mouse_pending_scroll(struct term *term);

void mouse_concat_selections(struct term *term, struct line *dst, struct line *src);
void mouse_realloc_selections(struct term *term, struct line *line, bool cut);
void mouse_free_selections(struct term *term, struct line *line);
void mouse_line_changed(struct term *term, struct line *line, int16_t x0, int16_t x1, bool damage);

inline static bool mouse_has_selection(struct term *term) {
    struct mouse_state *loc = term_get_mstate(term);
    return loc->state != state_sel_none &&
           loc->state != state_sel_pressed;
}


#endif

