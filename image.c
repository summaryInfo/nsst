/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */


#include "feature.h"

#define _GNU_SOURCE 1

#include "util.h"
#include "image.h"

#include <stdint.h>
#include <string.h>

#if USE_POSIX_SHM || USE_MEMFD
#   include <errno.h>
#   include <fcntl.h>
#   include <sys/mman.h>
#   include <sys/stat.h>
#   include <unistd.h>
#   include <time.h>
#else
#   include <sys/ipc.h>
#   include <sys/shm.h>
#endif

/*
 * smmintrin.h (SSE4.1) is required for:
 *    - _mm_cvtepu8_epi16()
 * tmmintrin.h (SSSE3) is required for
 *    - _mm_shuffle_epi8()
 * All other intrinsics are SSE2 or earlier.
 */
#ifdef __SSE4_1__
#   include <smmintrin.h>
#elif defined(__SSSE3__)
#   include <tmmintrin.h>
#elif defined(__SSE2__)
#   include <emmintrin.h>
#endif

void free_image(struct image *im) {
    if (im->shmid >= 0) {
#if USE_POSIX_SHM
        if (im->data && im->data != MAP_FAILED) munmap(im->data, im->width * im->height * sizeof(color_t));
        close(im->shmid);
#else
        if (im->data && im->data != (color_t *)-1) shmdt(im->data);
        shmctl(im->shmid, IPC_RMID, NULL);
#endif
    } else {
        if (im->data) free(im->data);
    }
    im->shmid = -1;
    im->data = NULL;
}

struct image create_shm_image(int16_t width, int16_t height) {
    struct image im = {
        .width = width,
        .height = height,
        .shmid = -1,
    };
    size_t size = STRIDE(width) * height * sizeof(color_t);

#if USE_MEMFD || USE_POSIX_SHM
#if USE_MEMFD
    im.shmid = memfd_create("buffer", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    fcntl(im.shmid, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_SEAL);
#else
    char temp[] = "/nsst-XXXXXX";
    int32_t attempts = 16;

    do {
        struct timespec cur;
        clock_gettime(CLOCK_REALTIME, &cur);
        uint64_t r = cur.tv_nsec;
        for (int i = 0; i < 6; ++i, r >>= 5)
            temp[6+i] = 'A' + (r & 15) + (r & 16) * 2;
        im.shmid = shm_open(temp, O_RDWR | O_CREAT | O_EXCL, 0600);
    } while (im.shmid < 0 && errno == EEXIST && attempts-- > 0);

    shm_unlink(temp);
#endif
    if (im.shmid < 0) goto error;

    if (ftruncate(im.shmid, size) < 0) goto error;

    im.data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, im.shmid, 0);
    if (im.data == MAP_FAILED) goto error;
#else
    im.shmid = shmget(IPC_PRIVATE, size, IPC_CREAT | 0600);
    if (im.shmid == -1) goto error;

    im.data = shmat(im.shmid, 0, 0);
    if ((void *)im.data == (void *)-1) goto error;
#endif
    return im;
error:
    warn("Can't create image: %s", strerror(errno));
    free_image(&im);
    return im;
}

struct image create_image(int16_t width, int16_t height) {
    struct image im = {
        .width = width,
        .height = height,
        .shmid = -1,
    };

    size_t size = STRIDE(width) * height * sizeof(color_t);
    im.data = aligned_alloc(_Alignof(struct glyph), ROUNDUP(size, _Alignof(struct glyph)));
    return im;
}

static inline FORCEINLINE void draw_mask(uint32_t *dst, __m128i mask, __m128i value) {
    __m128i p = _mm_andnot_si128(mask, _mm_load_si128((__m128i *)dst));
    _mm_store_si128((__m128i *)dst, _mm_or_si128(p, value));
}

static inline FORCEINLINE void draw_bulk(uint32_t rem, __m128i *dst, __m128i *limit, __m128i value) {
    switch (rem) do {
        case 0: _mm_store_si128(dst + 4 - 4, value); /* fallthrough */
        case 3: _mm_store_si128(dst + 4 - 3, value); /* fallthrough */
        case 2: _mm_store_si128(dst + 4 - 2, value); /* fallthrough */
        case 1: _mm_store_si128(dst + 4 - 1, value); continue;
        default: __builtin_unreachable();
    } while ((dst += 4) < limit);
}

void image_draw_rect(struct image im, struct rect rect, color_t fg) {
    if (intersect_with(&rect, &(struct rect){0, 0, im.width, im.height})) {
        /* That's a hack for PutImage
         * PutImage cannot pass stride */
        if (im.shmid < 0 && rect.x + rect.width == im.width)
            rect.width += STRIDE(im.width) - im.width;

        ssize_t width = rect.width, height = rect.height;
        ssize_t stride = STRIDE(im.width);
#ifdef __SSE2__
        color_t *ptr = im.data + rect.y*stride + (rect.x & ~3);

        ssize_t prefix = -rect.x & 3;
        ssize_t suffix = (rect.x + width) & 3;
        ssize_t width4 = ((width + rect.x + 3) & ~3) - (rect.x & ~3);
        ssize_t xmax = (width4 + 3)/4 - !!suffix;
        ssize_t xrem = (xmax - !!prefix) & 3;
        ssize_t xinit = !!prefix + (xrem - 4)*!!xrem;

        __m128i fg4 = _mm_set1_epi32(fg);
        __m128i pmask = _mm_cmpgt_epi32(_mm_set1_epi32(prefix), _mm_setr_epi32(3,2,1,0));
        __m128i smask = _mm_cmpgt_epi32(_mm_set1_epi32(suffix), _mm_setr_epi32(0,1,2,3));
        __m128i pmask_fg = _mm_and_si128(fg4, pmask);
        __m128i smask_fg = _mm_and_si128(fg4, smask);

        if (suffix && prefix) {
            if (width4 > 8) /* big with unaligned suffix and prefix */ {
                for (uint32_t *y = ptr, *yend = ptr + stride*height; y < yend; y += stride) {
                    draw_mask(y, pmask, pmask_fg);
                    draw_bulk(xrem, (__m128i *)y + xinit, (__m128i *)y + xmax, fg4);
                    draw_mask(y + width4 - 4, smask, smask_fg);
                }
            } else if (width4 > 4) /* small with unaligned suffix and prefix */ {
                for (uint32_t *y = ptr, *yend = ptr + stride*height; y < yend; y += stride) {
                    draw_mask(y, pmask, pmask_fg);
                    draw_mask(y + 4, smask, smask_fg);
                }
            } else /* less than 4, unaligned */ {
                __m128i mask = _mm_and_si128(pmask, smask);
                __m128i mask_fg = _mm_and_si128(fg4, mask);
                for (uint32_t *y = ptr, *yend = ptr + stride*height; y < yend; y += stride) {
                    draw_mask(y, mask, mask_fg);
                }
            }
        } else if (suffix) {
            if (width4 > 4) /* big with unaligned suffix */ {
                for (uint32_t *y = ptr, *yend = ptr + stride*height; y < yend; y += stride) {
                    draw_bulk(xrem, (__m128i *)y + xinit, (__m128i *)y + xmax, fg4);
                    draw_mask(y + width4 - 4, smask, smask_fg);
                }
            } else /* small with unaligned suffix */ {
                for (uint32_t *y = ptr, *yend = ptr + stride*height; y < yend; y += stride) {
                    draw_mask(y, smask, smask_fg);
                }
            }
        } else if (prefix) {
            if (width4 > 4) /* big with unaligned prefix */ {
                for (uint32_t *y = ptr, *yend = ptr + stride*height; y < yend; y += stride) {
                    draw_mask(y, pmask, pmask_fg);
                    draw_bulk(xrem, (__m128i *)y + xinit, (__m128i *)y + xmax, fg4);
                }
            } else /* small with unaligned prefix */ {
                for (uint32_t *y = ptr, *yend = ptr + stride*height; y < yend; y += stride) {
                    draw_mask(y, pmask, pmask_fg);
                }
            }
        } else /* everything is aligned */ {
            for (uint32_t *y = ptr, *yend = ptr + stride*height; y < yend; y += stride) {
                draw_bulk(xrem, (__m128i *)y + xinit, (__m128i *)y + xmax, fg4);
            }
        }
#else
        for (ssize_t j = 0; j < height; j++)
            for (ssize_t i = 0; i < width; i++)
                im.data[(rect.y + j) * stride + (rect.x + i)] = fg;
#endif
    }
}

#ifdef __SSE2__
FORCEINLINE
static inline __m128i op_over4(__m128i bg8, __m128i fg16, uint32_t alpha) {
    const __m128i m255 = _mm_set1_epi32(0x00FF00FF);
    const __m128i zero = _mm_set1_epi32(0x00000000);
    const __m128i div  = _mm_set1_epi16(-32639);

    /* alpha */
#ifdef __SSSE3__
    const __m128i allo = _mm_setr_epi32(0xFF00FF00, 0xFF00FF00, 0xFF01FF01, 0xFF01FF01);
    const __m128i alhi = _mm_setr_epi32(0xFF02FF02, 0xFF02FF02, 0xFF03FF03, 0xFF03FF03);
    __m128i valpha = _mm_set1_epi32(alpha);
    __m128i al_0 = _mm_shuffle_epi8(valpha, allo);
    __m128i al_1 = _mm_shuffle_epi8(valpha, alhi);
#else
    __m128i valpha = _mm_unpacklo_epi8(_mm_set1_epi32(alpha), zero);
    __m128i al_0 = _mm_shufflehi_epi16(_mm_shufflelo_epi16(valpha, 0x00), 0x55);
    __m128i al_1 = _mm_shufflehi_epi16(_mm_shufflelo_epi16(valpha, 0xAA), 0xFF);
#endif

    /* fg*alpha */
    __m128i mfg_0 = _mm_mullo_epi16(fg16, al_0);
    __m128i mfg_1 = _mm_mullo_epi16(fg16, al_1);

    /* 255-alpha */
    __m128i mal_0 = _mm_xor_si128(m255, al_0);
    __m128i mal_1 = _mm_xor_si128(m255, al_1);
    /* bg*(255-alpha) */
#ifdef __SSE4_1__
    __m128i mbg_0 = _mm_mullo_epi16(_mm_cvtepu8_epi16(bg8), mal_0);
#else
    __m128i mbg_0 = _mm_mullo_epi16(_mm_unpacklo_epi8(bg8, zero), mal_0);
#endif
    __m128i mbg_1 = _mm_mullo_epi16(_mm_unpackhi_epi8(bg8, zero), mal_1);

    /* bg*(255-alpha) + fg*alpha */
    __m128i res_0 = _mm_adds_epu16(mfg_0, mbg_0);
    __m128i res_1 = _mm_adds_epu16(mfg_1, mbg_1);

    /* (bg*(255-alpha) + fg*alpha)/255 */
    __m128i div_0 = _mm_srli_epi16(_mm_mulhi_epu16(res_0, div), 7);
    __m128i div_1 = _mm_srli_epi16(_mm_mulhi_epu16(res_1, div), 7);
    return _mm_packus_epi16(div_0, div_1);
}

FORCEINLINE
static inline __m128i op_over4_subpix(__m128i bg8, __m128i fg16, __m128i alpha) {
    const __m128i m255 = _mm_set1_epi32(0x00FF00FF);
    const __m128i zero = _mm_set1_epi32(0x00000000);
    const __m128i div  = _mm_set1_epi16(-32639);

    /* alpha */
#ifdef __SSE4_1__
    __m128i al_0 = _mm_cvtepu8_epi16(alpha);
#else
    __m128i al_0 = _mm_unpacklo_epi8(alpha, zero);
#endif
    __m128i al_1 = _mm_unpackhi_epi8(alpha, zero);
    /* fg*alpha */
    __m128i mfg_0 = _mm_mullo_epi16(fg16, al_0);
    __m128i mfg_1 = _mm_mullo_epi16(fg16, al_1);

    /* 255-alpha */
    __m128i mal_0 = _mm_xor_si128(m255, al_0);
    __m128i mal_1 = _mm_xor_si128(m255, al_1);
    /* bg*(255-alpha) */
#ifdef __SSE4_1__
    __m128i mbg_0 = _mm_mullo_epi16(_mm_cvtepu8_epi16(bg8), mal_0);
#else
    __m128i mbg_0 = _mm_mullo_epi16(_mm_unpacklo_epi8(bg8, zero), mal_0);
#endif
    __m128i mbg_1 = _mm_mullo_epi16(_mm_unpackhi_epi8(bg8, zero), mal_1);

    /* bg*(255-alpha) + fg*alpha */
    __m128i res_0 = _mm_adds_epu16(mfg_0, mbg_0);
    __m128i res_1 = _mm_adds_epu16(mfg_1, mbg_1);

    /* (bg*(255-alpha) + fg*alpha)/255 */
    __m128i div_0 = _mm_srli_epi16(_mm_mulhi_epu16(res_0, div), 7);
    __m128i div_1 = _mm_srli_epi16(_mm_mulhi_epu16(res_1, div), 7);
    return _mm_packus_epi16(div_0, div_1);
}

FORCEINLINE
static inline __m128i op_blend(__m128i under, __m128i over) {
    const __m128i zero = _mm_set1_epi32(0x00000000);
    const __m128  m255 = (__m128)_mm_set1_epi32(0x00FF00FF);
    const __m128i div  = _mm_set1_epi16(-32639);
#ifndef __SSSE3__
    __m128i al_0 = _mm_shufflehi_epi16(_mm_shufflelo_epi16(_mm_unpacklo_epi8(over, zero), 0xFF), 0xFF);
    __m128i al_1 = _mm_shufflehi_epi16(_mm_shufflelo_epi16(_mm_unpackhi_epi8(over, zero), 0xFF), 0xFF);
#else
    const __m128i allo = _mm_setr_epi32(0xFF03FF03, 0xFF03FF03, 0xFF07FF07, 0xFF07FF07);
    const __m128i alhi = _mm_setr_epi32(0xFF0BFF0B, 0xFF0BFF0B, 0xFF0FFF0F, 0xFF0FFF0F);
    __m128i al_0 = _mm_shuffle_epi8(over, allo);
    __m128i al_1 = _mm_shuffle_epi8(over, alhi);
#endif
    __m128i mal_0 = (__m128i)_mm_xor_ps(m255, (__m128)al_0);
    __m128i mal_1 = (__m128i)_mm_xor_ps(m255, (__m128)al_1);

#ifndef __SSE4_1__
    __m128i mul_0 = _mm_mullo_epi16(_mm_unpacklo_epi8(under, zero), mal_0);
#else
    __m128i mul_0 = _mm_mullo_epi16(_mm_cvtepu8_epi16(under), mal_0);
#endif
    __m128i mul_1 = _mm_mullo_epi16(_mm_unpackhi_epi8(under, zero), mal_1);
    __m128i div_0 = _mm_srli_epi16(_mm_mulhi_epu16(mul_0, div), 7);
    __m128i div_1 = _mm_srli_epi16(_mm_mulhi_epu16(mul_1, div), 7);
    return _mm_adds_epu8(over, _mm_packus_epi16(div_0, div_1));
}

FORCEINLINE
static inline __m128i load_masked(void *src, int w, int s) {
    uint32_t *ptr = src;
    if (s) {
        switch (w) {
        case 1: return _mm_setr_epi32(0, 0, 0, ptr[0]);
        case 2: return _mm_setr_epi32(0, 0, ptr[0], ptr[1]);
        case 3: return _mm_setr_epi32(0, ptr[0], ptr[1], ptr[2]);
        default: return _mm_set1_epi32(0);
        }
    } else {
        switch (w) {
        case 1: return _mm_setr_epi32(ptr[0], 0, 0, 0);
        case 2: return _mm_setr_epi32(ptr[0], ptr[1], 0, 0);
        case 3: return _mm_setr_epi32(ptr[0], ptr[1], ptr[2], 0);
        default: return _mm_set1_epi32(0);
        }
    }
}

FORCEINLINE
static inline void over_mask(void *dst, __m128i fg16, __m128i mask, void *palpha, int d, int s) {
    uint32_t alpha = 0;
    memcpy(&alpha, palpha, d);
    __m128i pref = _mm_load_si128(dst);
    __m128i dstm = _mm_andnot_si128(mask, pref);
    __m128i srcm = _mm_and_si128(mask, op_over4(pref, fg16, alpha << s*8));
    _mm_store_si128(dst, _mm_or_si128(srcm, dstm));
}

FORCEINLINE
static inline void over(void *dst, __m128i fg16, void *palpha) {
    uint32_t alpha;
    memcpy(&alpha, palpha, sizeof(alpha));
    __m128i pref = _mm_load_si128(dst);
    _mm_store_si128(dst, op_over4(pref, fg16, alpha));
}

FORCEINLINE
static inline void over_mask_subpix(void *dst, __m128i fg16, __m128i mask, void *palpha, int d, int s) {
    __m128i alpha = load_masked(palpha, d, s);
    __m128i pref = _mm_load_si128(dst);
    __m128i dstm = _mm_andnot_si128(mask, pref);
    __m128i srcm = _mm_and_si128(mask, op_over4_subpix(pref, fg16, alpha));
    _mm_store_si128(dst, _mm_or_si128(srcm, dstm));
}

FORCEINLINE
static inline void over_subpix(void *dst, __m128i fg16, void *palpha) {
    __m128i alpha = _mm_loadu_si128(palpha);
    __m128i pref = _mm_load_si128(dst);
    _mm_store_si128(dst, op_over4_subpix(pref, fg16, alpha));
}

FORCEINLINE
static inline void over_subpix_aligned(void *dst, __m128i fg16, void *palpha) {
    __m128i alpha = _mm_load_si128(palpha);
    __m128i pref = _mm_load_si128(dst);
    _mm_store_si128(dst, op_over4_subpix(pref, fg16, alpha));
}

FORCEINLINE
static inline void blend_mask(void *dst, __m128i mask, void *palpha, int d, int s) {
    __m128i alpha = load_masked(palpha, d, s);
    __m128i pref = _mm_load_si128(dst);
    __m128i dstm = _mm_andnot_si128(mask, pref);
    __m128i srcm = _mm_and_si128(mask, op_blend(pref, alpha));
    _mm_store_si128(dst, _mm_or_si128(srcm, dstm));
}

FORCEINLINE
static inline void blend(void *dst, void *palpha) {
    __m128i alpha = _mm_loadu_si128(palpha);
    __m128i pref = _mm_load_si128(dst);
    _mm_store_si128(dst, op_blend(pref, alpha));
}

FORCEINLINE
static inline void blend_aligned(void *dst, void *palpha) {
    __m128i alpha = _mm_load_si128(palpha);
    __m128i pref = _mm_load_si128(dst);
    _mm_store_si128(dst, op_blend(pref, alpha));
}
#else
static inline void op_over(color_t *bg, color_t fg, uint8_t alpha) {
    *bg =
        (((*bg >>  0) & 0xFF) * (255 - alpha) + ((fg >>  0) & 0xFF) * alpha) / 255 << 0 |
        (((*bg >>  8) & 0xFF) * (255 - alpha) + ((fg >>  8) & 0xFF) * alpha) / 255 << 8 |
        (((*bg >> 16) & 0xFF) * (255 - alpha) + ((fg >> 16) & 0xFF) * alpha) / 255 << 16 |
        (((*bg >> 24) & 0xFF) * (255 - alpha) + ((fg >> 24) & 0xFF) * alpha) / 255 << 24;
}

static inline void op_over_subpix(color_t *bg, color_t fg, uint8_t *alpha) {
    *bg =
        (((*bg >>  0) & 0xFF) * (255 - alpha[0]) + ((fg >>  0) & 0xFF) * alpha[0]) / 255 << 0 |
        (((*bg >>  8) & 0xFF) * (255 - alpha[1]) + ((fg >>  8) & 0xFF) * alpha[1]) / 255 << 8 |
        (((*bg >> 16) & 0xFF) * (255 - alpha[2]) + ((fg >> 16) & 0xFF) * alpha[2]) / 255 << 16 |
        (((*bg >> 24) & 0xFF) * (255 - alpha[3]) + ((fg >> 24) & 0xFF) * alpha[3]) / 255 << 24;
}

static inline void op_blend(color_t *bg, color_t fg) {
    uint32_t ralpha = 255 - ((fg >> 24) & 0xFF);
    *bg =
        (((*bg >>  0) & 0xFF) * ralpha / 255 + ((fg >>  0) & 0xFF)) << 0 |
        (((*bg >>  8) & 0xFF) * ralpha / 255 + ((fg >>  8) & 0xFF)) << 8 |
        (((*bg >> 16) & 0xFF) * ralpha / 255 + ((fg >> 16) & 0xFF)) << 16 |
        (((*bg >> 24) & 0xFF) * ralpha / 255 + ((fg >> 24) & 0xFF)) << 24;
}
#endif

void image_compose_glyph(struct image im, int16_t dx, int16_t dy, struct glyph *glyph, color_t fg, struct rect clip) {
    struct rect rect = { dx - glyph->x, dy - glyph->y, glyph->width, glyph->height };
    if (intersect_with(&rect, &(struct rect){0, 0, im.width, im.height}) &&
            intersect_with(&rect, &clip)) {
        int16_t i0 = rect.x - dx + glyph->x, j0 = rect.y - dy + glyph->y;
        ssize_t width = rect.width, height = rect.height;
        ssize_t stride = STRIDE(im.width), gstride = glyph->stride;
#ifdef __SSE2__
        if (glyph->pixmode == pixmode_mono) {
            uint8_t *aptr = glyph->data + j0 * gstride + i0  - (rect.x & 3);
            color_t *dptr = im.data + rect.y * stride + (rect.x & ~3);

            ssize_t prefix = -rect.x & 3, nprefix = rect.x & 3;
            ssize_t suffix = (rect.x + width) & 3;
            ssize_t width4 = ((width + rect.x + 3) & ~3) - (rect.x & ~3);

#ifndef __SSE4_1__
            const __m128i zero = _mm_set1_epi32(0x00000000);
            __m128i fg16 = _mm_unpacklo_epi8(_mm_set1_epi32(fg), zero);
#else
            __m128i fg16 = _mm_cvtepu8_epi16(_mm_set1_epi32(fg));
#endif
            __m128i pmask = _mm_cmpgt_epi32(_mm_set1_epi32(prefix), _mm_setr_epi32(3,2,1,0));
            __m128i smask = _mm_cmpgt_epi32(_mm_set1_epi32(suffix), _mm_setr_epi32(0,1,2,3));

            if (suffix && prefix) {
                if (width4 > 8) /* big with unaligned suffix and prefix */ {
                    for (ssize_t y = 0; y < height; y++) {
                        over_mask(&dptr[y * stride], fg16, pmask, &aptr[y * gstride + nprefix], prefix, nprefix);
                        for (ssize_t x = 4; x < width4 - 4; x += 4)
                            over(&dptr[y * stride + x], fg16, &aptr[y * gstride + x]);
                        over_mask(&dptr[y * stride + width4 - 4], fg16, smask, &aptr[y * gstride + width4 - 4], suffix, 0);
                    }
                } else if (width4 > 4) /* small with unaligned suffix and prefix */ {
                    for (ssize_t y = 0; y < height; y++) {
                        over_mask(&dptr[y * stride], fg16, pmask, &aptr[y * gstride + nprefix], prefix, nprefix);
                        over_mask(&dptr[y * stride + 4], fg16, smask, &aptr[y * gstride + 4], suffix, 0);
                    }
                } else /* less than 4, unaligned */ {
                    __m128i mask = _mm_and_si128(pmask, smask);
                    for (ssize_t y = 0; y < height; y++) {
                        over_mask(&dptr[y * stride], fg16, mask, &aptr[y * gstride + nprefix], width, nprefix);
                    }
                }
            } else if (suffix) {
                if (width4 > 4) /* big with unaligned suffix */ {
                    for (ssize_t y = 0; y < height; y++) {
                        for (ssize_t x = 0; x < width4 - 4; x += 4)
                            over(&dptr[y * stride + x], fg16, &aptr[y * gstride + x]);
                        over_mask(&dptr[y * stride + width4 - 4], fg16, smask, &aptr[y * gstride + width4 - 4], suffix, 0);
                    }
                } else /* small with unaligned suffix */ {
                    for (ssize_t y = 0; y < height; y++) {
                        over_mask(&dptr[y * stride], fg16, smask, &aptr[y * gstride], suffix, 0);
                    }
                }
            } else if (prefix) {
                if (width4 > 4) /* big with unaligned prefix */ {
                    for (ssize_t y = 0; y < height; y++) {
                        over_mask(&dptr[y * stride], fg16, pmask, &aptr[y * gstride + nprefix], prefix, nprefix);
                        for (ssize_t x = 4; x < width4; x += 4)
                            over(&dptr[y * stride + x], fg16, &aptr[y * gstride + x]);
                    }
                } else /* small with unaligned prefix */ {
                    for (ssize_t y = 0; y < height; y++) {
                        over_mask(&dptr[y * stride], fg16, pmask, &aptr[y * gstride + nprefix], prefix, nprefix);
                    }
                }
            } else /* everything is aligned */ {
                for (ssize_t y = 0; y < height; y++) {
                    for (ssize_t x = 0; x < width4; x += 4)
                        over(&dptr[y * stride + x], fg16, &aptr[y * gstride + x]);
                }
            }
        } else if (glyph->pixmode == pixmode_bgra) {
            uint8_t *aptr = glyph->data + j0 * gstride + 4*(i0  - (rect.x & 3));
            color_t *dptr = im.data + rect.y * stride + (rect.x & ~3);

            ssize_t prefix = -rect.x & 3, nprefix = rect.x & 3;
            ssize_t suffix = (rect.x + width) & 3;
            ssize_t width4 = ((width + rect.x + 3) & ~3) - (rect.x & ~3);

            __m128i pmask = _mm_cmpgt_epi32(_mm_set1_epi32(prefix), _mm_setr_epi32(3,2,1,0));
            __m128i smask = _mm_cmpgt_epi32(_mm_set1_epi32(suffix), _mm_setr_epi32(0,1,2,3));

            bool a_aligned = !((uintptr_t)aptr & 15);
            if (suffix && prefix) {
                if (width4 > 8) /* big with unaligned suffix and prefix */ {
                    if (a_aligned) {
                        for (ssize_t y = 0; y < height; y++) {
                            blend_mask(&dptr[y * stride], pmask, &aptr[y * gstride + 4*nprefix], prefix, nprefix);
                            for (ssize_t x = 4; x < width4 - 4; x += 4)
                                blend_aligned(&dptr[y * stride + x], &aptr[y * gstride + 4*x]);
                            blend_mask(&dptr[y * stride + width4 - 4], smask, &aptr[y * gstride + 4*(width4 - 4)], suffix, 0);
                        }
                    } else {
                        for (ssize_t y = 0; y < height; y++) {
                            blend_mask(&dptr[y * stride], pmask, &aptr[y * gstride + 4*nprefix], prefix, nprefix);
                            for (ssize_t x = 4; x < width4 - 4; x += 4)
                                blend(&dptr[y * stride + x], &aptr[y * gstride + 4*x]);
                            blend_mask(&dptr[y * stride + width4 - 4], smask, &aptr[y * gstride + 4*(width4 - 4)], suffix, 0);
                        }
                    }
                } else if (width4 > 4) /* small with unaligned suffix and prefix */ {
                    for (ssize_t y = 0; y < height; y++) {
                        blend_mask(&dptr[y * stride], pmask, &aptr[y * gstride + 4*nprefix], prefix, nprefix);
                        blend_mask(&dptr[y * stride + 4], smask, &aptr[y * gstride + 4*4], suffix, 0);
                    }
                } else /* less than 4, unaligned */ {
                    __m128i mask = _mm_and_si128(pmask, smask);
                    for (ssize_t y = 0; y < height; y++) {
                        blend_mask(&dptr[y * stride], mask, &aptr[y * gstride + 4*nprefix], width, nprefix);
                    }
                }
            } else if (suffix) {
                if (width4 > 4) /* big with unaligned suffix */ {
                    if (a_aligned) {
                        for (ssize_t y = 0; y < height; y++) {
                            for (ssize_t x = 0; x < width4 - 4; x += 4)
                                blend_aligned(&dptr[y * stride + x], &aptr[y * gstride + 4*x]);
                            blend_mask(&dptr[y * stride + width4 - 4], smask, &aptr[y * gstride + 4*(width4 - 4)], suffix, 0);
                        }
                    } else {
                        for (ssize_t y = 0; y < height; y++) {
                            for (ssize_t x = 0; x < width4 - 4; x += 4)
                                blend(&dptr[y * stride + x], &aptr[y * gstride + 4*x]);
                            blend_mask(&dptr[y * stride + width4 - 4], smask, &aptr[y * gstride + 4*(width4 - 4)], suffix, 0);
                        }
                    }
                } else /* small with unaligned suffix */ {
                    for (ssize_t y = 0; y < height; y++) {
                        blend_mask(&dptr[y * stride], smask, &aptr[y * gstride], suffix, 0);
                    }
                }
            } else if (prefix) {
                if (width4 > 4) /* big with unaligned prefix */ {
                    if (a_aligned) {
                        for (ssize_t y = 0; y < height; y++) {
                            blend_mask(&dptr[y * stride], pmask, &aptr[y * gstride + 4*nprefix], prefix, nprefix);
                            for (ssize_t x = 4; x < width4; x += 4)
                                blend_aligned(&dptr[y * stride + x], &aptr[y * gstride + 4*x]);
                        }
                    } else {
                        for (ssize_t y = 0; y < height; y++) {
                            blend_mask(&dptr[y * stride], pmask, &aptr[y * gstride + 4*nprefix], prefix, nprefix);
                            for (ssize_t x = 4; x < width4; x += 4)
                                blend(&dptr[y * stride + x], &aptr[y * gstride + 4*x]);
                        }
                    }
                } else /* small with unaligned prefix */ {
                    for (ssize_t y = 0; y < height; y++) {
                        blend_mask(&dptr[y * stride], pmask, &aptr[y * gstride + 4*nprefix], prefix, nprefix);
                    }
                }
            } else /* everything is aligned */ {
                if (a_aligned) {
                    for (ssize_t y = 0; y < height; y++) {
                        for (ssize_t x = 0; x < width4; x += 4)
                            blend_aligned(&dptr[y * stride + x], &aptr[y * gstride + 4*x]);
                    }
                } else {
                    for (ssize_t y = 0; y < height; y++) {
                        for (ssize_t x = 0; x < width4; x += 4)
                            blend(&dptr[y * stride + x], &aptr[y * gstride + 4*x]);
                    }
                }
            }
        } else {
            uint8_t *aptr = glyph->data + j0 * gstride + 4*(i0  - (rect.x & 3));
            color_t *dptr = im.data + rect.y * stride + (rect.x & ~3);

            ssize_t prefix = -rect.x & 3, nprefix = rect.x & 3;
            ssize_t suffix = (rect.x + width) & 3;
            ssize_t width4 = ((width + rect.x + 3) & ~3) - (rect.x & ~3);

#ifdef __SSE4_1__
            __m128i fg16 = _mm_cvtepu8_epi16(_mm_set1_epi32(fg));
#else
            const __m128i zero = _mm_set1_epi32(0x0);
            __m128i fg16 = _mm_unpacklo_epi8(_mm_set1_epi32(fg), zero);
#endif
            __m128i pmask = _mm_cmpgt_epi32(_mm_set1_epi32(prefix), _mm_setr_epi32(3,2,1,0));
            __m128i smask = _mm_cmpgt_epi32(_mm_set1_epi32(suffix), _mm_setr_epi32(0,1,2,3));

            bool a_aligned = !((uintptr_t)aptr & 15);
            if (suffix && prefix) {
                if (width4 > 8) /* big with unaligned suffix and prefix */ {
                    if (a_aligned) {
                        for (ssize_t y = 0; y < height; y++) {
                            over_mask_subpix(&dptr[y * stride], fg16, pmask, &aptr[y * gstride + 4*nprefix], prefix, nprefix);
                            for (ssize_t x = 4; x < width4 - 4; x += 4)
                                over_subpix_aligned(&dptr[y * stride + x], fg16, &aptr[y * gstride + 4*x]);
                            over_mask_subpix(&dptr[y * stride + width4 - 4], fg16, smask, &aptr[y * gstride + 4*(width4 - 4)], suffix, 0);
                        }
                    } else {
                        for (ssize_t y = 0; y < height; y++) {
                            over_mask_subpix(&dptr[y * stride], fg16, pmask, &aptr[y * gstride + 4*nprefix], prefix, nprefix);
                            for (ssize_t x = 4; x < width4 - 4; x += 4)
                                over_subpix(&dptr[y * stride + x], fg16, &aptr[y * gstride + 4*x]);
                            over_mask_subpix(&dptr[y * stride + width4 - 4], fg16, smask, &aptr[y * gstride + 4*(width4 - 4)], suffix, 0);
                        }
                    }
                } else if (width4 > 4) /* small with unaligned suffix and prefix */ {
                    for (ssize_t y = 0; y < height; y++) {
                        over_mask_subpix(&dptr[y * stride], fg16, pmask, &aptr[y * gstride + 4*nprefix], prefix, nprefix);
                        over_mask_subpix(&dptr[y * stride + 4], fg16, smask, &aptr[y * gstride + 4*4], suffix, 0);
                    }
                } else /* less than 4, unaligned */ {
                    __m128i mask = _mm_and_si128(pmask, smask);
                    for (ssize_t y = 0; y < height; y++) {
                        over_mask_subpix(&dptr[y * stride], fg16, mask, &aptr[y * gstride + 4*nprefix], width, nprefix);
                    }
                }
            } else if (suffix) {
                if (width4 > 4) /* big with unaligned suffix */ {
                    if (a_aligned) {
                        for (ssize_t y = 0; y < height; y++) {
                            for (ssize_t x = 0; x < width4 - 4; x += 4)
                                over_subpix_aligned(&dptr[y * stride + x], fg16, &aptr[y * gstride + 4*x]);
                            over_mask_subpix(&dptr[y * stride + width4 - 4], fg16, smask, &aptr[y * gstride + 4*(width4 - 4)], suffix, 0);
                        }
                    } else {
                        for (ssize_t y = 0; y < height; y++) {
                            for (ssize_t x = 0; x < width4 - 4; x += 4)
                                over_subpix(&dptr[y * stride + x], fg16, &aptr[y * gstride + 4*x]);
                            over_mask_subpix(&dptr[y * stride + width4 - 4], fg16, smask, &aptr[y * gstride + 4*(width4 - 4)], suffix, 0);
                        }
                    }
                } else /* small with unaligned suffix */ {
                    for (ssize_t y = 0; y < height; y++) {
                        over_mask_subpix(&dptr[y * stride], fg16, smask, &aptr[y * gstride], suffix, 0);
                    }
                }
            } else if (prefix) {
                if (width4 > 4) /* big with unaligned prefix */ {
                    if (a_aligned) {
                        for (ssize_t y = 0; y < height; y++) {
                            over_mask_subpix(&dptr[y * stride], fg16, pmask, &aptr[y * gstride + 4*nprefix], prefix, nprefix);
                            for (ssize_t x = 4; x < width4; x += 4)
                                over_subpix_aligned(&dptr[y * stride + x], fg16, &aptr[y * gstride + 4*x]);
                        }
                    } else {
                        for (ssize_t y = 0; y < height; y++) {
                            over_mask_subpix(&dptr[y * stride], fg16, pmask, &aptr[y * gstride + 4*nprefix], prefix, nprefix);
                            for (ssize_t x = 4; x < width4; x += 4)
                                over_subpix(&dptr[y * stride + x], fg16, &aptr[y * gstride + 4*x]);
                        }
                    }
                } else /* small with unaligned prefix */ {
                    for (ssize_t y = 0; y < height; y++) {
                        over_mask_subpix(&dptr[y * stride], fg16, pmask, &aptr[y * gstride + 4*nprefix], prefix, nprefix);
                    }
                }
            } else /* everything is aligned */ {
                if (a_aligned) {
                    for (ssize_t y = 0; y < height; y++) {
                        for (ssize_t x = 0; x < width4; x += 4)
                            over_subpix_aligned(&dptr[y * stride + x], fg16, &aptr[y * gstride + 4*x]);
                    }
                } else {
                    for (ssize_t y = 0; y < height; y++) {
                        for (ssize_t x = 0; x < width4; x += 4)
                            over_subpix(&dptr[y * stride + x], fg16, &aptr[y * gstride + 4*x]);
                    }
                }
            }
        }
#else
        if (glyph->pixmode == pixmode_mono) {
            uint8_t *aptr = glyph->data + j0 * gstride + i0;
            color_t *dptr = im.data + rect.y * stride + rect.x;

            for (ssize_t j = 0; j < height; j++)
                for (ssize_t i = 0; i < width; i++)
                    op_over(&dptr[j * stride + i], fg, aptr[j * gstride +i]);
        } else if (glyph->pixmode == pixmode_bgra) {
            uint8_t *aptr = glyph->data + j0 * gstride + 4*i0;
            color_t *dptr = im.data + rect.y * stride + rect.x;
            for (ssize_t j = 0; j < height; j++)
                for (ssize_t i = 0; i < width; i++) {
                    color_t c;
                    memcpy(&c, &aptr[j * gstride + 4 * i], sizeof(c));
                    op_blend(&dptr[j * stride + i], c);
                }
        } else {
            uint8_t *aptr = glyph->data + j0 * gstride + 4*i0;
            color_t *dptr = im.data + rect.y * stride + rect.x;
            for (ssize_t j = 0; j < height; j++)
                for (ssize_t i = 0; i < width; i++)
                    op_over_subpix(&dptr[j * stride + i], fg, &aptr[j * gstride + 4 * i]);
        }
#endif
    }
}

void image_copy(struct image dst, struct rect rect, struct image src, int16_t sx, int16_t sy) {
    rect.width = MAX(0, MIN(rect.width + sx, src.width) - sx);
    rect.height = MAX(0, MIN(rect.height + sy, src.height) - sy);

    if (intersect_with(&rect, &(struct rect){0, 0, dst.width, dst.height})) {
#ifdef __SSE2__
        ssize_t width = rect.width, height = rect.height;
        ssize_t dstride = STRIDE(dst.width);
        ssize_t sstride = STRIDE(src.width);

        color_t *dptr = dst.data + rect.y*dstride + rect.x;
        color_t *sptr = src.data + sy*sstride + sx;

        if (rect.y < sy || (rect.y == sy && rect.x <= sx)) /* Copy forward */ {

            /* First, fill unaligned prefix */
            if (rect.x & 3) {
                ssize_t w = MIN(4 - (rect.x & 3), width);
                for (ssize_t y = 0; y < height; y++)
                    for (ssize_t x = 0; x < w; x++)
                        dptr[y * dstride + x] = sptr[y * sstride + x];
                width -= w;
                dptr += w;
                sptr += w;
            }
            if (width <= 0) return;

            /* Then fill aligned part */
            ssize_t width4 = width & ~3;
            /* Two cases depending on aligning of source pointer
             * (destination pointer is aligned anyway) */
            if ((uintptr_t)sptr & 15) {
                for (ssize_t y = 0; y < height; y++) {
                    for (ssize_t x = 0; x < width4; x += 4) {
                        const __m128i fg4 = _mm_loadu_si128((__m128i *)&sptr[y * sstride + x]);
                        _mm_store_si128((__m128i *)&dptr[y * dstride + x], fg4);
                    }
                    for (ssize_t x = width4; x < width; x++)
                        dptr[y * dstride + x] = sptr[y * sstride + x];
                }
            } else {
                for (ssize_t y = 0; y < height; y++) {
                    for (ssize_t x = 0; x < width4; x += 4) {
                        const __m128i fg4 = _mm_load_si128((__m128i *)&sptr[y * sstride + x]);
                        _mm_store_si128((__m128i *)&dptr[y * dstride + x], fg4);
                    }
                    for (ssize_t x = width4; x < width; x++)
                        dptr[y * dstride + x] = sptr[y * sstride + x];
                }
            }
        } else /* Copy backward */ {
            /* First, fill unaligned suffix */
            if ((rect.x + width) & 3) {
                ssize_t w = MIN((rect.x + width) & 3, width);
                for (ssize_t y = height - 1; y >= 0; y--)
                    for (ssize_t x = width - 1; x >= width - w; x--)
                        dptr[y * dstride + x] = sptr[y * sstride + x];
                width -= w;
            }
            if (width <= 0) return;

            /* Then fill aligned part */

            /* Two cases depending on aligning of source pointer
             * (destination pointer is aligned anyway) */

            if ((uintptr_t)(sptr + width) & 15) {
                for (ssize_t y = height - 1; y >= 0; y--) {
                    for (ssize_t x = width - 4; x >= 0; x -= 4) {
                        const __m128i fg4 = _mm_loadu_si128((__m128i *)&sptr[y * sstride + x]);
                        _mm_store_si128((__m128i *)&dptr[y * dstride + x], fg4);
                    }
                    for (ssize_t x = (width & 3) - 1; x >= 0; x--)
                        dptr[y * dstride + x] = sptr[y * sstride + x];
                }
            } else {
                for (ssize_t y = height - 1; y >= 0; y--) {
                    for (ssize_t x = width - 4; x >= 0; x -= 4) {
                        const __m128i fg4 = _mm_load_si128((__m128i *)&sptr[y * sstride + x]);
                        _mm_store_si128((__m128i *)&dptr[y * dstride + x], fg4);
                    }
                    for (ssize_t x = (width & 3) - 1; x >= 0; x--)
                        dptr[y * dstride + x] = sptr[y * sstride + x];
                }
            }
        }
#else
        if (rect.y < sy || (rect.y == sy && rect.x <= sx)) {
            for (size_t j = 0; j < (size_t)rect.height; j++) {
                for (size_t i = 0; i < (size_t)rect.width; i++) {
                    dst.data[(rect.y + j) * STRIDE(dst.width) + (rect.x + i)] = src.data[STRIDE(src.width) * (sy + j) + (sx + i)];
                }
            }
        } else {
            for (size_t j = rect.height; j > 0; j--) {
                for (size_t i = rect.width; i > 0; i--) {
                    dst.data[(rect.y + j - 1) * STRIDE(dst.width) + (rect.x + i - 1)] = src.data[STRIDE(src.width) * (sy + j - 1) + (sx + i - 1)];
                }
            }
        }
#endif
    }
}
