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
    union {
        struct {
            color_t bg;
            color_t fg;
            color_t ul;
            /* Total length of union above
             * is assumed to be sizeof(uint32_t) */
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
                } PACKED;
            };
        };
        uint64_t mask64[2];
    };
} ALIGNED(MPA_ALIGNMENT);

struct line_attr {
    ssize_t caps;
    // TODO Make this refcounted
    struct attr data[];
};

struct line_span {
    struct line *line;
    int32_t offset;

    /* Only when used as line view, it does not get accounted
     * when splitting. (FIXME) */
    int32_t width;
} ALIGNED(MPA_ALIGNMENT);

struct screen_storage {
    struct line_span *begin;
    struct line_span *end;
    struct multipool pool;
};

struct line_handle {
    struct line_handle *prev;
    struct line_handle *next;
    struct line_span s;
} ALIGNED(MPA_ALIGNMENT);

struct line {
    struct line *next;
    struct line *prev;
    struct line_handle *first_handle;
    struct line_attr *attrs;
    uint64_t  seq; /* Global history counter */
    int32_t size;
    int32_t caps;
    uint32_t selection_index;
    uint16_t pad_attrid;
    bool force_damage;
    bool wrapped : 1;
    bool sh_ps1_start : 1;
    bool sh_cmd_start : 1;
    struct cell cell[];
} ALIGNED(MPA_ALIGNMENT);

uint32_t alloc_attr(struct line *line, const struct attr *attr);
struct line *create_line(struct screen_storage *screen, const struct attr *attr, ssize_t width);
struct line *realloc_line(struct screen_storage *screen, struct line *line, ssize_t width);
void split_line(struct screen_storage *screen, struct line *src, ssize_t offset);
struct line *concat_line(struct screen_storage *screen, struct line *src1, struct line *src2);
void optimize_line(struct screen_storage *screen, struct line *src);
void copy_line(struct line *dst, ssize_t dx, struct line *src, ssize_t sx, ssize_t len);
void fill_cells(struct cell *dst, struct cell c, ssize_t width);
void copy_utf32_to_cells(struct cell *dst, const uint32_t *src, const uint32_t *end, uint32_t attrid);
void copy_ascii_to_cells(struct cell *dst, const uint8_t *src, const uint8_t *end, uint32_t attrid);
void free_line(struct screen_storage *screen, struct line *line);

static inline color_t indirect_color(uint32_t idx) { return idx + 1; }
static inline uint32_t color_idx(color_t c) { return c - 1; }
static inline bool is_direct_color(color_t c) { return c >= PALETTE_SIZE; }
static inline color_t direct_color(color_t c, color_t *pal) { return is_direct_color(c) ? c : pal[color_idx(c)]; }

static inline uint8_t color_r(color_t c) { return (c >> 16) & 0xFF; }
static inline uint8_t color_g(color_t c) { return (c >> 8) & 0xFF; }
static inline uint8_t color_b(color_t c) { return c & 0xFF; }
static inline uint8_t color_a(color_t c) { return c >> 24; }
static inline color_t mk_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((color_t)a << 24U) | (r << 16U) | (g << 8U) | b;
}
static inline color_t color_apply_a(color_t c, double a) {
    return mk_color(color_r(c)*a, color_g(c)*a, color_b(c)*a, 255*a);
}

static inline uint32_t cell_get(struct cell *cell) {
    return uncompact(cell->ch);
}

static inline void cell_set(struct cell *cell, uint32_t ch) {
    cell->drawn = 0;
    cell->ch = compact(ch);
}

static inline bool cell_wide(struct cell *cell) {
    return iswide(uncompact(cell->ch));
}


static inline struct line *detach_prev_line(struct line *line) {
    struct line *prev = line->prev;
    if (prev)
        prev->next = NULL;
    line->prev = NULL;
    return prev;
}

static inline struct line *detach_next_line(struct line *line) {
    struct line *next = line->next;
    if (next)
        next->prev = NULL;
    line->next = NULL;
    return next;
}

static inline void attach_next_line(struct line *line, struct line *next) {
    if (next) {
#if DEBUG_LINES
        assert(!next->prev);
#endif
        next->prev = line;
    }
#if DEBUG_LINES
    assert(!line->next);
#endif
    line->next = next;
}

static inline void attach_prev_line(struct line *line, struct line *prev) {
    if (prev) {
#if DEBUG_LINES
        assert(!prev->next);
#endif
        prev->next = line;
    }
#if DEBUG_LINES
    assert(!line->prev);
#endif
    line->prev = prev;
}

static inline ssize_t line_length(struct line *line) {
    int16_t max_x = line->size;
    if (!line->wrapped) {
        while (LIKELY(max_x > 0) &&
               UNLIKELY(!line->cell[max_x - 1].ch) &&
               !line->cell[max_x - 1].attrid) max_x--;
    }
    return max_x;
}

static inline ssize_t line_advance_width(struct line *ln, ssize_t offset, ssize_t width) {
    offset += width;
    if (offset - 1 < ln->size)
        offset -= cell_wide(&ln->cell[offset - 1]);
    return MIN(offset, ln->size);
}

static inline ssize_t line_segments(struct line *ln, ssize_t offset, ssize_t width) {
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
    .bold = true, .italic = true, .faint = true,\
    .underlined = 3, .strikethrough = true, .invisible = true,\
    .reverse = true, .blink = true, .protected = true}.mask)
#define PROTECTED_MASK64 ((struct attr){ .protected = true }.mask64[1])

static inline uint32_t attr_mask(const struct attr *a) {
    return a->mask & ATTR_MASK;
}

static inline void attr_mask_set(struct attr *a, uint32_t mask) {
    a->mask = (a->mask & ~ATTR_MASK) | (mask & ATTR_MASK);
}

static inline const struct attr *attr_pad(struct line *ln) {
    return ln->pad_attrid ? &ln->attrs->data[ln->pad_attrid - 1] : &ATTR_DEFAULT;
}

static inline const struct attr *attr_at(struct line *ln, ssize_t x) {
    if (x >= ln->size) return attr_pad(ln);
    return ln->cell[x].attrid ? &ln->attrs->data[ln->cell[x].attrid - 1] : &ATTR_DEFAULT;
}

static inline void adjust_wide_left(struct line *line, ssize_t x) {
    if (x < 1 || x > line->size || !line->size) return;
    struct cell *cell = line->cell + x - 1;
    if (cell_wide(cell)) *cell = MKCELL(0, cell->attrid);
}

static inline void adjust_wide_right(struct line *line, ssize_t x) {
    if (x >= line->size - 1) return;
    struct cell *cell = &line->cell[x + 1];
    if (cell_wide(cell - 1)) cell->drawn = 0;
}

static inline bool attr_eq(const struct attr *a, const struct attr *b) {
    return a->mask64[0] == b->mask64[0] &&
           !((a->mask64[1] ^ b->mask64[1]) & ~PROTECTED_MASK64);
    // return a->fg == b->fg && a->bg == b->bg &&
    //         a->ul == b->ul && !((a->mask ^ b->mask) & ~PROTECTED_MASK);
}

#if DEBUG_LINES
static inline bool find_handle_in_line(struct line_handle *handle) {
    assert(handle->s.line);
    struct line_handle *first = handle->s.line->first_handle;
    while (first) {
        if (first == handle) return true;
        first = first->next;
    };
    return false;
}
#endif

static inline void line_handle_add(struct line_handle *handle) {
#if DEBUG_LINES
    assert(!handle->next);
    assert(!handle->prev);
    assert(!find_handle_in_line(handle));
#endif

    struct line_handle *next = handle->s.line->first_handle;
    handle->s.line->first_handle = handle;
    if (next) next->prev = handle;

    handle->next = next;
    handle->prev = NULL;

#if DEBUG_LINES
    assert(find_handle_in_line(handle));
#endif
}

static inline bool line_handle_is_registered(struct line_handle *handle) {
    return handle->prev || handle == handle->s.line->first_handle;
}

static inline void line_handle_remove(struct line_handle *handle) {
    if (!handle->s.line) return;

#if DEBUG_LINES
    assert(find_handle_in_line(handle));
    if (!handle->prev)
        assert(handle->s.line->first_handle == handle);
    else
        assert(handle->prev->next == handle);
    if (handle->next)
        assert(handle->next->prev == handle);
#endif

    struct line_handle *next = handle->next;
    struct line_handle *prev = handle->prev;

    if (!prev)
        handle->s.line->first_handle = next;
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

static inline uint64_t get_seqno_range(uint64_t inc) {
    uint64_t ret = line_next_seqno;
    line_next_seqno += inc;
    return ret;
}

static inline void fixup_lines_seqno(struct line *line) {
    while (line) {
        line->seq = get_seqno_range(SEQNO_INC);
        line = line->next;
    }
}

static inline bool line_span_cmpeq(struct line_span *a, struct line_span *b) {
    return a->line == b->line && a->offset == b->offset;
}

static inline int line_span_cmp(struct line_span *a, struct line_span *b) {
    if (a->line != b->line) {
        int64_t a_seq = a->line ? a->line->seq : 0;
        int64_t b_seq = b->line ? b->line->seq : 0;
        return a_seq < b_seq ? -1 : a_seq > b_seq;
    }
    return a->offset < b->offset ? -1 : a->offset > b->offset;
}

static inline void replace_handle(struct line_handle *dst, struct line_span *src) {
#if DEBUG_LINES
    assert(src->line);
    line_handle_remove(dst);
    dst->s = *src;
    line_handle_add(dst);
#else
    if (LIKELY(dst->s.line)) {
        struct line_handle *next = dst->next;
        struct line_handle *prev = dst->prev;
        if (!prev) dst->s.line->first_handle = next;
        else prev->next = next;
        if (next) next->prev = prev;
    }

    struct line *line = src->line;
    struct line_handle *next = line->first_handle;
    if (next) next->prev = dst;
    line->first_handle = dst;

    dst->s = *src;
    dst->next = next;
    dst->prev = NULL;
#endif
}

static inline ssize_t line_span_shift(struct line_span *pos, ssize_t width) {
    bool res = 0;

    ssize_t offset = line_advance_width(pos->line, pos->offset, width);
    if (offset >= pos->line->size) {
        if (pos->line->next) {
            pos->line = pos->line->next;
            pos->offset = 0;
        } else {
            res = 1;
        }
    } else {
        pos->offset = offset;
    }

    return res;
}

static inline ssize_t line_shift_n(struct line_span *pos, ssize_t amount, ssize_t width) {
    if (amount < 0) {
        // TODO Little optimization
        amount += line_segments(pos->line, 0, width) - line_segments(pos->line, pos->offset, width);
        pos->offset = 0;
        while (amount < 0) {
            if (!pos->line->prev)
                break;
            pos->line = pos->line->prev;
            amount += line_segments(pos->line, 0, width);
        }
    }
    if (amount > 0) {
        while (amount) {
            ssize_t offset = line_advance_width(pos->line, pos->offset, width);
            if (offset >= pos->line->size) {
                if (!pos->line->next)
                    break;
                pos->line = pos->line->next;
                pos->offset = 0;
            } else
                pos->offset = offset;
            amount--;
        }
    }

    return amount;

}

#endif
