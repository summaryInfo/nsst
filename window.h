/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#ifndef WINDOW_H_
#define WINDOW_H_ 1

#include "feature.h"

#include "font.h"
#include "util.h"

#include <stddef.h>
#include <stdint.h>

/* WARNING: Order is important */
typedef enum nss_cursor_type {
    nss_cursor_block_blink = 1,
    nss_cursor_block = 2,
    nss_cursor_underline_blink = 3,
    nss_cursor_underline = 4,
    nss_cursor_bar_blink = 5,
    nss_cursor_bar = 6,
} nss_cursor_type_t;

/* WARNING: Order is important */
typedef enum nss_mouse_event {
    nss_me_press,
    nss_me_release,
    nss_me_motion,
} nss_mouse_event_t;

/* WARNING: Order is important */
typedef enum nss_mouse_state {
    nss_ms_shift = 1 << 0,
    nss_ms_lock = 1 << 1,
    nss_ms_control = 1 << 2,
    nss_ms_mod_1 = 1 << 3,
    nss_ms_mod_2 = 1 << 4,
    nss_ms_mod_3 = 1 << 5,
    nss_ms_mod_4 = 1 << 6,
    nss_ms_mod_5 = 1 << 7,
    nss_ms_button_1 = 1 << 8,
    nss_ms_button_2 = 1 << 9,
    nss_ms_button_3 = 1 << 10,
    nss_ms_button_4 = 1 << 11,
    nss_ms_button_5 = 1 << 12,
    nss_ms_state_mask = 0x1FFF,
    nss_ms_modifer_mask = 0xFF,
} nss_mouse_state_t;

typedef enum nss_clipboard_target {
    nss_ct_primary,
    nss_ct_clipboard,
    nss_ct_secondary,
    nss_ct_MAX,
} nss_clipboard_target_t;

typedef enum nss_title_target {
    nss_tt_title = 1,
    nss_tt_icon_label = 2,
} nss_title_target_t;

typedef struct nss_window nss_window_t;
typedef struct nss_line_iter nss_line_iter_t;
typedef uint32_t nss_color_t;
typedef int16_t nss_coord_t;

void nss_init_context(void);
void nss_free_context(void);
void nss_context_run(void);

nss_window_t *nss_create_window();
void nss_free_window(nss_window_t *win);

_Bool nss_window_submit_screen(nss_window_t *win, nss_line_iter_t *it, nss_color_t *palette, nss_coord_t cur_x, nss_coord_t cur_y, _Bool cursor);
void nss_window_shift(nss_window_t *win, nss_coord_t ys, nss_coord_t yd, nss_coord_t height, _Bool delay);
void nss_window_paste_clip(nss_window_t *win, nss_clipboard_target_t target);
void nss_window_delay(nss_window_t *win);

void nss_window_set_title(nss_window_t *win, nss_title_target_t which, const char *name, _Bool utf8);
void nss_window_push_title(nss_window_t *win, nss_title_target_t which);
void nss_window_pop_title(nss_window_t *win, nss_title_target_t which);
/* Both at the same time are not supported */
char *nss_window_get_title(nss_window_t *win, nss_title_target_t which);

void nss_window_set_mouse(nss_window_t *win, _Bool enabled);
void nss_window_set_colors(nss_window_t *win, nss_color_t bg, nss_color_t cursor_fg);
void nss_window_set_cursor(nss_window_t *win, nss_cursor_type_t type);
void nss_window_get_dim(nss_window_t *win, int16_t *width, int16_t *height);
nss_cursor_type_t nss_window_get_cursor(nss_window_t *win);
void nss_window_set_clip(nss_window_t *win, uint8_t *data, uint32_t time, nss_clipboard_target_t target);
void nss_window_set_sync(nss_window_t *win, _Bool state);

#define NSS_TIME_NOW 0

#endif
