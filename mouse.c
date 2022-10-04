/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

#include "feature.h"

#include "config.h"
#include "input.h"
#include "mouse.h"
#include "term.h"
#include "time.h"
#include "window.h"

#include <stdbool.h>
#include <string.h>

#define SEL_INIT_SIZE 32
#define CSI "\233"

#define foreach_segment_indexed(seg, idx, head) \
    ssize_t idx = 0; for (struct segment *seg = (head)->segs; seg < (head)->segs + (head)->size ? (idx += seg->offset, 1) : 0; idx += seg->length, seg++)


inline static struct segments *seg_head(struct selection_state *sel, struct line *line) {
    /* First pointer is always 0,
     * so we don't have to perform
     * addition checks */
    return sel->seg[line->selection_index];
}

inline static void free_segments(struct selection_state *sel, struct segments *head) {

    /* Here we need to offset all selection below current by one
     * to keep selected lines heads continuous.
     * FIXME Make heads be organized in double linked list */

    struct segments **phead = sel->seg + head->line->selection_index;
    struct segments **end = sel->seg + sel->seg_size;

    head->line->selection_index = SELECTION_EMPTY;

    while (++phead < end) {
        *(phead - 1)  = *phead;
        (*phead)->line->selection_index--;
    }

    sel->seg_size--;
    free(head);
}

#define SEGS_INIT_SIZE 2

inline static struct segments *alloc_head(struct selection_state *sel, struct line *line) {
    if (!adjust_buffer((void **)&sel->seg, &sel->seg_caps,
                       sel->seg_size + 2, sizeof *sel->seg)) return NULL;
    struct segments *head = malloc(sizeof *head + sizeof *head->segs * SEGS_INIT_SIZE);

    line->selection_index = sel->seg_size;
    sel->seg[sel->seg_size++] = head;
    head->line = line;
    head->size = 0;
    head->caps = SEGS_INIT_SIZE;
    head->new_line_flag = 1;

    return head;
}

inline static bool adjust_head(struct selection_state *sel, struct segments **phead, ssize_t inc) {
    struct segments *head = *phead;
    if (head->size + inc > head->caps) {
        ssize_t new_caps = MAX(4 * head->caps / 3, head->size + inc);
        ssize_t idx = head->line->selection_index;
        head = realloc(head, sizeof *head + new_caps * sizeof *head->segs);
        if (!head) return 0;

        *phead = sel->seg[idx] = head;
        head->caps = new_caps;
    }
    return 1;
}

#define SNAP_RIGHT INT16_MAX

static void append_segment(struct selection_state *sel, struct line *line, ssize_t x0, ssize_t x1) {
    struct segments *head = seg_head(sel, line);

    x0 = MIN(line->size, x0);
    if (x1 > line->size) x1 = SNAP_RIGHT + x0;

    if (!head && !(head = alloc_head(sel, line))) return;

    foreach_segment_indexed(seg, last_i, head);

    if (last_i >= SNAP_RIGHT) return;

    if (last_i == x0 && head->size) {
        head->segs[head->size - 1].length += x1 - x0;
    } else if (last_i <= x0) {
        if (!adjust_head(sel, &head, 1)) return;
        head->segs[head->size++] = (struct segment) {x0 - last_i, x1 - x0 };
    } else assert(0);
}

void selection_concat(struct selection_state *sel, struct line *dst, struct line *src) {
    struct segments *src_head = seg_head(sel, src);
    struct segments *dst_head = seg_head(sel, dst);

    if (!src_head) {
        if (!dst_head) return;

        foreach_segment_indexed(seg, last_i, dst_head);
        if (last_i < SNAP_RIGHT) return;

        assert(dst_head->segs[dst_head->size - 1].length == SNAP_RIGHT);
        dst_head->segs[dst_head->size - 1].length = dst->size - (last_i - SNAP_RIGHT);
        return;
    }

    if (!dst_head) {
        dst->selection_index = src->selection_index;

        src->selection_index = SELECTION_EMPTY;
        if (src_head->size)
            src_head->segs[0].offset += dst->size;

        src_head->line = dst;
        return;
    }

    assert(dst->selection_index + 1 == src->selection_index);

    size_t offset = 0;
    foreach_segment_indexed(seg, last_i, dst_head);

    if (last_i >= SNAP_RIGHT) {
        assert(dst_head->segs[dst_head->size - 1].length == SNAP_RIGHT);
        assert(dst->size - (last_i - SNAP_RIGHT) >= 0);

        dst_head->segs[dst_head->size - 1].length = dst->size - (last_i - SNAP_RIGHT);
        last_i = dst->size;
    }

    assert(last_i <= dst->size);

    /* Merge adjacent */
    if (src_head->size && !src_head->segs->offset && last_i == dst->size) {
        dst_head->segs[dst_head->size - 1].length = MIN(SNAP_RIGHT,
                src_head->segs[0].length + dst_head->segs[dst_head->size - 1].length);
        src_head->size--;
        offset = 1;
    }

    /* Append tail */
    if (src_head->size && adjust_head(sel, &dst_head, src_head->size)) {
        memcpy(dst_head->segs + dst_head->size,
               src_head->segs + offset,
               src_head->size * sizeof *src_head->segs);

        // NOTE dst width computation should be compatible with
        //      one in line_concat()
        // Also, this does not change if first segment was merged
        dst_head->segs[dst_head->size].offset += dst->size - last_i;
        dst_head->size += src_head->size;
    }

    free_segments(sel, src_head);
}

void selection_split(struct selection_state *sel, struct line *line, struct line *tail) {
    // Left line is the old one that contains selected fragments

    struct segments *head = seg_head(sel, line);
    if (!head) return;

    head->line = line;

    ssize_t new_head_size = -1;
    foreach_segment_indexed(seg, idx, head) {
        if (idx + seg->length > line->size) {
            if (idx <= line->size) {
                append_segment(sel, tail, 0, idx + seg->length - line->size);
                seg++->length = SNAP_RIGHT;
            } else {
                append_segment(sel, tail, idx - line->size, idx + seg->length - line->size);
            }
            if (new_head_size < 0)
                new_head_size = seg - head->segs;
            break;
        }
    }

    if (new_head_size >= 0)
            head->size = new_head_size;
}

void selection_relocated(struct selection_state *sel, struct line *line) {
    struct segments *head = seg_head(sel, line);
    if (!head) return;

    head->line = line;

    foreach_segment_indexed(seg, idx, head) {
        if (idx + seg->length > line->size) {
            if (idx <= line->size)
                seg++->length = SNAP_RIGHT;
            head->size = seg - head->segs;
            break;
        }
    }
}

void selection_clear(struct selection_state *sel) {
    if (sel->state == state_sel_none ||
        sel->state == state_sel_pressed) return;

    line_handle_remove(&sel->start);
    line_handle_remove(&sel->end);

    sel->start.line = NULL;
    sel->end.line = NULL;

    sel->state = state_sel_none;

    for (size_t i = 1; i < sel->seg_size; i++) {
        sel->seg[i]->line->selection_index = SELECTION_EMPTY;
        free(sel->seg[i]);
    }

    sel->seg_size = 1;

    if (sel->targ != clip_invalid && !sel->keep_selection) {
        window_set_clip(sel->win, NULL, CLIP_TIME_NOW, sel->targ);
        sel->targ = clip_invalid;
    }
}

bool selection_intersects(struct selection_state *sel, struct line *line, int16_t x0, int16_t x1) {
    struct segments *head = seg_head(sel, line);
    if (!head) return 0;

    foreach_segment_indexed(seg, idx, head)
        if (idx <= x1 - 1 && idx + seg->length - 1 >= x0)
            return 1;
    return 0;
}

static void damage_head(struct segments *head) {
    struct cell *cell = head->line->cell;
    foreach_segment_indexed(seg, idx, head) {

        if (head->line->size < idx + seg->length) {
            head->line->force_damage = 1;
            return;
        }

        for (ssize_t i = idx; i < seg->length + idx; i++) {
            cell[i].drawn = 0;
        }
    }
}

void selection_damage(struct selection_state *sel, struct line *line) {
    struct segments *head = seg_head(sel, line);
    if (head) damage_head(head);
}

inline static bool is_separator(uint32_t ch, char *seps) {
    if (!ch) return 1;
    uint8_t cbuf[UTF8_MAX_LEN + 1];
    cbuf[utf8_encode(ch, cbuf, cbuf + UTF8_MAX_LEN)] = '\0';
    return strstr(seps, (char *)cbuf);
}

static struct line_handle snap_backward(struct selection_state *sel, struct line_handle pos) {
    char *seps = window_cfg(sel->win)->word_separators;

    if (sel->snap == snap_line) {
        pos.offset = 0;
        struct line *prev_line;
        for (;;) {
            prev_line = pos.line->prev;
            if (!prev_line || !prev_line->wrapped) break;
            pos.line = prev_line;
        }
    } else if (sel->snap == snap_word) {
        struct line *line = pos.line, *prev;
        if (pos.offset >= line->size) {
            pos.offset = line->size;
            return pos;
        }

        for (;;) {
            /* We should not land on second cell of wide character */
            pos.offset -= !line->cell[pos.offset].ch && pos.offset && cell_wide(line->cell + pos.offset - 1);

            bool sep_cur = is_separator(cell_get(line->cell + pos.offset), seps);

            /* Go backwards until we hit word border */

            for (; pos.offset; ) {
                int delta = 1 + (pos.offset > 1 && cell_wide(line->cell + pos.offset - 2));
                if (pos.offset - delta < 0) break;
                if (sep_cur != is_separator(cell_get(line->cell + pos.offset - delta), seps)) return pos;
                pos.offset -= delta;
            }

            /* Go to previous line only if:
             *     - it exists
             *     - it is wrapped
             *     - it ends with character of same class */

            if (!(prev = pos.line->prev) || !prev->wrapped) break;
            if (!prev->size) break;

            line = prev;

            int delta = 1 + (line->size > 1 && cell_wide(line->cell + line->size - 2));
            if (is_separator(cell_get(line->cell + line->size - delta), seps) != sep_cur) break;

            pos.line = prev;
            pos.offset = line->size - delta;
        }
    }

    return pos;
}

static struct line_handle snap_forward(struct selection_state *sel, struct line_handle pos) {
    char *seps = window_cfg(sel->win)->word_separators;

    if (sel->snap == snap_line) {
        struct line *next = pos.line, *line;
        do line = next, next = line->next;
        while (next && line->wrapped);
        pos.line = line;
        pos.offset = SNAP_RIGHT;
    } else if (sel->snap == snap_word) {
        struct line *line = pos.line, *next;
        if (pos.offset >= line->size) {
            pos.offset = SNAP_RIGHT;
            return pos;
        }

        for (;;) {
            bool sep_cur = is_separator(cell_get(line->cell + pos.offset), seps);

            /* We should not land on first cell of wide character */
            pos.offset += cell_wide(line->cell + pos.offset);

            /* Go forward until we hit word border */

            for (; pos.offset < line->size; ) {
                int delta = 1 + (cell_wide(line->cell + pos.offset) && pos.offset < line->size - 1);
                if (line->size <= pos.offset + delta) break;
                if (sep_cur != is_separator(cell_get(line->cell + pos.offset + delta), seps)) return pos;
                pos.offset += delta;
            }

            /* Go to previous line only if:
             *     - it exists
             *     - it is wrapped
             *     - it ends with character of same class */

            if (!line->wrapped || !(next = pos.line->next)) break;

            line = next;

            if (!line->size) break;
            if (is_separator(cell_get(line->cell), seps) != sep_cur) break;

            pos.line = next;
            pos.offset = 0;
        }
    }

    return pos;
}

/*
 * This function converts an absolute position
 * represented as pos into virtual position of the visual line
 * on screen return back to pos and offset in that line
 * as a return value of the function.
 */

inline static ssize_t virtual_pos(struct screen *scr, struct line_handle *pos) {
    struct line_handle orig = *pos, next = *pos;
    next.offset = 0;

    do {
        *pos = next;
        if (screen_inc_iter(scr, &next)) break;
    } while (line_handle_cmp(&orig, &next) >= 0);

    return orig.offset - pos->offset;
}

inline static struct line_handle absolute_pos(struct screen *scr, ssize_t x, ssize_t y) {
    struct line_handle offset = screen_view(scr);
    screen_advance_iter(scr, &offset, y);
    offset.offset += x;
    return offset;
}

static void damage_changed(struct selection_state *sel, struct segments **old, size_t old_size) {
    for (size_t i = 1; i < old_size; i++) {
        struct line *line = old[i]->line;
        struct segments *new_head = seg_head(sel, line);
        if (!new_head) {
            damage_head(old[i]);
        } else {
            struct segment *seg_new = new_head->segs, *seg_old = old[i]->segs;
            struct segment *seg_new_end = seg_new + new_head->size, *seg_old_end = seg_old + old[i]->size;
            ssize_t new_start = seg_new->offset, old_start = seg_old->offset;
            ssize_t new_end = new_start + seg_new->length, old_end = old_start + seg_old->length;

            new_head->new_line_flag = 0;

            for (; seg_new < seg_new_end || seg_old < seg_old_end;) {
                bool advance_new = 0, advance_old = 0;
                ssize_t from = 0, to = 0;

                if (new_start < old_start) {
                    from = new_start;
                    to = MIN(new_end, old_start);
                } else if (new_start > old_start) {
                    from = old_start;
                    to = MIN(old_end, new_start);
                }

                if (old_end <= new_end) {
                    new_start = old_end;
                    advance_old = 1;
                }

                if (old_end >= new_end) {
                    old_start = new_end;
                    advance_new = 1;
                }

                if (to <= line->size) {
                    while (from < to)
                        line->cell[from++].drawn = 0;
                } else line->force_damage = 1;

                if (advance_old) {
                    if (seg_old < seg_old_end) {
                        seg_old++;
                        old_start = old_end + seg_old->offset;
                        old_end = old_start + seg_old->length;
                    } else {
                        old_end = old_start = INTPTR_MAX;
                    }
                }
                if (advance_new) {
                    if (seg_new < seg_new_end) {
                        seg_new++;
                        new_start = new_end + seg_new->offset;
                        new_end = new_start + seg_new->length;
                    } else {
                        new_end = new_start = INTPTR_MAX;
                    }
                }
            }
        }
        free(old[i]);
    }

    free(old);

    for (size_t i = 1; i < sel->seg_size; i++)
        if (sel->seg[i]->new_line_flag)
            damage_head(sel->seg[i]);
}

static void decompose(struct selection_state *sel, struct screen *scr, struct line_handle start, struct line_handle end) {
    if (sel->rectangular) {
        struct line_handle vstart = start, vend = end;
        ssize_t vstart_x = virtual_pos(scr, &vstart);
        ssize_t vend_x = virtual_pos(scr, &vend);
        if (vstart_x > vend_x)
            SWAP(vstart_x, vend_x);

        do {
            append_segment(sel, vstart.line, vstart.offset + vstart_x, vstart.offset + vend_x + 1);
            if (screen_inc_iter(scr, &vstart)) break;
        } while (line_handle_cmp(&vstart, &vend) <= 0);

    } else {
        for (; start.line->seq < end.line->seq; start.line = start.line->next) {
            append_segment(sel, start.line, start.offset, SNAP_RIGHT);
            start.offset = 0;
        }
        append_segment(sel, end.line, start.offset, end.offset + 1);
    }
}

bool init_selection(struct selection_state *sel, struct window *win) {
    sel->seg_caps = 4;
    sel->win = win;
    sel->seg_size = 1;
    sel->seg = calloc(sizeof *sel->seg, sel->seg_caps);
    return sel->seg;
}

void free_selection(struct selection_state *sel) {
    for (size_t i = 1; i < sel->seg_size; i++) {
        sel->seg[i]->line->selection_index = SELECTION_EMPTY;
        free(sel->seg[i]);
    }

    free(sel->seg);
    sel->seg_caps = 0;
    sel->seg_size = 0;
    sel->seg = NULL;
}

void selection_scrolled(struct selection_state *sel, struct screen *scr, int16_t x, ssize_t top, ssize_t bottom) {

    if (sel->state == state_sel_pressed ||
            sel->state == state_sel_progress) {

        if (!sel->start.line || !sel->end.line)
            selection_clear(sel);

        /* NOTE: This is slow, but if the invariant of the lines
         * on the screen having one-to-one correspondence
         * between struct line and visual line changes it would
         * be the only correct way to calculate the position */

        struct line_handle top_pos = screen_line_iter(scr, top);
        struct line_handle bottom_pos = screen_line_iter(scr, bottom);
        struct line_handle screen_pos = screen_line_iter(scr, 0);

        if (line_handle_cmp(&sel->start, &screen_pos) < 0 ||
                (line_handle_cmp(&sel->start, &top_pos) >= 0 &&
                 line_handle_cmp(&sel->start, &bottom_pos) < 0)) {
            ssize_t x_off = virtual_pos(scr, &sel->start);

            screen_advance_iter(scr, &sel->start, -x);
            sel->start.offset += x_off;

            selection_view_scrolled(sel, scr);
        }
    }
}

static void selection_changed(struct selection_state *sel, struct screen *scr, uint8_t state, bool rectangular) {
    struct instance_config *cfg = window_cfg(sel->win);
    struct line_handle pos = absolute_pos(scr, sel->pointer_x, sel->pointer_y);
    assert(!line_handle_is_registered(&pos));
    assert(pos.line);

    if (state == state_sel_pressed) {
        line_handle_remove(&sel->start);
        sel->start = pos;
        line_handle_add(&sel->start);

        struct timespec now;
        clock_gettime(CLOCK_TYPE, &now);

        if (TIMEDIFF(sel->click1, now) < cfg->triple_click_time*1000LL)
            sel->snap = snap_line;
        else if (TIMEDIFF(sel->click0, now) < cfg->double_click_time*1000LL)
            sel->snap = snap_word;
        else
            sel->snap = snap_none;

        sel->click1 = sel->click0;
        sel->click0 = now;
    }

    sel->state = state;
    sel->rectangular = rectangular;
    line_handle_remove(&sel->end);
    sel->end = pos;
    line_handle_add(&sel->end);

    struct line_handle nstart = dup_handle(&sel->start);
    struct line_handle nend = dup_handle(&sel->end);

    if (line_handle_cmp(&nstart, &nend) > 0)
        SWAP(nstart, nend);

    nstart = snap_backward(sel, nstart);
    nend = snap_forward(sel, nend);

    if (sel->snap != snap_none && sel->state == state_sel_pressed)
        sel->state = state_sel_progress;

    struct segments **prev_heads = sel->seg;
    size_t prev_size = sel->seg_size;
    init_selection(sel, sel->win);

    for (size_t i = 1; i < prev_size; i++)
        prev_heads[i]->line->selection_index = 0;

    if (sel->state == state_sel_progress || sel->state == state_sel_released)
        decompose(sel, scr, nstart, nend);

    // dump(sel);

    // Make changed cells dirty
    damage_changed(sel, prev_heads, prev_size);
}

struct mouse_selection_iterator selection_begin_iteration(struct selection_state *sel, struct line_view *view) {
    struct segments *head = seg_head(sel, view->h.line);
    struct mouse_selection_iterator it = { 0 };
    if (!head || !head->size) return it;

    foreach_segment_indexed(seg, idx, head);

    it.seg = head->segs + head->size - 1;
    it.idx = idx;
    return it;
}

bool is_selected_prev(struct mouse_selection_iterator *it, struct line_view *view, int16_t x0) {
    if (!it->idx) return 0;

    ssize_t x = x0 + view->h.offset;

    do {
        assert(it->idx >= 0);

        if (x >= it->idx) return 0;
        if (x >= it->idx - it->seg->length) return 1;

        it->idx -= it->seg->length + it->seg->offset;
        it->seg--;
    } while (it->idx && x < it->idx);

    return 0;
}

static void append_line(size_t *pos, size_t *cap, uint8_t **res, struct line *line, ssize_t x0, ssize_t x1, bool first) {
    ssize_t max_x = MIN(x1, line_length(line));

    if (!first) {
        if (!adjust_buffer((void **)res, cap, *pos + 2, 1)) return;
        (*res)[(*pos)++] = ' ';
    }

    for (ssize_t j = x0; j < max_x; j++) {
        uint8_t buf[UTF8_MAX_LEN];
        if (line->cell[j].ch) {
            size_t len = utf8_encode(cell_get(&line->cell[j]), buf, buf + UTF8_MAX_LEN);
            // 2 is space for '\n' and '\0'
            if (!adjust_buffer((void **)res, cap, *pos + len + 2, 1)) return;
            memcpy(*res + *pos, buf, len);
            *pos += len;
        }
    }

    if (!line->wrapped || (x1 != line->size && x1 != SNAP_RIGHT)) {
        if (!adjust_buffer((void **)res, cap, *pos + 2, 1)) return;
        (*res)[(*pos)++] = '\n';
    }
}

static uint8_t *selection_data(struct selection_state *sel) {
    if (sel->state == state_sel_released) {
        uint8_t *res = malloc(SEL_INIT_SIZE * sizeof(*res));
        if (!res) return NULL;
        size_t pos = 0, cap = SEL_INIT_SIZE;

        for (size_t i = 1; i < sel->seg_size; i++) {
            struct segments *head = sel->seg[i];
            bool first = 1;

            foreach_segment_indexed(seg, idx, head) {
                append_line(&pos, &cap, &res, head->line,
                            idx, idx + seg->length, first);
                first = 0;
            }
        }

        res[pos -= !!pos] = '\0';
        return res;
    } else return NULL;
}

void selection_view_scrolled(struct selection_state *sel, struct screen *scr) {
    if (sel->state == state_sel_progress)
        selection_changed(sel, scr, state_sel_progress, sel->rectangular);
}

inline static void adj_coords(struct window *win, int16_t *x, int16_t *y, bool pixel) {
    struct extent c = window_get_cell_size(win);
    struct extent b = window_get_border(win);
    struct extent g = window_get_grid_size(win);

    *x = MAX(0, MIN(g.width - 1, (*x - b.width)));
    *y = MAX(0, MIN(g.height - 1, (*y - b.height)));

    if (!pixel) {
         *x /= c.width;
         *y /= c.height;
    }
}

static void pending_scroll(struct selection_state *sel, struct screen *scr, int16_t y, enum mouse_event_type event) {
    struct extent c = window_get_cell_size(sel->win);
    struct extent b = window_get_border(sel->win);
    struct extent g = window_get_grid_size(sel->win);

    if (event == mouse_event_motion) {
        if (y - b.height >= g.height) sel->pending_scroll = MIN(-1, (g.height + b.height - y - c.height + 1) / c.height / 2);
        else if (y < b.height) sel->pending_scroll = MAX(1, (b.height - y + c.height - 1) / c.height / 2);
        selection_pending_scroll(sel, scr);
    }
}

bool selection_pending_scroll(struct selection_state *sel, struct screen *scr) {
    struct instance_config *cfg = window_cfg(sel->win);

    if (sel->pending_scroll && sel->state == state_sel_progress) {
        struct timespec now;
        clock_gettime(CLOCK_TYPE, &now);
        bool can_scroll = TIMEDIFF(sel->last_scroll, now) > cfg->select_scroll_time*1000LL;
        if (can_scroll) {
            screen_scroll_view(scr, sel->pending_scroll);
            sel->last_scroll = now;
        }
    }
    return sel->pending_scroll;
}

bool is_selection_event(struct selection_state *sel, struct mouse_event *ev) {
    return (ev->event == mouse_event_press && ev->button == 0) ||
           (ev->event == mouse_event_motion && ev->mask & mask_button_1 &&
            (sel->state == state_sel_progress || sel->state == state_sel_pressed)) ||
           (ev->event == mouse_event_release && ev->button == 0 && sel->state == state_sel_progress);
}

void mouse_report_locator(struct term *term, uint8_t evt, int16_t x, int16_t y, uint32_t mask) {

    uint32_t lmask = 0;
    if (mask & mask_button_3) lmask |= 1;
    if (mask & mask_button_2) lmask |= 2;
    if (mask & mask_button_1) lmask |= 4;
    if (mask & mask_button_4) lmask |= 8;

    struct window *win = term_window(term);
    struct extent b = window_get_border(win);
    struct extent g = window_get_grid_size(win);

    if (x < b.width || x >= g.width + b.width || y < b.height || y > g.height + b.height) {
        if (evt == 1) term_answerback(term, CSI"0&w");
    } else {
        adj_coords(win, &x, &y, term_get_mstate(term)->locator_pixels);
        term_answerback(term, CSI"%d;%d;%d;%d;1&w", evt, lmask, y + 1, x + 1);
    }
}

void mouse_set_filter(struct term *term, iparam_t xs, iparam_t xe, iparam_t ys, iparam_t ye) {
    if (xs > xe) SWAP(xs, xe);
    if (ys > ye) SWAP(ys, ye);

    xe++, ye++;

    struct mouse_state *loc = term_get_mstate(term);
    struct window *win = term_window(term);
    struct extent c = window_get_cell_size(win);
    struct extent b = window_get_border(win);
    struct extent g = window_get_grid_size(win);

    if (!loc->locator_pixels) {
        xs = xs * c.width + b.width;
        xe = xe * c.width + b.width;
        ys = ys * c.height + b.height;
        ye = ye * c.height + b.height;
    }

    xs = MIN(xs, b.width + g.width - 1);
    xe = MIN(xe, b.width + g.width);
    ys = MIN(ys, b.height + g.height - 1);
    ye = MIN(ye, b.height + g.height);

    loc->filter = (struct rect) { xs, ys, xe - xs, ye - ys };
    loc->locator_filter = 1;

    window_set_mouse(term_window(term), 1);
}

#if USE_URI
inline static bool is_button1_down(struct mouse_event *ev) {
    return (ev->event == mouse_event_press && ev->button == 0) ||
           (ev->mask & mask_button_1 && (ev->event != mouse_event_release || ev->button != 0));
}

static void update_active_uri(struct screen *scr, struct window *win, struct mouse_event *ev) {
    if (!window_cfg(win)->allow_uris) return;

    struct extent c = window_get_cell_size(win);
    struct extent b = window_get_border(win);
    struct extent g = window_get_grid_size(win);

    uint32_t uri = EMPTY_URI;
    if ((ev->x >= b.width && ev->x < g.width + b.width) && (ev->y >= b.height || ev->y < g.height + b.height)) {
        int16_t x = (ev->x - b.width) / c.width;
        int16_t y = (ev->y - b.height) / c.height;


        struct line_handle pos = screen_view(scr);
        screen_advance_iter(scr, &pos, y);

        if (pos.offset + x < pos.line->size)
            uri = attr_at(pos.line, pos.offset + x).uri;
    }
    window_set_active_uri(win, uri, is_button1_down(ev));

    uint32_t uri_mask = window_cfg(win)->uri_click_mask;
    if (uri && ev->event == mouse_event_release && ev->button == 0 &&
        (ev->mask & mask_mod_mask) == uri_mask) uri_open(uri);
}
#endif

void mouse_handle_input(struct term *term, struct mouse_event ev) {
    struct mouse_state *loc = term_get_mstate(term);
    struct selection_state *sel = term_get_sstate(term);
    struct screen *scr = term_screen(term);
    sel->pending_scroll = 0;

    uint32_t force_mask = window_cfg(term_window(term))->force_mouse_mask;

    /* Report mouse */
    if ((loc->locator_enabled | loc->locator_filter) && (ev.mask & mask_mod_mask) != force_mask &&
            !term_get_kstate(term)->keyboad_vt52) {
        if (loc->locator_filter) {
            if (ev.x < loc->filter.x || ev.x >= loc->filter.x + loc->filter.width ||
                    ev.y < loc->filter.y || ev.y >= loc->filter.y + loc->filter.height) {
                if (ev.event == mouse_event_press) ev.mask |= 1 << (ev.button + 8);
                mouse_report_locator(term, 10, ev.x, ev.y, ev.mask);
                loc->locator_filter = 0;
                window_set_mouse(term_window(term), loc->mouse_mode == mouse_mode_motion);
            }
        } else if (loc->locator_enabled) {
            if (loc->locator_oneshot) {
                loc->locator_enabled = 0;
                loc->locator_oneshot = 0;
            }

            if (ev.event == mouse_event_motion) return;
            else if (ev.event == mouse_event_press && !loc->locator_report_press) return;
            else if (ev.event == mouse_event_release && !loc->locator_report_release) return;

            if (ev.button < 3) {
                if (ev.event == mouse_event_press) ev.mask |= 1 << (ev.button + 8);
                mouse_report_locator(term, 2 + ev.button * 2 + (ev.event == mouse_event_release), ev.x, ev.y, ev.mask);
            }
        }
    } else if (loc->mouse_mode != mouse_mode_none &&
            (ev.mask & mask_mod_mask) != force_mask && !term_get_kstate(term)->keyboad_vt52) {
        enum mouse_mode md = loc->mouse_mode;

        adj_coords(term_window(term), &ev.x, &ev.y, loc->mouse_format == mouse_format_pixel);

        if (md == mouse_mode_x10 && ev.button > 2) return;

        if (ev.event == mouse_event_motion) {
            if (md != mouse_mode_motion && md != mouse_mode_drag) return;
            if (md == mouse_mode_drag && loc->reported_button == 3) return;
            if (md != mouse_mode_motion && !(ev.mask & ~mask_mod_mask)) return;
            if (ev.x == loc->reported_x && ev.y == loc->reported_y) return;
            ev.button = loc->reported_button + 32;
        } else {
            if (ev.button > 6) ev.button += 128 - 7;
            else if (ev.button > 2) ev.button += 64 - 3;
            if (ev.event == mouse_event_release) {
                if (md == mouse_mode_x10) return;
                /* Don't report wheel release events */
                if (ev.button == 64 || ev.button == 65) return;
                if (loc->mouse_format != mouse_format_sgr) ev.button = 3;
            }
            loc->reported_button = ev.button;
        }

        if (md != mouse_mode_x10) {
            if (ev.mask & mask_shift) ev.button |= 4;
            if (ev.mask & mask_mod_1) ev.button |= 8;
            if (ev.mask & mask_control) ev.button |= 16;
        }

        switch (loc->mouse_format) {
        case mouse_format_sgr:
        case mouse_format_pixel:
            term_answerback(term, CSI"<%"PRIu8";%"PRIu16";%"PRIu16"%c",
                    ev.button, ev.x + 1, ev.y + 1, ev.event == mouse_event_release ? 'm' : 'M');
            break;
        case mouse_format_utf8:;
            size_t off = 0;
            uint8_t buf[UTF8_MAX_LEN * 3 + 3];
            off += utf8_encode(ev.button + ' ', buf + off, buf + sizeof buf);
            off += utf8_encode(ev.x + 1 + ' ', buf + off, buf + sizeof buf);
            utf8_encode(ev.y + 1 + ' ', buf + off, buf + sizeof buf);
            term_answerback(term, CSI"%s%s",
                    term_get_kstate(term)->keyboard_mapping == keymap_sco ? ">M" : "M", buf);
            break;
        case mouse_format_uxvt:
            term_answerback(term, CSI"%"PRIu8";%"PRIu16";%"PRIu16"M", ev.button + ' ', ev.x + 1, ev.y + 1);
            break;
        case mouse_format_default:
            if (ev.x > 222 || ev.y > 222) return;
            term_answerback(term, CSI"%s%c%c%c",
                    term_get_kstate(term)->keyboard_mapping == keymap_sco ? ">M" : "M",
                    ev.button + ' ', ev.x + 1 + ' ', ev.y + 1 + ' ');
        }

        loc->reported_x = ev.x;
        loc->reported_y = ev.y;
    /* Scroll view */
    } else if (ev.event == mouse_event_press && (ev.button == 3 || ev.button == 4)) {
        term_scroll_view(term, (2 *(ev.button == 3) - 1) * window_cfg(sel->win)->scroll_amount);
    /* Paste */
    } else if (ev.button == 1 && ev.event == mouse_event_release) {
        window_paste_clip(term_window(term), clip_primary);
    /* Select */
    } else if (is_selection_event(sel, &ev)) {
#if USE_URI
        if (ev.event == mouse_event_press && ev.button == 0) update_active_uri(scr, sel->win, &ev);
        else window_set_active_uri(term_window(term), EMPTY_URI, 0);
#endif
        int16_t y = ev.y;
        adj_coords(sel->win, &ev.x, &ev.y, 0);
        sel->pointer_x = ev.x;
        sel->pointer_y = ev.y;

        selection_changed(sel, scr, ev.event + 1, ev.mask & mask_mod_1);
        pending_scroll(sel, scr, y, ev.event);

        if (ev.event == mouse_event_release) {
            sel->targ = sel->select_to_clipboard ? clip_clipboard : clip_primary;
            window_set_clip(sel->win, selection_data(sel), CLIP_TIME_NOW, sel->targ);
        }
#if USE_URI
    } else {
        /* Update URI */
        update_active_uri(scr, sel->win, &ev);
#endif
    }
}
