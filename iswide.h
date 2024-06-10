/*
 * This file was generated with tools/gen_tables.py.
 * DO NOT EDIT IT DIRECTLY.
 * Edit generator instead.
 */

#ifndef _ISWIDE_H
#define _ISWIDE_H 1

#include <stdbool.h>
#include <stdint.h>

/*
 * Since Unicode does not allocate code points
 * in planes 4-13 (and plane 14 contains only control characters),
 * we can save a few bits for attributes by compressing unicode like:
 *
 *  [0x00000, 0x3FFFF] -> [0x00000, 0x3FFFF] (planes 0-3)
 *  [0x40000, 0xDFFFF] -> nothing
 *  [0xE0000,0x10FFFF] -> [0x40000, 0x7FFFF] (planes 14-16 -- Special Purpose Plane, PUA)
 *
 * And with this encoding scheme
 * we can encode all defined characters only with 19 bits.
 *
 * And so we have as much as 13 bits left for flags and attributes.
 */

#define CELL_ENC_COMPACT_BASE 0x40000
#define CELL_ENC_UTF8_BASE 0xE0000

static inline uint32_t uncompact(uint32_t u) {
    return u < CELL_ENC_COMPACT_BASE ? u : u + (CELL_ENC_UTF8_BASE - CELL_ENC_COMPACT_BASE);
}

static inline uint32_t compact(uint32_t u) {
    return u < CELL_ENC_UTF8_BASE ? u : u - (CELL_ENC_UTF8_BASE - CELL_ENC_COMPACT_BASE);
}

extern const uint8_t wide_table1_[804];
extern const uint8_t combining_table1_[1026];
extern const uint32_t width_data_[118][8];

static inline bool iswide_compact(uint32_t x) {
    return x - 0x1100U < 0x100*sizeof(wide_table1_)/sizeof(wide_table1_[0]) - 0x1100U &&
           width_data_[wide_table1_[x >> 8]][(x >> 5) & 7] & (1U << (x & 0x1F));
}

static inline bool iswide(uint32_t x) {
    return iswide_compact(compact(x));
}


static inline bool iscombining_compact(uint32_t x) {
    return x - 0x300U < 0x100*sizeof(combining_table1_)/sizeof(combining_table1_[0]) - 0x300U &&
           width_data_[combining_table1_[x >> 8]][(x >> 5) & 7] & (1U << (x & 0x1F));
}

static inline bool iscombining(uint32_t x) {
    return iscombining_compact(compact(x));
}

#endif
