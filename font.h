/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#ifndef FONT_H_
#define FONT_H_ 1

#include "feature.h"

#include <stddef.h>
#include <stdint.h>

// IMPORTANT: Order should be the same as
// in nss_attrib_flags_t
typedef enum nss_font_attrib {
    nss_font_attrib_normal = 0,
    nss_font_attrib_italic = 1 << 0,
    nss_font_attrib_bold = 1 << 1,
    nss_font_attrib_mask = 3,
    nss_font_attrib_max = 4,
} nss_font_attrib_t;

typedef enum nss_pixel_mode {
    nss_pm_mono,
    nss_pm_bgr,
    nss_pm_rgb,
    nss_pm_bgrv,
    nss_pm_rgbv,
} nss_pixel_mode_t;

typedef struct nss_glyph {
    // Hash table data
    struct nss_glyph *next;
    uint32_t g;

    uint16_t width, height;
    int16_t x, y;
    int16_t x_off, y_off;
    int16_t stride;

    int16_t pixmode;
    uint8_t data[];
} nss_glyph_t;

typedef uint32_t nss_char_t;
typedef struct nss_font nss_font_t;

nss_font_t *nss_create_font(const char* descr, double size);
void nss_free_font(nss_font_t *font);
nss_font_t *nss_font_reference(nss_font_t *font);
nss_glyph_t *nss_font_render_glyph(nss_font_t *font, uint32_t ch, nss_font_attrib_t face);
int16_t nss_font_get_size(nss_font_t *font);

typedef struct nss_glyph_cache nss_glyph_cache_t;

nss_glyph_cache_t *nss_create_cache(nss_font_t *font);
nss_glyph_cache_t *nss_cache_reference(nss_glyph_cache_t *ref);
void nss_free_cache(nss_glyph_cache_t *cache);
nss_glyph_t *nss_cache_fetch(nss_glyph_cache_t *cache, uint32_t ch, nss_font_attrib_t face);
void nss_cache_font_dim(nss_glyph_cache_t *cache, int16_t *w, int16_t *h, int16_t *d);
_Bool nss_cache_is_fetched(nss_glyph_cache_t *cache, nss_char_t ch);

#endif

