/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#include "feature.h"

#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

_Noreturn void die(const char *fmt, ...) {
    if (iconf(ICONF_LOG_LEVEL) > 0) {
        va_list args;
        va_start(args, fmt);
        fputs("[\033[31;1mFATAL\033[m] ", stderr);
        vfprintf(stderr, fmt, args);
        fputc('\n', stderr);
        va_end(args);
    }
    exit(EXIT_FAILURE);
}

void fatal(const char *fmt, ...) {
    if (iconf(ICONF_LOG_LEVEL) > 0) {
        va_list args;
        va_start(args, fmt);
        fputs("[\033[31;1mFATAL\033[m] ", stderr);
        vfprintf(stderr, fmt, args);
        fputc('\n', stderr);
        va_end(args);
    }
}

void warn(const char *fmt, ...) {
    if (iconf(ICONF_LOG_LEVEL) > 1) {
        va_list args;
        va_start(args, fmt);
        fputs("[\033[33;1mWARN\033[m] ", stderr);
        vfprintf(stderr, fmt, args);
        fputc('\n', stderr);
        va_end(args);
    }
}

void info(const char *fmt, ...) {
    if (iconf(ICONF_LOG_LEVEL) > 2) {
        va_list args;
        va_start(args, fmt);
        fputs("[\033[32;1mINFO\033[m] ", stderr);
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
            if (part >= 0xD800 && part < 0xE000 &&
                    part >= (uint32_t[]){0x80, 0x800, 0x10000, 0x110000}[len - 1])
                part = UTF_INVAL;
        }
    }
    *res = part;
    return 1;
}

inline static uint8_t tohexdigit(uint8_t c) {
    return  c > 9 ? c + 'A' - 10 : c + '0';
}

inline static uint8_t fromhexdigit(uint8_t c) {
    if (c - '0' < 10)
        return  c - '0';
    else if (c - 'A' < 6)
        return  10 + c - 'A';
    else if (c - 'a' < 6)
        return  10 + c - 'a';
    else
        return 0;
}

inline static int32_t frombase64digit(uint8_t b) {
    if ('A' <= b && b <= 'Z') return b - 'A';
    if ('a' <= b && b <= 'z') return b - 'a' + 26;
    if ('0' <= b && b <= '9') return b - '0' + 52;
    if (b == '+') return 62;
    if (b == '/') return 63;
    return -1;
}



color_t parse_color(const uint8_t *str, const uint8_t *end) {
    if (*str == '#') {
        // Format #RGB

        uint64_t val = 0;
        ptrdiff_t sz = end - str;

        while (++str < end) {
            if (!isxdigit(*str)) return 0;
            val = (val << 4) + fromhexdigit(*str);
        }

        color_t col = 0xFF000000;

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
    } else if (!strncasecmp((char*)str, "rgb:", 4)) {
        // Format rgb:R/G/B
        str += 4;

        size_t len = 0, i = 0;
        uint32_t rgb[3] = {0};
        for (; str < end && i < 3; i++) {
            size_t clen = 0;
            while(str + clen < end && str[clen] != '/') {
                if (!isxdigit(str[clen])) return 0;
                rgb[i] = (rgb[i] << 4) | fromhexdigit(str[clen]);
               clen++;
            }
            if (!i) {
                len = clen;
                if (!len || len > 4) return 0;
            }
            str += clen + 1;
            if (len != clen) return 0;
        }
        if (i != 3 || str - 1 != end) return 0;

        switch(len) {
        case 1:
            return 0xFF000000 |
                (rgb[0] << 20) |
                (rgb[1] << 12) |
                (rgb[2] <<  4);
        case 2:
            return 0xFF000000 |
                (rgb[0] << 16) |
                (rgb[1] <<  8) |
                (rgb[2] <<  0);
        default:
            return 0xFF000000 |
                ((rgb[0] & 0xFF) << 16) |
                ((rgb[1] & 0xFF) <<  8) |
                ((rgb[2] & 0xFF) <<  0);
        }
    } else return 0;
}

const uint8_t *hex_decode(uint8_t *dst, const uint8_t *hex, const uint8_t *end) {
    uint8_t val = 0;
    _Bool state = 0;
    while (hex  < end) {
        if (!isxdigit(*hex)) break;
        val = (val << 4) | fromhexdigit(*hex++);
        if (!(state = !state))
            *dst++ = val, val = 0;
    }
    *dst = '\0';
    return hex;
}

uint8_t *hex_encode(uint8_t *dst, const uint8_t *str, const uint8_t *end) {
    while (str < end) {
        *dst++ = tohexdigit(*str >> 4);
        *dst++ = tohexdigit(*str++ & 0xF);
    }
    *dst = '\0';
    return dst;
}

const uint8_t *base64_decode(uint8_t *dst, const uint8_t *buf, const uint8_t *end) {
    int32_t acc = 0, b, bits = 0;
    while (buf < end && (b = frombase64digit(*buf)) >= 0) {
        acc = (acc << 6) | b;
        if ((bits += 6) > 7) {
            *dst++ = acc >> (bits -= 8);
            acc &= (1 << bits) - 1;
        }
        buf++;
    }
    bits /= 2;
    while (bits-- && *buf == '=') buf++;
    *dst = '\0';
    return buf;
}

uint8_t *base64_encode(uint8_t *dst, const uint8_t *buf, const uint8_t *end) {
    static uint8_t conv[]  = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t acc = 0, bits = 0, pad = (3 - (end - buf) % 3) % 3;
    while (buf < end) {
        acc = (acc << 8) | *buf++;
        bits += 8;
        while (bits > 5) {
            *dst++ = conv[acc >> (bits -= 6)];
            acc &= (1 << bits) - 1;
        }
    }
    if (bits) *dst++ = conv[acc << (6 - bits)];
    while (pad--)  *dst++ = '=';

    return dst;
}

#if USE_PRECOMPOSE

#include "precompose-table.h"

int pre1_cmpfn(const void * a, const void *b) {
    const struct pre1_item *ai = a, *bi = b;
    if (ai->src != bi->src) return ai->src - bi->src;
    return ai->mod - bi->mod;
}
int pre2_cmpfn(const void * a, const void *b) {
    const struct pre2_item *ai = a, *bi = b;
    if (ai->src != bi->src) return ai->src - bi->src;
    return ai->mod - bi->mod;
}

nss_char_t try_precompose(nss_char_t ch, nss_char_t comb) {
    struct pre1_item *r1 = bsearch(&(struct pre1_item){ch, comb, 0},
            pre1_tab, sizeof(pre1_tab)/sizeof(*pre1_tab), sizeof(*pre1_tab), pre1_cmpfn);
    if (r1) return r1->dst;

    struct pre2_item *r2 = bsearch(&(struct pre2_item){ch, comb, 0},
            pre2_tab, sizeof(pre2_tab)/sizeof(*pre2_tab), sizeof(*pre2_tab), pre2_cmpfn);
    if (r2) return r2->dst;

    return ch;
}

#else

nss_char_t try_precompose(nss_char_t ch, nss_char_t comb) { (void)comb; return ch; }

#endif

