#ifndef WINDOW_H_
#define WINDOW_H_ 1

#include <stddef.h>
#include <stdint.h>

#include "font.h"
#include "util.h"

typedef uint32_t nss_color_t;

typedef enum nss_text_attrib_flags {
    nss_attrib_italic = 1 << 0, //done
    nss_attrib_bold = 1 << 1, //done
    nss_attrib_faint = 1 << 2, //ignored
    nss_attrib_underlined = 1 << 3, //done
    nss_attrib_strikethrough = 1 << 4, //done
    nss_attrib_overlined = 1 << 5, //ignored
    nss_attrib_inverse = 1 << 6, //done
    nss_attrib_background = 1 << 7, //done
    nss_attrib_cursor = 1 << 8, //done
} nss_attrib_flags_t;

typedef struct nss_text_attrib {
    nss_color_t fg;
    nss_color_t bg;
    nss_attrib_flags_t flags;
} nss_text_attrib_t;

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
} nss_wc_tag_t;

typedef struct nss_window nss_window_t;
typedef struct nss_context nss_context_t;

nss_context_t *nss_create_context(void);
void nss_free_context(nss_context_t *con);
uint16_t nss_context_get_dpi(nss_context_t *con);
void nss_context_run(nss_context_t *con);

nss_window_t *nss_create_window(nss_context_t *con, nss_rect_t rect, const char *font_name, nss_wc_tag_t tag, const uint32_t *values);
void nss_free_window(nss_context_t *con, nss_window_t *win);
void nss_window_draw_ucs4(nss_context_t *con, nss_window_t *win, size_t len, const uint32_t *ch, const nss_text_attrib_t *attr, int16_t x, int16_t y);
void nss_window_update(nss_context_t *con, nss_window_t *win, size_t len, const nss_rect_t *damage);
void nss_window_clear(nss_context_t *con, nss_window_t *win, size_t len, const nss_rect_t *damage);
void nss_window_set(nss_context_t *con, nss_window_t *win, nss_wc_tag_t tag, const uint32_t *values);
void nss_window_set_font(nss_context_t *con, nss_window_t *win, const char *name);
void nss_window_set_title(nss_context_t *con, nss_window_t *win, const char *name);
nss_font_t *nss_window_get_font(nss_context_t *con, nss_window_t *win);
char *nss_window_get_font_name(nss_context_t *con, nss_window_t *win);
uint32_t nss_window_get(nss_context_t *con, nss_window_t *win, nss_wc_tag_t tag);

#endif
