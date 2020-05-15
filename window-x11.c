/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#include "feature.h"

#define _POSIX_C_SOURCE 200809L

#if USE_PPOLL
#   define _GNU_SOURCE
#endif

#include "config.h"
#include "font.h"
#include "input.h"
#include "term.h"
#include "util.h"
#include "window-x11.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
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
#define NSS_CLASS "Nsst"
#define OPT_NAME_MAX 32
/* Need to be multiple of 4 */
#define PASTE_BLOCK_SIZE 1024

#define NSS_M_ALL (0xff)
#define NSS_M_TERM (XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_SHIFT)

struct nss_shortcut {
    uint32_t ksym;
    uint32_t mmask;
    uint32_t mstate;
    enum nss_shortcut_action {
        nss_sa_none,
        nss_sa_break,
        nss_sa_numlock,
        nss_sa_scroll_up,
        nss_sa_scroll_down,
        nss_sa_font_up,
        nss_sa_font_down,
        nss_sa_font_default,
        nss_sa_font_subpixel,
        nss_sa_new_window,
        nss_sa_copy,
        nss_sa_paste,
    } action;
} cshorts[] = {
    {XKB_KEY_Up, NSS_M_ALL, NSS_M_TERM, nss_sa_scroll_down},
    {XKB_KEY_Down, NSS_M_ALL, NSS_M_TERM, nss_sa_scroll_up},
    {XKB_KEY_Page_Up, NSS_M_ALL, NSS_M_TERM, nss_sa_font_up},
    {XKB_KEY_Page_Down, NSS_M_ALL, NSS_M_TERM, nss_sa_font_down},
    {XKB_KEY_Home, NSS_M_ALL, NSS_M_TERM, nss_sa_font_default},
    {XKB_KEY_End, NSS_M_ALL, NSS_M_TERM, nss_sa_font_subpixel},
    {XKB_KEY_N, NSS_M_ALL, NSS_M_TERM, nss_sa_new_window},
    {XKB_KEY_Num_Lock, NSS_M_ALL, NSS_M_TERM, nss_sa_numlock},
    {XKB_KEY_C, NSS_M_ALL, NSS_M_TERM, nss_sa_copy},
    {XKB_KEY_V, NSS_M_ALL, NSS_M_TERM, nss_sa_paste},
    {XKB_KEY_Break, 0, 0, nss_sa_break},
};

struct nss_context {
    _Bool daemon_mode;
    xcb_screen_t *screen;
    xcb_colormap_t mid;
    xcb_visualtype_t *vis;

    xcb_atom_t atom_net_wm_pid;
    xcb_atom_t atom_net_wm_name;
    xcb_atom_t atom_net_wm_icon_name;
    xcb_atom_t atom_wm_delete_window;
    xcb_atom_t atom_wm_protocols;
    xcb_atom_t atom_wm_normal_hints;
    xcb_atom_t atom_wm_size_hints;
    xcb_atom_t atom_utf8_string;
    xcb_atom_t atom_clipboard;
    xcb_atom_t atom_incr;
    xcb_atom_t atom_targets;

    struct xkb_context *xkb_ctx;
    struct xkb_state *xkb_state;
    struct xkb_keymap *xkb_keymap;

    int32_t xkb_core_kbd;
    uint8_t xkb_base_event;
    uint8_t xkb_base_err;

    struct pollfd *pfds;
    size_t pfdn;
    size_t pfdcap;
};

struct nss_context ctx;
xcb_connection_t *con = NULL;
nss_window_t *win_list_head = NULL;
volatile sig_atomic_t reload_config;

static nss_window_t *window_for_xid(xcb_window_t xid) {
    for (nss_window_t *win = win_list_head; win; win = win->next)
        if (win->wid == xid) return win;
    info("Window for xid not found");
    return NULL;
}
static nss_window_t *window_for_term_fd(int fd) {
    for (nss_window_t *win = win_list_head; win; win = win->next)
        if (win->term_fd == fd) return win;
    warn("Window for fd not found");
    return NULL;
}

static void load_params(void) {
    xcb_xrm_database_t *xrmdb = xcb_xrm_database_from_default(con);
    if (!xrmdb) return;

    char *res, name[OPT_NAME_MAX] = NSS_CLASS".color";
    const size_t n_pos = strlen(name);

    // .color0 -- .color255
    for (unsigned j = 0; j < NSS_PALETTE_SIZE - NSS_SPECIAL_COLORS; j++) {
        snprintf(name + n_pos, OPT_NAME_MAX, "%u", j);
        if (!xcb_xrm_resource_get_string(xrmdb, name, NULL, &res)) {
            nss_config_set_string(NSS_CCONFIG_COLOR_0 + j, res);
            free(res);
        }
    }

    //optmap is defined in config.c
    for(size_t i = 0; i < OPT_MAP_SIZE; i++) {
        snprintf(name, OPT_NAME_MAX, NSS_CLASS".%s", optmap[i].name);
        char *res = NULL;
        if (!xcb_xrm_resource_get_string(xrmdb, name, NULL, &res)) {
            nss_config_set_string(optmap[i].opt, res);
        }
        if (res) free(res);
    }

    xcb_xrm_database_free(xrmdb);
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

static _Bool update_keymap(void) {
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

static _Bool configure_xkb(void) {
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
void nss_init_context(void) {
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

    nss_init_render_context();

    if (!configure_xkb()) {
        xcb_disconnect(con);
        die("Can't configure XKB");
    }

    ctx.atom_net_wm_pid = intern_atom("_NET_WM_PID");
    ctx.atom_wm_delete_window = intern_atom("WM_DELETE_WINDOW");
    ctx.atom_wm_protocols = intern_atom("WM_PROTOCOLS");
    ctx.atom_wm_normal_hints = intern_atom("WM_NORMAL_HINTS");
    ctx.atom_wm_size_hints = intern_atom("WM_SIZE_HINTS");
    ctx.atom_utf8_string = intern_atom("UTF8_STRING");
    ctx.atom_net_wm_name = intern_atom("_NET_WM_NAME");
    ctx.atom_net_wm_icon_name = intern_atom("_NET_WM_ICON_NAME");
    ctx.atom_clipboard = intern_atom("CLIPBOARD");
    ctx.atom_incr = intern_atom("INCR");
    ctx.atom_targets = intern_atom("TARGETS");

    int32_t dpi = -1;
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(con));
    for (; it.rem; xcb_screen_next(&it))
        if (it.data) dpi = MAX(dpi, (it.data->width_in_pixels * 25.4)/it.data->width_in_millimeters);
    if (dpi > 0) nss_config_set_integer(NSS_ICONFIG_DPI, dpi);

    if (!nss_config_integer(NSS_ICONFIG_SKIP_CONFIG_FILE)) load_params();
    else nss_config_set_integer(NSS_ICONFIG_SKIP_CONFIG_FILE, 0);

    sigaction(SIGUSR1, &(struct sigaction){ .sa_handler = handle_sigusr1, .sa_flags = SA_RESTART}, NULL);
}

/* Free all resources */
void nss_free_context(void) {
    while (win_list_head)
        nss_free_window(win_list_head);
    xkb_state_unref(ctx.xkb_state);
    xkb_keymap_unref(ctx.xkb_keymap);
    xkb_context_unref(ctx.xkb_ctx);

    nss_free_render_context();
    free(ctx.pfds);

    xcb_disconnect(con);
    memset(&con, 0, sizeof(con));
}

void nss_window_get_dim(nss_window_t *win, int16_t *width, int16_t *height) {
    if (width) *width = win->width;
    if (height) *height = win->height;
}

void nss_window_set_cursor(nss_window_t *win, nss_cursor_type_t type) {
    win->cursor_type = type;
}

void nss_window_set_colors(nss_window_t *win, nss_color_t bg, nss_color_t cursor_fg) {
    nss_color_t obg = win->bg;
    if (bg) win->bg = bg;
    if (cursor_fg) win->cursor_fg = cursor_fg;

    if (bg && bg != obg) {
        uint32_t values2[2];
        values2[0] = values2[1] = win->bg;
        xcb_change_window_attributes(con, win->wid, XCB_CW_BACK_PIXEL, values2);
        xcb_change_gc(con, win->gc, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, values2);
    }

    if ((bg && bg != obg) || cursor_fg) {
        nss_term_damage(win->term, (nss_rect_t){0, 0, win->cw, win->ch});
        win->force_redraw = 1;
    }
}

void nss_window_set_mouse(nss_window_t *win, _Bool enabled) {
   if (enabled)
        win->ev_mask |= XCB_EVENT_MASK_POINTER_MOTION;
   else
       win->ev_mask &= ~XCB_EVENT_MASK_POINTER_MOTION;
   xcb_change_window_attributes(con, win->wid, XCB_CW_EVENT_MASK, &win->ev_mask);
}

void nss_window_set_sync(nss_window_t *win, _Bool state) {
    if (state) clock_gettime(CLOCK_MONOTONIC, &win->last_sync);
    win->sync_active = state;
}

/* This would probably be useful later when implemeting OSC for setting fonts
void nss_window_set_font(nss_window_t *win, const char * name) {
    if (!name) {
        warn("Empty font name");
        return;
    }
    free(win->font_name);
    win->font_name = strdup(name);
    nss_renderer_reload_font(win, 1);
    nss_term_damage(win->term, (nss_rect_t){0, 0, win->cw, win->ch});
    win->force_redraw = 1;
    xcb_flush(con);
}
*/

void nss_window_set_title(nss_window_t *win, const char *title, _Bool utf8) {
    if (!title) title = nss_config_string(NSS_SCONFIG_TITLE);
    xcb_change_property(con, XCB_PROP_MODE_REPLACE, win->wid,
            utf8 ? ctx.atom_net_wm_name : XCB_ATOM_WM_NAME,
            utf8 ? ctx.atom_utf8_string : XCB_ATOM_STRING, 8, strlen(title), title);
}

void nss_window_set_icon_name(nss_window_t *win, const char *title, _Bool utf8) {
    if (!title) title = nss_config_string(NSS_SCONFIG_TITLE);
    xcb_change_property(con, XCB_PROP_MODE_REPLACE, win->wid,
            utf8 ? ctx.atom_net_wm_icon_name : XCB_ATOM_WM_ICON_NAME,
            utf8 ? ctx.atom_utf8_string : XCB_ATOM_STRING, 8, strlen(title), title);
}

uint32_t get_win_gravity_from_config() {
    _Bool nx = nss_config_integer(NSS_ICONFIG_WINDOW_NEGATIVE_X);
    _Bool ny = nss_config_integer(NSS_ICONFIG_WINDOW_NEGATIVE_Y);
    switch (nx + 2 * ny) {
    case 0: return XCB_GRAVITY_NORTH_WEST;
    case 1: return XCB_GRAVITY_NORTH_EAST;
    case 2: return XCB_GRAVITY_SOUTH_WEST;
    default: return XCB_GRAVITY_NORTH_EAST;
    }
};

struct nss_cellspec nss_describe_cell(nss_cell_t cell, nss_color_t *palette, nss_color_t *extra, _Bool blink, _Bool selected) {
    struct nss_cellspec res;

    // Calculate colors

    if ((cell.attr & (nss_attrib_bold | nss_attrib_faint)) == nss_attrib_bold && cell.fg < 8) cell.fg += 8;
    res.bg = cell.bg < NSS_PALETTE_SIZE ? palette[cell.bg] : extra[cell.bg - NSS_PALETTE_SIZE];
    res.fg = cell.fg < NSS_PALETTE_SIZE ? palette[cell.fg] : extra[cell.fg - NSS_PALETTE_SIZE];
    if ((cell.attr & (nss_attrib_bold | nss_attrib_faint)) == nss_attrib_faint)
        res.fg = (res.fg & 0xFF000000) | ((res.fg & 0xFEFEFE) >> 1);
    if ((cell.attr & nss_attrib_inverse) ^ selected) SWAP(nss_color_t, res.fg, res.bg);
    if ((!selected && cell.attr & nss_attrib_invisible) || (cell.attr & nss_attrib_blink && blink)) res.fg = res.bg;

    // Optimize rendering of U+2588 FULL BLOCK

    if (cell.ch == 0x2588) res.bg = res.fg;
    if (cell.ch == ' ' || res.fg == res.bg) cell.ch = 0;

    // Calculate attributes

    res.ch = cell.ch;
    res.face = cell.ch ? (cell.attr & nss_font_attrib_mask) : 0;
    res.wide = !!(cell.attr & nss_attrib_wide);
    res.underlined = !!(cell.attr & nss_attrib_underlined) && (res.fg != res.bg);
    res.stroke = !!(cell.attr & nss_attrib_strikethrough) && (res.fg != res.bg);

    return res;
}

nss_window_t *nss_find_shared_font(nss_window_t *win, _Bool need_free) {
    _Bool found_font = 0, found_cache = 0;
    nss_window_t *found = 0;

    for (nss_window_t *src = win_list_head; src; src = src->next) {
        if ((src->font_size == win->font_size || (!win->font_size &&
                src->font_size == nss_config_integer(NSS_ICONFIG_FONT_SIZE))) &&
                !strcmp(win->font_name, src->font_name) && src != win) {
            found_font = 1;
            found = src;
            if (src->subpixel_fonts == win->subpixel_fonts) {
                found_cache = 1;
                break;
            }
        }
    }

    nss_font_t *newf = found_font ? nss_font_reference(found->font) :
            nss_create_font(win->font_name, win->font_size, nss_config_integer(NSS_ICONFIG_DPI));
    if (!newf) {
        warn("Can't create new font: %s", win->font_name);
        return NULL;
    }

    nss_glyph_cache_t *newc = found_cache ? nss_cache_reference(found->font_cache) :
            nss_create_cache(newf, win->subpixel_fonts);

    if (need_free) {
        nss_free_cache(win->font_cache);
        nss_free_font(win->font);
    }

    win->font = newf;
    win->font_cache = newc;
    win->font_size = nss_font_get_size(newf);

    //Initialize default font size
    if (!nss_config_integer(NSS_ICONFIG_FONT_SIZE))
        nss_config_set_integer(NSS_ICONFIG_FONT_SIZE, win->font_size);

    nss_cache_font_dim(win->font_cache, &win->char_width, &win->char_height, &win->char_depth);

    return found;
}

void nss_window_set_default_props(nss_window_t *win) {
    uint32_t pid = getpid();
    xcb_change_property(con, XCB_PROP_MODE_REPLACE, win->wid, ctx.atom_net_wm_pid, XCB_ATOM_CARDINAL, 32, 1, &pid);
    xcb_change_property(con, XCB_PROP_MODE_REPLACE, win->wid, ctx.atom_wm_protocols, XCB_ATOM_ATOM, 32, 1, &ctx.atom_wm_delete_window);
    const char class[] = "Nsst", *extra;
    xcb_change_property(con, XCB_PROP_MODE_REPLACE, win->wid, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, sizeof(class), class);
    if ((extra = nss_config_string(NSS_SCONFIG_TERM_CLASS)))
        xcb_change_property(con, XCB_PROP_MODE_APPEND, win->wid, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, strlen(extra), extra);
    uint32_t nhints[] = {
        64 | 256, //PResizeInc, PBaseSize
        nss_config_integer(NSS_ICONFIG_WINDOW_X), nss_config_integer(NSS_ICONFIG_WINDOW_Y), // Position
        win->width, win->height, // Size
        win->left_border * 2 + win->char_width, win->left_border * 2 + win->char_depth + win->char_height, // Min size
        0, 0, //Max size
        win->char_width, win->char_depth + win->char_height, // Size inc
        0, 0, 0, 0, // Min/max aspect
        win->left_border * 2 + win->char_width, win->left_border * 2 + win->char_depth + win->char_height, // Base size
        get_win_gravity_from_config(), // Gravity
    };
    if (nss_config_integer(NSS_ICONFIG_HAS_GEOMETRY))
        nhints[0] |= 1 | 2 | 512; // USPosition, USSize, PWinGravity
    else
        nhints[0] |= 4 | 8; // PPosition, PSize
    if (nss_config_integer(NSS_ICONFIG_FIXED_SIZE)) {
        nhints[7] = nhints[5] = nhints[3];
        nhints[8] = nhints[6] = nhints[4];
        nhints[0] |= 16 | 32; // PMinSize, PMaxSize
    }
    xcb_change_property(con, XCB_PROP_MODE_REPLACE, win->wid, ctx.atom_wm_normal_hints,
            ctx.atom_wm_size_hints, 8*sizeof(*nhints), sizeof(nhints)/sizeof(*nhints), nhints);
}

/* Create new window */
nss_window_t *nss_create_window(void) {
    nss_window_t *win = calloc(1, sizeof(nss_window_t));
    win->cursor_width = nss_config_integer(NSS_ICONFIG_CURSOR_WIDTH);
    win->underline_width = nss_config_integer(NSS_ICONFIG_UNDERLINE_WIDTH);
    win->left_border = nss_config_integer(NSS_ICONFIG_LEFT_BORDER);
    win->top_border = nss_config_integer(NSS_ICONFIG_TOP_BORDER);
    win->bg = nss_config_color(NSS_CCONFIG_BG);
    win->cursor_fg = nss_config_color(NSS_CCONFIG_CURSOR_FG);
    if (nss_config_integer(NSS_ICONFIG_REVERSE_VIDEO))
        SWAP(nss_color_t, win->bg, win->cursor_fg);
    win->cursor_type = nss_config_integer(NSS_ICONFIG_CURSOR_SHAPE);
    win->subpixel_fonts = nss_config_integer(NSS_ICONFIG_SUBPIXEL_FONTS);
    win->font_size = nss_config_integer(NSS_ICONFIG_FONT_SIZE);
    win->width = nss_config_integer(NSS_ICONFIG_WINDOW_WIDTH);
    win->height = nss_config_integer(NSS_ICONFIG_WINDOW_HEIGHT);

    win->active = 1;
    win->focused = 1;
    win->term_fd = -1;

    win->font_name = strdup(nss_config_string(NSS_SCONFIG_FONT_NAME));
    if (!win->font_name) {
        nss_free_window(win);
        return NULL;
    }

    xcb_void_cookie_t c;

    win->ev_mask = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_VISIBILITY_CHANGE |
        XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
        XCB_EVENT_MASK_BUTTON_MOTION | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_PROPERTY_CHANGE;
    uint32_t mask1 =  XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL |
        XCB_CW_BIT_GRAVITY | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
    uint32_t values1[5] = { win->bg, win->bg, XCB_GRAVITY_NORTH_WEST, win->ev_mask, ctx.mid };
    int16_t x = nss_config_integer(NSS_ICONFIG_WINDOW_X);
    int16_t y = nss_config_integer(NSS_ICONFIG_WINDOW_Y);

    // Adjust geometry
    if (nss_config_integer(NSS_ICONFIG_WINDOW_NEGATIVE_X))
        x += ctx.screen->width_in_pixels - win->width - 2;
    if (nss_config_integer(NSS_ICONFIG_WINDOW_NEGATIVE_Y))
        y += ctx.screen->height_in_pixels - win->height - 2;

    win->wid = xcb_generate_id(con);
    c = xcb_create_window_checked(con, TRUE_COLOR_ALPHA_DEPTH, win->wid, ctx.screen->root,
                                  x, y, win->width, win->height, 0,
                                  XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                  ctx.vis->visual_id, mask1, values1);
    if (check_void_cookie(c)) {
        warn("Can't create window");
        nss_free_window(win);
        return NULL;
    }

    win->gc = xcb_generate_id(con);
    uint32_t mask2 = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_GRAPHICS_EXPOSURES;
    uint32_t values2[3] = { win->bg, win->bg, 0 };

    c = xcb_create_gc_checked(con, win->gc, win->wid, mask2, values2);
    if (check_void_cookie(c)) {
        warn("Can't create GC");
        nss_free_window(win);
        return NULL;
    }

    if (!nss_renderer_reload_font(win, 0)) {
        warn("Can't create window");
        nss_free_window(win);
        return NULL;
    }

    win->term = nss_create_term(win, win->cw, win->ch);
    if (!win->term) {
        warn("Can't create term");
        nss_free_window(win);
        return NULL;
    }

    nss_window_set_default_props(win);
    nss_window_set_title(win, NULL, nss_config_integer(NSS_ICONFIG_UTF8));
    nss_window_set_icon_name(win, NULL, nss_config_integer(NSS_ICONFIG_UTF8));

    win->next = win_list_head;
    win->prev = NULL;
    if (win_list_head) win_list_head->prev = win;
    win_list_head = win;


    if (ctx.pfdn + 1 > ctx.pfdcap) {
        struct pollfd *new = realloc(ctx.pfds, ctx.pfdcap + INIT_PFD_NUM);

        if (new) {
            for (size_t i = 0; i < INIT_PFD_NUM; i++) {
                new[i + ctx.pfdn].fd = -1;
                new[i + ctx.pfdn].events = 0;
            }
            ctx.pfdcap += INIT_PFD_NUM;
            ctx.pfds = new;
        } else {
            warn("Can't reallocate ctx.pfds");
            nss_free_window(win);
            return NULL;
        }
    }

    ctx.pfdn++;
    size_t i = 1;
    while (ctx.pfds[i].fd >= 0) i++;
    // Because it might become -1 suddenly
    win->term_fd = nss_term_fd(win->term);
    ctx.pfds[i].events = POLLIN | POLLHUP;
    ctx.pfds[i].fd = win->term_fd;

    xcb_map_window(con, win->wid);
    xcb_flush(con);
    return win;
}

/* Free previously created windows */
void nss_free_window(nss_window_t *win) {
    if (win->wid) {
        xcb_unmap_window(con, win->wid);
        nss_renderer_free(win);
        xcb_free_gc(con, win->gc);
        xcb_destroy_window(con, win->wid);
        xcb_flush(con);
    }

    if (win->next) win->next->prev = win->prev;
    if (win->prev) win->prev->next = win->next;
    else win_list_head =  win->next;

    if (win->term_fd > 0) {
        size_t i = 0;
        while (ctx.pfds[i].fd != win->term_fd && i < ctx.pfdcap) i++;
        if (i < ctx.pfdcap)
            ctx.pfds[i].fd = -1;
        else warn("Window fd not found");
        ctx.pfdn--;
    }

    if (win->term)
        nss_free_term(win->term);
    if (win->font_cache)
        nss_free_cache(win->font_cache);
    if (win->font)
        nss_free_font(win->font);

    for (size_t i = 0; i < nss_ct_MAX; i++)
        free(win->clipped[i]);

    free(win->font_name);
    free(win);
};

void nss_window_shift(nss_window_t *win, nss_coord_t ys, nss_coord_t yd, nss_coord_t height, _Bool delay) {
    struct timespec cur;
    clock_gettime(CLOCK_MONOTONIC, &cur);

    _Bool scrolled_recently = TIMEDIFF(win->last_scroll, cur) <  SEC/2/nss_config_integer(NSS_ICONFIG_FPS);

    win->last_scroll = cur;

    ys = MAX(0, MIN(ys, win->ch));
    yd = MAX(0, MIN(yd, win->ch));
    height = MIN(height, MIN(win->ch - ys, win->ch - yd));

    if (delay && scrolled_recently) {
        if (!win->damaged_y1) {
            win->damaged_y0 = yd, win->damaged_y1 = yd + height;
            nss_term_damage(win->term, (nss_rect_t){ .x = 0, .y = yd,
                    .width = win->cw, .height = height });
        } else {
            if (yd < win->damaged_y0){
                nss_term_damage(win->term, (nss_rect_t){ .x = 0, .y = yd,
                        .width = win->cw, .height = MAX(yd + height, win->damaged_y0 + 1) - yd });
                win->damaged_y1 = yd;
            }
            if (yd + height > win->damaged_y1){
                nss_term_damage(win->term, (nss_rect_t){ .x = 0, .y = MIN(win->damaged_y1, yd) - 1,
                        .width = win->cw, .height = yd + height - MIN(win->damaged_y1, yd)});
                win->damaged_y1 = yd + height;
            }
        }
        return;
    }

    if (!height) return;

    ys *= win->char_height + win->char_depth;
    yd *= win->char_height + win->char_depth;
    nss_coord_t width = win->cw * win->char_width;
    height *= win->char_depth + win->char_height;

    nss_renderer_copy(win, (nss_rect_t){0, yd, width, height}, 0, ys);
}

inline static xcb_atom_t target_to_atom(nss_clipboard_target_t target) {
    switch (target) {
    case nss_ct_secondary: return XCB_ATOM_SECONDARY;
    case nss_ct_primary: return XCB_ATOM_PRIMARY;
    default: return ctx.atom_clipboard;
    }
}

static void window_clip_copy(nss_window_t *win) {
    if (win->clipped[nss_ct_primary]) {
        uint8_t *dup = (uint8_t*)strdup((char *)win->clipped[nss_ct_primary]);
        if (dup) {
            if (nss_term_keep_clipboard(win->term)) {
                uint8_t *dup2 = (uint8_t*)strdup((char *)win->clipped[nss_ct_primary]);
                free(win->clipboard);
                win->clipboard = dup2;
            }
            nss_window_set_clip(win, dup, NSS_TIME_NOW, nss_ct_clipboard);
        }
    }
}

void nss_window_set_clip(nss_window_t *win, uint8_t *data, uint32_t time, nss_clipboard_target_t target) {
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

void nss_window_paste_clip(nss_window_t *win, nss_clipboard_target_t target) {
    xcb_convert_selection(con, win->wid, target_to_atom(target),
          nss_term_is_utf8(win->term) ? ctx.atom_utf8_string : XCB_ATOM_STRING, target_to_atom(target), XCB_CURRENT_TIME);
}

static void redraw_borders(nss_window_t *win, _Bool top_left, _Bool bottom_right) {
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

void nss_window_handle_resize(nss_window_t *win, int16_t width, int16_t height) {
    //Handle resize

    win->width = width;
    win->height = height;

    nss_coord_t new_cw = MAX(1, (win->width - 2*win->left_border)/win->char_width);
    nss_coord_t new_ch = MAX(1, (win->height - 2*win->top_border)/(win->char_height+win->char_depth));
    nss_coord_t delta_x = new_cw - win->cw;
    nss_coord_t delta_y = new_ch - win->ch;

    if (delta_x || delta_y) {
        nss_term_resize(win->term, new_cw, new_ch);
        nss_renderer_resize(win, new_cw, new_ch);
        clock_gettime(CLOCK_MONOTONIC, &win->last_scroll);
        win->last_resize = win->last_scroll;
    }

    if (delta_x < 0 || delta_y < 0)
        redraw_borders(win, 0, 1);
}

static void handle_expose(nss_window_t *win, nss_rect_t damage) {
    int16_t width = win->cw * win->char_width + win->left_border;
    int16_t height = win->ch * (win->char_height + win->char_depth) + win->top_border;

    size_t num_damaged = 0;
    nss_rect_t damaged[NUM_BORDERS], borders[NUM_BORDERS] = {
        {0, 0, win->left_border, height},
        {win->left_border, 0, width, win->top_border},
        {width, 0, win->width - width, win->height},
        {0, height, width, win->height - height},
    };
    for (size_t i = 0; i < NUM_BORDERS; i++)
        if (intersect_with(&borders[i], &damage))
                damaged[num_damaged++] = borders[i];
    if (num_damaged) xcb_poly_fill_rectangle(con, win->wid, win->gc, num_damaged, (xcb_rectangle_t *)damaged);

    nss_rect_t inters = { 0, 0, width - win->left_border, height - win->top_border};
    damage = rect_shift(damage, -win->left_border, -win->top_border);
    if (intersect_with(&inters, &damage)) nss_renderer_update(win, inters);
}

static void handle_focus(nss_window_t *win, _Bool focused) {
    win->focused = focused;
    nss_term_focus(win->term, focused);
}

static void handle_keydown(nss_window_t *win, xkb_keycode_t keycode) {
    nss_key_t key = nss_describe_key(ctx.xkb_state, keycode);

    if (key.sym == XKB_KEY_NoSymbol) return;

    enum nss_shortcut_action action = nss_sa_none;
    for (size_t i = 0; i < sizeof(cshorts)/sizeof(*cshorts); i++) {
        if (cshorts[i].ksym == key.sym && (key.mask & cshorts[i].mmask) == cshorts[i].mstate) {
            action = cshorts[i].action;
            break;
        }
    }

    switch (action) {
    case nss_sa_break:
        nss_term_sendbreak(win->term);
        return;
    case nss_sa_numlock:
        nss_term_toggle_numlock(win->term);
        return;
    case nss_sa_scroll_up:
        nss_term_scroll_view(win->term, -nss_config_integer(NSS_ICONFIG_SCROLL_AMOUNT));
        return;
    case nss_sa_scroll_down:
        nss_term_scroll_view(win->term, nss_config_integer(NSS_ICONFIG_SCROLL_AMOUNT));
        return;
    case nss_sa_font_up:
    case nss_sa_font_down:
    case nss_sa_font_default:
    case nss_sa_font_subpixel:
        if (action == nss_sa_font_up)
            win->font_size += nss_config_integer(NSS_ICONFIG_FONT_SIZE_STEP);
        else if (action == nss_sa_font_down)
            win->font_size -= nss_config_integer(NSS_ICONFIG_FONT_SIZE_STEP);
        else if (action == nss_sa_font_default)
            win->font_size = nss_config_integer(NSS_ICONFIG_FONT_SIZE);
        else win->subpixel_fonts = !win->subpixel_fonts;
        nss_renderer_reload_font(win, 1);
        nss_term_damage(win->term, (nss_rect_t){0, 0, win->cw, win->ch});
        win->force_redraw = 1;
        return;
    case nss_sa_new_window:
        nss_create_window();
        return;
    case nss_sa_copy:
        window_clip_copy(win);
        return;
    case nss_sa_paste:
        nss_window_paste_clip(win, nss_ct_clipboard);
        return;
    case nss_sa_none: break;
    }

    nss_handle_input(key, win->term);
}

static void send_selection_data(nss_window_t *win, xcb_window_t req, xcb_atom_t sel, xcb_atom_t target, xcb_atom_t prop, xcb_timestamp_t time) {
    xcb_selection_notify_event_t ev;
    ev.property = XCB_NONE;
    ev.requestor = req;
    ev.response_type = XCB_SELECTION_NOTIFY;
    ev.selection = sel;
    ev.target = target;
    ev.time = time;

    if (prop == XCB_NONE) prop = target;

    if (target == ctx.atom_targets) {
        uint32_t data[] = {ctx.atom_utf8_string, XCB_ATOM_STRING};
        xcb_change_property(con, XCB_PROP_MODE_REPLACE, req, prop, XCB_ATOM_ATOM, 32, sizeof(data)/sizeof(*data), data);
    } else if (target == ctx.atom_utf8_string || target == XCB_ATOM_STRING) {
        uint8_t *data = NULL;

        if (sel == XCB_ATOM_PRIMARY) data = win->clipped[nss_ct_primary];
        else if (sel == XCB_ATOM_SECONDARY) data = win->clipped[nss_ct_secondary];
        else if (sel == ctx.atom_clipboard) {
            if (nss_term_keep_clipboard(win->term))
                data = win->clipboard;
            else
                data = win->clipped[nss_ct_clipboard];
        }

        if (data) {
            xcb_change_property(con, XCB_PROP_MODE_REPLACE, req, prop, target, 8, strlen((char *)data), data);
            ev.property = prop;
        }
    }

    xcb_send_event(con, 1, req, 0, (const char *)&ev);
}

static void receive_selection_data(nss_window_t *win, xcb_atom_t prop, _Bool pnotify) {
    if (prop == XCB_NONE) return;

    size_t left, offset = 0;
    uint8_t leftover[3], leftover_len = 0;

    do {
        xcb_get_property_cookie_t pc = xcb_get_property(con, 0, win->wid, prop, XCB_GET_PROPERTY_TYPE_ANY, offset, PASTE_BLOCK_SIZE/4);
        xcb_generic_error_t *err = NULL;
        xcb_get_property_reply_t *rep = xcb_get_property_reply(con, pc, &err);
        if (err) {
            free(err);
            free(rep);
            return;
        }
        left = rep->bytes_after;

        if (pnotify && !rep->value_len && !left) {
           win->ev_mask &= ~XCB_EVENT_MASK_PROPERTY_CHANGE;
           xcb_change_window_attributes(con, win->wid, XCB_CW_EVENT_MASK, &win->ev_mask);
        }

        if (rep->type == ctx.atom_incr) {
           win->ev_mask |= XCB_EVENT_MASK_PROPERTY_CHANGE;
           xcb_change_window_attributes(con, win->wid, XCB_CW_EVENT_MASK, &win->ev_mask);
           xcb_delete_property(con, win->wid, prop);
           xcb_flush(con);
           continue;
        }

        ssize_t size = rep->format * rep->value_len / 8;
        uint8_t *data = xcb_get_property_value(rep);
        uint8_t *pos = data, *end = data + size;

        while ((pos = memchr(pos, '\n', end - pos))) *pos++ = '\r';

        if (size) {
            if (!offset) nss_term_paste_begin(win->term);

            if ((rep->type == ctx.atom_utf8_string) ^ nss_term_is_utf8(win->term)) {
                static uint8_t buf[2*PASTE_BLOCK_SIZE];

                pos = data;
                size = 0;
                if (rep->type == ctx.atom_utf8_string) {
                    while(pos < end) {
                        nss_char_t ch;
                        if (utf8_decode(&ch, (const uint8_t **)&pos, end))
                            buf[size++] = ch;
                    }
                } else {
                    while(pos < end)
                        size += utf8_encode(*pos++, buf + size, buf + BUFSIZ);
                }

                data = buf;
            }

            if (nss_term_paste_need_encode(win->term)) {
                static uint8_t base64[4*(2*PASTE_BLOCK_SIZE)/3 + 4];

                while (leftover_len < 3 && size) leftover[leftover_len++] = *data++, size--;
                size_t pre = base64_encode(base64, leftover, leftover + leftover_len) - base64;

                if (size) {
                    if (left) {
                        leftover_len = size % 3;
                        if (leftover_len > 0) leftover[0] = data[size - leftover_len], size--;
                        if (leftover_len > 1) leftover[1] = data[size - 1], size--;
                    }

                    size = base64_encode(base64 + pre, data, data + size) - base64;
                }
                data = base64;
            }

            nss_term_sendkey(win->term, data, size);

            if (!left) nss_term_paste_end(win->term);
        }

        free(rep);

        offset += size / 4;
    } while (left > 0);

    xcb_delete_property(con, win->wid, prop);
}

/* Start window logic, handling all windows in context */
void nss_context_run(void) {
    int64_t next_timeout = SEC/nss_config_integer(NSS_ICONFIG_FPS);
    for (;;) {
#if USE_PPOLL
        struct timespec ppoll_timeout = { .tv_sec = 0, .tv_nsec = next_timeout};
        if (ppoll(ctx.pfds, ctx.pfdcap, &ppoll_timeout, NULL) < 0 && errno != EINTR)
#else
        if (poll(ctx.pfds, ctx.pfdcap, next_timeout/(SEC/1000)) < 0 && errno != EINTR)
#endif
            warn("Poll error: %s", strerror(errno));
        if (ctx.pfds[0].revents & POLLIN) {
            xcb_generic_event_t *event;
            while ((event = xcb_poll_for_event(con))) {
                switch (event->response_type &= 0x7f) {
                case XCB_EXPOSE:{
                    xcb_expose_event_t *ev = (xcb_expose_event_t*)event;
                    nss_window_t *win = window_for_xid(ev->window);
                    if (!win) break;
                    handle_expose(win, (nss_rect_t){ev->x, ev->y, ev->width, ev->height});
                    break;
                }
                case XCB_CONFIGURE_NOTIFY:{
                    xcb_configure_notify_event_t *ev = (xcb_configure_notify_event_t*)event;
                    nss_window_t *win = window_for_xid(ev->window);
                    if (!win) break;

                    if (ev->width != win->width || ev->height != win->height)
                        nss_window_handle_resize(win, ev->width, ev->height);
                    if (!win->got_configure) {
                        nss_term_resize(win->term, win->cw, win->ch);
                        nss_term_damage(win->term, (nss_rect_t){0, 0, win->cw, win->ch});
                        win->force_redraw = 1;
                        win->got_configure = 1;
                    }
                    break;
                }
                case XCB_KEY_RELEASE: /* ignore */ break;
                case XCB_KEY_PRESS:{
                    xcb_key_release_event_t *ev = (xcb_key_release_event_t*)event;
                    nss_window_t *win = window_for_xid(ev->event);
                    if (!win) break;
                    handle_keydown(win, ev->detail);
                    break;
                }
                case XCB_FOCUS_IN:
                case XCB_FOCUS_OUT:{
                    xcb_focus_in_event_t *ev = (xcb_focus_in_event_t*)event;
                    nss_window_t *win = window_for_xid(ev->event);
                    if (!win) break;
                    handle_focus(win, event->response_type == XCB_FOCUS_IN);
                    break;
                }
                case XCB_BUTTON_RELEASE: /* All these events have same structure */
                case XCB_BUTTON_PRESS:
                case XCB_MOTION_NOTIFY: {
                    xcb_motion_notify_event_t *ev = (xcb_motion_notify_event_t*)event;
                    nss_window_t *win = window_for_xid(ev->event);
                    if (!win) break;

                    uint8_t button = ev->detail - XCB_BUTTON_INDEX_1;
                    int16_t x = MAX(0, MIN(win->cw, (ev->event_x - win->left_border)/
                            win->char_width));
                    int16_t y = MAX(0, MIN(win->ch, (ev->event_y - win->top_border) /
                            (win->char_height + win->char_depth)));
                    /* XCB_BUTTON_PRESS -> nss_me_press
                     * XCB_BUTTON_RELEASE -> nss_me_release
                     * XCB_MOTION_NOTIFY -> nss_me_motion
                     */
                    nss_mouse_event_t evtype = (ev->response_type & 0xF7) - 4;
                    nss_mouse_state_t mask = ev->state & nss_ms_state_mask;

                    nss_term_mouse(win->term, x, y, mask, evtype, button);
                    break;
                }
                case XCB_SELECTION_CLEAR: {
                    xcb_selection_clear_event_t *ev = (xcb_selection_clear_event_t*)event;
                    nss_window_t *win = window_for_xid(ev->owner);
                    if (!win) break;
                    // Clear even if set keep?
                    nss_term_clear_selection(win->term);
                    break;
                }
                case XCB_PROPERTY_NOTIFY: {
                    xcb_property_notify_event_t *ev = (xcb_property_notify_event_t*)event;
                    nss_window_t *win = window_for_xid(ev->window);
                    if (!win) break;
                    if ((ev->atom == XCB_ATOM_PRIMARY || ev->atom == XCB_ATOM_PRIMARY) &&
                            ev->state == XCB_PROPERTY_NEW_VALUE)
                        receive_selection_data(win, ev->atom, 1);
                    break;
                }
                case XCB_SELECTION_NOTIFY: {
                    xcb_selection_notify_event_t *ev = (xcb_selection_notify_event_t*)event;
                    nss_window_t *win = window_for_xid(ev->requestor);
                    if (!win) break;
                    receive_selection_data(win, ev->property, 0);
                    break;
                }
                case XCB_SELECTION_REQUEST: {
                    xcb_selection_request_event_t *ev = (xcb_selection_request_event_t*)event;
                    nss_window_t *win = window_for_xid(ev->owner);
                    if (!win) break;
                    send_selection_data(win, ev->requestor, ev->selection, ev->target, ev->property, ev->time);
                    break;
                }
                case XCB_CLIENT_MESSAGE: {
                    xcb_client_message_event_t *ev = (xcb_client_message_event_t*)event;
                    nss_window_t *win = window_for_xid(ev->window);
                    if (!win) break;
                    if (ev->format == 32 && ev->data.data32[0] == ctx.atom_wm_delete_window) {
                        nss_free_window(win);
                        if (!win_list_head && !ctx.daemon_mode) {
                            free(event);
                            return;
                        }
                    }
                    break;
                }
                case XCB_VISIBILITY_NOTIFY: {
                    xcb_visibility_notify_event_t *ev = (xcb_visibility_notify_event_t*)event;
                    nss_window_t *win = window_for_xid(ev->window);
                    if (!win) break;
                    win->active = ev->state != XCB_VISIBILITY_FULLY_OBSCURED;
                    nss_term_visibility(win->term, win->active);
                    break;
                }
                case XCB_MAP_NOTIFY:
                case XCB_UNMAP_NOTIFY: {
                    xcb_map_notify_event_t *ev = (xcb_map_notify_event_t *)event;
                    nss_window_t *win = window_for_xid(ev->window);
                    if (!win) break;
                    win->active = ev->response_type == XCB_MAP_NOTIFY;
                    nss_term_visibility(win->term, win->active);
                    break;
                }
                case XCB_DESTROY_NOTIFY: {
                   break;
                }
                case 0: {
                    xcb_generic_error_t *err = (xcb_generic_error_t*)event;
                    warn("[X11 Error] major=%"PRIu8", minor=%"PRIu16", error=%"PRIu8, err->major_code, err->minor_code, err->error_code);
                    break;
                }
                default:
                    if (event->response_type == ctx.xkb_base_event) {
                        struct _xkb_any_event {
                            uint8_t response_type;
                            uint8_t xkbType;
                            uint16_t sequence;
                            xcb_timestamp_t time;
                            uint8_t deviceID;
                        } *xkb_ev;

                        xkb_ev = (struct _xkb_any_event*)event;
                        if (xkb_ev->deviceID == ctx.xkb_core_kbd) {
                            switch (xkb_ev->xkbType) {
                            case XCB_XKB_NEW_KEYBOARD_NOTIFY : {
                                xcb_xkb_new_keyboard_notify_event_t *ev = (xcb_xkb_new_keyboard_notify_event_t*)event;
                                if (ev->changed & XCB_XKB_NKN_DETAIL_KEYCODES)
                                    update_keymap();
                                break;
                            }
                            case XCB_XKB_MAP_NOTIFY: {
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
                                warn("Unknown xcb-xkb event type: %02"PRIu8, xkb_ev->xkbType);
                            }
                        }
                    } else warn("Unknown xcb event type: %02"PRIu8, event->response_type);
                    break;
                }
                free(event);
            }
        }

        for (size_t i = 1; i < ctx.pfdcap; i++) {
            if (ctx.pfds[i].fd > 0) {
                nss_window_t *win = window_for_term_fd(ctx.pfds[i].fd);
                if (ctx.pfds[i].revents & POLLIN & win->got_configure) {
                    nss_term_read(win->term);
                } else if (ctx.pfds[i].revents & (POLLERR | POLLNVAL | POLLHUP)) {
                    nss_free_window(win);
                }
            }
        }

        next_timeout = SEC/nss_config_integer(NSS_ICONFIG_FPS);
        struct timespec cur;
        clock_gettime(CLOCK_MONOTONIC, &cur);

        for (nss_window_t *win = win_list_head; win; win = win->next) {
            if (TIMEDIFF(win->last_sync, cur) > nss_config_integer(NSS_ICONFIG_SYNC_TIME)*1000LL && win->sync_active)
                win->sync_active = 0;
            if (TIMEDIFF(win->last_blink, cur) > nss_config_integer(NSS_ICONFIG_BLINK_TIME)*1000LL && win->active) {
                win->blink_state = !win->blink_state;
                win->blink_commited = 0;
                win->last_blink = cur;
            }

            if (win->sync_active) continue;

            int64_t scroll_delay = 1000LL * nss_config_integer(NSS_ICONFIG_SCROLL_DELAY);
            int64_t resize_delay = 1000LL * nss_config_integer(NSS_ICONFIG_RESIZE_DELAY);
            int64_t frame_time = SEC / nss_config_integer(NSS_ICONFIG_FPS);

            if (!win->resize_delayed && TIMEDIFF(win->last_resize, cur) < frame_time)
                TIMEINC(win->next_draw, resize_delay), win->resize_delayed = 1;

            if (!win->scroll_delayed && TIMEDIFF(win->last_scroll, cur) < frame_time)
                TIMEINC(win->next_draw, scroll_delay), win->scroll_delayed = 1;

            int64_t remains = TIMEDIFF(cur, win->next_draw);

            if (remains <= 1000LL || win->force_redraw) {
                if (win->force_redraw)
                    redraw_borders(win, 1, 1);

                remains = frame_time;
                _Bool old_drawn = win->drawn_somthing;
                win->drawn_somthing = nss_term_redraw_dirty(win->term, 1);

                if (win->drawn_somthing || old_drawn) {
                    win->next_draw = cur;
                    TIMEINC(win->next_draw, remains);
                }

                win->force_redraw = 0;
                win->resize_delayed = 0;
                win->scroll_delayed = 0;
                win->blink_commited = 1;
             }

             next_timeout = MIN(next_timeout,  remains);
        }
        xcb_flush(con);

        // TODO Try reconnect after timeout
        if ((!ctx.daemon_mode && !win_list_head) || xcb_connection_has_error(con)) break;

        if (reload_config) {
            reload_config = 0;
            load_params();
            for (nss_window_t *win = win_list_head; win; win = win->next)
                nss_renderer_reload_font(win, 1);
        }
    }
}
