/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

#include "feature.h"


#if USE_PPOLL
#   define _GNU_SOURCE
#endif

#include "config.h"
#include "mouse.h"
#include "poller.h"
#include "term.h"
#include "uri.h"
#include "util.h"
#include "window-impl.h"
#include "window-x11.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb_cursor.h>
#include <xcb/xcb.h>
#include <xcb/xkb.h>
#include <xkbcommon/xkbcommon-x11.h>

#define NSST_CLASS "Nsst"

struct cursor {
    ht_head_t link;
    char *name;
    int32_t refcount;
    xcb_cursor_t xcursor;
};

struct context {
    struct event *con_evt;
    xcb_screen_t *screen;
    xcb_colormap_t mid;
    xcb_visualtype_t *vis;
    xcb_cursor_context_t *cursor_ctx;

    hashtable_t cursors;
    xcb_cursor_t hidden_cursor;

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

    void (*renderer_recolor_border)(struct window *);
    void (*renderer_free)(struct window *);
    void (*renderer_free_context)(void);
};

static struct context ctx;
xcb_connection_t *con = NULL;

static struct window *window_for_xid(xcb_window_t xid) {
    LIST_FOREACH(it, &win_list_head) {
        struct window *win = CONTAINEROF(it, struct window, link);
        if (get_plat(win)->wid == xid) {
            win->any_event_happend = true;
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

static void x11_update_colors(struct window *win) {
    uint32_t values2[2];
    values2[0] = values2[1] = win->bg_premul;
    xcb_change_window_attributes(con, get_plat(win)->wid, XCB_CW_BACK_PIXEL, values2);
    xcb_change_gc(con, get_plat(win)->gc, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, values2);
    ctx.renderer_recolor_border(win);
}

static bool x11_resize_window(struct window *win, int16_t width, int16_t height) {
    if (win->cfg.geometry.r.height != height || win->cfg.geometry.r.width != width) {
        uint32_t vals[] = {width, height};
        xcb_void_cookie_t c = xcb_configure_window_checked(con, get_plat(win)->wid,
                XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, vals);
        return !check_void_cookie(c);
    }
    return false;
}

static void x11_move_window(struct window *win, int16_t x, int16_t y) {
    uint32_t vals[] = {x, y};
    xcb_configure_window(con, get_plat(win)->wid, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, vals);
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

static inline void save_pos(struct window *win) {
    if (get_plat(win)->saved.width > 0) {
        struct extent p = window_get_position(win);
        struct extent s = window_get_size(win);
        get_plat(win)->saved = (struct rect) { p.width, p.height, s.width, s.height };
        assert(s.width > 0);
    }
}

static inline bool restore_pos(struct window *win) {
    xcb_void_cookie_t c;
    if (get_plat(win)->saved.width > 0) {
        uint32_t vals[] = {get_plat(win)->saved.x, get_plat(win)->saved.y, get_plat(win)->saved.width, get_plat(win)->saved.height};
        c = xcb_configure_window_checked(con, get_plat(win)->wid, XCB_CONFIG_WINDOW_WIDTH |
                XCB_CONFIG_WINDOW_HEIGHT | XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, vals);
        get_plat(win)->saved.width = 0;
        return !check_void_cookie(c);
    }
    return false;
}

static bool x11_window_action(struct window *win, enum window_action act) {
    switch (act) {
        uint32_t val;
    case action_minimize:
        send_wm_client_event(get_plat(win)->wid, ctx.atom.WM_CHANGE_STATE, WM_STATE_ICONIC, 0);
        break;
    case action_restore_minimized:
        send_wm_client_event(get_plat(win)->wid, ctx.atom._NET_ACTIVE_WINDOW, 1, XCB_CURRENT_TIME);
        break;
    case action_lower:
        val = XCB_STACK_MODE_BELOW;
        xcb_configure_window(con, get_plat(win)->wid, XCB_CONFIG_WINDOW_STACK_MODE, &val);
        break;
    case action_raise:
        val = XCB_STACK_MODE_ABOVE;
        xcb_configure_window(con, get_plat(win)->wid, XCB_CONFIG_WINDOW_STACK_MODE, &val);
        break;
    case action_maximize:
        save_pos(win);
        send_wm_client_event(get_plat(win)->wid, ctx.atom._NET_WM_STATE, _NET_WM_STATE_REMOVE,  ctx.atom._NET_WM_STATE_MAXIMIZED_HORZ);
        send_wm_client_event(get_plat(win)->wid, ctx.atom._NET_WM_STATE, _NET_WM_STATE_REMOVE,  ctx.atom._NET_WM_STATE_MAXIMIZED_VERT);
        uint32_t vals[] = {0, 0, ctx.screen->width_in_pixels, ctx.screen->height_in_pixels};
        xcb_void_cookie_t c;
        c = xcb_configure_window_checked(con, get_plat(win)->wid, XCB_CONFIG_WINDOW_WIDTH |
                XCB_CONFIG_WINDOW_HEIGHT | XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, vals);
        return !check_void_cookie(c);
    case action_maximize_width:
        save_pos(win);
        send_wm_client_event(get_plat(win)->wid, ctx.atom._NET_WM_STATE,
                _NET_WM_STATE_ADD,  ctx.atom._NET_WM_STATE_MAXIMIZED_HORZ);
        break;
    case action_maximize_height:
        save_pos(win);
        send_wm_client_event(get_plat(win)->wid, ctx.atom._NET_WM_STATE,
                _NET_WM_STATE_ADD,  ctx.atom._NET_WM_STATE_MAXIMIZED_VERT);
        break;
    case action_fullscreen:
        save_pos(win);
        send_wm_client_event(get_plat(win)->wid, ctx.atom._NET_WM_STATE,
                _NET_WM_STATE_ADD,  ctx.atom._NET_WM_STATE_FULLSCREEN);
        break;
    case action_restore:
        send_wm_client_event(get_plat(win)->wid, ctx.atom._NET_WM_STATE, _NET_WM_STATE_REMOVE,  ctx.atom._NET_WM_STATE_MAXIMIZED_HORZ);
        send_wm_client_event(get_plat(win)->wid, ctx.atom._NET_WM_STATE, _NET_WM_STATE_REMOVE,  ctx.atom._NET_WM_STATE_MAXIMIZED_VERT);
        send_wm_client_event(get_plat(win)->wid, ctx.atom._NET_WM_STATE, _NET_WM_STATE_REMOVE,  ctx.atom._NET_WM_STATE_FULLSCREEN);
        return restore_pos(win);
    case action_toggle_fullscreen:
        return x11_window_action(win, get_plat(win)->saved.width > 0 ? action_restore : action_fullscreen);
    case action_none:
        break;
    }
    return false;
}

static struct extent x11_get_position(struct window *win) {
    xcb_get_geometry_cookie_t gc = xcb_get_geometry(con, get_plat(win)->wid);
    xcb_get_geometry_reply_t *rep = xcb_get_geometry_reply(con, gc, NULL);
    struct extent res = {0, 0};
    if (rep) {
        res.width = rep->x;
        res.height = rep->y;
        free(rep);
    }
    return res;
}

struct extent x11_get_screen_size(struct window *win) {
    (void)win;
    return (struct extent) { ctx.screen->width_in_pixels, ctx.screen->height_in_pixels };
}

static void x11_get_pointer(struct window *win, struct extent *p, int32_t *pmask) {
    xcb_query_pointer_cookie_t c = xcb_query_pointer(con, get_plat(win)->wid);
    xcb_query_pointer_reply_t *qre = xcb_query_pointer_reply(con, c, NULL);
    if (qre) {
        p->width = MIN(MAX(0, qre->win_x), win->cfg.geometry.r.width);
        p->height = MIN(MAX(0, qre->win_y), win->cfg.geometry.r.height);
        *pmask = qre->mask;
        free(qre);
    }
}

#define WM_HINTS_LEN 8
static void x11_set_urgency(struct window *win, bool set) {
    xcb_get_property_cookie_t c = xcb_get_property(con, 0, get_plat(win)->wid, XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS, 0, WM_HINTS_LEN);
    xcb_get_property_reply_t *rep = xcb_get_property_reply(con, c, NULL);
    if (rep) {
        uint32_t *hints = xcb_get_property_value(rep);
        if (set) *hints |= 256; /* UrgentcyHint */
        else *hints &= ~256; /* UrgentcyHint */
        xcb_change_property(con, XCB_PROP_MODE_REPLACE, get_plat(win)->wid, XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS, 32, WM_HINTS_LEN, hints);
        free(rep);
    }
}

static void x11_bell(struct window *win, uint8_t vol) {
    xcb_xkb_bell(con, XCB_XKB_ID_USE_CORE_KBD, XCB_XKB_ID_DFLT_XI_CLASS,
            XCB_XKB_ID_DFLT_XI_ID, vol, 1, 0, 0, 0, XCB_ATOM_ANY, get_plat(win)->wid);
}

static bool cursor_cmp(const ht_head_t *a, const ht_head_t *b) {
    const struct cursor *ua = (const struct cursor *)a;
    const struct cursor *ub = (const struct cursor *)b;
    return !strcmp(ua->name, ub->name);
}

static struct cursor *get_cursor(const char *name) {
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

    xcb_cursor_t xc = xcb_cursor_load_cursor(ctx.cursor_ctx, name);
    if (xc == XCB_NONE) {
        warn("Unable to load cursor '%s'", name);
        return NULL;
    }

    struct cursor *new = xalloc(sizeof *new);
    *new = (struct cursor) {
        .link.hash = dummy.link.hash,
        .name = strdup(name),
        .refcount = 1,
        .xcursor = xc,
    };

    ht_insert_hint(&ctx.cursors, it, &new->link);
    return new;
}

static void unref_cursor(struct cursor *csr) {
    if (!csr) return;
    if (!--csr->refcount) {
        ht_erase(&ctx.cursors, &csr->link);
        xcb_free_cursor(con, csr->xcursor);
        free(csr->name);
        free(csr);
    }
}

static inline struct cursor *ref_cursor(struct cursor *csr) {
    csr->refcount++;
    return csr;
}

static void update_hide_cursor(struct window *win) {
    bool hide = (get_plat(win)->cursor_mode == hide_always ||
                (get_plat(win)->cursor_mode == hide_no_tracking &&
                     term_get_mstate(win->term)->mouse_mode == mouse_mode_none));
    if (hide && !get_plat(win)->cursor_is_hidden) {
        uint32_t xc = ctx.hidden_cursor;
        xcb_change_window_attributes(con, get_plat(win)->wid, XCB_CW_CURSOR, &xc);
        get_plat(win)->cursor_is_hidden = true;
    } else if (!hide && get_plat(win)->cursor_is_hidden) {
        assert(get_plat(win)->cursor);
        uint32_t xc = get_plat(win)->cursor->xcursor;
        xcb_change_window_attributes(con, get_plat(win)->wid, XCB_CW_CURSOR, &xc);
        get_plat(win)->cursor_is_hidden = false;
    }
}

static void x11_enable_mouse_events(struct window *win, bool enabled) {
    if (enabled) get_plat(win)->ev_mask |= XCB_EVENT_MASK_POINTER_MOTION;
    else get_plat(win)->ev_mask &= ~XCB_EVENT_MASK_POINTER_MOTION;
    xcb_change_window_attributes(con, get_plat(win)->wid, XCB_CW_EVENT_MASK, &get_plat(win)->ev_mask);
    update_hide_cursor(win);
}

static void select_cursor(struct window *win, struct cursor *csr) {
    ref_cursor(csr);
    unref_cursor(get_plat(win)->cursor);
    get_plat(win)->cursor = csr;
    if (!get_plat(win)->cursor_is_hidden)
        xcb_change_window_attributes(con, get_plat(win)->wid, XCB_CW_CURSOR, &csr->xcursor);
}

static void do_create_hidden_cursor(void) {
    ctx.hidden_cursor = xcb_generate_id(con);
    /* This is is leaked, but it's fine, since we craete it only once */
    xcb_pixmap_t pm = xcb_generate_id(con);
    xcb_void_cookie_t c;
    c = xcb_create_pixmap_checked(con, 1, pm, ctx.screen->root, 1, 1);
    if (check_void_cookie(c)) return;
    c = xcb_create_cursor_checked(con, ctx.hidden_cursor, pm, pm, 0, 0, 0, 0, 0, 0, 0, 0);
    if (check_void_cookie(c)) return;
    xcb_free_pixmap(con, pm);
}

static void x11_select_cursor(struct window *win, const char *name) {
    if (!ctx.cursor_ctx) return;

    struct cursor *csr = get_cursor(name);
    unref_cursor(get_plat(win)->cursor_user);
    get_plat(win)->cursor_user = csr;

    if (!csr) csr = get_plat(win)->cursor_default;
    select_cursor(win, csr);
}

static void x11_set_pointer_mode(struct window *win, enum hide_pointer_mode mode) {
    get_plat(win)->cursor_mode = mode;
    update_hide_cursor(win);
}

static void x11_set_title(struct window *win, const char *title, bool utf8) {
    xcb_change_property(con, XCB_PROP_MODE_REPLACE, get_plat(win)->wid,
        utf8 ? ctx.atom._NET_WM_NAME : XCB_ATOM_WM_NAME,
        utf8 ? ctx.atom.UTF8_STRING : XCB_ATOM_STRING, 8, strlen(title), title);
}

static void x11_set_icon_label(struct window *win, const char *title, bool utf8) {
    xcb_change_property(con, XCB_PROP_MODE_REPLACE, get_plat(win)->wid,
        utf8 ? ctx.atom._NET_WM_ICON_NAME : XCB_ATOM_WM_ICON_NAME,
        utf8 ? ctx.atom.UTF8_STRING : XCB_ATOM_STRING, 8, strlen(title), title);
}

static char *get_full_property(xcb_window_t wid, xcb_atom_t prop, xcb_atom_t *type, size_t *psize) {
    size_t left = 0, offset = 0, size = 0, data_len = 0;
    char *data = NULL;
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

        data = xrealloc(data, data_len, size + len + 1);
        data_len = size + len + 1;

        memcpy(data + size, xcb_get_property_value(rep), len);
        data[size += len] = 0;

        free(rep);
    } while (left);

    if (psize) *psize = size - 1;

    return data;
}

static void x11_get_title(struct window *win, enum title_target which, char **name, bool *utf8) {
    xcb_atom_t type = XCB_ATOM_ANY;
    char *data = NULL;
    if (which & target_title) {
        data = get_full_property(get_plat(win)->wid, ctx.atom._NET_WM_NAME, &type, NULL);
        if (!data) data = get_full_property(get_plat(win)->wid, XCB_ATOM_WM_NAME, &type, NULL);
    } else if (which & target_icon_label) {
        data = get_full_property(get_plat(win)->wid, ctx.atom._NET_WM_ICON_NAME, &type, NULL);
        if (!data) data = get_full_property(get_plat(win)->wid, XCB_ATOM_WM_ICON_NAME, &type, NULL);
    }
    if (utf8) *utf8 = type == ctx.atom.UTF8_STRING;
    if (name) *name = data;
    else free(data);
}

static inline uint32_t get_win_gravity_from_config(bool nx, bool ny) {
    switch (nx + 2 * ny) {
    case 0: return XCB_GRAVITY_NORTH_WEST;
    case 1: return XCB_GRAVITY_NORTH_EAST;
    case 2: return XCB_GRAVITY_SOUTH_WEST;
    default: return XCB_GRAVITY_NORTH_EAST;
    }
}

enum {
    HINT_USPOSITION = (1 << 0),
    HINT_USSIZE = (1 << 1),
    HINT_PPOSITION = (1 << 2),
    HINT_PSIZE = (1 << 3),
    HINT_PMINSIZE = (1 << 4),
    HINT_PMAXSIZE = (1 << 5),
    HINT_PRESIZEINC = (1 << 6),
    HINT_PASPECT = (1 << 7),
    HINT_PBASESIZE = (1 << 8),
    HINT_PWINGRAVITY = (1 << 9),
};

struct normal_hints {
    uint32_t flags;
    uint32_t x, y;
    uint32_t width, height;
    uint32_t min_width, min_height;
    uint32_t max_width, max_height;
    uint32_t inc_width, inc_height;
    struct {
        uint32_t num;
        uint32_t den;
    } min_aspect, max_aspect;
    uint32_t base_width, base_height;
    uint32_t gravity;
};

enum {
    HINT_INPUT = (1 << 0),
    HINT_STATE = (1 << 1),
    HINT_ICONPIXMAP = (1 << 2),
    HINT_ICONWINDOW = (1 << 3),
    HINT_ICONPOSITION = (1 << 4),
    HINT_ICONMASK = (1 << 5),
    HINT_WINDOWGROUP = (1 << 6),
    HINT_MESSAGE = (1 << 7),
    HINT_URGENCY = (1 << 8),
};

struct wm_hints {
    uint32_t flags;
    uint32_t input;
    uint32_t initial_state;
    uint32_t icon_pixmap;
    uint32_t icon_window;
    uint32_t icon_x;
    uint32_t icon_y;
    uint32_t icon_mask;
};

void x11_update_window_props(struct window *win) {
    uint32_t pid = getpid();
    xcb_change_property(con, XCB_PROP_MODE_REPLACE, get_plat(win)->wid, ctx.atom._NET_WM_PID, XCB_ATOM_CARDINAL, 32, 1, &pid);
    xcb_change_property(con, XCB_PROP_MODE_REPLACE, get_plat(win)->wid, ctx.atom.WM_PROTOCOLS, XCB_ATOM_ATOM, 32, 1, &ctx.atom.WM_DELETE_WINDOW);
    const char *extra;
    xcb_change_property(con, XCB_PROP_MODE_REPLACE, get_plat(win)->wid, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, sizeof(NSST_CLASS), NSST_CLASS);
    if ((extra = win->cfg.window_class))
        xcb_change_property(con, XCB_PROP_MODE_APPEND, get_plat(win)->wid, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, strlen(extra), extra);

    struct normal_hints nhints = {
        .flags = HINT_PMINSIZE | HINT_PBASESIZE,
        .x = win->cfg.geometry.r.x, .y = win->cfg.geometry.r.y,
        .width = win->cfg.geometry.r.width, .height = win->cfg.geometry.r.height,
        .min_width = win->cfg.border.left + win->cfg.border.right + 2 * win->char_width,
        .min_height = win->cfg.border.top + win->cfg.border.bottom + win->char_depth + win->char_height,
        .base_width = win->cfg.geometry.r.width, .base_height = win->cfg.geometry.r.height,
        .gravity = get_win_gravity_from_config(win->cfg.geometry.stick_to_right, win->cfg.geometry.stick_to_bottom),
    };

    if (!win->cfg.smooth_resize) {
        nhints.flags |= HINT_PRESIZEINC;
        nhints.inc_width = win->char_width;
        nhints.inc_height = win->char_depth + win->char_height;
    }

    if (win->cfg.geometry.has_position)
        nhints.flags |= HINT_USPOSITION | HINT_PWINGRAVITY;
    else
        nhints.flags |= HINT_PPOSITION;

    if (win->cfg.geometry.has_extent)
        nhints.flags |= HINT_USSIZE;
    else
        nhints.flags |= HINT_PSIZE;

    if (win->cfg.fixed) {
        nhints.flags |= HINT_PMAXSIZE;
        nhints.max_width = nhints.min_width = nhints.width;
        nhints.max_height = nhints.min_height = nhints.height;
    }

    struct wm_hints wmhints = {
        .flags = HINT_INPUT,
        .input = XCB_WINDOW_CLASS_INPUT_OUTPUT, /* Input */
    };
    xcb_change_property(con, XCB_PROP_MODE_REPLACE, get_plat(win)->wid, ctx.atom.WM_NORMAL_HINTS,
            ctx.atom.WM_SIZE_HINTS, 8*sizeof(uint32_t), sizeof(nhints)/sizeof(uint32_t), &nhints);
    xcb_change_property(con, XCB_PROP_MODE_REPLACE, get_plat(win)->wid, XCB_ATOM_WM_HINTS,
            XCB_ATOM_WM_HINTS, 8*sizeof(uint32_t), sizeof(wmhints)/sizeof(uint32_t), &wmhints);
}

static void x11_move_resize(struct window *win, struct rect r) {
    uint32_t vals[] = { r.x, r.y, r.width, r.height };
    xcb_configure_window(con, get_plat(win)->wid,
        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
        XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, vals);
}

void x11_fixup_geometry(struct window *win) {
    if (win->cfg.geometry.char_geometry) {
        win->cw = MAX(win->cfg.geometry.r.width, 2);
        win->ch = MAX(win->cfg.geometry.r.height, 1);
        win->cfg.geometry.r.width = win->char_width * win->cw + win->cfg.border.left + win->cfg.border.right;
        win->cfg.geometry.r.height = (win->char_height + win->char_depth) * win->ch + win->cfg.border.top + win->cfg.border.bottom;
        win->cfg.geometry.char_geometry = false;

        struct extent ssize = x11_get_screen_size(win);
        if (win->cfg.geometry.stick_to_right)
            win->cfg.geometry.r.x += ssize.width - win->cfg.geometry.r.width - 2;
        if (win->cfg.geometry.stick_to_bottom)
            win->cfg.geometry.r.y += ssize.height - win->cfg.geometry.r.height - 2;

        x11_move_resize(win, win->cfg.geometry.r);
    } else {
        win->cw = MAX(2, (win->cfg.geometry.r.width - win->cfg.border.left - win->cfg.border.right) / win->char_width);
        win->ch = MAX(1, (win->cfg.geometry.r.height - win->cfg.border.top - win->cfg.border.bottom) / (win->char_height + win->char_depth));
    }
}

static bool x11_init_window(struct window *win) {
    xcb_void_cookie_t c;

    get_plat(win)->ev_mask = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_VISIBILITY_CHANGE |
        XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
        XCB_EVENT_MASK_BUTTON_MOTION | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_PROPERTY_CHANGE;
    uint32_t mask1 =  XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL |
        XCB_CW_BIT_GRAVITY | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
    uint32_t values1[5] = { win->bg_premul, win->bg_premul, XCB_GRAVITY_NORTH_WEST, get_plat(win)->ev_mask, ctx.mid };

    /* Adjust geometry. It will be incorrect if specified in characters. */
    struct rect r;
    if (!win->cfg.geometry.char_geometry) {
        if (win->cfg.geometry.stick_to_right)
            win->cfg.geometry.r.x += ctx.screen->width_in_pixels - win->cfg.geometry.r.width - 2;
        if (win->cfg.geometry.stick_to_bottom)
            win->cfg.geometry.r.y += ctx.screen->height_in_pixels - win->cfg.geometry.r.height - 2;
        r = win->cfg.geometry.r;
    } else {
        /* Dummy size */
        r = (struct rect) {0, 0, 640, 480};
    }

    get_plat(win)->wid = xcb_generate_id(con);
    c = xcb_create_window_checked(con, TRUE_COLOR_ALPHA_DEPTH, get_plat(win)->wid, ctx.screen->root,
                                  r.x, r.y, r.width, r.height, 0,
                                  XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                  ctx.vis->visual_id, mask1, values1);
    if (check_void_cookie(c)) return false;

    get_plat(win)->gc = xcb_generate_id(con);
    uint32_t mask2 = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_GRAPHICS_EXPOSURES;
    uint32_t values2[3] = { win->bg_premul, win->bg_premul, 0 };

    c = xcb_create_gc_checked(con, get_plat(win)->gc, get_plat(win)->wid, mask2, values2);
    if (check_void_cookie(c)) return false;

    // FIXME Reload cursor upon setting reloading (pointer_shape can change)
    // FIXME Try more cursor shapes, not just fall back to default one
    // FIXME Cleanup

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

static void x11_map_window(struct window *win) {
    xcb_map_window(con, get_plat(win)->wid);
    xcb_flush(con);
}

static void x11_free_window(struct window *win) {
    unref_cursor(get_plat(win)->cursor);
    unref_cursor(get_plat(win)->cursor_default);
    unref_cursor(get_plat(win)->cursor_uri);
    unref_cursor(get_plat(win)->cursor_user);

    if (get_plat(win)->wid) {
        xcb_unmap_window(con, get_plat(win)->wid);
        ctx.renderer_free(win);
        xcb_free_gc(con, get_plat(win)->gc);
        xcb_destroy_window(con, get_plat(win)->wid);
        xcb_flush(con);
    }
}

static inline xcb_atom_t target_to_atom(enum clip_target target) {
    switch (target) {
    case clip_secondary: return XCB_ATOM_SECONDARY;
    case clip_primary: return XCB_ATOM_PRIMARY;
    default: return ctx.atom.CLIPBOARD;
    }
}

#define CLIP_TIME_NOW 0

static bool x11_set_clip(struct window *win, enum clip_target target) {
    xcb_set_selection_owner(con, get_plat(win)->wid, target_to_atom(target), CLIP_TIME_NOW);
    xcb_get_selection_owner_cookie_t so = xcb_get_selection_owner_unchecked(con, target_to_atom(target));
    xcb_get_selection_owner_reply_t *rep = xcb_get_selection_owner_reply(con, so, NULL);
    bool res = rep && rep->owner == get_plat(win)->wid;
    free(rep);

    return res;
}

static void x11_paste(struct window *win, enum clip_target target) {
    xcb_convert_selection(con, get_plat(win)->wid, target_to_atom(target),
          term_is_utf8_enabled(win->term) ? ctx.atom.UTF8_STRING : XCB_ATOM_STRING, target_to_atom(target), XCB_CURRENT_TIME);
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
        uint32_t data[] = {ctx.atom.TARGETS, ctx.atom.UTF8_STRING, XCB_ATOM_STRING};
        ev.property = prop;
        xcb_change_property(con, XCB_PROP_MODE_REPLACE, req, prop, XCB_ATOM_ATOM, 32, LEN(data), data);
    } else if (target == ctx.atom.UTF8_STRING || target == XCB_ATOM_STRING) {
        uint8_t *data = NULL;

        if (sel == XCB_ATOM_PRIMARY) data = win->clipped[clip_primary];
        else if (sel == XCB_ATOM_SECONDARY) data = win->clipped[clip_secondary];
        else if (sel == ctx.atom.CLIPBOARD) data = term_is_keep_clipboard_enabled(win->term) ?
                win->clipboard : win->clipped[clip_clipboard];

        if (data) {
            // TODO Implement INCR selection
            //      In order to do that we need to remember every ongoing INCR
            //      request and associate an offset and data being sent.
            xcb_change_property(con, XCB_PROP_MODE_REPLACE, req, prop, target, 8, strlen((char *)data), data);
            ev.property = prop;
        }
    }

    xcb_send_event(con, 0, req, 0, (const char *)&ev);
}

static void receive_selection_data(struct window *win, xcb_atom_t prop, bool pnotify) {
    if (prop == XCB_NONE) return;

    size_t left, offset = 0;

    do {
        xcb_get_property_cookie_t pc = xcb_get_property(con, 0, get_plat(win)->wid, prop, XCB_GET_PROPERTY_TYPE_ANY, offset, PASTE_BLOCK_SIZE/4);
        xcb_generic_error_t *err = NULL;
        xcb_get_property_reply_t *rep = xcb_get_property_reply(con, pc, &err);
        if (err || !rep) {
            free(err);
            free(rep);
            return;
        }
        left = rep->bytes_after;

        if (pnotify && !rep->value_len && !left) {
           get_plat(win)->ev_mask &= ~XCB_EVENT_MASK_PROPERTY_CHANGE;
           xcb_change_window_attributes(con, get_plat(win)->wid, XCB_CW_EVENT_MASK, &get_plat(win)->ev_mask);
        }

        if (rep->type == ctx.atom.INCR) {
           get_plat(win)->ev_mask |= XCB_EVENT_MASK_PROPERTY_CHANGE;
           xcb_change_window_attributes(con, get_plat(win)->wid, XCB_CW_EVENT_MASK, &get_plat(win)->ev_mask);
           xcb_delete_property(con, get_plat(win)->wid, prop);
           xcb_flush(con);
           continue;
        }

        ssize_t size = rep->format * rep->value_len / 8;
        term_paste(win->term, xcb_get_property_value(rep), size,
                   rep->type == ctx.atom.UTF8_STRING, !offset, !left);

        free(rep);
        offset += size / 4;
    } while (left > 0);

    xcb_delete_property(con, get_plat(win)->wid, prop);
}

static bool x11_has_error(void) {
    return xcb_connection_has_error(con);
}

static ssize_t x11_get_opaque_size(void) {
    return sizeof(struct platform_window);
}

static void update_cursor(struct window *win) {
    struct cursor *new = NULL;
    if (get_plat(win)->cursor_user)
        new = get_plat(win)->cursor_user;
    else if (win->rcstate.active_uri != EMPTY_URI && !win->rcstate.uri_pressed)
        new = get_plat(win)->cursor_uri;
    else
        new = get_plat(win)->cursor_default;

    if (new != get_plat(win)->cursor)
        select_cursor(win, new);
}

static bool do_handle_events(bool can_read) {
    bool any = false;
    for (xcb_generic_event_t *event, *nextev = NULL; nextev || (event = can_read ? xcb_poll_for_event(con) : xcb_poll_for_queued_event(con)); free(event)) {
        any = true;
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
            struct rect damage = {ev->x, ev->y, ev->width, ev->height}, win_rect = window_rect(win);
            if (intersect_with(&damage, &win_rect))
                pvtbl->update(win, damage);
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
            if (ev->width != win->cfg.geometry.r.width || ev->height != win->cfg.geometry.r.height)
                handle_resize(win, ev->width, ev->height, false);
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
                info("Event: event=KeyPress win=0x%x keycode=0x%x state=%x", ev->event, ev->detail, ev->state);
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

            update_cursor(win);

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
            /* Clear even if set keep? */
            screen_damage_selection(term_screen(win->term));
            selection_clear(term_get_sstate(win->term));
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
                receive_selection_data(win, ev->atom, true);
            break;
        }
        case XCB_SELECTION_NOTIFY: {
            xcb_selection_notify_event_t *ev = (xcb_selection_notify_event_t *)event;
            if (!(win = window_for_xid(ev->requestor))) break;
            if (gconfig.trace_events) {
                info("Event: event=SelectionNotify owner=0x%x target=0x%x property=0x%x selection=0x%x",
                        ev->requestor, ev->target, ev->property, ev->selection);
            }
            receive_selection_data(win, ev->property, false);
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
            win->mapped = false;
            break;
        }
        case XCB_MAP_NOTIFY: {
            xcb_map_notify_event_t *ev = (xcb_map_notify_event_t *)event;
            if (!(win = window_for_xid(ev->window))) break;
            if (gconfig.trace_events) {
                info("Event: event=MapNotify window=0x%x", ev->window);
            }
            win->mapped = true;
            break;
        }
        case XCB_VISIBILITY_NOTIFY: {
            xcb_visibility_notify_event_t *ev = (xcb_visibility_notify_event_t *)event;
            if (!(win = window_for_xid(ev->window))) break;
            if (gconfig.trace_events) {
                info("Event: event=VisibilityNotify window=0x%x state=%d", ev->window, ev->state);
            }
            win->mapped &= ev->state != XCB_VISIBILITY_FULLY_OBSCURED;
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
            if (event->response_type != ctx.xkb_base_event) {
                warn("Unknown xcb event type: %02"PRIu8, event->response_type);
                break;
            }

            struct _xkb_any_event {
                uint8_t response_type;
                uint8_t xkb_type;
                uint16_t sequence;
                xcb_timestamp_t time;
                uint8_t device_id;
            } *xkb_ev = (struct _xkb_any_event *)event;

            if (gconfig.trace_events)
                info("Event: XKB Event %d", xkb_ev->xkb_type);

            if (xkb_ev->device_id != ctx.xkb_core_kbd) break;

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
    }
    return any;
}

static void x11_flush(void) {
    xcb_flush(con);
    if (do_handle_events(false))
        poller_skip_wait();
}

static void x11_handle_events(void *data_, uint32_t mask_) {
    (void)data_, (void)mask_;
    do_handle_events(true);
}

static void x11_free(void) {
    poller_remove(ctx.con_evt);

    ctx.renderer_free_context();

    if (ctx.cursors.data) {
        ht_iter_t it = ht_begin(&ctx.cursors);
        assert(!ht_current(&it));
        ht_free(&ctx.cursors);
    }

    if (ctx.hidden_cursor != XCB_NONE)
        xcb_free_cursor(con, ctx.hidden_cursor);
    if (ctx.xkb_state)
        xkb_state_unref(ctx.xkb_state);
    if (ctx.xkb_keymap)
        xkb_keymap_unref(ctx.xkb_keymap);
    if (ctx.xkb_ctx)
        xkb_context_unref(ctx.xkb_ctx);
    if (ctx.cursor_ctx)
        xcb_cursor_context_free(ctx.cursor_ctx);

    xcb_disconnect(con);
    con = NULL;
}

static struct platform_vtable x11_vtable = {
    .get_screen_size = x11_get_screen_size,
    .has_error = x11_has_error,
    .get_opaque_size = x11_get_opaque_size,
    .flush = x11_flush,
    .get_position = x11_get_position,
    .init_window = x11_init_window,
    .free_window = x11_free_window,
    .set_clip = x11_set_clip,
    .bell = x11_bell,
    .enable_mouse_events = x11_enable_mouse_events,
    .get_pointer = x11_get_pointer,
    .get_title = x11_get_title,
    .map_window = x11_map_window,
    .move_window = x11_move_window,
    .paste = x11_paste,
    .resize_window = x11_resize_window,
    .set_icon_label = x11_set_icon_label,
    .set_title = x11_set_title,
    .set_urgency = x11_set_urgency,
    .update_colors = x11_update_colors,
    .window_action = x11_window_action,
    .update_props = x11_update_window_props,
    .select_cursor = x11_select_cursor,
    .set_pointer_mode = x11_set_pointer_mode,
    .fixup_geometry = x11_fixup_geometry,
    .free = x11_free,
};

static inline const char *backend_to_str(enum renderer_backend backend) {
    switch (backend) {
    case renderer_x11_shm:
        return "X11 MIT-SHM";
    case renderer_x11_xrender:
        return "X11 XRender";
    case renderer_x11:
        return "X11";
    case renderer_wayland:
        return "Wayland";
    case renderer_auto:
        return "Auto";
    default:
        return "UNKNOWN";
    }
}

const struct platform_vtable *platform_init_x11(struct instance_config *cfg) {
    if (gconfig.backend != renderer_x11_xrender &&
        gconfig.backend != renderer_x11_shm &&
        gconfig.backend != renderer_x11 &&
        gconfig.backend != renderer_auto) return NULL;

    int screenp = 0;
    con = xcb_connect(NULL, &screenp);
    if (!con) {
        if (gconfig.backend == renderer_auto)
            return NULL;
        die("Can't connect to X server");
    }

    ctx.con_evt = poller_add_fd(x11_handle_events, NULL, xcb_get_file_descriptor(con), POLLIN);

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

    int result = xcb_cursor_context_new(con, ctx.screen, &ctx.cursor_ctx);
    if (result) {
        warn("Can't create cursor context: %d", result);
        ctx.cursor_ctx = NULL;
    } else {
        ht_init(&ctx.cursors, HT_INIT_CAPS, cursor_cmp);
    }
    do_create_hidden_cursor();

    /* Intern all used atoms */
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
    if (dpi > 0) set_default_dpi(dpi, cfg);

    switch (gconfig.backend) {
    case renderer_auto:
    case renderer_x11:
#if USE_XRENDER
    case renderer_x11_xrender:
        if (gconfig.trace_misc)
            info("Selected X11 XRender backend");
        x11_vtable.update = x11_xrender_update;
        x11_vtable.reload_font = x11_xrender_reload_font;
        x11_vtable.resize = x11_xrender_resize;
        x11_vtable.copy = x11_xrender_copy;
        x11_vtable.submit_screen = x11_xrender_submit_screen;
        ctx.renderer_recolor_border = x11_xrender_recolor_border;
        ctx.renderer_free = x11_xrender_free;
        ctx.renderer_free_context = x11_xrender_free_context;
        x11_xrender_init_context();
        break;
#endif
#if USE_X11SHM
    case renderer_x11_shm:
        if (gconfig.trace_misc)
            info("Selected X11 MIT-SHM backend");
        x11_vtable.update = x11_shm_update;
        x11_vtable.reload_font = shm_reload_font;
        x11_vtable.resize = shm_resize;
        x11_vtable.copy = shm_copy;
        x11_vtable.submit_screen = shm_submit_screen;
        x11_vtable.shm_create_image = x11_shm_create_image;
        ctx.renderer_recolor_border = shm_recolor_border;
        ctx.renderer_free = x11_shm_free;
        ctx.renderer_free_context = x11_shm_free_context;
        x11_shm_init_context();
        break;
#endif
    default:
        die("Unsupported backend '%s'", backend_to_str(gconfig.backend));
    }

    return &x11_vtable;
}
