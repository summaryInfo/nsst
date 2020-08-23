/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#include "feature.h"

#define _POSIX_C_SOURCE 200809L

#if USE_PPOLL
#   define _GNU_SOURCE
#endif

#include "config.h"
#include "font.h"
#include "input.h"
#include "mouse.h"
#include "term.h"
#include "util.h"
#include "window-x11.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xcb_xrm.h>
#include <xcb/xkb.h>
#include <xkbcommon/xkbcommon-x11.h>

#define INIT_PFD_NUM 16
#define NUM_BORDERS 4
#define NSST_CLASS "Nsst"
#define OPT_NAME_MAX 32
/* Need to be multiple of 4 */
#define PASTE_BLOCK_SIZE 1024

struct context {
    bool daemon_mode;
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

    struct pollfd *pfds;
    size_t pfdn;
    size_t pfdcap;

    size_t vbell_count;
};

struct context ctx;
xcb_connection_t *con = NULL;
struct window *win_list_head = NULL;
volatile sig_atomic_t reload_config;

static struct window *window_for_xid(xcb_window_t xid) {
    for (struct window *win = win_list_head; win; win = win->next)
        if (win->wid == xid) return win;
    info("Window for xid not found");
    return NULL;
}

static void load_config(void) {
    xcb_xrm_database_t *xrmdb = xcb_xrm_database_from_default(con);
    if (!xrmdb) return;

    char *res, name[OPT_NAME_MAX] = NSST_CLASS".color";
    const size_t n_pos = strlen(name);

    // .color0 -- .color255
    for (unsigned j = 0; j < PALETTE_SIZE - SPECIAL_PALETTE_SIZE; j++) {
        snprintf(name + n_pos, OPT_NAME_MAX, "%u", j);
        if (!xcb_xrm_resource_get_string(xrmdb, name, NULL, &res)) {
            sconf_set(CCONF_COLOR_0 + j, res);
            free(res);
        }
    }

    //optmap is defined in config.c
    for (size_t i = 0; i < OPT_MAP_SIZE; i++) {
        snprintf(name, OPT_NAME_MAX, NSST_CLASS".%s", optmap[i].name);
        char *res = NULL;
        if (!xcb_xrm_resource_get_string(xrmdb, name, NULL, &res))
            sconf_set(optmap[i].opt, res);
        if (res) free(res);
    }

    xcb_xrm_database_free(xrmdb);

    reload_config = 0;
}

static void handle_sigusr1(int sig) {
    reload_config = 1;
    (void)sig;
}

static xcb_atom_t intern_atom(const char *atom) {
    xcb_atom_t at;
    xcb_generic_error_t *err;
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

    ctx.xkb_keymap = xkb_x11_keymap_new_from_device(ctx.xkb_ctx, con, ctx.xkb_core_kbd, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!ctx.xkb_keymap) {
        warn("Can't create XKB keymap");
        goto cleanup_context;
    }
    ctx.xkb_state = xkb_x11_state_new_from_device(ctx.xkb_keymap, con, ctx.xkb_core_kbd);
    if (!ctx.xkb_state) {
        warn("Can't get condow xkb state");
        goto cleanup_keymap;
    }

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
cleanup_keymap:
    xkb_keymap_unref(ctx.xkb_keymap);
cleanup_context:
    xkb_context_unref(ctx.xkb_ctx);
    return 0;
}

/* Initialize global state object */
void init_context(void) {
    ctx.daemon_mode = 0;

    ctx.pfds = calloc(INIT_PFD_NUM, sizeof(struct pollfd));
    if (!ctx.pfds) die("Can't allocate pfds");
    ctx.pfdn = 1;
    ctx.pfdcap = INIT_PFD_NUM;
    for (size_t i = 1; i < INIT_PFD_NUM; i++)
        ctx.pfds[i].fd = -1;

    int screenp;
    con = xcb_connect(NULL, &screenp);
    ctx.pfds[0].events = POLLIN | POLLHUP;
    ctx.pfds[0].fd = xcb_get_file_descriptor(con);

    xcb_screen_iterator_t sit = xcb_setup_roots_iterator(xcb_get_setup(con));
    for (; sit.rem; xcb_screen_next(&sit))
        if (screenp-- == 0)break;
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

    init_render_context();

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
    if (dpi > 0) iconf_set(ICONF_DPI, dpi);

    if (!iconf(ICONF_SKIP_CONFIG_FILE)) load_config();
    else iconf_set(ICONF_SKIP_CONFIG_FILE, 0);

    sigaction(SIGUSR1, &(struct sigaction){ .sa_handler = handle_sigusr1, .sa_flags = SA_RESTART}, NULL);
}

/* Free all resources */
void free_context(void) {
    while (win_list_head)
        free_window(win_list_head);
    xkb_state_unref(ctx.xkb_state);
    xkb_keymap_unref(ctx.xkb_keymap);
    xkb_context_unref(ctx.xkb_ctx);

    free_render_context();
    free(ctx.pfds);

    xcb_disconnect(con);
    memset(&con, 0, sizeof(con));
}

void window_get_dim(struct window *win, int16_t *width, int16_t *height) {
    if (width) *width = win->width;
    if (height) *height = win->height;
}

void window_set_cursor(struct window *win, enum cursor_type type) {
    win->cursor_type = type;
}
enum cursor_type window_get_cursor(struct window *win) {
    return win->cursor_type;
}

void window_set_colors(struct window *win, color_t bg, color_t cursor_fg) {
    color_t obg = win->bg, ofg = win->cursor_fg;
    if (bg) win->bg = bg;
    if (cursor_fg) win->cursor_fg = cursor_fg;

    if (bg && bg != obg) {
        uint32_t values2[2];
        values2[0] = values2[1] = win->bg;
        xcb_change_window_attributes(con, win->wid, XCB_CW_BACK_PIXEL, values2);
        xcb_change_gc(con, win->gc, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, values2);
    }

    if ((bg && bg != obg) || (cursor_fg && cursor_fg != ofg)) {
        term_damage_lines(win->term, 0, win->ch);
        win->force_redraw = 1;
    }
}

void window_set_mouse(struct window *win, bool enabled) {
   if (enabled)
        win->ev_mask |= XCB_EVENT_MASK_POINTER_MOTION;
   else
       win->ev_mask &= ~XCB_EVENT_MASK_POINTER_MOTION;
   xcb_change_window_attributes(con, win->wid, XCB_CW_EVENT_MASK, &win->ev_mask);
}

void window_set_sync(struct window *win, bool state) {
    if (state) clock_gettime(CLOCK_TYPE, &win->last_sync);
    win->sync_active = state;
}

void window_delay(struct window *win) {
    clock_gettime(CLOCK_TYPE, &win->last_scroll);
}

void window_resize(struct window *win, int16_t width, int16_t height) {
    if (win->height != height || win->width != width) {
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
        break;
    }
}

void window_get_dim_ext(struct window *win, enum window_dimension which, int16_t *width, int16_t *height) {
    int16_t x = 0, y = 0;
    //TODO Handle reparenting
    switch (which) {
    case dim_window_position:
    case dim_grid_position:;
        xcb_get_geometry_cookie_t gc = xcb_get_geometry(con, win->wid);
        xcb_get_geometry_reply_t *rep = xcb_get_geometry_reply(con, gc, NULL);
        if (rep) {
            x = rep->x;
            y = rep->y;
            free(rep);
        }
        if (which == dim_grid_position) {
            x += win->left_border;
            y += win->top_border;
        }
        break;
    case dim_grid_size:
        x = win->char_width * win->cw;
        y = (win->char_height + win->char_depth) * win->ch;
        break;
    case dim_screen_size:
        x = ctx.screen->width_in_pixels;
        y = ctx.screen->height_in_pixels;
        break;
    case dim_cell_size:
        x = win->char_width;
        y = win->char_depth + win->char_height;
        break;
    case dim_border:
        x = win->left_border;
        y = win->top_border;
        break;
    }

    if (width) *width = x;
    if (height) *height = y;
}

void window_get_pointer(struct window *win, int16_t *px, int16_t *py, uint32_t *pmask) {
    int32_t x = 0, y = 0, mask = 0;
    xcb_query_pointer_cookie_t c = xcb_query_pointer(con, win->wid);
    xcb_query_pointer_reply_t *qre = xcb_query_pointer_reply(con, c, NULL);
    if (qre) {
        x = MIN(MAX(0, qre->win_x), win->width);
        y = MIN(MAX(0, qre->win_y), win->height);
        mask = qre->mask;
        free(qre);
    }
    if (px) *px = x;
    if (py) *py = y;
    if (pmask) *pmask = mask;
}

#define WM_HINTS_LEN 8
static void set_urgency(xcb_window_t wid, bool set) {
    xcb_get_property_cookie_t c = xcb_get_property(con, 0, wid, XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS, 0, WM_HINTS_LEN);
    xcb_get_property_reply_t *rep = xcb_get_property_reply(con, c, NULL);
    if (rep) {
        uint32_t *hints = xcb_get_property_value(rep);
        if (set) *hints |= 256; // UrgentcyHint
        else *hints &= ~256; // UrgentcyHint
        xcb_change_property(con, XCB_PROP_MODE_REPLACE, wid, XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS, 32, WM_HINTS_LEN, hints);
        free(rep);
    }

}

void window_bell(struct window *win, uint8_t vol) {
    if (!win->focused) {
        if (term_is_bell_raise_enabled(win->term)) window_action(win, action_restore_minimized);
        if (term_is_bell_urgent_enabled(win->term)) set_urgency(win->wid, 1);
    }
    if (iconf(ICONF_VISUAL_BELL)) {
        if (!win->in_blink) {
            win->init_invert = term_is_reverse(win->term);
            win->in_blink = 1;
            ctx.vbell_count++;
            clock_gettime(CLOCK_TYPE, &win->vbell_start);
            term_set_reverse(win->term, !win->init_invert);
        }
    } else if (vol) {
        xcb_xkb_bell(con, XCB_XKB_ID_USE_CORE_KBD, XCB_XKB_ID_DFLT_XI_CLASS,
                XCB_XKB_ID_DFLT_XI_ID, vol, 1, 0, 0, 0, XCB_ATOM_ANY, win->wid);
    }
}

bool window_is_mapped(struct window *win) {
    return win->active;
}

static int32_t window_get_font_size(struct window *win) {
    return win->font_size;
}

static void reload_all_fonts(void) {
    for (struct window *win = win_list_head; win; win = win->next) {
        renderer_reload_font(win, 1);
        term_damage_lines(win->term, 0, win->ch);
        win->force_redraw = 1;
    }
}

static void window_set_font(struct window *win, const char * name, int32_t size) {
    bool reload = name || size != win->font_size;
    if (name) {
        free(win->font_name);
        win->font_name = strdup(name);
    }

    if (size >= 0) win->font_size = size;

    if (reload) {
        renderer_reload_font(win, 1);
        term_damage_lines(win->term, 0, win->ch);
        win->force_redraw = 1;
    }
}

static void set_title(xcb_window_t wid, const char *title, bool utf8) {
    xcb_change_property(con, XCB_PROP_MODE_REPLACE, wid,
        utf8 ? ctx.atom._NET_WM_NAME : XCB_ATOM_WM_NAME,
        utf8 ? ctx.atom.UTF8_STRING : XCB_ATOM_STRING, 8, strlen(title), title);
}

static void set_icon_label(xcb_window_t wid, const char *title, bool utf8) {
    xcb_change_property(con, XCB_PROP_MODE_REPLACE, wid,
        utf8 ? ctx.atom._NET_WM_ICON_NAME : XCB_ATOM_WM_ICON_NAME,
        utf8 ? ctx.atom.UTF8_STRING : XCB_ATOM_STRING, 8, strlen(title), title);
}

void window_set_title(struct window *win, enum title_target which, const char *title, bool utf8) {
    if (!title) title = sconf(SCONF_TITLE);

    if (which & target_title) set_title(win->wid, title, utf8);

    if (which & target_icon_label) set_icon_label(win->wid, title, utf8);
}

char *get_full_property(xcb_window_t wid, xcb_atom_t prop, xcb_atom_t *type, size_t *psize) {
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

void window_push_title(struct window *win, enum title_target which) {
    char *title = NULL, *icon = NULL;
    bool tutf8 = 0, iutf8 = 0;
    if (which & target_title) window_get_title(win, target_title, &title, &tutf8);
    if (which & target_icon_label) window_get_title(win, target_icon_label, &icon, &iutf8);

    size_t len = sizeof(struct title_stack_item), tlen = 0, ilen = 0;
    if (title) len += tlen = strlen(title) + 1;
    if (icon) len += ilen = strlen(icon) + 1;

    struct title_stack_item *new = malloc(len);
    if (!new) {
        free(title);
        free(icon);
        return;
    }

    if (title) memcpy(new->title_data = new->data, title, tlen);
    else new->title_data = NULL;
    new->title_utf8 = tutf8;

    if (icon) memcpy(new->icon_data = new->data + tlen, icon, ilen);
    else new->icon_data = NULL;
    new->icon_utf8 = iutf8;

    new->next = win->title_stack;
    win->title_stack = new;

    free(title);
    free(icon);
}

void window_pop_title(struct window *win, enum title_target which) {
    struct title_stack_item *top = win->title_stack, *it;
    if (top) {
        if (which & target_title) {
            for (it = top; it && !it->title_data; it = it->next);
            if (it) set_title(win->wid, it->title_data, it->title_utf8);
        }
        if (which & target_icon_label) {
            for (it = top; it && !it->icon_data; it = it->next);
            if (it) set_icon_label(win->wid, it->icon_data, it->icon_utf8);
        }
        win->title_stack = top->next;
        free(top);
    }
}


uint32_t get_win_gravity_from_config() {
    bool nx = iconf(ICONF_WINDOW_NEGATIVE_X);
    bool ny = iconf(ICONF_WINDOW_NEGATIVE_Y);
    switch (nx + 2 * ny) {
    case 0: return XCB_GRAVITY_NORTH_WEST;
    case 1: return XCB_GRAVITY_NORTH_EAST;
    case 2: return XCB_GRAVITY_SOUTH_WEST;
    default: return XCB_GRAVITY_NORTH_EAST;
    }
};

struct cellspec describe_cell(struct cell cell, color_t *palette, color_t *extra, bool blink, bool selected) {
    struct cellspec res;

    // Check special colors
    if (__builtin_expect(iconf(ICONF_SPEICAL_BOLD), 0) &&
            palette[SPECIAL_BOLD] && cell.attr & attr_bold) {
        cell.fg = SPECIAL_BOLD;
        cell.attr &= ~attr_bold;
    }
    if (__builtin_expect(iconf(ICONF_SPEICAL_UNDERLINE), 0) &&
            palette[SPECIAL_UNDERLINE] && cell.attr & attr_underlined) {
        cell.fg = SPECIAL_UNDERLINE;
        cell.attr &= ~attr_underlined;
    }
    if (__builtin_expect(iconf(ICONF_SPEICAL_BLINK), 0) &&
            palette[SPECIAL_BLINK] && cell.attr & attr_blink) {
        cell.fg = SPECIAL_BLINK;
        cell.attr &= ~attr_blink;
    }
    if (__builtin_expect(iconf(ICONF_SPEICAL_REVERSE), 0) &&
            palette[SPECIAL_REVERSE] && cell.attr & attr_inverse) {
        cell.fg = SPECIAL_REVERSE;
        cell.attr &= ~attr_inverse;
    }
    if (__builtin_expect(iconf(ICONF_SPEICAL_ITALIC), 0) &&
            palette[SPECIAL_ITALIC] && cell.attr & attr_italic) {
        cell.fg = SPECIAL_ITALIC;
        cell.attr &= ~attr_italic;
    }

    // Calculate colors

    if ((cell.attr & (attr_bold | attr_faint)) == attr_bold && cell.fg < 8) cell.fg += 8;
    res.bg = cell.bg < PALETTE_SIZE ? palette[cell.bg] : extra[cell.bg - PALETTE_SIZE];
    res.fg = cell.fg < PALETTE_SIZE ? palette[cell.fg] : extra[cell.fg - PALETTE_SIZE];
    if ((cell.attr & (attr_bold | attr_faint)) == attr_faint)
        res.fg = (res.fg & 0xFF000000) | ((res.fg & 0xFEFEFE) >> 1);
    if ((cell.attr & attr_inverse) ^ selected) SWAP(color_t, res.fg, res.bg);
    if ((!selected && cell.attr & attr_invisible) || (cell.attr & attr_blink && blink)) res.fg = res.bg;

    // If selected colors are set use them

    if (palette[SPECIAL_SELECTED_BG] && selected) res.bg = palette[SPECIAL_SELECTED_BG];
    if (palette[SPECIAL_SELECTED_FG] && selected) res.fg = palette[SPECIAL_SELECTED_FG];

    // Optimize rendering of U+2588 FULL BLOCK

    if (cell.ch == 0x2588) res.bg = res.fg;
    if (cell.ch == ' ' || res.fg == res.bg) cell.ch = 0;

    // Calculate attributes

    res.ch = cell.ch;
    res.face = cell.ch ? (cell.attr & face_mask) : 0;
    res.wide = !!(cell.attr & attr_wide);
    res.underlined = !!(cell.attr & attr_underlined) && (res.fg != res.bg);
    res.stroke = !!(cell.attr & attr_strikethrough) && (res.fg != res.bg);

    return res;
}

struct window *find_shared_font(struct window *win, bool need_free) {
    bool found_font = 0, found_cache = 0;
    struct window *found = 0;

    win->font_pixmode = iconf(ICONF_PIXEL_MODE);

    for (struct window *src = win_list_head; src; src = src->next) {
        if ((src->font_size == win->font_size || (!win->font_size &&
                src->font_size == iconf(ICONF_FONT_SIZE))) &&
                !strcmp(win->font_name, src->font_name) && src != win) {
            found_font = 1;
            found = src;
            if (win->font_pixmode == src->font_pixmode) {
                found_cache = 1;
                break;
            }
        }
    }

    struct font *newf = found_font ? font_ref(found->font) :
            create_font(win->font_name, win->font_size);
    if (!newf) {
        warn("Can't create new font: %s", win->font_name);
        return NULL;
    }

    struct glyph_cache *newc = found_cache ? glyph_cache_ref(found->font_cache) :
            create_glyph_cache(newf);

    if (need_free) {
        free_glyph_cache(win->font_cache);
        free_font(win->font);
    }

    win->font = newf;
    win->font_cache = newc;
    win->font_size = font_get_size(newf);

    //Initialize default font size
    if (!iconf(ICONF_FONT_SIZE))
        iconf_set(ICONF_FONT_SIZE, win->font_size);

    glyph_cache_get_dim(win->font_cache, &win->char_width, &win->char_height, &win->char_depth);

    return found;
}

void window_set_default_props(struct window *win) {
    uint32_t pid = getpid();
    xcb_change_property(con, XCB_PROP_MODE_REPLACE, win->wid, ctx.atom._NET_WM_PID, XCB_ATOM_CARDINAL, 32, 1, &pid);
    xcb_change_property(con, XCB_PROP_MODE_REPLACE, win->wid, ctx.atom.WM_PROTOCOLS, XCB_ATOM_ATOM, 32, 1, &ctx.atom.WM_DELETE_WINDOW);
    const char *extra;
    xcb_change_property(con, XCB_PROP_MODE_REPLACE, win->wid, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, sizeof(NSST_CLASS), NSST_CLASS);
    if ((extra = sconf(SCONF_TERM_CLASS)))
        xcb_change_property(con, XCB_PROP_MODE_APPEND, win->wid, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, strlen(extra), extra);
    uint32_t nhints[] = {
        64 | 256, //PResizeInc, PBaseSize
        iconf(ICONF_WINDOW_X), iconf(ICONF_WINDOW_Y), // Position
        win->width, win->height, // Size
        win->left_border * 2 + win->char_width, win->left_border * 2 + win->char_depth + win->char_height, // Min size
        0, 0, //Max size
        win->char_width, win->char_depth + win->char_height, // Size inc
        0, 0, 0, 0, // Min/max aspect
        win->left_border * 2 + win->char_width, win->left_border * 2 + win->char_depth + win->char_height, // Base size
        get_win_gravity_from_config(), // Gravity
    };
    if (iconf(ICONF_HAS_GEOMETRY))
        nhints[0] |= 1 | 2 | 512; // USPosition, USSize, PWinGravity
    else
        nhints[0] |= 4 | 8; // PPosition, PSize
    if (iconf(ICONF_FIXED_SIZE)) {
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

/* Create new window */
struct window *create_window(void) {
    struct window *win = calloc(1, sizeof(struct window));
    win->cursor_width = iconf(ICONF_CURSOR_WIDTH);
    win->underline_width = iconf(ICONF_UNDERLINE_WIDTH);
    win->left_border = iconf(ICONF_LEFT_BORDER);
    win->top_border = iconf(ICONF_TOP_BORDER);
    win->bg = cconf(CCONF_BG);
    win->cursor_fg = cconf(CCONF_CURSOR_FG);
    if (iconf(ICONF_REVERSE_VIDEO))
        SWAP(color_t, win->bg, win->cursor_fg);
    win->cursor_type = iconf(ICONF_CURSOR_SHAPE);
    win->font_size = iconf(ICONF_FONT_SIZE);
    win->width = iconf(ICONF_WINDOW_WIDTH);
    win->height = iconf(ICONF_WINDOW_HEIGHT);

    win->active = 1;
    win->focused = 1;

    win->font_name = strdup(sconf(SCONF_FONT_NAME));
    if (!win->font_name) {
        free_window(win);
        return NULL;
    }

    xcb_void_cookie_t c;

    win->ev_mask = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_VISIBILITY_CHANGE |
        XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
        XCB_EVENT_MASK_BUTTON_MOTION | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_PROPERTY_CHANGE;
    uint32_t mask1 =  XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL |
        XCB_CW_BIT_GRAVITY | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
    uint32_t values1[5] = { win->bg, win->bg, XCB_GRAVITY_NORTH_WEST, win->ev_mask, ctx.mid };
    int16_t x = iconf(ICONF_WINDOW_X);
    int16_t y = iconf(ICONF_WINDOW_Y);

    // Adjust geometry
    if (iconf(ICONF_WINDOW_NEGATIVE_X))
        x += ctx.screen->width_in_pixels - win->width - 2;
    if (iconf(ICONF_WINDOW_NEGATIVE_Y))
        y += ctx.screen->height_in_pixels - win->height - 2;

    win->wid = xcb_generate_id(con);
    c = xcb_create_window_checked(con, TRUE_COLOR_ALPHA_DEPTH, win->wid, ctx.screen->root,
                                  x, y, win->width, win->height, 0,
                                  XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                  ctx.vis->visual_id, mask1, values1);
    if (check_void_cookie(c)) goto error;

    win->gc = xcb_generate_id(con);
    uint32_t mask2 = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_GRAPHICS_EXPOSURES;
    uint32_t values2[3] = { win->bg, win->bg, 0 };

    c = xcb_create_gc_checked(con, win->gc, win->wid, mask2, values2);
    if (check_void_cookie(c)) goto error;

    if (!renderer_reload_font(win, 0)) goto error;

    win->term = create_term(win, win->cw, win->ch);
    if (!win->term) goto error;

    window_set_default_props(win);
    window_set_title(win, target_title | target_icon_label, NULL, iconf(ICONF_UTF8));

    win->next = win_list_head;
    win->prev = NULL;
    if (win_list_head) win_list_head->prev = win;
    win_list_head = win;

    if (ctx.pfdn + 1 > ctx.pfdcap) {
        struct pollfd *new = realloc(ctx.pfds, (ctx.pfdcap + INIT_PFD_NUM)*sizeof(*ctx.pfds));
        if (new) {
            for (size_t i = 0; i < INIT_PFD_NUM; i++) {
                new[i + ctx.pfdcap].fd = -1;
                new[i + ctx.pfdcap].events = 0;
            }
            ctx.pfdcap += INIT_PFD_NUM;
            ctx.pfds = new;
        } else goto error;
    }

    ctx.pfdn++;
    size_t i = 1;
    while (ctx.pfds[i].fd >= 0) i++;
    ctx.pfds[i].events = POLLIN | POLLHUP;
    ctx.pfds[i].fd = term_fd(win->term);
    win->poll_index = i;

    xcb_map_window(con, win->wid);
    xcb_flush(con);
    return win;

error:
    warn("Can't create window");
    free_window(win);
    return NULL;
}

/* Free previously created windows */
void free_window(struct window *win) {
    if (win->wid) {
        xcb_unmap_window(con, win->wid);
        renderer_free(win);
        xcb_free_gc(con, win->gc);
        xcb_destroy_window(con, win->wid);
        xcb_flush(con);
    }

    // Decrement count of currently blinking
    // windows if window gets freed during blink
    if (win->in_blink) ctx.vbell_count--;

    if (win->next) win->next->prev = win->prev;
    if (win->prev) win->prev->next = win->next;
    else win_list_head =  win->next;

    if (win->poll_index > 0) {
        ctx.pfds[win->poll_index].fd = -1;
        ctx.pfdn--;
    }

    if (win->term)
        free_term(win->term);
    if (win->font_cache)
        free_glyph_cache(win->font_cache);
    if (win->font)
        free_font(win->font);

    for (size_t i = 0; i < clip_MAX; i++)
        free(win->clipped[i]);

    struct title_stack_item *tmp;
    while (win->title_stack) {
        tmp = win->title_stack->next;
        free(win->title_stack);
        win->title_stack = tmp;
    }

    free(win->font_name);
    free(win);
};

bool window_shift(struct window *win, nss_coord_t xs, nss_coord_t ys, nss_coord_t xd, nss_coord_t yd, nss_coord_t width, nss_coord_t height, bool delay) {
    struct timespec cur;
    clock_gettime(CLOCK_TYPE, &cur);

    bool scrolled_recently = TIMEDIFF(win->last_scroll, cur) <  SEC/2/iconf(ICONF_FPS);

    win->last_scroll = cur;

    if (delay && scrolled_recently) return 0;

    ys = MAX(0, MIN(ys, win->ch));
    yd = MAX(0, MIN(yd, win->ch));
    xs = MAX(0, MIN(xs, win->cw));
    xd = MAX(0, MIN(xd, win->cw));
    height = MIN(height, MIN(win->ch - ys, win->ch - yd));
    width = MIN(width, MIN(win->cw - xs, win->cw - xd));

    if (!height || !width) return 1;

    ys *= win->char_height + win->char_depth;
    yd *= win->char_height + win->char_depth;
    xs *= win->char_width;
    xd *= win->char_width;
    height *= win->char_depth + win->char_height;
    width *= win->char_width;

    renderer_copy(win, (struct rect){xd, yd, width, height}, xs, ys);

    return 1;
}

inline static xcb_atom_t target_to_atom(enum clip_target target) {
    switch (target) {
    case clip_secondary: return XCB_ATOM_SECONDARY;
    case clip_primary: return XCB_ATOM_PRIMARY;
    default: return ctx.atom.CLIPBOARD;
    }
}

static void clip_copy(struct window *win) {
    if (win->clipped[clip_primary]) {
        uint8_t *dup = (uint8_t*)strdup((char *)win->clipped[clip_primary]);
        if (dup) {
            if (term_is_keep_clipboard_enabled(win->term)) {
                uint8_t *dup2 = (uint8_t *)strdup((char *)dup);
                free(win->clipboard);
                win->clipboard = dup2;
            }
            window_set_clip(win, dup, CLIP_TIME_NOW, clip_clipboard);
        }
    }
}

void window_set_clip(struct window *win, uint8_t *data, uint32_t time, enum clip_target target) {
    if (data) {
        xcb_set_selection_owner(con, win->wid, target_to_atom(target), time);
        xcb_get_selection_owner_cookie_t so = xcb_get_selection_owner_unchecked(con, target_to_atom(target));
        xcb_get_selection_owner_reply_t *rep = xcb_get_selection_owner_reply(con, so, NULL);
        if (rep) {
            if (rep->owner != win->wid) {
                free(data);
                data = NULL;
            }
            free(rep);
        }
    }
    free(win->clipped[target]);
    win->clipped[target] = data;
}

void window_paste_clip(struct window *win, enum clip_target target) {
    xcb_convert_selection(con, win->wid, target_to_atom(target),
          term_is_utf8_enabled(win->term) ? ctx.atom.UTF8_STRING : XCB_ATOM_STRING, target_to_atom(target), XCB_CURRENT_TIME);
}

static void redraw_borders(struct window *win, bool top_left, bool bottom_right) {
        int16_t width = win->cw * win->char_width + win->left_border;
        int16_t height = win->ch * (win->char_height + win->char_depth) + win->top_border;
        xcb_rectangle_t borders[NUM_BORDERS] = {
            {0, 0, win->left_border, height},
            {win->left_border, 0, width, win->top_border},
            {width, 0, win->width - width, win->height},
            {0, height, width, win->height - height},
        };
        size_t count = 4, offset = 0;
        if (!top_left) count -= 2, offset += 2;
        if (!bottom_right) count -= 2;
        //TODO Handle zero height
        if (count) xcb_poly_fill_rectangle(con, win->wid, win->gc, count, borders + offset);
}

void handle_resize(struct window *win, int16_t width, int16_t height) {
    //Handle resize

    win->width = width;
    win->height = height;

    nss_coord_t new_cw = MAX(2, (win->width - 2*win->left_border)/win->char_width);
    nss_coord_t new_ch = MAX(1, (win->height - 2*win->top_border)/(win->char_height+win->char_depth));
    nss_coord_t delta_x = new_cw - win->cw;
    nss_coord_t delta_y = new_ch - win->ch;

    if (delta_x || delta_y) {
        term_resize(win->term, new_cw, new_ch);
        renderer_resize(win, new_cw, new_ch);
        clock_gettime(CLOCK_TYPE, &win->last_resize);
    }

    if (delta_x < 0 || delta_y < 0)
        redraw_borders(win, 0, 1);
}

static void handle_expose(struct window *win, struct rect damage) {
    int16_t width = win->cw * win->char_width + win->left_border;
    int16_t height = win->ch * (win->char_height + win->char_depth) + win->top_border;

    size_t num_damaged = 0;
    struct rect damaged[NUM_BORDERS], borders[NUM_BORDERS] = {
        {0, 0, win->left_border, height},
        {win->left_border, 0, width, win->top_border},
        {width, 0, win->width - width, win->height},
        {0, height, width, win->height - height},
    };
    for (size_t i = 0; i < NUM_BORDERS; i++)
        if (intersect_with(&borders[i], &damage))
                damaged[num_damaged++] = borders[i];
    if (num_damaged) xcb_poly_fill_rectangle(con, win->wid, win->gc, num_damaged, (xcb_rectangle_t *)damaged);

    struct rect inters = { 0, 0, width - win->left_border, height - win->top_border};
    damage = rect_shift(damage, -win->left_border, -win->top_border);
    if (intersect_with(&inters, &damage)) renderer_update(win, inters);
}

static void handle_focus(struct window *win, bool focused) {
    win->focused = focused;
    term_handle_focus(win->term, focused);
}

static void handle_keydown(struct window *win, xkb_keycode_t keycode) {
    struct key key = keyboard_describe_key(ctx.xkb_state, keycode);

    if (key.sym == XKB_KEY_NoSymbol) return;

    enum shortcut_action action = keyboard_find_shortcut(key);

    switch (action) {
    case shortcut_break:
        term_break(win->term);
        return;
    case shortcut_numlock:
        term_toggle_numlock(win->term);
        return;
    case shortcut_scroll_up:
        term_scroll_view(win->term, -iconf(ICONF_SCROLL_AMOUNT));
        return;
    case shortcut_scroll_down:
        term_scroll_view(win->term, iconf(ICONF_SCROLL_AMOUNT));
        return;
    case shortcut_font_up:
    case shortcut_font_down:
    case shortcut_font_default:;
        int32_t size = window_get_font_size(win);
        if (action == shortcut_font_up)
            size += iconf(ICONF_FONT_SIZE_STEP);
        else if (action == shortcut_font_down)
            size -= iconf(ICONF_FONT_SIZE_STEP);
        else if (action == shortcut_font_default)
            size = iconf(ICONF_FONT_SIZE);
        window_set_font(win, NULL, size);
        return;
    case shortcut_new_window:
        create_window();
        return;
    case shortcut_copy:
        clip_copy(win);
        return;
    case shortcut_paste:
        window_paste_clip(win, clip_clipboard);
        return;
    case shortcut_reload_config:
        reload_config = 1;
        return;
    case shortcut_reset:
        term_reset(win->term);
        return;
    case shortcut_MAX:
    case shortcut_none:;
    }

    keyboard_handle_input(key, win->term);
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
    uint8_t leftover[3], leftover_len = 0;

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
        uint8_t *data = xcb_get_property_value(rep);
        uint8_t *pos = data, *end = data + size;

        if (!term_is_paste_nl_enabled(win->term))
            while ((pos = memchr(pos, '\n', end - pos))) *pos++ = '\r';

        if (size) {
            if (!offset) term_paste_begin(win->term);

            static uint8_t buf1[2*PASTE_BLOCK_SIZE];
            static uint8_t buf2[4*PASTE_BLOCK_SIZE];

            if ((rep->type == ctx.atom.UTF8_STRING) ^ term_is_utf8_enabled(win->term)) {
                pos = data;
                size = 0;
                if (rep->type == ctx.atom.UTF8_STRING) {
                    term_char_t ch;
                    while (pos < end)
                        if (utf8_decode(&ch, (const uint8_t **)&pos, end))
                            buf1[size++] = ch;
                } else {
                    while (pos < end)
                        size += utf8_encode(*pos++, buf1 + size, buf1 + BUFSIZ);
                }

                data = buf1;
            }

            if (term_is_paste_requested(win->term)) {
                while (leftover_len < 3 && size) leftover[leftover_len++] = *data++, size--;
                size_t pre = base64_encode(buf2, leftover, leftover + leftover_len) - buf2;

                if (size) {
                    if (left) {
                        leftover_len = size % 3;
                        if (leftover_len > 0) leftover[0] = data[size - leftover_len], size--;
                        if (leftover_len > 1) leftover[1] = data[size - 1], size--;
                    }

                    size = base64_encode(buf2 + pre, data, data + size) - buf2;
                }
                data = buf2;
            } else if (term_is_paste_quote_enabled(win->term)) {
                bool quote_c1 = !term_is_utf8_enabled(win->term);
                ssize_t i = 0, j = 0;
                while (i < size) {
                    // Prefix control symbols with Ctrl-V
                    if (buf1[i] < 0x20 || buf1[i] == 0x7F ||
                            (quote_c1 && buf1[i] > 0x7F && buf1[i] < 0xA0))
                        buf2[j++] = 0x16;
                    buf2[j++] = buf1[i++];
                }
                size = j;
                data = buf2;
            }

            term_sendkey(win->term, data, size);

            if (!left) term_paste_end(win->term);
        }

        free(rep);

        offset += size / 4;
    } while (left > 0);

    xcb_delete_property(con, win->wid, prop);
}

/* Start window logic, handling all windows in context */
void run(void) {
    for (int64_t next_timeout = SEC/iconf(ICONF_FPS);;) {
#if USE_PPOLL
        if (ppoll(ctx.pfds, ctx.pfdcap, &(struct timespec){next_timeout / SEC, next_timeout % SEC}, NULL) < 0 && errno != EINTR)
#else
        if (poll(ctx.pfds, ctx.pfdcap, next_timeout/(SEC/1000)) < 0 && errno != EINTR)
#endif
            warn("Poll error: %s", strerror(errno));
        if (ctx.pfds[0].revents & POLLIN) {
            for (xcb_generic_event_t *event; (event = xcb_poll_for_event(con)); free(event)) {
                switch (event->response_type &= 0x7f) {
                    struct window *win;
                case XCB_EXPOSE:{
                    xcb_expose_event_t *ev = (xcb_expose_event_t*)event;
                    if (!(win = window_for_xid(ev->window))) break;
                    if (iconf(ICONF_TRACE_EVENTS)) {
                        info("Event: event=Expose win=0x%x x=%x y=%d width=%d height=%d",
                                ev->window, ev->x, ev->y, ev->width, ev->height);
                    }
                    handle_expose(win, (struct rect){ev->x, ev->y, ev->width, ev->height});
                    break;
                }
                case XCB_CONFIGURE_NOTIFY:{
                    xcb_configure_notify_event_t *ev = (xcb_configure_notify_event_t*)event;
                    if (!(win = window_for_xid(ev->window))) break;
                    if (iconf(ICONF_TRACE_EVENTS)) {
                        info("Event: event=ConfigureWindow win=0x%x x=%x y=%d width=%d"
                                " height=%d border=%d redir=%d above_win=0x%x event_win=0x%x",
                                ev->window, ev->x, ev->y, ev->width, ev->height, ev->border_width,
                                ev->override_redirect, ev->above_sibling, ev->event);
                    }
                    if (ev->width != win->width || ev->height != win->height)
                        handle_resize(win, ev->width, ev->height);
                    break;
                }
                case XCB_KEY_PRESS:{
                    xcb_key_release_event_t *ev = (xcb_key_release_event_t*)event;
                    if (!(win = window_for_xid(ev->event))) break;
                    if (iconf(ICONF_TRACE_EVENTS)) {
                        info("Event: event=KeyPress win=0x%x keycode=0x%x", ev->event, ev->detail);
                    }
                    handle_keydown(win, ev->detail);
                    break;
                }
                case XCB_FOCUS_IN:
                case XCB_FOCUS_OUT:{
                    xcb_focus_in_event_t *ev = (xcb_focus_in_event_t*)event;
                    if (!(win = window_for_xid(ev->event))) break;
                    if (iconf(ICONF_TRACE_EVENTS)) {
                        info("Event: event=%s win=0x%x", ev->response_type == XCB_FOCUS_IN ?
                                "FocusIn" : "FocusOut", ev->event);
                    }
                    handle_focus(win, event->response_type == XCB_FOCUS_IN);
                    break;
                }
                case XCB_BUTTON_RELEASE: /* All these events have same structure */
                case XCB_BUTTON_PRESS:
                case XCB_MOTION_NOTIFY: {
                    xcb_motion_notify_event_t *ev = (xcb_motion_notify_event_t*)event;
                    if (!(win = window_for_xid(ev->event))) break;
                    if (iconf(ICONF_TRACE_EVENTS)) {
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
                    xcb_selection_clear_event_t *ev = (xcb_selection_clear_event_t*)event;
                    if (!(win = window_for_xid(ev->owner))) break;
                    if (iconf(ICONF_TRACE_EVENTS)) {
                        info("Event: event=SelectionClear owner=0x%x selection=0x%x", ev->owner, ev->selection);
                    }
                    // Clear even if set keep?
                    mouse_clear_selection(win->term);
                    break;
                }
                case XCB_PROPERTY_NOTIFY: {
                    xcb_property_notify_event_t *ev = (xcb_property_notify_event_t*)event;
                    if (!(win = window_for_xid(ev->window))) break;
                    if (iconf(ICONF_TRACE_EVENTS)) {
                        info("Event: event=PropertyNotify window=0x%x property=0x%x state=%d",
                                ev->window, ev->atom, ev->state);
                    }
                    if ((ev->atom == XCB_ATOM_PRIMARY || ev->atom == XCB_ATOM_SECONDARY ||
                            ev->atom == ctx.atom.CLIPBOARD) && ev->state == XCB_PROPERTY_NEW_VALUE)
                        receive_selection_data(win, ev->atom, 1);
                    break;
                }
                case XCB_SELECTION_NOTIFY: {
                    xcb_selection_notify_event_t *ev = (xcb_selection_notify_event_t*)event;
                    if (!(win = window_for_xid(ev->requestor))) break;
                    if (iconf(ICONF_TRACE_EVENTS)) {
                        info("Event: event=SelectionNotify owner=0x%x target=0x%x property=0x%x selection=0x%x",
                                ev->requestor, ev->target, ev->property, ev->selection);
                    }
                    receive_selection_data(win, ev->property, 0);
                    break;
                }
                case XCB_SELECTION_REQUEST: {
                    xcb_selection_request_event_t *ev = (xcb_selection_request_event_t*)event;
                    if (!(win = window_for_xid(ev->owner))) break;
                    if (iconf(ICONF_TRACE_EVENTS)) {
                        info("Event: event=SelectionRequest owner=0x%x requestor=0x%x target=0x%x property=0x%x selection=0x%x",
                                ev->owner, ev->requestor, ev->target, ev->property, ev->selection);
                    }
                    send_selection_data(win, ev->requestor, ev->selection, ev->target, ev->property, ev->time);
                    break;
                }
                case XCB_CLIENT_MESSAGE: {
                    xcb_client_message_event_t *ev = (xcb_client_message_event_t*)event;
                    if (!(win = window_for_xid(ev->window))) break;
                    if (iconf(ICONF_TRACE_EVENTS)) {
                        info("Event: event=ClientMessage window=0x%x type=0x%x data=[0x%08x,0x%08x,0x%08x,0x%08x,0x%08x]",
                            ev->window, ev->type, ev->data.data32[0], ev->data.data32[1],
                            ev->data.data32[2], ev->data.data32[3], ev->data.data32[4]);
                    }
                    if (ev->format == 32 && ev->data.data32[0] == ctx.atom.WM_DELETE_WINDOW) {
                        free_window(win);
                        if (!win_list_head && !ctx.daemon_mode)
                            return free(event);
                    }
                    break;
                }
                case XCB_VISIBILITY_NOTIFY: {
                    xcb_visibility_notify_event_t *ev = (xcb_visibility_notify_event_t*)event;
                    if (!(win = window_for_xid(ev->window))) break;
                    if (iconf(ICONF_TRACE_EVENTS)) {
                        info("Event: event=ClientMessage window=0x%x state=%d", ev->window, ev->state);
                    }
                    win->active = ev->state != XCB_VISIBILITY_FULLY_OBSCURED;
                    break;
                }
                case XCB_KEY_RELEASE:
                case XCB_MAP_NOTIFY:
                case XCB_UNMAP_NOTIFY:
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
                        } *xkb_ev = (struct _xkb_any_event*)event;

                        if (iconf(ICONF_TRACE_EVENTS)) {
                            info("Event: XKB Event %d", xkb_ev->xkb_type);
                        }

                        if (xkb_ev->device_id == ctx.xkb_core_kbd) {
                            switch (xkb_ev->xkb_type) {
                            case XCB_XKB_NEW_KEYBOARD_NOTIFY: {
                                xcb_xkb_new_keyboard_notify_event_t *ev = (xcb_xkb_new_keyboard_notify_event_t*)event;
                                if (ev->changed & XCB_XKB_NKN_DETAIL_KEYCODES)
                            case XCB_XKB_MAP_NOTIFY:
                                    update_keymap();
                                break;
                            }
                            case XCB_XKB_STATE_NOTIFY: {
                                xcb_xkb_state_notify_event_t *ev = (xcb_xkb_state_notify_event_t*)event;
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

        for (struct window *win = win_list_head, *next; win; win = next) {
            next = win->next;
            if (ctx.pfds[win->poll_index].revents & POLLIN)
                term_read(win->term);
            else if (ctx.pfds[win->poll_index].revents & (POLLERR | POLLNVAL | POLLHUP))
                free_window(win);
        }

        next_timeout = iconf(ctx.vbell_count ? ICONF_VISUAL_BELL_TIME : ICONF_BLINK_TIME)*1000LL;
        struct timespec cur;
        clock_gettime(CLOCK_TYPE, &cur);

        for (struct window *win = win_list_head; win; win = win->next) {
            if (TIMEDIFF(win->last_sync, cur) > iconf(ICONF_SYNC_TIME)*1000LL && win->sync_active)
                win->sync_active = 0;
            if (win->in_blink && TIMEDIFF(win->vbell_start, cur) > iconf(ICONF_VISUAL_BELL_TIME)*1000LL) {
                term_set_reverse(win->term, win->init_invert);
                win->in_blink = 0;
                ctx.vbell_count--;
            }
            if (TIMEDIFF(win->last_blink, cur) > iconf(ICONF_BLINK_TIME)*1000LL &&
                    win->active && iconf(ICONF_ALLOW_BLINKING)) {
                win->blink_state = !win->blink_state;
                win->blink_commited = 0;
                win->last_blink = cur;
            }

            int64_t scroll_delay = 1000LL * iconf(ICONF_SCROLL_DELAY);
            int64_t resize_delay = 1000LL * iconf(ICONF_RESIZE_DELAY);
            int64_t frame_time = SEC / iconf(ICONF_FPS);

            if (!win->resize_delayed && TIMEDIFF(win->last_resize, cur) < frame_time) {
                if (win->slow_mode) win->next_draw = cur, win->slow_mode = 0;
                TIMEINC(win->next_draw, resize_delay), win->resize_delayed = 1;
            }

            if (!win->scroll_delayed && TIMEDIFF(win->last_scroll, cur) < frame_time) {
                if (win->slow_mode) win->next_draw = cur, win->slow_mode = 0;
                TIMEINC(win->next_draw, scroll_delay), win->scroll_delayed = 1;
            }

            if (win->sync_active) continue;
            if (!win->active && !win->force_redraw) continue;

            int64_t remains = TIMEDIFF(cur, win->next_draw);

            if (remains <= 10000LL || win->force_redraw) {
                if (win->force_redraw)
                    redraw_borders(win, 1, 1);

                remains = frame_time;
                bool old_drawn = win->drawn_somthing;
                win->drawn_somthing = term_redraw(win->term);

                if (iconf(ICONF_TRACE_MISC) && win->drawn_somthing) info("Redraw");

                if (win->drawn_somthing || old_drawn) {
                    win->next_draw = cur;
                    TIMEINC(win->next_draw, remains);
                    win->slow_mode = 0;
                } else win->slow_mode = 1;

                win->force_redraw = 0;
                win->resize_delayed = 0;
                win->scroll_delayed = 0;
                win->blink_commited = 1;
            }

            if (!win->slow_mode) next_timeout = MIN(next_timeout,  remains);
        }
        xcb_flush(con);

        // TODO Try reconnect after timeout
        if ((!ctx.daemon_mode && !win_list_head) || xcb_connection_has_error(con)) break;

        if (reload_config) {
            load_config();
            reload_all_fonts();
        }
    }
}
