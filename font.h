/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#ifndef FONT_H_
#define FONT_H_ 1

#include "features.h"

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

typedef struct nss_glyph {
#ifdef USE_X11SHM
    // Tree elements
    struct nss_glyph *l,*r,*p;
    uint32_t g;
#endif

    uint16_t width, height;
    int16_t x, y;
    int16_t x_off, y_off;
    int16_t stride;
    uint8_t data[];
} nss_glyph_t;

typedef uint32_t tchar_t;
typedef struct nss_font nss_font_t;

nss_font_t *nss_create_font(const char* descr, double size, uint16_t dpi);
void nss_free_font(nss_font_t *font);
nss_font_t *nss_font_reference(nss_font_t *font);
nss_glyph_t *nss_font_render_glyph(nss_font_t *font, uint32_t ch, nss_font_attrib_t face, _Bool lcd);
int16_t nss_font_get_size(nss_font_t *font);

#ifdef USE_X11SHM
typedef struct nss_glyph_cache nss_glyph_cache_t;

nss_glyph_cache_t *nss_create_cache(nss_font_t *font, _Bool lcd);
nss_glyph_cache_t *nss_cache_reference(nss_glyph_cache_t *ref);
void nss_free_cache(nss_glyph_cache_t *cache);
void nss_cache_font_dim(nss_glyph_cache_t *cache, int16_t *w, int16_t *h, int16_t *d);
nss_glyph_t *nss_cache_fetch(nss_glyph_cache_t *cache, uint32_t ch, nss_font_attrib_t face);
#else
_Bool nss_font_glyph_is_loaded(nss_font_t *font, tchar_t ch);
void nss_font_glyph_mark_loaded(nss_font_t *font, tchar_t ch);
#endif

#endif

