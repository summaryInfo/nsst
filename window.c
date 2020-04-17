/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#define _POSIX_C_SOURCE 200809L

#include "features.h"

#ifdef USE_PPOLL
#   define _GNU_SOURCE
#endif

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
#include <xcb/render.h>
#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_xrm.h>
#include <xcb/xkb.h>
#include <xkbcommon/xkbcommon-x11.h>

#ifdef USE_BOXDRAWING
#   include "boxdraw.h"
#endif
#include "config.h"
#include "font.h"
#include "input.h"
#include "term.h"
#include "util.h"
#include "window.h"

#define TRUE_COLOR_ALPHA_DEPTH 32
#define NUM_BORDERS 4
#define INIT_PFD_NUM 16

#define WORDS_IN_MESSAGE 256
#define HEADER_WORDS ((sizeof(nss_glyph_mesg_t)+sizeof(uint32_t))/sizeof(uint32_t))
#define CHARS_PER_MESG (WORDS_IN_MESSAGE - HEADER_WORDS)

#define CB(c) (((c) & 0xff) * 0x101)
#define CG(c) ((((c) >> 8) & 0xff) * 0x101)
#define CR(c) ((((c) >> 16) & 0xff) * 0x101)
#define CA(c) ((((c) >> 24) & 0xff) * 0x101)
#define MAKE_COLOR(c) {.red=CR(c), .green=CG(c), .blue=CB(c), .alpha=CA(c)}

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
    } action;
} cshorts[] = {
    {XKB_KEY_Up, NSS_M_ALL, NSS_M_TERM, nss_sa_scroll_down},
    {XKB_KEY_Down, NSS_M_ALL, NSS_M_TERM, nss_sa_scroll_up},
    {XKB_KEY_Page_Up, NSS_M_ALL, NSS_M_TERM, nss_sa_font_up},
    {XKB_KEY_Page_Down, NSS_M_ALL, NSS_M_TERM, nss_sa_font_down},
    {XKB_KEY_Home, NSS_M_ALL, NSS_M_TERM, nss_sa_font_default},
    {XKB_KEY_End, NSS_M_ALL, NSS_M_TERM, nss_sa_font_subpixel},
    {XKB_KEY_N, NSS_M_ALL, NSS_M_TERM, nss_sa_new_window},
    {XKB_KEY_Num_Lock, NSS_M_TERM, NSS_M_TERM, nss_sa_numlock},
    {XKB_KEY_Break, 0, 0, nss_sa_break},
};

struct nss_window {
    xcb_window_t wid;
    xcb_pixmap_t pid;
    xcb_gcontext_t gc;
    xcb_render_picture_t pic;
    xcb_event_mask_t ev_mask;
    xcb_render_picture_t pen;

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


    /*     * Glyph encoding:
     *  0x0TUUUUUU, where
     *      * 0xT - font fase
     *      * 0xUUUUUU - unicode character
     */
    nss_font_t *font;
    xcb_render_glyphset_t gsid;
    xcb_render_pictformat_t pfglyph;
    int16_t char_width;
    int16_t char_depth;
    int16_t char_height;

    char *font_name;
    nss_term_t *term;
    int term_fd;

    struct nss_window *prev, *next;
};

struct nss_context {
    _Bool daemon_mode;
    xcb_connection_t *con;
    xcb_screen_t *screen;
    xcb_colormap_t mid;
    xcb_visualtype_t *vis;

    xcb_render_pictformat_t pfargb;
    xcb_render_pictformat_t pfalpha;

    xcb_atom_t atom_net_wm_pid;
    xcb_atom_t atom_net_wm_name;
    xcb_atom_t atom_net_wm_icon_name;
    xcb_atom_t atom_wm_delete_window;
    xcb_atom_t atom_wm_protocols;
    xcb_atom_t atom_utf8_string;

    struct xkb_context *xkb_ctx;
    struct xkb_state *xkb_state;
    struct xkb_keymap *xkb_keymap;

    int32_t xkb_core_kbd;
    uint8_t xkb_base_event;
    uint8_t xkb_base_err;

    struct pollfd *pfds;
    size_t pfdn;
    size_t pfdcap;
    nss_window_t *first;


    struct cell_desc {
        int16_t x;
        int16_t y;
        nss_color_t bg;
        nss_color_t fg;
        uint32_t glyph : 29;
        uint32_t wide : 1;
        uint32_t underlined : 1;
        uint32_t strikethrough : 1;
    } *cbuffer;
    size_t cbufsize;
    size_t cbufpos;

    uint8_t *buffer;
    size_t bufsize;
    size_t bufpos;
};

typedef struct nss_glyph_mesg {
    uint8_t len;
    uint8_t pad[3];
    int16_t dx, dy;
    uint8_t data[];
} nss_glyph_mesg_t;

struct nss_context con;

static _Bool check_void_cookie(xcb_void_cookie_t ck) {
    xcb_generic_error_t *err = xcb_request_check(con.con, ck);
    if (err) {
        warn("[X11 Error] major=%"PRIu8", minor=%"PRIu16", error=%"PRIu8, err->major_code, err->minor_code, err->error_code);
        return 1;
    }
    free(err);
    return 0;
}

static nss_window_t *window_for_xid(xcb_window_t xid) {
    for (nss_window_t *win = con.first; win; win = win->next)
        if (win->wid == xid) return win;
    warn("Window for xid not found");
    return NULL;
}
static nss_window_t *window_for_term_fd(int fd) {
    for (nss_window_t *win = con.first; win; win = win->next)
        if (win->term_fd == fd) return win;
    warn("Window for fd not found");
    return NULL;
}

static xcb_atom_t intern_atom(const char *atom) {
    xcb_atom_t at;
    xcb_generic_error_t *err;
    xcb_intern_atom_cookie_t c = xcb_intern_atom(con.con, 0, strlen(atom), atom);
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(con.con, c, &err);
    if (err) {
        warn("Can't intern atom: %s", atom);
        free(err);
    }
    at = reply->atom;
    free(reply);
    return at;
}

static _Bool update_keymap(void) {
    struct xkb_keymap *new_keymap = xkb_x11_keymap_new_from_device(con.xkb_ctx, con.con, con.xkb_core_kbd, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!new_keymap) {
        warn("Can't create XKB keymap");
        return 0;
    }
    struct xkb_state *new_state = xkb_x11_state_new_from_device(new_keymap, con.con, con.xkb_core_kbd);
    if (!new_state) {
        warn("Can't get window xkb state");
        return 0;
    }
    if (con.xkb_state)
        xkb_state_unref(con.xkb_state);
    if (con.xkb_keymap)
        xkb_keymap_unref(con.xkb_keymap);
    con.xkb_keymap = new_keymap;
    con.xkb_state = new_state;
    return 1;
}

static _Bool configure_xkb(void) {
    uint16_t xkb_min = 0, xkb_maj = 0;
    int res = xkb_x11_setup_xkb_extension(con.con, XKB_X11_MIN_MAJOR_XKB_VERSION,
                                          XKB_X11_MIN_MINOR_XKB_VERSION, XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS,
                                          &xkb_maj, &xkb_min, &con.xkb_base_event, &con.xkb_base_err);

    if (!res || xkb_maj < XKB_X11_MIN_MAJOR_XKB_VERSION) {
        warn("Can't get suitable XKB verion");
        return 0;
    }
    con.xkb_core_kbd = xkb_x11_get_core_keyboard_device_id(con.con);
    if (con.xkb_core_kbd == -1) {
        warn("Can't get core keyboard device");
        return 0;
    }

    con.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!con.xkb_ctx) {
        warn("Can't create XKB context");
        return 0;
    }

    con.xkb_keymap = xkb_x11_keymap_new_from_device(con.xkb_ctx, con.con, con.xkb_core_kbd, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!con.xkb_keymap) {
        warn("Can't create XKB keymap");
        goto cleanup_context;
    }
    con.xkb_state = xkb_x11_state_new_from_device(con.xkb_keymap, con.con, con.xkb_core_kbd);
    if (!con.xkb_state) {
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
    c = xcb_xkb_select_events_aux_checked(con.con, con.xkb_core_kbd, events,
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
    xkb_state_unref(con.xkb_state);
cleanup_keymap:
    xkb_keymap_unref(con.xkb_keymap);
cleanup_context:
    xkb_context_unref(con.xkb_ctx);
    return 0;
}

#define NSS_CLASS "Nsst"
#define OPT_NAME_MAX 256
void load_params(void) {
    long dpi = -1;
    char name[OPT_NAME_MAX];
    xcb_xrm_database_t *xrmdb = xcb_xrm_database_from_default(con.con);
    if (xrmdb) {
        xcb_xrm_resource_get_long(xrmdb, NSS_CLASS".dpi", NULL, &dpi);

        for (unsigned j = 0; j < NSS_PALETTE_SIZE - NSS_SPECIAL_COLORS; j++) {
            snprintf(name, OPT_NAME_MAX, NSS_CLASS".color%u", j);
            char *res = NULL;
            if (!xcb_xrm_resource_get_string(xrmdb, name, NULL, &res)) {
                nss_color_t col = parse_color((uint8_t*)res, (uint8_t*)res + strlen(res));
                if (col) {
                    nss_config_set_color(NSS_CCONFIG_COLOR_0 + j, col);
                }
                free(res);
            }
        }

        static const char *snames[4] = {"background", "foreground", "cursorBackground", "cursorForeground"};

        for (size_t j = 0; j < sizeof(snames)/sizeof(snames[0]); j++) {
            snprintf(name, OPT_NAME_MAX, NSS_CLASS".%s", snames[j]);
            char *res = NULL;
            if (!xcb_xrm_resource_get_string(xrmdb, name, NULL, &res)) {
                nss_color_t col = parse_color((uint8_t*)res, (uint8_t*)res + strlen(res));
                if (!j) {
                    //Backround color preserves alpha
                    col &= 0xFFFFFF;
                    col |= nss_config_color(NSS_CCONFIG_BG) & 0xFF000000;
                }
                if (col) {
                    nss_config_set_color(NSS_CCONFIG_BG + j, col);
                }
                free(res);
            }
        }

        long res;
        if (!xcb_xrm_resource_get_long(xrmdb, NSS_CLASS".alpha", NULL, &res)) {
            nss_color_t col = nss_config_color(NSS_CCONFIG_BG);
            col &= 0xFFFFFF;
            col |= MAX(0, MIN(res, 255)) << 24;

            nss_config_set_color(NSS_CCONFIG_BG, col);
        }

        static const struct optmap_item {
        const char *name;
        enum nss_config_opt opt;
        } map[] = {
            {"allowAlternate", NSS_ICONFIG_ALLOW_ALTSCREEN},
            {"allowCharsets", NSS_ICONFIG_ALLOW_CHARSETS},
            {"allowNRCSs", NSS_ICONFIG_ALLOW_NRCS},
            {"answerbackString", NSS_SCONFIG_ANSWERBACK_STRING},
            {"appcursor", NSS_ICONFIG_INPUT_APPCURSOR},
            {"appkey", NSS_ICONFIG_INPUT_APPKEY},
            {"backspaceIsDelete", NSS_ICONFIG_INPUT_BACKSPACE_IS_DELETE},
            {"blinkTime",NSS_ICONFIG_BLINK_TIME},
            {"cursorShape", NSS_ICONFIG_CURSOR_SHAPE},
            {"cursorWidth",NSS_ICONFIG_CURSOR_WIDTH},
            {"deleteIsDelete", NSS_ICONFIG_INPUT_DELETE_IS_DELETE},
            {"dpi",NSS_ICONFIG_DPI},
            {"enableAutowrap", NSS_ICONFIG_INIT_WRAP},
            {"enableReverseVideo", NSS_ICONFIG_REVERSE_VIDEO},
            {"fkeyIncrement", NSS_ICONFIG_INPUT_FKEY_INCREMENT},
            {"font", NSS_SCONFIG_FONT_NAME},
            {"fontGamma", NSS_ICONFIG_GAMMA},
            {"fontSize", NSS_ICONFIG_FONT_SIZE},
            {"fontSizeStep", NSS_ICONFIG_FONT_SIZE_STEP},
            {"fontSpacing", NSS_ICONFIG_FONT_SPACING},
            {"fontSubpixel",NSS_ICONFIG_SUBPIXEL_FONTS},
            {"fps", NSS_ICONFIG_FPS},
            {"hasMeta", NSS_ICONFIG_INPUT_HAS_META},
            {"horizontalBorder",NSS_ICONFIG_TOP_BORDER},
            {"keyboardDialect", NSS_ICONFIG_KEYBOARD_NRCS},
            {"keyboardMapping", NSS_ICONFIG_INPUT_MAPPING},
            {"lineSpacing", NSS_ICONFIG_LINE_SPACING},
            {"lockKeyboard", NSS_ICONFIG_INPUT_LOCK},
            {"metaSendsEscape", NSS_ICONFIG_INPUT_META_IS_ESC},
            {"modifyCursor", NSS_ICONFIG_INPUT_MODIFY_CURSOR},
            {"modifyFunction", NSS_ICONFIG_INPUT_MODIFY_FUNCTION},
            {"modifyKeypad", NSS_ICONFIG_INPUT_MODIFY_KEYPAD},
            {"modifyOther", NSS_ICONFIG_INPUT_MODIFY_OTHER},
            {"modifyOtherFmt", NSS_ICONFIG_INPUT_MODIFY_OTHER_FMT},
            {"modkeyAllowEditKeypad", NSS_ICONFIG_INPUT_MALLOW_EDIT},
            {"modkeyAllowFunction", NSS_ICONFIG_INPUT_MALLOW_FUNCTION},
            {"modkeyAllowKeypad", NSS_ICONFIG_INPUT_MALLOW_KEYPAD},
            {"modkeyAllowMisc", NSS_ICONFIG_INPUT_MALLOW_MISC},
            {"numlock", NSS_ICONFIG_INPUT_NUMLOCK},
#ifdef USE_BOXDRAWING
            {"overrideBoxdrawing", NSS_ICONFIG_OVERRIDE_BOXDRAW},
#endif
            {"printer", NSS_SCONFIG_PRINTER},
            {"scrollAmout", NSS_ICONFIG_SCROLL_AMOUNT},
            {"scrollOnInput", NSS_ICONFIG_SCROLL_ON_INPUT},
            {"scrollOnOutput", NSS_ICONFIG_SCROLL_ON_OUTPUT},
            {"scrollbackSize", NSS_ICONFIG_HISTORY_LINES},
            {"shell", NSS_SCONFIG_SHELL},
            {"tabWidth", NSS_ICONFIG_TAB_WIDTH},
            {"termName", NSS_SCONFIG_TERM_NAME},
            {"title", NSS_SCONFIG_TITLE},
            {"underlineWidth",NSS_ICONFIG_UNDERLINE_WIDTH},
            {"useUtf8", NSS_ICONFIG_UTF8},
            {"verticalBorder",NSS_ICONFIG_LEFT_BORDER},
            {"vtVersion", NSS_ICONFIG_VT_VERION},
            {"windowClass", NSS_SCONFIG_TERM_CLASS},
        };
        for(size_t i = 0; i < sizeof(map)/sizeof(*map); i++) {
            snprintf(name, OPT_NAME_MAX, NSS_CLASS".%s", map[i].name);
            char *res = NULL;
            if (!xcb_xrm_resource_get_string(xrmdb, name, NULL, &res)) {
                nss_config_set_string(map[i].opt, res);
            }
            if (res) free(res);
        }

        xcb_xrm_database_free(xrmdb);
    }
    if (dpi <= 0) {
        warn("Can't fetch Xft.dpi, defaulting to highest dpi value");

        xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(con.con));
        for (; it.rem; xcb_screen_next(&it))
            if (it.data) dpi = MAX(dpi, (it.data->width_in_pixels * 25.4)/it.data->width_in_millimeters);
    }
    if (dpi > 0) nss_config_set_integer(NSS_ICONFIG_DPI, dpi);
}


volatile sig_atomic_t reload_config;

void handle_sigusr1(int sig) {
    reload_config = 1;
}

/* Initialize global state object */
void nss_init_context(void) {
    con.daemon_mode = 0;

    con.buffer = malloc(WORDS_IN_MESSAGE * sizeof(uint32_t));
    if (!con.buffer) die("Can't allocate buffer");
    con.bufsize = WORDS_IN_MESSAGE * sizeof(uint32_t);
    con.cbuffer = malloc(128 * sizeof(con.cbuffer[0]));
    if (!con.cbuffer) die("Can't allocate cbuffer");
    con.cbufsize = 128;

    con.pfds = calloc(INIT_PFD_NUM, sizeof(struct pollfd));
    if (!con.pfds) die("Can't allocate pfds");
    con.pfdn = 1;
    con.pfdcap = INIT_PFD_NUM;
    for (size_t i = 1; i < INIT_PFD_NUM; i++)
        con.pfds[i].fd = -1;

    int screenp;
    con.con = xcb_connect(NULL, &screenp);
    con.pfds[0].events = POLLIN | POLLHUP;
    con.pfds[0].fd = xcb_get_file_descriptor(con.con);

    xcb_screen_iterator_t sit = xcb_setup_roots_iterator(xcb_get_setup(con.con));
    for (; sit.rem; xcb_screen_next(&sit))
        if (screenp-- == 0)break;
    if (screenp != -1) {
        xcb_disconnect(con.con);
        die("Can't find default screen");
    }
    con.screen = sit.data;

    xcb_depth_iterator_t dit = xcb_screen_allowed_depths_iterator(con.screen);
    for (; dit.rem; xcb_depth_next(&dit))
        if (dit.data->depth == TRUE_COLOR_ALPHA_DEPTH) break;
    if (dit.data->depth != TRUE_COLOR_ALPHA_DEPTH) {
        xcb_disconnect(con.con);
        die("Can't get 32-bit visual");
    }

    xcb_visualtype_iterator_t vit = xcb_depth_visuals_iterator(dit.data);
    for (; vit.rem; xcb_visualtype_next(&vit))
        if (vit.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR) break;

    if (vit.data->_class != XCB_VISUAL_CLASS_TRUE_COLOR) {
        xcb_disconnect(con.con);
        die("Can't get 32-bit visual");
    }

    con.vis = vit.data;

    con.mid = xcb_generate_id(con.con);
    xcb_void_cookie_t c = xcb_create_colormap_checked(con.con, XCB_COLORMAP_ALLOC_NONE,
                                       con.mid, con.screen->root, con.vis->visual_id);
    if (check_void_cookie(c)) {
        xcb_disconnect(con.con);
        die("Can't create colormap");
    }

    // Check if XRender is present
    xcb_render_query_version_cookie_t vc = xcb_render_query_version(con.con, XCB_RENDER_MAJOR_VERSION, XCB_RENDER_MINOR_VERSION);
    xcb_generic_error_t *err;
    xcb_render_query_version_reply_t *rep = xcb_render_query_version_reply(con.con, vc, &err);
    // Any version is OK, so don't check
    free(rep);

    if (err) {
        uint8_t erc = err->error_code;
        xcb_disconnect(con.con);
        die("XRender not detected: %"PRIu8, erc);
    }


    xcb_render_query_pict_formats_cookie_t pfc = xcb_render_query_pict_formats(con.con);
    xcb_render_query_pict_formats_reply_t *pfr = xcb_render_query_pict_formats_reply(con.con, pfc, &err);

    if (err) {
        uint8_t erc = err->error_code;
        xcb_disconnect(con.con);
        die("Can't query picture formats: %"PRIu8, erc);
    }

    xcb_render_pictforminfo_iterator_t pfit =  xcb_render_query_pict_formats_formats_iterator(pfr);
    for (; pfit.rem; xcb_render_pictforminfo_next(&pfit)) {
        if (pfit.data->depth == TRUE_COLOR_ALPHA_DEPTH && pfit.data->type == XCB_RENDER_PICT_TYPE_DIRECT &&
           pfit.data->direct.red_mask == 0xff && pfit.data->direct.green_mask == 0xff &&
           pfit.data->direct.blue_mask == 0xff && pfit.data->direct.alpha_mask == 0xff &&
           pfit.data->direct.red_shift == 16 && pfit.data->direct.green_shift == 8 &&
           pfit.data->direct.blue_shift == 0 && pfit.data->direct.alpha_shift == 24 ) {
               con.pfargb = pfit.data->id;
        }
        if (pfit.data->depth == 8 && pfit.data->type == XCB_RENDER_PICT_TYPE_DIRECT &&
           pfit.data->direct.alpha_mask == 0xff && pfit.data->direct.alpha_shift == 0) {
               con.pfalpha = pfit.data->id;
        }
    }

    free(pfr);

    if (con.pfargb == 0 || con.pfalpha == 0) {
        xcb_disconnect(con.con);
        die("Can't find suitable picture format");
    }

    if (!configure_xkb()) {
        xcb_disconnect(con.con);
        die("Can't configure XKB");
    }


    con.atom_net_wm_pid = intern_atom("_NET_WM_PID");
    con.atom_wm_delete_window = intern_atom("WM_DELETE_WINDOW");
    con.atom_wm_protocols = intern_atom("WM_PROTOCOLS");
    con.atom_utf8_string = intern_atom("UTF8_STRING");
    con.atom_net_wm_name = intern_atom("_NET_WM_NAME");
    con.atom_net_wm_icon_name = intern_atom("_NET_WM_ICON_NAME");

    if (!nss_config_integer(NSS_ICONFIG_SKIP_CONFIG_FILE)) load_params();
    else nss_config_set_integer(NSS_ICONFIG_SKIP_CONFIG_FILE, 0);

    sigaction(SIGUSR1, &(struct sigaction){ .sa_handler = handle_sigusr1, .sa_flags = SA_RESTART}, NULL);
}

void nss_window_set_title(nss_window_t *win, const char *title) {
    if (!title) title = nss_config_string(NSS_SCONFIG_TITLE);
    xcb_change_property(con.con, XCB_PROP_MODE_REPLACE, win->wid, XCB_ATOM_WM_NAME, con.atom_utf8_string, 8, strlen(title), title);
    xcb_change_property(con.con, XCB_PROP_MODE_REPLACE, win->wid, con.atom_net_wm_name, con.atom_utf8_string, 8, strlen(title), title);
}

void nss_window_set_icon_name(nss_window_t *win, const char *title) {
    if (!title) title = nss_config_string(NSS_SCONFIG_TITLE);
    xcb_change_property(con.con, XCB_PROP_MODE_REPLACE, win->wid, XCB_ATOM_WM_ICON_NAME, con.atom_utf8_string, 8, strlen(title), title);
    xcb_change_property(con.con, XCB_PROP_MODE_REPLACE, win->wid, con.atom_net_wm_icon_name, con.atom_utf8_string, 8, strlen(title), title);
}


/* Free all resources */
void nss_free_context(void) {
    while (con.first)
        nss_free_window(con.first);
    xkb_state_unref(con.xkb_state);
    xkb_keymap_unref(con.xkb_keymap);
    xkb_context_unref(con.xkb_ctx);

    free(con.buffer);
    free(con.cbuffer);
    free(con.pfds);

    xcb_disconnect(con.con);
    memset(&con, 0, sizeof(con));
}

static void register_glyph(nss_window_t *win, uint32_t ch, nss_glyph_t * glyph) {
    xcb_render_glyphinfo_t spec = {
        .width = glyph->width, .height = glyph->height,
        .x = glyph->x, .y = glyph->y,
        .x_off = glyph->x_off, .y_off = glyph->y_off
    };
    xcb_void_cookie_t c;
    c = xcb_render_add_glyphs_checked(con.con, win->gsid, 1, &ch, &spec, glyph->height*glyph->stride, glyph->data);
    if (check_void_cookie(c))
        warn("Can't add glyph");
}

static void set_config(nss_window_t *win, nss_wc_tag_t tag, const uint32_t *values) {
    if (tag & nss_wc_cusror_width) win->cursor_width = *values++;
    if (tag & nss_wc_left_border) win->left_border = *values++;
    if (tag & nss_wc_top_border) win->top_border = *values++;
    if (tag & nss_wc_background) win->bg = *values++;
    if (tag & nss_wc_cursor_foreground) win->cursor_fg = *values++;
    if (tag & nss_wc_cursor_type) win->cursor_type = *values++;
    if (tag & nss_wc_subpixel_fonts) win->subpixel_fonts = *values++;
    if (tag & nss_wc_font_size) win->font_size = *values++;
    if (tag & nss_wc_underline_width) win->underline_width = *values++;
    if (tag & nss_wc_width) warn("Tag is not settable"), values++;
    if (tag & nss_wc_height) warn("Tag is not settable"), values++;
    if (tag & nss_wc_blink_time) win->blink_time = *values++;
    if (tag & nss_wc_mouse) win->mouse_events = *values++;
}

/* Reload font using win->font_size and win->font_name */
static _Bool reload_font(nss_window_t *win, _Bool need_free) {
    //Try find already existing font
    _Bool found_font = 0, found_gset = 0;
    nss_window_t *found = 0;
    for (nss_window_t *src = con.first; src; src = src->next) {
        if ((src->font_size == win->font_size || win->font_size == 0) &&
           !strcmp(win->font_name, src->font_name) && src != win) {
            found_font = 1;
            found = src;
            if (src->subpixel_fonts == win->subpixel_fonts) {
                found_gset = 1;
                break;
            }
        }
    }

    nss_font_t *new = found_font ? nss_font_reference(found->font) :
        nss_create_font(win->font_name, win->font_size, nss_config_integer(NSS_ICONFIG_DPI));
    if (!new) {
        warn("Can't create new font: %s", win->font_name);
        return 0;
    }

    if (need_free) nss_free_font(win->font);

    win->font = new;
    win->font_size = nss_font_get_size(new);
    win->pfglyph = win->subpixel_fonts ? con.pfargb : con.pfalpha;

    xcb_void_cookie_t c;

    if (need_free) {
        c = xcb_render_free_glyph_set_checked(con.con, win->gsid);
        if (check_void_cookie(c))
            warn("Can't free glyph set");
    }
    else win->gsid = xcb_generate_id(con.con);

    if (found_gset) {
        c = xcb_render_reference_glyph_set_checked(con.con, win->gsid, found->gsid);
        if (check_void_cookie(c))
            warn("Can't reference glyph set");

        win->char_height = found->char_height;
        win->char_depth = found->char_depth;
        win->char_width = found->char_width;
    } else {
        c = xcb_render_create_glyph_set_checked(con.con, win->gsid, win->pfglyph);
        if (check_void_cookie(c))
            warn("Can't create glyph set");

        //Preload ASCII
        nss_glyph_t *glyphs['~' - ' ' + 1][nss_font_attrib_max] = {{ NULL }};
        int16_t total = 0, maxd = 0, maxh = 0;
        for (tchar_t i = ' '; i <= '~'; i++) {
            for (size_t j = 0; j < nss_font_attrib_max; j++)
                glyphs[i - ' '][j] = nss_font_render_glyph(win->font, i, j, win->subpixel_fonts);

            total += glyphs[i - ' '][0]->x_off;
            maxd = MAX(maxd, glyphs[i - ' '][0]->height - glyphs[i - ' '][0]->y);
            maxh = MAX(maxh, glyphs[i - ' '][0]->y);
        }

        win->char_width = total / ('~' - ' ' + 1) + nss_config_integer(NSS_ICONFIG_FONT_SPACING);
        win->char_height = maxh;
        win->char_depth = maxd + nss_config_integer(NSS_ICONFIG_LINE_SPACING);

        for (tchar_t i = ' '; i <= '~'; i++) {
            for (size_t j = 0; j < nss_font_attrib_max; j++) {
                glyphs[i - ' '][j]->x_off = win->char_width;
                register_glyph(win, i | (j << 24), glyphs[i - ' '][j]);
                free(glyphs[i - ' '][j]);
            }
        }

    }

    win->cw = MAX(1, (win->width - 2*win->left_border) / win->char_width);
    win->ch = MAX(1, (win->height - 2*win->top_border) / (win->char_height + win->char_depth));

    xcb_rectangle_t bound = { 0, 0, win->cw*win->char_width, win->ch*(win->char_depth+win->char_height) };

    if (need_free) {
        xcb_free_pixmap(con.con, win->pid);
        xcb_free_gc(con.con, win->gc);
        xcb_render_free_picture(con.con, win->pic);
    } else {
        win->pid = xcb_generate_id(con.con);
        win->gc = xcb_generate_id(con.con);
        win->pic = xcb_generate_id(con.con);
    }

    c = xcb_create_pixmap_checked(con.con, TRUE_COLOR_ALPHA_DEPTH, win->pid, win->wid, bound.width, bound.height );
    if (check_void_cookie(c)) {
        warn("Can't create pixmap");
        return 0;
    }

    uint32_t mask2 = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_GRAPHICS_EXPOSURES;
    uint32_t values2[3] = { win->bg, win->bg, 0 };
    c = xcb_create_gc_checked(con.con, win->gc, win->pid, mask2, values2);
    if (check_void_cookie(c)) {
        warn("Can't create GC");
        return 0;
    }

    uint32_t mask3 = XCB_RENDER_CP_GRAPHICS_EXPOSURE | XCB_RENDER_CP_POLY_EDGE | XCB_RENDER_CP_POLY_MODE;
    uint32_t values3[3] = { 0, XCB_RENDER_POLY_EDGE_SMOOTH, XCB_RENDER_POLY_MODE_IMPRECISE };
    c = xcb_render_create_picture_checked(con.con, win->pic, win->pid, con.pfargb, mask3, values3);
    if (check_void_cookie(c)) {
        warn("Can't create XRender picture");
        return 0;
    }

    xcb_render_color_t color = MAKE_COLOR(win->bg);
    xcb_render_fill_rectangles(con.con, XCB_RENDER_PICT_OP_SRC, win->pic, color, 1, &bound);

    if (need_free)
        nss_term_resize(win->term, win->cw, win->ch);
    return 1;
}

static void set_wm_props(nss_window_t *win) {
    uint32_t pid = getpid();
    xcb_change_property(con.con, XCB_PROP_MODE_REPLACE, win->wid, con.atom_net_wm_pid, XCB_ATOM_CARDINAL, 32, 1, &pid);
    xcb_change_property(con.con, XCB_PROP_MODE_REPLACE, win->wid, con.atom_wm_protocols, XCB_ATOM_ATOM, 32, 1, &con.atom_wm_delete_window);
    const char class[] = "Nsst", *extra;
    xcb_change_property(con.con, XCB_PROP_MODE_REPLACE, win->wid, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, sizeof(class), class);
    if ((extra = nss_config_string(NSS_SCONFIG_TERM_CLASS)))
        xcb_change_property(con.con, XCB_PROP_MODE_APPEND, win->wid, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, strlen(extra), extra);
}

/* Create new window */
nss_window_t *nss_create_window(const char *font_name, nss_wc_tag_t tag, const uint32_t *values) {
    nss_window_t *win = calloc(1, sizeof(nss_window_t));
    win->cursor_width = nss_config_integer(NSS_ICONFIG_CURSOR_WIDTH);
    win->underline_width = nss_config_integer(NSS_ICONFIG_UNDERLINE_WIDTH);
    win->left_border = nss_config_integer(NSS_ICONFIG_LEFT_BORDER);
    win->top_border = nss_config_integer(NSS_ICONFIG_TOP_BORDER);
    win->bg = nss_config_color(NSS_CCONFIG_BG);
    win->cursor_fg = nss_config_color(NSS_CCONFIG_CURSOR_FG);
    win->cursor_type = nss_config_integer(NSS_ICONFIG_CURSOR_SHAPE);
    win->subpixel_fonts = nss_config_integer(NSS_ICONFIG_SUBPIXEL_FONTS);
    win->font_size = nss_config_integer(NSS_ICONFIG_FONT_SIZE);
    win->active = 1;
    win->focused = 1;

    win->term_fd = -1;
    win->blink_time = nss_config_integer(NSS_ICONFIG_BLINK_TIME);
    if (!font_name) font_name = nss_config_string(NSS_SCONFIG_FONT_NAME);
    win->font_name = strdup(font_name);
    if (!win->font_name) {
        nss_free_window(win);
        return NULL;
    }
    win->width = nss_config_integer(NSS_ICONFIG_WINDOW_WIDTH);
    win->height = nss_config_integer(NSS_ICONFIG_WINDOW_HEIGHT);

    set_config(win, tag, values);

    xcb_void_cookie_t c;

    uint32_t mask1 =  XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL |
        XCB_CW_BIT_GRAVITY | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
    win->ev_mask = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_VISIBILITY_CHANGE |
        XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
        XCB_EVENT_MASK_BUTTON_MOTION | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE;
    if (win->mouse_events) win->ev_mask |= XCB_EVENT_MASK_POINTER_MOTION;
    uint32_t values1[5] = { win->bg, win->bg, XCB_GRAVITY_NORTH_WEST, win->ev_mask, con.mid };
    int16_t x = nss_config_integer(NSS_ICONFIG_WINDOW_X);
    int16_t y = nss_config_integer(NSS_ICONFIG_WINDOW_Y);

    // Adjust geometry
    if (nss_config_integer(NSS_ICONFIG_WINDOW_NEGATIVE_X))
        x += con.screen->width_in_pixels - win->width - 2;
    if (nss_config_integer(NSS_ICONFIG_WINDOW_NEGATIVE_Y))
        y += con.screen->height_in_pixels - win->height - 2;

    win->wid = xcb_generate_id(con.con);
    c = xcb_create_window_checked(con.con, TRUE_COLOR_ALPHA_DEPTH, win->wid, con.screen->root,
                                  x, y, win->width, win->height, 0,
                                  XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                  con.vis->visual_id, mask1, values1);
    if (check_void_cookie(c)) {
        warn("Can't create window");
        nss_free_window(win);
        return NULL;
    }

    set_wm_props(win);
    nss_window_set_title(win, NULL);

    if (!reload_font(win, 0)) {
        warn("Can't create window");
        nss_free_window(win);
        return NULL;
    }

    win->next = con.first;
    win->prev = NULL;
    if (con.first) con.first->prev = win;
    con.first = win;


    xcb_pixmap_t pid = xcb_generate_id(con.con);
    c = xcb_create_pixmap_checked(con.con, TRUE_COLOR_ALPHA_DEPTH, pid, win->wid, 1, 1);
    if (check_void_cookie(c)) {
        warn("Can't create pixmap");
        nss_free_window(win);
        return NULL;
    }

    win->pen = xcb_generate_id(con.con);
    uint32_t values4[1] = { XCB_RENDER_REPEAT_NORMAL };
    c = xcb_render_create_picture_checked(con.con, win->pen, pid, con.pfargb, XCB_RENDER_CP_REPEAT, values4);
    if (check_void_cookie(c)) {
        warn("Can't create picture");
        nss_free_window(win);
        return NULL;
    }

    xcb_map_window(con.con, win->wid);

    xcb_free_pixmap(con.con, pid);

    if (con.pfdn + 1 > con.pfdcap) {
        struct pollfd *new = realloc(con.pfds, con.pfdcap + INIT_PFD_NUM);
        if (new) {
            for (size_t i = 0; i < INIT_PFD_NUM; i++) {
                new[i + con.pfdn].fd = -1;
                new[i + con.pfdn].events = 0;
            }
            con.pfdcap += INIT_PFD_NUM;
            con.pfds = new;
        } else {
            warn("Can't reallocate con.pfds");
            nss_free_window(win);
            return NULL;
        }
    }
    win->term = nss_create_term(win, win->cw, win->ch);
    if (!win->term) {
        warn("Can't create term");
        nss_free_window(win);
        return NULL;
    }

    con.pfdn++;
    size_t i = 1;
    while (con.pfds[i].fd >= 0) i++;
    // Because it might become -1 suddenly
    win->term_fd = nss_term_fd(win->term);
    con.pfds[i].events = POLLIN | POLLHUP;
    con.pfds[i].fd = win->term_fd;

    xcb_flush(con.con);
    return win;
}

/* Free previously created windows */
void nss_free_window(nss_window_t *win) {
    if (win->wid) {
        xcb_unmap_window(con.con, win->wid);
        xcb_render_free_picture(con.con, win->pen);
        xcb_render_free_picture(con.con, win->pic);
        xcb_free_gc(con.con, win->gc);
        xcb_free_pixmap(con.con, win->pid);
        xcb_render_free_glyph_set(con.con, win->gsid);
        xcb_destroy_window(con.con, win->wid);
        xcb_flush(con.con);
    }

    if (win->next)win->next->prev = win->prev;
    if (win->prev)win->prev->next = win->next;
    else con.first =  win->next;

    if (win->term_fd > 0) {
        size_t i = 0;
        while (con.pfds[i].fd != win->term_fd && i < con.pfdcap) i++;
        if (i < con.pfdcap)
            con.pfds[i].fd = -1;
        else warn("Window fd not found");
        con.pfdn--;
    }

    if (win->term)
        nss_free_term(win->term);
    if (win->font)
        nss_free_font(win->font);

    free(win->font_name);
    free(win);
};

static void push_cell(nss_window_t *win, coord_t x, coord_t y, nss_color_t *palette, nss_color_t *extra, nss_cell_t *cel) {
    nss_cell_t cell = *cel;

    if (!nss_font_glyph_is_loaded(win->font, cell.ch)) {
        for (size_t j = 0; j < nss_font_attrib_max; j++) {
            nss_glyph_t *glyph;
#ifdef USE_BOXDRAWING
            if (is_boxdraw(cell.ch) && nss_config_integer(NSS_ICONFIG_OVERRIDE_BOXDRAW)) {
                glyph = nss_make_boxdraw(cell.ch, win->char_width, win->char_height, win->char_depth, win->subpixel_fonts);
                nss_font_glyph_mark_loaded(win->font, cell.ch | (j << 24));
            } else
#endif
                glyph = nss_font_render_glyph(win->font, cell.ch, j, win->subpixel_fonts);
            //In case of non-monospace fonts
            glyph->x_off = win->char_width;
            register_glyph(win, cell.ch | (j << 24) , glyph);
            free(glyph);
        }
    }

    if ((cell.attr & (nss_attrib_bold | nss_attrib_faint)) == nss_attrib_bold && cell.fg < 8) cell.fg += 8;
    nss_color_t bg = cell.bg < NSS_PALETTE_SIZE ? palette[cell.bg] : extra[cell.bg - NSS_PALETTE_SIZE];
    nss_color_t fg = cell.fg < NSS_PALETTE_SIZE ? palette[cell.fg] : extra[cell.fg - NSS_PALETTE_SIZE];
    if ((cell.attr & (nss_attrib_bold | nss_attrib_faint)) == nss_attrib_faint)
        fg = (fg & 0xFF000000) | ((fg & 0xFEFEFE) >> 1);
    if (cell.attr & nss_attrib_inverse) SWAP(nss_color_t, fg, bg);
    if (cell.attr & nss_attrib_invisible || (cell.attr & nss_attrib_blink && win->blink_state)) fg = bg;

    if (2*(con.cbufpos + 1) >= con.cbufsize) {
        size_t new_size = MAX(3 * con.cbufsize / 2, 2 * con.cbufpos + 1);
        struct cell_desc *new = realloc(con.cbuffer, new_size * sizeof(*con.cbuffer));
        if (!new) return;
        con.cbuffer = new;
        con.cbufsize = new_size;
    }

    // U+2588 FULL BLOCK
    if (cell.ch == 0x2588) bg = fg;
    if (cell.ch == ' ' || fg == bg) cell.ch = 0;
    con.cbuffer[con.cbufpos++] = (struct cell_desc) {
        .x = x * win->char_width,
        .y = y * (win->char_height + win->char_depth),
        .fg = fg, .bg = bg,
        .glyph = cell.ch ? cell.ch | ((cell.attr & nss_font_attrib_mask) << 24) : 0,
        .wide = !!(cell.attr & nss_attrib_wide),
        .underlined = !!(cell.attr & nss_attrib_underlined) && (fg != bg),
        .strikethrough = !!(cell.attr & nss_attrib_strikethrough) && (fg != bg),
    };

    cel->attr |= nss_attrib_drawn;
}

static void push_rect(nss_window_t *win, xcb_rectangle_t *rect) {
    if (con.bufpos + sizeof(xcb_rectangle_t) >= con.bufsize) {
        size_t new_size = MAX(3 * con.bufsize / 2, 16 * sizeof(xcb_rectangle_t));
        uint8_t *new = realloc(con.buffer, new_size);
        if (!new) return;
        con.buffer = new;
        con.bufsize = new_size;
    }

    memcpy(con.buffer + con.bufpos, rect, sizeof(xcb_rectangle_t));
    con.bufpos += sizeof(xcb_rectangle_t);
}


// Use custom shell sort implementation, sice it works faster

static inline _Bool cmp_bg(const struct cell_desc *ad, const struct cell_desc *bd) {
    if (ad->bg < bd->bg) return 1;
    if (ad->bg > bd->bg) return 0;
    if (ad->y < bd->y) return 1;
    if (ad->y > bd->y) return 0;
    if (ad->x < bd->x) return 1;
    return 0;
}

static inline _Bool cmp_fg(const struct cell_desc *ad, const struct cell_desc *bd) {
    if (ad->fg < bd->fg) return 1;
    if (ad->fg > bd->fg) return 0;
    if (ad->y < bd->y) return 1;
    if (ad->y > bd->y) return 0;
    if (ad->x < bd->x) return 1;
    return 0;
}

/*
static inline void shell_sort_bg(struct cell_desc *array, size_t size) {
    size_t hmax = size/9;
    size_t h;
    for(h = 1; h <= hmax; h = 3*h+1);
    for(; h > 0; h /= 3) {
        for(size_t i = h; i < size; ++i) {
            const struct cell_desc v = array[i];
            size_t j = i;
            while(j >= h && cmp_bg(&v, &array[j-h])) {
                array[j] = array[j-h];
                j -= h;
            }
            array[j] = v;
        }
    }
}

static inline void shell_sort_fg(struct cell_desc *array, size_t size) {
    size_t hmax = size/9;
    size_t h;
    for(h = 1; h <= hmax; h = 3*h+1);
    for(; h > 0; h /= 3) {
        for(size_t i = h; i < size; ++i) {
            const struct cell_desc v = array[i];
            size_t j = i;
            while(j >= h && cmp_fg(&v, &array[j-h])) {
                array[j] = array[j-h];
                j -= h;
            }
            array[j] = v;
        }
    }
}
*/

static inline void merge_sort_fg(struct cell_desc *src, size_t size) {
    struct cell_desc *dst = src + size;
    for (size_t k = 2; k < size; k += k) {
        for (size_t i = 0; i < size; ) {
            size_t l_1 = i, h_1 = MIN(i + k/2, size);
            size_t l_2 = h_1, h_2 = MIN(i + k, size);
            while (l_1 < h_1 && l_2 < h_2)
                dst[i++] = src[cmp_fg(&src[l_1], &src[l_2]) ? l_1++ : l_2++];
            while (l_1 < h_1) dst[i++] = src[l_1++];
            while (l_2 < h_2) dst[i++] = src[l_2++];
        }
        SWAP(struct cell_desc *, dst, src);
    }
    if (dst < src) for (size_t i = 0; i < size; i++)
        dst[i] = src[i];
}

static inline void merge_sort_bg(struct cell_desc *src, size_t size) {
    struct cell_desc *dst = src + size;
    for (size_t k = 2; k < size; k += k) {
        for (size_t i = 0; i < size; ) {
            size_t l_1 = i, h_1 = MIN(i + k/2, size);
            size_t l_2 = h_1, h_2 = MIN(i + k, size);
            while (l_1 < h_1 && l_2 < h_2)
                dst[i++] = src[cmp_bg(&src[l_1], &src[l_2]) ? l_1++ : l_2++];
            while (l_1 < h_1) dst[i++] = src[l_1++];
            while (l_2 < h_2) dst[i++] = src[l_2++];
        }
        SWAP(struct cell_desc *, dst, src);
    }
    if (dst < src) for (size_t i = 0; i < size; i++)
        dst[i] = src[i];
}

/* new method of rendering: whole screen in a time */
void nss_window_submit_screen(nss_window_t *win, nss_line_t *list, nss_line_t **array, nss_color_t *palette, coord_t cur_x, coord_t cur_y, _Bool cursor) {
    con.cbufpos = 0;
    con.bufpos = 0;

    _Bool marg = win->cw == cur_x;
    cur_x -= marg;
    if (cursor && win->focused) {
        nss_cell_t cur_cell = array[cur_y]->cell[cur_x - marg];
        if (win->cursor_type == nss_cursor_block)
            cur_cell.attr ^= nss_attrib_inverse;
        array[cur_y]->cell[cur_x].attr |= nss_attrib_drawn;
        push_cell(win, cur_x, cur_y, palette, array[cur_y]->extra, &cur_cell);
    }

    coord_t h = 0;
    for (; h < win->ch && list; list = list->next, h++) {
        if (win->cw > list->width) {
            push_rect(win, &(xcb_rectangle_t){
                .x = list->width * win->char_width,
                .y = h * (win->char_height + win->char_depth),
                .width = (win->cw - list->width) * win->char_width,
                .height = win->char_height + win->char_depth
            });
        }
        for (coord_t i = 0; i < MIN(win->cw, list->width); i++)
            if (!(list->cell[i].attr & nss_attrib_drawn) ||
                    (!win->blink_commited && (list->cell[i].attr & nss_attrib_blink)))
                push_cell(win, i, h, palette, list->extra, &list->cell[i]);
    }
    for (coord_t j = 0; j < win->ch - h; j++) {
        if (win->cw > array[j]->width) {
            push_rect(win, &(xcb_rectangle_t){
                .x = array[j]->width * win->char_width,
                .y = (j + h) * (win->char_height + win->char_depth),
                .width = (win->cw - array[j]->width) * win->char_width,
                .height = win->char_height + win->char_depth
            });
        }
        for (coord_t i = 0; i < MIN(win->cw, array[j]->width); i++)
            if (!(array[j]->cell[i].attr & nss_attrib_drawn) ||
                    (!win->blink_commited && (array[j]->cell[i].attr & nss_attrib_blink)))
                push_cell(win, i, j + h, palette, array[j]->extra, &array[j]->cell[i]);
    }

    if (con.bufpos) {
        xcb_render_color_t col = MAKE_COLOR(win->bg);
        xcb_render_fill_rectangles(con.con, XCB_RENDER_PICT_OP_SRC, win->pic, col,
            con.bufpos/sizeof(xcb_rectangle_t), (xcb_rectangle_t *)con.buffer);
    }

    //qsort(con.cbuffer, con.cbufpos, sizeof(con.cbuffer[0]), cmp_by_bg);
    //shell_sort_bg(con.cbuffer, con.cbufpos);
    merge_sort_bg(con.cbuffer, con.cbufpos);

    // Draw background
    for (size_t i = 0; i < con.cbufpos; ) {
        con.bufpos = 0;
        size_t j = i;
        while(i < con.cbufpos && con.cbuffer[i].bg == con.cbuffer[j].bg) {
            size_t k = i;
            do i++;
            while (i < con.cbufpos && con.cbuffer[k].y == con.cbuffer[i].y &&
                    con.cbuffer[i - 1].x + win->char_width == con.cbuffer[i].x &&
                    con.cbuffer[k].bg == con.cbuffer[i].bg);
            push_rect(win, &(xcb_rectangle_t) {
                .x = con.cbuffer[k].x,
                .y = con.cbuffer[k].y,
                .width = con.cbuffer[i - 1].x - con.cbuffer[k].x + win->char_width,
                .height = win->char_depth + win->char_height
            });

        }
        if (con.bufpos) {
            xcb_render_color_t col = MAKE_COLOR(con.cbuffer[j].bg);
            xcb_render_fill_rectangles(con.con, XCB_RENDER_PICT_OP_SRC, win->pic, col,
                con.bufpos/sizeof(xcb_rectangle_t), (xcb_rectangle_t *)con.buffer);
        }
    }

    // Set clip rectangles for text rendering
    con.bufpos = 0;
    for (size_t i = 0; i < con.cbufpos; ) {
        while (i < con.cbufpos && !con.cbuffer[i].glyph) i++;
        if (i >= con.cbufpos) break;
        size_t k = i;
        do i++;
        while (i < con.cbufpos && con.cbuffer[k].y == con.cbuffer[i].y &&
                con.cbuffer[i - 1].x + win->char_width == con.cbuffer[i].x && con.cbuffer[i].glyph);
        push_rect(win, &(xcb_rectangle_t) {
            .x = con.cbuffer[k].x,
            .y = con.cbuffer[k].y,
            .width = con.cbuffer[i - 1].x - con.cbuffer[k].x + win->char_width*(1 + con.cbuffer[k].wide),
            .height = win->char_depth + win->char_height
        });
    }
    if (con.bufpos)
        xcb_render_set_picture_clip_rectangles(con.con, win->pic, 0, 0,
            con.bufpos/sizeof(xcb_rectangle_t), (xcb_rectangle_t *)con.buffer);

    //qsort(con.cbuffer, con.cbufpos, sizeof(con.cbuffer[0]), cmp_by_fg);
    //shell_sort_fg(con.cbuffer, con.cbufpos);
    merge_sort_fg(con.cbuffer, con.cbufpos);

    // Draw chars
    for (size_t i = 0; i < con.cbufpos; ) {
        while (i < con.cbufpos && !con.cbuffer[i].glyph) i++;
        if (i >= con.cbufpos) break;

        xcb_render_color_t col = MAKE_COLOR(con.cbuffer[i].fg);
        xcb_rectangle_t rect2 = { .x = 0, .y = 0, .width = 1, .height = 1 };
        xcb_render_fill_rectangles(con.con, XCB_RENDER_PICT_OP_SRC, win->pen, col, 1, &rect2);

        con.bufpos = 0;
        int16_t ox = 0, oy = 0;
        size_t j = i;

        while(i < con.cbufpos && con.cbuffer[i].fg == con.cbuffer[j].fg) {
            if (con.bufpos + WORDS_IN_MESSAGE * sizeof(uint32_t) >= con.bufsize) {
                uint8_t *new = realloc(con.buffer, con.bufsize + WORDS_IN_MESSAGE * sizeof(uint32_t));
                if (!new) break;
                con.buffer = new;
                con.bufsize += WORDS_IN_MESSAGE * sizeof(uint32_t);
            }
            nss_glyph_mesg_t *head = (nss_glyph_mesg_t *)(con.buffer + con.bufpos);
            con.bufpos += sizeof(*head);
            size_t k = i;
            *head = (nss_glyph_mesg_t){
                .dx = con.cbuffer[k].x - ox,
                .dy = con.cbuffer[k].y + win->char_height - oy
            };
            do {
                uint32_t glyph = con.cbuffer[i].glyph;
                memcpy(con.buffer + con.bufpos, &glyph, sizeof(uint32_t));
                con.bufpos += sizeof(uint32_t);
                i++;
            } while (i < con.cbufpos && con.cbuffer[k].y == con.cbuffer[i].y &&
                    con.cbuffer[i - 1].x + win->char_width == con.cbuffer[i].x &&
                    con.cbuffer[k].fg == con.cbuffer[i].fg &&
                    con.cbuffer[i].glyph && i - k < CHARS_PER_MESG);
            head->len = i - k;

            ox = con.cbuffer[i - 1].x + win->char_width;
            oy = con.cbuffer[i - 1].y + win->char_height;

            while (i < con.cbufpos && !con.cbuffer[i].glyph) i++;
        }
        if (con.bufpos)
            xcb_render_composite_glyphs_32(con.con, XCB_RENDER_PICT_OP_OVER,
                                           win->pen, win->pic, win->pfglyph, win->gsid,
                                           0, 0, con.bufpos, con.buffer);
    }

    if (con.cbufpos)
        xcb_render_set_picture_clip_rectangles(con.con, win->pic, 0, 0, 1, &(xcb_rectangle_t){
                0, 0, win->cw * win->char_width, win->ch * (win->char_height + win->char_depth)});

    // Draw underline and strikethrough lines
    for (size_t i = 0; i < con.cbufpos; ) {
        while(i < con.cbufpos && !con.cbuffer[i].underlined && !con.cbuffer[i].strikethrough) i++;
        if (i >= con.cbufpos) break;
        con.bufpos = 0;
        size_t j = i;
        while (i < con.cbufpos && con.cbuffer[j].fg == con.cbuffer[i].fg) {
            while (i < con.cbufpos && con.cbuffer[j].fg == con.cbuffer[i].fg && !con.cbuffer[i].underlined) i++;
            if (i >= con.cbufpos || !con.cbuffer[i].underlined) break;
            size_t k = i;
            do i++;
            while (i < con.cbufpos && con.cbuffer[k].y == con.cbuffer[i].y &&
                    con.cbuffer[i - 1].x + win->char_width == con.cbuffer[i].x &&
                    con.cbuffer[k].fg == con.cbuffer[i].fg && con.cbuffer[i].underlined);
            push_rect(win, &(xcb_rectangle_t) {
                .x = con.cbuffer[k].x,
                .y = con.cbuffer[k].y + win->char_height + 1,
                .width = con.cbuffer[i - 1].x + win->char_width - con.cbuffer[k].x,
                .height = win->underline_width
            });
        }
        i = j;
        while (i < con.cbufpos && con.cbuffer[j].fg == con.cbuffer[i].fg) {
            while (i < con.cbufpos && con.cbuffer[j].fg == con.cbuffer[i].fg && !(con.cbuffer[i].strikethrough)) i++;
            if (i >= con.cbufpos || !con.cbuffer[i].strikethrough) break;
            size_t k = i;
            do i++;
            while (i < con.cbufpos && con.cbuffer[k].y == con.cbuffer[i].y &&
                    con.cbuffer[i - 1].x + win->char_width == con.cbuffer[i].x &&
                    con.cbuffer[k].fg == con.cbuffer[i].fg && con.cbuffer[i].strikethrough);
            push_rect(win, &(xcb_rectangle_t) {
                .x = con.cbuffer[k].x,
                .y = con.cbuffer[k].y + 2*win->char_height/3 - win->underline_width/2,
                .width = con.cbuffer[i - 1].x + win->char_width - con.cbuffer[k].x,
                .height = win->underline_width
            });
        }
        if (con.bufpos) {
            xcb_render_color_t col = MAKE_COLOR(con.cbuffer[j].fg);
            xcb_render_fill_rectangles(con.con, XCB_RENDER_PICT_OP_SRC, win->pic, col,
                con.bufpos/sizeof(xcb_rectangle_t), (xcb_rectangle_t *)con.buffer);
        }
    }

    if (cursor) {
        cur_x *= win->char_width;
        cur_y *= win->char_depth + win->char_height;
        xcb_rectangle_t rects[4] = {
            {cur_x, cur_y, 1, win->char_height + win->char_depth},
            {cur_x, cur_y, win->char_width, 1},
            {cur_x + win->char_width - 1, cur_y, 1, win->char_height + win->char_depth},
            {cur_x, cur_y + (win->char_depth + win->char_height - 1), win->char_width, 1}
        };
        size_t off = 0, count = 4;
        if (win->focused) {
            if (win->cursor_type == nss_cursor_bar) {
                if(marg) {
                    off = 2;
                    rects[2].width = win->cursor_width;
                    rects[2].x -= win->cursor_width - 1;
                } else
                    rects[0].width = win->cursor_width;
                count = 1;
            } else if (win->cursor_type == nss_cursor_underline) {
                count = 1;
                off = 3;
                rects[3].height = win->cursor_width;
                rects[3].x -= win->cursor_width - 1;
            } else {
                count = 0;
            }
        }
        if (count) {
            xcb_render_color_t c = MAKE_COLOR(win->cursor_fg);
            xcb_render_fill_rectangles(con.con, XCB_RENDER_PICT_OP_OVER, win->pic, c, count, rects + off);
        }
    }

    if (con.cbufpos)
        xcb_copy_area(con.con, win->pid, win->wid, win->gc, 0, 0, win->left_border,
                      win->top_border, win->cw * win->char_width, win->ch * (win->char_depth + win->char_height));
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
        if (count) xcb_poly_fill_rectangle(con.con, win->wid, win->gc, count, borders + offset);
}

void nss_window_shift(nss_window_t *win, coord_t ys, coord_t yd, coord_t height, _Bool delay) {

    struct timespec cur;
    clock_gettime(CLOCK_MONOTONIC, &cur);

    ys = MAX(0, MIN(ys, win->ch));
    yd = MAX(0, MIN(yd, win->ch));
    height = MIN(height, MIN(win->ch - ys, win->ch - yd));

    if (delay && TIMEDIFF(win->last_scroll, cur) <  SEC/2/nss_config_integer(NSS_ICONFIG_FPS)) {
        nss_term_damage(win->term, (nss_rect_t){ .x = 0, .y = yd, .width = win->cw, .height = height });
        win->last_scroll = cur;
        return;
    }
    win->last_scroll = cur;

    if (!height) return;

    ys *= win->char_height + win->char_depth;
    yd *= win->char_height + win->char_depth;
    coord_t width = win->cw * win->char_width;
    height *= win->char_depth + win->char_height;

    xcb_copy_area(con.con, win->pid, win->pid, win->gc, 0, ys, 0, yd, width, height);
    //xcb_render_composite(con.con, XCB_RENDER_PICT_OP_SRC, win->pic, 0, win->pic, 0, ys, 0, 0, 0, yd, width, height);
}

void nss_window_set(nss_window_t *win, nss_wc_tag_t tag, const uint32_t *values) {
    set_config(win, tag, values);
    _Bool inval_screen = 0;

    if (tag & (nss_wc_font_size | nss_wc_subpixel_fonts))
        reload_font(win, 1), inval_screen = 1;
    if (tag & nss_wc_background) {
        uint32_t values2[2];
        values2[0] = values2[1] = win->bg;
        xcb_change_window_attributes(con.con, win->wid, XCB_CW_BACK_PIXEL, values2);
        xcb_change_gc(con.con, win->gc, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, values2);
        inval_screen = 1;
    }
   if (inval_screen) {
        nss_term_damage(win->term, (nss_rect_t){0, 0, win->cw, win->ch});
        win->force_redraw = 1;
   }
   if (tag & nss_wc_mouse) {
       if (win->mouse_events)
            win->ev_mask |= XCB_EVENT_MASK_POINTER_MOTION;
       else
           win->ev_mask &= ~XCB_EVENT_MASK_POINTER_MOTION;
       xcb_change_window_attributes(con.con, win->wid, XCB_CW_EVENT_MASK, &win->ev_mask);
   }
}

void nss_window_set_font(nss_window_t *win, const char * name) {
    if (!name) {
        warn("Empty font name");
        return;
    }
    free(win->font_name);
    win->font_name = strdup(name);
    reload_font(win, 1);
    nss_term_damage(win->term, (nss_rect_t){0, 0, win->cw, win->ch});
    win->force_redraw = 1;
    xcb_flush(con.con);
}

nss_font_t *nss_window_get_font(nss_window_t *win) {
    return win->font;
}

char *nss_window_get_font_name(nss_window_t *win) {
    return win->font_name;
}

uint32_t nss_window_get(nss_window_t *win, nss_wc_tag_t tag) {
    if (tag & nss_wc_cusror_width) return win->cursor_width;
    if (tag & nss_wc_left_border) return win->left_border;
    if (tag & nss_wc_top_border) return win->top_border;
    if (tag & nss_wc_background) return win->bg;
    if (tag & nss_wc_cursor_foreground) return win->cursor_fg;
    if (tag & nss_wc_cursor_type) return win->cursor_type;
    if (tag & nss_wc_subpixel_fonts) return win->subpixel_fonts;
    if (tag & nss_wc_font_size) return win->font_size;
    if (tag & nss_wc_width) return win->width;
    if (tag & nss_wc_height) return win->height;

    if (tag & nss_wc_blink_time) return win->blink_time;
    if (tag & nss_wc_mouse) return win->mouse_events;

    warn("Invalid option");
    return 0;
}

static void handle_resize(nss_window_t *win, int16_t width, int16_t height) {

    //Handle resize

    win->width = width;
    win->height = height;

    coord_t new_cw = MAX(1, (win->width - 2*win->left_border)/win->char_width);
    coord_t new_ch = MAX(1, (win->height - 2*win->top_border)/(win->char_height+win->char_depth));
    coord_t delta_x = new_cw - win->cw;
    coord_t delta_y = new_ch - win->ch;
    win->cw = new_cw;
    win->ch = new_ch;

    _Bool do_redraw_borders = delta_x < 0 || delta_y < 0;

    if (delta_x || delta_y) {

        int16_t width = win->cw * win->char_width;
        int16_t height = win->ch * (win->char_height + win->char_depth);
        int16_t common_w = MIN(width, width  - delta_x * win->char_width);
        int16_t common_h = MIN(height, height - delta_y * (win->char_height + win->char_depth)) ;

        xcb_pixmap_t pid = xcb_generate_id(con.con);
        xcb_create_pixmap(con.con, TRUE_COLOR_ALPHA_DEPTH, pid, win->wid, width, height);
        xcb_render_picture_t pic = xcb_generate_id(con.con);
        uint32_t mask3 = XCB_RENDER_CP_GRAPHICS_EXPOSURE | XCB_RENDER_CP_POLY_EDGE | XCB_RENDER_CP_POLY_MODE;
        uint32_t values3[3] = { 0, XCB_RENDER_POLY_EDGE_SMOOTH, XCB_RENDER_POLY_MODE_IMPRECISE };
        xcb_render_create_picture(con.con, pic, pid, con.pfargb, mask3, values3);

        xcb_render_composite(con.con, XCB_RENDER_PICT_OP_SRC, win->pic, 0, pic, 0, 0, 0, 0, 0, 0, common_w, common_h);

        SWAP(xcb_pixmap_t, win->pid, pid);
        SWAP(xcb_render_picture_t, win->pic, pic);
        xcb_free_pixmap(con.con, pid);
        xcb_render_free_picture(con.con, pic);

        nss_rect_t rectv[2];
        size_t rectc= 0;

        if (delta_y > 0)
            rectv[rectc++] = (nss_rect_t) { 0, win->ch - delta_y, MIN(win->cw, win->cw - delta_x), delta_y };
        if (delta_x > 0)
            rectv[rectc++] = (nss_rect_t) { win->cw - delta_x, 0, delta_x, MAX(win->ch, win->ch - delta_y) };

        clock_gettime(CLOCK_MONOTONIC, &win->last_scroll);
        nss_term_resize(win->term, win->cw, win->ch);

        for (size_t i = 0; i < rectc; i++)
            nss_term_damage(win->term, rectv[i]);

        for (size_t i = 0; i < rectc; i++)
            rectv[i] = rect_scale_up(rectv[i], win->char_width, win->char_height + win->char_depth);
        xcb_render_color_t color = MAKE_COLOR(win->bg);
        xcb_render_fill_rectangles(con.con, XCB_RENDER_PICT_OP_SRC, win->pic, color, rectc, (xcb_rectangle_t*)rectv);
    }

    if (do_redraw_borders) {
        redraw_borders(win, 0, 1);
        //TIP: May be redraw all borders here
    }

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
    if (num_damaged)
        xcb_poly_fill_rectangle(con.con, win->wid, win->gc, num_damaged, (xcb_rectangle_t*)damaged);

    nss_rect_t inters = { win->left_border, win->top_border, width - win->left_border, height  - win->top_border};
    if (intersect_with(&inters, &damage)) {
        xcb_copy_area(con.con, win->pid, win->wid, win->gc, inters.x - win->left_border, inters.y - win->top_border,
                      inters.x, inters.y, inters.width, inters.height);
    }
}

static void handle_focus(nss_window_t *win, _Bool focused) {
    win->focused = focused;
    nss_term_focus(win->term, focused);
}


static void handle_keydown(nss_window_t *win, xkb_keycode_t keycode) {
    nss_key_t key = nss_describe_key(con.xkb_state, keycode);

    if (key.sym == XKB_KEY_NoSymbol) return;

    enum nss_shortcut_action action = nss_sa_none;
    for (size_t i = 0; i < sizeof(cshorts)/sizeof(*cshorts); i++) {
        if (cshorts[i].ksym == key.sym && (key.mask & cshorts[i].mmask) == cshorts[i].mstate) {
            action = cshorts[i].action;
            break;
        }
    }

    switch (action) {
        uint32_t arg;
        nss_input_mode_t *inm;
    case nss_sa_break:
        nss_term_sendbreak(win->term);
        return;
    case nss_sa_numlock:
        inm = nss_term_inmode(win->term);
        inm->allow_numlock = !inm->allow_numlock;
        return;
    case nss_sa_scroll_up:
        nss_term_scroll_view(win->term, -nss_config_integer(NSS_ICONFIG_SCROLL_AMOUNT));
        return;
    case nss_sa_scroll_down:
        nss_term_scroll_view(win->term, nss_config_integer(NSS_ICONFIG_SCROLL_AMOUNT));
        return;
    case nss_sa_font_up:
        arg = win->font_size + nss_config_integer(NSS_ICONFIG_FONT_SIZE_STEP);
        nss_window_set(win, nss_wc_font_size, &arg);
        return;
    case nss_sa_font_down:
        arg = win->font_size - nss_config_integer(NSS_ICONFIG_FONT_SIZE_STEP);
        nss_window_set(win, nss_wc_font_size, &arg);
        return;
    case nss_sa_font_default:
        arg = nss_config_integer(NSS_ICONFIG_FONT_SIZE);
        nss_window_set(win, nss_wc_font_size, &arg);
        return;
    case nss_sa_font_subpixel:
        arg = !win->subpixel_fonts;
        nss_window_set(win, nss_wc_subpixel_fonts, &arg);
        return;
    case nss_sa_new_window:
        arg = 0;
        nss_create_window(NULL, 0, NULL);
        return;
    case nss_sa_none: break;
    }

    nss_handle_input(key, win->term);
}

/* Start window logic, handling all windows in context */
void nss_context_run(void) {
    int64_t next_timeout = SEC/nss_config_integer(NSS_ICONFIG_FPS);
    for (;;) {
#ifdef USE_PPOLL
        struct timespec ppoll_timeout = { .tv_sec = 0, .tv_nsec = next_timeout};
        if (ppoll(con.pfds, con.pfdcap, &ppoll_timeout, NULL) < 0 && errno != EINTR)
#else
        if (poll(con.pfds, con.pfdcap, next_timeout/(SEC/1000)) < 0 && errno != EINTR)
#endif
            warn("Poll error: %s", strerror(errno));
        if (con.pfds[0].revents & POLLIN) {
            xcb_generic_event_t *event;
            while ((event = xcb_poll_for_event(con.con))) {
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
                        handle_resize(win, ev->width, ev->height);
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
                    nss_mouse_state_t mask = ev->state;
                    nss_mouse_event_t evtype = -1;
                    switch (ev->response_type & 0xF7) {
                    case XCB_BUTTON_PRESS:
                        evtype = nss_me_press;
                        break;
                    case XCB_BUTTON_RELEASE:
                        evtype = nss_me_release;
                        break;
                    case XCB_MOTION_NOTIFY:
                        evtype = nss_me_motion;
                        break;
                    }

                    if (evtype == nss_me_press && !nss_term_is_altscreen(win->term) &&
                            (button == 3 || button == 4) && !mask) {
                        nss_term_scroll_view(win->term, (2 *(button == 3) - 1) *
                                nss_config_integer(NSS_ICONFIG_SCROLL_AMOUNT));
                    } else nss_term_mouse(win->term, x, y, mask, evtype, button);
                    break;
                }
                case XCB_CLIENT_MESSAGE: {
                    xcb_client_message_event_t *ev = (xcb_client_message_event_t*)event;
                    nss_window_t *win = window_for_xid(ev->window);
                    if (!win) break;
                    if (ev->format == 32 && ev->data.data32[0] == con.atom_wm_delete_window) {
                        nss_free_window(win);
                        if (!con.first && !con.daemon_mode) {
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
                    if (event->response_type == con.xkb_base_event) {
                        struct _xkb_any_event {
                            uint8_t response_type;
                            uint8_t xkbType;
                            uint16_t sequence;
                            xcb_timestamp_t time;
                            uint8_t deviceID;
                        } *xkb_ev;

                        xkb_ev = (struct _xkb_any_event*)event;
                        if (xkb_ev->deviceID == con.xkb_core_kbd) {
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
                                xkb_state_update_mask(con.xkb_state, ev->baseMods, ev->latchedMods, ev->lockedMods,
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
        for (size_t i = 1; i < con.pfdcap; i++) {
            if (con.pfds[i].fd > 0) {
                nss_window_t *win = window_for_term_fd(con.pfds[i].fd);
                if (con.pfds[i].revents & POLLIN & win->got_configure) {
                    nss_term_read(win->term);
                } else if (con.pfds[i].revents & (POLLERR | POLLNVAL | POLLHUP)) {
                    nss_free_window(win);
                }
            }
        }

        next_timeout = SEC/nss_config_integer(NSS_ICONFIG_FPS);
        struct timespec cur;
        clock_gettime(CLOCK_MONOTONIC, &cur);

        for (nss_window_t *win = con.first; win; win = win->next) {
            if (TIMEDIFF(win->last_blink, cur) > win->blink_time && win->active) {
                win->blink_state = !win->blink_state;
                win->blink_commited = 0;
                win->last_blink = cur;
            }

            int64_t frame_time = SEC/nss_config_integer(NSS_ICONFIG_FPS);
            if (TIMEDIFF(win->last_scroll, cur) < frame_time/2) frame_time += frame_time/2;
            int64_t remains = (frame_time - TIMEDIFF(win->last_draw, cur));

            if (remains/1000000 <= 0 || win->force_redraw) {
                if (win->force_redraw)
                    redraw_borders(win, 1, 1);
                nss_term_redraw_dirty(win->term, 1);
                win->last_draw = cur;
                win->force_redraw = 0;
                win->blink_commited = 1;
                remains = SEC/nss_config_integer(NSS_ICONFIG_FPS);
             }
            next_timeout = MIN(next_timeout,  remains);
        }
        xcb_flush(con.con);

        // TODO Try reconnect after timeout
        if ((!con.daemon_mode && !con.first) || xcb_connection_has_error(con.con)) break;

        if (reload_config) {
            reload_config = 0;
            load_params();
        }
    }
}
