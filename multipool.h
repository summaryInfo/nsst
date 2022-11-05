#ifndef MULTIPOOL_H_
#define MULTIPOOL_H_ 1

#include "feature.h"

#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>

#define MPA_ALIGNMENT (_Alignof(max_align_t))
#define MPA_POOL_SIZE 65536ULL

struct pool;

struct multipool {
    struct pool *unsealed;
    ssize_t max_pad;
    ssize_t pool_size;
    ssize_t unsealed_count;
    ssize_t pool_count;
    ssize_t max_unsealed;
    bool force_fast_resize;
};

void mpa_init(struct multipool *mp, ssize_t pool_size);

/*
 * Set the maximum amount of wasted bytes per pool.
 * This also sets the guaranteed maximal size,
 * which an unsealed object can be resized without
 * hitting slow path.
 */
void mpa_set_seal_max_pad(struct multipool *mp, ssize_t max_pad, ssize_t max_unsealed);
/* Release all allocated memory */
void mpa_release(struct multipool *mp);
/* Free an object */
void mpa_free(struct multipool *mp, void *ptr);
/* Allocate an object */
void *mpa_alloc(struct multipool *mp, ssize_t size);
/* Resize object, might move */
void *mpa_realloc(struct multipool *mp, void *ptr, ssize_t size, bool pin);

/* Mark an object to be not (easily) resizable */
void mpa_pin(struct multipool *mp, void *ptr);

/* Return allocated size */
ssize_t mpa_allocated_size(void *ptr);

#endif
