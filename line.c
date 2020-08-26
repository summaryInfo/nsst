/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#include "feature.h"

#include "line.h"
#include "util.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_LINE_LEN 16384
#define MAX_EXTRA_PALETTE 511
#define INIT_CAP 4
#define CAPS_INC_STEP(sz) MIN(MAX_EXTRA_PALETTE, MAX(3*(sz)/2, INIT_CAP))

inline static bool attr_eq_prot(struct attr *a, struct attr *b) {
    return attr_eq(a,b) && a->protected == b->protected;
}

static void optimize_attributes(struct line *line) {
    static uint32_t buf[ATTRID_MAX];
    static bool filled;
    if (!filled) {
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
        }
        line->attrs->size = k - 1;

        for (ssize_t i = 0; i < line->width; i++) {
            struct cell *c = &line->cell[i];
            if (c->attrid) c->attrid = pbuf[c->attrid];
        }
    }
}

uint32_t alloc_attr(struct line *line, struct attr attr) {
    if (attr_eq_prot(&attr, &(struct attr){ .fg = indirect_color(SPECIAL_FG), .bg = indirect_color(SPECIAL_BG)})) return 0;
    if (line->attrs && line->attrs->size && attr_eq_prot(line->attrs->data + line->attrs->size - 1, &attr)) return line->attrs->size;

    if (!line->attrs || line->attrs->size + 1 >= line->attrs->caps) {
        optimize_attributes(line);
        if (!line->attrs || line->attrs->size + 1 >= line->attrs->caps) {
            if (line->attrs && line->attrs->caps == MAX_EXTRA_PALETTE) return ATTRID_DEFAULT;
            size_t newc = line->attrs ? CAPS_INC_STEP(line->attrs->caps) : INIT_CAP;
            struct line_attr *new = realloc(line->attrs, sizeof(*new) + newc * sizeof(*new->data));
            if (!new) return ATTRID_DEFAULT;
            if (!line->attrs) new->size = 0;
            new->caps = newc;
            line->attrs = new;
        }
    }

    line->attrs->data[line->attrs->size++] = attr;
    return line->attrs->size;
}

inline static void fill_cells(struct cell *dst, struct cell c, ssize_t width) {
    ssize_t i = (width+7)/8, inc = width % 8;
    if (!inc) inc = 8;
    switch(inc) do {
        dst += inc;
        inc = 8;
        case 8: dst[7] = c;
        case 7: dst[6] = c;
        case 6: dst[5] = c;
        case 5: dst[4] = c;
        case 4: dst[3] = c;
        case 3: dst[2] = c;
        case 2: dst[1] = c;
        case 1: dst[0] = c;
    } while(--i > 0);
}

struct line *create_line(struct attr attr, ssize_t width) {
    struct line *line = malloc(sizeof(*line) + (size_t)width * sizeof(line->cell[0]));
    if (!line) die("Can't allocate line");
    line->width = 0;
    line->wrapped = 0;
    line->force_damage = 0;
    line->attrs = NULL;
    struct cell c = { .attrid = alloc_attr(line, attr) };
    for (ssize_t i = 0; i < width; i++) line->cell[i] = c;
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
        ssize_t llen = MAX(line_length(src2), 1);
        ssize_t oldw = src1->width;

        if (llen + oldw > MAX_LINE_LEN) return NULL;

        src1 = realloc_line(src1, oldw + llen);

        copy_line(src1, oldw, src2, 0, llen, 0);

        src1->wrapped = src2->wrapped;

        free_line(src2);
    } else if (opt) {
        ssize_t llen = MAX(line_length(src1), 1);
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

void copy_line(struct line *dst, ssize_t dx, struct line *src, ssize_t sx, ssize_t len, bool dmg) {
    uint32_t previd = ATTRID_MAX, newid = 0;
    struct cell *sc = src->cell + sx, *dc = dst->cell + dx;
    ssize_t dir = 1;
    if (dx > sx) {
        sc += len - 1;
        dc += len - 1;
        dir = -1;
    }
    for (ssize_t i = 0; i < len; i++) {
        struct cell c = *(sc += dir);
        if (dmg) c.drawn = 0;
        if (c.attrid && c.attrid != previd)
            newid = alloc_attr(dst, src->attrs->data[c.attrid - 1]);
        c.attrid = newid;
        *(dc += dir) = c;
    }
}

