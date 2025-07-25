/* Copyright (c) 2019-2022,2025, Evgeniy Baskov. All rights reserved */

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

#define PT_CHILDREN_COUNT 42
#define PT_LETTER_IDX 0
#define PT_DIGIT_IDX 26
#define PT_DASH_IDX 36
#define PT_POINT_IDX 37
#define PT_SLASH_IDX 38
#define PT_PLUS_IDX 39
#define PT_UNDERSCORE_IDX 40
#define PT_STAR_IDX 41

#define MAX_SERVICE_LINE 128

struct node_head {
    uint64_t first_child : 16;
    uint64_t has_child : 45;
    uint64_t leaf : 1;
};

#define mkurikey(_id, idlen, _uri, urilen) ((const struct uri){ .head = (ht_head_t){ .hash = hash64(_id, idlen) ^ hash64(_uri, urilen) }, .id = _id, .uri = _uri})

static struct id_table idtab;
static hashtable_t uritab;

static inline ssize_t char_to_index(char ch) {
    if (ch == '.')
        return PT_POINT_IDX;
    else if (ch == '-')
        return PT_DASH_IDX;
    else if (ch == '+')
        return PT_PLUS_IDX;
    else if (ch == '/')
        return PT_SLASH_IDX;
    else if (ch == '_')
        return PT_UNDERSCORE_IDX;
    else if (ch == '*')
        return PT_STAR_IDX;
    else if ('0' <= ch && ch <= '9')
        return PT_DIGIT_IDX + ch - '0';
    else if ('a' <= ch && ch <= 'z')
        return PT_LETTER_IDX + ch - 'a';
    else if ('A' <= ch && ch <= 'Z')
        return PT_LETTER_IDX + ch - 'A';

    return -1;
}

struct proto_tree {
    struct node_head *node_heads;
    size_t node_count;
    size_t node_heads_caps;
    uint16_t *node_children;
    size_t node_children_caps;
    ssize_t file_leaf;
};

static struct proto_tree proto_tree;

static inline struct node_head *child_index_add(struct node_head *node, char ch) {
    ssize_t idx1 = char_to_index(ch);
    if (idx1 < 0) return NULL;

    bool has_child = node->has_child & (1ULL << idx1);

    ssize_t child = node->first_child;
    child += __builtin_popcountll(node->has_child & ((1ULL << idx1) - 1));

    if (!has_child) {
        size_t i = node - proto_tree.node_heads + 1, ii = node - proto_tree.node_heads;
        adjust_buffer((void **)&proto_tree.node_children, &proto_tree.node_children_caps, proto_tree.node_count + 1, sizeof(*proto_tree.node_children));
        adjust_buffer((void **)&proto_tree.node_heads, &proto_tree.node_heads_caps, proto_tree.node_count + 2, sizeof(*proto_tree.node_heads));
        if ((ssize_t)proto_tree.node_count > child)
            memmove(proto_tree.node_children + child + 1, proto_tree.node_children + child, (proto_tree.node_count - child) * sizeof(*proto_tree.node_children));
        for (; i <= proto_tree.node_count; i++) proto_tree.node_heads[i].first_child++;
        proto_tree.node_count++;
        assert(proto_tree.node_count < UINT16_MAX);
        proto_tree.node_heads[proto_tree.node_count] = (struct node_head) { .first_child = proto_tree.node_count };
        proto_tree.node_children[child] = proto_tree.node_count;
        proto_tree.node_heads[ii].has_child |= 1ULL << idx1;
    }

    return &proto_tree.node_heads[proto_tree.node_children[child]];
}


static inline struct node_head *child_index(struct node_head *node, char ch) {
    ssize_t idx1 = char_to_index(ch);
    if (idx1 < 0) return NULL;

    bool has_child = node->has_child & (1ULL << idx1);
    if (!has_child) return NULL;

    ssize_t child = node->first_child;
    child += __builtin_popcountll(node->has_child & ((1ULL << idx1) - 1));

    return &proto_tree.node_heads[proto_tree.node_children[child]];
}

static bool reverse_proto_tree_add_proto(const char *proto, const char *end) {
    char ch;
    struct node_head *node = proto_tree.node_heads;
    for (const char *it = end - 1; it >= proto; it--) {
        node = child_index_add(node, ch = *it);
        if (!node) {
            warn("Invalid protocol name '%s', unexpected char '%c'", proto, ch);
            return false;
        }
    }

    node->leaf = true;
    return true;
}

void init_proto_tree(void) {
    FILE *services = fopen("/etc/services", "r");
    char *line = NULL;
    size_t len = 0;

    adjust_buffer((void **)&proto_tree.node_heads, &proto_tree.node_heads_caps, 1, sizeof(*proto_tree.node_heads));
    proto_tree.node_heads[0] = (struct node_head) {0};

    /* Always include 'file' pseudo protocol */
    const char *file = "\0file";
    reverse_proto_tree_add_proto(file + 1, file + 5);

    /* Save file protocol node for fast checking */
    struct node_head *node = proto_tree.node_heads;
    for (ssize_t i = 4; i > 0; i--)
        node = child_index(node, file[i]);
    proto_tree.file_leaf = node - proto_tree.node_heads;

    for (ssize_t sz; (sz = getline(&line, &len, services)) != -1; ) {
        char  *it = line;

        while (isspace(*it))
            it++;

        if (!*it || *it == '#' || *it == '\n')
            continue;

        char *proto = it;

        while (!isspace(*it))
            it++;

        if (line - it >= MAX_PROTOCOL_LEN) {
            warn("Skipping too long protocol name '%s'", it);
            continue;
        }

        reverse_proto_tree_add_proto(proto, it);
    }

    free(line);
    fclose(services);
}

const uint8_t *match_reverse_proto_tree(struct uri_match_state *stt, const uint8_t *str, ssize_t len) {
    struct node_head *node = proto_tree.node_heads;
    const uint8_t *at_valid_proto = 0;
    for (ssize_t i = 0; i < len; i++) {
        node = child_index(node, str[-i-1]);
        if (!node) break;

        if (node->leaf) {
            at_valid_proto = &str[-i-1];
            stt->matched_file_proto = node - proto_tree.node_heads == proto_tree.file_leaf;
        }
    }

    if (at_valid_proto) {
        stt->size = str - at_valid_proto;
        if (!stt->no_copy) {
            adjust_buffer((void **)&stt->data, &stt->caps, stt->size + 1, 1);
            memcpy(stt->data, at_valid_proto, stt->size);
        }
        stt->state = uris1_colon, stt->res = urim_need_more;
    } else
        stt->state = uris1_ground, stt->res = urim_ground;

    return at_valid_proto;
}

/* From window.c */
enum uri_match_result uri_match_next_from_colon(struct uri_match_state *stt, uint8_t ch) {
#define MATCH(tab, ch) (!!((tab)[((ch) >> 5) - 1] & (1U << ((ch) & 0x1F))))

    /* For real world purposes don't treat following characters as a part of URL: ()'
     * This makes parenthesized and quoted URLs match correctly. */
    // static uint32_t c_ext[] = {0xAFFFFFD2, 0x87FFFFFF, 0x47FFFFFE}; /* [\w\d\-._~!$&'()*+,;=:@/?] */
    static const uint32_t c_ext[] = {0xAFFFFC42, 0x87FFFFFF, 0x47FFFFFE}; /* [\w\d\-._~!$&*+,;=:@/?] */

    /* This code does not handle fancy unicode URIs that
     * can be displayed by browsers, but only strictly complying
     * that includes only subset of ASCII */

    /* Only ASCII graphical characters are accepted */
    if (ch - 0x21U > 0x5DU) goto finish_nak;

    if (!stt->no_copy) {
        adjust_buffer((void **)&stt->data, &stt->caps, stt->size + 2, 1);
        stt->data[stt->size] = ch;
    }
    stt->size++;

    switch (stt->state) {
    case uris1_ground:
        assert(false);
        break;
    case uris1_colon:
        if (ch == ':') {
            stt->state++;
            return (stt->res = urim_need_more);
        }
        break;
    case uris1_slash2:
    case uris1_slash1:
        if (ch == '/') {
            if (stt->state == uris1_slash2 && stt->matched_file_proto)
                stt->state = uris1_filename;
            else stt->state++;
            return (stt->res = urim_need_more);
        }
        break;
    case uris1_user:
        if (ch == '@') {
            stt->state = uris1_host;
            return (stt->res = urim_need_more);
        } else /* fallthrough */
    case uris1_host:
        if (ch == ':') {
            stt->state = uris1_port;
            return (stt->res = urim_need_more);
        } else /* fallthrough */
    case uris1_path:
        if (ch == '/') {
            stt->state = uris1_path;
            return (stt->res = urim_may_finish);
        } else if (ch == '?') {
            stt->state = uris1_query;
            return (stt->res = urim_may_finish);
        } else /* fallthrough */
    case uris1_query:
        if (ch == '#') {
            stt->state = uris1_fragment;
            return (stt->res = urim_may_finish);
        } else /* fallthrough */
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
    case uris1_filename:
        /* Allow everything except space and control characters in filenames */
        if (ch > ' ') {
            return (stt->res = urim_may_finish);
        }
        break;
    }

    if (stt->size) {
        stt->size--;
        if (stt->data) {
            /* Last character was not part of URL, remove it.
             * (buffer should be cleared by outer code.) */
            stt->data[stt->size] = '\0';
        }
    }

finish_nak:
    stt->state = uris1_ground;
    if (stt->res != urim_may_finish) {
        stt->size = 0;
        return (stt->res = urim_ground);
    } else
        return (stt->res = urim_finished);
#undef MATCH
}

void uri_match_reset(struct uri_match_state *state, bool soft) {
    if (soft) {
        state->state = uris1_colon;
        state->res = urim_ground;
        state->size =  0;
    } else {
        free(state->data);
        *state = (struct uri_match_state){0};
    }
}

char *uri_match_get(struct uri_match_state *state) {
    char *res = state->data;
    if (res) res[state->size] = '\0';
    return res;
}

static inline size_t valid_uri_len(const char *uri) {
    if (!uri) return 0;

    struct uri_match_state stt = {.no_copy = true};
    const char *uri2 = strstr(uri, "://");
    if (!uri2) return 0;

    if (!match_reverse_proto_tree(&stt,
        (const uint8_t *)uri2, uri2 - uri)) return 0;

    enum uri_match_result res = urim_ground;
    while (*uri2) {
         res = uri_match_next_from_colon(&stt, *uri2++);
        if (res == urim_finished || res == urim_ground) break;
    }

    if (!*uri2 && res == urim_may_finish) return stt.size;
    return 0;
}

static inline struct slot *alloc_slot(void) {
    if (idtab.first_free != EMPTY_URI) {
        struct slot *slot = &idtab.slots[idtab.first_free - 1];
        idtab.first_free = slot->next;
        return slot;
    } else {
        if (idtab.size + 1 >= URI_MAX)
            return NULL;
        if (idtab.size + 1 > idtab.caps) {
            size_t new_caps = URI_CAPS_STEP(idtab.caps);
            struct slot *tmp = xrealloc(idtab.slots, idtab.caps*sizeof(*tmp), new_caps*sizeof(*tmp));
            idtab.slots = tmp;
            idtab.caps = new_caps;
        }
        return &idtab.slots[idtab.size++];
    }
}

static inline void free_slot(struct slot *slot) {
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

    size_t uri_len = valid_uri_len(uri), id_len = 0;
    if (!uri_len) {
        if (*uri) warn("URI '%s' is invalid", uri);
        return EMPTY_URI;
    }

    if (!idtab.slots)
        ht_init(&uritab, HT_INIT_CAPS, uri_cmp);

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

    struct uri *new = xalloc(sizeof *new + uri_len + id_len + 2);

    /* Allocate id table slot */
    struct slot *slot = alloc_slot();
    if (!slot) {
        free(new);
        return EMPTY_URI;
    }

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
}

void uri_ref(uint32_t id) {
    if (id) {
        assert(id < URI_MAX);
        struct uri *uri = idtab.slots[id - 1].uri;
        assert(uri->refc > 0);
        uri->refc++;
    }
}

void uri_unref(uint32_t id) {
    if (!id) return;
    assert(id < URI_MAX);
    struct slot *slot = &idtab.slots[id - 1];
    struct uri *uri = slot->uri;
    assert(uri->refc > 0);
    if (!--uri->refc) {
        if (gconfig.trace_misc)
            info("URI free %d", id);

        ht_erase(&uritab, (ht_head_t *)uri);
        free_slot(slot);
        free(uri);

        ht_shrink(&uritab);
    }
}

void uri_open(const char *open_cmd, uint32_t id) {
    if (gconfig.trace_misc)
        info("URI open cmd='%s' id=%d path='%s'", open_cmd, id, uri_get(id));
    if (id && !fork()) {
        execlp(open_cmd, open_cmd, uri_get(id), NULL);
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

    proto_tree.file_leaf = 0;

    free(proto_tree.node_heads);
    proto_tree.node_heads = NULL;
    proto_tree.node_heads_caps = proto_tree.node_count = 0;

    free(proto_tree.node_children);
    proto_tree.node_children = NULL;
    proto_tree.node_children_caps = proto_tree.node_count = 0;

    memset(&idtab, 0, sizeof(idtab));
    memset(&uritab, 0, sizeof(uritab));
}

#endif
