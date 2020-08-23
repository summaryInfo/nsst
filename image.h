/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#ifndef IMAGE_H_
#define IMAGE_H_ 1

#include "feature.h"

#include "font.h"
#include "util.h"

#include <stdint.h>

typedef struct nss_image {
    int16_t width;
    int16_t height;
    int shmid;
    color_t *data;
} nss_image_t;

void nss_image_draw_rect(nss_image_t im, nss_rect_t rect, color_t fg);
void nss_image_compose_glyph(nss_image_t im, int16_t dx, int16_t dy, nss_glyph_t *glyph, color_t fg, nss_rect_t clip);
void nss_image_copy(nss_image_t im, nss_rect_t rect, nss_image_t src, int16_t sx, int16_t sy);

#endif

