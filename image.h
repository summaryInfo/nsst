#ifndef IMAGE_H_
#define IMAGE_H_ 1

#include <stdint.h>
#include "util.h"
#include "font.h"

typedef struct nss_image {
    int16_t width;
    int16_t height;
    uint32_t shmid;
    nss_color_t *data;
} nss_image_t;

void nss_image_draw_rect(nss_image_t im, nss_rect_t rect, nss_color_t fg);
void nss_image_composite_glyph(nss_image_t im, int16_t dx, int16_t dy, nss_glyph_t *glyph, nss_color_t fg, nss_rect_t clip, _Bool lcd);
void nss_image_copy(nss_image_t im, nss_rect_t rect, nss_image_t src, int16_t sx, int16_t sy);

#endif

