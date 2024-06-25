/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */


#include "feature.h"

/* Make linting always work for this
 * file (force choosing the right renderer
 * structure variant in window-impl.h)*/
#undef USE_X11SHM
#define USE_X11SHM 1

#include "config.h"
#include "font.h"
#include "mouse.h"
#include "window-impl.h"
#include "window-x11.h"

#include <stdbool.h>
#include <string.h>
#include <xcb/shm.h>
#include <xcb/xcb.h>

static bool has_shm_pixmaps;
extern bool has_fast_damage;

/* Returns old image */
struct image x11_shm_create_image(struct window *win, int16_t width, int16_t height) {
    struct image old = get_plat(win)->shm.im;
    xcb_void_cookie_t c;

    if (has_fast_damage) {
        get_plat(win)->shm.im = create_shm_image(width, height);

        if (!get_plat(win)->shm_seg) {
            get_plat(win)->shm_seg = xcb_generate_id(con);
        } else {
            if (has_shm_pixmaps && get_plat(win)->shm_pixmap)
                xcb_free_pixmap(con, get_plat(win)->shm_pixmap);
            xcb_shm_detach_checked(con, get_plat(win)->shm_seg);
        }

#if USE_POSIX_SHM
        c = xcb_shm_attach_fd_checked(con, get_plat(win)->shm_seg, dup(get_plat(win)->shm.im.shmid), 0);
#else
        c = xcb_shm_attach_checked(con, get_plat(win)->shm_seg, get_plat(win)->shm.im.shmid, 0);
#endif
        if (check_void_cookie(c)) goto error;

        if (has_shm_pixmaps) {
            if (!get_plat(win)->shm_pixmap)
                get_plat(win)->shm_pixmap = xcb_generate_id(con);
            c = xcb_shm_create_pixmap(con, get_plat(win)->shm_pixmap,
                    get_plat(win)->wid, STRIDE(width), height, TRUE_COLOR_ALPHA_DEPTH, get_plat(win)->shm_seg, 0);
            if (check_void_cookie(c)) goto error;
        }
    } else {
        get_plat(win)->shm.im = create_image(width, height);
    }

    return old;

error:
    free_image(&get_plat(win)->shm.im);
    get_plat(win)->shm.im = old;
    warn("Can't attach MITSHM image");
    return (struct image) {0};
}

void x11_shm_update(struct window *win, struct rect rect) {
    if (has_shm_pixmaps) {
        xcb_copy_area(con, get_plat(win)->shm_pixmap, get_plat(win)->wid, get_plat(win)->gc, rect.x, rect.y, rect.x, rect.y, rect.width, rect.height);
    } else if (has_fast_damage) {
        xcb_shm_put_image(con, get_plat(win)->wid, get_plat(win)->gc, STRIDE(get_plat(win)->shm.im.width), get_plat(win)->shm.im.height, rect.x, rect.y,
                rect.width, rect.height, rect.x, rect.y, TRUE_COLOR_ALPHA_DEPTH, XCB_IMAGE_FORMAT_Z_PIXMAP, 0, get_plat(win)->shm_seg, 0);
    } else {
        xcb_put_image(con, XCB_IMAGE_FORMAT_Z_PIXMAP, get_plat(win)->wid, get_plat(win)->gc,
                STRIDE(get_plat(win)->shm.im.width), rect.height, 0, rect.y, 0, TRUE_COLOR_ALPHA_DEPTH,
                rect.height * STRIDE(get_plat(win)->shm.im.width) * sizeof(color_t),
                (const uint8_t *)(get_plat(win)->shm.im.data + rect.y*STRIDE(get_plat(win)->shm.im.width)));
    }
}

struct extent x11_shm_size(struct window *win) {
    return (struct extent) {
        .width = (win->cw + 1) * win->char_width + 2*win->cfg.left_border - 1,
        .height = (win->ch + 1) * (win->char_height + win->char_depth) + 2*win->cfg.top_border - 1,
    };
}

void x11_shm_free(struct window *win) {
    if (has_fast_damage)
        xcb_shm_detach(con, get_plat(win)->shm_seg);
    if (has_shm_pixmaps)
        xcb_free_pixmap(con, get_plat(win)->shm_pixmap);
    if (get_plat(win)->shm.im.data)
        free_image(&get_plat(win)->shm.im);
    free(get_plat(win)->shm.bounds);
}

void x11_shm_free_context(void) {
    /* nothing */
}

void x11_shm_init_context(void) {
    /* That's kind of hack
     * Try guessing if DISPLAY refers to localhost */

    char *display = getenv("DISPLAY");
    const char *local[] = { "localhost:", "127.0.0.1:", "unix:", };
    bool localhost = display[0] == ':';
    for (size_t i = 0; !localhost && i < LEN(local); i++)
        localhost = display == strstr(display, local[i]);

    if (localhost) {
        xcb_shm_query_version_cookie_t q = xcb_shm_query_version(con);
        xcb_generic_error_t *er = NULL;
        xcb_shm_query_version_reply_t *qr = xcb_shm_query_version_reply(con, q, &er);
        if (er) free(er);

        if (qr) {
            has_shm_pixmaps = qr->shared_pixmaps &&
                    qr->pixmap_format == XCB_IMAGE_FORMAT_Z_PIXMAP;
            free(qr);
        }
        if (!(has_fast_damage = qr && !er)) {
            warn("MIT-SHM is not available");
        }
    }
}
