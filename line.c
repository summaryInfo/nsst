/* Copyright (c) 2019-2022, Evgeny Baskov. All rights reserved */

#include "feature.h"

#include "line.h"
#include "util.h"
#include "hashtable.h"

#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_LINE_LEN 16384
#define MAX_EXTRA_PALETTE (ATTRID_MAX - 1)
#define INIT_CAP 4
#define CAPS_INC_STEP(sz) MIN(MAX_EXTRA_PALETTE, MAX(3*(sz)/2, INIT_CAP))

const struct attr default_attr__ = {
        .fg = SPECIAL_FG + 1,
        .bg = SPECIAL_BG + 1,
        .ul = SPECIAL_BG + 1
};

inline static bool attr_eq_prot(const struct attr *a, const struct attr *b) {
    static_assert(sizeof(struct attr) == 4*sizeof(uint32_t), "Wrong attribute size");
    return a->fg == b->fg && a->bg == b->bg && a->ul == b->ul && a->mask == b->mask;
}

inline static uint32_t attr_hash(struct attr *attr) {
    return uint_hash32(attr->bg) ^
            uint_hash32(attr->fg) ^
            uint_hash32(attr->ul) ^
            uint_hash32(attr->mask);
}

inline static bool attr_empty(struct attr *attr) {
    return attr->fg == 0;
}

static uint32_t insert_attr(struct line_attr *tab, struct attr *attr, uint32_t hash) {
    size_t i = hash % tab->caps;
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

static void move_attrtab(struct line_attr *dst, struct line *src) {
    for (ssize_t i = 0; i < src->size; i++) {
        uint32_t old_id = src->cell[i].attrid;
        if (old_id == ATTRID_DEFAULT) continue;

        struct attr *at = &src->attrs->data[old_id - 1];

        if (!attr_empty(at)) {
            at->bg = insert_attr(dst, at, attr_hash(at));
            at->fg = 0;
            at->uri = EMPTY_URI;
        }

        src->cell[i].attrid = at->bg;
    }

    if (src->pad_attrid != ATTRID_DEFAULT) {
        struct attr *pad = &src->attrs->data[src->pad_attrid - 1];
        if (!attr_empty(pad))
            pad->bg = insert_attr(dst, pad, attr_hash(pad));
        src->pad_attrid = pad->bg;
    }

    free_attrs(src->attrs);
    src->attrs = dst;
}

uint32_t alloc_attr(struct line *line, struct attr attr) {
    if (attr_eq_prot(&attr, &ATTR_DEFAULT)) return ATTRID_DEFAULT;

    uint32_t hash = attr_hash(&attr);

    if (!line->attrs) {
        line->attrs = calloc(sizeof *line->attrs + INIT_CAP * sizeof *line->attrs->data, 1);
        if (!line->attrs) return ATTRID_DEFAULT;
        line->attrs->caps = INIT_CAP;
    }

#if USE_URI
    uri_ref(attr.uri);
#endif

    uint32_t id = insert_attr(line->attrs, &attr, hash);
    if (id) return id;

    size_t new_caps = CAPS_INC_STEP(line->attrs->caps);
    struct line_attr *new = calloc(sizeof *new + new_caps * sizeof *new->data, 1);
    if (!new) {
#if USE_URI
        uri_unref(attr.uri);
#endif
        return ATTRID_DEFAULT;
    }

    new->caps = new_caps;

    move_attrtab(new, line);

    return insert_attr(line->attrs, &attr, hash);
}

struct line *create_line(struct attr attr, ssize_t caps) {
    struct line *line = malloc(sizeof(*line) + (size_t)caps * sizeof(line->cell[0]));
    if (!line) die("Can't allocate line");

    memset(line, 0, sizeof *line);

    line->pad_attrid = alloc_attr(line, attr);
    line->force_damage = 1;
    line->caps = caps;
    return line;
}

struct line *realloc_line(struct line *line, ssize_t caps) {
    struct line *new = realloc(line, sizeof(*new) + (size_t)caps * sizeof(new->cell[0]));
    if (!new) die("Can't create lines");


    new->size = MIN(caps, new->size);
    new->caps = caps;

    return new;
}

static void optimize_attributes(struct line *line) {

    if (!line->attrs) return;

    uint64_t used[(MAX_EXTRA_PALETTE + 1)/64] = {0};

    used[line->pad_attrid / 64] |= 1ULL << (line->pad_attrid % 64);

    for (ssize_t i = 0; i < line->size; i++) {
        uint64_t id = line->cell[i].attrid;
        used[id / 64] |= 1ULL << (id % 64);
    }

    ssize_t cnt = -(used[0] & 1);
    for (size_t i = 0; i < sizeof used/sizeof *used; i++)
        cnt += __builtin_popcountll(used[i]);

    if (cnt) {
        struct line_attr *new = calloc(sizeof *new + cnt*sizeof *new->data, 1);
        if (!new) return;
        new->caps = cnt;

        move_attrtab(new, line);
    } else {
        free_attrs(line->attrs);
        line->attrs = NULL;
    }
}

struct line *concat_line(struct line *src1, struct line *src2, bool opt) {
    if (src2) {
        assert(src1->wrapped);
        ssize_t len = MIN(src2->size + src1->size, MAX_LINE_LEN);
        src1 = realloc_line(src1, len);
        src1->pad_attrid = alloc_attr(src1, attr_pad(src2));
        src1->wrapped = src2->wrapped;
        copy_line(src1, src1->size, src2, 0, len - src1->size);
        free_line(src2);
    } else if (opt && src1->size != src1->caps) {
        src1 = realloc_line(src1, src1->size);
    }

    if (opt) optimize_attributes(src1);
    return src1;
}

void HOT copy_line(struct line *dst, ssize_t dx, struct line *src, ssize_t sx, ssize_t len) {
    struct cell *sc = src->cell + sx, *dc = dst->cell + dx, c;

    assert(sx + len <= src->size);
    assert(dx + len <= dst->caps);

    if (dst != src) {
        uint32_t previd = ATTRID_MAX, newid = 0;
        for (ssize_t i = 0; i < len; i++) {
            c = *sc++;
            c.drawn = 0;
            if (UNLIKELY(c.attrid)) {
                if (UNLIKELY(c.attrid != previd))
                    newid = alloc_attr(dst, src->attrs->data[c.attrid - 1]);
                c.attrid = newid;
            }
            *dc++ = c;

            if (dc - dst->cell > dst->size)
                dst->size = dc - dst->cell;
        }
    } else {
        memmove(dc, sc, len * sizeof(*sc));
        while (len--) dc++->drawn = 0;
    }
    dst->size = MAX(dst->size, sx + len);
}

