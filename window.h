#ifndef WINDOW_H_
#define WINDOW_H_ 1

#include <stddef.h>
#include <stdint.h>

#include "font.h"
#include "util.h"
#include "attr.h"

#define NSS_WIN_FPS 120

typedef enum nss_cursor_type {
    nss_cursor_block = 2,
    nss_cursor_bar = 4,
    nss_cursor_underline = 6,
} nss_cursor_type_t;

typedef enum nss_wc_tag {
    nss_wc_cusror_width = 1 << 0,
    nss_wc_left_border = 1 << 1,
    nss_wc_top_border = 1 << 2,
    nss_wc_background = 1 << 3,
    nss_wc_foreground = 1 << 4,
    nss_wc_cursor_background = 1 << 5,
    nss_wc_cursor_foreground = 1 << 6,
    nss_wc_cursor_type = 1 << 7,
    nss_wc_subpixel_fonts = 1 << 8,
    nss_wc_font_size = 1 << 9,
    nss_wc_underline_width = 1 << 10,
    nss_wc_width = 1 << 11,
    nss_wc_height = 1 << 12,
    nss_wc_appkey = 1 << 13,
    nss_wc_appcursor = 1 << 14,
    nss_wc_numlock = 1 << 15,
    nss_wc_keylock = 1 << 16,
    nss_wc_has_meta = 1 << 17,
    nss_wc_blink_time = 1 << 18,
    nss_wc_reverse = 1 << 19,
    nss_wc_mouse = 1 << 20,
} nss_wc_tag_t;

typedef enum nss_mouse_event {
	nss_me_motion,
	nss_me_press,
	nss_me_release
} nss_mouse_event_t;

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
    nss_ms_button_5 = 1 << 12
} nss_mouse_state_t;

typedef struct nss_window nss_window_t;

void nss_init_context(void);
void nss_free_context(void);
uint16_t nss_context_get_dpi(void);
void nss_context_run(void);

nss_window_t *nss_create_window(const char *font_name, nss_wc_tag_t tag, const uint32_t *values);
void nss_free_window(nss_window_t *win);
void nss_window_draw(nss_window_t *win, int16_t x, int16_t y, size_t len, nss_cell_t *cells, nss_color_t *pal, nss_color_t *extra);
void nss_window_draw_commit(nss_window_t *win);
void nss_window_draw_cursor(nss_window_t *win, int16_t x, int16_t y, nss_cell_t *cell, nss_color_t *pal, nss_color_t *extra);
void nss_window_update(nss_window_t *win, size_t len, const nss_rect_t *damage);
void nss_window_clear(nss_window_t *win, size_t len, const nss_rect_t *damage);
void nss_window_shift(nss_window_t *win, int16_t ys, int16_t yd, int16_t height);
void nss_window_set(nss_window_t *win, nss_wc_tag_t tag, const uint32_t *values);
void nss_window_set_font(nss_window_t *win, const char *name);
void nss_window_set_title(nss_window_t *win, const char *name);
nss_font_t *nss_window_get_font(nss_window_t *win);
char *nss_window_get_font_name(nss_window_t *win);
uint32_t nss_window_get(nss_window_t *win, nss_wc_tag_t tag);

#endif
