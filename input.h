/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#ifndef INPUT_H_
#define INPUT_H_ 1

#include <stdbool.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#define UDK_MAX 37

enum shortcut_action {
    shortcut_none,
    shortcut_break,
    shortcut_numlock,
    shortcut_scroll_up,
    shortcut_scroll_down,
    shortcut_font_up,
    shortcut_font_down,
    shortcut_font_default,
    shortcut_new_window,
    shortcut_reset,
    shortcut_reload_config,
    shortcut_copy,
    shortcut_paste,
    shortcut_MAX = shortcut_paste + 1
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

};

typedef uint32_t term_char_t;

struct key {
    term_char_t utf32;
    uint32_t sym;
    uint32_t mask;
    uint8_t utf8data[6]; // zero terminated
    uint8_t utf8len;
    uint8_t ascii : 7;
    uint8_t is_fkey : 1;
};

typedef struct nss_term nss_term_t;

void keyboard_handle_input(struct key k, nss_term_t *term);
struct key keyboard_describe_key(struct xkb_state *state, xkb_keycode_t keycode);
uint32_t keyboard_force_select_mask(void);
void keyboard_set_shortcut(enum shortcut_action sa, const char *val);
enum shortcut_action keyboard_find_shortcut(struct key k);
void keyboard_reset_udk(nss_term_t *term);
bool keyboard_set_udk(nss_term_t *term, const uint8_t *str, const uint8_t *end, bool reset, bool lock);

#endif
