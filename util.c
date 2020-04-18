/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#include "features.h"

#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "util.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

_Noreturn void die(const char *fmt, ...) {
    if (nss_config_integer(NSS_ICONFIG_LOG_LEVEL) > 0) {
        va_list args;
        va_start(args, fmt);
        fputs("[\e[31;1mFATAL\e[0m] ", stderr);
        vfprintf(stderr, fmt, args);
        fputc('\n', stderr);
        va_end(args);
    }
    exit(EXIT_FAILURE);
}

void fatal(const char *fmt, ...) {
    if (nss_config_integer(NSS_ICONFIG_LOG_LEVEL) > 0) {
        va_list args;
        va_start(args, fmt);
        fputs("[\e[31;1mFATAL\e[0m] ", stderr);
        vfprintf(stderr, fmt, args);
        fputc('\n', stderr);
        va_end(args);
    }
}

void warn(const char *fmt, ...) {
    if (nss_config_integer(NSS_ICONFIG_LOG_LEVEL) > 1) {
        va_list args;
        va_start(args, fmt);
        fputs("[\e[33;1mWARN\e[0m] ", stderr);
        vfprintf(stderr, fmt, args);
        fputc('\n', stderr);
        va_end(args);
    }
}

void info(const char *fmt, ...) {
    if (nss_config_integer(NSS_ICONFIG_LOG_LEVEL) > 2) {
        va_list args;
        va_start(args, fmt);
        fputs("[\e[32;1mINFO\e[0m] ", stderr);
        vfprintf(stderr, fmt, args);
        fputc('\n', stderr);
        va_end(args);
    }
}

void debug(const char *fmt, ...) {
    if (nss_config_integer(NSS_ICONFIG_LOG_LEVEL) > 3) {
        va_list args;
        va_start(args, fmt);
        fputs("[DEBUG] ", stderr);
        vfprintf(stderr, fmt, args);
        fputc('\n', stderr);
        va_end(args);
    }
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

uint8_t *hex_decode(uint8_t *hex) {
    uint8_t val = 0;
    uint8_t *dst = hex;
    _Bool state = 0;
    while(*hex) {
        val <<= 4;
        if ('0' <= *hex && *hex <= '9')
            val |= *hex - '0';
        else if ('A' <= *hex && *hex <= 'F')
            val |= *hex - 'A' + 10;
        else break;
        hex++;
        if (!(state = !state))
            *dst++ = val, val = 0;
    }
    return dst;

}

nss_color_t parse_color(const uint8_t *str, const uint8_t *end) {
    uint64_t val = 0;
    ptrdiff_t sz = end - str;
    if (*str != '#') return 0;
    while (++str < end) {
        if (*str - '0' < 10)
            val = (val << 4) + *str - '0';
        else if (*str - 'A' < 6)
            val = (val << 4) + 10 + *str - 'A';
        else if (*str - 'a' < 6)
            val = (val << 4) + 10 + *str - 'a';
        else return 0;
    }
    nss_color_t col = 0xFF000000;
    switch (sz) {
    case 4:
        for (size_t i = 0; i < 3; i++) {
            col |= (val & 0xF) << (8*i + 4);
            val >>= 4;
        }
        break;
    case 7:
        col |= val;
        break;
    case 10:
        for (size_t i = 0; i < 3; i++) {
            col |= ((val >> 4) & 0xFF) << 8*i;
            val >>= 12;
        }
        break;
    case 13:
        for (size_t i = 0; i < 3; i++) {
            col |= ((val >> 8) & 0xFF) << 8*i;
            val >>= 16;
        }
        break;
    default:
        return 0;
    }
    return col;
}

