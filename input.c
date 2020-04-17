/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>
#include <string.h>

#include "input.h"
#include "config.h"
#include "term.h"
#include "nrcs.h"

typedef struct nss_esc_reply {
    uint8_t idx;
    uint8_t final;
    uint8_t priv;
    tchar_t init;
    tchar_t param[3];
} nss_reply_t;


static inline _Bool is_edit_keypad(tchar_t ks, _Bool deldel) {
    switch (ks) {
    case XKB_KEY_Delete:
        return !deldel;
    case XKB_KEY_Page_Down:
    case XKB_KEY_Page_Up:
    case XKB_KEY_Insert:
    case XKB_KEY_Select:
    case XKB_KEY_Find:
    case XKB_KEY_DRemove:
        return 1;
    }
    return 0;
}
static inline _Bool is_edit_function(tchar_t ks, _Bool deldel) {
    switch (ks) {
    case XKB_KEY_KP_Insert:
    case XKB_KEY_KP_Delete:
    case XKB_KEY_ISO_Left_Tab:
        return 1;
    }
    return is_edit_keypad(ks, deldel);
}
static inline _Bool is_cursor(tchar_t ks) {
    return XKB_KEY_Home <= ks && ks <= XKB_KEY_Select;
}
static inline _Bool is_keypad(tchar_t ks) {
    return XKB_KEY_KP_Space <= ks && ks <= XKB_KEY_KP_Equal;
}
static inline _Bool is_keypad_function(tchar_t ks) {
    return XKB_KEY_KP_F1 <= ks && ks <= XKB_KEY_KP_F4;
}
static inline _Bool is_function(tchar_t ks) {
    return XKB_KEY_F1 <= ks && ks <= XKB_KEY_F35;
}
static inline _Bool is_misc_function(tchar_t ks) {
    return XKB_KEY_Select <= ks && ks <= XKB_KEY_Break;
}
static inline _Bool is_special(tchar_t ks) {
    return XKB_KEY_ISO_Lock <= ks && ks <= XKB_KEY_Delete;
}
static inline _Bool is_privite(tchar_t ks) {
    return 0x11000000 <= ks && ks <= 0x1100FFFF;
}
static inline _Bool is_ctrl_input(tchar_t ks) {
    return ks >= 0x40 && ks <= 0x7F;
}
static inline _Bool is_ctrl_output(tchar_t ks) {
    return ks < 0x20 || (ks >= 0x7F && ks < 0x100);
}

static inline _Bool is_xkb_ctrl(nss_key_t *k) {
    // Detect if thats something like Ctrl-3
    // which gets translated to ESC by XKB
    return is_ctrl_output(k->utf32);
}

static inline _Bool is_modify_allowed(nss_key_t *k, nss_input_mode_t mode) {
    if (mode.keyboad_vt52) return 0;
    _Bool legacy = mode.keyboard_mapping > 0;
    if (is_cursor(k->sym) || is_edit_function(k->sym, mode.delete_is_del))
        return !legacy || mode.modkey_legacy_allow_edit_keypad;
    else if (is_keypad(k->sym))
        return !legacy || mode.modkey_legacy_allow_keypad;
    else if (is_function(k->sym))
        return !legacy || mode.modkey_legacy_allow_function;
    else if (is_misc_function(k->sym))
        return !legacy || mode.modkey_legacy_allow_misc;
    else return mode.modkey_other;
}



static tchar_t filter_modifiers(nss_key_t *k, nss_input_mode_t mode) {
    tchar_t res = k->mask & (nss_mm_control | nss_mm_shift | nss_mm_mod1);
    if (mode.modkey_other < 2) {
        if (is_ctrl_input(k->sym) && !(res & ~nss_mm_control)) {
            if (!mode.modkey_other) res &= ~nss_mm_control;
        } else if (k->sym == XKB_KEY_Return || k->sym == XKB_KEY_Tab) {
            /* do nothing */;
        } else if (is_xkb_ctrl(k)) {
            if (!(res & nss_mm_mod1)) res = 0;
        } else if (!is_ctrl_output(k->sym) || !is_special(k->sym)) {
            if (!(res & nss_mm_control)) res &= ~nss_mm_shift;
        }
        if (res & nss_mm_mod1) {
            if (!(res & ~nss_mm_mod1) && (mode.meta_escape || k->utf32 < 0x80)) res &= ~nss_mm_mod1;
            if (((is_ctrl_input(k->sym) || is_ctrl_output(k->sym)) && (res & nss_mm_control)))
                res &= ~(nss_mm_mod1 | nss_mm_control);
        }
    }
    return res;
}

static inline tchar_t mask_to_param(tchar_t mask) {
    tchar_t res = 0;
    if (mask & nss_mm_shift) res |= 1;
    if (mask & nss_mm_control) res |= 4;
    if (mask & nss_mm_mod1) res |= 2;
    return res + !!res;
}

static _Bool is_modify_others_allowed(nss_key_t *k, nss_input_mode_t mode) {
    if (!mode.modkey_other || k->is_fkey || is_edit_function(k->sym, mode.delete_is_del) || is_keypad(k->sym) ||
        is_cursor(k->sym) || is_keypad_function(k->sym) || is_misc_function(k->sym) || is_privite(k->sym)) return 0;
    if (!(k->mask & (nss_mm_control | nss_mm_shift | nss_mm_mod1))) return 0;

    if (mode.modkey_other == 1) {
        _Bool res = 0;
        switch (k->sym) {
        case XKB_KEY_BackSpace:
        case XKB_KEY_Delete:
            res = 1;
            break;
        case XKB_KEY_ISO_Left_Tab:
            res =  k->mask & (nss_mm_mod1 | nss_mm_control);
            break;
        case XKB_KEY_Return:
        case XKB_KEY_Tab:
            res = 1;
            break;
        default:
            if (is_ctrl_input(k->sym)) {
                res = k->mask != nss_mm_shift && k->mask != nss_mm_control;
            } else if (is_xkb_ctrl(k)) {
                res = k->mask != nss_mm_shift && (k->mask & (nss_mm_shift | nss_mm_mod1));
            } else res = 1;
        }
        if (res) {
            tchar_t new_mods = filter_modifiers(k, mode);
            if (new_mods) k->mask = new_mods;
            else return 0;
        }
        return res;
    } else {
        switch (k->sym) {
        case XKB_KEY_BackSpace:
            return k->mask & (nss_mm_mod1 | nss_mm_shift);
        case XKB_KEY_Delete:
            return k->mask & (nss_mm_mod1 | nss_mm_shift | nss_mm_control);
        case XKB_KEY_ISO_Left_Tab:
            return k->mask & (nss_mm_mod1 | nss_mm_control);
        case XKB_KEY_Return:
        case XKB_KEY_Tab:
            return 1;
        default:
            if (is_ctrl_input(k->sym)) {
                return 1;
            } else if (k->mask == nss_mm_shift) {
                return k->sym == XKB_KEY_space || k->sym == XKB_KEY_Return;
            } else if (k->mask & (nss_mm_mod1 | nss_mm_control)) {
                return 1;
            } else return 0;
        }
    }
}

static void modify_others(tchar_t ch, tchar_t param, _Bool fmt, nss_reply_t *reply) {
    if (!param) return;
    reply->init = '\233';
    reply->final = fmt ? 'u' : '~';
    reply->idx = 0;
    if (fmt) SWAP(tchar_t, ch, param)
    else reply->param[reply->idx++] = 27;
    reply->param[reply->idx++] = param;
    reply->param[reply->idx++] = ch;
}

static void modify_cursor(tchar_t param, tchar_t level, nss_reply_t *reply) {
    if (!param) return;
    switch(level) {
    case 4: reply->priv = '>';
    case 3: if (!reply->idx) reply->param[reply->idx++] = 1;
    case 2: reply->init = '\233';
    case 1: reply->param[reply->idx++] = param;
    }
}

static tchar_t fnkey_dec(tchar_t ks, _Bool is_fkey, nss_reply_t *reply) {
    if (is_fkey) {
        reply->final = '~';
        tchar_t values[] = {
            11, 12, 13, 14, 15, 17, 18, 19, 20, 21,
            23, 24, 25, 26, 28, 29, 31, 32, 33, 34
        };
        return reply->param[reply->idx++] =
                (XKB_KEY_F1 <= ks && ks <= XKB_KEY_F20) ?
                values[ks - XKB_KEY_F1] : 42 + ks - XKB_KEY_F21;
    } else {

        tchar_t p;
        switch (ks) {
        case XKB_KEY_Find: p = 1; break;
        case XKB_KEY_Insert: p = 2; break;
        case XKB_KEY_Delete: p = 3; break;
        case XKB_KEY_KP_Insert: p = 2; break;
        case XKB_KEY_KP_Delete: p = 3; break;
        case XKB_KEY_DRemove: p = 3; break;
        case XKB_KEY_Select: p = 4; break;
        case XKB_KEY_Prior: p = 5; break;
        case XKB_KEY_Next: p = 6; break;
        case XKB_KEY_ISO_Left_Tab:
            reply->final = 'Z';
            return 'Z';
        case XKB_KEY_Help: p = 28; break;
        case XKB_KEY_Menu: p = 29; break;
        default:
            return 0;
        }
        reply->final = '~';
        return reply->param[reply->idx++] = p;
    }
}

static _Bool fnkey_hp(tchar_t ks, _Bool is_fkey, nss_reply_t *reply) {
    tchar_t res;
    if (is_fkey) {
        tchar_t values[] = { 'p', 'q', 'r', 's', 't', 'u', 'v', 'w' };
        if (XKB_KEY_F1 <= ks && ks <= XKB_KEY_F8) {
            res = values[ks - XKB_KEY_F1];
        } else return 0;
    } else {
        switch (ks) {
        case XKB_KEY_Up: res = 'A'; break;
        case XKB_KEY_Down: res = 'B'; break;
        case XKB_KEY_Right: res = 'C'; break;
        case XKB_KEY_Left: res = 'D'; break;
        case XKB_KEY_End: res = 'F'; break;
        case XKB_KEY_Clear: res = 'J'; break;
        case XKB_KEY_Delete: res = 'P'; break;
        case XKB_KEY_Insert: res = 'Q'; break;
        case XKB_KEY_Next: res = 'S'; break;
        case XKB_KEY_Prior: res = 'T'; break;
        case XKB_KEY_Home: res = 'h'; break;
        case XKB_KEY_KP_Delete: res = 'P'; break;
        case XKB_KEY_KP_Insert: res = 'Q'; break;
        case XKB_KEY_DRemove: res = 'P'; break;
        case XKB_KEY_Select: res = 'F'; break;
        case XKB_KEY_Find: res = 'h'; break;
        default:
            return 0;
        }
    }
    reply->init = '\233';
    reply->final = res;
    return 1;
}

static _Bool fnkey_sco(tchar_t ks, _Bool is_fkey, nss_reply_t *reply) {
    tchar_t res;
    if (is_fkey) {
        if (ks - XKB_KEY_F1 < 48)
            res = "MNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz@[\\]^_`{"[ks - XKB_KEY_F1];
        else return 0;
    } else {
        switch (ks) {
        case XKB_KEY_Up: res = 'A'; break;
        case XKB_KEY_Down: res = 'B'; break;
        case XKB_KEY_Right: res = 'C'; break;
        case XKB_KEY_Left: res = 'D'; break;
        case XKB_KEY_Begin: res = 'E'; break;
        case XKB_KEY_End: res = 'F'; break;
        case XKB_KEY_Insert: res = 'L'; break;
        case XKB_KEY_Next: res = 'G'; break;
        case XKB_KEY_Prior: res = 'I'; break;
        case XKB_KEY_Home: res = 'H'; break;
        case XKB_KEY_KP_Insert: res = 'L'; break;
        default:
            return 0;
        }
    }
    reply->init = '\233';
    reply->final = res;
    return 1;
}

static _Bool fnkey_sun(tchar_t ks, _Bool is_fkey, nss_reply_t *reply) {
    tchar_t arg = 0, fin = 0;
    if (is_fkey) {
        if (ks - XKB_KEY_F1 < 37) {
            arg = (int32_t[]) {
                224, 225, 226, 227, 228, 229, 230, 231, 232, 233,
                192, 193, 194, 195, 196, 197, 198, 199, 200, 201,
                208, 209, 210, 211, 212, 213, 214, 215, 216, 217,
                218, 219, 220, 221, 222, 234, 235 }[ks - XKB_KEY_F1];
        } else return 0;
    } else {
        switch (ks) {
        case XKB_KEY_Help: arg = 196; break;
        case XKB_KEY_Menu: arg = 197; break;
        case XKB_KEY_Find: arg = 1; break;
        case XKB_KEY_Insert: arg = 2; break;
        case XKB_KEY_Delete: arg = 3; break;
        case XKB_KEY_KP_Insert: arg = 2; break;
        case XKB_KEY_KP_Delete: arg = 3; break;
        case XKB_KEY_DRemove: arg = 3; break;
        case XKB_KEY_Select: arg = 4; break;
        case XKB_KEY_Prior: arg = 216; break;
        case XKB_KEY_Next: arg = 222; break;
        case XKB_KEY_Home: arg = 214; break;
        case XKB_KEY_End: arg = 220; break;
        case XKB_KEY_Begin: arg = 218; break;
        default:
            if (is_cursor(ks)) fin = "HDACB  FE"[ks - XKB_KEY_Home];
            else return 0;
        }
    }
    reply->final = fin ? fin : 'z';
    reply->init = fin ? '\217' : '\233';
    if (arg) reply->param[reply->idx++] = arg;
    return 1;
}

static void translate_adjust(nss_key_t *k, nss_input_mode_t *mode) {
    if (k->utf8len <= 1 && !is_special(k->sym) && mode->modkey_other > 1 && !is_ctrl_input(k->sym))
        k->utf8len = 1, k->utf8data[0] = (uint8_t)k->sym;

    k->is_fkey = is_function(k->sym);

    if (mode->keyboard_mapping == nss_km_vt220) {
        if (!(k->mask & nss_mm_shift)) {
            if (k->sym == XKB_KEY_KP_Add) k->sym = XKB_KEY_KP_Separator;
            if (k->mask & nss_mm_control && k->sym == XKB_KEY_KP_Separator) {
                k->sym = XKB_KEY_KP_Subtract;
                k->mask &= ~nss_mm_control;
            }

            mode->appkey &= (k->utf8len == 1 && mode->allow_numlock && (k->mask & nss_mm_mod2));
        }
        if (k->sym != XKB_KEY_Delete || !mode->delete_is_del) {
            struct { tchar_t from, to; } tab[] = {
                { XKB_KEY_Delete, XKB_KEY_DRemove },
                { XKB_KEY_Home, XKB_KEY_Find },
                { XKB_KEY_End, XKB_KEY_Select },
                { XKB_KEY_KP_Delete, XKB_KEY_KP_Decimal },
                { XKB_KEY_KP_Insert, XKB_KEY_KP_0 },
                { XKB_KEY_KP_End, XKB_KEY_KP_1 },
                { XKB_KEY_KP_Down, XKB_KEY_KP_2 },
                { XKB_KEY_KP_Next, XKB_KEY_KP_3 },
                { XKB_KEY_KP_Left, XKB_KEY_KP_4 },
                { XKB_KEY_KP_Begin, XKB_KEY_KP_5 },
                { XKB_KEY_KP_Right, XKB_KEY_KP_6 },
                { XKB_KEY_KP_Home, XKB_KEY_KP_7 },
                { XKB_KEY_KP_Up, XKB_KEY_KP_8 },
                { XKB_KEY_KP_Prior, XKB_KEY_KP_9 },
            };
            for (size_t i = 0; i < sizeof(tab)/sizeof(tab[0]); i++) {
                if (tab[i].from == k->sym) {
                    k->sym = tab[i].to;
                    break;
                }
            }
        }
    }

    if (k->sym == XKB_KEY_Tab || k->sym == XKB_KEY_ISO_Left_Tab) {
        if (mode->modkey_other > 1) {
            if (!k->utf8len) k->utf8data[k->utf8len++] = '\t';
        } else if (k->utf8len < 2 && k->mask == nss_mm_shift)
            k->sym = XKB_KEY_ISO_Left_Tab;
    }

    if (k->utf8len == 1 && k->sym == XKB_KEY_BackSpace &&
            (mode->backspace_is_del ^ !!(k->mask & nss_mm_control)))
        k->utf8data[0] = '\177', k->mask &= ~nss_mm_control;

    if (XKB_KEY_KP_Home <= k->sym && k->sym <= XKB_KEY_KP_Begin)
        k->sym += XKB_KEY_Home - XKB_KEY_KP_Home;

    if (!k->is_fkey) {
        if (k->sym == XKB_KEY_SunF36)
            k->is_fkey = 1, k->sym = XKB_KEY_F1 + 36 - 1;
        else if (k->sym == XKB_KEY_SunF37)
            k->is_fkey = 1, k->sym = XKB_KEY_F1 + 37 - 1;
    }

    if (k->is_fkey && k->mask & (nss_mm_control | nss_mm_shift)) {
        if (mode->keyboard_mapping == nss_km_vt220 || mode->keyboard_mapping == nss_km_legacy) {
            if (k->mask & nss_mm_control)
                k->sym += mode->fkey_inc_step;
            k->mask &= ~nss_mm_control;
        } else if (!mode->modkey_fn) {
            if (k->mask & nss_mm_control)
                k->sym += mode->fkey_inc_step * 2;
            if (k->mask & nss_mm_shift)
                k->sym += mode->fkey_inc_step;
            k->mask &= ~(nss_mm_control | nss_mm_shift);
        }
    }
}

static void dump_reply(nss_term_t *term, nss_reply_t *reply) {
    uint8_t str[128] = { 0 };
    size_t strp = 0;
    if (!reply->init || !reply->final) {
        warn("Tried to dump empty escape");
        return;
    }
    str[strp++] = reply->init;
    if (reply->priv) str[strp++] = reply->priv;
    for (size_t i = 0; i < reply->idx; i++) {
        if (i) str[strp++] = ';';
        size_t j = strp;
        do str[strp++] = '0' + reply->param[i] % 10;
        while (reply->param[i] /= 10);
        for (size_t k = 0; k < (strp - j)/2; k++)
            SWAP(uint8_t, str[j + k], str[strp - k - 1]);
    }
    str[strp++] = reply->final;
    str[strp] = '\0';
    nss_term_sendkey(term, str, 0);
}

void nss_handle_input(nss_key_t k, nss_term_t *term) {
    nss_input_mode_t mode = *nss_term_inmode(term);

    if (mode.keylock) return;

    translate_adjust(&k, &mode);

    nss_reply_t reply = { 0 };
    tchar_t param = 0;
    if (k.mask && is_modify_allowed(&k, mode))
        param = mask_to_param(k.mask);

    switch (mode.keyboard_mapping) {
    case nss_km_hp: fnkey_hp(k.sym, k.is_fkey, &reply); break;
    case nss_km_sun: fnkey_sun(k.sym, k.is_fkey, &reply); break;
    case nss_km_sco: fnkey_sco(k.sym, k.is_fkey, &reply); break;
    default: break;
    }


    if (reply.final) { // Applied in one of fnkey_* functions
        modify_cursor(param, (k.is_fkey || is_misc_function(k.sym) || is_edit_function(k.sym, mode.delete_is_del)) ?
                mode.modkey_fn : mode.modkey_cursor, &reply);
        dump_reply(term, &reply);
    } else if (k.is_fkey || is_misc_function(k.sym) || is_edit_function(k.sym, mode.delete_is_del)) {
        tchar_t deccode = fnkey_dec(k.sym, k.is_fkey, &reply);
        if (k.mask & nss_mm_shift && mode.keyboard_mapping == nss_km_vt220) {
            /* TODO UDK Here. For now -- nothing */
            return;
        } else if (mode.keyboard_mapping != nss_km_legacy && deccode - 11 <= 3) {
            reply.init = mode.keyboad_vt52 ? '\033' : '\217';
            reply.final = deccode - 11 + 'P';
            reply.idx = 0;
            modify_cursor(param, mode.modkey_cursor, &reply);
        } else {
            reply.init = '\233';
            if (k.sym == XKB_KEY_ISO_Left_Tab) {
                if (mode.modkey_other >= 2 && (k.mask & (nss_mm_control | nss_mm_mod1)))
                    modify_others('\t', param, mode.modkey_other_fmt, &reply);
            } else {
                if (k.is_fkey) modify_cursor(param, mode.modkey_fn, &reply);
                else if (param) reply.param[reply.idx++] = param;
            }
        }
        dump_reply(term, &reply);
    } else if (is_keypad_function(k.sym)) {
        reply.init = mode.keyboad_vt52 ? '\033' : '\217';
        reply.final = k.sym - XKB_KEY_KP_F1 + 'P';
        modify_cursor(param, mode.modkey_keypad, &reply);
        dump_reply(term, &reply);
    } else if (is_keypad(k.sym)) {
        if (mode.appkey) {
            reply.init = mode.keyboad_vt52 ? '\033' : '\217';
            reply.final = " ABCDEFGHIJKLMNOPQRSTUVWXYZ??????abcdefghijklmnopqrstuvwxyzXXX" [k.sym - XKB_KEY_KP_Space];
            modify_cursor(param, mode.modkey_keypad, &reply);
            if(mode.keyboad_vt52) reply.priv = '?';
            dump_reply(term, &reply);
        } else {
            uint8_t ch = " XXXXXXXX\tXXX\rXXXxxxxXXXXXXXXXXXXXXXXXXXXX*+,-./0123456789XXX=" [k.sym - XKB_KEY_KP_Space];
            nss_term_sendkey(term, &ch, 1);
        }
    } else if (is_cursor(k.sym)) {
        reply.init = mode.keyboad_vt52 ? '\033' : mode.appcursor ? '\217' : '\233';
        reply.final = "HDACB  FE"[k.sym - XKB_KEY_Home];
        modify_cursor(param, mode.modkey_cursor, &reply);
        dump_reply(term, &reply);
    } else if (k.utf8len > 0) {
        if (is_modify_others_allowed(&k, mode)) {
            // This is done in order to override XKB control transformation
            // TODO Check if k.ascii can be used here
            tchar_t val = k.sym < 0x100 ? k.sym : k.utf32;
            modify_others(val, mask_to_param(k.mask), mode.modkey_other_fmt, &reply);
            dump_reply(term, &reply);
        } else {
            if (nss_term_is_utf8(term)) {
                if ((k.mask & nss_mm_mod1) && mode.has_meta) {
                    if (!mode.meta_escape) {
                        if (k.utf32 < 0x80)
                            k.utf8len = utf8_encode(k.utf32 | 0x80, k.utf8data,
                                    k.utf8data + sizeof(k.utf8data)/sizeof(k.utf8data[0]));
                    } else {
                        memmove(k.utf8data + 1, k.utf8data, k.utf8len);
                        k.utf8len++;
                        k.utf8data[0] = '\033';
                    }
                }
            } else {
                if (nss_term_is_nrcs_enabled(term))
                    nrcs_encode(nss_config_integer(NSS_ICONFIG_KEYBOARD_NRCS), &k.utf32, 1);

                if (k.utf32 > 0xFF) return;

                k.utf8len = 1;
                k.utf8data[0] = k.utf32;
                if (k.mask & nss_mm_mod1 && mode.has_meta) {
                    if (!mode.meta_escape) {
                        k.utf8data[0] |= 0x80;
                    } else {
                        k.utf8data[0] = '\033';
                        k.utf8data[k.utf8len++] = k.utf32;
                    }
                }
            }
            k.utf8data[k.utf8len] = '\0';
            nss_term_sendkey(term, k.utf8data, k.utf8len);
        }
    }
}

/* And now I should duplicate some code of xkbcommon
 * in order to be able to distunguish NUL symbol and error condition...
 */

/* Verbatim from libX11:src/xkb/XKBBind.c */
static char to_control(char ch) {
    if ((ch >= '@' && ch < '\177') || ch == ' ') ch &= 0x1F;
    else if (ch == '2') ch = '\000';
    else if (ch >= '3' && ch <= '7') ch -= ('3' - '\033');
    else if (ch == '8') ch = '\177';
    else if (ch == '/') ch = '_' & 0x1F;
    return ch;
}

nss_key_t nss_describe_key(struct xkb_state *state, xkb_keycode_t keycode) {
    nss_key_t k = { .sym = XKB_KEY_NoSymbol };

    k.mask = xkb_state_serialize_mods(state, XKB_STATE_MODS_EFFECTIVE);
    uint32_t consumed = xkb_state_key_get_consumed_mods(state, keycode);

    struct xkb_keymap *keymap = xkb_state_get_keymap(state);
    xkb_layout_index_t layout = xkb_state_key_get_layout(state, keycode);
    xkb_layout_index_t num_layouts = xkb_keymap_num_layouts_for_key(keymap, keycode);
    xkb_level_index_t level = xkb_state_key_get_level(state, keycode, layout);
    if (layout == XKB_LAYOUT_INVALID || !num_layouts || level == XKB_LEVEL_INVALID)
        return k;

    const xkb_keysym_t *syms;
    int nsyms = xkb_keymap_key_get_syms_by_level(keymap, keycode, layout, level, &syms);
    if (nsyms != 1) return k;

    k.ascii = k.sym = *syms;

    if (k.mask && k.sym >= 0x80) {
        for (xkb_layout_index_t i = 0; i < num_layouts; i++) {
            if ((level = xkb_state_key_get_level(state, keycode, i)) != XKB_LEVEL_INVALID) {
                nsyms = xkb_keymap_key_get_syms_by_level(keymap, keycode, i, level, &syms);
                if (nsyms == 1 && syms[0] < 0x80) {
                    k.ascii = syms[0];
                    break;
                }
            }
        }
    }

    if (k.mask & ~consumed & nss_mm_lock) k.sym = xkb_keysym_to_upper(k.sym);

    if ((k.utf32 = xkb_keysym_to_utf32(k.sym))) {
        if ((k.mask & ~consumed & nss_mm_control) && k.sym < 0x80)
            k.utf32 = to_control(k.ascii);
        k.utf8len = utf8_encode(k.utf32, k.utf8data, k.utf8data + sizeof(k.utf8data));
        k.utf8data[k.utf8len] = '\0';
    }

    return k;
}

