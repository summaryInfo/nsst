
#include "util.h"
#include "window.h"
#include <xcb/xcb.h>
#include <stdlib.h>


int main(int argc, char **argv){
    nss_window_t win;
    nss_context_t con;
    nss_geometry_t geo = {
		.x = 0, .y = 0, 
		.w = 320, .h = 200
    };

	nss_context_init(&con);
    nss_window_init(&con, &win, &geo);

	xcb_map_window(con.con, win.wid);
	xcb_flush(con.con);
    xcb_generic_event_t* event;
    while((event = xcb_wait_for_event(con.con))){
        switch(event->response_type & ~0x80){
        case XCB_EXPOSE:{
			//xcb_expose_event_t *expose = (xcb_expose_event_t*)event;

			break;
        }
        default:
            warn("Unknown xcb event type: %hhx", event->response_type);
        	break;
        }
		free(event);
    }
    nss_window_free(&con, &win);
    nss_context_free(&con);
}
