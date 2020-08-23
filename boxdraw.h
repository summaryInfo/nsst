/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#ifndef BOXDRAW_H_
#define BOXDRAW_H_ 1

#include "font.h"

#include <stdbool.h>
#include <stdint.h>

struct glyph *make_boxdraw(uint32_t ch, int16_t w, int16_t h, int16_t d);

inline static bool is_boxdraw(uint32_t ch) {
    return ch >= 0x2500 && ch < 0x25A0;
}

#endif

