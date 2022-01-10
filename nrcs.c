/* Copyright (c) 2019-2022, Evgeny Baskov. All rights reserved */

#include "feature.h"

#include "nrcs.h"
#include "util.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static const unsigned short *nrcs_trs[] = {
    /* [0x23] [0x40] [0x5B 0x5C 0x5D 0x5E 0x5F 0x60] [0x7B 0x7C 0x7D 0x7E] */
    [nrcs_french_canadian] =    u"#àâçêî_ôéùèû",
    [nrcs_french_canadian2] =   u"#àâçêî_ôéùèû",
    [nrcs_finnish] =            u"#@ÄÖÅÜ_éäöåü",
    [nrcs_finnish2] =           u"#@ÄÖÅÜ_éäöåü",
    [nrcs_german] =             u"#§ÄÖÜ^_`äöüß",
    [nrcs_dutch] =              u"£¾ĳ½|^_`¨f¼´",
    [nrcs_itallian] =           u"£§°çé^_ùàòèì",
    [nrcs_swiss] =              u"ùàéçêîèôäöüû",
    [nrcs_swedish] =            u"#ÉÆØÅÜ_éæøåü",
    [nrcs_swedish2] =           u"#ÉÆØÅÜ_éæøåü",
    [nrcs_norwegian_dannish] =  u"#ÄÆØÅÜ_äæøåü",
    [nrcs_norwegian_dannish2] = u"#ÄÆØÅÜ_äæøåü",
    [nrcs_norwegian_dannish3] = u"#ÄÆØÅÜ_äæøåü",
    [nrcs_french] =             u"£à°ç§^_`éùè¨",
    [nrcs_french2] =            u"£à°ç§^_`éùè¨",
    [nrcs_spannish] =           u"£§¡Ñ¿^_`°ñç~",
    [nrcs_portuguese] =         u"#@ÃÇÕ^_`ãçõ~",
    [nrcs_turkish] =            u"#İŞÖÇÜ_Ğşöçü",
};

static const uint8_t trans_idx[] = { 0x23, 0x40, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x7B, 0x7C, 0x7D, 0x7E };

static const unsigned short graph_tr[] = u" ◆▒␉␌␍␊°±␤␋┘┐┌└┼⎺⎻─⎼⎽├┤┴┬│≤≥π≠£·";

static const unsigned short tech_tr[] = {
            0x23B7, 0x250C, 0x2500, 0x2320, 0x2321, 0x2502, 0x23A1,
    0x23A3, 0x23A4, 0x23A6, 0x239B, 0x239D, 0x239E, 0x23A0, 0x23A8,
    0x23AC, 0xFFFE, 0xFFFE, 0xFFFE, 0xFFFE, 0xFFFE, 0xFFFE, 0xFFFE,
    0xFFFE, 0xFFFE, 0xFFFE, 0xFFFE, 0x2264, 0x2260, 0x2265, 0x222B,
    0x2234, 0x221D, 0x221E, 0x00F7, 0x0394, 0x2207, 0x03A6, 0x0393,
    0x223C, 0x2243, 0x0398, 0x00D7, 0x039B, 0x21D4, 0x21D2, 0x2261,
    0x03A0, 0x03A8, 0xFFFE, 0x03A3, 0xFFFE, 0xFFFE, 0x221A, 0x03A9,
    0x039E, 0x03A5, 0x2282, 0x2283, 0x2229, 0x222A, 0x2227, 0x2228,
    0x00AC, 0x03B1, 0x03B2, 0x03C7, 0x03B4, 0x03B5, 0x03C6, 0x03B3,
    0x03B7, 0x03B9, 0x03B8, 0x03BA, 0x03BB, 0xFFFE, 0x03BD, 0x2202,
    0x03C0, 0x03C8, 0x03C1, 0x03C3, 0x03C4, 0xFFFE, 0x0192, 0x03C9,
    0x03BE, 0x03C5, 0x03B6, 0x2190, 0x2191, 0x2192, 0x2193,
};

bool nrcs_encode(enum charset set, uint32_t *ch, bool nrcs) {
    bool done = 0;
    switch (set) {
    case cs94_ascii:
    case cs94_dec_altchars:
    case cs94_dec_altgraph:
        done = *ch < 0x80;
        break;
    case cs94_british:
    case cs96_latin_1:
        if (!nrcs || set == cs96_latin_1) {
            if (0x80 <= *ch && *ch < 0x100)
                *ch -= 0x80, done = 1;
        } else {
            if (*ch == U'£') *ch = '$', done = 1;
            else done = *ch != '$';
        }
        break;
    case cs94_dec_sup:
    case cs94_dec_sup_graph:
        switch (*ch) {
        case U'¤': *ch = 0xA8 - 0x80; done = 1; break;
        case U'Œ': *ch = 0xD7 - 0x80; done = 1; break;
        case U'Ÿ': *ch = 0xDD - 0x80; done = 1; break;
        case U'œ': *ch = 0xF7 - 0x80; done = 1; break;
        case U'ÿ': *ch = 0xFD - 0x80; done = 1; break;
        }
        if (*ch >= 0x80 && *ch < 0x100) {
            done = (*ch != 0xA8 &&
                   (*ch & ~0x20) != 0xD7 &&
                   (*ch & ~0x20) != 0xDD);
            if (done) *ch -= 0x80;
        }
        break;
    case cs96_latin_5:
        switch (*ch) {
        case U'Ğ': *ch = 0xD0 - 0x80; done = 1; break;
        case U'İ': *ch = 0xDD - 0x80; done = 1; break;
        case U'Ş': *ch = 0xDE - 0x80; done = 1; break;
        case U'ğ': *ch = 0xF0 - 0x80; done = 1; break;
        case U'ı': *ch = 0xFD - 0x80; done = 1; break;
        case U'ş': *ch = 0xFE - 0x80; done = 1; break;
        }
        if (*ch >= 0x80 && *ch < 0x100) {
            done = ((*ch & ~0x20) != 0xD0 &&
                    (*ch & ~0x20) != 0xDD &&
                    (*ch & ~0x20) != 0xDE);
            if (done) *ch -= 0x80;
        }
        break;

    case cs94_dec_graph:
        for (size_t i = 0; i < sizeof(graph_tr)/sizeof(*graph_tr); i++) {
            if (graph_tr[i] == *ch) {
                *ch = i + 0x5F;
                done = 1;
                break;
            }
        }
        done |= *ch < 0x5F || *ch == 0x7F;
        break;
    case cs94_dec_tech:
        for (size_t i = 0; i < sizeof(tech_tr)/sizeof(*tech_tr); i++) {
            if (tech_tr[i] == *ch) {
                *ch = i + 0x21;
                done = 1;
                break;
            }
        }
        done |= *ch < 0x21 || *ch == 0x7F;
        break;
    case nrcs_turkish:
        if (*ch == U'ğ') *ch = 0x26, done = 1;
        break;
    default:;
    }

    if (set <= nrcs__impl_high) {
        for (size_t i = 0; i < sizeof(trans_idx)/sizeof(*trans_idx); i++) {
            if (nrcs_trs[set][i] == *ch) {
                *ch = trans_idx[i];
                done = 1;
                break;
            }
        }
        done |= (*ch < 0x7B && *ch != 0x23 && *ch != 0x40
                 && !(0x5B <= *ch && *ch <= 0x60)) || *ch == 0x7F;
    }

    return done;
}

uint32_t nrcs_decode_fast(enum charset gl, uint32_t ch) {
    if (UNLIKELY(gl == cs94_dec_graph)) {
        if (0x5F <= ch && ch <= 0x7E)
            ch = graph_tr[ch - 0x5F];
    }
    return ch;
}

uint32_t nrcs_decode(enum charset gl, enum charset gr, enum charset ups, uint32_t ch, bool nrcs) {
    if (ch > 0xFF) return ch;
    if (ch == 0x7F) return U' ';

    enum charset set = ch > 0x7F ? gr : gl;

    // User prefered supplemental
    if (set == cs94_dec_sup) set = ups;

    switch (set) {
    case cs94_ascii:
    case cs94_dec_altchars:
    case cs94_dec_altgraph:
        return ch;
    case cs94_dec_sup:
    case cs94_dec_sup_graph:
        switch (ch |= 0x80) {
        case 0xA8: return U'¤';
        case 0xD7: return U'Œ';
        case 0xDD: return U'Ÿ';
        case 0xF7: return U'œ';
        case 0xFD: return U'ÿ';
        }
        return ch;
    case cs94_dec_graph:
        ch &= 0x7F;
        if (0x5F <= ch && ch <= 0x7E)
            ch = graph_tr[ch - 0x5F];
        return ch;
    case cs96_latin_1:
    case cs94_british:
        if (nrcs) {
            ch &= 0x7F;
            if (ch == '#') ch = U'£';
            return ch;
        }
        return ch | 0x80;
    case cs96_latin_5:
        switch (ch |= 0x80) {
        case 0xD0: ch = U'Ğ'; break;
        case 0xDD: ch = U'İ'; break;
        case 0xDE: ch = U'Ş'; break;
        case 0xF0: ch = U'ğ'; break;
        case 0xFD: ch = U'ı'; break;
        case 0xFE: ch = U'ş'; break;
        }
        return ch;
    case cs94_dec_tech:
        ch &= 0x7F;
        if (0x20 < ch && ch < 0x7F)
            return tech_tr[ch - 0x21];
        return ch;
    case nrcs_turkish:
        if ((ch & 0x7F) == 0x26) return U'ğ';
    default:;
    }

    if (/* nrcs && */ set <= nrcs__impl_high) {
        ch &= 0x7F;
        if (ch == 0x23) return nrcs_trs[set][0];
        if (ch == 0x40) return nrcs_trs[set][1];
        if (0x5B <= ch && ch <= 0x60)
            return nrcs_trs[set][2 + ch - 0x5B];
        if (0x7B <= ch && ch <= 0x7E)
            return nrcs_trs[set][8 + ch - 0x7B];
    }

    return ch;
}

const char *nrcs_unparse(enum charset cs) {
    return (const char *[nrcs_MAX + 1]){
        [cs94_ascii]              = "B",
        [cs94_british]            = "A",
        [cs94_dec_altchars]       = "1",
        [cs94_dec_altgraph]       = "2",
        [cs94_dec_graph]          = "0",
        [cs94_dec_greek]          = "\"?",
        [cs94_dec_hebrew]         = "\"4",
        [cs94_dec_sup]            = "<",
        [cs94_dec_sup_graph]      = "%5",
        [cs94_dec_tech]           = ">",
        [cs94_dec_turkish]        = "%0",
        [cs96_greek]              = "F",
        [cs96_hebrew]             = "H",
        [cs96_latin_1]            = "A",
        [cs96_latin_5]            = "M",
        [cs96_latin_cyrillic]     = "L",
        [nrcs_cyrillic]           = "&4",
        [nrcs_dutch]              = "4",
        [nrcs_finnish2]           = "5",
        [nrcs_finnish]            = "C",
        [nrcs_french2]            = "f",
        [nrcs_french]             = "R",
        [nrcs_french_canadian2]   = "9",
        [nrcs_french_canadian]    = "Q",
        [nrcs_german]             = "K",
        [nrcs_greek]              = "\">",
        [nrcs_hebrew]             = "%=",
        [nrcs_itallian]           = "Y",
        [nrcs_norwegian_dannish2] = "6",
        [nrcs_norwegian_dannish3] = "`",
        [nrcs_norwegian_dannish]  = "E",
        [nrcs_portuguese]         = "%6",
        [nrcs_spannish]           = "Z",
        [nrcs_swedish2]           = "7",
        [nrcs_swedish]            = "H",
        [nrcs_swiss]              = "=",
        [nrcs_turkish]            = "%2",
    }[cs];
}


enum charset nrcs_parse(uint32_t selector, bool is96, uint16_t vt_level, bool nrcs) {
#define E(c) ((c) & 0x7F)
#define I0(i) ((i) ? (((i) & 0xF) + 1) << 9 : 0)
#define I1(i) (I0(i) << 5)
#define E_MASK (0x7F)
#define I1_MASK (0x1F << 14)
#define NRC {if (!nrcs) return nrcs_invalid;}
    selector &= (I1_MASK | E_MASK);
    if (!is96) {
        switch (vt_level) {
        default:
            switch (selector) {
            case E('4') | I1('"'): return cs94_dec_hebrew;
            case E('?') | I1('"'): return cs94_dec_greek;
            case E('0') | I1('%'): return cs94_dec_turkish;
            case E('=') | I1('%'): NRC; return nrcs_hebrew;
            case E('>') | I1('"'): NRC; return nrcs_greek;
            case E('2') | I1('%'): NRC; return nrcs_turkish;
            case E('4') | I1('&'): NRC; return nrcs_cyrillic;
            }
            /* fallthrough */
        case 4: case 3:
            switch (selector) {
            case E('5') | I1('%'): return cs94_dec_sup_graph;
            case E('`'): NRC; return nrcs_norwegian_dannish3;
            case E('9'): NRC; return nrcs_french_canadian2;
            case E('>'): return cs94_dec_tech;
            case E('6') | I1('%'): NRC; return nrcs_portuguese;
            }
            /* fallthrough */
        case 2:
            switch (selector) {
            case E('C'): NRC; return nrcs_finnish;
            case E('5'): NRC; return nrcs_finnish2;
            case E('H'): NRC; return nrcs_swedish;
            case E('7'): NRC; return nrcs_swedish2;
            case E('K'): NRC; return nrcs_german;
            case E('Q'): NRC; return nrcs_french_canadian;
            case E('R'): NRC; return nrcs_french;
            case E('f'): NRC; return nrcs_french2;
            case E('Y'): NRC; return nrcs_itallian;
            case E('Z'): NRC; return nrcs_spannish;
            case E('4'): NRC; return nrcs_dutch;
            case E('='): NRC; return nrcs_swiss;
            case E('E'): NRC; return nrcs_norwegian_dannish;
            case E('6'): NRC; return nrcs_norwegian_dannish2;
            case E('<'): return cs94_dec_sup;
            }
            /* fallthrough */
        case 1:
            switch (selector) {
            case E('A'): return cs94_british;
            case E('B'): return cs94_ascii;
            case E('0'): return cs94_dec_graph;
            case E('1'): if (vt_level != 1) break;
                         return cs94_dec_altchars;
            case E('2'): if (vt_level != 1) break;
                         return cs94_dec_altgraph;
            }
            /* fallthrough */
        case 0: break;
        }
    } else {
        switch (vt_level) {
        default:
            switch (selector) {
            case E('F'): return cs96_greek;
            case E('H'): return cs96_hebrew;
            case E('L'): return cs96_latin_cyrillic;
            case E('M'): return cs96_latin_5;
            }
            /* fallthrough */
        case 4: case 3:
            switch (selector) {
            case E('A'): return cs96_latin_1;
            }
            /* fallthrough */
        case 2: case 1: case 0:
            break;
        }
    }
    return nrcs_invalid;
#undef NRC
}

