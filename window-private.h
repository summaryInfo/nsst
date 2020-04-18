#ifndef WINDOW_PRIVATE_H_
#define WINDOW_PRIVATE_H_ 1

#include "features.h"
#include "window.h"
#include "term.h"
#ifdef USE_X11SHM
#   include "image.h"
#endif

#include <inttypes.h>
#include <xcb/xcb.h>
#ifdef USE_X11SHM
#   include <xcb/shm.h>
#endif

#define TRUE_COLOR_ALPHA_DEPTH 32

typedef struct nss_renderer nss_renderer_t;

struct nss_renderer {
    xcb_gcontext_t gc;
#ifdef USE_X11SHM
    xcb_shm_seg_t shm_seg;
    xcb_pixmap_t shm_pixmap;

    nss_image_t im;
    nss_glyph_cache_t *cache;

    // It's size is 2*win->ch
    nss_rect_t *bounds;
    size_t boundc;
#endif
};

struct nss_window {
    struct nss_window *prev, *next;

    xcb_window_t wid;
    xcb_event_mask_t ev_mask;

    unsigned focused : 1;
    unsigned active : 1;
    unsigned subpixel_fonts : 1;
    unsigned got_configure : 1;
    unsigned blink_state : 1;
    unsigned mouse_events : 1;
    unsigned force_redraw : 1;
    unsigned blink_commited : 1;

    int16_t width;
    int16_t height;
    coord_t cw, ch;
    int16_t cursor_width;
    int16_t underline_width;
    int16_t left_border;
    int16_t top_border;
    int16_t font_size;
    uint32_t blink_time;
    struct timespec last_blink;
    struct timespec last_scroll;
    struct timespec last_draw;

    nss_color_t bg;
    nss_color_t cursor_fg;
    nss_cursor_type_t cursor_type;

    int16_t char_width;
    int16_t char_depth;
    int16_t char_height;
    char *font_name;
    nss_font_t *font;

    nss_term_t *term;
    int term_fd;

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
void nss_renderer_update(nss_window_t *win, nss_rect_t rect);
void nss_renderer_clear(nss_window_t *win, size_t count, nss_rect_t *rects);
void nss_renderer_background_changed(nss_window_t *win);
 _Bool nss_renderer_reload_font(nss_window_t *win, _Bool need_free);
void nss_renderer_resize(nss_window_t *win, int16_t new_cw, int16_t new_ch);
void nss_renderer_copy(nss_window_t *win, nss_rect_t dst, int16_t sx, int16_t sy);

#endif

