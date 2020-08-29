/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#include "feature.h"

#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "mouse.h"
#include "term.h"
#include "window.h"
#include "time.h"

#include <stdbool.h>
#include <string.h>

#define SEL_INIT_SIZE 32
#define CSI "\233"

// From term.c
int16_t term_max_y(struct term *term);
int16_t term_max_x(struct term *term);
int16_t term_min_y(struct term *term);
int16_t term_min_x(struct term *term);
int16_t term_width(struct term *term);
int16_t term_height(struct term *term);
ssize_t term_view(struct term *term);

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

    if (loc->targ != -1U) {
        if (term_is_keep_selection_enabled(term)) return;

        window_set_clip(term_window(term), NULL, CLIP_TIME_NOW, loc->targ);
        loc->targ = -1;
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
            SWAP(ssize_t, loc->r.y0, loc->r.y1);
            SWAP(ssize_t, loc->r.x0, loc->r.x1);
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
            SWAP(ssize_t, loc->r.y0, loc->r.y1);
            SWAP(ssize_t, loc->r.x0, loc->r.x1);
        }

        if (loc->n.y0 > loc->n.y1)
            mouse_clear_selection(term);
    }
 }

inline static bool is_separator(uint32_t ch) {
        if (!ch) return 1;
        uint8_t cbuf[UTF8_MAX_LEN + 1];
        cbuf[utf8_encode(ch, cbuf, cbuf + UTF8_MAX_LEN)] = '\0';
        return strstr(sconf(SCONF_WORD_SEPARATORS), (char *)cbuf);
}

static void snap_selection(struct term *term) {
    struct mouse_state *loc = term_get_mstate(term);
    loc->n.x0 = loc->r.x0, loc->n.y0 = loc->r.y0;
    loc->n.x1 = loc->r.x1, loc->n.y1 = loc->r.y1;
    loc->n.rect = loc->r.rect;
    if (loc->n.y1 <= loc->n.y0) {
        if (loc->n.y1 < loc->n.y0) {
            SWAP(ssize_t, loc->n.y0, loc->n.y1);
            SWAP(int16_t, loc->n.x0, loc->n.x1);
        } else if (loc->n.x1 < loc->n.x0) {
            SWAP(int16_t, loc->n.x0, loc->n.x1);
        }
    }
    if (loc->n.rect && loc->n.x1 < loc->n.x0)
            SWAP(int16_t, loc->n.x0, loc->n.x1);

    if (loc->snap != snap_none && loc->state == state_sel_pressed)
        loc->state = state_sel_progress;

    struct line_view line;
    struct line_offset vpos;

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
            bool cat = is_separator(line.cell[loc->n.x0].ch);
            while (1) {
                while (loc->n.x0 > 0) {
                    if (cat != is_separator(line.cell[loc->n.x0 - 1].ch)) goto outer;
                    loc->n.x0--;
                }
                if (cat == is_separator(line.cell[0].ch) && !term_line_next(term, &vpos, -1)) {
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
            bool cat = is_separator(line.cell[loc->n.x1].ch);
            while(1) {
                while (loc->n.x1 < line.width - 1) {
                    if (cat != is_separator(line.cell[loc->n.x1 + 1].ch)) goto outer2;
                    loc->n.x1++;
                }
                if (line.wrapped && !term_line_next(term, &vpos, 1)) {
                    line = term_line_at(term, vpos);
                    if (cat != is_separator(line.cell[0].ch)) {
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


inline static bool sel_adjust_buf(size_t *pos, size_t *cap, uint8_t **res) {
    if (*pos + UTF8_MAX_LEN + 2 >= *cap) {
        size_t new_cap = *cap * 3 / 2;
        uint8_t *tmp = realloc(*res, new_cap);
        if (!tmp) return 0;
        *cap = new_cap;
        *res = tmp;
    }
    return 1;
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
            if (!sel_adjust_buf(pos, cap, res)) return;
            memcpy(*res + *pos, buf, len);
            *pos += len;
        }
    }
    if (!line.wrapped) {
        if (!sel_adjust_buf(pos, cap, res)) return;
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
    struct selected old = loc->n;
    uint8_t oldstate = loc->state;

    if (state == state_sel_pressed) {
        loc->r.x0 = x;
        loc->r.y0 = y - term_view(term);

        struct timespec now;
        clock_gettime(CLOCK_TYPE, &now);

        if (TIMEDIFF(loc->click1, now) < iconf(ICONF_TRIPLE_CLICK_TIME)*1000LL)
            loc->snap = snap_line;
        else if (TIMEDIFF(loc->click0, now) < iconf(ICONF_DOUBLE_CLICK_TIME)*1000LL)
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

inline static void adj_coords(struct term *term, int16_t *x, int16_t *y) {
    struct window *win = term_window(term);
    int16_t cw, ch, w, h, bw, bh;

    window_get_dim_ext(win, dim_cell_size, &cw, &ch);
    window_get_dim_ext(win, dim_border, &bw, &bh);
    window_get_dim_ext(win, dim_grid_size, &w, &h);

    *x = MAX(0, MIN(w - 1, (*x - bw))) / cw;
    *y = MAX(0, MIN(h - 1, (*y - bh))) / ch;
}

void mouse_report_locator(struct term *term, uint8_t evt, int16_t x, int16_t y, uint32_t mask) {

    uint32_t lmask = 0;
    if (mask & mask_button_3) lmask |= 1;
    if (mask & mask_button_2) lmask |= 2;
    if (mask & mask_button_1) lmask |= 4;
    if (mask & mask_button_4) lmask |= 8;

    int16_t w, h, bw, bh;
    window_get_dim_ext(term_window(term), dim_border, &bw, &bh);
    window_get_dim_ext(term_window(term), dim_grid_size, &w, &h);

    if (x < bw || x >= w + bw || y < bh || y > h + bh) {
        if (evt == 1) term_answerback(term, CSI"0&w");
    } else {
        if (!term_get_mstate(term)->locator_pixels) adj_coords(term, &x, &y);
        term_answerback(term, CSI"%d;%d;%d;%d;1&w", evt, lmask, y + 1, x + 1);
    }
}

void mouse_set_filter(struct term *term, iparam_t xs, iparam_t xe, iparam_t ys, iparam_t ye) {
    if (xs > xe) SWAP(uparam_t, xs, xe);
    if (ys > ye) SWAP(uparam_t, ys, ye);

    xe++, ye++;

    struct mouse_state *loc = term_get_mstate(term);

    int16_t cw, ch, bw, bh, w, h;
    window_get_dim_ext(term_window(term), dim_border, &bw, &bh);
    window_get_dim_ext(term_window(term), dim_cell_size, &cw, &ch);
    window_get_dim_ext(term_window(term), dim_grid_size, &w, &h);

    if (!loc->locator_pixels) {
        xs = xs * cw + bw;
        xe = xe * cw + bw;
        ys = ys * ch + bh;
        ye = ye * ch + bh;
    }

    xs = MIN(xs, bw + w - 1);
    xe = MIN(xe, bw + w);
    ys = MIN(ys, bh + h - 1);
    ye = MIN(ye, bh + h);

    loc->filter = (struct rect) { xs, ys, xe - xs, ye - ys };
    loc->locator_filter = 1;

    window_set_mouse(term_window(term), 1);
}

void pending_scroll(struct term *term, int16_t y, enum mouse_event_type event) {
    struct mouse_state *loc = term_get_mstate(term);
    int16_t h, bh;

    window_get_dim_ext(term_window(term), dim_border, NULL, &bh);
    window_get_dim_ext(term_window(term), dim_grid_size, NULL, &h);

    if (event == mouse_event_motion) {
        if (y - bh >= h) loc->pending_scroll = -1;
        else if (y < bh) loc->pending_scroll = 1;
        mouse_pending_scroll(term);
    }
}

bool mouse_pending_scroll(struct term *term) {
    struct mouse_state *loc = term_get_mstate(term);

    if (loc->pending_scroll && loc->state == state_sel_progress) {
        struct timespec now;
        clock_gettime(CLOCK_TYPE, &now);
        bool can_scroll = TIMEDIFF(loc->last_scroll, now) > iconf(ICONF_SELECT_SCROLL_TIME)*1000LL;
        if (can_scroll) {
            term_scroll_view(term, loc->pending_scroll);
            loc->last_scroll = now;
        }
    }
    return loc->pending_scroll;
}

void mouse_handle_input(struct term *term, struct mouse_event ev) {
    struct mouse_state *loc = term_get_mstate(term);
    loc->pending_scroll = 0;
    /* Report mouse */
    if ((loc->locator_enabled | loc->locator_filter) && (ev.mask & mask_mod_mask) != keyboard_force_select_mask() &&
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
            (ev.mask & 0xFF) != keyboard_force_select_mask() && !term_get_kstate(term)->keyboad_vt52) {
        enum mouse_mode md = loc->mouse_mode;

        if (loc->mouse_format != mouse_format_pixel)
            adj_coords(term, &ev.x, &ev.y);

        if (md == mouse_mode_x10 && ev.button > 2) return;

        if (ev.event == mouse_event_motion) {
            if (md != mouse_mode_motion && md != mouse_mode_drag) return;
            if (md == mouse_mode_drag && loc->button == 3) return;
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
            off += utf8_encode(ev.x + ' ', buf + off, buf + sizeof buf);
            utf8_encode(ev.y + ' ', buf + off, buf + sizeof buf);
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
        term_scroll_view(term, (2 *(ev.button == 3) - 1) * iconf(ICONF_SCROLL_AMOUNT));
    /* Select */
    } else if ((ev.event == mouse_event_press && ev.button == 0) ||
               (ev.event == mouse_event_motion && ev.mask & mask_button_1 &&
                    (loc->state == state_sel_progress || loc->state == state_sel_pressed)) ||
               (ev.event == mouse_event_release && ev.button == 0 &&
                    (loc->state == state_sel_progress))) {

        int16_t y = ev.y;
        adj_coords(term, &ev.x, &ev.y);
        change_selection(term, ev.event + 1, ev.x, ev.y, ev.mask & mask_mod_1);
        pending_scroll(term, y, ev.event);

        if (ev.event == mouse_event_release) {
            loc->targ = term_is_select_to_clipboard_enabled(term) ? clip_clipboard : clip_primary;
            window_set_clip(term_window(term), selection_data(term), CLIP_TIME_NOW, loc->targ);
        }
    /* Paste */
    } else if (ev.button == 1 && ev.event == mouse_event_release) {
        window_paste_clip(term_window(term), clip_primary);
    }
}
