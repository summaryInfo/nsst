#include "features.h"
#ifdef USE_BOXDRAWING
#   include "boxdraw.h"
#endif
#include "font.h"
#include "config.h"
#include "window-private.h"

#include <string.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <xcb/xcb.h>
#include <xcb/xcb_renderutil.h>

typedef struct nss_render_context nss_render_context_t;


struct nss_render_context {
    _Bool has_shm;
};

nss_render_context_t rctx;

/* WARNING: don't try to use shm image functions and normal image functions interchangeably */

static nss_image_t *nss_create_image_shm(nss_window_t *win, int16_t width, int16_t height) {
    if (rctx.has_shm) {
        size_t size = width * height * sizeof(nss_color_t) + sizeof(nss_image_t);

        uint32_t shmid = shmget(IPC_PRIVATE, size, IPC_CREAT | 0666);
        if (shmid == -1U) return 0;

        nss_image_t *im = shmat(shmid, 0, 0);
        if ((void *)im == (void *) -1) goto error;

        im->shmid = shmid;
        im->width = width;
        im->height = height;

        xcb_void_cookie_t c;
        if(!win->ren.shm_seg) {
            win->ren.shm_seg = xcb_generate_id(con);
        } else {
            c = xcb_shm_detach_checked(con, win->ren.shm_seg);
            check_void_cookie(c);
        }

        c = xcb_shm_attach_checked(con, win->ren.shm_seg, shmid, 0);
        if (check_void_cookie(c)) goto error;

        xcb_flush(con);

        return im;
    error:
        if (im) free(im);
        if ((void *)im != (void *) -1 && im) shmdt(im);
        if (shmid != -1U) shmctl(shmid, IPC_RMID, NULL);
        return NULL;
    } else return nss_create_image(width, height);
}

static void nss_free_image_shm(nss_window_t *win, nss_image_t *im) {
    if (rctx.has_shm) {
        shmctl(im->shmid, IPC_RMID, NULL);
        shmdt(im);
    } else {
        nss_free_image(im);
    }
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

    if (need_free) nss_free_font(win->font);

    win->font = new;
    win->font_size = nss_font_get_size(new);

    xcb_void_cookie_t c;

    if (found_cache) {
        win->ren.cache = nss_cache_reference(found->ren.cache);
        win->char_height = found->char_height;
        win->char_depth = found->char_depth;
        win->char_width = found->char_width;
    } else {
        win->ren.cache = nss_create_cache(win->font, win->subpixel_fonts);
        int16_t total = 0, maxd = 0, maxh = 0;
        for (uint32_t i = ' '; i <= '~'; i++) {
            nss_glyph_t *g = nss_cache_fetch(win->ren.cache, i, nss_font_attrib_normal);

            total += g->x_off;
            maxd = MAX(maxd, g->height - g->y);
            maxh = MAX(maxh, g->y);
        }

        win->char_width = total / ('~' - ' ' + 1) + nss_config_integer(NSS_ICONFIG_FONT_SPACING);
        win->char_height = maxh;
        win->char_depth = maxd + nss_config_integer(NSS_ICONFIG_LINE_SPACING);
    }

    win->cw = MAX(1, (win->width - 2*win->left_border) / win->char_width);
    win->ch = MAX(1, (win->height - 2*win->top_border) / (win->char_height + win->char_depth));

    xcb_rectangle_t bound = { 0, 0, win->cw*win->char_width, win->ch*(win->char_depth+win->char_height) };

    if (need_free) {
        xcb_free_gc(con, win->ren.gc);
        nss_free_image_shm(win, win->ren.im);
    } else {
        win->ren.gc = xcb_generate_id(con);
    }

    uint32_t mask2 = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_GRAPHICS_EXPOSURES;
    uint32_t values2[3] = { win->bg, win->bg, 0 };
    c = xcb_create_gc_checked(con, win->ren.gc, win->wid, mask2, values2);
    if (check_void_cookie(c)) {
        warn("Can't create GC");
        return 0;
    }

    win->ren.im = nss_create_image_shm(win, bound.width, bound.height);
    if (!win->ren.im) {
        warn("Can't allocate image");
        return 0;
    }

    nss_image_draw_rect(win->ren.im, (nss_rect_t){0, 0, win->ren.im->width, win->ren.im->height}, win->bg);

    if (need_free)
        nss_term_resize(win->term, win->cw, win->ch);

    return 1; 
}

void nss_renderer_free(nss_window_t *win) {
    xcb_free_gc(con, win->ren.gc);
    if (rctx.has_shm)
        xcb_shm_detach(con, win->ren.shm_seg);
    if (win->ren.im)
        nss_free_image_shm(win, win->ren.im);
    if (win->ren.cache)
        nss_free_cache(win->ren.cache);
}

void nss_free_render_context() {
    /* nothing */
}

void nss_init_render_context() {
    xcb_shm_query_version_cookie_t q = xcb_shm_query_version(con);
    xcb_generic_error_t *er = NULL;
    xcb_shm_query_version_reply_t *qr = xcb_shm_query_version_reply(con, q, &er);
    if (er) free(er);
    if (qr) free(qr);
    if (!(rctx.has_shm = qr && !er)) {
        warn("MIT-SHM is not available");
    }
}

static _Bool draw_cell(nss_window_t *win, coord_t x, coord_t y, nss_color_t *palette, nss_color_t *extra, nss_cell_t *cel) {
    nss_cell_t cell = *cel;

    // Calculate colors
    if ((cell.attr & (nss_attrib_bold | nss_attrib_faint)) == nss_attrib_bold && cell.fg < 8) cell.fg += 8;
    nss_color_t bg = cell.bg < NSS_PALETTE_SIZE ? palette[cell.bg] : extra[cell.bg - NSS_PALETTE_SIZE];
    nss_color_t fg = cell.fg < NSS_PALETTE_SIZE ? palette[cell.fg] : extra[cell.fg - NSS_PALETTE_SIZE];
    if ((cell.attr & (nss_attrib_bold | nss_attrib_faint)) == nss_attrib_faint)
        fg = (fg & 0xFF000000) | ((fg & 0xFEFEFE) >> 1);
    if (cell.attr & nss_attrib_inverse) SWAP(nss_color_t, fg, bg);
    if (cell.attr & nss_attrib_invisible || (cell.attr & nss_attrib_blink && win->blink_state)) fg = bg;

    // U+2588 FULL BLOCK
    if (cell.ch == 0x2588) bg = fg;
    if (cell.ch == ' ' || fg == bg) cell.ch = 0;

    int16_t width = win->char_width*(1 + !!(cell.attr & nss_attrib_wide));
    int16_t height = win->char_depth + win->char_height;

    // Scale position
    x *= win->char_width;
    y *= win->char_height + win->char_depth;

    // And draw...

    // Backround
    nss_image_draw_rect(win->ren.im, (nss_rect_t) { x, y, width, height }, bg);

    // Glyph
    if (cell.ch && fg != bg) {
        nss_glyph_t *glyph = nss_cache_fetch(win->ren.cache, cell.ch, cell.attr & nss_font_attrib_mask);
        nss_rect_t clip = {x, y, width, height};
        nss_image_composite_glyph(win->ren.im, x, y + win->char_height, glyph, fg, clip);
    }

    // Underline
    if ((cell.attr & nss_attrib_underlined) && fg != bg)
            nss_image_draw_rect(win->ren.im, (nss_rect_t){ x, y + win->char_height + 1, win->char_width, win->underline_width }, fg);
    // Strikethough
    if ((cell.attr & nss_attrib_strikethrough) && fg != bg)
        nss_image_draw_rect(win->ren.im, (nss_rect_t){ x, y + 2*win->char_height/3 - win->underline_width/2, win->char_width, win->underline_width }, fg);

    win->ren.shifted = 1;
    cel->attr |= nss_attrib_drawn;

    return !!(cell.attr & nss_attrib_wide);
}

static inline void bound_include(nss_rect_t *rect, int16_t x, int16_t y) {
    rect->x = MIN(x, rect->x);
    rect->y = MIN(y, rect->y);
    rect->width = MAX(x + 1, rect->width);
    rect->height = MAX(y + 1, rect->height);
}

void nss_window_submit_screen(nss_window_t *win, nss_line_t *list, nss_line_t **array, nss_color_t *palette, coord_t cur_x, coord_t cur_y, _Bool cursor) {
    nss_rect_t bound = { win->cw, win->ch, 0, 0 };

    if (win->ren.shifted) {
        bound = win->ren.shift_bound;
        bound = rect_resize(bound, bound.x, bound.y);
        win->ren.shifted = 0;
    }

    _Bool marg = win->cw == cur_x;
    cur_x -= marg;
    if (cursor && win->focused) {
        nss_cell_t cur_cell = array[cur_y]->cell[cur_x - marg];
        if (win->cursor_type == nss_cursor_block)
            cur_cell.attr ^= nss_attrib_inverse;
        array[cur_y]->cell[cur_x].attr |= nss_attrib_drawn;
        if (draw_cell(win, cur_x, cur_y, palette, array[cur_y]->extra, &cur_cell)) {
            array[cur_y]->cell[MIN(cur_x + 1, array[cur_y]->width)].attr &= ~nss_attrib_drawn;
        }
    }

    coord_t h = 0;
    for (; h < win->ch && list; list = list->next, h++) {
        if (win->cw > list->width) {
            nss_image_draw_rect(win->ren.im, (nss_rect_t){
                .x = list->width * win->char_width,
                .y = h * (win->char_height + win->char_depth),
                .width = (win->cw - list->width) * win->char_width,
                .height = win->char_height + win->char_depth
            }, win->bg);
        }
        for (coord_t i = 0; i < MIN(win->cw, list->width); i++)
            if (!(list->cell[i].attr & nss_attrib_drawn) ||
                    (!win->blink_commited && (list->cell[i].attr & nss_attrib_blink))) {
                i += draw_cell(win, i, h, palette, list->extra, &list->cell[i]);
                bound_include(&bound, i, h);
            }
    }
    for (coord_t j = 0; j < win->ch - h; j++) {
        if (win->cw > array[j]->width) {
            nss_image_draw_rect(win->ren.im, (nss_rect_t){
                .x = array[j]->width * win->char_width,
                .y = (j + h) * (win->char_height + win->char_depth),
                .width = (win->cw - array[j]->width) * win->char_width,
                .height = win->char_height + win->char_depth
            }, win->bg);
        }
        for (coord_t i = 0; i < MIN(win->cw, array[j]->width); i++)
            if (!(array[j]->cell[i].attr & nss_attrib_drawn) ||
                    (!win->blink_commited && (array[j]->cell[i].attr & nss_attrib_blink))) {
                i += draw_cell(win, i, j + h, palette, array[j]->extra, &array[j]->cell[i]);
                bound_include(&bound, i, j + h);
            }
    }

    if (cursor) {
        bound_include(&bound, cur_x, cur_y);
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
        for (size_t i = 0; i < count; i++) {
            nss_image_draw_rect(win->ren.im, rects[i + off], win->cursor_fg);
        }
    }

    if (bound.x != win->cw) {
        bound = rect_resize(bound, -bound.x, -bound.y);
        bound = rect_scale_up(bound, win->char_width, win->char_height + win->char_depth);
        nss_renderer_update(win, bound);
    }
}
void nss_renderer_clear(nss_window_t *win, size_t count, nss_rect_t *rects) {
    if (count) xcb_poly_fill_rectangle(con, win->wid, win->ren.gc, count, (xcb_rectangle_t*)rects);
    /*
     * // Wrong way
    if (count) {
        xcb_render_color_t color = MAKE_COLOR(win->bg);
        xcb_render_fill_rectangles(con, XCB_RENDER_PICT_OP_SRC, win->ren.pic, color, count, (xcb_rectangle_t*)rects);
    }
    */
}
void nss_renderer_update(nss_window_t *win, nss_rect_t rect) {
    if (rctx.has_shm) {
        xcb_shm_put_image(con, win->wid, win->ren.gc, win->ren.im->width, win->ren.im->height, rect.x, rect.y, rect.width, rect.height,
                rect.x + win->left_border, rect.y + win->top_border, 32, XCB_IMAGE_FORMAT_Z_PIXMAP, 0, win->ren.shm_seg, sizeof(nss_image_t));
    } else {
        xcb_put_image(con, XCB_IMAGE_FORMAT_Z_PIXMAP, win->wid, win->ren.gc,
                win->ren.im->width, rect.height, win->left_border,
                win->top_border + rect.y, 0, 32, rect.height * win->ren.im->width * sizeof(nss_color_t),
                (const uint8_t *)(win->ren.im->data+rect.y*win->ren.im->width));
    }
}
void nss_renderer_background_changed(nss_window_t *win) {
    uint32_t values2[2];
    values2[0] = values2[1] = win->bg;
    xcb_change_window_attributes(con, win->wid, XCB_CW_BACK_PIXEL, values2);
    xcb_change_gc(con, win->ren.gc, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, values2);
}
void nss_renderer_copy(nss_window_t *win, nss_rect_t dst, int16_t sx, int16_t sy) {
    nss_image_copy(win->ren.im, dst, win->ren.im, sx, sy);

    win->ren.shifted = 1;
    dst = rect_resize(dst, win->char_width - 1, win->char_height + win->char_depth - 1);
    dst = rect_scale_down(dst, win->char_width, win->char_height + win->char_depth);
    win->ren.shift_bound = win->ren.shifted ? rect_union(win->ren.shift_bound, dst) : dst;

    /*
    xcb_copy_area(con, win->ren.pid, win->ren.pid, win->ren.gc, sx, sy, dst.x, dst.y, dst.width, dst.height);
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

    nss_image_t *new = nss_create_image_shm(win, width, height);
    nss_image_copy(new, (nss_rect_t){0, 0, common_w, common_h}, win->ren.im, 0, 0);
    SWAP(nss_image_t *, win->ren.im, new);
    nss_free_image_shm(win, new);
        
    if (delta_y > 0)
        nss_image_draw_rect(win->ren.im, (nss_rect_t) {
                0, win->ch - delta_y, MIN(win->cw, win->cw - delta_x), delta_y }, win->bg);
    if (delta_x > 0)
        nss_image_draw_rect(win->ren.im, (nss_rect_t) {
                win->cw - delta_x, 0, delta_x, MAX(win->ch, win->ch - delta_y) }, win->bg);

}

