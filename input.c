/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#include "feature.h"

#include "config.h"
#include "input.h"
#include "nrcs.h"
#include "term.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

struct reply {
    uint8_t idx;
    uint8_t final;
    uint8_t priv;
    uint32_t init;
    uint32_t param[3];
};

static inline bool is_edit_keypad(uint32_t ks, bool deldel) {
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

static inline bool is_edit_function(uint32_t ks, bool deldel) {
    switch (ks) {
    case XKB_KEY_KP_Insert:
    case XKB_KEY_KP_Delete:
    case XKB_KEY_ISO_Left_Tab:
        return 1;
    }
    return is_edit_keypad(ks, deldel);
}

static inline bool is_cursor(uint32_t ks) {
    return XKB_KEY_Home <= ks && ks <= XKB_KEY_Select;
}

static inline bool is_keypad(uint32_t ks) {
    return XKB_KEY_KP_Space <= ks && ks <= XKB_KEY_KP_Equal;
}

static inline bool is_keypad_function(uint32_t ks) {
    return XKB_KEY_KP_F1 <= ks && ks <= XKB_KEY_KP_F4;
}

static inline bool is_function(uint32_t ks) {
    return XKB_KEY_F1 <= ks && ks <= XKB_KEY_F35;
}

static inline bool is_misc_function(uint32_t ks) {
    return XKB_KEY_Select <= ks && ks <= XKB_KEY_Break;
}

static inline bool is_special(uint32_t ks) {
    return XKB_KEY_ISO_Lock <= ks && ks <= XKB_KEY_Delete;
}

static inline bool is_private(uint32_t ks) {
    return 0x11000000 <= ks && ks <= 0x1100FFFF;
}

static inline bool is_ctrl_letter(uint32_t ks) {
    return ks >= 0x40 && ks <= 0x7F;
}

static inline bool is_ctrl(uint32_t ks) {
    return ks < 0x20 || (ks >= 0x7F && ks < 0x100);
}

static inline bool is_xkb_ctrl(struct key *k) {
    // Detect if thats something like Ctrl-3
    // which gets translated to ESC by XKB
    return is_ctrl(k->utf32);
}

static inline bool is_modify_allowed(struct key *k, struct keyboard_state *mode) {
    if (mode->keyboad_vt52) return 0;
    bool legacy = mode->keyboard_mapping > 0;
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

static uint32_t filter_modifiers(struct key *k, struct keyboard_state *mode) {
    uint32_t res = k->mask & (mask_control | mask_shift | mask_mod_1);

    if (mode->modkey_other <= 1) {
        if (is_ctrl_letter(k->sym) && !(res & ~mask_control)) {
            if (!mode->modkey_other) res &= ~mask_control;
        } else if (k->sym == XKB_KEY_Return || k->sym == XKB_KEY_Tab) {
            /* do nothing */;
        } else if (is_xkb_ctrl(k)) {
            if (!(res & mask_mod_1)) res = 0;
        } else if (!is_ctrl(k->sym) || !is_special(k->sym)) {
            if (!(res & mask_control)) res &= ~mask_shift;
        }
        if (res & mask_mod_1) {
            if (!(res & ~mask_mod_1) && (mode->meta_escape || k->utf32 < 0x80)) res &= ~mask_mod_1;
            if (((is_ctrl_letter(k->sym) || is_ctrl(k->sym)) && (res & mask_control)))
                res &= ~(mask_mod_1 | mask_control);
            if (k->sym == XKB_KEY_Return || k->sym == XKB_KEY_Tab)
                res &= ~(mask_mod_1 | mask_control);
        }
    }
    return res;
}

static inline uint32_t mask_to_param(uint32_t mask) {
    uint32_t res = 0;
    if (mask & mask_shift) res |= 1;
    if (mask & mask_control) res |= 4;
    if (mask & mask_mod_1) res |= 2;
    return res + !!res;
}

static bool is_modify_others_allowed(struct key *k, struct keyboard_state *mode) {
    if (!mode->modkey_other || is_private(k->sym)) return 0;
    if (!(k->mask & (mask_control | mask_shift | mask_mod_1))) return 0;

    if (mode->modkey_other == 1) {
        bool res = 0;
        switch (k->sym) {
        case XKB_KEY_BackSpace:
        case XKB_KEY_Delete:
            break;
        case XKB_KEY_ISO_Left_Tab:
            res =  k->mask & (mask_mod_1 | mask_control);
            break;
        case XKB_KEY_Return:
        case XKB_KEY_Tab:
            res = 1;
            break;
        default:
            if (is_ctrl_letter(k->sym))
                res = k->mask != mask_shift && k->mask != mask_control;
            else if (is_xkb_ctrl(k))
                res = k->mask != mask_shift && (k->mask & (mask_shift | mask_mod_1));
            else res = 1;
        }
        if (res) {
            uint32_t new_mods = filter_modifiers(k, mode);
            if (new_mods) k->mask = new_mods;
            else return 0;
        }
        return res;
    } else {
        switch (k->sym) {
        case XKB_KEY_BackSpace:
            return k->mask & (mask_mod_1 | mask_shift);
        case XKB_KEY_Delete:
            return k->mask & (mask_mod_1 | mask_shift | mask_control);
        case XKB_KEY_ISO_Left_Tab:
            return k->mask & (mask_mod_1 | mask_control);
        case XKB_KEY_Return:
        case XKB_KEY_Tab:
            return 1;
        default:
            if (is_ctrl_letter(k->sym))
                return 1;
            else if (k->mask == mask_shift)
                return k->sym == XKB_KEY_space || k->sym == XKB_KEY_Return;
            else if (k->mask & (mask_mod_1 | mask_control))
                return 1;
            return 0;
        }
    }
}

static void modify_others(uint32_t ch, uint32_t param, bool fmt, struct reply *reply) {
    if (!param) return;

    if (fmt) *reply = (struct reply) { 2, 'u', 0, '\233', {ch, param} };
    else *reply = (struct reply) { 3, '~', 0, '\233', {27, param, ch} };
}

static void modify_cursor(uint32_t param, uint8_t level, struct reply *reply) {
    if (param) switch (level) {
    case 4: reply->priv = '>';
        /* fallthrough */
    case 3: if (!reply->idx)
                reply->param[reply->idx++] = 1;
        /* fallthrough */
    case 2: reply->init = '\233';
        /* fallthrough */
    case 1: reply->param[reply->idx++] = param;
    }
}

static uint32_t fnkey_dec(uint32_t ks, bool is_fkey, struct reply *reply) {
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
        uint32_t p;
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

static bool fnkey_hp(uint32_t ks, bool is_fkey, struct reply *reply) {
    uint32_t res;
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

static bool fnkey_sco(uint32_t ks, bool is_fkey, struct reply *reply) {
    uint32_t res;
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

static bool fnkey_sun(uint32_t ks, bool is_fkey, struct reply *reply) {
    uint32_t arg = 0, fin = 0;
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
    struct { uint32_t from, to; } tab[] = {
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

static void dump_reply(struct term *term, struct reply *reply) {
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
    term_sendkey(term, str, 0);

    if (gconfig.trace_input) {
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
        if (!(k->mask & mask_shift)) {
            if (k->sym == XKB_KEY_KP_Add) {
                k->sym = XKB_KEY_KP_Separator;
            } else if (k->sym == XKB_KEY_KP_Separator && k->mask & mask_control) {
                k->sym = XKB_KEY_KP_Subtract;
                k->mask &= ~mask_control;
            }
        }
        if (k->sym != XKB_KEY_Delete || !mode->delete_is_del)
            k->sym = translate_keypad(k->sym);
    }

    mode->appkey &= (k->utf8len == 1 && mode->allow_numlock && (k->mask & mask_mod_2));

    if (k->sym == XKB_KEY_Tab || k->sym == XKB_KEY_ISO_Left_Tab) {
        if (mode->modkey_other > 1) {
            if (!k->utf8len) k->utf8data[k->utf8len++] = '\t';
        } else if (k->utf8len < 2 && k->mask == mask_shift) {
            k->sym = XKB_KEY_ISO_Left_Tab;
        }
    } else if (XKB_KEY_KP_Home <= k->sym && k->sym <= XKB_KEY_KP_Begin) {
        k->sym += XKB_KEY_Home - XKB_KEY_KP_Home;
    } else if (k->sym == XKB_KEY_SunF36) {
        k->is_fkey = 1, k->sym = XKB_KEY_F1 + 36 - 1;
    } else if (k->sym == XKB_KEY_SunF37) {
        k->is_fkey = 1, k->sym = XKB_KEY_F1 + 37 - 1;
    } else if (k->sym == XKB_KEY_BackSpace) {
        if (k->utf8len == 1 && (mode->backspace_is_del ^ !!(k->mask & mask_control))) {
            k->utf8data[0] = '\177';
            k->mask &= ~mask_control;
            // k->sym = XKB_KEY_Delete;
        }
    }

    if (k->is_fkey && k->mask & (mask_control | mask_shift)) {
        if (mode->keyboard_mapping == keymap_vt220 || mode->keyboard_mapping == keymap_legacy) {
            if (k->mask & mask_control)
                k->sym += mode->fkey_inc_step;
            k->mask &= ~mask_control;
        } else if (!mode->modkey_fn) {
            if (k->mask & mask_control)
                k->sym += mode->fkey_inc_step * 2;
            if (k->mask & mask_shift)
                k->sym += mode->fkey_inc_step;
            k->mask &= ~(mask_control | mask_shift);
        }
    }
}

void keyboard_reset_udk(struct term *term) {
    struct keyboard_state *mode = term_get_kstate(term);
    for (size_t i = 0; i < UDK_MAX;  i++) {
        free(mode->udk[i].val);
        mode->udk[i].val = NULL;
        mode->udk[i].len = 0;
    }
}

bool keyboard_set_udk(struct term *term, const uint8_t *str, const uint8_t *end, bool reset, bool lock) {
    struct keyboard_state *mode = term_get_kstate(term);
    if (!mode->udk_locked) {
        if (reset) keyboard_reset_udk(term);
        mode->udk_locked = lock;
        for (; str < end; str++) {
            uparam_t k = 0;
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

void keyboard_handle_input(struct key k, struct term *term) {
    struct keyboard_state *mode = term_get_kstate(term);

    if (gconfig.trace_input) {
        info("Key: sym=0x%X mask=0x%X ascii=0x%X utf32=0x%X",
                k.sym, k.mask, k.ascii, k.utf32);
    }

    if (mode->keylock) return;

    // Appkey can be modified during adjustment
    bool saved_appkey = mode->appkey;

    translate_adjust(&k, mode);

    struct reply reply = {0};
    uint32_t param = k.mask && is_modify_allowed(&k, mode) ? mask_to_param(k.mask) : 0;


    bool (*kfn[keymap_MAX])(uint32_t, bool, struct reply *) = {
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
        uint32_t deccode = fnkey_dec(k.sym, k.is_fkey, &reply);
        if (k.is_fkey && k.mask & mask_shift && mode->keyboard_mapping == keymap_vt220) {
            struct udk udk = reply.param[0] < UDK_MAX ? mode->udk[reply.param[0]] : (struct udk){0};
            if (udk.val) {
                if (gconfig.trace_input)
                    info("Key str: '%s' ", udk.val);
                term_sendkey(term, udk.val, udk.len);
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
                if (mode->modkey_other >= 2 && (k.mask & (mask_control | mask_mod_1)))
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
            if (gconfig.trace_input)
                info("Key char: (%x) '%c' ", ch, ch);
            term_sendkey(term, &ch, 1);
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
             * uint32_t val = k.ascii ? k.ascii : k.sym; */
            uint32_t val = k.sym < 0x100 ? k.sym : k.utf32;
            modify_others(val, mask_to_param(k.mask), mode->modkey_other_fmt, &reply);
            dump_reply(term, &reply);
        } else {
            if (term_is_utf8_enabled(term)) {
                if ((k.mask & mask_mod_1) && mode->has_meta) {
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
                if (term_is_nrcs_enabled(term))
                    nrcs_encode(window_cfg(term_window(term))->keyboard_nrcs, &k.utf32, 1);

                if (k.utf32 > 0xFF) {
                    mode->appkey = saved_appkey;
                    return;
                }

                k.utf8len = 1;
                k.utf8data[0] = k.utf32;
                if (k.mask & mask_mod_1 && mode->has_meta) {
                    if (!mode->meta_escape) {
                        k.utf8data[0] |= 0x80;
                    } else {
                        k.utf8data[0] = '\033';
                        k.utf8data[k.utf8len++] = k.utf32;
                    }
                }
            }
            k.utf8data[k.utf8len] = '\0';
            if (gconfig.trace_input)
                info("Key char: (%x) '%s' ", k.utf32, k.utf8data);
            term_sendkey(term, k.utf8data, k.utf8len);
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

struct key keyboard_describe_key(struct xkb_state *state, xkb_keycode_t keycode) {
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

    if (k.mask & ~consumed & mask_lock) k.sym = xkb_keysym_to_upper(k.sym);

    if ((k.utf32 = xkb_keysym_to_utf32(k.sym))) {
        if ((k.mask & ~consumed & mask_control) && k.ascii) k.utf32 = to_control(k.ascii);
        k.utf8len = utf8_encode(k.utf32, k.utf8data, k.utf8data + sizeof(k.utf8data));
        k.utf8data[k.utf8len] = '\0';
    }

    return k;
}

static uint32_t decode_mask(const char *c, const char *end, const char *termmod) {
    uint32_t mask = 0;
    bool recur = 0, has_t = 0;

again:
    while (c < end) switch (*c++) {
    case 'T':
        if (recur) mask |= mask_shift | mask_control;
        else has_t = 1;
        break;
    case 'S':
        mask |= mask_shift;
        break;
    case 'C':
        mask |= mask_control;
        break;
    case 'L':
        mask |= mask_lock;
        break;
    case 'M': case 'A':
    case '1':
        mask |= mask_mod_1;
        break;
    case '2': case '3':
    case '4': case '5':
        mask |= mask_mod_1 + c[-1] - '1';
        break;
    }

    if (has_t && !recur) {
        c = termmod;
        end = c + strlen(c);
        recur = 1;
        goto again;
    }

    return mask;
}

void keyboard_parse_config(struct instance_config *cfg) {
    for (ssize_t i = shortcut_none + 1; i < shortcut_MAX; i++) {
        uint32_t sym, mask = 0;
        char *c = cfg->key[i];
        char *dash = strchr(c, '-');

        if (gconfig.trace_input)
            info("Set shortcut: %zd = '%s'", i, c);

        if (dash) {
            mask = decode_mask(c, dash, cfg->term_mod);
            c = dash + 1;
        }

        if ((sym = xkb_keysym_from_name(c, 0)) == XKB_KEY_NoSymbol)
            sym = xkb_keysym_from_name(c, XKB_KEYSYM_CASE_INSENSITIVE);
        if (sym == XKB_KEY_NoSymbol) warn("Wrong key name: '%s'", dash);


        cfg->cshorts[i] = (struct shortcut) { sym, mask };
    }

    cfg->force_mouse_mask = decode_mask(cfg->force_mouse_mod,
            cfg->force_mouse_mod + strlen(cfg->force_mouse_mod), cfg->term_mod);
}

enum shortcut_action keyboard_find_shortcut(struct instance_config *cfg, struct key k) {
    enum shortcut_action action = shortcut_none + 1;
    for (; action < shortcut_MAX; action++)
        if (cfg->cshorts[action].ksym == k.sym && (k.mask & 0xFF) == cfg->cshorts[action].mask)
            break;

    if (action == shortcut_MAX) return shortcut_none;

    return action;
}
