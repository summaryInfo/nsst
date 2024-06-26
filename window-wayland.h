#ifndef WINDOW_PLATFORM_WAYLAND_H_
#define WINDOW_PLATFORM_WAYLAND_H_

#include "feature.h"

#include "util.h"
#include "list.h"
#if USE_WAYLANDSHM
#   include "image.h"
#endif
#include  "window-impl.h"
#include <wayland-client.h>

#include <inttypes.h>
#include <stdbool.h>

enum win_ptr_kind {
    win_ptr_other,
    win_ptr_keyboard,
    win_ptr_paste,
};

struct window_ptr {
    struct list_head link;
    struct window *win;
    enum win_ptr_kind kind;
};

struct wayland_window {
    union {
#if USE_WAYLANDSHM
        struct {
            struct platform_shm shm;
            struct wl_buffer *buffer;
        };
#endif
    };

    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct wl_data_source *data_source;
    struct zwp_primary_selection_source_v1 *primary_selection_source;
    struct zxdg_toplevel_decoration_v1 *decoration;
    struct wl_callback *frame_callback;

    /* We cannot query the title so we need to store them */
    char *title;
    char *icon_title;

    /* These list is used to implement zeroing of week pointers */
    struct list_head pointers;

    /* We cannot query the pointer state, so store it here */
    struct {
        int32_t x;
        int32_t y;
        uint32_t mask;
    } mouse;

    struct {
        int32_t width;
        int32_t height;
        bool resize;
    } pending_configure;

    struct extent output_size;

    bool can_maximize : 1;
    bool can_minimize : 1;
    bool can_fullscreen : 1;
    bool is_maximized : 1;
    bool is_fullscreen : 1;
    bool is_resizing : 1;
    bool is_tiled : 1;
    bool use_ssd : 1;
} ALIGNED(MALLOC_ALIGNMENT);

extern struct wl_shm *wl_shm;

static inline struct wayland_window *get_plat(struct window *win) {
    return (struct wayland_window *)win->platform_window_opaque;
}

static inline void win_ptr_clear(struct window_ptr *ptr) {
    if (ptr->win) {
        list_remove(&ptr->link);
        ptr->win = NULL;
    }
}

static inline void win_ptr_set(struct window_ptr *ptr, struct window *win, enum win_ptr_kind kind) {
    win_ptr_clear(ptr);
    list_insert_after(&get_plat(win)->pointers, &ptr->link);
    ptr->win =  win;
    ptr->kind = kind;
}

/* Move pointer to the start of the list */
static inline void win_ptr_ping(struct window_ptr *ptr) {
    if (ptr->win) {
        list_remove(&ptr->link);
        list_insert_after(&get_plat(ptr->win)->pointers, &ptr->link);
    }
}

static inline struct extent wayland_image_size(struct window *win) {
    return (struct extent) {
        .width = win->cw * win->char_width + 2*win->cfg.left_border,
        .height = win->ch * (win->char_height + win->char_depth) + 2*win->cfg.top_border,
    };
}

#if USE_WAYLANDSHM
void wayland_shm_init_context(void);
void wayland_shm_free_context(void);
void wayland_shm_free(struct window *win);
void wayland_shm_update(struct window *win, struct rect rect);
struct extent wayland_shm_size(struct window *win, bool artificial);
struct image wayland_shm_create_image(struct window *win, int16_t width, int16_t height);

void shm_recolor_border(struct window *win);
bool shm_reload_font(struct window *win, bool need_free);
bool shm_submit_screen(struct window *win, int16_t cur_x, ssize_t cur_y, bool cursor, bool marg);
void shm_copy(struct window *win, struct rect dst, int16_t sx, int16_t sy);
void shm_resize(struct window *win, int16_t new_cw, int16_t new_ch, bool artificial);
#endif

#endif
