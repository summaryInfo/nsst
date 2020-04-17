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

nss_render_context_t rctx;

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
    _Bool found_font = 0, found_gset = 0;
    nss_window_t *found = 0;
    for (nss_window_t *src = win_list_head; src; src = src->next) {
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
        nss_create_font(win->font_name, win->font_size, nss_config_integer(NSS_ICONFIG_DPI));
    if (!new) {
        warn("Can't create new font: %s", win->font_name);
        return 0;
    }

    if (need_free) nss_free_font(win->font);

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

    if (found_gset) {
        c = xcb_render_reference_glyph_set_checked(con, win->ren.gsid, found->ren.gsid);
        if (check_void_cookie(c))
            warn("Can't reference glyph set");

        win->char_height = found->char_height;
        win->char_depth = found->char_depth;
        win->char_width = found->char_width;
    } else {
        c = xcb_render_create_glyph_set_checked(con, win->ren.gsid, win->ren.pfglyph);
        if (check_void_cookie(c))
            warn("Can't create glyph set");

        //Preload ASCII
        nss_glyph_t *glyphs['~' - ' ' + 1][nss_font_attrib_max] = {{ NULL }};
        int16_t total = 0, maxd = 0, maxh = 0;
        for (tchar_t i = ' '; i <= '~'; i++) {
            for (size_t j = 0; j < nss_font_attrib_max; j++)
                glyphs[i - ' '][j] = nss_font_render_glyph(win->font, i, j, win->subpixel_fonts);

            total += glyphs[i - ' '][0]->x_off;
            maxd = MAX(maxd, glyphs[i - ' '][0]->height - glyphs[i - ' '][0]->y);
            maxh = MAX(maxh, glyphs[i - ' '][0]->y);
        }

        win->char_width = total / ('~' - ' ' + 1) + nss_config_integer(NSS_ICONFIG_FONT_SPACING);
        win->char_height = maxh;
        win->char_depth = maxd + nss_config_integer(NSS_ICONFIG_LINE_SPACING);

        for (tchar_t i = ' '; i <= '~'; i++) {
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
        xcb_free_pixmap(con, win->ren.pid);
        xcb_free_gc(con, win->ren.gc);
        xcb_render_free_picture(con, win->ren.pic);
    } else {
        win->ren.pid = xcb_generate_id(con);
        win->ren.gc = xcb_generate_id(con);
        win->ren.pic = xcb_generate_id(con);
    }

    c = xcb_create_pixmap_checked(con, TRUE_COLOR_ALPHA_DEPTH, win->ren.pid, win->wid, bound.width, bound.height );
    if (check_void_cookie(c)) {
        warn("Can't create pixmap");
        return 0;
    }

    uint32_t mask2 = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_GRAPHICS_EXPOSURES;
    uint32_t values2[3] = { win->bg, win->bg, 0 };
    c = xcb_create_gc_checked(con, win->ren.gc, win->ren.pid, mask2, values2);
    if (check_void_cookie(c)) {
        warn("Can't create GC");
        return 0;
    }

    uint32_t mask3 = XCB_RENDER_CP_GRAPHICS_EXPOSURE | XCB_RENDER_CP_POLY_EDGE | XCB_RENDER_CP_POLY_MODE;
    uint32_t values3[3] = { 0, XCB_RENDER_POLY_EDGE_SMOOTH, XCB_RENDER_POLY_MODE_IMPRECISE };
    c = xcb_render_create_picture_checked(con, win->ren.pic, win->ren.pid, rctx.pfargb, mask3, values3);
    if (check_void_cookie(c)) {
        warn("Can't create XRender picture");
        return 0;
    }

    xcb_render_color_t color = MAKE_COLOR(win->bg);
    xcb_render_fill_rectangles(con, XCB_RENDER_PICT_OP_SRC, win->ren.pic, color, 1, &bound);

    if (need_free)
        nss_term_resize(win->term, win->cw, win->ch);

    if (!need_free) {
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
    xcb_free_gc(con, win->ren.gc);
    xcb_render_free_picture(con, win->ren.pen);
    xcb_render_free_picture(con, win->ren.pic);
    xcb_free_pixmap(con, win->ren.pid);
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

static void push_cell(nss_window_t *win, coord_t x, coord_t y, nss_color_t *palette, nss_color_t *extra, nss_cell_t *cel) {
    nss_cell_t cell = *cel;

    if (!nss_font_glyph_is_loaded(win->font, cell.ch)) {
        for (size_t j = 0; j < nss_font_attrib_max; j++) {
            nss_glyph_t *glyph;
#ifdef USE_BOXDRAWING
            if (is_boxdraw(cell.ch) && nss_config_integer(NSS_ICONFIG_OVERRIDE_BOXDRAW)) {
                glyph = nss_make_boxdraw(cell.ch, win->char_width, win->char_height, win->char_depth, win->subpixel_fonts);
                nss_font_glyph_mark_loaded(win->font, cell.ch | (j << 24));
            } else
#endif
                glyph = nss_font_render_glyph(win->font, cell.ch, j, win->subpixel_fonts);
            //In case of non-monospace fonts
            glyph->x_off = win->char_width;
            register_glyph(win, cell.ch | (j << 24) , glyph);
            free(glyph);
        }
    }

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
        .glyph = cell.ch ? cell.ch | ((cell.attr & nss_font_attrib_mask) << 24) : 0,
        .wide = !!(cell.attr & nss_attrib_wide),
        .underlined = !!(cell.attr & nss_attrib_underlined) && (fg != bg),
        .strikethrough = !!(cell.attr & nss_attrib_strikethrough) && (fg != bg),
    };

    cel->attr |= nss_attrib_drawn;
}

static void push_rect(nss_window_t *win, xcb_rectangle_t *rect) {
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

/*
static inline void shell_sort_bg(struct cell_desc *array, size_t size) {
    size_t hmax = size/9;
    size_t h;
    for(h = 1; h <= hmax; h = 3*h+1);
    for(; h > 0; h /= 3) {
        for(size_t i = h; i < size; ++i) {
            const struct cell_desc v = array[i];
            size_t j = i;
            while(j >= h && cmp_bg(&v, &array[j-h])) {
                array[j] = array[j-h];
                j -= h;
            }
            array[j] = v;
        }
    }
}

static inline void shell_sort_fg(struct cell_desc *array, size_t size) {
    size_t hmax = size/9;
    size_t h;
    for(h = 1; h <= hmax; h = 3*h+1);
    for(; h > 0; h /= 3) {
        for(size_t i = h; i < size; ++i) {
            const struct cell_desc v = array[i];
            size_t j = i;
            while(j >= h && cmp_fg(&v, &array[j-h])) {
                array[j] = array[j-h];
                j -= h;
            }
            array[j] = v;
        }
    }
}
*/

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
            push_rect(win, &(xcb_rectangle_t){
                .x = list->width * win->char_width,
                .y = h * (win->char_height + win->char_depth),
                .width = (win->cw - list->width) * win->char_width,
                .height = win->char_height + win->char_depth
            });
        }
        for (coord_t i = 0; i < MIN(win->cw, list->width); i++)
            if (!(list->cell[i].attr & nss_attrib_drawn) ||
                    (!win->blink_commited && (list->cell[i].attr & nss_attrib_blink)))
                push_cell(win, i, h, palette, list->extra, &list->cell[i]);
    }
    for (coord_t j = 0; j < win->ch - h; j++) {
        if (win->cw > array[j]->width) {
            push_rect(win, &(xcb_rectangle_t){
                .x = array[j]->width * win->char_width,
                .y = (j + h) * (win->char_height + win->char_depth),
                .width = (win->cw - array[j]->width) * win->char_width,
                .height = win->char_height + win->char_depth
            });
        }
        for (coord_t i = 0; i < MIN(win->cw, array[j]->width); i++)
            if (!(array[j]->cell[i].attr & nss_attrib_drawn) ||
                    (!win->blink_commited && (array[j]->cell[i].attr & nss_attrib_blink)))
                push_cell(win, i, j + h, palette, array[j]->extra, &array[j]->cell[i]);
    }

    if (rctx.bufpos) {
        xcb_render_color_t col = MAKE_COLOR(win->bg);
        xcb_render_fill_rectangles(con, XCB_RENDER_PICT_OP_SRC, win->ren.pic, col,
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
            push_rect(win, &(xcb_rectangle_t) {
                .x = rctx.cbuffer[k].x,
                .y = rctx.cbuffer[k].y,
                .width = rctx.cbuffer[i - 1].x - rctx.cbuffer[k].x + win->char_width,
                .height = win->char_depth + win->char_height
            });

        }
        if (rctx.bufpos) {
            xcb_render_color_t col = MAKE_COLOR(rctx.cbuffer[j].bg);
            xcb_render_fill_rectangles(con, XCB_RENDER_PICT_OP_SRC, win->ren.pic, col,
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
        push_rect(win, &(xcb_rectangle_t) {
            .x = rctx.cbuffer[k].x,
            .y = rctx.cbuffer[k].y,
            .width = rctx.cbuffer[i - 1].x - rctx.cbuffer[k].x + win->char_width*(1 + rctx.cbuffer[k].wide),
            .height = win->char_depth + win->char_height
        });
    }
    if (rctx.bufpos)
        xcb_render_set_picture_clip_rectangles(con, win->ren.pic, 0, 0,
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
                                           win->ren.pen, win->ren.pic, win->ren.pfglyph, win->ren.gsid,
                                           0, 0, rctx.bufpos, rctx.buffer);
    }

    if (rctx.cbufpos)
        xcb_render_set_picture_clip_rectangles(con, win->ren.pic, 0, 0, 1, &(xcb_rectangle_t){
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
            push_rect(win, &(xcb_rectangle_t) {
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
            push_rect(win, &(xcb_rectangle_t) {
                .x = rctx.cbuffer[k].x,
                .y = rctx.cbuffer[k].y + 2*win->char_height/3 - win->underline_width/2,
                .width = rctx.cbuffer[i - 1].x + win->char_width - rctx.cbuffer[k].x,
                .height = win->underline_width
            });
        }
        if (rctx.bufpos) {
            xcb_render_color_t col = MAKE_COLOR(rctx.cbuffer[j].fg);
            xcb_render_fill_rectangles(con, XCB_RENDER_PICT_OP_SRC, win->ren.pic, col,
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
            xcb_render_fill_rectangles(con, XCB_RENDER_PICT_OP_OVER, win->ren.pic, c, count, rects + off);
        }
    }

    if (rctx.cbufpos)
        nss_renderer_update(win, rect_scale_up((nss_rect_t){0, 0, win->cw, win->ch},
                win->char_width, win->char_height + win->char_depth));
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
    xcb_copy_area(con, win->ren.pid, win->wid, win->ren.gc, rect.x, rect.y,
                  rect.x + win->left_border, rect.y + win->top_border, rect.width, rect.height);
}
void nss_renderer_background_changed(nss_window_t *win) {
    uint32_t values2[2];
    values2[0] = values2[1] = win->bg;
    xcb_change_window_attributes(con, win->wid, XCB_CW_BACK_PIXEL, values2);
    xcb_change_gc(con, win->ren.gc, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, values2);
}
void nss_renderer_copy(nss_window_t *win, nss_rect_t dst, int16_t sx, int16_t sy) {
    xcb_copy_area(con, win->ren.pid, win->ren.pid, win->ren.gc, sx, sy, dst.x, dst.y, dst.width, dst.height);
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

    xcb_pixmap_t pid = xcb_generate_id(con);
    xcb_create_pixmap(con, TRUE_COLOR_ALPHA_DEPTH, pid, win->wid, width, height);
    xcb_render_picture_t pic = xcb_generate_id(con);
    uint32_t mask3 = XCB_RENDER_CP_GRAPHICS_EXPOSURE | XCB_RENDER_CP_POLY_EDGE | XCB_RENDER_CP_POLY_MODE;
    uint32_t values3[3] = { 0, XCB_RENDER_POLY_EDGE_SMOOTH, XCB_RENDER_POLY_MODE_IMPRECISE };
    xcb_render_create_picture(con, pic, pid, rctx.pfargb, mask3, values3);

    xcb_render_composite(con, XCB_RENDER_PICT_OP_SRC, win->ren.pic, 0, pic, 0, 0, 0, 0, 0, 0, common_w, common_h);

    SWAP(xcb_pixmap_t, win->ren.pid, pid);
    SWAP(xcb_render_picture_t, win->ren.pic, pic);
    xcb_free_pixmap(con, pid);
    xcb_render_free_picture(con, pic);

    nss_rect_t rectv[2];
    size_t rectc= 0;

    if (delta_y > 0)
        rectv[rectc++] = (nss_rect_t) { 0, win->ch - delta_y, MIN(win->cw, win->cw - delta_x), delta_y };
    if (delta_x > 0)
        rectv[rectc++] = (nss_rect_t) { win->cw - delta_x, 0, delta_x, MAX(win->ch, win->ch - delta_y) };

    for (size_t i = 0; i < rectc; i++)
        rectv[i] = rect_scale_up(rectv[i], win->char_width, win->char_height + win->char_depth);

    xcb_render_color_t color = MAKE_COLOR(win->bg);
    xcb_render_fill_rectangles(con, XCB_RENDER_PICT_OP_SRC, win->ren.pic, color, rectc, (xcb_rectangle_t*)rectv);
}

