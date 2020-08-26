/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */
#ifndef LINE_H_
#define LINE_H_ 1

#include "feature.h"

#include "util.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#define SPECIAL_PALETTE_SIZE 11
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

#define MKCELL(c, a) ((struct cell) {.ch = (c), .attrid = (a)})
#define ATTRID_MAX 512
#define ATTRID_DEFAULT 0

typedef uint32_t color_t;

struct cell {
    uint32_t ch : 21;
    uint32_t drawn : 1;
    uint32_t wide : 1;
    uint32_t attrid : 9;
};

struct attr {
    color_t fg;
    color_t bg;
    union {
        uint16_t mask;
        struct {
            bool bold : 1;
            bool italic : 1;
            bool faint : 1;
            bool underlined : 1;
            bool strikethrough : 1;
            bool invisible : 1;
            bool reverse : 1;
            bool blink : 1;
            bool protected : 1;
        };
    };
};

struct line_attr {
    ssize_t size;
    ssize_t caps;
    struct attr data[];
};

// Add default attrib value?
struct line {
    struct line_attr *attrs;
    ssize_t width;
    bool force_damage;
    bool wrapped;
    struct cell cell[];
};


uint32_t alloc_attr(struct line *line, struct attr attr);
struct line *create_line(struct attr attr, ssize_t width);
struct line *realloc_line(struct line *line, ssize_t width);
/* concat_line will return NULL not touching src1 and src2 if resulting line is too long */
/* if src2 is NULL, it will relocate src1 to its length if opt == 1 */
/* if opt == 1, line attributes will be minimized */
struct line *concat_line(struct line *src1, struct line *src2, bool opt);
void copy_line(struct line *dst, ssize_t dx, struct line *src, ssize_t sx, ssize_t len, bool dmg);


inline static color_t indirect_color(uint32_t idx) { return idx; }
inline static uint32_t color_idx(color_t c) { return c; }
inline static bool is_direct_color(color_t c) { return c > PALETTE_SIZE; }
inline static color_t direct_color(color_t c, color_t *pal) { return is_direct_color(c) ? c : pal[color_idx(c)]; }

inline static uint8_t color_r(color_t c) { return (c >> 16) & 0xFF; }
inline static uint8_t color_g(color_t c) { return (c >> 8) & 0xFF; }
inline static uint8_t color_b(color_t c) { return c & 0xFF; }
inline static uint8_t color_a(color_t c) { return c >> 24; }
inline static color_t mk_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (a << 24U) | (r << 16U) | (g << 8U) | b;
}
inline static color_t color_premult(color_t c, uint8_t a) {
    return mk_color(color_r(c)*a/255, color_g(c)*a/255,color_b(c)*a/255, a);
}

inline static void free_line(struct line *line) {
    if (line) free(line->attrs);
    free(line);
}

inline static int16_t line_length(struct line *line) {
    int16_t max_x = line->width;
    if (!line->wrapped)
        while (max_x > 0 && !line->cell[max_x - 1].ch) max_x--;
    return max_x;
}

inline static ssize_t line_width(struct line *ln, ssize_t off, ssize_t w) {
    off += w;
    if (off - 1 < ln->width)
        off -= ln->cell[off - 1].wide;
    return MIN(off, ln->width);
}

inline static ssize_t line_segments(struct line *ln, ssize_t off, ssize_t w) {
    ssize_t n = off < ln->width || (!ln->width && !off);
    while ((off = line_width(ln, off, w)) < ln->width) n++;
    return n;
}

inline static uint32_t attr_mask(struct attr *a) {
    return a->mask;
}

inline static void attr_mask_set(struct attr *a, uint32_t mask) {
    a->mask = mask;
}

inline static struct attr attr_at(struct line *ln, ssize_t x) {
    return ln->cell[x].attrid ? ln->attrs->data[ln->cell[x].attrid - 1] :
            (struct attr){ .fg = indirect_color(SPECIAL_FG), .bg = indirect_color(SPECIAL_BG)};
}

inline static bool attr_eq(struct attr *a, struct attr *b) {
    return a->fg == b->fg && a->bg == b->bg && attr_mask(a) == attr_mask(b);
}


#endif

