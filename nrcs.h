/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

#ifndef NRCS_H_
#define NRCS_H_ 1

#include "feature.h"

#include <stdbool.h>
#include <stdint.h>

/* NOTE: Order of groups is important */
enum charset {
    nrcs_french_canadian,
    nrcs_START = nrcs_french_canadian,
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
    nrcs_french_canadian2,
    nrcs_finnish2,
    nrcs_swedish2,
    nrcs_norwegian_dannish2,
    nrcs_norwegian_dannish3,
    nrcs_french2,
    nrcs__impl_high = nrcs_french2,
    nrcs_hebrew, /* Not implemented */
    nrcs_greek, /* Not implemented */
    nrcs_cyrillic, /* Not implemented */

    cs94_ascii,
    cs94_START = cs94_ascii,
    cs94_dec_altchars,
    cs94_dec_altgraph,
    cs94_british, /* Same as latin-1 */
    cs94_dec_sup, /* User prefered */
    cs94_dec_sup_graph,
    cs94_dec_graph,
    cs94_dec_tech,
    cs94_dec_greek, /* Not implemented */
    cs94_dec_hebrew, /* Not implemented */
    cs94_dec_turkish, /* Not implemented */
    cs94_END = cs94_dec_turkish,

    cs96_latin_1,
    cs96_START = cs96_latin_1,
    cs96_greek, /* Not implemented */
    cs96_hebrew, /* Not implemented */
    cs96_latin_cyrillic, /* Not implemented */
    cs96_latin_5,
    cs96_END = cs96_latin_5,

    nrcs_invalid = -1,
};

/*
 * Macros for encoding dispatch selectors
 * OSC commands just stores osc number as selector
 * (and OSC L/OSC l/OSC I are translated to 0/1/2).
 *
 * Generic escape sequences uses E(c) for final byte
 * and I0(c), I1(c) for first and second intermediate
 * bytes.
 *
 * CSI and DCS sequences use C(c) for final byte
 * P(c) for private indicator byte and
 * I0(c), I1(c) for intermediate bytes.
 *
 * *_MASK macros can be used to extract
 * corresponding parts of selectors.
 *
 * *_CHAR macros are used for getting
 * source character.
 */

#define I1_SHIFT 14
#define I0_SHIFT 9
#define P_SHIFT 6

#define C_MASK (0x3F)
#define E_MASK (0x7F)
#define I0_MASK (0x1F << I0_SHIFT)
#define I1_MASK (0x1F << I1_SHIFT)
#define P_MASK (0x7 << P_SHIFT)

#define C(c) ((c) & C_MASK)
#define E(c) ((c) & E_MASK)
#define I0(i) ((i) ? (((i) & 0xF) + 1) << I0_SHIFT : 0)
#define I1(i) ((i) ? (((i) & 0xF) + 1) << I1_SHIFT : 0)
#define P(p) ((p) ? ((((p) & 3) + 1) << P_SHIFT) : 0)

#define E_CHAR(s) ((s) & 0x7F)
#define I0_CHAR(s) ((s) >> I0_SHIFT ? (((s) >> I0_SHIFT) - 1) | ' ' : 0)
#define I1_CHAR(s) ((s) >> I1_SHIFT ? (((s) >> I1_SHIFT) - 1) | ' ' : 0)

static inline bool nrcs_is_96(enum charset cs) {
    return cs >= cs96_latin_1;
}

bool nrcs_encode(enum charset set, uint32_t *ch, bool nrcs);
uint32_t nrcs_decode(enum charset gl, enum charset gr, enum charset ups, uint32_t ch, bool nrcs);
uint32_t nrcs_decode_fast(enum charset gl, uint32_t ch);
enum charset nrcs_parse(uint32_t selector, bool is96, uint16_t vt_level, bool nrcs);
const char *nrcs_unparse(enum charset cs);

#endif
