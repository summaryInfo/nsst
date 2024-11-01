/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

#include "feature.h"

#include "config.h"
#include "feature.h"
#include "input.h"
#include "poller.h"
#include "tty.h"
#include "util.h"
#include "window.h"

#include <fcntl.h>
#include <inttypes.h>
#include <locale.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static _Noreturn void usage(const char *argv0, int code) {
    char buffer[MAX_OPTION_DESC+1];
    if (gconfig.log_level > 0 || code == EXIT_SUCCESS) {
        ssize_t i = 0;
        do fputs(argv0, stdout);
        while ((argv0 = usage_string(buffer, i++)));
    }
    exit(code);
}

static _Noreturn void version(void) {
    printf("%sFeatures: %s", version_string(), features_string());
    exit(EXIT_SUCCESS);
}

static void parse_options(struct instance_config *cfg, char **argv) {
    struct option *opt = NULL, *config_path_entry = find_short_option_entry('C');
    size_t arg_i = 1;
    char *name_end;

    for (const char *arg, *name; argv[arg_i] && argv[arg_i][0] == '-'; arg_i += !!argv[arg_i]) {
        switch (argv[arg_i][1]) {
        case '\0': /* Invalid syntax */;
            usage(argv[0], EXIT_FAILURE);
        case '-': /* Long options */;
            /* End of flags mark */
            if (!*(name = argv[arg_i] + 2)) {
                arg_i++;
                goto finish;
            }

            /* Options without arguments */
            if (!strcmp(name, "help"))
                usage(argv[0], EXIT_SUCCESS);
            if (!strcmp(name, "version"))
                version();

            /* Options with arguments */
            if ((arg = name_end = strchr(name, '=')))
                *name_end = '\0', arg++;

            if (!strncmp(name, "no-", 3) && is_boolean_option(opt = find_option_entry(name + 3, false)))
                arg = "false";
            else if (!(opt = find_option_entry(name, true)))
                usage(argv[0], EXIT_FAILURE);

            if (is_boolean_option(opt)) {
                if (!arg) arg = "true";
            } else {
                if (!arg || !*arg)
                    arg = argv[++arg_i];
            }

            if (!arg || (opt != config_path_entry && !set_option_entry(cfg, opt, arg, 1)))
                usage(argv[0], EXIT_FAILURE);
            continue;
        }

        /* Short options, may be clustered  */
        for (size_t char_i = 1; argv[arg_i] && argv[arg_i][char_i]; char_i++) {
            char letter = argv[arg_i][char_i];
            /* Handle options without arguments */
            switch (letter) {
            case 'e':
                /* Works the same way as -- */
                if (argv[arg_i++][char_i + 1])
                    usage(argv[0], EXIT_FAILURE);
                goto finish;
            case 'h':
                usage(argv[0], EXIT_SUCCESS);
            case 'v':
                version();
            }

            /* Handle options with arguments (including implicit arguments) */
            if ((opt = find_short_option_entry(letter))) {
                if (!is_boolean_option(opt)) {
                    if (!argv[arg_i][++char_i]) arg_i++, char_i = 0;
                    if (!argv[arg_i]) usage(argv[0], EXIT_FAILURE);
                    arg = argv[arg_i] + char_i;
                } else {
                    arg = "true";
                }

                /* Config path option should be ignored, since it is set before */
                if (opt != config_path_entry)
                    if (!set_option_entry(cfg, opt, arg, 1))
                        usage(argv[0], EXIT_FAILURE);

                if (is_boolean_option(opt)) continue;
                else break;
            }
        }
    }

    if (argv[arg_i]) {
finish:
        if (!argv[arg_i])
            usage(argv[0], EXIT_FAILURE);
        cfg->argv = &argv[arg_i];
    }

    /* Parse all shortcuts */
    keyboard_parse_config(cfg);
}

static inline char *parse_config_path(int argc, char **argv) {
    char *config_path = NULL;

    for (int opt_i = 1; opt_i < argc; opt_i++) {
        if (strncmp(argv[opt_i], "--config=", sizeof "--config=" - 1) &&
                strncmp(argv[opt_i], "-C", sizeof "-C" - 1)) continue;

        char *arg = argv[opt_i] + (argv[opt_i][1] == '-' ? sizeof "--config=" : sizeof "-C") - 1;
        if (!*arg) arg = argv[++opt_i];
        if (!arg) usage(argv[0], EXIT_FAILURE);
        config_path = arg;
    }

    return config_path;
}

struct instance_config global_instance_config;

static void free_instance_config_at_exit(void) {
    free_config(&global_instance_config);
}

int main(int argc, char **argv) {
    int result = EXIT_SUCCESS;

    /* Load locale from environment variable */
    setlocale(LC_CTYPE, "");

    /* Parse config path argument before parsing config file
     * to use correct one. This path is used as a default one later. */
    char *cpath = parse_config_path(argc, argv);

    init_options(cpath);
    atexit(free_options);

    init_instance_config(&global_instance_config, cpath, true);
    atexit(free_instance_config_at_exit);

    parse_options(&global_instance_config, argv);

#if USE_URI
    init_proto_tree();
    atexit(uri_release_memory);
#endif
    init_poller();
    atexit(free_poller);

    init_context(&global_instance_config);
    atexit(free_context);

    init_default_termios();

    if (gconfig.daemon_mode) {
        atexit(free_daemon);
        result = !init_daemon();
    } else {
        result = !create_window(&global_instance_config);
    }

    if (!result) poller_run();
    return result;
}
