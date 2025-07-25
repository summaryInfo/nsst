/* Copyright (c) 2019-2022,2025, Evgeniy Baskov. All rights reserved */

#ifndef UTIL_H_
#define UTIL_H_ 1

#include "feature.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define ROUNDUP(x, align) (((x) + (align) - 1) & ~((uintptr_t)(align) - 1))
#define ROUNDDOWN(x, align) ((x) & ~((uintptr_t)(align) - 1))

#define MIN(x, y) ((x) < (y) ? (x) : (y))
#define MAX(x, y) ((x) > (y) ? (x) : (y))

#define SWAP(a, b) do {__typeof__(a) t__ = (a); (a) = (b); (b) = t__;}while (0)

#define SEC 1000000000LL
#define CACHE_LINE 64

#define IS_SAME_TYPE_(a, b) __builtin_types_compatible_p(__typeof__(a), __typeof__(b))
#define IS_ARRAY_(arr) (!IS_SAME_TYPE_((arr), &(arr)[0]))
#define MUST_BE_(e) (0*(size_t)sizeof(struct {_Static_assert(e, "Argument has wrong type");int dummy__;}))

#define CONTAINEROF(ptr, T, member) ((T*)__builtin_assume_aligned((char *)ptr - offsetof(T, member), _Alignof(T)) \
                                     + MUST_BE_(IS_SAME_TYPE_(&((T*)0)->member, ptr)))

#define LEN(x) (sizeof(x)/sizeof((x)[0]) + MUST_BE_(IS_ARRAY_(x)))

#define LIKELY(x) (__builtin_expect(!!(x), 1))
#define UNLIKELY(x) (__builtin_expect((x), 0))
#define ASSUME(x) do { if(!(x)) __builtin_unreachable(); }while(0)
#define PACKED __attribute__((packed))
#define HOT __attribute__((hot))
#define FORCEINLINE __attribute__((always_inline))
#define ALIGNED(n) __attribute__((aligned(n)))

#ifdef CLOCK_MONOTONIC_RAW
#   define CLOCK_TYPE CLOCK_MONOTONIC_RAW
#else
#   define CLOCK_TYPE CLOCK_MONOTONIC
#endif

#define PROFILE_BEGIN do {\
        struct timespec start__, end__; \
        clock_gettime(CLOCK_TYPE, &start__);
#define PROFILE_END(label) \
        clock_gettime(CLOCK_TYPE, &end__); \
        warn(label " took %lfms", ts_diff(&start__, &end__)/1000000.); \
    } while (0)

#define PROFILE_FUNC(f) \
    PROFILE_BEGIN\
    f; \
    PROFILE_END(#f)

typedef uint32_t color_t;
struct rect {
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
};

void info(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));
void warn(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));
void fatal(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));
_Noreturn void die(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));

#define MALLOC_ALIGNMENT 16

/* malloc() wrappers which calls die() on failure and
 * ensure 16 bytes alignment */
void *xalloc(size_t size);
void *xzalloc(size_t size);
void *xrealloc(void *src, size_t old_size, size_t size);
void *xrezalloc(void *src, size_t old_size, size_t size);
void xtrim_heap(void);

static inline struct rect rect_scale_up(struct rect rect, int16_t x_factor, int16_t y_factor) {
    rect.x *= x_factor;
    rect.y *= y_factor;
    rect.width *= x_factor;
    rect.height *= y_factor;
    return rect;
}
static inline struct rect rect_scale_down(struct rect rect, int16_t x_factor, int16_t y_factor) {
    rect.x /= x_factor;
    rect.y /= y_factor;
    rect.width /= x_factor;
    rect.height /= y_factor;
    return rect;
}
static inline struct rect rect_shift(struct rect rect, int16_t x_off, int16_t y_off) {
    rect.x += x_off;
    rect.y += y_off;
    return rect;
}
static inline struct rect rect_resize(struct rect rect, int16_t x_off, int16_t y_off) {
    rect.width += x_off;
    rect.height += y_off;
    return rect;
}
static inline struct rect rect_union(struct rect rect, struct rect other) {
    rect.width = MAX(rect.width + rect.x, other.width + other.x);
    rect.height = MAX(rect.height + rect.y, other.height + other.y);
    rect.width -= rect.x = MIN(rect.x, other.x);
    rect.height -= rect.y = MIN(rect.y, other.y);
    return rect;
}

static inline bool intersect_with(struct rect *src, struct rect *dst) {
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

static inline bool ts_leq(struct timespec *a, struct timespec *b) {
    return a->tv_sec < b->tv_sec || (a->tv_sec == b->tv_sec && a->tv_nsec <= b->tv_nsec);
}

static inline struct timespec ts_sub_sat(struct timespec *a, struct timespec *b) {
    if (ts_leq(a, b))
        return (struct timespec) {0};
    struct timespec result = {
        .tv_sec = a->tv_sec - b->tv_sec,
        .tv_nsec = a->tv_nsec - b->tv_nsec,
    };
    if (result.tv_nsec < 0) {
        result.tv_nsec += SEC;
        result.tv_sec--;
    }
    return result;
}

static inline struct timespec ts_add(struct timespec *a, int64_t inc) {
    long ns = a->tv_nsec + inc;
    long sadj = (ns - (SEC + 1)*(inc < 0)) / SEC;
    return (struct timespec) {
        .tv_sec = a->tv_sec + sadj,
        .tv_nsec = ns - sadj * SEC
    };
}

static inline int64_t ts_diff(struct timespec *b, struct timespec *a) {
    return (a->tv_sec - b->tv_sec)*SEC + (a->tv_nsec - b->tv_nsec);
}

/* Adjust buffer capacity if no space left (size > *caps) */
void adjust_buffer(void **buf, size_t *caps, size_t size, size_t elem);

/* Version information helper functions */
#define MAX_OPTION_DESC 512
const char *version_string(void);
const char *features_string(void);
const char *usage_string(char buffer[static MAX_OPTION_DESC + 1], ssize_t idx);

#define UTF8_MAX_LEN 4
#define UTF_INVAL 0xFFFD

#include "iswide.h"

static inline int uwidth(uint32_t x) {
    /* This variant wcwidth treats
     * C0 and C1 characters as of width 1 */
    if (LIKELY(x < 0x300)) return 1;
    if (UNLIKELY(iscombining(x))) return 0;
    return 1 + iswide(x);
}


size_t utf8_encode(uint32_t u, uint8_t *buf, uint8_t *end);
bool utf8_decode(uint32_t *res, const uint8_t **buf, const uint8_t *end);

/* *_decode returns source buffer end */
/* *_encode returns destination buffer end */
uint8_t *hex_encode(uint8_t *dst, const uint8_t *str, const uint8_t *end);
const uint8_t *hex_decode(uint8_t *dst, const uint8_t *hex, const uint8_t *end);
uint8_t *base64_encode(uint8_t *dst, const uint8_t *buf, const uint8_t *end);
const uint8_t *base64_decode(uint8_t *dst, const uint8_t *buf, const uint8_t *end);

color_t parse_color(const uint8_t *str, const uint8_t *end);

/* Unicode precomposition */
uint32_t try_precompose(uint32_t ch, uint32_t comb);

int set_cloexec(int fd);
int set_nonblocking(int fd);

#endif
