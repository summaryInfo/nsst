/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#ifndef NRCS_H_
#define NRCS_H_ 1

#include "feature.h"

#include <stdint.h>

enum charset {
    nrcs_french_canadian,
    nrcs_finnish,
    nrcs_german,
    nrcs_dutch,
    nrcs_itallian,
    nrcs_swiss,
    nrcs_swedish,
    nrcs_norwegian_dannish,
    nrcs_french,
    nrcs_spannish,
    nrcs_portuguese,
    nrcs_turkish,
    nrcs_hebrew, // Not implemented
    nrcs_greek, // Not implemented
    nrcs_cyrillic, // Not implemented
    nrcs_french_canadian2,
    nrcs_finnish2,
    nrcs_swedish2,
    nrcs_norwegian_dannish2,
    nrcs_norwegian_dannish3,
    nrcs_french2,

    cs94_ascii,
    cs94_dec_altchars,
    cs94_dec_altgraph,
    cs94_british, // Same as latin-1
    cs94_dec_sup, // User prefered
    cs94_dec_sup_graph,
    cs94_dec_graph,
    cs94_dec_tech,
    cs94_dec_greek, // Not implemented
    cs94_dec_hebrew, // Not implemented
    cs94_dec_turkish, // Not implemented

    cs96_latin_1,
    cs96_greek, // Not implemented
    cs96_hebrew, // Not implemented
    cs96_latin_cyrillic, // Not implemented
    cs96_latin_5,
    nrcs_MAX = cs96_latin_5,
};

typedef uint32_t nss_char_t;

inline static _Bool nrcs_is_96(enum charset cs) {
    return cs >= cs96_latin_1;
}

_Bool nrcs_encode(nss_char_t set, enum charset *ch, _Bool nrcs);
nss_char_t nrcs_decode(enum charset gl, enum charset gr, enum charset ups, nss_char_t ch, _Bool nrcs);
nss_char_t nrcs_decode_fast(enum charset gl, nss_char_t ch);
enum charset nrcs_parse(uint32_t selector, _Bool is96, uint16_t vt_level, _Bool nrcs);
const char *nrcs_unparse(enum charset cs);

#endif

