/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

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
    shortcut_copy_uri,
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

#define MAX_DOMAIN_NAME 254

struct global_config {
    char *sockpath;
    char hostname[MAX_DOMAIN_NAME];

    char *open_command;
    bool unique_uris;

    int log_level;

    bool daemon_mode;
    bool trace_characters;
    bool trace_controls;
    bool trace_events;
    bool trace_fonts;
    bool trace_input;
    bool trace_misc;
    bool want_luit;
    bool fork;
    bool log_color;
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
    int64_t scrollback_size;

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
    char *uri_click_mod;


    enum keyboad_mapping mapping;
    enum cursor_type cursor_shape;
    enum pixel_mode pixel_mode;
    enum charset keyboard_nrcs;
    int modify_other_fmt;
    int margin_bell_volume;
    int bell_volume;

    uint32_t force_mouse_mask;
    uint32_t uri_click_mask;

    int16_t tab_width;
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
    uint8_t modify_cursor;
    uint8_t modify_function;
    uint8_t modify_keypad;
    uint8_t fkey_increment;
    uint8_t modify_other;

    bool blend_fg;
    bool blend_all_bg;
    bool allow_blinking;
    bool allow_luit;
    bool print_attr;
    bool extended_cir;
    bool minimize_scrollback;
    bool allow_erase_scrollback;
    bool alternate_scroll;
    bool keep_clipboard;
    bool keep_selection;
    bool select_to_clipboard;
    bool utf8;
    bool wrap;
    bool scroll_on_output;
    bool scroll_on_input;
    bool reverse_video;
    bool allow_altscreen;
    bool allow_nrcs;
    bool allow_window_ops;
    bool force_utf8_nrcs;
    bool visual_bell;
    bool raise_on_bell;
    bool urgency_on_bell;
    bool smooth_scroll;
    bool appcursor;
    bool appkey;
    bool backspace_is_delete;
    bool delete_is_delete;
    bool has_meta;
    bool lock;
    bool meta_is_esc;
    bool allow_legacy_edit;
    bool allow_legacy_function;
    bool allow_legacy_keypad;
    bool allow_legacy_misc;
    bool numlock;
    bool special_bold;
    bool special_blink;
    bool special_underline;
    bool special_italic;
    bool special_reverse;
    bool stick_to_bottom;
    bool stick_to_right;
    bool fixed;
    bool user_geometry;
    bool allow_subst_font;
    bool force_scalable;
    bool autorepeat;
    bool allow_uris;
    bool override_boxdraw;
};

extern struct global_config gconfig;

struct option;

void init_options(void);
void free_options(void);

struct option *find_option_entry(const char *name, bool need_warn);
struct option *find_short_option_entry(char name);
bool is_boolean_option(struct option *opt);

bool set_option_entry(struct instance_config *c, struct option *, const char *value, bool allow_global);
void set_default_dpi(double dpi);
void copy_config(struct instance_config *dst, struct instance_config *src);
void free_config(struct instance_config *src);
void init_instance_config(struct instance_config *cfg, const char *config_path, bool allow_global);

#endif
