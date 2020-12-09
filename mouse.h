/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#ifndef MOUSE_H_
#define MOUSE_H_ 1

#include "term.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

struct selected {
    int16_t x0;
    ssize_t y0;
    int16_t x1;
    ssize_t y1;
    bool rect;
};

struct mouse_state {
    struct selected r;
    struct selected n;

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

    ssize_t y;
    int16_t x;
    uint8_t button;

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

void mouse_handle_input(struct term *term, struct mouse_event ev);
void mouse_scroll_selection(struct term *term, ssize_t amount, bool save);
void mouse_scroll_view(struct term *term, ssize_t delta);
bool mouse_is_selected(struct term *term, int16_t x, ssize_t y);
bool mouse_is_selected_2(struct term *term, int16_t x0, int16_t x1, ssize_t y);
bool mouse_is_selected_in_view(struct term *term, int16_t x, ssize_t y);
void mouse_clear_selection(struct term *term);
void mouse_damage_selection(struct term *term);
void mouse_selection_erase(struct term *term, struct rect rect);
void mouse_report_locator(struct term *term, uint8_t evt, int16_t x, int16_t y, uint32_t mask);
void mouse_set_filter(struct term *term, iparam_t xs, iparam_t xe, iparam_t ys, iparam_t ye);
bool mouse_pending_scroll(struct term *term);

#endif

