/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#ifndef WINDOW_X11_H_
#define WINDOW_X11_H_ 1

#include "feature.h"

#include "window.h"
#include "term.h"
#if USE_X11SHM
#   include "image.h"
#endif

#include <inttypes.h>
#include <stdbool.h>
#include <xcb/xcb.h>
#if USE_X11SHM
#   include <xcb/shm.h>
#else
#   include <xcb/render.h>
#endif

#define TRUE_COLOR_ALPHA_DEPTH 32

typedef struct nss_renderer nss_renderer_t;

struct nss_renderer {
#if USE_X11SHM
    xcb_shm_seg_t shm_seg;
    xcb_pixmap_t shm_pixmap;
    struct image im;

    // It's size is 2*win->ch
    struct rect *bounds;
    size_t boundc;
#else
    // Active IDs, actual X11 objects
    xcb_pixmap_t pid1;
    xcb_render_picture_t pic1;
    // Cached IDs, used for copying
    xcb_pixmap_t pid2;
    xcb_render_picture_t pic2;

    xcb_render_picture_t pen;
    xcb_render_glyphset_t gsid;
    xcb_render_pictformat_t pfglyph;
#endif
};

struct nss_cellspec {
    color_t fg;
    color_t bg;
    term_char_t ch;
    uint8_t face;
    _Bool underlined;
    _Bool stroke;
    _Bool wide;
};

typedef struct nss_title_stack_item {
    struct nss_title_stack_item *next;
    char *title_data;
    char *icon_data;
    _Bool title_utf8;
    _Bool icon_utf8;
    char data[];
} nss_title_stack_item_t;

struct nss_window {
    struct nss_window *prev, *next;

    xcb_window_t wid;
    int32_t screen;
    xcb_gcontext_t gc;
    xcb_event_mask_t ev_mask;

    unsigned focused : 1;
    unsigned active : 1;
    unsigned blink_state : 1;
    unsigned mouse_events : 1;
    unsigned force_redraw : 1;
    unsigned blink_commited : 1;
    unsigned scroll_delayed : 1;
    unsigned resize_delayed : 1;
    unsigned drawn_somthing : 1;
    unsigned sync_active : 1;
    unsigned slow_mode : 1;
    unsigned bell_urgent : 1;
    unsigned bell_raise : 1;
    unsigned in_blink : 1;
    unsigned init_invert : 1;

    int16_t width;
    int16_t height;
    nss_coord_t cw, ch;
    int16_t cursor_width;
    int16_t underline_width;
    int16_t left_border;
    int16_t top_border;
    int16_t font_size;
    struct timespec last_blink;
    struct timespec last_scroll;
    struct timespec last_resize;
    struct timespec last_sync;
    struct timespec next_draw;
    struct timespec vbell_start;
    nss_coord_t damaged_y0;
    nss_coord_t damaged_y1;

    // Used to restore maximized window
    _Bool saved_geometry;
    int16_t saved_x;
    int16_t saved_y;
    int16_t saved_width;
    int16_t saved_height;

    color_t bg;
    color_t cursor_fg;
    nss_cursor_type_t cursor_type;

    uint8_t *clipped[clip_MAX];
    uint8_t *clipboard;

    int16_t char_width;
    int16_t char_depth;
    int16_t char_height;
    char *font_name;
    nss_font_t *font;
    nss_glyph_cache_t *font_cache;
    nss_pixel_mode_t font_pixmode;

    nss_term_t *term;
    size_t poll_index;

    nss_title_stack_item_t *title_stack;

    nss_renderer_t ren;
};

extern xcb_connection_t *con;
extern nss_window_t *win_list_head;

inline static _Bool check_void_cookie(xcb_void_cookie_t ck) {
    xcb_generic_error_t *err = xcb_request_check(con, ck);
    if (err) {
        warn("[X11 Error] major=%"PRIu8", minor=%"PRIu16", error=%"PRIu8, err->major_code, err->minor_code, err->error_code);
        return 1;
    }
    free(err);
    return 0;
}

void nss_init_render_context(void);
void nss_free_render_context(void);
void nss_renderer_free(nss_window_t *win);
void nss_renderer_update(nss_window_t *win, struct rect rect);
 _Bool nss_renderer_reload_font(nss_window_t *win, _Bool need_free);
void nss_renderer_resize(nss_window_t *win, int16_t new_cw, int16_t new_ch);
void nss_renderer_copy(nss_window_t *win, struct rect dst, int16_t sx, int16_t sy);

void nss_window_set_default_props(nss_window_t *win);
void nss_window_handle_resize(nss_window_t *win, int16_t width, int16_t height);
nss_window_t *nss_find_shared_font(nss_window_t *win, _Bool need_free);
struct nss_cellspec nss_describe_cell(nss_cell_t cell, color_t *palette, color_t *extra, _Bool blink, _Bool selected);

#endif

