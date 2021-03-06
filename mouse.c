/* Copyright (c) 2019-2021, Evgeny Baskov. All rights reserved */

#include "feature.h"

#define _POSIX_C_SOURCE 200809L

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

inline static size_t descomose_selection(struct rect dst[static 3], struct selected seld, struct rect bound, ssize_t pos) {
    size_t count = 0;
    int16_t x0 = seld.x0, x1 = seld.x1 + 1;
    ssize_t y0 = seld.y0 + pos, y1 = seld.y1 + 1 + pos;
    if (seld.rect || y1 - y0 == 1) {
        struct rect r0 = {x0, y0, x1 - x0, y1 - y0};
        if (intersect_with(&r0, &bound))
            dst[count++] = r0;
    } else {
        struct rect r0 = {x0, y0, bound.width - x0, 1};
        struct rect r1 = {0, y0 + 1, bound.width, y1 - y0 - 1};
        struct rect r2 = {0, y1 - 1, x1, 1};
        if (intersect_with(&r0, &bound))
            dst[count++] = r0;
        if (y1 - y0 > 2 && intersect_with(&r1, &bound))
            dst[count++] = r1;
        if (intersect_with(&r2, &bound))
            dst[count++] = r2;
    }
    return count;
}

inline static size_t xor_bands(struct rect dst[static 2], int16_t x00, int16_t x01, int16_t x10, int16_t x11, int16_t y0, int16_t y1) {
    int16_t x0_min = MIN(x00, x10), x0_max = MAX(x00, x10);
    int16_t x1_min = MIN(x01, x11), x1_max = MAX(x01, x11);
    size_t count = 0;
    if (x0_max >= x1_min - 1) {
        dst[count++] = (struct rect) {x0_min, y0, x1_min - x0_min, y1 - y0};
        dst[count++] = (struct rect) {x0_max, y0, x1_max - x0_max, y1 - y0};
    } else {
        if (x0_min != x0_max) dst[count++] = (struct rect) {x0_min, y0, x0_max - x0_min + 1, y1 - y0};
        if (x1_min != x1_max) dst[count++] = (struct rect) {x1_min - 1, y0, x1_max - x1_min + 1, y1 - y0};
    }
    return count;
}

static void update_selection(struct term *term, uint8_t oldstate, struct selected old) {
    // There could be at most 6 difference rectangles

    struct mouse_state *loc = term_get_mstate(term);

    struct rect d_old[4] = {{0}}, d_new[4] = {{0}}, d_diff[16] = {{0}};
    size_t sz_old = 0, sz_new = 0, count = 0;
    struct rect *res = d_diff, bound = {0, 0, term_width(term), term_height(term)};

    if (oldstate != state_sel_none && oldstate != state_sel_pressed)
        sz_old = descomose_selection(d_old, old, bound, term_view(term));
    if (loc->state != state_sel_none && loc->state != state_sel_pressed)
        sz_new = descomose_selection(d_new, loc->n, bound, term_view(term));

    if (!sz_old) res = d_new, count = sz_new;
    else if (!sz_new) res = d_old, count = sz_old;
    else {
        // Insert dummy rectangles to simplify code
        int16_t max_yo = d_old[sz_old - 1].y + d_old[sz_old - 1].height;
        int16_t max_yn = d_new[sz_new - 1].y + d_new[sz_new - 1].height;
        d_old[sz_old] = (struct rect) {0, max_yo, 0, 0};
        d_new[sz_new] = (struct rect) {0, max_yn, 0, 0};

        // Calculate y positions of bands
        int16_t ys[8];
        size_t yp = 0;
        for (size_t i_old = 0, i_new = 0; i_old <= sz_old || i_new <= sz_new; ) {
            if (i_old > sz_old) ys[yp++] = d_new[i_new++].y;
            else if (i_new > sz_new) ys[yp++] = d_old[i_old++].y;
            else {
                ys[yp++] = MIN(d_new[i_new].y, d_old[i_old].y);
                i_old += ys[yp - 1] == d_old[i_old].y;
                i_new += ys[yp - 1] == d_new[i_new].y;
            }
        }

        struct rect *ito = d_old, *itn = d_new;
        int16_t x00 = 0, x01 = 0, x10 = 0, x11 = 0;
        for (size_t i = 0; i < yp - 1; i++) {
            if (ys[i] >= max_yo) x00 = x01 = 0;
            else if (ys[i] == ito->y) x00 = ito->x, x01 = ito->x + ito->width, ito++;
            if (ys[i] >= max_yn) x10 = x11 = 0;
            else if (ys[i] == itn->y) x10 = itn->x, x11 = itn->x + itn->width, itn++;
            count += xor_bands(d_diff + count, x00, x01, x10, x11, ys[i], ys[i + 1]);
        }
    }

    for (size_t i = 0; i < count; i++)
        term_damage(term, res[i]);
}

void mouse_damage_selection(struct term *term) {
    update_selection(term, state_sel_none, (struct selected){0});
}

void mouse_clear_selection(struct term* term) {
    struct mouse_state *loc = term_get_mstate(term);
    struct selected old = loc->n;
    uint8_t oldstate = loc->state;

    loc->state = state_sel_none;

    update_selection(term, oldstate, old);

    if (loc->targ != clip_invalid) {
        if (term_is_keep_selection_enabled(term)) return;

        window_set_clip(term_window(term), NULL, CLIP_TIME_NOW, loc->targ);
        loc->targ = clip_invalid;
    }
}

void mouse_selection_erase(struct term *term, struct rect rect) {
    struct mouse_state *loc = term_get_mstate(term);

#define RECT_INTRS(x10, x11, y10, y11) \
    ((MAX(rect.x, x10) <= MIN(rect.x + rect.width - 1, x11)) && (MAX(rect.y, y10) <= MIN(rect.y + rect.height - 1, y11)))

    if (loc->state != state_sel_none) {
        if (loc->n.rect || loc->n.y0 == loc->n.y1) {
            if (RECT_INTRS(loc->n.x0, loc->n.x1, loc->n.y0, loc->n.y1))
                mouse_clear_selection(term);
        } else {
            if (RECT_INTRS(loc->n.x0, term_width(term) - 1, loc->n.y0, loc->n.y0))
                mouse_clear_selection(term);
            if (loc->n.y1 - loc->n.y0 > 1)
                if (RECT_INTRS(0, term_width(term) - 1, loc->n.y0 + 1, loc->n.y1 - 1))
                    mouse_clear_selection(term);
            if (RECT_INTRS(0, loc->n.x1, loc->n.y1, loc->n.y1))
                mouse_clear_selection(term);
        }
    }
#undef RECT_INTRS
}


void mouse_scroll_selection(struct term *term, ssize_t amount, bool save) {
    struct mouse_state *loc = term_get_mstate(term);

    if (loc->state == state_sel_none) return;

    ssize_t x0, x1, y0 = loc->n.y0, y1 = loc->n.y1;
    if (y1 == y0 || loc->n.rect)
        x1 = loc->n.x1, x0 = loc->n.x0;
    else
        x1 = term_width(term) - 1, x0 = 0;

    ssize_t top = term_min_y(term);

    bool yins = (save || top <= y0) && y1 < term_max_y(term);
    bool youts = (save || top > y1) || y0 >= term_max_y(term);
    bool xins = term_min_x(term) <= x0 && x1 < term_max_x(term);
    bool xouts = term_min_x(term) > x1 || x0 >= term_max_x(term);
    bool damaged = term_min_y(term) && save && term_min_y(term) < y1 && y0 - amount < 0;

    save &= amount >= 0;

    // Clear sellection if it is going to be split by scroll
    if ((!xins && !xouts) || (!yins && !youts) || (xins && yins && damaged)) {
        mouse_clear_selection(term);
    } else if (xins && yins) {
        // Scroll and cut off scroll off lines
        loc->r.y0 -= amount;
        loc->n.y0 -= amount;
        loc->r.y1 -= amount;
        loc->n.y1 -= amount;

        bool swapped = loc->r.y0 > loc->r.y1;
        if (swapped) {
            SWAP(loc->r.y0, loc->r.y1);
            SWAP(loc->r.x0, loc->r.x1);
        }


        if (!save && loc->n.y0 < top) {
            loc->r.y0 = top;
            loc->n.y0 = top;
            if (!loc->n.rect) {
                loc->r.x0 = 0;
                loc->n.x0 = 0;
            }
        }

        ssize_t bottom = term_max_y(term);
        if (loc->n.y1 > bottom) {
            loc->r.y1 = bottom;
            loc->n.y1 = bottom;
            if (!loc->n.rect) {
                loc->r.x1 = term_width(term);
                loc->n.x1 = term_width(term);
            }
        }


        if (swapped) {
            SWAP(loc->r.y0, loc->r.y1);
            SWAP(loc->r.x0, loc->r.x1);
        }

        if (loc->n.y0 > loc->n.y1)
            mouse_clear_selection(term);
    }
 }

inline static bool is_separator(uint32_t ch, char *seps) {
        if (!ch) return 1;
        uint8_t cbuf[UTF8_MAX_LEN + 1];
        cbuf[utf8_encode(ch, cbuf, cbuf + UTF8_MAX_LEN)] = '\0';
        return strstr(seps, (char *)cbuf);
}

static void snap_selection(struct term *term) {
    struct mouse_state *loc = term_get_mstate(term);
    loc->n.x0 = loc->r.x0, loc->n.y0 = loc->r.y0;
    loc->n.x1 = loc->r.x1, loc->n.y1 = loc->r.y1;
    loc->n.rect = loc->r.rect;
    if (loc->n.y1 <= loc->n.y0) {
        if (loc->n.y1 < loc->n.y0) {
            SWAP(loc->n.y0, loc->n.y1);
            SWAP(loc->n.x0, loc->n.x1);
        } else if (loc->n.x1 < loc->n.x0) {
            SWAP(loc->n.x0, loc->n.x1);
        }
    }
    if (loc->n.rect && loc->n.x1 < loc->n.x0)
            SWAP(loc->n.x0, loc->n.x1);

    if (loc->snap != snap_none && loc->state == state_sel_pressed)
        loc->state = state_sel_progress;

    struct line_view line;
    struct line_offset vpos;
    char *seps = window_cfg(term_window(term))->word_separators;

    if (loc->snap == snap_line) {
        loc->n.x0 = 0;
        loc->n.x1 = term_width(term) - 1;
    }

    vpos = term_get_line_pos(term, loc->n.y0);

    if (loc->snap == snap_line) {
        while (!term_line_next(term, &vpos, -1)) {
            line = term_line_at(term, vpos);
            if (!line.wrapped) {
                term_line_next(term, &vpos, 1);
                break;
            }
            loc->n.y0--;
        }
    } else if (loc->snap == snap_word) {
        if ((line = term_line_at(term, vpos)).line) {
            loc->n.x0 = MAX(MIN(loc->n.x0, line.width - 1), 0);
            bool cat = is_separator(line.cell[loc->n.x0].ch, seps);
            while (1) {
                while (loc->n.x0 > 0) {
                    if (cat != is_separator(line.cell[loc->n.x0 - 1].ch, seps)) goto outer;
                    loc->n.x0--;
                }
                if (cat == is_separator(line.cell[0].ch, seps) && !term_line_next(term, &vpos, -1)) {
                    line = term_line_at(term, vpos);
                    if (!line.wrapped) {
                        term_line_next(term, &vpos, 1);
                        break;
                    }
                    loc->n.y0--;
                    loc->n.x0 = line.width - 1;
                } else break;
            }
        }
    }
outer:

    line = term_line_at(term, vpos);
    if (loc->n.x0 > 0 && loc->n.x0 < line.width)
        loc->n.x0 -= !!(line.cell[loc->n.x0 - 1].wide);

    vpos = term_get_line_pos(term, loc->n.y1);
    if (loc->snap == snap_line) {
        while (term_line_at(term, vpos).wrapped) {
            if (term_line_next(term, &vpos, 1)) break;
            loc->n.y1++;
        }
    } else if (loc->snap == snap_word) {
        if ((line = term_line_at(term, vpos)).line) {
            loc->n.x1 = MAX(MIN(loc->n.x1, line.width - 1), 0);
            bool cat = is_separator(line.cell[loc->n.x1].ch, seps);
            while(1) {
                while (loc->n.x1 < line.width - 1) {
                    if (cat != is_separator(line.cell[loc->n.x1 + 1].ch, seps)) goto outer2;
                    loc->n.x1++;
                }
                if (line.wrapped && !term_line_next(term, &vpos, 1)) {
                    line = term_line_at(term, vpos);
                    if (cat != is_separator(line.cell[0].ch, seps)) {
                        term_line_next(term, &vpos, -1);
                        break;
                    }
                    loc->n.x1 = 0;
                    loc->n.y1++;
                } else break;
            }
        }
    }
outer2:

    line = term_line_at(term, vpos);
    if (loc->n.x1 < line.width)
        loc->n.x1 += !!(line.cell[loc->n.x1].wide);
}

bool mouse_is_selected(struct term *term, int16_t x, ssize_t y) {
    struct mouse_state *loc = term_get_mstate(term);

    if (loc->state == state_sel_none || loc->state == state_sel_pressed) return 0;

    if (loc->n.rect) {
        return (loc->n.x0 <= x && x <= loc->n.x1) &&
                (loc->n.y0 <= y && y <= loc->n.y1);
    } else {
        ssize_t w = term_width(term);
        return loc->n.y0*w + loc->n.x0 <= y*w + x &&
                y*w + x <= loc->n.y1*w + loc->n.x1;
    }
}

bool mouse_is_selected_2(struct term *term, int16_t x0, int16_t x1, ssize_t y) {
    struct mouse_state *loc = term_get_mstate(term);

    if (loc->state == state_sel_none || loc->state == state_sel_pressed) return 0;

    if (loc->n.rect) {
        return (loc->n.x0 <= x1 && x0 <= loc->n.x1) &&
                (loc->n.y0 <= y && y <= loc->n.y1);
    } else {
        ssize_t w = term_width(term);
        return loc->n.y0*w + loc->n.x0 <= y*w + x1 &&
                y*w + x0 <= loc->n.y1*w + loc->n.x1;
    }
}

inline static int16_t line_len(struct line_view line) {
    int16_t max_x = line.width;
    if (!line.wrapped)
        while (max_x > 0 && !line.cell[max_x - 1].ch)
            max_x--;
    return max_x;
}

bool mouse_is_selected_in_view(struct term *term, int16_t x, ssize_t y) {
    return mouse_is_selected(term, x, y - term_view(term));
}

static void append_line(size_t *pos, size_t *cap, uint8_t **res, struct line_view line, ssize_t x0, ssize_t x1) {
    if (!line.cell) return;

    ssize_t max_x = MIN(x1, line_len(line));

    for (ssize_t j = x0; j < max_x; j++) {
        uint8_t buf[UTF8_MAX_LEN];
        if (line.cell[j].ch) {
            size_t len = utf8_encode(line.cell[j].ch, buf, buf + UTF8_MAX_LEN);
            // 2 is space for '\n' and '\0'
            if (!adjust_buffer((void **)res, cap, *pos + len + 2, 1)) return;
            memcpy(*res + *pos, buf, len);
            *pos += len;
        }
    }
    if (!line.wrapped) {
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

        ssize_t y = loc->n.y0;
        struct line_offset vpos = term_get_line_pos(term, y);
        if (loc->n.rect || loc->n.y0 == loc->n.y1) {
            while (y++ <= loc->n.y1) {
                append_line(&pos, &cap, &res, term_line_at(term, vpos), loc->n.x0, loc->n.x1 + 1);
                if (term_line_next(term, &vpos, 1)) break;
            }
        } else {
            while (y <= loc->n.y1) {
                struct line_view line = term_line_at(term, vpos);
                if (y == loc->n.y0)
                    append_line(&pos, &cap, &res, line, loc->n.x0, line.width);
                else if (y == loc->n.y1)
                    append_line(&pos, &cap, &res, line, 0, loc->n.x1 + 1);
                else
                    append_line(&pos, &cap, &res, line, 0, line.width);
                if (!term_line_next(term, &vpos, 1)) y++;
                else break;
            }
        }
        res[pos -= !!pos] = '\0';
        return res;
    } else return NULL;
}

static void change_selection(struct term *term, uint8_t state, int16_t x, ssize_t y, bool rectangular) {
    struct mouse_state *loc = term_get_mstate(term);
    struct instance_config *cfg = window_cfg(term_window(term));
    struct selected old = loc->n;
    uint8_t oldstate = loc->state;

    if (state == state_sel_pressed) {
        loc->r.x0 = x;
        loc->r.y0 = y - term_view(term);

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
    loc->r.rect = rectangular;
    loc->r.x1 = x;
    loc->r.y1 = y - term_view(term);

    snap_selection(term);
    update_selection(term, oldstate, old);
}

void mouse_scroll_view(struct term *term, ssize_t delta) {
    struct mouse_state *loc = term_get_mstate(term);
    if (loc->state == state_sel_progress) {
        change_selection(term, state_sel_progress,
                loc->r.x1, loc->r.y1 + term_view(term) - delta, loc->r.rect);
    }
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

        struct line_offset vpos = term_get_line_pos(term, y - term_view(term));
        struct line_view lv = term_line_at(term, vpos);
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
            if (md == mouse_mode_drag && loc->button == 3) return;
            if (md != mouse_mode_motion && !(ev.mask & ~mask_mod_mask)) return;
            if (ev.x == loc->x && ev.y == loc->y) return;
            ev.button = loc->button + 32;
        } else {
            if (ev.button > 6) ev.button += 128 - 7;
            else if (ev.button > 2) ev.button += 64 - 3;
            if (ev.event == mouse_event_release) {
                if (md == mouse_mode_x10) return;
                /* Don't report wheel relese events */
                if (ev.button == 64 || ev.button == 65) return;
                if (loc->mouse_format != mouse_format_sgr) ev.button = 3;
            }
            loc->button = ev.button;
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

        loc->x = ev.x;
        loc->y = ev.y;
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
        change_selection(term, ev.event + 1, ev.x, ev.y, ev.mask & mask_mod_1);
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
