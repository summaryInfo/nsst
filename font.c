/* Copyright (c) 2019-2022,2025, Evgeniy Baskov. All rights reserved */

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

struct face {
    ht_head_t head;
    FT_Face face;
    size_t len;
    int index;
    uint16_t size;
    bool has_transform;
    FT_Matrix transform;
    char file[];
};

struct face_list {
    size_t length;
    size_t caps;
    hashtable_t face_ht;
    struct face **faces;
};

struct font {
    size_t refs;
    double dpi;
    double pixel_size;
    double size;
    double gamma;
    bool allow_subst_font;
    bool force_scalable;
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
            for (size_t j = 0; j < font->face_types[i].length; j++) {
                struct face *face = font->face_types[i].faces[j];
                ht_erase(&font->face_types[i].face_ht, &face->head);
                FT_Done_Face(face->face);
                free(face);
            }
            free(font->face_types[i].faces);
            ht_free(&font->face_types[i].face_ht);
        }
        if (--global.fonts == 0) {
            FcFini();
            FT_Done_FreeType(global.library);
        }
        free(font);
    }
}

static bool load_append_fonts(struct font *font, struct face_list *faces, struct patern_holder pats, double override_pixsize) {
    size_t new_size = faces->length + pats.length;
    adjust_buffer((void **)&faces->faces, &faces->caps, new_size, sizeof(faces->faces[0]));
    bool has_svg = false;

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

        if (!override_pixsize) {
            FcResult res = FcPatternGet(pats.pats[i], FC_PIXEL_SIZE, 0, &pixsize);
            if (res != FcResultMatch || pixsize.u.d == 0) {
                warn("Font has no pixel size, selecting default");
                pixsize.type = FcTypeDouble;
                pixsize.u.d = font->pixel_size;
            }
        } else {
            pixsize.type = FcTypeDouble;
            pixsize.u.d = override_pixsize;
        }

        size_t len = (sizeof(struct face)) + strlen((const char *)file.u.s) + 1;
        struct face *newface = xalloc(len);
        *newface = (struct face) {
            .len = len - offsetof(struct face, len),
            .index = index.u.i,
            .size = (pixsize.u.d/font->dpi*72.0)*64,
        };

        strcpy(newface->file, (const char *)file.u.s);

        if (FcPatternGet(pats.pats[i], FC_MATRIX, 0, &matrix) == FcResultMatch) {
            newface->has_transform = true;
            newface->transform.xx = (FT_Fixed)(matrix.u.m->xx * 0x10000L);
            newface->transform.xy = (FT_Fixed)(matrix.u.m->xy * 0x10000L);
            newface->transform.yx = (FT_Fixed)(matrix.u.m->yx * 0x10000L);
            newface->transform.yy = (FT_Fixed)(matrix.u.m->yy * 0x10000L);
        }

        newface->head.hash = hash64((char *)&newface->len, newface->len);
        ht_head_t **pfound = ht_lookup_ptr(&faces->face_ht, &newface->head);
        if (gconfig.trace_fonts)
            info("Font file: %s:%d found=%d total=%zd", file.u.s, index.u.i, !!*pfound, faces->length);

        if (*pfound)
            goto err_free_face;

        FT_Error err = FT_New_Face(global.library, newface->file, newface->index, &newface->face);
        if (err != FT_Err_Ok) {
            if (err == FT_Err_Unknown_File_Format) warn("Wrong font file format");
            else if (err == FT_Err_Cannot_Open_Resource) warn("Can't open resource");
            else warn("Some error happend loading file: %u", err);
            goto err_free_face;
        }

        if (!newface->face) {
            warn("Empty font face");
            goto err_free_face;
        }

        if (newface->has_transform)
            FT_Set_Transform(newface->face, &newface->transform, NULL);

        if (FT_Set_Char_Size(newface->face, 0, newface->size, font->dpi, font->dpi) != FT_Err_Ok) {
            warn("Error: %s", strerror(errno));
            warn("Can't set char size");
            FT_Done_Face(newface->face);
err_free_face:
            free(newface);
            continue;
        }

        has_svg |= FT_HAS_SVG(newface->face);
        ht_insert_hint(&faces->face_ht, pfound, &newface->head);
        faces->faces[faces->length++] = newface;
    }

    return has_svg;
}

static bool load_face_list(struct font *font, struct face_list* faces, const char *str, enum face_name attr, double size) {
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

        FcValue fc_color;
        if (FcPatternGet(final_pat, FC_COLOR, 0, &fc_color) != FcResultMatch)
            fc_color.u.b = false;

        FcValue pixsize;
        if (!fc_color.u.b && FcPatternGet(final_pat, FC_PIXEL_SIZE, 0, &pixsize) == FcResultMatch) {
            if (pixsize.u.d > font->pixel_size)
                font->pixel_size = pixsize.u.d;
        }
        FcValue fsize;
        if (!fc_color.u.b && size < 2 && FcPatternGet(final_pat, FC_SIZE, 0, &fsize) == FcResultMatch) {
            if (fsize.u.d > font->size) {
                font->size = fsize.u.d;
            }
        }

        pats.pats[pats.length++] = final_pat;
    }

    bool has_svg = load_append_fonts(font, faces, pats, 0);

    for (size_t i = 0; i < pats.length; i++)
        FcPatternDestroy(pats.pats[i]);
    free(pats.pats);
    free(tmp);

    return has_svg;
}

static bool face_cmp(const ht_head_t *a, const ht_head_t *b) {
    const struct face *af = CONTAINEROF(a, const struct face, head);
    const struct face *bf = CONTAINEROF(b, const struct face, head);
    if (af->len != bf->len) return false;
    if (af->index != bf->index) return false;
    if (af->size != bf->size) return false;
    if (af->has_transform != bf->has_transform) return false;
    if (af->transform.xx != bf->transform.xx) return false;
    if (af->transform.xy != bf->transform.xy) return false;
    if (af->transform.yx != bf->transform.yx) return false;
    if (af->transform.yy != bf->transform.yy) return false;
    return !strcmp(af->file, bf->file);
}

struct font *create_font(const char* descr, double size, double dpi, double gamma, bool force_scalable, bool allow_subst) {
    if (global.fonts++ == 0) {
        if (FcInit() == FcFalse)
            die("Can't initialize fontconfig");
        if (FT_Init_FreeType(&global.library) != FT_Err_Ok)
            die("Can't initialize freetype2, error: %s", strerror(errno));
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

    bool has_svg = false;
    for (size_t i = 0; i < face_MAX; i++) {
        ht_init(&font->face_types[i].face_ht, HT_INIT_CAPS, face_cmp);
        has_svg |= load_face_list(font, &font->face_types[i], descr, i, size);
    }

    if (has_svg)
        warn("Font '%s' contains SVG glyphs and might rendered incorrectly", descr);

    /* If can't find suitable, use 13 by default */
    if (font->pixel_size == 0)
        font->pixel_size = 13.0;

    return font;
}

struct font *font_ref(struct font *font) {
    font->refs++;
    return font;
}

static void add_font_substitute(struct font *font, struct face_list *faces, enum face_name attr, uint32_t ch) {
    FcCharSet *subst_chars = FcCharSetCreate();
    FcCharSetAddChar(subst_chars, ch);

    FcPattern *chset_pat = FcPatternCreate();
    FcPatternAddDouble(chset_pat, FC_DPI, font->dpi);
    int pixsize = faces->faces[0]->face->size->metrics.x_ppem;
    FcPatternAddInteger(chset_pat, FC_PIXEL_SIZE, pixsize);
    FcPatternAddCharSet(chset_pat, FC_CHARSET, subst_chars);
    FcCharSetDestroy(subst_chars);

    if (font->force_scalable || FT_IS_SCALABLE(faces->faces[0]->face))
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
        FcPatternDestroy(chset_pat);
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

    bool has_svg = load_append_fonts(font, faces, (struct patern_holder){ .length = 1, .pats = &final_pat }, pixsize);
    if (has_svg)
        warn("Substitute font for character U+%04x (%lc) contains SVG glyphs and might rendered incorrectly", ch, ch);

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
    double x_scale = targ_width/(double)glyph->width;
    double y_scale = x_scale;

    ssize_t stride = targ_width*sizeof(color_t);
    if (!force_aligned) stride = ROUNDUP(stride, GLYPH_STRIDE_ALIGNMENT);
    else stride = ROUNDUP(stride, GLYPH_STRIDE_ALIGNMENT*sizeof(color_t));

    ssize_t targ_height = nearbyint(glyph->height * y_scale);
    struct glyph *nglyph = aligned_alloc(_Alignof(struct glyph), ROUNDUP(sizeof(*glyph) + stride * targ_height, _Alignof(struct glyph)));
    nglyph->x = nearbyint(glyph->x * x_scale);
    nglyph->y = nearbyint(glyph->y * y_scale);
    nglyph->width = targ_width;
    nglyph->height = targ_height;
    nglyph->y_off = nearbyint(glyph->y_off * y_scale);
    nglyph->x_off = nearbyint(glyph->x_off * x_scale);
    nglyph->stride = stride;
    nglyph->pixmode = glyph->pixmode;
    nglyph->g = glyph->g;
    nglyph->id = 0;

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
    FT_Face face = faces->faces[0]->face;
    for (size_t i = 0; !glyph_index && i < faces->length; i++)
        if ((glyph_index = FT_Get_Char_Index(faces->faces[i]->face, ch)))
            face = faces->faces[i]->face;
    if (!glyph_index && font->allow_subst_font) {
        size_t oldlen = faces->length;
        add_font_substitute(font, faces, attr, ch);
        for (size_t i = oldlen; !glyph_index && i < faces->length; i++)
            if ((glyph_index = FT_Get_Char_Index(faces->faces[i]->face, ch)))
                face = faces->faces[i]->face;
    }

    bool ordv = ord == pixmode_bgrv || ord == pixmode_rgbv;
    bool ordrev = ord == pixmode_rgb || ord == pixmode_rgbv;
    bool lcd = ord != pixmode_mono;

    FT_Int32 load_flags = FT_LOAD_DEFAULT | FT_LOAD_NO_SVG;
    FT_Int32 render_mode = FT_RENDER_MODE_NORMAL;
    if (FT_HAS_COLOR(face))
        load_flags |= FT_LOAD_COLOR;
    else if (lcd) {
        load_flags |= ordv ? FT_LOAD_TARGET_LCD_V : FT_LOAD_TARGET_LCD;
        render_mode = ordv ? FT_RENDER_MODE_LCD_V : FT_RENDER_MODE_LCD;
    }

    FT_Load_Glyph(face, glyph_index, load_flags);
    if (face->glyph->format != FT_GLYPH_FORMAT_BITMAP)
        FT_Render_Glyph(face->glyph, render_mode);

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

    struct glyph *glyph = aligned_alloc(_Alignof(struct glyph), ROUNDUP(sizeof(*glyph) + stride * face->glyph->bitmap.rows, _Alignof(struct glyph)));
    glyph->x = -face->glyph->bitmap_left;
    glyph->y = face->glyph->bitmap_top;
    glyph->width = face->glyph->bitmap.width;
    glyph->height = face->glyph->bitmap.rows;
    glyph->x_off = face->glyph->advance.x/64.;
    glyph->y_off = face->glyph->advance.y/64.;
    glyph->stride = stride;
    glyph->pixmode = ord;
    glyph->id = 0;

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
                fprintf(stderr, "%02x", glyph->data[glyph->stride*k+m]);
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
    struct list_head lru_list;
    size_t lru_size;
    int16_t char_width;
    int16_t char_height;
    int16_t char_depth;
    int16_t vspacing;
    int16_t hspacing;
    int16_t underline_width;
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

struct glyph_cache *create_glyph_cache(struct font *font, enum pixel_mode pixmode, int16_t vspacing, int16_t hspacing, int16_t underline_width, bool boxdraw, bool force_aligned) {
    struct glyph_cache *cache = xzalloc(sizeof(struct glyph_cache));

    cache->refc = 1;
    cache->font = font;
    cache->vspacing = vspacing;
    cache->hspacing = hspacing;
    cache->underline_width = underline_width;
    cache->override_boxdraw = boxdraw;
    cache->force_aligned = force_aligned;
    cache->pixmode = pixmode;
    ht_init(&cache->glyphs, HASH_INIT_CAP, glyph_cmp);
    list_init(&cache->lru_list);

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
        while (ht_current(&it)) {
            ht_head_t *elem = ht_erase_current(&it);
#if USE_XRENDER
            x11_xrender_release_glyph((struct glyph *)elem);
#endif
            free(elem);
        }
        ht_free(&cache->glyphs);
        free(cache);
    }
}

static inline void put(struct glyph *glyph, bool lcd, int16_t x, int16_t y, uint8_t val) {
    y = MIN(glyph->height - 1, y);
    if (!lcd) glyph->data[glyph->stride * y + x] = val;
    else for (size_t i = 0; i < 4; i++)
        glyph->data[glyph->stride*y + 4*x + i] = val;
}

static struct glyph *make_undercurl(int16_t width, int16_t depth, int16_t underline_width, enum pixel_mode pixmode, int16_t hspacing, bool force_aligned) {
    bool lcd = pixmode != pixmode_mono;
    size_t stride;

    if (force_aligned) {
        /* X11 MIT-SHM requires 16 byte alignment */
        stride = ROUNDUP(width, 4);
        if (lcd) stride *= 4;
    } else {
        /* X11 XRender backend requires all glyphs
         * to be in the same format (i.e. subpixel or not) */
        stride = lcd ? width * 4U : ROUNDUP(width, 4);
    }

    struct glyph *glyph = aligned_alloc(_Alignof(struct glyph), sizeof(struct glyph) +
            ROUNDUP(stride * depth * sizeof(uint8_t), _Alignof(struct glyph)));
    if (!glyph) return NULL;

    glyph->y_off = 0;
    glyph->y = 0;
    glyph->x = hspacing/2;
    glyph->height = depth;
    glyph->x_off = glyph->width = width;
    glyph->stride = stride;
    glyph->pixmode = pixmode;
    memset(glyph->data, 0x00, stride * depth);

    double underline = MAX(depth/4., MIN(underline_width, depth - 2));

    for (ssize_t x = 0; x < width; x++) {
        double y0f = (depth - underline - 1.5)*.5*(cos((x-width/4.)*2*M_PI/width) + 1);
        double y1f = y0f + underline;
        ssize_t y0 = MAX(0, MIN((ssize_t)y0f, depth - 1));
        ssize_t y1 = MAX(0, MIN((ssize_t)y1f, depth - 1));
        if (y0 != y1) {
            bool offset_0 = ceil(y0f) != y0;
            if (offset_0)
                put(glyph, lcd, x, y0, MIN(255, 255*(1 - (y0f - y0))));
            for (ssize_t i = y0 + offset_0; i < y1; i++)
                put(glyph, lcd, x, i, 255);
            put(glyph, lcd, x, y1, MIN(255, 255*(y1f - y1)));
        } else {
            put(glyph, lcd, x, y0, 255*underline);
        }
    }

    return glyph;
}

/* We don't cache ASCII characters */
#define LRU_MIN_CACHED 128

struct glyph *glyph_cache_fetch(struct glyph_cache *cache, uint32_t ch, enum face_name face, bool *is_new) {
    struct glyph dummy = mkglyphkey(ch | (face << 24));
    ht_head_t **h = ht_lookup_ptr(&cache->glyphs, (ht_head_t *)&dummy);
    if (*h) {
        if (is_new) *is_new = false;
        struct glyph *g = CONTAINEROF(*h, struct glyph, head);
        if (g->g >= LRU_MIN_CACHED) {
            list_remove(&g->lru);
            list_insert_after(&cache->lru_list, &g->lru);
        }
        return g;
    }

    if (is_new) *is_new = true;

    struct glyph *new;
#if USE_BOXDRAWING
    if (is_boxdraw(ch) && cache->override_boxdraw)
        new = make_boxdraw(ch, cache->char_width, cache->char_height, cache->char_depth,
                           cache->pixmode, cache->hspacing, cache->vspacing, cache->force_aligned);
    else
#endif
    if (ch == GLYPH_UNDERCURL)
        new = make_undercurl(cache->char_width, cache->char_depth, cache->underline_width,
                             cache->pixmode, cache->hspacing, cache->force_aligned);
    else
        new = font_render_glyph(cache->font, cache->pixmode, ch, face, cache->force_aligned, cache->char_width*2);
    if (!new) return NULL;

    new->g = dummy.g;
    new->head = dummy.head;

    ht_insert_hint(&cache->glyphs, h, &new->head);

    if (dummy.g >= LRU_MIN_CACHED) {
        if (cache->lru_size >= gconfig.font_cache_size) {
            struct glyph *old = CONTAINEROF(cache->lru_list.prev, struct glyph, lru);
            assert(old != new);
            list_remove(&old->lru);
#if USE_XRENDER
            x11_xrender_release_glyph(old);
#endif
            ht_erase(&cache->glyphs, &old->head);
            free(old);
        } else {
            cache->lru_size++;
        }
        list_insert_after(&cache->lru_list, &new->lru);
    }

    return new;
}
