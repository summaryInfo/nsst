/* Copyright (c) 2019-2021, Evgeny Baskov. All rights reserved */

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


inline static struct segments *seg_head(struct mouse_state *loc, struct line *line) {
    /* First pointer is always 0,
     * so we don't have to perform
     * addition checks */
    return loc->seg[line->selection_index];
}

inline static void free_segments(struct mouse_state *loc, struct segments *head) {
    head->line->selection_index = SELECTION_EMPTY;

    free(head);

    /* Here we need to offset all selection below current by one
     * to keep selected lines heads continous.
     * FIXME Make heads be organized in double linked list */

    struct segments **phead = loc->seg + head->line->selection_index;
    struct segments **end = loc->seg + loc->seg_size;
    while (++phead < end) {
        *(phead - 1)  = *phead;
        (*phead)->line->selection_index--;
    }

    loc->seg_size--;
}

#define SEGS_INIT_SIZE 2

inline static struct segments *alloc_head(struct mouse_state *loc, struct line *line) {
    if (!adjust_buffer((void **)&loc->seg, &loc->seg_caps,
                       loc->seg_size + 2, sizeof *loc->seg)) return NULL;
    struct segments *head = malloc(sizeof *head + sizeof *head->segs * SEGS_INIT_SIZE);

    line->selection_index = loc->seg_size;
    loc->seg[loc->seg_size++] = head;
    head->line = line;
    head->size = 0;
    head->caps = SEGS_INIT_SIZE;
    head->new_line_flag = 1;

    return head;
}

inline static bool adjust_head(struct mouse_state *loc, struct segments **phead, ssize_t inc) {
    struct segments *head = *phead;
    if (head->size + inc > head->caps) {
        ssize_t new_caps = MAX(4 * head->caps / 3, head->size + inc);
        ssize_t idx = head->line->selection_index;
        head = realloc(head, sizeof *head + new_caps * sizeof *head->segs);
        if (!head) return 0;

        *phead = loc->seg[idx] = head;
        head->caps = new_caps;
    }
    return 1;
}

static void append_segment(struct mouse_state *loc, struct line *line, int16_t x0, int16_t x1) {
    struct segments *head = seg_head(loc, line);

    // Clip if selecting over the line end
    // (clipped lines always have one space at the end)
    // FIXME Let it work without one character at the end
    if (x0 >= line->width) x0 = line->width - 1;
    if (x1 > line->width) x1 = line->width;

    if (!head && !(head = alloc_head(loc, line))) return;


    foreach_segment_indexed(seg, last_i, head);

    if (last_i == x0 && head->size) {
        head->segs[head->size - 1].length += x1 - x0;
    } else if (last_i <= x0) {
        if (!adjust_head(loc, &head, 1)) return;
        head->segs[head->size++] = (struct segment) {x0 - last_i, x1 - x0 };
    } else assert(0);
}

void mouse_concat_selections(struct term *term, struct line *dst, struct line *src) {
    struct segments *src_head, *dst_head;
    struct mouse_state *loc = term_get_mstate(term);

    if (!(src_head = seg_head(loc, src))) return;
    if (!(dst_head = seg_head(loc, dst))) {
        dst->selection_index = src->selection_index;
        src->selection_index = 0;
        src_head->line = dst;
        return;
    }

    assert(dst->selection_index + 1 == src->selection_index);

    size_t offset = 0;
    foreach_segment_indexed(seg, last_i, dst_head);

    /* Merge adjacent */
    if (src_head->size && !src_head->segs->offset && last_i == dst->width) {
        dst_head->segs[dst_head->size - 1].length += src_head->segs[0].length;
        src_head->size--;
        offset = 1;
    }

    /* Append tail */
    if (src_head->size && adjust_head(loc, &dst_head, src_head->size)) {
        memcpy(dst_head->segs + dst_head->size,
               src_head->segs + offset,
               src_head->size * sizeof *src_head->segs);

        // NOTE dst width computation should be compatible with
        //      one in line_concat()
        // Also, this does not change if first segment was merged
        dst_head->segs[dst_head->size].offset += dst->width - last_i;
        dst_head->size += src_head->size;
    }

    free_segments(loc, src_head);
    src->selection_index = SELECTION_EMPTY;
}

void mouse_realloc_selections(struct term *term, struct line *line, bool cut) {
    struct mouse_state *loc = term_get_mstate(term);
    struct segments *head = seg_head(loc, line);
    if (!head) return;

    head->line = line;

    foreach_segment_indexed(seg, idx, head) {
        if (idx + seg->length > line->width) {
            if (idx <= line->width)
                seg++->length = line->width - idx;
            head->size = seg - head->segs;
            if (cut)
                mouse_clear_selection(term, 0);
            break;
        }
    }
}

void mouse_clear_selection(struct term* term, bool damage) {
    struct mouse_state *loc = term_get_mstate(term);

    if (loc->state == state_sel_none ||
        loc->state == state_sel_pressed) return;

    loc->state = state_sel_none;

    if (damage) {
        struct line_offset vpos = term_get_view(term);
        struct line *prev = NULL;
        for (ssize_t i = 0; i < term_height(term); i++) {
            struct line_view v = term_line_at(term, vpos);
            if (prev != v.line)
                mouse_damage_selected(term, v.line);
            term_line_next(term, &vpos, 1);
            prev = v.line;
        }
    }

    for (size_t i = 1; i < loc->seg_size; i++) {
        loc->seg[i]->line->selection_index = SELECTION_EMPTY;
        free(loc->seg[i]);
    }

    loc->seg_size = 1;

    if (loc->targ != clip_invalid) {
        if (term_is_keep_selection_enabled(term)) return;

        window_set_clip(term_window(term), NULL, CLIP_TIME_NOW, loc->targ);
        loc->targ = clip_invalid;
    }
}

void mouse_line_changed(struct term *term, struct line *line, int16_t x0, int16_t x1, bool damage) {
    struct mouse_state *loc = term_get_mstate(term);
    struct segments *head = seg_head(loc, line);
    if (!head) return;

    foreach_segment_indexed(seg, idx, head) {
        if (idx < x1 - 1 && idx + seg->length - 1 > x0) {
            mouse_clear_selection(term, damage);
            return;
        }
    }
}

static void damage_head(struct segments *head) {
    struct cell *cell = head->line->cell;
    foreach_segment_indexed(seg, idx, head)
        for (ssize_t i = idx; i < seg->length + idx; i++)
            cell[i].drawn = 0;
}

void mouse_damage_selected(struct term *term, struct line *line) {
    struct segments *head = seg_head(term_get_mstate(term), line);
    if (head) damage_head(head);
}

inline static bool is_separator(uint32_t ch, char *seps) {
    if (!ch) return 1;
    uint8_t cbuf[UTF8_MAX_LEN + 1];
    cbuf[utf8_encode(ch, cbuf, cbuf + UTF8_MAX_LEN)] = '\0';
    return strstr(seps, (char *)cbuf);
}

static struct line_offset snap_backward(struct term *term, struct line_offset pos) {
    char *seps = window_cfg(term_window(term))->word_separators;
    struct mouse_state *loc = term_get_mstate(term);

    if (loc->snap == snap_line) {
        pos.offset = 0;
        struct line *prev_line;
        for (;;) {
            prev_line = term_raw_line_at(term, pos.line - 1);
            if (!prev_line || !prev_line->wrapped) break;
            pos.line--;
        }
    } else if (loc->snap == snap_word) {
        struct line *line = term_raw_line_at(term, pos.line), *prev;
        if (pos.offset > line->width) pos.offset = line->width - 1;
        for (;;) {
            /* We should not land on second cell of wide character */
            pos.offset -= !line->cell[pos.offset].ch && pos.offset && cell_wide(line->cell + pos.offset - 1);

            bool sep_cur = is_separator(cell_get(line->cell + pos.offset), seps);

            /* Go backwards until we hit word border */

            for (; pos.offset; ) {
                int delta = 1 + (pos.offset > 1 && cell_wide(line->cell + pos.offset - 2));
                if (sep_cur != is_separator(cell_get(line->cell + pos.offset - delta), seps)) return pos;
                pos.offset -= delta;
            }

            /* Go to previous line only if:
             *     - it exitsts
             *     - it is wrapped
             *     - it ends with character of same class */

            if (!(prev = term_raw_line_at(term, pos.line - 1)) || !prev->wrapped) break;

            line = prev;

            int delta = 1 + (line->width > 1 && cell_wide(line->cell + line->width - 2));
            if (is_separator(cell_get(line->cell + line->width - delta), seps) != sep_cur) break;

            pos.line--;
            pos.offset = line->width - delta;
        }
    }

    return pos;
}

static struct line_offset snap_forward(struct term *term, struct line_offset pos) {
    char *seps = window_cfg(term_window(term))->word_separators;
    struct mouse_state *loc = term_get_mstate(term);

    if (loc->snap == snap_line) {
        struct line *next = term_raw_line_at(term, pos.line), *line;
        do line = next, next = term_raw_line_at(term, ++pos.line);
        while (next && line->wrapped);
        pos.line--;
        pos.offset = line->width - 1;
    } else if (loc->snap == snap_word) {
        struct line *line = term_raw_line_at(term, pos.line), *next;
        if (line->width < pos.offset) pos.offset = line->width - 1;
        for (;;) {
            bool sep_cur = is_separator(cell_get(line->cell + pos.offset), seps);

            /* We should not land on first cell of wide character */
            pos.offset += cell_wide(line->cell + pos.offset);

            /* Go forward until we hit word border */

            for (; pos.offset < line->width; ) {
                int delta = 1 + (cell_wide(line->cell + pos.offset) && pos.offset < line->width - 1);
                if (sep_cur != is_separator(cell_get(line->cell + pos.offset + delta), seps)) return pos;
                pos.offset += delta;
            }

            /* Go to previous line only if:
             *     - it exitsts
             *     - it is wrapped
             *     - it ends with character of same class */

            if (!line->wrapped || !(next = term_raw_line_at(term, pos.line + 1))) break;

            line = next;

            if (is_separator(cell_get(line->cell), seps) != sep_cur) break;

            pos.line++;
            pos.offset = 0;
        }
    }

    return pos;
}

inline static int16_t virtual_pos(struct term *term, struct line_offset *pos) {
    struct line_offset orig = *pos, next = *pos;
    next.offset = 0;

    do {
        *pos = next;
        term_line_next(term, &next, 1);
    } while (line_offset_cmp(orig, next) >= 0);

    return orig.offset - pos->offset;
}

static void damage_changed(struct mouse_state *loc, struct segments **old, size_t old_size) {
    for (size_t i = 1; i < old_size; i++) {
        struct line *line = old[i]->line;
        struct segments *new_head = seg_head(loc, line);
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

                assert(to <= line->width);
                while (from < to)
                    line->cell[from++].drawn = 0;

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

    for (size_t i = 1; i < loc->seg_size; i++)
        if (loc->seg[i]->new_line_flag)
            damage_head(loc->seg[i]);
}

static void decompose(struct term *term, struct line_offset start, struct line_offset end) {
    struct mouse_state *loc = term_get_mstate(term);

    // Rebuild

    if (loc->rectangular) {
        struct line_offset vstart = start, vend = end;
        int16_t vstart_x = virtual_pos(term, &vstart);
        int16_t vend_x = virtual_pos(term, &vend);
        if (vstart_x > vend_x)
            SWAP(vstart_x, vend_x);

        do {
            struct line *line = term_raw_line_at(term, vstart.line);
            append_segment(loc, line, vstart.offset + vstart_x, vstart.offset + vend_x + 1);
            term_line_next(term, &vstart, 1);
        } while (line_offset_cmp(vstart, vend) <= 0);

    } else {
        for (; start.line < end.line; start.line++) {
            struct line *line = term_raw_line_at(term, start.line);
            append_segment(loc, line, start.offset, line->width);
            start.offset = 0;
        }
        struct line *line = term_raw_line_at(term, end.line);
        append_segment(loc, line, start.offset, end.offset + 1);
    }
}

inline static struct line_offset absolute_pos(struct term *term, ssize_t x, ssize_t y) {
    struct line_offset offset = term_get_view(term);
    term_line_next(term, &offset, y);
    offset.offset += x;
    return offset;
}

bool init_mouse(struct term *term) {
    struct mouse_state *loc = term_get_mstate(term);

    loc->seg_caps = 4;
    loc->seg_size = 1;
    loc->seg = calloc(sizeof *loc->seg, loc->seg_caps);
    return loc->seg;
}

void free_mouse(struct term *term) {
    struct mouse_state *loc = term_get_mstate(term);
    for (size_t i = 1; i < loc->seg_size; i++) {
        loc->seg[i]->line->selection_index = SELECTION_EMPTY;
        free(loc->seg[i]);
    }

    free(loc->seg);
    loc->seg_caps = 0;
    loc->seg_size = 0;
    loc->seg = NULL;
}

static void selection_changed(struct term *term, uint8_t state, bool rectangular) {
    struct mouse_state *loc = term_get_mstate(term);
    struct instance_config *cfg = window_cfg(term_window(term));

    struct line_offset pos = absolute_pos(term, loc->pointer_x, loc->pointer_y);

    if (state == state_sel_pressed) {
        loc->start = pos;

        struct timespec now;
        clock_gettime(CLOCK_TYPE, &now);

        if (TIMEDIFF(loc->click1, now) < cfg->triple_click_time*1000LL)
            loc->snap = snap_line;
        else if (TIMEDIFF(loc->click0, now) < cfg->double_click_time*1000LL)
            loc->snap = snap_word;
        else
            loc->snap = snap_none;

        loc->click1 = loc->click0;
        loc->click0 = now;
    }

    loc->state = state;
    loc->rectangular = rectangular;
    loc->end = pos;

    struct line_offset nstart = loc->start;
    struct line_offset nend = loc->end;

    if (line_offset_cmp(nstart, nend) > 0)
        SWAP(nstart, nend);

    nstart = snap_backward(term, nstart);
    nend = snap_forward(term, nend);

    if (loc->snap != snap_none && loc->state == state_sel_pressed)
        loc->state = state_sel_progress;

    struct segments **prev_heads = loc->seg;
    size_t prev_size = loc->seg_size;
    init_mouse(term);

    for (size_t i = 1; i < prev_size; i++)
        prev_heads[i]->line->selection_index = 0;

    if (loc->state == state_sel_progress || loc->state == state_sel_released)
        decompose(term, nstart, nend);

    // Make changed cells dirty
    damage_changed(loc, prev_heads, prev_size);
}

bool mouse_is_selected(struct term *term, struct line_view *view, int16_t x) {
    struct mouse_state *loc = term_get_mstate(term);

    struct segments *head = seg_head(loc, view->line);
    if (!head) return 0;

    // FIXME This should be optimized in renderer

    x += view->cell - view->line->cell;

    foreach_segment_indexed(seg, idx, head) {
        if (idx > x) return 0;
        if (idx + seg->length > x) return 1;
    }

    if (idx >= view->line->width) return 1;
    return 0;
}

inline static int16_t line_len(struct line *line) {
    int16_t max_x = line->mwidth;
    assert(line->mwidth <= line->width);
    if (!line->wrapped)
        while (max_x > 0 && !line->cell[max_x - 1].ch)
            max_x--;
    return max_x;
}

static void append_line(size_t *pos, size_t *cap, uint8_t **res, struct line *line, ssize_t x0, ssize_t x1, bool first) {
    ssize_t max_x = MIN(x1, line_len(line));

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

    if (!line->wrapped || x1 != line->width) {
        if (!adjust_buffer((void **)res, cap, *pos + 2, 1)) return;
        (*res)[(*pos)++] = '\n';
    }
}

static uint8_t *selection_data(struct term *term) {
    struct mouse_state *loc = term_get_mstate(term);
    if (loc->state == state_sel_released) {
        uint8_t *res = malloc(SEL_INIT_SIZE * sizeof(*res));
        if (!res) return NULL;
        size_t pos = 0, cap = SEL_INIT_SIZE;

        for (size_t i = 1; i < loc->seg_size; i++) {
            struct segments *head = loc->seg[i];
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

void mouse_view_scrolled(struct term *term) {
    struct mouse_state *loc = term_get_mstate(term);
    if (loc->state == state_sel_progress)
        selection_changed(term, state_sel_progress, loc->rectangular);
}

inline static void adj_coords(struct term *term, int16_t *x, int16_t *y, bool pixel) {
    struct window *win = term_window(term);
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
        adj_coords(term, &x, &y, term_get_mstate(term)->locator_pixels);
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

static void pending_scroll(struct term *term, int16_t y, enum mouse_event_type event) {
    struct mouse_state *loc = term_get_mstate(term);

    struct window *win = term_window(term);
    struct extent c = window_get_cell_size(win);
    struct extent b = window_get_border(win);
    struct extent g = window_get_grid_size(win);

    if (event == mouse_event_motion) {
        if (y - b.height >= g.height) loc->pending_scroll = MIN(-1, (g.height + b.height - y - c.height + 1) / c.height / 2);
        else if (y < b.height) loc->pending_scroll = MAX(1, (b.height - y + c.height - 1) / c.height / 2);
        mouse_pending_scroll(term);
    }
}

bool mouse_pending_scroll(struct term *term) {
    struct mouse_state *loc = term_get_mstate(term);
    struct instance_config *cfg = window_cfg(term_window(term));

    if (loc->pending_scroll && loc->state == state_sel_progress) {
        struct timespec now;
        clock_gettime(CLOCK_TYPE, &now);
        bool can_scroll = TIMEDIFF(loc->last_scroll, now) > cfg->select_scroll_time*1000LL;
        if (can_scroll) {
            term_scroll_view(term, loc->pending_scroll);
            loc->last_scroll = now;
        }
    }
    return loc->pending_scroll;
}

#if USE_URI
inline static bool is_button1_down(struct mouse_event *ev) {
    return (ev->event == mouse_event_press && ev->button == 0) ||
           (ev->mask & mask_button_1 && (ev->event != mouse_event_release || ev->button != 0));
}

static void update_active_uri(struct term *term, struct mouse_event *ev) {
    struct window *win = term_window(term);

    if (!window_cfg(win)->allow_uris) return;

    struct extent c = window_get_cell_size(win);
    struct extent b = window_get_border(win);
    struct extent g = window_get_grid_size(win);

    uint32_t uri = EMPTY_URI;
    if ((ev->x >= b.width && ev->x < g.width + b.width) && (ev->y >= b.height || ev->y < g.height + b.height)) {
        int16_t x = (ev->x - b.width) / c.width;
        int16_t y = (ev->y - b.height) / c.height;

        struct line_offset pos = term_get_view(term);
        term_line_next(term, &pos, y);

        struct line_view lv = term_line_at(term, pos);
        uint32_t lx = x + lv.cell - lv.line->cell;
        if (lx < lv.line->width) uri = attr_at(lv.line, lx).uri;
    }
    window_set_active_uri(win, uri, is_button1_down(ev));

    uint32_t uri_mask = window_cfg(win)->uri_click_mask;
    if (uri && ev->event == mouse_event_release && ev->button == 0 &&
        (ev->mask & mask_mod_mask) == uri_mask) uri_open(uri);
}
#endif

void mouse_handle_input(struct term *term, struct mouse_event ev) {
    struct mouse_state *loc = term_get_mstate(term);

    loc->pending_scroll = 0;

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

        adj_coords(term, &ev.x, &ev.y, loc->mouse_format == mouse_format_pixel);

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
                /* Don't report wheel relese events */
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
        term_scroll_view(term, (2 *(ev.button == 3) - 1) * window_cfg(term_window(term))->scroll_amount);
    /* Paste */
    } else if (ev.button == 1 && ev.event == mouse_event_release) {
        window_paste_clip(term_window(term), clip_primary);
    /* Select */
    } else if ((ev.event == mouse_event_press && ev.button == 0) ||
               (ev.event == mouse_event_motion && ev.mask & mask_button_1 &&
                    (loc->state == state_sel_progress || loc->state == state_sel_pressed)) ||
               (ev.event == mouse_event_release && ev.button == 0 &&
                    (loc->state == state_sel_progress))) {
#if USE_URI
        if (ev.event == mouse_event_press && ev.button == 0) update_active_uri(term, &ev);
        else window_set_active_uri(term_window(term), EMPTY_URI, 0);
#endif
        int16_t y = ev.y;
        adj_coords(term, &ev.x, &ev.y, 0);
        loc->pointer_x = ev.x;
        loc->pointer_y = ev.y;

        selection_changed(term, ev.event + 1, ev.mask & mask_mod_1);
        pending_scroll(term, y, ev.event);

        if (ev.event == mouse_event_release) {
            loc->targ = term_is_select_to_clipboard_enabled(term) ? clip_clipboard : clip_primary;
            window_set_clip(term_window(term), selection_data(term), CLIP_TIME_NOW, loc->targ);
        }
#if USE_URI
    } else {
        /* Update URI */
        update_active_uri(term, &ev);
#endif
    }
}
