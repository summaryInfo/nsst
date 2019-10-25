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
    nss_attrib_faint = 1 << 2, //ignored
    nss_attrib_underlined = 1 << 3, //done
    nss_attrib_strikethrough = 1 << 4, //done
    nss_attrib_overlined = 1 << 5, //ignored
    nss_attrib_inverse = 1 << 6, //done
} nss_attrs_t;

#define NSS_GLYPH_MASK (0xffffff | (nss_font_attrib_mask << 24))
#define NSS_CELL_CHAR(s) ((s).ch & 0xffffff)
#define NSS_CELL_FG(s,b) (nss_color_get((s)->fg + (b)))
#define NSS_CELL_BG(s,b) (nss_color_get((s)->bg + (b)))
#define NSS_CELL_ATTRS(s) ((s).attrs)
#define NSS_CELL_GLYPH(s) ((s).ch & NSS_GLYPH_MASK)
#define NSS_MKCELL(f, b, l, c) ((nss_cell_t) { .bg = (b), .fg = (f), .ch = (c) | ((l) << 24)})
#define NSS_EQCELL(s, z) ((s).fg == (z).fg && (s).bg == (z).bg && (s).attrs == (z).attrs)

typedef struct nss_cell {
        union {
            struct {
                uint8_t pad[3];
                uint8_t attrs;
            };
            uint32_t ch;
        };
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

