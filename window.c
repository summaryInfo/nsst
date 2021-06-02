/* Copyright (c) 2019-2021, Evgeny Baskov. All rights reserved */

#include "feature.h"

#include "term.h"
#include "input.h"
#include "mouse.h"
#include "window-x11.h"

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <poll.h>
#include <signal.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define NUM_BORDERS 4

struct context {
    double font_size;
    size_t vbell_count;
};

static struct context ctx;

struct window *win_list_head = NULL;
static volatile sig_atomic_t reload_config;

static void handle_sigusr1(int sig) {
    reload_config = 1;
    (void)sig;
}

_Noreturn static void handle_term(int sig) {
    for (struct window *win = win_list_head; win; win = win->next)
        term_hang(win->term);

    if (gconfig.daemon_mode)
        free_daemon();

    (void)sig;

    _exit(EXIT_SUCCESS);
}

void init_context(void) {
    init_poller();

    init_platform_context();
    init_render_context();

    sigaction(SIGUSR1, &(struct sigaction){ .sa_handler = handle_sigusr1, .sa_flags = SA_RESTART }, NULL);

    struct sigaction sa = { .sa_handler = handle_term };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
}

void free_context(void) {
    while (win_list_head)
        free_window(win_list_head);

    if (gconfig.daemon_mode)
        unlink(gconfig.sockpath);

    free_render_context();
    free_platform_context();
    free_poller();

    memset(&con, 0, sizeof(con));

#if USE_URI
    uri_release_memory();
#endif
}

struct instance_config *window_cfg(struct window *win) {
    return &win->cfg;
}

void window_set_colors(struct window *win, color_t bg, color_t cursor_fg) {
    color_t obg = win->bg_premul, ofg = win->cursor_fg;

    if (bg) {
        win->bg = bg;
        win->bg_premul = color_apply_a(bg, win->cfg.alpha);
    }
    if (cursor_fg) win->cursor_fg = cursor_fg;

    if (bg && win->bg_premul != obg)
        window_platform_update_colors(win);

    if ((bg && win->bg_premul != obg) || (cursor_fg && cursor_fg != ofg)) {
        // If reverse video is set via option
        // win->term can be NULL at this point
        if (win->term) term_damage_lines(win->term, 0, win->ch);
        win->force_redraw = 1;
    }
}

void window_set_mouse(struct window *win, bool enabled) {
#if USE_URI
    window_set_active_uri(win, EMPTY_URI, 0);
#endif
    window_platform_set_mouse(win, enabled);
}

void window_get_pointer(struct window *win, int16_t *px, int16_t *py, uint32_t *pmask) {
    int32_t x = 0, y = 0, mask = 0;
    window_platform_get_pointer(win, &x, &y, &mask);
    if (px) *px = x;
    if (py) *py = y;
    if (pmask) *pmask = mask;
}

void window_set_clip(struct window *win, uint8_t *data, uint32_t time, enum clip_target target) {
    if (target == clip_invalid) {
        warn("Invalid clipboard target");
        free(data);
        return;
    }
    if (data && !window_platform_set_clip(win, time, target)) {
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
        term_damage_uri(win->term, win->rcstate.active_uri);
        term_damage_uri(win->term, uri);
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

void window_get_dim(struct window *win, int16_t *width, int16_t *height) {
    if (width) *width = win->cfg.width;
    if (height) *height = win->cfg.height;
}

void window_set_sync(struct window *win, bool state) {
    if (state) clock_gettime(CLOCK_TYPE, &win->last_sync);
    win->sync_active = state;
}

void window_set_autorepeat(struct window *win, bool state) {
    win->autorepeat = state;
}

void window_delay_redraw(struct window *win) {
    if (!win->wait_for_redraw) clock_gettime(CLOCK_TYPE, &win->last_wait_start);
    win->wait_for_redraw = 1;
}

void window_request_scroll_flush(struct window *win) {
    clock_gettime(CLOCK_TYPE, &win->last_scroll);
    poller_enable(win->poll_index, 0);
    win->force_redraw = 1;
    win->wait_for_redraw = 0;
}

void window_bell(struct window *win, uint8_t vol) {
    if (!win->focused) {
        if (term_is_bell_raise_enabled(win->term)) window_action(win, action_restore_minimized);
        if (term_is_bell_urgent_enabled(win->term)) window_platform_set_urgency(win, 1);
    }
    if (win->cfg.visual_bell) {
        if (!win->in_blink) {
            win->init_invert = term_is_reverse(win->term);
            win->in_blink = 1;
            ctx.vbell_count++;
            clock_gettime(CLOCK_TYPE, &win->vbell_start);
            term_set_reverse(win->term, !win->init_invert);
        }
    } else if (vol) {
        window_platform_bell(win, vol);
    }
}

void window_get_dim_ext(struct window *win, enum window_dimension which, int16_t *width, int16_t *height) {
    int16_t x = 0, y = 0;
    switch (which) {
    case dim_window_position:
    case dim_grid_position:
        window_platform_get_position(win, &x, &y);
        if (which == dim_grid_position) {
            x += win->cfg.left_border;
            y += win->cfg.top_border;
        }
        break;
    case dim_grid_size:
        x = win->char_width * win->cw;
        y = (win->char_height + win->char_depth) * win->ch;
        break;
    case dim_screen_size:
        platform_context_get_screen_size(&x, &y);
        break;
    case dim_cell_size:
        x = win->char_width;
        y = win->char_depth + win->char_height;
        break;
    case dim_border:
        x = win->cfg.left_border;
        y = win->cfg.top_border;
        break;
    }

    if (width) *width = x;
    if (height) *height = y;
}

void window_push_title(struct window *win, enum title_target which) {
    char *title = NULL, *icon = NULL;
    bool tutf8 = 0, iutf8 = 0;
    if (which & target_title) window_get_title(win, target_title, &title, &tutf8);
    if (which & target_icon_label) window_get_title(win, target_icon_label, &icon, &iutf8);

    size_t len = sizeof(struct title_stack_item), tlen = 0, ilen = 0;
    if (title) len += tlen = strlen(title) + 1;
    if (icon) len += ilen = strlen(icon) + 1;

    struct title_stack_item *new = malloc(len);
    if (!new) {
        free(title);
        free(icon);
        return;
    }

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
            if (it) window_platform_set_title(win->wid, it->title_data, it->title_utf8);
        }
        if (which & target_icon_label) {
            for (it = top; it && !it->icon_data; it = it->next);
            if (it) window_platform_set_icon_label(win->wid, it->icon_data, it->icon_utf8);
        }
        win->title_stack = top->next;
        free(top);
    }
}

static void reload_window(struct window *win) {
    int16_t w = win->cfg.width, h = win->cfg.height;

    //TODO Reload more options here

    char *cpath = win->cfg.config_path;
    win->cfg.config_path = NULL;

    init_instance_config(&win->cfg, cpath, 0);

    win->cfg.width = w, win->cfg.height = h;

    window_set_alpha(win, win->cfg.alpha);

    renderer_reload_font(win, 1);
}

static void do_reload_config(void) {
    for (struct window *win = win_list_head; win; win = win->next)
        reload_window(win);
    reload_config = 0;
}

static void window_set_font(struct window *win, const char * name, int32_t size) {
    bool reload = name || size != win->cfg.font_size;
    if (name) {
        free(win->cfg.font_name);
        win->cfg.font_name = strdup(name);
    }

    if (size >= 0) win->cfg.font_size = size;

    if (reload) {
        renderer_reload_font(win, 1);
        term_damage_lines(win->term, 0, win->ch);
        win->force_redraw = 1;
    }
}

void window_set_title(struct window *win, enum title_target which, const char *title, bool utf8) {
    if (!title) title = win->cfg.title;

    if (which & target_title) window_platform_set_title(win->wid, title, utf8);

    if (which & target_icon_label) window_platform_set_icon_label(win->wid, title, utf8);
}

struct window *find_shared_font(struct window *win, bool need_free) {
    bool found_font = 0, found_cache = 0;
    struct window *found = NULL;

    for (struct window *src = win_list_head; src; src = src->next) {
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
            create_glyph_cache(newf, win->cfg.pixel_mode, win->cfg.line_spacing, win->cfg.font_spacing, win->cfg.override_boxdraw);

    if (need_free) {
        free_glyph_cache(win->font_cache);
        free_font(win->font);
    }

    win->font = newf;
    win->font_cache = newc;
    win->cfg.font_size = font_get_size(newf);

    //Initialize default font size
    if (!ctx.font_size) ctx.font_size = win->cfg.font_size;

    glyph_cache_get_dim(win->font_cache, &win->char_width, &win->char_height, &win->char_depth);

    return found;
}


struct window *create_window(struct instance_config *cfg) {
    struct window *win = calloc(1, sizeof(struct window));
    if (!win) {
        free(win);
        return NULL;
    }

    copy_config(&win->cfg, cfg);

    win->bg = win->cfg.palette[cfg->reverse_video ? SPECIAL_FG : SPECIAL_BG];
    win->cursor_fg = win->cfg.palette[cfg->reverse_video ? SPECIAL_CURSOR_BG : SPECIAL_CURSOR_FG];
    win->bg_premul = color_apply_a(win->bg, win->cfg.alpha);
    win->autorepeat = win->cfg.autorepeat;
    win->active = 1;
    win->focused = 1;

    if (!win->cfg.font_name) {
        free_window(win);
        return NULL;
    }

    if (!init_platform_window(win)) goto error;

    if (!renderer_reload_font(win, 0)) goto error;

    win->term = create_term(win, MAX(win->cw, 2), MAX(win->ch, 1));
    win->rcstate = (struct render_cell_state) {
        .palette = term_palette(win->term),
    };
    if (!win->term) goto error;

    window_set_title(win, target_title | target_icon_label, NULL, win->cfg.utf8);

    win->next = win_list_head;
    win->prev = NULL;
    if (win_list_head) win_list_head->prev = win;
    win_list_head = win;

    win->poll_index = poller_alloc_index(term_fd(win->term), POLLIN | POLLHUP);
    if (win->poll_index < 0) goto error;

    window_platform_map(win);
    return win;

error:
    warn("Can't create window");
    free_window(win);
    return NULL;
}


void free_window(struct window *win) {
    free_platform_window(win);

    // Decrement count of currently blinking
    // windows if window gets freed during blink
    if (win->in_blink) ctx.vbell_count--;

    if (win->next) win->next->prev = win->prev;
    if (win->prev) win->prev->next = win->next;
    else win_list_head =  win->next;

    if (win->poll_index > 0)
        poller_free_index(win->poll_index);
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


bool window_shift(struct window *win, int16_t xs, int16_t ys, int16_t xd, int16_t yd, int16_t width, int16_t height, bool delay) {
    static struct timespec cur ALIGNED(16);
    clock_gettime(CLOCK_TYPE, &cur);

    bool scrolled_recently = TIMEDIFF(win->last_shift, cur) <  SEC/2/win->cfg.fps;
    win->last_shift = cur;
    if (delay && scrolled_recently) return 0;

    ys = MAX(0, MIN(ys, win->ch));
    yd = MAX(0, MIN(yd, win->ch));
    xs = MAX(0, MIN(xs, win->cw));
    xd = MAX(0, MIN(xd, win->cw));
    height = MIN(height, MIN(win->ch - ys, win->ch - yd));
    width = MIN(width, MIN(win->cw - xs, win->cw - xd));

    if (!height || !width) return 1;

    ys *= win->char_height + win->char_depth;
    yd *= win->char_height + win->char_depth;
    xs *= win->char_width;
    xd *= win->char_width;
    height *= win->char_depth + win->char_height;
    width *= win->char_width;

    renderer_copy(win, (struct rect){xd, yd, width, height}, xs, ys);

    return 1;
}

static void redraw_borders(struct window *win, bool top_left, bool bottom_right) {
        int16_t width = win->cw * win->char_width + win->cfg.left_border;
        int16_t height = win->ch * (win->char_height + win->char_depth) + win->cfg.top_border;
        struct rect borders[NUM_BORDERS] = {
            {0, 0, win->cfg.left_border, height},
            {win->cfg.left_border, 0, width, win->cfg.top_border},
            {width, 0, win->cfg.width - width, win->cfg.height},
            {0, height, width, win->cfg.height - height},
        };
        size_t count = 4, offset = 0;
        if (!top_left) count -= 2, offset += 2;
        if (!bottom_right) count -= 2;

        //TODO Handle zero height
        window_platform_draw_rectangles(win, borders + offset, count);
}

void handle_expose(struct window *win, struct rect damage) {
    int16_t width = win->cw * win->char_width + win->cfg.left_border;
    int16_t height = win->ch * (win->char_height + win->char_depth) + win->cfg.top_border;

    size_t num_damaged = 0;
    struct rect damaged[NUM_BORDERS], borders[NUM_BORDERS] = {
        {0, 0, win->cfg.left_border, height},
        {win->cfg.left_border, 0, width, win->cfg.top_border},
        {width, 0, win->cfg.width - width, win->cfg.height},
        {0, height, width, win->cfg.height - height},
    };
    for (size_t i = 0; i < NUM_BORDERS; i++)
        if (intersect_with(&borders[i], &damage))
                damaged[num_damaged++] = borders[i];

    // TODO Include borders in window
    window_platform_draw_rectangles(win, damaged, num_damaged);

    struct rect inters = { 0, 0, width - win->cfg.left_border, height - win->cfg.top_border};
    damage = rect_shift(damage, -win->cfg.left_border, -win->cfg.top_border);
    if (intersect_with(&inters, &damage)) renderer_update(win, inters);
}

void handle_resize(struct window *win, int16_t width, int16_t height) {
    //Handle resize

    win->cfg.width = width;
    win->cfg.height = height;

    int16_t new_cw = MAX(2, (win->cfg.width - 2*win->cfg.left_border)/win->char_width);
    int16_t new_ch = MAX(1, (win->cfg.height - 2*win->cfg.top_border)/(win->char_height+win->char_depth));
    int16_t delta_x = new_cw - win->cw;
    int16_t delta_y = new_ch - win->ch;

    if (delta_x || delta_y) {
        term_resize(win->term, new_cw, new_ch);
        renderer_resize(win, new_cw, new_ch);
        clock_gettime(CLOCK_TYPE, &win->last_read);
        window_delay_redraw(win);
    }

    if (delta_x < 0 || delta_y < 0)
        redraw_borders(win, 0, 1);
}

void handle_focus(struct window *win, bool focused) {
    win->focused = focused;
    term_handle_focus(win->term, focused);
}

static void clip_copy(struct window *win, bool uri) {
    const char *src = uri ? uri_get(win->rcstate.active_uri) :
                (const char *)win->clipped[clip_primary];
    uint8_t *dup;
    if (src && (dup = (uint8_t *)strdup(src))) {
        if (term_is_keep_clipboard_enabled(win->term)) {
            uint8_t *dup2 = (uint8_t *)strdup((char *)dup);
            free(win->clipboard);
            win->clipboard = dup2;
        }
        window_set_clip(win, dup, CLIP_TIME_NOW, clip_clipboard);
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
    case shortcut_MAX:
    case shortcut_none:;
    }

    keyboard_handle_input(key, win->term);
}


void window_paste_data(struct window *win, uint8_t *data, ssize_t size, bool utf8, bool is_first, bool is_last) {
    static uint8_t leftover[3], leftover_len;
    static uint8_t buf1[2*PASTE_BLOCK_SIZE];
    static uint8_t buf2[4*PASTE_BLOCK_SIZE];

    uint8_t *pos = data, *end = data + size;

    if (!term_is_paste_nl_enabled(win->term))
        while ((pos = memchr(pos, '\n', end - pos))) *pos++ = '\r';

    if (size) {
        if (is_first) {
            leftover_len = 0;
            term_paste_begin(win->term);
        }

        if (utf8 ^ term_is_utf8_enabled(win->term)) {
            pos = data;
            size = 0;
            if (utf8) {
                uint32_t ch;
                while (pos < end)
                    if (utf8_decode(&ch, (const uint8_t **)&pos, end))
                        buf1[size++] = ch;
            } else {
                while (pos < end)
                    size += utf8_encode(*pos++, buf1 + size, buf1 + sizeof buf1/sizeof *buf1);
            }

            data = buf1;
        }

        if (term_is_paste_requested(win->term)) {
            while (leftover_len < 3 && size) leftover[leftover_len++] = *data++, size--;
            size_t pre = base64_encode(buf2, leftover, leftover + leftover_len) - buf2;

            if (size) {
                if (!is_last) {
                    leftover_len = size % 3;
                    if (leftover_len > 0) leftover[0] = data[size - leftover_len], size--;
                    if (leftover_len > 1) leftover[1] = data[size - 1], size--;
                }

                size = base64_encode(buf2 + pre, data, data + size) - buf2;
            }
            data = buf2;
        } else if (term_is_paste_quote_enabled(win->term)) {
            bool quote_c1 = !term_is_utf8_enabled(win->term);
            ssize_t i = 0, j = 0;
            while (i < size) {
                // Prefix control symbols with Ctrl-V
                if (buf1[i] < 0x20 || buf1[i] == 0x7F ||
                        (quote_c1 && buf1[i] > 0x7F && buf1[i] < 0xA0))
                    buf2[j++] = 0x16;
                buf2[j++] = buf1[i++];
            }
            size = j;
            data = buf2;
        }

        term_sendkey(win->term, data, size);

        if (is_last) term_paste_end(win->term);
    }
}

bool window_is_mapped(struct window *win) {
    return win->active;
}

/* Start window logic, handling all windows in context */
void run(void) {
    for (int64_t next_timeout = SEC;;) {
        poller_poll(next_timeout);

        // First check window system events
        handle_event();

        // Reload config if requested
        if (reload_config) do_reload_config();

        // Process connecting clients
        daemon_process_clients();

        next_timeout = 30*SEC;
        struct timespec cur ALIGNED(16);
        clock_gettime(CLOCK_TYPE, &cur);

        // Then read for PTYs
        for (struct window *win = win_list_head, *next; win; win = next) {
            next = win->next;
            int evt = poller_index_events(win->poll_index);
            if (UNLIKELY(evt & (POLLERR | POLLNVAL | POLLHUP))) {
                free_window(win);
            } else {
                bool need_read = evt & POLLIN;
                // If we requested flush scroll, pty fd got disabled from polling
                // to prevent active waiting loop. If smooth scroll timeout got expired
                // we can enable it back and attempt to read from pty.
                // If there is nothing to read it won't block since O_NONBLOCK is set for ptys
                if (!need_read && !poller_is_enabled(win->poll_index) && TIMEDIFF(win->last_scroll, cur) > win->cfg.smooth_scroll_delay*1000LL) {
                    poller_enable(win->poll_index, 1);
                    need_read = 1;
                }
                if (need_read && term_read(win->term)) {
                    win->last_read = cur;
                    win->any_event_happend = 1;
                }
                if (win->wait_for_redraw) {
                    // If we are waiting for the frame to finish, we need to
                    // reduce poll timeout
                    int64_t diff = (win->cfg.frame_finished_delay + 1)*1000LL - TIMEDIFF(win->last_read, cur);
                    if (win->wait_for_redraw &= diff > 0 && win->active) next_timeout = MIN(next_timeout, diff);
                }
            }
        }

        for (struct window *win = win_list_head; win; win = win->next) {
            next_timeout = MIN(next_timeout, (win->in_blink ? win->cfg.visual_bell_time : win->cfg.blink_time)*1000LL);

            // Scroll down selection
            bool pending_scroll = mouse_pending_scroll(win->term);

            // Change blink state if blinking interval is expired
            if (win->active && win->cfg.allow_blinking &&
                    TIMEDIFF(win->last_blink, cur) > win->cfg.blink_time*1000LL) {
                win->rcstate.blink = !win->rcstate.blink;
                win->blink_commited = 0;
                win->last_blink = cur;
            }

            if (!win->any_event_happend && !pending_scroll && win->blink_commited) continue;

            // Deactivate syncronous update mode if it has expired
            if (UNLIKELY(win->sync_active) && TIMEDIFF(win->last_sync, cur) > win->cfg.sync_time*1000LL)
                win->sync_active = 0, win->wait_for_redraw = 0;

            // Reset revert if visual blink duration finished
            if (UNLIKELY(win->in_blink) && TIMEDIFF(win->vbell_start, cur) > win->cfg.visual_bell_time*1000LL) {
                term_set_reverse(win->term, win->init_invert);
                win->in_blink = 0;
                ctx.vbell_count--;
            }

            // We need to skeep frame if redraw is not forced
            // and ether syncronous update is active, window is not visible
            // or we are waiting for frame to finish and maximal frame time is not expired
            if (!win->force_redraw && !pending_scroll) {
                if (UNLIKELY(win->sync_active || !win->active)) continue;
                if (win->wait_for_redraw) {
                    if (TIMEDIFF(win->last_wait_start, cur) < win->cfg.max_frame_time*1000LL) continue;
                    else win->wait_for_redraw = 0;
                }
            }

            int64_t frame_time = SEC / win->cfg.fps;
            int64_t remains = frame_time - TIMEDIFF(win->last_draw, cur);

            if (remains <= 10000LL || win->force_redraw || pending_scroll) {
                if (win->force_redraw) redraw_borders(win, 1, 1);

                remains = frame_time;
                if ((win->drawn_somthing = term_redraw(win->term, win->blink_commited))) win->last_draw = cur;

                if (gconfig.trace_misc && win->drawn_somthing) info("Redraw");

                // If we haven't been drawn anything
                // increase poll timeout
                win->slow_mode = !win->drawn_somthing;

                win->force_redraw = 0;
                win->any_event_happend = 0;
                win->blink_commited = 1;
            }

            if (!win->slow_mode) next_timeout = MIN(next_timeout,  remains);
            if (pending_scroll) next_timeout = MIN(next_timeout, win->cfg.select_scroll_time*1000LL);
        }

        next_timeout = MAX(0, next_timeout);
        xcb_flush(con);

        if ((!gconfig.daemon_mode && !win_list_head) || platform_context_has_error()) break;
    }
}
