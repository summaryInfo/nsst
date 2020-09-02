/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#ifndef FONT_H_
#define FONT_H_ 1

#include "feature.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// IMPORTANT: Order should be the same as
// in enum cell_attr
enum face_name {
    face_normal = 0,
    face_italic = 1 << 0,
    face_bold = 1 << 1,
    face_bold_italic = face_bold | face_italic,
    face_MAX = 4,
};

enum pixel_mode {
    pixmode_mono,
    pixmode_bgr,
    pixmode_rgb,
    pixmode_bgrv,
    pixmode_rgbv,
};

struct glyph {
    // Hash table data
    struct glyph *next;
    uint32_t g;

    uint16_t width, height;
    int16_t x, y;
    int16_t x_off, y_off;
    int16_t stride;

    int16_t pixmode;
    uint8_t data[];
};


struct font *create_font(const char* descr, double size, double dpi, double gamma, bool force_scalable);
void free_font(struct font *font);
struct font *font_ref(struct font *font);
struct glyph *font_render_glyph(struct font *font, enum pixel_mode ord, uint32_t ch, enum face_name attr);
int16_t font_get_size(struct font *font);

struct glyph_cache *create_glyph_cache(struct font *font, enum pixel_mode, int16_t vspacing, int16_t hspacing, bool boxdraw);
struct glyph_cache *glyph_cache_ref(struct glyph_cache *ref);
void free_glyph_cache(struct glyph_cache *cache);
struct glyph *glyph_cache_fetch(struct glyph_cache *cache, uint32_t ch, enum face_name face);
void glyph_cache_get_dim(struct glyph_cache *cache, int16_t *w, int16_t *h, int16_t *d);
bool glyph_cache_is_fetched(struct glyph_cache *cache, uint32_t ch);

#endif

