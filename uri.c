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

/* We prefix internally generated IDs with BEL
 * since this character cannot appear in supplied string
 * (it terminates OSC sequence)*/
#define URI_ID_PREF '\007'

#define MAX_NUMBER_LEN 6

#define URI_CAPS_STEP(x) ((x)?(4*(x)/3):8)
#define URI_HASHTAB_CAPS_STEP(x) ((x)?3*(x)/2:16)

/* URI table entry */
struct uri {
    /* Number of attributes referencing this URI.
     * It is decremented during attributes optimization
     * (when attribute gets deleted) or line deletion
     * and incremented on adding an attribute to the line */
    int64_t refc;
    /* URI string itself */
    char *uri;
    /* Assocciated ID */
    char *id;
    /* this field is used for linking
     * free URIs cells in a list conatined in array
     * uri_ids */
    uint32_t next;
    /* URI hash */
    uint32_t hash;
};

struct uri_table {
    size_t size;
    size_t count;
    size_t caps;
    /* free slot list for faster allocation
     * (stores index + 1, zero means the end) */
    size_t first_free;
    struct uri *uris;

    // Since URIs should have
    // fixed indices, we need
    // to use separate array for
    // hash table
    // (not using pointer for consistency)
    uint32_t *hash_tab;
    size_t hash_tab_caps;
};

static struct uri_table table;

// Murmur3 32-bit hash
uint32_t hash(const char *data, ssize_t len) {
#define MIX(x) (x *= 0xCC9E2D51, x = (x << 15) | (x >> 17), x *= 0x1B873593)
#define FMIX(x) (x ^= x >> 16, x *= 0x85EBCA6B, x ^= x >> 13, x *= 0xC2B2AE35, x ^= x >> 16)
    const ssize_t nblocks = len / 4;
    const uint32_t *blocks = (const uint32_t *)data + nblocks;

    uint32_t k1 = 0, h1 = 123;
    for(ssize_t i = -nblocks; i; i++) {
        k1 = blocks[i];
        MIX(k1);
        h1 ^= k1;
        k1 = (k1 << 13) | (k1 >> 19);
        h1 = h1*5 + 0xE6546B64;
    }

    const uint8_t * tail = (const uint8_t *)data + (len & ~3LU);
    switch(len & 3) {
    case 3:
        k1 ^= tail[2] << 16;
        // fallthrough
    case 2:
        k1 ^= tail[1] << 8;
        // fallthrough
    case 1:
        k1 ^= tail[0];
        MIX(k1);
        h1 ^= k1;
    };

    h1 ^= len;
    FMIX(h1);
    return h1;
#undef MIX
#undef FMIX
}

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

size_t vaild_uri_len(const char *uri) {
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

static bool realloc_hashtable(size_t new_caps, struct uri *new) {
    uint32_t *newtab = malloc(new_caps * sizeof(*newtab));
    if (!newtab) return 0;

    memset(newtab, 0xFF, new_caps * sizeof(*newtab));
    for (size_t i = 0; i < table.size; i++) {
        if (table.uris[i].uri && new != &table.uris[i]) {
            uint32_t *newidx = &newtab[table.uris[i].hash % new_caps];
            table.uris[i].next = *newidx;
            *newidx = i;
        }
    }
    free(table.hash_tab);
    table.hash_tab_caps = new_caps;
    table.hash_tab = newtab;
    return 1;
}

/* If URI is invalid returns EMPTY_URI */
uint32_t uri_add(char *uri, const char *id) {
    static size_t id_counter = 0;

    size_t uri_len = vaild_uri_len(uri), id_len = 0;
    if (!uri_len) {
        if (*uri) warn("URI '%s' is invalid", uri);
        free(uri);
        return EMPTY_URI;
    }

    // Generate internal identifier
    // if not exiplicitly provided
    char buf[MAX_NUMBER_LEN + 2];
    buf[0] = 0;
    if (LIKELY(!id)) {
        if (gconfig.unique_uris) {
            id = buf;
            buf[id_len++] = URI_ID_PREF;
            // Convert privite id to string (non-human readable)
            uint32_t idn = id_counter++;
            do buf[id_len++] = ' ' + (idn & 63);
            while (idn >>= 6);
            buf[id_len] = '\0';
        } else id = "";
    } else id_len = strlen(id);

    // Lookup in hash table for speed
    uint32_t new_hash = hash(id, id_len) ^ hash(uri, uri_len);
    if (LIKELY(table.hash_tab)) {
        uint32_t slot = table.hash_tab[new_hash % table.hash_tab_caps];
        while (slot != UINT32_MAX) {
            struct uri *cand = &table.uris[slot];
            if (cand->hash == new_hash && cand->uri &&
                    !strcmp(cand->uri, uri) && !strcmp(cand->id, id)) {
                 free(uri);
                 uri_ref(slot + 1);
                 return slot + 1;
            }
            slot = cand->next;
        }
    }

    // Duplicate string it we haven't done it already
    char *id_s = strdup(id);
    if (!id_s) goto alloc_failed;

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
        .hash = new_hash,
        .id = id_s,
        .next = UINT32_MAX,
    };

    table.count++;
    uint32_t uriid = new - table.uris + 1;

    // Insert URI into hash table
    // (resizing if necessery)
    if (UNLIKELY(4*table.count/3 > table.hash_tab_caps)) {
        size_t new_caps = URI_HASHTAB_CAPS_STEP(table.hash_tab_caps);
        if (UNLIKELY(!realloc_hashtable(new_caps, new))) {
            uri_unref(uriid);
            return EMPTY_URI;
        }
    }

    uint32_t *slot = &table.hash_tab[new_hash % table.hash_tab_caps];
    new->next = *slot;
    *slot = new - table.uris;


    if (gconfig.trace_misc) {
        if (!buf[0]) info("URI new id=%d path='%s' name='%s'", uriid, uri, id);
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
    if (id) assert(table.uris[id - 1].refc > 0);
    if (id) table.uris[id - 1].refc++;
}

void uri_unref(uint32_t id) {
    struct uri *uri = &table.uris[id - 1];
    if (id) {
        assert(uri->refc > 0);
        assert(uri->uri);
        assert(uri->id);
    }
    if (id && !--uri->refc) {
        table.count--;
        if (gconfig.trace_misc) {
            info("URI free %d (%zd left)", id, table.count);
        }
        free(uri->uri);
        free(uri->id);

        // Remove URI from hash table
        uint32_t *slot = &table.hash_tab[uri->hash % table.hash_tab_caps];
        while (*slot != UINT32_MAX && table.uris + *slot != uri) {
            slot = &table.uris[*slot].next;
        }
        if (*slot != UINT32_MAX)
            *slot = uri->next;

        uri->uri = NULL;
        uri->id = NULL;

        /* Not actually free,
         * just add to free list */
        uri->next = table.first_free;
        table.first_free = id;

        /* Shrink hash table */
        if (3*table.hash_tab_caps/4 >= 32 && table.count < table.hash_tab_caps/2)
            realloc_hashtable(3*table.hash_tab_caps/4, NULL);

        /* Try shrinking table to free memory */
        // TODO May be add bias and shrink from the start?
        // TODO Optimize to stop requiring full free list rebuilding
        size_t old_size = table.size;
        while (table.size && !table.uris[table.size - 1].uri) table.size--;
        if (old_size != table.size) {
            table.first_free = 0;
            for (size_t i = 0; i < table.size; i++) {
                if (!table.uris[i].uri) {
                    table.uris[i].next = table.first_free;
                    table.first_free = i + 1;
                }
            }
            size_t new_caps = 2*table.caps/3;
            if (new_caps >= 8 && table.size < new_caps) {
                struct uri *tmp = realloc(table.uris, new_caps*sizeof(*tmp));
                if (!tmp) return;
                table.uris = tmp;
                table.caps = new_caps;
            }
        }
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

void uri_release_memory(void) {
    for (size_t i = 0; i < table.size; i++) {
        free(table.uris[i].uri);
        free(table.uris[i].id);
    }
    free(table.uris);
    free(table.hash_tab);
    memset(&table, 0, sizeof(table));
}

#endif
