#define _POSIX_C_SOURCE 200809L

#include "window.h"
#include "util.h"

int main(int argc, char **argv) {
    nss_init_context();
    nss_create_window(NULL, 0, NULL);
    nss_context_run();
    info("Top level");
    nss_free_context();
    return 0;
}
