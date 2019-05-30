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

#include "window.h"
#include "util.h"
#include "font.h"

#define TRUE_COLOR_ALPHA_DEPTH 32
#define TRUE_COLOR_DEPTH 24
#define ASCII_MAX 128
#define ASCII_PRINTABLE_HIGH 127
#define ASCII_PRINTABLE_LOW 32
#define CHARS_PER_MESG (254/4)
#define BLINK_TIME (CLOCKS_PER_SEC/4)
#define BG_ALPHA 0xffff

#define CR(c) (((c) & 0xff) * 0x101)
#define CG(c) ((((c) >> 8) & 0xff) * 0x101)
#define CB(c) ((((c) >> 16) & 0xff) * 0x101)
#define CA(c) ((((c) >> 24) & 0xff) * 0x101)
#define MAKE_COLOR(c) {.red=CR(c),.green=CG(c),.blue=CB(c),.alpha=CA(c)}

#define MIN(x,y) ((x) < (y) ? (x) : (y))
#define MAX(x,y) ((x) > (y) ? (x) : (y))
#define SWAP(T,a,b) {T tmp_ = a; a = b; b = tmp_; }

typedef struct nss_term nss_term_t;
typedef xcb_rectangle_t nss_rect_t;

struct nss_window {
    xcb_window_t wid;
    xcb_pixmap_t pid;
    xcb_gcontext_t gc;
    xcb_render_picture_t pic;

    int16_t w,h;
    int16_t cw,ch;
    int16_t char_width;
    int16_t char_depth;
    int16_t char_height;
    int16_t border_width;
    int16_t cursor_width;
    _Bool lcd_mode;

    nss_color_t background;
    nss_color_t foreground;
    nss_color_t cursor_background;
    nss_color_t cursor_foreground;
    nss_cursor_type_t cursor_type;

    _Bool focused;

    /*
     * Glyph encoding:
     *  0xTTUUUUUU, where
     *      * 0xTT - font fase
     *      * 0xUUUUUU - unicode character
     */
    xcb_render_glyphset_t gsid;
    xcb_render_pictformat_t pfglyph;
    nss_font_t *font;
    nss_term_t *term;
    struct nss_window *prev, *next;
};


struct nss_context {
    size_t refs;
    xcb_connection_t* con;
    xcb_screen_t* screen;
    xcb_colormap_t mid;
    xcb_visualtype_t* vis;

    xcb_render_pictformat_t pfargb;
    xcb_render_pictformat_t pfalpha;

    nss_window_t *first;
    clock_t start_time;
};

typedef struct nss_glyph_mesg {
    uint8_t len;
    uint8_t pad[3];
    int16_t dx, dy;
    uint8_t data[];
} nss_glyph_mesg_t;



static _Bool intersect_with(nss_rect_t *src, nss_rect_t *dst){
        nss_rect_t inters = {
            MAX(src->x, dst->x),
            MAX(src->y, dst->y),
            MIN(src->x + src->width, dst->x + dst->width),
            MIN(src->y + src->height, dst->y + dst->height),
        };
        if(inters.width <= inters.x || inters.height <= inters.y){
            *src = (xcb_rectangle_t){0,0,0,0};
            return 0;
        } else {
            inters.width -= inters.x;
            inters.height -= inters.y;
            *src = inters;
            return 1;
        }
}

inline static nss_rect_t rect_scale_up(nss_rect_t rect, int16_t x_factor, int16_t y_factor){
    rect.x *= x_factor;
    rect.y *= y_factor;
    rect.width *= x_factor;
    rect.height *= y_factor;
    return rect;
}
inline static nss_rect_t rect_scale_down(nss_rect_t rect, int16_t x_factor, int16_t y_factor){
    rect.x /= x_factor;
    rect.y /= y_factor;
    rect.width /= x_factor;
    rect.height /= y_factor;
    return rect;
}
inline static nss_rect_t rect_shift(nss_rect_t rect, int16_t x_off, int16_t y_off){
    rect.x += x_off;
    rect.y += y_off;
    return rect;
}
inline static nss_rect_t rect_resize(nss_rect_t rect, int16_t x_off, int16_t y_off){
    rect.x += x_off;
    rect.y += y_off;
    return rect;
}

struct nss_term {
    int16_t cursor_x;
    int16_t cursor_y;
    void(*callback_redraw_damage)(nss_context_t *con,nss_window_t *win, nss_term_t *term, nss_rect_t damage);
    void(*callback_initialize)(nss_context_t *con,nss_window_t *win, nss_term_t *term);
    void(*callback_free)(nss_context_t *con,nss_window_t *win, nss_term_t *term);
    void(*callback_get_cursor)(nss_context_t *con,nss_window_t *win, nss_term_t *term, int16_t *cursor_x, int16_t *cursor_y);

    uint32_t *ch_attr;
    uint32_t *ch_symb;
    
    nss_text_attrib_t *attr;
    nss_rect_t clip;
};

void callback_get_cursor(nss_context_t *con,nss_window_t *win, nss_term_t *term, int16_t *cursor_x, int16_t *cursor_y){
    if(cursor_x) *cursor_x = term->cursor_x;
    if(cursor_y) *cursor_y = term->cursor_y;
}

void callback_redraw_damage(nss_context_t *con,nss_window_t *win, nss_term_t *term, nss_rect_t damage){
    //TODO: Better handle groups of same attrib
    //      Preprocess region
    //           Group lines
    //           Group same font foreground
    //           Group background squares

    // Separate array of chars and attribute indeces

    // Write something like nss_win_multi_draw_text
    //    - Gets a set of pattern strings with some attributes 
    //         nss_text_attrib_t *attr;
    //         size_t length;
    //         uint32_t *string;

    
    if(intersect_with(&damage,&term->clip)){
        for(size_t j = damage.y; j < damage.y + damage.height; j++){
            size_t index, count = 0;
            for(size_t i = damage.x; i <= damage.x + damage.width; i++){
                index = j*term->clip.width+i;
                if((i > damage.x && term->ch_attr[index-1] != term->ch_attr[index]) || 
                    i == damage.x + damage.width){
                    nss_text_attrib_t cattr = term->attr[term->ch_attr[index - count]];
                    nss_win_render_ucs4(con, win, count, &term->ch_symb[index - count], &cattr, i-count, j);
					count = 0;
                }
                else count++;
            }
        }
    }
}

void callback_resize(nss_context_t *con,nss_window_t *win, nss_term_t *term){
}

void callback_initialize(nss_context_t *con,nss_window_t *win, nss_term_t *term){
    term->cursor_x = 15;
    term->cursor_y = 4;
    term->clip = (nss_rect_t){0,0,127-33,5};
    term->ch_attr = calloc(term->clip.width*term->clip.height,sizeof(uint32_t));
    term->ch_symb = calloc(term->clip.width*term->clip.height,sizeof(uint32_t));
    term->attr = malloc(5*sizeof(nss_text_attrib_t));

    term->attr[1-1] = (nss_text_attrib_t){ .fg = 0xffffffff, .bg = 0xff005500, .flags = nss_attrib_italic | nss_attrib_bold };
    term->attr[2-1] = (nss_text_attrib_t){ .fg = 0xffffffff, .bg = 0xff000000, .flags = nss_attrib_italic | nss_attrib_underlined };
    term->attr[3-1] = (nss_text_attrib_t){ .fg = 0xffffffff, .bg = 0xff000000, .flags = nss_attrib_strikethrough };
    term->attr[4-1] = (nss_text_attrib_t){ .fg = 0xffffffff, .bg = 0xff000000, .flags = nss_attrib_underlined | nss_attrib_inverse };
    term->attr[5-1] = (nss_text_attrib_t){ .fg = 0xffffffff, .bg = 0xff000000, .flags = 0 };

    for(size_t k = 0; k < 5; k++)
        for(size_t i = 33; i < 127; i++){
            term->ch_attr[k*term->clip.width+(i-33)] = k;
            term->ch_symb[k*term->clip.width+(i-33)] = i;
        }

    term->ch_symb[2*term->clip.width+17] = L'';
}

void callback_free(nss_context_t *con,nss_window_t *win, nss_term_t *term){
    free(term->ch_attr);
    free(term->ch_symb);
    free(term->attr);
}

nss_term_t* nss_make_term(void){
    nss_term_t *term = malloc(sizeof(nss_term_t));
    term->callback_free = callback_free;
    term->callback_initialize = callback_initialize;
    term->callback_redraw_damage = callback_redraw_damage;
    term->callback_get_cursor = callback_get_cursor;
    return term;
}






static void assert_void_cookie(nss_context_t *con, xcb_void_cookie_t ck, const char* msg){
    xcb_generic_error_t *err = xcb_request_check(con->con,ck);
    if(err){
        uint32_t c = err->error_code,
        ma = err->major_code,
        mi = err->minor_code;
        nss_win_free(con);
        die("%s: %"PRIu32" %"PRIu32" %"PRIu32,msg,ma,mi,c);
    }
}

static nss_window_t* nss_window_for_xid(nss_context_t* con, xcb_window_t xid){
    for(nss_window_t *win = con->first; win; win = win->next)
        if(win->wid == xid) return win;
    warn("Unknown window 0x%08x",xid);
    return NULL;
}

nss_context_t* nss_win_create(void){
    nss_context_t *con = calloc(1, sizeof(*con));
    con->start_time = clock();

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

    return con;
}

void nss_win_free(nss_context_t *con){
        while(con->first)
            nss_win_remove_window(con,con->first);
        xcb_disconnect(con->con);
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

nss_window_t* nss_win_add_window(nss_context_t* con, nss_geometry_t *geo, nss_font_t *font){
    nss_window_t* win = malloc(sizeof(nss_window_t));
    win->next = con->first;
    win->prev = NULL;
    if(con->first) con->first->prev = win;
    con->first = win;

    win->font = font;
    win->border_width = 8;
    win->cursor_width = 2;
    win->background = 0xff000000;
    win->foreground = 0xffffffff;
    win->cursor_background = 0xff000000;
    win->cursor_foreground = 0xffffffff;
    win->cursor_type = nss_cursor_bar;
    win->w = geo->w;
    win->h = geo->h;
    win->lcd_mode = 0;

    win->pfglyph = win->lcd_mode ? con->pfargb : con->pfalpha;

    //TODO: Choose gravity depending on padding centering XCB_GRAVITY_CENTER
    uint8_t depth = 32;
    uint32_t mask1 = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_BIT_GRAVITY | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
    uint32_t values1[5] = { win->background, win->background, XCB_GRAVITY_NORTH_WEST,  XCB_EVENT_MASK_EXPOSURE |
                           XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_FOCUS_CHANGE |
                           XCB_EVENT_MASK_STRUCTURE_NOTIFY /*| XCB_EVENT_MASK_RESIZE_REDIRECT*/, con->mid};
    xcb_void_cookie_t c;

    win->wid = xcb_generate_id(con->con);
    c = xcb_create_window_checked(con->con, depth, win->wid, con->screen->root,
                                  /*x,     y,      w,     h,      border*/
                                  geo->x,geo->y, geo->w,geo->h, 1,
                                  XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                  con->vis->visual_id, mask1,values1);
    assert_void_cookie(con,c,"Can't create window");

    /* TODO: Deduplicate glyphsets between windows */
    win->gsid = xcb_generate_id(con->con);
    xcb_render_create_glyph_set(con->con, win->gsid, win->pfglyph);

    //Preload ASCII
    int16_t total = 0, maxd = 0, maxh = 0;
    for(uint32_t i = ASCII_PRINTABLE_LOW; i < ASCII_PRINTABLE_HIGH; i++){
        nss_glyph_t *glyphs[nss_font_attrib_max];
        for(size_t j = 0; j < nss_font_attrib_max; j++){
            glyphs[j] = nss_font_render_glyph(font, i, j, win->lcd_mode);
            register_glyph(con,win,i | (j << 24),glyphs[j]);
        }

        total += glyphs[0]->x_off + glyphs[0]->x;
        maxd = MAX(maxd, glyphs[0]->height - glyphs[0]->y);
        maxh = MAX(maxh, glyphs[0]->y);

        for(size_t j = 0; j < nss_font_attrib_max; j++)
            free(glyphs[j]);
    }

    win->char_width = (total+3*(ASCII_PRINTABLE_HIGH-ASCII_PRINTABLE_LOW)/2) /
        (ASCII_PRINTABLE_HIGH-ASCII_PRINTABLE_LOW);
    win->char_height = maxh;
    win->char_depth = maxd;
    win->cw = (win->w - 2*win->border_width) / win->char_width;
    win->ch = (win->h - 2*win->border_width) / (win->char_height + win->char_depth);
    win->cw = MAX(win->cw, 1);
    win->ch = MAX(win->ch, 1);

    xcb_rectangle_t rect = { 0, 0, win->cw*win->char_width, win->ch*(win->char_depth+win->char_height) };

    win->pid = xcb_generate_id(con->con);
    c = xcb_create_pixmap_checked(con->con, depth, win->pid, win->wid, rect.width, rect.height );
    assert_void_cookie(con,c,"Can't create pixmap");

    uint32_t mask2 = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_GRAPHICS_EXPOSURES;
    uint32_t values2[3] = { win->background, win->background, 0 };

    win->gc = xcb_generate_id(con->con);
    c = xcb_create_gc_checked(con->con,win->gc,win->pid,mask2, values2);
    assert_void_cookie(con,c,"Can't create GC");

    uint32_t mask3 = XCB_RENDER_CP_GRAPHICS_EXPOSURE | XCB_RENDER_CP_POLY_EDGE | XCB_RENDER_CP_POLY_MODE;
    uint32_t values3[3] = { 0, XCB_RENDER_POLY_EDGE_SMOOTH, XCB_RENDER_POLY_MODE_IMPRECISE };

    win->pic = xcb_generate_id(con->con);
    c = xcb_render_create_picture_checked(con->con,win->pic, win->pid, con->pfargb, mask3, values3);
    assert_void_cookie(con,c,"Can't create XRender picture");

    xcb_render_color_t color = MAKE_COLOR(win->background);
    xcb_render_fill_rectangles(con->con, XCB_RENDER_PICT_OP_OVER, win->pic, color, 1, &rect);

    info("Font size: %d %d %d", win->char_height, win->char_depth, win->char_width);

    win->term = nss_make_term();
    win->term->callback_initialize(con, win, win->term);

    return win;
}

void nss_win_remove_window(nss_context_t* con, nss_window_t* win){
    win->term->callback_free(con,win,win->term);
    free(win->term);

    xcb_unmap_window(con->con,win->wid);
    xcb_render_free_picture(con->con,win->pic);
    xcb_free_gc(con->con,win->gc);
    xcb_free_pixmap(con->con,win->pid);
    xcb_render_free_glyph_set(con->con,win->gsid);
    xcb_destroy_window(con->con,win->wid);


    if(win->next)win->next->prev = win->prev;
    if(win->prev)win->prev->next = win->next;
    else con->first =  win->next;
    free(win);
};

static void update_window_damage(nss_context_t *con, nss_window_t *win, size_t len, xcb_rectangle_t *rects){
    for(size_t i = 0; i < len; i++){
        const xcb_expose_event_t ev = {
            .response_type = XCB_EXPOSE,
            .sequence = 0, .window = win->wid,
            .x = rects[i].x, .y = rects[i].y,
            .width = rects[i].width, .height = rects[i].height,
            .count = 1,
        };
        xcb_send_event(con->con, 0, win->wid, XCB_EVENT_MASK_EXPOSURE, (const char *)&ev);
    }
}

uint16_t nss_win_get_dpi(nss_context_t *con){
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
    for(; it.rem; xcb_screen_next(&it)){
        if(it.data){
            long new = (it.data->width_in_pixels * 25.4)/it.data->width_in_millimeters;
            dpi = MAX(dpi, new);
        }
    }
    if(!dpi){
        warn("Can't get highest dpi, defaulting to 96");
        dpi = 96;
    }
    return dpi;
}

static xcb_render_picture_t create_pen(nss_context_t *con, nss_window_t *win, xcb_render_color_t *color){
    xcb_pixmap_t pid = xcb_generate_id(con->con);
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

void nss_win_render_ucs4(nss_context_t* con, nss_window_t* win, size_t len,  uint32_t* str,
                         nss_text_attrib_t *attr, uint16_t x, uint16_t y){
    x = x * win->char_width;
    y = y * (win->char_height + win->char_depth) + win->char_height;

    nss_color_t fgc = attr->fg, bgc = attr->bg;
    nss_font_attrib_t fattr = attr->flags & nss_font_attrib_mask;
    _Bool blink_state = ((clock() - con->start_time)/BLINK_TIME) & 1;

    if(attr->flags & nss_attrib_inverse)
        SWAP(nss_color_t, fgc, bgc);
    if((attr->flags & nss_attrib_blink) && blink_state)
        fgc = bgc;
    if(attr->flags & nss_attrib_cursor && win->cursor_type == nss_cursor_block)
        fgc = win->cursor_foreground, bgc = win->cursor_background;

    xcb_render_color_t fg = { CR(fgc), CG(fgc), CB(fgc), CA(fgc) };
    xcb_render_color_t bg = { CR(bgc), CG(bgc), CB(bgc), CA(bgc) };

    if(attr->flags & nss_attrib_background)
        bg.alpha = BG_ALPHA;

    xcb_rectangle_t rect = { .x = x, .y = y-win->char_height,
                        .width = win->char_width*len, .height = win->char_height+win->char_depth  };
    xcb_rectangle_t lines[2] = {
        { .x = x, .y= y + 1, .width = win->char_width*len, .height = 1 },
        { .x = x, .y= y - win->char_height/3, .width = win->char_width*len, .height = 1 }
    };
    xcb_render_picture_t pen = create_pen(con, win, &fg);
    xcb_void_cookie_t c;

    //TODO: Set bound region?
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

    xcb_void_cookie_t *cookies = malloc(sizeof(xcb_void_cookie_t)*len);
    for(size_t i = 0; i < len; i++){
        if(str[i] > ASCII_MAX){
            nss_glyph_t *glyph = nss_font_render_glyph(win->font, str[i], fattr, win->lcd_mode);
            register_glyph(con,win,str[i] | (fattr << 24), glyph);
            free(glyph);
        }
    }

    free(cookies);

    c = xcb_render_fill_rectangles(con->con, XCB_RENDER_PICT_OP_OVER, win->pic, bg, 1, &rect);
    assert_void_cookie(con, c, "Can't render filled rectangles");

    if(attr->flags & (nss_attrib_underlined|nss_attrib_strikethrough)){
        size_t count = !!(attr->flags & nss_attrib_underlined) + !!(attr->flags & nss_attrib_strikethrough);
        size_t offset = !(attr->flags & nss_attrib_underlined) && !!(attr->flags & nss_attrib_strikethrough);
        c = xcb_render_fill_rectangles(con->con, XCB_RENDER_PICT_OP_OVER, win->pic, fg, count, lines+offset);
        assert_void_cookie(con, c, "Can't render filled rectangles");
    }

    c = xcb_render_composite_glyphs_32_checked(con->con, XCB_RENDER_PICT_OP_OVER,
                                   pen, win->pic, win->pfglyph, win->gsid,
                                   0, 0, data_len, (uint8_t*)data);
    assert_void_cookie(con, c, "Can't render text");

    if(attr->flags & nss_attrib_cursor)
        draw_cursor(con,win,x,y);

    free(data);
    xcb_render_free_picture(con->con, pen);
}

static void redraw_damage(nss_context_t *con, nss_window_t *win, nss_rect_t damage){

        int16_t width = win->cw * win->char_width;
        int16_t height = win->ch * (win->char_height + win->char_depth);

        xcb_rectangle_t damaged[4], borders[4] = {
            {0, 0, win->border_width, win->h},
            {win->border_width, 0, width, win->border_width},
            {win->border_width, height + win->border_width, width, win->h - height - win->border_width},
            {width + win->border_width, 0, win->w - width - win->border_width, win->h},
        };
        size_t num_damaged = 0;
        for(size_t i = 0; i < sizeof(borders)/sizeof(borders[0]); i++)
            if(intersect_with(&borders[i],&damage))
                    damaged[num_damaged++] = borders[i];
        if(num_damaged)
            xcb_poly_fill_rectangle(con->con, win->wid, win->gc, num_damaged, damaged);

        xcb_rectangle_t inters = { win->border_width, win->border_width, width + win->border_width, height + win->border_width };
        if(intersect_with(&inters, &damage))
            xcb_copy_area(con->con,win->pid,win->wid,win->gc, inters.x - win->border_width, inters.y - win->border_width,
                          inters.x, inters.y, inters.width, inters.height);
}

//TODO: Periodially update window
// => Send XCB_EXPOSE from another thread

void nss_win_run(nss_context_t *con){

    for(nss_window_t *win = con->first; win; win = win->next){
        xcb_map_window(con->con,win->wid);
        win->term->callback_redraw_damage(con,win,win->term,(nss_rect_t){0,0,win->cw,win->ch});
    }

    xcb_flush(con->con);

    xcb_generic_event_t* event;
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

            _Bool redraw_borders = ev->width < win->w || ev->height < win->h;
            _Bool single_line = 0;

            if(ev->width != win->w || ev->height != win->h){
                //Handle resize

                win->w = ev->width;
                win->h = ev->height;

                int16_t new_cw = MAX(1,(win->w - 2*win->border_width)/win->char_width);
                int16_t new_ch = MAX(1,(win->h - 2*win->border_width)/(win->char_height+win->char_depth));
                int16_t delta_x = new_cw - win->cw;
                int16_t delta_y = new_ch - win->ch;

                single_line |= new_cw == 1 && delta_x == 0;
                single_line |= new_ch == 1 && delta_y == 0;

                if(delta_x || delta_y){

                    int16_t width = new_cw * win->char_width;
                    int16_t height = new_ch * (win->char_height + win->char_depth);
                    int16_t common_w = MIN(win->cw, new_cw) * win->char_width;
                    int16_t common_h = MIN(win->ch, new_ch) * (win->char_height + win->char_depth);
                    win->cw = new_cw;
                    win->ch = new_ch;

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

					for(size_t i = 0; i < rectc; i++)
    					rectv[i] = rect_scale_up(rectv[i], win->char_width, win->char_height+win->char_depth);
    					
                    xcb_render_color_t color = MAKE_COLOR(win->background);
                    xcb_render_fill_rectangles(con->con, XCB_RENDER_PICT_OP_OVER, win->pic, color, rectc, rectv);

					for(size_t i = 0; i < rectc; i++){
                        win->term->callback_redraw_damage(con,win,win->term, 
                                                         rect_scale_down(rectv[i], win->char_width, win->char_height+win->char_depth));
                        redraw_damage(con,win, rect_shift(rectv[i], win->border_width, win->border_width));
					}
                }
                if(single_line){ //Update everything
                    redraw_damage(con,win, (nss_rect_t){0,0,win->w, win->h});
                }
                if(redraw_borders && !single_line){ //Update borders
                    int16_t width = win->cw * win->char_width + win->border_width;
                    int16_t height = win->ch * (win->char_height + win->char_depth) + win->border_width;
                    redraw_damage(con,win, (nss_rect_t){width, 0, win->w - width, win->h});
                    redraw_damage(con,win, (nss_rect_t){0, height, width, win->h - height});
                }
                xcb_flush(con->con);
            }
            break;
        }
        case XCB_KEY_RELEASE:{
            //xcb_key_release_event_t *ev = (xcb_key_release_event_t*)event;
            break;
        }
        case XCB_FOCUS_IN:
        case XCB_FOCUS_OUT:{
            xcb_focus_in_event_t *ev = (xcb_focus_in_event_t*)event;
            nss_window_t *win = nss_window_for_xid(con,ev->event);
            win->focused = event->response_type == XCB_FOCUS_IN;

            nss_rect_t damage = { 0, 0, 1, 1 };
            win->term->callback_get_cursor(con,win,win->term,&damage.x,&damage.y);
            win->term->callback_redraw_damage(con,win,win->term, damage);

            damage = rect_scale_up(damage, win->char_width, win->char_height + win->char_depth);
            xcb_copy_area(con->con,win->pid,win->wid,win->gc, damage.x, damage.y, 
                          damage.x+win->border_width, damage.y+win->border_width, damage.width, damage.height);
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

