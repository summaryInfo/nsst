/* Copyright (c) 2019-2021, Evgeny Baskov. All rights reserved */

#include "feature.h"

#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "hashtable.h"
#include "util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

_Noreturn void die(const char *fmt, ...) {
    if (gconfig.log_level > 0) {
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
    if (gconfig.log_level > 0) {
        va_list args;
        va_start(args, fmt);
        fputs("[\033[31;1mFATAL\033[m] ", stderr);
        vfprintf(stderr, fmt, args);
        fputc('\n', stderr);
        va_end(args);
    }
}

void warn(const char *fmt, ...) {
    if (gconfig.log_level > 1) {
        va_list args;
        va_start(args, fmt);
        fputs("[\033[33;1mWARN\033[m] ", stderr);
        vfprintf(stderr, fmt, args);
        fputc('\n', stderr);
        va_end(args);
    }
}

void info(const char *fmt, ...) {
    if (gconfig.log_level > 2) {
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
    static const uint8_t utf8_mask[] = {0x00, 0xC0, 0xE0, 0xF0};
    if (u > 0x10FFFF) u = UTF_INVAL;
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

bool utf8_decode(uint32_t *res, const uint8_t **buf, const uint8_t *end) {
    int8_t len = (const int8_t[32]){
        /* 00xx xxxx */  0, 0, 0, 0, 0, 0, 0, 0,
        /* 01xx xxxx */  0, 0, 0, 0, 0, 0, 0, 0,
        /* 10xx xxxx */ -1,-1,-1,-1,-1,-1,-1,-1,
        /* 11xx xxxx */  1, 1, 1, 1, 2, 2, 3,-1 }[**buf >> 3U];

    if (UNLIKELY(len < 0)) goto inval;
    else if (*buf + len >= end) return 0;

    uint32_t part = *(*buf)++ & (0x7F >> len), i = len;

    while (i--) {
        if (UNLIKELY((**buf & 0xC0) != 0x80)) goto inval2;
        part = (part << 6) | (*(*buf)++ & 0x3F);
    }

    static const uint32_t maxv[] = {0x80, 0x800, 0x10000, 0x110000};
    if (UNLIKELY(part >= maxv[len]) || UNLIKELY(part - 0xD800 < 0xE000 - 0xD800)) goto inval2;

    *res = part;
    return 1;
inval:
    (*buf)++;
inval2:
    *res = UTF_INVAL;
    return 1;
}

inline static uint8_t tohexdigit(uint8_t c) {
    return  c > 9 ? c + 'A' - 10 : c + '0';
}

inline static uint8_t fromhexdigit(uint8_t c) {
    if (c - (unsigned)'0' < 10U)
        return  c - '0';
    else if (c - (unsigned)'A' < 6U)
        return  10 + c - 'A';
    else if (c - (unsigned)'a' < 6U)
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
    } else if (!strncasecmp((const char*)str, "rgb:", 4)) {
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
    bool state = 0;
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

#define CAPS_STEP(x) ((x)?4*(x)/3:8)

bool adjust_buffer(void **buf, size_t *caps, size_t size, size_t elem) {
    if (UNLIKELY(size > *caps)) {
        void *tmp = realloc(*buf, elem * MAX(CAPS_STEP(*caps), size));
        if (!tmp) return 0;
        *buf = tmp;
        *caps = CAPS_STEP(*caps);
    }
    return 1;
}

const char *version_string(void) {
    static char str[32];
    if (!str[0]) {
        snprintf(str, sizeof str, "nsst v%d.%d.%d\n",
            (NSST_VERSION / 10000) % 100, (NSST_VERSION / 100) % 100, NSST_VERSION % 100);
    }
    return str;
}

const char *features_string(void) {
    return "nsst"
#if USE_PPOLL
            "+ppoll"
#endif
#if USE_BOXDRAWING
            "+boxdrawing"
#endif
#if USE_X11SHM
            "+mitshm"
#endif
#if USE_POSIX_SHM
            "+posixshm"
#endif
#if USE_PRECOMPOSE
            "+precompose"
#endif
            "\n";
}

#define MAX_OPTION_DESC 512

const char *usage_string(ssize_t idx) {
    static char buffer[MAX_OPTION_DESC + 1];

    if (!idx) {
        return /* argv0 here*/ " [-options] [-e] [command [args]]\n"
            "Where options are:\n"
                "\t--help, -h\t\t\t(Print this message and exit)\n"
                "\t--version, -v\t\t\t(Print version and exit)\n"
                "\t--color<N>=<color>, \t\t(Set palette color <N>, <N> is from 0 to 255)\n"
                "\t--geometry=<value>, -g<value> \t(Window geometry, format is [=][<width>{xX}<height>][{+-}<xoffset>{+-}<yoffset>])\n";
    } else if (idx - 1 < o_MAX) {
        snprintf(buffer, sizeof buffer, "\t--%s=<value>%s\n", optmap[idx - 1].opt, optmap[idx - 1].descr);
        return buffer;
    } else if (idx == o_MAX + 1) {
        return  "For every boolean option --<X>=<Y>\n"
                "\t--<X>, --<X>=yes, --<X>=y,  --<X>=true\n"
            "are equivalent to --<X>=1, and\n"
                "\t--no-<X>, --<X>=no, --<X>=n, --<X>=false\n"
            "are equivalent to --<X>=0,\n"
            "where 'yes', 'y', 'true', 'no', 'n' and 'false' are case independet\n"
            "All options are also accept special value 'default' to reset to built-in default\n";
    } else return NULL;
}


#define HT_LOAD_FACTOR(x) (4*(x)/3)
#define HT_CAPS_STEP(x) (3*(x)/2)

bool ht_adjust(struct hashtable *ht, intptr_t inc) {
    ht->size += inc;

    if (UNLIKELY(HT_LOAD_FACTOR(ht->size) > ht->caps)) {
        struct hashtable tmp = {
            .cmpfn = ht->cmpfn,
            .caps = HT_CAPS_STEP(ht->caps),
            .data = calloc(HT_CAPS_STEP(ht->caps), sizeof(*ht->data)),
        };
        if (!tmp.data) return 0;

        ht_iter_t it = ht_begin(ht);
        while(ht_current(&it))
            ht_insert(&tmp, ht_erase_current(&it));
        free(ht->data);
        *ht = tmp;
    }

    return 1;
}

bool ht_shrink(struct hashtable *ht, intptr_t new_caps) {
    struct hashtable tmp = {
        .cmpfn = ht->cmpfn,
        .caps = new_caps,
        .data = calloc(new_caps, sizeof(*ht->data)),
    };
    if (!tmp.data) return 0;

    ht_iter_t it = ht_begin(ht);
    while(ht_current(&it))
        ht_insert(&tmp, ht_erase_current(&it));
    free(ht->data);
    *ht = tmp;

    return 1;
}

#if USE_PRECOMPOSE

#include "precompose-table.h"

static int pre1_cmpfn(const void * a, const void *b) {
    const struct pre1_item *ai = a, *bi = b;
    if (ai->src != bi->src) return ai->src - bi->src;
    return ai->mod - bi->mod;
}
static int pre2_cmpfn(const void * a, const void *b) {
    const struct pre2_item *ai = a, *bi = b;
    if (ai->src != bi->src) return ai->src - bi->src;
    return ai->mod - bi->mod;
}

uint32_t try_precompose(uint32_t ch, uint32_t comb) {
    struct pre1_item *r1 = bsearch(&(struct pre1_item){ch, comb, 0},
            pre1_tab, sizeof(pre1_tab)/sizeof(*pre1_tab), sizeof(*pre1_tab), pre1_cmpfn);
    if (r1) return r1->dst;

    struct pre2_item *r2 = bsearch(&(struct pre2_item){ch, comb, 0},
            pre2_tab, sizeof(pre2_tab)/sizeof(*pre2_tab), sizeof(*pre2_tab), pre2_cmpfn);
    if (r2) return r2->dst;

    return ch;
}

#else

static uint32_t try_precompose(uint32_t ch, uint32_t comb) { (void)comb; return ch; }

#endif

