/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

#ifndef WINDOW_X11_H_
#define WINDOW_X11_H_ 1

#include "feature.h"

#include "window.h"
#include "config.h"
#include "term.h"

#include <inttypes.h>
#include <stdbool.h>
#include <xkbcommon/xkbcommon.h>

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

    struct timespec last_scroll ALIGNED(MALLOC_ALIGNMENT);
    struct timespec last_blink ALIGNED(MALLOC_ALIGNMENT);
    struct timespec last_sync ALIGNED(MALLOC_ALIGNMENT);
    struct timespec last_read ALIGNED(MALLOC_ALIGNMENT);
    struct timespec last_wait_start ALIGNED(MALLOC_ALIGNMENT);
    struct timespec last_draw ALIGNED(MALLOC_ALIGNMENT);
    struct timespec vbell_start ALIGNED(MALLOC_ALIGNMENT);
    struct timespec wait_for_configure ALIGNED(MALLOC_ALIGNMENT);

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
    struct glyph *undercurl_glyph;
    enum pixel_mode font_pixmode;

    struct term *term;
    struct render_cell_state rcstate;
    int poll_index;

    struct title_stack_item *title_stack;

    /* Window configuration */
    struct instance_config cfg;

    char platform_window_opaque[] ALIGNED(MALLOC_ALIGNMENT);
} ALIGNED(MALLOC_ALIGNMENT);

extern struct window *win_list_head;

FORCEINLINE
inline static struct cellspec describe_cell(struct cell cell, struct attr *attr, struct instance_config *cfg, struct render_cell_state *rcs, bool selected, bool slow_path) {
    struct cellspec res = {0};
    // TODO Better URI rendering
    //      -- underline colors
    //      -- dotted underlines
#if USE_URI
    bool has_uri = attr->uri && cfg->uri_mode != uri_mode_off;
    bool active_uri = attr->uri == rcs->active_uri;
#else
    bool has_uri = 0, active_uri = 0;
#endif

    if (LIKELY(!slow_path && !has_uri)) {
        /* Calculate colors */
        if (attr->bold && !attr->faint && color_idx(attr->fg) < 8) attr->fg = indirect_color(color_idx(attr->fg) + 8);
        res.bg = direct_color(attr->bg, rcs->palette);
        res.fg = direct_color(attr->fg, rcs->palette);
        if (!attr->bold && attr->faint) res.fg = (res.fg & 0xFF000000) | ((res.fg & 0xFEFEFE) >> 1);
        if (attr->reverse) SWAP(res.fg, res.bg);

        res.ul = attr->ul != indirect_color(SPECIAL_BG) ? direct_color(attr->ul, rcs->palette) : res.fg;

        /* Apply background opacity */
        if (color_idx(attr->bg) == SPECIAL_BG) res.bg = color_apply_a(res.bg, cfg->alpha);
        if (attr->invisible || (attr->blink && rcs->blink)) res.ul = res.fg = res.bg;

    } else {
        /* Check special colors */
        if (cfg->special_bold && rcs->palette[SPECIAL_BOLD] && attr->bold)
            attr->fg = rcs->palette[SPECIAL_BOLD], attr->bold = 0;
        if (cfg->special_underline && rcs->palette[SPECIAL_UNDERLINE] && attr->underlined)
            attr->fg = rcs->palette[SPECIAL_UNDERLINE], attr->underlined = 0;
        if (cfg->special_blink && rcs->palette[SPECIAL_BLINK] && attr->blink)
            attr->fg = rcs->palette[SPECIAL_BLINK], attr->blink = 0;
        if (cfg->special_reverse && rcs->palette[SPECIAL_REVERSE] && attr->reverse)
            attr->fg = rcs->palette[SPECIAL_REVERSE], attr->reverse = 0;
        if (cfg->special_italic && rcs->palette[SPECIAL_ITALIC] && attr->italic)
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
                res.underlined = true;
            }
        }
    }

    /* Optimize rendering of U+2588 FULL BLOCK */
    if (cell.ch == 0x2588) res.bg = res.fg;

    /* Calculate attributes */

    if (res.ul != res.bg) res.underlined |= attr->underlined;
    if (res.fg != res.bg) {
        res.stroke = attr->strikethrough;

        if (cell.ch != '\t' && cell.ch != ' ' && (res.ch = cell_get(&cell))) {
            if (attr->bold) res.face |= face_bold;
            if (attr->italic) res.face |= face_italic;
            res.wide = iswide(res.ch);
        }
    }

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

const struct platform_vtable *platform_init_x11(struct instance_config *cfg);

struct platform_vtable {
    /* Renderer dependent functions */
    void (*update)(struct window *win, struct rect rect);
    bool (*reload_font)(struct window *win, bool need_free);
    void (*resize)(struct window *win, int16_t new_cw, int16_t new_ch);
    void (*copy)(struct window *win, struct rect dst, int16_t sx, int16_t sy);
    bool (*submit_screen)(struct window *win, int16_t cur_x, ssize_t cur_y, bool cursor, bool marg);

    /* Platform dependent functions */
    struct extent (*get_screen_size)(void);
    bool (*has_error)(void);
    ssize_t (*get_opaque_size)(void);
    void (*flush)(void);
    void (*handle_events)(void);

    struct extent (*get_position)(struct window *win);
    bool (*init_window)(struct window *win);
    void (*free_window)(struct window *win);
    bool (*set_clip)(struct window *win, uint32_t time, enum clip_target target);
    void (*bell)(struct window *win, uint8_t vol);
    void (*enable_mouse_events)(struct window *win, bool enabled);
    void (*get_pointer)(struct window *win, struct extent *ext, int32_t *mask);
    void (*get_title)(struct window *win, enum title_target which, char **name, bool *utf8);
    void (*map_window)(struct window *win);
    void (*move_window)(struct window *win, int16_t x, int16_t y);
    void (*paste)(struct window *win, enum clip_target target);
    bool (*resize_window)(struct window *win, int16_t width, int16_t height);
    void (*set_icon_label)(struct window *win, const char *title, bool utf8);
    void (*set_title)(struct window *win, const char *title, bool utf8);
    void (*set_urgency)(struct window *win, bool set);
    void (*update_colors)(struct window *win);
    bool (*window_action)(struct window *win, enum window_action action);

    void (*free)(void);
};

/* Platform independent functions */
void handle_expose(struct window *win, struct rect damage);
void handle_resize(struct window *win, int16_t width, int16_t height);
void handle_focus(struct window *win, bool focused);
void handle_keydown(struct window *win, struct xkb_state *state, xkb_keycode_t keycode);
void handle_resize(struct window *win, int16_t width, int16_t height);
/* mouse event handler is defined elsewhere */

struct window *window_find_shared_font(struct window *win, bool need_free, bool force_aligned);

#endif
