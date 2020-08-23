/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#include "feature.h"

#include "config.h"
#include "font.h"
#include "mouse.h"
#include "window-x11.h"

#include <stdbool.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/render.h>

typedef struct nss_render_context nss_render_context_t;

struct nss_render_context {
    xcb_render_pictformat_t pfargb;
    xcb_render_pictformat_t pfalpha;

    struct cell_desc {
        int16_t x;
        int16_t y;
        color_t bg;
        color_t fg;
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

static void register_glyph(struct window *win, uint32_t ch, nss_glyph_t * glyph) {
    xcb_render_glyphinfo_t spec = {
        .width = glyph->width, .height = glyph->height,
        .x = glyph->x - iconf(ICONF_FONT_SPACING)/2, .y = glyph->y - iconf(ICONF_LINE_SPACING)/2,
        .x_off = win->char_width, .y_off = glyph->y_off
    };
    xcb_void_cookie_t c;
    c = xcb_render_add_glyphs_checked(con, win->ren.gsid, 1, &ch, &spec, glyph->height*glyph->stride, glyph->data);
    if (check_void_cookie(c))
        warn("Can't add glyph");
}

_Bool nss_renderer_reload_font(struct window *win, _Bool need_free) {
    struct window *found = nss_find_shared_font(win, need_free);

    win->ren.pfglyph = iconf(ICONF_PIXEL_MODE) ? rctx.pfargb : rctx.pfalpha;

    xcb_void_cookie_t c;

    if (need_free) {
        c = xcb_render_free_glyph_set_checked(con, win->ren.gsid);
        if (check_void_cookie(c)) warn("Can't free glyph set");
    }
    else win->ren.gsid = xcb_generate_id(con);

    if (found && win->font_pixmode == found->font_pixmode) {
        c = xcb_render_reference_glyph_set_checked(con, win->ren.gsid, found->ren.gsid);
        if (check_void_cookie(c)) warn("Can't reference glyph set");
    } else {
        c = xcb_render_create_glyph_set_checked(con, win->ren.gsid, win->ren.pfglyph);
        if (check_void_cookie(c)) warn("Can't create glyph set");

        for (term_char_t i = ' '; i <= '~'; i++) {
            nss_glyph_t *glyph = nss_cache_fetch(win->font_cache, i, nss_font_attrib_normal);
            glyph->x_off = win->char_width;
            register_glyph(win, i, glyph);
        }
    }

    if (need_free) {
        nss_window_handle_resize(win, win->width, win->height);
        window_set_default_props(win);
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
            free_window(win);
            return NULL;
        }

        win->ren.pen = xcb_generate_id(con);
        uint32_t values4[1] = { XCB_RENDER_REPEAT_NORMAL };
        c = xcb_render_create_picture_checked(con, win->ren.pen, pid, rctx.pfargb, XCB_RENDER_CP_REPEAT, values4);
        if (check_void_cookie(c)) {
            warn("Can't create picture");
            free_window(win);
            return NULL;
        }
        xcb_free_pixmap(con, pid);
    }

    return 1;
}

void nss_renderer_free(struct window *win) {
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

_Bool window_submit_screen(struct window *win, color_t *palette, nss_coord_t cur_x, nss_coord_t cur_y, _Bool cursor, _Bool marg) {

    rctx.cbufpos = 0;
    rctx.bufpos = 0;

    _Bool cond_cblink = !win->blink_commited && (win->cursor_type & 1) && nss_term_is_cursor_enabled(win->term);

    if (cond_cblink) cursor |= win->blink_state;

    nss_line_pos_t vpos = nss_term_get_view(win->term);
    for (ssize_t k = 0; k < win->ch; k++, nss_term_inc_line_pos(win->term, &vpos, 1)) {
        nss_line_view_t line = nss_term_line_at(win->term, vpos);
        _Bool next_dirty = 0;
        if (win->cw > line.width) {
            push_rect(&(xcb_rectangle_t){
                .x = line.width * win->char_width,
                .y = k * (win->char_height + win->char_depth),
                .width = (win->cw - line.width) * win->char_width,
                .height = win->char_height + win->char_depth
            });
            next_dirty = 1;
        }
        for (nss_coord_t i = MIN(win->cw, line.width) - 1; i >= 0; i--) {
            _Bool dirty = line.line->force_damage || !(line.cell[i].attr & nss_attrib_drawn) ||
                    (!win->blink_commited && (line.cell[i].attr & nss_attrib_blink)) ||
                    (cond_cblink && k == cur_y && i == cur_x);

            struct cellspec spec;
            nss_cell_t cel;
            nss_glyph_t *glyph = NULL;
            _Bool g_wide = 0;
            term_char_t g = 0;
            if (dirty || next_dirty) {
                cel = line.cell[i];

                if (k == cur_y && i == cur_x && cursor &&
                        win->focused && ((win->cursor_type + 1) & ~1) == cusor_type_block)
                    cel.attr ^= nss_attrib_inverse;

                spec = nss_describe_cell(cel, palette, line.line->pal->data,
                        win->blink_state, mouse_is_selected_in_view(win->term, i, k));
                g =  spec.ch | (spec.face << 24);

                _Bool fetched = nss_cache_is_fetched(win->font_cache, g);
                if (spec.ch) glyph = nss_cache_fetch(win->font_cache, spec.ch, spec.face);

                if (!fetched && glyph) register_glyph(win, g, glyph);

                g_wide = glyph && glyph->x_off > win->char_width - iconf(ICONF_FONT_SPACING);
            }
            if (dirty || (g_wide && next_dirty)) {
                if (2*(rctx.cbufpos + 1) >= rctx.cbufsize) {
                    size_t new_size = MAX(3 * rctx.cbufsize / 2, 2 * rctx.cbufpos + 1);
                    struct cell_desc *new = realloc(rctx.cbuffer, new_size * sizeof(*rctx.cbuffer));
                    if (!new) return 0;
                    rctx.cbuffer = new;
                    rctx.cbufsize = new_size;
                }

                rctx.cbuffer[rctx.cbufpos++] = (struct cell_desc) {
                    .x = i * win->char_width,
                    .y = k * (win->char_height + win->char_depth),
                    .fg = spec.fg, .bg = spec.bg,
                    .glyph = g,
                    .wide = spec.wide || g_wide,
                    .underlined = spec.underlined,
                    .strikethrough = spec.stroke
                };

                line.cell[i].attr |= nss_attrib_drawn;
            }
            next_dirty = dirty;
        }
        // Only reset force flag for last part of the line
        if (!nss_term_is_continuation_line(line)) line.line->force_damage = 0;
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
        while (i < rctx.cbufpos && rctx.cbuffer[i].bg == rctx.cbuffer[j].bg) {
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

        while (i < rctx.cbufpos && rctx.cbuffer[i].fg == rctx.cbuffer[j].fg) {
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
        while (i < rctx.cbufpos && !rctx.cbuffer[i].underlined && !rctx.cbuffer[i].strikethrough) i++;
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
                .y = rctx.cbuffer[k].y + win->char_height + 1 + iconf(ICONF_LINE_SPACING)/2,
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
                .y = rctx.cbuffer[k].y + 2*win->char_height/3 - win->underline_width/2 + iconf(ICONF_LINE_SPACING)/2,
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
            if (((win->cursor_type + 1) & ~1) == cusor_type_bar) {
                if (marg) {
                    off = 2;
                    rects[2].width = win->cursor_width;
                    rects[2].x -= win->cursor_width - 1;
                } else
                    rects[0].width = win->cursor_width;
                count = 1;
            } else if (((win->cursor_type + 1) & ~1) == cusor_type_underline) {
                count = 1;
                off = 3;
                rects[3].height = win->cursor_width;
                rects[3].y -= win->cursor_width - 1;
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
        nss_renderer_update(win, rect_scale_up((struct rect){0, 0, win->cw, win->ch},
                win->char_width, win->char_height + win->char_depth));

    return rctx.cbufpos;
}

void nss_renderer_update(struct window *win, struct rect rect) {
    xcb_copy_area(con, win->ren.pid1, win->wid, win->gc, rect.x, rect.y,
            rect.x + win->left_border, rect.y + win->top_border, rect.width, rect.height);
}

void nss_renderer_copy(struct window *win, struct rect dst, int16_t sx, int16_t sy) {
    xcb_copy_area(con, win->ren.pid1, win->ren.pid1, win->gc, sx, sy, dst.x, dst.y, dst.width, dst.height);
    /*
    xcb_render_composite(con, XCB_RENDER_PICT_OP_SRC, win->ren.pic, 0, win->ren.pic, sx, sy, 0, 0, dst.x, dst.y, dst.width, dst.height);
    */
}

void nss_renderer_resize(struct window *win, int16_t new_cw, int16_t new_ch) {
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

    struct rect rectv[2];
    size_t rectc= 0;

    if (delta_y > 0)
        rectv[rectc++] = (struct rect) { 0, win->ch - delta_y, MIN(win->cw, win->cw - delta_x), delta_y };
    if (delta_x > 0)
        rectv[rectc++] = (struct rect) { win->cw - delta_x, 0, delta_x, MAX(win->ch, win->ch - delta_y) };

    for (size_t i = 0; i < rectc; i++)
        rectv[i] = rect_scale_up(rectv[i], win->char_width, win->char_height + win->char_depth);

    xcb_render_color_t color = MAKE_COLOR(win->bg);
    xcb_render_fill_rectangles(con, XCB_RENDER_PICT_OP_SRC, win->ren.pic1, color, rectc, (xcb_rectangle_t*)rectv);
}
