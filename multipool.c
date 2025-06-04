/* Copyright (c) 2023,2025, Evgeniy Baskov. All rights reserved */

#include "feature.h"

#define _GNU_SOURCE

#include "list.h"
#include "util.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>

#include "multipool.h"

#if 1
#define DO_ALLOC(size) xalloc(size)
#define DO_FREE(x, size) free(x)
#define ALLOC_ERROR NULL
#else
#define DO_ALLOC(size) mmap(NULL, (size), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0)
#define DO_FREE(x, size) munmap(x, size)
#define ALLOC_ERROR MAP_FAILED
#endif

#define GET_PTR_HEADER(ptr) ((struct header *)(ptr)-1)
#define GET_HEADER_POOL(header) ((struct pool *)((uint8_t *)(header)-(header)->offset-sizeof(struct pool)))

/* allocation metadata */
struct header {
    /* allocation size */
    int32_t size;
    /* offset from pool beginning */
    int32_t offset;
    /* allocation data */
    uint8_t data[];
};

/* pool metadata */
struct pool {
    struct list_head link;

    int32_t n_alloc;
    int32_t offset;
    int32_t size;
    bool sealed;
    uint8_t data[] ALIGNED(MPA_ALIGNMENT/2);
};

#define INIT_OFFSET ((int32_t)(ROUNDUP(sizeof(struct pool) + sizeof(struct header), MPA_ALIGNMENT)\
                     - (sizeof(struct pool) + sizeof(struct header))))

static inline void pool_seal(struct multipool *mp, struct pool *pool) {
    list_remove(&pool->link);
    mp->unsealed_count--;
    pool->sealed = true;
}

static inline void pool_unseal(struct multipool *mp, struct pool *pool) {
    list_insert_before(&mp->unsealed, &pool->link);
    mp->unsealed_count++;
    pool->sealed = false;
}

static void pool_free(struct multipool *mp, struct pool *pool) {
    if (!pool->sealed)
        pool_seal(mp, pool);

    DO_FREE(pool, pool->size + sizeof *pool);
    mp->pool_count--;
}

static struct pool *get_fitting_pool(struct multipool *mp, ssize_t size) {
    LIST_FOREACH(it, &mp->unsealed) {
        struct pool *p = CONTAINEROF(it, struct pool, link);
        if (p->size - p->offset >= size)
            return p;
    }

    ssize_t pool_size = MAX(mp->pool_size, size + INIT_OFFSET);

    struct pool *pool = DO_ALLOC(sizeof *pool + pool_size);
    if (pool == ALLOC_ERROR) return NULL;

    memset(pool, 0, sizeof *pool);

    mp->pool_count++;
    pool->size = pool_size;
    pool->offset = INIT_OFFSET;
    pool_unseal(mp, pool);

    return pool;
}

void mpa_release(struct multipool *mp) {
    LIST_FOREACH_SAFE(it, &mp->unsealed) {
        struct pool *p = CONTAINEROF(it, struct pool, link);
        list_remove(it);
        DO_FREE(p, sizeof *p + p->size);
    }

    assert(mp->pool_count == mp->unsealed_count);

    memset(mp, 0, sizeof *mp);
}

void mpa_init(struct multipool *mp, ssize_t pool_size) {
    list_init(&mp->unsealed);
    mp->max_pad = 0;
    mp->pool_count = mp->unsealed_count = 0;
    mp->pool_size = pool_size - sizeof(struct pool);
}

void mpa_free(struct multipool *mp, void *ptr) {
    struct header *header = GET_PTR_HEADER(ptr);
    struct pool *pool = GET_HEADER_POOL(header);

    assert(pool->n_alloc);

    if (header->offset + header->size == pool->offset)
        pool->offset -= header->size;

    if (!--pool->n_alloc) {
        pool->offset = INIT_OFFSET;
        if (mp->unsealed_count + 1 > mp->max_unsealed) {
            pool_free(mp, pool);
        } else if (pool->sealed) {
            pool_unseal(mp, pool);
        }
    }
}

static inline int32_t round_size(int32_t size) {
    static_assert(sizeof(struct header)*2 == MPA_ALIGNMENT, "Alignment and header size are not synchronized");
    static_assert(0 == (MPA_ALIGNMENT & (MPA_ALIGNMENT - 1)), "Alignment is not a power of two");
    return ROUNDUP(size + sizeof(struct header), MPA_ALIGNMENT);
}

void mpa_set_seal_max_pad(struct multipool *mp, ssize_t max_pad, ssize_t max_unsealed) {
    mp->max_pad = max_pad = round_size(max_pad);
    mp->max_unsealed = max_unsealed;

    LIST_FOREACH_SAFE(it, &mp->unsealed) {
        struct pool *pool = CONTAINEROF(it, struct pool, link);
        if (pool->size < max_pad + pool->offset) {
            pool_seal(mp, pool);
            if (!pool->n_alloc) {
                DO_FREE(pool, pool->size + sizeof *pool);
                mp->pool_count--;
            }
        }
    }
}

void *mpa_alloc(struct multipool *mp, ssize_t size) {
    size = round_size(size);

    struct pool *pool = get_fitting_pool(mp, MAX(size, mp->max_pad));
    if (!pool) return NULL;

    struct header *dst = (void *)(pool->data + pool->offset);
    dst->offset = pool->offset;
    dst->size = size;

    pool->offset += size;
    pool->n_alloc++;

    pool_seal(mp, pool);

    return (void *)(dst + 1);
}

ssize_t mpa_allocated_size(void *ptr) {
    struct header *header = GET_PTR_HEADER(ptr);
    return header->size - sizeof *header;
}

void *mpa_realloc(struct multipool *mp, void *ptr, ssize_t size, bool pin) {
    struct header *header = GET_PTR_HEADER(ptr);
    struct pool *pool = GET_HEADER_POOL(header);

    size = round_size(size);

    bool is_last = header->offset + header->size == pool->offset;

    /* Can resize inside pool */
    if (is_last && size - header->size <= pool->size - pool->offset) {
        pool->offset += size - header->size;
        header->size = size;

    } else if (header->size < size) {
        void *new = mpa_alloc(mp, size - sizeof *header);
        if (!new) return NULL;

        memcpy(new, ptr, MIN(header->size, size) - sizeof *header);
        mpa_free(mp, ptr);
        ptr = new;
    }

    if (pin && pool->sealed && pool->size - pool->offset >= mp->max_pad)
        pool_unseal(mp, pool);

    return ptr;
}

void mpa_pin(struct multipool *mp, void *ptr) {
    struct header *header = GET_PTR_HEADER(ptr);
    struct pool *pool = GET_HEADER_POOL(header);

    if (pool->sealed && pool->size - pool->offset >= mp->max_pad)
        pool_unseal(mp, pool);
}
