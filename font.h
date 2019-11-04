#ifndef FONT_H_
#define FONT_H_ 1

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
    uint16_t width, height;
    int16_t x, y;
    int16_t x_off, y_off;
    int16_t stride;
    uint8_t data[];
} nss_glyph_t;

typedef struct nss_font nss_font_t;

nss_font_t *nss_create_font(const char* descr, double size, uint16_t dpi);
void nss_free_font(nss_font_t *font);
nss_font_t *nss_font_reference(nss_font_t *font);
nss_glyph_t *nss_font_render_glyph(nss_font_t *font, uint32_t ch, nss_font_attrib_t face, _Bool lcd);
int16_t nss_font_get_size(nss_font_t *font);
_Bool nss_font_glyph_is_loaded(nss_font_t *font, uint32_t ch);
_Bool nss_font_glyph_mark_loaded(nss_font_t *font, uint32_t ch);

#endif

