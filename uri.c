/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

#include "feature.h"

#if USE_URI

#include "config.h"
#include "hashtable.h"
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

/* We prefix internally generated IDs with BEL
 * since this character cannot appear in supplied string
 * (it terminates OSC sequence)*/
#define URI_ID_PREF '\007'

#define MAX_NUMBER_LEN 6

#define URI_CAPS_STEP(x) ((x)?(4*(x)/3):8)
#define URI_HASHTAB_CAPS_STEP(x) ((x)?3*(x)/2:16)

/* URI table entry */
struct uri {
    ht_head_t head;
    /* Number of attributes referencing this URI.
     * It is decremented during attributes optimization
     * (when attribute gets deleted) or line deletion
     * and incremented on adding an attribute to the line */
    int64_t refc;
    uintptr_t slot;
    /* Associated ID */
    char *id;
    /* URI string itself */
    char *uri;
};

struct id_table {
    size_t size;
    size_t caps;
    uintptr_t first_free;
    struct slot {
        struct uri *uri;
        uintptr_t next;
    } *slots;
};

#define mkurikey(_id, idlen, _uri, urilen) ((const struct uri){ .head = (ht_head_t){ .hash = hash64(_id, idlen) ^ hash64(_uri, urilen) }, .id = _id, .uri = _uri})

static struct id_table idtab;
static hashtable_t uritab;

/* From window.c */
enum uri_match_result uri_match_next(struct uri_match_state *stt, uint8_t ch) {
#define MATCH(tab, ch) (!!((tab)[((ch) >> 5) - 1] & (1U << ((ch) & 0x1F))))
    static uint32_t c_proto[] = {0x03FF6000, 0x07FFFFFE, 0x07FFFFFE}; // [\w\d\-.]
    static uint32_t c_ext[] = {0xAFFFFFD2, 0x87FFFFFF, 0x47FFFFFE}; // [\w\d\-._~!$&'()*+,;=:@/?]

    /* This code does not handle fancy unicode URIs that
     * can be displayed by browsers, but only strictly complying
     * that includes only subset of ASCII */

    // Only ASCII graphical characters are accepted
    if (ch - 0x21U > 0x5DU) goto finish_nak;

    if (!stt->no_copy) {
        if (UNLIKELY(!adjust_buffer((void **)&stt->data, &stt->caps, stt->size + 1, 1))) {
            uri_match_reset(stt);
            return urim_ground;
        }
        stt->data[stt->size] = ch;
    }
    stt->size++;

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

    if (stt->size) {
        stt->size--;
        if (stt->data) {
            // Last character was not part of URL,
            // remove it (buffer should be cleared by outer code)
            stt->data[stt->size] = '\0';
        }
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

inline static size_t vaild_uri_len(const char *uri) {
    if (!uri) return 0;

    struct uri_match_state stt = {.no_copy = 1};
    enum uri_match_result res = urim_ground;
    while (*uri) {
        res = uri_match_next(&stt, *uri++);
        if (res == urim_finished ||
            res == urim_ground) break;
    }

    if (!*uri && res == urim_may_finish) return stt.size;
    return 0;
}

inline static struct slot *alloc_slot(void) {
    if (idtab.first_free != EMPTY_URI) {
        struct slot *slot = &idtab.slots[idtab.first_free - 1];
        idtab.first_free = slot->next;
        return slot;
    } else {
        if (idtab.size + 1 > idtab.caps) {
            size_t new_caps = URI_CAPS_STEP(idtab.caps);
            struct slot *tmp = realloc(idtab.slots, new_caps*sizeof(*tmp));
            if (!tmp)  return NULL;
            idtab.slots = tmp;
            idtab.caps = new_caps;
        }
        return &idtab.slots[idtab.size++];
    }
}

inline static void free_slot(struct slot *slot) {
    slot->next = idtab.first_free;
    idtab.first_free = slot - idtab.slots + 1;
}

HOT
static bool uri_cmp(const ht_head_t *a, const ht_head_t *b) {
    const struct uri *ua = (const struct uri *)a;
    const struct uri *ub = (const struct uri *)b;
    return !strcmp(ua->id, ub->id) && !strcmp(ua->uri, ub->uri);
}

/* If URI is invalid returns EMPTY_URI */
uint32_t uri_add(const char *uri, const char *id) {
    static size_t id_counter = 0;

    if (!idtab.slots) ht_init(&uritab, HT_INIT_CAPS, uri_cmp);

    size_t uri_len = vaild_uri_len(uri), id_len = 0;
    if (!uri_len) {
        if (*uri) warn("URI '%s' is invalid", uri);
        return EMPTY_URI;
    }

    /* Generate internal identifier
     * if not explicitly provided */
    char buf[MAX_NUMBER_LEN + 2];
    buf[0] = 0;
    if (LIKELY(!id)) {
        if (gconfig.unique_uris) {
            id = buf;
            buf[id_len++] = URI_ID_PREF;
            /* Convert private id to string (non-human readable) */
            uint32_t idn = id_counter++;
            do buf[id_len++] = ' ' + (idn & 63);
            while (idn >>= 6);
            buf[id_len] = '\0';
        } else id = "";
    } else id_len = strlen(id);


    /* First, lookup URI in hash table */
    const struct uri dummy = mkurikey((char *)id, id_len, (char *)uri, uri_len);
    ht_head_t **h = ht_lookup_ptr(&uritab, (ht_head_t *)&dummy);
    if (*h) {
        struct uri *new = (struct uri *)*h;
        uri_ref(new->slot);
        return new->slot;
    }

    /* Allocate URI hash table node */

    struct uri *new = malloc(sizeof *new + uri_len + id_len + 2);
    if (!new) goto alloc_failed;

    /* Allocate id table slot */
    struct slot *slot = alloc_slot();
    if (!new) goto alloc_failed;

    slot->uri = new;
    *new = (struct uri) {
        .uri = (char *)(new + 1),
        .id = (char *)(new + 1) + uri_len + 1,
        .head = dummy.head,
        .slot = slot - idtab.slots + 1,
        .refc = 1,
    };

    memcpy(new->uri, uri, uri_len + 1);
    memcpy(new->id, id, id_len + 1);

    ht_insert_hint(&uritab, h, (ht_head_t *)new);

    if (gconfig.trace_misc) {
        if (!buf[0]) info("URI new id=%zd path='%s' name='%s'", new->slot, uri, id);
        else info("URI new id=%zd path='%s' name=%zd (privite)", new->slot, uri, id_counter);
    }

    /* External ID is actually index + 1, not index*/
    return new->slot;

alloc_failed:
    free(new);
    return EMPTY_URI;
}

void uri_ref(uint32_t id) {
    if (id) {
        struct uri *uri = idtab.slots[id - 1].uri;
        assert(uri->refc > 0);
        uri->refc++;
    }
}

void uri_unref(uint32_t id) {
    if (!id) return;
    struct slot *slot = &idtab.slots[id - 1];
    struct uri *uri = slot->uri;
    assert(uri->refc > 0);
    if (!--uri->refc) {
        if (gconfig.trace_misc)
            info("URI free %d", id);

        ht_erase(&uritab, (ht_head_t *)uri);
        free_slot(slot);
        free(uri);

        /* Shrink hash table */
        if (3*uritab.size/4 > HT_INIT_CAPS &&
                uritab.size < uritab.caps/2) {
            ht_shrink(&uritab, uritab.caps/2);
        }
    }
}

void uri_open(uint32_t id) {
    if (gconfig.trace_misc) {
        info("URI open cmd='%s' id=%d path='%s'",
                gconfig.open_command, id, uri_get(id));
    }
    if (id && !fork()) {
        execlp(gconfig.open_command,
               gconfig.open_command,
               uri_get(id), NULL);
        _exit(127);
    }
}

const char *uri_get(uint32_t id) {
    return id ? idtab.slots[id - 1].uri->uri : "";
}

void uri_release_memory(void) {
    if (uritab.data) {
        ht_iter_t it = ht_begin(&uritab);
        while (ht_current(&it))
            free(ht_erase_current(&it));
        ht_free(&uritab);
    }
    free(idtab.slots);

    memset(&idtab, 0, sizeof(idtab));
    memset(&uritab, 0, sizeof(uritab));
}

#endif
