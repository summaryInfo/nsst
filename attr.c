#include <stdlib.h>
#include "attr.h"

#define CAP_INC_STEP(s) (8*(s)/5)
#define CAP_ALLOC_INIT 128

typedef struct nss_attr_storage_element {
    union {
        nss_color_t col;
        size_t next;
    };
    size_t refs;
} nss_attr_storage_element_t;

typedef struct nss_attr_storage {
    size_t capacity;
    size_t length;
    size_t free;
    nss_attr_storage_element_t *data;
} nss_attr_storage_t;

static nss_attr_storage_t stor;


void nss_init_color(nss_color_t dflt) {
    stor.data = calloc(CAP_ALLOC_INIT, sizeof(stor.data[0]));
    stor.capacity = CAP_ALLOC_INIT;
    stor.free = NSS_CID_NONE;
    stor.length = 1;
    stor.data[0].col = dflt;
    stor.data[0].refs = 1;
}

void nss_free_color(void) {
    free(stor.data);
    stor.capacity = 0;
    stor.length = 0;
    stor.free = NSS_CID_NONE;
    stor.data = NULL;
}

nss_cid_t nss_color_find(nss_color_t col) {
    for(size_t i = 0; i < stor.length; i++)
        if(stor.data[i].refs && stor.data[i].col == col) {
            stor.data[i].refs++;
            return i;
        }
    return nss_color_alloc(col);
}

nss_cid_t nss_color_alloc(nss_color_t col) {
    nss_cid_t idx = NSS_CID_NONE;
    if (stor.free != NSS_CID_NONE) {
        idx = stor.free;
        stor.free = stor.data[stor.free].next;
    } else {
        if (stor.length + 1 > stor.capacity) {
            nss_attr_storage_element_t *new = realloc(stor.data, CAP_INC_STEP(stor.capacity)*sizeof(stor.data[0]));
            if(!new) return NSS_CID_NONE;
            stor.capacity = CAP_INC_STEP(stor.capacity);
            stor.data = new;
        }
        idx = stor.length++;
    }
    stor.data[idx].refs = 1;
    stor.data[idx].col = col;
    return idx;
}

void nss_color_ref(nss_cid_t idx) {
    if (idx == NSS_CID_NONE || idx >= stor.length || !stor.data[idx].refs) return;
    stor.data[idx].refs++;
}

nss_color_t nss_color_get(nss_cid_t idx) {
    if (idx == NSS_CID_NONE || idx >= stor.length || !stor.data[idx].refs)
        return stor.data[NSS_CID_DEFAULT].col;
    return stor.data[idx].col;
}

void nss_color_set(nss_cid_t idx, nss_color_t col) {
    if (idx == NSS_CID_NONE || idx >= stor.length || !stor.data[idx].refs) return;
    stor.data[idx].col = col;
}

void nss_color_free(nss_cid_t idx) {
    if (idx == NSS_CID_NONE || idx >= stor.length || !stor.data[idx].refs) return;
    if (stor.data[idx].refs <= 1) {
        stor.data[idx].refs = 0;
        stor.data[idx].next = stor.free;
        stor.free = idx;
    }
}
