/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#ifndef IMAGE_H_
#define IMAGE_H_ 1

#include "feature.h"

#include "font.h"
#include "util.h"

#include <stdint.h>

struct image {
    int16_t width;
    int16_t height;
    int shmid;
    color_t *data;
};

void image_draw_rect(struct image im, struct rect rect, color_t fg);
void image_compose_glyph(struct image im, int16_t dx, int16_t dy, struct glyph *glyph, color_t fg, struct rect clip);
void image_copy(struct image im, struct rect rect, struct image src, int16_t sx, int16_t sy);

#endif

