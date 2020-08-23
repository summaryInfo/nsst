/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#ifndef MOUSE_H_
#define MOUSE_H_ 1

#include "term.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

struct selected {
    nss_coord_t x0;
    ssize_t y0;
    nss_coord_t x1;
    ssize_t y1;
    _Bool rect;
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

    enum clip_target targ;

    uint8_t button;
    nss_coord_t x;
    nss_coord_t y;

    uint32_t locator_enabled : 1;
    uint32_t locator_oneshot : 1;
    uint32_t locator_filter : 1;
    uint32_t locator_pixels : 1;
    uint32_t locator_report_press : 1;
    uint32_t locator_report_release : 1;

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
        mouse_format_uxvt
    } mouse_format;
};

void mouse_handle_input(struct term *term, struct mouse_event ev);
void mouse_scroll_selection(struct term *term, nss_coord_t amount, _Bool save);
void mouse_scroll_view(struct term *term, ssize_t delta);
_Bool mouse_is_selected(struct term *term, nss_coord_t x, nss_coord_t y);
_Bool mouse_is_selected_in_view(struct term *term, nss_coord_t x, nss_coord_t y);
void mouse_clear_selection(struct term *term);
void mouse_damage_selection(struct term *term);
void mouse_selection_erase(struct term *term, struct rect rect);
void mouse_report_locator(struct term *term, uint8_t evt, int16_t x, int16_t y, uint32_t mask);
void mouse_set_filter(struct term *term, iparam_t xs, iparam_t xe, iparam_t ys, iparam_t ye);

#endif

