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
        state_sel_pressed = nss_me_press + 1,
        state_sel_released = nss_me_release + 1,
        state_sel_progress = nss_me_motion + 1,
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

    nss_rect_t filter;

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

void mouse_handle_input(nss_term_t *term, struct mouse_event ev);
void mouse_scroll_selection(nss_term_t *term, nss_coord_t amount, _Bool save);
void mouse_scroll_view(nss_term_t *term, ssize_t delta);
_Bool mouse_is_selected(nss_term_t *term, nss_coord_t x, nss_coord_t y);
_Bool mouse_is_selected_in_view(nss_term_t *term, nss_coord_t x, nss_coord_t y);
void mouse_clear_selection(nss_term_t *term);
void mouse_damage_selection(nss_term_t *term);
void mouse_selection_erase(nss_term_t *term, nss_rect_t rect);
void mouse_report_locator(nss_term_t *term, uint8_t evt, int16_t x, int16_t y, uint32_t mask);
void mouse_set_filter(nss_term_t *term, nss_sparam_t xs, nss_sparam_t xe, nss_sparam_t ys, nss_sparam_t ye);

#endif

