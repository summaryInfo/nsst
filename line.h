/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */
#ifndef LINE_H_
#define LINE_H_ 1

#include "feature.h"

#include "multipool.h"
#include "uri.h"
#include "util.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

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

#define MAX_LINE_LEN 16384

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
} ALIGNED(MPA_ALIGNMENT);

struct line_attr {
    ssize_t caps;
    // TODO Make this refcounted
    struct attr data[];
};

struct line_handle {
    struct line_handle *prev;
    struct line_handle *next;
    struct line *line;
    int32_t offset;

    /* Only when used as line view, it does not get accounted
     * when splitting. (FIXME) */
    int32_t width;
} ALIGNED(MPA_ALIGNMENT);

// Add default attrib value?
struct line {
    struct line *next;
    struct line *prev;
    uint64_t  seq; // Global history counter
    struct line_handle *first_handle;
    struct line_attr *attrs;
    ssize_t size;
    ssize_t caps;
    uint32_t selection_index;
    uint16_t pad_attrid;
    bool force_damage;
    bool wrapped;
    struct cell cell[];
} ALIGNED(MPA_ALIGNMENT);

uint32_t alloc_attr(struct line *line, const struct attr *attr);
struct line *create_line(struct multipool *mp, const struct attr *attr, ssize_t width);
struct line *create_line_with_seq(struct multipool *mp, const struct attr *attr, ssize_t width, uint64_t seq);
struct line *realloc_line(struct multipool *mp, struct line *line, ssize_t width);
void split_line(struct multipool *mp, struct line *src, ssize_t offset);
/* concat_line will return NULL not touching src1 and src2 if resulting line is too long */
/* if src2 is NULL, it will relocate src1 to its length if opt == 1 */
/* if opt == 1, line attributes will be minimized */
struct line *concat_line(struct multipool *mp, struct line *src1, struct line *src2, bool opt);
void copy_line(struct line *dst, ssize_t dx, struct line *src, ssize_t sx, ssize_t len);
void fill_cells(struct cell *dst, struct cell c, ssize_t width);
void copy_cells_with_attr(struct cell *dst, const uint32_t *src, const uint32_t *end, uint32_t attrid);
void free_line(struct multipool *mp, struct line *line);

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
    return iswide(uncompact(cell->ch));
}


inline static struct line *detach_prev_line(struct line *line) {
    struct line *prev = line->prev;
    if (prev)
        prev->next = NULL;
    line->prev = NULL;
    return prev;
}

inline static struct line *detach_next_line(struct line *line) {
    struct line *next = line->next;
    if (next)
        next->prev = NULL;
    line->next = NULL;
    return next;
}

inline static void attach_next_line(struct line *line, struct line *next) {
    if (next) {
#if DEBUG_LINES
        assert(!next->prev);
#endif
        next->prev = line;
    }
    if (line) {
#if DEBUG_LINES
        assert(!line->next);
#endif
        line->next = next;
    }
}

inline static void attach_prev_line(struct line *line, struct line *prev) {
    if (prev) {
#if DEBUG_LINES
        assert(!prev->next);
#endif
        prev->next = line;
    }
    if (line) {
#if DEBUG_LINES
        assert(!line->prev);
#endif
        line->prev = prev;
    }
}

inline static int16_t line_length(struct line *line) {
    int16_t max_x = line->size;
    if (!line->wrapped)
        while (LIKELY(max_x > 0 && !line->cell[max_x - 1].ch && !line->cell[max_x - 1].attrid)) max_x--;
    return max_x;
}

inline static ssize_t line_advance_width(struct line *ln, ssize_t offset, ssize_t width) {
    offset += width;
    if (offset - 1 < ln->size)
        offset -= cell_wide(&ln->cell[offset - 1]);
    return MIN(offset, ln->size);
}

inline static ssize_t line_segments(struct line *ln, ssize_t offset, ssize_t width) {
    if (!ln->size && !offset)
        return 1;

    ssize_t n = 0;

    while (offset < ln->size) {
        n++;
        offset = line_advance_width(ln, offset, width);
    }

    return n;
}

#define ATTR_MASK ((struct attr) {\
    .bold = 1, .italic = 1, .faint = 1,\
    .underlined = 3, .strikethrough = 1, .invisible = 1,\
    .reverse = 1, .blink = 1, .protected = 1}.mask)
#define PROTECTED_MASK ((struct attr){ .protected = 1}.mask)

inline static uint32_t attr_mask(const struct attr *a) {
    return a->mask & ATTR_MASK;
}

inline static void attr_mask_set(struct attr *a, uint32_t mask) {
    a->mask = (a->mask & ~ATTR_MASK) | (mask & ATTR_MASK);
}

inline static const struct attr *attr_pad(struct line *ln) {
    return ln->pad_attrid ? &ln->attrs->data[ln->pad_attrid - 1] : &ATTR_DEFAULT;
}

inline static const struct attr *attr_at(struct line *ln, ssize_t x) {
    if (x >= ln->size) return attr_pad(ln);
    return ln->cell[x].attrid ? &ln->attrs->data[ln->cell[x].attrid - 1] : &ATTR_DEFAULT;
}

inline static void adjust_wide_left(struct line *line, ssize_t x) {
    if (x < 1 || x > line->size || !line->size) return;
    struct cell *cell = line->cell + x - 1;
    if (cell_wide(cell)) *cell = MKCELL(0, cell->attrid);
}

inline static void adjust_wide_right(struct line *line, ssize_t x) {
    if (x >= line->size - 1) return;
    struct cell *cell = &line->cell[x + 1];
    if (cell_wide(cell - 1)) cell->drawn = 0;
}

inline static bool attr_eq(const struct attr *a, const struct attr *b) {
    return a->fg == b->fg && a->bg == b->bg &&
            a->ul == b->ul && !((a->mask ^ b->mask) & ~PROTECTED_MASK);
}

#if DEBUG_LINES
inline static bool find_handle_in_line(struct line_handle *handle) {
    assert(handle->line);
    struct line_handle *first = handle->line->first_handle;
    while (first) {
        if (first == handle) return true;
        first = first->next;
    };
    return false;
}
#endif

inline static void line_handle_add(struct line_handle *handle) {
#if DEBUG_LINES
    assert(!handle->next);
    assert(!handle->prev);
    assert(!find_handle_in_line(handle));
#endif

    struct line_handle *next = handle->line->first_handle;
    handle->line->first_handle = handle;
    if (next) next->prev = handle;

    handle->next = next;
    handle->prev = NULL;

#if DEBUG_LINES
    assert(find_handle_in_line(handle));
#endif
}

inline static bool line_handle_is_registered(struct line_handle *handle) {
    return handle->prev || handle == handle->line->first_handle;
}

inline static void line_handle_remove(struct line_handle *handle) {
    if (!handle->line) return;

#if DEBUG_LINES
    assert(find_handle_in_line(handle));
    if (!handle->prev)
        assert(handle->line->first_handle == handle);
    else
        assert(handle->prev->next == handle);
    if (handle->next)
        assert(handle->next->prev == handle);
#endif

    struct line_handle *next = handle->next;
    struct line_handle *prev = handle->prev;

    if (!prev)
        handle->line->first_handle = next;
    else {
        prev->next = next;
        handle->prev = NULL;
    }
    if (next) {
        next->prev = prev;
        handle->next = NULL;
    }

#if DEBUG_LINES
    assert(!find_handle_in_line(handle));
#endif
}

#define SEQNO_INC 16

extern uint64_t line_next_seqno;

inline static uint64_t get_seqno_range(uint64_t inc) {
    uint64_t ret = line_next_seqno;
    line_next_seqno += inc;
    return ret;
}

inline static void fixup_lines_seqno(struct line *line) {
    while (line) {
        line->seq = get_seqno_range(SEQNO_INC);
        line = line->next;
    }
}

inline static bool line_handle_cmpeq(struct line_handle *a, struct line_handle *b) {
    return a->line == b->line && a->offset == b->offset;
}

inline static int line_handle_cmp(struct line_handle *a, struct line_handle *b) {
    if (a->line != b->line) {
        int64_t a_seq = a->line ? a->line->seq : 0;
        int64_t b_seq = b->line ? b->line->seq : 0;
        return a_seq < b_seq ? -1 : a_seq > b_seq;
    }
    return a->offset < b->offset ? -1 : a->offset > b->offset;
}

/* Returns unregistered copy of handle */
inline static struct line_handle dup_handle(struct line_handle *handle) {
    return (struct line_handle) {
        .line = handle->line,
        .offset = handle->offset,
        .width = handle->width
    };
}

inline static void replace_handle(struct line_handle *dst, struct line_handle *src) {
#if DEBUG_LINES
    assert(src->line);
#endif
    line_handle_remove(dst);
    *dst = dup_handle(src);
    line_handle_add(dst);
}

#endif
