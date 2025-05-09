/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

#ifndef WINDOW_H_
#define WINDOW_H_ 1

#include "feature.h"

#include "font.h"
#include "config.h"
#include "util.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

/* WARNING: Order is important */
enum mouse_event_type {
    mouse_event_press,
    mouse_event_release,
    mouse_event_motion,
};

enum modifier_mask {
    mask_shift = 1 << 0,
    mask_lock = 1 << 1,
    mask_control = 1 << 2,
    mask_mod_1 = 1 << 3, /* Alt */
    mask_mod_2 = 1 << 4, /* Numlock */
    mask_mod_3 = 1 << 5,
    mask_mod_4 = 1 << 6, /* Super */
    mask_mod_5 = 1 << 7,
    mask_button_1 = 1 << 8,
    mask_button_2 = 1 << 9,
    mask_button_3 = 1 << 10,
    mask_button_4 = 1 << 11,
    mask_button_5 = 1 << 12,
    mask_state_mask = 0x1FFF,
    mask_mod_mask = 0xFF,
};

enum clip_target {
    clip_primary,
    clip_clipboard,
    clip_secondary,
    clip_MAX,
    clip_invalid = -1,
};

enum title_target {
    target_title = 1,
    target_icon_label = 2,
    target_title_icon_label = target_title | target_icon_label,
};

enum window_action {
    action_none,
    action_minimize,
    action_restore_minimized,
    action_maximize,
    action_maximize_width,
    action_maximize_height,
    action_fullscreen,
    action_toggle_fullscreen,
    action_restore,
    action_lower,
    action_raise,
};


struct mouse_event {
    enum mouse_event_type event;
    enum modifier_mask mask;
    int16_t x;
    int16_t y;
    uint8_t button;
};

struct extent {
    int16_t width, height;
};

enum hide_pointer_mode {
    hide_never,
    hide_no_tracking,
    hide_always,
    hide_invalid = -1,
};

typedef uint32_t color_t;

void init_context(struct instance_config *cfg);
void free_context(void);

struct window *create_window(struct instance_config *cfg);
void free_window(struct window *win);

bool window_submit_screen(struct window *win, int16_t cur_x, ssize_t cur_y, bool cursor, bool marg, bool cmoved);
void window_shift(struct window *win, int16_t ys, int16_t yd, int16_t height);
void window_paste_clip(struct window *win, enum clip_target target);
void window_delay_redraw(struct window *win);
void window_request_scroll_flush(struct window *win);
bool window_resize(struct window *win, int16_t width, int16_t height);
void window_move(struct window *win, int16_t x, int16_t y);
bool window_action(struct window *win, enum window_action act);
bool window_is_mapped(struct window *win);
void window_bell(struct window *win, uint8_t vol);
void window_reset_delayed_redraw(struct window *win);

struct extent window_get_position(struct window *win);
struct extent window_get_grid_position(struct window *win);
struct extent window_get_grid_size(struct window *win);
struct extent window_get_screen_size(struct window *win);
struct extent window_get_cell_size(struct window *win);
struct border window_get_border(struct window *win);
struct extent window_get_size(struct window *win);

void window_set_title(struct window *win, enum title_target which, const char *name, bool utf8);
void window_push_title(struct window *win, enum title_target which);
void window_pop_title(struct window *win, enum title_target which);
/* Both at the same time are not supported */
void window_get_title(struct window *win, enum title_target which, char **name, bool *utf8);

void window_set_active_uri(struct window *win, uint32_t uri, bool pressed);
void window_set_mouse(struct window *win, bool enabled);
void window_set_colors(struct window *win, color_t bg, color_t cursor_fg);
void window_get_pointer(struct window *win, int16_t *px, int16_t *py, uint32_t *pmask);
enum cursor_type window_get_cursor(struct window *win);
void window_set_clip(struct window *win, uint8_t *data, enum clip_target target);
void window_set_sync(struct window *win, bool state);
bool window_get_sync(struct window *win);
void window_set_autorepeat(struct window *win, bool state);
bool window_get_autorepeat(struct window *win);
void window_set_alpha(struct window *win, double alpha);
struct instance_config *window_cfg(struct window *win);
void window_set_pointer_mode(struct window *win, enum hide_pointer_mode mode);
void window_set_pointer_shape(struct window *win, const char *name);

bool init_daemon(void);
void free_daemon(void);
bool daemon_process_clients(void);

extern struct instance_config global_instance_config;

void x11_xrender_release_glyph(struct glyph *);

#endif
