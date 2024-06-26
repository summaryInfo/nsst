/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */


#include "feature.h"

/* Make linting always work for this
 * file (force choosing the right renderer
 * structure variant in window-impl.h)*/
#undef USE_WAYLANDSHM
#define USE_WAYLANDSHM 1

#include "config.h"
#include "font.h"
#include "mouse.h"
#include "window-impl.h"
#include "window-wayland.h"

#include <stdbool.h>
#include <string.h>
#include <wayland-client.h>

extern bool has_fast_damage;

static bool resize_buffer(struct window *win, struct wl_shm_pool *pool) {
    /* We create buffer width width smaller than the image by (ab-)using stride to avoid extra copies.
     * Who needs wp-viewporter when we have dirty hacks */
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, win->cfg.geometry.r.width, win->cfg.geometry.r.height,
                                                         STRIDE(get_plat(win)->shm.im.width)*sizeof(color_t), WL_SHM_FORMAT_ARGB8888);
    if (!buffer)
        return false;

    if (get_plat(win)->buffer)
        wl_buffer_destroy(get_plat(win)->buffer);
    get_plat(win)->buffer = buffer;

    return true;
}

struct image wayland_shm_create_image(struct window *win, int16_t width, int16_t height) {
    struct image old = get_plat(win)->shm.im;

    get_plat(win)->shm.im = create_shm_image(width, height);

    static_assert(USE_POSIX_SHM, "System V shared memory is not supported with wayland backend");

    struct wl_shm_pool *pool = wl_shm_create_pool(wl_shm, get_plat(win)->shm.im.shmid, STRIDE(width)*height*sizeof(color_t));
    if (!pool)
        goto error;

    if (!resize_buffer(win, pool))
        goto error;

    if (get_plat(win)->pool)
        wl_shm_pool_destroy(get_plat(win)->pool);
    get_plat(win)->pool = pool;

    return old;

error:
    free_image(&get_plat(win)->shm.im);
    get_plat(win)->shm.im = old;
    warn("Can't create shm image");
    return (struct image) {0};
}

void wayland_shm_update(struct window *win, struct rect rect) {
    wl_surface_damage_buffer(get_plat(win)->surface, rect.x, rect.y, rect.width, rect.height);
}

void wayland_shm_resize_exact(struct window *win, int16_t new_w, int16_t new_h, int16_t old_w, int16_t old_h) {
    if (!resize_buffer(win, get_plat(win)->pool)) return;

    int16_t min_h = MIN(new_h, old_h);
    int16_t min_w = MIN(new_w, old_w);

    if (new_w > old_w)
        wayland_shm_update(win, (struct rect) { min_w, 0, new_w - old_w, MAX(new_h, old_h) });
    if (new_h > old_h)
        wayland_shm_update(win, (struct rect) { 0, min_h, min_w, new_h - old_h });
}

void wayland_shm_free(struct window *win) {
    if (get_plat(win)->buffer)
        wl_buffer_destroy(get_plat(win)->buffer);
    if (get_plat(win)->pool)
        wl_shm_pool_destroy(get_plat(win)->pool);
   if (get_plat(win)->shm.im.data)
        free_image(&get_plat(win)->shm.im);
    free(get_plat(win)->shm.bounds);
}

void wayland_shm_free_context(void) {
    /* nothing */
}

void wayland_shm_init_context(void) {
    has_fast_damage = true;
}
