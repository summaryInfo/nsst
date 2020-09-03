/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#ifndef CONFIG_H_
#define CONFIG_H_ 1

#include <stdbool.h>
#include <stdint.h>

#include "feature.h"
#include "font.h"
#include "line.h"
#include "nrcs.h"

enum cursor_type {
    cusor_type_block_blink = 1,
    cusor_type_block = 2,
    cusor_type_underline_blink = 3,
    cusor_type_underline = 4,
    cusor_type_bar_blink = 5,
    cusor_type_bar = 6,
};

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
    shortcut_reverse_video,
    shortcut_MAX
};

enum keyboad_mapping {
    keymap_default,
    keymap_legacy,
    keymap_vt220,
    keymap_hp,
    keymap_sun,
    keymap_sco,
    keymap_MAX
};

struct global_config {
    uint8_t log_level : 2;

	bool daemon_mode : 1;
    bool trace_characters : 1;
    bool trace_controls : 1;
    bool trace_events : 1;
    bool trace_fonts : 1;
    bool trace_input : 1;
    bool trace_misc : 1;
    bool want_luit : 1;
    bool utf8 : 1;
};

struct instance_config {
    color_t palette[PALETTE_SIZE];

    double gamma;
    double dpi;
    double alpha;

    int64_t fps;
    int64_t smooth_scroll_delay;
    int64_t blink_time;
    int64_t visual_bell_time;
    int64_t max_frame_time;
    int64_t frame_finished_delay;
    int64_t sync_time;
    int64_t double_click_time;
    int64_t triple_click_time;
    int64_t select_scroll_time;

    struct shortcut {
        uint32_t ksym;
        uint32_t mask;
    } cshorts[shortcut_MAX];

    char *key[shortcut_MAX];
    char *word_separators;
    char *cwd;
    char *printer_cmd;
    char *printer_file;
    char *luit;
    char *terminfo;
    char **argv;
    char *answerback_string;
    char *title;
    char *window_class;
    char *font_name;
    char *config_path;
    char *term_mod;
    char *force_mouse_mod;
    char *shell;

    ssize_t tab_width;
    ssize_t scrollback_size;

    enum keyboad_mapping mapping;
    enum cursor_type cursor_shape;
    enum pixel_mode pixel_mode;
    enum charset keyboard_nrcs;

    uint32_t force_mouse_mask;

    int16_t vt_version;
    int16_t margin_bell_column;
    int16_t smooth_scroll_step;
    int16_t x;
    int16_t y;
    int16_t width;
    int16_t height;
    int16_t underline_width;
    int16_t cursor_width;
    int16_t left_border;
    int16_t top_border;
    int16_t font_size;
    int16_t font_size_step;
    int16_t font_spacing;
    int16_t line_spacing;
    int16_t scroll_amount;

    uint8_t margin_bell_high_volume;
    uint8_t margin_bell_low_volume;
    uint8_t bell_high_volume;
    uint8_t bell_low_volume;

    uint8_t margin_bell_volume : 2;
    uint8_t bell_volume : 2;
    uint8_t modify_cursor : 2;
    uint8_t modify_function : 2;
    uint8_t modify_keypad : 2;
    uint8_t fkey_increment : 6;
    uint8_t modify_other : 3;

    bool blend_fg : 1;
    bool blend_all_bg : 1;
    bool allow_blinking : 1;
    bool allow_luit : 1;
    bool print_attr : 1;
    bool extended_cir : 1;
    bool rewrap : 1;
    bool cut_lines : 1;
    bool minimize_scrollback : 1;
    bool allow_erase_scrollback : 1;
    bool alternate_scroll : 1;
    bool keep_clipboard : 1;
    bool keep_selection : 1;
    bool select_to_clipboard : 1;
    bool utf8 : 1;
    bool wrap : 1;
    bool scroll_on_output : 1;
    bool scroll_on_input : 1;
    bool reverse_video : 1;
    bool allow_altscreen : 1;
    bool allow_nrcs : 1;
    bool allow_window_ops : 1;
    bool force_utf8_nrcs : 1;
    bool visual_bell : 1;
    bool raise_on_bell : 1;
    bool urgency_on_bell : 1;
    bool smooth_scroll : 1;
    bool appcursor : 1;
    bool appkey : 1;
    bool backspace_is_delete : 1;
    bool delete_is_delete : 1;
    bool has_meta : 1;
    bool lock : 1;
    bool meta_is_esc : 1;
    bool modify_other_fmt : 1;
    bool allow_legacy_edit : 1;
    bool allow_legacy_function : 1;
    bool allow_legacy_keypad : 1;
    bool allow_legacy_misc : 1;
    bool numlock : 1;
    bool special_bold : 1;
    bool special_blink : 1;
    bool special_underline : 1;
    bool special_italic : 1;
    bool special_reverse : 1;
    bool stick_to_bottom : 1;
    bool stick_to_right : 1;
    bool fixed : 1;
    bool user_geometry : 1;
    bool allow_subst_font : 1;
    bool force_scalable : 1;
#if USE_BOXDRAWING
    bool override_boxdraw : 1;
#endif
};

enum optidx {
    o_allow_alternate,
    o_allow_blinking,
    o_allow_modify_edit_keypad,
    o_allow_modify_function,
    o_allow_modify_keypad,
    o_allow_modify_misc,
    o_alpha,
    o_alternate_scroll,
    o_answerback_string,
    o_appcursor,
    o_appkey,
    o_autowrap,
    o_background,
    o_backspace_is_del,
    o_bell,
    o_bell_high_volume,
    o_bell_low_volume,
    o_blend_all_background,
    o_blend_foreground,
    o_blink_color,
    o_blink_time,
    o_bold_color,
    o_config,
    o_cursor_background,
    o_cursor_foreground,
    o_cursor_shape,
    o_cursor_width,
    o_cut_lines,
    o_cwd,
    o_delete_is_del,
    o_double_click_time,
    o_dpi,
    o_erase_scrollback,
    o_extended_cir,
    o_fixed,
    o_fkey_increment,
    o_font,
    o_font_gamma,
    o_font_size,
    o_font_size_step,
    o_font_spacing,
    o_force_mouse_mod,
    o_force_nrcs,
    o_force_scalable,
    o_foreground,
    o_fps,
    o_frame_wait_delay,
    o_has_meta,
    o_horizontal_border,
    o_italic_color,
    o_keep_clipboard,
    o_keep_selection,
    o_key_break,
    o_key_copy,
    o_key_dec_font,
    o_key_inc_font,
    o_key_new_window,
    o_key_numlock,
    o_key_paste,
    o_key_reload_config,
    o_key_reset,
    o_key_reset_font,
    o_key_reverse_video,
    o_key_scroll_down,
    o_key_scroll_up,
    o_keyboard_dialect,
    o_keyboard_mapping,
    o_line_spacing,
    o_lock_keyboard,
    o_log_level,
    o_luit,
    o_luit_path,
    o_margin_bell,
    o_margin_bell_column,
    o_margin_bell_high_volume,
    o_margin_bell_low_volume,
    o_max_frame_time,
    o_meta_sends_escape,
    o_minimize_scrollback,
    o_modify_cursor,
    o_modify_function,
    o_modify_keypad,
    o_modify_other,
    o_modify_other_fmt,
    o_nrcs,
    o_numlock,
#if USE_BOXDRAWING
    o_override_boxdrawing,
#endif
    o_pixel_mode,
    o_print_attributes,
    o_print_command,
    o_printer_file,
    o_raise_on_bell,
    o_reverse_video,
    o_reversed_color,
    o_rewrap,
    o_scroll_amount,
    o_scroll_on_input,
    o_scroll_on_output,
    o_scrollback_size,
    o_select_scroll_time,
    o_select_to_clipboard,
    o_selected_background,
    o_selected_foreground,
    o_shell,
    o_smooth_scroll,
    o_smooth_scroll_delay,
    o_smooth_scroll_step,
    o_special_blink,
    o_special_bold,
    o_special_italic,
    o_special_reverse,
    o_special_underlined,
    o_substitute_fonts,
    o_sync_timeout,
    o_tab_width,
    o_term_mod,
    o_term_name,
    o_title,
    o_trace_characters,
    o_trace_controls,
    o_trace_events,
    o_trace_fonts,
    o_trace_input,
    o_trace_misc,
    o_triple_click_time,
    o_underline_width,
    o_underlined_color,
    o_urgent_on_bell,
    o_use_utf8,
    o_vertical_border,
    o_visual_bell,
    o_visual_bell_time,
    o_vt_version,
    o_window_class,
    o_window_ops,
    o_word_break,
    o_MAX,
};


struct optmap_item {
    char *opt;
    char *descr;
};

extern struct optmap_item optmap[];
extern struct global_config gconfig;

bool set_option(struct instance_config *c, const char *name, const char *value, bool allow_global);
void set_default_dpi(double dpi);
void copy_config(struct instance_config *dst, struct instance_config *src);
void free_config(struct instance_config *src);
void parse_config(struct instance_config *cfg);
void init_instance_config(struct instance_config *cfg);

#endif
