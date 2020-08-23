/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#ifndef BOXDRAW_H_
#define BOXDRAW_H_ 1

#include "font.h"

#include <stdbool.h>
#include <stdint.h>

nss_glyph_t *nss_make_boxdraw(uint32_t ch, int16_t w, int16_t h, int16_t d);

inline static _Bool is_boxdraw(uint32_t ch) {
    return ch >= 0x2500 && ch < 0x25A0;
}

#endif

