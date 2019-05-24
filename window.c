#define _POSIX_C_SOURCE 200809L

#include "window.h"
#include "util.h"

#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>

#include <xcb/xcb.h>

#define TRUE_COLOR_ALPHA_DEPTH 32

static void assert_void_cookie(nss_context_t *con, xcb_void_cookie_t ck, const char* msg){
    xcb_generic_error_t *err = xcb_request_check(con->con,ck);
	if(err){
    	uint32_t c = err->error_code, 
    	         ma = err->major_code, 
    	         mi = err->minor_code;
    	nss_win_free_windows(con);
    	die("%s: %"PRIu32" %"PRIu32" %"PRIu32,msg,ma,mi,c);
	}
}

static nss_window_t* nss_window_for_xid(nss_context_t* con, xcb_window_t xid){
    for(nss_window_t *win = con->first; win; win = win->next)
        if(win->wid == xid) return win;
    warn("Unknown window 0x%08x",xid);
    return NULL;
}

static void acquire_context(nss_context_t *con, nss_window_t *win){
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

static void release_context(nss_context_t* con, nss_window_t* win){
    if(win->next)win->next->prev = win->prev;
    if(win->prev)win->prev->next = win->next;
    else con->first =  win->next;

	if(--con->refs == 0){
        xcb_disconnect(con->con);
        con->con = NULL;
        con->screen = NULL;
	}
}

void nss_win_initialize(nss_context_t* con){
    con->refs = 0;
    con->first = NULL;
    con->con = NULL;
    con->screen = NULL;
    con->vis = NULL;
    con->start_time = time(NULL);
}

nss_window_t* nss_win_add_window(nss_context_t* con, nss_geometry_t *geo){
    nss_window_t* win = malloc(sizeof(nss_window_t));
    acquire_context(con, win);

    uint8_t depth = 32;
    uint32_t mask1 = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
    uint32_t values1[4] = { 0xff000000, 0xff000000 , XCB_EVENT_MASK_EXPOSURE | 
    	XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_FOCUS_CHANGE | 
    	XCB_EVENT_MASK_STRUCTURE_NOTIFY /*| XCB_EVENT_MASK_RESIZE_REDIRECT*/, con->mid}; 
    xcb_void_cookie_t c;

    win->w = geo->w;
    win->h = geo->h;

    win->wid = xcb_generate_id(con->con);
    c = xcb_create_window_checked(con->con, depth, win->wid, con->screen->root, 
                                /*x,     y,      w,     h,      border*/ 
                                  geo->x,geo->y, geo->w,geo->h, 1,
                                  XCB_WINDOW_CLASS_INPUT_OUTPUT, 
                                  con->vis->visual_id, mask1,values1);
	assert_void_cookie(con,c,"Can't create window");

    win->pid = xcb_generate_id(con->con);
    c = xcb_create_pixmap_checked(con->con, depth, win->pid, win->wid, win->w, win->h);
	assert_void_cookie(con,c,"Can't create pixmap");

	uint32_t mask2 = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_GRAPHICS_EXPOSURES;
	uint32_t values2[3] = { 0xff000000, 0xff000000, 0 };

	win->gc = xcb_generate_id(con->con);
	c = xcb_create_gc_checked(con->con,win->gc,win->pid,mask2, values2);
	assert_void_cookie(con,c,"Can't create GC");

	xcb_rectangle_t rect = { 0,0,win->w,win->h };
	xcb_poly_fill_rectangle(con->con,win->pid,win->gc,1,&rect);
    
    return win;
}

void nss_win_remove_window(nss_context_t* con, nss_window_t* win){
	xcb_unmap_window(con->con,win->wid);
    xcb_free_gc(con->con,win->gc);
	xcb_free_pixmap(con->con,win->pid);
	xcb_destroy_window(con->con,win->wid);
	release_context(con, win);
	free(win);
};

void nss_win_free_windows(nss_context_t* con){
	while(con->first)
    	nss_win_remove_window(con,con->first);
}


void nss_win_run(nss_context_t *con){

    for(nss_window_t *win = con->first; win; win = win->next)
		xcb_map_window(con->con,win->wid);

	nss_window_t *win = con->first;
	xcb_change_gc(con->con,win->gc,XCB_GC_FOREGROUND,&(uint32_t[]){0x12345678});
    xcb_point_t polyline[] = {{50, 10}, { 5, 20}, {25,-20}, {10, 10}};
	xcb_poly_line(con->con,XCB_COORD_MODE_PREVIOUS,win->pid, win->gc, 4, polyline);

	xcb_flush(con->con);

    xcb_generic_event_t* event;
    while((event = xcb_wait_for_event(con->con))){
        switch(event->response_type &= 0x7f){
        case XCB_EXPOSE:{
            //_Bool cursor_blink_state = ((con->start_time - time(NULL))/win->conf->blink_interval) & 1;
            //win->draw_callback(con,win);
			xcb_expose_event_t *ev = (xcb_expose_event_t*)event;
            nss_window_t *win = nss_window_for_xid(con,ev->window);

			xcb_copy_area(con->con,win->pid,win->wid,win->gc, 0,0,0,0, win->w, win->h);
        	xcb_flush(con->con);
			break;
        }
        case XCB_CONFIGURE_NOTIFY:{
			xcb_configure_notify_event_t *ev = (xcb_configure_notify_event_t*)event;
			nss_window_t *win = nss_window_for_xid(con, ev->window);
			if(ev->width != win->w || ev->height != win->h){
				//Handle resize
				xcb_pixmap_t new = xcb_generate_id(con->con), old = win->pid;
				xcb_create_pixmap(con->con, TRUE_COLOR_ALPHA_DEPTH, new, win->wid, ev->width, ev->height);

            	xcb_rectangle_t rectv[2];
            	size_t rectc= 0;
            	if(ev->height > win->h){
                	size_t minw = win->w < ev->width ? win->w : ev->width;
                	rectv[rectc++] = (xcb_rectangle_t){
            			.x = 0, 
            			.y = win->h, 
            			.width = minw, 
            			.height = ev->height - win->h
            		};
    			}
            	if(ev->width > win->w){
                	size_t maxh = win->h > ev->height ? win->h : ev->height;
                	rectv[rectc++] = (xcb_rectangle_t){
            			.x = win->w, 
            			.y = 0, 
            			.width = ev->width - win->w, 
            			.height = maxh,
                	};
            	}
    				
				xcb_poly_fill_rectangle(con->con, new, win->gc, rectc, rectv);
				xcb_copy_area(con->con, old, new, win->gc, 0, 0, 0, 0, win->w, win->h);
				win->pid = new;
				win->w = ev->width;
				win->h = ev->height;
				xcb_free_pixmap(con->con, old);
				xcb_flush(con->con);
				//win->resize_callback(con,win);
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
