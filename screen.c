/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

#define _DEFAULT_SOURCE

#include "feature.h"

#include "screen.h"
#include "tty.h"

#include <assert.h>
#include <stdio.h>
#include <strings.h>

#define CBUF_STEP(c,m) ((c) ? MIN(4 * (c) / 3, m) : MIN(16, m))
#define PRINT_BLOCK_SIZE 256


inline static bool screen_at_bottom(struct screen *scr) {
    return !line_handle_cmp(&scr->view_pos, scr->screen);
}

inline static struct line_handle **get_main_screen(struct screen *scr) {
    return scr->mode.altscreen ? &scr->back_screen : &scr->screen;
}

inline static struct line_handle **get_alt_screen(struct screen *scr) {
    return !scr->mode.altscreen ? &scr->back_screen : &scr->screen;
}

inline static void free_line_list_until(struct screen *scr, struct line *line, struct line *until) {
    struct line *next;
    while (line != until) {
        next = line->next;
        if (line->selection_index)
            selection_clear(&scr->sstate);
        free_line(line);
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

    free_line_list_until(scr, scr->top_line.line, NULL);
    free_line_list_until(scr, (*get_alt_screen(scr))->line, NULL);

    free(scr->screen);
    free(scr->back_screen);
    free(scr->temp_screen);

    free(scr->tabs);
    free(scr->predec_buf);
}


/* Damage terminal screen, relative to view
 * Faster version for whole lines */
void screen_damage_lines(struct screen *scr, ssize_t ys, ssize_t yd) {
    struct line_handle vpos = screen_view(scr);
    screen_advance_iter(scr, &vpos, ys);
    for (ssize_t i = ys; i < yd; i++, screen_inc_iter(scr, &vpos))
        vpos.line->force_damage = 1;
}

void screen_damage_selection(struct screen *scr) {
    struct line_handle vpos = screen_view(scr);
    struct line *prev = NULL;
    for (ssize_t i = 0; i < scr->height; i++) {
        if (prev != vpos.line)
            selection_damage(&scr->sstate, vpos.line);
        screen_inc_iter(scr, &vpos);
        prev = vpos.line;
    }
}

void screen_damage_uri(struct screen *scr, uint32_t uri) {
    if (!uri) return;

    struct line_handle vpos = screen_view(scr);
    for (ssize_t i = 0; i < 0 + scr->height; i++) {
        struct line_handle view = screen_view_at(scr, &vpos);
        for (ssize_t j = 0; j <  MIN(scr->width, view.width); j++) {
            struct cell *pcell = view_cell(&view, j);
            if (view_attr(&view, pcell->attrid).uri == uri)
                pcell->drawn = 0;
        }
        screen_inc_iter(scr, &vpos);
    }
}

struct line_handle screen_view_at(struct screen *scr, struct line_handle *pos) {
    ssize_t wid = line_advance_width(pos->line, pos->offset, scr->width);
    struct line_handle res = dup_handle(pos);
    res.width = wid - pos->offset;
    return res;
}

inline static ssize_t inc_iter_width_width(struct line_handle *pos, ssize_t width) {
    bool registered = line_handle_is_registered(pos);
    bool res = 0;
    if (registered)
        line_handle_remove(pos);

    ssize_t offset = line_advance_width(pos->line, pos->offset, width);
    if (offset >= pos->line->size) {
        if (pos->line->next) {
            pos->line = pos->line->next;
            pos->offset = 0;
        } else {
            res = 1;
        }
    } else {
        pos->offset = offset;
    }

    if (registered)
        line_handle_add(pos);
    return res;
}

ssize_t screen_inc_iter(struct screen *scr, struct line_handle *pos) {
    return inc_iter_width_width(pos, scr->width);
}

inline static ssize_t advance_iter_with_width(struct line_handle *pos, ssize_t amount, ssize_t width) {
    bool registered = line_handle_is_registered(pos);
    if (registered)
        line_handle_remove(pos);

    if (amount < 0) {
        // TODO Little optimization
        amount += line_segments(pos->line, 0, width) - line_segments(pos->line, pos->offset, width);
        pos->offset = 0;
        while (amount < 0) {
            if (!pos->line->prev)
                break;
            pos->line = pos->line->prev;
            amount += line_segments(pos->line, 0, width);
        }
    }
    if (amount > 0) {
        while (amount) {
            ssize_t offset = line_advance_width(pos->line, pos->offset, width);
            if (offset >= pos->line->size) {
                if (!pos->line->next)
                    break;
                pos->line = pos->line->next;
                pos->offset = 0;
            } else
                pos->offset = offset;
            amount--;
        }
    }

    if (registered)
        line_handle_add(pos);
    return amount;

}

ssize_t screen_advance_iter(struct screen *scr, struct line_handle *pos, ssize_t amount) {
    return advance_iter_with_width(pos, amount, scr->width);
}

struct line_handle screen_view(struct screen *scr) {
    return dup_handle(&scr->view_pos);
}

struct line_handle screen_line_iter(struct screen *scr, ssize_t y) {
    struct line_handle pos = dup_handle(scr->screen);
    screen_advance_iter(scr, &pos, y);
    return pos;
}

void screen_reset_view(struct screen *scr, bool damage) {
    if (!screen_at_bottom(scr)) {
        scr->prev_c_view_changed = true;
        replace_handle(&scr->view_pos, scr->screen);
        selection_view_scrolled(&scr->sstate, scr);
    }

    if (damage)
        screen_damage_lines(scr, 0, scr->height);
}

inline static struct line *screen_concat_line(struct screen *scr, struct line *dst, struct line *src, bool opt) {
    if (dst && src)
        selection_concat(&scr->sstate, dst, src);
    struct line *new = concat_line(dst, src, opt);
    if (new && new->selection_index)
        selection_relocated(&scr->sstate, new);
    return new;
}

FORCEINLINE
inline static struct line *screen_split_line(struct screen *scr, struct line *src, ssize_t offset, struct line **dst1, struct  line **dst2) {
    if (LIKELY(!offset || offset >= src->size)) return src;

    struct line *dst1p, *dst2p;
    if (!dst1) dst1 = &dst1p;
    if (!dst2) dst2 = &dst2p;

    split_line(src, offset, dst1, dst2);

    if ((*dst1)->selection_index)
        selection_split(&scr->sstate, *dst1, *dst2);

    return *dst1;
}

inline static struct line *screen_realloc_line(struct screen *scr, struct line *line, ssize_t width) {
    struct line *new = realloc_line(line, width);

    if (new->selection_index)
        selection_relocated(&scr->sstate, new);

    return new;
}

inline static void screen_unwrap_line(struct screen *scr, ssize_t y) {
    struct line_handle *view = &scr->screen[y];

    if (!view_wrapped(view)) return;

    // Views are updates automatically
    // since they are based on line_handle's

    screen_split_line(scr, view->line, view->offset, NULL, NULL);
}

void screen_unwrap_cursor_line(struct screen *scr) {
    screen_unwrap_line(scr, scr->c.y);
}

inline static void screen_adjust_line2(struct screen *scr, struct line_handle *screen, ssize_t y, ssize_t size) {
    struct line_handle *view = &screen[y];
    ssize_t old_size = view->line->size;
    ssize_t new_size = view->offset + size;

    if (LIKELY(old_size >= new_size)) return;

    if (new_size > view->line->caps)
        screen_realloc_line(scr, view->line, new_size);

    struct cell c = MKCELL(0, view->line->pad_attrid);
    fill_cells(view->line->cell + old_size, c, new_size - old_size);

    /* When we are resizing continuation line view fixup
     * widths of previous parts of line */
    if (UNLIKELY(view->offset) && y > 0) {
        screen[--y].width = scr->width;
#if DEBUG_LINES
        assert(screen[y].offset <= old_size);
        while (--y > 0 && screen[y].line == view->line)
            assert(screen[y].width == scr->width);
#endif
    }

    view->width = size;
    view->line->size = new_size;
}

inline static void screen_adjust_line(struct screen *scr, ssize_t y, ssize_t size) {
    screen_adjust_line2(scr, scr->screen, y, size);
}

void screen_do_wrap(struct screen *scr) {
    screen_autoprint(scr);
    bool moved = screen_index(scr);
    screen_cr(scr);

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
            scr->screen[scr->c.y].line->size > MAX_LINE_LEN)
        scr->screen[scr->c.y].line->prev->wrapped = true;

    /* Otherwise if everything is fine and we can wrap */

    if (scr->c.y > 0)
        screen_adjust_line(scr, scr->c.y - 1, scr->width);

    struct line *current = scr->screen[scr->c.y].line;
    screen_concat_line(scr, current->prev, current, false);
}

void screen_free_scrollback(struct screen *scr, ssize_t max_size) {
    struct line_handle *screen_top = *get_main_screen(scr);
    if (screen_top) {

        if (screen_top->line->prev)
            screen_reset_view(scr, 0);

        free_line_list_until(scr, scr->top_line.line, screen_top->line);

        replace_handle(&scr->top_line, screen_top);
    }

    scr->sb_max_caps = max_size;
    scr->sb_limit = 0;
}

void screen_scroll_view(struct screen *scr, int16_t amount) {
    if (scr->mode.altscreen || !scr->sb_max_caps) return;

    amount = -amount;

    bool old_viewr = !line_handle_cmp(&scr->view_pos, scr->screen);
    /* Shortcut for the case when view is already at the bottom */
    if (old_viewr && amount > 0) return;

    ssize_t delta = screen_advance_iter(scr, &scr->view_pos, amount) - amount;
    int new_viewr = line_handle_cmp(&scr->view_pos, scr->screen);
    if (new_viewr > 0) {
        screen_reset_view(scr, 1);
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
    scr->prev_c_view_changed |= old_viewr != new_viewr;
}

/* Returns true if view should move down */
inline static ssize_t try_free_top_line(struct screen *scr) {
    struct line_handle *screen = *get_main_screen(scr);
    if (scr->top_line.line == screen->line) return 0;

    ssize_t view_moved = 0;
    if (scr->top_line.line == scr->view_pos.line)
        view_moved = line_segments(scr->view_pos.line, scr->view_pos.offset, scr->width);

    struct line *next_top = scr->top_line.line->next;
    scr->sb_limit--;

    if (scr->top_line.line->selection_index)
        selection_clear(&scr->sstate);

#if DEBUG_LINES
    assert(!scr->top_line.line->prev);
    assert(find_handle_in_line(&scr->top_line));
#endif

    free_line(scr->top_line.line);
    scr->top_line.line = next_top;
    scr->top_line.offset = 0;
    line_handle_add(&scr->top_line);

    return view_moved;
}

ssize_t screen_push_history_until(struct screen *scr, struct line *from, struct line *to, bool opt) {
    ssize_t view_offset = 0;

    if (from->seq > to->seq) {
        for (struct line *next; from->seq > to->seq; from = next)
            next = from->prev;
    } else {
        for (struct line *next; from->seq < to->seq; from = next) {
            next = from->next;
            if (opt) screen_concat_line(scr, from, NULL, true);
            if (++scr->sb_limit > scr->sb_max_caps)
                view_offset += try_free_top_line(scr);
        }
    }

    return view_offset;
}

static void resize_tabs(struct screen *scr, int16_t width) {
    scr->tabs = xrezalloc(scr->tabs, scr->width * sizeof *scr->tabs, width * sizeof *scr->tabs);

    if (width > scr->width) {
        ssize_t tab = scr->width ? scr->width - 1: 0, tabw = window_cfg(scr->win)->tab_width;
        while (tab > 0 && !scr->tabs[tab]) tab--;
        while ((tab += tabw) < width) scr->tabs[tab] = 1;
    }
}

struct line *create_lines_range(struct line *prev, struct line *next, struct line_handle *dst, ssize_t width,
                                const struct attr *attr, ssize_t count, struct line_handle *top, bool need_register) {
    if (count <= 0) return NULL;

    struct line *line = create_line(*attr, width);
    if (UNLIKELY(!prev && top))
        replace_handle(top, &(struct line_handle) { .line =  line });

    for (ssize_t i = 0; i < count; i++) {
        dst[i] = (struct line_handle) {
            .line = line,
        };
        if (need_register)
            line_handle_add(&dst[i]);
        attach_prev_line(line, prev);
        prev = line;
        if (i != count - 1)
            line = create_line(*attr, width);
    }

    attach_next_line(line, next);

    return line;
}

static void resize_altscreen(struct screen *scr, ssize_t width, ssize_t height) {
    struct line_handle **alts = get_alt_screen(scr);
    struct line_handle *screen = *alts;
    ssize_t minh = MIN(scr->height, height);

    for (ssize_t i = 0; i < scr->height; i++)
        line_handle_remove(&screen[i]);

    if (height < scr->height)
        free_line_list_until(scr, screen[height].line, NULL);

    static_assert(MALLOC_ALIGNMENT == _Alignof(struct line_handle), "Insufficient alignment");
    screen = xrealloc(screen, scr->height * sizeof *screen, height * sizeof *screen);
    *alts = screen;

    for (ssize_t i = 0; i < minh; i++) {
        line_handle_add(&screen[i]);
        screen_realloc_line(scr, screen[i].line, width);
        screen[i].width = MIN(screen[i].width, width);
    }

    if (scr->height < height) {
        create_lines_range(scr->height ? screen[scr->height - 1].line : NULL, NULL,
                           &screen[scr->height], width, &ATTR_DEFAULT, height - scr->height, NULL, true);
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
    // Resize predecode buffer
    scr->predec_buf = xrealloc(scr->predec_buf, scr->width*sizeof *scr->predec_buf, width*sizeof *scr->predec_buf);

    // Resize temporary screen buffer
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
        replace_handle(&scr->view_pos, &scr->top_line);
        break;
    case stick_none:
        // Keep line of lower left view cell at the bottom
        line_handle_remove(&scr->view_pos);
        scr->view_pos = dup_handle(lower_left);
        scr->view_pos.offset -= scr->view_pos.offset % scr->width;
        screen_advance_iter(scr, &scr->view_pos, 1 - scr->height);
        line_handle_add(&scr->view_pos);
    }

#if DEBUG_LINES
    assert(scr->view_pos.line->seq <= scr->screen->line->seq);
#endif
}

inline static void translate_screen_position(struct line_handle *first, struct line_handle *pos, struct cursor *c, ssize_t width) {
    struct line_handle it = dup_handle(first);

    if (line_handle_cmp(&it, pos) > 0) {
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
    } while (!inc_iter_width_width(&it, width));

#if DEBUG_LINES
    if (c->y == -1)
        warn("w=%zd y=%zd x=%zd cy=%zd", width, y, c->x, yy);
    assert(c->y >= 0);
    assert(c->x >= 0);
#endif
}

#if DEBUG_LINES
static void validate_main_screen(struct screen *scr) {
    struct line_handle *screen = *get_main_screen(scr);

    struct line_handle *prev = NULL;
    struct line *prev_ln = NULL;
    assert(!scr->top_line.line->prev);
    assert(!scr->top_line.offset);
    assert(!screen[scr->height - 1].line->next);
    assert(scr->view_pos.line);
    assert(scr->top_line.line);
    assert(line_handle_is_registered(&scr->top_line));
    assert(line_handle_is_registered(&scr->view_pos));
    if (!scr->mode.altscreen)
        assert(line_handle_cmp(&scr->top_line, &scr->view_pos) <= 0);
    assert(line_handle_cmp(&scr->view_pos, scr->screen) <= 0);
    assert(!screen[scr->height - 1].line->next);
    bool has_scr = false, has_view = false;
    for (struct line *ln = scr->top_line.line; ln; ln = ln->next) {
        if (ln == scr->view_pos.line) has_view = true;
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

    for (ssize_t i = 0; i < scr->height; i++) {
        struct line_handle *view = &screen[i];
        assert(view->width <= scr->width);
        if (view->width < scr->width) assert(!view_wrapped(view));
        assert(view->offset + view->width <= view->line->size);
        assert(line_handle_is_registered(view));
        assert(find_handle_in_line(view));
        if (prev) {
            assert((prev->line == view->line->prev && prev->line->next == view->line) || prev->line == view->line);
            assert(prev->line->seq <= view->line->seq);
        }
        prev = view;
    }
}

static void validate_altscreen(struct screen *scr) {
    struct line_handle *altscr = *get_alt_screen(scr);

    struct line_handle *prev = NULL;
    assert(!altscr[0].line->prev);
    assert(!altscr[scr->height - 1].line->next);
    for (ssize_t i = 0; i < scr->height; i++) {
        struct line_handle *view = &altscr[i];
        assert(view->width <= scr->width);
        assert(!view->offset);
        assert(line_handle_is_registered(view));
        assert(find_handle_in_line(view));
        assert(!view_wrapped(view));
        if (prev) {
            assert(prev->line == view->line->prev);
            assert(prev->line->next == view->line);
            assert(prev->line->seq < view->line->seq);
        }
        prev = view;
    }
}
#endif

inline static void round_offset_to_width(struct line_handle *handle, ssize_t width) {
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

enum stick_view resize_main_screen(struct screen *scr, ssize_t width, ssize_t height, struct line_handle *lower_left) {
    enum stick_view ret = stick_none;

    struct line_handle **pscr = get_main_screen(scr);
    struct line_handle *screen = *pscr;

    ssize_t y = 0;

    if (screen) {
        /* Create cursor handles */
        struct line_handle prev_first_line = dup_handle(screen);
        line_handle_add(&prev_first_line);

        struct cursor *c = scr->mode.altscreen ? &scr->last_scr_c : &scr->c;
        struct line_handle cursor_handle = {
            .line = screen[c->y].line,
            .offset = screen[c->y].offset + c->x,
        };
        line_handle_add(&cursor_handle);

        struct cursor *saved_c = scr->mode.altscreen ? &scr->back_saved_c : &scr->saved_c;
        struct line_handle saved_cursor_handle = {
            .line = screen[saved_c->y].line,
            .offset = screen[saved_c->y].offset + saved_c->x
        };
        line_handle_add(&saved_cursor_handle);

        screen_adjust_line2(scr, screen, c->y, c->x + 1);
        screen_adjust_line2(scr, screen, saved_c->y, saved_c->x + 1);

        struct line_handle it = dup_handle(&cursor_handle);
        round_offset_to_width(&it, width);

        /* Remove handles that are gonna be freed by realloc below */
        for (ssize_t i = 0; i < scr->height; i++)
            line_handle_remove(&screen[i]);

        *pscr = screen = xrealloc(*pscr, scr->height * sizeof **pscr, height * sizeof **pscr);

#if DEBUG_LINES
        assert(c->y >= 0);
        struct line_handle d0 = dup_handle(&it);
#endif
        ssize_t rest = advance_iter_with_width(&it, -c->y, width);
        if (rest) {
        /* Not enough lines in scrollback buffer to keep cursor on it original line,
         * need to allocate more */
#if DEBUG_LINES
            assert(rest < 0);
            assert(!it.line->prev);
            assert(!it.offset);
            assert(it.line == scr->top_line.line);
            if (scr->top_line.line)
                assert(find_handle_in_line(&scr->top_line));
#endif
            create_lines_range(NULL, it.line, screen, width,
                               &ATTR_DEFAULT, -rest, &scr->top_line, false);
            fixup_lines_seqno(it.line);
            it.line = scr->top_line.line;
#if DEBUG_LINES
        } else {
            struct line_handle d = dup_handle(&it);
            assert(!advance_iter_with_width(&d, c->y, width));
            assert(!line_handle_cmp(&d, &d0));
#endif
        }

        /* Calculate new cursor position */
        translate_screen_position(&it, &saved_cursor_handle, saved_c, width);
        translate_screen_position(&it, &cursor_handle, c, width);
        saved_c->y = MIN(saved_c->y, height - 1);

        /* If cursor will be shifted off-screen, some lines needs to be pushed
         * to scrollback to keep cusor on screen.. */
        if (c->y >= height) {
            ssize_t delta = c->y - (height - 1);
            c->y -= delta;
            saved_c->y = MAX(0, saved_c->y - delta);

            delta = advance_iter_with_width(&it, delta, width);
#if DEBUG_LINES
            assert(!delta);
#endif
        }

        // FIXME Can lower_left be reset at this point?

        /* Fixup history count */
        if (!lower_left->line) ret = stick_to_bottom;
        if (screen_push_history_until(scr, prev_first_line.line, it.line, scr->mode.minimize_scrollback) &&
            ret == stick_none && !lower_left->line) ret = stick_to_top;

        /* Recalculate line views that are on screen */
        do {
            if (y >= 0) {
                ssize_t view_width = line_advance_width(it.line, it.offset, width);
                screen[y] = it;
                screen[y].width = view_width - it.offset;
                line_handle_add(&screen[y]);
            }
            if (++y >= height) break;
        } while (!inc_iter_width_width(&it, width));

        /* Truncate lines that are below the last line of the screen */
        if (y >= height) {
            struct line_handle *bottom = &screen[height - 1];
            screen_split_line(scr, bottom->line, bottom->offset + width, NULL, NULL);
            free_line_list_until(scr, bottom->line->next, NULL);
            y = height;
        }

        line_handle_remove(&cursor_handle);
        line_handle_remove(&saved_cursor_handle);
        line_handle_remove(&prev_first_line);

#if DEBUG_LINES
        assert(scr->top_line.line);
        assert(find_handle_in_line(&scr->top_line));
#endif
    } else {
        *pscr = screen = xalloc(height * sizeof **pscr);
    }

    create_lines_range(y ? screen[y - 1].line : NULL, NULL,
                       &screen[y], width, &ATTR_DEFAULT, height - y, &scr->top_line, true);

    if (ret == stick_none && !lower_left->line)
        ret = stick_to_bottom;

    return ret;
}

void screen_resize(struct screen *scr, int16_t width, int16_t height) {
#if USE_URI
    // Reset active URL
    window_set_active_uri(scr->win, EMPTY_URI, 0);
#endif

    resize_aux(scr, width, height);
    resize_tabs(scr, width);
    resize_altscreen(scr, width, height);

    // Find line of bottom left cell
    struct line_handle lower_left = dup_handle(&scr->view_pos);
    if (lower_left.line) {
        line_handle_add(&lower_left);
        screen_advance_iter(scr, &lower_left, scr->height - 1);
    }

    enum stick_view stick = stick_none;

    if (!scr->screen || !line_handle_cmp(&scr->view_pos, scr->screen)) stick = stick_to_bottom;
    else if (!line_handle_cmp(&scr->view_pos, &scr->top_line)) stick = stick_to_top;

    enum stick_view stick_after_resize = resize_main_screen(scr, width, height, &lower_left);
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

    if (scr->c.x != scr->prev_c_x || scr->c.y != scr->prev_c_y ||
            scr->prev_c_hidden != c_hidden || scr->prev_c_view_changed || !blink_commited) {
        if (!c_hidden) screen_damage_cursor(scr);
        if ((!scr->prev_c_hidden || scr->prev_c_view_changed) &&
                scr->prev_c_y < scr->height && scr->prev_c_x < scr->screen[scr->prev_c_y].width)
            view_cell(&scr->screen[scr->prev_c_y],scr->prev_c_x)->drawn = 0;
    }

    scr->prev_c_x = scr->c.x;
    scr->prev_c_y = scr->c.y;
    scr->prev_c_hidden = c_hidden;
    scr->prev_c_view_changed = 0;

    if (scr->scroll_damage) {
        screen_damage_lines(scr, 0, scr->height);
        scr->scroll_damage = 0;
    }

    struct line_handle *cl = &scr->screen[scr->c.y];
    bool cursor = !c_hidden && (scr->c.x >= cl->width || !view_cell(cl, scr->c.x)->drawn || cl->line->force_damage);

    return window_submit_screen(scr->win, scr->c.x, scr->c.y, cursor, scr->c.pending);
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
inline static void screen_rect_pre(struct screen *scr, int16_t *xs, int16_t *ys, int16_t *xe, int16_t *ye) {
    *xs = MAX(screen_min_oy(scr), MIN(*xs, screen_max_ox(scr) - 1));
    *xe = MAX(screen_min_oy(scr), MIN(*xe, screen_max_ox(scr)));
    *ys = MAX(screen_min_oy(scr), MIN(*ys, screen_max_oy(scr) - 1));
    *ye = MAX(screen_min_oy(scr), MIN(*ye, screen_max_oy(scr)));

    for (int16_t i = *ys; i < *ye; i++) {
        screen_unwrap_line(scr, i);
        screen_adjust_line(scr, i, *xe);
    }
}

FORCEINLINE
inline static void screen_erase_pre(struct screen *scr, int16_t *xs, int16_t *ys, int16_t *xe, int16_t *ye, bool origin) {
    if (origin) screen_rect_pre(scr, xs, ys, xe, ye);
    else {
        *xs = MAX(0, MIN(*xs, scr->width - 1));
        *xe = MAX(0, MIN(*xe, scr->width));
        *ys = MAX(0, MIN(*ys, scr->height - 1));
        *ye = MAX(0, MIN(*ye, scr->height));

        for (int16_t i = *ys; i < *ye; i++) {
            screen_unwrap_line(scr, i);
            screen_adjust_line(scr, i, *xe);
        }
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
    screen_rect_pre(scr, &xs, &ys, &xe, &ye);

    // TODO Test this thing

    uint32_t res = 0, spc = 0, trm = 0;
    enum charset gr = scr->c.gn[scr->c.gr];
    bool first = 1, notrim = mode.no_trim;

    for (; ys < ye; ys++) {
        struct line_handle *line = &scr->screen[ys];
        for (int16_t i = xs; i < xe; i++) {
            uint32_t ch_orig = i >= line->width ? 0 : view_cell(line, i)->ch;
            uint32_t ch = ch_orig;
            struct attr attr = view_attr_at(line, i);
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
                if (attr.underlined) ch += 0x10;
                if (attr.reverse) ch += 0x20;
                if (attr.blink) ch += 0x40;
                if (attr.bold) ch += 0x80;
                if (attr.italic) ch += 0x100;
                if (attr.faint) ch += 0x200;
                if (attr.strikethrough) ch += 0x400;
                if (attr.invisible) ch += 0x800;
            }
            if (first || ch_orig || !attr_eq(&attr, &(struct attr){ .fg = attr.fg, .bg = attr.bg, .ul = attr.ul }))
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
    screen_erase_pre(scr, &xs, &ys, &xe, &ye, 1);
    uint32_t mask = attr_mask(attr);

    bool rect = scr->mode.attr_ext_rectangle;
    for (; ys < ye; ys++) {
        struct line_handle *line = &scr->screen[ys];
        // TODO Optimize
        for (int16_t i = xs; i < (rect || ys == ye - 1 ? xe : screen_max_ox(scr)); i++) {
            struct attr newa = view_attr_at(line, i);
            attr_mask_set(&newa, attr_mask(&newa) ^ mask);
            struct cell *cell = view_cell(line, i);
            cell->attrid = alloc_attr(line->line, newa);
            cell->drawn = 0;
        }
        if (!rect) xs = screen_min_ox(scr);
    }
}

void screen_apply_sgr(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye, struct attr *mask, struct attr *attr) {
    screen_erase_pre(scr, &xs, &ys, &xe, &ye, 1);
    uint32_t mmsk = attr_mask(mask);
    uint32_t amsk = attr_mask(attr) & mmsk;

    bool rect = scr->mode.attr_ext_rectangle;
    for (; ys < ye; ys++) {
        int16_t xend = rect || ys == ye - 1 ? xe : screen_max_ox(scr);
        screen_adjust_line(scr, ys, xend);
        struct line_handle *line = &scr->screen[ys];
        // TODO Optimize
        for (int16_t i = xs; i < xend; i++) {
            struct attr newa = view_attr_at(line, i);
            attr_mask_set(&newa, (attr_mask(&newa) & ~mmsk) | amsk);
            if (mask->fg) newa.fg = attr->fg;
            if (mask->bg) newa.bg = attr->bg;
            if (mask->ul) newa.ul = attr->ul;

            struct cell *cell = view_cell(line, i);
            cell->attrid = alloc_attr(line->line, newa);
            cell->drawn = 0;
        }
        if (!rect) xs = screen_min_ox(scr);
    }

}

struct attr screen_common_sgr(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye) {
    screen_rect_pre(scr, &xs, &ys, &xe, &ye);

    struct attr common = view_attr_at(&scr->screen[ys], xs);
    bool has_common_fg = 1, has_common_bg = 1, has_common_ul = 1;

    for (; ys < ye; ys++) {
        struct line_handle *line = &scr->screen[ys];
        for (int16_t i = xs; i < xe; i++) {
            struct attr attr = view_attr_at(line, i);
            has_common_fg &= (common.fg == attr.fg);
            has_common_bg &= (common.bg == attr.bg);
            has_common_ul &= (common.ul == attr.ul);
            attr_mask_set(&common, attr_mask(&common) & attr_mask(&attr));
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
        xs = MAX(0, MIN(xs, scr->width - 1));
        ys = MAX(0, MIN(ys, scr->height - 1));
        xd = MAX(0, MIN(xd, scr->width - 1));
        yd = MAX(0, MIN(yd, scr->height - 1));
        xe = MAX(0, MIN(MIN(xe - xs + xd, scr->width) - xd + xs, scr->width));
        ye = MAX(0, MIN(MIN(ye - ys + yd, scr->height) - yd + ys, scr->height));
    }

    if (xs >= xe || ys >= ye) return;

    if (yd <= ys) {
        for (; ys < ye; ys++, yd++) {
            screen_adjust_line(scr, ys, xe);
            screen_adjust_line(scr, yd, xd + (xe - xs));
            screen_unwrap_line(scr, yd);
            struct line_handle *sl = &scr->screen[ys], *dl = &scr->screen[yd];
            copy_line(dl->line, xd + dl->offset,
                      sl->line, xs + sl->offset, xe - xs);
        }
    } else {
        for (yd += ye - ys; ys < ye; ye--, yd--) {
            screen_adjust_line(scr, ye - 1, xe);
            screen_adjust_line(scr, yd - 1, xd + (xe - xs));
            screen_unwrap_line(scr, yd - 1);
            struct line_handle *sl = &scr->screen[ye - 1], *dl = &scr->screen[yd - 1];
            copy_line(dl->line, xd + dl->offset,
                      sl->line, xs + sl->offset, xe - xs);
        }
    }
}

void screen_fill(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye, bool origin, uint32_t ch) {
    // FIXME This does not necesseraly work since padding attribute is per line now

    screen_erase_pre(scr, &xs, &ys, &xe, &ye, origin);

    for (; ys < ye; ys++) {
        struct line_handle *line = &scr->screen[ys];
        if (!ch && line->width <= xs && !view_wrapped(line) &&
            attr_eq(attr_pad(line->line), &scr->sgr)) continue;
        ssize_t xe1 = MIN(xe, line->width);
        struct cell c = {
            .attrid = alloc_attr(line->line, scr->sgr),
            .ch = compact(ch),
        };
        fill_cells(view_cell(line, xs), c, xe1 - xs);
    }
}

/* Erase whole lines by resetting their sizes to 0 */
void screen_erase_fast(struct screen *scr, int16_t ys, int16_t ye, struct attr *attr) {

    struct line_handle *view = &scr->screen[ys];
    screen_split_line(scr, view->line, view->offset, NULL, NULL);

    while (ys < ye) {
#if DEBUG_LINES
        assert(!view->offset);
#endif

        screen_split_line(scr, view->line, scr->width, NULL, NULL);
        view->line->size = 0;
        view->line->force_damage = true;
        view->width = 0;

        // FIXME Prefer explicit relocation after moving to custom line allocator.
        // screen_realloc_line(scr, view->line, 0);
        // view->line->pad_attrid = alloc_attr(view->line, *attr);
        (void)attr;

        view = &scr->screen[++ys];
    }
}

void screen_erase(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye, bool origin) {
    screen_fill(scr, xs, ys, xe, ye, origin, 0);
}

void screen_protective_erase(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye, bool origin) {
    screen_erase_pre(scr, &xs, &ys, &xe, &ye, origin);

    for (; ys < ye; ys++) {
        struct line_handle *line = &scr->screen[ys];
        struct cell c = { .attrid = alloc_attr(line->line, scr->sgr) };
        for (int16_t i = xs; i < xe; i++)
            if (!view_attr_at(line, i).protected)
                *view_cell(line, i) = c;
    }
}

void screen_selective_erase(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye, bool origin) {
    screen_erase_pre(scr, &xs, &ys, &xe, &ye, origin);

    for (; ys < ye; ys++) {
        struct line_handle *line = &scr->screen[ys];
        for (ssize_t i = xs; i < xe; i++) {
            if (!view_attr_at(line, i).protected) {
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


    // This is a hack that allows using proper line editing with reverse wrap
    // mode while staying compatible with VT100 wrapping mode
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

void screen_swap_screen(struct screen *scr, bool damage) {
    selection_clear(&scr->sstate);
    if (!scr->mode.altscreen)
        scr->last_scr_c = scr->c;
    scr->mode.altscreen ^= 1;
    SWAP(scr->back_saved_c, scr->saved_c);
    SWAP(scr->back_saved_sgr, scr->saved_sgr);
    SWAP(scr->back_screen, scr->screen);
    screen_reset_view(scr, damage);
}

void screen_set_altscreen(struct screen *scr, bool set, bool clear, bool save) {
    if (scr->mode.disable_altscreen) return;
    if (set != scr->mode.altscreen) {
        if (set && save) screen_save_cursor(scr, 1);
        screen_swap_screen(scr, !set || !clear);
        if (!set && save) screen_save_cursor(scr, 0);
    }
    if (set && clear) {
        screen_erase(scr, 0, 0, scr->width, scr->height, 0);
    }
}

void screen_scroll_horizontal(struct screen *scr, int16_t left, int16_t amount) {
    ssize_t top = screen_min_y(scr), right = screen_max_x(scr), bottom = screen_max_y(scr);

    for (ssize_t i = top; i < bottom; i++) {
        struct line_handle *line = &scr->screen[i];
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

inline static void swap_3(struct line *top_after, struct line *mid_before, struct line *bottom_before) {
        struct line *top_before = detach_prev_line(top_after);
        struct line *mid_after = detach_next_line(mid_before);
        struct line *bottom_after = detach_next_line(bottom_before);

#if DEBUG_LINES
        assert(top_after->seq <= mid_before->seq);
        assert(mid_before->seq < bottom_before->seq);
#endif

        attach_next_line(top_before, mid_after);
        attach_next_line(bottom_before, top_after);
        attach_next_line(mid_before, bottom_after);
}

int16_t screen_scroll_fast(struct screen *scr, int16_t top, int16_t amount, bool save) {;
    ssize_t bottom = screen_max_y(scr);

    save &= save && !scr->mode.altscreen && top == 0 && amount >= 0;

    bool should_reset_view = screen_at_bottom(scr);
    bool should_reset_top = !save && !top && !line_handle_cmp(&scr->top_line, scr->screen);

    struct line_handle *first = &scr->screen[top];
    /* Force scrolled region borders to be line borders */
    if (!save)
        screen_split_line(scr, first->line, first->offset, NULL, NULL);

    struct line_handle *last = &scr->screen[bottom - 1];
    screen_split_line(scr, last->line, last->offset + scr->width, NULL, NULL);

    if (amount > 0) /* up */ {
        amount = MIN(amount, (bottom - top));
        ssize_t rest = (bottom - top) - amount;

        if (save) {
            struct line *first_to_hist = first->line;
            struct line *bottom_line = last->line;
            struct line *bottom_next = detach_next_line(bottom_line);

            ssize_t i = 0;
            for (; i < amount; i++) {
                struct line_handle *handle = &scr->screen[i];

                struct line_handle *prev = handle->prev;
                struct line_handle *next = handle->next;

                if (prev) prev->next = next;
                else handle->line->first_handle = next;
                if (next) next->prev = prev;
            }

            for (; i < bottom; i++) {
                struct line_handle *src = &scr->screen[i];
                struct line_handle *dst = &scr->screen[i - amount];

                *dst = *src;

                struct line_handle *prev = src->prev;
                struct line_handle *next = src->next;

                if (prev) prev->next = dst;
                else dst->line->first_handle = dst;
                if (next) next->prev = dst;
            }

#if DEBUG_LINES
            if (rest) assert(scr->screen[rest - 1].line == bottom_line);
            assert(!bottom_line->next);
            if (bottom_next) assert(!bottom_next->prev);
#endif

            create_lines_range(bottom_line, bottom_next, &scr->screen[rest],
                               scr->width, &scr->sgr, amount, NULL, true);

            fixup_lines_seqno(bottom_next);

            ssize_t scrolled = screen_push_history_until(scr, first_to_hist, first->line, scr->mode.minimize_scrollback);
            if (UNLIKELY(scrolled)) /* View down, image up */ {
                replace_handle(&scr->view_pos, &scr->top_line);
                selection_view_scrolled(&scr->sstate, scr);
            }

        } else {
            screen_erase_fast(scr, top, top + amount, &scr->sgr);

            if (rest && amount) {
                struct line *mid = scr->screen[top + amount - 1].line;
                swap_3(first->line, mid, last->line);
                if (should_reset_top && !top)
                    replace_handle(&scr->top_line, scr->screen);
            }

            for (ssize_t i = top; i < bottom; i++)
                line_handle_remove(&scr->screen[i]);

            memcpy(scr->temp_screen, scr->screen + top, amount*sizeof(*scr->temp_screen));
            memmove(scr->screen + top, scr->screen + top + amount, rest*sizeof(*scr->temp_screen));
            memcpy(scr->screen + top + rest, scr->temp_screen, amount*sizeof(*scr->temp_screen));

            for (ssize_t i = top; i < bottom; i++)
                line_handle_add(&scr->screen[i]);

            if (bottom - amount >= 0)
                fixup_lines_seqno(scr->screen[bottom - amount].line);
        }
    } else if (amount < 0) /* down */ {
        amount = MAX(amount, -(bottom - top));
        ssize_t rest = (bottom - top) + amount;

        screen_erase_fast(scr, bottom + amount, bottom, &scr->sgr);

        if (rest /* && amount */) {
            struct line *mid = scr->screen[bottom - 1 + amount].line;
            swap_3(first->line, mid, last->line);
        }

        for (ssize_t i = top; i < bottom; i++)
            line_handle_remove(&scr->screen[i]);

        memcpy(scr->temp_screen, scr->screen + bottom + amount, -amount*sizeof(*scr->temp_screen));
        memmove(scr->screen + top - amount, scr->screen + top, rest*sizeof(*scr->temp_screen));
        memcpy(scr->screen + top, scr->temp_screen, -amount*sizeof(*scr->temp_screen));

        for (ssize_t i = top; i < bottom; i++)
            line_handle_add(&scr->screen[i]);

        if (top - amount < scr->height)
            fixup_lines_seqno(scr->screen[top - amount].line);
    }

    if (LIKELY(amount)) {
        scr->scroll_damage = 1;

        if (should_reset_top)
            replace_handle(&scr->top_line, &scr->screen[0]);

        if (should_reset_view) {
            replace_handle(&scr->view_pos, &scr->screen[0]);
            window_delay_redraw(scr->win);
        }

        // Update position of selection, if scrolled
        selection_scrolled(&scr->sstate, scr, amount, top, bottom);
    }

#if DEBUG_LINES
    validate_altscreen(scr);
    validate_main_screen(scr);
#endif
    return amount;
}

void screen_scroll(struct screen *scr, int16_t top, int16_t amount, bool save) {
    ssize_t left = screen_min_x(scr), right = screen_max_x(scr);

    if (LIKELY(left == 0 && right == scr->width)) { // Fast scrolling without margins
        amount = screen_scroll_fast(scr, top, amount, save);
    } else { // Slow scrolling with margins
        ssize_t bottom = screen_max_y(scr);
        for (ssize_t i = top; i < bottom; i++) {
            struct line_handle *line = &scr->screen[i];
            view_adjust_wide_left(line, left);
            view_adjust_wide_right(line, right - 1);
        }

        if (amount > 0) { /* up */
            amount = MIN(amount, bottom - top);
            screen_copy(scr, left, top + amount, right, bottom, left, top, 0);
            screen_erase(scr, left, bottom - amount, right, bottom, 0);
        } else { /* down */
            amount = MIN(-amount, bottom - top);
            screen_copy(scr, left, top, right, bottom - amount, left, top + amount, 0);
            screen_erase(scr, left, top, right, top + amount, 0);
        }
    }

    if (UNLIKELY(scr->mode.smooth_scroll) && (scr->scrolled += abs(amount)) > window_cfg(scr->win)->smooth_scroll_step) {
        window_request_scroll_flush(scr->win);
        scr->scrolled = 0;
    }
}

void screen_insert_cells(struct screen *scr, int16_t n) {
    if (screen_cursor_in_region(scr)) {
        n = MIN(n, screen_max_x(scr) - scr->c.x);
        if (n <= 0) return;

        // TODO Don't resize line to terminal width
        //      if it is not required.

        screen_adjust_line(scr, scr->c.y, screen_max_x(scr));
        struct line_handle *line = &scr->screen[scr->c.y];

        view_adjust_wide_left(line, scr->c.x);
        view_adjust_wide_right(line, scr->c.x);

        memmove(view_cell(line, scr->c.x + n), view_cell(line, scr->c.x),
                (screen_max_x(scr) - scr->c.x - n) * sizeof(struct cell));
        for (int16_t i = scr->c.x + n; i < screen_max_x(scr); i++)
            view_cell(line, i)->drawn = 0;

        screen_erase(scr, scr->c.x, scr->c.y, scr->c.x + n, scr->c.y + 1, 0);
        if (view_selection_intersects(&scr->sstate, line, screen_max_x(scr) - n, screen_max_x(scr))) {
            screen_damage_selection(scr);
            selection_clear(&scr->sstate);
        }
    }

    screen_reset_pending(scr);
}

void screen_delete_cells(struct screen *scr, int16_t n) {
    // Do not check top/bottom margins, DCH should work outside them
    if (scr->c.x >= screen_min_x(scr) && scr->c.x < screen_max_x(scr)) {

        // TODO Shrink line
        // We can optimize this code by avoiding allocation and movement of empty cells
        // and just shrink the line.
        screen_adjust_line(scr, scr->c.y, screen_max_x(scr));
        struct line_handle *line = &scr->screen[scr->c.y];

        n = MIN(n, screen_max_x(scr) - scr->c.x);
        if (n <= 0) return;

        view_adjust_wide_left(line, scr->c.x);
        view_adjust_wide_right(line, scr->c.x + n - 1);

        memmove(view_cell(line, scr->c.x), view_cell(line, scr->c.x + n),
                (screen_max_x(scr) - scr->c.x - n) * sizeof(struct cell));
        for (int16_t i = scr->c.x; i < screen_max_x(scr) - n; i++)
            view_cell(line, i)->drawn = 0;

        screen_erase(scr, screen_max_x(scr) - n, scr->c.y, screen_max_x(scr), scr->c.y + 1, 0);
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

    if (reset) {
        free_selection(&scr->sstate);
        if (!init_selection(&scr->sstate, screen_window(scr))) return 0;

        scr->mode = (struct screen_mode) {
            .disable_altscreen = !cfg->allow_altscreen,
            .wrap = cfg->wrap,
        };

        scr->c = scr->back_saved_c = scr->saved_c = (struct cursor) {
            .gl = 0, .gl_ss = 0, .gr = 2,
            .gn = {cs94_ascii, cs94_ascii, cs94_ascii, cs94_ascii}
        };

#if USE_URI
        window_set_mouse(scr->win, 1);
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
    scr->mode.minimize_scrollback = cfg->minimize_scrollback;
    return 1;
}

bool init_screen(struct screen *scr, struct window *win) {
    scr->win = win;
    init_printer(&scr->printer, window_cfg(win));
    return screen_load_config(scr, 1);
}

char *encode_sgr(char *dst, char *end, struct attr *attr) {
#define FMT(...) dst += snprintf(dst, end - dst, __VA_ARGS__)
#define MAX_SGR_LEN 54
    // Maximal length sequence is "0;1;2;3;4;6;7;8;9;38:2:255:255:255;48:2:255:255:255"

    // Reset everything
    FMT("0");

    // Encode attributes
    if (attr->bold) FMT(";1");
    if (attr->faint) FMT(";2");
    if (attr->italic) FMT(";3");
    if (attr->underlined == 1) FMT(";4");
    else if (attr->underlined > 1) FMT(";4:%d", attr->underlined);
    if (attr->blink) FMT(";6");
    if (attr->reverse) FMT(";7");
    if (attr->invisible) FMT(";8");
    if (attr->strikethrough) FMT(";9");

    // Encode foreground color
    if (color_idx(attr->fg) < 8) FMT(";%u", 30 + color_idx(attr->fg));
    else if (color_idx(attr->fg) < 16) FMT(";%u", 90 + color_idx(attr->fg) - 8);
    else if (color_idx(attr->fg) < PALETTE_SIZE - SPECIAL_PALETTE_SIZE) FMT(";38:5:%u", color_idx(attr->fg));
    else if (color_idx(attr->fg) == SPECIAL_FG) /* FMT(";39") -- default, skip */;
    else if (is_direct_color(attr->fg)) FMT(";38:2:%u:%u:%u", color_r(attr->fg), color_g(attr->fg), color_b(attr->fg));

    // Encode background color
    if (color_idx(attr->bg) < 8) FMT(";%u", 40 + color_idx(attr->bg));
    else if (color_idx(attr->bg) < 16) FMT(";%u", 100 + color_idx(attr->bg) - 8);
    else if (color_idx(attr->bg) < PALETTE_SIZE - SPECIAL_PALETTE_SIZE) FMT(";48:5:%u", color_idx(attr->bg));
    else if (color_idx(attr->bg) == SPECIAL_FG) /* FMT(";49") -- default, skip */;
    else if (is_direct_color(attr->bg)) FMT(";48:2:%u:%u:%u", color_r(attr->bg), color_g(attr->bg), color_b(attr->bg));

    // Encode underline color
    if (color_idx(attr->ul) < PALETTE_SIZE - SPECIAL_PALETTE_SIZE) FMT(";58:5:%u", color_idx(attr->ul));
    else if (color_idx(attr->ul) == SPECIAL_FG) /* FMT(";59") -- default, skip */;
    else if (is_direct_color(attr->ul)) FMT(";58:2:%u:%u:%u", color_r(attr->ul), color_g(attr->ul), color_b(attr->ul));

    return dst;
#undef FMT
}

void screen_print_line(struct screen *scr, struct line_handle *line) {
    if (!printer_is_available(&scr->printer)) return;

    uint8_t buf[PRINT_BLOCK_SIZE];
    uint8_t *pbuf = buf, *pend = buf + PRINT_BLOCK_SIZE;

    struct attr prev = {0};

    for (int16_t i = 0; i < line->width; i++) {
        struct cell c = *view_cell(line, i);
        struct attr attr = view_attr_at(line, i);

        if (window_cfg(scr->win)->print_attr && (!attr_eq(&prev, &attr) || !i)) {
            /* Print SGR state, if it have changed */
            *pbuf++ = '\033';
            *pbuf++ = '[';
            pbuf = (uint8_t *)encode_sgr((char *)pbuf, (char *)pend, &attr);
            *pbuf++ = 'm';
        }

        /* Print blanks as ASCII space */
        if (!c.ch) c.ch = ' ';

        //TODO Encode NRCS when UTF is disabled
        //for now just always print as UTF-8
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
    struct line *line = !scr->mode.altscreen && scr->top_line.line ?
            scr->top_line.line : scr->screen[0].line;

    while (line) {
        screen_print_line(scr, &(struct line_handle) {
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
            screen_do_wrap(scr);
        while (scr->c.x < screen_max_x(scr) - 1 && n--) {
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

inline static int32_t decode_special(const uint8_t **buf, const uint8_t *end, bool raw) {
    uint32_t part = *(*buf)++, i;
    if (LIKELY(part < 0xC0 || raw)) return part;
    if (UNLIKELY(part > 0xF7)) return UTF_INVAL;

    uint32_t len = (uint8_t[7]){ 1, 1, 1, 1, 2, 2, 3 }[(part >> 3U) - 24];

    if (*buf + len > end) {
        (*buf)--;
        return -1;
    }

    part &= 0x7F >> len;
    for (i = len; i--;) {
        if (UNLIKELY((**buf & 0xC0) != 0x80)) return UTF_INVAL;
        part = (part << 6) | (*(*buf)++ & 0x3F);
    }

    static const uint32_t maxv[] = {0x80, 0x800, 0x10000, 0x110000};
    if (UNLIKELY(part >= maxv[len] || part - 0xD800 < 0xE000 - 0xD800)) return UTF_INVAL;

    return part;
}

#define BLOCK_MASK 0x4040404040404040ULL

inline static uint64_t read_block_mask(const uint8_t *start) {
    uint64_t b = *(const uint64_t *)start;
    return ((b | (b << 1)) & BLOCK_MASK) ^ BLOCK_MASK;
}

inline static const uint8_t *block_mask_offset(const uint8_t *start, const uint8_t *end, uint64_t b) {
    return MIN(end, start + (__builtin_ffsll(b) - sizeof(uint64_t) + 1)/sizeof(uint64_t));
}

inline static const uint8_t *find_chunk(const uint8_t *start, const uint8_t *end, ssize_t max_chunk) {
    if (end - start >= (ssize_t)sizeof(uint64_t)) {
        if (start + max_chunk < end) end = start + max_chunk;

        ssize_t unaligned_prefix = (uintptr_t)start & (sizeof(uint64_t) - 1);
        if (unaligned_prefix) {
            uint64_t res = read_block_mask(start - unaligned_prefix) >> (unaligned_prefix * sizeof(uint64_t));
            if (UNLIKELY(res))
                return block_mask_offset(start, end, res);
            start += sizeof(uint64_t) - unaligned_prefix;
        }

        // Process 8 aligned bytes at a time
        // We have out-of bounds read here but it
        // should be fine since it it aligned
        // and buffer size is a power of two

        for (; start < end; start += sizeof(uint64_t)) {
            uint64_t res = read_block_mask(start);
            if (UNLIKELY(res))
                return block_mask_offset(start, end, res);
        }

        return end;
    } else {
        while (start < end && !IS_CBYTE(*start)) start++;
        return start;
    }
}

inline static void print_buffer(struct screen *scr, uint32_t *bstart, uint32_t *bend) {
    uint32_t totalw = bend - bstart;

    if (scr->mode.wrap) {
        if (scr->c.pending || (scr->c.x == screen_max_x(scr) - 1 && !bstart[1]))
            screen_do_wrap(scr);
    } else scr->c.x = MIN(scr->c.x, screen_max_x(scr) - totalw);

    struct line_handle *line = &scr->screen[scr->c.y];
    struct cell *cell = NULL;

    // Writing to the line resets its wrapping state
    screen_unwrap_line(scr, scr->c.y);

    ssize_t max_cx = scr->c.x + totalw, cx = scr->c.x;
    ssize_t max_tx = screen_max_x(scr);

    if (max_cx < line->width)
        view_adjust_wide_right(line, max_cx - 1);

    // Shift characters to the left if insert mode is enabled
    if (UNLIKELY(scr->mode.insert) && max_cx < max_tx && cx < line->width) {
        ssize_t max_new_size = MIN(max_tx, line->width + totalw);
        if (line->width < max_new_size)
            screen_adjust_line(scr, scr->c.y, max_new_size);

        cell = view_cell(line, cx);

        for (struct cell *c = cell + totalw; c < view_cell(line, max_tx); c++) c->drawn = 0;
        memmove(cell + totalw, cell, (max_tx - max_cx)*sizeof(*cell));
        max_cx = MAX(max_cx, max_tx);
    } else {
        if (line->width < max_cx)
            screen_adjust_line(scr, scr->c.y, max_cx);
        cell = view_cell(line, cx);
    }

    // Clear selection if writing over it
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

    // Erase overwritten parts of wide characters
    view_adjust_wide_left(line, cx);

    cx += totalw;

    scr->c.pending = cx == max_tx;
    scr->c.x = cx - scr->c.pending;

    // Allocate color for cell
    uint32_t attrid = alloc_attr(line->line, *screen_sgr(scr));

    // Put charaters
    copy_cells_with_attr(cell, bstart, bend, attrid);

    if (UNLIKELY(gconfig.trace_characters)) {
        for (uint32_t *px = bstart; px < bend; px++) {
            info("Char: (%x) '%lc' ", *px, (wint_t)*px);
        }
    }
}

ssize_t screen_dispatch_print(struct screen *scr, const uint8_t **start, const uint8_t *end, bool utf8, bool nrcs) {
    ssize_t res = 1;

    // Compute maximal with to be printed at once
    register ssize_t maxw = screen_max_x(scr) - screen_min_x(scr), totalw = 0;
    uint32_t *pbuf = scr->predec_buf;
    if (!scr->c.pending || !scr->mode.wrap)
        maxw = (scr->c.x >= screen_max_x(scr) ? screen_width(scr) : screen_max_x(scr)) - scr->c.x;

    register int32_t ch;

    uint32_t prev = -1U;
    const uint8_t *xstart = *start;
    enum charset glv = scr->c.gn[scr->c.gl_ss];

    bool fast_nrcs = utf8 && !window_cfg(scr->win)->force_utf8_nrcs;
    bool skip_del = glv > cs96_latin_1 || (!nrcs && (glv == cs96_latin_1 || glv == cs94_british));

    // Find the actual end of buffer
    // (control character or number of characters)
    // to prevent checking that on each iteration
    // and preload cache.
    // Maximal possible size of the chunk is
    // 4 times width of the lines since
    // each UTF-8 can be at most 4 bytes single width

    const uint8_t *chunk = find_chunk(xstart, end, maxw*4);

    do {
        const uint8_t *char_start = xstart;
        if (UNLIKELY((ch = decode_special(&xstart, end, !utf8)) < 0)) {
            // If we encountered partial UTF-8,
            // print all we have and return
            res = 0;
            break;
        }

        // Skip DEL char if not 96 set
        if (UNLIKELY(IS_DEL(ch)) && skip_del) continue;

        // Decode nrcs
        // In theory this should be disabled while in UTF-8 mode, but
        // in practice applications use these symbols, so keep translating.
        // But decode only allow only DEC Graph in GL, unless configured otherwise
        if (LIKELY(fast_nrcs))
            ch = nrcs_decode_fast(glv, ch);
        else
            ch = nrcs_decode(glv, scr->c.gn[scr->c.gr], scr->upcs, ch, nrcs);
        scr->c.gl_ss = scr->c.gl; // Reset single shift

        prev = ch;

        if (UNLIKELY(iscombining(ch))) {
            // Don't put zero-width charactes
            // to predecode buffer
            if (!totalw) screen_precompose_at_cursor(scr, ch);
            else {
                uint32_t *p = pbuf - 1 - !pbuf[-1];
                *p = compact(try_precompose(uncompact(*p), ch));
            }
        } else {
            int wid = 1 + iswide(ch);

            // Don't include char if its too wide, unless its a wide char
            // at right margin, or autowrap is disabled, and we are at right size of the screen
            // In those cases recalculate maxw
            if (UNLIKELY(totalw + wid > maxw)) {
                if (LIKELY(totalw || wid != 2)) {
                    xstart = char_start;
                    break;
                } else if (scr->c.x == screen_max_x(scr) - 1) {
                    maxw = scr->mode.wrap ? screen_max_x(scr) - screen_min_x(scr) : wid;
                } else if (scr->c.x == screen_width(scr) - 1) {
                    maxw = wid;
                } else {
                    xstart = char_start;
                    break;
                }
            }

            *pbuf++ = compact(ch);
            totalw += wid;

            if (wid > 1)
                *pbuf++ = 0;
        }

        // Since maxw < width == length of predec_buf, don't check it
    } while(totalw < maxw && /* count < FD_BUF_SIZE && */ xstart < chunk);

    *start = xstart;

    if (prev != -1U) scr->prev_ch = prev; // For REP CSI

    assert(pbuf - scr->predec_buf == totalw);
    print_buffer(scr, scr->predec_buf, pbuf);
    return res;
}

ssize_t screen_dispatch_rep(struct screen *scr, int32_t rune, ssize_t rep) {
    if (iscombining(rune)) {
        // Don't put zero-width charactes
        // to predecode buffer
        screen_precompose_at_cursor(scr, rune);
        return 0;
    }

    // Compute maximal with to be printed at once
    ssize_t maxw = screen_max_x(scr) - screen_min_x(scr), totalw = 0;

    uint32_t *pbuf = scr->predec_buf;
    if (!scr->c.pending || !scr->mode.wrap)
        maxw = (scr->c.x >= screen_max_x(scr) ? screen_width(scr) : screen_max_x(scr)) - scr->c.x;

    if (iswide(rune)) {
        // Allow printing at least one wide char at right margin
        // if autowrap is off
        if (maxw < 2) maxw = 2;

        // If autowrap is on, in this case line will be
        // wrapped so we can print a lot more
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
    print_buffer(scr, scr->predec_buf, pbuf);
    return rep;
}
