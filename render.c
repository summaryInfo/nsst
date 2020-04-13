#include "features.h"
#ifdef USE_BOXDRAWING
#   include "boxdraw.h"
#endif
#include "font.h"
#include "config.h"
#include "window-private.h"

#include <string.h>
#include <xcb/xcb.h>
#include <xcb/xcb_renderutil.h>

typedef struct nss_glyph_mesg {
    uint8_t len;
    uint8_t pad[3];
    int16_t dx, dy;
    uint8_t data[];
} nss_glyph_mesg_t;


typedef struct nss_render_context nss_render_context_t;


struct nss_render_context {
    struct cell_desc {
        int16_t x;
        int16_t y;
        nss_color_t bg;
        nss_color_t fg;
        uint32_t ch : 24;
        uint32_t face : 5;
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

nss_render_context_t rctx;

#define WORDS_IN_MESSAGE 256
#define HEADER_WORDS ((sizeof(nss_glyph_mesg_t)+sizeof(uint32_t))/sizeof(uint32_t))
#define CHARS_PER_MESG (WORDS_IN_MESSAGE - HEADER_WORDS)

#define CB(c) (((c) & 0xff) * 0x101)
#define CG(c) ((((c) >> 8) & 0xff) * 0x101)
#define CR(c) ((((c) >> 16) & 0xff) * 0x101)
#define CA(c) ((((c) >> 24) & 0xff) * 0x101)
#define MAKE_COLOR(c) {.red=CR(c), .green=CG(c), .blue=CB(c), .alpha=CA(c)}

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
            nss_glyph_t *g = nss_cache_glyph(win->ren.cache, nss_font_attrib_normal, i);

            total += g->x_off;
            maxd = MAX(maxd, g->height - g->y);
            maxh = MAX(maxh, g->y);

            nss_cache_post(win->ren.cache, nss_font_attrib_normal, i, g);
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
        nss_free_image(win->ren.im);
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

    win->ren.im = nss_create_image(bound.width, bound.height);
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
    if (win->ren.im)
        nss_free_image(win->ren.im);
    if (win->ren.cache)
        nss_free_cache(win->ren.cache);
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
}

static void push_cell(nss_window_t *win, coord_t x, coord_t y, nss_color_t *palette, nss_color_t *extra, nss_cell_t *cel) {
    nss_cell_t cell = *cel;

    if ((cell.attr & (nss_attrib_bold | nss_attrib_faint)) == nss_attrib_bold && cell.fg < 8) cell.fg += 8;
    nss_color_t bg = cell.bg < NSS_PALETTE_SIZE ? palette[cell.bg] : extra[cell.bg - NSS_PALETTE_SIZE];
    nss_color_t fg = cell.fg < NSS_PALETTE_SIZE ? palette[cell.fg] : extra[cell.fg - NSS_PALETTE_SIZE];
    if ((cell.attr & (nss_attrib_bold | nss_attrib_faint)) == nss_attrib_faint)
        fg = (fg & 0xFF000000) | ((fg & 0xFEFEFE) >> 1);
    if (cell.attr & nss_attrib_inverse) SWAP(nss_color_t, fg, bg);
    if (cell.attr & nss_attrib_invisible || (cell.attr & nss_attrib_blink && win->blink_state)) fg = bg;

    if (2*(rctx.cbufpos + 1) >= rctx.cbufsize) {
        size_t new_size = MAX(3 * rctx.cbufsize / 2, 2 * rctx.cbufpos + 1);
        struct cell_desc *new = realloc(rctx.cbuffer, new_size * sizeof(*rctx.cbuffer));
        if (!new) return;
        rctx.cbuffer = new;
        rctx.cbufsize = new_size;
    }

    // U+2588 FULL BLOCK
    if (cell.ch == 0x2588) bg = fg;
    if (cell.ch == ' ' || fg == bg) cell.ch = 0;
    rctx.cbuffer[rctx.cbufpos++] = (struct cell_desc) {
        .x = x * win->char_width,
        .y = y * (win->char_height + win->char_depth),
        .fg = fg, .bg = bg,
        .ch = cell.ch,
        .face = cell.attr & nss_font_attrib_mask,
        .wide = !!(cell.attr & nss_attrib_wide),
        .underlined = !!(cell.attr & nss_attrib_underlined) && (fg != bg),
        .strikethrough = !!(cell.attr & nss_attrib_strikethrough) && (fg != bg),
    };

    cel->attr |= nss_attrib_drawn;
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
void nss_window_submit_screen(nss_window_t *win, nss_line_t *list, nss_line_t **array, nss_color_t *palette, coord_t cur_x, coord_t cur_y, _Bool cursor) {
    rctx.cbufpos = 0;
    rctx.bufpos = 0;

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
            nss_image_draw_rect(win->ren.im, (nss_rect_t){
                .x = list->width * win->char_width,
                .y = h * (win->char_height + win->char_depth),
                .width = (win->cw - list->width) * win->char_width,
                .height = win->char_height + win->char_depth
            }, win->bg);
        }
        for (coord_t i = 0; i < MIN(win->cw, list->width); i++)
            if (!(list->cell[i].attr & nss_attrib_drawn) ||
                    (!win->blink_commited && (list->cell[i].attr & nss_attrib_blink)))
                push_cell(win, i, h, palette, list->extra, &list->cell[i]);
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
                    (!win->blink_commited && (array[j]->cell[i].attr & nss_attrib_blink)))
                push_cell(win, i, j + h, palette, array[j]->extra, &array[j]->cell[i]);
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
            nss_image_draw_rect(win->ren.im, (nss_rect_t) {
                .x = rctx.cbuffer[k].x,
                .y = rctx.cbuffer[k].y,
                .width = rctx.cbuffer[i - 1].x - rctx.cbuffer[k].x + win->char_width,
                .height = win->char_depth + win->char_height
            }, rctx.cbuffer[j].bg);

        }
    }

    for (size_t i = 0; i < rctx.cbufpos; i++) {
        if (rctx.cbuffer[i].ch && rctx.cbuffer[i].fg != rctx.cbuffer[i].bg) {
            nss_glyph_t *glyph = nss_cache_glyph(win->ren.cache, rctx.cbuffer[i].face, rctx.cbuffer[i].ch);
            nss_rect_t clip = {rctx.cbuffer[i].x, rctx.cbuffer[i].y, win->char_width * (1 + rctx.cbuffer[i].wide), win->char_depth + win->char_height};
            nss_image_composite_glyph(win->ren.im, rctx.cbuffer[i].x, rctx.cbuffer[i].y + win->char_height, glyph, rctx.cbuffer[i].fg, clip);
            nss_cache_post(win->ren.cache, rctx.cbuffer[i].face, rctx.cbuffer[i].ch, glyph);
        }
    }

    //qsort(rctx.cbuffer, rctx.cbufpos, sizeof(rctx.cbuffer[0]), cmp_by_fg);
    //shell_sort_fg(rctx.cbuffer, rctx.cbufpos);
    merge_sort_fg(rctx.cbuffer, rctx.cbufpos);

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
            nss_image_draw_rect(win->ren.im, (nss_rect_t){
                .x = rctx.cbuffer[k].x,
                .y = rctx.cbuffer[k].y + win->char_height + 1,
                .width = rctx.cbuffer[i - 1].x + win->char_width - rctx.cbuffer[k].x,
                .height = win->underline_width
            }, rctx.cbuffer[j].fg);
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
            nss_image_draw_rect(win->ren.im, (nss_rect_t){
                .x = rctx.cbuffer[k].x,
                .y = rctx.cbuffer[k].y + 2*win->char_height/3 - win->underline_width/2,
                .width = rctx.cbuffer[i - 1].x + win->char_width - rctx.cbuffer[k].x,
                .height = win->underline_width
            }, rctx.cbuffer[j].fg);
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
        for (size_t i = 0; i < count; i++) {
            nss_image_draw_rect(win->ren.im, rects[i + off], win->cursor_fg);

        }
    }

    if (rctx.cbufpos || win->ren.shifted) {
        win->ren.shifted = 0;
        xcb_put_image(con, XCB_IMAGE_FORMAT_Z_PIXMAP, win->wid, win->ren.gc, win->ren.im->width, win->ren.im->height,
                win->left_border, win->top_border, 0, 32, win->ren.im->height * win->ren.im->width * sizeof(nss_color_t), (const uint8_t *)win->ren.im->data);
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
    xcb_put_image(con, XCB_IMAGE_FORMAT_Z_PIXMAP, win->wid, win->ren.gc,
            win->ren.im->width, rect.height, win->left_border,
            win->top_border + rect.y, 0, 32, rect.height * win->ren.im->width * sizeof(nss_color_t),
            (const uint8_t *)(win->ren.im->data+rect.y*win->ren.im->width));
}
void nss_renderer_background_changed(nss_window_t *win) {
    uint32_t values2[2];
    values2[0] = values2[1] = win->bg;
    xcb_change_window_attributes(con, win->wid, XCB_CW_BACK_PIXEL, values2);
    xcb_change_gc(con, win->ren.gc, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, values2);
}
void nss_renderer_copy(nss_window_t *win, nss_rect_t dst, int16_t sx, int16_t sy) {
    win->ren.shifted = 1;
    nss_image_copy(win->ren.im, dst, win->ren.im, sx, sy);
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

	nss_image_t *new = nss_create_image(width, height);
	nss_image_copy(new, (nss_rect_t){0, 0, common_w, common_h}, win->ren.im, 0, 0);
	SWAP(nss_image_t *, win->ren.im, new);
	nss_free_image(new);
		
    if (delta_y > 0)
        nss_image_draw_rect(win->ren.im, (nss_rect_t) {
    			0, win->ch - delta_y, MIN(win->cw, win->cw - delta_x), delta_y }, win->bg);
    if (delta_x > 0)
        nss_image_draw_rect(win->ren.im, (nss_rect_t) {
    			win->cw - delta_x, 0, delta_x, MAX(win->ch, win->ch - delta_y) }, win->bg);

}

