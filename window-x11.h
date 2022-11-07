/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

#ifndef WINDOW_X11_H_
#define WINDOW_X11_H_ 1

#include "feature.h"

#include "window.h"
#include "config.h"
#include "term.h"
#if USE_X11SHM
#   include "image.h"
#endif

#include <inttypes.h>
#include <stdbool.h>
#include <xcb/xcb.h>
#include <xkbcommon/xkbcommon.h>
#if USE_X11SHM
#   include <xcb/shm.h>
#else
#   include <xcb/render.h>
#endif

#define TRUE_COLOR_ALPHA_DEPTH 32

struct platform_window {
    xcb_window_t wid;
    xcb_gcontext_t gc;
    xcb_event_mask_t ev_mask;

#if USE_X11SHM
    xcb_shm_seg_t shm_seg;
    xcb_pixmap_t shm_pixmap;
    struct image im;

    /* It's size is 2*win->ch */
    struct rect *bounds;
    size_t boundc;
#else
    /* Active IDs, actual X11 objects */
    xcb_pixmap_t pid1;
    xcb_render_picture_t pic1;
    /* Cached IDs, used for copying */
    xcb_pixmap_t pid2;
    xcb_render_picture_t pic2;

    xcb_render_picture_t pen;
    xcb_render_glyphset_t gsid;
    xcb_render_pictformat_t pfglyph;
#endif

    /* Used to restore maximized window */
    struct rect saved;
};

extern xcb_connection_t *con;

inline static bool check_void_cookie(xcb_void_cookie_t ck) {
    xcb_generic_error_t *err = xcb_request_check(con, ck);
    if (err) {
        warn("[X11 Error] major=%"PRIu8", minor=%"PRIu16", error=%"PRIu8,
                err->major_code, err->minor_code, err->error_code);
        return 1;
    }
    free(err);
    return 0;
}

/* The code below is X11-independent */

struct cellspec {
    color_t fg;
    color_t bg;
    color_t ul;
    uint32_t ch : 24;
    uint32_t face : 4;
    uint32_t underlined : 2;
    bool stroke : 1;
    bool wide : 1;
};

struct title_stack_item {
    struct title_stack_item *next;
    char *title_data;
    char *icon_data;
    bool title_utf8;
    bool icon_utf8;
    char data[];
};

struct render_cell_state {
    color_t *palette;
    uint32_t active_uri : 23;
    uint32_t dummy_ : 7;
    bool blink : 1;
    bool uri_pressed : 1;
};

struct window {
    struct window *prev, *next;
    struct platform_window plat;

    bool focused : 1;
    bool active : 1;
    bool mouse_events : 1;
    bool force_redraw : 1;
    bool blink_commited : 1;
    bool drawn_somthing : 1;
    bool sync_active : 1;
    bool slow_mode : 1;
    bool in_blink : 1;
    bool init_invert : 1;
    bool wait_for_redraw : 1;
    bool autorepeat : 1;
    bool any_event_happend : 1;
    bool redraw_borders : 1;

    int16_t damaged_y0;
    int16_t damaged_y1;

    struct timespec last_scroll ALIGNED(16);
    struct timespec last_blink ALIGNED(16);
    struct timespec last_sync ALIGNED(16);
    struct timespec last_read ALIGNED(16);
    struct timespec last_wait_start ALIGNED(16);
    struct timespec last_draw ALIGNED(16);
    struct timespec vbell_start ALIGNED(16);

    color_t bg;
    color_t bg_premul;
    color_t cursor_fg;

    uint8_t *clipped[clip_MAX];
    uint8_t *clipboard;

    int16_t cw;
    int16_t ch;
    int16_t char_width;
    int16_t char_depth;
    int16_t char_height;
    struct font *font;
    struct glyph_cache *font_cache;
    enum pixel_mode font_pixmode;

    struct term *term;
    struct render_cell_state rcstate;
    int poll_index;

    struct title_stack_item *title_stack;

    /* Window configuration */
    struct instance_config cfg;
};

extern struct window *win_list_head;

FORCEINLINE
inline static struct cellspec describe_cell(struct cell cell, struct attr *attr, struct instance_config *cfg, struct render_cell_state *rcs, bool selected) {
    struct cellspec res;
    // TODO Better URI rendering
    //      -- underline colors
    //      -- dotted underlines
#if USE_URI
    bool has_uri = attr->uri && cfg->uri_mode != uri_mode_off;
    bool active_uri = attr->uri == rcs->active_uri;
#else
    bool has_uri = 0, active_uri = 0;
#endif

    /* Check special colors */
    if (UNLIKELY(cfg->special_bold) && rcs->palette[SPECIAL_BOLD] && attr->bold)
        attr->fg = rcs->palette[SPECIAL_BOLD], attr->bold = 0;
    if (UNLIKELY(cfg->special_underline) && rcs->palette[SPECIAL_UNDERLINE] && attr->underlined)
        attr->fg = rcs->palette[SPECIAL_UNDERLINE], attr->underlined = 0;
    if (UNLIKELY(cfg->special_blink) && rcs->palette[SPECIAL_BLINK] && attr->blink)
        attr->fg = rcs->palette[SPECIAL_BLINK], attr->blink = 0;
    if (UNLIKELY(cfg->special_reverse) && rcs->palette[SPECIAL_REVERSE] && attr->reverse)
        attr->fg = rcs->palette[SPECIAL_REVERSE], attr->reverse = 0;
    if (UNLIKELY(cfg->special_italic) && rcs->palette[SPECIAL_ITALIC] && attr->italic)
        attr->fg = rcs->palette[SPECIAL_ITALIC], attr->italic = 0;

    /* Calculate colors */

    if (attr->bold && !attr->faint && color_idx(attr->fg) < 8) attr->fg = indirect_color(color_idx(attr->fg) + 8);
    res.bg = direct_color(attr->bg, rcs->palette);
    res.fg = direct_color(attr->fg, rcs->palette);
    if (!attr->bold && attr->faint) res.fg = (res.fg & 0xFF000000) | ((res.fg & 0xFEFEFE) >> 1);
    if (attr->reverse ^ selected ^ (has_uri && active_uri && rcs->uri_pressed)) SWAP(res.fg, res.bg);

    res.ul = attr->ul != indirect_color(SPECIAL_BG) ? direct_color(attr->ul, rcs->palette) : res.fg;

    /* Apply background opacity */
    if (color_idx(attr->bg) == SPECIAL_BG || cfg->blend_all_bg) res.bg = color_apply_a(res.bg, cfg->alpha);
    if (UNLIKELY(cfg->blend_fg)) {
        res.fg = color_apply_a(res.fg, cfg->alpha);
        res.ul = color_apply_a(res.ul, cfg->alpha);
    }

    if ((!selected && attr->invisible) || (attr->blink && rcs->blink)) res.ul = res.fg = res.bg;

    /* If selected colors are set use them */

    if (selected) {
        if (rcs->palette[SPECIAL_SELECTED_BG]) res.bg = rcs->palette[SPECIAL_SELECTED_BG];
        if (rcs->palette[SPECIAL_SELECTED_FG]) res.fg = rcs->palette[SPECIAL_SELECTED_FG];
    }

    if (UNLIKELY(has_uri)) {
        if (rcs->palette[SPECIAL_URI_TEXT])
            res.fg = rcs->palette[SPECIAL_URI_TEXT];
        if (active_uri) {
            if (rcs->palette[SPECIAL_URI_UNDERLINE])
                res.ul = rcs->palette[SPECIAL_URI_UNDERLINE];
            res.underlined = 1;
        }
    }

    /* Optimize rendering of U+2588 FULL BLOCK */

    if (cell.ch == 0x2588) res.bg = res.fg;
    if (cell.ch == ' ' || res.fg == res.bg) cell.ch = 0;

    /* Calculate attributes */

    res.ch = cell_get(&cell);
    res.face = 0;
    if (cell.ch && attr->bold) res.face |= face_bold;
    if (cell.ch && attr->italic) res.face |= face_italic;
    res.wide = cell_wide(&cell);
    if (res.ul != res.bg && !(has_uri && active_uri))
        res.underlined = attr->underlined;
    res.stroke = attr->strikethrough && res.fg != res.bg;

    return res;
}

/*
 * This is specialized version of function above that only calculates
 * background color. It is used for padding rendering.
 */
FORCEINLINE
inline static color_t describe_bg(struct attr *attr, struct instance_config *cfg, struct render_cell_state *rcs, bool selected) {
    color_t bg = direct_color(attr->bg, rcs->palette);

    if (UNLIKELY(attr->reverse ^ selected)) {
        color_t fg;

        if (UNLIKELY(cfg->special_bold) && rcs->palette[SPECIAL_BOLD] && attr->bold)
            attr->fg = rcs->palette[SPECIAL_BOLD], attr->bold = 0;
        if (UNLIKELY(cfg->special_underline) && rcs->palette[SPECIAL_UNDERLINE] && attr->underlined)
            attr->fg = rcs->palette[SPECIAL_UNDERLINE], attr->underlined = 0;
        if (UNLIKELY(cfg->special_blink) && rcs->palette[SPECIAL_BLINK] && attr->blink)
            attr->fg = rcs->palette[SPECIAL_BLINK], attr->blink = 0;
        if (UNLIKELY(cfg->special_reverse) && rcs->palette[SPECIAL_REVERSE] && attr->reverse)
            attr->fg = rcs->palette[SPECIAL_REVERSE], attr->reverse = 0;
        if (UNLIKELY(cfg->special_italic) && rcs->palette[SPECIAL_ITALIC] && attr->italic)
            attr->fg = rcs->palette[SPECIAL_ITALIC], attr->italic = 0;
        if (attr->bold && !attr->faint && color_idx(attr->fg) < 8)
            attr->fg = indirect_color(color_idx(attr->fg) + 8);

        fg = direct_color(attr->fg, rcs->palette);

        if (!attr->bold && attr->faint)
            fg = (fg & 0xFF000000) | ((fg & 0xFEFEFE) >> 1);

        if (attr->reverse ^ selected) SWAP(fg, bg);
    }

    /* Apply background opacity */
    if (color_idx(attr->bg) == SPECIAL_BG || cfg->blend_all_bg)
        bg = color_apply_a(bg, cfg->alpha);

    if (selected && rcs->palette[SPECIAL_SELECTED_BG])
        bg = rcs->palette[SPECIAL_SELECTED_BG];

    return bg;
}

/* Renderer dependent functions */
void init_render_context(void);
void free_render_context(void);
void renderer_free(struct window *win);
void renderer_update(struct window *win, struct rect rect);
bool renderer_reload_font(struct window *win, bool need_free);
void renderer_resize(struct window *win, int16_t new_cw, int16_t new_ch);
void renderer_copy(struct window *win, struct rect dst, int16_t sx, int16_t sy);
void renderer_recolor_border(struct window *win);

/* Platform dependent functions */
void platform_init_context(void);
void platform_free_context(void);
struct extent platform_get_screen_size(void);
struct extent platform_get_position(struct window *win);
bool platform_has_error(void);
void platform_handle_events(void);
bool platform_init_window(struct window *win);
void platform_free_window(struct window *win);
bool platform_set_clip(struct window *win, uint32_t time, enum clip_target target);
void platform_bell(struct window *win, uint8_t vol);
void platform_enable_mouse_events(struct window *win, bool enabled);
void platform_get_pointer(struct window *win, struct extent *ext, int32_t *mask);
void platform_get_title(struct window *win, enum title_target which, char **name, bool *utf8);
void platform_map_window(struct window *win);
void platform_move_window(struct window *win, int16_t x, int16_t y);
void platform_paste(struct window *win, enum clip_target target);
void platform_resize_window(struct window *win, int16_t width, int16_t height);
void platform_set_icon_label(struct window *win, const char *title, bool utf8);
void platform_set_title(struct window *win, const char *title, bool utf8);
void platform_set_urgency(struct window *win, bool set);
void platform_update_colors(struct window *win);
void platform_update_window_props(struct window *win);
void platform_window_action(struct window *win, enum window_action action);

/* Platform independent functions */
void handle_expose(struct window *win, struct rect damage);
void handle_resize(struct window *win, int16_t width, int16_t height);
void handle_focus(struct window *win, bool focused);
void handle_keydown(struct window *win, struct xkb_state *state, xkb_keycode_t keycode);
void handle_resize(struct window *win, int16_t width, int16_t height);
/* mouse event handler is defined elsewhere */

struct window *window_find_shared_font(struct window *win, bool need_free);

#endif
