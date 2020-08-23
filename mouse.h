/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#ifndef MOUSE_H_
#define MOUSE_H_ 1

#include "term.h"

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
        nss_ssnap_none,
        nss_ssnap_word,
        nss_ssnap_line,
    } snap;

    enum {
        nss_sstate_none,
        nss_sstate_pressed = nss_me_press + 1,
        nss_sstate_released = nss_me_release + 1,
        nss_sstate_progress = nss_me_motion + 1,
    } state;

    struct timespec click0;
    struct timespec click1;

    nss_clipboard_target_t targ;

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

    enum nss_mouse_mode {
        nss_mouse_mode_none,
        nss_mouse_mode_x10,
        nss_mouse_mode_button,
        nss_mouse_mode_drag,
        nss_mouse_mode_motion,
    } mouse_mode;

    enum nss_mouse_format {
        nss_mouse_format_default,
        nss_mouse_format_sgr,
        nss_mouse_format_utf8,
        nss_mouse_format_uxvt
    } mouse_format;
} nss_mouse_state_t;

void nss_handle_mouse(nss_term_t *term, nss_mouse_event_t ev);
void nss_mouse_scroll_selection(nss_term_t *term, nss_coord_t amount, _Bool save);
void nss_mouse_scroll_view(nss_term_t *term, ssize_t delta);
_Bool nss_mouse_is_selected(nss_term_t *term, nss_coord_t x, nss_coord_t y);
_Bool nss_mouse_is_selected_in_view(nss_term_t *term, nss_coord_t x, nss_coord_t y);
void nss_mouse_clear_selection(nss_term_t *term);
void nss_mouse_damage_selection(nss_term_t *term);
void nss_mouse_selection_erase(nss_term_t *term, nss_rect_t rect);
void nss_mouse_report_locator(nss_term_t *term, uint8_t evt, int16_t x, int16_t y, uint32_t mask);
void nss_mouse_set_filter(nss_term_t *term, nss_sparam_t xs, nss_sparam_t xe, nss_sparam_t ys, nss_sparam_t ye);

#endif

