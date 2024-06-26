/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

#include "feature.h"

#include "config.h"
#include "font.h"
#include "mouse.h"
#include "window-impl.h"
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

#define FREE_IDS_INIT_CAPS 32

struct element {
    /* Only one structure is used
     * for both text and rectangles
     * in order to use only one non-generic
     * sort function */
    int16_t x;
    int16_t y;
    union {
        color_t color;
        uint32_t picture;
    };
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
    struct element *sorted;
    size_t size;
    size_t caps;
};

struct render_context {
    /* X11-specific fields */
    struct {
        xcb_render_pictformat_t pfargb;
        xcb_render_pictformat_t pfalpha;
    };

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
    struct element_buffer decoration_buf2;
    struct element_buffer image_buf;

    /* To avoid leaking IDs of XRender pictures we cache
     * free IDs in this array and reuse them as necessary */
    uint32_t *free_ids;
    size_t id_capacity;
    size_t id_count;
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

#define INIT_GLYPHS_CAPS 128
#define INIT_FG_CAPS 128
#define INIT_BG_CAPS 256
#define INIT_DEC_CAPS 16
#define INIT_PAYLOAD_CAPS (WORDS_IN_MESSAGE * sizeof(uint32_t))

static uint32_t alloc_cached_id(void) {
    if (rctx.id_count == 0)
        return xcb_generate_id(con);
    return rctx.free_ids[--rctx.id_count];
}

static void free_cached_id(uint32_t id) {
    adjust_buffer((void **)&rctx.free_ids, &rctx.id_capacity, rctx.id_count + 1, sizeof *rctx.free_ids);
    rctx.free_ids[rctx.id_count++] = id;
}

static void register_glyph(struct window *win, uint32_t ch, struct glyph *glyph) {
    if (glyph->pixmode == pixmode_bgra) {
        glyph->id = alloc_cached_id();

        if (get_plat(win)->glyph_pid == 0)
            get_plat(win)->glyph_pid = xcb_generate_id(con);

        xcb_create_pixmap(con, TRUE_COLOR_ALPHA_DEPTH, get_plat(win)->glyph_pid, get_plat(win)->wid, glyph->width, glyph->height);
        xcb_put_image(con, XCB_IMAGE_FORMAT_Z_PIXMAP, get_plat(win)->glyph_pid, get_plat(win)->gc,
                glyph->stride/sizeof(color_t), glyph->height, 0, 0, 0, TRUE_COLOR_ALPHA_DEPTH,
                glyph->height * glyph->stride, (const uint8_t *)glyph->data);
        uint32_t mask3 = XCB_RENDER_CP_GRAPHICS_EXPOSURE | XCB_RENDER_CP_POLY_EDGE | XCB_RENDER_CP_POLY_MODE;
        uint32_t values3[3] = { 0, XCB_RENDER_POLY_EDGE_SMOOTH, XCB_RENDER_POLY_MODE_IMPRECISE };
        xcb_render_create_picture(con, glyph->id, get_plat(win)->glyph_pid, rctx.pfargb, mask3, values3);
        xcb_free_pixmap(con, get_plat(win)->glyph_pid);

    } else {
        xcb_render_glyphinfo_t spec = {
            .width = glyph->width, .height = glyph->height,
            .x = glyph->x - win->cfg.font_spacing/2, .y = glyph->y - win->cfg.line_spacing/2,
            .x_off = win->char_width*(1 + (uwidth(ch & 0xFFFFFF) > 1)), .y_off = glyph->y_off
        };

        xcb_render_add_glyphs(con, get_plat(win)->gsid, 1, &ch, &spec,
                              glyph->height*glyph->stride, glyph->data);
    }
}

static inline void do_draw_rects(struct window *win, struct rect *rects, ssize_t count, color_t color) {
    if (!count) return;

    static_assert(sizeof(struct rect) == sizeof(xcb_rectangle_t), "Rectangle types does not match");

    xcb_render_color_t col = MAKE_COLOR(color);
    xcb_render_fill_rectangles(con, XCB_RENDER_PICT_OP_SRC,
            get_plat(win)->pic1, col, count, (xcb_rectangle_t *)rects);
}

static inline void do_set_clip(struct window *win, struct rect *rects, ssize_t count) {
    if (!count) return;
    xcb_render_set_picture_clip_rectangles(con, get_plat(win)->pic1, 0, 0,
                                           count, (xcb_rectangle_t *)rects);
}

void x11_xrender_recolor_border(struct window *win) {
    struct rect rects[4];
    describe_borders(win, rects);
    do_draw_rects(win, rects, LEN(rects), win->bg_premul);
}

void x11_xrender_resize(struct window *win, int16_t new_w, int16_t new_h, int16_t new_cw, int16_t new_ch, bool artificial) {
    win->cfg.geometry.r.width = new_w;
    win->cfg.geometry.r.height = new_h;

    if (win->cw == new_cw && win->ch == new_ch) return;
    (void)artificial;

    int16_t delta_x = new_cw - win->cw;
    int16_t delta_y = new_ch - win->ch;

    win->cw = new_cw;
    win->ch = new_ch;

    struct extent bx = win_image_size(win);

    xcb_create_pixmap(con, TRUE_COLOR_ALPHA_DEPTH, get_plat(win)->pid2, get_plat(win)->wid, bx.width, bx.height);
    uint32_t mask3 = XCB_RENDER_CP_GRAPHICS_EXPOSURE | XCB_RENDER_CP_POLY_EDGE | XCB_RENDER_CP_POLY_MODE;
    uint32_t values3[3] = { 0, XCB_RENDER_POLY_EDGE_SMOOTH, XCB_RENDER_POLY_MODE_IMPRECISE };
    xcb_render_create_picture(con, get_plat(win)->pic2, get_plat(win)->pid2, rctx.pfargb, mask3, values3);

    SWAP(get_plat(win)->pid1, get_plat(win)->pid2);
    SWAP(get_plat(win)->pic1, get_plat(win)->pic2);

    do_draw_rects(win, &(struct rect){0, 0, bx.width, bx.height}, 1, win->bg_premul);

    int16_t common_w = MIN(new_cw, new_cw - delta_x) * win->char_width;
    int16_t common_h = MIN(new_ch, new_ch - delta_y) * (win->char_height + win->char_depth);
    xcb_render_composite(con, XCB_RENDER_PICT_OP_SRC, get_plat(win)->pic2, 0, get_plat(win)->pic1,
                         win->cfg.border.left, win->cfg.border.top, 0, 0,
                         win->cfg.border.left, win->cfg.border.top, common_w, common_h);

    xcb_render_free_picture(con, get_plat(win)->pic2);
    xcb_free_pixmap(con, get_plat(win)->pid2);
}


void x11_xrender_release_glyph(struct glyph *glyph) {
    if (glyph->pixmode == pixmode_bgra && glyph->id) {
        xcb_render_free_picture(con, glyph->id);
        free_cached_id(glyph->id);
    }
}

bool x11_xrender_reload_font(struct window *win, bool need_free) {
    struct window *found = window_find_shared_font(win, need_free, false);

    get_plat(win)->pfglyph = win->cfg.pixel_mode ? rctx.pfargb : rctx.pfalpha;

    xcb_void_cookie_t c;

    if (need_free) {
        c = xcb_render_free_glyph_set_checked(con, get_plat(win)->gsid);
        if (check_void_cookie(c)) warn("Can't free glyph set");
    }
    else get_plat(win)->gsid = xcb_generate_id(con);

    if (found && win->font_pixmode == found->font_pixmode) {
        c = xcb_render_reference_glyph_set_checked(con, get_plat(win)->gsid, get_plat(found)->gsid);
        if (check_void_cookie(c)) warn("Can't reference glyph set");
    } else {
        c = xcb_render_create_glyph_set_checked(con, get_plat(win)->gsid, get_plat(win)->pfglyph);
        if (check_void_cookie(c)) warn("Can't create glyph set");

        for (uint32_t i = ' '; i <= '~'; i++) {
            struct glyph *glyph = glyph_cache_fetch(win->font_cache, i, face_normal, NULL);
            glyph->x_off = win->char_width;
            register_glyph(win, i, glyph);
        }

        register_glyph(win, GLYPH_UNDERCURL, win->undercurl_glyph);
    }

    if (need_free) {
        handle_resize(win, win->cfg.geometry.r.width, win->cfg.geometry.r.height, true);
    } else {
        /* We need to resize window here if it's size is specified in chracters */
        x11_fixup_geometry(win);
        struct extent bx = win_image_size(win);

        get_plat(win)->pid1 = xcb_generate_id(con);
        get_plat(win)->pid2 = xcb_generate_id(con);

        c = xcb_create_pixmap_checked(con, TRUE_COLOR_ALPHA_DEPTH, get_plat(win)->pid1, get_plat(win)->wid, bx.width, bx.height );
        if (check_void_cookie(c)) {
            warn("Can't create pixmap");
            return 0;
        }

        uint32_t mask3 = XCB_RENDER_CP_GRAPHICS_EXPOSURE | XCB_RENDER_CP_POLY_EDGE | XCB_RENDER_CP_POLY_MODE;
        uint32_t values3[3] = { 0, XCB_RENDER_POLY_EDGE_SMOOTH, XCB_RENDER_POLY_MODE_IMPRECISE };

        get_plat(win)->pic1 = xcb_generate_id(con);
        get_plat(win)->pic2 = xcb_generate_id(con);

        c = xcb_render_create_picture_checked(con, get_plat(win)->pic1, get_plat(win)->pid1, rctx.pfargb, mask3, values3);
        if (check_void_cookie(c)) {
            warn("Can't create XRender picture");
            return 0;
        }

        do_draw_rects(win, &(struct rect) { 0, 0, bx.width, bx.height }, 1, win->bg_premul);

        xcb_pixmap_t pid = xcb_generate_id(con);
        c = xcb_create_pixmap_checked(con, TRUE_COLOR_ALPHA_DEPTH, pid, get_plat(win)->wid, 1, 1);
        if (check_void_cookie(c)) {
            warn("Can't create pixmap");
            free_window(win);
            return NULL;
        }

        get_plat(win)->pen = xcb_generate_id(con);
        uint32_t values4[1] = { XCB_RENDER_REPEAT_NORMAL };
        c = xcb_render_create_picture_checked(con, get_plat(win)->pen, pid, rctx.pfargb, XCB_RENDER_CP_REPEAT, values4);
        if (check_void_cookie(c)) {
            warn("Can't create picture");
            free_window(win);
            return NULL;
        }
        xcb_free_pixmap(con, pid);
    }

    x11_update_window_props(win);

    win->redraw_borders = 1;

    return 1;
}

void x11_xrender_free(struct window *win) {
    xcb_render_free_picture(con, get_plat(win)->pen);
    xcb_render_free_picture(con, get_plat(win)->pic1);
    xcb_free_pixmap(con, get_plat(win)->pid1);
    xcb_render_free_glyph_set(con, get_plat(win)->gsid);
}

static void xrender_init_context(void) {
    /* Check if XRender is present */
    xcb_render_query_version_cookie_t vc = xcb_render_query_version(con, XCB_RENDER_MAJOR_VERSION, XCB_RENDER_MINOR_VERSION);
    xcb_generic_error_t *err = NULL;
    xcb_render_query_version_reply_t *rep = xcb_render_query_version_reply(con, vc, &err);
    /* Any version is OK, so don't check */
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

    rctx.free_ids = xalloc(FREE_IDS_INIT_CAPS * sizeof *rctx.free_ids);
    rctx.id_capacity = FREE_IDS_INIT_CAPS;
}

void x11_xrender_update(struct window *win, struct rect rect) {
    xcb_copy_area(con, get_plat(win)->pid1, get_plat(win)->wid, get_plat(win)->gc, rect.x, rect.y, rect.x, rect.y, rect.width, rect.height);
}

void x11_xrender_copy(struct window *win, struct rect dst, int16_t sx, int16_t sy) {
    xcb_copy_area(con, get_plat(win)->pid1, get_plat(win)->pid1, get_plat(win)->gc, sx, sy, dst.x, dst.y, dst.width, dst.height);
}

static inline void adjust_msg_buffer(void) {
    if (UNLIKELY(rctx.payload_size + WORDS_IN_MESSAGE * sizeof(uint32_t) > rctx.payload_caps)) {
        rctx.payload = xrealloc(rctx.payload, rctx.payload_caps,
                                rctx.payload_caps + WORDS_IN_MESSAGE * sizeof(uint32_t));
        rctx.payload_caps  += WORDS_IN_MESSAGE * sizeof(uint32_t);
    }
}

static inline struct glyph_msg *start_msg(int16_t dx, int16_t dy) {
    struct glyph_msg *head = (struct glyph_msg *)(rctx.payload + rctx.payload_size);
    *head = (struct glyph_msg) {
        .dx = dx,
        .dy = dy,
    };
    rctx.payload_size += sizeof(*head);
    return head;
}

static void draw_text(struct window *win, struct element_buffer *buf) {
    for (struct element *it = buf->sorted, *end = buf->sorted + buf->size; it < end; ) {
        /* Prepare pen */
        color_t color = it->color;
        xcb_render_color_t col = MAKE_COLOR(color);
        xcb_rectangle_t rect2 = { .x = 0, .y = 0, .width = 1, .height = 1 };
        xcb_render_fill_rectangles(con, XCB_RENDER_PICT_OP_SRC, get_plat(win)->pen, col, 1, &rect2);

        /* Build payload... */

        rctx.payload_size = 0;

        int16_t old_x = 0, old_y = 0, x = it->x;
        uint32_t *glyph = rctx.glyphs + rctx.glyphs_caps - it->glyphs;
        for (;;) {
            adjust_msg_buffer();

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

        /* ..and send it */

        if (rctx.payload_size) {
            xcb_render_composite_glyphs_32(con, XCB_RENDER_PICT_OP_OVER,
                    get_plat(win)->pen, get_plat(win)->pic1, get_plat(win)->pfglyph, get_plat(win)->gsid,
                    0, 0, rctx.payload_size, rctx.payload);
        }
    }
}

static void draw_undercurls(struct window *win, struct element_buffer *buf) {
    for (struct element *it = buf->sorted, *end = buf->sorted + buf->size; it < end; ) {
        /* Prepare pen */
        color_t color = it->color;
        xcb_render_color_t col = MAKE_COLOR(color);
        xcb_rectangle_t rect2 = { .x = 0, .y = 0, .width = 1, .height = 1 };
        xcb_render_fill_rectangles(con, XCB_RENDER_PICT_OP_SRC, get_plat(win)->pen, col, 1, &rect2);

        /* Build payload... */

        rctx.payload_size = 0;

        int16_t old_x = 0, old_y = 0, x = it->x;
        uint32_t count = it->width;
        for (;;) {
            adjust_msg_buffer();

            struct glyph_msg *head = start_msg(x - old_x, it->y - old_y);
            old_x = x, old_y = it->y;

            do {
                uint32_t g = GLYPH_UNDERCURL;
                memcpy(rctx.payload + rctx.payload_size, &g, sizeof g);
                rctx.payload_size += sizeof g;
                head->len++;

                x += win->char_width;
                old_x += win->char_width;
            } while (--count && head->len < CHARS_PER_MESG);

            if (!count) {
                if (++it >= end || color != it->color) break;
                count = it->width;
                x = it->x;
            }
        }

        /* ..and send it */

        if (rctx.payload_size) {
            xcb_render_composite_glyphs_32(con, XCB_RENDER_PICT_OP_OVER,
                    get_plat(win)->pen, get_plat(win)->pic1, get_plat(win)->pfglyph, get_plat(win)->gsid,
                    0, 0, rctx.payload_size, rctx.payload);
        }
    }
}


/* X11-independent code is below */


static inline void free_elem_buffer(struct element_buffer *buf) {
    free(buf->data);
    *buf = (struct element_buffer){ 0 };
}

void x11_xrender_free_context(void) {
    free(rctx.payload);
    free(rctx.glyphs);
    free(rctx.free_ids);
    free_elem_buffer(&rctx.foreground_buf);
    free_elem_buffer(&rctx.background_buf);
    free_elem_buffer(&rctx.decoration_buf);
    free_elem_buffer(&rctx.decoration_buf2);
    free_elem_buffer(&rctx.image_buf);
}

void x11_xrender_init_context(void) {
    adjust_buffer((void **)&rctx.payload, &rctx.payload_caps, INIT_PAYLOAD_CAPS, 1);
    adjust_buffer((void **)&rctx.glyphs, &rctx.glyphs_caps, INIT_GLYPHS_CAPS, sizeof(rctx.glyphs[0]));
    adjust_buffer((void **)&rctx.foreground_buf.data, &rctx.foreground_buf.caps, INIT_FG_CAPS, sizeof(struct element));
    adjust_buffer((void **)&rctx.background_buf.data, &rctx.background_buf.caps, INIT_BG_CAPS, sizeof(struct element));
    adjust_buffer((void **)&rctx.decoration_buf.data, &rctx.decoration_buf.caps, INIT_DEC_CAPS, sizeof(struct element));
    adjust_buffer((void **)&rctx.decoration_buf2.data, &rctx.decoration_buf2.caps, INIT_DEC_CAPS, sizeof(struct element));
    adjust_buffer((void **)&rctx.image_buf.data, &rctx.image_buf.caps, INIT_DEC_CAPS, sizeof(struct element));

    xrender_init_context();
}


static void push_rect(struct rect *rect) {
    if (UNLIKELY(rctx.payload_size + sizeof(*rect) >= rctx.payload_caps)) {
        size_t new_size = 3 * rctx.payload_caps / 2;
        rctx.payload = xrealloc(rctx.payload, rctx.payload_caps, new_size);
        rctx.payload_caps = new_size;
    }

    memcpy(rctx.payload + rctx.payload_size, rect, sizeof(*rect));
    rctx.payload_size += sizeof(*rect);
}

static inline void push_element(struct element_buffer *dst, struct element *elem) {
    /* We need buffer twice as big a the number of elements to
     * be able to use merge sort efficiently */
    adjust_buffer((void **)&dst->data, &dst->caps, 2*(dst->size + 1), sizeof *elem);
    dst->data[dst->size++] = *elem;
}

static inline uint32_t push_char(uint32_t ch) {
    /* Character are pushed in buffer in reverse order,
     * since lines are scanned in reverse order */
    if (UNLIKELY(rctx.glyphs_size + 1 > rctx.glyphs_caps)) {
        size_t new_caps = 4*rctx.glyphs_caps/3;
        uint32_t *tmp = xzalloc(new_caps * sizeof(uint32_t));
        memcpy(tmp + new_caps - rctx.glyphs_size, rctx.glyphs, rctx.glyphs_size*sizeof(uint32_t));
        free(rctx.glyphs);
        rctx.glyphs = tmp;
        rctx.glyphs_caps = new_caps;
    }

    size_t new_pos = ++rctx.glyphs_size;
    rctx.glyphs[rctx.glyphs_caps - new_pos] = ch;
    return new_pos;
}

static inline void sort_by_color(struct element_buffer *buf) {
    struct element *dst = buf->data + buf->size, *src = buf->data;
    for (size_t k = 2; k < 2*buf->size; k += k) {
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
    buf->sorted = src;
}

static void prepare_multidraw(struct window *win, int16_t cur_x, ssize_t cur_y, bool reverse_cursor, bool *beyond_eol) {
    rctx.foreground_buf.size = 0;
    rctx.background_buf.size = 0;
    rctx.decoration_buf.size = 0;
    rctx.decoration_buf2.size = 0;
    rctx.image_buf.size = 0;
    rctx.glyphs_size = 0;

    bool slow_path = win->cfg.special_bold || win->cfg.special_underline || win->cfg.special_blink || win->cfg.blend_fg ||
                     win->cfg.special_reverse || win->cfg.special_italic || win->cfg.blend_all_bg || selection_active(term_get_sstate(win->term));

    struct screen *scr = term_screen(win->term);
    struct line_span span = screen_view(scr);
    for (ssize_t k = 0; k < win->ch; k++, screen_span_shift(scr, &span)) {
        screen_span_width(scr, &span);
        bool next_dirty = 0, first_in_line = 1;

        struct mouse_selection_iterator sel_it = selection_begin_iteration(term_get_sstate(win->term), &span);

        if (win->cw > span.width) {
            bool selected = is_selected_prev(&sel_it, &span, win->cw - 1);
            struct attr attr = *attr_pad(span.line);
            color_t bg = describe_bg(&attr, &win->cfg, &win->rcstate, selected);

            push_element(&rctx.background_buf, &(struct element) {
                .x = win->cfg.border.left + span.width * win->char_width,
                .y = win->cfg.border.top + k * (win->char_height + win->char_depth),
                .color = bg,
                .width = (win->cw - span.width) * win->char_width,
                .height = win->char_height + win->char_depth,
            });

            if (cur_y == k && cur_x >= span.width)
                *beyond_eol = true;

            next_dirty = 1;
            first_in_line = 0;
        }

        for (int16_t i = MIN(win->cw, span.width) - 1; i >= 0; i--) {
            struct cell *pcell = view_cell(&span, i);
            struct cell cel = *pcell;
            pcell->drawn = 1;

            struct attr attr = *view_attr(&span, cel.attrid);
            bool dirty = span.line->force_damage || !cel.drawn || (!win->blink_commited && attr.blink);

            struct cellspec spec;
            struct glyph *glyph = NULL;
            bool g_wide = 0;
            uint32_t g = 0;
            if (dirty || next_dirty) {
                if (k == cur_y && i == cur_x && reverse_cursor) {
                    attr.fg = win->rcstate.palette[SPECIAL_CURSOR_FG];
                    attr.bg = win->rcstate.palette[SPECIAL_CURSOR_BG];
                    attr.reverse ^= 1;
                }

                bool selected = is_selected_prev(&sel_it, &span, i);
                spec = describe_cell(cel, &attr, &win->cfg, &win->rcstate, selected, slow_path);
                g =  spec.ch | (spec.face << 24);

                bool is_new = 0;
                if (spec.ch) glyph = glyph_cache_fetch(win->font_cache, spec.ch, spec.face, &is_new);

                if (UNLIKELY(is_new) && glyph) register_glyph(win, g, glyph);

                g_wide = glyph && glyph->x_off > win->char_width - win->cfg.font_spacing;
            }
            if (dirty || (g_wide && next_dirty)) {
                int16_t y = k * (win->char_depth + win->char_height);

                /* Queue background, grouping by color */

                struct element *prev_bg = rctx.background_buf.data + rctx.background_buf.size - 1;
                if (LIKELY(!first_in_line && prev_bg->color == spec.bg &&
                        prev_bg->x == (i + 1)*win->char_width)) {
                    prev_bg->x -= win->char_width;
                    prev_bg->width += win->char_width;
                } else {
                    first_in_line = 0;
                    push_element(&rctx.background_buf, &(struct element) {
                        .x = win->cfg.border.left + i * win->char_width,
                        .y = win->cfg.border.top + y,
                        .color = spec.bg,
                        .width = win->char_width,
                        .height = win->char_height + win->char_depth,
                    });
                }

                /* Push character if present, grouping by color */

                if (spec.ch) {
                    if (glyph->pixmode == pixmode_bgra) {
                        push_element(&rctx.image_buf, &(struct element) {
                            .x = win->cfg.border.left + i * win->char_width - glyph->x,
                            .y = win->cfg.border.top + y + win->char_height - glyph->y,
                            .picture = glyph->id,
                            .width = glyph->width,
                            .height = glyph->height,
                        });
                    } else {
                        g |=  (uint32_t)spec.wide << 31;
                        struct element *prev_fg = rctx.foreground_buf.data + rctx.foreground_buf.size - 1;
                        if (LIKELY(rctx.foreground_buf.size && prev_fg->y == y + win->char_height &&
                                prev_fg->color == spec.fg && prev_fg->x == (i + spec.wide + 1)*win->char_width)) {
                            prev_fg->glyphs = push_char(g);
                            prev_fg->x -= win->char_width*(1 + spec.wide);
                        } else {
                            push_char(0);
                            push_element(&rctx.foreground_buf, &(struct element) {
                                .x = win->cfg.border.left + i * win->char_width,
                                .y = win->cfg.border.top + y + win->char_height,
                                .color = spec.fg,
                                .glyphs = push_char(g),
                            });
                        }
                    }
                }


                /* Push strikethrough/underline rects, if present */

                if (UNLIKELY(spec.underlined)) {
                    int16_t line_y = y + win->char_height + 1 + win->cfg.line_spacing/2;
                    if (spec.underlined == 3) {
                        struct element *prev_dec = rctx.decoration_buf2.data + rctx.decoration_buf2.size - 1;
                        if (rctx.decoration_buf2.size && prev_dec->y == line_y && prev_dec->color == spec.ul &&
                                prev_dec->x == (i + 1)*win->char_width) {
                            prev_dec->width++;
                            prev_dec->x -= win->char_width;
                        } else {
                            push_element(&rctx.decoration_buf2, &(struct element) {
                                .x = win->cfg.border.left + i * win->char_width,
                                .y = win->cfg.border.top + line_y,
                                .color = spec.ul,
                                .width = 1,
                            });
                        }
                    } else {
                        struct element *prev_dec = rctx.decoration_buf.data + rctx.decoration_buf.size - 1;
                        if (rctx.decoration_buf.size && prev_dec->y == line_y  && prev_dec->color == spec.ul &&
                                prev_dec->x == (i + 1)*win->char_width) {
                            prev_dec->x -= win->char_width;
                            prev_dec->width += win->char_width;
                        } else if (rctx.decoration_buf.size > 1 && prev_dec[-1].y == line_y  && prev_dec[-1].color == spec.ul &&
                                prev_dec[-1].x == (i + 1)*win->char_width) {
                            prev_dec[-1].x -= win->char_width;
                            prev_dec[-1].width += win->char_width;
                        } else if (rctx.decoration_buf.size > 2 && prev_dec[-2].y == line_y && prev_dec[-2].color == spec.ul &&
                                prev_dec[-2].x == (i + 1)*win->char_width) {
                            prev_dec[-2].x -= win->char_width;
                            prev_dec[-2].width += win->char_width;
                        } else {
                            push_element(&rctx.decoration_buf, &(struct element) {
                                .x = win->cfg.border.left + i * win->char_width,
                                .y = win->cfg.border.top + line_y,
                                .color = spec.ul,
                                .width = win->char_width,
                                .height = win->cfg.underline_width,
                            });
                            if (spec.underlined > 1) {
                                push_element(&rctx.decoration_buf, &(struct element) {
                                    .x = win->cfg.border.left + i * win->char_width,
                                    .y = win->cfg.border.top + line_y + win->cfg.underline_width + 1,
                                    .color = spec.ul,
                                    .width = win->char_width,
                                    .height = win->cfg.underline_width,
                                });
                            }
                        }
                    }
                }

                if (UNLIKELY(spec.stroke)) {
                    int16_t line_y = y + 2*win->char_height/3 - win->cfg.underline_width/2 + win->cfg.line_spacing/2;
                    struct element *prev_dec = rctx.decoration_buf.data + rctx.decoration_buf.size - 1;
                    if (rctx.decoration_buf.size && prev_dec->y == line_y  && prev_dec->color == spec.bg &&
                            prev_dec->x == (i + 1)*win->char_width) {
                        prev_dec->x -= win->char_width;
                        prev_dec->width += win->char_width;
                    } else if (rctx.decoration_buf.size > 1 && prev_dec[-1].y == line_y  && prev_dec[-1].color == spec.ul &&
                            prev_dec[-1].x == (i + 1)*win->char_width) {
                        prev_dec[-1].x -= win->char_width;
                        prev_dec[-1].width += win->char_width;
                    } else if (rctx.decoration_buf.size > 2 && prev_dec[-2].y == line_y  && prev_dec[-2].color == spec.ul &&
                            prev_dec[-2].x == (i + 1)*win->char_width) {
                        prev_dec[-2].x -= win->char_width;
                        prev_dec[-2].width += win->char_width;
                    } else {
                        push_element(&rctx.decoration_buf, &(struct element) {
                            .x = win->cfg.border.left + i * win->char_width,
                            .y = win->cfg.border.top + line_y,
                            .color = spec.ul,
                            .width = win->char_width,
                            .height = win->cfg.underline_width,
                        });
                    }
                }
            }
            next_dirty = dirty;
        }
        /* Only reset force flag for last part of the line */
        if (!view_wrapped(&span))
            span.line->force_damage = 0;
    }
}

static void reset_clip(struct window *win) {
    struct rect rect = window_rect(win);
    do_set_clip(win, &rect, 1);
}

static void set_clip(struct window *win, struct element_buffer *buf) {
    rctx.payload_size = 0;
    for (struct element *it = buf->sorted, *end = buf->sorted + buf->size; it < end; ) {
        struct element *it2 = it;
        do it2++;
        while (it2 < end && it2->y == it->y &&
                it2->x + it2->width == it2[-1].x);
        push_rect(&(struct rect) {
            .x = it2[-1].x,
            .y = it->y,
            .width = it->x - it2[-1].x + it->width,
            .height = win->char_depth + win->char_height
        });
        it = it2;
    }
    do_set_clip(win, (struct rect *)rctx.payload, rctx.payload_size/sizeof(struct rect));
}

static void draw_rects(struct window *win, struct element_buffer *buf) {
    for (struct element *it = buf->sorted, *end = buf->sorted + buf->size; it < end; ) {
        color_t color = it ->color;

        rctx.payload_size = 0;
        do {
            push_rect(&(struct rect) {
                .x = it->x,
                .y = it->y,
                .width = it->width,
                .height = it->height,
            });
        } while (++it < end && it->color == color);

        do_draw_rects(win, (struct rect *)rctx.payload, rctx.payload_size/sizeof(struct rect), color);
    }
}

static void draw_images(struct window *win, struct element_buffer *buf) {
    for (struct element *it = buf->data, *end = buf->data + buf->size; it < end; it++) {
        xcb_render_composite(con, XCB_RENDER_PICT_OP_OVER, it->picture, 0, get_plat(win)->pic1,
                             0, 0, 0, 0, it->x, it->y, it->width, it->height);
    }
}

bool x11_xrender_submit_screen(struct window *win, int16_t cur_x, ssize_t cur_y, bool cursor, bool on_margin) {
    bool reverse_cursor = cursor && win->focused && ((win->cfg.cursor_shape + 1) & ~1) == cusor_type_block;
    bool cond_cblink = !win->blink_commited && (win->cfg.cursor_shape & 1);
    bool beyond_eol = false;
    if (cond_cblink) cursor &= win->rcstate.blink;

    prepare_multidraw(win, cur_x, cur_y, reverse_cursor, &beyond_eol);

    sort_by_color(&rctx.background_buf);
    set_clip(win, &rctx.background_buf);

    /* Draw cells backgrounds */
    draw_rects(win, &rctx.background_buf);

    sort_by_color(&rctx.foreground_buf);
    draw_text(win, &rctx.foreground_buf);
    draw_images(win, &rctx.image_buf);

    if (rctx.background_buf.size) reset_clip(win);

    /* Draw underline and strikethrough lines */
    sort_by_color(&rctx.decoration_buf);
    draw_rects(win, &rctx.decoration_buf);

    sort_by_color(&rctx.decoration_buf2);
    draw_undercurls(win, &rctx.decoration_buf2);

    if (cursor) {
        struct cursor_rects cr = describe_cursor(win, cur_x, cur_y, on_margin, beyond_eol);
        do_draw_rects(win, cr.rects + cr.offset, cr.count, win->cursor_fg);
    }

    bool drawn = 0;

    if (rctx.background_buf.size || win->redraw_borders) {
        x11_xrender_update(win, window_rect(win));
        win->redraw_borders = 0;
        drawn = 1;
    }

    return drawn;
}
