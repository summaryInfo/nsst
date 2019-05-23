#include "window.h"
#include "util.h"

int main(int argc, char **argv){
    nss_context_t con;
	nss_context_init(&con);
	nss_window_add(&con,&(nss_geometry_t){100,100,400,200});
	nss_window_add(&con,&(nss_geometry_t){200,200,800,500});
	nss_main_loop(&con);
    nss_context_free(&con);
    return 0;
}
