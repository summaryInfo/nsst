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
    shortcut_view_next_cmd,
    shortcut_view_prev_cmd,
    shortcut_MAX
};

enum renderer_backend {
    renderer_auto,
    renderer_x11,
    renderer_wayland,
    renderer_x11_xrender,
    renderer_x11_shm,
    renderer_wayland_shm,
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

enum uri_mode {
    uri_mode_off,
    uri_mode_manual,
    uri_mode_auto,
};

#define MAX_DOMAIN_NAME 254

struct global_config {
    char *sockpath;
    char hostname[MAX_DOMAIN_NAME];

    char *open_command;
    char *notify_command;
    bool unique_uris;

    enum renderer_backend backend;

    int log_level;

    bool daemon_mode;
    bool clone_config;
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

struct geometry {
    struct rect r;
    bool stick_to_bottom : 1;
    bool stick_to_right : 1;
    bool char_geometry : 1;
    bool has_position : 1;
    bool has_extent : 1;
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
    int64_t wait_for_configure_delay;

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
    char *normal_pointer;
    char *resize_pointer;
    char *uri_pointer;

    enum keyboad_mapping mapping;
    enum cursor_type cursor_shape;
    enum pixel_mode pixel_mode;
    enum charset keyboard_nrcs;
    enum uri_mode uri_mode;
    int modify_other_fmt;
    int margin_bell_volume;
    int bell_volume;

    uint32_t force_mouse_mask;
    uint32_t uri_click_mask;

    struct geometry geometry;

    int16_t tab_width;
    int16_t vt_version;
    int16_t margin_bell_column;
    int16_t smooth_scroll_step;
    int16_t underline_width;
    int16_t cursor_width;
    struct border {
        int16_t left;
        int16_t right;
        int16_t top;
        int16_t bottom;
    } border;
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

    bool allow_altscreen;
    bool allow_blinking;
    bool allow_erase_scrollback;
    bool allow_legacy_edit;
    bool allow_legacy_function;
    bool allow_legacy_keypad;
    bool allow_legacy_misc;
    bool allow_luit;
    bool allow_nrcs;
    bool allow_subst_font;
    bool allow_window_ops;
    bool alternate_scroll;
    bool appcursor;
    bool appkey;
    bool autorepeat;
    bool backspace_is_delete;
    bool blend_all_bg;
    bool blend_fg;
    bool delete_is_delete;
    bool extended_cir;
    bool fixed;
    bool force_scalable;
    bool force_utf8_nrcs;
    bool force_utf8_title;
    bool force_wayland_csd;
    bool has_meta;
    bool keep_clipboard;
    bool keep_selection;
    bool lock;
    bool meta_is_esc;
    bool numlock;
    bool override_boxdraw;
    bool print_attr;
    bool raise_on_bell;
    bool reverse_video;
    bool scroll_on_input;
    bool scroll_on_output;
    bool select_to_clipboard;
    bool smooth_resize;
    bool smooth_scroll;
    bool special_blink;
    bool special_bold;
    bool special_italic;
    bool special_reverse;
    bool special_underline;
    bool urgency_on_bell;
    bool utf8;
    bool visual_bell;
    bool wrap;
};

extern struct global_config gconfig;

struct option;

void init_options(void);
void free_options(void);

struct option *find_option_entry(const char *name, bool need_warn);
struct option *find_short_option_entry(char name);
bool is_boolean_option(struct option *opt);

bool set_option_entry(struct instance_config *c, struct option *, const char *value, bool allow_global);
void set_default_dpi(double dpi, struct instance_config *cfg);
void copy_config(struct instance_config *dst, struct instance_config *src);
void free_config(struct instance_config *src);
void init_instance_config(struct instance_config *cfg, const char *config_path, bool allow_global);

#endif
