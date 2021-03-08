/* Copyright (c) 2019-2021, Evgeny Baskov. All rights reserved */

#define _POSIX_C_SOURCE 200809L

#include "feature.h"

#if USE_URI

#include "config.h"
#include "uri.h"

#include <errno.h>
#include <assert.h>
#include <ctype.h>
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

static struct uri_table table;

/* From window.c */
enum uri_match_result uri_match_next(struct uri_match_state *stt, uint8_t ch) {
#define MATCH(tab, ch) (!!((tab)[((ch) >> 5) - 1] & (1U << ((ch) & 0x1F))))
    static uint32_t c_proto[] = {0x03FF6000, 0x07FFFFFE, 0x07FFFFFE}; // [\w\d\-.]
    static uint32_t c_ext[] = {0xAFFFFFD2, 0x87FFFFFF, 0x47FFFFFE}; // [\w\d\-._~!$&'()*+,;=:@/?]

    /* This code does not handle fancy unicode URIs that
     * can be displayed by browsers, but only strictly complying
     * that includes only subset of ASCII */

    if (UNLIKELY(!adjust_buffer((void **)&stt->data, &stt->caps, stt->size + 1, 1))) {
        uri_match_reset(stt);
        return urim_ground;
    }

    // Only ASCII graphical characters are accepted
    if (ch - 0x21U > 0x5DU) goto finish_nak;

    stt->data[stt->size++] = ch;

    switch(stt->state) {
    case uris1_ground:
        if (isalpha(ch)) {
            stt->state++;
            return (stt->res = urim_need_more);
        }
        break;
    case uris1_proto:
        if (MATCH(c_proto, ch)) {
            return urim_need_more;
        } else if (ch == ':') {
            stt->state++;
            return (stt->res = urim_need_more);
        }
        break;
    case uris1_slash1:
    case uris1_slash2:
        if (ch == '/') {
            stt->state++;
            return (stt->res = urim_need_more);
        }
        break;
    case uris1_user:
        if (ch == '@') {
            stt->state = uris1_host;
            return (stt->res = urim_need_more);
        } else //fallthrough
    case uris1_host:
        if (ch == ':') {
            stt->state = uris1_port;
            return (stt->res = urim_need_more);
        } else //fallthrough
    case uris1_path:
        if (ch == '/') {
            stt->state = uris1_path;
            return (stt->res = urim_may_finish);
        } else if (ch == '?') {
            stt->state = uris1_query;
            return (stt->res = urim_may_finish);
        } else //fallthrough
    case uris1_query:
        if (ch == '#') {
            stt->state = uris1_fragment;
            return (stt->res = urim_may_finish);
        } else //fallthrough
    case uris1_fragment:
        if (ch == '%') {
            stt->saved = stt->state;
            stt->state = uris1_p_hex1;
            return (stt->res = urim_need_more);
        } else if (MATCH(c_ext, ch)) {
            return (stt->res = urim_may_finish);
        }
        break;
    case uris1_port:
        if (ch == '/') {
            stt->state = uris1_path;
            return (stt->res = urim_may_finish);
        } else if (ch == '?') {
            stt->state = uris1_query;
            return (stt->res = urim_may_finish);
        } else if (ch == '#') {
            stt->state = uris1_fragment;
            return (stt->res = urim_may_finish);
        } else if (isdigit(ch)) {
            return (stt->res = urim_may_finish);
        }
        break;
    case uris1_p_hex1:
        if (isxdigit(ch)) {
            stt->state++;
            return (stt->res = urim_need_more);
        }
        break;
    case uris1_p_hex2:
        if (isxdigit(ch)) {
            stt->state = stt->saved;
            return (stt->res = urim_may_finish);
        }
        break;
    }

    if (stt->data && stt->size) {
        // Last character was not part of URL,
        // remove it (buffer should be cleared by outer code)
        stt->data[--stt->size] = '\0';
    }

finish_nak:
    stt->state = uris1_ground;
    return (stt->res = stt->res == urim_may_finish ? urim_finished : urim_ground);
#undef MATCH
}

void uri_match_reset(struct uri_match_state *state) {
    free(state->data);
    *state = (struct uri_match_state){0};
}

char *uri_match_move(struct uri_match_state *state) {
    char *res = state->data;
    if (res) res[state->size] = '\0';
    *state = (struct uri_match_state){0};
    return res;
}

bool is_vaild_uri(const char *uri) {
    if (!uri) return 0;

    struct uri_match_state stt = {0};
    enum uri_match_result res = urim_ground;
    while (*uri) {
        res = uri_match_next(&stt, *uri++);
        if (res == urim_finished ||
            res == urim_ground) break;
    }

    uri_match_reset(&stt);
    return !*uri && res == urim_may_finish;
}

/* We prefix internally generated IDs with BEL
 * since this character cannot appear in supplied string
 * (it terminates OSC sequence)*/
#define URI_ID_PREF "\007"

#define MAX_NUMBER_LEN 10

#define URI_CAPS_STEP(x) ((x)?(4*(x)/3):8)

/* If URI is invalid returns EMPTY_URI */
uint32_t uri_add(char *uri, const char *id) {
    static size_t id_counter = 0;

    if (!is_vaild_uri(uri)) {
        if (*uri) warn("URI '%s' is invalid", uri);
        return EMPTY_URI;
    }

    char *id_s = NULL;
    if (id) id_s = strdup(id);
    else if ((id_s = malloc(MAX_NUMBER_LEN + 2))) {
        snprintf(id_s, MAX_NUMBER_LEN + 2,
                 URI_ID_PREF"%0*zx", MAX_NUMBER_LEN, id_counter++);
    }
    if (!id_s) goto alloc_failed;

    assert(!table.size || table.uris);

    for (size_t i = 0; i < table.size; i++) {
        if (table.uris[i].uri && !strcmp(table.uris[i].uri, uri) &&
                !strcmp(table.uris[i].id, id_s)) {
            free(id_s);
            uri_ref(i + 1);
            return i + 1;
        }
    }

    struct uri *new = NULL;
    if (table.first_free) {
        /* We have available free slots in the pool */
        new = &table.uris[table.first_free - 1];
        table.first_free = new->next;
    } else {
        /* Need to allocate new */

        if (table.size + 1 > table.caps) {
            struct uri *tmp = realloc(table.uris, URI_CAPS_STEP(table.caps)*sizeof(*tmp));
            if (!tmp) goto alloc_failed;
            table.uris = tmp;
            table.caps = URI_CAPS_STEP(table.caps);
        }

        new = &table.uris[table.size++];
    }

    *new = (struct uri) {
        .refc = 1,
        .uri = uri,
        .id = id_s,
        .next = 0,
    };

    uint32_t uriid = new - table.uris + 1;
    if (gconfig.trace_misc) {
        if (id) info("URI new id=%d path='%s' name='%s'", uriid, uri, id);
        else info("URI new id=%d path='%s' name=%zd (privite)", uriid, uri, id_counter);
    }

    /* External ID is actually index + 1, not index*/
    return uriid;

alloc_failed:
    free(uri);
    free(id_s);
    return EMPTY_URI;
}

void uri_ref(uint32_t id) {
    if (id) table.uris[id - 1].refc++;
}

void uri_unref(uint32_t id) {
    struct uri *uri = &table.uris[id - 1];
    if (id && !--uri->refc) {
        if (gconfig.trace_misc) {
            warn("URI free %d", id);
        }
        free(uri->uri);
        free(uri->id);

        uri->uri = NULL;
        uri->id = NULL;

        /* Not actually free,
         * just add to free list */
        uri->next = table.first_free;
        table.first_free = id;
    }
}

void uri_open(uint32_t id) {
    if (gconfig.trace_misc) {
        info("URI open cmd='%s' id=%d path='%s'",
                gconfig.open_command, id, id ? table.uris[id - 1].uri : "");
    }
    if (id && !fork()) {
        execlp(gconfig.open_command,
               gconfig.open_command,
               table.uris[id - 1].uri, NULL);
        _exit(127);
    }
}

const char *uri_get(uint32_t id) {
    return id ? table.uris[id - 1].uri : "";
}

#endif
