/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#ifndef UTIL_H_
#define UTIL_H_ 1

#include "feature.h"

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define MIN(x, y) ((x) < (y) ? (x) : (y))
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#define SWAP(T, a, b) {T tmp_ = (a); (a) = (b); (b) = tmp_; }
#define SEC 1000000000LL
#define TIMEDIFF(t, d)  ((((d).tv_sec - (t).tv_sec) * SEC + ((d).tv_nsec - (t).tv_nsec)))
#define TIMEINC(t, in) ((t).tv_sec += (in)/SEC), ((t).tv_nsec += (in)%SEC)

#ifdef CLOCK_MONOTONIC_RAW
#   define CLOCK_TYPE CLOCK_MONOTONIC_RAW
#else
#   define CLOCK_TYPE CLOCK_MONOTONIC
#endif

typedef int16_t nss_coord_t;
typedef uint32_t color_t;
typedef uint32_t term_char_t;
struct rect {
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
};

void info(const char *fmt, ...);
void warn(const char *fmt, ...);
void fatal(const char *fmt, ...);
_Noreturn void die(const char *fmt, ...);

inline static struct rect rect_scale_up(struct rect rect, nss_coord_t x_factor, nss_coord_t y_factor) {
    rect.x *= x_factor;
    rect.y *= y_factor;
    rect.width *= x_factor;
    rect.height *= y_factor;
    return rect;
}
inline static struct rect rect_scale_down(struct rect rect, nss_coord_t x_factor, nss_coord_t y_factor) {
    rect.x /= x_factor;
    rect.y /= y_factor;
    rect.width /= x_factor;
    rect.height /= y_factor;
    return rect;
}
inline static struct rect rect_shift(struct rect rect, nss_coord_t x_off, nss_coord_t y_off) {
    rect.x += x_off;
    rect.y += y_off;
    return rect;
}
inline static struct rect rect_resize(struct rect rect, nss_coord_t x_off, nss_coord_t y_off) {
    rect.width += x_off;
    rect.height += y_off;
    return rect;
}
inline static struct rect rect_union(struct rect rect, struct rect other) {
    rect.width = MAX(rect.width + rect.x, other.width + other.x);
    rect.height = MAX(rect.height + rect.y, other.height + other.y);
    rect.width -= rect.x = MIN(rect.x, other.x);
    rect.height -= rect.y = MIN(rect.y, other.y);
    return rect;
}

inline static bool intersect_with(struct rect *src, struct rect *dst) {
        struct rect inters = { .x = MAX(src->x, dst->x), .y = MAX(src->y, dst->y) };

        int32_t x1 = MIN(src->x + (int32_t)src->width, dst->x + (int32_t)dst->width);
        int32_t y1 = MIN(src->y + (int32_t)src->height, dst->y + (int32_t)dst->height);

        if (x1 <= inters.x || y1 <= inters.y) {
            *src = (struct rect) {0, 0, 0, 0};
            return 0;
        } else {
            inters.width = x1 - inters.x;
            inters.height = y1 - inters.y;
            *src = inters;
            return 1;
        }
}


#define UTF8_MAX_LEN 4
#define UTF_INVAL 0xfffd

size_t utf8_encode(term_char_t u, uint8_t *buf, uint8_t *end);
bool utf8_decode(term_char_t *res, const uint8_t **buf, const uint8_t *end);

/* *_decode returns source buffer end */
/* *_encode returns destination buffer end */
uint8_t *hex_encode(uint8_t *dst, const uint8_t *str, const uint8_t *end);
const uint8_t *hex_decode(uint8_t *dst, const uint8_t *hex, const uint8_t *end);
uint8_t *base64_encode(uint8_t *dst, const uint8_t *buf, const uint8_t *end);
const uint8_t *base64_decode(uint8_t *dst, const uint8_t *buf, const uint8_t *end);

color_t parse_color(const uint8_t *str, const uint8_t *end);

/* Unicode precomposition */
term_char_t try_precompose(term_char_t ch, term_char_t comb);
#endif
