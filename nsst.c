#define _POSIX_C_SOURCE 200809L

#include "window.h"
#include "util.h"

void usage(char *argv0) {
    die("%s [-c class ] [-a] [-f font] [-o dump_file] [-[tT] title] [-v] [-e] [command [args]]", argv0);
}

int main(int argc, char **argv) {

#define GETARG {\
        if (!argv[ind][++cind]) ind++, cind = 0;\
        if (!argv[ind]) usage(argv[0]);\
        arg = argv[ind] + cind;}
    nss_init_context();

    char *arg;

    size_t ind = 1;
    while (argv[ind] && argv[ind][0] == '-') {
        size_t cind = 0;
        if (!argv[ind][1] || (argv[ind][1] == '-' && !argv[ind][2])) break;
        while (argv[ind] && argv[ind][++cind]) {
            switch(argv[ind][cind]) {
            case 'c':
                GETARG;
                nss_config_set_string(NSS_SCONFIG_TERM_CLASS, arg);
                goto next;
            case 'a':
                nss_config_set_integer(NSS_ICONFIG_ALLOW_ALTSCREEN, 0);
                break;
            case 'f':
                GETARG;
                nss_config_set_string(NSS_SCONFIG_FONT_NAME, arg);
                goto next;
            case 'o':
                GETARG;
                nss_config_set_string(NSS_SCONFIG_PRINTER, arg);
                goto next;
            case 't':
            case 'T':
                GETARG;
                nss_config_set_string(NSS_SCONFIG_TITLE, arg);
                goto next;
            case 'e':
                if (!argv[++ind]) usage(argv[0]);
                goto do_run;
            case 'v':
                usage(argv[0]);
            default:
                warn("Unknown option: -%c", argv[ind][cind]);
            }
        }
next:
        if (argv[ind]) ind++;
    }
do_run:
    if (argv[ind]) {
        nss_config_set_argv((const char**)argv + ind);
    }

    nss_create_window(NULL, 0, NULL);
    nss_context_run();
    nss_free_context();
    return 0;
}
