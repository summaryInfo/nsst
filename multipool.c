/* Copyright (c) 2022, Evgeniy Baskov. All rights reserved */

#include "feature.h"

#define _GNU_SOURCE

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
    uint32_t size;
    /* offset from pool beginning */
    uint32_t offset;
    /* allocation data */
    uint8_t data[];
};

/* pool metadata */
struct pool {
    struct pool *next;
    struct pool *prev;

    uint32_t n_alloc;
    bool sealed;
    ssize_t offset;
    ssize_t size;
    uint8_t data[];
};

inline static void pool_detach(struct pool **head, struct pool *pool) {
    if (pool->next)
        pool->next->prev = pool->prev;

    if (pool->prev)
        pool->prev->next = pool->next;
    else
        *head = pool->next;

    pool->next = pool->prev = NULL;
}

inline static void pool_attach(struct pool **head, struct pool *pool) {
    assert(!pool->next);
    assert(!pool->prev);

    if (*head)
        (*head)->prev = pool;

    pool->next = *head;
    *head = pool;
}

inline static void pool_seal(struct multipool *mp, struct pool *pool) {
    assert(!pool->sealed);

    pool_detach(&mp->unsealed, pool);
    pool_attach(&mp->sealed, pool);

    mp->unsealed_count--;
    pool->sealed = true;
}

inline static void pool_unseal(struct multipool *mp, struct pool *pool) {
    assert(pool->sealed);

    pool_detach(&mp->sealed, pool);
    pool_attach(&mp->unsealed, pool);

    mp->unsealed_count++;
    pool->sealed = false;
}

static struct pool *get_fitting_pool(struct multipool *mp, ssize_t size) {
    struct pool *pool = mp->unsealed;
    while (pool && pool->size - pool->offset < size)
        pool = pool->next;

    if (!pool) {
        ssize_t pool_size = MAX(mp->pool_size, size);

        pool = DO_ALLOC(sizeof *pool + pool_size);
        if (pool == ALLOC_ERROR) return NULL;

        memset(pool, 0, sizeof *pool);

        mp->pool_count++;
        mp->unsealed_count++;

        pool->size = pool_size;

        pool_attach(&mp->unsealed, pool);
    }

    return pool;
}

void mpa_release(struct multipool *mp) {
    struct pool *pool = mp->sealed, *next;

    while (pool) {
        next = pool->next;
        DO_FREE(pool, sizeof *pool + pool->size);
        pool = next;
    }

    pool = mp->unsealed;
    while (pool) {
        next = pool->next;
        DO_FREE(pool, sizeof *pool + pool->size);
        pool = next;
    }

    memset(mp, 0, sizeof *mp);
}

void mpa_init(struct multipool *mp, ssize_t pool_size, bool force_fast_resize) {
    mp->max_pad = 0;
    mp->pool_count = mp->unsealed_count = 0;
    mp->pool_size = pool_size;
    mp->force_fast_resize = force_fast_resize;
    mp->sealed = mp->unsealed = NULL;
}

void mpa_free(struct multipool *mp, void *ptr) {
    struct header *header = GET_PTR_HEADER(ptr);
    struct pool *pool = GET_HEADER_POOL(header);

    assert(pool->n_alloc);

    if (header->offset + header->size == pool->offset)
        pool->offset -= header->size;

    if (!--pool->n_alloc) {
        pool_detach(pool->sealed ? &mp->sealed : &mp->unsealed, pool);
        if (!pool->sealed)
            mp->unsealed_count--;

        if (mp->unsealed_count + 1 > mp->max_unsealed) {
            DO_FREE(pool, pool->size + sizeof *pool);
            mp->pool_count--;
        } else {
            pool_attach(&mp->unsealed, pool);
            pool->sealed = false;
            mp->unsealed_count++;
        }
    }
}

void mpa_set_seal_max_pad(struct multipool *mp, ssize_t max_pad, ssize_t max_unsealed)  {
    mp->max_pad = max_pad;
    mp->max_unsealed = max_unsealed;
}

void *mpa_alloc(struct multipool *mp, ssize_t size) {
    size = ROUNDUP(size + sizeof (struct header), MPA_ALIGNMENT);

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

void *mpa_realloc(struct multipool *mp, void *ptr, ssize_t size) {
    size = ROUNDUP(size + sizeof (struct header), MPA_ALIGNMENT);

    struct header *header = GET_PTR_HEADER(ptr);
    struct pool *pool = GET_HEADER_POOL(header);

    bool is_last = (ssize_t)header->offset + (ssize_t)header->size == (ssize_t)pool->offset;

    /* Can resize inside pool */
    if (is_last && size - (ssize_t)header->size <= pool->size - pool->offset) {
        pool->offset += size - header->size;
        header->size = size;

        return ptr;
    }

    (mp->force_fast_resize ? die : warn)
        ("Multi-pool relocation hit slow path: size=%zd max_resize=%zd is_last=%d",
         size, pool->size - pool->offset, is_last);

    if (header->size >= size)
        return ptr;

    void *new = mpa_alloc(mp, size);
    if (!new) return NULL;

    memcpy(new, ptr, MIN(header->size, size) - sizeof *header);
    mpa_free(mp, ptr);
    return new;
}

void mpa_pin(struct multipool *mp, void *ptr) {
    struct header *header = GET_PTR_HEADER(ptr);
    struct pool *pool = GET_HEADER_POOL(header);

    if (pool->sealed && pool->size - pool->offset >= mp->max_pad)
        pool_unseal(mp, pool);
}
