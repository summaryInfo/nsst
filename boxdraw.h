#ifndef BOXDRAW_H_
#define BOXDRAW_H_ 1

#include <stdint.h>

#include "font.h"

nss_glyph_t *nss_make_boxdraw(uint32_t ch, int16_t w, int16_t h, int16_t d, _Bool lcd);

inline static _Bool is_boxdraw(uint32_t ch) {
    return ch >= 0x2500 && ch < 0x2600;
}

#endif
