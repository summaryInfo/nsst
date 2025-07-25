/* Copyright (c) 2019-2022,2025, Evgeniy Baskov. All rights reserved */

#include "feature.h"

#include "line.h"
#include "util.h"
#include "hashtable.h"

#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef __SSE2__
#include <emmintrin.h>
#endif

#define MAX_EXTRA_PALETTE (ATTRID_MAX - 1)
#define INIT_CAP 4
#define CAPS_INC_STEP(sz) MIN(MAX_EXTRA_PALETTE + 1, MAX(2*(sz), INIT_CAP))

static_assert(!(CAPS_INC_STEP(UINT32_MAX) & (CAPS_INC_STEP(UINT32_MAX) - 1)), "Must be power of two");

const struct attr default_attr__ = {
        .fg = SPECIAL_FG + 1,
        .bg = SPECIAL_BG + 1,
        .ul = SPECIAL_BG + 1
};

uint64_t line_next_seqno = 1;

static inline bool attr_eq_prot(const struct attr *a, const struct attr *b) {
    static_assert(sizeof(struct attr) == 2*sizeof(uint64_t), "Wrong attribute size");
    return a->mask64[0] == b->mask64[0] && a->mask64[1] == b->mask64[1];
    //return a->fg == b->fg && a->bg == b->bg && a->ul == b->ul && a->mask == b->mask;
}

static inline uint32_t attr_hash(const struct attr *attr) {
    return uint_hash32(attr->bg) ^
            uint_hash32(attr->fg) ^
            uint_hash32(attr->ul) ^
            uint_hash32(attr->mask);
}

static inline bool attr_empty(struct attr *attr) {
    return attr->fg == 0;
}

static uint32_t insert_attr(struct line_attr *tab, const struct attr *attr, uint32_t hash) {
    size_t i = hash & (tab->caps - 1);
    size_t i0 = i, caps = tab->caps;
    do {
        struct attr *pattr = &tab->data[i];
        if (attr_empty(pattr)) {
            *pattr = *attr;
            return i + 1;
        } else if (attr_eq_prot(pattr, attr)) {
            return i + 1;
        }

        if (++i >= caps) i -= caps;
    } while (i0 != i);

    return 0;
}

void free_attrs(struct line_attr *attrs) {
#if USE_URI
    for (ssize_t i = 0; i < attrs->caps; i++)
        uri_unref(attrs->data[i].uri);
#endif

    free(attrs);
}

static inline bool need_fix_span_array(struct line *line, struct line_span *first, struct line_span *last) {
    return first &&
        (!first->line || first->line->seq <= line->seq) &&
        (!last[-1].line || line->seq <= last[-1].line->seq);
}

#define for_span_array_line(line_, it, first, last) \
    struct line_span *it = (first); \
    while ((it) < (last) && (it)->line != (line_)) it++; \
    for (; (it) < (last) && (it)->line == (line_); (it)++)

void free_line(struct screen_storage *screen, struct line *line) {

    if (!line) return;

    /* If we are freeing line its selection should be reset */
    // TODO Make selection a set of regular handles
#if DEBUG_LINES
    assert(!line->selection_index);
#endif

    detach_prev_line(line);
    detach_next_line(line);

    /* Handles are effectively weak pointers in
     * a sence that they can become NULL after the
     * line is freed. This is useful to avoid explicitly
     * updating e.g. selection data. */
    while (line->first_handle) {
        struct line_handle *handle = line->first_handle;
#if DEBUG_LINES
        assert(!handle->prev);
        assert(handle->s.line == line);
#endif
        line_handle_remove(handle);
        handle->s.line = NULL;
    }

    if (need_fix_span_array(line, screen->begin, screen->end)) {
        for_span_array_line(line, it, screen->begin, screen->end) {
            it->line = NULL;
        }
    }

    if (line->attrs)
        free_attrs(line->attrs);
    mpa_free(&screen->pool, line);
}

static uint32_t move_one_attr(struct line_attr *dst, struct line *src, uint32_t id) {
    struct attr *at = &src->attrs->data[id - 1];
    if (UNLIKELY(!attr_empty(at))) {
        at->bg = insert_attr(dst, at, attr_hash(at));
        at->fg = 0;
        at->uri = EMPTY_URI;
    }
    return at->bg;
}

static void move_attrtab(struct line_attr *dst, struct line *src) {
    for (ssize_t i = 0; i < src->size; i++) {
        uint32_t old_id = src->cell[i].attrid;
        if (old_id == ATTRID_DEFAULT) continue;
#if DEBUG_LINES
        assert(old_id <= src->attrs->caps);
#endif
        src->cell[i].attrid = move_one_attr(dst, src, old_id);
    }

#if DEBUG_LINES
    assert(src->pad_attrid <= src->attrs->caps);
#endif
    if (src->pad_attrid != ATTRID_DEFAULT)
        src->pad_attrid = move_one_attr(dst, src, src->pad_attrid);

    free_attrs(src->attrs);
    src->attrs = dst;
}

uint32_t alloc_attr(struct line *line, const struct attr *attr) {
    if (attr_eq_prot(attr, &ATTR_DEFAULT)) return ATTRID_DEFAULT;

    uint32_t hash = attr_hash(attr);

    if (!line->attrs) {
        line->attrs = xzalloc(sizeof *line->attrs + INIT_CAP * sizeof *line->attrs->data);
        line->attrs->caps = INIT_CAP;
    }

#if USE_URI
    uri_ref(attr->uri);
#endif

    uint32_t id = insert_attr(line->attrs, attr, hash);
    if (id) return id;

    size_t new_caps = CAPS_INC_STEP(line->attrs->caps);
#if DEBUG_LINES
    assert(!(new_caps & (new_caps - 1)));
#endif
    struct line_attr *new = xzalloc(sizeof *new + new_caps * sizeof *new->data);

    new->caps = new_caps;

    move_attrtab(new, line);

    return insert_attr(line->attrs, attr, hash);
}

static struct line *create_line_with_seq(struct screen_storage *screen, const struct attr *attr,
                                         ssize_t caps, uint64_t seq) {
    struct line *line = mpa_alloc(&screen->pool, sizeof *line + (size_t)caps * sizeof *line->cell);

#if DEBUG_LINES
    assert(caps >= 0);
#endif

    *line = (struct line) {
        .seq = seq,
        .caps = caps,
        .pad_attrid = 0,
        .force_damage = true,
    };

    line->pad_attrid = alloc_attr(line, attr);
    return line;
}

struct line *create_line(struct screen_storage *screen, const struct attr *attr, ssize_t caps) {
    return create_line_with_seq(screen, attr, caps, get_seqno_range(SEQNO_INC));
}

struct line *realloc_line_(struct multipool *pool, struct line *line, ssize_t caps) {
    size_t new_size = sizeof(*line) + (size_t)caps * sizeof(line->cell[0]);

    struct line *new = mpa_realloc(pool, line, new_size, false);

    new->size = MIN(caps, new->size);
    new->caps = caps;

    new->force_damage = true;

    if (new == line)
        return new;

    if (new->next)
        new->next->prev = new;
    if (new->prev)
        new->prev->next = new;

    /* Update registered handles */
    struct line_handle *handle = new->first_handle;
    while (handle) {
        handle->s.line = new;
        handle = handle->next;
    }

    return new;
}

struct line *realloc_line(struct screen_storage *screen, struct line *line, ssize_t caps) {
    bool need_fixup_array = need_fix_span_array(line, screen->begin, screen->end);

    struct line *new = realloc_line_(&screen->pool, line, caps);

    if (new != line && need_fixup_array) {
        for_span_array_line(line, it, screen->begin, screen->end) {
            it->line = new;
        }
    }

    return new;
}

/* We know that byte has 8 bits */
#define LONG_BITS ((ssize_t)(8*sizeof(unsigned long)))

static void optimize_attributes(struct line *line) {
    if (!line->attrs) return;

    unsigned long used[(MAX_EXTRA_PALETTE + 1)/LONG_BITS];
    ssize_t max_elem = (line->attrs->caps + LONG_BITS - 1)/LONG_BITS;

    memset(used, 0, max_elem*sizeof *used);

    used[line->pad_attrid / LONG_BITS] |= 1ULL << (line->pad_attrid % LONG_BITS);
#if DEBUG_LINES
    assert(line->pad_attrid == ATTRID_DEFAULT || line->pad_attrid <= line->attrs->caps);
#endif

    for (ssize_t i = 0; i < line->size; i++) {
        uint64_t id = line->cell[i].attrid;
        used[id / LONG_BITS] |= 1ULL << (id % LONG_BITS);
#if DEBUG_LINES
        assert(id == ATTRID_DEFAULT || (int64_t)id <= line->attrs->caps);
#endif
    }

    ssize_t cnt = -(used[0] & 1);
    for (ssize_t i = 0; i < max_elem; i++)
        cnt += __builtin_popcountll(used[i]);

    if (cnt) {
        cnt = ceil_power_of_2(cnt);
        struct line_attr *new = xzalloc(sizeof *new + cnt*sizeof *new->data);
        new->caps = cnt;

        move_attrtab(new, line);
    } else {
        free_attrs(line->attrs);
        line->attrs = NULL;
    }
}

void split_line(struct screen_storage *screen, struct line *src, ssize_t offset) {
    ssize_t tail_len = src->size - offset;
#if DEBUG_LINES
    assert(tail_len >= 0);
#endif

    uint64_t dist = src->next ? src->next->seq - src->seq : 0;
    bool need_fixup = src->next && dist < 2;
    uint64_t tail_seq = dist < 2 ? get_seqno_range(SEQNO_INC) : src->seq + dist/2;

    struct line *tail = create_line_with_seq(screen, attr_pad(src), tail_len, tail_seq);
#if DEBUG_LINES
    assert(tail_seq < get_seqno_range(0));
    assert(tail->seq > src->seq);
    assert(tail);
#endif

    copy_line(tail, 0, src, offset, tail_len);

    /* Fixup line properties */

    tail->force_damage = src->force_damage;
    tail->wrapped = src->wrapped;

    src->size = offset;
    src->wrapped = false;

    /* Fixup line ordering */

    if (src->next)
        src->next->prev = tail;
    tail->next = src->next;
    tail->prev = src;
    src->next = tail;

    /* Fixup references */

    struct line_handle *handle = src->first_handle, *next;
    while (handle) {
        next = handle->next;
        if (handle->s.offset >= offset) {
            line_handle_remove(handle);
            handle->s.line = tail;
            handle->s.offset -= offset;
            line_handle_add(handle);
        }
        handle = next;
    }


    if (need_fixup)
        fixup_lines_seqno(tail->next);

    struct line *new = realloc_line_(&screen->pool, src, offset);

    if (need_fix_span_array(new, screen->begin, screen->end)) {
        for_span_array_line(src, it, screen->begin, screen->end) {
            if (it->offset >= offset) {
                it->line = tail;
                it->offset -= offset;
            } else {
                it->line = new;
            }
        }
    }

}

void optimize_line(struct screen_storage *screen, struct line *line) {
    /* NOTE After this point line will never be resized */
    if (line->size < line->caps) {
        size_t new_size = sizeof(*line) + (size_t)line->size * sizeof(line->cell[0]);
        struct line *new = mpa_realloc(&screen->pool, line, new_size, true);
        (void)new;

#if DEBUG_LINES
        assert(new == line);
#endif
        line->caps = line->size;
    } else
        mpa_pin(&screen->pool, line);

    optimize_attributes(line);
}

struct line *concat_line(struct screen_storage *screen, struct line *src1, struct line *src2) {
#if DEBUG_LINES
    assert(src1 && src2);
    assert(src1->next == src2);
    assert(src2->prev == src1);
#endif

    ssize_t len = src2->size + src1->size;
    ssize_t first_len = src1->size;
    src1 = realloc_line(screen, src1, src1->size + src2->caps);
    src1->wrapped = src2->wrapped;
    src1->force_damage |= src2->force_damage;

    if (src1->attrs && src2->attrs) {
        src1->pad_attrid = alloc_attr(src1, attr_pad(src2));
        copy_line(src1, src1->size, src2, 0, len - src1->size);
    } else {
        /* Faster line content copying in we do
         * not need to merge attributes */
        memcpy(src1->cell + src1->size, src2->cell,
               (len - src1->size) * sizeof *src1->cell);
        src1->size = len;
        src1->pad_attrid = src2->pad_attrid;
        if (src2->attrs) {
            src1->attrs = src2->attrs;
            src2->attrs = NULL;
        }
    }

    /* Update line links */
    struct line *next_line = detach_next_line(src2);
    detach_prev_line(src2);
    attach_next_line(src1, next_line);

    if (need_fix_span_array(src2, screen->begin, screen->end)) {
        for_span_array_line(src2, it, screen->begin, screen->end) {
            it->line = src1;
            it->offset += first_len;
        }
    }

    /* Update line handles */
    struct line_handle *handle = src2->first_handle, *next;
    while (handle) {
        next = handle->next;
        line_handle_remove(handle);
        handle->s.line = src1;
        handle->s.offset += first_len;
        line_handle_add(handle);
        handle = next;
    }

    src2->seq = 0;
    free_line(screen, src2);

    return src1;
}

void HOT copy_line(struct line *dst, ssize_t dx, struct line *src, ssize_t sx, ssize_t len) {
    struct cell *sc = src->cell + sx, *dc = dst->cell + dx, c;

    assert(sx + len <= src->size);
    assert(dx + len <= dst->caps);

    if (dst != src) {
        uint32_t previd = ATTRID_MAX, newid = 0;
        if (dx + len > dst->size) {
            memset(dst->cell + dst->size, 0, (dx + len - dst->size)*sizeof *dc);
            dst->size = dx + len;
        }

        for (ssize_t i = 0; i < len; i++) {
            c = *sc++;
            c.drawn = 0;
            if (UNLIKELY(c.attrid)) {
                if (UNLIKELY(c.attrid != previd))
                    newid = alloc_attr(dst, &src->attrs->data[c.attrid - 1]);
                c.attrid = newid;
            }
            *dc++ = c;
        }
    } else {
        if (dx + len > dst->size) {
            if (dx > dst->size)
                memset(dst->cell + dst->size, 0, (dx - dst->size)*sizeof *dc);
            dst->size = dx + len;
        }
        memmove(dc, sc, len * sizeof(*sc));
        while (len--) dc++->drawn = 0;
    }
}

HOT
void fill_cells(struct cell *dst, struct cell c, ssize_t width) {
#if defined(__SSE2__)
    /* Well... this looks ugly but its fast */

    static_assert(sizeof(struct cell) == sizeof(uint32_t), "Wrong size of cell");
    int32_t pref = MIN((4 - (intptr_t)(((uintptr_t)dst/sizeof(uint32_t)) & 3)) & 3, width);
    switch (pref) {
    case 3: dst[2] = c; /* fallthrough */
    case 2: dst[1] = c; /* fallthrough */
    case 1: dst[0] = c; /* fallthrough */
    case 0:;
    }
    dst += pref;
    width -= pref;
    if (width <= 0) return;

    uint32_t cell_val;
    memcpy(&cell_val, &c, sizeof cell_val);
    const __m128i four_cells = _mm_set1_epi32(cell_val);
    for (ssize_t i = 0; i < (width & ~3); i += 4)
        _mm_store_si128((__m128i *)&dst[i], four_cells);

    dst += width & ~3;
    switch (width & 3) {
    case 3: dst[2] = c; /* fallthrough */
    case 2: dst[1] = c; /* fallthrough */
    case 1: dst[0] = c; /* fallthrough */
    case 0:;
    }
#else
    ssize_t i = (width+7)/8, inc = width % 8;
    if (!inc) inc = 8;
    switch (inc) do {
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
    } while (--i > 0);
#endif
}

HOT
void copy_utf32_to_cells(struct cell *dst, const uint32_t *src, const uint32_t *end, uint32_t attrid) {
    uint32_t *restrict dstp = (uint32_t *)dst;
    attrid <<= 20;

    if (UNLIKELY(end - src < 4))
        goto short_copy;

    int32_t pref = (-(uintptr_t)dstp & 15ULL) / sizeof(uint32_t);

#ifdef __SSE2__
    const __m128i four_attrs = _mm_set1_epi32(attrid);
    if (pref) {
        /* Write unaligned prefix separately */
        _mm_storeu_si128((__m128i *)dstp,
                         _mm_or_si128(four_attrs, _mm_loadu_si128((__m128i *)src)));

        src += pref;
        dstp += pref;
    }

    register ssize_t blocks = (end - src)/4;

    if ((uintptr_t)src & (4 * sizeof(uint32_t) - 1)) {
        for (ssize_t i = 0; i < blocks; i++)
            _mm_store_si128((__m128i *)(dstp + i*4),
                             _mm_or_si128(four_attrs,
                                          _mm_loadu_si128((__m128i *)(src + i*4))));
    } else {
        for (ssize_t i = 0; i < blocks; i++)
            _mm_store_si128((__m128i *)(dstp + i*4),
                             _mm_or_si128(four_attrs,
                                          _mm_load_si128((__m128i *)(src + i*4))));
    }
#else
    switch (pref) {
        case 3: dstp[2] = src[2] | attrid; /* fallthrough */
        case 2: dstp[1] = src[1] | attrid; /* fallthrough */
        case 1: dstp[0] = src[0] | attrid; /* fallthrough */
        default:;
    }

    src += pref;
    dstp += pref;

    register ssize_t blocks = (end - src)/4;

    for (ssize_t i = 0; i < blocks; i++) {
        dstp[4*i + 0] = attrid | src[4*i + 0];
        dstp[4*i + 1] = attrid | src[4*i + 1];
        dstp[4*i + 2] = attrid | src[4*i + 2];
        dstp[4*i + 3] = attrid | src[4*i + 3];
    }

#endif

    src += blocks*4;
    dstp += blocks*4;

short_copy:
    switch ((end - src)) {
        case 3: dstp[2] = src[2] | attrid; /* fallthrough */
        case 2: dstp[1] = src[1] | attrid; /* fallthrough */
        case 1: dstp[0] = src[0] | attrid; /* fallthrough */
        default:;
    }

}

#ifdef __SSE2__
static inline __m128i unpack_u8x4_to_cells(const void *pdata, __m128i zero, __m128i attr) {
    uint32_t data;
    memcpy(&data, pdata, sizeof data);
    return _mm_or_si128(attr, _mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_set1_epi32(data), zero), zero));
}
#endif

HOT
void copy_ascii_to_cells(struct cell *dst, const uint8_t *src, const uint8_t *end, uint32_t attrid) {
    uint32_t *restrict dstp = (uint32_t *)dst;
    attrid <<= 20;

    if (UNLIKELY(end - src < 4))
        goto short_copy;

    int32_t pref = (-(uintptr_t)dstp & 15ULL) / sizeof(uint32_t);

#ifdef __SSE2__
    const __m128i four_attrs = _mm_set1_epi32(attrid);
    const __m128i zero = _mm_set1_epi32(0);

    if (pref) {
        /* Write unaligned prefix separately */
        _mm_storeu_si128((__m128i *)dstp,
                         unpack_u8x4_to_cells(src, zero, four_attrs));

        src += pref;
        dstp += pref;
    }

    register ssize_t blocks = (end - src)/4;

    for (ssize_t i = 0; i < blocks; i++)
        _mm_store_si128((__m128i *)(dstp + i*4),
                        unpack_u8x4_to_cells(src + i*4, zero, four_attrs));

#else
    switch (pref) {
        case 3: dstp[2] = src[2] | attrid; /* fallthrough */
        case 2: dstp[1] = src[1] | attrid; /* fallthrough */
        case 1: dstp[0] = src[0] | attrid; /* fallthrough */
        default:;
    }

    src += pref;
    dstp += pref;

    register ssize_t blocks = (end - src)/4;

    for (ssize_t i = 0; i < blocks; i++) {
        dstp[4*i + 0] = attrid | src[4*i + 0];
        dstp[4*i + 1] = attrid | src[4*i + 1];
        dstp[4*i + 2] = attrid | src[4*i + 2];
        dstp[4*i + 3] = attrid | src[4*i + 3];
    }

#endif

    src += blocks*4;
    dstp += blocks*4;

short_copy:
    switch ((end - src)) {
        case 3: dstp[2] = src[2] | attrid; /* fallthrough */
        case 2: dstp[1] = src[1] | attrid; /* fallthrough */
        case 1: dstp[0] = src[0] | attrid; /* fallthrough */
        default:;
    }

}
