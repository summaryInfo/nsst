/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#define _POSIX_C_SOURCE 200809L

#include "feature.h"

#if USE_URI

#include "config.h"
#include "uri.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* URI table entry */
struct uri {
    /* Number of attributes referencing this URI.
     * It is decremented during attributes optimization
     * (when attribute gets deleted) or line deletion
     * and incremented on adding an attribute to the line */
    uint32_t refc;
    /* URI string itself */
    char *uri;
    /* Assocciated ID */
    char *id;
    /* this field is used for linking
     * free URIs cells in a list conatined in array
     * uri_ids */
    uint32_t next;
};

struct uri_table {
    size_t size;
    size_t caps;
    /* free slot list for faster allocation
     * (stores index + 1, zero means the end) */
    size_t first_free;
    struct uri *uris;
};

struct uri_table table;


enum uri_match_result uri_match_next(struct uri_match_state *state, char ch) {
    // TODO
    return 0;
}

void uri_match_reset(struct uri_match_state *state) {
    // TODO
}

bool is_vaild_uri(const char *uri) {
    struct uri_match_state stt = {};
    enum uri_match_result res = urim_error;
    do {
        res = uri_match_next(&stt, *uri++);
        if (res == urim_finished ||
            res == urim_error) break;
    } while (*uri);

    return !*uri && res == urim_finished;
}

/* We prefix internally generated IDs with BEL
 * since this character cannot appear in supplied string
 * (it terminates OSC sequence)*/
#define URI_ID_PREF "\007"

#define MAX_NUMBER_LEN 10

#define URI_CAPS_STEP(x) ((x)?(4*(x)/3):8)

/* If URI is invalid returns EMPTY_URI */
uint32_t uri_add(const char *uri, const char *id) {
    static size_t id_counter = 0;

    if (!is_vaild_uri(uri)) return EMPTY_URI;

    char *id_s = NULL, *uri_s = NULL;
    if (id) id_s = strdup(id);
    else if ((id_s = malloc(MAX_NUMBER_LEN + 2))) {
        snprintf(id_s, MAX_NUMBER_LEN + 2,
                 URI_ID_PREF"%0*zx", MAX_NUMBER_LEN, id_counter++);
    }
    if (!id_s) goto alloc_failed;

    for (size_t i = 0; i < table.size; i++) {
        if (!strcmp(table.uris[i].uri, uri) && !strcmp(table.uris[i].id, id_s)) {
            free(id_s);
            return i + 1;
        }
    }

    if (!(uri_s = strdup(uri))) goto alloc_failed;

    struct uri *new = NULL;
    if (table.first_free) {
        /* We have available free slots in the pool */
        new = &table.uris[table.first_free - 1];
        table.first_free = new->next;
    } else {
        /* Need to allocate new */

        if (table.size + 1 > table.caps) {
            struct uri *tmp = realloc(table.uris, URI_CAPS_STEP(table.caps)*sizeof(*tmp));
            if (tmp) goto alloc_failed;
            table.uris = tmp;
            table.caps = URI_CAPS_STEP(table.caps);
        }

        new = &table.uris[table.size++];
    }

    *new = (struct uri) {
        .refc = 1,
        .uri = strdup(uri),
        .id = id ? strdup(id) : NULL,
        .next = 0,
    };

    /* External ID is actually index + 1, not index*/
    return new - table.uris + 1;

alloc_failed:
    free(uri_s);
    free(id_s);
    return EMPTY_URI;
}

void uri_ref(uint32_t id) {
    if (id) table.uris[id - 1].refc++;
}

void uri_unref(uint32_t id) {
    struct uri *uri = &table.uris[id - 1];
    if (id && !--uri->refc) {
        free(uri->uri);
        free(uri->id);

        uri->uri = NULL;
        uri->id = NULL;

        /* Not actually free,
         * just add to free list */
        uri->next = table.first_free;
        table.first_free = id;
    }

    assert(id >= 0);
}

void uri_open(uint32_t id) {
    if (id && fork() > 0) {
        execl(gconfig.open_command, gconfig.open_command,
              table.uris[id - 1].uri, NULL);
        _exit(127);
    }
}

const char *uri_get(uint32_t id) {
    return id ? table.uris[id - 1].uri : "";
}

#endif
