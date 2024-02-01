/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

#include "feature.h"


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

struct font_context {
    size_t fonts;
    FT_Library library;
};

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

static struct font_context global;

void free_font(struct font *font) {
    if (--font->refs == 0) {
        for (size_t i = 0; i < face_MAX; i++) {
            for (size_t j = 0; j < font->face_types[i].length; j++)
                FT_Done_Face(font->face_types[i].faces[j]);
            free(font->face_types[i].faces);
        }
        if (--global.fonts == 0) {
            if (font->subst_chars)
                FcCharSetDestroy(font->subst_chars);
            FcFini();
            FT_Done_FreeType(global.library);
        }
        free(font);
    }
}

static void load_append_fonts(struct font *font, struct face_list *faces, struct patern_holder pats) {
    size_t new_size = faces->length + pats.length;
    adjust_buffer((void **)&faces->faces, &faces->caps, new_size, sizeof(FT_Face));

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
        .pats = xzalloc(CAPS_STEP * sizeof *pats.pats)
    };

    for (char *tok = strtok(tmp, ","); tok; tok = strtok(NULL, ",")) {
        FcPattern *final_pat = NULL;
        FcPattern *pat = FcNameParse((FcChar8*) tok);
        FcPatternAddDouble(pat, FC_DPI, font->dpi);
        if (font->force_scalable)
            FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
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


        if (pats.length + 1 > pats.caps)
            adjust_buffer((void **)&pats.pats, &pats.caps, pats.length + 1, sizeof(*pats.pats));

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

    struct font *font = xzalloc(sizeof *font);

    font->refs = 1;
    font->pixel_size = 0;
    font->dpi = dpi;
    font->size = size;
    font->gamma = gamma;
    font->force_scalable = force_scalable;
    font->allow_subst_font = allow_subst;


    for (size_t i = 0; i < face_MAX; i++)
        load_face_list(font, &font->face_types[i], descr, i, size);

    /* If can't find suitable, use 13 by default */
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
    FcPatternDestroy(chset_pat);

    if (result != FcResultMatch) {
        warn("Font doesn't match");
        return;
    }

    load_append_fonts(font, faces, (struct patern_holder){ .length = 1, .pats = &final_pat });
    FcPatternDestroy(final_pat);
}

#define GLYPH_STRIDE_ALIGNMENT 4

FORCEINLINE
static inline color_t image_sample(struct glyph *glyph, double x0, double x1, double y0, double y1, double gamma) {
    assert(x1 - x0 >= 1.);
    assert(y1 - y0 >= 1.);

    double x0_w = 1 - (x0 - floor(x0));
    double x1_w = x1 - floor(x1);
    double y0_w = 1 - (y0 - floor(y0));
    double y1_w = y1 - floor(y1);
    double rgamma = 1/gamma;

    ssize_t x0_i = MIN((ssize_t)x0, glyph->width - 1);
    ssize_t x1_i = MIN((ssize_t)x1, glyph->width - 1);
    ssize_t y0_i = MIN((ssize_t)y0, glyph->height - 1);
    ssize_t y1_i = MIN((ssize_t)y1, glyph->height - 1);

    double acc[4] = {0};

    for (ssize_t y = y0_i; y <= y1_i; y++) {
        uint8_t *data = glyph->data + y*glyph->stride;
        for (ssize_t x = x0_i; x <= x1_i; x++) {
            double w = 1;
            if (x == x0_i) w *= x0_w;
            if (x == x1_i) w *= x1_w;
            if (y == y0_i) w *= y0_w;
            if (y == y1_i) w *= y1_w;

            double ralpha = data[4*x + 3] ? 1./data[4*x + 3] : 0.;
            acc[0] += pow(data[4*x + 0]*ralpha, rgamma) * w;
            acc[1] += pow(data[4*x + 1]*ralpha, rgamma) * w;
            acc[2] += pow(data[4*x + 2]*ralpha, rgamma) * w;
            acc[3] += data[4*x + 3] * w;
        }
    }

    double scale = 1./((x1-x0)*(y1-y0));
    double alpha = acc[3]*scale;
    return mk_color(
        (MIN(pow(acc[2]*scale, gamma)*alpha, 255)),
        (MIN(pow(acc[1]*scale, gamma)*alpha, 255)),
        (MIN(pow(acc[0]*scale, gamma)*alpha, 255)),
        (MIN(alpha, 255)));
}

HOT
static struct glyph *downsample_glyph(struct glyph *glyph, uint32_t targ_width, bool force_aligned, double gamma) {
    double scale = targ_width/(double)glyph->width;

    ssize_t stride = targ_width*sizeof(color_t);
    if (!force_aligned) stride = ROUNDUP(stride, GLYPH_STRIDE_ALIGNMENT);
    else stride = ROUNDUP(stride, GLYPH_STRIDE_ALIGNMENT*sizeof(color_t));

    ssize_t targ_height = nearbyint(glyph->height * scale);
    struct glyph *nglyph = aligned_alloc(CACHE_LINE, ROUNDUP(sizeof(*glyph) + stride * targ_height, CACHE_LINE));
    nglyph->x = nearbyint(glyph->x * scale);
    nglyph->y = nearbyint(glyph->y * scale);
    nglyph->width = targ_width;
    nglyph->height = targ_height;
    nglyph->x_off = nearbyint(glyph->x_off * scale);
    nglyph->y_off = nearbyint(glyph->y_off * scale);
    nglyph->stride = stride;
    nglyph->pixmode = glyph->pixmode;
    nglyph->g = glyph->g;

    for (ssize_t i = 0; i < targ_height; i++) {
        for (ssize_t j = 0; j < targ_width; j++) {
            double xp = glyph->width*j/(double)targ_width;
            double xe = glyph->width*(j + 1)/(double)targ_width;
            double yp = glyph->height*i/(double)targ_height;
            double ye = glyph->height*(i + 1)/(double)targ_height;
            color_t c = image_sample(glyph, xp, xe, yp, ye, gamma);
            memcpy(&nglyph->data[stride*i + 4*j], &c, sizeof(c));
        }
        memset(nglyph->data + stride*i + nglyph->width * 4, 0, stride - nglyph->width * 4);
    }

    free(glyph);
    return nglyph;
}

static struct glyph *font_render_glyph(struct font *font, enum pixel_mode ord, uint32_t ch, enum face_name attr, bool force_aligned, uint32_t targ_width) {
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

    FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT | FT_LOAD_COLOR);

    bool ordv = ord == pixmode_bgrv || ord == pixmode_rgbv;
    bool ordrev = ord == pixmode_bgr || ord == pixmode_bgrv;
    bool lcd = ord != pixmode_mono;

    FT_Render_Glyph(face->glyph, lcd ? (ordv ? FT_RENDER_MODE_LCD_V : FT_RENDER_MODE_LCD) : FT_RENDER_MODE_NORMAL);

    size_t stride = face->glyph->bitmap.width;

    if (face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_LCD) stride /= 3;
    else if (face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
        lcd = true;
        ord = pixmode_bgra;
    }

    if (force_aligned) {
         /* Soft renderer backend requires subpixel glyphs
         * to be aligned on 16 bytes */
        stride = ROUNDUP(stride, GLYPH_STRIDE_ALIGNMENT);
        if (lcd) stride *= 4;
    } else {
        /* XRender required stride to be aligned exactly to 4,
         * software renderer wants stride to be aligned to 4/16
         * depending on whether subpixel rendering is off or on.
         */
        if (lcd) stride *= 4;
        stride = ROUNDUP(stride, GLYPH_STRIDE_ALIGNMENT);
    }

    struct glyph *glyph = aligned_alloc(CACHE_LINE, ROUNDUP(sizeof(*glyph) + stride * face->glyph->bitmap.rows, CACHE_LINE));
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
                memset(glyph->data + stride*i + glyph->width * 4, 0, stride - glyph->width * 4);
            }
        } else {
            for (size_t i = 0; i < glyph->height; i++) {
                for (size_t j = 0; j < glyph->width; j++)
                    glyph->data[stride*i + j] = 0xFF * pow(src[pitch*i + j] / (double)(num_grays - 1), gamma);
                memset(glyph->data + stride*i + glyph->width, 0, stride - glyph->width);
            }
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
            memset(glyph->data + stride*i + glyph->width * 4, 0, stride - glyph->width * 4);
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
            memset(glyph->data + stride*i + glyph->width * 4, 0, stride - glyph->width * 4);
        }
        break;
    case FT_PIXEL_MODE_BGRA:
        if (stride != glyph->width * 4) {
            for (size_t i = 0; i < glyph->height; i++) {
                memcpy(glyph->data + stride*i, src + pitch*i, glyph->width * 4);
                memset(glyph->data + stride*i + glyph->width * 4, 0, stride - glyph->width * 4);
            }
        } else if (stride != (size_t)pitch) {
            for (size_t i = 0; i < glyph->height; i++)
                memcpy(glyph->data + stride*i, src + pitch*i, glyph->width * 4);
        } else {
            memcpy(glyph->data, src, glyph->height * glyph->width * 4);
        }
        if (glyph->width > targ_width)
            glyph = downsample_glyph(glyph, targ_width, force_aligned, gamma);
    }

    if (gconfig.log_level == 3 && gconfig.trace_fonts) {
        info("[Glyph] char=%"PRId32"(%lc) pixel_mode=%d num_grays=%d width=%d height=%d data:",
            ch, (wint_t)ch, face->glyph->bitmap.pixel_mode,
            face->glyph->bitmap.num_grays, glyph->width, glyph->height);

        for (size_t k = 0; k < glyph->height; k++) {
            fputs("\t", stderr);
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
    bool force_aligned;
    enum pixel_mode pixmode;
    size_t refc;

    hashtable_t glyphs;
};

#define mkglyphkey(gl) ((struct glyph){ .head = { .hash = uint_hash32(gl)}, .g = (gl) })

static bool glyph_cmp(const ht_head_t *a, const ht_head_t *b) {
    const struct glyph *ga = (const struct glyph *)a;
    const struct glyph *gb = (const struct glyph *)b;
    return ga->g == gb->g;
}

struct glyph_cache *create_glyph_cache(struct font *font, enum pixel_mode pixmode, int16_t vspacing, int16_t hspacing, bool boxdraw, bool force_aligned) {
    struct glyph_cache *cache = xzalloc(sizeof(struct glyph_cache));

    cache->refc = 1;
    cache->font = font;
    cache->vspacing = vspacing;
    cache->hspacing = hspacing;
    cache->override_boxdraw = boxdraw;
    cache->force_aligned = force_aligned;
    cache->pixmode = pixmode;
    ht_init(&cache->glyphs, HASH_INIT_CAP, glyph_cmp);

    int16_t total = 0, maxd = 0, maxh = 0;
    for (uint32_t i = ' '; i <= '~'; i++) {
        struct glyph *g = glyph_cache_fetch(cache, i, face_normal, NULL);

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
        ht_iter_t it = ht_begin(&cache->glyphs);
        while (ht_current(&it))
            free(ht_erase_current(&it));
        ht_free(&cache->glyphs);
        free(cache);
    }
}

struct glyph *glyph_cache_fetch(struct glyph_cache *cache, uint32_t ch, enum face_name face, bool *is_new) {
    struct glyph dummy = mkglyphkey(ch | (face << 24));
    ht_head_t **h = ht_lookup_ptr(&cache->glyphs, (ht_head_t *)&dummy);
    if (*h) {
        if (is_new) *is_new = 0;
        return (struct glyph *)*h;
    }

    if (is_new) *is_new = 1;

    struct glyph *new;
#if USE_BOXDRAWING
    if (is_boxdraw(ch) && cache->override_boxdraw)
        new = make_boxdraw(ch, cache->char_width, cache->char_height, cache->char_depth,
                           cache->pixmode, cache->hspacing, cache->vspacing, cache->force_aligned);
    else
#endif
        new = font_render_glyph(cache->font, cache->pixmode, ch, face, cache->force_aligned, cache->char_width*2);
    if (!new) return NULL;

    new->g = dummy.g;
    new->head = dummy.head;

    ht_insert_hint(&cache->glyphs, h, (ht_head_t *)new);
    return new;
}
