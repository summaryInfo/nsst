/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#include "feature.h"
#include "config.h"
#include "font.h"
#include "window-private.h"

#include <string.h>
#include <xcb/xcb.h>

#if USE_X11SHM
#   include <sys/shm.h>
#   include <sys/ipc.h>
#   include <xcb/shm.h>
#endif

typedef struct nss_render_context nss_render_context_t;

struct nss_cellspec {
    nss_color_t fg;
    nss_color_t bg;
    nss_char_t ch;
    uint8_t face;
    _Bool underlined;
    _Bool stroke;
    _Bool wide;
};

inline static struct nss_cellspec describe_cell(nss_cell_t cell, nss_color_t *palette, nss_color_t *extra, _Bool blink, _Bool selected) {
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

#if USE_X11SHM

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

/* Reload font using win->font_size and win->font_name */
_Bool nss_renderer_reload_font(nss_window_t *win, _Bool need_free) {
    //Try find already existing font
    _Bool found_font = 0, found_cache = 0;
    nss_window_t *found = 0;
    for (nss_window_t *src = win_list_head; src; src = src->next) {
        if ((src->font_size == win->font_size || win->font_size == 0) &&
           !strcmp(win->font_name, src->font_name) && src != win) {
            found_font = 1;
            found = src;
            if (src->subpixel_fonts == win->subpixel_fonts) {
                found_cache = 1;
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

    if (need_free) {
        nss_free_cache(win->font_cache);
        nss_free_font(win->font);
    }

    win->font = new;
    win->font_size = nss_font_get_size(new);

    if (found_cache)
        win->font_cache = nss_cache_reference(found->font_cache);
    else
        win->font_cache = nss_create_cache(win->font, win->subpixel_fonts);
    nss_cache_font_dim(win->font_cache, &win->char_width, &win->char_height, &win->char_depth);

    if (need_free) nss_window_handle_resize(win, win->width, win->height);
    else {
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

void nss_window_submit_screen(nss_window_t *win, nss_line_iter_t *it, nss_color_t *palette, nss_coord_t cur_x, nss_coord_t cur_y, _Bool cursor) {
    _Bool marg = win->cw == cur_x;
    cur_x -= marg;

    for (nss_line_t *line; (line = line_iter_next(it));) {
        _Bool damaged = 0;
        nss_rect_t l_bound = {0, line_iter_y(it), 0, 1};
        for (nss_coord_t i = 0; i < MIN(win->cw, line->width); i++) {
            if (!(line->cell[i].attr & nss_attrib_drawn) || (!win->blink_commited && (line->cell[i].attr & nss_attrib_blink))) {
                nss_cell_t cel = line->cell[i];

                if (line_iter_y(it) == cur_y && i == cur_x && cursor &&
                    win->focused && win->cursor_type == nss_cursor_block) cel.attr ^= nss_attrib_inverse;

                struct nss_cellspec spec = describe_cell(cel, palette,
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
                l_bound.width = i;
                damaged = 1;
            }
        }
        if (damaged) {
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
        for (size_t i = 0; i < count; i++)
            nss_image_draw_rect(win->ren.im, rects[i + off], win->cursor_fg);
    }


    if (win->ren.boundc) {
        optimize_bounds(win->ren.bounds, &win->ren.boundc, rctx.has_shm);
        for (size_t k = 0; k < win->ren.boundc; k++) {
            nss_renderer_update(win, rect_scale_up(win->ren.bounds[k], win->char_width, win->char_depth + win->char_height));
        }
        win->ren.boundc = 0;
    }

    win->damaged_y0 = win->damaged_y1 = 0;
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

#else //USE_X11SHM is not defined

struct nss_render_context {
    xcb_render_pictformat_t pfargb;
    xcb_render_pictformat_t pfalpha;

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

static nss_render_context_t rctx;

#define WORDS_IN_MESSAGE 256
#define HEADER_WORDS ((sizeof(nss_glyph_mesg_t)+sizeof(uint32_t))/sizeof(uint32_t))
#define CHARS_PER_MESG (WORDS_IN_MESSAGE - HEADER_WORDS)

#define CB(c) (((c) & 0xff) * 0x101)
#define CG(c) ((((c) >> 8) & 0xff) * 0x101)
#define CR(c) ((((c) >> 16) & 0xff) * 0x101)
#define CA(c) ((((c) >> 24) & 0xff) * 0x101)
#define MAKE_COLOR(c) {.red=CR(c), .green=CG(c), .blue=CB(c), .alpha=CA(c)}

static void register_glyph(nss_window_t *win, uint32_t ch, nss_glyph_t * glyph) {
    xcb_render_glyphinfo_t spec = {
        .width = glyph->width, .height = glyph->height,
        .x = glyph->x, .y = glyph->y,
        .x_off = glyph->x_off, .y_off = glyph->y_off
    };
    xcb_void_cookie_t c;
    c = xcb_render_add_glyphs_checked(con, win->ren.gsid, 1, &ch, &spec, glyph->height*glyph->stride, glyph->data);
    if (check_void_cookie(c))
        warn("Can't add glyph");
}

/* Reload font using win->font_size and win->font_name */
_Bool nss_renderer_reload_font(nss_window_t *win, _Bool need_free) {
    //Try find already existing font
    _Bool found_font = 0, found_cache = 0;
    nss_window_t *found = 0;
    for (nss_window_t *src = win_list_head; src; src = src->next) {
        if ((src->font_size == win->font_size || win->font_size == 0) &&
           !strcmp(win->font_name, src->font_name) && src != win) {
            found_font = 1;
            found = src;
            if (src->subpixel_fonts == win->subpixel_fonts) {
                found_cache = 1;
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

    if (need_free) {
        nss_free_cache(win->font_cache);
        nss_free_font(win->font);
    }

    win->font = new;
    win->font_size = nss_font_get_size(new);

    win->ren.pfglyph = win->subpixel_fonts ? rctx.pfargb : rctx.pfalpha;

    xcb_void_cookie_t c;

    if (need_free) {
        c = xcb_render_free_glyph_set_checked(con, win->ren.gsid);
        if (check_void_cookie(c))
            warn("Can't free glyph set");
    }
    else win->ren.gsid = xcb_generate_id(con);

    if (found_cache) {
        win->font_cache = nss_cache_reference(found->font_cache);
        c = xcb_render_reference_glyph_set_checked(con, win->ren.gsid, found->ren.gsid);
        if (check_void_cookie(c))
            warn("Can't reference glyph set");

        nss_cache_font_dim(win->font_cache, &win->char_width, &win->char_height, &win->char_depth);
    } else {
        win->font_cache = nss_create_cache(win->font, win->subpixel_fonts);
        c = xcb_render_create_glyph_set_checked(con, win->ren.gsid, win->ren.pfglyph);
        if (check_void_cookie(c))
            warn("Can't create glyph set");

        //Preload ASCII
        nss_glyph_t *glyphs['~' - ' ' + 1][nss_font_attrib_max] = {{ NULL }};
        for (nss_char_t i = ' '; i <= '~'; i++)
            for (size_t j = 0; j < nss_font_attrib_max; j++)
                glyphs[i - ' '][j] = nss_cache_fetch(win->font_cache, i, j);

        nss_cache_font_dim(win->font_cache, &win->char_width, &win->char_height, &win->char_depth);

        for (nss_char_t i = ' '; i <= '~'; i++) {
            for (size_t j = 0; j < nss_font_attrib_max; j++) {
                glyphs[i - ' '][j]->x_off = win->char_width;
                register_glyph(win, i | (j << 24), glyphs[i - ' '][j]);
                free(glyphs[i - ' '][j]);
            }
        }
    }

    if (need_free) {
        nss_window_handle_resize(win, win->width, win->height);
    } else {
        win->cw = MAX(1, (win->width - 2*win->left_border) / win->char_width);
        win->ch = MAX(1, (win->height - 2*win->top_border) / (win->char_height + win->char_depth));

        xcb_rectangle_t bound = { 0, 0, win->cw*win->char_width, win->ch*(win->char_depth+win->char_height) };

        win->ren.pid1 = xcb_generate_id(con);
        win->ren.pid2 = xcb_generate_id(con);

        c = xcb_create_pixmap_checked(con, TRUE_COLOR_ALPHA_DEPTH, win->ren.pid1, win->wid, bound.width, bound.height );
        if (check_void_cookie(c)) {
            warn("Can't create pixmap");
            return 0;
        }

        uint32_t mask3 = XCB_RENDER_CP_GRAPHICS_EXPOSURE | XCB_RENDER_CP_POLY_EDGE | XCB_RENDER_CP_POLY_MODE;
        uint32_t values3[3] = { 0, XCB_RENDER_POLY_EDGE_SMOOTH, XCB_RENDER_POLY_MODE_IMPRECISE };

        win->ren.pic1 = xcb_generate_id(con);
        win->ren.pic2 = xcb_generate_id(con);

        c = xcb_render_create_picture_checked(con, win->ren.pic1, win->ren.pid1, rctx.pfargb, mask3, values3);
        if (check_void_cookie(c)) {
            warn("Can't create XRender picture");
            return 0;
        }

        xcb_render_color_t color = MAKE_COLOR(win->bg);
        xcb_render_fill_rectangles(con, XCB_RENDER_PICT_OP_SRC, win->ren.pic1, color, 1, &bound);

        xcb_pixmap_t pid = xcb_generate_id(con);
        c = xcb_create_pixmap_checked(con, TRUE_COLOR_ALPHA_DEPTH, pid, win->wid, 1, 1);
        if (check_void_cookie(c)) {
            warn("Can't create pixmap");
            nss_free_window(win);
            return NULL;
        }

        win->ren.pen = xcb_generate_id(con);
        uint32_t values4[1] = { XCB_RENDER_REPEAT_NORMAL };
        c = xcb_render_create_picture_checked(con, win->ren.pen, pid, rctx.pfargb, XCB_RENDER_CP_REPEAT, values4);
        if (check_void_cookie(c)) {
            warn("Can't create picture");
            nss_free_window(win);
            return NULL;
        }
        xcb_free_pixmap(con, pid);
    }

    return 1;
}

void nss_renderer_free(nss_window_t *win) {
    xcb_render_free_picture(con, win->ren.pen);
    xcb_render_free_picture(con, win->ren.pic1);
    xcb_free_pixmap(con, win->ren.pid1);
    xcb_render_free_glyph_set(con, win->ren.gsid);
}

void nss_free_render_context() {
    free(rctx.buffer);
    free(rctx.cbuffer);
}

void nss_init_render_context() {
    rctx.buffer = malloc(WORDS_IN_MESSAGE * sizeof(uint32_t));
    if (!rctx.buffer) {
        xcb_disconnect(con);
        die("Can't allocate buffer");
    }
    rctx.bufsize = WORDS_IN_MESSAGE * sizeof(uint32_t);
    rctx.cbuffer = malloc(128 * sizeof(rctx.cbuffer[0]));
    if (!rctx.cbuffer) {
        xcb_disconnect(con);
        die("Can't allocate cbuffer");
    }
    rctx.cbufsize = 128;

    // Check if XRender is present
    xcb_render_query_version_cookie_t vc = xcb_render_query_version(con, XCB_RENDER_MAJOR_VERSION, XCB_RENDER_MINOR_VERSION);
    xcb_generic_error_t *err;
    xcb_render_query_version_reply_t *rep = xcb_render_query_version_reply(con, vc, &err);
    // Any version is OK, so don't check
    free(rep);

    if (err) {
        uint8_t erc = err->error_code;
        xcb_disconnect(con);
        die("XRender not detected: %"PRIu8, erc);
    }

    xcb_render_query_pict_formats_cookie_t pfc = xcb_render_query_pict_formats(con);
    xcb_render_query_pict_formats_reply_t *pfr = xcb_render_query_pict_formats_reply(con, pfc, &err);

    if (err) {
        uint8_t erc = err->error_code;
        xcb_disconnect(con);
        die("Can't query picture formats: %"PRIu8, erc);
    }

    xcb_render_pictforminfo_iterator_t pfit =  xcb_render_query_pict_formats_formats_iterator(pfr);
    for (; pfit.rem; xcb_render_pictforminfo_next(&pfit)) {
        if (pfit.data->depth == TRUE_COLOR_ALPHA_DEPTH && pfit.data->type == XCB_RENDER_PICT_TYPE_DIRECT &&
           pfit.data->direct.red_mask == 0xff && pfit.data->direct.green_mask == 0xff &&
           pfit.data->direct.blue_mask == 0xff && pfit.data->direct.alpha_mask == 0xff &&
           pfit.data->direct.red_shift == 16 && pfit.data->direct.green_shift == 8 &&
           pfit.data->direct.blue_shift == 0 && pfit.data->direct.alpha_shift == 24 ) {
               rctx.pfargb = pfit.data->id;
        }
        if (pfit.data->depth == 8 && pfit.data->type == XCB_RENDER_PICT_TYPE_DIRECT &&
           pfit.data->direct.alpha_mask == 0xff && pfit.data->direct.alpha_shift == 0) {
               rctx.pfalpha = pfit.data->id;
        }
    }

    free(pfr);

    if (rctx.pfargb == 0 || rctx.pfalpha == 0) {
        xcb_disconnect(con);
        die("Can't find suitable picture format");
    }
}

static void push_rect(xcb_rectangle_t *rect) {
    if (rctx.bufpos + sizeof(xcb_rectangle_t) >= rctx.bufsize) {
        size_t new_size = MAX(3 * rctx.bufsize / 2, 16 * sizeof(xcb_rectangle_t));
        uint8_t *new = realloc(rctx.buffer, new_size);
        if (!new) return;
        rctx.buffer = new;
        rctx.bufsize = new_size;
    }

    memcpy(rctx.buffer + rctx.bufpos, rect, sizeof(xcb_rectangle_t));
    rctx.bufpos += sizeof(xcb_rectangle_t);
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
void nss_window_submit_screen(nss_window_t *win, nss_line_iter_t *it, nss_color_t *palette, nss_coord_t cur_x, nss_coord_t cur_y, _Bool cursor) {
    rctx.cbufpos = 0;
    rctx.bufpos = 0;

    _Bool marg = win->cw == cur_x;
    cur_x -= marg;


    for (nss_line_t *line; (line = line_iter_next(it));) {
        if (win->cw > line->width) {
            push_rect(&(xcb_rectangle_t){
                .x = line->width * win->char_width,
                .y = line_iter_y(it) * (win->char_height + win->char_depth),
                .width = (win->cw - line->width) * win->char_width,
                .height = win->char_height + win->char_depth
            });
        }
        for (nss_coord_t i = 0; i < MIN(win->cw, line->width); i++) {
            if (!(line->cell[i].attr & nss_attrib_drawn) || (!win->blink_commited && (line->cell[i].attr & nss_attrib_blink))) {
                nss_cell_t cel = line->cell[i];

                if (line_iter_y(it) == cur_y && i == cur_x && cursor &&
                    win->focused && win->cursor_type == nss_cursor_block) cel.attr ^= nss_attrib_inverse;

                struct nss_cellspec spec = describe_cell(cel, palette,
                        line->pal->data, win->blink_state, nss_term_is_selected(win->term, i, line_iter_y(it)));

                if (!nss_cache_is_fetched(win->font_cache, spec.ch)) {
                    for (size_t j = 0; j < nss_font_attrib_max; j++) {
                        nss_glyph_t *glyph = nss_cache_fetch(win->font_cache, spec.ch, j);
                        //In case of non-monospace fonts
                        glyph->x_off = win->char_width;
                        register_glyph(win, spec.ch | (j << 24) , glyph);
                        free(glyph);
                    }
                }

                if (2*(rctx.cbufpos + 1) >= rctx.cbufsize) {
                    size_t new_size = MAX(3 * rctx.cbufsize / 2, 2 * rctx.cbufpos + 1);
                    struct cell_desc *new = realloc(rctx.cbuffer, new_size * sizeof(*rctx.cbuffer));
                    if (!new) return;
                    rctx.cbuffer = new;
                    rctx.cbufsize = new_size;
                }

                rctx.cbuffer[rctx.cbufpos++] = (struct cell_desc) {
                    .x = i * win->char_width,
                    .y = line_iter_y(it) * (win->char_height + win->char_depth),
                    .fg = spec.fg, .bg = spec.bg,
                    .glyph = spec.ch | (spec.face << 24),
                    .wide = spec.wide,
                    .underlined = spec.underlined,
                    .strikethrough = spec.stroke
                };

                line->cell[i].attr |= nss_attrib_drawn;
            }
        }
    }

    if (rctx.bufpos) {
        xcb_render_color_t col = MAKE_COLOR(win->bg);
        xcb_render_fill_rectangles(con, XCB_RENDER_PICT_OP_SRC, win->ren.pic1, col,
            rctx.bufpos/sizeof(xcb_rectangle_t), (xcb_rectangle_t *)rctx.buffer);
    }

    //qsort(rctx.cbuffer, rctx.cbufpos, sizeof(rctx.cbuffer[0]), cmp_by_bg);
    //shell_sort_bg(rctx.cbuffer, rctx.cbufpos);
    merge_sort_bg(rctx.cbuffer, rctx.cbufpos);

    // Draw background
    for (size_t i = 0; i < rctx.cbufpos; ) {
        rctx.bufpos = 0;
        size_t j = i;
        while(i < rctx.cbufpos && rctx.cbuffer[i].bg == rctx.cbuffer[j].bg) {
            size_t k = i;
            do i++;
            while (i < rctx.cbufpos && rctx.cbuffer[k].y == rctx.cbuffer[i].y &&
                    rctx.cbuffer[i - 1].x + win->char_width == rctx.cbuffer[i].x &&
                    rctx.cbuffer[k].bg == rctx.cbuffer[i].bg);
            push_rect(&(xcb_rectangle_t) {
                .x = rctx.cbuffer[k].x,
                .y = rctx.cbuffer[k].y,
                .width = rctx.cbuffer[i - 1].x - rctx.cbuffer[k].x + win->char_width,
                .height = win->char_depth + win->char_height
            });

        }
        if (rctx.bufpos) {
            xcb_render_color_t col = MAKE_COLOR(rctx.cbuffer[j].bg);
            xcb_render_fill_rectangles(con, XCB_RENDER_PICT_OP_SRC, win->ren.pic1, col,
                rctx.bufpos/sizeof(xcb_rectangle_t), (xcb_rectangle_t *)rctx.buffer);
        }
    }

    // Set clip rectangles for text rendering
    rctx.bufpos = 0;
    for (size_t i = 0; i < rctx.cbufpos; ) {
        while (i < rctx.cbufpos && !rctx.cbuffer[i].glyph) i++;
        if (i >= rctx.cbufpos) break;
        size_t k = i;
        do i++;
        while (i < rctx.cbufpos && rctx.cbuffer[k].y == rctx.cbuffer[i].y &&
                rctx.cbuffer[i - 1].x + win->char_width == rctx.cbuffer[i].x && rctx.cbuffer[i].glyph);
        push_rect(&(xcb_rectangle_t) {
            .x = rctx.cbuffer[k].x,
            .y = rctx.cbuffer[k].y,
            .width = rctx.cbuffer[i - 1].x - rctx.cbuffer[k].x + win->char_width*(1 + rctx.cbuffer[k].wide),
            .height = win->char_depth + win->char_height
        });
    }
    if (rctx.bufpos)
        xcb_render_set_picture_clip_rectangles(con, win->ren.pic1, 0, 0,
            rctx.bufpos/sizeof(xcb_rectangle_t), (xcb_rectangle_t *)rctx.buffer);

    //qsort(rctx.cbuffer, rctx.cbufpos, sizeof(rctx.cbuffer[0]), cmp_by_fg);
    //shell_sort_fg(rctx.cbuffer, rctx.cbufpos);
    merge_sort_fg(rctx.cbuffer, rctx.cbufpos);

    // Draw chars
    for (size_t i = 0; i < rctx.cbufpos; ) {
        while (i < rctx.cbufpos && !rctx.cbuffer[i].glyph) i++;
        if (i >= rctx.cbufpos) break;

        xcb_render_color_t col = MAKE_COLOR(rctx.cbuffer[i].fg);
        xcb_rectangle_t rect2 = { .x = 0, .y = 0, .width = 1, .height = 1 };
        xcb_render_fill_rectangles(con, XCB_RENDER_PICT_OP_SRC, win->ren.pen, col, 1, &rect2);

        rctx.bufpos = 0;
        int16_t ox = 0, oy = 0;
        size_t j = i;

        while(i < rctx.cbufpos && rctx.cbuffer[i].fg == rctx.cbuffer[j].fg) {
            if (rctx.bufpos + WORDS_IN_MESSAGE * sizeof(uint32_t) >= rctx.bufsize) {
                uint8_t *new = realloc(rctx.buffer, rctx.bufsize + WORDS_IN_MESSAGE * sizeof(uint32_t));
                if (!new) break;
                rctx.buffer = new;
                rctx.bufsize += WORDS_IN_MESSAGE * sizeof(uint32_t);
            }
            nss_glyph_mesg_t *head = (nss_glyph_mesg_t *)(rctx.buffer + rctx.bufpos);
            rctx.bufpos += sizeof(*head);
            size_t k = i;
            *head = (nss_glyph_mesg_t){
                .dx = rctx.cbuffer[k].x - ox,
                .dy = rctx.cbuffer[k].y + win->char_height - oy
            };
            do {
                uint32_t glyph = rctx.cbuffer[i].glyph;
                memcpy(rctx.buffer + rctx.bufpos, &glyph, sizeof(uint32_t));
                rctx.bufpos += sizeof(uint32_t);
                i++;
            } while (i < rctx.cbufpos && rctx.cbuffer[k].y == rctx.cbuffer[i].y &&
                    rctx.cbuffer[i - 1].x + win->char_width == rctx.cbuffer[i].x &&
                    rctx.cbuffer[k].fg == rctx.cbuffer[i].fg &&
                    rctx.cbuffer[i].glyph && i - k < CHARS_PER_MESG);
            head->len = i - k;

            ox = rctx.cbuffer[i - 1].x + win->char_width;
            oy = rctx.cbuffer[i - 1].y + win->char_height;

            while (i < rctx.cbufpos && !rctx.cbuffer[i].glyph) i++;
        }
        if (rctx.bufpos)
            xcb_render_composite_glyphs_32(con, XCB_RENDER_PICT_OP_OVER,
                                           win->ren.pen, win->ren.pic1, win->ren.pfglyph, win->ren.gsid,
                                           0, 0, rctx.bufpos, rctx.buffer);
    }

    if (rctx.cbufpos)
        xcb_render_set_picture_clip_rectangles(con, win->ren.pic1, 0, 0, 1, &(xcb_rectangle_t){
                0, 0, win->cw * win->char_width, win->ch * (win->char_height + win->char_depth)});

    // Draw underline and strikethrough lines
    for (size_t i = 0; i < rctx.cbufpos; ) {
        while(i < rctx.cbufpos && !rctx.cbuffer[i].underlined && !rctx.cbuffer[i].strikethrough) i++;
        if (i >= rctx.cbufpos) break;
        rctx.bufpos = 0;
        size_t j = i;
        while (i < rctx.cbufpos && rctx.cbuffer[j].fg == rctx.cbuffer[i].fg) {
            while (i < rctx.cbufpos && rctx.cbuffer[j].fg == rctx.cbuffer[i].fg && !rctx.cbuffer[i].underlined) i++;
            if (i >= rctx.cbufpos || !rctx.cbuffer[i].underlined) break;
            size_t k = i;
            do i++;
            while (i < rctx.cbufpos && rctx.cbuffer[k].y == rctx.cbuffer[i].y &&
                    rctx.cbuffer[i - 1].x + win->char_width == rctx.cbuffer[i].x &&
                    rctx.cbuffer[k].fg == rctx.cbuffer[i].fg && rctx.cbuffer[i].underlined);
            push_rect(&(xcb_rectangle_t) {
                .x = rctx.cbuffer[k].x,
                .y = rctx.cbuffer[k].y + win->char_height + 1,
                .width = rctx.cbuffer[i - 1].x + win->char_width - rctx.cbuffer[k].x,
                .height = win->underline_width
            });
        }
        i = j;
        while (i < rctx.cbufpos && rctx.cbuffer[j].fg == rctx.cbuffer[i].fg) {
            while (i < rctx.cbufpos && rctx.cbuffer[j].fg == rctx.cbuffer[i].fg && !(rctx.cbuffer[i].strikethrough)) i++;
            if (i >= rctx.cbufpos || !rctx.cbuffer[i].strikethrough) break;
            size_t k = i;
            do i++;
            while (i < rctx.cbufpos && rctx.cbuffer[k].y == rctx.cbuffer[i].y &&
                    rctx.cbuffer[i - 1].x + win->char_width == rctx.cbuffer[i].x &&
                    rctx.cbuffer[k].fg == rctx.cbuffer[i].fg && rctx.cbuffer[i].strikethrough);
            push_rect(&(xcb_rectangle_t) {
                .x = rctx.cbuffer[k].x,
                .y = rctx.cbuffer[k].y + 2*win->char_height/3 - win->underline_width/2,
                .width = rctx.cbuffer[i - 1].x + win->char_width - rctx.cbuffer[k].x,
                .height = win->underline_width
            });
        }
        if (rctx.bufpos) {
            xcb_render_color_t col = MAKE_COLOR(rctx.cbuffer[j].fg);
            xcb_render_fill_rectangles(con, XCB_RENDER_PICT_OP_SRC, win->ren.pic1, col,
                rctx.bufpos/sizeof(xcb_rectangle_t), (xcb_rectangle_t *)rctx.buffer);
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
            xcb_render_fill_rectangles(con, XCB_RENDER_PICT_OP_OVER, win->ren.pic1, c, count, rects + off);
        }
    }

    if (rctx.cbufpos)
        nss_renderer_update(win, rect_scale_up((nss_rect_t){0, 0, win->cw, win->ch},
                win->char_width, win->char_height + win->char_depth));

    win->damaged_y0 = win->damaged_y1 = 0;
}

void nss_renderer_update(nss_window_t *win, nss_rect_t rect) {
    xcb_copy_area(con, win->ren.pid1, win->wid, win->gc, rect.x, rect.y,
            rect.x + win->left_border, rect.y + win->top_border, rect.width, rect.height);
}

void nss_renderer_copy(nss_window_t *win, nss_rect_t dst, int16_t sx, int16_t sy) {
    xcb_copy_area(con, win->ren.pid1, win->ren.pid1, win->gc, sx, sy, dst.x, dst.y, dst.width, dst.height);
    /*
    xcb_render_composite(con, XCB_RENDER_PICT_OP_SRC, win->ren.pic, 0, win->ren.pic, sx, sy, 0, 0, dst.x, dst.y, dst.width, dst.height);
    */
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
    xcb_create_pixmap(con, TRUE_COLOR_ALPHA_DEPTH, win->ren.pid2, win->wid, width, height);
    uint32_t mask3 = XCB_RENDER_CP_GRAPHICS_EXPOSURE | XCB_RENDER_CP_POLY_EDGE | XCB_RENDER_CP_POLY_MODE;
    uint32_t values3[3] = { 0, XCB_RENDER_POLY_EDGE_SMOOTH, XCB_RENDER_POLY_MODE_IMPRECISE };
    xcb_render_create_picture(con, win->ren.pic2, win->ren.pid2, rctx.pfargb, mask3, values3);

    xcb_render_composite(con, XCB_RENDER_PICT_OP_SRC, win->ren.pic1, 0, win->ren.pic2, 0, 0, 0, 0, 0, 0, common_w, common_h);

    SWAP(xcb_pixmap_t, win->ren.pid1, win->ren.pid2);
    SWAP(xcb_render_picture_t, win->ren.pic1, win->ren.pic2);

    xcb_render_free_picture(con, win->ren.pic2);
    xcb_free_pixmap(con, win->ren.pid2);

    nss_rect_t rectv[2];
    size_t rectc= 0;

    if (delta_y > 0)
        rectv[rectc++] = (nss_rect_t) { 0, win->ch - delta_y, MIN(win->cw, win->cw - delta_x), delta_y };
    if (delta_x > 0)
        rectv[rectc++] = (nss_rect_t) { win->cw - delta_x, 0, delta_x, MAX(win->ch, win->ch - delta_y) };

    for (size_t i = 0; i < rectc; i++)
        rectv[i] = rect_scale_up(rectv[i], win->char_width, win->char_height + win->char_depth);

    xcb_render_color_t color = MAKE_COLOR(win->bg);
    xcb_render_fill_rectangles(con, XCB_RENDER_PICT_OP_SRC, win->ren.pic1, color, rectc, (xcb_rectangle_t*)rectv);
}

#endif
