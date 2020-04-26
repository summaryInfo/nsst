/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#ifndef NRCS_H_
#define NRCS_H_ 1

#include "features.h"

#include <stdint.h>

enum nss_char_set {
    nss_nrcs_french_canadian,
    nss_nrcs_finnish,
    nss_nrcs_german,
    nss_nrcs_dutch,
    nss_nrcs_itallian,
    nss_nrcs_swiss,
    nss_nrcs_swedish,
    nss_nrcs_norwegian_dannish,
    nss_nrcs_french,
    nss_nrcs_spannish,
    nss_nrcs_portuguese,
    nss_nrcs_turkish,
    nss_nrcs_hebrew, // Not implemented
    nss_nrcs_greek, // Not implemented
    nss_nrcs_cyrillic, // Not implemented
    nss_nrcs_french_canadian2,
    nss_nrcs_finnish2,
    nss_nrcs_swedish2,
    nss_nrcs_norwegian_dannish2,
    nss_nrcs_norwegian_dannish3,
    nss_nrcs_french2,

    nss_94cs_ascii,
    nss_94cs_dec_altchars,
    nss_94cs_dec_altgraph,
    nss_94cs_british, // same as latin-1
    nss_94cs_dec_sup,
    nss_94cs_dec_sup_graph,
    nss_94cs_dec_graph,
    nss_94cs_dec_tech,
    nss_94cs_dec_greek, // Not implemented
    nss_94cs_dec_hebrew, // Not implemented
    nss_94cs_dec_turkish, // Not implemented

    nss_96cs_latin_1,
    nss_96cs_greek, // Not implemented
    nss_96cs_hebrew, // Not implemented
    nss_96cs_latin_cyrillic, // Not implemented
    nss_96cs_latin_5,
    nss_nrcs_MAX = nss_96cs_latin_5,
};

typedef uint32_t nss_char_t;

_Bool nrcs_encode(nss_char_t set, enum nss_char_set *ch, _Bool nrcs);
nss_char_t nrcs_decode(enum nss_char_set gl, enum nss_char_set gr, nss_char_t ch, _Bool nrcs);

#endif

