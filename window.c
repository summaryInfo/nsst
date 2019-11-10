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
#include "input.h"

#define TRUE_COLOR_ALPHA_DEPTH 32
#define NUM_BORDERS 4
#define INIT_PFD_NUM 16

#define WORDS_IN_MESSAGE 256
#define HEADER_WORDS ((sizeof(nss_glyph_mesg_t)+sizeof(uint32_t))/sizeof(uint32_t))
#define CHARS_PER_MESG (WORDS_IN_MESSAGE - HEADER_WORDS)

#define CB(c) (((c) & 0xff) * 0x100)
#define CG(c) ((((c) >> 8) & 0xff) * 0x100)
#define CR(c) ((((c) >> 16) & 0xff) * 0x100)
#define CA(c) ((((c) >> 24) & 0xff) * 0x100)
#define MAKE_COLOR(c) {.red=CR(c), .green=CG(c), .blue=CB(c), .alpha=CA(c)}

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
    unsigned appkey : 1;
    unsigned appcursor : 1;
    unsigned numlock : 1;
    unsigned keylock : 1;
    unsigned has_meta : 1;
    unsigned meta_escape : 1;
    unsigned bs_del : 1;
    unsigned del_del : 1;
    unsigned reverse_video : 1;
    unsigned mouse_events : 1;

    int16_t width;
    int16_t height;
    int16_t cw, ch;
    int16_t cursor_width;
    int16_t underline_width;
    int16_t left_border;
    int16_t top_border;
    int16_t font_size;
    uint32_t blink_time;
    struct timespec prev_blink;
    struct timespec prev_draw;

    nss_color_t bg;
    nss_color_t fg;
    nss_color_t cursor_fg;
    nss_color_t cursor_bg;
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

    uint8_t *render_buffer;
    size_t render_buffer_size;
    uint8_t *mark_buffer;
    size_t mark_buffer_size;
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
        warn("X11 error: %"PRIu8" %"PRIu16" %"PRIu8, err->major_code, err->minor_code, err->error_code);
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

int key_cmpfn(const void *a, const void *b) {
    const nss_ckey_t *ka = a;
    const nss_ckey_t *kb = b;
    return (long long) ka->ksym - kb->ksym;
}

/* Initialize global state object */
void nss_init_context(void) {
    con.daemon_mode = 0;

    con.render_buffer = malloc(WORDS_IN_MESSAGE * sizeof(uint32_t));
    if (!con.render_buffer) die("Can't allocate render_buffer");
    con.render_buffer_size = WORDS_IN_MESSAGE * sizeof(uint32_t);

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

    qsort(ckeys, sizeof(ckeys)/sizeof(*ckeys), sizeof(*ckeys), key_cmpfn);
}

void nss_window_set_title(nss_window_t *win, const char *title) {
    if (!title) title = "Not So Simple Terminal";
    xcb_change_property(con.con, XCB_PROP_MODE_REPLACE, win->wid, XCB_ATOM_WM_NAME, con.atom_utf8_string, 8, strlen(title), title);
    xcb_change_property(con.con, XCB_PROP_MODE_REPLACE, win->wid, con.atom_net_wm_name, con.atom_utf8_string, 8, strlen(title), title);
}

/* Free all resources */
void nss_free_context(void) {
    while (con.first)
        nss_free_window(con.first);
    xkb_state_unref(con.xkb_state);
    xkb_keymap_unref(con.xkb_keymap);
    xkb_context_unref(con.xkb_ctx);

    free(con.render_buffer);
    free(con.mark_buffer);
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
    if (tag & nss_wc_foreground) win->fg = *values++;
    if (tag & nss_wc_cursor_background) win->cursor_bg = *values++;
    if (tag & nss_wc_cursor_foreground) win->cursor_fg = *values++;
    if (tag & nss_wc_cursor_type) win->cursor_type = *values++;
    if (tag & nss_wc_subpixel_fonts) win->subpixel_fonts = *values++;
    if (tag & nss_wc_font_size) win->font_size = *values++;
    if (tag & nss_wc_underline_width) win->underline_width = *values++;
    if (tag & nss_wc_width) warn("Tag is not settable"), values++;
    if (tag & nss_wc_height) warn("Tag is not settable"), values++;
    if (tag & nss_wc_appcursor) win->appcursor = *values++;
    if (tag & nss_wc_appkey) win->appkey = *values++;
    if (tag & nss_wc_numlock) win->numlock = *values++;
    if (tag & nss_wc_keylock) win->keylock = *values++;
    if (tag & nss_wc_has_meta) win->has_meta = *values++;
    if (tag & nss_wc_meta_escape) win->meta_escape = *values++;
    if (tag & nss_wc_bs_del) win->bs_del = *values++;
    if (tag & nss_wc_del_del) win->del_del = *values++;
    if (tag & nss_wc_blink_time) win->blink_time = *values++;
    if (tag & nss_wc_reverse) win->reverse_video = *values++;
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
        nss_create_font(win->font_name, win->font_size, nss_context_get_dpi());
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
        nss_glyph_t *glyphs['~' - ' ' + 1][nss_font_attrib_max] = { NULL };
        int16_t total = 0, maxd = 0, maxh = 0;
        for (uint32_t i = ' '; i <= '~'; i++) {
            for (size_t j = 0; j < nss_font_attrib_max; j++)
                glyphs[i - ' '][j] = nss_font_render_glyph(win->font, i, j, win->subpixel_fonts);

            total += glyphs[i - ' '][0]->x_off;
            maxd = MAX(maxd, glyphs[i - ' '][0]->height - glyphs[i - ' '][0]->y);
            maxh = MAX(maxh, glyphs[i - ' '][0]->y);
        }

        // TODO Make character width adjustment configurable
        win->char_width = (total - 1) / ('~' - ' ' + 1);
        win->char_height = maxh;
        win->char_depth = maxd;

        for (uint32_t i = ' '; i <= '~'; i++) {
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
    uint32_t values2[3] = { win->reverse_video ? win->fg : win->bg,
                            win->reverse_video ? win->fg : win->bg, 0 };
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

    xcb_render_color_t color = MAKE_COLOR(win->reverse_video ? win->fg : win->bg);
    xcb_render_fill_rectangles(con.con, XCB_RENDER_PICT_OP_SRC, win->pic, color, 1, &bound);

    if (need_free)
        nss_term_resize(win->term, win->cw, win->ch);
    return 1;
}

static void set_wm_props(nss_window_t *win) {
    uint32_t pid = getpid();
    xcb_change_property(con.con, XCB_PROP_MODE_REPLACE, win->wid, con.atom_net_wm_pid, XCB_ATOM_CARDINAL, 32, 1, &pid);
    xcb_change_property(con.con, XCB_PROP_MODE_REPLACE, win->wid, con.atom_wm_protocols, XCB_ATOM_ATOM, 32, 1, &con.atom_wm_delete_window);
    const char class[] = "nsst\0Nsst";
    xcb_change_property(con.con, XCB_PROP_MODE_REPLACE, win->wid, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, sizeof(class), class);
}

/* Create new window */
nss_window_t *nss_create_window(const char *font_name, nss_wc_tag_t tag, const uint32_t *values) {
    nss_window_t *win = calloc(1, sizeof(nss_window_t));
    win->cursor_width = nss_config_integer(nss_config_cursor_width, 1, 32);
    win->underline_width = nss_config_integer(nss_config_underline_width, 0, 32);
    win->left_border = nss_config_integer(nss_config_left_border, 0, 256);
    win->top_border = nss_config_integer(nss_config_top_border, 0, 256);
    win->bg = nss_config_color(nss_config_bg);
    win->fg = nss_config_color(nss_config_fg);
    win->cursor_bg = nss_config_color(nss_config_cursor_bg);
    win->cursor_fg = nss_config_color(nss_config_cursor_fg);
    win->cursor_type = nss_config_integer(nss_config_cursor_shape, 0, 6);
    win->subpixel_fonts = nss_config_integer(nss_config_subpixel_fonts, 0, 1);
    win->reverse_video = nss_config_integer(nss_config_reverse_video, 0, 1);
    win->appkey = nss_config_integer(nss_config_appkey, 0, 1);
    win->appcursor = nss_config_integer(nss_config_appcursor, 0, 1);
    win->numlock = nss_config_integer(nss_config_numlock, 0, 1);
    win->has_meta = nss_config_integer(nss_config_has_meta, 0, 1);
    win->meta_escape = nss_config_integer(nss_config_meta_escape, 0, 1);
    win->bs_del = nss_config_integer(nss_config_backspace_is_delete, 0, 1);
    win->del_del = nss_config_integer(nss_config_delete_is_delete, 0, 1);
    win->font_size = nss_config_integer(nss_config_font_size, 0, 1000);
    win->active = 1;
    win->numlock = 1;
    win->term_fd = -1;
    win->blink_time = nss_config_integer(nss_config_blink_time, 10000, 10000000);
    if (!font_name) font_name = nss_config_string(nss_config_font_name, "fixed");
    win->font_name = strdup(font_name);
    if (!win->font_name) {
        nss_free_window(win);
        return NULL;
    }
    win->width = nss_config_integer(nss_config_window_width, 0, 32767);
    win->height = nss_config_integer(nss_config_window_height, 0, 32767);
    clock_gettime(CLOCK_MONOTONIC, &win->prev_blink);

    set_config(win, tag, values);

    xcb_void_cookie_t c;

    uint32_t mask1 =  XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL |
        XCB_CW_BIT_GRAVITY | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
    win->ev_mask = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_VISIBILITY_CHANGE |
        XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
        XCB_EVENT_MASK_BUTTON_MOTION | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE;
    if (win->mouse_events) win->ev_mask |= XCB_EVENT_MASK_POINTER_MOTION;
    uint32_t values1[5] = {
        win->reverse_video ? win->fg : win->bg,
        win->reverse_video ? win->fg : win->bg,
        XCB_GRAVITY_NORTH_WEST, win->ev_mask, con.mid
    };
    int16_t x = nss_config_integer(nss_config_window_x, -32768, 32767);
    int16_t y = nss_config_integer(nss_config_window_y, -32768, 32767);
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

    if (!reload_font(win, 0)) {
        warn("Can't create window");
        nss_free_window(win);
        return NULL;
    }

    info("Font size: %d %d %d", win->char_height, win->char_depth, win->char_width);


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
                con.pfds[i + con.pfdn].fd = -1;
                con.pfds[i + con.pfdn].events = 0;
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
    info("Freeing window");
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

/* Get monitor DPI */
uint16_t nss_context_get_dpi(void) {
    xcb_xrm_database_t *xrmdb = xcb_xrm_database_from_default(con.con);
    long dpi = 0;
    if (xrmdb) {
        if (xcb_xrm_resource_get_long(xrmdb, "Xft.dpi", NULL, &dpi) >= 0) {
            xcb_xrm_database_free(xrmdb);
            return dpi;
        }
        xcb_xrm_database_free(xrmdb);
    }
    warn("Can't fetch Xft.dpi, defaulting to highest dpi value");

    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(con.con));
    for (; it.rem; xcb_screen_next(&it))
        if (it.data)
            dpi = MAX(dpi, (it.data->width_in_pixels * 25.4)/it.data->width_in_millimeters);
    if (!dpi) {
        warn("Can't get highest dpi, defaulting to 96");
        dpi = 96;
    }
    return dpi;
}

void nss_window_draw_cursor(nss_window_t *win, int16_t x, int16_t y, nss_cell_t *cell, nss_color_t *pal, nss_color_t *extra) {
    int16_t cx = x, cy = y;
    x = MIN(x, win->cw - 1) * win->char_width;
    y = y * (win->char_height + win->char_depth) + win->char_height;
    xcb_rectangle_t rects[4] = {
        {x, y-win->char_height, 1, win->char_height+win->char_depth},
        {x, y-win->char_height, win->char_width, 1},
        {x+win->char_width-1, y-win->char_height, 1, win->char_height+win->char_depth},
        {x, y+win->char_depth-1, win->char_width, 1}
    };
    size_t off = 0, count = 4;
    nss_cell_t cel = *cell;
    if (win->focused) {
        if (win->cursor_type == nss_cursor_bar) {
            if(win->cw == cx) {
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
            CELL_ATTR_INVERT(cel, nss_attrib_inverse);
        }
    }
    nss_window_draw(win, MIN(cx, win->cw - 1), cy, 1, &cel, pal, extra);
    xcb_render_color_t c = MAKE_COLOR(win->reverse_video ? win->cursor_bg : win->cursor_fg);
    xcb_render_fill_rectangles(con.con, XCB_RENDER_PICT_OP_OVER, win->pic, c, count, rects + off);
}


static inline void eval_color(nss_window_t *win, nss_cell_t cell, nss_color_t *pal, nss_color_t *extra, xcb_render_color_t *fgr, xcb_render_color_t *bgr) {
        uint32_t attr = CELL_ATTR(cell);
        nss_cid_t bgi = cell.bg, fgi = cell.fg;

        if ((attr & (nss_attrib_bold | nss_attrib_faint)) == nss_attrib_bold && fgi < 8) fgi += 8;

        nss_color_t bcolor = bgi < NSS_PALETTE_SIZE ? pal[bgi] : extra[bgi - NSS_PALETTE_SIZE];
        nss_color_t fcolor = fgi < NSS_PALETTE_SIZE ? pal[fgi] : extra[fgi - NSS_PALETTE_SIZE];

        if (win->reverse_video) {
            if (bcolor == win->bg)
                bcolor = win->fg;
            if (fcolor == win->fg)
                fcolor = win->bg;
        }

        xcb_render_color_t fg = MAKE_COLOR(fcolor);
        xcb_render_color_t bg = MAKE_COLOR(bcolor);

        if ((attr & (nss_attrib_bold | nss_attrib_faint)) == nss_attrib_faint)
            fg.red /= 2, fg.green /= 2, fg.blue /= 2;
        if (attr & nss_attrib_inverse) SWAP(xcb_render_color_t, fg, bg);
        if (attr & nss_attrib_invisible || (attr & nss_attrib_blink && win->blink_state)) fg = bg;

        if (fgr) *fgr = fg;
        if (bgr) *bgr = bg;
}

static inline _Bool cell_equal_bg(nss_cell_t a, nss_cell_t b) {
    uint32_t a_attr = CELL_ATTR(a), b_attr = CELL_ATTR(b);
    if (!(a_attr & nss_attrib_inverse) && !(b_attr & nss_attrib_inverse))
        return a.bg == b.bg;

    nss_cid_t a_bg = a.bg;
    if (a_attr & nss_attrib_inverse)
        a_bg = a.fg + 8 * ((a_attr & (nss_attrib_bold | nss_attrib_faint)) == nss_attrib_bold && a.fg < 8);
    nss_cid_t b_bg = b.bg;
    if (b_attr & nss_attrib_inverse)
        b_bg = b.fg + 8 * ((b_attr & (nss_attrib_bold | nss_attrib_faint)) == nss_attrib_bold && b.fg < 8);

    if (a_bg != b_bg) return 0;

    if ((((a_attr & (nss_attrib_bold | nss_attrib_faint)) == nss_attrib_faint) && (a_attr & nss_attrib_inverse)) ||
            (((b_attr & (nss_attrib_bold | nss_attrib_faint)) == nss_attrib_faint) && (b_attr & nss_attrib_inverse))) {
        return ((a_attr & (nss_attrib_bold | nss_attrib_faint | nss_attrib_inverse))
                == (b_attr & (nss_attrib_bold | nss_attrib_faint | nss_attrib_inverse)));
    } else return 1;
}

static inline _Bool cell_equal_fg(nss_cell_t a, nss_cell_t b, _Bool blink) {
    uint32_t a_attr = CELL_ATTR(a), b_attr = CELL_ATTR(b);
    _Bool a_inv = a_attr & (nss_attrib_inverse | nss_attrib_invisible) || (blink && a_attr & nss_attrib_blink);
    _Bool b_inv = b_attr & (nss_attrib_inverse | nss_attrib_invisible) || (blink && b_attr & nss_attrib_blink);
    if (a_inv && b_inv) return a.bg == b.bg;

    nss_cid_t a_bg = a.bg;
    if (!a_inv)
        a_bg = a.fg + 8 * ((a_attr & (nss_attrib_bold | nss_attrib_faint)) == nss_attrib_bold && a.fg < 8);
    nss_cid_t b_bg = b.bg;
    if (!b_inv)
        b_bg = b.fg + 8 * ((b_attr & (nss_attrib_bold | nss_attrib_faint)) == nss_attrib_bold && b.fg < 8);

    if (a_bg != b_bg) return 0;

    if ((((a_attr & (nss_attrib_bold | nss_attrib_faint)) == nss_attrib_faint) && !a_inv) ||
            (((b_attr & (nss_attrib_bold | nss_attrib_faint)) == nss_attrib_faint) && !b_inv)) {
        return ((a_attr & (nss_attrib_bold | nss_attrib_faint)) == (b_attr & (nss_attrib_bold | nss_attrib_faint))) && (a_inv == b_inv);
    } else return 1;
}

/* Draw line with attributes */
/* TODO Draw multiple lines at a time -- or I should not do that since it's then quadratic */
void nss_window_draw(nss_window_t *win, int16_t x, int16_t y, size_t len, nss_cell_t *cells, nss_color_t *pal, nss_color_t *extra) {
    x = x * win->char_width;
    y = y * (win->char_height + win->char_depth) + win->char_height;
    if (!cells || !len) return;

    xcb_rectangle_t clip = {0, y - win->char_height, win->char_width * win->cw, win->char_depth + win->char_height};
    xcb_render_set_picture_clip_rectangles(con.con, win->pic, 0, 0, 1, &clip);

    for (size_t i = 0; i < len; i++) {
        uint32_t ch = CELL_CHAR(cells[i]);
        if (!nss_font_glyph_is_loaded(win->font, ch)) {
            for (size_t j = 0; j < nss_font_attrib_max; j++) {
                nss_glyph_t *glyph = nss_font_render_glyph(win->font, ch, j, win->subpixel_fonts);
                //In case of non-monospace fonts
                glyph->x_off = win->char_width;
                register_glyph(win, ch | (j << 24) , glyph);
                free(glyph);
            }
        }
    }

    if (con.mark_buffer_size < len) {
        uint8_t *new = realloc(con.mark_buffer, len);
        if (!new) return;
        con.mark_buffer = new;
        con.mark_buffer_size = len;
    }

    memset(con.mark_buffer, 0, con.mark_buffer_size);

    for (size_t i = 0; i < len; ) {
        xcb_render_color_t bg;
        eval_color(win, cells[i], pal, extra, NULL, &bg);

        xcb_rectangle_t *rects = (xcb_rectangle_t *)con.render_buffer;
        xcb_rectangle_t *rend = rects + con.render_buffer_size / sizeof(xcb_rectangle_t), *rpos = rects;
        for (size_t j = i; j < len; ) {
            size_t blk_len = 0;
            while (j + blk_len < len && cell_equal_bg(cells[i], cells[j + blk_len]))
                con.mark_buffer[j + blk_len++] = 1;

            if (rpos + 1 >= rend) {
                size_t new_size = MAX(3 * con.render_buffer_size / 2, 16 * sizeof(xcb_rectangle_t));
                uint8_t *new = realloc(con.render_buffer, new_size);
                if (!new) return;
                con.render_buffer = new;
                con.render_buffer_size = new_size;
                rpos = (xcb_rectangle_t *)con.render_buffer + (rpos - rects);
                rects = (xcb_rectangle_t *)con.render_buffer;
                rend = rects + new_size / sizeof(xcb_rectangle_t);
            }

            *rpos++ = (xcb_rectangle_t) {
                .x = x + j * win->char_width, .y = y - win->char_height,
                .width = blk_len * win->char_width,
                .height = win->char_depth + win->char_height
            };

            j += blk_len;
            while (j < len && !cell_equal_bg(cells[i], cells[j])) j++;
        }

        xcb_render_fill_rectangles(con.con, XCB_RENDER_PICT_OP_SRC, win->pic, bg, rpos - rects, rects);

        while(i < len && con.mark_buffer[i]) i++;
    }

    memset(con.mark_buffer, 0, con.mark_buffer_size);

    for (size_t i = 0; i < len; ) {
        xcb_render_color_t fg;
        eval_color(win, cells[i], pal, extra, &fg, NULL);

        xcb_rectangle_t rect2 = { .x = 0, .y = 0, .width = 1, .height = 1 };
        xcb_render_fill_rectangles(con.con, XCB_RENDER_PICT_OP_SRC, win->pen, fg, 1, &rect2);

        nss_glyph_mesg_t *msg_head = (nss_glyph_mesg_t *)con.render_buffer;
        uint8_t *bpos = con.render_buffer + sizeof(nss_glyph_mesg_t);
        uint8_t *bend = con.render_buffer + con.render_buffer_size;
        size_t msg_count = 0, jump = 0;

        *msg_head = (nss_glyph_mesg_t) { .dx = x + i * win->char_width, .dy = y };

        for (size_t j = i; j < len; j++) {
            if (!con.mark_buffer[j] && cell_equal_fg(cells[i], cells[j], win->blink_state)) {
                con.mark_buffer[j] = 1;
                size_t inc = sizeof(uint32_t);
                if ((size_t)msg_head->len + 1 > CHARS_PER_MESG || jump) inc += sizeof(nss_glyph_mesg_t);
                if (bend - bpos <= (ssize_t)inc) {
                    size_t new_size = MAX(3 * con.render_buffer_size / 2, 128 * sizeof(uint32_t));
                    uint8_t *new = realloc(con.render_buffer, new_size);
                    if (!new) return;
                    msg_head = (nss_glyph_mesg_t *)(new + ((uint8_t *)msg_head - con.render_buffer));
                    bpos = new + (bpos - con.render_buffer);
                    bend = new + new_size;
                    con.render_buffer = new;
                    con.render_buffer_size = new_size;
                }
                if ((size_t)msg_head->len + 1 > CHARS_PER_MESG || jump) {
                    msg_count++;
                    msg_head = (nss_glyph_mesg_t *)bpos;
                    *msg_head = (nss_glyph_mesg_t) { .dx = jump * win->char_width };
                    bpos += sizeof(msg_head);
                    jump = 0;
                }
                uint32_t ch = CELL_CHAR(cells[j]) | ((CELL_ATTR(cells[j]) & nss_font_attrib_mask) << 24);
                memcpy(bpos, &ch, sizeof(ch));
                bpos += sizeof(ch);
                msg_head->len++;
            } else jump++;
        }
        xcb_render_composite_glyphs_32(con.con, XCB_RENDER_PICT_OP_OVER,
                                       win->pen, win->pic, win->pfglyph, win->gsid,
                                       0, 0, bpos - con.render_buffer, con.render_buffer);
        while(i < len && con.mark_buffer[i]) i++;
    }

    memset(con.mark_buffer, 0, con.mark_buffer_size);
    for (size_t i = 0; i < len; i++) {
        while(i < len && (con.mark_buffer[i] || (CELL_ATTR(cells[i]) & nss_attrib_invisible) ||
                (CELL_ATTR(cells[i]) & nss_attrib_blink && win->blink_state) || !(CELL_ATTR(cells[i]) & (nss_attrib_strikethrough | nss_attrib_underlined)))) i++;
        if (i >= len) break;

        xcb_render_color_t fg;
        eval_color(win, cells[i], pal, extra, &fg, NULL);

        xcb_rectangle_t *rects = (xcb_rectangle_t *)con.render_buffer;
        xcb_rectangle_t *rend = rects + con.render_buffer_size / sizeof(xcb_rectangle_t), *rpos = rects;
        for (size_t j = i; j < len; ) {
            while (j < len && (!cell_equal_fg(cells[i], cells[j], win->blink_state) ||
                    !(CELL_ATTR(cells[j]) & nss_attrib_underlined))) j++;
            if (j >= len) break;

            size_t blk_len = 0;
            while (j + blk_len < len && cell_equal_fg(cells[i], cells[j + blk_len], win->blink_state) &&
                (CELL_ATTR(cells[j + blk_len]) & nss_attrib_underlined))
                con.mark_buffer[j + blk_len++] = 1;

            if (rpos + 1 >= rend) {
                size_t new_size = MAX(3 * con.render_buffer_size / 2, 16 * sizeof(xcb_rectangle_t));
                uint8_t *new = realloc(con.render_buffer, new_size);
                if (!new) return;
                con.render_buffer = new;
                con.render_buffer_size = new_size;
                rpos = (xcb_rectangle_t *)con.render_buffer + (rpos - rects);
                rects = (xcb_rectangle_t *)con.render_buffer;
                rend = rects + new_size / sizeof(xcb_rectangle_t);
            }

            *rpos++ = (xcb_rectangle_t) {
                .x = x + j * win->char_width, .y = y + 1,
                .width = blk_len * win->char_width,
                .height = win->underline_width
            };

            j += blk_len;
        }
        for (size_t j = i; j < len; ) {
            while (j < len && (!cell_equal_fg(cells[i], cells[j], win->blink_state) ||
                    !(CELL_ATTR(cells[j]) & nss_attrib_strikethrough))) j++;
            if (j >= len) break;

            size_t blk_len = 0;
            while (j + blk_len < len && cell_equal_fg(cells[i], cells[j + blk_len], win->blink_state) &&
                (CELL_ATTR(cells[j + blk_len]) & nss_attrib_strikethrough))
                con.mark_buffer[j + blk_len++] = 1;

            if (rpos + 1 >= rend) {
                size_t new_size = MAX(3 * con.render_buffer_size / 2, 16 * sizeof(xcb_rectangle_t));
                uint8_t *new = realloc(con.render_buffer, new_size);
                if (!new) return;
                con.render_buffer = new;
                con.render_buffer_size = new_size;
                rpos = (xcb_rectangle_t *)con.render_buffer + (rpos - rects);
                rects = (xcb_rectangle_t *)con.render_buffer;
                rend = rects + new_size / sizeof(xcb_rectangle_t);
            }

            *rpos++ = (xcb_rectangle_t) {
                .x = x + j * win->char_width, .y = y - win->char_height/3,
                .width = blk_len * win->char_width,
                .height = win->underline_width
            };

            j += blk_len;
        }

        xcb_render_fill_rectangles(con.con, XCB_RENDER_PICT_OP_SRC, win->pic, fg, rpos - rects, rects);

    }

    clip = (xcb_rectangle_t) {0, 0, win->cw * win->char_width, win->ch * (win->char_height + win->char_depth)};
    xcb_render_set_picture_clip_rectangles(con.con, win->pic, 0, 0, 1, &clip);

}

static void redraw_damage(nss_window_t *win, nss_rect_t damage) {

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

        nss_rect_t inters = { win->left_border, win->top_border, width, height };
        if (intersect_with(&inters, &damage)) {
            xcb_copy_area(con.con, win->pid, win->wid, win->gc, inters.x - win->left_border, inters.y - win->top_border,
                          inters.x, inters.y, inters.width, inters.height);
        }
}

/* Redraw a region of window specified by <damage[len]> in terminal coordinates */
void nss_window_update(nss_window_t *win, size_t len, const nss_rect_t *damage) {
    for (size_t i = 0; i < len; i++) {
        nss_rect_t rect = damage[i];
        rect = rect_scale_up(rect, win->char_width, win->char_height + win->char_depth);
        rect = rect_shift(rect, win->left_border, win->top_border);
        xcb_copy_area(con.con, win->pid, win->wid, win->gc,
                      rect.x - win->left_border, rect.y - win->top_border,
                      rect.x, rect.y, rect.width, rect.height);
    }
}

void nss_window_shift(nss_window_t *win, int16_t ys, int16_t yd, int16_t height) {
    ys = MAX(0, MIN(ys, win->ch));
    yd = MAX(0, MIN(yd, win->ch));
    height = MIN(height, MIN(win->ch - ys, win->ch - yd));

    if (!height) return;

    ys *= win->char_height + win->char_depth;
    yd *= win->char_height + win->char_depth;
    int16_t width = win->cw * win->char_width;
    height *= win->char_depth + win->char_height;

    xcb_copy_area(con.con, win->pid, win->pid, win->gc, 0, ys, 0, yd, width, height);
}

void nss_window_clear(nss_window_t *win, size_t len, const nss_rect_t *damage) {
    nss_rect_t *rects = malloc(len*sizeof(nss_rect_t));
    for (size_t i = 0; i < len; i++)
        rects[i] = rect_scale_up(damage[i], win->char_width, win->char_height + win->char_depth);

    xcb_render_color_t color = MAKE_COLOR(win->reverse_video ? win->fg : win->bg);
    xcb_render_fill_rectangles(con.con, XCB_RENDER_PICT_OP_SRC, win->pic, color, len, (xcb_rectangle_t*)rects);
    free(rects);
}

void nss_window_set(nss_window_t *win, nss_wc_tag_t tag, const uint32_t *values) {
    set_config(win, tag, values);
    _Bool inval_screen = 0;

    if (tag & (nss_wc_font_size | nss_wc_subpixel_fonts))
        reload_font(win, 1), inval_screen = 1;
    if (tag & (nss_wc_cursor_background | nss_wc_cursor_foreground | nss_wc_background | nss_wc_foreground | nss_wc_reverse)) {
        uint32_t values2[2];
        if (tag & nss_wc_reverse && win->reverse_video)
            values2[0] = values2[1] = win->fg;
        else
            values2[0] = values2[1] = win->bg;
        xcb_change_window_attributes(con.con, win->wid, XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL, values2);
        xcb_change_gc(con.con, win->gc, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, values2);
        inval_screen = 1;
    }
   if (inval_screen) {
        nss_term_invalidate_screen(win->term);
        redraw_damage(win, (nss_rect_t) {0, 0, win->width, win->height});
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
    nss_term_redraw(win->term, (nss_rect_t) {0, 0, win->cw, win->ch}, 1);
    redraw_damage(win, (nss_rect_t) {0, 0, win->width, win->height});
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
    if (tag & nss_wc_foreground) return win->fg;
    if (tag & nss_wc_cursor_background) return win->cursor_bg;
    if (tag & nss_wc_cursor_foreground) return win->cursor_fg;
    if (tag & nss_wc_cursor_type) return win->cursor_type;
    if (tag & nss_wc_subpixel_fonts) return win->subpixel_fonts;
    if (tag & nss_wc_font_size) return win->font_size;
    if (tag & nss_wc_width) return win->width;
    if (tag & nss_wc_height) return win->height;
    if (tag & nss_wc_numlock) return win->numlock;
    if (tag & nss_wc_appcursor) return win->appcursor;
    if (tag & nss_wc_appkey) return win->appkey;
    if (tag & nss_wc_keylock) return win->keylock;
    if (tag & nss_wc_has_meta) return win->has_meta;
    if (tag & nss_wc_meta_escape) return win->meta_escape;
    if (tag & nss_wc_bs_del) return win->bs_del;
    if (tag & nss_wc_del_del) return win->del_del;
    if (tag & nss_wc_blink_time) return win->blink_time;
    if (tag & nss_wc_reverse) return win->reverse_video;
    if (tag & nss_wc_mouse) return win->mouse_events;

    warn("Invalid option");
    return 0;
}

static void handle_resize(nss_window_t *win, int16_t width, int16_t height) {

    _Bool redraw_borders = width < win->width || height < win->height;
    //Handle resize

    win->width = width;
    win->height = height;

    int16_t new_cw = MAX(1, (win->width - 2*win->left_border)/win->char_width);
    int16_t new_ch = MAX(1, (win->height - 2*win->top_border)/(win->char_height+win->char_depth));
    int16_t delta_x = new_cw - win->cw;
    int16_t delta_y = new_ch - win->ch;
    win->cw = new_cw;
    win->ch = new_ch;

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

        nss_term_resize(win->term, win->cw, win->ch);

        for (size_t i = 0; i < rectc; i++)
            nss_term_redraw(win->term, rectv[i], 1);
        nss_window_update(win, rectc, rectv);
    }

    if (redraw_borders) { //Update borders
        int16_t width = win->cw * win->char_width + win->left_border;
        int16_t height = win->ch * (win->char_height + win->char_depth) + win->top_border;
        redraw_damage(win, (nss_rect_t) {width, 0, win->width - width, win->height});
        redraw_damage(win, (nss_rect_t) {0, height, width, win->height - height});
        //TIP: May be redraw all borders here
    }

}

static void handle_focus(nss_window_t *win, _Bool focused) {
    win->focused = focused;
    nss_term_focus(win->term, focused);
}

static void handle_keydown(nss_window_t *win, xkb_keycode_t keycode) {

    if (win->keylock) return;

    xkb_keysym_t sym = xkb_state_key_get_one_sym(con.xkb_state, keycode);
    xkb_mod_mask_t mods = xkb_state_serialize_mods(con.xkb_state, XKB_STATE_MODS_EFFECTIVE);

    // 1. Key bindings
    enum nss_shortcut_action action = nss_sa_none;
    for (size_t i = 0; i < sizeof(cshorts)/sizeof(*cshorts); i++) {
        if (cshorts[i].ksym == sym && (mods & cshorts[i].mmask) == cshorts[i].mstate) {
            action = cshorts[i].action;
        	break;
        }
    }
    switch (action) {
        uint32_t arg;
    case nss_sa_break:
        nss_term_sendbreak(win->term);
        return;
    case nss_sa_reverse:
        arg = !win->reverse_video;
        nss_window_set(win, nss_wc_reverse, &arg);
        return;
    case nss_sa_numlock:
        arg = !win->numlock;
        nss_window_set(win, nss_wc_numlock, &arg);
        return;
    case nss_sa_scroll_up:
        nss_term_scroll_view(win->term, -2); //TODO Amount configurable
        return;
    case nss_sa_scroll_down:
        nss_term_scroll_view(win->term, 2);
        return;
    case nss_sa_font_up:
        arg = win->font_size + 1; // TODO Amount configurable
        nss_window_set(win, nss_wc_font_size, &arg);
        return;
    case nss_sa_font_down:
        arg = win->font_size - 1;
        nss_window_set(win, nss_wc_font_size, &arg);
        return;
    case nss_sa_font_default:
        arg = 0; // TODO Use config
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
    case nss_sa_none:
      	break;
    }

    // 2. Custom translations

    nss_ckey_t ck_pat = { .ksym = sym };
    nss_ckey_t *ck = bsearch(&ck_pat, ckeys, sizeof(ckeys)/sizeof(*ckeys), sizeof(*ckeys), key_cmpfn);
    if (ck) {
        for (nss_ckey_key_t *it = ck->inst; it->string; it++) {
            if ((it->mmask & mods) != it->mstate) continue;
            if (it->flag & (win->appkey ? NSS_M_NOAPPK : NSS_M_APPK)) continue;
            if (it->flag & (win->appcursor ? NSS_M_NOAPPCUR : NSS_M_APPCUR)) continue;
            if (it->flag & (win->bs_del ? NSS_M_NOBSDEL : NSS_M_BSDEL)) continue;
            if (it->flag & (win->del_del ? NSS_M_NODELDEL : NSS_M_DELDEL)) continue;
            if ((it->flag & NSS_M_NUM) && (!win->numlock || !win->appkey)) continue;
            nss_term_sendkey(win->term, it->string);
            return;
        }
    }

    // 3. Basic keycode passing
    uint8_t buf[8] = {0};
    size_t sz = xkb_state_key_get_utf8(con.xkb_state, keycode, NULL, 0);
    if (sz && sz < sizeof(buf) - 1) {
        xkb_state_key_get_utf8(con.xkb_state, keycode, (char *)buf, sizeof(buf));
        buf[sz] = '\0';
    }

    if (!sz) return;

    if (nss_term_is_utf8(win->term)) {
        if (sz == 1 && mods & XCB_MOD_MASK_1 && win->has_meta) {
            if (!win->meta_escape) {
                buf[utf8_encode(*buf | 0x80, buf, buf + 8)] = '\0';
            } else {
                buf[2] = '\0';
                buf[1] = buf[0];
                buf[0] = '\033';
            }
        }
    } else {
        buf[1] = '\0';
        if (mods & XCB_MOD_MASK_1 && win->has_meta) {
            if (!win->meta_escape) {
                *buf |= 0x80;
            } else {
                buf[2] = '\0';
                buf[1] = buf[0];
                buf[0] = '\033';
            }
        }
    }
    nss_term_sendkey(win->term, (char *)buf);
}

#define POLL_TIMEOUT (1000/NSS_WIN_FPS)
/* Start window logic, handling all windows in context */
void nss_context_run(void) {
    for (;;) {
        if (poll(con.pfds, con.pfdcap, POLL_TIMEOUT) < 0 && errno != EINTR)
            warn("Poll error: %s", strerror(errno));
        if (con.pfds[0].revents & POLLIN) {
            xcb_generic_event_t *event;
            while ((event = xcb_poll_for_event(con.con))) {
                switch (event->response_type &= 0x7f) {
                case XCB_EXPOSE:{
                    xcb_expose_event_t *ev = (xcb_expose_event_t*)event;
                    nss_window_t *win = window_for_xid(ev->window);
                    if (!win) break;

                    nss_rect_t damage = {ev->x, ev->y, ev->width, ev->height};
                    //info("Damage: %d %d %d %d", damage.x, damage.y, damage.width, damage.height);

                    redraw_damage(win, damage);
                    xcb_flush(con.con);
                    break;
                }
                case XCB_CONFIGURE_NOTIFY:{
                    xcb_configure_notify_event_t *ev = (xcb_configure_notify_event_t*)event;
                    nss_window_t *win = window_for_xid(ev->window);
                    if (!win) break;

                    if (ev->width != win->width || ev->height != win->height) {
                        handle_resize(win, ev->width, ev->height);
                        xcb_flush(con.con);
                    }
                    if (!win->got_configure) {
                        nss_term_redraw(win->term, (nss_rect_t){0, 0, win->cw, win->ch}, 1);
                        nss_window_update(win, 1, &(nss_rect_t){0, 0, win->cw, win->ch});
                    }
                    win->got_configure |= 1;
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
                    xcb_flush(con.con);
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

                    if (evtype == nss_me_press &&
                            !nss_term_is_altscreen(win->term) &&
                            (button == 3 || button == 4) &&
                            !mask) {
                        nss_term_scroll_view(win->term, button == 3 ? 2 : -2);
                    } else nss_term_mouse(win->term, x, y, mask, evtype, button);

                    // What is that?
                    //if (ev->detail == XCB_BUTTON_INDEX_4 && !mask) {
                    //    nss_term_answerback(win->term, "\031");
                    //    return;
                    //} else if (ev->detail == XCB_BUTTON_INDEX_5 && !mask) {
                    //    nss_term_answerback(win->term, "\005");
                    //    return;
                    //}
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
        struct timespec cur;
        clock_gettime(CLOCK_MONOTONIC, &cur);
        for (nss_window_t *win = con.first; win; win = win->next) {
            struct timespec *lastscroll = nss_term_last_scroll_time(win->term);
            long long ms_diff1 = TIMEDIFF(win->prev_blink, cur);
            long long ms_diff2 = TIMEDIFF(win->prev_draw, cur);
            long long ms_diff3 = TIMEDIFF(*lastscroll, cur);
            if (ms_diff1 > win->blink_time && win->active) {
                win->blink_state = !win->blink_state;
                win->prev_blink = cur;
            }
            if ((ms_diff2 > NSS_TERM_REDRAW_RATE && ms_diff3 > NSS_TERM_SCROLL_DELAY) || ms_diff2 > NSS_TERM_MAX_DELAY_SKIP) {
                win->prev_draw = cur;
                nss_term_redraw_dirty(win->term, 1);
            }
        }
        // TODO Adjust timeouts like in st
        xcb_flush(con.con);

        if (!con.daemon_mode && !con.first) break;
    }
}
