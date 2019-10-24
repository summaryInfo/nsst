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
    nss_init_color(0xff000000);
    nss_context_t *con = nss_create_context();
    char *fname= argc >= 2 ? argv[1] : "Iosevka-13";
    nss_create_window(con,(nss_rect_t){100,100,400,200}, fname, 0, NULL);
    nss_context_run(con);
    nss_free_context(con);
    nss_free_color();
    return 0;
}
