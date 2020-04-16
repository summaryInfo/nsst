/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_BITMAP_H
#include FT_FREETYPE_H

#include "config.h"
#include "font.h"
#include "util.h"
#include "window.h"

struct nss_font_state {
    size_t fonts;
    FT_Library library;
} global = { 0 };

typedef struct nss_face_list {
        size_t length;
        size_t caps;
        FT_Face *faces;
} nss_face_list_t;

/* Cache first two unicode planes */
#define LOADED_MAP_SIZE (2 * 65536 / 32)

struct nss_font {
    size_t refs;
    uint16_t dpi;
    double pixel_size;
    double size;
    uint32_t *loaded_map;
    nss_face_list_t face_types[nss_font_attrib_max];
};

typedef struct nss_paterns_holder {
    size_t length;
    size_t caps;
    FcPattern **pats;
} nss_paterns_holder_t;

void nss_free_font(nss_font_t *font) {
    if (--font->refs == 0) {
        for (size_t i = 0; i < nss_font_attrib_max; i++) {
            for (size_t j = 0; j < font->face_types[i].length; j++)
                FT_Done_Face(font->face_types[i].faces[j]);
            free(font->face_types[i].faces);
        }
        if (--global.fonts == 0) {
            FcFini();
            FT_Done_FreeType(global.library);
        }
        free(font->loaded_map);
        free(font);
    }
}

static void load_append_fonts(nss_font_t *font, nss_face_list_t *faces, nss_paterns_holder_t pats) {
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

#define CAPS_STEP 2

static void load_face_list(nss_font_t *font, nss_face_list_t* faces, const char *str, nss_font_attrib_t attr, double size) {
    char *tmp = strdup(str);
    nss_paterns_holder_t pats = {
        .length = 0,
        .caps = CAPS_STEP,
        .pats = calloc(CAPS_STEP, sizeof(*pats.pats))
    };

    for (char *tok = strtok(tmp, ","); tok; tok = strtok(NULL, ",")) {
        FcPattern *final_pat = NULL;
        FcPattern *pat = FcNameParse((FcChar8*) tok);
        FcPatternAddDouble(pat, FC_DPI, font->dpi);
        //FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
        FcPatternDel(pat, FC_STYLE);
        FcPatternDel(pat, FC_WEIGHT);
        FcPatternDel(pat, FC_SLANT);

        switch (attr) {
        case nss_font_attrib_normal:
            FcPatternAddString(pat, FC_STYLE, (FcChar8*) "Regular");
            FcPatternAddInteger(pat, FC_WEIGHT, FC_WEIGHT_REGULAR);
            FcPatternAddInteger(pat, FC_SLANT, FC_SLANT_ROMAN);
            break;
        case nss_font_attrib_normal | nss_font_attrib_italic:
            FcPatternAddString(pat, FC_STYLE, (FcChar8*) "Italic");
            FcPatternAddInteger(pat, FC_SLANT, FC_SLANT_ITALIC);
            FcPatternAddInteger(pat, FC_WEIGHT, FC_WEIGHT_REGULAR);
            break;
        case nss_font_attrib_bold:
            FcPatternAddString(pat, FC_STYLE, (FcChar8*) "Bold");
            FcPatternAddInteger(pat, FC_SLANT, FC_SLANT_ROMAN);
            FcPatternAddInteger(pat, FC_WEIGHT, FC_WEIGHT_BOLD);
            break;
        case nss_font_attrib_bold | nss_font_attrib_italic:
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

nss_font_t *nss_create_font(const char* descr, double size, uint16_t dpi) {
    if (global.fonts++ == 0) {
        if (FcInit() == FcFalse)
            die("Can't initialize fontconfig");
        if (FT_Init_FreeType(&global.library) != FT_Err_Ok) {
            die("Can't initialize freetype2, error: %s", strerror(errno));
        }
    }

    nss_font_t *font = calloc(1, sizeof(*font));
    if (!font) {
        warn("Can't allocate font");
        return NULL;
    }
    font->loaded_map = calloc(LOADED_MAP_SIZE, sizeof(font->loaded_map[0]));
    if (!font->loaded_map) {
        warn("Can't allocate font glyph map");
        free(font);
        return NULL;
    }

    font->refs = 1;
    font->pixel_size = 0;
    font->dpi = dpi;
    font->size = size;


    for (size_t i = 0; i < nss_font_attrib_max; i++)
        load_face_list(font, &font->face_types[i], descr, i, size);

    // If can't find suitable, use 13 by default
    if (font->pixel_size == 0)
        font->pixel_size = 13.0;

    return font;
}

nss_font_t *nss_font_reference(nss_font_t *font) {
    font->refs++;
    return font;
}

nss_glyph_t *nss_font_render_glyph(nss_font_t *font, uint32_t ch, nss_font_attrib_t attr, _Bool lcd) {
    nss_face_list_t *faces = &font->face_types[attr];
    int glyph_index = 0;
    FT_Face face = faces->faces[0];
    for (size_t i = 0; glyph_index == 0 && i < faces->length; i++)
        if ((glyph_index = FT_Get_Char_Index(faces->faces[i], ch)))
            face = faces->faces[i];
    //size_t sz = faces->faces[0]->size->metrics.x_ppem/font->dpi*72.0*64;

    FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT);

    size_t stride;
    if (lcd) {
        FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LCD);
        stride = 4*face->glyph->bitmap.width/3;
    } else {
        FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
        stride = (face->glyph->bitmap.width + 3) & ~3;
    }

    nss_glyph_t *glyph = malloc(sizeof(*glyph) + stride * face->glyph->bitmap.rows);
    glyph->x = -face->glyph->bitmap_left;
    glyph->y = face->glyph->bitmap_top;

    glyph->width = face->glyph->bitmap.width;
    if (lcd) glyph->width /= 3;
    glyph->height = face->glyph->bitmap.rows;
    glyph->x_off = face->glyph->advance.x/64.;
    glyph->y_off = face->glyph->advance.y/64.;
    glyph->stride = stride;


    int pitch = face->glyph->bitmap.pitch;
    uint8_t *src = face->glyph->bitmap.buffer;
    unsigned short num_grays = face->glyph->bitmap.num_grays;

    if (pitch < 0)
        src -= pitch*(face->glyph->bitmap.rows - 1);

    double gamma = nss_config_integer(NSS_ICONFIG_GAMMA) / 10000.0;

    switch(face->glyph->bitmap.pixel_mode) {
    case FT_PIXEL_MODE_MONO:
        for (size_t i = 0; i < glyph->height; i++)
            for (size_t j = 0; j < glyph->width; j++)
                glyph->data[stride*i + j] = 0xFF * ((src[pitch*i + j/8] >> (7 - j%8)) & 0x1);
        break;
    case FT_PIXEL_MODE_GRAY:
        for (size_t i = 0; i < glyph->height; i++)
            for (size_t j = 0; j < glyph->width; j++)
                glyph->data[stride*i + j] = 0xFF * pow(src[pitch*i + j] / (double)(num_grays - 1), gamma);
        break;
    case FT_PIXEL_MODE_GRAY2:
        for (size_t i = 0; i < glyph->height; i++)
            for (size_t j = 0; j < glyph->width; j++)
                glyph->data[stride*i + j] = 0x55 * ((src[pitch*i + j/4] >> (3 - j%4)) & 0x3);
        break;
    case FT_PIXEL_MODE_GRAY4:
        for (size_t i = 0; i < glyph->height; i++)
            for (size_t j = 0; j < glyph->width; j++)
                glyph->data[stride*i + j] = 0x11 * ((src[pitch*i + j/2] >> (1 - j%2)) & 0xF);
        break;
    case FT_PIXEL_MODE_LCD:
    case FT_PIXEL_MODE_LCD_V:
        for (size_t i = 0; i < glyph->height; i++) {
            for (size_t j = 0; j < glyph->width; j++) {
                int16_t acc = 0;
                acc += glyph->data[4*j + stride*i + 0] = 0xFF * pow(src[pitch*i + 3*j + 2] / 255., gamma);
                acc += glyph->data[4*j + stride*i + 1] = 0xFF * pow(src[pitch*i + 3*j + 1] / 255., gamma);
                acc += glyph->data[4*j + stride*i + 2] = 0xFF * pow(src[pitch*i + 3*j + 0] / 255., gamma);
                glyph->data[4*j + stride*i + 3] = acc/3;
            }
        }
        break;
    case FT_PIXEL_MODE_BGRA:
        warn("Colored glyph encountered");
        free(glyph);
        return nss_font_render_glyph(font, 0, attr, lcd);
    }

    if(nss_config_integer(NSS_ICONFIG_LOG_LEVEL) == 4) {

        debug("Bitmap mode: %d", face->glyph->bitmap.pixel_mode);
        debug("Num grays: %d", face->glyph->bitmap.num_grays);
        debug("Glyph: %d %d", glyph->width, glyph->height);
        size_t img_width = glyph->width;
        if (lcd) img_width *= 3;

        for (size_t k = 0; k < glyph->height; k++) {
            for (size_t m = 0 ;m < img_width; m++)
                fprintf(stderr, "%02x", src[pitch*k+m]);
            putc('\n', stderr);
        }
    }

    nss_font_glyph_mark_loaded(font, ch);

    return glyph;
}

void nss_font_glyph_mark_loaded(nss_font_t *font, tchar_t ch) {
    if (ch < LOADED_MAP_SIZE * 32)
        font->loaded_map[ch / 32] |= 1 << (ch % 32);
}

_Bool nss_font_glyph_is_loaded(nss_font_t *font, tchar_t ch) {
    if (ch >= 32 * LOADED_MAP_SIZE) return 0;
    else return font->loaded_map[ch / 32] & (1 << (ch % 32));
}

int16_t nss_font_get_size(nss_font_t *font) {
    return font->size;
}
