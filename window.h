#ifndef WINDOW_H_ 
#define WINDOW_H_ 1

#include <stddef.h>
#include <xcb/xcb.h>


typedef struct nss_window {
    xcb_window_t wid;
    xcb_pixmap_t pid;
	xcb_gcontext_t gc;
    struct nss_window *prev, *next;
} nss_window_t;

typedef struct nss_context {
    size_t refs;
	xcb_connection_t* con;
	xcb_screen_t* screen;
    xcb_colormap_t mid;
	xcb_visualtype_t* vis;
	nss_window_t *first;
} nss_context_t;

typedef struct nss_geometry {
	uint32_t x,y;
	uint32_t w,h; 
} nss_geometry_t;

void nss_context_init(nss_context_t* con);
nss_window_t* nss_window_add(nss_context_t* con, nss_geometry_t* geo);
void nss_window_remove(nss_context_t* con, nss_window_t* win);
void nss_context_free(nss_context_t* con);
void nss_main_loop(nss_context_t* con);

#endif
