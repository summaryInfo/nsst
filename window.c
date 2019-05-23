#include "window.h"
#include "util.h"

#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>

#include <xcb/xcb.h>

static void assert_void_cookie(nss_context_t *con, xcb_void_cookie_t ck, const char* msg){
    xcb_generic_error_t *err = xcb_request_check(con->con,ck);
	if(err){
    	uint32_t c = err->error_code, 
    	         ma = err->major_code, 
    	         mi = err->minor_code;
    	nss_context_free(con);
    	die("%s: %"PRIu32" %"PRIu32" %"PRIu32,msg,ma,mi,c);
	}
}

static nss_window_t* nss_window_for_xid(nss_context_t* con, xcb_window_t xid){
    for(nss_window_t *win = con->first; win; win = win->next)
        if(win->wid == xid) return win;
    warn("Unknown window 0x%08x",xid);
    return NULL;
}

void nss_context_init(nss_context_t* con){
    con->refs = 0;
    con->first = NULL;
    con->con = NULL;
    con->screen = NULL;
    con->vis = NULL;
}

#define TRUE_COLOR_ALPHA_DEPTH 32

void nss_context_acquire(nss_context_t *con, nss_window_t *win){
    win->next = con->first;
    win->prev = NULL;
    if(con->first) con->first->prev = win;
    con->first = win;
    
	if(con->refs++ == 0){
    	int screenp;
    	con->con = xcb_connect(NULL, &screenp);
    	xcb_screen_iterator_t sit = xcb_setup_roots_iterator(xcb_get_setup(con->con));
    	for(; sit.rem; xcb_screen_next(&sit))
        	if(screenp-- == 0)break;
    	if(screenp != -1) die("Can't find default screen"); 
    	con->screen = sit.data;
    	
        xcb_depth_iterator_t dit = xcb_screen_allowed_depths_iterator(con->screen);
    	for(; dit.rem; xcb_depth_next(&dit))
        	if(dit.data->depth == TRUE_COLOR_ALPHA_DEPTH) break;
    	if(dit.data->depth != TRUE_COLOR_ALPHA_DEPTH) 
        	die("Can't get 32-bit visual");

    	xcb_visualtype_iterator_t vit = xcb_depth_visuals_iterator(dit.data);
    	for(; vit.rem; xcb_visualtype_next(&vit))
        	if(vit.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR) break;
    	if(vit.data->_class != XCB_VISUAL_CLASS_TRUE_COLOR)
        	die("Can't get 32-bit visual");
    	
    	con->vis = vit.data;

    	con->mid = xcb_generate_id(con->con);
        xcb_void_cookie_t c = xcb_create_colormap_checked(con->con, XCB_COLORMAP_ALLOC_NONE,
                                           con->mid, con->screen->root, con->vis->visual_id);	
    	assert_void_cookie(con,c,"Can't create colormap");

	}
}
void nss_context_release(nss_context_t* con, nss_window_t* win){
    if(win->next)win->next->prev = win->prev;
    if(win->prev)win->prev->next = win->next;
    else con->first =  win->next;

	if(--con->refs == 0){
        xcb_disconnect(con->con);
        con->con = NULL;
        con->screen = NULL;
	}
}

nss_window_t* nss_window_add(nss_context_t* con, nss_geometry_t *geo){
    nss_window_t* win = malloc(sizeof(nss_window_t));
    nss_context_acquire(con, win);

    uint8_t depth = 32;
    uint32_t mask1 = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
    uint32_t values1[4] = { 0xff000000, 0xff000000 , XCB_EVENT_MASK_EXPOSURE, con->mid }; 
    xcb_void_cookie_t c;

    win->wid = xcb_generate_id(con->con);
    c = xcb_create_window_checked(con->con, depth, win->wid, con->screen->root, 
                                /*x,     y,      w,     h,      border*/ 
                                  geo->x,geo->y, geo->w,geo->h, 1,
                                  XCB_WINDOW_CLASS_INPUT_OUTPUT, 
                                  con->vis->visual_id, mask1,values1);
	assert_void_cookie(con,c,"Can't create window");

    win->pid = xcb_generate_id(con->con);
    c = xcb_create_pixmap_checked(con->con, depth, win->pid, win->wid, geo->w, geo->h);
	assert_void_cookie(con,c,"Can't create pixmap");

	uint32_t mask2 = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_GRAPHICS_EXPOSURES;
	uint32_t values2[3] = { con->screen->white_pixel, con->screen->black_pixel, 0 };

	win->gc = xcb_generate_id(con->con);
	c = xcb_create_gc_checked(con->con,win->gc,win->pid,mask2, values2);
	assert_void_cookie(con,c,"Can't create GC");
    
    return win;
}

void nss_window_remove(nss_context_t* con, nss_window_t* win){
	xcb_unmap_window(con->con,win->wid);
    xcb_free_gc(con->con,win->gc);
	xcb_free_pixmap(con->con,win->pid);
	xcb_destroy_window(con->con,win->wid);
	nss_context_release(con, win);
	free(win);
};


void nss_context_free(nss_context_t* con){
	while(con->first)
    	nss_window_remove(con,con->first);
}


void nss_main_loop(nss_context_t *con){
	nss_window_t *win = con->first;
    nss_geometry_t geo = {
		.x = 0, .y = 0, 
		.w = 320, .h = 200
    };

    for(nss_window_t *win = con->first; win; win = win->next)
		xcb_map_window(con->con,win->wid);

    xcb_point_t points[] = {{10, 10}, {10, 20}, {20, 10}, {20, 20}};
    xcb_point_t polyline[] = {{50, 10}, { 5, 20}, {25,-20}, {10, 10}};
	xcb_rectangle_t rect = { 0,0,geo.w,geo.h };

	xcb_change_gc_checked(con->con,win->gc,XCB_GC_FOREGROUND,&(uint32_t[]){0xff0000ff});
	xcb_poly_fill_rectangle_checked(con->con,win->pid,win->gc,1,&rect);
	xcb_change_gc_checked(con->con,win->gc,XCB_GC_FOREGROUND,&(uint32_t[]){con->screen->white_pixel});
	xcb_poly_line_checked(con->con,XCB_COORD_MODE_PREVIOUS,win->pid, win->gc, 4, polyline);
	
	xcb_flush(con->con);

    xcb_generic_event_t* event;
    while((event = xcb_wait_for_event(con->con))){
        switch(event->response_type & 0x7f){
        case XCB_EXPOSE:{
			xcb_expose_event_t *ev = (xcb_expose_event_t*)event;
            nss_window_t *win = nss_window_for_xid(con,ev->window);

			xcb_copy_area(con->con,win->pid,ev->window,win->gc, 0,0,0,0,geo.w,geo.h);
    		xcb_poly_point (con->con, XCB_COORD_MODE_ORIGIN, ev->window, win->gc, 4, points);
        	xcb_flush(con->con);
			break;
        }
        case XCB_CONFIGURE_NOTIFY:{
			//xcb_configure_notify_event_t *cnotify = (xcb_configure_notify_event_t*)event;
			break;
        }
        default:
            warn("Unknown xcb event type: 0x%02hhx", event->response_type);
        	break;
        }
		free(event);
    }
}
