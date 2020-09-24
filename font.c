/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#include "feature.h"

#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "font.h"
#include "util.h"
#include "window.h"
#if USE_BOXDRAWING
#   include "boxdraw.h"
#endif

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_BITMAP_H
#include FT_FREETYPE_H
#include FT_LCD_FILTER_H

#define CAPS_STEP 2
#define HASH_INIT_CAP 167
#define HASH_CAP_INC(x) (8*(x)/5)

struct font_context {
    size_t fonts;
    FT_Library library;
} global;

struct face_list {
        size_t length;
        size_t caps;
        FT_Face *faces;
};

struct font {
    size_t refs;
    double dpi;
    double pixel_size;
    double size;
    double gamma;
    bool allow_subst_font;
    bool force_scalable;
    FcCharSet *subst_chars;
    struct face_list face_types[face_MAX];
};

struct patern_holder {
    size_t length;
    size_t caps;
    FcPattern **pats;
};

void free_font(struct font *font) {
    if (--font->refs == 0) {
        for (size_t i = 0; i < face_MAX; i++) {
            for (size_t j = 0; j < font->face_types[i].length; j++)
                FT_Done_Face(font->face_types[i].faces[j]);
            free(font->face_types[i].faces);
        }
        if (--global.fonts == 0) {
            FcFini();
            FT_Done_FreeType(global.library);
            if (font->subst_chars) FcCharSetDestroy(font->subst_chars);
        }
        free(font);
    }
}

static void load_append_fonts(struct font *font, struct face_list *faces, struct patern_holder pats) {
    size_t new_size = faces->length + pats.length;
    if (new_size > faces->caps) {
        FT_Face *new = realloc(faces->faces, (faces->length + pats.length)*sizeof(FT_Face));
        if (!new) {
            warn("Can't relocate face list");
            return;
        }
        faces->faces = new;
        faces->caps += pats.length;
    }

    for (size_t i = 0; i < pats.length; i++) {
        FcValue file, index, matrix, pixsize;
        if (FcPatternGet(pats.pats[i], FC_FILE, 0, &file) != FcResultMatch) {
            warn("Can't find file for font");
            continue;
        }
        if (FcPatternGet(pats.pats[i], FC_INDEX, 0, &index) != FcResultMatch) {
            warn("Can't get font file index, selecting 0");
            index.type = FcTypeInteger;
            index.u.i = 0;
        }

        if (gconfig.trace_fonts)
            info("Font file: %s:%d", file.u.s, index.u.i);
        FT_Error err = FT_New_Face(global.library, (const char*)file.u.s, index.u.i, &faces->faces[faces->length]);
        if (err != FT_Err_Ok) {
            if (err == FT_Err_Unknown_File_Format) warn("Wrong font file format");
            else if (err == FT_Err_Cannot_Open_Resource) warn("Can't open resource");
            else warn("Some error happend loading file: %u", err);
            continue;
        }

        if (!faces->faces[faces->length]) {
            warn("Empty font face");
            continue;
        }

        if (FcPatternGet(pats.pats[i], FC_MATRIX, 0, &matrix) == FcResultMatch) {
            FT_Matrix ftmat;
            ftmat.xx = (FT_Fixed)(matrix.u.m->xx * 0x10000L);
            ftmat.xy = (FT_Fixed)(matrix.u.m->xy * 0x10000L);
            ftmat.yx = (FT_Fixed)(matrix.u.m->yx * 0x10000L);
            ftmat.yy = (FT_Fixed)(matrix.u.m->yy * 0x10000L);
            FT_Set_Transform(faces->faces[faces->length], &ftmat, NULL);
        }

        FcResult res = FcPatternGet(pats.pats[i], FC_PIXEL_SIZE, 0, &pixsize);
        if (res != FcResultMatch || pixsize.u.d == 0) {
            warn("Font has no pixel size, selecting default");
            pixsize.type = FcTypeDouble;
            pixsize.u.d = font->pixel_size;
        }

        uint16_t sz = (pixsize.u.d/font->dpi*72.0)*64;
        if (FT_Set_Char_Size(faces->faces[faces->length], 0, sz, font->dpi, font->dpi) != FT_Err_Ok) {
            warn("Error: %s", strerror(errno));
            warn("Can't set char size");
            continue;
        }

        faces->length++;
    }
}

static void load_face_list(struct font *font, struct face_list* faces, const char *str, enum face_name attr, double size) {
    char *tmp = strdup(str);
    struct patern_holder pats = {
        .length = 0,
        .caps = CAPS_STEP,
        .pats = calloc(CAPS_STEP, sizeof(*pats.pats))
    };

    for (char *tok = strtok(tmp, ","); tok; tok = strtok(NULL, ",")) {
        FcPattern *final_pat = NULL;
        FcPattern *pat = FcNameParse((FcChar8*) tok);
        FcPatternAddDouble(pat, FC_DPI, font->dpi);
        if (font->force_scalable) FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
        FcPatternDel(pat, FC_STYLE);
        FcPatternDel(pat, FC_WEIGHT);
        FcPatternDel(pat, FC_SLANT);

        switch (attr) {
        case face_normal:
            FcPatternAddString(pat, FC_STYLE, (FcChar8*) "Regular");
            FcPatternAddInteger(pat, FC_WEIGHT, FC_WEIGHT_REGULAR);
            FcPatternAddInteger(pat, FC_SLANT, FC_SLANT_ROMAN);
            break;
        case face_normal | face_italic:
            FcPatternAddString(pat, FC_STYLE, (FcChar8*) "Italic");
            FcPatternAddInteger(pat, FC_SLANT, FC_SLANT_ITALIC);
            FcPatternAddInteger(pat, FC_WEIGHT, FC_WEIGHT_REGULAR);
            break;
        case face_bold:
            FcPatternAddString(pat, FC_STYLE, (FcChar8*) "Bold");
            FcPatternAddInteger(pat, FC_SLANT, FC_SLANT_ROMAN);
            FcPatternAddInteger(pat, FC_WEIGHT, FC_WEIGHT_BOLD);
            break;
        case face_bold | face_italic:
            FcPatternAddString(pat, FC_STYLE, (FcChar8*) "Bold Italic");
            FcPatternAddString(pat, FC_STYLE, (FcChar8*) "BoldItalic");
            FcPatternAddInteger(pat, FC_SLANT, FC_SLANT_ITALIC);
            FcPatternAddInteger(pat, FC_WEIGHT, FC_WEIGHT_BOLD);
            break;
        default:
            warn("Unknown face type");
        }

        if (size > 1) {
            FcPatternDel(pat, FC_SIZE);
            FcPatternDel(pat, FC_PIXEL_SIZE);
            FcPatternAddDouble(pat, FC_SIZE, size);
        }

        FcDefaultSubstitute(pat);
        if (FcConfigSubstitute(NULL, pat, FcMatchPattern) == FcFalse) {
            warn("Can't substitute font config for font: %s", tok);
            FcPatternDestroy(pat);
            continue;
        }

        FcResult result;
        final_pat = FcFontMatch(NULL, pat, &result);
        FcPatternDestroy(pat);
        if (result != FcResultMatch) {
            warn("No match for font: %s", tok);
            continue;
        }


        if (pats.length + 1 > pats.caps) {
            FcPattern **new = realloc(pats.pats, sizeof(*pats.pats)*(pats.caps + CAPS_STEP));
            if (!new) {
                warn("Out of memory");
                continue;
            }
            pats.caps += CAPS_STEP;
            pats.pats = new;
        }

        FcValue pixsize;
        if (FcPatternGet(final_pat, FC_PIXEL_SIZE, 0, &pixsize) == FcResultMatch) {
            if (pixsize.u.d > font->pixel_size)
                font->pixel_size = pixsize.u.d;
        }
        FcValue fsize;
        if (size < 2 && FcPatternGet(final_pat, FC_SIZE, 0, &fsize) == FcResultMatch) {
            if (fsize.u.d > font->size) {
                font->size = fsize.u.d;
            }
        }

        pats.pats[pats.length++] = final_pat;
    }

    load_append_fonts(font, faces, pats);

    for (size_t i = 0; i < pats.length; i++)
        FcPatternDestroy(pats.pats[i]);
    free(pats.pats);
    free(tmp);
}

struct font *create_font(const char* descr, double size, double dpi, double gamma, bool force_scalable, bool allow_subst) {
    if (global.fonts++ == 0) {
        if (FcInit() == FcFalse)
            die("Can't initialize fontconfig");
        if (FT_Init_FreeType(&global.library) != FT_Err_Ok) {
            die("Can't initialize freetype2, error: %s", strerror(errno));
        }
        FT_Library_SetLcdFilter(global.library, FT_LCD_FILTER_DEFAULT);
    }

    struct font *font = calloc(1, sizeof(*font));
    if (!font) {
        warn("Can't allocate font");
        return NULL;
    }

    font->refs = 1;
    font->pixel_size = 0;
    font->dpi = dpi;
    font->size = size;
    font->gamma = gamma;
    font->force_scalable = force_scalable;
    font->allow_subst_font = allow_subst;


    for (size_t i = 0; i < face_MAX; i++)
        load_face_list(font, &font->face_types[i], descr, i, size);

    // If can't find suitable, use 13 by default
    if (font->pixel_size == 0)
        font->pixel_size = 13.0;

    return font;
}

struct font *font_ref(struct font *font) {
    font->refs++;
    return font;
}

static void add_font_substitute(struct font *font, struct face_list *faces, enum face_name attr, uint32_t ch){
    if (!font->subst_chars) font->subst_chars = FcCharSetCreate();
    FcCharSetAddChar(font->subst_chars, ch);

    FcPattern *chset_pat = FcPatternCreate();
    FcPatternAddDouble(chset_pat, FC_DPI, font->dpi);
    FcPatternAddCharSet(chset_pat, FC_CHARSET, font->subst_chars);
    if (font->force_scalable || FT_IS_SCALABLE(faces->faces[0]))
        FcPatternAddBool(chset_pat, FC_SCALABLE, FcTrue);

    switch (attr) {
    case face_normal:
        FcPatternAddString(chset_pat, FC_STYLE, (FcChar8*) "Regular");
        FcPatternAddInteger(chset_pat, FC_WEIGHT, FC_WEIGHT_REGULAR);
        FcPatternAddInteger(chset_pat, FC_SLANT, FC_SLANT_ROMAN);
        break;
    case face_normal | face_italic:
        FcPatternAddString(chset_pat, FC_STYLE, (FcChar8*) "Italic");
        FcPatternAddInteger(chset_pat, FC_SLANT, FC_SLANT_ITALIC);
        FcPatternAddInteger(chset_pat, FC_WEIGHT, FC_WEIGHT_REGULAR);
        break;
    case face_bold:
        FcPatternAddString(chset_pat, FC_STYLE, (FcChar8*) "Bold");
        FcPatternAddInteger(chset_pat, FC_SLANT, FC_SLANT_ROMAN);
        FcPatternAddInteger(chset_pat, FC_WEIGHT, FC_WEIGHT_BOLD);
        break;
    case face_bold | face_italic:
        FcPatternAddString(chset_pat, FC_STYLE, (FcChar8*) "Bold Italic");
        FcPatternAddString(chset_pat, FC_STYLE, (FcChar8*) "BoldItalic");
        FcPatternAddInteger(chset_pat, FC_SLANT, FC_SLANT_ITALIC);
        FcPatternAddInteger(chset_pat, FC_WEIGHT, FC_WEIGHT_BOLD);
        break;
    default:
        warn("Unknown face type");
    }

    FcDefaultSubstitute(chset_pat);
    if (FcConfigSubstitute(NULL, chset_pat, FcMatchPattern) == FcFalse) {
        warn("Can't find substitute font");
        return;
    }

    FcResult result;
    FcPattern *final_pat = FcFontMatch(NULL, chset_pat, &result);
    if (result != FcResultMatch) {
        warn("Font doesn't match");
        return;
    }

    load_append_fonts(font, faces, (struct patern_holder){ .length = 1, .pats = &final_pat });
    FcPatternDestroy(final_pat);
}

struct glyph *font_render_glyph(struct font *font, enum pixel_mode ord, uint32_t ch, enum face_name attr) {
    struct face_list *faces = &font->face_types[attr];
    int glyph_index = 0;
    FT_Face face = faces->faces[0];
    for (size_t i = 0; !glyph_index && i < faces->length; i++)
        if ((glyph_index = FT_Get_Char_Index(faces->faces[i], ch)))
            face = faces->faces[i];
    if (!glyph_index && font->allow_subst_font) {
        size_t oldlen = faces->length, sz = faces->faces[0]->size->metrics.x_ppem*72.0/font->dpi*64;
        add_font_substitute(font, faces, attr, ch);
        for (size_t i = oldlen; !glyph_index && i < faces->length; i++) {
            FT_Set_Char_Size(faces->faces[i], 0, sz, font->dpi, font->dpi);
            if ((glyph_index = FT_Get_Char_Index(faces->faces[i],ch))) face = faces->faces[i];
        }
    }

    FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT);

    bool ordv = ord == pixmode_bgrv || ord == pixmode_rgbv;
    bool ordrev = ord == pixmode_bgr || ord == pixmode_bgrv;
    bool lcd = ord != pixmode_mono;

    FT_Render_Glyph(face->glyph, lcd ? (ordv ? FT_RENDER_MODE_LCD_V : FT_RENDER_MODE_LCD_V) : FT_RENDER_MODE_NORMAL);

    size_t stride = face->glyph->bitmap.width;

    if (face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_LCD) stride /= 3;
    if (lcd) stride *= 4;
    stride = (stride + 3) & ~3;

    struct glyph *glyph = malloc(sizeof(*glyph) + stride * face->glyph->bitmap.rows);
    glyph->x = -face->glyph->bitmap_left;
    glyph->y = face->glyph->bitmap_top;
    glyph->width = face->glyph->bitmap.width;
    glyph->height = face->glyph->bitmap.rows;
    glyph->x_off = face->glyph->advance.x/64.;
    glyph->y_off = face->glyph->advance.y/64.;
    glyph->stride = stride;
    glyph->pixmode = ord;

    double gamma = font->gamma;

    int pitch = face->glyph->bitmap.pitch;
    uint8_t *src = face->glyph->bitmap.buffer;
    if (pitch < 0) src -= pitch*(face->glyph->bitmap.rows - 1);
    uint16_t num_grays = face->glyph->bitmap.num_grays;

    switch (face->glyph->bitmap.pixel_mode) {
    case FT_PIXEL_MODE_MONO:
    case FT_PIXEL_MODE_GRAY2:
    case FT_PIXEL_MODE_GRAY4:;
        FT_Bitmap sbm;
        FT_Bitmap_Init(&sbm);
        FT_Bitmap_Convert(global.library, &face->glyph->bitmap, &sbm, 4);
        pitch = sbm.pitch;
        src = sbm.buffer;
        if (pitch < 0) src -= pitch*(sbm.rows - 1);
        num_grays = sbm.num_grays;
        /* fallthrough */
    case FT_PIXEL_MODE_GRAY:
        if (lcd) {
            for (size_t i = 0; i < glyph->height; i++) {
                for (size_t j = 0; j < glyph->width; j++) {
                    uint8_t v = MIN(0xFF, 0xFF * pow(src[pitch*i + j] / (double)(num_grays - 1), gamma));
                    glyph->data[stride*i + 4*j + 0] = v;
                    glyph->data[stride*i + 4*j + 1] = v;
                    glyph->data[stride*i + 4*j + 2] = v;
                    glyph->data[stride*i + 4*j + 3] = v;
                }
            }
        } else {
            for (size_t i = 0; i < glyph->height; i++)
                for (size_t j = 0; j < glyph->width; j++)
                    glyph->data[stride*i + j] = 0xFF * pow(src[pitch*i + j] / (double)(num_grays - 1), gamma);
        }
        if (face->glyph->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY)
            FT_Bitmap_Done(global.library, &sbm);
        break;
    case FT_PIXEL_MODE_LCD_V:
        glyph->height /= 3;
        for (size_t i = 0; i < glyph->height; i++) {
            for (size_t j = 0; j < glyph->width; j++) {
                int16_t acc = 0;
                acc += glyph->data[4*j + stride*i + 0] = 0xFF * pow(src[pitch*(3*i+2*ordrev) + j] / 255., gamma);
                acc += glyph->data[4*j + stride*i + 1] = 0xFF * pow(src[pitch*(3*i+1) + j] / 255., gamma);
                acc += glyph->data[4*j + stride*i + 2] = 0xFF * pow(src[pitch*(3*i+2*(1-ordrev)) + j] / 255., gamma);
                glyph->data[4*j + stride*i + 3] = acc/3;
            }
        }
        break;
    case FT_PIXEL_MODE_LCD:
        glyph->width /= 3;
        for (size_t i = 0; i < glyph->height; i++) {
            for (size_t j = 0; j < glyph->width; j++) {
                int16_t acc = 0;
                acc += glyph->data[4*j + stride*i + 0] = 0xFF * pow(src[pitch*i + 3*j + 2*ordrev] / 255., gamma);
                acc += glyph->data[4*j + stride*i + 1] = 0xFF * pow(src[pitch*i + 3*j + 1] / 255., gamma);
                acc += glyph->data[4*j + stride*i + 2] = 0xFF * pow(src[pitch*i + 3*j + 2*(1-ordrev)] / 255., gamma);
                glyph->data[4*j + stride*i + 3] = acc/3;
            }
        }
        break;
    case FT_PIXEL_MODE_BGRA:
        warn("Colored glyph encountered");
        free(glyph);
        return font_render_glyph(font, ord, 0, attr);
    }

    if (gconfig.log_level == 3 && gconfig.trace_fonts) {
        info("Bitmap mode: %d", face->glyph->bitmap.pixel_mode);
        info("Num grays: %d", face->glyph->bitmap.num_grays);
        info("Glyph: %d %d", glyph->width, glyph->height);

        for (size_t k = 0; k < glyph->height; k++) {
            for (size_t m = 0; m < glyph->width; m++)
                fprintf(stderr, "%02x", glyph->data[stride*k+m]);
            putc('\n', stderr);
        }
    }

    return glyph;
}

int16_t font_get_size(struct font *font) {
    return font->size;
}

struct glyph_cache {
    struct font *font;
    int16_t char_width;
    int16_t char_height;
    int16_t char_depth;
    int16_t vspacing;
    int16_t hspacing;
    bool override_boxdraw;
    enum pixel_mode pixmode;
    size_t refc;
    struct glyph **tab;
    size_t size;
    size_t caps;
};

static inline uint64_t ror64(uint64_t v, int r) {
    return (v >> r) | (v << (64 - r));
}

uint64_t hash(uint64_t v) {
    v ^= ror64(v, 25) ^ ror64(v, 50);
    v *= 0xA24BAED4963EE407ULL;
    v ^= ror64(v, 24) ^ ror64(v, 49);
    v *= 0x9FB21C651E98DF25ULL;
    return v ^ v >> 28;
}

struct glyph_cache *create_glyph_cache(struct font *font, enum pixel_mode pixmode, int16_t vspacing, int16_t hspacing, bool boxdraw) {
    struct glyph_cache *cache = calloc(1, sizeof(struct glyph_cache));
    if (!cache) {
        warn("Can't allocate glyph cache");
        return NULL;
    }
    cache->tab = calloc(HASH_INIT_CAP, sizeof(*cache->tab));
    cache->caps = HASH_INIT_CAP;
    cache->refc = 1;
    cache->font = font;
    cache->vspacing = vspacing;
    cache->hspacing = hspacing;
    cache->override_boxdraw = boxdraw;
    cache->pixmode = pixmode;

    if (!cache->tab) {
        free(cache);
        warn("Can't allocate glyph cache");
        return NULL;
    }

    int16_t total = 0, maxd = 0, maxh = 0;
    for (uint32_t i = ' '; i <= '~'; i++) {
        struct glyph *g = glyph_cache_fetch(cache, i, face_normal);

        total += g->x_off;
        maxd = MAX(maxd, g->height - g->y);
        maxh = MAX(maxh, g->y);
    }

    cache->char_width = total / ('~' - ' ' + 1) + hspacing;
    cache->char_height = maxh;
    cache->char_depth = maxd + vspacing;

    if (gconfig.trace_fonts) {
        info("Font dim: width=%"PRId16", height=%"PRId16", depth=%"PRId16,
                cache->char_width, cache->char_height, cache->char_depth);
    }

    return cache;
}

struct glyph_cache *glyph_cache_ref(struct glyph_cache *ref) {
    ref->refc++;
    return ref;
}

void glyph_cache_get_dim(struct glyph_cache *cache, int16_t *w, int16_t *h, int16_t *d) {
    if (w) *w = cache->char_width;
    if (h) *h = cache->char_height;
    if (d) *d = cache->char_depth;
}

void free_glyph_cache(struct glyph_cache *cache) {
    if (!--cache->refc) {
        for (size_t i = 0; i < cache->caps; i++)
            for (struct glyph *next = NULL, *it = cache->tab[i]; it; it = next)
                next = it->next, free(it);
        free(cache->tab);
        free(cache);
    }
}

struct glyph *glyph_cache_fetch(struct glyph_cache *cache, uint32_t ch, enum face_name face) {
    uint32_t g = ch | (face << 24);
    size_t h = hash(g);

    struct glyph *res = cache->tab[h % cache->caps];
    for (; res; res = res->next)
        if (g == res->g) return res;

    struct glyph *new;
#if USE_BOXDRAWING
    if (is_boxdraw(ch) && cache->override_boxdraw)
        new = make_boxdraw(ch, cache->char_width, cache->char_height, cache->char_depth, cache->pixmode, cache->hspacing, cache->vspacing);
    else
#endif
        new = font_render_glyph(cache->font, cache->pixmode, ch, face);

    if (!new) return NULL;

    if (3*(cache->size + 1)/2 > cache->caps) {
        size_t newc = HASH_CAP_INC(cache->caps);
        struct glyph **newt = calloc(newc, sizeof(*newt));
        if (!newt) return NULL;
        for (size_t i = 0; i < cache->caps; i++) {
            for (struct glyph *next = NULL, *it = cache->tab[i]; it; it = next) {
                next = it->next;
                size_t ha = hash(it->g);
                it->next = newt[ha % newc];
                newt[ha % newc] = it;
            }
        }
        free(cache->tab);
        cache->tab = newt;
        cache->caps = newc;
    }

    new->g = g;
    new->next = cache->tab[h % cache->caps];
    cache->tab[h % cache->caps] = new;
    cache->size++;

    return new;
}

bool glyph_cache_is_fetched(struct glyph_cache *cache, uint32_t ch) {
    size_t h = hash(ch);
    struct glyph *res = cache->tab[h % cache->caps];
    for (; res; res = res->next)
        if (ch == res->g) return 1;
    return 0;
}
