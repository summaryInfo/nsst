/* Copyright (c) 2019-2021, Evgeny Baskov. All rights reserved */

#define _XOPEN_SOURCE

#include "feature.h"

#include "config.h"
#include "font.h"
#include "mouse.h"
#include "window-x11.h"

#include <stdbool.h>
#include <string.h>
#include <wchar.h>
#include <xcb/render.h>
#include <xcb/xcb.h>

struct glyph_msg {
    uint8_t len;
    uint8_t pad[3];
    int16_t dx, dy;
    uint8_t data[];
};

struct element {
    /* Only one structure is used
     * for both text and rectangles
     * in order to use only one non-generic
     * sort function */
    int16_t x;
    int16_t y;
    color_t color;
    union {
        /* Width and height of rectangle to draw */
        struct {
            int16_t width;
            int16_t height;
        };
        /* Offset in glyphs buffer
         * (And the sequence of glyphs is 0-terminated) */
        uint32_t glyphs;
    };
};

struct element_buffer {
    struct element *data;
    size_t size;
    size_t caps;
};

struct render_context {
    xcb_render_pictformat_t pfargb;
    xcb_render_pictformat_t pfalpha;

    /* This buffer is used for:
     *    - XRender glyph drawing requests construction
     *    - XRender rectangle drawing requests construction
     *    - XRender clip rectangles */
    uint8_t *payload;
    size_t payload_caps;
    size_t payload_size;

    uint32_t *glyphs;
    size_t glyphs_size;
    size_t glyphs_caps;

    struct element_buffer foreground_buf;
    struct element_buffer background_buf;
    struct element_buffer decoration_buf;
};

static struct render_context rctx;

#define WORDS_IN_MESSAGE 256
#define HEADER_WORDS ((sizeof(struct glyph_msg)+sizeof(uint32_t))/sizeof(uint32_t))
#define CHARS_PER_MESG (WORDS_IN_MESSAGE - HEADER_WORDS)

#define CB(c) (((c) & 0xff) * 0x101)
#define CG(c) ((((c) >> 8) & 0xff) * 0x101)
#define CR(c) ((((c) >> 16) & 0xff) * 0x101)
#define CA(c) ((((c) >> 24) & 0xff) * 0x101)
#define MAKE_COLOR(c) {.red=CR(c), .green=CG(c), .blue=CB(c), .alpha=CA(c)}

static void register_glyph(struct window *win, uint32_t ch, struct glyph * glyph) {
    xcb_render_glyphinfo_t spec = {
        .width = glyph->width, .height = glyph->height,
        .x = glyph->x - win->cfg.font_spacing/2, .y = glyph->y - win->cfg.line_spacing/2,
        .x_off = win->char_width*(1 + (wcwidth(ch & 0xFFFFFF) > 1)), .y_off = glyph->y_off
    };

    xcb_void_cookie_t c;
    c = xcb_render_add_glyphs_checked(con, win->ren.gsid, 1, &ch, &spec, glyph->height*glyph->stride, glyph->data);
    if (check_void_cookie(c))
        warn("Can't add glyph");
}

bool renderer_reload_font(struct window *win, bool need_free) {
    struct window *found = find_shared_font(win, need_free);

    win->ren.pfglyph = win->cfg.pixel_mode ? rctx.pfargb : rctx.pfalpha;

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

        for (uint32_t i = ' '; i <= '~'; i++) {
            struct glyph *glyph = glyph_cache_fetch(win->font_cache, i, face_normal, NULL);
            glyph->x_off = win->char_width;
            register_glyph(win, i, glyph);
        }
    }

    if (need_free) {
        handle_resize(win, win->cfg.width, win->cfg.height);
        window_set_default_props(win);
    } else {
        win->cw = MAX(1, (win->cfg.width - 2*win->cfg.left_border) / win->char_width);
        win->ch = MAX(1, (win->cfg.height - 2*win->cfg.top_border) / (win->char_height + win->char_depth));

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

        xcb_render_color_t color = MAKE_COLOR(win->bg_premul);
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

void renderer_free(struct window *win) {
    xcb_render_free_picture(con, win->ren.pen);
    xcb_render_free_picture(con, win->ren.pic1);
    xcb_free_pixmap(con, win->ren.pid1);
    xcb_render_free_glyph_set(con, win->ren.gsid);
}

static void free_elem_buffer(struct element_buffer *buf) {
    free(buf->data);
    *buf = (struct element_buffer){ 0 };
}

void free_render_context(void) {
    free(rctx.payload);
    free(rctx.glyphs);
    free_elem_buffer(&rctx.foreground_buf);
    free_elem_buffer(&rctx.background_buf);
    free_elem_buffer(&rctx.decoration_buf);
}

#define INIT_GLYPHS_CAPS 128
#define INIT_FG_CAPS 128
#define INIT_BG_CAPS 256
#define INIT_DEC_CAPS 16

void init_render_context(void) {
    rctx.payload = malloc(WORDS_IN_MESSAGE * sizeof(uint32_t));
    if (!rctx.payload) {
        xcb_disconnect(con);
        die("Can't allocate buffer");
    }
    rctx.payload_caps = WORDS_IN_MESSAGE * sizeof(uint32_t);
    rctx.glyphs = malloc(INIT_GLYPHS_CAPS * sizeof(rctx.glyphs[0]));
    if (!rctx.glyphs) {
        xcb_disconnect(con);
        die("Can't allocate cbuffer");
    }
    rctx.glyphs_caps = INIT_GLYPHS_CAPS;

    adjust_buffer((void **)&rctx.foreground_buf.data, &rctx.foreground_buf.caps, INIT_FG_CAPS, sizeof(struct element));
    adjust_buffer((void **)&rctx.background_buf.data, &rctx.background_buf.caps, INIT_BG_CAPS, sizeof(struct element));
    adjust_buffer((void **)&rctx.decoration_buf.data, &rctx.decoration_buf.caps, INIT_DEC_CAPS, sizeof(struct element));

    // Check if XRender is present
    xcb_render_query_version_cookie_t vc = xcb_render_query_version(con, XCB_RENDER_MAJOR_VERSION, XCB_RENDER_MINOR_VERSION);
    xcb_generic_error_t *err = NULL;
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
    if (UNLIKELY(rctx.payload_size + sizeof(xcb_rectangle_t) >= rctx.payload_caps)) {
        size_t new_size = 3 * rctx.payload_caps / 2;
        uint8_t *new = realloc(rctx.payload, new_size);
        if (!new) return;
        rctx.payload_caps = new_size;
        rctx.payload = new;
    }

    memcpy(rctx.payload + rctx.payload_size, rect, sizeof(xcb_rectangle_t));
    rctx.payload_size += sizeof(xcb_rectangle_t);
}

inline static void push_element(struct element_buffer *dst, struct element *elem) {
    // We need buffer twice as big a the number of elements to
    // be able to use merge sort efficiently
    if (!adjust_buffer((void **)&dst->data, &dst->caps,
                       2*(dst->size + 1), sizeof *elem)) return;
    dst->data[dst->size++] = *elem;
}

inline static uint32_t push_char(uint32_t ch) {
    /* Character are pushed in buffer in reverse order,
     * since lines are scanned in reverse order */
    if (UNLIKELY(rctx.glyphs_size + 1 > rctx.glyphs_caps)) {
        size_t new_caps = 4*rctx.glyphs_caps/3;
        uint32_t *tmp = calloc(new_caps, sizeof(uint32_t));
        if (!tmp) return UINT32_MAX;
        memcpy(tmp + new_caps - rctx.glyphs_size, rctx.glyphs, rctx.glyphs_size*sizeof(uint32_t));
        free(rctx.glyphs);
        rctx.glyphs = tmp;
        rctx.glyphs_caps = new_caps;
    }

    size_t new_pos = ++rctx.glyphs_size;
    rctx.glyphs[rctx.glyphs_caps - new_pos] = ch;
    return new_pos;
}

inline static struct glyph_msg *start_msg(int16_t dx, int16_t dy) {
    struct glyph_msg *head = (struct glyph_msg *)(rctx.payload + rctx.payload_size);
    *head = (struct glyph_msg) {
        .dx = dx,
        .dy = dy,
    };
    rctx.payload_size += sizeof(*head);
    return head;
}

inline static bool adjust_msg_buffer(void) {
    if (UNLIKELY(rctx.payload_size + WORDS_IN_MESSAGE * sizeof(uint32_t) > rctx.payload_caps)) {
        uint8_t *new = realloc(rctx.payload, rctx.payload_caps + WORDS_IN_MESSAGE * sizeof(uint32_t));
        if (!new) return 0;
        rctx.payload = new;
        rctx.payload_caps  += WORDS_IN_MESSAGE * sizeof(uint32_t);
    }
    return 1;
}

inline static void sort_by_color(struct element_buffer *buf) {
    struct element *dst = buf->data + buf->size, *src = buf->data;
    for (size_t k = 2; k < buf->size; k += k) {
        for (size_t i = 0; i < buf->size; ) {
            size_t l_1 = i, h_1 = MIN(i + k/2, buf->size);
            size_t l_2 = h_1, h_2 = MIN(i + k, buf->size);
            while (l_1 < h_1 && l_2 < h_2)
                dst[i++] = src[src[l_1].color < src[l_2].color ? l_1++ : l_2++];
            while (l_1 < h_1) dst[i++] = src[l_1++];
            while (l_2 < h_2) dst[i++] = src[l_2++];
        }
        SWAP(dst, src);
    }
    if (dst < src) for (size_t i = 0; i < buf->size; i++)
        dst[i] = src[i];
}

static void draw_cursor(struct window *win, int16_t cur_x, int16_t cur_y, bool on_margin) {
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
        if (((win->cfg.cursor_shape + 1) & ~1) == cusor_type_bar) {
            if (on_margin) {
                off = 2;
                rects[2].width = win->cfg.cursor_width;
                rects[2].x -= win->cfg.cursor_width - 1;
            } else
                rects[0].width = win->cfg.cursor_width;
            count = 1;
        } else if (((win->cfg.cursor_shape + 1) & ~1) == cusor_type_underline) {
            count = 1;
            off = 3;
            rects[3].height = win->cfg.cursor_shape;
            rects[3].y -= win->cfg.cursor_shape - 1;
        } else {
            count = 0;
        }
    }
    if (count) {
        xcb_render_color_t c = MAKE_COLOR(win->cursor_fg);
        xcb_render_fill_rectangles(con, XCB_RENDER_PICT_OP_OVER, win->ren.pic1, c, count, rects + off);
    }
}

void prepare_multidraw(struct window *win, int16_t cur_x, ssize_t cur_y, bool reverse_cursor) {
    rctx.foreground_buf.size = 0;
    rctx.background_buf.size = 0;
    rctx.decoration_buf.size = 0;
    rctx.glyphs_size = 0;

    struct line_offset vpos = term_get_view(win->term);
    for (ssize_t k = 0; k < win->ch; k++, term_line_next(win->term, &vpos, 1)) {
        struct line_view line = term_line_at(win->term, vpos);
        bool next_dirty = 0, first_in_line = 1;
        if (win->cw > line.width) {
            color_t color = mouse_is_selected_in_view(win->term, win->cw - 1, k) ? (win->rcstate.palette[SPECIAL_SELECTED_BG] ?
                        win->rcstate.palette[SPECIAL_SELECTED_BG] : win->rcstate.palette[SPECIAL_FG]) : win->bg_premul;
            push_element(&rctx.background_buf, &(struct element) {
                .x = line.width * win->char_width,
                .y = k * (win->char_height + win->char_depth),
                .color = color,
                .width = (win->cw - line.width) * win->char_width,
                .height = win->char_height + win->char_depth,
            });
            next_dirty = 1;
            first_in_line = 0;
        }

        for (int16_t i = MIN(win->cw, line.width) - 1; i >= 0; i--) {
            struct cell cel = line.cell[i];
            struct attr attr = attr_at(line.line, i + line.cell - line.line->cell);
            bool dirty = line.line->force_damage || !cel.drawn ||
                    (!win->blink_commited && attr.blink);

            struct cellspec spec;
            struct glyph *glyph = NULL;
            bool g_wide = 0;
            uint32_t g = 0;
            if (dirty || next_dirty) {
                if (k == cur_y && i == cur_x && reverse_cursor) attr.reverse ^= 1;

                bool selected = mouse_is_selected_in_view(win->term, i, k);
                spec = describe_cell(cel, attr, &win->cfg, &win->rcstate, selected);
                g =  spec.ch | (spec.face << 24);

                bool is_new = 0;
                if (spec.ch) glyph = glyph_cache_fetch(win->font_cache, spec.ch, spec.face, &is_new);

                if (UNLIKELY(is_new) && glyph) register_glyph(win, g, glyph);

                g_wide = glyph && glyph->x_off > win->char_width - win->cfg.font_spacing;
            }
            if (dirty || (g_wide && next_dirty)) {
                int16_t y = k * (win->char_depth + win->char_height);

                // Queue background, groupping by color

                struct element *prev_bg = rctx.background_buf.data + rctx.background_buf.size - 1;
                if (LIKELY(!first_in_line && prev_bg->color == spec.bg &&
                        prev_bg->x == (i + 1)*win->char_width)) {
                    prev_bg->x -= win->char_width;
                    prev_bg->width += win->char_width;
                } else {
                    first_in_line = 0;
                    push_element(&rctx.background_buf, &(struct element) {
                        .x = i * win->char_width,
                        .y = y,
                        .color = spec.bg,
                        .width = win->char_width,
                        .height = win->char_height + win->char_depth,
                    });
                }

                // Push character if present, groupping by color

                if (spec.ch) {
                    g |=  (uint32_t)spec.wide << 31;
                    struct element *prev_fg = rctx.foreground_buf.data + rctx.foreground_buf.size - 1;
                    if (LIKELY(rctx.foreground_buf.size && prev_fg->y == y + win->char_height &&
                            prev_fg->color == spec.fg && prev_fg->x == (i + spec.wide + 1)*win->char_width)) {
                        prev_fg->glyphs = push_char(g);
                        prev_fg->x -= win->char_width*(1 + spec.wide);
                    } else {
                        push_char(0);
                        push_element(&rctx.foreground_buf, &(struct element) {
                            .x = i * win->char_width,
                            .y = y + win->char_height,
                            .color = spec.fg,
                            .glyphs = push_char(g),
                        });
                    }
                }


                // Push strikethrough/underline rects, if present

                if (UNLIKELY(spec.underlined)) {
                    int16_t line_y = y + win->char_height + 1 + win->cfg.line_spacing/2;
                    struct element *prev_dec = rctx.decoration_buf.data + rctx.decoration_buf.size - 1;
                    if (rctx.decoration_buf.size && prev_dec->y == line_y  && prev_dec->color == spec.bg &&
                            prev_dec->x == (i + 1)*win->char_width) {
                        prev_dec->x -= win->char_width;
                        prev_dec->width += win->char_width;
                    } else if (rctx.decoration_buf.size > 1 && prev_dec[-1].y == line_y  && prev_dec[-1].color == spec.bg &&
                            prev_dec[-1].x == (i + 1)*win->char_width) {
                        prev_dec[-1].x -= win->char_width;
                        prev_dec[-1].width += win->char_width;
                    } else {
                        push_element(&rctx.decoration_buf, &(struct element) {
                            .x = i * win->char_width,
                            .y = line_y,
                            .color = spec.fg,
                            .width = win->char_width,
                            .height = win->cfg.underline_width,
                        });
                    }
                }

                if (UNLIKELY(spec.stroke)) {
                    int16_t line_y = y + 2*win->char_height/3 - win->cfg.underline_width/2 + win->cfg.line_spacing/2;
                    struct element *prev_dec = rctx.decoration_buf.data + rctx.decoration_buf.size - 1;
                    if (rctx.decoration_buf.size && prev_dec->y == line_y  && prev_dec->color == spec.bg &&
                            prev_dec->x == (i + 1)*win->char_width) {
                        prev_dec->x -= win->char_width;
                        prev_dec->width += win->char_width;
                    } else if (rctx.decoration_buf.size > 1 && prev_dec[-1].y == line_y  && prev_dec[-1].color == spec.bg &&
                            prev_dec[-1].x == (i + 1)*win->char_width) {
                        prev_dec[-1].x -= win->char_width;
                        prev_dec[-1].width += win->char_width;
                    } else {
                        push_element(&rctx.decoration_buf, &(struct element) {
                            .x = i * win->char_width,
                            .y = line_y,
                            .color = spec.fg,
                            .width = win->char_width,
                            .height = win->cfg.underline_width,
                        });
                    }
                }

                line.cell[i].drawn = 1;
            }
            next_dirty = dirty;
        }
        // Only reset force flag for last part of the line
        if (is_last_line(line, win->cfg.rewrap)) line.line->force_damage = 0;
    }
}

static void draw_rects(struct window *win, struct element_buffer *buf) {
    for (struct element *it = buf->data, *end = buf->data + buf->size; it < end; ) {
        color_t color = it ->color;

        rctx.payload_size = 0;
        do {
            push_rect(&(xcb_rectangle_t) {
                .x = it->x,
                .y = it->y,
                .width = it->width,
                .height = it->height,
            });
        } while (++it < end && it->color == color);

        if (rctx.payload_size) {
            xcb_render_color_t col = MAKE_COLOR(color);
            xcb_render_fill_rectangles(con, XCB_RENDER_PICT_OP_SRC, win->ren.pic1, col,
                rctx.payload_size/sizeof(xcb_rectangle_t), (xcb_rectangle_t *)rctx.payload);
        }
    }

}

bool window_submit_screen(struct window *win, int16_t cur_x, ssize_t cur_y, bool cursor, bool marg) {
    bool reverse_cursor = cursor && win->focused && ((win->cfg.cursor_shape + 1) & ~1) == cusor_type_block;
    bool cond_cblink = !win->blink_commited && (win->cfg.cursor_shape & 1) && term_is_cursor_enabled(win->term);
    if (cond_cblink) cursor |= win->rcstate.blink;

    prepare_multidraw(win, cur_x, cur_y, reverse_cursor);

    // Set clip rectangles for text rendering
    rctx.payload_size = 0;
    for (struct element *it = rctx.background_buf.data, *end = rctx.background_buf.data + rctx.background_buf.size; it < end; ) {
        struct element *it2 = it;
        do it2++;
        while (it2 < end && it2->y == it->y &&
                it2->x + it2->width == it2[-1].x);
        push_rect(&(xcb_rectangle_t) {
            .x = it2[-1].x,
            .y = it->y,
            .width = it->x - it2[-1].x + it->width,
            .height = win->char_depth + win->char_height
        });
        it = it2;
    }
    if (rctx.payload_size)
        xcb_render_set_picture_clip_rectangles(con, win->ren.pic1, 0, 0,
            rctx.payload_size/sizeof(xcb_rectangle_t), (xcb_rectangle_t *)rctx.payload);

    sort_by_color(&rctx.foreground_buf);
    sort_by_color(&rctx.background_buf);
    sort_by_color(&rctx.decoration_buf);

    // Draw background
    draw_rects(win, &rctx.background_buf);

    // Draw characters
    for (struct element *it = rctx.foreground_buf.data, *end = rctx.foreground_buf.data + rctx.foreground_buf.size; it < end; ) {
        // Prepare pen
        color_t color = it->color;
        xcb_render_color_t col = MAKE_COLOR(color);
        xcb_rectangle_t rect2 = { .x = 0, .y = 0, .width = 1, .height = 1 };
        xcb_render_fill_rectangles(con, XCB_RENDER_PICT_OP_SRC, win->ren.pen, col, 1, &rect2);

        // Build payload...

        rctx.payload_size = 0;

        int16_t old_x = 0, old_y = 0, x = it->x;
        uint32_t *glyph = rctx.glyphs + rctx.glyphs_caps - it->glyphs;
        for (;;) {
            if (!adjust_msg_buffer()) break;

            struct glyph_msg *head = start_msg(x - old_x, it->y - old_y);
            old_x = x, old_y = it->y;

            do {
                uint32_t g = *glyph & ~(1U << 31);
                memcpy(rctx.payload + rctx.payload_size, &g, sizeof *glyph);
                rctx.payload_size += sizeof(uint32_t);
                head->len++;

                bool wide = *glyph & (1U << 31);
                x += (1 + wide) * win->char_width;
                old_x += (1 + wide) * win->char_width;
            } while (*++glyph && head->len < CHARS_PER_MESG);

            if (!*glyph) {
                if (++it >= end || color != it->color) break;
                glyph = rctx.glyphs + rctx.glyphs_caps - it->glyphs;
                x = it->x;
            }
        }

        // ..and send it

        if (rctx.payload_size) {
            xcb_render_composite_glyphs_32(con, XCB_RENDER_PICT_OP_OVER,
                    win->ren.pen, win->ren.pic1, win->ren.pfglyph, win->ren.gsid,
                    0, 0, rctx.payload_size, rctx.payload);
        }
    }

    if (rctx.foreground_buf.size)
        xcb_render_set_picture_clip_rectangles(con, win->ren.pic1, 0, 0, 1, &(xcb_rectangle_t) {
                0, 0, win->cw * win->char_width, win->ch * (win->char_height + win->char_depth)});

    // Draw underline and strikethrough lines
    draw_rects(win, &rctx.decoration_buf);

    if (cursor) draw_cursor(win, cur_x, cur_y, marg);

    if (rctx.background_buf.size)
        renderer_update(win, rect_scale_up((struct rect){0, 0, win->cw, win->ch},
                win->char_width, win->char_height + win->char_depth));

    return !!rctx.background_buf.size;
}

void renderer_update(struct window *win, struct rect rect) {
    xcb_copy_area(con, win->ren.pid1, win->wid, win->gc, rect.x, rect.y,
            rect.x + win->cfg.left_border, rect.y + win->cfg.top_border, rect.width, rect.height);
}

void renderer_copy(struct window *win, struct rect dst, int16_t sx, int16_t sy) {
    xcb_copy_area(con, win->ren.pid1, win->ren.pid1, win->gc, sx, sy, dst.x, dst.y, dst.width, dst.height);
    /*
    xcb_render_composite(con, XCB_RENDER_PICT_OP_SRC, win->ren.pic, 0, win->ren.pic, sx, sy, 0, 0, dst.x, dst.y, dst.width, dst.height);
    */
}

void renderer_resize(struct window *win, int16_t new_cw, int16_t new_ch) {
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

    SWAP(win->ren.pid1, win->ren.pid2);
    SWAP(win->ren.pic1, win->ren.pic2);

    xcb_render_free_picture(con, win->ren.pic2);
    xcb_free_pixmap(con, win->ren.pid2);

    static_assert(sizeof(struct rect) == sizeof(xcb_rectangle_t), "Rectangle types does not match");

    struct rect rectv[2];
    size_t rectc= 0;

    if (delta_y > 0)
        rectv[rectc++] = (struct rect) { 0, win->ch - delta_y, MIN(win->cw, win->cw - delta_x), delta_y };
    if (delta_x > 0)
        rectv[rectc++] = (struct rect) { win->cw - delta_x, 0, delta_x, MAX(win->ch, win->ch - delta_y) };

    for (size_t i = 0; i < rectc; i++)
        rectv[i] = rect_scale_up(rectv[i], win->char_width, win->char_height + win->char_depth);

    xcb_render_color_t color = MAKE_COLOR(win->bg_premul);
    xcb_render_fill_rectangles(con, XCB_RENDER_PICT_OP_SRC, win->ren.pic1, color, rectc, (xcb_rectangle_t*)rectv);
}
