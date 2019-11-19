#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stddef.h>

#include "util.h"
#define NSS_NOLOGS

_Noreturn void die(const char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    fputs("[\e[31;1mFATAL\e[0m] ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);

    exit(EXIT_FAILURE);
}

void fatal(const char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    fputs("[\e[31;1mFATAL\e[0m] ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

void warn(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fputs("[\e[33;1mWARN\e[0m] ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

void info(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fputs("[\e[32;1mINFO\e[0m] ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}


size_t utf8_encode(uint32_t u, uint8_t *buf, uint8_t *end) {
    static const uint32_t utf8_min[] = {0x80, 0x800, 0x10000, 0x110000};
    static const uint8_t utf8_mask[] = {0x00, 0xc0, 0xe0, 0xf0};
    if (u > 0x10ffff) u = UTF_INVAL;
    size_t i = 0, j;
    while (u > utf8_min[i++]);
    if ((ptrdiff_t)i > end - buf) return 0;
    for (j = i; j > 1; j--) {
        buf[j - 1] = (u & 0x3f) | 0x80;
        u >>= 6;
    }
    buf[0] = u | utf8_mask[i - 1];
    return i;
}

_Bool utf8_decode(uint32_t *res, const uint8_t **buf, const uint8_t *end) {
    if (*buf >= end) return 0;
    uint32_t part = *(*buf)++;
    uint8_t len = 0, i = 0x80;
    if (part > 0xF7) {
        part = UTF_INVAL;
    } else {
        while (part & i) {
            len++;
            part &= ~i;
            i /= 2;
        }
        if (len == 1) {
            part = UTF_INVAL;
        }  else if (len > 1) {
            uint8_t i = --len;
            if (end - *buf < i) {
                (*buf)--;
                return 0;
            }
            while (i--) {
                if ((**buf & 0xC0) != 0x80) {
                    *res = UTF_INVAL;
                    return 1;
                }
                part = (part << 6) + (*(*buf)++ & ~0xC0);
            }
            if(part >= 0xD800 && part < 0xE000 &&
                    part >= (uint32_t[]){0x80, 0x800, 0x10000, 0x110000}[len - 1])
                part = UTF_INVAL;
        }
    }
    *res = part;
    return 1;
}

