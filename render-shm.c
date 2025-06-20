/* Copyright (c) 2019-2022,2025, Evgeniy Baskov. All rights reserved */


#include "feature.h"

/* Make linting always work for this
 * file (force choosing the right renderer
 * structure variant in window-impl.h)*/
#undef USE_X11SHM
#define USE_X11SHM 1

#include "config.h"
#include "font.h"
#include "mouse.h"
#include "window-impl.h"
#include "image.h"
#include "poller.h"

#include <stdbool.h>
#include <string.h>

bool has_fast_damage;

static inline struct platform_shm *get_shm(struct window *win) {
    return (struct platform_shm *)win->platform_window_opaque;
}

static void resize_bounds(struct window *win, bool h_changed) {
    if (get_shm(win)->bounds) {
        size_t j = 0;
        struct rect *r_dst = get_shm(win)->bounds;
        if (h_changed) r_dst = xalloc(sizeof(struct rect) * 2 * win->c.height);
        for (size_t i = 0; i < get_shm(win)->boundc; i++)
            if (intersect_with(&get_shm(win)->bounds[i], &(struct rect){0, 0, win->c.width, win->c.height}))
                r_dst[j++] = get_shm(win)->bounds[i];
        get_shm(win)->boundc = j;
        if (h_changed) {
            SWAP(get_shm(win)->bounds, r_dst);
            free(r_dst);
        }
    } else {
        get_shm(win)->boundc = 0;
        get_shm(win)->bounds = xalloc(sizeof(struct rect) * 2 * win->c.height);
    }
}

static int rect_cmp(const void *a, const void *b) {
    return ((struct rect*)a)->y - ((struct rect*)b)->y;
}

static void optimize_bounds(struct rect *bounds, size_t *boundc, bool fine_grained) {
    qsort(bounds, *boundc, sizeof(struct rect), rect_cmp);
    size_t j = 0;
    for (size_t i = 0; i < *boundc; ) {
        bounds[j] = bounds[i];
        while (++i < *boundc && (bounds[i].y <= bounds[j].y + bounds[j].height)) {
            struct rect uni = rect_union(bounds[j], bounds[i]);
            if (fine_grained && bounds[i].y >= bounds[j].y + bounds[j].height &&
                3*(bounds[j].height*bounds[j].width + bounds[i].height*bounds[i].width)/2 < uni.width*uni.height) break;
            bounds[j] = uni;
        }
        j++;
    }
    *boundc = j;
}

bool shm_reload_font(struct window *win, bool need_free) {
    window_find_shared_font(win, need_free, true);
    win->redraw_borders = true;

    int w = win->w.width, h = win->w.height;
    if (need_free) {
        handle_resize(win, w, h, true);

        int cw = win->char_width, ch = win->char_height, cd = win->char_depth;
        int bw = win->cfg.border.left, bh = win->cfg.border.top;
        image_draw_rect(get_shm(win)->im, (struct rect) {win->c.width*cw + bw, bh, w - win->c.width*cw - bw, win->c.height*(ch + cd)}, win->bg_premul);
        image_draw_rect(get_shm(win)->im, (struct rect) {0, win->c.height*(ch + cd) + bh, w, h - win->c.height*(ch + cd) - bh}, win->bg_premul);
    } else {
        /* We need to resize window here if
         * it's size is specified in chracters */
        pvtbl->apply_geometry(win, &win->cfg.geometry);
        struct extent bx = win_image_size(win);

        resize_bounds(win, true);

        pvtbl->shm_create_image(win, bx.width, bx.height);
        if (!get_shm(win)->im.data) {
            warn("Can't allocate image");
            return false;
        }

        image_draw_rect(get_shm(win)->im, (struct rect){0, 0, get_shm(win)->im.width, get_shm(win)->im.height}, win->bg_premul);
    }

    pvtbl->update_props(win);

    return true;
}

void shm_recolor_border(struct window *win) {
    struct rect rects[4];
    describe_borders(win, rects);
    for (size_t i = 0; i < LEN(rects); i++)
        image_draw_rect(get_shm(win)->im, rects[i], win->bg_premul);
}

bool shm_submit_screen(struct window *win, int16_t cur_x, ssize_t cur_y, bool cursor_visible, bool on_margin) {
    bool scrolled = get_shm(win)->boundc;
    bool has_blinking = (win->cfg.cursor_shape & 1) && cursor_visible && !win->rcstate.cursor_blink_inhibit;
    bool beyond_eol = false;
    cursor_visible &= !has_blinking || (!win->blink_commited && !win->rcstate.blink && !win->rcstate.cursor_blink_inhibit);
    cursor_visible &= !win->rcstate.cursor_blink_inhibit || !win->cfg.cursor_hide_on_input;

    int cw = win->char_width, ch = win->char_height;
    int cd = win->char_depth, ul = win->cfg.underline_width;
    int bw = win->cfg.border.left, bh = win->cfg.border.top;

    bool slow_path = win->cfg.special_bold || win->cfg.special_underline || win->cfg.special_blink || win->cfg.blend_fg ||
                     win->cfg.special_reverse || win->cfg.special_italic || win->cfg.blend_all_bg || selection_active(term_get_sstate(win->term));
    bool reverse_cursor = cursor_visible && win->focused && ((win->cfg.cursor_shape + 1) & ~1) == cusor_type_block;

    struct screen *scr = term_screen(win->term);
    struct line_span span = screen_view(scr);
    struct line *prev_line = NULL;
    for (ssize_t k = 0; k < win->c.height; k++, screen_span_shift(scr, &span)) {
        screen_span_width(scr, &span);
        bool next_dirty = false;
        struct rect l_bound = {-1, k, 0, 1};

        struct mouse_selection_iterator sel_it = selection_begin_iteration(term_get_sstate(win->term), &span);
        bool last_selected = is_selected_prev(&sel_it, &span, win->c.width - 1);

        if (k == cur_y) {
            beyond_eol = cur_x >= span.width;
            bool dirty = span.line->force_damage;
            if (cur_x < span.width) {
                struct cell *pcell = view_cell(&span, cur_x);
                dirty |= !pcell->drawn || (!win->blink_commited && view_attr(&span, pcell->attrid)->blink);
            }
            cursor_visible &= dirty;
        }

        for (int16_t i = MIN(win->c.width, span.width) - 1; i >= 0; i--) {
            struct cell *pcell = view_cell(&span, i);
            struct cell cel = *pcell;
            pcell->drawn = true;

            struct attr attr = *view_attr(&span, cel.attrid);
            bool dirty = span.line->force_damage || !cel.drawn || (!win->blink_commited && attr.blink);
            has_blinking |= attr.blink;

            struct cellspec spec;
            struct glyph *glyph = NULL;
            bool g_wide = false;
            if (dirty || next_dirty) {

                if (k == cur_y && i == cur_x && reverse_cursor) {
                    attr.fg = win->rcstate.palette[SPECIAL_CURSOR_FG];
                    attr.bg = win->rcstate.palette[SPECIAL_CURSOR_BG];
                    attr.reverse ^= true;
                }

                bool selected = is_selected_prev(&sel_it, &span, i);
                spec = describe_cell(cel, &attr, &win->cfg, &win->rcstate, selected, slow_path);

                if (spec.ch) glyph = glyph_cache_fetch(win->font_cache, spec.ch, spec.face, NULL);
                g_wide = glyph && glyph->x_off > win->char_width - win->cfg.font_spacing;
            }

            if (dirty || (g_wide && next_dirty)) {
                int x = i * cw + bw;
                int y = k * (ch + cd) + bh;
                int ls = win->cfg.line_spacing/2;
                int fs = win->cfg.font_spacing/2;

                struct rect r_cell = { x, y, cw * (1 + spec.wide), ch + cd};
                struct rect r_under = { x + fs, y + ch + 1 + ls, cw, ul };
                struct rect r_strike = { x + fs, y + 2*ch/3 - ul/2 + ls, cw, ul };

                /* Background */
                image_draw_rect(get_shm(win)->im, r_cell, spec.bg);

                /* Glyph */
                if (glyph) {
                    if (g_wide) r_cell.width = 2*cw;
                    image_compose_glyph(get_shm(win)->im, x + fs, y + ch + ls, glyph, spec.fg, r_cell);
                }

                /* Underline */
                if (spec.underlined) {
                    if (spec.underlined < 3) {
                        image_draw_rect(get_shm(win)->im, r_under, spec.ul);
                        if (spec.underlined == 2) {
                            image_draw_rect(get_shm(win)->im, r_under, spec.ul);
                            r_under.y += ul + 1;
                        }
                    } else {
                        r_under.height = win->char_depth + 1;
                        image_compose_glyph(get_shm(win)->im, r_under.x, r_under.y, win->undercurl_glyph, spec.ul, r_under);
                    }
                }

                /* Strikethrough */
                if (spec.stroke) image_draw_rect(get_shm(win)->im, r_strike, spec.ul);

                if (l_bound.x < 0) l_bound.width = i + g_wide;

                l_bound.x = i;
            }
            next_dirty = dirty;
        }

        if (l_bound.x >= 0 || span.line->force_damage || (scrolled && win->c.width > span.width)) {
            if (win->c.width > span.width) {
                struct attr attr = *attr_pad(span.line);
                color_t bg = describe_bg(&attr, &win->cfg, &win->rcstate, last_selected);

                image_draw_rect(get_shm(win)->im, (struct rect){
                    .x = bw + span.width * win->char_width,
                    .y = bh + k * (win->char_height + win->char_depth),
                    .width = (win->c.width - span.width) * win->char_width,
                    .height = win->char_height + win->char_depth
                }, bg);
                l_bound.width = win->c.width - 1;
                if (l_bound.x < 0) l_bound.x = span.width;
            }

            l_bound.width = MIN(l_bound.width - l_bound.x + 1, win->c.width);
            get_shm(win)->bounds[get_shm(win)->boundc++] = l_bound;
        }

        /* Only reset force flag for last part of the line */
        if (prev_line != span.line && prev_line)
            prev_line->force_damage = false;
        prev_line = span.line;
    }
    if (prev_line)
        prev_line->force_damage = false;

    if (cursor_visible) {
        struct cursor_rects cr = describe_cursor(win, cur_x, cur_y, on_margin, beyond_eol);
        for (size_t i = 0; i < cr.count; i++)
            image_draw_rect(get_shm(win)->im, cr.rects[i + cr.offset], win->cursor_fg);
    }

    bool drawn_any = get_shm(win)->boundc;

    if (win->redraw_borders) {
        if (!has_fast_damage) {
            pvtbl->update(win, window_rect(win));
            get_shm(win)->boundc = 0;
        } else {
            struct rect rects[4];
            describe_borders(win, rects);
            for (size_t i = 0; i < LEN(rects); i++)
                pvtbl->update(win, rects[i]);
        }
        win->redraw_borders = false;
    }

    if (get_shm(win)->boundc) {
        optimize_bounds(get_shm(win)->bounds, &get_shm(win)->boundc, has_fast_damage);
        for (size_t k = 0; k < get_shm(win)->boundc; k++)
            pvtbl->update(win, rect_shift(rect_scale_up(get_shm(win)->bounds[k], cw, cd + ch), bw, bh));
        get_shm(win)->boundc = 0;
    }

    bool blink_enabled = poller_is_enabled(win->blink_timer);
    if (blink_enabled)
        win->blink_commited = true;
    if (has_blinking ^ poller_is_enabled(win->blink_timer))
        poller_toggle(win->blink_timer, has_blinking);

    if (drawn_any && pvtbl->draw_end)
        pvtbl->draw_end(win);

    return drawn_any;
}

void shm_copy(struct window *win, struct rect dst, int16_t sx, int16_t sy) {
    image_copy(get_shm(win)->im, dst, get_shm(win)->im, sx, sy);

    int16_t w = win->char_width, h = win->char_depth + win->char_height;

    dst.y -= win->cfg.border.top;
    dst.x -= win->cfg.border.left;

    dst.height = (dst.height + dst.y + h - 1) / h;
    dst.height -= dst.y /= h;
    dst.width = (dst.width + dst.x + w - 1) / w;
    dst.width -= dst.x /= w;

    if (get_shm(win)->boundc + 1 > (size_t)win->c.height)
        optimize_bounds(get_shm(win)->bounds, &get_shm(win)->boundc, 0);
    get_shm(win)->bounds[get_shm(win)->boundc++] = dst;
}

void shm_resize(struct window *win, int16_t new_w, int16_t new_h, int16_t new_cw, int16_t new_ch, bool artificial) {
    int16_t old_w = win->w.width;
    int16_t old_h = win->w.height;

    if (!artificial)
        win->w = win_derive_window_size(win, new_cw, new_ch);
    else
        win->w = (struct extent) { new_w, new_h };

    if (win->c.width == new_cw && win->c.height == new_ch) {
        if (artificial && pvtbl->resize_exact)
            pvtbl->resize_exact(win, new_w, new_h, old_w, old_h);
        return;
    }

    int16_t delta_x = new_cw - win->c.width;
    int16_t delta_y = new_ch - win->c.height;

    win->c.width = new_cw;
    win->c.height = new_ch;

    struct extent sz = win_image_size(win);
    int16_t common_w = MIN(sz.width, sz.width  - delta_x * win->char_width);
    int16_t common_h = MIN(sz.height, sz.height - delta_y * (win->char_height + win->char_depth));

    struct image old = pvtbl->shm_create_image(win, sz.width, sz.height);
    image_copy(get_shm(win)->im, (struct rect){0, 0, common_w, common_h}, old, 0, 0);
    free_image(&old);

    resize_bounds(win, delta_y);

    int16_t xw = win->c.width * win->char_width + win->cfg.border.left;
    int16_t xh = win->c.height * (win->char_height + win->char_depth) + win->cfg.border.top;

    if (delta_y > 0) {
        image_draw_rect(get_shm(win)->im, (struct rect) { 0, common_h, common_w, sz.height - common_h }, win->bg_premul);
    } else if (delta_y < 0) {
        image_draw_rect(get_shm(win)->im, (struct rect) { 0, xh, sz.width, sz.height - xh }, win->bg_premul);
        win->redraw_borders = true;
    }

    if (delta_x > 0) {
        image_draw_rect(get_shm(win)->im, (struct rect) { common_w, 0, sz.width - common_w, sz.height }, win->bg_premul);
    } else if (delta_x < 0) {
        image_draw_rect(get_shm(win)->im, (struct rect) { xw, 0, sz.width - xw, xh }, win->bg_premul);
        win->redraw_borders = true;
    }
}
