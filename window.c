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
    nss_color_t background;
    nss_color_t foreground;

    _Bool focused;

    xcb_render_glyphset_t gsid;
    nss_font_t *font;
    struct nss_window *prev, *next;
};

struct nss_context {
    size_t refs;
    xcb_connection_t* con;
    xcb_screen_t* screen;
    xcb_colormap_t mid;
    xcb_visualtype_t* vis;

    /*
     * Glyph encoding:
     *  0xTTUUUUUU, where 
     *      0xTT - font fase
     *          0 normal
     *          1 normal_italic
     *          2 bold
     *          3 bold_italic
     *          4 faint
     *          5 faint_italic
     *      0xUUUUUU - unicode character
     */
    xcb_render_pictformat_t pfargb;
    xcb_render_pictformat_t pfalpha;

    nss_window_t *first;
    clock_t start_time; 
};

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
    win->background = 0xff000000;
    win->foreground = 0xffffffff;
    win->w = geo->w;
    win->h = geo->h;

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
    xcb_render_create_glyph_set(con->con, win->gsid, con->pfalpha);

	//Preload ASCII
    int16_t total = 0, maxd = 0, maxh = 0;
    for(uint32_t i = ASCII_PRINTABLE_LOW; i < ASCII_PRINTABLE_HIGH; i++){
        nss_glyph_t *glyphs[nss_font_attrib_max];
        for(size_t j = 0; j < nss_font_attrib_max; j++){
            glyphs[j] = nss_font_render_glyph(font, i, j);
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

    return win;
}

void nss_win_remove_window(nss_context_t* con, nss_window_t* win){
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

void nss_update_window_damage(nss_context_t *con, nss_window_t *win, size_t len, xcb_rectangle_t *rects){
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

typedef struct nss_glyph_mesg {
    uint8_t len;
    uint8_t pad[3];
    int16_t dx, dy;
    uint8_t data[];
} nss_glyph_mesg_t;

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
            nss_glyph_t *glyph = nss_font_render_glyph(win->font, str[i], fattr);
            register_glyph(con,win,str[i] | (fattr << 24), glyph);
            /*
            for(size_t i = 0; i < glyph->height; i++){
                for(size_t j = 0; j < glyph->width; j++)
                    fprintf(stderr,"%02x",glyph->data[glyph->stride*i+j]);
                putc('\n',stderr);
            }
            */
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
                                   pen, win->pic, con->pfalpha, win->gsid,
                                   0, 0, data_len, (uint8_t*)data);
    assert_void_cookie(con, c, "Can't render text");

    free(data);
    xcb_render_free_picture(con->con, pen);
}


static _Bool intersect_with(xcb_rectangle_t *src, xcb_rectangle_t *dst){
        xcb_rectangle_t inters = {
			MAX(src->x, dst->x),
			MAX(src->y, dst->y),
			MAX(src->x + src->width, dst->x + dst->width),
			MAX(src->y = src->height, dst->y + dst->height),
        };
        inters.width -= inters.x;
        inters.height -= inters.y;
        if(inters.width < 0 || inters.height < 0){
            *src = (xcb_rectangle_t){0,0,0,0};
            return 0;
        } else {
            *src = inters;
            return 1;
        }
}

//TODO: Periodially update window
// => Send XCB_EXPOSE from another thread

void nss_win_run(nss_context_t *con){

    for(nss_window_t *win = con->first; win; win = win->next)
        xcb_map_window(con->con,win->wid);

    xcb_flush(con->con);

    xcb_generic_event_t* event;
    while((event = xcb_wait_for_event(con->con))){
        switch(event->response_type &= 0x7f){
        case XCB_EXPOSE:{
            xcb_expose_event_t *ev = (xcb_expose_event_t*)event;
            nss_window_t *win = nss_window_for_xid(con,ev->window);

            //TODO: Build border rectangles considering damage
            info("Damage: %d %d %d %d", ev->x, ev->y, ev->width, ev->height);

            int16_t width = win->cw * win->char_width; 
            info("CHeight: %d %d", win->cw, win->ch);
            int16_t height = win->ch * (win->char_height + win->char_depth);
            info("Height: %d %d %d %d", width, height, win->w, win->h);

            xcb_rectangle_t damage_rect = {ev->x, ev->y, ev->width, ev->height};
            xcb_rectangle_t damaged[4], borders[4] = {
                {0, 0, win->border_width, win->h},
                {win->border_width, 0, width, win->border_width},
                {win->border_width, height + win->border_width, width, win->h - height - win->border_width},
                {width + win->border_width, 0, win->w - width - win->border_width, win->h},
            };
            size_t num_damaged = 0;
            for(size_t i = 0; i < sizeof(borders)/sizeof(borders[0]); i++)
                if(intersect_with(&borders[i],&damage_rect))
                        damaged[num_damaged++] = borders[i];
            if(num_damaged)
                xcb_poly_fill_rectangle(con->con, win->wid, win->gc, num_damaged, damaged);

            xcb_rectangle_t inters = { win->border_width, win->border_width, width + win->border_width, height + win->border_width };
            if(intersect_with(&inters, &damage_rect))
                xcb_copy_area(con->con,win->pid,win->wid,win->gc, inters.x - win->border_width, inters.y - win->border_width, 
            	             inters.x, inters.y, inters.width - win->border_width, inters.height - win->border_width);
            xcb_flush(con->con);
            break;
        }
        case XCB_CONFIGURE_NOTIFY:{
            xcb_configure_notify_event_t *ev = (xcb_configure_notify_event_t*)event;
            nss_window_t *win = nss_window_for_xid(con, ev->window);

            _Bool part_update = ev->width < win->w || ev->height < win->h; 
            _Bool full_update = 0;

            if(ev->width != win->w || ev->height != win->h){
                //Handle resize

                win->w = ev->width;
                win->h = ev->height;

                int16_t new_cw = MAX(1,(win->w - 2*win->border_width)/win->char_width);
                int16_t new_ch = MAX(1,(win->h - 2*win->border_width)/(win->char_height+win->char_depth));
                int16_t delta_x = new_cw - win->cw;
                int16_t delta_y = new_ch - win->ch;

				full_update |= new_cw == 1 && delta_x == 0;
				full_update |= new_ch == 1 && delta_y == 0;
                
                if(delta_x || delta_y){
                    full_update = 1;

                    int16_t new_w = new_cw * win->char_width;
                    int16_t new_h = new_ch * (win->char_height + win->char_depth);
                    
                    xcb_pixmap_t new = xcb_generate_id(con->con);
                    xcb_create_pixmap(con->con, TRUE_COLOR_ALPHA_DEPTH, new, win->wid, new_w, new_h);
                    xcb_render_picture_t newp = xcb_generate_id(con->con);
                    uint32_t mask3 = XCB_RENDER_CP_GRAPHICS_EXPOSURE | XCB_RENDER_CP_POLY_EDGE | XCB_RENDER_CP_POLY_MODE;
                    uint32_t values3[3] = { 0, XCB_RENDER_POLY_EDGE_SMOOTH, XCB_RENDER_POLY_MODE_IMPRECISE };
                    xcb_render_create_picture(con->con, newp, new, con->pfargb, mask3, values3);

                    int16_t common_w = MIN(win->cw, new_cw) * win->char_width;
                    int16_t common_h = MIN(win->ch, new_ch) * (win->char_height + win->char_depth);
                    xcb_render_composite(con->con, XCB_RENDER_PICT_OP_OVER, win->pic, 0, newp, 0, 0, 0, 0, 0, 0, common_w, common_h);

                    xcb_pixmap_t old = win->pid;
                    xcb_render_picture_t oldp  = win->pic;
                    win->pid = new;
                    win->pic = newp;
                    win->cw = new_cw;
                    win->ch = new_ch;
                    xcb_free_pixmap(con->con, old);
                    xcb_render_free_picture(con->con, oldp);

        			// TEST CODE {
                    xcb_rectangle_t rectv[2];
                    size_t rectc= 0;
                    if(delta_y > 0){
                        size_t mincw = MIN(win->cw, win->cw - delta_x);
                        rectv[rectc++] = (xcb_rectangle_t){
                            .x = 0,
                            .y = (win->ch - delta_y)*(win->char_height + win->char_depth),
                            .width = mincw*win->char_width,
                            .height = delta_y * (win->char_height + win->char_depth)
                        };
                    }
                    if(delta_x > 0){
                        size_t maxch = MAX(win->ch, win->ch - delta_y);
                        rectv[rectc++] = (xcb_rectangle_t){
                            .x = (win->cw - delta_x)*win->char_width, 
                            .y = 0, 
                            .width = delta_x * win->char_width, 
                            .height = maxch * (win->char_height + win->char_depth),
                        };
                    }
                    xcb_render_color_t color = MAKE_COLOR(win->background);
                    xcb_render_fill_rectangles(con->con, XCB_RENDER_PICT_OP_OVER, newp, color, rectc, rectv);

                    uint32_t test[127-33];
                    for(size_t i = 33; i < 127; i++) test[i-33] = i;

                    nss_text_attrib_t attr = { .fg = 0xffffffff, .bg = 0xff005500, .flags = nss_attrib_italic | nss_attrib_bold };
                    nss_win_render_ucs4(con, win, sizeof(test)/sizeof(test[0]), test, &attr, 0, 0);
                    attr.bg = 0xff000000;
                    attr.flags = nss_attrib_underlined;
                    nss_win_render_ucs4(con, win, sizeof(test)/sizeof(test[0]), test, &attr, 0, 1);
                    attr.flags = nss_attrib_strikethrough;
                    nss_win_render_ucs4(con, win, sizeof(test)/sizeof(test[0]), test, &attr, 0, 2);
                    attr.flags = nss_attrib_underlined | nss_attrib_inverse;
                    nss_win_render_ucs4(con, win, sizeof(test)/sizeof(test[0]), test, &attr, 0, 3);
                    attr.flags = nss_attrib_blink;
                    nss_win_render_ucs4(con, win, sizeof(test)/sizeof(test[0]), test, &attr, 0, 4);
                    attr.flags = 0;
                    uint32_t test2[] = {'F', L'И',' ',L'ч',L'т',L'о','-',L'т',L'о',' ',L'п',L'о','-',L'р',L'у',L'с',L'с',L'к',L'и'};
                    nss_win_render_ucs4(con, win, sizeof(test2)/sizeof(test2[0]), test2, &attr, 0, 5);
                    // } TEST CODE

                    xcb_flush(con->con);
                }
                if(full_update){
					//Update everything
                    nss_update_window_damage(con,win,1,&(xcb_rectangle_t){0,0,win->w,win->h});

                }
                if(part_update && !full_update){ //Update borders
                    int16_t width = win->cw * win->char_width;
                    int16_t height = win->ch*(win->char_height + win->char_depth); 
                    xcb_rectangle_t damage[2] = {
                        {win->border_width + width, 0, win->w - win->border_width - width, win->h},
                        {0, win->border_width + height, win->w, win->h - win->border_width - height}
                    };
                    damage[1].width -= damage[0].width;
                    nss_update_window_damage(con,win,2,damage);
                }
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

