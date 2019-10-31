#ifndef NSS_ATTR_H_
#define NSS_ATTR_H_ 1

#include <stdint.h>

typedef uint32_t nss_cid_t;
typedef uint16_t nss_short_cid_t;
typedef uint32_t nss_color_t;

#define NSS_CID_NONE UINT32_MAX
#define NSS_CID_DEFAULT 0
#define NSS_N_BASE_COLORS (256)

// Just 8 bits
typedef enum nss_attrs {
    nss_attrib_italic = 1 << 0, //done
    nss_attrib_bold = 1 << 1, //done
    nss_attrib_faint = 1 << 2, //done
    nss_attrib_underlined = 1 << 3, //done
    nss_attrib_strikethrough = 1 << 4, //done
    nss_attrib_invisible = 1 << 5, //done
    nss_attrib_inverse = 1 << 6, //done
    nss_attrib_blink = 1 << 7, //todo
    nss_attrib_wide = 1 << 8, //todo
    nss_attrib_wdummy = 1 << 9, //todo
    nss_attrib_reserved = 1 << 10
    // 11 bits total, sice unicode codepoint is 21 bit
} nss_attrs_t;

#define NSS_CELL_CHAR_BITS 21
#define NSS_CELL_CHAR_MASK ((1 << NSS_CELL_CHAR_BITS) - 1)
#define NSS_GLYPH_MASK (NSS_CELL_CHAR_MASK | (nss_font_attrib_mask << 21))
#define NSS_CELL_CHAR(s) ((s).ch & NSS_CELL_CHAR_MASK)
#define NSS_CELL_FG(s, b) (nss_color_get((s)->fg + (b)))
#define NSS_CELL_BG(s, b) (nss_color_get((s)->bg + (b)))
#define NSS_CELL_ATTRS_ZERO(s) ((s).ch &= NSS_CELL_CHAR_MASK)
#define NSS_CELL_ATTRS(s) ((s).ch >> NSS_CELL_CHAR_BITS)
#define NSS_CELL_ATTRSET(s, l) ((s).ch |= (l) << NSS_CELL_CHAR_BITS)
#define NSS_CELL_ATTRCLR(s, l) ((s).ch &= ~((l) << NSS_CELL_CHAR_BITS))
#define NSS_CELL_ATTR_INVERT(s, l) ((s).ch ^= (l) << NSS_CELL_CHAR_BITS)
#define NSS_CELL_GLYPH(s) ((s).ch & NSS_GLYPH_MASK)
#define NSS_MKCELL(f, b, l, c) ((nss_cell_t) { .bg = (b), .fg = (f), .ch = ((c) & NSS_CELL_CHAR_MASK) | ((l) << NSS_CELL_CHAR_BITS)})
#define NSS_EQCELL(s, z) ((s).fg == (z).fg && (s).bg == (z).bg && NSS_CELL_ATTRS(s) == NSS_CELL_ATTRS(z))
#define NSS_MKCELLWITH(s, c) NSS_MKCELL((s).fg, (s).bg, NSS_CELL_ATTRS(s), c)

typedef struct nss_cell {
        uint32_t ch; /* not really char but char + attributes */
        nss_short_cid_t fg;
        nss_short_cid_t bg;
} nss_cell_t;

void nss_init_color(void);
nss_cid_t nss_color_alloc(nss_color_t col);
nss_cid_t nss_color_find(nss_color_t col);
void nss_color_ref(nss_cid_t idx);
nss_color_t nss_color_get(nss_cid_t col);
void nss_color_set(nss_cid_t idx, nss_color_t col);
void nss_color_free(nss_cid_t i);
void nss_free_color(void);

#endif

