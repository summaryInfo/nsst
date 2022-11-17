#ifndef WINDOW_PLATFORM_X11_H_
#define WINDOW_PLATFORM_X11_H_

#include "feature.h"

#include "util.h"
#if USE_X11SHM
#   include "image.h"
#endif
#include  "window-impl.h"

#include <inttypes.h>
#include <stdbool.h>
#include <xcb/xcb.h>
#if USE_X11SHM
#   include <xcb/shm.h>
#else
#   include <xcb/render.h>
#endif

#define TRUE_COLOR_ALPHA_DEPTH 32

struct platform_window {
    xcb_window_t wid;
    xcb_gcontext_t gc;
    xcb_event_mask_t ev_mask;

#if USE_X11SHM
    xcb_shm_seg_t shm_seg;
    xcb_pixmap_t shm_pixmap;
    struct image im;

    /* It's size is 2*win->ch */
    struct rect *bounds;
    size_t boundc;
#else
    /* Active IDs, actual X11 objects */
    xcb_pixmap_t pid1;
    xcb_render_picture_t pic1;
    /* Cached IDs, used for copying */
    xcb_pixmap_t pid2;
    xcb_render_picture_t pic2;

    xcb_render_picture_t pen;
    xcb_render_glyphset_t gsid;
    xcb_render_pictformat_t pfglyph;
#endif

    /* Used to restore maximized window */
    struct rect saved;
} ALIGNED(MALLOC_ALIGNMENT);

extern xcb_connection_t *con;

inline static struct platform_window *get_plat(struct window *win) {
    return (struct platform_window *)win->platform_window_opaque;
}

inline static bool check_void_cookie(xcb_void_cookie_t ck) {
    xcb_generic_error_t *err = xcb_request_check(con, ck);
    if (err) {
        warn("[X11 Error] major=%"PRIu8", minor=%"PRIu16", error=%"PRIu8,
                err->major_code, err->minor_code, err->error_code);
        return 1;
    }
    free(err);
    return 0;
}

/* Renderer dependent functions */
void x11_init_render_context(void);
void x11_free_render_context(void);
void x11_renderer_free(struct window *win);
void x11_renderer_update(struct window *win, struct rect rect);
bool x11_renderer_reload_font(struct window *win, bool need_free);
void x11_renderer_resize(struct window *win, int16_t new_cw, int16_t new_ch);
void x11_renderer_copy(struct window *win, struct rect dst, int16_t sx, int16_t sy);
void x11_renderer_recolor_border(struct window *win);
bool x11_renderer_submit_screen(struct window *win, int16_t cur_x, ssize_t cur_y, bool cursor, bool marg);

void x11_update_window_props(struct window *win);

#endif
