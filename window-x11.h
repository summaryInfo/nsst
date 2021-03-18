/* Copyright (c) 2019-2021, Evgeny Baskov. All rights reserved */

#ifndef WINDOW_X11_H_
#define WINDOW_X11_H_ 1

#include "feature.h"

#include "window.h"
#include "config.h"
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

struct renderer {
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

struct cellspec {
    color_t fg;
    color_t bg;
    uint32_t ch;
    uint8_t face;
    bool underlined;
    bool stroke;
    bool wide;
};

struct title_stack_item {
    struct title_stack_item *next;
    char *title_data;
    char *icon_data;
    bool title_utf8;
    bool icon_utf8;
    char data[];
};

struct render_cell_state {
    color_t *palette;
    uint32_t active_uri : 23;
    uint32_t dummy_ : 7;
    bool blink : 1;
    bool uri_pressed : 1;
};

struct window {
    struct window *prev, *next;

    xcb_window_t wid;
    int32_t screen;
    xcb_gcontext_t gc;
    xcb_event_mask_t ev_mask;

    bool focused : 1;
    bool active : 1;
    bool mouse_events : 1;
    bool force_redraw : 1;
    bool blink_commited : 1;
    bool drawn_somthing : 1;
    bool sync_active : 1;
    bool slow_mode : 1;
    bool in_blink : 1;
    bool init_invert : 1;
    bool wait_for_redraw : 1;
    bool autorepeat : 1;
    bool uri_damaged : 1;

    struct timespec last_scroll ALIGNED(16);
    struct timespec last_shift ALIGNED(16);
    struct timespec last_blink ALIGNED(16);
    struct timespec last_sync ALIGNED(16);
    struct timespec last_read ALIGNED(16);
    struct timespec last_wait_start ALIGNED(16);
    struct timespec last_draw ALIGNED(16);
    struct timespec vbell_start ALIGNED(16);
    int16_t damaged_y0;
    int16_t damaged_y1;

    // Used to restore maximized window
    bool saved_geometry;
    int16_t saved_x;
    int16_t saved_y;
    int16_t saved_width;
    int16_t saved_height;

    color_t bg;
    color_t bg_premul;
    color_t cursor_fg;

    uint8_t *clipped[clip_MAX];
    uint8_t *clipboard;

    int16_t cw;
    int16_t ch;
    int16_t char_width;
    int16_t char_depth;
    int16_t char_height;
    struct font *font;
    struct glyph_cache *font_cache;
    enum pixel_mode font_pixmode;

    struct term *term;
    struct render_cell_state rcstate;
    size_t poll_index;

    struct title_stack_item *title_stack;

    struct renderer ren;

    // Window configuration
    struct instance_config cfg;
};

extern xcb_connection_t *con;
extern struct window *win_list_head;

inline static bool check_void_cookie(xcb_void_cookie_t ck) {
    xcb_generic_error_t *err = xcb_request_check(con, ck);
    if (err) {
        warn("[X11 Error] major=%"PRIu8", minor=%"PRIu16", error=%"PRIu8, err->major_code, err->minor_code, err->error_code);
        return 1;
    }
    free(err);
    return 0;
}

void init_render_context(void);
void free_render_context(void);
void renderer_free(struct window *win);
void renderer_update(struct window *win, struct rect rect);
 bool renderer_reload_font(struct window *win, bool need_free);
void renderer_resize(struct window *win, int16_t new_cw, int16_t new_ch);
void renderer_copy(struct window *win, struct rect dst, int16_t sx, int16_t sy);

void window_set_default_props(struct window *win);
void handle_resize(struct window *win, int16_t width, int16_t height);
struct window *find_shared_font(struct window *win, bool need_free);
struct cellspec describe_cell(struct cell cell, struct attr attr, struct instance_config *cfg, struct render_cell_state *rcs, bool selected);

#endif

