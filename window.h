#ifndef WINDOW_H_ 
#define WINDOW_H_ 1

#include <stddef.h>
#include <stdint.h>

#include "font.h"

typedef uint32_t nss_color_t;

typedef struct nss_geometry {
	uint32_t x,y;
	uint32_t w,h; 
} nss_geometry_t;

typedef struct nss_window nss_window_t;
typedef struct nss_context nss_context_t;

typedef enum nss_text_attrib_flags {
	nss_attrib_italic = 1 << 0, //done
	nss_attrib_bold = 1 << 1, //done
	nss_attrib_faint = 1 << 2, //ignored

	nss_attrib_underlined = 1 << 3, //done
	nss_attrib_strikethrough = 1 << 4, //done
	nss_attrib_overlined = 1 << 5, //ignored
	nss_attrib_inverse = 1 << 6, //done
	nss_attrib_blink = 1 << 7, //done
	nss_attrib_background = 1 << 8, //done
} nss_attrib_flags_t;

typedef struct nss_text_attrib {
	nss_color_t fg;
	nss_color_t bg;
	nss_attrib_flags_t flags;
} nss_text_attrib_t;

nss_context_t* nss_win_create(void);
void nss_win_free(nss_context_t* con);

nss_window_t* nss_win_add_window(nss_context_t *con, nss_geometry_t *geo, nss_font_t *font);
void nss_win_remove_window(nss_context_t *con, nss_window_t *win);
void nss_win_run(nss_context_t* con);

void nss_win_render_ucs4(nss_context_t* con, nss_window_t* win, size_t len,  uint32_t *ch, nss_text_attrib_t *attr, uint16_t x, uint16_t y);
uint16_t nss_win_get_dpi(nss_context_t* con);

#endif
