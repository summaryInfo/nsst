/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#include "feature.h"

#include "line.h"
#include "util.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_LINE_LEN 16384
#define MAX_EXTRA_PALETTE (0x10000 - PALETTE_SIZE)
#define CAPS_INC_STEP(sz) MIN(MAX_EXTRA_PALETTE, (sz) ? 8*(sz)/5 : 4)

static bool optimize_line_palette(struct line *line) {
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
    }

    return 1;
}

color_id_t alloc_color(struct line *line, color_t col) {
    if (line->pal) {
        if (line->pal->size > 0 && line->pal->data[line->pal->size - 1] == col)
            return PALETTE_SIZE + line->pal->size - 1;
        if (line->pal->size > 1 && line->pal->data[line->pal->size - 2] == col)
            return PALETTE_SIZE + line->pal->size - 2;
    }

    if (!line->pal || line->pal->size + 1 > line->pal->caps) {
        if (!optimize_line_palette(line)) return SPECIAL_BG;
        if (!line->pal || line->pal->size + 1 >= line->pal->caps) {
            if (line->pal && line->pal->caps == MAX_EXTRA_PALETTE) return SPECIAL_BG;
            size_t newc = CAPS_INC_STEP(line->pal ? line->pal->caps : 0);
            struct line_palette *new = realloc(line->pal, sizeof(struct line_palette) + newc * sizeof(color_t));
            if (!new) return SPECIAL_BG;
            if (!line->pal) new->size = 0;
            new->caps = newc;
            line->pal = new;
        }
    }

    line->pal->data[line->pal->size++] = col;
    return PALETTE_SIZE + line->pal->size - 1;
}

struct line *create_line(struct sgr sgr, ssize_t width) {
    struct line *line = malloc(sizeof(*line) + (size_t)width * sizeof(line->cell[0]));
    if (line) {
        line->width = width;
        line->pal = NULL;
        line->wrapped = 0;
        line->force_damage = 0;
        struct cell cel = fixup_color(line, sgr);
        for (ssize_t i = 0; i < width; i++)
            line->cell[i] = cel;
    } else warn("Can't allocate line");
    return line;
}

struct line *realloc_line(struct line *line, struct sgr sgr, ssize_t width) {
    struct line *new = realloc(line, sizeof(*new) + (size_t)width * sizeof(new->cell[0]));
    if (!new) die("Can't create lines");

    if (width > new->width) {
        struct cell cell = fixup_color(new, sgr);
        cell.attr = 0;

        for (ssize_t i = new->width; i < width; i++)
            new->cell[i] = cell;
    }

    new->width = width;
    return new;
}

struct line *concat_line(struct line *src1, struct line *src2, bool opt) {
    ssize_t llen = MAX(line_length(src2 ? src2 : src1), 1);
    if (src2) {
        ssize_t oldw = src1->width;

        if (llen + oldw > MAX_LINE_LEN) return NULL;

        src1 = realloc_line(src1, (struct sgr){0}, oldw + llen);

        for (ssize_t k = 0; k < llen; k++)
            copy_cell(src1, oldw + k, src2, k, 0);

        src1->wrapped = src2->wrapped;

        free_line(src2);
    } else if (opt && src1->width != llen) {
        src1 = realloc_line(src1, (struct sgr){0}, llen);
    }

    if (opt && src1->pal) {
        optimize_line_palette(src1);
        struct line_palette *pal = realloc(src1->pal, sizeof(struct line_palette) + sizeof(color_t)*(src1->pal->size));
        if (pal) {
            src1->pal = pal;
            pal->caps = pal->size;
        } else warn("Can't allocate palette");
    }

    return src1;
}

