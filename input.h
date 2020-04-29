/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#ifndef INPUT_H_
#define INPUT_H_ 1

#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

/* mod1 is alt
 * mod2 is numlock
 * mod4 is super
 */
enum mode_mask {
    nss_mm_shift = 1 << 0,
    nss_mm_lock = 1 << 1,
    nss_mm_control = 1 << 2,
    nss_mm_mod1 = 1 << 3,
    nss_mm_mod2 = 1 << 4,
    nss_mm_mod3 = 1 << 5,
    nss_mm_mod4 = 1 << 6,
    nss_mm_mod5 = 1 << 7,
};

typedef struct nss_input_mode {
    uint32_t modkey_fn : 3;
    uint32_t modkey_cursor : 3;
    uint32_t modkey_keypad : 3;
        // 0 ->
        // 1 -> SS3 ...
        // 2 -> CSI ...
        // 3 -> CSI 1 ; ...
        // 4 -> CSI > 1 ; ...
        // 5,6,7 reserved
    uint32_t modkey_other : 2;
        // 0 -> nothing
        // 1 -> all, but common
        // 2 -> all
        // 3 reserved
    uint32_t modkey_other_fmt : 1;
        // 0 -> CSI 27 ; M ; K ~
        // 1 -> CSI K ; M u
    uint32_t modkey_legacy_allow_keypad : 1;
    uint32_t modkey_legacy_allow_edit_keypad : 1;
    uint32_t modkey_legacy_allow_function : 1;
    uint32_t modkey_legacy_allow_misc : 1;

    uint32_t appkey : 1;
    uint32_t appcursor : 1;
    uint32_t allow_numlock : 1;
    uint32_t keylock : 1;

    uint32_t has_meta : 1;
    uint32_t meta_escape : 1;
    uint32_t backspace_is_del : 1;
    uint32_t delete_is_del : 1;

    uint32_t fkey_inc_step : 4;

    uint32_t keyboad_vt52 : 1;
    enum nss_keyboad_mapping {
        nss_km_default,
        nss_km_legacy,
        nss_km_vt220,
        nss_km_hp,
        nss_km_sun,
        nss_km_sco,
        nss_km_MAX
    } keyboard_mapping : 3;
    // 32 bits total
} nss_input_mode_t;

typedef uint32_t nss_char_t;

typedef struct nss_key {
    nss_char_t utf32;
    uint32_t sym;
    uint32_t mask;
    uint8_t utf8data[6]; // zero terminated
    uint8_t utf8len;
    uint8_t ascii : 7;
    uint8_t is_fkey : 1;
} nss_key_t;

typedef struct nss_term nss_term_t;
void nss_handle_input(nss_key_t k, nss_term_t *term);
nss_key_t nss_describe_key(struct xkb_state *state, xkb_keycode_t keycode);

#endif
