#define _POSIX_C_SOURCE 200809L

#include "window.h"
#include "util.h"

/* TODO:
	font loading
	glyph rendering with attributes
	event handling
	terminal logic
	add backend selection
*/

int main(int argc, char **argv){
    nss_context_t con;
	nss_win_initialize(&con);
	nss_win_add_window(&con,&(nss_geometry_t){100,100,400,200});
	nss_win_add_window(&con,&(nss_geometry_t){200,200,800,500});
	nss_win_run(&con);
    nss_win_free_windows(&con);
    return 0;
}
