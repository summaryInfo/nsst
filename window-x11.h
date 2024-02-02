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
#endif
#if USE_XRENDER
#   include <xcb/render.h>
#endif

#define TRUE_COLOR_ALPHA_DEPTH 32

struct platform_window {
    xcb_window_t wid;
    xcb_gcontext_t gc;
    xcb_event_mask_t ev_mask;

    union {
#if USE_X11SHM
        struct {
            xcb_shm_seg_t shm_seg;
            xcb_pixmap_t shm_pixmap;
            struct image im;

            /* It's size is 2*win->ch */
            struct rect *bounds;
            size_t boundc;
        };
#endif
#if USE_XRENDER
        struct {
            /* Active IDs, actual X11 objects */
            xcb_pixmap_t pid1;
            xcb_render_picture_t pic1;
            /* Cached IDs, used for copying */
            xcb_pixmap_t pid2;
            xcb_render_picture_t pic2;

            xcb_pixmap_t glyph_pid;

            xcb_render_picture_t pen;
            xcb_render_glyphset_t gsid;
            xcb_render_pictformat_t pfglyph;
        };
#endif
    };

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
        return true;
    }
    free(err);
    return false;
}

#if USE_X11SHM
void x11_shm_init_context(void);
void x11_shm_free_context(void);
void x11_shm_free(struct window *win);
void x11_shm_update(struct window *win, struct rect rect);
bool x11_shm_reload_font(struct window *win, bool need_free);
void x11_shm_resize(struct window *win, int16_t new_cw, int16_t new_ch);
void x11_shm_copy(struct window *win, struct rect dst, int16_t sx, int16_t sy);
void x11_shm_recolor_border(struct window *win);
bool x11_shm_submit_screen(struct window *win, int16_t cur_x, ssize_t cur_y, bool cursor, bool marg);
#endif

#if USE_XRENDER
void x11_xrender_init_context(void);
void x11_xrender_free_context(void);
void x11_xrender_free(struct window *win);
void x11_xrender_update(struct window *win, struct rect rect);
bool x11_xrender_reload_font(struct window *win, bool need_free);
void x11_xrender_resize(struct window *win, int16_t new_cw, int16_t new_ch);
void x11_xrender_copy(struct window *win, struct rect dst, int16_t sx, int16_t sy);
void x11_xrender_recolor_border(struct window *win);
bool x11_xrender_submit_screen(struct window *win, int16_t cur_x, ssize_t cur_y, bool cursor, bool marg);
#endif

void x11_update_window_props(struct window *win);

#endif
