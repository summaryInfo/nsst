/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

#include "feature.h"


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

#define LOG_BUFFER_SIZE 1024

static char log_buffer[LOG_BUFFER_SIZE + 1];

static void do_log(int level, const char *fmt, va_list args)  __attribute__((format(printf, 2, 0)));

static void do_log(int level, const char *fmt, va_list args) {
    static struct log_prefix {
        const char *msg;
        int len;
        int color;
    } log_prefix[] = {
        {"FATAL", sizeof "FATAL" - 1, 31},
        {"WARN", sizeof "WARN" - 1, 33},
        {"INFO", sizeof "INFO" - 1, 32},
        {"DEBUG", sizeof "DEBUG" - 1, 0},
    };

    if (gconfig.log_level < level) return;

    size_t len = log_prefix[level].len;
    if (gconfig.log_color) {
        len += snprintf(log_buffer + len, LOG_BUFFER_SIZE - len,
                        "[\033[%d;1m%s\033[m] ", log_prefix[level].color, log_prefix[level].msg);
    } else {
        len += snprintf(log_buffer + len, LOG_BUFFER_SIZE - len,
                        "[%s] ", log_prefix[level].msg);
    }

    len += vsnprintf(log_buffer + len, LOG_BUFFER_SIZE - len, fmt, args);
    log_buffer[len++] = '\n';

    for (size_t written = 0, res; written < len; written += res)
        if ((res = write(STDERR_FILENO, log_buffer, len - written)) <= 0)
            break;
}

void *xalloc(size_t size) {
    void *res;
#ifdef __SSE2__
    if (_Alignof(max_align_t) < MALLOC_ALIGNMENT) {
        size = (size + MALLOC_ALIGNMENT - 1) & ~(MALLOC_ALIGNMENT - 1);
        res = aligned_alloc(MALLOC_ALIGNMENT, size);
    } else
#endif
        res = malloc(size);

    if (!res)
        die("Failed to allocate %zd bytes of memory", size);
    return res;
}

void *xrealloc(void *src, size_t old_size, size_t size) {
    void *res;

    if (!src) return xalloc(size);
    assert(size);

#ifdef __SSE2__
    if (_Alignof(max_align_t) < MALLOC_ALIGNMENT) {
        size = (size + MALLOC_ALIGNMENT - 1) & ~(MALLOC_ALIGNMENT - 1);
        res = aligned_alloc(MALLOC_ALIGNMENT, size);
        if (res) memcpy(res, src, MIN(old_size, size));
        free(src);
    } else
#endif
        res = realloc(src, size);

    if (!res)
        die("Failed to allocate %zd bytes of memory", size);
    return res;
}

void *xzalloc(size_t size) {
    void *res;
#ifdef __SSE2__
    if (_Alignof(max_align_t) < MALLOC_ALIGNMENT) {
        size = (size + MALLOC_ALIGNMENT - 1) & ~(MALLOC_ALIGNMENT - 1);
        res = aligned_alloc(MALLOC_ALIGNMENT, size);
        if (res) memset(res, 0, size);
    } else
#endif
        res = calloc(1, size);

    if (!res)
        die("Failed to allocate %zd bytes of memory", size);
    return res;
}

void *xrezalloc(void *src, size_t old_size, size_t size) {
    void *res;

    if (!src) return xzalloc(size);
    assert(size);

#ifdef __SSE2__
    if (_Alignof(max_align_t) < MALLOC_ALIGNMENT) {
        size = (size + MALLOC_ALIGNMENT - 1) & ~(MALLOC_ALIGNMENT - 1);
        res = aligned_alloc(MALLOC_ALIGNMENT, size);
        if (res) memcpy(res, src, MIN(old_size, size));
        free(src);
    } else
#endif
        res = realloc(src, size);

    if (!res)
        die("Failed to allocate %zd bytes of memory", size);

    if (size > old_size)
        memset((char *)res + old_size, 0, size - old_size);

    return res;
}

_Noreturn void die(const char *fmt, ...) {
    if (gconfig.log_level > 0) {
        va_list args;
        va_start(args, fmt);
        do_log(0, fmt, args);
        va_end(args);
    }
    exit(EXIT_FAILURE);
}

void fatal(const char *fmt, ...) {
    if (gconfig.log_level > 0) {
        va_list args;
        va_start(args, fmt);
        do_log(0, fmt, args);
        va_end(args);
    }
}

void warn(const char *fmt, ...) {
    if (gconfig.log_level > 1) {
        va_list args;
        va_start(args, fmt);
        do_log(1, fmt, args);
        va_end(args);
    }
}

void info(const char *fmt, ...) {
    if (gconfig.log_level > 2) {
        va_list args;
        va_start(args, fmt);
        do_log(2, fmt, args);
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

void adjust_buffer(void **buf, size_t *caps, size_t size, size_t elem) {
    if (UNLIKELY(size > *caps)) {
        *buf = xrealloc(*buf, elem * *caps, elem * MAX(CAPS_STEP(*caps), size));
        *caps = CAPS_STEP(*caps);
    }
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

#define HT_LOAD_FACTOR(x) (4*(x)/3)
#define HT_CAPS_STEP(x) (3*(x)/2)

void ht_adjust(struct hashtable *ht, intptr_t inc) {
    ht->size += inc;

    if (UNLIKELY(HT_LOAD_FACTOR(ht->size) > ht->caps)) {
        struct hashtable tmp = {
            .cmpfn = ht->cmpfn,
            .caps = HT_CAPS_STEP(ht->caps),
            .data = xzalloc(HT_CAPS_STEP(ht->caps) * sizeof *ht->data),
        };

        ht_iter_t it = ht_begin(ht);
        while(ht_current(&it))
            ht_insert(&tmp, ht_erase_current(&it));
        free(ht->data);
        *ht = tmp;
    }
}

void ht_shrink(struct hashtable *ht, intptr_t new_caps) {
    struct hashtable tmp = {
        .cmpfn = ht->cmpfn,
        .caps = new_caps,
        .data = xzalloc(new_caps * sizeof *ht->data),
    };

    ht_iter_t it = ht_begin(ht);
    while(ht_current(&it))
        ht_insert(&tmp, ht_erase_current(&it));
    free(ht->data);
    *ht = tmp;
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
            pre1_tab, LEN(pre1_tab), sizeof(*pre1_tab), pre1_cmpfn);
    if (r1) return r1->dst;

    struct pre2_item *r2 = bsearch(&(struct pre2_item){ch, comb, 0},
            pre2_tab, LEN(pre2_tab), sizeof(*pre2_tab), pre2_cmpfn);
    if (r2) return r2->dst;

    return ch;
}

#else

static uint32_t try_precompose(uint32_t ch, uint32_t comb) { (void)comb; return ch; }

#endif

#include "wide.h"
