/* Copyright (c) 2019-2021, Evgeny Baskov. All rights reserved */

#define _POSIX_C_SOURCE 200809L

#include "feature.h"

/* Make linting always work for this
 * file (force choosing the right renderer
 * structure variant in window-x11.h)*/
#undef USE_X11SHM
#define USE_X11SHM 1

#include "config.h"
#include "font.h"
#include "mouse.h"
#include "window-x11.h"

#include <stdbool.h>
#include <string.h>
#include <xcb/shm.h>
#include <xcb/xcb.h>

struct render_context {
    bool has_shm;
    bool has_shm_pixmaps;
};

static struct render_context rctx;

static void resize_bounds(struct window *win, bool h_changed) {
    if (win->ren.bounds) {
        size_t j = 0;
        struct rect *r_dst = win->ren.bounds;
        if (h_changed) r_dst = malloc(sizeof(struct rect) * 2 * win->ch);
        if (!r_dst) die("Can't allocate bounds");
        for (size_t i = 0; i < win->ren.boundc; i++)
            if (intersect_with(&win->ren.bounds[i], &(struct rect){0, 0, win->cw, win->ch}))
                r_dst[j++] = win->ren.bounds[i];
        win->ren.boundc = j;
        if (h_changed) {
            SWAP(win->ren.bounds, r_dst);
            free(r_dst);
        }
    } else {
        win->ren.boundc = 0;
        win->ren.bounds = malloc(sizeof(struct rect) * 2 * win->ch);
        if (!win->ren.bounds) die("Can't allocate bounds");
    }
}

// Returns old image
static struct image create_mitshm_image(struct window *win, int16_t width, int16_t height) {
    struct image old = win->ren.im;
    xcb_void_cookie_t c;

    if (rctx.has_shm) {
        win->ren.im = create_shm_image(width, height);

        if (!win->ren.shm_seg) {
            win->ren.shm_seg = xcb_generate_id(con);
        } else {
            if (rctx.has_shm_pixmaps && win->ren.shm_pixmap)
                xcb_free_pixmap(con, win->ren.shm_pixmap);
            xcb_shm_detach_checked(con, win->ren.shm_seg);
        }

#if USE_POSIX_SHM
        c = xcb_shm_attach_fd_checked(con, win->ren.shm_seg, dup(win->ren.im.shmid), 0);
#else
        c = xcb_shm_attach_checked(con, win->ren.shm_seg, win->ren.im.shmid, 0);
#endif
        if (check_void_cookie(c)) goto error;

        if (rctx.has_shm_pixmaps) {
            if (!win->ren.shm_pixmap)
                win->ren.shm_pixmap = xcb_generate_id(con);
            c = xcb_shm_create_pixmap(con, win->ren.shm_pixmap,
                    win->wid, STRIDE(width), height, TRUE_COLOR_ALPHA_DEPTH, win->ren.shm_seg, 0);
            if (check_void_cookie(c)) goto error;
        }
    } else {
        win->ren.im = create_image(width, height);
    }

    return old;

error:
    free_image(&win->ren.im);
    win->ren.im = old;
    warn("Can't attach MITSHM image");
    return (struct image) {0};
}


bool renderer_reload_font(struct window *win, bool need_free) {
    find_shared_font(win, need_free);

    if (need_free) {
        handle_resize(win, win->cfg.width, win->cfg.height);
        window_set_default_props(win);
    } else {
        win->cw = MAX(1, (win->cfg.width - 2*win->cfg.left_border) / win->char_width);
        win->ch = MAX(1, (win->cfg.height - 2*win->cfg.top_border) / (win->char_height + win->char_depth));

        resize_bounds(win, 1);

        create_mitshm_image(win, win->cw*win->char_width, win->ch*(win->char_depth+win->char_height));
        if (!win->ren.im.data) {
            warn("Can't allocate image");
            return 0;
        }

        image_draw_rect(win->ren.im, (struct rect){0, 0, win->ren.im.width, win->ren.im.height}, win->bg_premul);
    }

    return 1;
}

void renderer_free(struct window *win) {
    if (rctx.has_shm)
        xcb_shm_detach(con, win->ren.shm_seg);
    if (rctx.has_shm_pixmaps)
        xcb_free_pixmap(con, win->ren.shm_pixmap);
    if (win->ren.im.data)
        free_image(&win->ren.im);
    free(win->ren.bounds);
}

void free_render_context(void) {
    /* nothing */
}

void init_render_context(void) {
    // That's kind of hack
    // Try guessing if DISPLAY refers to localhost

    char *display = getenv("DISPLAY");
    const char *local[] = { "localhost:", "127.0.0.1:", "unix:", };
    bool localhost = display[0] == ':';
    for (size_t i = 0; !localhost && i < sizeof(local)/sizeof(*local); i++)
        localhost = local[i] == strstr(display, local[i]);

    if (localhost) {
        xcb_shm_query_version_cookie_t q = xcb_shm_query_version(con);
        xcb_generic_error_t *er = NULL;
        xcb_shm_query_version_reply_t *qr = xcb_shm_query_version_reply(con, q, &er);
        if (er) free(er);

        if (qr) {
            rctx.has_shm_pixmaps = qr->shared_pixmaps &&
                    qr->pixmap_format == XCB_IMAGE_FORMAT_Z_PIXMAP;
            free(qr);
        }
        if (!(rctx.has_shm = qr && !er)) {
            warn("MIT-SHM is not available");
        }
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

bool window_submit_screen(struct window *win, int16_t cur_x, ssize_t cur_y, bool cursor, bool marg) {
    bool scrolled = win->ren.boundc;
    bool cond_cblink = !win->blink_commited && (win->cfg.cursor_shape & 1) && term_is_cursor_enabled(win->term);
    if (cond_cblink) cursor |= win->rcstate.blink;

    struct line_offset vpos = term_get_view(win->term);
    for (ssize_t k = 0; k < win->ch; k++, term_line_next(win->term, &vpos, 1)) {
        struct line_view line = term_line_at(win->term, vpos);
        bool next_dirty = 0;
        struct rect l_bound = {-1, k, 0, 1};
        for (int16_t i =  MIN(win->cw, line.width) - 1; i >= 0; i--) {
            struct cell cel = line.cell[i];
            struct attr attr = line_view_attr_at(line, i);

            bool dirty = line.line->force_damage || !cel.drawn || (!win->blink_commited && attr.blink);

            struct cellspec spec;
            struct glyph *glyph = NULL;
            bool g_wide = 0;
            if (dirty || next_dirty) {

                if (k == cur_y && i == cur_x && cursor &&
                        win->focused && ((win->cfg.cursor_shape + 1) & ~1) == cusor_type_block)
                    attr.reverse ^= 1;

                bool selected = mouse_is_selected_in_view(win->term, i, k);
                spec = describe_cell(cel, attr, &win->cfg, &win->rcstate, selected);

                if (spec.ch) glyph = glyph_cache_fetch(win->font_cache, spec.ch, spec.face, NULL);
                g_wide = glyph && glyph->x_off > win->char_width - win->cfg.font_spacing;
            }

            if (dirty || (g_wide && next_dirty)) {
                int16_t cw = win->char_width, ch = win->char_height;
                int16_t cd = win->char_depth, ul = win->cfg.underline_width;
                int16_t x = i * cw, y = k * (ch + cd);
                int16_t ls = win->cfg.line_spacing/2;
                int16_t fs = win->cfg.font_spacing/2;

                struct rect r_cell = { x, y, cw * (1 + spec.wide), ch + cd};
                struct rect r_under = { x + fs, y + ch + 1 + ls, cw, ul };
                struct rect r_strike = { x + fs, y + 2*ch/3 - ul/2 + ls, cw, ul };

                // Backround
                image_draw_rect(win->ren.im, r_cell, spec.bg);

                // Glyph
                if (glyph) {
                    if (g_wide) r_cell.width = 2*cw;
                    image_compose_glyph(win->ren.im, x + fs, y + ch + ls, glyph, spec.fg, r_cell);
                }

                // Underline
                if (spec.underlined) image_draw_rect(win->ren.im, r_under, spec.fg);

                // Strikethough
                if (spec.stroke) image_draw_rect(win->ren.im, r_strike, spec.fg);

                line.cell[i].drawn = 1;

                if (l_bound.x < 0) l_bound.width = i + g_wide;

                l_bound.x = i;
            }
            next_dirty = dirty;
        }
        if (l_bound.x >= 0 || (scrolled && win->cw > line.width)) {
            if (win->cw > line.width) {
                color_t c = win->bg_premul;
                if (mouse_is_selected_in_view(win->term, win->cw - 1, k)) {
                    c = win->rcstate.palette[SPECIAL_SELECTED_BG];
                    if (!c) c = win->rcstate.palette[SPECIAL_FG];
                    c = color_apply_a(c, win->cfg.alpha);
                }
                image_draw_rect(win->ren.im, (struct rect){
                    .x = line.width * win->char_width,
                    .y = k * (win->char_height + win->char_depth),
                    .width = (win->cw - line.width) * win->char_width,
                    .height = win->char_height + win->char_depth
                }, c);
                l_bound.width = win->cw - 1;
                if (l_bound.x < 0) l_bound.x = line.width;
            }
            l_bound.width = MIN(l_bound.width - l_bound.x + 1, win->cw);
            win->ren.bounds[win->ren.boundc++] = l_bound;
        }

        // Only reset force flag for last part of the line
        if (is_last_line(line, win->cfg.rewrap)) line.line->force_damage = 0;
    }

    if (cursor) {
        cur_x *= win->char_width;
        cur_y *= win->char_depth + win->char_height;
        struct rect rects[4] = {
            {cur_x, cur_y, 1, win->char_height + win->char_depth},
            {cur_x, cur_y, win->char_width, 1},
            {cur_x + win->char_width - 1, cur_y, 1, win->char_height + win->char_depth},
            {cur_x, cur_y + (win->char_depth + win->char_height - 1), win->char_width, 1}
        };
        size_t off = 0, count = 4;
        if (win->focused) {
            if (((win->cfg.cursor_shape + 1) & ~1) == cusor_type_bar) {
                if (marg) {
                    off = 2;
                    rects[2].width = win->cfg.cursor_width;
                    rects[2].x -= win->cfg.cursor_width - 1;
                } else
                    rects[0].width = win->cfg.cursor_width;
                count = 1;
            } else if (((win->cfg.cursor_shape + 1) & ~1) == cusor_type_underline) {
                count = 1;
                off = 3;
                rects[3].height = win->cfg.cursor_width;
                rects[3].y -= win->cfg.cursor_width - 1;
            } else {
                count = 0;
            }
        }
        for (size_t i = 0; i < count; i++)
            image_draw_rect(win->ren.im, rects[i + off], win->cursor_fg);
    }

    bool drawn_any = win->ren.boundc;

    if (win->ren.boundc) {
        optimize_bounds(win->ren.bounds, &win->ren.boundc, rctx.has_shm);
        for (size_t k = 0; k < win->ren.boundc; k++) {
            renderer_update(win, rect_scale_up(win->ren.bounds[k], win->char_width, win->char_depth + win->char_height));
        }
        win->ren.boundc = 0;
    }

    return drawn_any;
}

void renderer_update(struct window *win, struct rect rect) {
    if (rctx.has_shm_pixmaps) {
        xcb_copy_area(con, win->ren.shm_pixmap, win->wid, win->gc, rect.x, rect.y,
                rect.x + win->cfg.left_border, rect.y + win->cfg.top_border, rect.width, rect.height);
    } else if (rctx.has_shm) {
        xcb_shm_put_image(con, win->wid, win->gc, STRIDE(win->ren.im.width), win->ren.im.height, rect.x, rect.y, rect.width, rect.height,
                rect.x + win->cfg.left_border, rect.y + win->cfg.top_border, TRUE_COLOR_ALPHA_DEPTH, XCB_IMAGE_FORMAT_Z_PIXMAP, 0, win->ren.shm_seg, 0);
    } else {
        xcb_put_image(con, XCB_IMAGE_FORMAT_Z_PIXMAP, win->wid, win->gc,
                STRIDE(win->ren.im.width), rect.height, win->cfg.left_border,
                win->cfg.top_border + rect.y, 0, TRUE_COLOR_ALPHA_DEPTH,
                rect.height * STRIDE(win->ren.im.width) * sizeof(color_t),
                (const uint8_t *)(win->ren.im.data + rect.y*STRIDE(win->ren.im.width)));
    }
}

void renderer_copy(struct window *win, struct rect dst, int16_t sx, int16_t sy) {
    image_copy(win->ren.im, dst, win->ren.im, sx, sy);

    int16_t w = win->char_width, h = win->char_depth + win->char_height;

    /*
    xcb_copy_area(con, win->wid, win->wid, win->ren.gc, sx + win->left_border,
                  sy + win->top_border, dst.x + win->left_border, dst.y + win->top_border, dst.width, dst.height);
    */

    dst.height = (dst.height + dst.y + h - 1) / h;
    dst.height -= dst.y /= h;
    dst.width = (dst.width + dst.x + w - 1) / w;
    dst.width -= dst.x /= w;

    if (win->ren.boundc + 1 > (size_t)win->ch)
        optimize_bounds(win->ren.bounds, &win->ren.boundc, 0);
    win->ren.bounds[win->ren.boundc++] = dst;
}

void renderer_resize(struct window *win, int16_t new_cw, int16_t new_ch) {
    int16_t delta_x = new_cw - win->cw;
    int16_t delta_y = new_ch - win->ch;

    win->cw = new_cw;
    win->ch = new_ch;

    int16_t width = win->cw * win->char_width;
    int16_t height = win->ch * (win->char_height + win->char_depth);

    int16_t common_w = MIN(width, width  - delta_x * win->char_width);
    int16_t common_h = MIN(height, height - delta_y * (win->char_height + win->char_depth));

    struct image old = create_mitshm_image(win, width, height);
    image_copy(win->ren.im, (struct rect){0, 0, common_w, common_h}, old, 0, 0);
    free_image(&old);

    resize_bounds(win, delta_y);

    color_t c = win->bg_premul;
    if (delta_y > 0) image_draw_rect(win->ren.im,
            (struct rect) { 0, common_h, common_w, height - common_h }, c);
    if (delta_x > 0) image_draw_rect(win->ren.im,
            (struct rect) { common_w, 0, width - common_w, height }, c);

}
