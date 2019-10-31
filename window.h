#ifndef WINDOW_H_
#define WINDOW_H_ 1

#include <stddef.h>
#include <stdint.h>

#include "font.h"
#include "util.h"
#include "attr.h"

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
    nss_wc_lcd_mode = 1 << 8,
    nss_wc_font_size = 1 << 9,
    nss_wc_underline_width = 1 << 10,
    nss_wc_width = 1 << 11,
    nss_wc_height = 1 << 12,
    // TODO
    nss_wc_appkey = 1 << 13,
    nss_wc_appcursor = 1 << 14,
    nss_wc_numlock = 1 << 15,
    nss_wc_8bit = 1 << 16,
    nss_wc_inverted = 1 << 17,
    nss_wc_mouse = 1 << 18, // Report mouse
    nss_wc_hide_cursor = 1 << 19 // Don't render cursor
} nss_wc_tag_t;

typedef struct nss_window nss_window_t;

void nss_init_context(void);
void nss_free_context(void);
uint16_t nss_context_get_dpi(void);
void nss_context_run(void);

nss_window_t *nss_create_window(nss_rect_t rect, const char *font_name, nss_wc_tag_t tag, const uint32_t *values);
void nss_free_window(nss_window_t *win);
void nss_window_draw(nss_window_t *win, int16_t x, int16_t y, size_t len, nss_cell_t *cells);
void nss_window_draw_commit(nss_window_t *win);
void nss_window_draw_cursor(nss_window_t *win, int16_t x, int16_t y, nss_cell_t *cell);
void nss_window_update(nss_window_t *win, size_t len, const nss_rect_t *damage);
void nss_window_clear(nss_window_t *win, size_t len, const nss_rect_t *damage);
void nss_window_set(nss_window_t *win, nss_wc_tag_t tag, const uint32_t *values);
void nss_window_set_font(nss_window_t *win, const char *name);
void nss_window_set_title(nss_window_t *win, const char *name);
nss_font_t *nss_window_get_font(nss_window_t *win);
char *nss_window_get_font_name(nss_window_t *win);
uint32_t nss_window_get(nss_window_t *win, nss_wc_tag_t tag);

#endif
