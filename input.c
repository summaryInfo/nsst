/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#include "feature.h"

#include "config.h"
#include "input.h"
#include "nrcs.h"
#include "term.h"

#include <ctype.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

typedef struct nss_esc_reply {
    uint8_t idx;
    uint8_t final;
    uint8_t priv;
    nss_char_t init;
    nss_char_t param[3];
} nss_reply_t;

struct nss_shortcut {
    uint32_t ksym;
    uint32_t mask;
} cshorts[nss_sa_MAX] = {
    [nss_sa_scroll_down] = {XKB_KEY_Up, nss_mm_shift | nss_mm_control},
    [nss_sa_scroll_up] = {XKB_KEY_Down, nss_mm_shift | nss_mm_control},
    [nss_sa_font_up] = {XKB_KEY_Page_Up, nss_mm_shift | nss_mm_control},
    [nss_sa_font_down] = {XKB_KEY_Page_Down, nss_mm_shift | nss_mm_control},
    [nss_sa_font_default] = {XKB_KEY_Home, nss_mm_shift | nss_mm_control},
    [nss_sa_new_window] = {XKB_KEY_N, nss_mm_shift | nss_mm_control},
    [nss_sa_numlock] = {XKB_KEY_Num_Lock, nss_mm_shift | nss_mm_control},
    [nss_sa_copy] = {XKB_KEY_C, nss_mm_shift | nss_mm_control},
    [nss_sa_paste] = {XKB_KEY_V, nss_mm_shift | nss_mm_control},
    [nss_sa_break] = {XKB_KEY_Break, 0},
    [nss_sa_reset] = {XKB_KEY_R, nss_mm_shift | nss_mm_control},
    [nss_sa_reload_config] = {XKB_KEY_X, nss_mm_shift | nss_mm_control},
};

static inline _Bool is_edit_keypad(nss_char_t ks, _Bool deldel) {
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

static inline _Bool is_edit_function(nss_char_t ks, _Bool deldel) {
    switch (ks) {
    case XKB_KEY_KP_Insert:
    case XKB_KEY_KP_Delete:
    case XKB_KEY_ISO_Left_Tab:
        return 1;
    }
    return is_edit_keypad(ks, deldel);
}

static inline _Bool is_cursor(nss_char_t ks) {
    return XKB_KEY_Home <= ks && ks <= XKB_KEY_Select;
}

static inline _Bool is_keypad(nss_char_t ks) {
    return XKB_KEY_KP_Space <= ks && ks <= XKB_KEY_KP_Equal;
}

static inline _Bool is_keypad_function(nss_char_t ks) {
    return XKB_KEY_KP_F1 <= ks && ks <= XKB_KEY_KP_F4;
}

static inline _Bool is_function(nss_char_t ks) {
    return XKB_KEY_F1 <= ks && ks <= XKB_KEY_F35;
}

static inline _Bool is_misc_function(nss_char_t ks) {
    return XKB_KEY_Select <= ks && ks <= XKB_KEY_Break;
}

static inline _Bool is_special(nss_char_t ks) {
    return XKB_KEY_ISO_Lock <= ks && ks <= XKB_KEY_Delete;
}

static inline _Bool is_private(nss_char_t ks) {
    return 0x11000000 <= ks && ks <= 0x1100FFFF;
}

static inline _Bool is_ctrl_letter(nss_char_t ks) {
    return ks >= 0x40 && ks <= 0x7F;
}

static inline _Bool is_ctrl(nss_char_t ks) {
    return ks < 0x20 || (ks >= 0x7F && ks < 0x100);
}

static inline _Bool is_xkb_ctrl(struct key *k) {
    // Detect if thats something like Ctrl-3
    // which gets translated to ESC by XKB
    return is_ctrl(k->utf32);
}

static inline _Bool is_modify_allowed(struct key *k, struct keyboard_state *mode) {
    if (mode->keyboad_vt52) return 0;
    _Bool legacy = mode->keyboard_mapping > 0;
    if (is_cursor(k->sym) || is_edit_function(k->sym, mode->delete_is_del))
        return !legacy || mode->modkey_legacy_allow_edit_keypad;
    else if (is_keypad(k->sym))
        return !legacy || mode->modkey_legacy_allow_keypad;
    else if (is_function(k->sym))
        return !legacy || mode->modkey_legacy_allow_function;
    else if (is_misc_function(k->sym))
        return !legacy || mode->modkey_legacy_allow_misc;
    else return mode->modkey_other;
}

static nss_char_t filter_modifiers(struct key *k, struct keyboard_state *mode) {
    nss_char_t res = k->mask & (nss_mm_control | nss_mm_shift | nss_mm_mod1);

    if (mode->modkey_other <= 1) {
        if (is_ctrl_letter(k->sym) && !(res & ~nss_mm_control)) {
            if (!mode->modkey_other) res &= ~nss_mm_control;
        } else if (k->sym == XKB_KEY_Return || k->sym == XKB_KEY_Tab) {
            /* do nothing */;
        } else if (is_xkb_ctrl(k)) {
            if (!(res & nss_mm_mod1)) res = 0;
        } else if (!is_ctrl(k->sym) || !is_special(k->sym)) {
            if (!(res & nss_mm_control)) res &= ~nss_mm_shift;
        }
        if (res & nss_mm_mod1) {
            if (!(res & ~nss_mm_mod1) && (mode->meta_escape || k->utf32 < 0x80)) res &= ~nss_mm_mod1;
            if (((is_ctrl_letter(k->sym) || is_ctrl(k->sym)) && (res & nss_mm_control)))
                res &= ~(nss_mm_mod1 | nss_mm_control);
            if (k->sym == XKB_KEY_Return || k->sym == XKB_KEY_Tab)
                res &= ~(nss_mm_mod1 | nss_mm_control);
        }
    }
    return res;
}

static inline nss_char_t mask_to_param(nss_char_t mask) {
    nss_char_t res = 0;
    if (mask & nss_mm_shift) res |= 1;
    if (mask & nss_mm_control) res |= 4;
    if (mask & nss_mm_mod1) res |= 2;
    return res + !!res;
}

static _Bool is_modify_others_allowed(struct key *k, struct keyboard_state *mode) {
    if (!mode->modkey_other || is_private(k->sym)) return 0;
    if (!(k->mask & (nss_mm_control | nss_mm_shift | nss_mm_mod1))) return 0;

    if (mode->modkey_other == 1) {
        _Bool res = 0;
        switch (k->sym) {
        case XKB_KEY_BackSpace:
        case XKB_KEY_Delete:
            break;
        case XKB_KEY_ISO_Left_Tab:
            res =  k->mask & (nss_mm_mod1 | nss_mm_control);
            break;
        case XKB_KEY_Return:
        case XKB_KEY_Tab:
            res = 1;
            break;
        default:
            if (is_ctrl_letter(k->sym))
                res = k->mask != nss_mm_shift && k->mask != nss_mm_control;
            else if (is_xkb_ctrl(k))
                res = k->mask != nss_mm_shift && (k->mask & (nss_mm_shift | nss_mm_mod1));
            else res = 1;
        }
        if (res) {
            nss_char_t new_mods = filter_modifiers(k, mode);
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
            if (is_ctrl_letter(k->sym))
                return 1;
            else if (k->mask == nss_mm_shift)
                return k->sym == XKB_KEY_space || k->sym == XKB_KEY_Return;
            else if (k->mask & (nss_mm_mod1 | nss_mm_control))
                return 1;
            return 0;
        }
    }
}

static void modify_others(nss_char_t ch, nss_char_t param, _Bool fmt, nss_reply_t *reply) {
    if (!param) return;

    if (fmt) *reply = (nss_reply_t) { 2, 'u', 0, '\233', {ch, param} };
    else *reply = (nss_reply_t) { 3, '~', 0, '\233', {27, param, ch} };
}

static void modify_cursor(nss_char_t param, uint8_t level, nss_reply_t *reply) {
    if (param) switch (level) {
    case 4: reply->priv = '>';
    case 3: if (!reply->idx)
                reply->param[reply->idx++] = 1;
    case 2: reply->init = '\233';
    case 1: reply->param[reply->idx++] = param;
    }
}

static nss_char_t fnkey_dec(nss_char_t ks, _Bool is_fkey, nss_reply_t *reply) {
    if (is_fkey) {
        reply->final = '~';
        uint8_t values[] = {
            11, 12, 13, 14, 15, 17, 18, 19, 20, 21,
            23, 24, 25, 26, 28, 29, 31, 32, 33, 34
        };
        return reply->param[reply->idx++] =
                (XKB_KEY_F1 <= ks && ks <= XKB_KEY_F20) ?
                values[ks - XKB_KEY_F1] : 42 + ks - XKB_KEY_F21;
    } else {
        nss_char_t p;
        switch (ks) {
        case XKB_KEY_Find: p = 1; break;
        case XKB_KEY_Insert:
        case XKB_KEY_KP_Insert: p = 2; break;
        case XKB_KEY_Delete:
        case XKB_KEY_KP_Delete:
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

static _Bool fnkey_hp(nss_char_t ks, _Bool is_fkey, nss_reply_t *reply) {
    nss_char_t res;
    if (is_fkey) {
        if (XKB_KEY_F1 <= ks && ks <= XKB_KEY_F8)
            res = "pqrstuvw"[ks - XKB_KEY_F1];
        else return 0;
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

static _Bool fnkey_sco(nss_char_t ks, _Bool is_fkey, nss_reply_t *reply) {
    nss_char_t res;
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

static _Bool fnkey_sun(nss_char_t ks, _Bool is_fkey, nss_reply_t *reply) {
    nss_char_t arg = 0, fin = 0;
    if (is_fkey) {
        if (ks - XKB_KEY_F1 < 37) {
            arg = (uint8_t[]) {
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

inline static uint32_t translate_keypad(uint32_t in) {
    struct { nss_char_t from, to; } tab[] = {
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
    for (size_t i = 0; i < sizeof(tab)/sizeof(tab[0]); i++)
        if (tab[i].from == in) return tab[i].to;
    return in;
}

static void dump_reply(nss_term_t *term, nss_reply_t *reply) {
    uint8_t str[128] = { 0 };
    size_t strp = 0;
    if (!reply->init || !reply->final) {
        warn("Attempted to dump empty escape");
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

    if (iconf(ICONF_TRACE_INPUT)) {
        char pre[4] = {'^'};
        if (reply->init < 0x80) pre[1] = reply->init ^ 0x40;
        else pre[1] = '[', pre[2] = reply->init ^ 0xC0;
        info("Key seq: %s%s", pre, str + 1);
    }
}


static void translate_adjust(struct key *k, struct keyboard_state *mode) {
    if (k->utf8len <= 1 && !is_special(k->sym) && mode->modkey_other > 1 && !is_ctrl_letter(k->sym))
        k->utf8len = 1, k->utf8data[0] = (uint8_t)k->sym;

    k->is_fkey = is_function(k->sym);

    if (mode->keyboard_mapping == keymap_vt220) {
        if (!(k->mask & nss_mm_shift)) {
            if (k->sym == XKB_KEY_KP_Add) {
                k->sym = XKB_KEY_KP_Separator;
            } else if (k->sym == XKB_KEY_KP_Separator && k->mask & nss_mm_control) {
                k->sym = XKB_KEY_KP_Subtract;
                k->mask &= ~nss_mm_control;
            }
        }
        if (k->sym != XKB_KEY_Delete || !mode->delete_is_del)
            k->sym = translate_keypad(k->sym);
    }

    mode->appkey &= (k->utf8len == 1 && mode->allow_numlock && (k->mask & nss_mm_mod2));

    if (k->sym == XKB_KEY_Tab || k->sym == XKB_KEY_ISO_Left_Tab) {
        if (mode->modkey_other > 1) {
            if (!k->utf8len) k->utf8data[k->utf8len++] = '\t';
        } else if (k->utf8len < 2 && k->mask == nss_mm_shift) {
            k->sym = XKB_KEY_ISO_Left_Tab;
        }
    } else if (XKB_KEY_KP_Home <= k->sym && k->sym <= XKB_KEY_KP_Begin) {
        k->sym += XKB_KEY_Home - XKB_KEY_KP_Home;
    } else if (k->sym == XKB_KEY_SunF36) {
        k->is_fkey = 1, k->sym = XKB_KEY_F1 + 36 - 1;
    } else if (k->sym == XKB_KEY_SunF37) {
        k->is_fkey = 1, k->sym = XKB_KEY_F1 + 37 - 1;
    } else if (k->sym == XKB_KEY_BackSpace) {
        if (k->utf8len == 1 && (mode->backspace_is_del ^ !!(k->mask & nss_mm_control))) {
            k->utf8data[0] = '\177';
            k->mask &= ~nss_mm_control;
            // k->sym = XKB_KEY_Delete;
        }
    }

    if (k->is_fkey && k->mask & (nss_mm_control | nss_mm_shift)) {
        if (mode->keyboard_mapping == keymap_vt220 || mode->keyboard_mapping == keymap_legacy) {
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

void nss_input_reset_udk(nss_term_t *term) {
    struct keyboard_state *mode = nss_term_keyboard_state(term);
    for (size_t i = 0; i < UDK_MAX;  i++) {
        free(mode->udk[i].val);
        mode->udk[i].val = NULL;
        mode->udk[i].len = 0;
    }
}

_Bool nss_input_set_udk(nss_term_t *term, const uint8_t *str, const uint8_t *end, _Bool reset, _Bool lock) {
    struct keyboard_state *mode = nss_term_keyboard_state(term);
    if (!mode->udk_locked) {
        if (reset) nss_input_reset_udk(term);
        mode->udk_locked = lock;
        for (; str < end; str++) {
            nss_param_t k = 0;
            while (isdigit(*str) && *str != '/')
                k = 10 * k + *str - '0', str++;
            if (*str++ != '/' || k >= UDK_MAX) return 0;
            const uint8_t *sem = memchr(str, ';', end - str);
            if (!sem) sem = end;
            uint8_t *udk = malloc((sem - str + 1)/2 + 1);
            if (!udk || (hex_decode(udk, str, sem) != sem)) {
                free(udk);
                return 0;
            }
            free(mode->udk[k].val);
            mode->udk[k].len = (sem - str)/2;
            mode->udk[k].val = udk;
            str = sem;
        }
    }
    return 1;
}

void nss_handle_input(struct key k, nss_term_t *term) {
    struct keyboard_state *mode = nss_term_keyboard_state(term);

    if (iconf(ICONF_TRACE_INPUT)) {
        info("Key: sym=0x%X mask=0x%X ascii=0x%X utf32=0x%X",
                k.sym, k.mask, k.ascii, k.utf32);
    }

    if (mode->keylock) return;

    // Appkey can be modified during adjustment
    _Bool saved_appkey = mode->appkey;

    translate_adjust(&k, mode);

    nss_reply_t reply = { 0 };
    nss_char_t param = k.mask && is_modify_allowed(&k, mode) ? mask_to_param(k.mask) : 0;


    _Bool (*kfn[keymap_MAX])(nss_char_t, _Bool, nss_reply_t *) = {
        [keymap_hp] = fnkey_hp,
        [keymap_sun] = fnkey_sun,
        [keymap_sco] = fnkey_sco
    };
    if (kfn[mode->keyboard_mapping]) kfn[mode->keyboard_mapping](k.sym, k.is_fkey, &reply);

    if (reply.final) { // Applied in one of fnkey_* functions
        modify_cursor(param, (k.is_fkey || is_misc_function(k.sym) || is_edit_function(k.sym, mode->delete_is_del)) ?
                mode->modkey_fn : mode->modkey_cursor, &reply);
        dump_reply(term, &reply);
    } else if (k.is_fkey || is_misc_function(k.sym) || is_edit_function(k.sym, mode->delete_is_del)) {
        nss_char_t deccode = fnkey_dec(k.sym, k.is_fkey, &reply);
        if (k.is_fkey && k.mask & nss_mm_shift && mode->keyboard_mapping == keymap_vt220) {
            struct udk udk = reply.param[0] < UDK_MAX ? mode->udk[reply.param[0]] : (struct udk){0};
            if (udk.val) {
                if (iconf(ICONF_TRACE_INPUT))
                    info("Key str: '%s' ", udk.val);
                nss_term_sendkey(term, udk.val, udk.len);
            }
        } else if (mode->keyboard_mapping != keymap_legacy && deccode - 11 <= 3) {
            reply.init = mode->keyboad_vt52 ? '\033' : '\217';
            reply.final = deccode - 11 + 'P';
            reply.idx = 0;
            modify_cursor(param, mode->modkey_cursor, &reply);
            dump_reply(term, &reply);
        } else {
            reply.init = '\233';
            if (k.sym == XKB_KEY_ISO_Left_Tab) {
                if (mode->modkey_other >= 2 && (k.mask & (nss_mm_control | nss_mm_mod1)))
                    modify_others('\t', param, mode->modkey_other_fmt, &reply);
            } else {
                if (k.is_fkey) modify_cursor(param, mode->modkey_fn, &reply);
                else if (param) reply.param[reply.idx++] = param;
            }
            dump_reply(term, &reply);
        }
    } else if (is_keypad_function(k.sym)) {
        reply.init = mode->keyboad_vt52 ? '\033' : '\217';
        reply.final = k.sym - XKB_KEY_KP_F1 + 'P';
        modify_cursor(param, mode->modkey_keypad, &reply);
        dump_reply(term, &reply);
    } else if (is_keypad(k.sym)) {
        if (mode->appkey) {
            reply.init = mode->keyboad_vt52 ? '\033' : '\217';
            reply.final = " ABCDEFGHIJKLMNOPQRSTUVWXYZ??????"
                    "abcdefghijklmnopqrstuvwxyzXXX" [k.sym - XKB_KEY_KP_Space];
            modify_cursor(param, mode->modkey_keypad, &reply);
            if (mode->keyboad_vt52) reply.priv = '?';
            dump_reply(term, &reply);
        } else {
            uint8_t ch = " XXXXXXXX\tXXX\rXXXxxxxXXXXXXXXXX"
                    "XXXXXXXXXXX*+,-./0123456789XXX=" [k.sym - XKB_KEY_KP_Space];
            if (iconf(ICONF_TRACE_INPUT))
                info("Key char: (%x) '%c' ", ch, ch);
            nss_term_sendkey(term, &ch, 1);
        }
    } else if (is_cursor(k.sym)) {
        reply.init = mode->keyboad_vt52 ? '\033' : mode->appcursor ? '\217' : '\233';
        reply.final = "HDACB  FE"[k.sym - XKB_KEY_Home];
        modify_cursor(param, mode->modkey_cursor, &reply);
        dump_reply(term, &reply);
    } else if (k.utf8len > 0) {
        if (is_modify_others_allowed(&k, mode)) {
            /* Is it OK to use k.ascii here?
             * It allows to identify key in layout independent fasion
             * nss_char_t val = k.ascii ? k.ascii : k.sym; */
            nss_char_t val = k.sym < 0x100 ? k.sym : k.utf32;
            modify_others(val, mask_to_param(k.mask), mode->modkey_other_fmt, &reply);
            dump_reply(term, &reply);
        } else {
            if (nss_term_is_utf8(term)) {
                if ((k.mask & nss_mm_mod1) && mode->has_meta) {
                    if (!mode->meta_escape) {
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
                    nrcs_encode(iconf(ICONF_KEYBOARD_NRCS), &k.utf32, 1);

                if (k.utf32 > 0xFF) {
                    mode->appkey = saved_appkey;
                    return;
                }

                k.utf8len = 1;
                k.utf8data[0] = k.utf32;
                if (k.mask & nss_mm_mod1 && mode->has_meta) {
                    if (!mode->meta_escape) {
                        k.utf8data[0] |= 0x80;
                    } else {
                        k.utf8data[0] = '\033';
                        k.utf8data[k.utf8len++] = k.utf32;
                    }
                }
            }
            k.utf8data[k.utf8len] = '\0';
            if (iconf(ICONF_TRACE_INPUT))
                info("Key char: (%x) '%s' ", k.utf32, k.utf8data);
            nss_term_sendkey(term, k.utf8data, k.utf8len);
        }
    }

    mode->appkey = saved_appkey;
}

/* And now I should duplicate some code of xkbcommon
 * in order to be able to distunguish NUL symbol and error condition...
 */

static char to_control(char ch) {
    if ((ch >= '@' && ch < '\177') || ch == ' ') ch &= 0x1F;
    else if (ch == '2') ch = '\000';
    else if (ch >= '3' && ch <= '7') ch -= ('3' - '\033');
    else if (ch == '8') ch = '\177';
    else if (ch == '/') ch = '_' & 0x1F;
    return ch;
}

struct key nss_describe_key(struct xkb_state *state, xkb_keycode_t keycode) {
    struct key k = { .sym = XKB_KEY_NoSymbol };

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

    k.sym = *syms;

    if (k.mask && k.sym >= 0x80) {
        for (xkb_layout_index_t i = 0; !k.ascii && i < num_layouts; i++)
            if ((level = xkb_state_key_get_level(state, keycode, i)) != XKB_LEVEL_INVALID)
                if (1 == xkb_keymap_key_get_syms_by_level(keymap, keycode, i, level, &syms) && syms[0] < 0x80)
                    k.ascii = syms[0];
    } else k.ascii = k.sym;

    if (k.mask & ~consumed & nss_mm_lock) k.sym = xkb_keysym_to_upper(k.sym);

    if ((k.utf32 = xkb_keysym_to_utf32(k.sym))) {
        if ((k.mask & ~consumed & nss_mm_control) && k.ascii) k.utf32 = to_control(k.ascii);
        k.utf8len = utf8_encode(k.utf32, k.utf8data, k.utf8data + sizeof(k.utf8data));
        k.utf8data[k.utf8len] = '\0';
    }

    return k;
}

static uint32_t decode_mask(const char *c, const char *end) {
    uint32_t mask = 0;
    _Bool recur = 0, has_t = 0;

again:
    while (c < end) switch (*c++) {
    case 'T':
        if (recur) mask |= nss_mm_shift | nss_mm_control;
        else has_t = 1;
        break;
    case 'S':
        mask |= nss_mm_shift;
        break;
    case 'C':
        mask |= nss_mm_control;
        break;
    case 'L':
        mask |= nss_mm_lock;
        break;
    case 'M': case 'A':
    case '1':
        mask |= nss_mm_mod1;
        break;
    case '2': case '3':
    case '4': case '5':
        mask |= nss_mm_mod1 + c[-1] - '1';
        break;
    }

    if (has_t && !recur) {
        c = sconf(SCONF_TERM_MOD);
        end = c + strlen(c);
        recur = 1;
        goto again;
    }

    return mask;
}

uint32_t nss_input_force_mouse_mask(void) {
    const char *tmodstr = sconf(SCONF_FORCE_MOUSE_MOD);
    return decode_mask(tmodstr, tmodstr + strlen(tmodstr));
}

void nss_input_set_hotkey(enum nss_shortcut_action sa, const char *c) {
    uint32_t sym, mask = 0;
    char *dash = strchr(c, '-');

    if (iconf(ICONF_TRACE_INPUT))
        info("Set hotkey: %d = '%s'", sa, c);

    if (dash) {
        mask = decode_mask(c, dash);
        c = dash + 1;
    }

    if ((sym = xkb_keysym_from_name(c, 0)) == XKB_KEY_NoSymbol)
        sym = xkb_keysym_from_name(c, XKB_KEYSYM_CASE_INSENSITIVE);
    if (sym == XKB_KEY_NoSymbol) warn("Wrong key name: '%s'", dash);


    cshorts[sa] = (struct nss_shortcut) { sym, mask };
}

enum nss_shortcut_action nss_input_lookup_hotkey(struct key k) {
    enum nss_shortcut_action action = nss_sa_none + 1;
    for (; action < nss_sa_MAX; action++)
        if (cshorts[action].ksym == k.sym && (k.mask & 0xFF) == cshorts[action].mask)
            break;

    if (action == nss_sa_MAX) return nss_sa_none;

    return action;
}

