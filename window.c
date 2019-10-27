#define _POSIX_C_SOURCE 200809L


#include <errno.h>
#include <inttypes.h>
#include <poll.h>
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

#include "window.h"
#include "util.h"
#include "font.h"
#include "term.h"

#define TRUE_COLOR_ALPHA_DEPTH 32
#define NUM_BORDERS 4
#define INIT_PFD_NUM 16

#define ASCII_MAX 128
#define ASCII_PRINTABLE_HIGH 127
#define ASCII_PRINTABLE_LOW 32

#define WORDS_IN_MESSAGE 256
#define HEADER_WORDS ((sizeof(nss_glyph_mesg_t)+sizeof(uint32_t))/sizeof(uint32_t))
#define CHARS_PER_MESG (WORDS_IN_MESSAGE - HEADER_WORDS)

#define CR(c) (((c) & 0xff) * 0x101)
#define CG(c) ((((c) >> 8) & 0xff) * 0x101)
#define CB(c) ((((c) >> 16) & 0xff) * 0x101)
#define CA(c) ((((c) >> 24) & 0xff) * 0x101)
#define MAKE_COLOR(c) {.red=CR(c),.green=CG(c),.blue=CB(c),.alpha=CA(c)}


struct nss_window {
    xcb_window_t wid;
    xcb_pixmap_t pid;
    xcb_gcontext_t gc;
    xcb_render_picture_t pic;

    _Bool focused;
    _Bool active;
    _Bool lcd_mode;
    _Bool got_configure;

    int16_t width;
    int16_t height;
    int16_t cw, ch;
    int16_t cursor_width;
    int16_t underline_width;
    int16_t left_border;
    int16_t top_border;
    int16_t font_size;

    nss_cid_t bg_cid;
    nss_cid_t fg_cid;
    nss_cid_t cursor_fg_cid;
    nss_cid_t cursor_bg_cid;
    nss_cursor_type_t cursor_type;


    /*     * Glyph encoding:
     *  0xTTUUUUUU, where
     *      * 0xTT - font fase
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
};

typedef struct nss_glyph_mesg {
    uint8_t len;
    uint8_t pad[3];
    int16_t dx, dy;
    uint8_t data[];
} nss_glyph_mesg_t;

static _Bool check_void_cookie(nss_context_t *con, xcb_void_cookie_t ck) {
    xcb_generic_error_t *err = xcb_request_check(con->con,ck);
    if (err) {
        warn("X11 error: %"PRIu8" %"PRIu16" %"PRIu8,err->major_code,err->minor_code,err->error_code);
        return 1;
    }
    free(err);
    return 0;
}

static nss_window_t *nss_window_for_xid(nss_context_t *con, xcb_window_t xid) {
    for (nss_window_t *win = con->first; win; win = win->next)
        if (win->wid == xid) return win;
    warn("Window for xid not found");
    return NULL;
}
static nss_window_t *nss_window_for_term_fd(nss_context_t *con, int fd) {
    for (nss_window_t *win = con->first; win; win = win->next)
        if (win->term_fd == fd) return win;
    warn("Window for fd not found");
    return NULL;
}

static xcb_atom_t intern_atom(nss_context_t *con, const char *atom) {
    xcb_atom_t at;
    xcb_generic_error_t *err;
    xcb_intern_atom_cookie_t c = xcb_intern_atom(con->con, 0, strlen(atom), atom);
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(con->con, c, &err);
    if (err) {
        warn("Can't intern atom: %s", atom);
        free(err);
    }
    at = reply->atom;
    free(reply);
    return at;
}

static _Bool update_keymap(nss_context_t *con) {
    struct xkb_keymap *new_keymap = xkb_x11_keymap_new_from_device(con->xkb_ctx, con->con, con->xkb_core_kbd, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!new_keymap) {
        warn("Can't create XKB keymap");
        return 0;
    }
    struct xkb_state *new_state = xkb_x11_state_new_from_device(new_keymap, con->con, con->xkb_core_kbd);
    if (!new_state) {
        warn("Can't get window xkb state");
        return 0;
    }
    if (con->xkb_state)
        xkb_state_unref(con->xkb_state);
    if (con->xkb_keymap)
        xkb_keymap_unref(con->xkb_keymap);
    con->xkb_keymap = new_keymap;
    con->xkb_state = new_state;
    return 1;
}

static _Bool configure_xkb(nss_context_t *con) {
    uint16_t xkb_min = 0, xkb_maj = 0;
    int res = xkb_x11_setup_xkb_extension(con->con, XKB_X11_MIN_MAJOR_XKB_VERSION,
                                          XKB_X11_MIN_MINOR_XKB_VERSION, XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS,
                                          &xkb_maj, &xkb_min, &con->xkb_base_event, &con->xkb_base_err);
    info("XKB base event: %02"PRIu8, con->xkb_base_event);
    if (!res || xkb_maj < XKB_X11_MIN_MAJOR_XKB_VERSION) {
        warn("Can't get suitable XKB verion");
        return 0;
    }
    con->xkb_core_kbd = xkb_x11_get_core_keyboard_device_id(con->con);
    if (con->xkb_core_kbd == -1) {
        warn("Can't get core keyboard device");
        return 0;
    }

    con->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!con->xkb_ctx) {
        warn("Can't create XKB context");
        return 0;
    }

    con->xkb_keymap = xkb_x11_keymap_new_from_device(con->xkb_ctx, con->con, con->xkb_core_kbd, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!con->xkb_keymap) {
        warn("Can't create XKB keymap");
        goto cleanup_context;
    }
    con->xkb_state = xkb_x11_state_new_from_device(con->xkb_keymap, con->con, con->xkb_core_kbd);
    if (!con->xkb_state) {
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
    c = xcb_xkb_select_events_aux_checked(con->con, con->xkb_core_kbd, events,
                                          0, 0, map_parts, map_parts, &details);
    if (check_void_cookie(con, c)) {
        warn("Can't select XKB events");
        goto cleanup_state;
    }

    if (!update_keymap(con)) {
        warn("Can't update keymap");
        goto cleanup_state;
    }

    return 1;

cleanup_state:
    xkb_state_unref(con->xkb_state);
cleanup_keymap:
    xkb_keymap_unref(con->xkb_keymap);
cleanup_context:
    xkb_context_unref(con->xkb_ctx);
    return 1;
}

/* Initialize global state object */
nss_context_t *nss_create_context(void) {
    nss_context_t *con = calloc(1, sizeof(*con));
    if(!con) die("Can't allocate context");
    con->daemon_mode = 0;

    con->pfds = calloc(INIT_PFD_NUM, sizeof(struct pollfd));
    if(!con->pfds) die("Can't allocate pfds");
    con->pfdn = 1;
    con->pfdcap = INIT_PFD_NUM;
    for(size_t i = 1; i < INIT_PFD_NUM; i++)
        con->pfds[i].fd = -1;

    int screenp;
    con->con = xcb_connect(NULL, &screenp);
    con->pfds[0].events = POLLIN | POLLHUP;
    con->pfds[0].fd = xcb_get_file_descriptor(con->con);

    xcb_screen_iterator_t sit = xcb_setup_roots_iterator(xcb_get_setup(con->con));
    for (; sit.rem; xcb_screen_next(&sit))
        if (screenp-- == 0)break;
    if (screenp != -1) {
        xcb_disconnect(con->con);
        die("Can't find default screen");
    }
    con->screen = sit.data;

    xcb_depth_iterator_t dit = xcb_screen_allowed_depths_iterator(con->screen);
    for (; dit.rem; xcb_depth_next(&dit))
        if (dit.data->depth == TRUE_COLOR_ALPHA_DEPTH) break;
    if (dit.data->depth != TRUE_COLOR_ALPHA_DEPTH) {
        xcb_disconnect(con->con);
        die("Can't get 32-bit visual");
    }

    xcb_visualtype_iterator_t vit = xcb_depth_visuals_iterator(dit.data);
    for (; vit.rem; xcb_visualtype_next(&vit))
        if (vit.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR) break;

    if (vit.data->_class != XCB_VISUAL_CLASS_TRUE_COLOR) {
        xcb_disconnect(con->con);
        die("Can't get 32-bit visual");
    }

    con->vis = vit.data;

    con->mid = xcb_generate_id(con->con);
    xcb_void_cookie_t c = xcb_create_colormap_checked(con->con, XCB_COLORMAP_ALLOC_NONE,
                                       con->mid, con->screen->root, con->vis->visual_id);
    if (check_void_cookie(con,c)) {
        nss_free_context(con);
        die("Can't create colormap");
    }

    // Check if XRender is present
    xcb_render_query_version_cookie_t vc = xcb_render_query_version(con->con, XCB_RENDER_MAJOR_VERSION, XCB_RENDER_MINOR_VERSION);
    xcb_generic_error_t *err;
    xcb_render_query_version_reply_t *rep = xcb_render_query_version_reply(con->con, vc, &err);
    // Any version is OK, so don't check
    free(rep);

    if (err) {
        uint8_t erc = err->error_code;
        xcb_disconnect(con->con);
        die("XRender not detected: %"PRIu8, erc);
    }


    xcb_render_query_pict_formats_cookie_t pfc = xcb_render_query_pict_formats(con->con);
    xcb_render_query_pict_formats_reply_t *pfr = xcb_render_query_pict_formats_reply(con->con, pfc, &err);

    if (err) {
        uint8_t erc = err->error_code;
        xcb_disconnect(con->con);
        die("Can't query picture formats: %"PRIu8, erc);
    }

    xcb_render_pictforminfo_iterator_t pfit =  xcb_render_query_pict_formats_formats_iterator(pfr);
    for (; pfit.rem; xcb_render_pictforminfo_next(&pfit)) {
        if (pfit.data->depth == TRUE_COLOR_ALPHA_DEPTH && pfit.data->type == XCB_RENDER_PICT_TYPE_DIRECT &&
           pfit.data->direct.red_mask == 0xff && pfit.data->direct.green_mask == 0xff &&
           pfit.data->direct.blue_mask == 0xff && pfit.data->direct.alpha_mask == 0xff &&
           pfit.data->direct.red_shift == 16 && pfit.data->direct.green_shift == 8 &&
           pfit.data->direct.blue_shift == 0 && pfit.data->direct.alpha_shift == 24 ) {
               con->pfargb = pfit.data->id;
        }
        if (pfit.data->depth == 8 && pfit.data->type == XCB_RENDER_PICT_TYPE_DIRECT &&
           pfit.data->direct.alpha_mask == 0xff && pfit.data->direct.alpha_shift == 0) {
               con->pfalpha = pfit.data->id;
        }
    }

    free(pfr);

    if (con->pfargb == 0 || con->pfalpha == 0) {
        xcb_disconnect(con->con);
        die("Can't find suitable picture format");
    }

    if (!configure_xkb(con)) {
        xcb_disconnect(con->con);
        die("Can't configure XKB");
    }

    con->atom_net_wm_pid = intern_atom(con,"_NET_WM_PID");
    con->atom_wm_delete_window = intern_atom(con, "WM_DELETE_WINDOW");
    con->atom_wm_protocols = intern_atom(con, "WM_PROTOCOLS");
    con->atom_utf8_string = intern_atom(con, "UTF8_STRING");
    con->atom_net_wm_name = intern_atom(con, "_NET_WM_NAME");

    return con;
}

void nss_window_set_title(nss_context_t *con, nss_window_t *win, const char *title) {
    xcb_change_property(con->con, XCB_PROP_MODE_REPLACE, win->wid, XCB_ATOM_WM_NAME, con->atom_utf8_string, 8, strlen(title), title);
    xcb_change_property(con->con, XCB_PROP_MODE_REPLACE, win->wid, con->atom_net_wm_name, con->atom_utf8_string, 8, strlen(title), title);
}

/* Free all resources */
void nss_free_context(nss_context_t *con) {
    while (con->first)
        nss_free_window(con,con->first);
    xkb_state_unref(con->xkb_state);
    xkb_keymap_unref(con->xkb_keymap);
    xkb_context_unref(con->xkb_ctx);

    xcb_disconnect(con->con);
    free(con);
}

static void register_glyph(nss_context_t *con, nss_window_t *win, uint32_t ch, nss_glyph_t * glyph) {
    xcb_render_glyphinfo_t spec = {
        .width = glyph->width, .height = glyph->height,
        .x = glyph->x, .y = glyph->y,
        .x_off = glyph->x_off, .y_off = glyph->y_off
    };
    xcb_render_add_glyphs(con->con, win->gsid, 1, &ch, &spec, glyph->height*glyph->stride, glyph->data);
}

static void set_config(nss_window_t *win, nss_wc_tag_t tag, const uint32_t *values) {
    if (tag & nss_wc_cusror_width) win->cursor_width = *values++;
    if (tag & nss_wc_left_border) win->left_border = *values++;
    if (tag & nss_wc_top_border) win->top_border = *values++;
    if (tag & nss_wc_background) win->bg_cid = *values++;
    if (tag & nss_wc_foreground) win->fg_cid = *values++;
    if (tag & nss_wc_cursor_background) win->cursor_bg_cid = *values++;
    if (tag & nss_wc_cursor_foreground) win->cursor_fg_cid = *values++;
    if (tag & nss_wc_cursor_type) win->cursor_type = *values++;
    if (tag & nss_wc_lcd_mode) win->lcd_mode = *values++;
    if (tag & nss_wc_font_size) win->font_size = *values++;
    if (tag & nss_wc_underline_width) win->underline_width = *values++;
}

/* Reload font using win->font_size and win->font_name */
static _Bool reload_font(nss_context_t *con, nss_window_t *win, _Bool need_free) {
    //Try find already existing font
    _Bool found_font = 0, found_gset = 0;
    nss_window_t *found = 0;
    for (nss_window_t *src = con->first; src; src = src->next) {
        if ((src->font_size == win->font_size || win->font_size == 0) &&
           !strcmp(win->font_name, src->font_name) && src != win) {
            found_font = 1;
            found = src;
            if (src->lcd_mode == win->lcd_mode) {
                found_gset = 1;
                break;
            }
        }
    }

    nss_font_t *new = found_font ? nss_font_reference(found->font) :
        nss_create_font(win->font_name, win->font_size, nss_context_get_dpi(con));
    if (!new) {
        warn("Can't create new font: %s", win->font_name);
        return 0;
    }

    if (need_free) nss_free_font(win->font);

    win->font = new;
    win->font_size = nss_font_get_size(new);
    win->pfglyph = win->lcd_mode ? con->pfargb : con->pfalpha;

    xcb_void_cookie_t c;

    if (need_free) xcb_render_free_glyph_set(con->con, win->gsid);
    else win->gsid = xcb_generate_id(con->con);

    if (found_gset) {
        xcb_render_reference_glyph_set(con->con, win->gsid, found->gsid);

        win->char_height = found->char_height;
        win->char_depth = found->char_depth;
        win->char_width = found->char_width;
    } else {
        xcb_render_create_glyph_set(con->con, win->gsid, win->pfglyph);

        //Preload ASCII
        int16_t total = 0, maxd = 0, maxh = 0;
        for (uint32_t i = ' '; i <= '~'; i++) {
            nss_glyph_t *glyphs[nss_font_attrib_max];
            for (size_t j = 0; j < nss_font_attrib_max; j++) {
                glyphs[j] = nss_font_render_glyph(win->font, i, j, win->lcd_mode);
                register_glyph(con, win, i | (j << 24),glyphs[j]);
            }

            total += glyphs[0]->x_off;
            maxd = MAX(maxd, glyphs[0]->height - glyphs[0]->y);
            maxh = MAX(maxh, glyphs[0]->y);

            for (size_t j = 0; j < nss_font_attrib_max; j++)
                free(glyphs[j]);
        }

        win->char_width = (total + '~' - ' ') / ('~' - ' ' + 1);
        win->char_height = maxh;
        win->char_depth = maxd;
    }

    win->cw = MAX(1,(win->width - 2*win->left_border) / win->char_width);
    win->ch = MAX(1,(win->height - 2*win->top_border) / (win->char_height + win->char_depth));

    xcb_rectangle_t bound = { 0, 0, win->cw*win->char_width, win->ch*(win->char_depth+win->char_height) };

    if (need_free) {
        xcb_free_pixmap(con->con, win->pid);
        xcb_free_gc(con->con, win->gc);
        xcb_render_free_picture(con->con, win->pic);
        nss_term_resize(win->term, win->cw, win->ch);
    } else {
        win->pid = xcb_generate_id(con->con);
        win->gc = xcb_generate_id(con->con);
        win->pic = xcb_generate_id(con->con);
    }

    c = xcb_create_pixmap_checked(con->con, TRUE_COLOR_ALPHA_DEPTH, win->pid, win->wid, bound.width, bound.height );
    if (check_void_cookie(con,c)) {
        warn("Can't create pixmap");
        return 0;
    }

    uint32_t mask2 = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_GRAPHICS_EXPOSURES;
    uint32_t values2[3] = { nss_color_get(win->bg_cid), nss_color_get(win->bg_cid), 0 };
    c = xcb_create_gc_checked(con->con, win->gc, win->pid, mask2, values2);
    if (check_void_cookie(con,c)) {
        warn("Can't create GC");
        return 0;
    }

    uint32_t mask3 = XCB_RENDER_CP_GRAPHICS_EXPOSURE | XCB_RENDER_CP_POLY_EDGE | XCB_RENDER_CP_POLY_MODE;
    uint32_t values3[3] = { 0, XCB_RENDER_POLY_EDGE_SMOOTH, XCB_RENDER_POLY_MODE_IMPRECISE };
    c = xcb_render_create_picture_checked(con->con,win->pic, win->pid, con->pfargb, mask3, values3);
    if (check_void_cookie(con,c)) {
        warn("Can't create XRender picture");
        return 0;
    }

    xcb_render_color_t color = MAKE_COLOR(nss_color_get(win->bg_cid));
    xcb_render_fill_rectangles(con->con, XCB_RENDER_PICT_OP_OVER, win->pic, color, 1, &bound);
    return 1;
}

static void set_wm_props(nss_context_t *con, nss_window_t *win) {
    uint32_t pid = getpid();
    xcb_change_property(con->con, XCB_PROP_MODE_REPLACE, win->wid, con->atom_net_wm_pid, XCB_ATOM_CARDINAL, 32, 1, &pid);
    xcb_change_property(con->con, XCB_PROP_MODE_REPLACE, win->wid, con->atom_wm_protocols, XCB_ATOM_ATOM, 32, 1, &con->atom_wm_delete_window);
    const char class[] = "nss\0Nss";
    xcb_change_property(con->con, XCB_PROP_MODE_REPLACE, win->wid, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, sizeof(class), class);
}

/* Create new window */
nss_window_t *nss_create_window(nss_context_t *con, nss_rect_t rect, const char *font_name, nss_wc_tag_t tag, const uint32_t *values) {
    nss_window_t *win = malloc(sizeof(nss_window_t));
    win->cursor_width = 2;
    win->underline_width = 1;
    win->left_border = 8;
    win->top_border = 8;
    win->bg_cid = 0;
    win->fg_cid = 7;
    win->cursor_bg_cid = 0;
    win->cursor_fg_cid = 7;
    win->cursor_type = nss_cursor_block;
    win->lcd_mode = 0;
    win->font_size = 0;
    win->focused = 0;
    win->active = 0;
    win->got_configure = 0;
    win->font_name = strdup(font_name);

    set_config(win,tag, values);

    xcb_void_cookie_t c;

    uint32_t mask1 =  XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL |
        XCB_CW_BIT_GRAVITY | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
    uint32_t values1[5] = {
        nss_color_get(win->bg_cid),  nss_color_get(win->bg_cid),
        XCB_GRAVITY_NORTH_WEST, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_VISIBILITY_CHANGE |
        XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY, con->mid
    };
    win->wid = xcb_generate_id(con->con);
    c = xcb_create_window_checked(con->con, TRUE_COLOR_ALPHA_DEPTH, win->wid, con->screen->root,
                                  rect.x,rect.y, rect.width, rect.height, 0,
                                  XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                  con->vis->visual_id, mask1,values1);
    if (check_void_cookie(con, c)) {
        warn("Can't create window");
        free(win);
        return NULL;
    }

    set_wm_props(con,win);

    if (!reload_font(con, win, 0)) {
        warn("Can't create window");
        free(win);
        return NULL;
    }

    info("Font size: %d %d %d", win->char_height, win->char_depth, win->char_width);

    win->term = nss_create_term(con, win, win->cw, win->ch);
    if(!win->term) {
        warn("Can't create term");
        xcb_render_free_picture(con->con,win->pic);
        xcb_free_gc(con->con,win->gc);
        xcb_free_pixmap(con->con,win->pid);
        xcb_render_free_glyph_set(con->con,win->gsid);
        xcb_destroy_window(con->con,win->wid);
        free(win);
        return NULL;
    }
    nss_term_redraw(win->term, (nss_rect_t) {0,0,win->cw,win->ch});


    win->next = con->first;
    win->prev = NULL;
    if (con->first) con->first->prev = win;
    con->first = win;

    xcb_map_window(con->con,win->wid);
    xcb_flush(con->con);

    if (con->pfdn + 1 > con->pfdcap) {
        struct pollfd *new = realloc(con->pfds, con->pfdcap + INIT_PFD_NUM);
        if (new) {
            for(size_t i = 0; i < INIT_PFD_NUM; i++) {
                con->pfds[i + con->pfdn].fd = -1;
                con->pfds[i + con->pfdn].events = 0;
            }
            con->pfdcap += INIT_PFD_NUM;
            con->pfds = new;
        } else {
            warn("Can't reallocate con->pfds");
            nss_free_window(con, win);
            return NULL;
        }
    }
    con->pfdn++;
    size_t i = 1;
    while(con->pfds[i].fd >= 0) i++;
    // Because it might become -1 suddenly
    win->term_fd = nss_term_fd(win->term);
    con->pfds[i].events = POLLIN | POLLHUP;
    con->pfds[i].fd = win->term_fd;

    return win;
}

/* Free previously created windows */
void nss_free_window(nss_context_t *con, nss_window_t *win) {
    info("Freeing window");
    xcb_unmap_window(con->con,win->wid);
    xcb_flush(con->con);

    if (win->next)win->next->prev = win->prev;
    if (win->prev)win->prev->next = win->next;
    else con->first =  win->next;

    size_t i = 0;
    while(con->pfds[i].fd != win->term_fd && i < con->pfdcap) i++;
    if(i < con->pfdcap)
        con->pfds[i].fd = -1;
    else warn("Window fd not found");
    con->pfdn--;

    nss_free_term(win->term);
    nss_free_font(win->font);

    xcb_render_free_picture(con->con,win->pic);
    xcb_free_gc(con->con,win->gc);
    xcb_free_pixmap(con->con,win->pid);
    xcb_render_free_glyph_set(con->con,win->gsid);
    xcb_destroy_window(con->con,win->wid);
    xcb_flush(con->con);

    free(win->font_name);
    free(win);
};

/* Get monitor DPI */
uint16_t nss_context_get_dpi(nss_context_t *con) {
    xcb_xrm_database_t *xrmdb = xcb_xrm_database_from_default(con->con);
    long dpi = 0;
    if (xrmdb) {
        if (xcb_xrm_resource_get_long(xrmdb, "Xft.dpi", NULL, &dpi) >= 0) {
            xcb_xrm_database_free(xrmdb);
            return dpi;
        }
        xcb_xrm_database_free(xrmdb);
    }
    warn("Can't fetch Xft.dpi, defaulting to highest dpi value");

    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(con->con));
    for (; it.rem; xcb_screen_next(&it))
        if (it.data)
            dpi = MAX(dpi, (it.data->width_in_pixels * 25.4)/it.data->width_in_millimeters);
    if (!dpi) {
        warn("Can't get highest dpi, defaulting to 96");
        dpi = 96;
    }
    return dpi;
}

static xcb_render_picture_t create_pen(nss_context_t *con, nss_window_t *win, xcb_render_color_t *color) {
    static xcb_pixmap_t pid = 0;

    if (pid == 0) pid = xcb_generate_id(con->con);

    xcb_create_pixmap(con->con, TRUE_COLOR_ALPHA_DEPTH, pid, win->wid, 1, 1);

    uint32_t values[1] = { XCB_RENDER_REPEAT_NORMAL };
    xcb_render_picture_t pic = xcb_generate_id(con->con);
    xcb_render_create_picture(con->con, pic, pid, con->pfargb, XCB_RENDER_CP_REPEAT, values);

    xcb_rectangle_t rect = { .x = 0, .y = 0, .width = 1, .height = 1 };
    xcb_render_fill_rectangles(con->con, XCB_RENDER_PICT_OP_OVER, pic, *color, 1, &rect);

    xcb_free_pixmap(con->con, pid);

    return pic;
}

void nss_window_draw_cursor(nss_context_t *con, nss_window_t *win, int16_t x, int16_t y, nss_cell_t *cell) {
    int16_t cx = x, cy = y;
    x = x * win->char_width;
    y = y * (win->char_height + win->char_depth) + win->char_height;
    xcb_rectangle_t rects[4] = {
        {x,y-win->char_height,1,win->char_height+win->char_depth},
        {x,y-win->char_height,win->char_width, 1},
        {x+win->char_width-1,y-win->char_height,1,win->char_height+win->char_depth},
        {x,y+win->char_depth-1,win->char_width, 1}
    };
    size_t off = 0, count = 4;
    nss_cell_t cel = *cell;
    if (win->focused) {
        if (win->cursor_type == nss_cursor_bar) {
            count = 1;
            rects[0].width = win->cursor_width;
        } else if (win->cursor_type == nss_cursor_underline) {
            count = 1;
            off = 3;
            rects[3].height = win->cursor_width;
            rects[3].y -= win->cursor_width - 1;
        } else {
            count = 0;
            cel.attrs ^= nss_attrib_inverse;
        }
    }
    nss_window_draw(con, win, cx, cy, &cel, 1);
    xcb_render_color_t c = MAKE_COLOR(nss_color_get(win->cursor_fg_cid));
    xcb_render_fill_rectangles(con->con, XCB_RENDER_PICT_OP_OVER, win->pic, c, count, rects + off);
    xcb_flush(con->con);
}

/* Draw line with attributes */
/* TODO: Implement query caching */
void nss_window_draw(nss_context_t *con, nss_window_t *win, int16_t x, int16_t y, nss_cell_t *cells, size_t len) {
    x = x * win->char_width;
    y = y * (win->char_height + win->char_depth) + win->char_height;
    if (!cells || !len) return;

    for (size_t i = 0; i < len; i++) {
        if (NSS_CELL_CHAR(cells[i]) > ASCII_MAX) {
            uint8_t fattr = NSS_CELL_ATTRS(cells[i]) & nss_font_attrib_mask;
            nss_glyph_t *glyph = nss_font_render_glyph(win->font, NSS_CELL_CHAR(cells[i]), fattr, win->lcd_mode);
            //In case of non-monospace fonts
            glyph->x_off = win->char_width - glyph->x;
            register_glyph(con, win, NSS_CELL_GLYPH(cells[i]), glyph);
            free(glyph);
        }
    }
    while (len > 0) {
        uint8_t attr = cells->attrs;
        uint8_t fattr = attr & nss_font_attrib_mask;
        nss_cid_t bgc = cells->bg, fgc = cells->fg;

        if (attr & nss_attrib_inverse)
            SWAP(nss_cid_t, fgc, bgc);

        xcb_render_color_t fg = MAKE_COLOR(nss_color_get(fgc));
        xcb_render_color_t bg = MAKE_COLOR(nss_color_get(bgc));
        xcb_render_picture_t pen = create_pen(con, win, &fg);

        size_t blk_len = 1;
        while (blk_len < len && NSS_EQCELL(cells[blk_len], cells[blk_len - 1]))
            blk_len++;

        xcb_rectangle_t rect = {
            .x = x, .y = y-win->char_height,
            .width = win->char_width*blk_len,
            .height = win->char_height+win->char_depth
        };

        xcb_rectangle_t lines[2] = {
            { .x = x, .y= y + 1, .width = win->char_width*blk_len, .height = win->underline_width },
            { .x = x, .y= y - win->char_height/3, .width = win->char_width*blk_len, .height = win->underline_width }
        };

        size_t messages = (blk_len + CHARS_PER_MESG - 1)/CHARS_PER_MESG;
        size_t data_len = messages*sizeof(nss_glyph_mesg_t) + blk_len*sizeof(uint32_t);
        uint32_t *data = malloc(data_len);
        uint32_t *off = data;
        nss_glyph_mesg_t msg = {.dx=x,.dy=y};

        for (size_t msgi = 0,chari = 0; msgi < messages; msgi++, chari += CHARS_PER_MESG) {
                msg.len = (blk_len - chari < CHARS_PER_MESG ? blk_len - chari : CHARS_PER_MESG);
            memcpy(off, &msg, sizeof(msg));
            off += sizeof(msg) / sizeof(uint32_t);
            for (size_t j = 0; j < msg.len; j++)
                off[j] = NSS_CELL_CHAR(cells[chari + j]) | (fattr << 24);
            off += msg.len;
            //Reset for all, except first
            msg.dx = msg.dy = 0;
        }

        xcb_render_fill_rectangles(con->con, XCB_RENDER_PICT_OP_OVER, win->pic, bg, 1, &rect);

        if (attr & (nss_attrib_underlined|nss_attrib_strikethrough)) {
            size_t count = !!(attr & nss_attrib_underlined) + !!(attr & nss_attrib_strikethrough);
            size_t offset = !(attr & nss_attrib_underlined) && !!(attr & nss_attrib_strikethrough);
            xcb_render_fill_rectangles(con->con, XCB_RENDER_PICT_OP_OVER, win->pic, fg, count, lines+offset);
        }

        xcb_render_composite_glyphs_32(con->con, XCB_RENDER_PICT_OP_OVER,
                                       pen, win->pic, win->pfglyph, win->gsid,
                                       0, 0, data_len, (uint8_t*)data);

        free(data);
        xcb_render_free_picture(con->con, pen);
        cells += blk_len;
        len -= blk_len;
        x += blk_len * win->char_width;
    }

}

void nss_window_draw_commit(nss_context_t *con, nss_window_t *win) {
    xcb_flush(con->con);
}

static void redraw_damage(nss_context_t *con, nss_window_t *win, nss_rect_t damage) {

        int16_t width = win->cw * win->char_width + win->left_border;
        int16_t height = win->ch * (win->char_height + win->char_depth) + win->top_border;

        size_t num_damaged = 0;
        nss_rect_t damaged[NUM_BORDERS], borders[NUM_BORDERS] = {
            {0, 0, win->left_border, height},
            {win->left_border, 0, width, win->top_border},
            {width, win->top_border, win->width - width, win->height},
            {0, height, width, win->height - height},
        };
        for (size_t i = 0; i < NUM_BORDERS; i++)
            if (intersect_with(&borders[i],&damage))
                    damaged[num_damaged++] = borders[i];
        if (num_damaged)
            xcb_poly_fill_rectangle(con->con, win->wid, win->gc, num_damaged, (xcb_rectangle_t*)damaged);

        nss_rect_t inters = { win->left_border, win->top_border, width, height };
        if (intersect_with(&inters, &damage)) {
            xcb_copy_area(con->con, win->pid, win->wid, win->gc, inters.x - win->left_border, inters.y - win->top_border,
                          inters.x, inters.y, inters.width, inters.height);
        }
}

/* Redraw a region of window specified by <damage[len]> in terminal coordinates */
void nss_window_update(nss_context_t *con, nss_window_t *win, size_t len, const nss_rect_t *damage) {
    for (size_t i = 0; i < len; i++) {
        nss_rect_t rect = damage[i];
        rect = rect_scale_up(rect, win->char_width, win->char_height + win->char_depth);
        rect = rect_shift(rect, win->left_border, win->top_border);
        xcb_copy_area(con->con, win->pid, win->wid, win->gc,
                      rect.x - win->left_border, rect.y - win->top_border,
                      rect.x, rect.y, rect.width, rect.height);
    }
}

void nss_window_clear(nss_context_t *con, nss_window_t *win, size_t len, const nss_rect_t *damage) {
    nss_rect_t *rects = malloc(len*sizeof(nss_rect_t));
    for (size_t i = 0; i < len; i++)
        rects[i] = rect_scale_up(damage[i], win->char_width, win->char_height + win->char_depth);

    xcb_render_color_t color = MAKE_COLOR(nss_color_get(win->bg_cid));
    xcb_render_fill_rectangles(con->con, XCB_RENDER_PICT_OP_OVER, win->pic, color, len, (xcb_rectangle_t*)rects);
    free(rects);
}

void nss_window_set(nss_context_t *con, nss_window_t *win, nss_wc_tag_t tag, const uint32_t *values) {
    set_config(win, tag, values);
    if (tag & (nss_wc_font_size | nss_wc_lcd_mode))
        reload_font(con, win, 1);
    if (tag & (nss_wc_cursor_background | nss_wc_cursor_foreground | nss_wc_background | nss_wc_foreground)) {
        //@@TODO Test this
        // Do this also for nss_color_set
        uint32_t values2[2] = { nss_color_get(win->bg_cid), nss_color_get(win->bg_cid) };
        xcb_change_window_attributes(con->con, win->wid, XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL, values2);
        xcb_change_gc(con->con, win->gc, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, values2);
    }
    nss_term_redraw(win->term, (nss_rect_t) {0,0,win->cw, win->ch});
    redraw_damage(con, win, (nss_rect_t) {0,0,win->width, win->height});
    xcb_flush(con->con);
}

void nss_window_set_font(nss_context_t *con, nss_window_t *win, const char * name) {
    if (!name) {
        warn("Empty font name");
        return;
    }
    free(win->font_name);
    win->font_name = strdup(name);
    reload_font(con, win, 1);
    nss_term_redraw(win->term, (nss_rect_t) {0,0,win->cw, win->ch});
    redraw_damage(con, win, (nss_rect_t) {0,0,win->width, win->height});
    xcb_flush(con->con);
}

nss_font_t *nss_window_get_font(nss_context_t *con, nss_window_t *win) {
    return win->font;
}

char *nss_window_get_font_name(nss_context_t *con, nss_window_t *win) {
    return win->font_name;
}

uint32_t nss_window_get(nss_context_t *con, nss_window_t *win, nss_wc_tag_t tag) {
    if (tag & nss_wc_cusror_width) return win->cursor_width;
    if (tag & nss_wc_left_border) return win->left_border;
    if (tag & nss_wc_top_border) return win->top_border;
    if (tag & nss_wc_background) return win->bg_cid;
    if (tag & nss_wc_foreground) return win->fg_cid;
    if (tag & nss_wc_cursor_background) return win->cursor_bg_cid;
    if (tag & nss_wc_cursor_foreground) return win->cursor_fg_cid;
    if (tag & nss_wc_cursor_type) return win->cursor_type;
    if (tag & nss_wc_lcd_mode) return win->lcd_mode;
    if (tag & nss_wc_font_size) return win->font_size;
    warn("Invalid option");
    return 0;
}

static void handle_resize(nss_context_t *con, nss_window_t *win, int16_t width, int16_t height) {

    _Bool redraw_borders = width < win->width || height < win->height;
    //Handle resize

    win->width = width;
    win->height = height;

    int16_t new_cw = MAX(1,(win->width - 2*win->left_border)/win->char_width);
    int16_t new_ch = MAX(1,(win->height - 2*win->top_border)/(win->char_height+win->char_depth));
    int16_t delta_x = new_cw - win->cw;
    int16_t delta_y = new_ch - win->ch;
    win->cw = new_cw;
    win->ch = new_ch;

    if (delta_x || delta_y) {

        int16_t width = win->cw * win->char_width;
        int16_t height = win->ch * (win->char_height + win->char_depth);
        int16_t common_w = MIN(width, width  - delta_x * win->char_width);
        int16_t common_h = MIN(height, height - delta_y * (win->char_height + win->char_depth)) ;

        xcb_pixmap_t pid = xcb_generate_id(con->con);
        xcb_create_pixmap(con->con, TRUE_COLOR_ALPHA_DEPTH, pid, win->wid, width, height);
        xcb_render_picture_t pic = xcb_generate_id(con->con);
        uint32_t mask3 = XCB_RENDER_CP_GRAPHICS_EXPOSURE | XCB_RENDER_CP_POLY_EDGE | XCB_RENDER_CP_POLY_MODE;
        uint32_t values3[3] = { 0, XCB_RENDER_POLY_EDGE_SMOOTH, XCB_RENDER_POLY_MODE_IMPRECISE };
        xcb_render_create_picture(con->con, pic, pid, con->pfargb, mask3, values3);

        xcb_render_composite(con->con, XCB_RENDER_PICT_OP_OVER, win->pic, 0, pic, 0, 0, 0, 0, 0, 0, common_w, common_h);

        SWAP(xcb_pixmap_t, win->pid, pid);
        SWAP(xcb_render_picture_t, win->pic, pic);
        xcb_free_pixmap(con->con, pid);
        xcb_render_free_picture(con->con, pic);

        nss_rect_t rectv[2];
        size_t rectc= 0;

        if (delta_y > 0)
            rectv[rectc++] = (nss_rect_t) { 0, win->ch - delta_y, MIN(win->cw, win->cw - delta_x), delta_y };
        if (delta_x > 0)
            rectv[rectc++] = (nss_rect_t) { win->cw - delta_x, 0, delta_x, MAX(win->ch, win->ch - delta_y) };

        nss_term_resize(win->term, win->cw, win->ch);

        for (size_t i = 0; i < rectc; i++)
            nss_term_redraw(win->term, rectv[i]);
        nss_window_update(con, win, rectc, rectv);
    }

    if (redraw_borders) { //Update borders
        int16_t width = win->cw * win->char_width + win->left_border;
        int16_t height = win->ch * (win->char_height + win->char_depth) + win->top_border;
        redraw_damage(con,win, (nss_rect_t) {width, 0, win->width - width, win->height});
        redraw_damage(con,win, (nss_rect_t) {0, height, width, win->height - height});
        //TIP: May be redraw all borders here
    }

}

static void handle_focus(nss_context_t *con, nss_window_t *win, _Bool focused) {
    win->focused = focused;
    nss_term_focus(win->term, focused);
}

/* Start window logic, handling all windows in context */
void nss_context_run(nss_context_t *con) {

    for (;;) {
        if(poll(con->pfds, con->pfdcap, -1) < 0 && errno != EINTR)
            warn("Poll error: %s", strerror(errno));
        if (con->pfds[0].revents & POLLIN) {
            xcb_generic_event_t *event;
            while ((event = xcb_poll_for_event(con->con))) {
                switch (event->response_type &= 0x7f) {
                case XCB_EXPOSE:{
                    xcb_expose_event_t *ev = (xcb_expose_event_t*)event;
                    nss_window_t *win = nss_window_for_xid(con,ev->window);
                    if (!win) break;

                    nss_rect_t damage = {ev->x, ev->y, ev->width, ev->height};
                    //info("Damage: %d %d %d %d", damage.x, damage.y, damage.width, damage.height);

                    redraw_damage(con,win, damage);
                    xcb_flush(con->con);
                    break;
                }
                case XCB_CONFIGURE_NOTIFY:{
                    xcb_configure_notify_event_t *ev = (xcb_configure_notify_event_t*)event;
                    nss_window_t *win = nss_window_for_xid(con, ev->window);
                    if (!win) break;

                    if (ev->width != win->width || ev->height != win->height) {
                        handle_resize(con,win, ev->width, ev->height);
                        xcb_flush(con->con);
                    }
                    win->got_configure |= 1;
                    break;
                }
                case XCB_KEY_RELEASE: /* ignore */ break;
                case XCB_KEY_PRESS:{
                    xcb_key_release_event_t *ev = (xcb_key_release_event_t*)event;
                    nss_window_t *win = nss_window_for_xid(con, ev->event);
                    if (!win) break;

                    /* That's just a temporal solution */

                    xkb_keycode_t keycode = ev->detail;
                    uint32_t rune = xkb_state_key_get_utf32(con->xkb_state, keycode);
                    if (rune == '1') {
                        uint32_t arg = win->font_size + 2;
                        nss_window_set(con, win, nss_wc_font_size, &arg);
                    } else if (rune == '2') {
                        uint32_t arg = win->font_size - 2;
                        nss_window_set(con, win, nss_wc_font_size, &arg);
                    } else if (rune == '3') {
                        uint32_t arg = !win->lcd_mode;
                        nss_window_set(con, win, nss_wc_lcd_mode, &arg);
                    } else if (rune == '4') {
                        uint32_t arg = win->font_size;
                        nss_create_window(con,(nss_rect_t) {100,100,400,200}, win->font_name, nss_wc_font_size, &arg);
                    } else {
                        uint8_t buf[8];
                        size_t sz = xkb_state_key_get_utf8(con->xkb_state, keycode, NULL, 0);
                        if (sz > 0 && sz < 8) {
                            xkb_state_key_get_utf8(con->xkb_state, keycode, (char *)buf, 7);
                            info("Got key: '%s'", buf);
                            nss_term_write(win->term, buf, sz, 1);
                        }
                    }
                    break;
                }
                case XCB_FOCUS_IN:
                case XCB_FOCUS_OUT:{
                    xcb_focus_in_event_t *ev = (xcb_focus_in_event_t*)event;
                    nss_window_t *win = nss_window_for_xid(con,ev->event);
                    if (!win) break;

                    handle_focus(con,win,event->response_type == XCB_FOCUS_IN);
                    xcb_flush(con->con);
                    break;
                }
                case XCB_CLIENT_MESSAGE: {
                    xcb_client_message_event_t *ev = (xcb_client_message_event_t*)event;
                    nss_window_t *win = nss_window_for_xid(con, ev->window);
                    if (!win) break;
                    if (ev->format == 32 && ev->data.data32[0] == con->atom_wm_delete_window) {
                        nss_free_window(con, win);
                        if (!con->first && !con->daemon_mode) {
                            free(event);
                            return;
                        }
                    }
                    break;
                }
                case XCB_VISIBILITY_NOTIFY: {
                    xcb_visibility_notify_event_t *ev = (xcb_visibility_notify_event_t*)event;
                    nss_window_t *win = nss_window_for_xid(con, ev->window);
                    if (!win) break;
                    win->active = ev->state != XCB_VISIBILITY_FULLY_OBSCURED;
                    nss_term_visibility(win->term, win->active);
                    break;
                }
                case XCB_MAP_NOTIFY:
                case XCB_UNMAP_NOTIFY: {
                    xcb_map_notify_event_t *ev = (xcb_map_notify_event_t *)event;
                    nss_window_t *win = nss_window_for_xid(con, ev->window);
                    if (!win) break;
                    win->active = ev->response_type == XCB_MAP_NOTIFY;
                    nss_term_visibility(win->term, win->active);
                    break;
                }
                case XCB_DESTROY_NOTIFY: {
                   break;
                }
                default:
                    if (event->response_type == con->xkb_base_event) {
                        struct _xkb_any_event {
                            uint8_t response_type;
                            uint8_t xkbType;
                            uint16_t sequence;
                            xcb_timestamp_t time;
                            uint8_t deviceID;
                        } *xkb_ev;

                        xkb_ev = (struct _xkb_any_event*)event;
                        if (xkb_ev->deviceID == con->xkb_core_kbd) {
                            switch (xkb_ev->xkbType) {
                            case XCB_XKB_NEW_KEYBOARD_NOTIFY : {
                                xcb_xkb_new_keyboard_notify_event_t *ev = (xcb_xkb_new_keyboard_notify_event_t*)event;
                                if (ev->changed & XCB_XKB_NKN_DETAIL_KEYCODES)
                                    update_keymap(con);
                                break;
                            }
                            case XCB_XKB_MAP_NOTIFY: {
                                update_keymap(con);
                                break;
                            }
                            case XCB_XKB_STATE_NOTIFY: {
                                xcb_xkb_state_notify_event_t *ev = (xcb_xkb_state_notify_event_t*)event;
                                xkb_state_update_mask(con->xkb_state, ev->baseMods, ev->latchedMods, ev->lockedMods,
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
        for(size_t i = 1; i < con->pfdcap; i++) {
            if(con->pfds[i].fd > 0) {
                nss_window_t *win = nss_window_for_term_fd(con, con->pfds[i].fd);
                if(con->pfds[i].revents & POLLIN & win->got_configure) {
                    nss_term_read(win->term);
                } else if(con->pfds[i].revents & (POLLERR | POLLNVAL | POLLHUP)) {
                    nss_free_window(con, win);
                }
            }
        }
    }
}
