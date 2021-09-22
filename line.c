/* Copyright (c) 2019-2021, Evgeny Baskov. All rights reserved */

#include "feature.h"

#include "line.h"
#include "util.h"

#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_LINE_LEN 16384
#define MAX_EXTRA_PALETTE (ATTRID_MAX - 1)
#define INIT_CAP 4
#define CAPS_INC_STEP(sz) MIN(MAX_EXTRA_PALETTE, MAX(3*(sz)/2, INIT_CAP))

inline static bool attr_eq_prot(struct attr *a, struct attr *b) {
    static_assert(sizeof(struct attr) == 4*sizeof(uint32_t), "Wrong attribute size");
    return a->fg == b->fg && a->bg == b->bg && a->ul == b->ul && a->mask == b->mask;
}

static void optimize_attributes(struct line *line) {
    static uint32_t buf[ATTRID_MAX];
    static bool filled;
    if (UNLIKELY(!filled)) {
        memset(buf, 0xFF, sizeof buf);
        filled = 1;
    }

    if (line->attrs) {
        uint32_t k = 1, *pbuf = buf - 1;
        for (ssize_t i = 0; i < line->width; i++) {
            uint32_t id = line->cell[i].attrid;
            if (id) pbuf[id] = 0;
        }

        struct attr *pal = line->attrs->data - 1;
        for (ssize_t i = 1; i <= line->attrs->size; i++) {
            if (!pbuf[i]) {
                pal[k] = pal[i];
                for (ssize_t j = i + 1; j <= line->attrs->size; j++)
                    if (attr_eq_prot(pal + k, pal + j)) pbuf[j] = k;
                pbuf[i] = k++;
            }
#if USE_URI
            else uri_unref(pal[i].uri);
#endif
        }
        line->attrs->size = k - 1;

        for (ssize_t i = 0; i < line->width; i++) {
            struct cell *c = &line->cell[i];
            if (c->attrid) c->attrid = pbuf[c->attrid];
        }
    }
}

uint32_t alloc_attr(struct line *line, struct attr attr) {
    if (attr_eq_prot(&attr, &ATTR_DEFAULT)) return 0;
    if (line->attrs && line->attrs->size && attr_eq_prot(line->attrs->data + line->attrs->size - 1, &attr)) return line->attrs->size;

#if USE_URI
    uri_ref(attr.uri);
#endif

    if (!line->attrs || line->attrs->size + 1 >= line->attrs->caps) {
        optimize_attributes(line);
        if (!line->attrs || line->attrs->size + 1 >= line->attrs->caps) {
            if (line->attrs && line->attrs->caps == MAX_EXTRA_PALETTE) return ATTRID_DEFAULT;
            size_t newc = line->attrs ? CAPS_INC_STEP(line->attrs->caps) : INIT_CAP;
            struct line_attr *new = realloc(line->attrs, sizeof(*new) + newc * sizeof(*new->data));
            if (!new) {
                uri_unref(attr.uri);
                return ATTRID_DEFAULT;
            }
            if (!line->attrs) new->size = 0;
            new->caps = newc;
            line->attrs = new;
        }
    }

    line->attrs->data[line->attrs->size++] = attr;
    return line->attrs->size;
}

struct line *create_line(struct attr attr, ssize_t width) {
    struct line *line = malloc(sizeof(*line) + (size_t)width * sizeof(line->cell[0]));
    if (!line) die("Can't allocate line");
    memset(line, 0, sizeof(*line));
    struct cell c = { .attrid = alloc_attr(line, attr) };
    fill_cells(line->cell, c, width);
    line->width = width;
    return line;
}

struct line *realloc_line(struct line *line, ssize_t width) {
    struct line *new = realloc(line, sizeof(*new) + (size_t)width * sizeof(new->cell[0]));
    if (!new) die("Can't create lines");

    if (width > new->width) {
        fill_cells(new->cell + new->width,
                MKCELL(0, new->cell[new->width - 1].attrid), width - new->width);
    }

    new->width = width;
    return new;
}

struct line *concat_line(struct line *src1, struct line *src2, bool opt) {
    if (src2) {
        ssize_t llen = MAX(src2->mwidth, 1);
        ssize_t oldw = src1->width;

        if (llen + oldw > MAX_LINE_LEN) return NULL;

        src1 = realloc_line(src1, oldw + llen);

        copy_line(src1, oldw, src2, 0, llen, 1);

        src1->wrapped = src2->wrapped;

        free_line(src2);
    } else if (opt) {
        ssize_t llen = MAX(src1->mwidth, 1);
        if (llen != src1->width)
            src1 = realloc_line(src1, llen);
    }

    if (opt && src1->attrs) {
        optimize_attributes(src1);
        struct line_attr *attrs = realloc(src1->attrs, sizeof(*attrs) + sizeof(*attrs->data)*(src1->attrs->size));
        if (attrs) {
            src1->attrs = attrs;
            attrs->caps = attrs->size;
        } else warn("Can't allocate palette");
    }

    return src1;
}

void HOT copy_line(struct line *dst, ssize_t dx, struct line *src, ssize_t sx, ssize_t len, bool dmg) {
    struct cell *sc = src->cell + sx, *dc = dst->cell + dx, c;
    if (dst != src) {
        uint32_t previd = ATTRID_MAX, newid = 0;
        if (dmg) {
            for (ssize_t i = 0; i < len; i++) {
                c = *sc++;
                c.drawn = 0;
                if (UNLIKELY(c.attrid)) {
                    if (UNLIKELY(c.attrid != previd))
                        newid = alloc_attr(dst, src->attrs->data[c.attrid - 1]);
                    c.attrid = newid;
                }
                *dc++ = c;
            }
        } else {
            for (ssize_t i = 0; i < len; i++) {
                c = *sc++;
                if (UNLIKELY(c.attrid)) {
                    if (UNLIKELY(c.attrid != previd))
                        newid = alloc_attr(dst, src->attrs->data[c.attrid - 1]);
                    c.attrid = newid;
                }
                *dc++ = c;
            }
        }
    } else {
        memmove(dc, sc, len * sizeof(*sc));
        if (dmg) while (len--) dc++->drawn = 0;
    }
    dst->mwidth = MAX(dst->mwidth, sx + len);
}

