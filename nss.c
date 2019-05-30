#define _POSIX_C_SOURCE 200809L

#include "window.h"
#include "util.h"

/* TODO:
	event handling
	add window config changing support
	terminal logic
	add frontend selection
*/

int main(int argc, char **argv){
    nss_context_t *con = nss_win_create();
    //nss_font_t *font = nss_create_font("DejaVu Sans Mono-13", nss_win_get_dpi(con));
    //nss_font_t *font = nss_create_font("Iosevka-13", nss_win_get_dpi(con));
    char *fname= argc >= 2 ? argv[1] : "Iosevka-13";//, Material Design Icons";
    nss_font_t *font = nss_create_font(fname, 0, nss_win_get_dpi(con));
	nss_win_add_window(con,&(nss_geometry_t){100,100,400,200}, font);
	//nss_win_add_window(con,&(nss_geometry_t){200,200,800,500}, &conf);
	nss_win_run(con);
    nss_win_free(con);
	info("Finished");
    nss_free_font(font);
    return 0;
}
