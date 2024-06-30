/* Copyright (c) 2024, Evgeniy Baskov. All rights reserved */

#include "feature.h"


#if USE_PPOLL
#   define _GNU_SOURCE
#endif

#include "config.h"
#include "list.h"
#include "mouse.h"
#include "poller.h"
#include "term.h"
#include "uri.h"
#include "util.h"
#include "window-impl.h"
#include "window-wayland.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include <wayland-cursor.h>

#include "xdg-shell-protocol.h"
#include "xdg-decoration-protocol.h"
#include "xdg-output-protocol.h"
#include "primary-selection-protocol.h"

#define NSST_CLASS "Nsst"

enum pointer_event_mask {
    POINTER_EVENT_ENTER = 1 << 0,
    POINTER_EVENT_LEAVE = 1 << 1,
    POINTER_EVENT_MOTION = 1 << 2,
    POINTER_EVENT_BUTTON = 1 << 3,
    POINTER_EVENT_AXIS = 1 << 4,
    POINTER_EVENT_AXIS_SOURCE = 1 << 5,
    POINTER_EVENT_AXIS_STOP = 1 << 6,
    POINTER_EVENT_AXIS_DISCRETE = 1 << 7,
    POINTER_EVENT_AXIS_RELATIVE_DIRECTION = 1 << 9,
};

struct cursor {
    ht_head_t link;
    char *name;
    int32_t refcount;
    struct wl_cursor *cursor;
    struct wl_buffer *cursor_buffer;
    struct wl_surface *cursor_surface;
};

struct active_paste {
    struct list_head link;
    struct window_ptr wptr;
    struct event *event;
    bool utf8;
    bool tail;
    int fd;
};

struct output {
    struct list_head link;
    struct wl_output *output;
    struct zxdg_output_v1 *xdg_output;
    struct rect logical;
    struct rect physical;
    struct extent mm;
    char *name;
    char *descr;
    uint32_t id;
    int32_t refresh;
    enum wl_output_subpixel subpixel;
    enum wl_output_transform transform;
    int32_t scale;
    bool xdg_output_done;
    bool output_done;
    double dpi;
};

struct seat {
    struct wl_seat *seat;
    struct list_head link;
    char *name;
    uint32_t capabilities;
    uint32_t id;

    struct {
        struct wl_data_device *data_device;
        struct wl_data_offer *data_offer;
        bool is_selection : 1;
        bool mime_utf8 : 1;
        unsigned supported_index;
        const char *supported_mime;
    } selection;

    struct {
        struct zwp_primary_selection_device_v1 *primary_selection_device;
        struct zwp_primary_selection_offer_v1 *primary_selection_offer;
        bool is_selection : 1;
        bool mime_utf8 : 1;
        unsigned supported_index;
        const char *supported_mime;
    } primary_selection;

    struct {
        struct window_ptr wptr;
        struct wl_pointer *pointer;
        uint32_t event_mask;
        wl_fixed_t surface_x;
        wl_fixed_t surface_y;
        uint32_t button;
        uint32_t state;
        uint32_t time;
        uint32_t serial;
        struct {
            bool used;
            wl_fixed_t value;
            int32_t discrete;
            int32_t discrete120;
            uint32_t direction;
        } axes[2];
        uint32_t axis_source;
        uint32_t mask;
    } pointer;

    struct {
        struct window_ptr wptr;
        struct wl_keyboard *keyboard;
        uint32_t last_key;
        uint32_t serial;
        struct xkb_context *xkb_ctx;
        struct xkb_state *xkb_state;
        uint32_t mask;

        struct event *autorepeat_timer;
        int64_t autorepeat_initial;
        int64_t autorepeat_repeat;
    } keyboard;

    // FIXME Touch?
};

struct context {
    struct event *dpl_event;

    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct xdg_wm_base *xdg_wm_base;
    struct wl_data_device_manager *data_device_manager;
    struct zxdg_decoration_manager_v1 *decoration_manager;
    struct zwp_primary_selection_device_manager_v1 *primary_selection_device_manager;
    struct zxdg_output_manager_v1 *output_manager;

    struct wl_cursor_theme *cursor_theme;
    hashtable_t cursors;

    struct list_head paste_fds;
    struct list_head seats;
    struct list_head outputs;

    void (*renderer_recolor_border)(struct window *);
    void (*renderer_free)(struct window *);
    void (*renderer_free_context)(void);
};

static struct context ctx;
struct wl_display *dpl;
struct wl_shm *wl_shm;

static inline void seat_stop_autorepeat(struct seat *seat, uint32_t key);

static void wayland_update_colors(struct window *win) {
    ctx.renderer_recolor_border(win);
}

static bool wayland_resize_window(struct window *win, int16_t width, int16_t height) {
    get_plat(win)->pending_configure.resize = true;
    get_plat(win)->pending_configure.width = width;
    get_plat(win)->pending_configure.height = height;
    return false;
}

static void wayland_after_read(struct window *win) {
    if (get_plat(win)->pending_configure.resize) {
        handle_resize(win, get_plat(win)->pending_configure.width, get_plat(win)->pending_configure.height, true);
        get_plat(win)->pending_configure.resize = false;
    }
}

static void wayland_move_window(struct window *win, int16_t x, int16_t y) {
    // NOTE: Wayland does not support moving to specified position
    (void)win, (void)x, (void)y;
}

static bool wayland_window_action(struct window *win, enum window_action act) {
    switch (act) {
    case action_minimize:
        xdg_toplevel_set_minimized(get_plat(win)->xdg_toplevel);
        break;
        // NOTE: There is no way to unset minimized state
    case action_restore_minimized:
        // NOTE: These are not supported on wayland
    case action_lower:
    case action_raise:
        break;
    case action_maximize:
        xdg_toplevel_set_maximized(get_plat(win)->xdg_toplevel);
        break;
        // NOTE: There is no way to maximize window in only one direction
        //       so we just equate these states with the normal fullscreen state
    case action_maximize_width:
    case action_maximize_height:
    case action_fullscreen:
        xdg_toplevel_set_fullscreen(get_plat(win)->xdg_toplevel, NULL);
        break;
    case action_restore:
        if (get_plat(win)->is_maximized)
            xdg_toplevel_unset_maximized(get_plat(win)->xdg_toplevel);
        if (get_plat(win)->is_fullscreen)
            xdg_toplevel_unset_fullscreen(get_plat(win)->xdg_toplevel);
        // NOTE: There is no way to unset minimized state
        break;
    case action_toggle_fullscreen:
        return wayland_window_action(win, get_plat(win)->is_fullscreen ? action_restore : action_fullscreen);
    case action_none:
        break;
    }
    return false;
}

static struct extent wayland_get_position(struct window *win) {
    (void)win;
    // NOTE Wayland does not support querying window position
    return (struct extent) {0, 0};
}

static struct extent wayland_get_screen_size(struct window *win) {
    return get_plat(win)->output_size;
}

static void wayland_get_pointer(struct window *win, struct extent *p, int32_t *pmask) {
    // NOTE In wayland we cannot manually query pointer position,
    //      so we need to track state manually
    p->width = get_plat(win)->mouse.x;
    p->height = get_plat(win)->mouse.y;
    *pmask = get_plat(win)->mouse.mask;
}

static void wayland_set_urgency(struct window *win, bool set) {
    // FIXME Use xdg_activation_v1 (see https://wayland.app/protocols/xdg-activation-v1)
    (void)win, (void)set;
}

static void wayland_bell(struct window *win, uint8_t vol) {
    // NOTE Not supported on wayland
    (void)win, (void)vol;
}

static bool cursor_cmp(const ht_head_t *a, const ht_head_t *b) {
    const struct cursor *ua = (const struct cursor *)a;
    const struct cursor *ub = (const struct cursor *)b;
    return !strcmp(ua->name, ub->name);
}

static inline struct cursor *ref_cursor(struct cursor *csr) {
    csr->refcount++;
    return csr;
}

static struct cursor *get_cursor(const char *name) {
    if (!ctx.cursor_theme) return NULL;

    struct cursor dummy = {
        .name = (char *)name,
        .link.hash = hash64(name, strlen(name)),
    };

    ht_head_t **it = ht_lookup_ptr(&ctx.cursors, &dummy.link);
    if (*it) {
        struct cursor *found = (struct cursor *)*it;
        found->refcount++;
        return found;
    }

    struct wl_cursor *cursor = wl_cursor_theme_get_cursor(ctx.cursor_theme, name);
    if (!cursor) {
        warn("Unable to load cursor '%s'", name);
        return NULL;
    }

    struct wl_surface *cursor_surface = wl_compositor_create_surface(ctx.compositor);
    if (!cursor_surface) {
        warn("Unable to create cursor surface");
        return NULL;
    }

    struct wl_buffer *cursor_buffer = wl_cursor_image_get_buffer(cursor->images[0]);
    if (!cursor_buffer) {
        warn("Unable to create cursor buffer");
        return NULL;
    }

    wl_surface_attach(cursor_surface, cursor_buffer, 0, 0);
    wl_surface_damage_buffer(cursor_surface, 0, 0, UINT32_MAX, UINT32_MAX);
    wl_surface_commit(cursor_surface);

    struct cursor *new = xalloc(sizeof *new);
    *new = (struct cursor) {
        .link.hash = dummy.link.hash,
        .name = strdup(name),
        .refcount = 1,
        .cursor = cursor,
        .cursor_buffer = cursor_buffer,
        .cursor_surface = cursor_surface,
    };

    ht_insert_hint(&ctx.cursors, it, &new->link);
    return new;
}

static void unref_cursor(struct cursor *csr) {
    if (!csr) return;
    if (!--csr->refcount) {
        ht_erase(&ctx.cursors, &csr->link);
        wl_surface_destroy(csr->cursor_surface);
        /* NOTE: cursor_buffer and cursor and owned by cursor_theme */
        free(csr->name);
        free(csr);
    }
}

static void activate_cursor_for_seat(struct window *win, struct seat *seat) {
    if (get_plat(win)->cursor_is_hidden) {
        wl_pointer_set_cursor(seat->pointer.pointer, seat->pointer.serial, NULL, 0, 0);
    } else {
        wl_pointer_set_cursor(seat->pointer.pointer, seat->pointer.serial,
                              get_plat(win)->cursor->cursor_surface,
                              get_plat(win)->cursor->cursor->images[0]->hotspot_x,
                              get_plat(win)->cursor->cursor->images[0]->hotspot_y);
    }
}

static void activate_cursor(struct window *win) {
    LIST_FOREACH(it, &ctx.seats) {
        struct seat *seat = CONTAINEROF(it, struct seat, link);
        if (seat->pointer.wptr.win == win)
            activate_cursor_for_seat(win, seat);
    }
}

static void select_cursor(struct window *win, struct cursor *csr) {
    ref_cursor(csr);
    unref_cursor(get_plat(win)->cursor);
    get_plat(win)->cursor = csr;

    activate_cursor(win);
}

static void update_hide_cursor(struct window *win) {
    bool hide = (get_plat(win)->cursor_mode == hide_always ||
                (get_plat(win)->cursor_mode == hide_no_tracking &&
                     term_get_mstate(win->term)->mouse_mode == mouse_mode_none));
    if (hide && !get_plat(win)->cursor_is_hidden) {
        get_plat(win)->cursor_is_hidden = true;
        activate_cursor(win);
    } else if (!hide && get_plat(win)->cursor_is_hidden) {
        get_plat(win)->cursor_is_hidden = false;
        assert(get_plat(win)->cursor);
        activate_cursor(win);
    }
}

static void wayland_enable_mouse_events(struct window *win, bool enabled) {
    (void)enabled;
    update_hide_cursor(win);
}

static void wayland_select_cursor(struct window *win, const char *name) {
    struct cursor *csr = get_cursor(name);
    unref_cursor(get_plat(win)->cursor_user);
    get_plat(win)->cursor_user = csr;

    if (!csr) csr = get_plat(win)->cursor_default;
    select_cursor(win, csr);
}

static void wayland_set_pointer_mode(struct window *win, enum hide_pointer_mode mode) {
    get_plat(win)->cursor_mode = mode;
    update_hide_cursor(win);
}

static void wayland_set_title(struct window *win, const char *title, bool utf8) {
    if (get_plat(win)->title)
        free(get_plat(win)->title);

    assert(utf8);
    get_plat(win)->title = strdup(title);
    xdg_toplevel_set_title(get_plat(win)->xdg_toplevel, get_plat(win)->title);
}

static void wayland_set_icon_label(struct window *win, const char *icon_title, bool utf8) {
    if (get_plat(win)->icon_title)
        free(get_plat(win)->icon_title);

    assert(utf8);
    get_plat(win)->icon_title = strdup(icon_title);
    // NOTE: Wayland does not support separate icon title
}

static void wayland_get_title(struct window *win, enum title_target which, char **name, bool *utf8) {
    char *title = which & target_title ? get_plat(win)->title :
                  which & target_icon_label ? get_plat(win)->icon_title : NULL;
    if (utf8) *utf8 = true;
    if (name) *name = strdup(title);
}

void wayland_update_window_props(struct window *win) {
    // NOTE: Wayland does not support multiple window classes
    xdg_toplevel_set_app_id(get_plat(win)->xdg_toplevel,
                            win->cfg.window_class ? win->cfg.window_class : NSST_CLASS);
    xdg_toplevel_set_min_size(get_plat(win)->xdg_toplevel,
                              win->cfg.border.left + win->cfg.border.right + 2*win->char_width,
                              win->cfg.border.top + win->cfg.border.bottom + win->char_depth + win->char_height);
    wl_surface_commit(get_plat(win)->surface);
}

void wayland_fixup_geometry(struct window *win) {
    win->cfg.geometry.r.x = 0;
    win->cfg.geometry.r.y = 0;
    win->cfg.geometry.stick_to_bottom = false;
    win->cfg.geometry.stick_to_right = false;

    if (win->cfg.geometry.char_geometry) {
        int16_t cw = MAX(win->cfg.geometry.r.width, 2);
        int16_t ch = MAX(win->cfg.geometry.r.height, 1);
        win->cfg.geometry.r.width = win->char_width * cw + win->cfg.border.left + win->cfg.border.right;
        win->cfg.geometry.r.height = (win->char_height + win->char_depth) * ch + win->cfg.border.top + win->cfg.border.bottom;
        win->cfg.geometry.char_geometry = false;
        win->cw = cw;
        win->ch = ch;
    } else {
        win->cw = MAX(2, (win->cfg.geometry.r.width - win->cfg.border.left - win->cfg.border.right) / win->char_width);
        win->ch = MAX(1, (win->cfg.geometry.r.height - win->cfg.border.top - win->cfg.border.bottom) / (win->char_height + win->char_depth));
    }
}

static void handle_surface_enter(void *data, struct wl_surface *wl_surface, struct wl_output *wl_output) {
    struct window *win = data;
    (void)wl_surface;

    struct output *output = wl_output_get_user_data(wl_output);
    if (output) {
        if (ctx.output_manager) {
            get_plat(win)->output_size.height = output->logical.height;
            get_plat(win)->output_size.width = output->logical.width;
        } else {
            get_plat(win)->output_size.height = output->physical.height / output->scale;
            get_plat(win)->output_size.width = output->physical.width / output->scale;
        }
    }

    // FIXME Adjust fonts and scale to the new output
}

static void handle_surface_leave(void *data, struct wl_surface *wl_surface, struct wl_output *output) {
    struct window *win = data;
    (void)wl_surface;

    // FIXME Adjust fonts and scale to the new output
    (void)output, (void)win;
}

static void handle_surface_preferred_buffer_scale(void *data, struct wl_surface *wl_surface, int32_t factor) {
    (void)data, (void)wl_surface, (void)factor;
    // FIXME HiDPI
}

static void handle_surface_preferred_buffer_transform(void *data, struct wl_surface *wl_surface, uint32_t transform) {
    (void)data, (void)wl_surface, (void)transform;
    // FIXME HiDPI
}

struct wl_surface_listener surface_listener = {
    .enter = handle_surface_enter,
    .leave = handle_surface_leave,
    .preferred_buffer_scale = handle_surface_preferred_buffer_scale,
    .preferred_buffer_transform = handle_surface_preferred_buffer_transform,
};

void handle_xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
    struct window *win = data;

    if (gconfig.trace_events)
        info("Event[%p]: xdg_surface.configure(serial=%x)", data, serial);

    win->any_event_happend = true;

    xdg_surface_ack_configure(xdg_surface, serial);

    uint32_t width = get_plat(win)->pending_configure.width ? get_plat(win)->pending_configure.width : win->cfg.geometry.r.width;
    uint32_t height = get_plat(win)->pending_configure.height ? get_plat(win)->pending_configure.height : win->cfg.geometry.r.height;
    bool exact = get_plat(win)->is_maximized || get_plat(win)->is_fullscreen || get_plat(win)->is_tiled || win->cfg.smooth_resize;

    handle_resize(win, width, height, exact);

    wl_surface_attach(get_plat(win)->surface, get_plat(win)->buffer, 0, 0);
    wl_surface_commit(get_plat(win)->surface);
}

struct xdg_surface_listener xdg_surface_listener = {
    .configure = handle_xdg_surface_configure,
};

void handle_xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel, int32_t width, int32_t height, struct wl_array *states) {
    struct window *win = data;

    (void)xdg_toplevel;

    get_plat(win)->is_maximized = false;
    get_plat(win)->is_fullscreen = false;
    get_plat(win)->is_resizing = false;
    get_plat(win)->is_tiled = false;
    if (height) get_plat(win)->pending_configure.height = height;
    if (width) get_plat(win)->pending_configure.width = width;
    win->any_event_happend = true;
    win->mapped = true;

    uint32_t states_mask = 0;
    enum xdg_toplevel_state *it;
    wl_array_for_each(it, states) {
        states_mask = 1U << *it;
        switch (*it) {
        case XDG_TOPLEVEL_STATE_MAXIMIZED:
            get_plat(win)->is_maximized = true;
            break;
        case XDG_TOPLEVEL_STATE_FULLSCREEN:
            get_plat(win)->is_fullscreen = true;
            break;
        case XDG_TOPLEVEL_STATE_ACTIVATED:
            // FIXME Should we treat is as win->focused? Probably not
            break;
        case XDG_TOPLEVEL_STATE_SUSPENDED:
            win->mapped = false;
            break;
        case XDG_TOPLEVEL_STATE_RESIZING:
            get_plat(win)->is_resizing = true;
            break;
        case XDG_TOPLEVEL_STATE_TILED_LEFT:
        case XDG_TOPLEVEL_STATE_TILED_RIGHT:
        case XDG_TOPLEVEL_STATE_TILED_TOP:
        case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
            get_plat(win)->is_tiled = true;
            break;
        default:
            break;
        }
    }

    if (gconfig.trace_events)
        info("Event[%p]: xdg_toplevel.configure(width=%d, height=%d, mask=%x)", data, width, height, states_mask);

}

void handle_xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel) {
    struct window *win = data;
    (void)xdg_toplevel;

    if (gconfig.trace_events)
        info("Event[%p]: xdg_toplevel.close", data);

   free_window(win);
}

void handle_xdg_toplevel_configure_bounds(void *data, struct xdg_toplevel *xdg_toplevel, int32_t width, int32_t height) {
    struct window *win = data;
    (void)xdg_toplevel;

    if (width) get_plat(win)->pending_configure.width = MIN(get_plat(win)->pending_configure.width, width);
    if (height) get_plat(win)->pending_configure.height = MIN(get_plat(win)->pending_configure.height, height);

    if (gconfig.trace_events)
        info("Event[%p]: xdg_toplevel.configure_bounds(width=%d, height=%d)", data, width, height);
}

void handle_xdg_toplevel_wm_capabilities(void *data, struct xdg_toplevel *xdg_toplevel, struct wl_array *capabilities) {
    struct window *win = data;
    (void)xdg_toplevel;

    get_plat(win)->can_maximize = false;
    get_plat(win)->can_minimize = false;
    get_plat(win)->can_fullscreen = false;

    uint32_t mask = 0;

    enum xdg_toplevel_wm_capabilities *it;
    wl_array_for_each(it, capabilities) {
        mask |= 1U << mask;
        switch (*it) {
        case XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE:
            get_plat(win)->can_maximize = true;
            break;
        case XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE:
            get_plat(win)->can_minimize = true;
            break;
        case XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN:
            get_plat(win)->can_fullscreen = true;
            break;
        default:
            break;
        }
    }
    if (gconfig.trace_events)
        info("Event[%p]: xdg_toplevel.wm_capabilities(mask=%x)", data, mask);
}

static struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = handle_xdg_toplevel_configure,
    .close = handle_xdg_toplevel_close,
    .configure_bounds = handle_xdg_toplevel_configure_bounds,
    .wm_capabilities = handle_xdg_toplevel_wm_capabilities,
};

static void handle_xdg_toplevel_decoration_configure(void *data, struct zxdg_toplevel_decoration_v1 *zxdg_toplevel_decoration_v1, uint32_t mode) {
    struct window *win = data;
    (void)zxdg_toplevel_decoration_v1;

    if (mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE) {
        if (!win->cfg.force_wayland_csd)
            warn("Wayland compositor does not support server side decorations");
    } else {
        get_plat(win)->use_ssd = true;
    }
}

static struct zxdg_toplevel_decoration_v1_listener xdg_toplevel_decoration_listener = {
    .configure = handle_xdg_toplevel_decoration_configure,
};

static bool wayland_init_window(struct window *win) {
    struct wayland_window *ww = get_plat(win);

    // FIXME Remove manual FPS tracking at all
    win->cfg.fps = 1000;
    win->cfg.force_utf8_title = true;

    list_init(&get_plat(win)->pointers);

    ww->surface = wl_compositor_create_surface(ctx.compositor);
    if (!ww->surface)
        return false;
    wl_surface_add_listener(ww->surface, &surface_listener, win);

    ww->xdg_surface = xdg_wm_base_get_xdg_surface(ctx.xdg_wm_base, ww->surface);
    if (!ww->xdg_surface)
        return false;
    xdg_surface_add_listener(ww->xdg_surface, &xdg_surface_listener, win);

    ww->xdg_toplevel = xdg_surface_get_toplevel(ww->xdg_surface);
    if (!ww->xdg_toplevel)
        return false;
    xdg_toplevel_add_listener(ww->xdg_toplevel, &xdg_toplevel_listener, win);

    if (ctx.decoration_manager) {
        ww->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(ctx.decoration_manager, ww->xdg_toplevel);
        if (!ww->decoration)
            return false;
        zxdg_toplevel_decoration_v1_add_listener(ww->decoration, &xdg_toplevel_decoration_listener, win);
        uint32_t mode = win->cfg.force_wayland_csd ?
                ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE :
                ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
        zxdg_toplevel_decoration_v1_set_mode(ww->decoration, mode);
    } else {
        warn("Wayland compositor does not support server side decorations");
    }

    // FIXME Reload cursor upon setting reloading (pointer_shape can change)
    // FIXME Try more cursor shapes, not just fall back to default one
    // FIXME Cleanup

    get_plat(win)->cursor_resize = get_cursor("size_all");
    if (!get_plat(win)->cursor_resize)
        get_plat(win)->cursor_resize = get_cursor("default");
    get_plat(win)->cursor_uri = get_cursor("hand1");
    if (!get_plat(win)->cursor_uri)
        get_plat(win)->cursor_uri = get_cursor("pointing_hand");
    if (!get_plat(win)->cursor_uri)
        get_plat(win)->cursor_uri = get_cursor("default");
    get_plat(win)->cursor_default = get_cursor(win->cfg.pointer_shape);
    if (!get_plat(win)->cursor_default)
        get_plat(win)->cursor_default = get_cursor("xterm");
    if (!get_plat(win)->cursor_default)
        get_plat(win)->cursor_default = get_cursor("ibeam");
    if (!get_plat(win)->cursor_default)
        get_plat(win)->cursor_default = get_cursor("default");

    select_cursor(win, get_plat(win)->cursor_default);
    update_hide_cursor(win);
    return true;
}

static void wayland_map_window(struct window *win) {
    (void)win;
    wl_display_roundtrip(dpl);
}

static void free_paste(struct active_paste *paste) {
    list_remove(&paste->link);
    win_ptr_clear(&paste->wptr);
    poller_remove(paste->event);
    close(paste->fd);
    free(paste);
}

static void wayland_free_window(struct window *win) {
    ctx.renderer_free(win);

    unref_cursor(get_plat(win)->cursor);
    unref_cursor(get_plat(win)->cursor_uri);
    unref_cursor(get_plat(win)->cursor_resize);
    unref_cursor(get_plat(win)->cursor_default);
    unref_cursor(get_plat(win)->cursor_user);

    if (get_plat(win)->primary_selection_source)
        zwp_primary_selection_source_v1_destroy(get_plat(win)->primary_selection_source);
    if (get_plat(win)->data_source)
        wl_data_source_destroy(get_plat(win)->data_source);

    if (get_plat(win)->decoration)
        zxdg_toplevel_decoration_v1_destroy(get_plat(win)->decoration);
    if (get_plat(win)->xdg_toplevel)
        xdg_toplevel_destroy(get_plat(win)->xdg_toplevel);
    if (get_plat(win)->xdg_surface)
        xdg_surface_destroy(get_plat(win)->xdg_surface);
    if (get_plat(win)->surface)
        wl_surface_destroy(get_plat(win)->surface);
    if (get_plat(win)->frame_callback)
        wl_callback_destroy(get_plat(win)->frame_callback);

    if (get_plat(win)->title)
        free(get_plat(win)->title);
    if (get_plat(win)->icon_title)
        free(get_plat(win)->icon_title);

    LIST_FOREACH_SAFE(it, &get_plat(win)->pointers) {
        struct window_ptr *ptr = CONTAINEROF(it, struct window_ptr, link);
        switch (ptr->kind) {
            case win_ptr_keyboard: {
                struct seat *seat = CONTAINEROF(ptr, struct seat, keyboard.wptr);
                seat_stop_autorepeat(seat, 0);
                break;
            }
            case win_ptr_paste: {
                struct active_paste *paste = CONTAINEROF(ptr, struct active_paste, wptr);
                free_paste(paste);
                break;
            }
            default:
                break;
        }
        win_ptr_clear(ptr);
    }

    wl_display_flush(dpl);
}

static void handle_primary_selection_source_send(void *data, struct zwp_primary_selection_source_v1 *primary_selection_source, const char *mime_type, int32_t fd) {
    struct window *win = data;
    (void)primary_selection_source;

    if (gconfig.trace_events)
        info("Event[%p]: primary_selection_source.send(mime_type=%s, fd=%d)", data, mime_type, fd);

    win->any_event_happend = true;

    uint8_t *source = win->clipped[clip_primary];
    if (!source) goto empty;

    size_t len = strlen((char *)source);
    while (len) {
        ssize_t inc = write(fd, source, len);
        if (inc <= 0) break;
        source += inc;
        len -= inc;
    }

empty:
    close(fd);
}

static const char *selection_supported_types[] = {
    "text/plain",
    "text/plain;charset=utf-8",
    /* Non-compliant but used mime types... */
    "UTF8_STRING",
    "STRING",
    "TEXT",
};

static void handle_primary_selection_source_cancelled(void *data, struct zwp_primary_selection_source_v1 *primary_selection_source) {
    struct window *win = data;

    if (gconfig.trace_events)
        info("Event[%p]: primary_selection_source.cancelled", data);

    win->any_event_happend = true;

    assert(primary_selection_source == get_plat(win)->primary_selection_source);

    get_plat(win)->primary_selection_source = NULL;
    zwp_primary_selection_source_v1_destroy(primary_selection_source);

    screen_damage_selection(term_screen(win->term));
    selection_clear(term_get_sstate(win->term));
}

struct zwp_primary_selection_source_v1_listener primary_selection_source_listener = {
    .send = handle_primary_selection_source_send,
    .cancelled = handle_primary_selection_source_cancelled,
};

static void handle_primary_selection_offer_offer(void *data, struct zwp_primary_selection_offer_v1 *primary_selection_offer, const char *mime_type) {
    struct seat *seat = data;
    (void)primary_selection_offer;

    if (gconfig.trace_events)
        info("Event[%p]: primary_selection_offer.offer(mime_type=%s)", data, mime_type);

    for (size_t i = 0; i < LEN(selection_supported_types); i++) {
        if (!strcmp(mime_type, selection_supported_types[i])) {
            if (!seat->primary_selection.supported_mime ||
                    seat->primary_selection.supported_index > i) {
                seat->primary_selection.supported_mime = selection_supported_types[i];
                seat->primary_selection.supported_index = i;
                seat->primary_selection.mime_utf8 = i < 3;
            }
            break;
        }
    }
}

struct zwp_primary_selection_offer_v1_listener primary_selection_offer_listener = {
    .offer = handle_primary_selection_offer_offer,
};

static void handle_primary_selection_device_data_offer(void *data, struct zwp_primary_selection_device_v1 *primary_selection_device, struct zwp_primary_selection_offer_v1 *id) {
    struct seat *seat = data;
    (void)primary_selection_device;

    if (gconfig.trace_events)
        info("Event[%p]: primary_selection_device.data_offer(id=%p)", data, (void *)id);

    if (seat->primary_selection.primary_selection_offer)
        zwp_primary_selection_offer_v1_destroy(seat->primary_selection.primary_selection_offer);

    seat->primary_selection.primary_selection_offer = id;
    seat->primary_selection.is_selection = false;
    seat->primary_selection.supported_mime = NULL;

    if (id)
        zwp_primary_selection_offer_v1_add_listener(id, &primary_selection_offer_listener, seat);
}

static void handle_primary_selection_device_selection(void *data, struct zwp_primary_selection_device_v1 *primary_selection_device, struct zwp_primary_selection_offer_v1 *id) {
    struct seat *seat = data;
    (void)primary_selection_device;

    if (gconfig.trace_events)
        info("Event[%p]: primary_selection_device.selecion(id=%p)", data, (void *)id);

    if (!id) {
        if (seat->primary_selection.primary_selection_offer)
            zwp_primary_selection_offer_v1_destroy(seat->primary_selection.primary_selection_offer);
        seat->primary_selection.primary_selection_offer = NULL;
    } else {
        assert(seat->primary_selection.primary_selection_offer == id);
        seat->primary_selection.is_selection = true;
    }
}

struct zwp_primary_selection_device_v1_listener primary_selection_device_listener = {
    .data_offer = handle_primary_selection_device_data_offer,
    .selection = handle_primary_selection_device_selection,
};

static void handle_data_source_send(void *data, struct wl_data_source *wl_data_source, const char *mime_type, int32_t fd) {
    struct window *win = data;
    (void)wl_data_source;

    if (gconfig.trace_events)
        info("Event[%p]: data_source.send(mime_type=%s, fd=%d)", data, mime_type, fd);

    win->any_event_happend = true;

    uint8_t *source = term_is_keep_clipboard_enabled(win->term) ? win->clipboard : win->clipped[clip_clipboard];
    if (!source) goto empty;

    size_t len = strlen((char *)source);
    while (len) {
        ssize_t inc = write(fd, source, len);
        if (inc <= 0) break;
        source += inc;
        len -= inc;
    }

empty:
    close(fd);
}

static void handle_data_source_cancelled(void *data, struct wl_data_source *wl_data_source) {
    struct window *win = data;

    if (gconfig.trace_events)
        info("Event[%p]: data_source.cancelled", data);

    win->any_event_happend = true;

    assert(wl_data_source == get_plat(win)->data_source);

    get_plat(win)->data_source = NULL;
    wl_data_source_destroy(wl_data_source);

    screen_damage_selection(term_screen(win->term));
    selection_clear(term_get_sstate(win->term));
}

static void handle_data_source_target(void *data, struct wl_data_source *wl_data_source, const char *mime_type) {
    // NOTE: DnD is not implemented
    (void)data, (void)wl_data_source, (void)mime_type;
}

static void handle_data_source_dnd_drop_performed(void *data, struct wl_data_source *wl_data_source) {
    // NOTE: DnD is not implemented
    (void)data, (void)wl_data_source;
}

static void handle_data_source_dnd_finished(void *data, struct wl_data_source *wl_data_source) {
    // NOTE: DnD is not implemented
    (void)data, (void)wl_data_source;
}

static void handle_data_source_action(void *data, struct wl_data_source *wl_data_source, uint32_t dnd_action) {
    // NOTE: DnD is not implemented
    (void)data, (void)wl_data_source, (void)dnd_action;
}

struct wl_data_source_listener data_source_listener = {
    .target = handle_data_source_target,
    .send = handle_data_source_send,
    .cancelled = handle_data_source_cancelled,
    .dnd_drop_performed = handle_data_source_dnd_drop_performed,
    .dnd_finished = handle_data_source_dnd_finished,
    .action = handle_data_source_action,
};

static void handle_data_offer_offer(void *data, struct wl_data_offer *wl_data_offer, const char *mime_type) {
    struct seat *seat = data;
    (void)wl_data_offer;

    if (gconfig.trace_events)
        info("Event[%p]: data_offer.offer(mime_type=%s)", data, mime_type);

    for (size_t i = 0; i < LEN(selection_supported_types); i++) {
        if (!strcmp(mime_type, selection_supported_types[i])) {
            if (!seat->selection.supported_mime ||
                    seat->selection.supported_index > i) {
                seat->selection.supported_mime = selection_supported_types[i];
                seat->selection.supported_index = i;
                seat->selection.mime_utf8 = i < 3;
            }
            break;
        }
    }
}

static void handle_data_offer_source_actions(void *data, struct wl_data_offer *wl_data_offer, uint32_t source_actions) {
    // NOTE: DnD is not implemented
    (void)data, (void)wl_data_offer, (void)source_actions;
}

static void handle_data_offer_action(void *data, struct wl_data_offer *wl_data_offer, uint32_t dnd_action) {
    // NOTE: DnD is not implemented
    (void)data, (void)wl_data_offer, (void)dnd_action;
}

struct wl_data_offer_listener data_offer_listener = {
    .offer = handle_data_offer_offer,
    .source_actions = handle_data_offer_source_actions,
    .action = handle_data_offer_action,
};

static void handle_data_device_data_offer(void *data, struct wl_data_device *wl_data_device, struct wl_data_offer *id) {
    struct seat *seat = data;
    (void)wl_data_device;

    if (gconfig.trace_events)
        info("Event[%p]: data_device.data_offer(id=%p)", data, (void *)id);

    if (seat->selection.data_offer)
        wl_data_offer_destroy(seat->selection.data_offer);

    seat->selection.data_offer = id;
    seat->selection.is_selection = false;
    seat->selection.supported_mime = NULL;

    if (id)
        wl_data_offer_add_listener(id, &data_offer_listener, seat);
}

static void handle_data_device_selection(void *data, struct wl_data_device *wl_data_device, struct wl_data_offer *id) {
    struct seat *seat = data;
    (void)wl_data_device;

    if (gconfig.trace_events)
        info("Event[%p]: data_device.selecion(id=%p)", data, (void *)id);

    if (!id) {
        if (seat->selection.data_offer)
            wl_data_offer_destroy(seat->selection.data_offer);
        seat->selection.data_offer = NULL;
    } else {
        assert(seat->selection.data_offer == id);
        seat->selection.is_selection = true;
    }
}

static void handle_data_device_enter(void *data, struct wl_data_device *wl_data_device, uint32_t serial, struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *id) {
    // NOTE: DnD is not implemented
    (void)data, (void)wl_data_device, (void)serial, (void)surface, (void)x, (void)y, (void)id;
}

static void handle_data_device_leave(void *data, struct wl_data_device *wl_data_device) {
    // NOTE: DnD is not implemented
    (void)data, (void)wl_data_device;
}

static void handle_data_device_motion(void *data, struct wl_data_device *wl_data_device, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    // NOTE: DnD is not implemented
    (void)data, (void)wl_data_device, (void)time, (void)x, (void)y;
}

static void handle_data_device_drop(void *data, struct wl_data_device *wl_data_device) {
    // NOTE: DnD is not implemented
    (void)data, (void)wl_data_device;
}

struct wl_data_device_listener data_device_listener = {
    .data_offer = handle_data_device_data_offer,
    .enter = handle_data_device_enter,
    .leave = handle_data_device_leave,
    .motion = handle_data_device_motion,
    .drop = handle_data_device_drop,
    .selection = handle_data_device_selection,
};

static inline struct seat *find_first_seat(struct window *win) {
    /* Find the seat that last received input event to paste from the correct source */
    LIST_FOREACH(it, &get_plat(win)->pointers) {
        struct window_ptr *ptr = CONTAINEROF(it, struct window_ptr, link);
        if (ptr->kind == win_ptr_keyboard)
            return CONTAINEROF(ptr, struct seat, keyboard.wptr);
    }

    return NULL;
}

static bool do_set_clipboard(struct window *win) {
    if (get_plat(win)->data_source)
        wl_data_source_destroy(get_plat(win)->data_source);

    struct wl_data_source *source = wl_data_device_manager_create_data_source(ctx.data_device_manager);
    get_plat(win)->data_source = source;
    if (!source)
        return false;

    wl_data_source_add_listener(source, &data_source_listener, win);
    wl_data_source_offer(source, "text/plain");
    wl_data_source_offer(source, "text/plain;charset=utf-8");

    struct seat *seat = find_first_seat(win);
    wl_data_device_set_selection(seat->selection.data_device, source, seat->keyboard.serial);
    return true;
}

static bool do_set_primary(struct window *win) {
    if (!ctx.primary_selection_device_manager)
        return true;

    if (get_plat(win)->primary_selection_source)
        zwp_primary_selection_source_v1_destroy(get_plat(win)->primary_selection_source);

    struct zwp_primary_selection_source_v1 *source = zwp_primary_selection_device_manager_v1_create_source(ctx.primary_selection_device_manager);
    get_plat(win)->primary_selection_source = source;
    if (!source)
        return false;

    zwp_primary_selection_source_v1_add_listener(source, &primary_selection_source_listener, win);
    zwp_primary_selection_source_v1_offer(source, "text/plain");
    zwp_primary_selection_source_v1_offer(source, "text/plain;charset=utf-8");

    struct seat *seat = find_first_seat(win);
    zwp_primary_selection_device_v1_set_selection(seat->primary_selection.primary_selection_device, source, seat->keyboard.serial);
    return true;
}

static bool wayland_set_clip(struct window *win, enum clip_target target) {
    if (target == clip_clipboard)
        return do_set_clipboard(win);
    if (target == clip_primary)
        return do_set_primary(win);
    return false;
}

static bool do_paste_chunk(struct active_paste *paste) {
    uint8_t buf[4096+1];
    ssize_t n = read(paste->fd, buf, sizeof buf - 1);
    ssize_t n2 = n;

    /* Read second time to determine whether we are done */
    if (n > 0 && n < (ssize_t)sizeof buf - 1) {
        n2 = read(paste->fd, buf + n, sizeof buf - n);
        if (n2 > 0) n += n2;
    }

    if (n < 0) n = 0;

    bool done = (n2 == 0 || (n2 < 0 && errno != EINTR));

    if (paste->tail || n)
        term_paste(paste->wptr.win->term, buf, n, paste->utf8, false, done);

    paste->tail = true;
    return done;
}

static void handle_paste(void *paste_, uint32_t mask) {
    struct active_paste *paste = paste_;
    (void)mask;
    if (do_paste_chunk(paste))
        free_paste(paste);
}

static inline int do_start_paste(struct window *win, bool utf8) {
    int fds[2];
    if (pipe(fds) < 0)
        return -1;

    if (set_cloexec(fds[0]) < 0 || set_nonblocking(fds[0]) < 0) {
       close(fds[0]);
       close(fds[1]);
       return -1;
    }

    struct active_paste *paste = xzalloc(sizeof *paste);
    paste->fd = fds[0];
    paste->event = poller_add_fd(handle_paste, paste, fds[0], POLLIN);
    paste->utf8 = utf8;
    win_ptr_set(&paste->wptr, win, win_ptr_paste);
    list_insert_after(&ctx.paste_fds, &paste->link);

    return fds[1];
}

static void do_paste_primary(struct window *win) {
    if (!ctx.primary_selection_device_manager)
        return;

    struct seat *seat = find_first_seat(win);

    if (!seat) return;
    if (!seat->primary_selection.primary_selection_offer) return;
    if (!seat->primary_selection.is_selection) return;
    if (!seat->primary_selection.supported_mime) return;

    int fd = do_start_paste(win, seat->primary_selection.mime_utf8);
    if (fd < 0) return;

    zwp_primary_selection_offer_v1_receive(seat->primary_selection.primary_selection_offer, seat->primary_selection.supported_mime, fd);
    close(fd);
}

static void do_paste_clipboard(struct window *win) {
    struct seat *seat = find_first_seat(win);

    if (!seat) return;
    if (!seat->selection.data_offer) return;
    if (!seat->selection.is_selection) return;
    if (!seat->selection.supported_mime) return;

    int fd = do_start_paste(win, seat->selection.mime_utf8);
    if (fd < 0) return;

    wl_data_offer_receive(seat->selection.data_offer, seat->selection.supported_mime, fd);
    close(fd);
}

static void wayland_paste(struct window *win, enum clip_target target) {
    if (target == clip_clipboard)
        do_paste_clipboard(win);
    else if (target == clip_primary)
        do_paste_primary(win);
}

static bool wayland_has_error(void) {
    return wl_display_get_error(dpl) != 0;
}

static ssize_t wayland_get_opaque_size(void) {
    return sizeof(struct wayland_window);
}

static void wayland_flush(void) {
    wl_display_flush(dpl);
    if (wl_display_dispatch_pending(dpl))
        poller_skip_wait();
}

static void handle_keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard, uint32_t format, int32_t fd, uint32_t size) {
    struct seat *seat = data;
    (void)wl_keyboard;

    if (gconfig.trace_events)
        info("Event[%p]: keyboard.keymap(%x, %d, %d)", data, format, fd, size);

    assert(format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1);

    /* Only need to create context once, while keymap can change dynamically */
    if (!seat->keyboard.xkb_ctx) {
        seat->keyboard.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        if (!seat->keyboard.xkb_ctx) {
            warn("Can't create XKB context");
            return;
        }
    }

    void *addr = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);

    close(fd);

    if (addr == MAP_FAILED) {
        warn("Can't map XKB keymap");
        return;
    }

    struct xkb_keymap *new_keymap = xkb_keymap_new_from_string(seat->keyboard.xkb_ctx, addr,
                                                               XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);

    munmap(addr, size);

    if (!new_keymap) {
        warn("Can't create XKB keymap");
        return;
    }

    struct xkb_state *new_state = xkb_state_new(new_keymap);

    xkb_keymap_unref(new_keymap);

    if (!new_state) {
        warn("Can't get window xkb state");
        return;
    }

    if (seat->keyboard.xkb_state)
        xkb_state_unref(seat->keyboard.xkb_state);

    seat->keyboard.xkb_state = new_state;

}

static void handle_keyboard_enter(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
    struct seat *seat = data;
    (void)wl_keyboard, (void)keys;

    struct window *win = wl_surface_get_user_data(surface);

    if (gconfig.trace_events)
        info("Event[%p,%p]: keyboard.enter(serial=%x)", data, (void *)win, serial);

    win->any_event_happend = true;
    win_ptr_set(&seat->keyboard.wptr, win, win_ptr_keyboard);

    seat->keyboard.serial = serial;
    seat->keyboard.last_key = 0;

    handle_focus(win, true);

#if 0
    // FIXME: Should we really do that?
    uint32_t *key;
    wl_array_for_each(key, keys) {
        seat->keyboard.last_key = *key + 8;
        handle_keydown(win, seat->keyboard.xkb_state, *key + 8);
    }
#endif

}

static inline void seat_stop_autorepeat(struct seat *seat, uint32_t key) {
    if (seat->keyboard.last_key != key && key) return;
    if (!seat->keyboard.autorepeat_timer) return;

    poller_remove(seat->keyboard.autorepeat_timer);
    seat->keyboard.autorepeat_timer = NULL;
}

static bool handle_autorepeat2(void *seat_) {
    struct seat *seat = seat_;
    struct window *win = seat->keyboard.wptr.win;
    if (!win) return false;

    handle_keydown(win, seat->keyboard.xkb_state, seat->keyboard.last_key);
    return true;
}

static bool handle_autorepeat(void *seat_) {
    struct seat *seat = seat_;
    struct window *win = seat->keyboard.wptr.win;
    if (win) {
        seat->keyboard.autorepeat_timer = poller_add_timer(handle_autorepeat2, seat, seat->keyboard.autorepeat_repeat);
        handle_keydown(win, seat->keyboard.xkb_state, seat->keyboard.last_key);
    }
    return false;
}

static inline void seat_start_autorepeat(struct seat *seat, uint32_t key) {
    if (seat->keyboard.autorepeat_timer)
        poller_remove(seat->keyboard.autorepeat_timer);
    seat->keyboard.autorepeat_timer = poller_add_timer(handle_autorepeat, seat, seat->keyboard.autorepeat_initial);
    seat->keyboard.last_key = key;
}

static void wayland_set_autorepeat(struct window *win, bool set) {
    LIST_FOREACH(it, &get_plat(win)->pointers) {
        struct window_ptr *ptr = CONTAINEROF(it, struct window_ptr, link);
        if (ptr->kind == win_ptr_keyboard) {
            struct seat *seat = CONTAINEROF(ptr, struct seat, keyboard.wptr);
            if (!set) seat_stop_autorepeat(seat, 0);
        }
    }
}

static void handle_keyboard_leave(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, struct wl_surface *surface) {
    struct seat *seat = data;
    (void)wl_keyboard;

    struct window *win = surface ? wl_surface_get_user_data(surface) : NULL;

    if (gconfig.trace_events)
        info("Event[%p,%p]: keyboard.leave(serial=%x)", data, (void *)win, serial);

    if (seat->keyboard.autorepeat_timer)
        seat_stop_autorepeat(seat, 0);

    if (seat->keyboard.wptr.win) {
        seat->keyboard.wptr.win->any_event_happend = true;
        handle_focus(seat->keyboard.wptr.win, false);
    }

    assert(seat->keyboard.wptr.win == win);

    win_ptr_clear(&seat->keyboard.wptr);
}

static void handle_keyboard_key(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    struct seat *seat = data;
    (void)wl_keyboard;

   struct window *win = seat->keyboard.wptr.win;

    if (gconfig.trace_events)
        info("Event[%p]: keyboard.key(serial=%x, time=%x, key=%x, state=%x)", data, serial, time, key, state);

    win->any_event_happend = true;
    win_ptr_ping(&seat->keyboard.wptr);

    /* We need to fixup linux keycode and convert it to XKB keycode
     * (yes, that's just offset by 8 Linux)*/
    key += 8;

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        if (win->autorepeat)
            seat_start_autorepeat(seat, key);
        handle_keydown(win, seat->keyboard.xkb_state, key);
    } else {
        if (win->autorepeat)
            seat_stop_autorepeat(seat, key);
    }
}

static void handle_keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
    struct seat *seat = data;
    (void)wl_keyboard;

    win_ptr_ping(&seat->keyboard.wptr);

    xkb_state_update_mask(seat->keyboard.xkb_state, mods_depressed, mods_latched, mods_locked, 0, 0, group);
    seat->keyboard.mask = xkb_state_serialize_mods(seat->keyboard.xkb_state, XKB_STATE_MODS_EFFECTIVE) & mask_mod_mask;

    if (gconfig.trace_events) {
        info("Event[%p]: keyboard.modifiers(serial=%x, mods_depressed=%x, mods_latched=%x, mods_locked=%x, group=%x)",
             data, serial, mods_depressed, mods_latched, mods_locked, group);
    }
}

static void handle_keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard, int32_t rate, int32_t delay) {
    struct seat *seat = data;
    (void)wl_keyboard;

    seat->keyboard.autorepeat_initial = delay*(SEC/1000);
    seat->keyboard.autorepeat_repeat = SEC / rate;

    if (gconfig.trace_events)
        info("Event[%p]: keyboard.repeat_info(rate=%d, delay=%d)", data, rate, delay);
}

struct wl_keyboard_listener keyboard_listener = {
    .enter = handle_keyboard_enter,
    .leave = handle_keyboard_leave,
    .key = handle_keyboard_key,
    .keymap = handle_keyboard_keymap,
    .modifiers = handle_keyboard_modifiers,
    .repeat_info = handle_keyboard_repeat_info,
};

static void handle_pointer_enter(void *data, struct wl_pointer *wl_pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t surface_x, wl_fixed_t surface_y) {
    struct seat *seat = data;
    struct window *win = wl_surface_get_user_data(surface);
    (void)wl_pointer;

    if (gconfig.trace_events) {
        info("Event[%p, %p]: pointer.enter(serial=%x, x=%f, y=%f)",
             data, (void *)win, serial, wl_fixed_to_double(surface_x), wl_fixed_to_double(surface_y));
    }

    activate_cursor_for_seat(win, seat);

    win->any_event_happend = true;
    win_ptr_set(&seat->pointer.wptr, win, win_ptr_other);

    seat->pointer.event_mask |= POINTER_EVENT_ENTER;
    seat->pointer.serial = serial;
    seat->pointer.surface_x = surface_x;
    seat->pointer.surface_y = surface_y;
}

static void handle_pointer_leave(void *data, struct wl_pointer *wl_pointer, uint32_t serial, struct wl_surface *surface) {
    struct seat *seat = data;
    (void)wl_pointer;

    struct window *win = surface ? wl_surface_get_user_data(surface) : NULL;

    if (gconfig.trace_events)
        info("Event[%p, %p]: pointer.leave(serial=%x)", data, (void *)win, serial);

    assert(win == seat->pointer.wptr.win);

    seat->pointer.serial = serial;
    seat->pointer.event_mask |= POINTER_EVENT_LEAVE;
}

static void handle_pointer_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {
    struct seat *seat = data;
    (void)wl_pointer;

    if (gconfig.trace_events) {
        info("Event[%p]: pointer.motion(time=%x, x=%f, y=%f)",
             data, time, wl_fixed_to_double(surface_x), wl_fixed_to_double(surface_y));
    }

    seat->pointer.event_mask |= POINTER_EVENT_MOTION;
    seat->pointer.time = time;
    seat->pointer.surface_x = surface_x;
    seat->pointer.surface_y = surface_y;
}

static void handle_pointer_button(void *data, struct wl_pointer *wl_pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
    struct seat *seat = data;
    (void)wl_pointer;

    if (gconfig.trace_events)
        info("Event[%p]: pointer.button(serial=%x, time=%x, button=%x, state=%x)", data, serial, time, button, state);

    assert(!(seat->pointer.event_mask & POINTER_EVENT_BUTTON));

    seat->pointer.event_mask |= POINTER_EVENT_BUTTON;
    seat->pointer.serial = serial;
    seat->pointer.time = time;
    seat->pointer.button = button;
    seat->pointer.state = state;
}

static void handle_pointer_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time, uint32_t axis, wl_fixed_t value) {
    struct seat *seat = data;
    (void)wl_pointer;

    if (gconfig.trace_events)
        info("Event[%p]: pointer.axis(time=%x, axis=%x, value=%x)", data, time, axis, value);

    seat->pointer.event_mask |= POINTER_EVENT_AXIS;
    seat->pointer.time = time;
    seat->pointer.axes[axis].used = true;
    seat->pointer.axes[axis].value = value;
}

static void handle_pointer_axis_source(void *data, struct wl_pointer *wl_pointer, uint32_t axis_source) {
    struct seat *seat = data;
    (void)wl_pointer;

    if (gconfig.trace_events)
        info("Event[%p]: pointer.axis_source(axis_source=%x)", data, axis_source);

    seat->pointer.event_mask |= POINTER_EVENT_AXIS_SOURCE;
    seat->pointer.axis_source = axis_source;
}

static void handle_pointer_axis_stop(void *data, struct wl_pointer *wl_pointer, uint32_t time, uint32_t axis) {
    struct seat *seat = data;
    (void)wl_pointer;

    if (gconfig.trace_events)
        info("Event[%p]: pointer.axis_stop(time=%x, axis=%x)", data, time, axis);

    seat->pointer.event_mask |= POINTER_EVENT_AXIS_STOP;
    seat->pointer.time = time;
    seat->pointer.axes[axis].used = true;
}

static void handle_pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer, uint32_t axis, int32_t discrete) {
    struct seat *seat = data;
    (void)wl_pointer;

    if (gconfig.trace_events)
        info("Event[%p]: pointer.axis_discrete(axis=%x, discrete=%x)", data, axis, discrete);

    seat->pointer.event_mask |= POINTER_EVENT_AXIS_DISCRETE;
    seat->pointer.axes[axis].used = true;
    seat->pointer.axes[axis].discrete = discrete;
}

static void handle_pointer_axis_value120(void *data, struct wl_pointer *wl_pointer, uint32_t axis, int32_t value120) {
    struct seat *seat = data;
    (void)wl_pointer;

    if (gconfig.trace_events)
        info("Event[%p]: pointer.axis_value120(axis=%x, value120=%x)", data, axis, value120);

    /* Only one of axis_value120, axis_discrete is sent to the client, depending on the protocol version.
     * axis_value120 is for version >= 8, axis_discrete is for lower versions. */

    seat->pointer.event_mask |= POINTER_EVENT_AXIS_DISCRETE;
    seat->pointer.axes[axis].used = true;

    int32_t v120 = seat->pointer.axes[axis].discrete120 + value120;
    seat->pointer.axes[axis].discrete = v120/120;
    seat->pointer.axes[axis].discrete120 = v120%120;
}

static void handle_pointer_axis_relative_direction(void *data, struct wl_pointer *wl_pointer, uint32_t axis, uint32_t direction) {
    struct seat *seat = data;
    (void)wl_pointer;

    if (gconfig.trace_events)
        info("Event[%p]: pointer.axis_relative_direction(axis=%x, direction=%x)", data, axis, direction);

    seat->pointer.event_mask |= POINTER_EVENT_AXIS_RELATIVE_DIRECTION;
    seat->pointer.axes[axis].used = true;
    seat->pointer.axes[axis].direction = direction;
}

static inline int32_t button_decode_one(uint32_t btn) {
    switch (btn) {
    case BTN_LEFT:
        return 0;
    case BTN_RIGHT:
        return 2;
    case BTN_MIDDLE:
        return 1;
    default:
        return -1;
    }
}

static bool try_handle_csd_button(struct window *win, struct seat *seat, int32_t code, bool pressed, int32_t x, int32_t y) {
    bool handled = false;
    /* If server does not provide server side decorations, provide some controls ourselves */
    if (get_plat(win)->use_ssd) return false;
    if (code >= 3) return false;
    if (!pressed) return false;

    bool left = x < win->cfg.border.left;
    bool right = x > win->cw*win->char_width + win->cfg.border.left;
    bool top = y < win->cfg.border.top;
    bool bottom = y > win->ch*(win->char_height + win->char_depth) + win->cfg.border.top;
    if (!left && !right && !top && !bottom) return false;

    if (code == 1) {
        /* Middle mouse button on top border --- close */
        if (top) {
            handled = true;
            free_window(win);
        }
    } else if (code == 0) {
        /* Left mouse button --- move */
        handled = true;
        xdg_toplevel_move(get_plat(win)->xdg_toplevel, seat->seat, seat->pointer.serial);
    } else if (code == 2) {
        /* Right mouse button --- resize */
        handled = true;
        enum xdg_toplevel_resize_edge edges = 0;
        if (left) edges |= XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
        if (right) edges |= XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
        if (top) edges |= XDG_TOPLEVEL_RESIZE_EDGE_TOP;
        if (bottom) edges |= XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
        xdg_toplevel_resize(get_plat(win)->xdg_toplevel, seat->seat, seat->pointer.serial, edges);
    }
    return handled;
}

static bool try_handle_csd_axis(struct window *win, struct seat *seat, int step, int32_t y) {
    /* If server does not provide server side decorations, provide some controls ourselves */
    if (get_plat(win)->use_ssd) return false;
    if (!seat->pointer.axes[0].discrete) return false;

    bool top = y < win->cfg.border.top;
    if (!top) return false;

    if (step > 0) {
        if (get_plat(win)->is_maximized) {
            xdg_toplevel_set_fullscreen(get_plat(win)->xdg_toplevel, NULL);
        } else {
            xdg_toplevel_set_maximized(get_plat(win)->xdg_toplevel);
        }
    } else if (step < 0) {
        if (get_plat(win)->is_fullscreen) {
            xdg_toplevel_unset_fullscreen(get_plat(win)->xdg_toplevel);
        } else if (get_plat(win)->is_maximized) {
            xdg_toplevel_unset_maximized(get_plat(win)->xdg_toplevel);
        } else {
            xdg_toplevel_set_minimized(get_plat(win)->xdg_toplevel);
        }
    }

    return true;
}

static void update_cursor(struct window *win, int32_t x, int32_t y) {
    struct cursor *new = NULL;
    bool left = x < win->cfg.border.left;
    bool right = x > win->cw*win->char_width + win->cfg.border.left;
    bool top = y < win->cfg.border.top;
    bool bottom = y > win->ch*(win->char_height + win->char_depth) + win->cfg.border.top;

    if ((left || right || top || bottom) && !get_plat(win)->use_ssd) {
        new = get_plat(win)->cursor_resize;
    } else {
        if (get_plat(win)->cursor_user)
            new = get_plat(win)->cursor_user;
        else if (win->rcstate.active_uri != EMPTY_URI && !win->rcstate.uri_pressed)
            new = get_plat(win)->cursor_uri;
        else
            new = get_plat(win)->cursor_default;
    }

    if (new != get_plat(win)->cursor)
        select_cursor(win, new);
}

static void handle_pointer_frame(void *data, struct wl_pointer *wl_pointer) {
    struct seat *seat = data;
    (void)wl_pointer;

    struct window *win =  seat->pointer.wptr.win;

    if (gconfig.trace_events)
        info("Event[%p,%p]: pointer.frame", data, (void *)win);

    if (!win) return;

    win->any_event_happend = true;
    win_ptr_ping(&seat->keyboard.wptr);

    int32_t x = wl_fixed_to_int(seat->pointer.surface_x);
    int32_t y = wl_fixed_to_int(seat->pointer.surface_y);

    if (seat->pointer.event_mask & POINTER_EVENT_BUTTON) {
        int32_t code = button_decode_one(seat->pointer.button);
        bool pressed = seat->pointer.state == WL_KEYBOARD_KEY_STATE_PRESSED;
        if (code >= 0 && !try_handle_csd_button(win, seat, code, pressed, x, y)) {
            if (pressed) seat->pointer.mask |= mask_button_1 << code;
            mouse_handle_input(win->term, (struct mouse_event) {
                /* mouse_event_press/mouse_event_release */
                .event = !pressed,
                .mask = seat->pointer.mask | seat->keyboard.mask,
                .x = x, .y = y,
                .button = code,
            });
            if (!pressed) seat->pointer.mask &= ~(mask_button_1 << code);
        }
    }

    /* Scroll wheel might report multiple button presses */
    if (seat->pointer.event_mask & POINTER_EVENT_AXIS_DISCRETE && seat->pointer.axes[0].used) {
        int step = 1 - 2*(seat->pointer.axes[0].discrete > 0);
        if (!try_handle_csd_axis(win, seat, step, y)) {
            for (; seat->pointer.axes[0].discrete; seat->pointer.axes[0].discrete += step) {
                struct mouse_event evt = {
                    .event = mouse_event_press,
                    .mask = seat->pointer.mask | seat->keyboard.mask,
                    .x = x, .y = y,
                    .button = 3 + (step < 0),
                };
                mouse_handle_input(win->term, evt);
                evt.mask |= mask_button_1 << evt.button;
                evt.event = mouse_event_release;
                mouse_handle_input(win->term, evt);
            }
        }
    }

    if (seat->pointer.event_mask & (POINTER_EVENT_ENTER | POINTER_EVENT_MOTION)) {

        /* Select appropriate cursor shape depending on context */
        update_cursor(win, x, y);

        mouse_handle_input(win->term, (struct mouse_event) {
            .event = mouse_event_motion,
            .mask = seat->pointer.mask | seat->keyboard.mask,
            .x = x, .y = y,
        });
    }

    /* We generally don't care about other events in the terminal... */

    /* We need to save last known state of the mouse to future reporting */
    get_plat(win)->mouse.x = x;
    get_plat(win)->mouse.y = y;
    get_plat(win)->mouse.mask = seat->pointer.mask | seat->keyboard.mask;

    if (seat->pointer.event_mask & POINTER_EVENT_LEAVE) {
        if (win) win->any_event_happend = true;
        win_ptr_clear(&seat->pointer.wptr);
    }

    seat->pointer.event_mask = 0;
}

struct wl_pointer_listener pointer_listener = {
    .enter = handle_pointer_enter,
    .leave = handle_pointer_leave,
    .motion = handle_pointer_motion,
    .button = handle_pointer_button,
    .axis = handle_pointer_axis,
    .frame = handle_pointer_frame,
    .axis_source = handle_pointer_axis_source,
    .axis_stop = handle_pointer_axis_stop,
    .axis_discrete = handle_pointer_axis_discrete,
    .axis_value120 = handle_pointer_axis_value120,
    .axis_relative_direction = handle_pointer_axis_relative_direction,
};

static void handle_seat_capabilities(void *data, struct wl_seat *wl_seat, uint32_t capabilities) {
    struct seat *seat = data;

    if (gconfig.trace_events)
        info("Event[%p]: seat.capabilities(capabilities=%x)", data, capabilities);

    if ((seat->capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !(capabilities & WL_SEAT_CAPABILITY_KEYBOARD)) {
        xkb_state_unref(seat->keyboard.xkb_state);
        xkb_context_unref(seat->keyboard.xkb_ctx);
        wl_keyboard_release(seat->keyboard.keyboard);
        seat->keyboard.xkb_state = NULL;
        seat->keyboard.xkb_ctx = NULL;
        seat->keyboard.keyboard = NULL;
    } else if (!(seat->capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && (capabilities & WL_SEAT_CAPABILITY_KEYBOARD)) {
        seat->keyboard.keyboard = wl_seat_get_keyboard(wl_seat);
        if (seat->keyboard.keyboard)
            wl_keyboard_add_listener(seat->keyboard.keyboard, &keyboard_listener, seat);
    }

    if ((seat->capabilities & WL_SEAT_CAPABILITY_POINTER) && !(capabilities & WL_SEAT_CAPABILITY_POINTER)) {
        wl_pointer_release(seat->pointer.pointer);
        seat->pointer.pointer = NULL;
    } else if (!(seat->capabilities & WL_SEAT_CAPABILITY_POINTER) && (capabilities & WL_SEAT_CAPABILITY_POINTER)) {
        seat->pointer.pointer = wl_seat_get_pointer(wl_seat);
        if (seat->pointer.pointer)
            wl_pointer_add_listener(seat->pointer.pointer, &pointer_listener, seat);
    }

    seat->capabilities =  capabilities;
}

static void handle_seat_name(void *data, struct wl_seat *wl_seat, const char *name) {
    struct seat *seat = data;
    (void)wl_seat;

    if (gconfig.trace_events)
        info("Event[%p]: seat.name(name=%s)", data, name);

    if (seat->name)
        free(seat->name);

    seat->name = strdup(name);
}

static struct wl_seat_listener seat_listener = {
    .capabilities = handle_seat_capabilities,
    .name = handle_seat_name,
};

static void handle_xdg_wm_base_pong(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial) {
    if (gconfig.trace_events)
        info("Event[%p]: xdg_wm_base.ping(serial=%x)", data, serial);

    xdg_wm_base_pong(xdg_wm_base, serial);
}

static struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = handle_xdg_wm_base_pong,
};

static void output_compute_dpi(struct output *output) {
    double dpi;
    if (ctx.output_manager)
        dpi = output->logical.width;
    else {
        if (output->scale == 0)
            output->scale = 1;
        dpi = output->physical.width / (double)output->scale;
    }

    if (!output->mm.width) {
        dpi = 96;
    } else {
        dpi *= 25.4/output->mm.width;
        if (dpi > 1000) dpi = 96;
    }

    output->dpi = dpi;
}

static void handle_output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y,
                                   int32_t physical_width, int32_t physical_height, int32_t subpixel,
                                   const char *make, const char *model, int32_t transform) {
    struct output *output = data;
    (void)wl_output, (void)make, (void)model;
    output->physical.x = x;
    output->physical.y = y;
    output->mm.height = physical_height;
    output->mm.width = physical_width;
    output->subpixel = subpixel;
    output->transform = transform;
}

static void handle_output_mode(void *data, struct wl_output *wl_output, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    struct output *output = data;
    (void)wl_output;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        output->refresh = refresh;
        output->physical.height = height;
        output->physical.width = width;
    }
}

static void handle_output_done(void *data, struct wl_output *wl_output) {
    struct output *output = data;
    (void)wl_output;
    output_compute_dpi(output);
    output->output_done = true;
}

static void handle_output_scale(void *data, struct wl_output *wl_output, int32_t factor) {
    struct output *output = data;
    (void)wl_output;
    output->scale = factor;
}

static void handle_output_name(void *data, struct wl_output *wl_output, const char *name) {
    struct output *output = data;
    (void)wl_output;
    output->name= strdup(name);
}

static void handle_output_description(void *data, struct wl_output *wl_output, const char *description) {
    struct output *output = data;
    (void)wl_output;
    output->descr = strdup(description);
}

static struct wl_output_listener output_listener = {
    .geometry = handle_output_geometry,
    .mode = handle_output_mode,
    .done = handle_output_done,
    .scale = handle_output_scale,
    .name = handle_output_name,
    .description = handle_output_description,
};

static void handle_xdg_output_logical_position(void *data, struct zxdg_output_v1 *zxdg_output_v1, int32_t x, int32_t y) {
    struct output *output = data;
    (void)zxdg_output_v1;
    output->logical.x = x;
    output->logical.y = y;
}

static void handle_xdg_output_logical_size(void *data, struct zxdg_output_v1 *zxdg_output_v1, int32_t width, int32_t height) {
    struct output *output = data;
    (void)zxdg_output_v1;
    output->logical.width = width;
    output->logical.height = height;
}

static void handle_xdg_output_done(void *data, struct zxdg_output_v1 *zxdg_output_v1) {
    // NOTE: Deprecated
    struct output *output = data;
    (void)zxdg_output_v1;
    output_compute_dpi(output);
    output->output_done = true;
}

static void handle_xdg_output_name(void *data, struct zxdg_output_v1 *zxdg_output_v1, const char *name) {
    // NOTE: Deprecated
    (void)data, (void)zxdg_output_v1, (void)name;
}

static void handle_xdg_output_description(void *data, struct zxdg_output_v1 *zxdg_output_v1, const char *description) {
    // NOTE: Deprecated
    (void)data, (void)zxdg_output_v1, (void)description;
}

static struct zxdg_output_v1_listener xdg_output_listener = {
    .logical_position = handle_xdg_output_logical_position,
    .logical_size = handle_xdg_output_logical_size,
    .done = handle_xdg_output_done,
    .name = handle_xdg_output_name,
    .description = handle_xdg_output_description,
};

static void handle_registry_global(void *data, struct wl_registry *wl_registry, uint32_t name, const char *interface, uint32_t version) {
    (void)data;

    if (gconfig.trace_events)
        info("Event[%p]: registry.global(name=%x, interface=%s, version=%d)", data, name, interface, version);

    // FIXME Specify actually used versions

    if (!strcmp(interface, wl_compositor_interface.name))
        ctx.compositor = wl_registry_bind(wl_registry, name, &wl_compositor_interface, version);
    else if (!strcmp(interface, wl_shm_interface.name))
        wl_shm = wl_registry_bind(wl_registry, name, &wl_shm_interface, version);
    else if (!strcmp(interface, xdg_wm_base_interface.name)) {
        ctx.xdg_wm_base = wl_registry_bind(wl_registry, name, &xdg_wm_base_interface, version);
        xdg_wm_base_add_listener(ctx.xdg_wm_base, &xdg_wm_base_listener, NULL);
    } else if (!strcmp(interface, wl_data_device_manager_interface.name)) {
        ctx.data_device_manager = wl_registry_bind(wl_registry, name, &wl_data_device_manager_interface, version);
    } else if (!strcmp(interface, zwp_primary_selection_device_manager_v1_interface.name)) {
        ctx.primary_selection_device_manager = wl_registry_bind(wl_registry, name, &zwp_primary_selection_device_manager_v1_interface, version);
    } else if (!strcmp(interface, zxdg_decoration_manager_v1_interface.name)) {
        ctx.decoration_manager = wl_registry_bind(wl_registry, name, &zxdg_decoration_manager_v1_interface, version);
    } else if (!strcmp(interface, zxdg_output_manager_v1_interface.name)) {
        ctx.output_manager = wl_registry_bind(wl_registry, name, &zxdg_output_manager_v1_interface, version);
        if (ctx.output_manager) {
            /* Iterate over all output objects encountered before zxdg_output_manager_v1 was found */
            LIST_FOREACH(it, &ctx.outputs) {
                struct output *output = CONTAINEROF(it, struct output, link);
                output->xdg_output = zxdg_output_manager_v1_get_xdg_output(ctx.output_manager, output->output);
                zxdg_output_v1_add_listener(output->xdg_output, &xdg_output_listener, output);
            }
        }
    } else if (!strcmp(interface, wl_output_interface.name)) {
        struct wl_output *wl_output = wl_registry_bind(wl_registry, name, &wl_output_interface, version);
        if (wl_output) {
            struct output *output = xzalloc(sizeof *output);
            output->id = name;
            output->output = wl_output;
            wl_output_add_listener(wl_output, &output_listener, output);
            if (ctx.output_manager) {
                list_insert_after(&ctx.outputs, &output->link);
                output->xdg_output = zxdg_output_manager_v1_get_xdg_output(ctx.output_manager, wl_output);
                zxdg_output_v1_add_listener(output->xdg_output, &xdg_output_listener, output);
            } else {
                free(output);
            }
        }
    } else if (!strcmp(interface, wl_seat_interface.name)) {
        struct seat *seat = xzalloc(sizeof *seat);
        seat->seat = wl_registry_bind(wl_registry, name, &wl_seat_interface, version);

        if (seat->seat) {
            wl_seat_add_listener(seat->seat, &seat_listener, seat);
            list_insert_after(&ctx.seats, &seat->link);
            seat->id = name;
        } else {
            free(seat);
        }
    }
}

static void free_output(struct output *output) {
    list_remove(&output->link);

    if (output->xdg_output)
        zxdg_output_v1_destroy(output->xdg_output);
    if (output->output)
        wl_output_destroy(output->output);
    if (output->name)
        free(output->name);
    if (output->descr)
        free(output->descr);
    free(output);
}

static void free_seat(struct seat *seat) {
    if (!seat) return;

    if (seat->keyboard.wptr.win) {
        handle_keyboard_leave(seat, seat->keyboard.keyboard,
                              UINT32_MAX, get_plat(seat->keyboard.wptr.win)->surface);
    }

    if (seat->pointer.wptr.win) {
        handle_pointer_leave(seat, seat->pointer.pointer,
                             UINT32_MAX, get_plat(seat->pointer.wptr.win)->surface);
    }

    list_remove(&seat->link);

    if (seat->capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
        if (seat->keyboard.xkb_state)
            xkb_state_unref(seat->keyboard.xkb_state);
        if (seat->keyboard.xkb_ctx)
            xkb_context_unref(seat->keyboard.xkb_ctx);
        if (seat->keyboard.keyboard)
            wl_keyboard_release(seat->keyboard.keyboard);
    }

    if (seat->capabilities & WL_SEAT_CAPABILITY_POINTER) {
        if (seat->pointer.pointer)
            wl_pointer_release(seat->pointer.pointer);
    }

    if (seat->selection.data_offer)
        wl_data_offer_destroy(seat->selection.data_offer);
    if (seat->selection.data_device)
        wl_data_device_release(seat->selection.data_device);

    if (seat->primary_selection.primary_selection_offer)
        zwp_primary_selection_offer_v1_destroy(seat->primary_selection.primary_selection_offer);
    if (seat->primary_selection.primary_selection_device)
        zwp_primary_selection_device_v1_destroy(seat->primary_selection.primary_selection_device);

    if (seat->keyboard.autorepeat_timer)
        poller_remove(seat->keyboard.autorepeat_timer);

    if (seat->name)
        free(seat->name);
    if (seat->seat)
        wl_seat_release(seat->seat);
    free(seat);
}

static void handle_registry_global_remove(void *data, struct wl_registry *wl_registry, uint32_t name) {
    (void)wl_registry;

    if (gconfig.trace_events)
        info("Event[%p]: registry.global_remove(name=%x)", data, name);

    LIST_FOREACH_SAFE(it, &ctx.seats) {
        struct seat *seat = CONTAINEROF(it, struct seat, link);
        if (seat->id == name) {
            free_seat(seat);
            return;
        }
    }

    LIST_FOREACH_SAFE(it, &ctx.outputs) {
        struct output *output = CONTAINEROF(it, struct output, link);
        if (output->id == name) {
            free_output(output);
            return;
        }
    }

    warn("Unknown global removed: %x", name);
}

static struct wl_registry_listener registry_listener = {
    .global = handle_registry_global,
    .global_remove = handle_registry_global_remove,
};

static void wayland_free(void) {
    poller_remove(ctx.dpl_event);

    ctx.renderer_free_context();

    LIST_FOREACH_SAFE(it, &ctx.seats) {
        struct seat *seat = CONTAINEROF(it, struct seat, link);
        free_seat(seat);
    }

    LIST_FOREACH_SAFE(it, &ctx.outputs) {
        struct output *output = CONTAINEROF(it, struct output, link);
        free_output(output);
    }

    if (ctx.cursors.data) {
        ht_iter_t it = ht_begin(&ctx.cursors);
        assert(!ht_current(&it));
        ht_free(&ctx.cursors);
    }

    if (ctx.cursor_theme)
        wl_cursor_theme_destroy(ctx.cursor_theme);
    if (wl_shm)
        wl_shm_destroy(wl_shm);
    if (ctx.registry)
        wl_registry_destroy(ctx.registry);
    if (ctx.compositor)
        wl_compositor_destroy(ctx.compositor);
    if (ctx.data_device_manager)
        wl_data_device_manager_destroy(ctx.data_device_manager);
    if (ctx.primary_selection_device_manager)
        zwp_primary_selection_device_manager_v1_destroy(ctx.primary_selection_device_manager);
    if (ctx.decoration_manager)
        zxdg_decoration_manager_v1_destroy(ctx.decoration_manager);
    if (ctx.output_manager)
        zxdg_output_manager_v1_destroy(ctx.output_manager);
    if (ctx.xdg_wm_base)
        xdg_wm_base_destroy(ctx.xdg_wm_base);

    if (dpl)
        wl_display_disconnect(dpl);

    dpl = NULL;
    wl_shm = NULL;
    memset(&ctx, 0, sizeof ctx);
}

static void wayland_handle_events(void *data_, uint32_t mask) {
    (void)data_;
    if (mask & (POLLIN | POLLERR | POLLHUP))
        wl_display_dispatch(dpl);
}

static void handle_callback_done(void *data, struct wl_callback *wl_callback, uint32_t callback_data) {
    struct window *win = data;
    (void)callback_data;
    if (get_plat(win)->frame_callback)
        win->inhibit_render_counter--;
    wl_callback_destroy(wl_callback);
    get_plat(win)->frame_callback = NULL;
}

struct wl_callback_listener frame_callback_listener = {
    .done = handle_callback_done,
};

static void wayland_draw_done(struct window *win) {
    wl_surface_attach(get_plat(win)->surface, get_plat(win)->buffer, 0, 0);

    struct wl_callback *cb = wl_surface_frame(get_plat(win)->surface);
    wl_callback_add_listener(cb, &frame_callback_listener, win);
    if (!get_plat(win)->frame_callback)
        win->inhibit_render_counter++;
    get_plat(win)->frame_callback = cb;

    wl_surface_commit(get_plat(win)->surface);
}

static struct platform_vtable wayland_vtable = {
    .get_screen_size = wayland_get_screen_size,
    .has_error = wayland_has_error,
    .get_opaque_size = wayland_get_opaque_size,
    .flush = wayland_flush,
    .get_position = wayland_get_position,
    .init_window = wayland_init_window,
    .free_window = wayland_free_window,
    .set_clip = wayland_set_clip,
    .bell = wayland_bell,
    .enable_mouse_events = wayland_enable_mouse_events,
    .get_pointer = wayland_get_pointer,
    .get_title = wayland_get_title,
    .map_window = wayland_map_window,
    .move_window = wayland_move_window,
    .paste = wayland_paste,
    .resize_window = wayland_resize_window,
    .set_icon_label = wayland_set_icon_label,
    .set_title = wayland_set_title,
    .set_urgency = wayland_set_urgency,
    .update_colors = wayland_update_colors,
    .window_action = wayland_window_action,
    .update_props = wayland_update_window_props,
    .fixup_geometry = wayland_fixup_geometry,
    .set_autorepeat = wayland_set_autorepeat,
    .select_cursor = wayland_select_cursor,
    .set_pointer_mode = wayland_set_pointer_mode,
    .draw_end = wayland_draw_done,
    .after_read = wayland_after_read,
    .free = wayland_free,
};

static inline const char *backend_to_str(enum renderer_backend backend) {
    switch (backend) {
    case renderer_wayland_shm:
        return "Wayland shm";
    default:
        return "UNKNOWN";
    }
}

static void load_cursor_theme(void) {
    const char *theme = getenv("XCURSOR_THEME");
    const char *size_string = getenv("XCURSOR_SIZE");
    ssize_t size = 24;

    ht_init(&ctx.cursors, HT_INIT_CAPS, cursor_cmp);

    if (size_string) {
        errno = 0;
        char *end = NULL;
        unsigned long xsize = strtoul(size_string, &end, 0);
        if (!errno && !*end)
            size = xsize;
        else
            warn("Invalid XCURSOR_SIZE=\"%s\"", size_string);
    }

    ctx.cursor_theme = wl_cursor_theme_load(theme, size, wl_shm);
    if (!ctx.cursor_theme) {
        warn("Unable to load cursor theme '%s'", theme);
        return;
    }
}

const struct platform_vtable *platform_init_wayland(struct instance_config *cfg) {
    if (gconfig.backend != renderer_wayland_shm &&
        gconfig.backend != renderer_wayland &&
        gconfig.backend != renderer_auto) return NULL;

    if (!(dpl = wl_display_connect(NULL))) {
        if (gconfig.backend == renderer_auto)
            return NULL;
        die("Can't connect to Wayland server");
    }

    ctx.dpl_event = poller_add_fd(wayland_handle_events, NULL, wl_display_get_fd(dpl), POLLIN);
    list_init(&ctx.paste_fds);
    list_init(&ctx.seats);
    list_init(&ctx.outputs);

    ctx.registry = wl_display_get_registry(dpl);
    if (!ctx.registry) {
        wayland_free();
        die("Can't get wayland registry");
    }

    if (wl_registry_add_listener(ctx.registry, &registry_listener, NULL) == -1 ||
            wl_display_roundtrip(dpl) == -1) {
        wayland_free();
        die("Can't perform initial roundtrip");
    }

    if (!wl_shm || !ctx.compositor || !ctx.data_device_manager || !ctx.xdg_wm_base || list_empty(&ctx.seats)) {
        wayland_free();
        die("Can't find required globals");
    }

    load_cursor_theme();

    // FIXME: Can we do that inside a callback?

    LIST_FOREACH(it, &ctx.seats) {
        struct seat *seat = CONTAINEROF(it, struct seat, link);
        seat->selection.data_device = wl_data_device_manager_get_data_device(ctx.data_device_manager, seat->seat);
        if (!seat->selection.data_device) {
            wayland_free();
            die("Can't add data device");
        }
        wl_data_device_add_listener(seat->selection.data_device, &data_device_listener, seat);

        if (ctx.primary_selection_device_manager) {
            seat->primary_selection.primary_selection_device = zwp_primary_selection_device_manager_v1_get_device(ctx.primary_selection_device_manager, seat->seat);
            if (!seat->primary_selection.primary_selection_device) {
                warn("Can't add primary data device");
            } else {
                zwp_primary_selection_device_v1_add_listener(seat->primary_selection.primary_selection_device, &primary_selection_device_listener, seat);
            }
        }
    }

    wl_display_roundtrip(dpl);

    double dpi = 0;
    LIST_FOREACH(it, &ctx.outputs)
        dpi = MAX(dpi, CONTAINEROF(it, struct output, link)->dpi);
    if (dpi > 0)
        set_default_dpi(dpi, cfg);

    switch (gconfig.backend) {
    case renderer_auto:
    case renderer_wayland:
#if USE_WAYLANDSHM
    case renderer_wayland_shm:
        if (gconfig.trace_misc)
            info("Selected Wayland SHM backend");
        wayland_vtable.update = wayland_shm_update;
        wayland_vtable.reload_font = shm_reload_font;
        wayland_vtable.resize = shm_resize;
        wayland_vtable.resize_exact = wayland_shm_resize_exact;
        wayland_vtable.copy = shm_copy;
        wayland_vtable.submit_screen = shm_submit_screen;
        wayland_vtable.shm_create_image = wayland_shm_create_image;
        ctx.renderer_recolor_border = shm_recolor_border;
        ctx.renderer_free = wayland_shm_free;
        ctx.renderer_free_context = wayland_shm_free_context;
        wayland_shm_init_context();
        break;
#endif
    default:
        die("Unsupported backend '%s'", backend_to_str(gconfig.backend));
    }

    return &wayland_vtable;
}
