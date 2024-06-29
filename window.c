/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

#include "feature.h"

#include "input.h"
#include "mouse.h"
#include "poller.h"
#include "term.h"
#include "tty.h"
#include "window-impl.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define NUM_BORDERS 4

struct context {
    double font_size;
};

static struct context ctx;
const struct platform_vtable *pvtbl;

struct list_head win_list_head;
static volatile sig_atomic_t reload_config;

static void handle_sigusr1(int sig) {
    reload_config = 1;
    (void)sig;
}

_Noreturn static void handle_term(int sig) {
    hang_watched_children();

    if (gconfig.daemon_mode)
        free_daemon();

    (void)sig;

    _exit(EXIT_SUCCESS);
}

static void handle_hup(int sig) {
    /* We need to ignore SIGHUPs sent by our children */
    if (fcntl(STDOUT_FILENO, F_GETFD) < 0)
        handle_term(sig);
}

static void tick(void *arg, const struct timespec *now);

void init_context(struct instance_config *cfg) {
    poller_add_tick(tick, NULL);
    list_init(&win_list_head);

    if (!pvtbl && USE_WAYLAND)
        pvtbl = platform_init_wayland(cfg);
    if (!pvtbl && USE_X11)
        pvtbl = platform_init_x11(cfg);
    if (!pvtbl)
        die("Cannot find suitable backend");

    sigaction(SIGUSR1, &(struct sigaction){ .sa_handler = handle_sigusr1, .sa_flags = SA_RESTART }, NULL);
    sigaction(SIGUSR2, &(struct sigaction){ .sa_handler = handle_sigusr1, .sa_flags = SA_RESTART }, NULL);
    sigaction(SIGHUP, &(struct sigaction) { .sa_handler = handle_hup, .sa_flags = SA_RESTART}, NULL);

    struct sigaction sa = { .sa_handler = handle_term };
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

void free_context(void) {
    LIST_FOREACH_SAFE(it, &win_list_head)
        free_window(CONTAINEROF(it, struct window, link));

    if (gconfig.daemon_mode)
        unlink(gconfig.sockpath);

    pvtbl->free();

#if USE_URI
    uri_release_memory();
#endif
}

struct instance_config *window_cfg(struct window *win) {
    return &win->cfg;
}

static void queue_force_redraw(struct window *win) {
    win->force_redraw = true;
}

void window_set_colors(struct window *win, color_t bg, color_t cursor_fg) {
    color_t obg = win->bg_premul, ofg = win->cursor_fg;

    if (bg) {
        win->bg = bg;
        win->bg_premul = color_apply_a(bg, win->cfg.alpha);
    }

    if (cursor_fg) {
        win->cursor_fg = cursor_fg;
    }

    bool cfg_changed = cursor_fg && cursor_fg != ofg;
    bool bg_changed = bg && win->bg_premul != obg;

    if (bg_changed) {
        pvtbl->update_colors(win);
    }

    if (cfg_changed || bg_changed) {
        /* If reverse video is set via option during initialization
         * win->term can be NULL at this point. */

        if (win->term) screen_damage_lines(term_screen(win->term), 0, win->ch);
        queue_force_redraw(win);
    }
}

void window_set_mouse(struct window *win, bool enabled) {
#if USE_URI
    window_set_active_uri(win, EMPTY_URI, 0);
#endif
    pvtbl->enable_mouse_events(win, enabled);
}

static void window_delay_redraw_after_read(struct window *win);

static void handle_term_read(void *win_, uint32_t mask) {
    struct window *win = win_;

    if (mask & (POLLHUP | POLLERR | POLLNVAL)) {
        free_window(win);
        return;
    }

    if (!term_read(win->term)) return;

    window_delay_redraw_after_read(win);
    win->any_event_happend = true;
    if (pvtbl->after_read)
        pvtbl->after_read(win);
}

static inline void dec_read_inhibit(struct window *win) {
    if (!--win->inhibit_read_counter) {
        poller_toggle(win->tty_event, true);
        handle_term_read(win, POLLIN);
    }
}

static inline void inc_read_inhibit(struct window *win) {
    if (!win->inhibit_read_counter++)
        poller_toggle(win->tty_event, false);
}

void window_reset_delayed_redraw(struct window *win) {
    win->inhibit_render_counter -= poller_unset(&win->redraw_delay_timer);
    win->any_event_happend = true;
}

static bool handle_read_delay_timeout(void *win_, const struct timespec *now_) {
    struct window *win = win_;
    (void)now_;
    /* If we haven't read for a while, reset a redraw delay,
     * which is used for redraw trottling. */
    window_reset_delayed_redraw(win);
    return false;
}

static void window_delay_redraw_after_read(struct window *win) {
    poller_set_timer(&win->read_delay_timer, handle_read_delay_timeout,
                     win, win->cfg.frame_finished_delay*1000LL);
}

static bool handle_configure_timeout(void *win_, const struct timespec *now_) {
    struct window *win = win_;
    (void)now_;
    win->configure_delay_timer = NULL;
    dec_read_inhibit(win);
    return false;
}

static inline void wait_for_configure(struct window *win, int mult) {
   if (!poller_set_timer(&win->configure_delay_timer, handle_configure_timeout,
                        win, mult*win->cfg.wait_for_configure_delay*1000L)) {
        inc_read_inhibit(win);
   }
}

bool window_action(struct window *win, enum window_action act) {
    bool success = pvtbl->window_action(win, act);
    if (success)
        wait_for_configure(win, 1);
    return success;
}

void window_move(struct window *win, int16_t x, int16_t y) {
    pvtbl->move_window(win, x, y);
}

bool window_resize(struct window *win, int16_t width, int16_t height) {
    bool success = pvtbl->resize_window(win, width, height);
    if (success)
        wait_for_configure(win, 1);
    return success;
}


void window_get_pointer(struct window *win, int16_t *px, int16_t *py, uint32_t *pmask) {
    struct extent ext = {0, 0};
    int32_t mask = 0;
    pvtbl->get_pointer(win, &ext, &mask);
    if (px) *px = ext.width;
    if (py) *py = ext.height;
    if (pmask) *pmask = mask;
}

void window_set_clip(struct window *win, uint8_t *data, enum clip_target target) {
    if (target == clip_invalid) {
        warn("Invalid clipboard target");
        free(data);
        return;
    }
    if (data && !pvtbl->set_clip(win, target)) {
        free(data);
        data = NULL;
    }
    free(win->clipped[target]);
    win->clipped[target] = data;
}


void window_set_alpha(struct window *win, double alpha) {
    win->cfg.alpha = MAX(MIN(1, alpha), 0);
    window_set_colors(win, win->bg, 0);
}

#if USE_URI
void window_set_active_uri(struct window *win, uint32_t uri, bool pressed) {
    bool uri_damaged = win->rcstate.active_uri != uri ||
                       (win->rcstate.uri_pressed != pressed && uri);

    if (uri_damaged) {
        struct screen *scr = term_screen(win->term);
        screen_damage_uri(scr, win->rcstate.active_uri);
        screen_damage_uri(scr, uri);
    }

    uri_ref(uri);
    uri_unref(win->rcstate.active_uri);
    win->rcstate.active_uri = uri;
    win->rcstate.uri_pressed = pressed;

    if (gconfig.trace_misc && uri_damaged) {
        info("URI set active id=%d pressed=%d", uri, pressed);
    }
}
#endif

static bool handle_sync_update_timeout(void *win_, const struct timespec *now_) {
    struct window *win = win_;
    (void)now_;
    win->inhibit_render_counter--;
    window_reset_delayed_redraw(win);
    return false;
}

void window_set_sync(struct window *win, bool state) {
    win->inhibit_render_counter -= poller_unset(&win->sync_update_timeout_timer);
    if (state) {
        poller_set_timer(&win->sync_update_timeout_timer, handle_sync_update_timeout, win, win->cfg.sync_time*1000L);
        win->inhibit_render_counter++;
    }
}

bool window_get_sync(struct window *win) {
    return win->sync_update_timeout_timer;
}

void window_set_autorepeat(struct window *win, bool state) {
    if (pvtbl->set_autorepeat)
        pvtbl->set_autorepeat(win, state);
    win->autorepeat = state;
}

bool window_get_autorepeat(struct window *win) {
    return win->autorepeat;
}

static bool handle_frame_timeout(void *win_, const struct timespec *now_) {
    struct window *win = win_;
    (void)now_;
    win->inhibit_render_counter--;
    return false;
}

void window_delay_redraw(struct window *win) {
    if (!win->redraw_delay_timer) {
        win->redraw_delay_timer = poller_add_timer(handle_frame_timeout, win, win->cfg.max_frame_time*1000LL);
        poller_set_autoreset(win->redraw_delay_timer, &win->redraw_delay_timer);
        win->inhibit_render_counter++;
    }
}

static bool handle_smooth_scroll(void *win_, const struct timespec *now_) {
    struct window *win = win_;
    (void)now_;
    dec_read_inhibit(win);
    window_reset_delayed_redraw(win);
    return false;
}

void window_request_scroll_flush(struct window *win) {
    window_reset_delayed_redraw(win);
    queue_force_redraw(win);
    if (!poller_set_timer(&win->smooth_scrooll_timer, handle_smooth_scroll,
                          win, win->cfg.smooth_scroll_delay*1000L)) {
        inc_read_inhibit(win);
   }
}

static bool handle_visual_bell(void *win_, const struct timespec *now_) {
    struct window *win = win_;
    (void)now_;
    term_set_reverse(win->term, win->init_invert);
    return false;
}

void window_bell(struct window *win, uint8_t vol) {
    if (!win->focused) {
        if (term_is_bell_raise_enabled(win->term))
            window_action(win, action_raise);
        if (term_is_bell_urgent_enabled(win->term))
            pvtbl->set_urgency(win, 1);
    }
    if (win->cfg.visual_bell) {
        if (!win->visual_bell_timer) {
            win->init_invert = term_is_reverse(win->term);
            win->visual_bell_timer = poller_add_timer(handle_visual_bell, win, win->cfg.visual_bell_time*1000L);
            poller_set_autoreset(win->visual_bell_timer, &win->visual_bell_timer);
            term_set_reverse(win->term, !win->init_invert);
        }
    } else if (vol) {
        pvtbl->bell(win, vol);
    }
}

void window_set_pointer_mode(struct window *win, enum hide_pointer_mode mode) {
    pvtbl->set_pointer_mode(win, mode);
}

void window_set_pointer_shape(struct window *win, const char *shape) {
    pvtbl->select_cursor(win, shape);
}

struct extent window_get_position(struct window *win) {
    return pvtbl->get_position(win);
}

struct extent window_get_grid_position(struct window *win) {
    struct extent res = pvtbl->get_position(win);
    res.width += win->cfg.border.left;
    res.height += win->cfg.border.top;
    return res;
}

struct extent window_get_grid_size(struct window *win) {
    return (struct extent) { win->char_width * win->cw, (win->char_height + win->char_depth) * win->ch };
}

struct extent window_get_screen_size(struct window *win) {
    return pvtbl->get_screen_size(win);
}

struct extent window_get_cell_size(struct window *win) {
    return (struct extent) { win->char_width, win->char_depth + win->char_height };
}

struct border window_get_border(struct window *win) {
    return win->cfg.border;
}

struct extent window_get_size(struct window *win) {
    return (struct extent) { win->cfg.geometry.r.width, win->cfg.geometry.r.height };
}

void window_get_title(struct window *win, enum title_target which, char **name, bool *utf8) {
    pvtbl->get_title(win, which, name, utf8);
}

void window_push_title(struct window *win, enum title_target which) {
    char *title = NULL, *icon = NULL;
    bool tutf8 = 0, iutf8 = 0;
    if (which & target_title) window_get_title(win, target_title, &title, &tutf8);
    if (which & target_icon_label) window_get_title(win, target_icon_label, &icon, &iutf8);

    size_t len = sizeof(struct title_stack_item), tlen = 0, ilen = 0;
    if (title) len += tlen = strlen(title) + 1;
    if (icon) len += ilen = strlen(icon) + 1;

    struct title_stack_item *new = xalloc(len);

    if (title) memcpy(new->title_data = new->data, title, tlen);
    else new->title_data = NULL;
    new->title_utf8 = tutf8;

    if (icon) memcpy(new->icon_data = new->data + tlen, icon, ilen);
    else new->icon_data = NULL;
    new->icon_utf8 = iutf8;

    new->next = win->title_stack;
    win->title_stack = new;

    free(title);
    free(icon);
}

void window_pop_title(struct window *win, enum title_target which) {
    struct title_stack_item *top = win->title_stack, *it;
    if (top) {
        if (which & target_title) {
            for (it = top; it && !it->title_data; it = it->next);
            if (it) pvtbl->set_title(win, it->title_data, it->title_utf8);
        }
        if (which & target_icon_label) {
            for (it = top; it && !it->icon_data; it = it->next);
            if (it) pvtbl->set_icon_label(win, it->icon_data, it->icon_utf8);
        }
        win->title_stack = top->next;
        free(top);
    }
}

static bool handle_blink(void *win_, const struct timespec *now_) {
    struct window *win = win_;
    (void)now_;
    win->rcstate.blink = !win->rcstate.blink;
    win->blink_commited = false;
    win->any_event_happend = true;
    return true;
}

static void reload_window(struct window *win) {
    int16_t w = win->cfg.geometry.r.width, h = win->cfg.geometry.r.height;

    char *cpath = win->cfg.config_path;
    win->cfg.config_path = NULL;

    init_instance_config(&win->cfg, cpath, false);

    win->cfg.geometry.r.width = w;
    win->cfg.geometry.r.height = h;

    window_set_alpha(win, win->cfg.alpha);
    term_reload_config(win->term);
    screen_damage_lines(term_screen(win->term), 0, win->ch);

    // FIXME Reset timers... (including mouse selection timer)
    // FIXME Check that we are adjusting read/render inhibitors on timers removal
    // asdf; Probably done???

    if (poller_unset(&win->smooth_scrooll_timer))
        dec_read_inhibit(win);
    if (poller_unset(&win->configure_delay_timer))
        dec_read_inhibit(win);

    win->inhibit_render_counter -= poller_unset(&win->sync_update_timeout_timer);
    window_reset_delayed_redraw(win);

    poller_unset(&win->read_delay_timer);
    poller_unset(&win->visual_bell_timer);
    poller_unset(&win->blink_timer);
    if (win->cfg.allow_blinking)
         poller_set_timer(&win->blink_timer, handle_blink, win, win->cfg.blink_time*1000L);

    pvtbl->reload_font(win, true);
    queue_force_redraw(win);
}

static void do_reload_config(void) {
    LIST_FOREACH_SAFE(it, &win_list_head)
        reload_window(CONTAINEROF(it, struct window, link));
    reload_config = false;
}

static void window_set_font(struct window *win, const char * name, int32_t size) {
    bool reload = name || size != win->cfg.font_size;
    if (name) {
        free(win->cfg.font_name);
        win->cfg.font_name = strdup(name);
    }

    if (size >= 0) win->cfg.font_size = size;

    if (reload) {
        pvtbl->reload_font(win, true);
        screen_damage_lines(term_screen(win->term), 0, win->ch);
        queue_force_redraw(win);
    }
}

void window_set_title(struct window *win, enum title_target which, const char *title, bool utf8) {
    if (!title) title = win->cfg.title;

    if (which & target_title) pvtbl->set_title(win, title, utf8);

    if (which & target_icon_label) pvtbl->set_icon_label(win, title, utf8);
}

struct window *window_find_shared_font(struct window *win, bool need_free, bool force_aligned) {
    bool found_font = 0, found_cache = 0;
    struct window *found = NULL;

    LIST_FOREACH_SAFE(it, &win_list_head) {
        struct window *src = CONTAINEROF(it, struct window, link);
        if ((src->cfg.font_size == win->cfg.font_size || (!win->cfg.font_size && src->cfg.font_size == ctx.font_size)) &&
                src->cfg.dpi == win->cfg.dpi && src->cfg.force_scalable == win->cfg.force_scalable &&
                src->cfg.allow_subst_font == win->cfg.allow_subst_font && src->cfg.gamma == win->cfg.gamma &&
                !strcmp(win->cfg.font_name, src->cfg.font_name) && src != win) {
            found_font = 1;
            found = src;
            if (win->font_pixmode == src->font_pixmode && win->cfg.font_spacing == src->cfg.font_spacing &&
                    win->cfg.line_spacing == src->cfg.line_spacing && win->cfg.override_boxdraw == src->cfg.override_boxdraw) {
                found_cache = 1;
                break;
            }
        }
    }

    struct font *newf = found_font ? font_ref(found->font) :
            create_font(win->cfg.font_name, win->cfg.font_size, win->cfg.dpi, win->cfg.gamma, win->cfg.force_scalable, win->cfg.allow_subst_font);
    if (!newf) {
        warn("Can't create new font: %s", win->cfg.font_name);
        return NULL;
    }

    struct glyph_cache *newc = found_cache ? glyph_cache_ref(found->font_cache) :
            create_glyph_cache(newf, win->cfg.pixel_mode, win->cfg.line_spacing, win->cfg.font_spacing,
                               win->cfg.underline_width, win->cfg.override_boxdraw, force_aligned);

    if (need_free) {
        free_glyph_cache(win->font_cache);
        free_font(win->font);
    }

    win->font = newf;
    win->font_cache = newc;
    win->undercurl_glyph = glyph_cache_fetch(win->font_cache, GLYPH_UNDERCURL, face_normal, NULL);
    win->cfg.font_size = font_get_size(newf);

    /* Initialize default font size */
    if (!ctx.font_size) ctx.font_size = win->cfg.font_size;

    glyph_cache_get_dim(win->font_cache, &win->char_width, &win->char_height, &win->char_depth);

    return found;
}

struct window *create_window(struct instance_config *cfg) {
    struct window *win = xzalloc(sizeof(struct window) + pvtbl->get_opaque_size());

    copy_config(&win->cfg, cfg);

    win->bg = win->cfg.palette[cfg->reverse_video ? SPECIAL_FG : SPECIAL_BG];
    win->cursor_fg = win->cfg.palette[cfg->reverse_video ? SPECIAL_CURSOR_BG : SPECIAL_CURSOR_FG];
    win->bg_premul = color_apply_a(win->bg, win->cfg.alpha);
    win->autorepeat = win->cfg.autorepeat;
    win->mapped = true;
    win->focused = true;

    if (!win->cfg.font_name) {
        free_window(win);
        return NULL;
    }

    if (!pvtbl->init_window(win)) goto error;

    if (!pvtbl->reload_font(win, false)) goto error;

    win->term = create_term(win, MAX(win->cw, 2), MAX(win->ch, 1));
    if (!win->term) goto error;

    win->rcstate = (struct render_cell_state) {
        .palette = term_palette(win->term),
    };

    if (win->cfg.allow_blinking) {
        win->blink_timer = poller_add_timer(handle_blink, win, win->cfg.blink_time*1000L);
        poller_set_autoreset(win->blink_timer, &win->blink_timer);
    }

    window_set_title(win, target_title | target_icon_label, NULL,
                     win->cfg.utf8 || win->cfg.force_utf8_title);

    list_insert_after(&win_list_head, &win->link);

    win->tty_event = poller_add_fd(handle_term_read, win, term_fd(win->term), POLLIN);
    if (!win->tty_event) goto error;

    pvtbl->map_window(win);
    return win;

error:
    warn("Can't create window");
    free_window(win);
    return NULL;
}


void free_window(struct window *win) {
    poller_unset(&win->tty_event);
    poller_unset(&win->frame_timer);
    poller_unset(&win->smooth_scrooll_timer);
    poller_unset(&win->blink_timer);
    poller_unset(&win->sync_update_timeout_timer);
    poller_unset(&win->visual_bell_timer);
    poller_unset(&win->configure_delay_timer);
    poller_unset(&win->read_delay_timer);
    poller_unset(&win->redraw_delay_timer);

    pvtbl->free_window(win);

    list_remove(&win->link);

    if (win->term)
        free_term(win->term);
    if (win->font_cache)
        free_glyph_cache(win->font_cache);
    if (win->font)
        free_font(win->font);

    for (size_t i = 0; i < clip_MAX; i++)
        free(win->clipped[i]);

    struct title_stack_item *tmp;
    while (win->title_stack) {
        tmp = win->title_stack->next;
        free(win->title_stack);
        win->title_stack = tmp;
    }

#if USE_URI
    uri_unref(win->rcstate.active_uri);
#endif

    free_config(&win->cfg);
    free(win);
}

bool window_submit_screen(struct window *win, int16_t cur_x, ssize_t cur_y, bool cursor, bool marg) {
    return pvtbl->submit_screen(win, cur_x, cur_y, cursor, marg);
}

void window_shift(struct window *win, int16_t ys, int16_t yd, int16_t height) {
    ys = MAX(0, MIN(ys, win->ch));
    yd = MAX(0, MIN(yd, win->ch));
    height = MIN(height, MIN(win->ch - ys, win->ch - yd));
    if (!height) return;

    ys = ys*(win->char_height + win->char_depth) + win->cfg.border.top;
    yd = yd*(win->char_height + win->char_depth) + win->cfg.border.top;
    height *= win->char_depth + win->char_height;

    int16_t xs = win->cfg.border.left;
    int16_t width = win->cw*win->char_width;

    pvtbl->copy(win, (struct rect){xs, yd, width, height}, xs, ys);
}

void handle_resize(struct window *win, int16_t width, int16_t height, bool artificial) {
    int16_t new_cw = MAX(2, (width - win->cfg.border.left - win->cfg.border.right)/win->char_width);
    int16_t new_ch = MAX(1, (height - win->cfg.border.top - win->cfg.border.bottom)/(win->char_height + win->char_depth));

    if (new_cw != win->cw || new_ch != win->ch) {
        /* First try to read from tty to empty out input queue since this is input
         * from an application that is not yet aware about the resize. Don't try reading
         * if it's a requested resize, since the application is already aware. */
        if (term_is_requested_resize(win->term)) {
            term_read(win->term);
            wait_for_configure(win, 2);
        }

        /* Notify application and delay window redraw expecting the application to redraw
         * itself to reduce visual artifacts. */
        term_notify_resize(win->term, width, height, new_cw, new_ch);
        window_delay_redraw(win);

        term_resize(win->term, new_cw, new_ch);
        window_delay_redraw_after_read(win);

        // FIXME: Hack! Need to move active size from geometry
        win->cfg.geometry.r.width = 0;
    }

    if (width != win->cfg.geometry.r.width || height != win->cfg.geometry.r.height)
        pvtbl->resize(win, width, height, new_cw, new_ch, artificial);

}

void handle_focus(struct window *win, bool focused) {
    win->focused = focused;
    term_handle_focus(win->term, focused);
}

void window_paste_clip(struct window *win, enum clip_target target) {
    pvtbl->paste(win, target);
}

static void clip_copy(struct window *win, bool uri) {
    const char *src = !uri ? (const char *)win->clipped[clip_primary] :
#if USE_URI
        uri_get(win->rcstate.active_uri);
#else
        NULL;
#endif
                ;
    uint8_t *dup;
    if (src && (dup = (uint8_t *)strdup(src))) {
        if (term_is_keep_clipboard_enabled(win->term)) {
            uint8_t *dup2 = (uint8_t *)strdup((char *)dup);
            free(win->clipboard);
            win->clipboard = dup2;
        }
        window_set_clip(win, dup, clip_clipboard);
    }
}

void handle_keydown(struct window *win, struct xkb_state *state, xkb_keycode_t keycode) {
    struct key key = keyboard_describe_key(state, keycode);

    if (key.sym == XKB_KEY_NoSymbol) return;

    enum shortcut_action action = keyboard_find_shortcut(&win->cfg, key);

    switch (action) {
    case shortcut_break:
        term_break(win->term);
        return;
    case shortcut_numlock:
        term_toggle_numlock(win->term);
        return;
    case shortcut_scroll_up:
        term_scroll_view(win->term, win->cfg.scroll_amount);
        return;
    case shortcut_scroll_down:
        term_scroll_view(win->term, -win->cfg.scroll_amount);
        return;
    case shortcut_font_up:
    case shortcut_font_down:
    case shortcut_font_default:;
        int32_t size = win->cfg.font_size;
        if (action == shortcut_font_up)
            size += win->cfg.font_size_step;
        else if (action == shortcut_font_down)
            size -= win->cfg.font_size_step;
        else if (action == shortcut_font_default)
            size = ctx.font_size;
        window_set_font(win, NULL, size);
        return;
    case shortcut_new_window:
        create_window(&win->cfg);
        return;
    case shortcut_copy:
        clip_copy(win, 0);
        return;
    case shortcut_copy_uri:
        clip_copy(win, 1);
        return;
    case shortcut_paste:
        window_paste_clip(win, clip_clipboard);
        return;
    case shortcut_reload_config:
        reload_window(win);
        return;
    case shortcut_reset:
        term_reset(win->term);
        return;
    case shortcut_reverse_video:
        term_set_reverse(win->term, !term_is_reverse(win->term));
        return;
    case shortcut_view_next_cmd:
        term_scroll_view_to_cmd(win->term, -1);
        return;
    case shortcut_view_prev_cmd:
        term_scroll_view_to_cmd(win->term, 1);
        return;
    case shortcut_MAX:
    case shortcut_none:;
    }

    keyboard_handle_input(key, win->term);
}


bool window_is_mapped(struct window *win) {
    return win->mapped;
}

static bool handle_frame(void *win_, const struct timespec *now_) {
    struct window *win = win_;
    (void)now_;
    win->inhibit_render_counter--;
    return false;
}

static void tick(void *arg, const struct timespec *now) {
    (void)arg, (void)now;

    // FIXME Move pvtbl->has_error() into handle events
    if ((!gconfig.daemon_mode && list_empty(&win_list_head)) || pvtbl->has_error()) {
        poller_stop();
        return;
    }

    if (reload_config)
        do_reload_config();

    /* Perform redraw here so that it happens after the read from the terminal */

    LIST_FOREACH(it, &win_list_head) {
        struct window *win = CONTAINEROF(it, struct window, link);

        if ((win->any_event_happend && !win->inhibit_render_counter) || win->force_redraw) {
            if ((win->drawn_somthing = screen_redraw(term_screen(win->term), win->blink_commited))) {
                win->inhibit_render_counter += !poller_set_timer(&win->frame_timer, handle_frame, win, SEC/win->cfg.fps);
                window_reset_delayed_redraw(win);
                if (gconfig.trace_misc) info("Redraw");
            }

            win->force_redraw = false;
            win->any_event_happend = false;
            win->blink_commited = true;
        }
    }

    pvtbl->flush();

}
