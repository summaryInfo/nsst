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
#define MAX_EXTRA_PALETTE (0x10000 - PALETTE_SIZE - 1)
#define INIT_CAP 4
#define CAPS_INC_STEP(sz) MIN(MAX_EXTRA_PALETTE, MAX(3*(sz)/2, INIT_CAP))

static bool optimize_line_palette(struct line *line, color_id_t *preserve) {
    // Buffer here causes a leak in theory
    static color_id_t *buf = NULL, buf_len = 0, *new;

    if (!line) {
        free(buf);
        buf = NULL;
        buf_len = 0;
        return 0;
    }

    if (line->pal) {
        if (buf_len < line->pal->size) {
            if (!(new = realloc(buf, line->pal->size * sizeof(color_id_t)))) return 0;
            memset(new + buf_len, 0xFF, (line->pal->size - buf_len) * sizeof(color_id_t));
            buf_len = line->pal->size, buf = new;
        }
        color_id_t k = PALETTE_SIZE, *pbuf = buf - PALETTE_SIZE;
        for (ssize_t i = 0; i < line->width; i++) {
            struct cell *cel = &line->cell[i];
            if (cel->fg >= PALETTE_SIZE) pbuf[cel->fg] = 0;
            if (cel->bg >= PALETTE_SIZE) pbuf[cel->bg] = 0;
        }

        if (preserve && *preserve >= PALETTE_SIZE) pbuf[*preserve] = 0;

        color_t *pal = line->pal->data - PALETTE_SIZE;
        for (color_id_t i = PALETTE_SIZE; i < line->pal->size + PALETTE_SIZE; i++) {
            if (!pbuf[i]) {
                pal[k] = pal[i];
                for (color_id_t j = i + 1; j < line->pal->size + PALETTE_SIZE; j++)
                    if (pal[k] == pal[j]) pbuf[j] = k;
                pbuf[i] = k++;
            }
        }
        line->pal->size = k - PALETTE_SIZE;

        for (ssize_t i = 0; i < line->width; i++) {
            struct cell *cel = &line->cell[i];
            if (cel->fg >= PALETTE_SIZE) cel->fg = pbuf[cel->fg];
            if (cel->bg >= PALETTE_SIZE) cel->bg = pbuf[cel->bg];
        }

        if (preserve && *preserve >= PALETTE_SIZE) *preserve = pbuf[*preserve];
    }

    return 1;
}

color_id_t alloc_color(struct line *line, color_t col, color_id_t *pre) {
    if (line->pal) {
        if (line->pal->size > 0 && line->pal->data[line->pal->size - 1] == col)
            return PALETTE_SIZE + line->pal->size - 1;
        if (line->pal->size > 1 && line->pal->data[line->pal->size - 2] == col)
            return PALETTE_SIZE + line->pal->size - 2;
    }

    if (!line->pal || line->pal->size + 1 > line->pal->caps) {
        if (!optimize_line_palette(line, pre)) return SPECIAL_BG;
        if (!line->pal || line->pal->size + 1 > line->pal->caps) {
            if (line->pal && line->pal->caps == MAX_EXTRA_PALETTE) return SPECIAL_BG;
            size_t newc = line->pal ? CAPS_INC_STEP(line->pal->caps) : INIT_CAP;
            struct line_palette *new = realloc(line->pal, sizeof(struct line_palette) + newc * sizeof(color_t));
            if (!new) return SPECIAL_BG;
            if (!line->pal) new->size = 0;
            new->caps = newc;
            line->pal = new;
        }
    }

    line->pal->data[line->pal->size] = col;
    return PALETTE_SIZE + line->pal->size++;
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

struct line *create_line(struct sgr sgr, ssize_t width) {
    struct line *line = malloc(sizeof(*line) + (size_t)width * sizeof(line->cell[0]));
    if (line) {
        line->pal = NULL;
        line->wrapped = 0;
        line->force_damage = 0;
        line->width = 0;
        if (width) {
            struct cell cel = fixup_color(line, sgr);
            cel.attr = 0;
            fill_cells(line->cell, cel, width);
        }
        line->width = width;
    } else warn("Can't allocate line");
    return line;
}

struct line *realloc_line(struct line *line, ssize_t width) {
    struct line *new = realloc(line, sizeof(*new) + (size_t)width * sizeof(new->cell[0]));
    if (!new) die("Can't create lines");

    if (width > new->width) {
        struct cell cel = new->cell[new->width - 1];
        cel.attr = 0;
        cel.ch = 0;
        fill_cells(new->cell + new->width, cel, width - new->width);
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

        for (ssize_t k = 0; k < llen; k++)
            copy_cell(src1, oldw + k, src2, k, 0);

        src1->wrapped = src2->wrapped;

        free_line(src2);
    } else if (opt) {
        ssize_t llen = MAX(line_length(src1), 1);
        if (llen != src1->width)
            src1 = realloc_line(src1, llen);
    }

    if (opt && src1->pal) {
        optimize_line_palette(src1, NULL);
        struct line_palette *pal = realloc(src1->pal, sizeof(struct line_palette) + sizeof(color_t)*(src1->pal->size));
        if (pal) {
            src1->pal = pal;
            pal->caps = pal->size;
        } else warn("Can't allocate palette");
    }

    return src1;
}

