/* Copyright (c) 2019-2022,2025, Evgeniy Baskov. All rights reserved */

#define _DEFAULT_SOURCE

#include "feature.h"

#include "screen.h"
#include "tty.h"

#include <assert.h>
#include <stdio.h>
#include <strings.h>

#ifdef __AVX__
#include <immintrin.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif

#define CBUF_STEP(c,m) ((c) ? MIN(4 * (c) / 3, m) : MIN(16, m))
#define PRINT_BLOCK_SIZE 256

static inline bool screen_at_bottom(struct screen *scr) {
    return line_span_cmpeq(&scr->view_pos.s, scr->screen);
}

static inline struct screen_storage *get_current_screen(struct screen *scr) {
    return scr->mode.altscreen ? &scr->alt_screen : &scr->main_screen;
}

static inline void free_line_list_until(struct screen *scr, struct screen_storage *screen, struct line *line, struct line *until) {
    struct line *next;
    while (line != until) {
        next = line->next;
        if (line->selection_index)
            selection_clear(&scr->sstate);
        free_line(screen, line);
        line = next;
    }
}

void free_screen(struct screen *scr) {
    free_printer(&scr->printer);
    free_selection(&scr->sstate);

#if USE_URI
    uri_unref(scr->sgr.uri);
    uri_unref(scr->saved_sgr.uri);
    uri_unref(scr->back_saved_sgr.uri);
#endif

    free_line_list_until(scr, &scr->main_screen, scr->top_line.s.line, NULL);
    free_line_list_until(scr, &scr->alt_screen, scr->alt_screen.begin->line, NULL);

    free(scr->main_screen.begin);
    free(scr->alt_screen.begin);
    free(scr->temp_screen);

    free(scr->tabs);
    free(scr->predec_buf);

    mpa_release(&scr->main_screen.pool);
    mpa_release(&scr->alt_screen.pool);
}


/* Damage terminal screen, relative to view
 * Faster version for whole lines */
void screen_damage_lines(struct screen *scr, ssize_t ys, ssize_t yd) {
    struct line_span view = screen_view(scr);
    screen_span_shift_n(scr, &view, ys);
    for (ssize_t i = ys; i < yd; i++, screen_span_shift(scr, &view))
        view.line->force_damage = true;
}

void screen_damage_selection(struct screen *scr) {
    struct line_span view = screen_view(scr);
    struct line *prev = NULL;
    for (ssize_t i = 0; i < scr->height; i++) {
        if (prev != view.line)
            selection_damage(&scr->sstate, view.line);
        prev = view.line;
        screen_span_shift(scr, &view);
    }
}

void screen_damage_uri(struct screen *scr, uint32_t uri) {
    if (!uri) return;

    struct line_span view = screen_view(scr);
    for (ssize_t i = 0; i < scr->height; screen_span_shift(scr, &view), i++) {
        struct line_attr *attrs = view.line->attrs;
        if (!attrs) continue;

        bool has_uri = false;
        for (ssize_t j = 0; j < attrs->caps; j++) {
            if (attrs->data[j].uri == uri) {
                has_uri = true;
                break;
            }
        }
        if (!has_uri) continue;

        screen_span_width(scr, &view);

        struct cell *cel = view_cell(&view, 0), *end = view_cell(&view, view.width);
        for (; cel < end; cel++)
            if (view_attr(&view, cel->attrid)->uri == uri)
                cel->drawn = 0;
    }
}

void screen_span_width(struct screen *scr, struct line_span *pos) {
    pos->width = line_advance_width(pos->line, pos->offset, scr->width) - pos->offset;
}

ssize_t screen_span_shift(struct screen *scr, struct line_span *pos) {
    return line_span_shift(pos, scr->width);
}

ssize_t screen_span_shift_n(struct screen *scr, struct line_span *pos, ssize_t amount) {
    return line_shift_n(pos, amount, scr->width);
}

struct line_span screen_view(struct screen *scr) {
    return scr->view_pos.s;
}

struct line_span screen_top(struct screen *scr) {
    return scr->top_line.s;
}

struct line_span screen_span(struct screen *scr, ssize_t y) {
#if DEBUG_LINES
    assert(y >= 0);
    assert(y < scr->height);
#endif
    return scr->screen[y];
}

void screen_reset_view(struct screen *scr, bool damage) {
    if (!screen_at_bottom(scr)) {
        scr->prev_c_view_changed = true;
        replace_handle(&scr->view_pos, scr->screen);
        selection_view_scrolled(&scr->sstate, scr);

        if (damage)
            screen_damage_lines(scr, 0, scr->height);
    }
}

FORCEINLINE
static inline void screen_split_line_after_ex(struct screen *scr, struct screen_storage *screen, ssize_t y) {
    struct line_span *src = screen->begin + y;
    ssize_t offset = src->offset + src->width;
    if (LIKELY(src + 1 >= screen->end || src[1].line != src->line)) return;

    assert(offset <= src->line->size);

    split_line(screen, src->line, offset);
    if (UNLIKELY(src->line->selection_index))
        selection_split(&scr->sstate, src->line, src->line->next);
}

FORCEINLINE
static inline void screen_split_line_before(struct screen *scr, ssize_t y) {
    struct line_span *src = &scr->screen[y];
    if (LIKELY(!src->offset)) return;

    split_line(get_current_screen(scr), src->line, src->offset);
    if (UNLIKELY(src->line->selection_index))
        selection_split(&scr->sstate, src->line, src->line->next);
}

FORCEINLINE
static inline void screen_split_line_after(struct screen *scr, ssize_t y) {
    screen_split_line_after_ex(scr, get_current_screen(scr), y);
}

static inline struct line *screen_realloc_line(struct screen *scr, struct line *line, ssize_t width, struct screen_storage *screen) {
    struct line *new = realloc_line(screen, line, width);

    if (UNLIKELY(new->selection_index))
        selection_relocated(&scr->sstate, new);

    return new;
}

void screen_cursor_line_set_prompt(struct screen *scr) {
    struct line *line = scr->screen[scr->c.y].line;
    line->sh_ps1_start = true;
}

void screen_cursor_line_set_cmd_start(struct screen *scr) {
    struct line *line = scr->screen[scr->c.y].line;
    if (screen_cursor_x(scr) <= screen_min_x(scr))
        line = line->prev;
    if (line)
        line->sh_cmd_start = true;
}

static inline void screen_adjust_line_ex(struct screen *scr, struct screen_storage *screen, ssize_t y, ssize_t clear_to, ssize_t size) {
    struct line_span *view = &screen->begin[y];
    ssize_t old_size = view->line->size;
    ssize_t new_size = view->offset + size;
    clear_to += view->offset;

    if (LIKELY(old_size >= new_size)) return;

    if (new_size > view->line->caps)
        screen_realloc_line(scr, view->line, new_size, screen);

    if (clear_to > old_size) {
        struct cell c = MKCELL(0, view->line->pad_attrid);
        fill_cells(view->line->cell + old_size, c, clear_to - old_size);
    }

#if DEBUG_LINES
    /* When we are resizing continuation line we need
     * to ensure all wrapped parts are withing the line (sic)
     * and are wrapped to the screen width. */
    if (UNLIKELY(view->offset) && y > 0) {
        assert(screen->begin[y].offset <= old_size);
        while (--y > 0 && screen->begin[y].line == view->line)
            assert((uintptr_t)(scr->width - screen->begin[y].width) <= 1);
    }
#endif

    view->width = size;
    view->line->size = new_size;
}

static inline void screen_adjust_line(struct screen *scr, ssize_t y, ssize_t size) {
    screen_adjust_line_ex(scr, get_current_screen(scr), y, size, size);
}

static inline void screen_wrap(struct screen *scr, bool hard) {
    screen_autoprint(scr);
    bool moved = screen_index(scr);
    screen_cr(scr);

    if (hard) return;
    if (scr->mode.altscreen) return;

    /* If we have not scrolled and did not create new line
     * we need to avoid fancy re-wrapping behaviour.
     * Same thing for left-right magrins set */

    if (!moved || screen_min_x(scr) || screen_max_x(scr) != scr->width) return;

    /* If there's no previous line, we cannot soft wrap */
    if (!scr->screen[scr->c.y].line->prev) return;

    /* If this and the next line are already the same,
     * avoid reallocation and do nothing. */
    // FIXME: Check if this can actually happend right now and make it possible if not.

    if (scr->screen[scr->c.y].offset) return;

    /* If paragraph is too long, force hard wrap */

    if (scr->screen[scr->c.y].line->prev->size +
            scr->screen[scr->c.y].line->size > MAX_LINE_LEN) {
        scr->screen[scr->c.y].line->prev->wrapped = true;
        return;
    }

    /* Otherwise if everything is fine and we can wrap */

    if (scr->c.y > 0)
        screen_adjust_line(scr, scr->c.y - 1, scr->width);

    struct line *current = scr->screen[scr->c.y].line;

    selection_concat(&scr->sstate, current->prev, current);

    struct line *new = concat_line(&scr->main_screen, current->prev, current);
    if (UNLIKELY(new->selection_index))
        selection_relocated(&scr->sstate, new);
}

void screen_ensure_new_paragaph(struct screen *scr) {
    if (screen_cursor_x(scr) > screen_min_x(scr))
        screen_wrap(scr, true);
    screen_split_line_before(scr, scr->c.y);
}

void screen_free_scrollback(struct screen *scr, ssize_t max_size) {
    struct line_span *screen_top = scr->main_screen.begin;
    if (screen_top) {

        if (screen_top->line->prev)
            screen_reset_view(scr, false);

        free_line_list_until(scr, &scr->main_screen, scr->top_line.s.line, screen_top->line);

        replace_handle(&scr->top_line, &(struct line_span){ .line = screen_top->line });
    }

    scr->sb_max_caps = max_size;
    scr->sb_limit = 0;
}

void screen_scroll_view_to_cmd(struct screen *scr, int16_t amount) {

    amount = -amount;

    bool old_viewr = line_span_cmpeq(&scr->view_pos.s, scr->screen);
    /* Shortcut for the case when view is already at the bottom */
    if (old_viewr && amount > 0) return;

    struct line *line = scr->view_pos.s.line;
    if (amount > 0) {
        do line = line->next;
        while (line && !line->sh_ps1_start && line != scr->screen->line);
    } else {
        while (line->prev && !line->sh_ps1_start)
            line = line->prev;
    }

    if (!line) return;

    replace_handle(&scr->view_pos, &(struct line_span) { .line = line });
    scr->scroll_damage = true;

    selection_view_scrolled(&scr->sstate, scr);

    bool new_viewr = line_span_cmpeq(&scr->view_pos.s, scr->screen);
    scr->prev_c_view_changed |= old_viewr != new_viewr;
}

void screen_scroll_view(struct screen *scr, int16_t amount) {
    if (scr->mode.altscreen || !scr->sb_max_caps) return;

    amount = -amount;

    bool old_viewr = line_span_cmpeq(&scr->view_pos.s, scr->screen);
    /* Shortcut for the case when view is already at the bottom */
    if (old_viewr && amount > 0) return;

    line_handle_remove(&scr->view_pos);
    ssize_t delta = screen_span_shift_n(scr, &scr->view_pos.s, amount) - amount;
    line_handle_add(&scr->view_pos);
    int new_viewr = line_span_cmp(&scr->view_pos.s, scr->screen);
    if (new_viewr > 0) {
        screen_reset_view(scr, true);
    } else {
        if (delta > 0) /* View down, image up */ {
            window_shift(scr->win, 0, delta, scr->height - delta);
            screen_damage_lines(scr, 0, delta);
        } else if (delta < 0) /* View down, image up */ {
            window_shift(scr->win, -delta, 0, scr->height + delta);
            screen_damage_lines(scr, scr->height + delta, scr->height);
        }
    }

    selection_view_scrolled(&scr->sstate, scr);
    scr->prev_c_view_changed |= old_viewr != !!new_viewr;
}

HOT
bool free_extra_lines(struct screen *scr) {
    ssize_t extra = scr->sb_limit - scr->sb_max_caps;
    if (extra <= 0) return false;

    scr->sb_limit -= extra;

    bool view_moved = false;
    struct line *next = scr->top_line.s.line;

#if DEBUG_LINES
    assert(find_handle_in_line(&scr->top_line));
    assert(!scr->top_line.s.line->prev);
#endif

    for (ssize_t i = 0; i < extra; i++) {
        struct line *top = next;
        next = top->next;

        if (UNLIKELY(top == scr->main_screen.begin->line)) {
            line_handle_remove(&scr->top_line);
            next = scr->main_screen.begin->line;
            goto finish;
        }
        if (UNLIKELY(top == scr->view_pos.s.line))
            view_moved |= line_segments(top, scr->view_pos.s.offset, scr->width);
        if (UNLIKELY(top->selection_index))
            selection_clear(&scr->sstate);

        free_line(&scr->main_screen, top);
    }

finish:
#if DEBUG_LINES
    assert(next);
#endif

    scr->top_line.s.line = next;
    scr->top_line.s.offset = 0;
    line_handle_add(&scr->top_line);

    return view_moved;
}

static inline void push_history_until(struct screen *scr, struct line *from, struct line *to) {
    if (LIKELY(from->seq < to->seq)) {
        /* Push to history */
        for (; from->seq < to->seq; from = from->next) {
            optimize_line(&scr->main_screen, from);
            scr->sb_limit++;
        }
    } else {
        /* Pull from history */
        for (; to->seq > from->seq; to = to->next)
            scr->sb_limit--;
    }

#if DEBUG_LINES
    assert(scr->sb_limit >= 0);
#endif
}

static void resize_tabs(struct screen *scr, int16_t width) {
    scr->tabs = xrezalloc(scr->tabs, scr->width * sizeof *scr->tabs, width * sizeof *scr->tabs);

    if (width > scr->width) {
        ssize_t tab = scr->width ? scr->width - 1: 0, tabw = window_cfg(scr->win)->tab_width;
        while (tab > 0 && !scr->tabs[tab]) tab--;
        while ((tab += tabw) < width) scr->tabs[tab] = 1;
    }
}

static inline struct line *create_lines_range(struct screen_storage *screen, struct line *prev, struct line *next,
                                              ssize_t y, ssize_t width, const struct attr *attr,
                                              ssize_t count, struct line_handle *top) {
    if (count <= 0) return NULL;

    struct line *line = create_line(screen, attr, width);
    if (UNLIKELY(!prev && top))
        replace_handle(top, &(struct line_span) { .line =  line });

    struct line_span *dst = screen->begin + y;

    *dst = (struct line_span) { .line = line };
    attach_prev_line(line, prev);
    prev = line;

    if (UNLIKELY(count > 1)) {
        ssize_t onscreen = MIN(count, screen->end - dst);
        if (onscreen > 1)
            memset(dst + 1, 0, (onscreen - 1)*sizeof *dst);
        ssize_t i = 1;
        do {
            line = create_line(screen, attr, width);
            if (i < onscreen)
                dst[i].line = line;
            attach_prev_line(line, prev);
            prev = line;
        } while (++i < count);
    }

    attach_next_line(line, next);

    return line;
}

static void resize_altscreen(struct screen *scr, ssize_t width, ssize_t height) {
    struct line_span *screen = scr->alt_screen.begin;
    ssize_t minh = MIN(scr->height, height);

    if (height < scr->height)
        free_line_list_until(scr, &scr->alt_screen, screen[height].line, NULL);

    static_assert(MALLOC_ALIGNMENT == _Alignof(struct line_handle), "Insufficient alignment");
    screen = xrealloc(screen, scr->height * sizeof *screen, height * sizeof *screen);
    scr->alt_screen.begin = screen;
    scr->alt_screen.end = screen + MIN(height, scr->height);
    if (scr->mode.altscreen) scr->screen = screen;

    for (ssize_t i = 0; i < minh; i++) {
        screen[i].line = screen_realloc_line(scr, screen[i].line, width, &scr->alt_screen);
        screen[i].width = MIN(screen[i].width, width);
    }

    scr->alt_screen.end = screen + height;

    if (scr->height < height) {
        create_lines_range(&scr->alt_screen, scr->height ? screen[scr->height - 1].line : NULL, NULL,
                           scr->height, width, &ATTR_DEFAULT, height - scr->height, NULL);
    }

    /* Adjust altscreen saved cursor position */

    struct cursor *c = scr->mode.altscreen ? &scr->saved_c : &scr->back_saved_c;

    c->x = MIN(MAX(c->x, 0), width - 1);
    c->y = MIN(MAX(c->y, 0), height - 1);
    if (c->pending) c->x = width - 1;

    if (scr->mode.altscreen) {
        scr->c.x = MIN(MAX(scr->c.x, 0), width - 1);
        scr->c.y = MIN(MAX(scr->c.y, 0), height - 1);
        if (scr->c.pending) scr->c.x = width - 1;
    }
}

static void resize_aux(struct screen *scr, ssize_t width, ssize_t height) {
    /* Resize predecode buffer */
    size_t old_pbuf_size = ROUNDUP((2 + scr->width)*sizeof *scr->predec_buf, MPA_ALIGNMENT);
    size_t new_pbuf_size = ROUNDUP((2 + width)*sizeof *scr->predec_buf, MPA_ALIGNMENT);
    scr->predec_buf = xrealloc(scr->predec_buf, old_pbuf_size, new_pbuf_size);

    /* Resize temporary screen buffer */
    scr->temp_screen = xrealloc(scr->temp_screen, scr->height * sizeof *scr->temp_screen, height * sizeof *scr->temp_screen);

}

enum stick_view {
    stick_to_top,
    stick_to_bottom,
    stick_none,
};

static void fixup_view(struct screen *scr, struct line_handle *lower_left, enum stick_view stick) {
#if DEBUG_LINES
    if (scr->mode.altscreen)
        assert(stick == stick_to_bottom);
#endif

    switch (stick) {
    case stick_to_bottom:
        replace_handle(&scr->view_pos, scr->screen);
        break;
    case stick_to_top:
        replace_handle(&scr->view_pos, &scr->top_line.s);
        break;
    case stick_none:
        /* Keep line of lower left view cell at the bottom */
        line_handle_remove(&scr->view_pos);
        scr->view_pos.s = lower_left->s;
        scr->view_pos.s.offset -= scr->view_pos.s.offset % scr->width;
        screen_span_shift_n(scr, &scr->view_pos.s, 1 - scr->height);
        line_handle_add(&scr->view_pos);
    }

#if DEBUG_LINES
    assert(scr->view_pos.s.line->seq <= scr->screen->line->seq);
#endif
}

static inline void translate_screen_position(struct line_span *first, struct line_span *pos, struct cursor *c, ssize_t width) {
    struct line_span it = *first;

    if (line_span_cmp(&it, pos) > 0) {
        c->y = MAX(c->y, 0);
        if (c->pending) c->x = width - 1;
        else c->x = MIN(c->x, width - 1);
        return;
    }

#if DEBUG_LINES
    ssize_t yy = c->y;
#endif

    c->y = -1;
    ssize_t y = 0;

    do {
        ssize_t next_offset = line_advance_width(it.line, it.offset, width);
        if (it.line == pos->line && it.offset <= pos->offset && next_offset > pos->offset) {
            if (c->pending) c->x = width - 1;
            else c->x = MIN(pos->offset - it.offset, width - 1);
            c->y = y;
            break;
        }
        y++;
    } while (!line_span_shift(&it, width));

#if DEBUG_LINES
    if (c->y == -1)
        warn("w=%zd y=%zd x=%zd cy=%zd", width, y, c->x, yy);
    assert(c->y >= 0);
    assert(c->x >= 0);
#endif
}

#if DEBUG_LINES
static void validate_main_screen(struct screen *scr) {
    struct line_span *screen = scr->main_screen.begin;

    struct line *prev_ln = NULL;
    assert(!scr->top_line.s.line->prev);
    assert(!scr->top_line.s.offset);
    assert(!screen[scr->height - 1].line->next);
    assert(scr->view_pos.s.line);
    assert(scr->top_line.s.line);
    assert(line_handle_is_registered(&scr->top_line));
    assert(line_handle_is_registered(&scr->view_pos));
    if (!scr->mode.altscreen)
        assert(line_span_cmp(&scr->top_line.s, &scr->view_pos.s) <= 0);
    assert(line_span_cmp(&scr->view_pos.s, scr->screen) <= 0);
    assert(!screen[scr->height - 1].line->next);
    bool has_scr = false, has_view = false;
    for (struct line *ln = scr->top_line.s.line; ln; ln = ln->next) {
        if (ln == scr->view_pos.s.line) has_view = true;
        if (ln == screen->line) has_scr = true;
        if (prev_ln) {
            assert(prev_ln == ln->prev);
            assert(prev_ln->next == ln);
            assert(prev_ln->seq < ln->seq);
        }
        prev_ln = ln;
    }
    assert(has_scr);
    assert(!scr->mode.altscreen == has_view);

    struct line_span *prevs = NULL;
    for (ssize_t i = 0; i < scr->height; i++) {
        struct line_span *view = &screen[i];
        assert(view->width <= scr->width);
        assert(view->offset + view->width <= view->line->size);
        if (prevs) {
            assert((prevs->line == view->line->prev && prevs->line->next == view->line) || prevs->line == view->line);
            assert(prevs->line->seq <= view->line->seq);
        }
        prevs = view;
    }
}

static void validate_altscreen(struct screen *scr) {
    struct line_span *altscr = scr->alt_screen.begin;

    struct line_span *prev = NULL;
    assert(!altscr[0].line->prev);
    assert(!altscr[scr->height - 1].line->next);
    for (ssize_t i = 0; i < scr->height; i++) {
        struct line_span *view = &altscr[i];
        assert(view->width <= scr->width);
        assert(!view->offset);
        if (prev) {
            assert(prev->line == view->line->prev);
            assert(prev->line->next == view->line);
            assert(prev->line->seq < view->line->seq);
        }
        prev = view;
    }
}
#endif

static inline void round_offset_to_width(struct line_span *handle, ssize_t width) {
    ssize_t to = handle->offset;
    handle->offset = 0;

#if DEBUG_LINES
    assert(to < handle->line->size);
#endif

    do {
        ssize_t next = line_advance_width(handle->line, handle->offset, width);
        if (next > to) break;
        handle->offset = next;
    } while (handle->offset < handle->line->size);
}

enum stick_view resize_main_screen(struct screen *scr, ssize_t width, ssize_t height, struct line_span *lower_left) {
    enum stick_view ret = stick_none;

    struct line_span *screen = scr->main_screen.begin;

    ssize_t y = 0;

    if (screen) {
        /* Create cursor handles */
        struct line_handle prev_first_line = { .s = *screen };
        line_handle_add(&prev_first_line);

        struct cursor *c = scr->mode.altscreen ? &scr->last_scr_c : &scr->c;
        struct line_handle cursor_handle = { .s = {
                .line = screen[c->y].line,
                .offset = screen[c->y].offset + c->x,
        } };
        line_handle_add(&cursor_handle);

        struct cursor *saved_c = scr->mode.altscreen ? &scr->back_saved_c : &scr->saved_c;
        struct line_handle saved_cursor_handle = { .s = {
            .line = screen[saved_c->y].line,
            .offset = screen[saved_c->y].offset + saved_c->x
        } };
        line_handle_add(&saved_cursor_handle);

        screen_adjust_line_ex(scr, &scr->main_screen, c->y, c->x + 1, c->x + 1);
        screen_adjust_line_ex(scr, &scr->main_screen, saved_c->y, saved_c->x + 1, saved_c->x + 1);

        struct line_span it = cursor_handle.s;
        round_offset_to_width(&it, width);

        screen = xrezalloc(screen, scr->height * sizeof *screen, height * sizeof *screen);
        scr->main_screen.begin = screen;
        scr->main_screen.end = screen + height;
        if (!scr->mode.altscreen) scr->screen = screen;

#if DEBUG_LINES
        assert(c->y >= 0);
        struct line_span d0 = it;
#endif
        ssize_t rest = line_shift_n(&it, -c->y, width);
        if (rest) {
        /* Not enough lines in scrollback buffer to keep cursor on it original line,
         * need to allocate more */
#if DEBUG_LINES
            assert(rest < 0);
            assert(!it.line->prev);
            assert(!it.offset);
            assert(it.line == scr->top_line.s.line);
            if (scr->top_line.s.line)
                assert(find_handle_in_line(&scr->top_line));
#endif
            scr->sb_limit += -rest;
            create_lines_range(&scr->main_screen, NULL, it.line, 0, width,
                               &ATTR_DEFAULT, -rest, &scr->top_line);
            fixup_lines_seqno(it.line);
            it.line = scr->top_line.s.line;
#if DEBUG_LINES
        } else {
            struct line_span d = it;
            assert(!line_shift_n(&d, c->y, width));
            assert(!line_span_cmp(&d, &d0));
#endif
        }

        /* Calculate new cursor position */
        translate_screen_position(&it, &saved_cursor_handle.s, saved_c, width);
        translate_screen_position(&it, &cursor_handle.s, c, width);

        /* If cursor will be shifted off-screen, some lines needs to be pushed
         * to scrollback to keep cusor on screen.. */
        if (c->y >= height) {
            ssize_t delta = c->y - (height - 1);
            c->y -= delta;
            saved_c->y = MAX(0, saved_c->y - delta);

            delta = line_shift_n(&it, delta, width);
#if DEBUG_LINES
            assert(!delta);
#endif
        }

        saved_c->y = MIN(saved_c->y, height - 1);

        // FIXME Can lower_left be reset at this point?

        /* Fixup history count */
        if (!lower_left->line) ret = stick_to_bottom;

        push_history_until(scr, prev_first_line.s.line, it.line);
        if (free_extra_lines(scr) && ret == stick_none && !lower_left->line)
            ret = stick_to_top;

        /* Recalculate line views that are on screen */
        do {
#if DEBUG_LINES
            assert(it.line->size >= it.offset);
#endif
            ssize_t view_width = line_advance_width(it.line, it.offset, width);
            screen[y] = it;
            screen[y].width = view_width - it.offset;
            if (++y >= height) break;
        } while (!line_span_shift(&it, width));

        /* Truncate lines that are below the last line of the screen */
        if (y >= height) {
            screen_split_line_after_ex(scr, &scr->main_screen, height -  1);
            free_line_list_until(scr, &scr->main_screen, screen[height - 1].line->next, NULL);
            y = height;
        }

        line_handle_remove(&cursor_handle);
        line_handle_remove(&saved_cursor_handle);
        line_handle_remove(&prev_first_line);

#if DEBUG_LINES
        assert(scr->top_line.s.line);
        assert(find_handle_in_line(&scr->top_line));
#endif
    } else {
        screen = xzalloc(height * sizeof *screen);
        scr->main_screen.begin = screen;
        scr->main_screen.end = screen + height;
        if (!scr->mode.altscreen) scr->screen = screen;
    }

    create_lines_range(&scr->main_screen, y ? screen[y - 1].line : NULL, NULL,
                       y, width, &ATTR_DEFAULT, height - y, &scr->top_line);

    if (ret == stick_none && !lower_left->line)
        ret = stick_to_bottom;

    return ret;
}

void screen_drain_scrolled(struct screen *scr) {
    if (free_extra_lines(scr)) {
        replace_handle(&scr->view_pos, &scr->top_line.s);
        selection_view_scrolled(&scr->sstate, scr);
    }
}

void screen_resize(struct screen *scr, int16_t width, int16_t height) {

    mpa_set_seal_max_pad(&scr->main_screen.pool, width * sizeof(struct cell) + sizeof(struct line), 4*height);
    mpa_set_seal_max_pad(&scr->alt_screen.pool, width * sizeof(struct cell) + sizeof(struct line), 1);

    screen_drain_scrolled(scr);
#if USE_URI
    /* Reset active URL */
    window_set_active_uri(scr->win, EMPTY_URI, 0);
#endif

    resize_aux(scr, width, height);
    resize_tabs(scr, width);
    resize_altscreen(scr, width, height);

    /* Find line of bottom left cell */
    struct line_handle lower_left = { .s = scr->view_pos.s };
    if (lower_left.s.line) {
        screen_span_shift_n(scr, &lower_left.s, scr->height - 1);
        line_handle_add(&lower_left);
    }

    enum stick_view stick = stick_none;

    if (!scr->screen || line_span_cmpeq(&scr->view_pos.s, scr->screen)) stick = stick_to_bottom;
    else if (line_span_cmpeq(&scr->view_pos.s, &scr->top_line.s)) stick = stick_to_top;

    enum stick_view stick_after_resize = resize_main_screen(scr, width, height, &lower_left.s);
    if (!scr->mode.altscreen && stick_after_resize != stick_none)
        stick = stick_after_resize;

    scr->width = width;
    scr->height = height;
    scr->left = 0;
    scr->top = 0;
    scr->right = width - 1;
    scr->bottom = height - 1;

    line_handle_remove(&lower_left);
    fixup_view(scr, &lower_left, stick);

    screen_damage_lines(scr, 0, scr->height);

#if DEBUG_LINES
    validate_altscreen(scr);
    validate_main_screen(scr);
#endif
}

bool screen_redraw(struct screen *scr, bool blink_commited) {
    bool c_hidden = scr->mode.hide_cursor || !screen_at_bottom(scr);
    bool c_moved = scr->c.x != scr->prev_c_x || scr->c.y != scr->prev_c_y;

    if (c_moved || scr->prev_c_hidden != c_hidden || scr->prev_c_view_changed || !blink_commited) {
        if (!c_hidden) screen_damage_cursor(scr);
        if ((!scr->prev_c_hidden || scr->prev_c_view_changed) && scr->prev_c_y < scr->height) {
            if (scr->prev_c_x < scr->screen[scr->prev_c_y].width)
                view_cell(&scr->screen[scr->prev_c_y],scr->prev_c_x)->drawn = false;
            else if (scr->prev_c_x == scr->screen[scr->prev_c_y].width)
                scr->screen[scr->prev_c_y].line->force_damage = true;
        }
    }

    scr->prev_c_x = scr->c.x;
    scr->prev_c_y = scr->c.y;
    scr->prev_c_hidden = c_hidden;
    scr->prev_c_view_changed = false;

    if (scr->scroll_damage) {
        screen_damage_lines(scr, 0, scr->height);
        scr->scroll_damage = false;
    }

    return window_submit_screen(scr->win, scr->c.x, scr->c.y, !c_hidden, scr->c.pending, c_moved);
}

void screen_set_tb_margins(struct screen *scr, int16_t top, int16_t bottom) {
    if (top < bottom) {
        scr->top = MAX(0, MIN(scr->height - 1, top));
        scr->bottom = MAX(0, MIN(scr->height - 1, bottom));
    } else {
        scr->top = 0;
        scr->bottom = scr->height - 1;
    }
}

bool screen_set_lr_margins(struct screen *scr, int16_t left, int16_t right) {
    if (!scr->mode.lr_margins) return 0;

    if (left < right) {
        scr->left = MAX(0, MIN(scr->width - 1, left));
        scr->right = MAX(0, MIN(scr->width - 1, right));
    } else {
        scr->left = 0;
        scr->right = scr->width - 1;
    }

    return 1;
}

void screen_reset_margins(struct screen *scr) {
    scr->top = 0;
    scr->left = 0;
    scr->bottom = scr->height - 1;
    scr->right = scr->width - 1;
}


FORCEINLINE
static inline void prep_lines(struct screen *scr, ssize_t xs, ssize_t ys, ssize_t xe, ssize_t ye, bool erase) {
    if (erase) {
        ssize_t xs_val = xs, xe_val = xe;
        if (!xs_val && ys < ye) screen_split_line_before(scr, ys);
        for (ssize_t i = ys; i < ye; i++) {
            screen_split_line_after(scr, i);
            struct line_span *view = &scr->screen[i];
            if (view->width <= xe_val && attr_eq(attr_pad(view->line), &scr->sgr)) {
                if (view->width > xs_val) {
                    screen_realloc_line(scr, view->line, view->offset + xs_val, get_current_screen(scr));
                    view->width = xs_val;
                }
            } else {
                screen_adjust_line(scr, i, xe);
            }
        }
    } else {
        for (ssize_t i = ys; i < ye; i++) {
            screen_split_line_after(scr, i);
            screen_adjust_line(scr, i, xe);
        }
    }
}

FORCEINLINE
static inline void screen_rect_pre(struct screen *scr, int16_t *xs, int16_t *ys, int16_t *xe, int16_t *ye, bool erase) {
    *xs = MAX(screen_min_oy(scr), MIN(*xs, screen_max_ox(scr) - 1));
    *xe = MAX(screen_min_oy(scr), MIN(*xe, screen_max_ox(scr)));
    *ys = MAX(screen_min_oy(scr), MIN(*ys, screen_max_oy(scr) - 1));
    *ye = MAX(screen_min_oy(scr), MIN(*ye, screen_max_oy(scr)));

    prep_lines(scr, *xs, *ys, *xe, *ye, erase);
}

FORCEINLINE
static inline void screen_erase_pre(struct screen *scr, int16_t *xs, int16_t *ys, int16_t *xe, int16_t *ye, bool origin, bool erase) {
    if (origin) screen_rect_pre(scr, xs, ys, xe, ye, erase);
    else {
        *xs = MAX(0, MIN(*xs, scr->width));
        *xe = MAX(0, MIN(*xe, scr->width));
        *ys = MAX(0, MIN(*ys, scr->height));
        *ye = MAX(0, MIN(*ye, scr->height));

        prep_lines(scr, *xs, *ys, *xe, *ye, erase);
    }

    if (screen_at_bottom(scr))
        window_delay_redraw(scr->win);

    if (!selection_active(&scr->sstate)) return;
    for (ssize_t y = *ys; y < *ye; y++) {
        ssize_t offset = scr->screen[y].offset;
        if (selection_intersects(&scr->sstate, scr->screen[y].line, *xs + offset, *xe + offset)) {
            screen_damage_selection(scr);
            selection_clear(&scr->sstate);
            break;
        }
    }
}

uint16_t screen_checksum(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye, struct checksum_mode mode, bool nrcs) {
    screen_rect_pre(scr, &xs, &ys, &xe, &ye, false);

    // TODO Test this thing

    uint32_t res = 0, spc = 0, trm = 0;
    enum charset gr = scr->c.gn[scr->c.gr];
    bool first = 1, notrim = mode.no_trim;

    for (; ys < ye; ys++) {
        struct line_span *line = &scr->screen[ys];
        for (int16_t i = xs; i < xe; i++) {
            uint32_t ch_orig = i >= line->width ? 0 : view_cell(line, i)->ch;
            if (ch_orig == '\t') ch_orig = '\0';

            uint32_t ch = ch_orig;
            const struct attr *attr = view_attr_at(line, i);
            if (!(mode.no_implicit) && !ch) ch = ' ';

            if (!(mode.wide)) {
                if (ch > 0x7F && gr != cs94_ascii) {
                    nrcs_encode(gr, &ch, nrcs);
                    if (!mode.eight_bit && ch < 0x80) ch |= 0x80;
                }
                ch &= 0xFF;
            } else {
                ch = uncompact(ch);
            }

            if (!(mode.no_attr)) {
                if (attr->underlined) ch += 0x10;
                if (attr->reverse) ch += 0x20;
                if (attr->blink) ch += 0x40;
                if (attr->bold) ch += 0x80;
                if (attr->italic) ch += 0x100;
                if (attr->faint) ch += 0x200;
                if (attr->strikethrough) ch += 0x400;
                if (attr->invisible) ch += 0x800;
            }
            if (first || ch_orig || !attr_eq(attr, &(struct attr){ .fg = attr->fg, .bg = attr->bg, .ul = attr->ul }))
                trm += ch + spc, spc = 0;
            else if (!ch_orig && notrim) spc += ' ';

            res += ch;
            first = notrim;
        }
        if (!notrim) spc = first = 0;
    }

    if (!notrim) res = trm;
    return mode.positive ? res : -res;
}

void screen_reverse_sgr(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye, struct attr *attr) {
    screen_erase_pre(scr, &xs, &ys, &xe, &ye, true, false);
    uint32_t mask = attr_mask(attr);

    bool rect = scr->mode.attr_ext_rectangle;
    for (; ys < ye; ys++) {
        struct line_span *line = &scr->screen[ys];
        ssize_t xend = rect || ys == ye - 1 ? xe : screen_max_ox(scr);
        screen_adjust_line(scr, ys, xend);

        uint32_t prev_id = ATTRID_MAX, new_id = ATTRID_MAX;
        for (struct cell *c = view_cell(line, xs), *cend = view_cell(line, xend); c < cend; c++) {
            if (UNLIKELY(c->attrid != prev_id)) {
                struct attr newa = *view_attr(line, c->attrid);
                attr_mask_set(&newa, attr_mask(&newa) ^ mask);
                new_id = alloc_attr(line->line, &newa);
                prev_id = c->attrid;
            }
            c->attrid = new_id;
            c->drawn = false;
        }
        if (!rect) xs = screen_min_ox(scr);
    }
}

void screen_apply_sgr(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye, struct attr *mask, struct attr *attr) {
    screen_erase_pre(scr, &xs, &ys, &xe, &ye, true, false);
    uint32_t mmsk = attr_mask(mask);
    uint32_t amsk = attr_mask(attr) & mmsk;

    bool rect = scr->mode.attr_ext_rectangle;
    for (; ys < ye; ys++) {
        struct line_span *line = &scr->screen[ys];
        ssize_t xend = rect || ys == ye - 1 ? xe : screen_max_ox(scr);
        screen_adjust_line(scr, ys, xend);

        uint32_t prev_id = ATTRID_MAX, new_id = ATTRID_MAX;
        for (struct cell *c = view_cell(line, xs), *cend = view_cell(line, xend); c < cend; c++) {
            if (UNLIKELY(c->attrid != prev_id)) {
                struct attr newa = *view_attr(line, c->attrid);
                attr_mask_set(&newa, (attr_mask(&newa) & ~mmsk) | amsk);
                if (mask->fg) newa.fg = attr->fg;
                if (mask->bg) newa.bg = attr->bg;
                if (mask->ul) newa.ul = attr->ul;
                new_id = alloc_attr(line->line, &newa);
                prev_id = c->attrid;
            }
            c->attrid = new_id;
            c->drawn = false;
        }
        if (!rect) xs = screen_min_ox(scr);
    }
}

struct attr screen_common_sgr(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye) {
    screen_rect_pre(scr, &xs, &ys, &xe, &ye, false);

    struct attr common = *view_attr_at(&scr->screen[ys], xs);
    bool has_common_fg = 1, has_common_bg = 1, has_common_ul = 1;

    for (; ys < ye; ys++) {
        struct line_span *line = &scr->screen[ys];
        for (int16_t i = xs; i < xe; i++) {
            const struct attr *attr = view_attr_at(line, i);
            has_common_fg &= (common.fg == attr->fg);
            has_common_bg &= (common.bg == attr->bg);
            has_common_ul &= (common.ul == attr->ul);
            attr_mask_set(&common, attr_mask(&common) & attr_mask(attr));
        }
    }

    if (!has_common_bg) common.bg = indirect_color(SPECIAL_BG);
    if (!has_common_fg) common.fg = indirect_color(SPECIAL_FG);
    if (!has_common_ul) common.ul = indirect_color(SPECIAL_BG);

    return common;
}

void screen_copy(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye, int16_t xd, int16_t yd, bool origin) {
    if (ye < ys) SWAP(ye, ys);
    if (xe < xs) SWAP(xe, xs);

    if (origin) {
        xs = MAX(screen_min_ox(scr), MIN(xs, screen_max_ox(scr) - 1));
        ys = MAX(screen_min_oy(scr), MIN(ys, screen_max_oy(scr) - 1));
        xd = MAX(screen_min_ox(scr), MIN(xd, screen_max_ox(scr) - 1));
        yd = MAX(screen_min_oy(scr), MIN(yd, screen_max_oy(scr) - 1));
        xe = MAX(screen_min_ox(scr), MIN(MIN(xe - xs + xd, screen_max_ox(scr)) - xd + xs, screen_max_ox(scr)));
        ye = MAX(screen_min_oy(scr), MIN(MIN(ye - ys + yd, screen_max_oy(scr)) - yd + ys, screen_max_oy(scr)));
    } else {
        xs = MAX(0, MIN(xs, scr->width));
        ys = MAX(0, MIN(ys, scr->height));
        xd = MAX(0, MIN(xd, scr->width));
        yd = MAX(0, MIN(yd, scr->height));
        xe = MAX(0, MIN(MIN(xe - xs + xd, scr->width) - xd + xs, scr->width));
        ye = MAX(0, MIN(MIN(ye - ys + yd, scr->height) - yd + ys, scr->height));
    }

    if (xs >= xe || ys >= ye) return;

    if (yd <= ys) {
        for (; ys < ye; ys++, yd++) {
            screen_adjust_line(scr, ys, xe);
            screen_adjust_line(scr, yd, xd + (xe - xs));
            screen_split_line_after(scr, yd);
            struct line_span *sl = &scr->screen[ys], *dl = &scr->screen[yd];
            copy_line(dl->line, xd + dl->offset,
                      sl->line, xs + sl->offset, xe - xs);
        }
    } else {
        for (yd += ye - ys; ys < ye; ye--, yd--) {
            screen_adjust_line(scr, ye - 1, xe);
            screen_adjust_line(scr, yd - 1, xd + (xe - xs));
            screen_split_line_after(scr, yd - 1);
            struct line_span *sl = &scr->screen[ye - 1], *dl = &scr->screen[yd - 1];
            copy_line(dl->line, xd + dl->offset,
                      sl->line, xs + sl->offset, xe - xs);
        }
    }
}

void screen_fill(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye, bool origin, uint32_t ch) {
    screen_erase_pre(scr, &xs, &ys, &xe, &ye, origin, ch == 0);

    for (; ys < ye; ys++) {
        struct line_span *line = &scr->screen[ys];
        ssize_t xe1 = MIN(xe, line->width);
        if (xe1 <= xs) continue;
        struct cell c = {
            .attrid = alloc_attr(line->line, &scr->sgr),
            .ch = compact(ch),
        };
        fill_cells(view_cell(line, xs), c, xe1 - xs);
    }
}

/* Erase whole lines by resetting their sizes to 0 */
void screen_erase_fast(struct screen *scr, int16_t ys, int16_t ye, struct attr *attr) {

    struct line_span *view = &scr->screen[ys];
    screen_split_line_before(scr, ys);

    while (ys < ye) {
#if DEBUG_LINES
        assert(!view->offset);
#endif

        screen_split_line_after(scr, ys);

        screen_realloc_line(scr, view->line, 0, get_current_screen(scr));
        view->line->pad_attrid = alloc_attr(view->line, attr);
        view->width = 0;

        view = &scr->screen[++ys];
    }
}

void screen_erase(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye, bool origin) {
    screen_fill(scr, xs, ys, xe, ye, origin, 0);
}

void screen_protective_erase(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye, bool origin) {
    screen_erase_pre(scr, &xs, &ys, &xe, &ye, origin, false);

    for (; ys < ye; ys++) {
        struct line_span *line = &scr->screen[ys];
        struct cell c = { .attrid = alloc_attr(line->line, &scr->sgr) };
        for (int16_t i = xs; i < xe; i++)
            if (!view_attr_at(line, i)->protected)
                *view_cell(line, i) = c;
    }
}

void screen_selective_erase(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye, bool origin) {
    screen_erase_pre(scr, &xs, &ys, &xe, &ye, origin, false);

    for (; ys < ye; ys++) {
        struct line_span *line = &scr->screen[ys];
        for (ssize_t i = xs; i < xe; i++) {
            if (!view_attr_at(line, i)->protected) {
                struct cell *cell = view_cell(line, i);
                cell->ch = cell->drawn = 0;
            }
        }
    }
}

void screen_move_to(struct screen *scr, int16_t x, int16_t y) {
    scr->c.x = MIN(MAX(x, 0), scr->width - 1);
    scr->c.y = MIN(MAX(y, 0), scr->height - 1);
    screen_reset_pending(scr);
}


void screen_bounded_move_to(struct screen *scr, int16_t x, int16_t y) {
    scr->c.x = MIN(MAX(x, screen_min_x(scr)), screen_max_x(scr) - 1);
    scr->c.y = MIN(MAX(y, screen_min_y(scr)), screen_max_y(scr) - 1);
    screen_reset_pending(scr);
}

void screen_move_left(struct screen *scr, int16_t amount) {
    int16_t x = scr->c.x, y = scr->c.y,
        first_left = x < screen_min_x(scr) ? 0 : screen_min_x(scr);


    /* This is a hack that allows using proper line editing with reverse wrap
     * mode while staying compatible with VT100 wrapping mode */
    if (scr->mode.reverse_wrap) x += scr->c.pending;

    if (amount > x - first_left && scr->mode.wrap && scr->mode.reverse_wrap) {
        bool in_tbm = screen_min_y(scr) <= scr->c.y && scr->c.y < screen_max_y(scr);
        int16_t height = in_tbm ? screen_max_y(scr) - screen_min_y(scr) : scr->height;
        int16_t top = in_tbm ? screen_min_y(scr) : 0;

        amount -= x - first_left;
        x = screen_max_x(scr);
        y -= 1 + amount/(screen_max_x(scr) - screen_min_x(scr));
        amount %= screen_max_x(scr) - screen_min_x(scr);

        y = (y - top) % height + top;
        if (y < top) y += height;
    }

    (scr->c.x >= screen_min_x(scr) ? screen_bounded_move_to : screen_move_to)(scr, x - amount, y);
}

void screen_save_cursor(struct screen *scr, bool mode) {
    if (mode) /* save */ {
        scr->saved_c = scr->c;
#if USE_URI
        uri_ref(scr->sgr.uri);
        uri_unref(scr->saved_sgr.uri);
#endif
        scr->saved_sgr = scr->sgr;
    } else /* restore */ {
        scr->c = scr->saved_c;
#if USE_URI
        uri_ref(scr->saved_sgr.uri);
        uri_unref(scr->sgr.uri);
#endif
        scr->sgr = scr->saved_sgr;

        assert(scr->c.x < scr->width);
        assert(scr->c.y < scr->height);
    }
}

static void screen_swap_screen(struct screen *scr) {
    selection_clear(&scr->sstate);
    if (!scr->mode.altscreen)
        scr->last_scr_c = scr->c;
    scr->mode.altscreen ^= 1;
    SWAP(scr->back_saved_c, scr->saved_c);
    SWAP(scr->back_saved_sgr, scr->saved_sgr);
    scr->screen = get_current_screen(scr)->begin;
    screen_reset_view(scr, true);
}

void screen_set_altscreen(struct screen *scr, bool set, bool clear, bool save) {
    if (scr->mode.disable_altscreen) return;
    if (set != scr->mode.altscreen) {
        if (set && save) screen_save_cursor(scr, true);
        screen_swap_screen(scr);
        if (!set && save) screen_save_cursor(scr, false);
    }
    if (set && clear) {
        screen_erase(scr, 0, 0, scr->width, scr->height, 0);
    }
}

void screen_scroll_horizontal(struct screen *scr, int16_t left, int16_t amount) {
    ssize_t top = screen_min_y(scr), right = screen_max_x(scr), bottom = screen_max_y(scr);

    for (ssize_t i = top; i < bottom; i++) {
        struct line_span *line = &scr->screen[i];
        view_adjust_wide_left(line, left);
        view_adjust_wide_right(line, right - 1);
    }

    if (amount > 0) { /* left */
        amount = MIN(amount, right - left);
        screen_copy(scr, left + amount, top, right, bottom, left, top, 0);
        screen_erase(scr, right - amount, top, right, bottom, 0);
    } else { /* right */
        amount = MIN(-amount, right - left);
        screen_copy(scr, left, top, right - amount, bottom, left + amount, top, 0);
        screen_erase(scr, left, top, left + amount, bottom, 0);
    }
}

static inline void swap_3(struct line *top_after, struct line *mid_before, struct line *bottom_before) {
        struct line *top_before = detach_prev_line(top_after);
        struct line *mid_after = detach_next_line(mid_before);
        struct line *bottom_after = detach_next_line(bottom_before);

#if DEBUG_LINES
        assert(top_after->seq <= mid_before->seq);
        assert(mid_before->seq < bottom_before->seq);
#endif

        if (top_before) attach_next_line(top_before, mid_after);
        attach_next_line(bottom_before, top_after);
        attach_next_line(mid_before, bottom_after);
}

static inline int16_t screen_scroll_fast(struct screen *scr, int16_t top, int16_t amount, bool save) {
    ssize_t bottom = screen_max_y(scr);

    save &= save && !scr->mode.altscreen && top == 0 && amount >= 0;

    bool should_reset_view = screen_at_bottom(scr);
    bool should_reset_top = UNLIKELY(!save) && !top && UNLIKELY(line_span_cmpeq(&scr->top_line.s, scr->screen));

    struct line_span *first = &scr->screen[top];
    /* Force scrolled region borders to be line borders */
    if (!save) screen_split_line_before(scr, top);

    struct line_span *last = &scr->screen[bottom - 1];
    screen_split_line_after(scr, bottom - 1);

    if (amount > 0) /* up */ {
        amount = MIN(amount, (bottom - top));
        ssize_t rest = (bottom - top) - amount;

        if (save) {
            struct line *first_to_hist = first->line;
            struct line *bottom_line = last->line;
            struct line *bottom_next = detach_next_line(bottom_line);

            memmove(scr->screen, scr->screen + amount, (bottom - amount)*sizeof *scr->screen);

#if DEBUG_LINES
            if (rest) assert(scr->screen[rest - 1].line == bottom_line);
            assert(!bottom_line->next);
            if (bottom_next) assert(!bottom_next->prev);
#endif

            create_lines_range(&scr->main_screen, bottom_line, bottom_next, rest,
                               scr->width, &scr->sgr, amount, NULL);

            fixup_lines_seqno(bottom_next);

            push_history_until(scr, first_to_hist, first->line);

        } else {
            screen_erase_fast(scr, top, top + amount, &scr->sgr);

            if (rest && amount) {
                struct line *mid = scr->screen[top + amount - 1].line;
                swap_3(first->line, mid, last->line);
            }

            memcpy(scr->temp_screen, scr->screen + top, amount*sizeof(*scr->temp_screen));
            memmove(scr->screen + top, scr->screen + top + amount, rest*sizeof(*scr->temp_screen));
            memcpy(scr->screen + top + rest, scr->temp_screen, amount*sizeof(*scr->temp_screen));

            if (bottom - amount >= 0)
                fixup_lines_seqno(scr->screen[bottom - amount].line);
        }

        if (scr->main_screen.pool.unsealed_count == 0)
            screen_drain_scrolled(scr);
    } else if (amount < 0) /* down */ {
        amount = MAX(amount, -(bottom - top));
        ssize_t rest = (bottom - top) + amount;

        screen_erase_fast(scr, bottom + amount, bottom, &scr->sgr);

        if (rest /* && amount */) {
            struct line *mid = scr->screen[bottom - 1 + amount].line;
            swap_3(first->line, mid, last->line);
        }

        memcpy(scr->temp_screen, scr->screen + bottom + amount, -amount*sizeof(*scr->temp_screen));
        memmove(scr->screen + top - amount, scr->screen + top, rest*sizeof(*scr->temp_screen));
        memcpy(scr->screen + top, scr->temp_screen, -amount*sizeof(*scr->temp_screen));

        if (top - amount < scr->height)
            fixup_lines_seqno(scr->screen[top - amount].line);
    }

    if (LIKELY(amount)) {
        scr->scroll_damage = true;

        if (should_reset_top)
            replace_handle(&scr->top_line, &(struct line_span){ .line = scr->screen->line });

        if (should_reset_view) {
            replace_handle(&scr->view_pos, &scr->screen[0]);
            window_delay_redraw(scr->win);
            if (UNLIKELY(selection_active(&scr->sstate)))
                selection_scrolled(&scr->sstate, scr, top, bottom, save);
        }

        /* Update position of selection, if scrolled */
    }

#if DEBUG_LINES
    validate_altscreen(scr);
    validate_main_screen(scr);
#endif
    return amount;
}

void screen_scroll(struct screen *scr, int16_t top, int16_t amount, bool save) {
    ssize_t left = screen_min_x(scr), right = screen_max_x(scr);

    if (LIKELY(left == 0 && right == scr->width)) { /* Fast scrolling without margins */
        amount = screen_scroll_fast(scr, top, amount, save);
    } else { /* Slow scrolling with margins */
        ssize_t bottom = screen_max_y(scr);
        for (ssize_t i = top; i < bottom; i++) {
            struct line_span *line = &scr->screen[i];
            view_adjust_wide_left(line, left);
            view_adjust_wide_right(line, right - 1);
        }

        if (amount > 0) { /* up */
            amount = MIN(amount, bottom - top);
            screen_copy(scr, left, top + amount, right, bottom, left, top, false);
            screen_erase(scr, left, bottom - amount, right, bottom, false);
        } else { /* down */
            amount = MIN(-amount, bottom - top);
            screen_copy(scr, left, top, right, bottom - amount, left, top + amount, false);
            screen_erase(scr, left, top, right, top + amount, false);
        }
    }

    if (UNLIKELY(scr->mode.smooth_scroll) && (scr->scrolled += abs(amount)) > window_cfg(scr->win)->smooth_scroll_step) {
        window_request_scroll_flush(scr->win);
        scr->scrolled = 0;
    }
}


void screen_insert_cells(struct screen *scr, int16_t n) {
    if (screen_cursor_in_region(scr)) {
        ssize_t max_x = screen_max_x(scr);
        n = MIN(n, max_x - scr->c.x);
        if (n <= 0) return;

        struct line_span *line = &scr->screen[scr->c.y];

        ssize_t tail = line->width - scr->c.x;
        if (tail > 0) {
            screen_adjust_line(scr, scr->c.y, MIN(line->width + n, max_x));

            view_adjust_wide_left(line, scr->c.x);
            view_adjust_wide_right(line, scr->c.x);

            ssize_t tail_len = MIN(max_x - n - scr->c.x, tail);
            if (tail_len > 0) {
                for (int16_t i = scr->c.x; i < scr->c.x + tail_len; i++)
                    view_cell(line, i)->drawn = 0;
                memmove(view_cell(line, scr->c.x + n),
                        view_cell(line, scr->c.x), tail_len * sizeof(struct cell));
            }
        }

        screen_erase(scr, scr->c.x, scr->c.y, scr->c.x + n, scr->c.y + 1, false);

        if (view_selection_intersects(&scr->sstate, line, max_x - n, max_x)) {
            screen_damage_selection(scr);
            selection_clear(&scr->sstate);
        }
    }

    screen_reset_pending(scr);
}

void screen_delete_cells(struct screen *scr, int16_t n) {
    /* Do not check top/bottom margins, DCH should work outside them */
    ssize_t max_x = screen_max_x(scr);
    if (scr->c.x >= screen_min_x(scr) && scr->c.x < max_x) {
        n = MIN(n, max_x - scr->c.x);
        if (n <= 0) return;

        struct line_span *line = &scr->screen[scr->c.y];

        ssize_t tail = line->width - scr->c.x;
        if (tail > 0) {
            screen_split_line_after(scr, scr->c.y);
            view_adjust_wide_left(line, scr->c.x);
            view_adjust_wide_right(line, scr->c.x + n - 1);

            ssize_t tail_len = MIN(max_x, line->width) - n - scr->c.x;
            if (tail_len > 0) {
                memmove(view_cell(line, scr->c.x),
                        view_cell(line, scr->c.x + n), tail_len * sizeof(struct cell));
                for (int16_t i = scr->c.x; i < scr->c.x + tail_len; i++)
                    view_cell(line, i)->drawn = 0;
            }

            if (max_x >= line->width) {
                ssize_t new_width = scr->c.x + MAX(0, tail_len);
                if (!new_width) screen_split_line_before(scr, scr->c.y);
                screen_realloc_line(scr, line->line, line->offset + new_width, get_current_screen(scr));
                line->width = new_width;
            }
        }

        screen_erase(scr, max_x - n, scr->c.y, max_x, scr->c.y + 1, false);

        if (view_selection_intersects(&scr->sstate, line, scr->c.x, scr->c.x + n)) {
            screen_damage_selection(scr);
            selection_clear(&scr->sstate);
        }
    }

    screen_reset_pending(scr);
}

void screen_insert_lines(struct screen *scr, int16_t n) {
    if (screen_cursor_in_region(scr))
        screen_scroll(scr, scr->c.y, -n, 0);
    screen_move_to(scr, screen_min_x(scr), scr->c.y);
}

void screen_delete_lines(struct screen *scr, int16_t n) {
    if (screen_cursor_in_region(scr))
        screen_scroll(scr, scr->c.y, n, 0);
    screen_move_to(scr, screen_min_x(scr), scr->c.y);
}

void screen_insert_columns(struct screen *scr, int16_t n) {
    if (screen_cursor_in_region(scr))
        screen_scroll_horizontal(scr, scr->c.x, -n);
}

void screen_delete_columns(struct screen *scr, int16_t n) {
    if (screen_cursor_in_region(scr))
        screen_scroll_horizontal(scr, scr->c.x, n);
}

void screen_index_horizonal(struct screen *scr) {
    if (scr->c.x == screen_max_x(scr) - 1 && screen_cursor_in_region(scr)) {
        screen_scroll_horizontal(scr, screen_min_x(scr), 1);
        screen_reset_pending(scr);
    } else if (scr->c.x != screen_max_x(scr) - 1)
        screen_move_to(scr, scr->c.x + 1, scr->c.y);
}

void screen_rindex_horizonal(struct screen *scr) {
    if (scr->c.x == screen_min_x(scr) && screen_cursor_in_region(scr)) {
        screen_scroll_horizontal(scr, screen_min_x(scr), -1);
        screen_reset_pending(scr);
    } else if (scr->c.x != screen_min_x(scr))
        screen_move_to(scr, scr->c.x - 1, scr->c.y);
}

bool screen_index(struct screen *scr) {
    if (scr->c.y == screen_max_y(scr) - 1 && screen_cursor_in_region(scr)) {
        screen_scroll(scr, screen_min_y(scr), 1, 1);

        screen_reset_pending(scr);
        return true;
    } else if (scr->c.y != screen_max_y(scr) - 1) {
        screen_move_to(scr, scr->c.x, scr->c.y + 1);
        return true;
    }
    return false;
}

void screen_rindex(struct screen *scr) {
    if (scr->c.y == screen_min_y(scr) && screen_cursor_in_region(scr)) {
        screen_scroll(scr,  screen_min_y(scr), -1, 1);
        screen_reset_pending(scr);
    } else if (scr->c.y != screen_min_y(scr))
        screen_move_to(scr, scr->c.x, scr->c.y - 1);
}

void screen_cr(struct screen *scr) {
    screen_move_to(scr, scr->c.x < screen_min_x(scr) ?
            screen_min_ox(scr) : screen_min_x(scr), scr->c.y);
}

uint8_t screen_get_margin_bell_volume(struct screen *scr) {
    if (!scr->mbvol) return 0;
    return 2 - (scr->mbvol == window_cfg(scr->win)->margin_bell_low_volume);
}

void screen_set_margin_bell_volume(struct screen *scr, uint8_t vol) {
    switch (vol) {
    case 0:
        scr->mbvol = 0;
        break;
    case 1:
        scr->mbvol = window_cfg(scr->win)->margin_bell_low_volume;
        break;
    default:
        scr->mbvol = window_cfg(scr->win)->margin_bell_high_volume;
    }
}

bool screen_load_config(struct screen *scr, bool reset) {
    struct instance_config *cfg = window_cfg(screen_window(scr));

    selection_load_config(&scr->sstate);

    if (reset) {
        free_selection(&scr->sstate);
        if (!init_selection(&scr->sstate, screen_window(scr), scr)) return 0;

        scr->mode = (struct screen_mode) {
            .disable_altscreen = !cfg->allow_altscreen,
            .wrap = cfg->wrap,
        };

        scr->c = scr->back_saved_c = scr->saved_c = (struct cursor) {
            .gl = 0, .gl_ss = 0, .gr = 2,
            .gn = {cs94_ascii, cs94_ascii, cs94_ascii, cs94_ascii}
        };

#if USE_URI
        window_set_mouse(scr->win, true);
        uri_unref(scr->sgr.uri);
        uri_unref(scr->saved_sgr.uri);
        uri_unref(scr->back_saved_sgr.uri);
#endif

        scr->sgr = scr->saved_sgr = scr->back_saved_sgr = ATTR_DEFAULT;
        scr->upcs = cs96_latin_1;
    }

    screen_set_margin_bell_volume(scr, cfg->margin_bell_volume);

    scr->sstate.keep_selection = cfg->keep_selection;
    scr->sstate.select_to_clipboard = cfg->select_to_clipboard;

    scr->mode.smooth_scroll = cfg->smooth_scroll;
    return 1;
}

bool init_screen(struct screen *scr, struct window *win) {
    scr->win = win;

    mpa_init(&scr->main_screen.pool, MPA_POOL_SIZE);
    mpa_init(&scr->alt_screen.pool, sizeof(struct line) + 400*sizeof(struct cell));

    init_printer(&scr->printer, window_cfg(win));
    return screen_load_config(scr, 1);
}

char *encode_sgr(char *dst, char *end, const struct attr *attr) {
#define FMT(...) dst += snprintf(dst, end - dst, __VA_ARGS__)
#define MAX_SGR_LEN 54
    /* Maximal length sequence is "0;1;2;3;4;6;7;8;9;38:2:255:255:255;48:2:255:255:255" */

    /* Reset everything */
    FMT("0");

    /* Encode attributes */
    if (attr->bold) FMT(";1");
    if (attr->faint) FMT(";2");
    if (attr->italic) FMT(";3");
    if (attr->underlined == 1) FMT(";4");
    else if (attr->underlined > 1) FMT(";4:%d", attr->underlined);
    if (attr->blink) FMT(";6");
    if (attr->reverse) FMT(";7");
    if (attr->invisible) FMT(";8");
    if (attr->strikethrough) FMT(";9");

    /* Encode foreground color */
    if (color_idx(attr->fg) < 8) FMT(";%u", 30 + color_idx(attr->fg));
    else if (color_idx(attr->fg) < 16) FMT(";%u", 90 + color_idx(attr->fg) - 8);
    else if (color_idx(attr->fg) < PALETTE_SIZE - SPECIAL_PALETTE_SIZE) FMT(";38:5:%u", color_idx(attr->fg));
    else if (color_idx(attr->fg) == SPECIAL_FG) /* FMT(";39") -- default, skip */;
    else if (is_direct_color(attr->fg)) FMT(";38:2:1:%u:%u:%u", color_r(attr->fg), color_g(attr->fg), color_b(attr->fg));

    /* Encode background color */
    if (color_idx(attr->bg) < 8) FMT(";%u", 40 + color_idx(attr->bg));
    else if (color_idx(attr->bg) < 16) FMT(";%u", 100 + color_idx(attr->bg) - 8);
    else if (color_idx(attr->bg) < PALETTE_SIZE - SPECIAL_PALETTE_SIZE) FMT(";48:5:%u", color_idx(attr->bg));
    else if (color_idx(attr->bg) == SPECIAL_FG) /* FMT(";49") -- default, skip */;
    else if (is_direct_color(attr->bg)) FMT(";48:2:1:%u:%u:%u", color_r(attr->bg), color_g(attr->bg), color_b(attr->bg));

    /* Encode underline color */
    if (color_idx(attr->ul) < PALETTE_SIZE - SPECIAL_PALETTE_SIZE) FMT(";58:5:%u", color_idx(attr->ul));
    else if (color_idx(attr->ul) == SPECIAL_FG) /* FMT(";59") -- default, skip */;
    else if (is_direct_color(attr->ul)) FMT(";58:2:1:%u:%u:%u", color_r(attr->ul), color_g(attr->ul), color_b(attr->ul));

    return dst;
#undef FMT
}

void screen_print_line(struct screen *scr, struct line_span *line) {
    if (!printer_is_available(&scr->printer)) return;

    uint8_t buf[PRINT_BLOCK_SIZE];
    uint8_t *pbuf = buf, *pend = buf + PRINT_BLOCK_SIZE;

    const struct attr *prev = &ATTR_DEFAULT;

    for (int16_t i = 0; i < line->width; i++) {
        struct cell c = *view_cell(line, i);
        const struct attr *attr = view_attr_at(line, i);

        if (window_cfg(scr->win)->print_attr && (!attr_eq(prev, attr) || !i)) {
            /* Print SGR state, if it have changed */
            *pbuf++ = '\033';
            *pbuf++ = '[';
            pbuf = (uint8_t *)encode_sgr((char *)pbuf, (char *)pend, attr);
            *pbuf++ = 'm';
        }

        /* Print blanks as ASCII space */
        if (!c.ch) c.ch = ' ';

        //TODO Encode NRCS when UTF is disabled
        //     for now just always print as UTF-8
        if (c.ch < 0xA0) *pbuf++ = cell_get(&c);
        else pbuf += utf8_encode(cell_get(&c), pbuf, pend);

        prev = attr;

        /* If there's no more space for next char, flush buffer */
        if (pbuf + MAX_SGR_LEN + UTF8_MAX_LEN + 1 >= pend) {
            printer_print_string(&scr->printer, buf, pbuf - buf);
            pbuf = buf;
        }
    }

    *pbuf++ = '\n';
    printer_print_string(&scr->printer, buf, pbuf - buf);
}


void screen_print_all(struct screen *scr) {
    struct line *line = !scr->mode.altscreen && scr->top_line.s.line ?
            scr->top_line.s.line : scr->screen[0].line;

    while (line) {
        screen_print_line(scr, &(struct line_span) {
            .line = line, .width = line->size });
        line = line->next;
    }
}

void screen_print_screen(struct screen *scr, bool force_ext) {
    if (!printer_is_available(&scr->printer)) return;

    force_ext |= scr->mode.print_extend;

    int16_t top = force_ext ? 0 : screen_min_y(scr);
    int16_t bottom = (force_ext ? scr->height : screen_max_y(scr)) - 1;

    while (top < bottom)
        screen_print_line(scr, &scr->screen[top++]);

    if (scr->mode.print_form_feed)
        printer_print_string(&scr->printer, (uint8_t[]){'\f'}, 1);
}

void screen_tabs(struct screen *scr, int16_t n) {
    //TODO CHT is not affected by DECCOM but CBT is?

    if (n >= 0) {
        if (scr->mode.xterm_more_hack && scr->c.pending)
            screen_wrap(scr, false);
        while (scr->c.x < screen_max_x(scr) - 1 && n--) {
            /* If current cell is empty, put \t over there */
            struct cell *c = view_cell(&scr->screen[scr->c.y], scr->c.x);
            if (scr->screen[scr->c.y].width <= scr->c.x || !c->ch) {
                screen_adjust_line(scr, scr->c.y, scr->c.x + 1);
                c->ch = '\t';
            }

            do scr->c.x++;
            while (scr->c.x < screen_max_x(scr) - 1 && !scr->tabs[scr->c.x]);
        }
    } else {
        while (scr->c.x > screen_min_ox(scr) && n++) {
            do scr->c.x--;
            while (scr->c.x > screen_min_ox(scr) && !scr->tabs[scr->c.x]);
        }
    }
}

void screen_reset_tabs(struct screen *scr) {
    memset(scr->tabs, 0, screen_width(scr) * sizeof(scr->tabs[0]));
    int16_t tabw = window_cfg(screen_window(scr))->tab_width;
    for (int16_t i = tabw; i < screen_width(scr); i += tabw)
        scr->tabs[i] = 1;
}

#ifdef __AVX__

typedef __m256i block_type;

#define read_aligned(ptr) _mm256_load_si256((const __m256i *)(ptr))
#define read_unaligned(ptr) _mm256_loadu_si256((const __m256i *)(ptr))
#define movemask(x) _mm256_movemask_epi8(x)
#define const8(x) _mm256_set1_epi8(x)

#elif defined(__SSE2__)

typedef __m128i block_type;

#define read_aligned(ptr) _mm_load_si128((const __m128i *)(ptr))
#define read_unaligned(ptr) _mm_loadu_si128((const __m128i *)(ptr))
#define movemask(x) _mm_movemask_epi8(x)
#define const8(x) _mm_set1_epi8(x)

#else

typedef uint64_t block_type;

static inline block_type read_aligned(const uint8_t *ptr) {
    return *(const block_type *)ptr;
}

static inline block_type read_unaligned(const uint8_t *ptr) {
    /* Let's hope memcpy to be optimized out */
    block_type d;
    memcpy(&d, ptr, sizeof d);
    return d;
}

static inline block_type const8(uint8_t x) {
    return 0x0101010101010101UL*x;
}

static inline uint32_t movemask(block_type b) {
    b &= const8(0x80);
    b = (b >> 7) | (b >> 14);
    b |= b >> 14;
    b |= b >> 28;
    return b & 0xFF;
}

#endif

static inline uint32_t block_mask(block_type b) {
    return movemask(~((b << 2) | (b << 1)));
}

static inline const uint8_t *mask_offset(const uint8_t *start, const uint8_t *end, uint32_t mask) {
    return MIN(end, start + __builtin_ffsl(mask) - 1);
}

static inline uint32_t contains_non_ascii(block_type b, block_type ones) {
    return movemask(b | (b + ones));
}

static inline const uint8_t *find_chunk(const uint8_t *start, const uint8_t *end, ssize_t max_chunk, bool *has_nonascii) {
    if (start + max_chunk < end)
        end = start + max_chunk;

    ssize_t prefix = end - start;

    if (prefix >= (ssize_t)sizeof(block_type))
        prefix = -(uintptr_t)start & (sizeof(block_type) - 1);

    const block_type ones = const8(0x01);

    if (prefix) {
        block_type b = read_unaligned(start);

        uint32_t msk = (1U << prefix) - 1U;
        static_assert(sizeof(block_type) <= sizeof(msk)*8, "Mask is too small");

        *has_nonascii = contains_non_ascii(b, ones) & msk;

        msk &= block_mask(b);
        if (msk)
            return mask_offset(start, end, msk);

        start += prefix;
    }

    /* We have out-of bounds read here but it should be fine,
     * since it is aligned and buffer size is a power of two */

    if (start < end) do {
        block_type b = read_aligned(start);
        *has_nonascii |= contains_non_ascii(b, ones);

        uint32_t msk = block_mask(b);
        if (UNLIKELY(msk))
            return mask_offset(start, end, msk);
    } while ((start += sizeof(block_type)) < end);

    return end;
}

static inline void print_buffer(struct screen *scr, const uint32_t *bstart, const uint8_t *astart, ssize_t totalw) {
    if (scr->mode.wrap) {
        if (scr->c.pending || (bstart && scr->c.x == screen_max_x(scr) - 1 && totalw > 1 && !bstart[1]))
            screen_wrap(scr, false);
    } else scr->c.x = MIN(scr->c.x, screen_max_x(scr) - totalw);

    struct line_span *line = &scr->screen[scr->c.y];
    struct cell *cell = NULL;

    /* Writing to the line resets its wrapping state */
    screen_split_line_after(scr, scr->c.y);

    ssize_t max_cx = scr->c.x + totalw, cx = scr->c.x;
    ssize_t max_tx = screen_max_x(scr);

    if (max_cx < line->width)
        view_adjust_wide_right(line, max_cx - 1);

    view_adjust_wide_left(line, cx);

    /* Clear selection if writing over it */
    if (UNLIKELY(selection_active(&scr->sstate)) && UNLIKELY(line->line->selection_index) &&
        view_selection_intersects(&scr->sstate, line, cx, scr->mode.insert ? max_tx : max_cx)) {
        screen_damage_selection(scr);
        selection_clear(&scr->sstate);
    }

    if (UNLIKELY(scr->mode.margin_bell)) {
        ssize_t bcol = screen_max_x(scr) - window_cfg(scr->win)->margin_bell_column;
        if (cx < bcol && max_cx >= bcol)
            window_bell(scr->win, scr->mbvol);
    }

    /* Allocate color for cell */
    uint32_t attrid = alloc_attr(line->line, screen_sgr(scr));

    /* NOTE: screen_adjust_line_ex() does not fill line with correct values after resizing
     * here. So we should not try dereferencing attributes of undefined cells.
     * That is why attriubte allocation is performed above. */

    /* Shift characters to the left if insert mode is enabled */
    if (UNLIKELY(scr->mode.insert) && max_cx < max_tx && cx < line->width) {
        ssize_t max_new_size = MIN(max_tx, line->width + totalw);
        if (line->width < max_new_size)
            screen_adjust_line_ex(scr, get_current_screen(scr), scr->c.y, cx, max_new_size);

        cell = view_cell(line, cx);

        for (struct cell *c = cell; c < view_cell(line, max_tx - max_cx); c++) c->drawn = 0;
        memmove(cell + totalw, cell, (max_tx - max_cx)*sizeof(*cell));
    } else {
        if (line->width < max_cx)
            screen_adjust_line_ex(scr, get_current_screen(scr), scr->c.y, cx, max_cx);
        cell = view_cell(line, cx);
    }

    cx += totalw;

    scr->c.pending = cx == max_tx;
    scr->c.x = cx - scr->c.pending;

    /* Put charaters, with fast path for ASCII-only */
    if (astart) {
        copy_ascii_to_cells(cell, astart, astart + totalw, attrid);
        if (UNLIKELY(gconfig.trace_characters)) {
            for (const uint8_t *px = astart; px < astart + totalw; px++)
                info("Char: (%x) '%lc' ", *px, (wint_t)*px);
        }
    } else {
        copy_utf32_to_cells(cell, bstart, bstart + totalw, attrid);
        if (UNLIKELY(gconfig.trace_characters)) {
            for (const uint32_t *px = bstart; px < bstart + totalw; px++)
                info("Char: (%x) '%lc' ", *px, (wint_t)*px);
        }
    }
}

bool screen_dispatch_print(struct screen *scr, const uint8_t **start, const uint8_t *end, bool utf8, bool nrcs) {
    bool complete = true;

    /* Compute maximum width to be printed at once */
    register ssize_t maxw = screen_max_x(scr) - screen_min_x(scr);
    if (!scr->c.pending || !scr->mode.wrap)
        maxw = (scr->c.x >= screen_max_x(scr) ? screen_width(scr) : screen_max_x(scr)) - scr->c.x;

    enum charset glv = scr->c.gn[scr->c.gl_ss];

    bool fast_nrcs = utf8 && !window_cfg(scr->win)->force_utf8_nrcs;
    bool skip_del = LIKELY(fast_nrcs) || glv < cs96_START || (!nrcs && (glv == cs96_latin_1 || glv == cs94_british));
    bool has_non_ascii = false;

    /* Find the actual end of buffer (control character or number of characters)
     * to prevent checking that on each iteration and preload cache.
     * Maximal possible size of the chunk is 4 times width of the lines since
     * each UTF-8 can be at most 4 bytes single width. */

    const uint8_t *xstart = *start;
    const uint8_t *chunk = find_chunk(xstart, end, maxw*4, &has_non_ascii);

    /* Really fast short path for common case */
    if (LIKELY(!has_non_ascii && fast_nrcs && glv != cs94_dec_graph && scr->c.gl_ss == scr->c.gl)) {

        maxw = MIN(chunk - xstart, maxw);
        *start = xstart + maxw;
        scr->prev_ch = xstart[maxw - 1];
        print_buffer(scr, NULL, xstart, maxw);

        if (xstart <= scr->save_handle_at_print && scr->save_handle_at_print < xstart + maxw) {
            replace_handle(&scr->saved_handle, &scr->screen[scr->c.y]);
            scr->saved_handle.s.offset += scr->c.x - (xstart + maxw - scr->c.pending - scr->save_handle_at_print);
        }

        return true;
    }

    register uint32_t *pbuf = scr->predec_buf;
    uint32_t *pbuf_end = pbuf + maxw;
    uint32_t *save_offset = NULL;

    if (fast_nrcs && skip_del && utf8) {
        do {
            const uint8_t *char_start = xstart;
            if (char_start == scr->save_handle_at_print)
                save_offset = pbuf;

            register uint32_t ch = *xstart++;

            if (LIKELY(ch < 0xC0)) {
                if (UNLIKELY(IS_DEL(ch)))
                    continue;
                if (UNLIKELY(pbuf == pbuf_end)) {
                    xstart = char_start;
                    break;
                }

                scr->c.gl_ss = scr->c.gl;

                *pbuf++ = nrcs_decode_fast(glv, ch);

                continue;
            } else if (LIKELY(ch < 0xE0)) {
                if (UNLIKELY((xstart += 1) > end)) goto partial;
                ch = (ch & 0x1F) << 6 | (char_start[1] & 0x3F);
                ch = UNLIKELY(ch < 0x80U) ? UTF_INVAL : ch;
            } else if (LIKELY(ch < 0xF0)) {
                if (UNLIKELY((xstart += 2) > end)) goto partial;
                ch = (ch & 0xF) << 12 | (char_start[1] & 0x3F) << 6 | (char_start[2] & 0x3F);
                ch = UNLIKELY(ch < 0x800U || ch - 0xD800U < 0x800U) ? UTF_INVAL : ch;
            } else if (LIKELY(ch < 0xF8)) {
                if (UNLIKELY((xstart += 3) > end)) {
partial:
                    xstart = char_start;
                    complete = false;
                    break;
                }
                ch = (ch & 0x7) << 18 | (char_start[1] & 0x3F) << 12 | (char_start[2] & 0x3F) << 6 | (char_start[3] & 0x3F);
                ch = UNLIKELY(ch - 0x10000U > 0xFFFFFU) ? UTF_INVAL : compact(ch);
            } else {
                ch = UTF_INVAL;
            }

            scr->c.gl_ss = scr->c.gl;

            if (UNLIKELY(iscombining_compact(ch))) {
                if (UNLIKELY(pbuf == scr->predec_buf)) {
                    screen_precompose_at_cursor(scr, ch);
                } else {
                    uint32_t *p = pbuf - (1 + !pbuf[-1]);
                    *p = try_precompose(*p, ch);
                }
            } else {
                bool wide = iswide_compact(ch);

                *pbuf++ = ch;
                if (wide) *pbuf++ = 0;

                if (UNLIKELY(pbuf >= pbuf_end)) {
                    if (pbuf == pbuf_end) break;
                    uint32_t *p = pbuf - (1 + wide);
                    if (UNLIKELY(scr->predec_buf == p) && wide) {
                        if (scr->c.x == screen_max_x(scr) - 1) {
                            pbuf_end = scr->predec_buf + (scr->mode.wrap ? screen_max_x(scr) - screen_min_x(scr) : 2);
                            continue;
                        } else if (scr->c.x == screen_width(scr) - 1) {
                            pbuf_end = scr->predec_buf + 2;
                            continue;
                        }
                    }
                    pbuf = p;
                    xstart = char_start;
                    break;
                }
            }
        } while (xstart < chunk);
    } else {
        /* This is the most generic printing loop */
        do {
            const uint8_t *char_start = xstart;
            if (char_start == scr->save_handle_at_print)
                save_offset = pbuf;

            register uint32_t ch = *xstart++;

            /* Use fast path for the ASCII characters */
            if (LIKELY(ch < 0xC0 || !utf8)) {
                /* Skip DEL char if not 96 set. DEL can only be
                 * incountered in this branch. */
                if (UNLIKELY(IS_DEL(ch)) && skip_del)
                    continue;
                if (UNLIKELY(pbuf == pbuf_end)) {
                    xstart = char_start;
                    break;
                }

                /* Decode nrcs. In theory this should be disabled while in UTF-8 mode, but
                 * in practice applications use these symbols, so keep translating.
                 * But decode only allow only DEC Graph in GL, unless configured otherwise. */
                ch = LIKELY(fast_nrcs) ? nrcs_decode_fast(glv, ch) :
                        nrcs_decode(glv, scr->c.gn[scr->c.gr], scr->upcs, ch, nrcs);

                /* Reset single shift */
                scr->c.gl_ss = scr->c.gl;

                *pbuf++ = ch;

                continue;
            } else if (LIKELY(ch < 0xE0)) {
                if (UNLIKELY((xstart += 1) > end)) goto partial2;

                ch = (ch & 0x1F) << 6 | (char_start[1] & 0x3F);
                ch = UNLIKELY(ch < 0x80U) ? UTF_INVAL : ch;

                /* Decode nrcs. In theory this should be disabled while in UTF-8 mode, but
                 * in practice applications use these symbols, so keep translating.
                 * But decode only allow only DEC Graph in GL, unless configured otherwise. */
                ch = LIKELY(fast_nrcs) ? ch : nrcs_decode(glv, scr->c.gn[scr->c.gr], scr->upcs, ch, nrcs);

            } else if (LIKELY(ch < 0xF0)) {
                if (UNLIKELY((xstart += 2) > end)) goto partial2;

                ch = (ch & 0xF) << 12 | (char_start[1] & 0x3F) << 6 | (char_start[2] & 0x3F);
                ch = UNLIKELY(ch < 0x800U || ch - 0xD800U < 0x800U) ? UTF_INVAL : ch;
            } else if (LIKELY(ch < 0xF8)) {
                if (UNLIKELY((xstart += 3) > end)) {
partial2:          /* If we have encountered a partial UTF-8, print all we have and return */
                    xstart = char_start;
                    complete = false;
                    break;
                }

                ch = (ch & 0x7) << 18 | (char_start[1] & 0x3F) << 12 | (char_start[2] & 0x3F) << 6 | (char_start[3] & 0x3F);
                ch = UNLIKELY(ch - 0x10000U > 0xFFFFFU) ? UTF_INVAL : compact(ch);
            } else {
                ch = UTF_INVAL;
            }

            /* Reset single shift */
            scr->c.gl_ss = scr->c.gl;

            if (UNLIKELY(iscombining_compact(ch))) {
                /* Don't put zero-width charactes to predecode buffer */
                if (UNLIKELY(pbuf == scr->predec_buf))
                    screen_precompose_at_cursor(scr, ch);
                else {
                    uint32_t *p = pbuf - (1 + !pbuf[-1]);
                    /* Don't need uncompact/compact since all characters
                     * in precomposition table are less than CELL_ENC_COMPACT_BASE. */
                    *p = try_precompose(*p, ch);
                }
            } else {
                bool wide = iswide_compact(ch);

                *pbuf++ = ch;
                if (wide) *pbuf++ = 0;

                /* Don't include char if its too wide, unless we haven't printed anything
                 * it's a wide char at the right margin, or autowrap is disabled,
                 * and we are at right size of the screen. In those cases recalculate the limit. */
                if (UNLIKELY(pbuf >= pbuf_end)) {
                    if (pbuf == pbuf_end) break;
                    uint32_t *p = pbuf - (1 + wide);
                    if (UNLIKELY(p == scr->predec_buf) && wide) {
                        if (scr->c.x == screen_max_x(scr) - 1) {
                            pbuf_end = scr->predec_buf + (scr->mode.wrap ? screen_max_x(scr) - screen_min_x(scr) : 2);
                            continue;
                        } else if (scr->c.x == screen_width(scr) - 1) {
                            pbuf_end = scr->predec_buf + 2;
                            continue;
                        }
                    }
                    pbuf = p;
                    xstart = char_start;
                    break;
                }
            }
        } while (xstart < chunk);
    }

    scr->prev_ch = scr->predec_buf != pbuf ? pbuf[-1] : -1U; /* For REP CSI */

    *start = xstart;

    print_buffer(scr, scr->predec_buf, NULL, pbuf - scr->predec_buf);

    if (save_offset) {
        replace_handle(&scr->saved_handle, &scr->screen[scr->c.y]);
        scr->saved_handle.s.offset += scr->c.x - (pbuf - save_offset - scr->c.pending);
    }

    return complete;
}

ssize_t screen_dispatch_rep(struct screen *scr, int32_t rune, ssize_t rep) {
    if (iscombining(rune)) {
        /* Don't put zero-width charactes to predecode buffer */
        screen_precompose_at_cursor(scr, rune);
        return 0;
    }

    /* Compute maximal with to be printed at once */
    ssize_t maxw = screen_max_x(scr) - screen_min_x(scr), totalw = 0;

    uint32_t *pbuf = scr->predec_buf;
    if (!scr->c.pending || !scr->mode.wrap)
        maxw = (scr->c.x >= screen_max_x(scr) ? screen_width(scr) : screen_max_x(scr)) - scr->c.x;

    if (iswide(rune)) {
        /* Allow printing at least one wide char at right margin
         * if autowrap is off. */
        if (maxw < 2) maxw = 2;

        /* If autowrap is on, in this case line will be
         * wrapped so we can print a lot more. */
        if (scr->mode.wrap && scr->c.x == screen_max_x(scr) - 1)
            maxw = screen_max_x(scr) - screen_min_x(scr);

        totalw = MIN(maxw/2, rep);
        rep -= totalw;
        while (totalw-- > 0)
            *pbuf++ = rune, *pbuf++ = 0;
    } else {
        totalw = MIN(maxw, rep);
        rep -= totalw;
        while (totalw-- > 0)
            *pbuf++ = rune;
    }

    print_buffer(scr, scr->predec_buf, NULL, pbuf - scr->predec_buf);
    return rep;
}
