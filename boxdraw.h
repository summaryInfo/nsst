/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

#ifndef BOXDRAW_H_
#define BOXDRAW_H_ 1

#include "font.h"

#include <stdbool.h>
#include <stdint.h>

struct glyph *make_boxdraw(uint32_t c, int16_t width, int16_t height, int16_t depth, enum pixel_mode pixmode, int16_t hspacing, int16_t vspacing, bool force_same);

static inline bool is_boxdraw(uint32_t ch) {
    return ch >= 0x2500 && ch < 0x25A0;
}

#endif
