/* Copyright (c) 2019-2022, Evgeny Baskov. All rights reserved */
#ifndef LINE_H_
#define LINE_H_ 1

#include "feature.h"

#include "uri.h"
#include "util.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

#ifdef __SSE2__
#include <emmintrin.h>
#endif

#define SPECIAL_PALETTE_SIZE 13
#define PALETTE_SIZE (256 + SPECIAL_PALETTE_SIZE)
#define SPECIAL_BOLD 256
#define SPECIAL_UNDERLINE 257
#define SPECIAL_BLINK 258
#define SPECIAL_REVERSE 259
#define SPECIAL_ITALIC 260
#define SPECIAL_BG 261
#define SPECIAL_FG 262
#define SPECIAL_CURSOR_BG 263
#define SPECIAL_CURSOR_FG 264
#define SPECIAL_SELECTED_BG 265
#define SPECIAL_SELECTED_FG 266
#define SPECIAL_URI_TEXT 267
#define SPECIAL_URI_UNDERLINE 268

extern const struct attr default_attr__;

#define ATTR_DEFAULT default_attr__
#define MKCELL(c, a) ((struct cell) {.ch = (c), .attrid = (a)})
#define ATTRID_MAX 4096
#define ATTRID_DEFAULT 0

struct cell {
    uint32_t ch : 19;
    uint32_t drawn : 1;
    uint32_t attrid : 12;
};

#define UNDERLINE_NONE 0
#define UNDERLINE_SINGLE 1
#define UNDERLINE_DOUBLE 2
#define UNDERLINE_CURLY 3

struct attr {
    color_t bg;
    color_t fg;
    color_t ul;
    union {
        uint32_t mask;
        struct {
            /* URI index in terminal URI table
             * if this field is 0, theres no URI
             * associated with attribute.
             * This field is only used when
             * USE_URI option is active */
            uint32_t uri : 22;

            /* Attributes */
            bool bold : 1;
            bool italic : 1;
            bool faint : 1;
            uint32_t underlined : 2;
            bool strikethrough : 1;
            bool invisible : 1;
            bool reverse : 1;
            bool blink : 1;
            bool protected : 1;
        };
    } PACKED;
    /* Total length of union above
     * is assumed to be sizeof(uint32_t) */
};

struct line_attr {
    ssize_t caps;
    // TODO Make this refcounted
    struct attr data[];
};

// Add default attrib value?
struct line {
    struct line_attr *attrs;
    ssize_t width;
    ssize_t mwidth;
    uint32_t selection_index : 30;
    bool force_damage : 1;
    bool wrapped : 1;
    struct cell cell[];
};


uint32_t alloc_attr(struct line *line, struct attr attr);
struct line *create_line(struct attr attr, ssize_t width);
struct line *realloc_line(struct line *line, ssize_t width);
/* concat_line will return NULL not touching src1 and src2 if resulting line is too long */
/* if src2 is NULL, it will relocate src1 to its length if opt == 1 */
/* if opt == 1, line attributes will be minimized */
struct line *concat_line(struct line *src1, struct line *src2, bool opt);
void copy_line(struct line *dst, ssize_t dx, struct line *src, ssize_t sx, ssize_t len);

inline static color_t indirect_color(uint32_t idx) { return idx + 1; }
inline static uint32_t color_idx(color_t c) { return c - 1; }
inline static bool is_direct_color(color_t c) { return c >= PALETTE_SIZE; }
inline static color_t direct_color(color_t c, color_t *pal) { return is_direct_color(c) ? c : pal[color_idx(c)]; }

inline static uint8_t color_r(color_t c) { return (c >> 16) & 0xFF; }
inline static uint8_t color_g(color_t c) { return (c >> 8) & 0xFF; }
inline static uint8_t color_b(color_t c) { return c & 0xFF; }
inline static uint8_t color_a(color_t c) { return c >> 24; }
inline static color_t mk_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((color_t)a << 24U) | (r << 16U) | (g << 8U) | b;
}
inline static color_t color_apply_a(color_t c, double a) {
    return mk_color(color_r(c)*a, color_g(c)*a, color_b(c)*a, 255*a);
}

/*
 * Since Unicode does not allocate code points
 * in planes 4-13 (and plane 14 contains only control characters),
 * we can save a few bits for attributes by compressing unicode like:
 *
 *  [0x00000, 0x5FFFF] -> [0x00000, 0x5FFFF] (planes 0-4)
 *  [0x60000, 0xEFFFF] -> nothing
 *  [0xF0000,0x10FFFF] -> [0x60000, 0x7FFFF] (planes 15-16 -- PUA)
 *
 * And with this encoding scheme
 * we can encode all defined characters only with 19 bits
 *
 * And so we have as much as 13 bits left for flags and attributes
 */

#define CELL_ENC_COMPACT_BASE 0x60000
#define CELL_ENC_UTF8_BASE 0xF0000

inline static uint32_t uncompact(uint32_t u) {
    return u < CELL_ENC_COMPACT_BASE ? u : u + (CELL_ENC_UTF8_BASE - CELL_ENC_UTF8_BASE);
}

inline static uint32_t compact(uint32_t u) {
    return u < CELL_ENC_UTF8_BASE ? u : u - (CELL_ENC_UTF8_BASE - CELL_ENC_UTF8_BASE);

}

inline static uint32_t cell_get(struct cell *cell) {
    return uncompact(cell->ch);
}

inline static void cell_set(struct cell *cell, uint32_t ch) {
    cell->drawn = 0;
    cell->ch = compact(ch);
}

inline static bool cell_wide(struct cell *cell) {
    return iswide(cell->ch);
}

void free_attrs(struct line_attr *attrs);

inline static void free_line(struct line *line) {

    // If we are freeing line its selection
    // should be reset
    assert(!line->selection_index);

    if (line && line->attrs)
        free_attrs(line->attrs);
    free(line);
}

inline static int16_t line_length(struct line *line) {
    int16_t max_x = line->width;
    if (!line->wrapped)
        while (LIKELY(max_x > 0 && !line->cell[max_x - 1].ch)) max_x--;
    return max_x;
}

inline static ssize_t line_width(struct line *ln, ssize_t off, ssize_t w) {
    off += w;
    if (off - 1 < ln->width)
        off -= cell_wide(&ln->cell[off - 1]);
    return MIN(off, ln->width);
}

inline static ssize_t line_segments(struct line *ln, ssize_t off, ssize_t w) {
    ssize_t n = off < ln->width || (!ln->width && !off);
    while ((off = line_width(ln, off, w)) < ln->width) n++;
    return n;
}

#define ATTR_MASK ((struct attr) {\
    .bold = 1, .italic = 1, .faint = 1,\
    .underlined = 3, .strikethrough = 1, .invisible = 1,\
    .reverse = 1, .blink = 1, .protected = 1}.mask)
#define PROTECTED_MASK ((struct attr){ .protected = 1}.mask)

inline static uint32_t attr_mask(struct attr *a) {
    return a->mask & ATTR_MASK;
}

inline static void attr_mask_set(struct attr *a, uint32_t mask) {
    a->mask = (a->mask & ~ATTR_MASK) | (mask & ATTR_MASK);
}

inline static struct attr attr_at(struct line *ln, ssize_t x) {
    return ln->cell[x].attrid ? ln->attrs->data[ln->cell[x].attrid - 1] : ATTR_DEFAULT;
}

inline static void adjust_wide_left(struct line *line, ssize_t x) {
    if (x < 1) return;
    struct cell *cell = line->cell + x - 1;
    if (cell_wide(cell)) *cell = MKCELL(0, cell->attrid);
}

inline static void adjust_wide_right(struct line *line, ssize_t x) {
    if (x >= line->width - 1) return;
    struct cell *cell = &line->cell[x + 1];
    if (cell_wide(cell - 1)) cell->drawn = 0;
}

inline static bool attr_eq(struct attr *a, struct attr *b) {
    return a->fg == b->fg && a->bg == b->bg &&
            a->ul == b->ul && !((a->mask ^ b->mask) & ~PROTECTED_MASK);
}

inline static void fill_cells(struct cell *dst, struct cell c, ssize_t width) {
#if defined(__SSE2__)
    // Well... this looks ugly but its fast

    static_assert(sizeof(struct cell) == sizeof(uint32_t), "Wrong size of cell");
    int32_t pref = MIN((4 - (intptr_t)(((uintptr_t)dst/sizeof(uint32_t)) & 3)) & 3, width);
    switch (pref) {
    case 3: dst[2] = c; //fallthrough
    case 2: dst[1] = c; //fallthrough
    case 1: dst[0] = c; //fallthrough
    case 0:;
    }
    dst += pref;
    width -= pref;
    if (width <= 0) return;

    uint32_t cell_val;
    memcpy(&cell_val, &c, sizeof cell_val);
    const __m128i four_cells = _mm_set1_epi32(cell_val);
    for (ssize_t i = 0; i < (width & ~3); i += 4)
        _mm_stream_si128((__m128i *)&dst[i], four_cells);

    dst += width & ~3;
    switch (width & 3) {
    case 3: dst[2] = c; //fallthrough
    case 2: dst[1] = c; //fallthrough
    case 1: dst[0] = c; //fallthrough
    case 0:;
    }
#else
    ssize_t i = (width+7)/8, inc = width % 8;
    if (!inc) inc = 8;
    switch(inc) do {
        dst += inc;
        inc = 8; /* fallthrough */
    case 8: dst[7] = c; /* fallthrough */
    case 7: dst[6] = c; /* fallthrough */
    case 6: dst[5] = c; /* fallthrough */
    case 5: dst[4] = c; /* fallthrough */
    case 4: dst[3] = c; /* fallthrough */
    case 3: dst[2] = c; /* fallthrough */
    case 2: dst[1] = c; /* fallthrough */
    case 1: dst[0] = c;
    } while(--i > 0);
#endif
}

#endif

