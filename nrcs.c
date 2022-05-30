/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

#include "feature.h"

#include "nrcs.h"
#include "util.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static const unsigned short *nrcs_trs[] = {
    /*
     * Order of characters in this  string, like in trans_idx[]:
     * [0x23] [0x40] [0x5B 0x5C 0x5D 0x5E 0x5F 0x60] [0x7B 0x7C 0x7D 0x7E]
     * NOTE: This assumes UTF-8 encoding for source files.
     */
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

static const uint8_t trans_idx[] = {
    0x23, 0x40, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x7B, 0x7C, 0x7D, 0x7E
};

/* DEC Graph character set */
static const unsigned short graph_tr[] = u" ◆▒␉␌␍␊°±␤␋┘┐┌└┼⎺⎻─⎼⎽├┤┴┬│≤≥π≠£·";

/* DEC Technical character set */
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
        for (size_t i = 0; i < LEN(graph_tr); i++) {
            if (graph_tr[i] == *ch) {
                *ch = i + 0x5F;
                done = 1;
                break;
            }
        }
        done |= *ch < 0x5F || *ch == 0x7F;
        break;
    case cs94_dec_tech:
        for (size_t i = 0; i < LEN(tech_tr); i++) {
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
        for (size_t i = 0; i < LEN(trans_idx); i++) {
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

    // User preferred supplemental
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

struct nrcs_desc {
    uint16_t min_vt_level;
    uint16_t max_vt_level;
    uint32_t selector;
} descs[] = {
    [nrcs_finnish]            = { 2, 9, E('C') },
    [nrcs_finnish2]           = { 2, 9, E('5') },
    [nrcs_swedish]            = { 2, 9, E('H') },
    [nrcs_swedish2]           = { 2, 9, E('7') },
    [nrcs_german]             = { 2, 9, E('K') },
    [nrcs_french_canadian]    = { 2, 9, E('Q') },
    [nrcs_french]             = { 2, 9, E('R') },
    [nrcs_french2]            = { 2, 9, E('f') },
    [nrcs_itallian]           = { 2, 9, E('Y') },
    [nrcs_spannish]           = { 2, 9, E('Z') },
    [nrcs_dutch]              = { 2, 9, E('4') },
    [nrcs_swiss]              = { 2, 9, E('=') },
    [nrcs_norwegian_dannish]  = { 2, 9, E('E') },
    [nrcs_norwegian_dannish2] = { 2, 9, E('6') },
    [nrcs_norwegian_dannish3] = { 3, 9, E('`') },
    [nrcs_french_canadian2]   = { 3, 9, E('9') },
    [nrcs_portuguese]         = { 3, 9, E('6') | I1('%') },
    [nrcs_hebrew]             = { 5, 9, E('=') | I1('%') },
    [nrcs_greek]              = { 5, 9, E('>') | I1('"') },
    [nrcs_turkish]            = { 5, 9, E('2') | I1('%') },
    [nrcs_cyrillic]           = { 5, 9, E('4') | I1('&') },
    [cs94_ascii]              = { 1, 9, E('B') },
    [cs94_british]            = { 1, 9, E('A') },
    [cs94_dec_graph]          = { 1, 9, E('0') },
    [cs94_dec_altchars]       = { 1, 1, E('1') },
    [cs94_dec_altgraph]       = { 1, 1, E('2') },
    [cs94_dec_sup]            = { 2, 9, E('<') },
    [cs94_dec_sup_graph]      = { 3, 9, E('5') | I1('%') },
    [cs94_dec_tech]           = { 3, 9, E('>') },
    [cs94_dec_hebrew]         = { 5, 9, E('4') | I1('"') },
    [cs94_dec_greek]          = { 5, 9, E('?') | I1('"') },
    [cs94_dec_turkish]        = { 5, 9, E('0') | I1('%') },
    [cs96_latin_1]            = { 3, 9, E('A') },
    [cs96_greek]              = { 5, 9, E('F') },
    [cs96_hebrew]             = { 5, 9, E('H') },
    [cs96_latin_cyrillic]     = { 5, 9, E('L') },
    [cs96_latin_5]            = { 5, 9, E('M') },
};

enum charset nrcs_parse(uint32_t selector, bool is96, uint16_t vt_level, bool nrcs) {
    size_t start = is96 ? (nrcs ? cs96_END + 1 : cs96_START) :
                          (nrcs ? nrcs_START : cs94_START);
    size_t end = is96 ? cs96_END + 1 : cs94_END + 1;
    selector &= I1_MASK | E_MASK;

    for (size_t i = start; i < end; i++)
        if (descs[i].selector == selector &&
                descs[i].min_vt_level <= vt_level &&
                vt_level <= descs[i].max_vt_level)
            return i;

    return nrcs_invalid;
}

const char *nrcs_unparse(enum charset cs) {
    static char selstring[3];
    selstring[1] = '\0';
    selstring[0] = I1_CHAR(descs[cs].selector);
    selstring[!!selstring[0]] = E_CHAR(descs[cs].selector);
    return selstring;
}
