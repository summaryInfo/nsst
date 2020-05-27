/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#include "feature.h"

#include "util.h"
#include "image.h"

#include <stdint.h>

void nss_image_draw_rect(nss_image_t im, nss_rect_t rect, nss_color_t fg) {
    if (intersect_with(&rect, &(nss_rect_t){0, 0, im.width, im.height})) {
        for (size_t j = 0; j < (size_t)rect.height; j++) {
            for (size_t i = 0; i < (size_t)rect.width; i++) {
                im.data[(rect.y + j) * im.width + (rect.x + i)] = fg;
            }
        }
    }
}
void nss_image_compose_glyph(nss_image_t im, int16_t dx, int16_t dy, nss_glyph_t *glyph, nss_color_t fg, nss_rect_t clip) {
    nss_rect_t rect = { dx - glyph->x, dy - glyph->y, glyph->width, glyph->height };
    if (intersect_with(&rect, &(nss_rect_t){0, 0, im.width, im.height}) &&
            intersect_with(&rect, &clip)) {
        int16_t i0 = rect.x - dx + glyph->x, j0 = rect.y - dy + glyph->y;
        if (glyph->pixmode == nss_pm_mono) {
            for (size_t j = 0; j < (size_t)rect.height; j++) {
                for (size_t i = 0; i < (size_t)rect.width; i++) {
                    uint8_t alpha = glyph->data[(j0 + j) * glyph->stride + i0 + i];
                    nss_color_t *bg = &im.data[(rect.y + j) * im.width + (rect.x + i)];
                    *bg =
                        (((*bg >>  0) & 0xFF) * (255 - alpha) + ((fg >>  0) & 0xFF) * alpha) / 255 << 0 |
                        (((*bg >>  8) & 0xFF) * (255 - alpha) + ((fg >>  8) & 0xFF) * alpha) / 255 << 8 |
                        (((*bg >> 16) & 0xFF) * (255 - alpha) + ((fg >> 16) & 0xFF) * alpha) / 255 << 16 |
                        (((*bg >> 24) & 0xFF) * (255 - alpha) + ((fg >> 24) & 0xFF) * alpha) / 255 << 24;
                }
            }
        } else {
            for (size_t j = 0; j < (size_t)rect.height; j++) {
                for (size_t i = 0; i < (size_t)rect.width; i++) {
                    uint8_t *alpha = &glyph->data[(j0 + j) * glyph->stride + 4 * (i0 + i)];
                    nss_color_t *bg = &im.data[(rect.y + j) * im.width + (rect.x + i)];
                    *bg =
                        (((*bg >>  0) & 0xFF) * (255 - alpha[0]) + ((fg >>  0) & 0xFF) * alpha[0]) / 255 << 0 |
                        (((*bg >>  8) & 0xFF) * (255 - alpha[1]) + ((fg >>  8) & 0xFF) * alpha[1]) / 255 << 8 |
                        (((*bg >> 16) & 0xFF) * (255 - alpha[2]) + ((fg >> 16) & 0xFF) * alpha[2]) / 255 << 16 |
                        (((*bg >> 24) & 0xFF) * (255 - alpha[3]) + ((fg >> 24) & 0xFF) * alpha[3]) / 255 << 24;
                }
            }

        }
    }
}

void nss_image_copy(nss_image_t im, nss_rect_t rect, nss_image_t src, int16_t sx, int16_t sy) {
    rect.width = MAX(0, MIN(rect.width + sx, src.width) - sx);
    rect.height = MAX(0, MIN(rect.height + sy, src.height) - sy);

    if (intersect_with(&rect, &(nss_rect_t){0, 0, im.width, im.height})) {
        if (rect.y < sy || (rect.y == sy && rect.x <= sx)) {
            for (size_t j = 0; j < (size_t)rect.height; j++) {
                for (size_t i = 0; i < (size_t)rect.width; i++) {
                    im.data[(rect.y + j) * im.width + (rect.x + i)] = src.data[src.width * (sy + j) + (sx + i)];
                }
            }
        } else {
            for (size_t j = rect.height; j > 0; j--) {
                for (size_t i = rect.width; i > 0; i--) {
                    im.data[(rect.y + j - 1) * im.width + (rect.x + i - 1)] = src.data[src.width * (sy + j - 1) + (sx + i - 1)];
                }
            }
        }
    }
}
