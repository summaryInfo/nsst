#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "font.h"
#include "window.h"
#include "util.h"

struct nss_font_state {
    _Bool fc_initialized;
    size_t fonts;
} global = { 0, 0 };

typedef struct nss_face_list {
        size_t length;
        size_t caps;
        FT_Face *faces;
} nss_face_list_t;

struct nss_font {
    FT_Library library;
    FcCharSet *subst_chars;
    uint16_t dpi;
    double pixel_size;
    nss_face_list_t face_types[nss_font_attrib_max];
};

typedef struct nss_paterns_holder {
    size_t length;
    size_t caps;
    FcPattern **pats;
} nss_paterns_holder_t;

void nss_free_font(nss_font_t *font){
    for(size_t i = 0; i < nss_font_attrib_max; i++){
        for(size_t j = 0; j < font->face_types[i].length; j++)
            FT_Done_Face(font->face_types[i].faces[j]);
        free(font->face_types[i].faces);
    }
    FT_Done_FreeType(font->library);
    FcCharSetDestroy(font->subst_chars);
    if(--global.fonts == 0){
        FcFini();
        global.fc_initialized = 0;
    }
}

static void load_append_fonts(nss_font_t *font, nss_face_list_t *faces, nss_paterns_holder_t pats){
    size_t new_size = faces->length + pats.length;
    if(new_size > faces->caps){
        FT_Face *new = realloc(faces->faces, (faces->length + pats.length)*sizeof(FT_Face));
        if(!new){
            warn("Can't relocate face list");
            return;
        }
        faces->faces = new;
        faces->caps += pats.length;
    }

    for(size_t i = 0; i < pats.length; i++){
        FcValue file, index, matrix, pixsize;
        if(FcPatternGet(pats.pats[i], FC_FILE, 0, &file) != FcResultMatch){
            warn("Can't find file for font");
            continue;
        }
        if(FcPatternGet(pats.pats[i], FC_INDEX, 0, &index) != FcResultMatch){
            warn("Can't get font file index, selecting 0");
            index.type = FcTypeInteger;
            index.u.i = 0;
        }

        info("Font file: %s:%d", file.u.s, index.u.i);
        FT_Error err = FT_New_Face(font->library, (const char*)file.u.s, index.u.i, &faces->faces[faces->length]);
        if(err != FT_Err_Ok){
            if(err == FT_Err_Unknown_File_Format) warn("Wrong font file format");
            else if(err == FT_Err_Cannot_Open_Resource) warn("Can't open resource");
            else warn("Some error happend loading file: %u", err);
            continue;
        }
        if(!faces->faces[faces->length]){
            warn("Empty font face");
            continue;
        }

        if(FcPatternGet(pats.pats[i], FC_MATRIX, 0, &matrix) == FcResultMatch){
            FT_Matrix ftmat; 
            ftmat.xx = (FT_Fixed)(matrix.u.m->xx * 0x10000L);
            ftmat.xy = (FT_Fixed)(matrix.u.m->xy * 0x10000L);
            ftmat.yx = (FT_Fixed)(matrix.u.m->yx * 0x10000L);
            ftmat.yy = (FT_Fixed)(matrix.u.m->yy * 0x10000L);
            FT_Set_Transform(faces->faces[faces->length], &ftmat, NULL);
        }

        FcResult res = FcPatternGet(pats.pats[i], FC_PIXEL_SIZE, 0, &pixsize);
        if(res != FcResultMatch || pixsize.u.d == 0) {
            warn("Font has no pixel size, selecting default");
            pixsize.type = FcTypeDouble;
            pixsize.u.d = font->pixel_size;
        }

        uint16_t sz = (pixsize.u.d/font->dpi*72.0)*64;
        if(FT_Set_Char_Size(faces->faces[faces->length], 0, sz, font->dpi, font->dpi) != FT_Err_Ok){
            warn("Error: %s", strerror(errno));
            warn("Can't set char size");
            continue;
        }

        faces->length++;
    }
}

static void add_font_substitude(nss_font_t *font, nss_face_list_t *faces, FcChar32 ch, const FcPattern *pat){
    if(!font->subst_chars)
        font->subst_chars = FcCharSetCreate();
    FcCharSetAddChar(font->subst_chars, ch);

    FcPattern *chset_pat = pat ? FcPatternDuplicate(pat) : FcPatternCreate();
    FcPatternAddDouble(chset_pat, FC_DPI, font->dpi);
    FcPatternAddCharSet(chset_pat, FC_CHARSET, font->subst_chars);
    FcPatternAddBool(chset_pat, FC_SCALABLE, FcTrue);
    FcDefaultSubstitute(chset_pat);
    if(FcConfigSubstitute(NULL, chset_pat, FcMatchPattern) == FcFalse){
        warn("Can't substitude font config");
        return;
    }

    FcResult result;
    FcPattern *final_pat = FcFontMatch(NULL, chset_pat, &result);
    if(result != FcResultMatch){
        warn("Font doesn't match");
        return;
    }
    
    load_append_fonts(font, faces, (nss_paterns_holder_t){.length = 1, .pats = &final_pat });
    FcPatternDestroy(final_pat);
}

#define CAPS_STEP 8

static void load_face_list(nss_font_t *font, nss_face_list_t* faces, const char *str, nss_font_attrib_t attr){
    char *tmp = strdup(str);
    nss_paterns_holder_t pats = {
        .length = 0, 
        .caps = CAPS_STEP,
        .pats = malloc(sizeof(*pats.pats)*pats.caps)
    };
    
    for(char *tok = strtok(tmp, ","); tok; tok = strtok(NULL, ",")){
        FcPattern *final_pat = NULL;
        FcPattern *pat = FcNameParse((FcChar8*) tok);
        FcPatternAddDouble(pat, FC_DPI, font->dpi);
        FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
        FcPatternDel(pat, FC_STYLE);

		//TODO: May be add more styles here?
        switch(attr){
        case nss_font_attrib_normal:
            FcPatternAddString(pat, FC_STYLE, (FcChar8*) "Regular"); break;
        case nss_font_attrib_normal | nss_font_attrib_italic:
            FcPatternAddString(pat, FC_STYLE, (FcChar8*) "Italic"); break;
        case nss_font_attrib_bold:
            FcPatternAddString(pat, FC_STYLE, (FcChar8*) "Bold"); break;
        case nss_font_attrib_bold | nss_font_attrib_italic:
            FcPatternAddString(pat, FC_STYLE, (FcChar8*) "Bold Italic"); break;
        default:
            warn("Unknown face type");
        }

        FcDefaultSubstitute(pat);
        if(FcConfigSubstitute(NULL, pat, FcMatchPattern) == FcFalse){
            warn("Can't substitute font config for font: %s", tok);
            continue;
        }

        FcResult result;
        final_pat = FcFontMatch(NULL, pat, &result);
        if(result != FcResultMatch) {
            warn("No match for font: %s", tok);
            continue;
        }

        if(pats.length + 1 > pats.caps){
            FcPattern **new = realloc(pats.pats,sizeof(*pats.pats)*(pats.caps + CAPS_STEP));
            if(!new){
                warn("Out of memory");
                continue;
            }
            pats.caps += CAPS_STEP;
            pats.pats = new;
        }

        font->pixel_size = 0;
        for(size_t i = 0; i < pats.length; i++){
            FcValue pixsize;
            if(FcPatternGet(pats.pats[i], FC_PIXEL_SIZE, 0, &pixsize) == FcResultMatch){
                if(pixsize.u.d > font->pixel_size)
                    font->pixel_size = pixsize.u.d;
            }
        }

        pats.pats[pats.length++] = final_pat; 
    }
    free(tmp);

    load_append_fonts(font, faces, pats);

    for(size_t i = 0; i < pats.length; i++)
        FcPatternDestroy(pats.pats[i]);
    free(pats.pats);
}

nss_font_t *nss_create_font(const char* descr, uint16_t dpi){
    if(global.fonts++ == 0){
        if(FcInit() == FcFalse)
            die("Can't initialize fontconfig");
        global.fc_initialized = 1;
    }

    nss_font_t *font = calloc(1, sizeof(*font));
    if(!font) {
        warn("Can't allocate font");
        return NULL;
    }

    font->dpi = dpi;

    if(FT_Init_FreeType(&font->library) != FT_Err_Ok){
        free(font);
        warn("Error: %s", strerror(errno));
        return NULL;
    }

    for(size_t i = 0; i < nss_font_attrib_max; i++)
        load_face_list(font, &font->face_types[i], descr, i);

    // If can't find suitable, use 13 by default
    if(font->pixel_size == 0)
        font->pixel_size = 13.0;

    return font;
}

nss_glyph_t *nss_font_render_glyph(nss_font_t *font, uint32_t ch, nss_font_attrib_t attr){
    nss_face_list_t *faces = &font->face_types[attr];
    int glyph_index = 0;
    FT_Face face = faces->faces[0];
    for(size_t i = 0; !glyph_index && i < faces->length; i++)
        if((glyph_index = FT_Get_Char_Index(faces->faces[i],ch)))
            face = faces->faces[i];
    if(glyph_index == 0){
        size_t sz = faces->faces[0]->size->metrics.x_ppem/font->dpi*72.0*64;
        size_t oldlen = faces->length;
        //TODO: Clarify pattern here?
        add_font_substitude(font,faces, ch, NULL);
        for(size_t i = oldlen; !glyph_index && i < faces->length; i++){
            FT_Set_Char_Size(faces->faces[i], 0, sz, font->dpi, font->dpi);
            if((glyph_index = FT_Get_Char_Index(faces->faces[i],ch)))
                face = faces->faces[i];
        }
    }

    FT_Load_Glyph(face, glyph_index, FT_LOAD_RENDER | FT_LOAD_FORCE_AUTOHINT);

    size_t stride = (face->glyph->bitmap.width + 3) & ~3; 
    nss_glyph_t *glyph = malloc(sizeof(*glyph) + stride * face->glyph->bitmap.rows);
    glyph->x = -face->glyph->bitmap_left;
    glyph->y = face->glyph->bitmap_top;
    glyph->width = face->glyph->bitmap.width;
    glyph->height = face->glyph->bitmap.rows;
    glyph->x_off = face->glyph->advance.x/64.;
    glyph->y_off = face->glyph->advance.y/64.;
    glyph->stride = stride;

    int pitch = face->glyph->bitmap.pitch;
    uint8_t *src = face->glyph->bitmap.buffer;

    if(pitch < 0) {
        info("Triggered");
        src -= pitch*(face->glyph->bitmap.rows - 1);
    }

    for(size_t i = 0; i < glyph->height; i++)
        memcpy(glyph->data + stride*i, src + pitch*i, glyph->width);

    return glyph;
}
