/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

#ifndef FONT_H_
#define FONT_H_ 1

#include "feature.h"

#include "hashtable.h"
#include "util.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* IMPORTANT: Order should be the same as
 * in enum cell_attr */
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
    pixmode_bgra,
};

#define GLYPH_ALIGNMENT MAX(16, MALLOC_ALIGNMENT)

struct glyph {
    ht_head_t head;

#ifdef USE_XRENDER
    uint32_t id;
#endif

    uint32_t g;
    uint16_t width, height;
    int16_t x, y;
    int16_t x_off, y_off;
    int16_t stride;

    int16_t pixmode;
    _Alignas(GLYPH_ALIGNMENT) uint8_t data[];
};

#define GLYPH_UNDERCURL UINT32_MAX

struct font *create_font(const char* descr, double size, double dpi, double gamma, bool force_scalable, bool allow_subst);
void free_font(struct font *font);
struct font *font_ref(struct font *font);
int16_t font_get_size(struct font *font);

struct glyph_cache *create_glyph_cache(struct font *font, enum pixel_mode pixmode, int16_t vspacing, int16_t hspacing, int16_t underline_width, bool boxdraw, bool force_aligned);
struct glyph_cache *glyph_cache_ref(struct glyph_cache *ref);
void free_glyph_cache(struct glyph_cache *cache);
void glyph_cache_get_dim(struct glyph_cache *cache, int16_t *w, int16_t *h, int16_t *d);
struct glyph *glyph_cache_fetch(struct glyph_cache *cache, uint32_t ch, enum face_name face, bool *is_new);

#endif
