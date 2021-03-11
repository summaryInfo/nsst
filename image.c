/* Copyright (c) 2019-2021, Evgeny Baskov. All rights reserved */

#define _POSIX_C_SOURCE 200809L

#include "feature.h"

#include "util.h"
#include "image.h"

#include <stdint.h>

#if USE_POSIX_SHM
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

#if USE_POSIX_SHM
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
    warn("Can't create image");
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
    im.data = aligned_alloc(CACHE_LINE, (size + CACHE_LINE - 1) & ~(CACHE_LINE - 1));
    return im;
}

void image_draw_rect(struct image im, struct rect rect, color_t fg) {
    if (intersect_with(&rect, &(struct rect){0, 0, im.width, im.height})) {
        // That's a hack for PutImage
        // PutImage cannot pass stride
        if (im.shmid < 0 && rect.x + rect.width == im.width)
            rect.width += STRIDE(im.width) - im.width;

        for (size_t j = 0; j < (size_t)rect.height; j++) {
            for (size_t i = 0; i < (size_t)rect.width; i++) {
                im.data[(rect.y + j) * STRIDE(im.width) + (rect.x + i)] = fg;
            }
        }
    }
}
void image_compose_glyph(struct image im, int16_t dx, int16_t dy, struct glyph *glyph, color_t fg, struct rect clip) {
    struct rect rect = { dx - glyph->x, dy - glyph->y, glyph->width, glyph->height };
    if (intersect_with(&rect, &(struct rect){0, 0, im.width, im.height}) &&
            intersect_with(&rect, &clip)) {
        int16_t i0 = rect.x - dx + glyph->x, j0 = rect.y - dy + glyph->y;
        if (glyph->pixmode == pixmode_mono) {
            for (size_t j = 0; j < (size_t)rect.height; j++) {
                for (size_t i = 0; i < (size_t)rect.width; i++) {
                    uint8_t alpha = glyph->data[(j0 + j) * glyph->stride + i0 + i];
                    color_t *bg = &im.data[(rect.y + j) * STRIDE(im.width) + (rect.x + i)];
                    *bg =
                        (((*bg >>  0) & 0xFF) * (255 - alpha) + ((fg >>  0) & 0xFF) * alpha) / 255 << 0 |
                        (((*bg >>  8) & 0xFF) * (255 - alpha) + ((fg >>  8) & 0xFF) * alpha) / 255 << 8 |
                        (((*bg >> 16) & 0xFF) * (255 - alpha) + ((fg >> 16) & 0xFF) * alpha) / 255 << 16 |
                        (((*bg >> 24) & 0xFF) * (255 - alpha) + ((fg >> 24) & 0xFF) * alpha) / 255 << 24;
                }
            }
        } else {
            for (size_t j = 0; j < (size_t)rect.height; j++) {
                for (size_t i = 0; i < (size_t)rect.width; i++) {
                    uint8_t *alpha = &glyph->data[(j0 + j) * glyph->stride + 4 * (i0 + i)];
                    color_t *bg = &im.data[(rect.y + j) * STRIDE(im.width) + (rect.x + i)];
                    *bg =
                        (((*bg >>  0) & 0xFF) * (255 - alpha[0]) + ((fg >>  0) & 0xFF) * alpha[0]) / 255 << 0 |
                        (((*bg >>  8) & 0xFF) * (255 - alpha[1]) + ((fg >>  8) & 0xFF) * alpha[1]) / 255 << 8 |
                        (((*bg >> 16) & 0xFF) * (255 - alpha[2]) + ((fg >> 16) & 0xFF) * alpha[2]) / 255 << 16 |
                        (((*bg >> 24) & 0xFF) * (255 - alpha[3]) + ((fg >> 24) & 0xFF) * alpha[3]) / 255 << 24;
                }
            }

        }
    }
}

void image_copy(struct image im, struct rect rect, struct image src, int16_t sx, int16_t sy) {
    rect.width = MAX(0, MIN(rect.width + sx, src.width) - sx);
    rect.height = MAX(0, MIN(rect.height + sy, src.height) - sy);

    if (intersect_with(&rect, &(struct rect){0, 0, im.width, im.height})) {
        if (rect.y < sy || (rect.y == sy && rect.x <= sx)) {
            for (size_t j = 0; j < (size_t)rect.height; j++) {
                for (size_t i = 0; i < (size_t)rect.width; i++) {
                    im.data[(rect.y + j) * STRIDE(im.width) + (rect.x + i)] = src.data[STRIDE(src.width) * (sy + j) + (sx + i)];
                }
            }
        } else {
            for (size_t j = rect.height; j > 0; j--) {
                for (size_t i = rect.width; i > 0; i--) {
                    im.data[(rect.y + j - 1) * STRIDE(im.width) + (rect.x + i - 1)] = src.data[STRIDE(src.width) * (sy + j - 1) + (sx + i - 1)];
                }
            }
        }
    }
}
