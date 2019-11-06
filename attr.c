#include <stdlib.h>
#include <string.h>
#include "attr.h"
#include "util.h"

#define CAP_INC_STEP(s) ((s) ? 8*(s)/5 : 16)

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
    nss_palette_t def_palette;
} nss_attr_storage_t;

static nss_attr_storage_t stor;

#define CN_BASE 16
#define CN_EXT (6*6*6)
#define CN_GRAY (NSS_PALETTE_SIZE - CN_BASE - CN_EXT)
#define SD28B(x) ((x) ? 0 : 0x37 + 0x28 * (x))

void nss_init_color(void) {
    stor.data = NULL;
    stor.capacity = 0;
    stor.free = NSS_CID_NONE;
    stor.length = 0;
    stor.def_palette = malloc(NSS_PALETTE_SIZE * sizeof(nss_color_t));
    // Generate basic palette
    // TODO: Make it  congigurable
    nss_color_t base[CN_BASE] = {
        0xff000000, 0xff0000cd, 0xff00cd00, 0xff00cdcd,
        0xffee0000, 0xffcd00cd, 0xffcdcd00, 0xffe5e5e5,
        0xff7f7f7f, 0xff0000ff, 0xff00ff00, 0xff00ffff,
        0xffff5c5c, 0xffff00ff, 0xffffff00, 0xffffffff
    };
    for (size_t i = 0; i < CN_BASE; i++) {
        stor.def_palette[i] = base[i];
    }
    for (size_t i = 0; i < CN_EXT; i++) {
        stor.def_palette[CN_BASE + i] = 0xff000000 +  SD28B((i / 36) % 6) +
                (SD28B((i / 36) % 6) << 8) + (SD28B((i / 36) % 6) << 16);
    }
    for (size_t i = 0; i < CN_GRAY; i++) {
        uint8_t val = MIN(0x08 + 0x0a * i, 0xff);
        stor.def_palette[CN_BASE + CN_EXT + i] = 0xff000000 + val * 0x10101;
    }
    stor.def_palette[NSS_SPECIAL_BG] = 0x77000000;
    stor.def_palette[NSS_SPECIAL_FG] = base[7];
    stor.def_palette[NSS_SPECIAL_CURSOR_BG] = base[0];
    stor.def_palette[NSS_SPECIAL_CURSOR_FG] = base[7];
}

nss_palette_t nss_create_palette(void) {
    nss_palette_t pal = malloc(NSS_PALETTE_SIZE * sizeof(nss_color_t));
    memcpy(pal, stor.def_palette, NSS_PALETTE_SIZE * sizeof(nss_color_t));
    return pal;
}

void nss_free_color(void) {
    free(stor.data);
    stor.capacity = 0;
    stor.length = 0;
    stor.free = NSS_CID_NONE;
    stor.data = NULL;
}

nss_cid_t nss_color_find(nss_palette_t pal, nss_color_t col) {
    for (size_t i = 0; i < stor.length; i++)
        if (stor.data[i].refs && stor.data[i].col == col) {
            stor.data[i].refs++;
            return i + NSS_PALETTE_SIZE;
        }
    return nss_color_alloc(pal, col);
}

nss_cid_t nss_color_alloc(nss_palette_t pal, nss_color_t col) {
    nss_cid_t idx = NSS_CID_NONE;
    if (stor.free != NSS_CID_NONE) {
        idx = stor.free;
        stor.free = stor.data[stor.free].next;
    } else {
        if (stor.length >= NSS_CID_NONE - NSS_PALETTE_SIZE - 1) return NSS_CID_NONE;
        if (stor.length + 1 > stor.capacity) {
            nss_attr_storage_element_t *new = realloc(stor.data, CAP_INC_STEP(stor.capacity)*sizeof(stor.data[0]));
            if (!new) return NSS_CID_NONE;
            memset(new + stor.capacity, 0, (CAP_INC_STEP(stor.capacity) - stor.capacity) * sizeof(stor.data[0]));
            stor.capacity = CAP_INC_STEP(stor.capacity);
            stor.data = new;
        }
        idx = stor.length++;
    }
    stor.data[idx].refs = 1;
    stor.data[idx].col = col;
    return idx == NSS_CID_NONE ? idx : idx + NSS_PALETTE_SIZE;
}

void nss_color_ref(nss_palette_t pal, nss_cid_t idx) {
    if (idx < NSS_PALETTE_SIZE) return;
    idx -= NSS_PALETTE_SIZE;
    if (idx == NSS_CID_NONE || idx >= stor.length || !stor.data[idx].refs) return;
    stor.data[idx].refs++;
}

nss_color_t nss_color_get(nss_palette_t pal, nss_cid_t idx) {
    if (idx < NSS_PALETTE_SIZE)
        return (pal ? pal : stor.def_palette)[idx];
    idx -= NSS_PALETTE_SIZE;

    if (idx == NSS_CID_NONE || idx >= stor.length || !stor.data[idx].refs)
        return stor.def_palette[idx];
    return stor.data[idx].col;
}

void nss_color_set(nss_palette_t pal, nss_cid_t idx, nss_color_t col) {
    if (idx < NSS_PALETTE_SIZE) {
        /* You can't change default colors */
        if (!pal) return;
        else pal[idx] = col;
    } else {
        idx -= NSS_PALETTE_SIZE;
        if (idx == NSS_CID_NONE || idx >= stor.length || !stor.data[idx].refs) return;
        stor.data[idx].col = col;
    }
}

void nss_color_free(nss_palette_t pal, nss_cid_t idx) {
    if (idx < NSS_PALETTE_SIZE) return;
    idx -= NSS_PALETTE_SIZE;
    if (idx == NSS_CID_NONE || idx >= stor.length || !stor.data[idx].refs) return;
    if (stor.data[idx].refs <= 1) {
        stor.data[idx].refs = 0;
        stor.data[idx].next = stor.free;
        stor.free = idx;
    }
}

void nss_free_palette(nss_palette_t pal) {
    /* Just as simple as this */
    free(pal);
}
