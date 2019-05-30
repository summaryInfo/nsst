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

typedef struct nss_window nss_window_t;
typedef struct nss_context nss_context_t;

nss_context_t *nss_create_context(void);
void nss_free_context(nss_context_t *con);
uint16_t nss_context_get_dpi(nss_context_t *con);
void nss_context_run(nss_context_t *con);

nss_window_t *nss_create_window(nss_context_t *con, nss_rect_t rect, nss_font_t *font);
void nss_free_window(nss_context_t *con, nss_window_t *win);
void nss_window_draw_ucs4(nss_context_t *con, nss_window_t *win, size_t len,  uint32_t *ch, nss_text_attrib_t *attr, int16_t x, int16_t y);
void nss_window_update(nss_context_t *con, nss_window_t *win, size_t len, nss_rect_t *damage);

#endif
