#define _POSIX_C_SOURCE 200809L


#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>

#include <xcb/xcb.h>
#include <xcb/render.h>
#include <xcb/xcb_xrm.h>
#include <xcb/xcb_keysyms.h>
#include <X11/keysym.h>
#include <X11/XF86keysym.h>

#include "window.h"
#include "util.h"
#include "font.h"
#include "term.h"

#define TRUE_COLOR_ALPHA_DEPTH 32
#define NUM_BORDERS 4

#define ASCII_MAX 128
#define ASCII_PRINTABLE_HIGH 127
#define ASCII_PRINTABLE_LOW 32

#define WORDS_IN_MESSAGE 256
#define HEADER_WORDS ((sizeof(nss_glyph_mesg_t)+sizeof(uint32_t))/sizeof(uint32_t))
#define CHARS_PER_MESG (WORDS_IN_MESSAGE - HEADER_WORDS)

#define CR(c) (((c) & 0xff) * 0x101)
#define CG(c) ((((c) >> 8) & 0xff) * 0x101)
#define CB(c) ((((c) >> 16) & 0xff) * 0x101)
#define CA(c) ((((c) >> 24) & 0xff) * 0x101)
#define MAKE_COLOR(c) {.red=CR(c),.green=CG(c),.blue=CB(c),.alpha=CA(c)}


struct nss_window {
    xcb_window_t wid;
    xcb_pixmap_t pid;
    xcb_gcontext_t gc;
    xcb_render_picture_t pic;

    _Bool focused;
    _Bool lcd_mode;

    int16_t width;
    int16_t height;
    int16_t cw, ch;
    int16_t cursor_width;
    int16_t underline_width;
    int16_t left_border;
    int16_t top_border;
    int16_t font_size;

    nss_color_t background;
    nss_color_t foreground;
    nss_color_t cursor_background;
    nss_color_t cursor_foreground;
    nss_cursor_type_t cursor_type;


    /*     * Glyph encoding:
     *  0xTTUUUUUU, where
     *      * 0xTT - font fase
     *      * 0xUUUUUU - unicode character
     */
    nss_font_t *font;
    xcb_render_glyphset_t gsid;
    xcb_render_pictformat_t pfglyph;
    int16_t char_width;
    int16_t char_depth;
    int16_t char_height;

    char *font_name;
    nss_term_t *term;
    struct nss_window *prev, *next;
};


struct nss_context {
    size_t refs;
    xcb_connection_t *con;
    xcb_screen_t *screen;
    xcb_colormap_t mid;
    xcb_visualtype_t *vis;
    xcb_key_symbols_t* keysyms;

    xcb_render_pictformat_t pfargb;
    xcb_render_pictformat_t pfalpha;

    nss_window_t *first;
};

typedef struct nss_glyph_mesg {
    uint8_t len;
    uint8_t pad[3];
    int16_t dx, dy;
    uint8_t data[];
} nss_glyph_mesg_t;

static void assert_void_cookie(nss_context_t *con, xcb_void_cookie_t ck, const char *msg){
    xcb_generic_error_t *err = xcb_request_check(con->con,ck);
    if(err){
        uint32_t c = err->error_code,
        ma = err->major_code,
        mi = err->minor_code;
        nss_free_context(con);
        die("%s: %"PRIu32" %"PRIu32" %"PRIu32,msg,ma,mi,c);
    }
}

static nss_window_t *nss_window_for_xid(nss_context_t *con, xcb_window_t xid){
    for(nss_window_t *win = con->first; win; win = win->next)
        if(win->wid == xid) return win;
    warn("Unknown window 0x%08x",xid);
    return NULL;
}

/* Initialize global state object */
nss_context_t *nss_create_context(void){
    nss_context_t *con = calloc(1, sizeof(*con));

    int screenp;
    con->con = xcb_connect(NULL, &screenp);
    xcb_screen_iterator_t sit = xcb_setup_roots_iterator(xcb_get_setup(con->con));
    for(; sit.rem; xcb_screen_next(&sit))
        if(screenp-- == 0)break;
    if(screenp != -1){
        xcb_disconnect(con->con);
        die("Can't find default screen");
    }
    con->screen = sit.data;

    xcb_depth_iterator_t dit = xcb_screen_allowed_depths_iterator(con->screen);
    for(; dit.rem; xcb_depth_next(&dit))
        if(dit.data->depth == TRUE_COLOR_ALPHA_DEPTH) break;
    if(dit.data->depth != TRUE_COLOR_ALPHA_DEPTH){
        xcb_disconnect(con->con);
        die("Can't get 32-bit visual");
    }

    xcb_visualtype_iterator_t vit = xcb_depth_visuals_iterator(dit.data);
    for(; vit.rem; xcb_visualtype_next(&vit))
        if(vit.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR) break;

    if(vit.data->_class != XCB_VISUAL_CLASS_TRUE_COLOR){
        xcb_disconnect(con->con);
        die("Can't get 32-bit visual");
    }

    con->vis = vit.data;

    con->mid = xcb_generate_id(con->con);
    xcb_void_cookie_t c = xcb_create_colormap_checked(con->con, XCB_COLORMAP_ALLOC_NONE,
                                       con->mid, con->screen->root, con->vis->visual_id);
    assert_void_cookie(con,c,"Can't create colormap");

    // Check if XRender is present
    xcb_render_query_version_cookie_t vc = xcb_render_query_version(con->con, XCB_RENDER_MAJOR_VERSION, XCB_RENDER_MINOR_VERSION);
    xcb_generic_error_t *err;
    xcb_render_query_version_reply_t *rep = xcb_render_query_version_reply(con->con, vc, &err);
    // Any version is OK, so don't check
    free(rep);

    if(err){
        uint8_t erc = err->error_code;
        xcb_disconnect(con->con);
        die("XRender not detected: %"PRIu8, erc);
    }


    xcb_render_query_pict_formats_cookie_t pfc = xcb_render_query_pict_formats(con->con);
    xcb_render_query_pict_formats_reply_t *pfr = xcb_render_query_pict_formats_reply(con->con, pfc, &err);

    if(err){
        uint8_t erc = err->error_code;
        xcb_disconnect(con->con);
        die("Can't query picture formats: %"PRIu8, erc);
    }

    xcb_render_pictforminfo_iterator_t pfit =  xcb_render_query_pict_formats_formats_iterator(pfr);
    for(; pfit.rem; xcb_render_pictforminfo_next(&pfit)){
        if(pfit.data->depth == TRUE_COLOR_ALPHA_DEPTH && pfit.data->type == XCB_RENDER_PICT_TYPE_DIRECT &&
           pfit.data->direct.red_mask == 0xff && pfit.data->direct.green_mask == 0xff &&
           pfit.data->direct.blue_mask == 0xff && pfit.data->direct.alpha_mask == 0xff &&
           pfit.data->direct.red_shift == 16 && pfit.data->direct.green_shift == 8 &&
           pfit.data->direct.blue_shift == 0 && pfit.data->direct.alpha_shift == 24 ){
               con->pfargb = pfit.data->id;
        }
        if(pfit.data->depth == 8 && pfit.data->type == XCB_RENDER_PICT_TYPE_DIRECT &&
           pfit.data->direct.alpha_mask == 0xff && pfit.data->direct.alpha_shift == 0){
               con->pfalpha = pfit.data->id;
        }
    }

    free(pfr);

    if(con->pfargb == 0 || con->pfalpha == 0){
        xcb_disconnect(con->con);
        die("Can't find suitable picture format");
    }

    con->keysyms = xcb_key_symbols_alloc(con->con);
    return con;
}

/* Free all resources */
void nss_free_context(nss_context_t *con){
        while(con->first)
            nss_free_window(con,con->first);
        xcb_disconnect(con->con);
        xcb_key_symbols_free(con->keysyms);
        free(con);
}

static void register_glyph(nss_context_t *con, nss_window_t *win, uint32_t ch, nss_glyph_t * glyph){
        xcb_render_glyphinfo_t spec = {
            .width = glyph->width, .height = glyph->height,
            .x = glyph->x, .y = glyph->y,
            .x_off = glyph->x_off, .y_off = glyph->y_off
        };
        xcb_render_add_glyphs(con->con, win->gsid, 1, &ch, &spec, glyph->height*glyph->stride, glyph->data);
}

static void set_config(nss_window_t *win, nss_wc_tag_t tag, uint32_t *values){
    if(tag & nss_wc_cusror_width) win->cursor_width = *values++;
    if(tag & nss_wc_left_border) win->left_border = *values++;
    if(tag & nss_wc_top_border) win->top_border = *values++;
    if(tag & nss_wc_background) win->background = *values++;
    if(tag & nss_wc_foreground) win->foreground = *values++;
    if(tag & nss_wc_cursor_background) win->cursor_background = *values++;
    if(tag & nss_wc_cursor_foreground) win->cursor_foreground = *values++;
    if(tag & nss_wc_cursor_type) win->cursor_type = *values++;
    if(tag & nss_wc_lcd_mode) win->lcd_mode = *values++;
    if(tag & nss_wc_font_size) win->font_size = *values++;
    if(tag & nss_wc_underline_width) win->underline_width = *values++;
}

/* Reload font using win->font_size and win->font_name */
static void reload_font(nss_context_t *con, nss_window_t *win, _Bool need_free){
    xcb_void_cookie_t c;

    nss_font_t *new = nss_create_font(win->font_name, win->font_size, nss_context_get_dpi(con));
    if(!new) {
        warn("Can't reload font");
        return;
    }
    if(need_free) nss_free_font(win->font);

    win->font = new;
    win->font_size = nss_font_get_size(new);
    win->pfglyph = win->lcd_mode ? con->pfargb : con->pfalpha;

    /* TODO: Deduplicate glyphsets between windows */
    if(need_free) xcb_render_free_glyph_set(con->con,win->gsid);
    else win->gsid = xcb_generate_id(con->con);
    xcb_render_create_glyph_set(con->con,win->gsid, win->pfglyph);

    //Preload ASCII
    int16_t total = 0, maxd = 0, maxh = 0;
    for(uint32_t i = ' '; i <= '~'; i++){
        nss_glyph_t *glyphs[nss_font_attrib_max];
        for(size_t j = 0; j < nss_font_attrib_max; j++){
            glyphs[j] = nss_font_render_glyph(win->font, i, j, win->lcd_mode);
            register_glyph(con,win, i | (j << 24),glyphs[j]);
        }

        total += glyphs[0]->x_off;
        maxd = MAX(maxd, glyphs[0]->height - glyphs[0]->y);
        maxh = MAX(maxh, glyphs[0]->y);

        for(size_t j = 0; j < nss_font_attrib_max; j++)
            free(glyphs[j]);
    }
    win->char_width = (total + '~' - ' ') / ('~' - ' ' + 1);

    win->char_height = maxh;
    win->char_depth = maxd;
    win->cw = MAX(1,(win->width - 2*win->left_border) / win->char_width);
    win->ch = MAX(1,(win->height - 2*win->top_border) / (win->char_height + win->char_depth));

    xcb_rectangle_t bound = { 0, 0, win->cw*win->char_width, win->ch*(win->char_depth+win->char_height) };

    if(need_free){
        xcb_free_pixmap(con->con, win->pid);
        xcb_free_gc(con->con, win->gc);
        xcb_render_free_picture(con->con, win->pic);
        nss_term_resize(win->term, win->cw, win->ch);
    } else {
        win->pid = xcb_generate_id(con->con);
        win->gc = xcb_generate_id(con->con);
        win->pic = xcb_generate_id(con->con);
    }

    c = xcb_create_pixmap_checked(con->con, TRUE_COLOR_ALPHA_DEPTH, win->pid, win->wid, bound.width, bound.height );
    assert_void_cookie(con,c,"Can't create pixmap");

    uint32_t mask2 = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_GRAPHICS_EXPOSURES;
    uint32_t values2[3] = { win->background, win->background, 0 };
    c = xcb_create_gc_checked(con->con,win->gc,win->pid,mask2, values2);
    assert_void_cookie(con,c,"Can't create GC");

    uint32_t mask3 = XCB_RENDER_CP_GRAPHICS_EXPOSURE | XCB_RENDER_CP_POLY_EDGE | XCB_RENDER_CP_POLY_MODE;
    uint32_t values3[3] = { 0, XCB_RENDER_POLY_EDGE_SMOOTH, XCB_RENDER_POLY_MODE_IMPRECISE };
    c = xcb_render_create_picture_checked(con->con,win->pic, win->pid, con->pfargb, mask3, values3);
    assert_void_cookie(con,c,"Can't create XRender picture");

    xcb_render_color_t color = MAKE_COLOR(win->background);
    xcb_render_fill_rectangles(con->con, XCB_RENDER_PICT_OP_OVER, win->pic, color, 1, &bound);

}

/* Create new window */
nss_window_t *nss_create_window(nss_context_t *con, nss_rect_t rect, const char *font_name, nss_wc_tag_t tag, uint32_t *values){
    nss_window_t *win = malloc(sizeof(nss_window_t));
    win->next = con->first;
    win->prev = NULL;
    if(con->first) con->first->prev = win;
    con->first = win;

    win->cursor_width = 2;
    win->underline_width = 1;
    win->left_border = 8;
    win->top_border = 8;
    win->background = 0xff000000;
    win->foreground = 0xffffffff;
    win->cursor_background = 0xff000000;
    win->cursor_foreground = 0xffffffff;
    win->cursor_type = nss_cursor_bar;
    win->lcd_mode = 1;
    win->font_size = 0;
    win->font_name = strdup(font_name);

    set_config(win,tag, values);


    xcb_void_cookie_t c;

    uint32_t mask1 =  XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL |
        XCB_CW_BIT_GRAVITY | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
    uint32_t values1[5] = {
        win->background,  win->background,
        XCB_GRAVITY_NORTH_WEST, XCB_EVENT_MASK_EXPOSURE |
        XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_FOCUS_CHANGE |
        XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_KEY_RELEASE, con->mid
    };
    win->wid = xcb_generate_id(con->con);
    c = xcb_create_window_checked(con->con, TRUE_COLOR_ALPHA_DEPTH, win->wid, con->screen->root,
                                  rect.x,rect.y, rect.width, rect.height, 0,
                                  XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                  con->vis->visual_id, mask1,values1);
    assert_void_cookie(con,c,"Can't create window");

    reload_font(con, win, 0);

    info("Font size: %d %d %d", win->char_height, win->char_depth, win->char_width);

    win->term = nss_create_term(con, win, win->cw, win->ch);

    return win;
}

/* Free previously created windows */
void nss_free_window(nss_context_t *con, nss_window_t *win){
    xcb_unmap_window(con->con,win->wid);
    xcb_flush(con->con);

    if(win->next)win->next->prev = win->prev;
    if(win->prev)win->prev->next = win->next;
    else con->first =  win->next;

    nss_free_term(win->term);
    nss_free_font(win->font);

    xcb_render_free_picture(con->con,win->pic);
    xcb_free_gc(con->con,win->gc);
    xcb_free_pixmap(con->con,win->pid);
    xcb_render_free_glyph_set(con->con,win->gsid);
    xcb_destroy_window(con->con,win->wid);

    free(win->font_name);
    free(win);
};

/* Get monitor DPI */
uint16_t nss_context_get_dpi(nss_context_t *con){
    xcb_xrm_database_t *xrmdb = xcb_xrm_database_from_default(con->con);
    long dpi = 0;
    if(xrmdb){
        if(xcb_xrm_resource_get_long(xrmdb, "Xft.dpi", NULL, &dpi) >= 0){
            xcb_xrm_database_free(xrmdb);
            return dpi;
        }
        xcb_xrm_database_free(xrmdb);
    }
    warn("Can't fetch Xft.dpi, defaulting to highest dpi value");

    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(con->con));
    for(; it.rem; xcb_screen_next(&it))
        if(it.data)
            dpi = MAX(dpi, (it.data->width_in_pixels * 25.4)/it.data->width_in_millimeters);
    if(!dpi){
        warn("Can't get highest dpi, defaulting to 96");
        dpi = 96;
    }
    return dpi;
}

static xcb_render_picture_t create_pen(nss_context_t *con, nss_window_t *win, xcb_render_color_t *color){
    static xcb_pixmap_t pid = 0;

    if(pid == 0) pid = xcb_generate_id(con->con);

    xcb_create_pixmap(con->con, TRUE_COLOR_ALPHA_DEPTH, pid, win->wid, 1, 1);

    uint32_t values[1] = { XCB_RENDER_REPEAT_NORMAL };
    xcb_render_picture_t pic = xcb_generate_id(con->con);
    xcb_render_create_picture(con->con, pic, pid, con->pfargb, XCB_RENDER_CP_REPEAT, values);

    xcb_rectangle_t rect = { .x = 0, .y = 0, .width = 1, .height = 1 };
    xcb_render_fill_rectangles(con->con, XCB_RENDER_PICT_OP_OVER, pic, *color, 1, &rect);

    xcb_free_pixmap(con->con, pid);

    return pic;
}

static void draw_cursor(nss_context_t *con, nss_window_t *win, uint16_t x, uint16_t y){
    xcb_rectangle_t rects[4] = {
        {x-1,y-win->char_height,1,win->char_height+win->char_depth},
        {x,y-win->char_height,win->char_width, 1},
        {x+win->char_width-1,y-win->char_height,1,win->char_height+win->char_depth},
        {x,y+win->char_depth-1,win->char_width, 1}
    };
    size_t off = 0, count = 4;
    if(win->focused){
        if(win->cursor_type == nss_cursor_bar){
            count = 1;
            rects[0].width = win->cursor_width;
        } else if(win->cursor_type == nss_cursor_underline){
            count = 1;
            off = 3;
            rects[3].height = win->cursor_width;
            rects[3].y -= win->cursor_width - 1;
        }
    }
    xcb_render_color_t c = MAKE_COLOR(win->cursor_foreground);
    xcb_render_fill_rectangles(con->con,XCB_RENDER_PICT_OP_OVER, win->pic, c, count, rects + off);
}

/* Draw UCS4-encoded string of at terminal coordinates (x,y) with attribute set attr */
void nss_window_draw_ucs4(nss_context_t *con, nss_window_t *win, size_t len,  uint32_t *str,
                         nss_text_attrib_t *attr, int16_t x, int16_t y){
    x = x * win->char_width;
    y = y * (win->char_height + win->char_depth) + win->char_height;

    nss_color_t fgc = attr->fg, bgc = attr->bg;
    nss_font_attrib_t fattr = attr->flags & nss_font_attrib_mask;

    if(attr->flags & nss_attrib_inverse)
        SWAP(nss_color_t, fgc, bgc);
    if(attr->flags & nss_attrib_cursor && win->cursor_type == nss_cursor_block)
        fgc = win->cursor_foreground, bgc = win->cursor_background;

    xcb_render_color_t fg = MAKE_COLOR(fgc);
    xcb_render_color_t bg = MAKE_COLOR(bgc);

    if(attr->flags & nss_attrib_background)
        bg.alpha = CA(win->background);

    xcb_rectangle_t rect = {
        .x = x, .y = y-win->char_height,
        .width = win->char_width*len,
        .height = win->char_height+win->char_depth
    };
    xcb_rectangle_t lines[2] = {
        { .x = x, .y= y + 1, .width = win->char_width*len, .height = win->underline_width },
        { .x = x, .y= y - win->char_height/3, .width = win->char_width*len, .height = win->underline_width }
    };
    xcb_render_picture_t pen = create_pen(con, win, &fg);

    size_t messages = (len+CHARS_PER_MESG - 1)/CHARS_PER_MESG;
    size_t data_len = messages*sizeof(nss_glyph_mesg_t)+len*sizeof(uint32_t);
    uint32_t *data = malloc(data_len);
    uint32_t *off = data;
    nss_glyph_mesg_t msg = {.dx=x,.dy=y};

    for(size_t msgi = 0,chari = 0; msgi < messages; msgi++, chari += CHARS_PER_MESG){
        msg.len = (len - chari < CHARS_PER_MESG ? len - chari : CHARS_PER_MESG);

        memcpy(off, &msg, sizeof(msg));
        off += sizeof(msg) / sizeof(uint32_t);
        memcpy(off, str+chari, msg.len*sizeof(uint32_t));
        for(size_t i = 0; i < msg.len; i++)
            off[i] |= fattr << 24;
        off += msg.len;
        //Reset for all, except first
        msg.dx = msg.dy = 0;
    }

    for(size_t i = 0; i < len; i++){
        if(str[i] > ASCII_MAX){
            nss_glyph_t *glyph = nss_font_render_glyph(win->font, str[i], fattr, win->lcd_mode);
            //In case of non-monospace fonts
            glyph->x_off = win->char_width - glyph->x;
            register_glyph(con,win,str[i] | (fattr << 24), glyph);
            free(glyph);
        }
    }

    xcb_render_fill_rectangles(con->con, XCB_RENDER_PICT_OP_OVER, win->pic, bg, 1, &rect);

    if(attr->flags & (nss_attrib_underlined|nss_attrib_strikethrough)){
        size_t count = !!(attr->flags & nss_attrib_underlined) + !!(attr->flags & nss_attrib_strikethrough);
        size_t offset = !(attr->flags & nss_attrib_underlined) && !!(attr->flags & nss_attrib_strikethrough);
        xcb_render_fill_rectangles(con->con, XCB_RENDER_PICT_OP_OVER, win->pic, fg, count, lines+offset);
    }

    xcb_render_composite_glyphs_32(con->con, XCB_RENDER_PICT_OP_OVER,
                                   pen, win->pic, win->pfglyph, win->gsid,
                                   0, 0, data_len, (uint8_t*)data);

    if(attr->flags & nss_attrib_cursor)
        draw_cursor(con,win,x,y);

    free(data);
    xcb_render_free_picture(con->con, pen);
}


static void redraw_damage(nss_context_t *con, nss_window_t *win, nss_rect_t damage){

        int16_t width = win->cw * win->char_width + win->left_border;
        int16_t height = win->ch * (win->char_height + win->char_depth) + win->top_border;

        size_t num_damaged = 0;
        nss_rect_t damaged[NUM_BORDERS], borders[NUM_BORDERS] = {
            {0, 0, win->left_border, height},
            {win->left_border, 0, width, win->top_border},
            {width, win->top_border, win->width - width, win->height},
            {0, height, width, win->height - height},
        };
        for(size_t i = 0; i < NUM_BORDERS; i++)
            if(intersect_with(&borders[i],&damage))
                    damaged[num_damaged++] = borders[i];
        if(num_damaged)
            xcb_poly_fill_rectangle(con->con, win->wid, win->gc, num_damaged, (xcb_rectangle_t*)damaged);

        nss_rect_t inters = { win->left_border, win->top_border, width, height };
        if(intersect_with(&inters, &damage)){
            xcb_copy_area(con->con,win->pid,win->wid,win->gc, inters.x - win->left_border, inters.y - win->top_border,
                          inters.x, inters.y, inters.width, inters.height);
        }
}

/* Redraw a region of window specified by <damage[len]> in terminal coordinates */
void nss_window_update(nss_context_t *con, nss_window_t *win, size_t len, nss_rect_t *damage){
    for(size_t i = 0; i < len; i++){
        nss_rect_t rect = damage[i];
        rect = rect_scale_up(rect, win->char_width, win->char_height + win->char_depth);
        rect = rect_shift(rect, win->left_border, win->top_border);
        xcb_copy_area(con->con,win->pid,win->wid,win->gc,
                      rect.x - win->left_border, rect.y - win->top_border,
                      rect.x, rect.y, rect.width, rect.height);
    }
}

void nss_window_clear(nss_context_t *con, nss_window_t *win, size_t len, nss_rect_t *damage){
    for(size_t i = 0; i < len; i++)
        damage[i] = rect_scale_up(damage[i], win->char_width, win->char_height + win->char_depth);

    xcb_render_color_t color = MAKE_COLOR(win->background);
    xcb_render_fill_rectangles(con->con, XCB_RENDER_PICT_OP_OVER, win->pic, color, len, (xcb_rectangle_t*)damage);
}

void nss_window_set(nss_context_t *con, nss_window_t *win, nss_wc_tag_t tag, uint32_t *values){
    set_config(win, tag, values);
    if (tag & (nss_wc_font_size | nss_wc_lcd_mode))
        reload_font(con, win, 1);
    nss_term_redraw(win->term, (nss_rect_t){0,0,win->cw, win->ch});
    redraw_damage(con, win, (nss_rect_t){0,0,win->width, win->height});
    xcb_flush(con->con);
}

void nss_window_set_font(nss_context_t *con, nss_window_t *win, const char * name){
    if(!name) {
        warn("Empty font name");
        return;
    }
    free(win->font_name);
    win->font_name = strdup(name);
    reload_font(con, win, 1);
    nss_term_redraw(win->term, (nss_rect_t){0,0,win->cw, win->ch});
    redraw_damage(con, win, (nss_rect_t){0,0,win->width, win->height});
    xcb_flush(con->con);
}

nss_font_t *nss_window_get_font(nss_context_t *con, nss_window_t *win){
    return win->font;
}

char *nss_window_get_font_name(nss_context_t *con, nss_window_t *win){
    return win->font_name;
}

uint32_t nss_window_get(nss_context_t *con, nss_window_t *win, nss_wc_tag_t tag){
    if(tag & nss_wc_cusror_width) return win->cursor_width;
    if(tag & nss_wc_left_border) return win->left_border;
    if(tag & nss_wc_top_border) return win->top_border;
    if(tag & nss_wc_background) return win->background;
    if(tag & nss_wc_foreground) return win->foreground;
    if(tag & nss_wc_cursor_background) return win->cursor_background;
    if(tag & nss_wc_cursor_foreground) return win->cursor_foreground;
    if(tag & nss_wc_cursor_type) return win->cursor_type;
    if(tag & nss_wc_lcd_mode) return win->lcd_mode;
    if(tag & nss_wc_font_size) return win->font_size;
    warn("Invalid option");
    return 0;
}

static void handle_resize(nss_context_t *con, nss_window_t *win, int16_t width, int16_t height){

    _Bool redraw_borders = width < win->width || height < win->height;
    //Handle resize

    win->width = width;
    win->height = height;

    int16_t new_cw = MAX(1,(win->width - 2*win->left_border)/win->char_width);
    int16_t new_ch = MAX(1,(win->height - 2*win->top_border)/(win->char_height+win->char_depth));
    int16_t delta_x = new_cw - win->cw;
    int16_t delta_y = new_ch - win->ch;
    win->cw = new_cw;
    win->ch = new_ch;

    if(delta_x || delta_y){

        int16_t width = win->cw * win->char_width;
        int16_t height = win->ch * (win->char_height + win->char_depth);
        int16_t common_w = MIN(width, width  - delta_x * win->char_width);
        int16_t common_h = MIN(height, height - delta_y * (win->char_height + win->char_depth)) ;

        xcb_pixmap_t pid = xcb_generate_id(con->con);
        xcb_create_pixmap(con->con, TRUE_COLOR_ALPHA_DEPTH, pid, win->wid, width, height);
        xcb_render_picture_t pic = xcb_generate_id(con->con);
        uint32_t mask3 = XCB_RENDER_CP_GRAPHICS_EXPOSURE | XCB_RENDER_CP_POLY_EDGE | XCB_RENDER_CP_POLY_MODE;
        uint32_t values3[3] = { 0, XCB_RENDER_POLY_EDGE_SMOOTH, XCB_RENDER_POLY_MODE_IMPRECISE };
        xcb_render_create_picture(con->con, pic, pid, con->pfargb, mask3, values3);

        xcb_render_composite(con->con, XCB_RENDER_PICT_OP_OVER, win->pic, 0, pic, 0, 0, 0, 0, 0, 0, common_w, common_h);

        SWAP(xcb_pixmap_t, win->pid, pid);
        SWAP(xcb_render_picture_t, win->pic, pic);
        xcb_free_pixmap(con->con, pid);
        xcb_render_free_picture(con->con, pic);

        nss_rect_t rectv[2];
        size_t rectc= 0;

        if(delta_y > 0)
            rectv[rectc++] = (nss_rect_t){ 0, win->ch - delta_y, MIN(win->cw, win->cw - delta_x), delta_y };
        if(delta_x > 0)
            rectv[rectc++] = (nss_rect_t){ win->cw - delta_x, 0, delta_x, MAX(win->ch, win->ch - delta_y) };

        nss_term_resize(win->term, win->cw, win->ch);

        for(size_t i = 0; i < rectc; i++)
            nss_term_redraw(win->term, rectv[i]);
        nss_window_update(con, win, rectc, rectv);
    }

    if(redraw_borders){ //Update borders
        int16_t width = win->cw * win->char_width + win->left_border;
        int16_t height = win->ch * (win->char_height + win->char_depth) + win->top_border;
        redraw_damage(con,win, (nss_rect_t){width, 0, win->width - width, win->height});
        redraw_damage(con,win, (nss_rect_t){0, height, width, win->height - height});
        //TIP: May be redraw all borders here
    }

}

static void handle_focus(nss_context_t *con, nss_window_t *win, _Bool focused){
    win->focused = focused;
    nss_term_focus(win->term, focused);
}

//TODO: Periodially update window
// => Send XCB_EXPOSE from another thread

/* Start window logic, handling all windows in context */
void nss_context_run(nss_context_t *con){

    for(nss_window_t *win = con->first; win; win = win->next){
        xcb_map_window(con->con,win->wid);
        nss_term_redraw(win->term, (nss_rect_t){0,0,win->cw,win->ch});
    }

    xcb_flush(con->con);

    xcb_generic_event_t *event;
    while((event = xcb_wait_for_event(con->con))){
        switch(event->response_type &= 0x7f){
        case XCB_EXPOSE:{
            xcb_expose_event_t *ev = (xcb_expose_event_t*)event;
            nss_window_t *win = nss_window_for_xid(con,ev->window);

            nss_rect_t damage = {ev->x, ev->y, ev->width, ev->height};
            info("Damage: %d %d %d %d", damage.x, damage.y, damage.width, damage.height);

            redraw_damage(con,win, damage);
            xcb_flush(con->con);
            break;
        }
        case XCB_CONFIGURE_NOTIFY:{
            xcb_configure_notify_event_t *ev = (xcb_configure_notify_event_t*)event;
            nss_window_t *win = nss_window_for_xid(con, ev->window);

            if(ev->width != win->width || ev->height != win->height){
                handle_resize(con,win, ev->width, ev->height);
                xcb_flush(con->con);
            }
            break;
        }
        case XCB_KEY_RELEASE:{
            xcb_key_release_event_t *ev = (xcb_key_release_event_t*)event;
            nss_window_t *win = nss_window_for_xid(con, ev->event);
            xcb_key_but_mask_t mask = ev->state;
            xcb_keysym_t keysym = xcb_key_symbols_get_keysym(con->keysyms, ev->detail, mask);
            if(keysym == XK_a){
                uint32_t arg = win->font_size + 2;
                nss_window_set(con, win, nss_wc_font_size, &arg);
            }
            if(keysym == XK_d){
                uint32_t arg = win->font_size - 2;
                nss_window_set(con, win, nss_wc_font_size, &arg);
            }
            if(keysym == XK_w){
                uint32_t arg = !win->lcd_mode;
                nss_window_set(con, win, nss_wc_lcd_mode, &arg);
            }

            break;
        }
        case XCB_FOCUS_IN:
        case XCB_FOCUS_OUT:{
            xcb_focus_in_event_t *ev = (xcb_focus_in_event_t*)event;
            nss_window_t *win = nss_window_for_xid(con,ev->event);
            handle_focus(con,win,event->response_type == XCB_FOCUS_IN);
            xcb_flush(con->con);
            break;
        }
        case XCB_MAP_NOTIFY:
        case XCB_UNMAP_NOTIFY:
           break;
        default:
            warn("Unknown xcb event type: %02"PRIu8, event->response_type);
            break;
        }
        free(event);
    }
}

