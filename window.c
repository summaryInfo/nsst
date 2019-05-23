#include "window.h"

#include <stddef.h>
#include <stdint.h>

#include <xcb/xcb.h>

void nss_context_init(nss_context_t* con){
	con->con = xcb_connect(NULL,NULL);
	con->screen = xcb_setup_roots_iterator(xcb_get_setup(con->con)).data;

	xcb_drawable_t root = con->screen->root;
	con->gc = xcb_generate_id(con->con);
	uint32_t mask = XCB_GC_FOREGROUND | XCB_GC_GRAPHICS_EXPOSURES;
	uint32_t values[2] = {
    	con->screen->white_pixel, 
    	0
    };

	xcb_create_gc(con->con,con->gc,root,mask, values);
}

void nss_context_free(nss_context_t* con){
    xcb_free_gc(con->con,con->gc);
    xcb_disconnect(con->con);
    con->con = NULL;
    con->screen = NULL;
}

void nss_window_init(nss_context_t* con, nss_window_t* win, nss_geometry_t *geo){
    win->wid = xcb_generate_id(con->con);

    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[2] = {
        con->screen->black_pixel, 
        XCB_EVENT_MASK_EXPOSURE
    }; 

    xcb_create_window(con->con, XCB_COPY_FROM_PARENT,
                      win->wid, con->screen->root,
                      geo->x,geo->y, geo->w,geo->h, 1,/*x,y, w,h, border*/
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      con->screen->root_visual,
                      mask,values);
}

void nss_window_free(nss_context_t* con, nss_window_t* win){
	xcb_unmap_window(con->con,win->wid);
	xcb_destroy_window(con->con,win->wid);
};
