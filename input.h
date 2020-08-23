/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#ifndef INPUT_H_
#define INPUT_H_ 1

#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#define UDK_MAX 37

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

enum nss_shortcut_action {
    nss_sa_none,
    nss_sa_break,
    nss_sa_numlock,
    nss_sa_scroll_up,
    nss_sa_scroll_down,
    nss_sa_font_up,
    nss_sa_font_down,
    nss_sa_font_default,
    nss_sa_new_window,
    nss_sa_reset,
    nss_sa_reload_config,
    nss_sa_copy,
    nss_sa_paste,
    nss_sa_MAX = nss_sa_paste + 1
};

struct keyboard_state {
    _Bool keyboad_vt52 : 1;

    _Bool modkey_legacy_allow_keypad : 1;
    _Bool modkey_legacy_allow_edit_keypad : 1;
    _Bool modkey_legacy_allow_function : 1;
    _Bool modkey_legacy_allow_misc : 1;

    _Bool appkey : 1;
    _Bool appcursor : 1;
    _Bool allow_numlock : 1;
    _Bool keylock : 1;

    _Bool has_meta : 1;
    _Bool meta_escape : 1;
    _Bool backspace_is_del : 1;
    _Bool delete_is_del : 1;

    _Bool udk_locked : 1;

    _Bool modkey_other_fmt : 1;
        // 0 -> CSI 27 ; M ; K ~
        // 1 -> CSI K ; M u

    uint16_t modkey_fn : 3;
    uint16_t modkey_cursor : 3;
    uint16_t modkey_keypad : 3;
        // 0 ->
        // 1 -> SS3 ...
        // 2 -> CSI ...
        // 3 -> CSI 1 ; ...
        // 4 -> CSI > 1 ; ...
        // 5,6,7 reserved
    uint16_t modkey_other : 2;
        // 0 -> nothing
        // 1 -> all, but common
        // 2 -> all
        // 3 reserved

    uint16_t fkey_inc_step : 5;

    enum keyboad_mapping {
        keymap_default,
        keymap_legacy,
        keymap_vt220,
        keymap_hp,
        keymap_sun,
        keymap_sco,
        keymap_MAX
    } keyboard_mapping;

    struct udk {
        uint8_t *val;
        size_t len;
    } udk[UDK_MAX];

} nss_keyboard_state_t;

typedef uint32_t nss_char_t;

struct key {
    nss_char_t utf32;
    uint32_t sym;
    uint32_t mask;
    uint8_t utf8data[6]; // zero terminated
    uint8_t utf8len;
    uint8_t ascii : 7;
    uint8_t is_fkey : 1;
};

typedef struct nss_term nss_term_t;
typedef struct nss_window nss_window_t;

void nss_handle_input(struct key k, nss_term_t *term);
struct key nss_describe_key(struct xkb_state *state, xkb_keycode_t keycode);
uint32_t nss_input_force_mouse_mask(void);
void nss_input_set_hotkey(enum nss_shortcut_action sa, const char *val);
enum nss_shortcut_action nss_input_lookup_hotkey(struct key k);
void nss_input_reset_udk(nss_term_t *term);
_Bool nss_input_set_udk(nss_term_t *term, const uint8_t *str, const uint8_t *end, _Bool reset, _Bool lock);

#endif
