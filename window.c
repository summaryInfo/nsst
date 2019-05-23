#include "window.h"

#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>

#include <xcb/xcb.h>

void nss_context_init(nss_context_t* con){
    con->refs = 0;
    con->first = NULL;
}

void nss_context_acquire(nss_context_t *con, nss_window_t *win){
    win->next = con->first;
    win->prev = NULL;
    if(con->first) con->first->prev = win;
    con->first = win;
    
	if(con->refs++ == 0){
    	con->con = xcb_connect(NULL,NULL);
    	con->screen = xcb_setup_roots_iterator(xcb_get_setup(con->con)).data;

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
    win->wid = xcb_generate_id(con->con);

    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[2] = {
        con->screen->black_pixel, 
        XCB_EVENT_MASK_EXPOSURE
    }; 

    //uint8_t depth = 32;
    uint8_t depth = XCB_COPY_FROM_PARENT;
    
    xcb_create_window(con->con, depth,
                      win->wid, con->screen->root,
                      geo->x,geo->y, geo->w,geo->h, 1,/*x,y, w,h, border*/
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      con->screen->root_visual,
                      mask,values);

    win->pid = xcb_generate_id(con->con);
    xcb_create_pixmap(con->con, depth, win->pid, win->wid, geo->w, geo->h);

	win->gc = xcb_generate_id(con->con);
	uint32_t mask2 = XCB_GC_FOREGROUND | XCB_GC_GRAPHICS_EXPOSURES;
	uint32_t values2[2] = {
    	con->screen->white_pixel, 
    	0
    };
    
    xcb_point_t polyline[] = {
        {50, 10},
        { 5, 20},     /* rest of points are relative */
        {25,-20},
        {10, 10}};
	xcb_create_gc(con->con,win->gc,win->pid,mask2, values2);
	xcb_poly_line(con->con,XCB_COORD_MODE_PREVIOUS,win->pid, win->gc, 4, polyline);
	xcb_flush(con->con);

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
