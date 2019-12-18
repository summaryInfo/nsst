/* Copyright (c) 2019, Evgeny Baskov. All rights reserved */

#ifndef UTIL_H_
#define UTIL_H_ 1

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdlib.h>

#define MIN(x, y) ((x) < (y) ? (x) : (y))
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#define SWAP(T, a, b) {T tmp_ = (a); (a) = (b); (b) = tmp_; }
#define TIMEDIFF(t, d)  ((((d).tv_sec - (t).tv_sec) * 1000000000 + ((d).tv_nsec - (t).tv_nsec)) / 1000)

typedef uint32_t nss_color_t;
typedef struct nss_rect {
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
} nss_rect_t;

_Noreturn void die(const char *fmt, ...);
void warn(const char *fmt, ...);
void info(const char *fmt, ...);
void fatal(const char *fmt, ...);

inline static nss_rect_t rect_scale_up(nss_rect_t rect, int16_t x_factor, int16_t y_factor) {
    rect.x *= x_factor;
    rect.y *= y_factor;
    rect.width *= x_factor;
    rect.height *= y_factor;
    return rect;
}
inline static nss_rect_t rect_scale_down(nss_rect_t rect, int16_t x_factor, int16_t y_factor) {
    rect.x /= x_factor;
    rect.y /= y_factor;
    rect.width /= x_factor;
    rect.height /= y_factor;
    return rect;
}
inline static nss_rect_t rect_shift(nss_rect_t rect, int16_t x_off, int16_t y_off) {
    rect.x += x_off;
    rect.y += y_off;
    return rect;
}
inline static nss_rect_t rect_resize(nss_rect_t rect, int16_t x_off, int16_t y_off) {
    rect.x += x_off;
    rect.y += y_off;
    return rect;
}

inline static _Bool intersect_with(nss_rect_t *src, nss_rect_t *dst) {
        nss_rect_t inters = {
            .x = MAX(src->x, dst->x),
            .y = MAX(src->y, dst->y),
            .width = MIN(src->x + src->width, dst->x + dst->width),
            .height = MIN(src->y + src->height, dst->y + dst->height),
        };
        if (inters.width <= inters.x || inters.height <= inters.y) {
            *src = (nss_rect_t) {0, 0, 0, 0};
            return 0;
        } else {
            inters.width -= inters.x;
            inters.height -= inters.y;
            *src = inters;
            return 1;
        }
}

#define UTF8_MAX_LEN 4
#define UTF_INVAL 0xfffd

size_t utf8_encode(uint32_t u, uint8_t *buf, uint8_t *end);
_Bool utf8_decode(uint32_t *res, const uint8_t **buf, const uint8_t *end);
uint8_t *hex_decode(uint8_t *hex);
nss_color_t parse_color(char *str, char *end);

#endif 
