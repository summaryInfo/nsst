#ifndef NSS_ATTR_H_
#define NSS_ATTR_H_ 1

#include <stdint.h>

typedef uint16_t nss_cid_t;
typedef uint32_t nss_color_t;
typedef nss_color_t* nss_palette_t;

#define NSS_CID_NONE UINT16_MAX
#define NSS_CID_DEFAULT 0
#define NSS_DEFAULT_PALETTE NULL
#define NSS_SPECIAL_COLORS 4
#define NSS_PALETTE_SIZE (256 + NSS_SPECIAL_COLORS)
#define NSS_SPECIAL_BG 256
#define NSS_SPECIAL_FG 257
#define NSS_SPECIAL_CURSOR_BG 258
#define NSS_SPECIAL_CURSOR_FG 259

typedef enum nss_attrs {
    nss_attrib_italic = 1 << 0,
    nss_attrib_bold = 1 << 1,
    nss_attrib_faint = 1 << 2,
    nss_attrib_underlined = 1 << 3,
    nss_attrib_strikethrough = 1 << 4,
    nss_attrib_invisible = 1 << 5,
    nss_attrib_inverse = 1 << 6,
    nss_attrib_blink = 1 << 7,
    nss_attrib_wide = 1 << 8,
    nss_attrib_wdummy = 1 << 9,
    nss_attrib_protected = 1 << 10
    // 11 bits total, sice unicode codepoint is 21 bit
} nss_attrs_t;

#define NSS_CELL_CHAR_BITS 21
#define NSS_CELL_CHAR_MASK ((1 << NSS_CELL_CHAR_BITS) - 1)
#define NSS_GLYPH_MASK (NSS_CELL_CHAR_MASK | (nss_font_attrib_mask << 21))
#define NSS_CELL_CHAR(s) ((s).ch & NSS_CELL_CHAR_MASK)
#define NSS_CELL_FG(s, b) (nss_color_get((s).fg + (b)))
#define NSS_CELL_BG(s, b) (nss_color_get((s).bg + (b)))
#define NSS_CELL_ATTRS_ZERO(s) ((s).ch &= NSS_CELL_CHAR_MASK)
#define NSS_CELL_ATTRS(s) ((s).ch >> NSS_CELL_CHAR_BITS)
#define NSS_CELL_ATTRSET(s, l) ((s).ch |= (l) << NSS_CELL_CHAR_BITS)
#define NSS_CELL_ATTRCLR(s, l) ((s).ch &= ~((l) << NSS_CELL_CHAR_BITS))
#define NSS_CELL_ATTR_INVERT(s, l) ((s).ch ^= (l) << NSS_CELL_CHAR_BITS)
#define NSS_MKCELLWITH(s, c) NSS_MKCELL((s).fg, (s).bg, NSS_CELL_ATTRS(s), c)
#define NSS_MKCELL(f, b, l, c) ((nss_cell_t) { .bg = (b), .fg = (f), .ch = ((c) & NSS_CELL_CHAR_MASK) | ((l) << NSS_CELL_CHAR_BITS)})
#define NSS_EQCELL(s, z) ((s).fg == (z).fg && (s).bg == (z).bg && NSS_CELL_ATTRS(s) == NSS_CELL_ATTRS(z))

typedef struct nss_cell {
        uint32_t ch; /* not really char but char + attributes */
        nss_cid_t fg;
        nss_cid_t bg;
} nss_cell_t;

void nss_init_color(void);
nss_palette_t nss_create_palette(void);
nss_cid_t nss_color_alloc(nss_palette_t pal, nss_color_t col);
nss_cid_t nss_color_find(nss_palette_t pal, nss_color_t col);
void nss_color_ref(nss_palette_t pal, nss_cid_t idx);
nss_color_t nss_color_get(nss_palette_t pal, nss_cid_t col);
void nss_color_set(nss_palette_t pal, nss_cid_t idx, nss_color_t col);
void nss_color_free(nss_palette_t pal, nss_cid_t i);
void nss_free_palette(nss_palette_t pal);
void nss_free_color(void);

#endif

