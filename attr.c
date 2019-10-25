#include <stdlib.h>
#include "attr.h"
#include "util.h"

#define CAP_INC_STEP(s) (8*(s)/5)

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

#define CN_BASE 16
#define CN_EXT (6*6*6)
#define CN_GRAY (NSS_N_BASE_COLORS - CN_BASE - CN_EXT)
#define SD28B(x) ((x) ? 0 : 0x37 + 0x28 * (x))

void nss_init_color() {
    stor.data = calloc(NSS_N_BASE_COLORS, sizeof(stor.data[0]));
    stor.capacity = NSS_N_BASE_COLORS;
    stor.free = NSS_CID_NONE;
    stor.length = NSS_N_BASE_COLORS;
    // Generate basic palette
    nss_color_t base[CN_BASE] = {
        0xff000000, 0xff0000cd, 0xff00cd00, 0xff00cdcd,
        0xffee0000, 0xffcd00cd, 0xffcdcd00, 0xffe5e5e5,
        0xff7f7f7f, 0xff0000ff, 0xff00ff00, 0xff00ffff,
        0xffff5c5c, 0xffff00ff, 0xffffff00, 0xffffffff
    };
    for (size_t i = 0; i < CN_BASE; i++) {
        stor.data[i].col = base[i];
        stor.data[i].refs = 1;
    }
    for (size_t i = 0; i < CN_EXT; i++) {
        stor.data[CN_BASE + i].col = 0xff000000 +  SD28B((i / 36) % 6) +
                (SD28B((i / 36) % 6) << 8) + (SD28B((i / 36) % 6) << 16);
        stor.data[CN_BASE + i].refs = 1;
    }
    for (size_t i = 0; i < CN_GRAY; i++) {
        uint8_t val = MIN(0x08 + 0x0a * i, 0xff);
        stor.data[CN_BASE + CN_EXT + i].col = 0xff000000 + val * 0x10101;
        stor.data[CN_BASE + CN_EXT + i].refs = 1;
    }
}

void nss_free_color(void) {
    free(stor.data);
    stor.capacity = 0;
    stor.length = 0;
    stor.free = NSS_CID_NONE;
    stor.data = NULL;
}

nss_cid_t nss_color_find(nss_color_t col) {
    for (size_t i = 0; i < stor.length; i++)
        if (stor.data[i].refs && stor.data[i].col == col) {
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
            if (!new) return NSS_CID_NONE;
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
