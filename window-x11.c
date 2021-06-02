/* Copyright (c) 2019-2021, Evgeny Baskov. All rights reserved */

#include "feature.h"

#define _POSIX_C_SOURCE 200809L

#if USE_PPOLL
#   define _GNU_SOURCE
#endif

#include "config.h"
#include "mouse.h"
#include "term.h"
#include "uri.h"
#include "util.h"
#include "window-x11.h"

#include <errno.h>
#include <poll.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xkb.h>
#include <xkbcommon/xkbcommon-x11.h>

#define NSST_CLASS "Nsst"

struct context {
    xcb_screen_t *screen;
    xcb_colormap_t mid;
    xcb_visualtype_t *vis;

    struct atom_ {
        xcb_atom_t _NET_WM_PID;
        xcb_atom_t _NET_WM_NAME;
        xcb_atom_t _NET_WM_ICON_NAME;
        xcb_atom_t _NET_WM_STATE;
        xcb_atom_t _NET_WM_STATE_FULLSCREEN;
        xcb_atom_t _NET_WM_STATE_MAXIMIZED_VERT;
        xcb_atom_t _NET_WM_STATE_MAXIMIZED_HORZ;
        xcb_atom_t _NET_ACTIVE_WINDOW;
        xcb_atom_t _NET_MOVERESIZE_WINDOW;
        xcb_atom_t WM_DELETE_WINDOW;
        xcb_atom_t WM_PROTOCOLS;
        xcb_atom_t WM_NORMAL_HINTS;
        xcb_atom_t WM_SIZE_HINTS;
        xcb_atom_t WM_CHANGE_STATE;
        xcb_atom_t UTF8_STRING;
        xcb_atom_t CLIPBOARD;
        xcb_atom_t INCR;
        xcb_atom_t TARGETS;
    } atom;

    struct xkb_context *xkb_ctx;
    struct xkb_state *xkb_state;
    struct xkb_keymap *xkb_keymap;

    int32_t xkb_core_kbd;
    uint8_t xkb_base_event;
    uint8_t xkb_base_err;
};

static struct context ctx;
xcb_connection_t *con = NULL;

static struct window *window_for_xid(xcb_window_t xid) {
    for (struct window *win = win_list_head; win; win = win->next) {
        if (win->wid == xid) {
            win->any_event_happend = 1;
            return win;
        }
    }
    return NULL;
}

static xcb_atom_t intern_atom(const char *atom) {
    xcb_atom_t at;
    xcb_generic_error_t *err = NULL;
    xcb_intern_atom_cookie_t c = xcb_intern_atom(con, 0, strlen(atom), atom);
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(con, c, &err);
    if (err) {
        warn("Can't intern atom: %s", atom);
        free(err);
    }
    at = reply->atom;
    free(reply);
    return at;
}

static bool update_keymap(void) {
    struct xkb_keymap *new_keymap = xkb_x11_keymap_new_from_device(ctx.xkb_ctx, con, ctx.xkb_core_kbd, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!new_keymap) {
        warn("Can't create XKB keymap");
        return 0;
    }
    struct xkb_state *new_state = xkb_x11_state_new_from_device(new_keymap, con, ctx.xkb_core_kbd);
    if (!new_state) {
        xkb_keymap_unref(new_keymap);
        warn("Can't get window xkb state");
        return 0;
    }
    if (ctx.xkb_state)
        xkb_state_unref(ctx.xkb_state);
    if (ctx.xkb_keymap)
        xkb_keymap_unref(ctx.xkb_keymap);
    ctx.xkb_keymap = new_keymap;
    ctx.xkb_state = new_state;
    return 1;
}

static bool configure_xkb(void) {
    uint16_t xkb_min = 0, xkb_maj = 0;
    int res = xkb_x11_setup_xkb_extension(con, XKB_X11_MIN_MAJOR_XKB_VERSION,
                                          XKB_X11_MIN_MINOR_XKB_VERSION, XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS,
                                          &xkb_maj, &xkb_min, &ctx.xkb_base_event, &ctx.xkb_base_err);

    if (!res || xkb_maj < XKB_X11_MIN_MAJOR_XKB_VERSION) {
        warn("Can't get suitable XKB verion");
        return 0;
    }
    ctx.xkb_core_kbd = xkb_x11_get_core_keyboard_device_id(con);
    if (ctx.xkb_core_kbd == -1) {
        warn("Can't get core keyboard device");
        return 0;
    }

    ctx.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ctx.xkb_ctx) {
        warn("Can't create XKB context");
        return 0;
    }

    if (!update_keymap())
        goto cleanup_context;

    const uint32_t events = XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY |
            XCB_XKB_EVENT_TYPE_MAP_NOTIFY | XCB_XKB_EVENT_TYPE_STATE_NOTIFY;
    const uint32_t nkn_details = XCB_XKB_NKN_DETAIL_KEYCODES;
    const uint32_t map_parts = XCB_XKB_MAP_PART_KEY_TYPES | XCB_XKB_MAP_PART_KEY_SYMS |
            XCB_XKB_MAP_PART_MODIFIER_MAP | XCB_XKB_MAP_PART_EXPLICIT_COMPONENTS |
            XCB_XKB_MAP_PART_KEY_ACTIONS | XCB_XKB_MAP_PART_VIRTUAL_MODS |
            XCB_XKB_MAP_PART_VIRTUAL_MOD_MAP;
    const uint32_t state_details = XCB_XKB_STATE_PART_MODIFIER_BASE | XCB_XKB_STATE_PART_MODIFIER_LATCH |
            XCB_XKB_STATE_PART_MODIFIER_LOCK | XCB_XKB_STATE_PART_GROUP_BASE |
            XCB_XKB_STATE_PART_GROUP_LATCH | XCB_XKB_STATE_PART_GROUP_LOCK;
    const xcb_xkb_select_events_details_t details = {
        .affectNewKeyboard = nkn_details, .newKeyboardDetails = nkn_details,
        .affectState = state_details, .stateDetails = state_details,
    };
    xcb_void_cookie_t c;
    c = xcb_xkb_select_events_aux_checked(con, ctx.xkb_core_kbd, events,
                                          0, 0, map_parts, map_parts, &details);
    if (check_void_cookie(c)) {
        warn("Can't select XKB events");
        goto cleanup_state;
    }

    if (!update_keymap()) {
        warn("Can't update keymap");
        goto cleanup_state;
    }

    return 1;

cleanup_state:
    xkb_state_unref(ctx.xkb_state);
    xkb_keymap_unref(ctx.xkb_keymap);
cleanup_context:
    xkb_context_unref(ctx.xkb_ctx);
    return 0;
}

void init_platform_context(void) {
    int screenp = 0;
    con = xcb_connect(NULL, &screenp);
    if (!con) die("Can't connect to X server");

    poller_alloc_index(xcb_get_file_descriptor(con), POLLIN | POLLHUP);

    xcb_screen_iterator_t sit = xcb_setup_roots_iterator(xcb_get_setup(con));
    for (; sit.rem; xcb_screen_next(&sit))
        if (!screenp--) break;
    if (screenp != -1) {
        xcb_disconnect(con);
        die("Can't find default screen");
    }
    ctx.screen = sit.data;

    xcb_depth_iterator_t dit = xcb_screen_allowed_depths_iterator(ctx.screen);
    for (; dit.rem; xcb_depth_next(&dit))
        if (dit.data->depth == TRUE_COLOR_ALPHA_DEPTH) break;
    if (dit.data->depth != TRUE_COLOR_ALPHA_DEPTH) {
        xcb_disconnect(con);
        die("Can't get 32-bit visual");
    }

    xcb_visualtype_iterator_t vit = xcb_depth_visuals_iterator(dit.data);
    for (; vit.rem; xcb_visualtype_next(&vit))
        if (vit.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR) break;

    if (vit.data->_class != XCB_VISUAL_CLASS_TRUE_COLOR) {
        xcb_disconnect(con);
        die("Can't get 32-bit visual");
    }

    ctx.vis = vit.data;

    ctx.mid = xcb_generate_id(con);
    xcb_void_cookie_t c = xcb_create_colormap_checked(con, XCB_COLORMAP_ALLOC_NONE,
                                       ctx.mid, ctx.screen->root, ctx.vis->visual_id);
    if (check_void_cookie(c)) {
        xcb_disconnect(con);
        die("Can't create colormap");
    }

    if (!configure_xkb()) {
        xcb_disconnect(con);
        die("Can't configure XKB");
    }

    // Intern all used atoms
    ctx.atom._NET_WM_PID = intern_atom("_NET_WM_PID");
    ctx.atom._NET_WM_NAME = intern_atom("_NET_WM_NAME");
    ctx.atom._NET_WM_ICON_NAME = intern_atom("_NET_WM_ICON_NAME");
    ctx.atom._NET_WM_STATE = intern_atom("_NET_WM_STATE");
    ctx.atom._NET_WM_STATE_FULLSCREEN = intern_atom("_NET_WM_STATE_FULLSCREEN");
    ctx.atom._NET_WM_STATE_MAXIMIZED_VERT = intern_atom("_NET_WM_STATE_MAXIMIZED_VERT");
    ctx.atom._NET_WM_STATE_MAXIMIZED_HORZ = intern_atom("_NET_WM_STATE_MAXIMIZED_HORZ");
    ctx.atom._NET_ACTIVE_WINDOW = intern_atom("_NET_ACTIVE_WINDOW");
    ctx.atom._NET_MOVERESIZE_WINDOW = intern_atom("_NET_MOVERESIZE_WINDOW");
    ctx.atom.WM_DELETE_WINDOW = intern_atom("WM_DELETE_WINDOW");
    ctx.atom.WM_PROTOCOLS = intern_atom("WM_PROTOCOLS");
    ctx.atom.WM_NORMAL_HINTS = intern_atom("WM_NORMAL_HINTS");
    ctx.atom.WM_SIZE_HINTS = intern_atom("WM_SIZE_HINTS");
    ctx.atom.WM_CHANGE_STATE = intern_atom("WM_CHANGE_STATE");
    ctx.atom.UTF8_STRING = intern_atom("UTF8_STRING");
    ctx.atom.CLIPBOARD = intern_atom("CLIPBOARD");
    ctx.atom.INCR = intern_atom("INCR");
    ctx.atom.TARGETS = intern_atom("TARGETS");

    int32_t dpi = -1;
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(con));
    for (; it.rem; xcb_screen_next(&it))
        if (it.data) dpi = MAX(dpi, (it.data->width_in_pixels * 25.4)/it.data->width_in_millimeters);
    if (dpi > 0) set_default_dpi(dpi);
}

void free_platform_context(void) {
    xkb_state_unref(ctx.xkb_state);
    xkb_keymap_unref(ctx.xkb_keymap);
    xkb_context_unref(ctx.xkb_ctx);

    xcb_disconnect(con);
}

void window_platform_update_colors(struct window *win) {
    uint32_t values2[2];
    values2[0] = values2[1] = win->bg_premul;
    xcb_change_window_attributes(con, win->wid, XCB_CW_BACK_PIXEL, values2);
    xcb_change_gc(con, win->gc, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, values2);
}

void window_platform_set_mouse(struct window *win, bool enabled) {
    if (enabled) win->ev_mask |= XCB_EVENT_MASK_POINTER_MOTION;
    else win->ev_mask &= ~XCB_EVENT_MASK_POINTER_MOTION;
    xcb_change_window_attributes(con, win->wid, XCB_CW_EVENT_MASK, &win->ev_mask);
}

void window_resize(struct window *win, int16_t width, int16_t height) {
    if (win->cfg.height != height || win->cfg.width != width) {
        uint32_t vals[] = {width, height};
        xcb_configure_window(con, win->wid, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, vals);
        handle_resize(win, width, height);
    }
}

void window_move(struct window *win, int16_t x, int16_t y) {
    uint32_t vals[] = {x, y};
    xcb_configure_window(con, win->wid, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, vals);
}

static void send_wm_client_event(xcb_window_t win, uint32_t type, uint32_t data0, uint32_t data1) {
    xcb_client_message_event_t ev = {
        .response_type = XCB_CLIENT_MESSAGE,
        .format = 32,
        .sequence = 0,
        .window = win,
        .type = type,
        .data = {
            .data32 = {
                data0,
                data1,
                0, 0, 0,
            }
        }
    };
    xcb_send_event(con, 0, ctx.screen->root, XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
            XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT, (const char *)&ev);
}

#define WM_STATE_WITHDRAWN 0
#define WM_STATE_NORMAL 1
#define WM_STATE_ICONIC 3
#define _NET_WM_STATE_REMOVE 0
#define _NET_WM_STATE_ADD 1
#define _NET_WM_STATE_TOGGLE 2

inline static void save_pos(struct window *win) {
    if (!win->saved_geometry) {
        window_get_dim_ext(win, dim_window_position, &win->saved_x, &win->saved_y);
        window_get_dim(win, &win->saved_width, &win->saved_height);
        win->saved_geometry = 1;
    }
}

inline static void restore_pos(struct window *win) {
    if (win->saved_geometry) {
        uint32_t vals[] = {win->saved_x, win->saved_y, win->saved_width, win->saved_height};
        xcb_configure_window(con, win->wid, XCB_CONFIG_WINDOW_WIDTH |
                XCB_CONFIG_WINDOW_HEIGHT | XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, vals);
        handle_resize(win, vals[2], vals[3]);
        win->saved_geometry = 0;
    }
}

void window_action(struct window *win, enum window_action act) {
    switch(act) {
        uint32_t val;
    case action_minimize:
        send_wm_client_event(win->wid, ctx.atom.WM_CHANGE_STATE, WM_STATE_ICONIC, 0);
        break;
    case action_restore_minimized:
        send_wm_client_event(win->wid, ctx.atom._NET_ACTIVE_WINDOW, 1, XCB_CURRENT_TIME);
        break;
    case action_lower:
        val = XCB_STACK_MODE_BELOW;
        xcb_configure_window(con, win->wid, XCB_CONFIG_WINDOW_STACK_MODE, &val);
        break;
    case action_raise:
        val = XCB_STACK_MODE_ABOVE;
        xcb_configure_window(con, win->wid, XCB_CONFIG_WINDOW_STACK_MODE, &val);
        break;
    case action_maximize:
        save_pos(win);
        send_wm_client_event(win->wid, ctx.atom._NET_WM_STATE,
                _NET_WM_STATE_REMOVE,  ctx.atom._NET_WM_STATE_MAXIMIZED_HORZ);
        send_wm_client_event(win->wid, ctx.atom._NET_WM_STATE,
                _NET_WM_STATE_REMOVE,  ctx.atom._NET_WM_STATE_MAXIMIZED_HORZ);
        uint32_t vals[] = {0, 0, ctx.screen->width_in_pixels, ctx.screen->height_in_pixels};
        xcb_configure_window(con, win->wid, XCB_CONFIG_WINDOW_WIDTH |
                XCB_CONFIG_WINDOW_HEIGHT | XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, vals);
        handle_resize(win, vals[2], vals[3]);
        break;
    case action_maximize_width:
        save_pos(win);
        send_wm_client_event(win->wid, ctx.atom._NET_WM_STATE,
                _NET_WM_STATE_ADD,  ctx.atom._NET_WM_STATE_MAXIMIZED_HORZ);
        break;
    case action_maximize_height:
        save_pos(win);
        send_wm_client_event(win->wid, ctx.atom._NET_WM_STATE,
                _NET_WM_STATE_ADD,  ctx.atom._NET_WM_STATE_MAXIMIZED_VERT);
        break;
    case action_fullscreen:
        save_pos(win);
        send_wm_client_event(win->wid, ctx.atom._NET_WM_STATE,
                _NET_WM_STATE_ADD,  ctx.atom._NET_WM_STATE_FULLSCREEN);
        break;
    case action_restore:
        send_wm_client_event(win->wid, ctx.atom._NET_WM_STATE, _NET_WM_STATE_REMOVE,  ctx.atom._NET_WM_STATE_MAXIMIZED_HORZ);
        send_wm_client_event(win->wid, ctx.atom._NET_WM_STATE, _NET_WM_STATE_REMOVE,  ctx.atom._NET_WM_STATE_MAXIMIZED_HORZ);
        send_wm_client_event(win->wid, ctx.atom._NET_WM_STATE, _NET_WM_STATE_REMOVE,  ctx.atom._NET_WM_STATE_FULLSCREEN);
        restore_pos(win);
        break;
    case action_toggle_fullscreen:
        window_action(win, win->saved_geometry ? action_restore : action_fullscreen);
        /* fallthrough */
    case action_none:
        break;
    }
}

void window_platform_get_position(struct window *win, int16_t *x, int16_t *y) {
    xcb_get_geometry_cookie_t gc = xcb_get_geometry(con, win->wid);
    xcb_get_geometry_reply_t *rep = xcb_get_geometry_reply(con, gc, NULL);
    if (rep) {
        *x = rep->x;
        *y = rep->y;
        free(rep);
    }
}

void platform_context_get_screen_size(int16_t *x, int16_t *y) {
    *x = ctx.screen->width_in_pixels;
    *y = ctx.screen->height_in_pixels;
}

void window_platform_get_pointer(struct window *win, int32_t *px, int32_t *py, int32_t *pmask) {
    xcb_query_pointer_cookie_t c = xcb_query_pointer(con, win->wid);
    xcb_query_pointer_reply_t *qre = xcb_query_pointer_reply(con, c, NULL);
    if (qre) {
        *px = MIN(MAX(0, qre->win_x), win->cfg.width);
        *py = MIN(MAX(0, qre->win_y), win->cfg.height);
        *pmask = qre->mask;
        free(qre);
    }
}

#define WM_HINTS_LEN 8
void window_platform_set_urgency(struct window *win, bool set) {
    xcb_get_property_cookie_t c = xcb_get_property(con, 0, win->wid, XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS, 0, WM_HINTS_LEN);
    xcb_get_property_reply_t *rep = xcb_get_property_reply(con, c, NULL);
    if (rep) {
        uint32_t *hints = xcb_get_property_value(rep);
        if (set) *hints |= 256; // UrgentcyHint
        else *hints &= ~256; // UrgentcyHint
        xcb_change_property(con, XCB_PROP_MODE_REPLACE, win->wid, XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS, 32, WM_HINTS_LEN, hints);
        free(rep);
    }

}

void window_platform_bell(struct window *win, uint8_t vol) {
    xcb_xkb_bell(con, XCB_XKB_ID_USE_CORE_KBD, XCB_XKB_ID_DFLT_XI_CLASS,
            XCB_XKB_ID_DFLT_XI_ID, vol, 1, 0, 0, 0, XCB_ATOM_ANY, win->wid);
}

void window_platform_set_title(xcb_window_t wid, const char *title, bool utf8) {
    xcb_change_property(con, XCB_PROP_MODE_REPLACE, wid,
        utf8 ? ctx.atom._NET_WM_NAME : XCB_ATOM_WM_NAME,
        utf8 ? ctx.atom.UTF8_STRING : XCB_ATOM_STRING, 8, strlen(title), title);
}

void window_platform_set_icon_label(xcb_window_t wid, const char *title, bool utf8) {
    xcb_change_property(con, XCB_PROP_MODE_REPLACE, wid,
        utf8 ? ctx.atom._NET_WM_ICON_NAME : XCB_ATOM_WM_ICON_NAME,
        utf8 ? ctx.atom.UTF8_STRING : XCB_ATOM_STRING, 8, strlen(title), title);
}

static char *get_full_property(xcb_window_t wid, xcb_atom_t prop, xcb_atom_t *type, size_t *psize) {
    size_t left = 0, offset = 0, size = 0;
    char *data = NULL, *tmp;
    do {
        xcb_get_property_cookie_t c = xcb_get_property(con, 0, wid,
                prop, XCB_GET_PROPERTY_TYPE_ANY, offset, PASTE_BLOCK_SIZE/4);
        xcb_get_property_reply_t *rep = xcb_get_property_reply(con, c, NULL);
        if (!rep || !rep->value_len) {
            free(rep);
            break;
        }

        size_t len = rep->value_len * rep->format / 8;
        left = rep->bytes_after;

        if (type && !offset) *type = rep->type;
        offset += len/4;

        tmp = realloc(data, size + len + 1);
        if (!tmp) {
            free(rep);
            break;
        }

        data = tmp;
        memcpy(data + size, xcb_get_property_value(rep), len);
        data[size += len] = 0;

        free(rep);
    } while(left);

    if (psize) *psize = size - 1;

    return data;
}

/* there's no window_platform_get_title() */
void window_get_title(struct window *win, enum title_target which, char **name, bool *utf8) {
    xcb_atom_t type = XCB_ATOM_ANY;
    char *data = NULL;
    if (which & target_title) {
        data = get_full_property(win->wid, ctx.atom._NET_WM_NAME, &type, NULL);
        if (!data) data = get_full_property(win->wid, XCB_ATOM_WM_NAME, &type, NULL);
    } else if (which & target_icon_label) {
        data = get_full_property(win->wid, ctx.atom._NET_WM_ICON_NAME, &type, NULL);
        if (!data) data = get_full_property(win->wid, XCB_ATOM_WM_ICON_NAME, &type, NULL);
    }
    if (utf8) *utf8 = type == ctx.atom.UTF8_STRING;
    if (name) *name = data;
    else free(data);
}

inline static uint32_t get_win_gravity_from_config(bool nx, bool ny) {
    switch (nx + 2 * ny) {
    case 0: return XCB_GRAVITY_NORTH_WEST;
    case 1: return XCB_GRAVITY_NORTH_EAST;
    case 2: return XCB_GRAVITY_SOUTH_WEST;
    default: return XCB_GRAVITY_NORTH_EAST;
    }
}

void window_platform_update_props(struct window *win) {
    uint32_t pid = getpid();
    xcb_change_property(con, XCB_PROP_MODE_REPLACE, win->wid, ctx.atom._NET_WM_PID, XCB_ATOM_CARDINAL, 32, 1, &pid);
    xcb_change_property(con, XCB_PROP_MODE_REPLACE, win->wid, ctx.atom.WM_PROTOCOLS, XCB_ATOM_ATOM, 32, 1, &ctx.atom.WM_DELETE_WINDOW);
    const char *extra;
    xcb_change_property(con, XCB_PROP_MODE_REPLACE, win->wid, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, sizeof(NSST_CLASS), NSST_CLASS);
    if ((extra = win->cfg.window_class))
        xcb_change_property(con, XCB_PROP_MODE_APPEND, win->wid, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, strlen(extra), extra);
    uint32_t nhints[] = {
        64 | 256, //PResizeInc, PBaseSize
        win->cfg.x, win->cfg.y, // Position
        win->cfg.width, win->cfg.height, // Size
        win->cfg.left_border * 2 + win->char_width, win->cfg.left_border * 2 + win->char_depth + win->char_height, // Min size
        0, 0, //Max size
        win->char_width, win->char_depth + win->char_height, // Size inc
        0, 0, 0, 0, // Min/max aspect
        win->cfg.left_border * 2 + win->char_width, win->cfg.left_border * 2 + win->char_depth + win->char_height, // Base size
        get_win_gravity_from_config(win->cfg.stick_to_right, win->cfg.stick_to_bottom), // Gravity
    };
    if (win->cfg.user_geometry)
        nhints[0] |= 1 | 2 | 512; // USPosition, USSize, PWinGravity
    else
        nhints[0] |= 4 | 8; // PPosition, PSize
    if (win->cfg.fixed) {
        nhints[7] = nhints[5] = nhints[3];
        nhints[8] = nhints[6] = nhints[4];
        nhints[0] |= 16 | 32; // PMinSize, PMaxSize
    }
    uint32_t wmhints[] = {
        1, // Flags: InputHint
        XCB_WINDOW_CLASS_INPUT_OUTPUT, // Input
        0, // Inital state
        XCB_NONE, // Icon pixmap
        XCB_NONE, // Icon Window
        0, 0, // Icon X and Y
        XCB_NONE, // Icon mask bitmap
        XCB_NONE // Window group
    };
    xcb_change_property(con, XCB_PROP_MODE_REPLACE, win->wid, ctx.atom.WM_NORMAL_HINTS,
            ctx.atom.WM_SIZE_HINTS, 8*sizeof(*nhints), sizeof(nhints)/sizeof(*nhints), nhints);
    xcb_change_property(con, XCB_PROP_MODE_REPLACE, win->wid, XCB_ATOM_WM_HINTS,
            XCB_ATOM_WM_HINTS, 8*sizeof(*wmhints), sizeof(wmhints)/sizeof(*wmhints), wmhints);
}


bool init_platform_window(struct window *win) {
    xcb_void_cookie_t c;

    win->ev_mask = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_VISIBILITY_CHANGE |
        XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
        XCB_EVENT_MASK_BUTTON_MOTION | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_PROPERTY_CHANGE;
    uint32_t mask1 =  XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL |
        XCB_CW_BIT_GRAVITY | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
    uint32_t values1[5] = { win->bg_premul, win->bg_premul, XCB_GRAVITY_NORTH_WEST, win->ev_mask, ctx.mid };

    int16_t x = win->cfg.x;
    int16_t y = win->cfg.y;

    // Adjust geometry
    if (win->cfg.stick_to_right) x += ctx.screen->width_in_pixels - win->cfg.width - 2;
    if (win->cfg.stick_to_bottom) y += ctx.screen->height_in_pixels - win->cfg.height - 2;

    win->wid = xcb_generate_id(con);
    c = xcb_create_window_checked(con, TRUE_COLOR_ALPHA_DEPTH, win->wid, ctx.screen->root,
                                  x, y, win->cfg.width, win->cfg.height, 0,
                                  XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                  ctx.vis->visual_id, mask1, values1);
    if (check_void_cookie(c)) return 0;

    win->gc = xcb_generate_id(con);
    uint32_t mask2 = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_GRAPHICS_EXPOSURES;
    uint32_t values2[3] = { win->bg_premul, win->bg_premul, 0 };

    c = xcb_create_gc_checked(con, win->gc, win->wid, mask2, values2);
    if (check_void_cookie(c)) return 0;

    window_platform_update_props(win);

    return 1;
}

void window_platform_map(struct window *win) {
    xcb_map_window(con, win->wid);
    xcb_flush(con);
}

void free_platform_window(struct window *win) {
    if (win->wid) {
        xcb_unmap_window(con, win->wid);
        renderer_free(win);
        xcb_free_gc(con, win->gc);
        xcb_destroy_window(con, win->wid);
        xcb_flush(con);
    }
}

inline static xcb_atom_t target_to_atom(enum clip_target target) {
    switch (target) {
    case clip_secondary: return XCB_ATOM_SECONDARY;
    case clip_primary: return XCB_ATOM_PRIMARY;
    default: return ctx.atom.CLIPBOARD;
    }
}

bool window_platform_set_clip(struct window *win, uint32_t time, enum clip_target target) {
    xcb_set_selection_owner(con, win->wid, target_to_atom(target), time);
    xcb_get_selection_owner_cookie_t so = xcb_get_selection_owner_unchecked(con, target_to_atom(target));
    xcb_get_selection_owner_reply_t *rep = xcb_get_selection_owner_reply(con, so, NULL);
    bool res = rep && rep->owner == win->wid;
    free(rep);

    return res;
}

void window_paste_clip(struct window *win, enum clip_target target) {
    xcb_convert_selection(con, win->wid, target_to_atom(target),
          term_is_utf8_enabled(win->term) ? ctx.atom.UTF8_STRING : XCB_ATOM_STRING, target_to_atom(target), XCB_CURRENT_TIME);
}

void window_platform_draw_rectangles(struct window *win, struct rect *rects, ssize_t rectc) {
    static_assert(sizeof(struct rect) == sizeof(xcb_rectangle_t), "These structs should be compatible");
    if (rectc) xcb_poly_fill_rectangle(con, win->wid, win->gc, rectc, (xcb_rectangle_t *)rects);
}

static void send_selection_data(struct window *win, xcb_window_t req, xcb_atom_t sel, xcb_atom_t target, xcb_atom_t prop, xcb_timestamp_t time) {
    xcb_selection_notify_event_t ev;
    ev.property = XCB_NONE;
    ev.requestor = req;
    ev.response_type = XCB_SELECTION_NOTIFY;
    ev.selection = sel;
    ev.target = target;
    ev.time = time;

    if (prop == XCB_NONE) prop = target;

    if (target == ctx.atom.TARGETS) {
        uint32_t data[] = {ctx.atom.UTF8_STRING, XCB_ATOM_STRING};
        xcb_change_property(con, XCB_PROP_MODE_REPLACE, req, prop, XCB_ATOM_ATOM, 32, sizeof(data)/sizeof(*data), data);
    } else if (target == ctx.atom.UTF8_STRING || target == XCB_ATOM_STRING) {
        uint8_t *data = NULL;

        if (sel == XCB_ATOM_PRIMARY) data = win->clipped[clip_primary];
        else if (sel == XCB_ATOM_SECONDARY) data = win->clipped[clip_secondary];
        else if (sel == ctx.atom.CLIPBOARD) data = term_is_keep_clipboard_enabled(win->term) ?
                win->clipboard : win->clipped[clip_clipboard];

        if (data) {
            xcb_change_property(con, XCB_PROP_MODE_REPLACE, req, prop, target, 8, strlen((char *)data), data);
            ev.property = prop;
        }
    }

    xcb_send_event(con, 1, req, 0, (const char *)&ev);
}

static void receive_selection_data(struct window *win, xcb_atom_t prop, bool pnotify) {
    if (prop == XCB_NONE) return;

    size_t left, offset = 0;

    do {
        xcb_get_property_cookie_t pc = xcb_get_property(con, 0, win->wid, prop, XCB_GET_PROPERTY_TYPE_ANY, offset, PASTE_BLOCK_SIZE/4);
        xcb_generic_error_t *err = NULL;
        xcb_get_property_reply_t *rep = xcb_get_property_reply(con, pc, &err);
        if (err || !rep) {
            free(err);
            free(rep);
            return;
        }
        left = rep->bytes_after;

        if (pnotify && !rep->value_len && !left) {
           win->ev_mask &= ~XCB_EVENT_MASK_PROPERTY_CHANGE;
           xcb_change_window_attributes(con, win->wid, XCB_CW_EVENT_MASK, &win->ev_mask);
        }

        if (rep->type == ctx.atom.INCR) {
           win->ev_mask |= XCB_EVENT_MASK_PROPERTY_CHANGE;
           xcb_change_window_attributes(con, win->wid, XCB_CW_EVENT_MASK, &win->ev_mask);
           xcb_delete_property(con, win->wid, prop);
           xcb_flush(con);
           continue;
        }

        ssize_t size = rep->format * rep->value_len / 8;
        window_paste_data(win, xcb_get_property_value(rep), size,
                           rep->type == ctx.atom.UTF8_STRING, !offset, !left);

        free(rep);
        offset += size / 4;
    } while (left > 0);

    xcb_delete_property(con, win->wid, prop);
}

bool platform_context_has_error(void) {
    return xcb_connection_has_error(con);
}

void handle_event(void) {
    for (xcb_generic_event_t *event, *nextev = NULL; nextev || (event = xcb_poll_for_event(con)); free(event)) {
        if (nextev) event = nextev, nextev = NULL;
        switch (event->response_type &= 0x7F) {
            struct window *win;
        case XCB_EXPOSE: {
            xcb_expose_event_t *ev = (xcb_expose_event_t *)event;
            if (!(win = window_for_xid(ev->window))) break;
            if (gconfig.trace_events) {
                info("Event: event=Expose win=0x%x x=%x y=%d width=%d height=%d",
                        ev->window, ev->x, ev->y, ev->width, ev->height);
            }
            handle_expose(win, (struct rect){ev->x, ev->y, ev->width, ev->height});
            break;
        }
        case XCB_CONFIGURE_NOTIFY: {
            xcb_configure_notify_event_t *ev = (xcb_configure_notify_event_t *)event;
            if (!(win = window_for_xid(ev->window))) break;
            if (gconfig.trace_events) {
                info("Event: event=ConfigureWindow win=0x%x x=%x y=%d width=%d"
                        " height=%d border=%d redir=%d above_win=0x%x event_win=0x%x",
                        ev->window, ev->x, ev->y, ev->width, ev->height, ev->border_width,
                        ev->override_redirect, ev->above_sibling, ev->event);
            }
            if (ev->width != win->cfg.width || ev->height != win->cfg.height)
                handle_resize(win, ev->width, ev->height);
            break;
        }
        case XCB_KEY_RELEASE: {
            xcb_key_release_event_t *ev = (xcb_key_release_event_t *)event;
            if (!(win = window_for_xid(ev->event))) break;
            if (gconfig.trace_events) {
                info("Event: event=KeyRelease win=0x%x keycode=0x%x", ev->event, ev->detail);
            }
            /* Skip key repeats if disabled */
            if (!win->autorepeat && (nextev = xcb_poll_for_queued_event(con)) &&
                    (nextev->response_type &= 0x7F) == XCB_KEY_PRESS &&
                    event->full_sequence == nextev->full_sequence) {
                free(nextev);
                nextev = NULL;
            }
            break;
        }
        case XCB_KEY_PRESS: {
            xcb_key_press_event_t *ev = (xcb_key_press_event_t *)event;
            if (!(win = window_for_xid(ev->event))) break;
            if (gconfig.trace_events) {
                info("Event: event=KeyPress win=0x%x keycode=0x%x", ev->event, ev->detail);
            }
            handle_keydown(win, ctx.xkb_state, ev->detail);
            break;
        }
        case XCB_FOCUS_IN:
        case XCB_FOCUS_OUT: {
            xcb_focus_in_event_t *ev = (xcb_focus_in_event_t *)event;
            if (!(win = window_for_xid(ev->event))) break;
            if (gconfig.trace_events) {
                info("Event: event=%s win=0x%x", ev->response_type == XCB_FOCUS_IN ?
                        "FocusIn" : "FocusOut", ev->event);
            }
            handle_focus(win, event->response_type == XCB_FOCUS_IN);
            break;
        }
        case XCB_BUTTON_RELEASE: /* All these events have same structure */
        case XCB_BUTTON_PRESS:
        case XCB_MOTION_NOTIFY: {
            xcb_motion_notify_event_t *ev = (xcb_motion_notify_event_t *)event;
            if (!(win = window_for_xid(ev->event))) break;
            if (gconfig.trace_events) {
                info("Event: event=%s mask=%d button=%d x=%d y=%d",
                        ev->response_type == XCB_BUTTON_PRESS ? "ButtonPress" :
                        ev->response_type == XCB_BUTTON_RELEASE ? "ButtonRelease" : "MotionNotify",
                        ev->state, ev->detail, ev->event_x, ev->event_y);
            }

            mouse_handle_input(win->term, (struct mouse_event) {
                /* XCB_BUTTON_PRESS -> mouse_event_press
                 * XCB_BUTTON_RELEASE -> mouse_event_release
                 * XCB_MOTION_NOTIFY -> mouse_event_motion */
                .event = (ev->response_type & 0xF7) - 4,
                .mask = ev->state & mask_state_mask,
                .x = ev->event_x,
                .y = ev->event_y,
                .button = ev->detail - XCB_BUTTON_INDEX_1,
            });
            break;
        }
        case XCB_SELECTION_CLEAR: {
            xcb_selection_clear_event_t *ev = (xcb_selection_clear_event_t *)event;
            if (!(win = window_for_xid(ev->owner))) break;
            if (gconfig.trace_events) {
                info("Event: event=SelectionClear owner=0x%x selection=0x%x", ev->owner, ev->selection);
            }
            // Clear even if set keep?
            mouse_clear_selection(win->term);
            break;
        }
        case XCB_PROPERTY_NOTIFY: {
            xcb_property_notify_event_t *ev = (xcb_property_notify_event_t *)event;
            if (!(win = window_for_xid(ev->window))) break;
            if (gconfig.trace_events) {
                info("Event: event=PropertyNotify window=0x%x property=0x%x state=%d",
                        ev->window, ev->atom, ev->state);
            }
            if ((ev->atom == XCB_ATOM_PRIMARY || ev->atom == XCB_ATOM_SECONDARY ||
                    ev->atom == ctx.atom.CLIPBOARD) && ev->state == XCB_PROPERTY_NEW_VALUE)
                receive_selection_data(win, ev->atom, 1);
            break;
        }
        case XCB_SELECTION_NOTIFY: {
            xcb_selection_notify_event_t *ev = (xcb_selection_notify_event_t *)event;
            if (!(win = window_for_xid(ev->requestor))) break;
            if (gconfig.trace_events) {
                info("Event: event=SelectionNotify owner=0x%x target=0x%x property=0x%x selection=0x%x",
                        ev->requestor, ev->target, ev->property, ev->selection);
            }
            receive_selection_data(win, ev->property, 0);
            break;
        }
        case XCB_SELECTION_REQUEST: {
            xcb_selection_request_event_t *ev = (xcb_selection_request_event_t *)event;
            if (!(win = window_for_xid(ev->owner))) break;
            if (gconfig.trace_events) {
                info("Event: event=SelectionRequest owner=0x%x requestor=0x%x target=0x%x property=0x%x selection=0x%x",
                        ev->owner, ev->requestor, ev->target, ev->property, ev->selection);
            }
            send_selection_data(win, ev->requestor, ev->selection, ev->target, ev->property, ev->time);
            break;
        }
        case XCB_CLIENT_MESSAGE: {
            xcb_client_message_event_t *ev = (xcb_client_message_event_t *)event;
            if (!(win = window_for_xid(ev->window))) break;
            if (gconfig.trace_events) {
                info("Event: event=ClientMessage window=0x%x type=0x%x data=[0x%08x,0x%08x,0x%08x,0x%08x,0x%08x]",
                    ev->window, ev->type, ev->data.data32[0], ev->data.data32[1],
                    ev->data.data32[2], ev->data.data32[3], ev->data.data32[4]);
            }
            if (ev->format == 32 && ev->data.data32[0] == ctx.atom.WM_DELETE_WINDOW) free_window(win);
            break;
        }
        case XCB_UNMAP_NOTIFY: {
            xcb_unmap_notify_event_t *ev = (xcb_unmap_notify_event_t *)event;
            if (!(win = window_for_xid(ev->window))) break;
            if (gconfig.trace_events) {
                info("Event: event=UnmapNotify window=0x%x", ev->window);
            }
            win->active = 0;
            break;
        }
        case XCB_MAP_NOTIFY: {
            xcb_map_notify_event_t *ev = (xcb_map_notify_event_t *)event;
            if (!(win = window_for_xid(ev->window))) break;
            if (gconfig.trace_events) {
                info("Event: event=MapNotify window=0x%x", ev->window);
            }
            win->active = 1;
            break;
        }
        case XCB_VISIBILITY_NOTIFY: {
            xcb_visibility_notify_event_t *ev = (xcb_visibility_notify_event_t *)event;
            if (!(win = window_for_xid(ev->window))) break;
            if (gconfig.trace_events) {
                info("Event: event=VisibilityNotify window=0x%x state=%d", ev->window, ev->state);
            }
            win->active = ev->state != XCB_VISIBILITY_FULLY_OBSCURED;
            break;
        }
        case XCB_DESTROY_NOTIFY:
        case XCB_REPARENT_NOTIFY:
           /* ignore */
           break;
        case 0: {
            xcb_generic_error_t *err = (xcb_generic_error_t*)event;
            warn("[X11 Error] major=%"PRIu8", minor=%"PRIu16", error=%"PRIu8, err->major_code, err->minor_code, err->error_code);
            break;
        }
        default:
            if (event->response_type == ctx.xkb_base_event) {
                struct _xkb_any_event {
                    uint8_t response_type;
                    uint8_t xkb_type;
                    uint16_t sequence;
                    xcb_timestamp_t time;
                    uint8_t device_id;
                } *xkb_ev = (struct _xkb_any_event *)event;

                if (gconfig.trace_events) {
                    info("Event: XKB Event %d", xkb_ev->xkb_type);
                }

                if (xkb_ev->device_id == ctx.xkb_core_kbd) {
                    switch (xkb_ev->xkb_type) {
                    case XCB_XKB_NEW_KEYBOARD_NOTIFY: {
                        xcb_xkb_new_keyboard_notify_event_t *ev = (xcb_xkb_new_keyboard_notify_event_t *)event;
                        if (ev->changed & XCB_XKB_NKN_DETAIL_KEYCODES)
                    case XCB_XKB_MAP_NOTIFY:
                            update_keymap();
                        break;
                    }
                    case XCB_XKB_STATE_NOTIFY: {
                        xcb_xkb_state_notify_event_t *ev = (xcb_xkb_state_notify_event_t *)event;
                        xkb_state_update_mask(ctx.xkb_state, ev->baseMods, ev->latchedMods, ev->lockedMods,
                                              ev->baseGroup, ev->latchedGroup, ev->lockedGroup);
                        break;
                    }
                    default:
                        warn("Unknown xcb-xkb event type: %02"PRIu8, xkb_ev->xkb_type);
                    }
                }
            } else warn("Unknown xcb event type: %02"PRIu8, event->response_type);
        }
    }
}
