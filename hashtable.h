/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

#ifndef HASHTABLE_H_
#define HASHTABLE_H_ 1

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HT_INIT_CAPS 8

#ifndef HT_CALLOC
#define HT_CALLOC xzalloc
extern void *xzalloc(size_t size);
#endif

typedef struct ht_head ht_head_t;
typedef struct ht_iter ht_iter_t;
typedef struct hashtable hashtable_t;
typedef bool ht_cmpfn_t(const ht_head_t *a, const ht_head_t *b);

struct ht_head {
    ht_head_t *next;
    uintptr_t hash;
};

struct hashtable {
    ht_cmpfn_t *cmpfn;
    intptr_t caps;
    intptr_t size;
    ht_head_t **data;
};

struct ht_iter {
    hashtable_t *ht;
    ht_head_t **bucket;
    ht_head_t **elem;
};

extern void ht_shrink(struct hashtable *ht);
extern void ht_adjust(struct hashtable *ht, intptr_t inc);

static inline size_t ceil_power_of_2(size_t n) {
    if (n < 2) return n;
    return 1UL << (sizeof(n)*8 - (__builtin_clzl(n - 1)));
}

static inline ht_iter_t ht_begin(hashtable_t *ht) {
    ht_iter_t it = { .ht = ht, ht->data, ht->data };
    ht_head_t **end = ht->data + ht->caps;

    while (!*it.bucket && ++it.bucket < end);
    it.elem = it.bucket;
    return it;
}

static inline ht_head_t *ht_next(ht_iter_t *it) {
    ht_head_t **end = it->ht->data + it->ht->caps;
    if (it->bucket >= end) return NULL;

    ht_head_t *cur = *it->elem;

    if ((*it->elem)->next) {
        /* Next in chain */
        it->elem = &(*it->elem)->next;
    } else {
        /* Next bucket */
        while (++it->bucket < end && !*it->bucket);
        it->elem = it->bucket;
    }

    return cur;
}

static inline ht_head_t *ht_current(ht_iter_t *it) {
    ht_head_t **end = it->ht->data + it->ht->caps;
    if (it->bucket >= end) return NULL;
    return *it->elem;
}

static inline ht_head_t *ht_erase_current(ht_iter_t *it) {
    ht_head_t **end = it->ht->data + it->ht->caps;
    if (it->bucket >= end) return NULL;

    it->ht->size--;
    ht_head_t *cur = *it->elem;
    *it->elem = cur->next;
    cur->next = NULL;

    if (!*it->elem) {
        /* Next bucket */
        while (++it->bucket < end && !*it->bucket);
        it->elem = it->bucket;
    }

    return cur;
}

static inline void ht_free(hashtable_t *ht) {
    /* This function assumes, that all elements was freed before */
    assert(!ht->size);

    free(ht->data);
    *ht = (hashtable_t){ 0 };
}

static inline void ht_init(hashtable_t *ht, size_t caps, ht_cmpfn_t *cmpfn) {
    caps = ceil_power_of_2(caps);
    *ht = (hashtable_t) {
        .data = HT_CALLOC(caps * sizeof(ht->data[0])),
        .caps = caps,
        .cmpfn = cmpfn,
    };
}

static inline ht_head_t **ht_lookup_ptr(hashtable_t *ht, ht_head_t *elem) {
    ht_head_t **cand = &ht->data[elem->hash & (ht->caps - 1)];
    while (*cand) {
        if (elem->hash == (*cand)->hash &&
            ht->cmpfn(*cand, elem)) break;
        cand = &(*cand)->next;
    }

    return cand;
}

static inline ht_head_t *ht_insert_hint(hashtable_t *ht, ht_head_t **cand, ht_head_t *elem) {
    ht_head_t *old = *cand;
    if (!*cand) {
        *cand = elem;
        ht_adjust(ht, 1);
    }

    return old;
}

static inline ht_head_t *ht_replace_hint(hashtable_t *ht, ht_head_t **cand, ht_head_t *elem) {
    ht_head_t *old = *cand;

    *cand = elem;
    if (old) {
        elem->next = old->next;
        old->next = NULL;
    } else {
        ht_adjust(ht, 1);
    }

    return old;
}

static inline ht_head_t *ht_erase_hint(hashtable_t *ht, ht_head_t **cand) {
    ht_head_t *old = *cand;
    if (old) {
        *cand = old->next;
        old->next = NULL;
        ht_adjust(ht, -1);
    }
    return old;
}

static inline ht_head_t *ht_find(hashtable_t *ht, ht_head_t *elem) {
    return *ht_lookup_ptr(ht, elem);
}

static inline ht_head_t *ht_replace(hashtable_t *ht, ht_head_t *elem) {
    ht_head_t **cand = ht_lookup_ptr(ht, elem);
    return ht_replace_hint(ht, cand, elem);
}

static inline ht_head_t *ht_insert(hashtable_t *ht, ht_head_t *elem) {
    ht_head_t **cand = ht_lookup_ptr(ht, elem);
    return ht_insert_hint(ht, cand, elem);
}

static inline ht_head_t *ht_erase(hashtable_t *ht, ht_head_t *elem) {
    ht_head_t **cand = ht_lookup_ptr(ht, elem);
    return ht_erase_hint(ht, cand);
}

/* Murmur64A */
static inline uint64_t hash64(const void *vdata, size_t len) {
    const uint64_t m = 0xC6A4A7935BD1E995LLU;


    const uint64_t *data = (const uint64_t *)vdata;
    const uint64_t *end = data + (len >> 3);

    uint64_t k = 0, h = 123 ^ (len * m);
    while (data < end) {
        memcpy(&k, data++, sizeof(k));
        k *= m, k ^= k >> 47, k *= m;
        h ^= k, h *= m;
    }

    const uint8_t *tail = (const uint8_t *)data;
    switch (len & 7) {
    case 7: h ^= (uint64_t)tail[6] << 48; /* fallthrough */
    case 6: h ^= (uint64_t)tail[5] << 40; /* fallthrough */
    case 5: h ^= (uint64_t)tail[4] << 32; /* fallthrough */
    case 4: h ^= (uint64_t)tail[3] << 24; /* fallthrough */
    case 3: h ^= (uint64_t)tail[2] << 16; /* fallthrough */
    case 2: h ^= (uint64_t)tail[1] << 8; /* fallthrough */
    case 1: h ^= (uint64_t)tail[0];
          h *= m;
    };

    h ^= h >> 47;
    h *= m;
    h ^= h >> 47;
    return h;
}

static inline uint64_t uint_hash64(uint64_t h) {
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDUL;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53UL;
    h ^= h >> 33;
    return h;
}

static inline uint32_t uint_hash32(uint32_t v) {
    v = ((v >> 16) ^ v) * 0x45D9F3B;
    v = ((v >> 16) ^ v) * 0x45D9F3B;
    v = (v >> 16) ^ v;
    return v;
}

#endif
