#include <stdint.h>
#include <stddef.h>

#include "nrcs.h"

static const unsigned short *nrcs_trs[] = {
    /* [0x23] [0x40] [0x5B 0x5C 0x5D 0x5E 0x5F 0x60] [0x7B 0x7C 0x7D 0x7E] */
    [nss_nrcs_french_canadian] =   u"#àâçêî_ôéùèû",
    [nss_nrcs_finnish] =           u"#@ÄÖÅÜ_éäöåü",
    [nss_nrcs_german] =            u"#§ÄÖÜ^_`äöüß",
    [nss_nrcs_dutch] =             u"£¾\u0133½|^_`¨f¼´",
    [nss_nrcs_itallian] =          u"£§°çé^_ùàòèì",
    [nss_nrcs_swiss] =             u"ùàéçêîèôäöüû",
    [nss_nrcs_swedish] =           u"#ÉÆØÅÜ_éæøåü",
    [nss_nrcs_norwegian_dannish] = u"#ÄÆØÅÜ_äæøåü",
    [nss_nrcs_french] =            u"£à°ç§^_`éùè¨",
    [nss_nrcs_spannish] =          u"£§¡Ñ¿^_`°ñç~",
    [nss_nrcs_portuguese] =        u"#@ÃÇÕ^_`ãçõ~",
    [nss_nrcs_turkish] =           u"#İŞÖÇÜ_Ğşöçü",
};

static const uint8_t trans_idx[] = {
    0x23, 0x40, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x7B, 0x7C, 0x7D, 0x7E,
};

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

_Bool nrcs_encode(enum nss_char_set set, tchar_t *ch, _Bool nrcs) {
    _Bool done = 0;
    switch (set) {
    case nss_94cs_ascii:
    case nss_94cs_dec_altchars:
    case nss_94cs_dec_altgraph:
        done = *ch < 0x80;
        break;
    case nss_94cs_british:
    case nss_96cs_latin_1:
        if (!nrcs) {
            if (0x80 <= *ch && *ch < 0x100)
                *ch -= 0x80, done = 1;
        } else {
            if (*ch == U'£') *ch = '$', done = 1;
            else done = *ch != '$';
        }
        break;
    case nss_94cs_dec_sup:
    case nss_94cs_dec_sup_graph:
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
    case nss_96cs_latin_5:
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

    case nss_94cs_dec_graph:
        for (size_t i = 0; i < sizeof(graph_tr)/sizeof(*graph_tr); i++) {
            if (graph_tr[i] == *ch) {
                *ch = i + 0x5F;
                done = 1;
                break;
            }
        }
        done |= *ch < 0x5F || *ch == 0x7F;
        break;
    case nss_94cs_dec_tech:
        for (size_t i = 0; i < sizeof(tech_tr)/sizeof(*tech_tr); i++) {
            if (tech_tr[i] == *ch) {
                *ch = i + 0x21;
                done = 1;
                break;
            }
        }
        done |= *ch < 0x21 || *ch == 0x7F;
        break;
    case nss_nrcs_french_canadian2:
        set = nss_nrcs_french_canadian; break;
    case nss_nrcs_finnish2:
        set = nss_nrcs_finnish; break;
    case nss_nrcs_swedish2:
        set = nss_nrcs_swedish; break;
    case nss_nrcs_norwegian_dannish2:
    case nss_nrcs_norwegian_dannish3:
        set = nss_nrcs_norwegian_dannish; break;
    case nss_nrcs_french2:
        set = nss_nrcs_french; break;
    case nss_nrcs_turkish:
        if (*ch == U'ğ') *ch = 0x26, done = 1;
        break;
    default:;
    }

    if (set <= nss_nrcs_turkish) {
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

tchar_t nrcs_decode(enum nss_char_set gl, enum nss_char_set gr, tchar_t ch, _Bool nrcs) {
    if (ch > 0xFF) return ch;
    if (ch == 0x7F) return U' ';

    enum nss_char_set set = ch > 0x7F ? gr : gl;

    switch (set) {
    case nss_94cs_ascii:
    case nss_94cs_dec_altchars:
    case nss_94cs_dec_altgraph:
        return ch;
    case nss_94cs_dec_sup:
    case nss_94cs_dec_sup_graph:
        switch(ch |= 0x80) {
        case 0xA8: return U'¤';
        case 0xD7: return U'Œ';
        case 0xDD: return U'Ÿ';
        case 0xF7: return U'œ';
        case 0xFD: return U'ÿ';
        }
        return ch;
    case nss_94cs_dec_graph:
        ch &= 0x7F;
        if (0x5F <= ch && ch <= 0x7E)
            ch = graph_tr[ch - 0x5F];
        return ch;
    case nss_96cs_latin_1:
    case nss_94cs_british:
        if (nrcs) {
            ch &= 0x7F;
            if (ch == '#') ch = U'£';
            return ch;
        }
        return ch | 0x80;
    case nss_96cs_latin_5:
        switch(ch |= 0x80) {
        case 0xD0: ch = U'Ğ'; break;
        case 0xDD: ch = U'İ'; break;
        case 0xDE: ch = U'Ş'; break;
        case 0xF0: ch = U'ğ'; break;
        case 0xFD: ch = U'ı'; break;
        case 0xFE: ch = U'ş'; break;
        }
        return ch;
    case nss_94cs_dec_tech:
        ch &= 0x7F;
        if (0x20 < ch && ch < 0x7F)
            return tech_tr[ch - 0x21];
        return ch;
    case nss_nrcs_french_canadian2:
        set = nss_nrcs_french_canadian; break;
    case nss_nrcs_finnish2:
        set = nss_nrcs_finnish; break;
    case nss_nrcs_swedish2:
        set = nss_nrcs_swedish; break;
    case nss_nrcs_norwegian_dannish2:
    case nss_nrcs_norwegian_dannish3:
        set = nss_nrcs_norwegian_dannish; break;
    case nss_nrcs_french2:
        set = nss_nrcs_french; break;
    case nss_nrcs_turkish:
        if ((ch & 0x7F) == 0x26) return U'ğ';
    default:;
    }
    if (/* nrcs && */ set <= nss_nrcs_turkish) {
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

