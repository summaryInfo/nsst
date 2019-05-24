#ifndef WINDOW_H_ 
#define WINDOW_H_ 1

#include <stddef.h>
#include <xcb/xcb.h>


typedef struct nss_window {
    xcb_window_t wid;
    xcb_pixmap_t pid;
	xcb_gcontext_t gc;
	uint16_t w,h;
	_Bool focused;
    struct nss_window *prev, *next;
} nss_window_t;

typedef struct nss_context {
    size_t refs;
	xcb_connection_t* con;
	xcb_screen_t* screen;
    xcb_colormap_t mid;
	xcb_visualtype_t* vis;
	nss_window_t *first;
	time_t start_time; 
} nss_context_t;

typedef struct nss_geometry {
	uint32_t x,y;
	uint32_t w,h; 
} nss_geometry_t;

typedef uint32_t nss_color_t;
typedef struct nss_glyph nss_glyph_t;

typedef enum nss_glyph_attrib_flags {
	nss_attrib_bold = 2 << 0,
	nss_attrib_faint = 2 << 1,
	nss_attrib_italic = 2 << 2,
	nss_attrib_underlined = 2 << 3,
	nss_attrib_strikethrough = 2 << 4,
	nss_attrib_overlined = 2 << 5,
	nss_attrib_inverse = 2 << 6,
	nss_attrib_blink = 2 << 7,
	nss_attrib_background = 2 << 8,
	nss_attrib_cursor = 2 << 9,
} nss_attrib_flags_t;

typedef struct nss_glyph_attrib {
	nss_color_t fg;
	nss_color_t bg;
	nss_attrib_flags_t flags;
} nss_glyph_attrib_t;

/*
typedef struct nss_window_config {
	uint16_t line_skip;
	uint16_t glyph_skip;
	uint16_t char_width;
	uint16_t char_height;
	uint8_t cursor_type; // block, half, vline, snowman 
	uint8_t resize_centered;
	uint16_t border_width;
	uint16_t alpha;
	nss_color_t bg;
	nss_color_t fg;
} nss_window_config_t;
*/

void nss_win_initialize(nss_context_t* con);
void nss_win_free_windows(nss_context_t* con);

nss_window_t* nss_win_add_window(nss_context_t *con, nss_geometry_t *geo);
void nss_win_remove_window(nss_context_t *con, nss_window_t *win);
//nss_window_config_t* nss_window_config(nss_context_t *con, nss_window_t *win, nss_window_config_t *config);

//void nss_window_glyph_render(nss_context_t *con, nss_window_t* win, 
//                             uint16_t x, uint16_t y, nss_glyph_attrib_t *attr, nss_glyph_t* glyph);
//void nss_window_update(nss_context_t *con, nss_window_t *win);
//void nss_window_set_render_callback(nss_context_t *con, nss_window_t *win, void(*fn)(nss_context_t *con, nss_window_t* win));

void nss_win_run(nss_context_t* con);

#endif
