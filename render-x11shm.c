/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#include "feature.h"
#include "config.h"
#include "font.h"
#include "window-x11.h"

#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <xcb/shm.h>
#include <xcb/xcb.h>

typedef struct nss_render_context nss_render_context_t;

struct nss_render_context {
    _Bool has_shm;
    _Bool has_shm_pixmaps;
};

static nss_render_context_t rctx;

static void resize_bounds(nss_window_t *win, _Bool h_changed) {
    if (win->ren.bounds) {
        size_t j = 0;
        nss_rect_t *r_dst = win->ren.bounds;
        if (h_changed) r_dst = malloc(sizeof(nss_rect_t) * 2 * win->ch);
        if (!r_dst) die("Can't allocate bounds");
        for (size_t i = 0; i < win->ren.boundc; i++)
            if (intersect_with(&win->ren.bounds[i], &(nss_rect_t){0, 0, win->cw, win->ch}))
                r_dst[j++] = win->ren.bounds[i];
        win->ren.boundc = j;
        if (h_changed) {
            SWAP(nss_rect_t *, win->ren.bounds, r_dst);
            free(r_dst);
        }
    } else {
        win->ren.boundc = 0;
        win->ren.bounds = malloc(sizeof(nss_rect_t) * 2 * win->ch);
        if (!win->ren.bounds) die("Can't allocate bounds");
    }
}

static nss_image_t nss_create_image_shm(nss_window_t *win, int16_t width, int16_t height) {
    nss_image_t im = {
        .width = width,
        .height = height,
        .shmid = -1,
    };
    size_t size = width * height * sizeof(nss_color_t) + sizeof(nss_image_t);

    if (rctx.has_shm) {
        im.shmid = shmget(IPC_PRIVATE, size, IPC_CREAT | 0600);
        if (im.shmid == -1U) return im;

        im.data = shmat(im.shmid, 0, 0);
        if ((void *)im.data == (void *) -1) goto error;

        xcb_void_cookie_t c;
        if(!win->ren.shm_seg) {
            win->ren.shm_seg = xcb_generate_id(con);
        } else {
            if (rctx.has_shm_pixmaps && win->ren.shm_pixmap)
                xcb_free_pixmap(con, win->ren.shm_pixmap);
            c = xcb_shm_detach_checked(con, win->ren.shm_seg);
            check_void_cookie(c);
        }

        c = xcb_shm_attach_checked(con, win->ren.shm_seg, im.shmid, 0);
        if (check_void_cookie(c)) goto error;

        if (rctx.has_shm_pixmaps) {
            if (!win->ren.shm_pixmap)
                win->ren.shm_pixmap = xcb_generate_id(con);
            xcb_shm_create_pixmap(con, win->ren.shm_pixmap,
                    win->wid, width, height, 32, win->ren.shm_seg, 0);
        }

        return im;
    error:
        warn("Can't create image");
        if ((void *)im.data != (void *) -1) shmdt(im.data);
        if (im.shmid != -1U) shmctl(im.shmid, IPC_RMID, NULL);

        im.shmid = -1;
        im.data = NULL;
        return im;
    } else {
        im.data = malloc(size);
        return im;
    }
}

static void nss_free_image_shm(nss_image_t *im) {
    if (rctx.has_shm) {
        if (im->data) shmdt(im->data);
        if (im->shmid != -1U) shmctl(im->shmid, IPC_RMID, NULL);
    } else {
        if (im->data) free(im->data);
    }
    im->shmid = -1;
    im->data = NULL;
}

_Bool nss_renderer_reload_font(nss_window_t *win, _Bool need_free) {
    nss_find_shared_font(win, need_free);

    if (need_free) {
        nss_window_handle_resize(win, win->width, win->height);
        nss_window_set_default_props(win);
    } else {
        win->cw = MAX(1, (win->width - 2*win->left_border) / win->char_width);
        win->ch = MAX(1, (win->height - 2*win->top_border) / (win->char_height + win->char_depth));

        resize_bounds(win, 1);

        win->ren.im = nss_create_image_shm(win, win->cw*win->char_width, win->ch*(win->char_depth+win->char_height));
        if (!win->ren.im.data) {
            warn("Can't allocate image");
            return 0;
        }

        nss_image_draw_rect(win->ren.im, (nss_rect_t){0, 0, win->ren.im.width, win->ren.im.height}, win->bg);
    }

    return 1;
}

void nss_renderer_free(nss_window_t *win) {
    if (rctx.has_shm)
        xcb_shm_detach(con, win->ren.shm_seg);
    if (rctx.has_shm_pixmaps)
        xcb_free_pixmap(con, win->ren.shm_pixmap);
    if (win->ren.im.data)
        nss_free_image_shm(&win->ren.im);
    free(win->ren.bounds);
}

void nss_free_render_context() {
    /* nothing */
}

void nss_init_render_context() {
    // That's kind of hack
    // Try guessing if DISPLAY refers to localhost

    char *display = getenv("DISPLAY");
    char *local[] = { "localhost:", "127.0.0.1:", "unix:", };
    _Bool localhost = display[0] == ':';
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
    return ((nss_rect_t*)a)->y - ((nss_rect_t*)b)->y;
}

static void optimize_bounds(nss_rect_t *bounds, size_t *boundc, _Bool fine_grained) {
    qsort(bounds, *boundc, sizeof(nss_rect_t), rect_cmp);
    size_t j = 0;
    for(size_t i = 0; i < *boundc; ) {
        bounds[j] = bounds[i];
        while(++i < *boundc && (bounds[i].y <= bounds[j].y + bounds[j].height)) {
            nss_rect_t uni = rect_union(bounds[j], bounds[i]);
            if (fine_grained && bounds[i].y >= bounds[j].y + bounds[j].height &&
                3*(bounds[j].height*bounds[j].width + bounds[i].height*bounds[i].width)/2 < uni.width*uni.height) break;
            bounds[j] = uni;
        }
        j++;
    }
    *boundc = j;
}

_Bool nss_window_submit_screen(nss_window_t *win, nss_line_iter_t *it, nss_color_t *palette, nss_coord_t cur_x, nss_coord_t cur_y, _Bool cursor) {
    _Bool marg = win->cw == cur_x;
    cur_x -= marg;

    _Bool scrolled = win->ren.boundc;

    if (!win->blink_commited && (win->cursor_type & 1) &&
            nss_term_is_cursor_enabled(win->term)) cursor |= win->blink_state;

    for (nss_line_t *line; (line = line_iter_next(it));) {
        _Bool damaged = 0;
        nss_rect_t l_bound = {0, line_iter_y(it), 0, 1};
        for (nss_coord_t i = 0; i < MIN(win->cw, line->width); i++) {
            if (!(line->cell[i].attr & nss_attrib_drawn) ||
                    (!win->blink_commited && ((line->cell[i].attr & nss_attrib_blink) ||
                    ((win->cursor_type & 1) && line_iter_y(it) == cur_y && i == cur_x)))) {
                nss_cell_t cel = line->cell[i];

                if (line_iter_y(it) == cur_y && i == cur_x && cursor &&
                        win->focused && ((win->cursor_type + 1) & ~1) == nss_cursor_block)
                    cel.attr ^= nss_attrib_inverse;

                struct nss_cellspec spec = nss_describe_cell(cel, palette,
                        line->pal->data, win->blink_state, nss_term_is_selected(win->term, i, line_iter_y(it)));

                int16_t cw = win->char_width, ch = win->char_height;
                int16_t cd = win->char_depth, ul = win->underline_width;
                int16_t x = i * cw, y = line_iter_y(it) * (ch + cd);

                nss_rect_t r_cell = { x, y, cw * (1 + spec.wide), ch + cd};
                nss_rect_t r_under = { x, y + ch + 1, cw, ul };
                nss_rect_t r_strike = { x, y + 2*ch/3 - ul/2, cw, ul };

                // Backround
                nss_image_draw_rect(win->ren.im, r_cell, spec.bg);

                // Glyph
                if (spec.ch) {
                    nss_glyph_t *glyph = nss_cache_fetch(win->font_cache, spec.ch, spec.face);
                    nss_image_compose_glyph(win->ren.im, x, y + ch, glyph, spec.fg, r_cell, win->subpixel_fonts);
                }

                // Underline
                if (spec.underlined) nss_image_draw_rect(win->ren.im, r_under, spec.fg);

                // Strikethough
                if (spec.stroke) nss_image_draw_rect(win->ren.im, r_strike, spec.fg);

                line->cell[i].attr |= nss_attrib_drawn;

                if (!damaged) l_bound.x = i;

                i += spec.wide;
                line->cell[i].attr |= nss_attrib_drawn;

                l_bound.width = i;
                damaged = 1;
            }
        }
        if (damaged || (scrolled && win->cw > line->width)) {
            if (win->cw > line->width) {
                nss_image_draw_rect(win->ren.im, (nss_rect_t){
                    .x = line->width * win->char_width,
                    .y = line_iter_y(it) * (win->char_height + win->char_depth),
                    .width = (win->cw - line->width) * win->char_width,
                    .height = win->char_height + win->char_depth
                }, win->bg);
                l_bound.width = win->cw - 1;
            }
            l_bound.width = MIN(l_bound.width - l_bound.x + 1, win->cw);
            win->ren.bounds[win->ren.boundc++] = l_bound;
        }

    }

    if (cursor) {
        cur_x *= win->char_width;
        cur_y *= win->char_depth + win->char_height;
        nss_rect_t rects[4] = {
            {cur_x, cur_y, 1, win->char_height + win->char_depth},
            {cur_x, cur_y, win->char_width, 1},
            {cur_x + win->char_width - 1, cur_y, 1, win->char_height + win->char_depth},
            {cur_x, cur_y + (win->char_depth + win->char_height - 1), win->char_width, 1}
        };
        size_t off = 0, count = 4;
        if (win->focused) {
            if (((win->cursor_type + 1) & ~1) == nss_cursor_bar) {
                if(marg) {
                    off = 2;
                    rects[2].width = win->cursor_width;
                    rects[2].x -= win->cursor_width - 1;
                } else
                    rects[0].width = win->cursor_width;
                count = 1;
            } else if (((win->cursor_type + 1) & ~1) == nss_cursor_underline) {
                count = 1;
                off = 3;
                rects[3].height = win->cursor_width;
                rects[3].y -= win->cursor_width - 1;
            } else {
                count = 0;
            }
        }
        for (size_t i = 0; i < count; i++)
            nss_image_draw_rect(win->ren.im, rects[i + off], win->cursor_fg);
    }

    _Bool drawn_any = win->ren.boundc;

    if (win->ren.boundc) {
        optimize_bounds(win->ren.bounds, &win->ren.boundc, rctx.has_shm);
        for (size_t k = 0; k < win->ren.boundc; k++) {
            nss_renderer_update(win, rect_scale_up(win->ren.bounds[k], win->char_width, win->char_depth + win->char_height));
        }
        win->ren.boundc = 0;
    }

    win->damaged_y0 = win->damaged_y1 = 0;

    return drawn_any;
}

void nss_renderer_update(nss_window_t *win, nss_rect_t rect) {
    if (rctx.has_shm_pixmaps) {
        xcb_copy_area(con, win->ren.shm_pixmap, win->wid, win->gc, rect.x, rect.y,
                rect.x + win->left_border, rect.y + win->top_border, rect.width, rect.height);
    } else if (rctx.has_shm) {
        xcb_shm_put_image(con, win->wid, win->gc, win->ren.im.width, win->ren.im.height, rect.x, rect.y, rect.width, rect.height,
                rect.x + win->left_border, rect.y + win->top_border, 32, XCB_IMAGE_FORMAT_Z_PIXMAP, 0, win->ren.shm_seg, 0);
    } else {
        xcb_put_image(con, XCB_IMAGE_FORMAT_Z_PIXMAP, win->wid, win->gc,
                win->ren.im.width, rect.height, win->left_border,
                win->top_border + rect.y, 0, 32, rect.height * win->ren.im.width * sizeof(nss_color_t),
                (const uint8_t *)(win->ren.im.data+rect.y*win->ren.im.width));
    }
}

void nss_renderer_copy(nss_window_t *win, nss_rect_t dst, int16_t sx, int16_t sy) {
    nss_image_copy(win->ren.im, dst, win->ren.im, sx, sy);

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

void nss_renderer_resize(nss_window_t *win, int16_t new_cw, int16_t new_ch) {
    int16_t delta_x = new_cw - win->cw;
    int16_t delta_y = new_ch - win->ch;

    win->cw = new_cw;
    win->ch = new_ch;

    int16_t width = win->cw * win->char_width;
    int16_t height = win->ch * (win->char_height + win->char_depth);

    int16_t common_w = MIN(width, width  - delta_x * win->char_width);
    int16_t common_h = MIN(height, height - delta_y * (win->char_height + win->char_depth)) ;

    nss_image_t new = nss_create_image_shm(win, width, height);
    nss_image_copy(new, (nss_rect_t){0, 0, common_w, common_h}, win->ren.im, 0, 0);
    SWAP(nss_image_t, win->ren.im, new);
    nss_free_image_shm(&new);

    resize_bounds(win, delta_y);

    if (delta_y > 0) nss_image_draw_rect(win->ren.im,
            (nss_rect_t) { 0, common_h, common_w, height - common_h }, win->bg);
    if (delta_x > 0) nss_image_draw_rect(win->ren.im,
            (nss_rect_t) { common_w, 0, width - common_w, height }, win->bg);

}
