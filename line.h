/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */
#ifndef LINE_H_
#define LINE_H_ 1

#include "feature.h"

#include "util.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

typedef uint16_t color_id_t;
typedef uint32_t color_t;
typedef uint32_t term_char_t;

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

enum cell_attr {
    attr_italic = 1 << 0,
    attr_bold = 1 << 1,
    attr_faint = 1 << 2,
    attr_underlined = 1 << 3,
    attr_strikethrough = 1 << 4,
    attr_invisible = 1 << 5,
    attr_inverse = 1 << 6,
    attr_blink = 1 << 7,
    attr_wide = 1 << 8,
    attr_protected = 1 << 9,
    attr_drawn = 1 << 10
    // 11 bits total, sice unicode codepoint is 21 bit
};

#define MKCELLWITH(s, c) MKCELL((s).fg, (s).bg, (s).attr, (c))
#define MKCELL(f, b, l, c) ((struct cell) { .bg = (b), .fg = (f), .ch = (c), .attr = (l) & ~attr_drawn})

struct cell {
    uint32_t ch : 21;
    uint32_t attr : 11;
    color_id_t fg;
    color_id_t bg;
};

struct line_palette {
    color_id_t size;
    color_id_t caps;
    color_t data[];
};

struct line {
    struct line_palette *pal;
    int16_t width;
    bool force_damage;
    bool wrapped;
    struct cell cell[];
};

struct sgr {
    color_t fg;
    color_t bg;
    struct cell cel;
};

color_id_t alloc_color(struct line *line, color_t col, color_id_t *pre);
struct line *create_line(struct sgr sgr, ssize_t width);
struct line *realloc_line(struct line *line, ssize_t width);
/* concat_line will return NULL not touching src1 and src2 if resulting line is too long */
/* if src2 is NULL, it will relocate src1 to its length if opt == 1 */
/* if opt == 1, line palette will be minimized */
struct line *concat_line(struct line *src1, struct line *src2, bool opt);

inline static void free_line(struct line *line) {
    if (line) free(line->pal);
    free(line);
}

inline static int16_t line_length(struct line *line) {
    int16_t max_x = line->width;
    if (!line->wrapped)
        while (max_x > 0 && !line->cell[max_x - 1].ch) max_x--;
    return max_x;
}

inline static struct cell fixup_color(struct line *line, struct sgr sgr) {
    if (__builtin_expect(sgr.cel.bg >= PALETTE_SIZE, 0))
        sgr.cel.bg = alloc_color(line, sgr.bg, NULL);
    if (__builtin_expect(sgr.cel.fg >= PALETTE_SIZE, 0))
        sgr.cel.fg = alloc_color(line, sgr.fg, &sgr.cel.bg);
    return sgr.cel;
}

inline static void copy_cell(struct line *dst, ssize_t dx, struct line *src, ssize_t sx, bool dmg) {
    struct cell cel = src->cell[sx];
    if (cel.bg >= PALETTE_SIZE) cel.bg = alloc_color(dst,
            src->pal->data[cel.bg - PALETTE_SIZE], NULL);
    if (cel.fg >= PALETTE_SIZE) cel.fg = alloc_color(dst,
            src->pal->data[cel.fg - PALETTE_SIZE], &cel.bg);
    if (dmg) cel.attr &= ~attr_drawn;
    dst->cell[dx] = cel;
}

inline static ssize_t line_width(struct line *ln, ssize_t off, ssize_t w) {
    off += w;
    if (off - 1 < ln->width)
        off -= !!(ln->cell[off - 1].attr & attr_wide);
    return MIN(off, ln->width);

}

inline static ssize_t line_segments(struct line *ln, ssize_t off, ssize_t w) {
    ssize_t n = off < ln->width || (!ln->width && !off);
    while ((off = line_width(ln, off, w)) < ln->width) n++;
    return n;
}

#endif

