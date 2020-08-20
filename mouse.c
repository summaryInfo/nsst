/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#include "feature.h"

#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "mouse.h"
#include "term.h"
#include "window.h"
#include "time.h"

#include <string.h>

#define SEL_INIT_SIZE 32
#define CSI "\233"

// From term.c
nss_coord_t term_max_y(nss_term_t *term);
nss_coord_t term_max_x(nss_term_t *term);
nss_coord_t term_min_y(nss_term_t *term);
nss_coord_t term_min_x(nss_term_t *term);
nss_coord_t term_width(nss_term_t *term);
nss_coord_t term_height(nss_term_t *term);
ssize_t term_scrollback_size(nss_term_t *term);
ssize_t term_view_pos(nss_term_t *term);
nss_locator_state_t *term_locstate(nss_term_t *term);

inline static size_t descomose_selection(nss_rect_t dst[static 3], nss_selected_t seld, nss_rect_t bound, ssize_t pos) {
    size_t count = 0;
    nss_coord_t x0 = seld.x0, x1 = seld.x1 + 1;
    ssize_t y0 = seld.y0 + pos, y1 = seld.y1 + 1 + pos;
    if (seld.rect || y1 - y0 == 1) {
        nss_rect_t r0 = {x0, y0, x1 - x0, y1 - y0};
        if (intersect_with(&r0, &bound))
            dst[count++] = r0;
    } else {
        nss_rect_t r0 = {x0, y0, bound.width - x0, 1};
        nss_rect_t r1 = {0, y0 + 1, bound.width, y1 - y0 - 1};
        nss_rect_t r2 = {0, y1 - 1, x1, 1};
        if (intersect_with(&r0, &bound))
            dst[count++] = r0;
        if (y1 - y0 > 2 && intersect_with(&r1, &bound))
            dst[count++] = r1;
        if (intersect_with(&r2, &bound))
            dst[count++] = r2;
    }
    return count;
}

inline static size_t xor_bands(nss_rect_t dst[static 2], nss_coord_t x00, nss_coord_t x01, nss_coord_t x10, nss_coord_t x11, nss_coord_t y0, nss_coord_t y1) {
    nss_coord_t x0_min = MIN(x00, x10), x0_max = MAX(x00, x10);
    nss_coord_t x1_min = MIN(x01, x11), x1_max = MAX(x01, x11);
    size_t count = 0;
    if (x0_max >= x1_min - 1) {
        dst[count++] = (nss_rect_t) {x0_min, y0, x1_min - x0_min, y1 - y0};
        dst[count++] = (nss_rect_t) {x0_max, y0, x1_max - x0_max, y1 - y0};
    } else {
        if (x0_min != x0_max) dst[count++] = (nss_rect_t) {x0_min, y0, x0_max - x0_min + 1, y1 - y0};
        if (x1_min != x1_max) dst[count++] = (nss_rect_t) {x1_min - 1, y0, x1_max - x1_min + 1, y1 - y0};
    }
    return count;
}

static void update_selection(nss_term_t *term, uint8_t oldstate, nss_selected_t old) {
    // There could be at most 6 difference rectangles

    nss_locator_state_t *loc = term_locstate(term);

    nss_rect_t d_old[4] = {{0}}, d_new[4] = {{0}}, d_diff[16] = {{0}};
    size_t sz_old = 0, sz_new = 0, count = 0;
    nss_rect_t *res = d_diff, bound = {0, 0, term_width(term), term_height(term)};

    if (oldstate != nss_sstate_none && oldstate != nss_sstate_pressed)
        sz_old = descomose_selection(d_old, old, bound, term_view_pos(term));
    if (loc->state != nss_sstate_none && loc->state != nss_sstate_pressed)
        sz_new = descomose_selection(d_new, loc->n, bound, term_view_pos(term));

    if (!sz_old) res = d_new, count = sz_new;
    else if (!sz_new) res = d_old, count = sz_old;
    else {
        // Insert dummy rectangles to simplify code
        nss_coord_t max_yo = d_old[sz_old - 1].y + d_old[sz_old - 1].height;
        nss_coord_t max_yn = d_new[sz_new - 1].y + d_new[sz_new - 1].height;
        d_old[sz_old] = (nss_rect_t) {0, max_yo, 0, 0};
        d_new[sz_new] = (nss_rect_t) {0, max_yn, 0, 0};

        // Calculate y positions of bands
        nss_coord_t ys[8];
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

        nss_rect_t *ito = d_old, *itn = d_new;
        nss_coord_t x00 = 0, x01 = 0, x10 = 0, x11 = 0;
        for (size_t i = 0; i < yp - 1; i++) {
            if (ys[i] >= max_yo) x00 = x01 = 0;
            else if (ys[i] == ito->y) x00 = ito->x, x01 = ito->x + ito->width, ito++;
            if (ys[i] >= max_yn) x10 = x11 = 0;
            else if (ys[i] == itn->y) x10 = itn->x, x11 = itn->x + itn->width, itn++;
            count += xor_bands(d_diff + count, x00, x01, x10, x11, ys[i], ys[i + 1]);
        }
    }

    for (size_t i = 0; i < count; i++)
        nss_term_damage(term, res[i]);
}

void nss_mouse_damage_selection(nss_term_t *term) {
    update_selection(term, nss_sstate_none, (nss_selected_t){0});
}

void nss_mouse_clear_selection(nss_term_t* term) {
    nss_locator_state_t *loc = term_locstate(term);
    nss_selected_t old = loc->n;
    uint8_t oldstate = loc->state;

    loc->state = nss_sstate_none;

    update_selection(term, oldstate, old);

    if (loc->targ != -1U) {
        if (nss_term_keep_selection(term)) return;

        nss_window_set_clip(nss_term_window(term), NULL, NSS_TIME_NOW, loc->targ);
        loc->targ = -1;
    }
}

void nss_mouse_selection_erase(nss_term_t *term, nss_rect_t rect) {
    nss_locator_state_t *loc = term_locstate(term);

#define RECT_INTRS(x10, x11, y10, y11) \
    ((MAX(rect.x, x10) <= MIN(rect.x + rect.width - 1, x11)) && (MAX(rect.y, y10) <= MIN(rect.y + rect.height - 1, y11)))

    if (loc->state != nss_sstate_none) {
        if (loc->r.rect || loc->n.y0 == loc->n.y1) {
            if (RECT_INTRS(loc->n.x0, loc->n.x1, loc->n.y0, loc->n.y1))
                nss_mouse_clear_selection(term);
        } else {
            if (RECT_INTRS(loc->n.x0, term_width(term) - 1, loc->n.y0, loc->n.y0))
                nss_mouse_clear_selection(term);
            if (loc->n.y1 - loc->n.y0 > 1)
                if (RECT_INTRS(0, term_width(term) - 1, loc->n.y0 + 1, loc->n.y1 - 1))
                    nss_mouse_clear_selection(term);
            if (RECT_INTRS(0, loc->n.x1, loc->n.y1, loc->n.y1))
                nss_mouse_clear_selection(term);
        }
    }
#undef RECT_INTRS
}


void nss_mouse_scroll_selection(nss_term_t *term, nss_coord_t amount, _Bool save) {
    nss_locator_state_t *loc = term_locstate(term);

    if (loc->state == nss_sstate_none) return;

    ssize_t x0, x1, y0 = loc->n.y0, y1 = loc->n.y1;
    if (y1 == y0 || loc->n.rect)
        x1 = loc->n.x1, x0 = loc->n.x0;
    else
        x1 = term_width(term) - 1, x0 = 0;
    ssize_t top = save ? -term_scrollback_size(term) : term_min_y(term);

    save &= amount >= 0;
    _Bool yins = top <= y0 && y1 < term_max_y(term);
    _Bool youts = top > y1 || y0 >= term_max_y(term);
    _Bool xins = term_min_x(term) <= x0 && x1 < term_max_x(term);
    _Bool xouts = term_min_x(term) > x1 || x0 >= term_max_x(term);
    _Bool damaged = term_min_y(term) && save && term_min_y(term) < y1 && y0 - amount < 0;

    // Clear sellection if it is going to be split by scroll
    if ((!xins && !xouts) || (!yins && !youts) || (xins && yins && damaged)) {
        nss_mouse_clear_selection(term);
    } else if (xins && yins) {
        // Scroll and cut off scroll off lines
        loc->r.y0 -= amount;
        loc->n.y0 -= amount;
        loc->r.y1 -= amount;
        loc->n.y1 -= amount;

        _Bool swapped = loc->r.y0 > loc->r.y1;
        if (swapped) {
            SWAP(ssize_t, loc->r.y0, loc->r.y1);
            SWAP(ssize_t, loc->r.x0, loc->r.x1);
        }

        if (loc->n.y0 < top) {
            loc->r.y0 = top;
            loc->n.y0 = top;
            if (!loc->r.rect) {
                loc->r.x0 = 0;
                loc->n.x0 = 0;
            }
        }

        ssize_t bottom = term_max_y(term);
        if (loc->n.y1 > bottom) {
            loc->r.y1 = bottom;
            loc->n.y1 = bottom;
            if (!loc->r.rect) {
                loc->r.x1 = nss_term_line_at(term, bottom)->width - 1;
                loc->n.x1 = nss_term_line_at(term, bottom)->width - 1;
            }
        }


        if (swapped) {
            SWAP(ssize_t, loc->r.y0, loc->r.y1);
            SWAP(ssize_t, loc->r.x0, loc->r.x1);
        }

        if (loc->n.y0 > loc->n.y1)
            nss_mouse_clear_selection(term);
    }
 }

inline static _Bool is_separator(nss_char_t ch) {
        if (!ch) return 1;
        uint8_t cbuf[UTF8_MAX_LEN + 1];
        cbuf[utf8_encode(ch, cbuf, cbuf + UTF8_MAX_LEN)] = '\0';
        return strstr(nss_config_string(NSS_SCONFIG_WORD_SEPARATORS), (char *)cbuf);
}

static void snap_selection(nss_term_t *term) {
    nss_locator_state_t *loc = term_locstate(term);
    loc->n.x0 = loc->r.x0, loc->n.y0 = loc->r.y0;
    loc->n.x1 = loc->r.x1, loc->n.y1 = loc->r.y1;
    loc->r.rect = loc->n.rect;
    if (loc->n.y1 <= loc->n.y0) {
        if (loc->n.y1 < loc->n.y0) {
            SWAP(nss_coord_t, loc->n.y0, loc->n.y1);
            SWAP(nss_coord_t, loc->n.x0, loc->n.x1);
        } else if (loc->n.x1 < loc->n.x0) {
            SWAP(nss_coord_t, loc->n.x0, loc->n.x1);
        }
    }
    if (loc->n.rect && loc->n.x1 < loc->n.x0)
            SWAP(nss_coord_t, loc->n.x0, loc->n.x1);

    if (loc->snap != nss_ssnap_none && loc->state == nss_sstate_pressed)
        loc->state = nss_sstate_progress;

    if (loc->snap == nss_ssnap_line) {
        loc->n.x0 = 0;
        loc->n.x1 = term_width(term) - 1;

        nss_line_t *line;

        do line = nss_term_line_at(term, --loc->n.y0);
        while (line && line->wrap_at);
        loc->n.y0++;

        do line = nss_term_line_at(term, loc->n.y1++);
        while (line && line->wrap_at);
        loc->n.y1--;
    } else if (loc->snap == nss_ssnap_word) {
        nss_line_t *line;

        if ((line = nss_term_line_at(term, loc->n.y0))) {
            loc->n.x0 = MAX(MIN(loc->n.x0, line->width - 1), 0);
            _Bool first = 1, cat = is_separator(line->cell[loc->n.x0].ch);
            do {
                if (!first) loc->n.x0 = line->wrap_at - 1;
                first = 0;
                while (loc->n.x0 > 0 &&
                        cat == is_separator(line->cell[loc->n.x0 - 1].ch)) loc->n.x0--;
                if (loc->n.x0 > 0 || cat != is_separator(line->cell[0].ch)) {
                    loc->n.y0--;
                    break;
                }
            } while ((line = nss_term_line_at(term, --loc->n.y0)) && line->wrap_at);
            loc->n.y0++;
        }

        if ((line = nss_term_line_at(term, loc->n.y1))) {
            loc->n.x1 = MAX(MIN(loc->n.x1, line->width - 1), 0);
            _Bool first = 1, cat = is_separator(line->cell[loc->n.x1].ch);
            ssize_t line_len;
            do {
                line_len = line->wrap_at ? line->wrap_at : line->width;
                if (!first) {
                    if (cat != is_separator(line->cell[0].ch)) break;
                    loc->n.x1 = 0;
                } else first = 0;
                while (loc->n.x1 < line_len - 1 &&
                        cat == is_separator(line->cell[loc->n.x1 + 1].ch)) loc->n.x1++;
                if (loc->n.x1 < line_len - 1 || cat != is_separator(line->cell[line_len - 1].ch)) break;
            } while (line->wrap_at && (line = nss_term_line_at(term, ++loc->n.y1)));
            if (!line) loc->n.y1--;
        }
    }

    nss_line_t *ly1 = nss_term_line_at(term, loc->n.y1);
    if (loc->n.x1 < ly1->width)
        loc->n.x1 += !!(ly1->cell[loc->n.x1].attr & nss_attrib_wide);

    nss_line_t *ly0 = nss_term_line_at(term, loc->n.y0);
    if (loc->n.x0 > 0 && loc->n.x0 < ly0->width)
        loc->n.x0 -= !!(ly0->cell[loc->n.x0 - 1].attr & nss_attrib_wide);
}

_Bool nss_mouse_is_selected(nss_term_t *term, nss_coord_t x, nss_coord_t y) {
    nss_locator_state_t *loc = term_locstate(term);

    if (loc->state == nss_sstate_none || loc->state == nss_sstate_pressed) return 0;

    y -= term_view_pos(term);

    if (loc->n.rect) {
        return (loc->n.x0 <= x && x <= loc->n.x1) &&
                (loc->n.y0 <= y && y <= loc->n.y1);
    } else {
        return (loc->n.y0 <= y && y <= loc->n.y1) &&
                !(loc->n.y0 == y && x < loc->n.x0) &&
                !(loc->n.y1 == y && x > loc->n.x1);
    }
}

inline static _Bool sel_adjust_buf(size_t *pos, size_t *cap, uint8_t **res) {
    if (*pos + UTF8_MAX_LEN + 2 >= *cap) {
        size_t new_cap = *cap * 3 / 2;
        uint8_t *tmp = realloc(*res, new_cap);
        if (!tmp) return 0;
        *cap = new_cap;
        *res = tmp;
    }
    return 1;
}

static void append_line(size_t *pos, size_t *cap, uint8_t **res, nss_line_t *line, nss_coord_t x0, nss_coord_t x1) {
    nss_coord_t max_x = MIN(x1, line_length(line));

    for (nss_coord_t j = x0; j < max_x; j++) {
        uint8_t buf[UTF8_MAX_LEN];
        if (line->cell[j].ch) {
            size_t len = utf8_encode(line->cell[j].ch, buf, buf + UTF8_MAX_LEN);
            // 2 is space for '\n' and '\0'
            if (!sel_adjust_buf(pos, cap, res)) return;
            memcpy(*res + *pos, buf, len);
            *pos += len;
        }
    }
    if (max_x != line->wrap_at) {
        if (!sel_adjust_buf(pos, cap, res)) return;
        (*res)[(*pos)++] = '\n';
    }
}

static uint8_t *selection_data(nss_term_t *term) {
    nss_locator_state_t *loc = term_locstate(term);
    if (loc->state == nss_sstate_released) {
        uint8_t *res = malloc(SEL_INIT_SIZE * sizeof(*res));
        if (!res) return NULL;
        size_t pos = 0, cap = SEL_INIT_SIZE;

        ssize_t y = loc->n.y0;
        if (loc->n.rect || loc->n.y0 == loc->n.y1) {
            while (y++ <= loc->n.y1)
                append_line(&pos, &cap, &res, nss_term_line_at(term, y - 1), loc->n.x0, loc->n.x1 + 1);
        } else {
            while (y++ <= loc->n.y1) {
                nss_line_t *line = nss_term_line_at(term, y - 1);
                if (y - 1 == loc->n.y0)
                    append_line(&pos, &cap, &res, line, loc->n.x0, line->width);
                else if (y - 1 == loc->n.y1)
                    append_line(&pos, &cap, &res, line, 0, loc->n.x1 + 1);
                else
                    append_line(&pos, &cap, &res, line, 0, line->width);
            }
        }
        res[pos -= !!pos] = '\0';
        return res;
    } else return NULL;
}

static void change_selection(nss_term_t *term, uint8_t state, nss_coord_t x, nss_color_t y, _Bool rectangular) {
    nss_locator_state_t *loc = term_locstate(term);
    nss_selected_t old = loc->n;
    uint8_t oldstate = loc->state;

    if (state == nss_sstate_pressed) {
        loc->r.x0 = x;
        loc->r.y0 = y - term_view_pos(term);

        struct timespec now;
        clock_gettime(NSS_CLOCK, &now);

        if (TIMEDIFF(loc->click1, now) < nss_config_integer(NSS_ICONFIG_TRIPLE_CLICK_TIME)*(SEC/1000))
            loc->snap = nss_ssnap_line;
        else if (TIMEDIFF(loc->click0, now) < nss_config_integer(NSS_ICONFIG_DOUBLE_CLICK_TIME)*(SEC/1000))
            loc->snap = nss_ssnap_word;
        else
            loc->snap = nss_ssnap_none;

        loc->click1 = loc->click0;
        loc->click0 = now;
    }

    loc->state = state;
    loc->r.rect = rectangular;
    loc->r.x1 = x;
    loc->r.y1 = y - term_view_pos(term);

    snap_selection(term);
    update_selection(term, oldstate, old);
}

void nss_mouse_scroll_view(nss_term_t *term, ssize_t delta) {
    nss_locator_state_t *loc = term_locstate(term);
    if (loc->state == nss_sstate_progress) {
        change_selection(term, nss_sstate_progress,
                loc->r.x1, loc->r.y1 + term_view_pos(term) - delta, loc->r.rect);
    }
}

inline static void adj_coords(nss_window_t *win, int16_t *x, int16_t *y) {
    int16_t cw, ch, w, h, bw, bh;

    nss_window_get_dim_ext(win, nss_dt_cell_size, &cw, &ch);
    nss_window_get_dim_ext(win, nss_dt_border, &bw, &bh);
    nss_window_get_dim_ext(win, nss_dt_grid_size, &w, &h);

    *x = MAX(0, MIN(w - 1, (*x - bw))) / cw;
    *y = MAX(0, MIN(h - 1, (*y - bh))) / ch;
}

void nss_mouse_report_locator(nss_term_t *term, uint8_t evt, int16_t x, int16_t y, uint32_t mask) {

    uint32_t lmask = 0;
    if (mask & nss_ms_button_3) lmask |= 1;
    if (mask & nss_ms_button_2) lmask |= 2;
    if (mask & nss_ms_button_1) lmask |= 4;
    if (mask & nss_ms_button_4) lmask |= 8;

    int16_t w, h, bw, bh;
    nss_window_get_dim_ext(nss_term_window(term), nss_dt_border, &bw, &bh);
    nss_window_get_dim_ext(nss_term_window(term), nss_dt_grid_size, &w, &h);

    if (x < bw || x >= w + bw || y < bh || y > h + bh) {
        if (evt == 1) nss_term_answerback(term, CSI"0&w");
    } else {
        if (!term_locstate(term)->locator_pixels)
            adj_coords(nss_term_window(term), &x, &y);
        nss_term_answerback(term, CSI"%d;%d;%d;%d;1&w", evt, mask, y + 1, x + 1);
    }
}

void nss_mouse_set_filter(nss_term_t *term, nss_sparam_t xs, nss_sparam_t xe, nss_sparam_t ys, nss_sparam_t ye) {
    if (xs > xe) SWAP(nss_param_t, xs, xe);
    if (ys > ye) SWAP(nss_param_t, ys, ye);

    xe++, ye++;

    nss_locator_state_t *loc = term_locstate(term);

    int16_t cw, ch, bw, bh, w, h;
    nss_window_get_dim_ext(nss_term_window(term), nss_dt_border, &bw, &bh);
    nss_window_get_dim_ext(nss_term_window(term), nss_dt_cell_size, &cw, &ch);
    nss_window_get_dim_ext(nss_term_window(term), nss_dt_grid_size, &w, &h);

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

    loc->filter = (nss_rect_t) { xs, ys, xe - xs, ye - ys };
    loc->locator_filter = 1;

    nss_window_set_mouse(nss_term_window(term), 1);
}

void nss_handle_mouse(nss_term_t *term, nss_mouse_event_t ev) {
    nss_locator_state_t *loc = term_locstate(term);
    /* Report mouse */
    if ((loc->locator_enabled | loc->locator_filter) && (ev.mask & 0xFF) != nss_input_force_mouse_mask() &&
            !nss_term_inmode(term)->keyboad_vt52) {
        if (loc->locator_filter) {
            if (ev.x < loc->filter.x || ev.x >= loc->filter.x + loc->filter.width ||
                    ev.y < loc->filter.y || ev.y >= loc->filter.y + loc->filter.height) {
                if (ev.event == nss_me_press) ev.mask |= 1 << (ev.button + 8);
                nss_mouse_report_locator(term, 10, ev.x, ev.y, ev.mask);
                loc->locator_filter = 0;
                nss_window_set_mouse(nss_term_window(term), loc->mouse_mode == nss_mouse_mode_motion);
            }
        } else if (loc->locator_enabled) {
            if (loc->locator_oneshot) {
                loc->locator_enabled = 0;
                loc->locator_oneshot = 0;
            }

            if (ev.event == nss_me_motion) return;
            else if (ev.event == nss_me_press && !loc->locator_report_press) return;
            else if (ev.event == nss_me_release && !loc->locator_report_release) return;

            if (ev.button < 3) {
                if (ev.event == nss_me_press) ev.mask |= 1 << (ev.button + 8);
                nss_mouse_report_locator(term, 2 + ev.button * 2 + (ev.event == nss_me_release), ev.x, ev.y, ev.mask);
            }
        }
    } else if (loc->mouse_mode != nss_mouse_mode_none &&
            (ev.mask & 0xFF) != nss_input_force_mouse_mask() && !nss_term_inmode(term)->keyboad_vt52) {
        enum nss_mouse_mode md = loc->mouse_mode;

        adj_coords(nss_term_window(term), &ev.x, &ev.y);

        if (md == nss_mouse_mode_x10 && ev.button > 2) return;

        if (ev.event == nss_me_motion) {
            if (md != nss_mouse_mode_motion && md != nss_mouse_mode_drag) return;
            if (md == nss_mouse_mode_drag && loc->button == 3) return;
            if (ev.x == loc->x && ev.y == loc->y) return;
            ev.button = loc->button + 32;
        } else {
            if (ev.button > 6) ev.button += 128 - 7;
            else if (ev.button > 2) ev.button += 64 - 3;
            if (ev.event == nss_me_release) {
                if (md == nss_mouse_mode_x10) return;
                /* Don't report wheel relese events */
                if (ev.button == 64 || ev.button == 65) return;
                if (loc->mouse_format != nss_mouse_format_sgr) ev.button = 3;
            }
            loc->button = ev.button;
        }

        if (md != nss_mouse_mode_x10) {
            if (ev.mask & nss_ms_shift) ev.button |= 4;
            if (ev.mask & nss_ms_mod_1) ev.button |= 8;
            if (ev.mask & nss_ms_control) ev.button |= 16;
        }

        switch (loc->mouse_format) {
        case nss_mouse_format_sgr:
            nss_term_answerback(term, CSI"<%"PRIu8";%"PRIu16";%"PRIu16"%c",
                    ev.button, ev.x + 1, ev.y + 1, ev.event == nss_me_release ? 'm' : 'M');
            break;
        case nss_mouse_format_utf8:;
            size_t off = 0;
            uint8_t buf[UTF8_MAX_LEN * 3 + 3];
            off += utf8_encode(ev.button + ' ', buf + off, buf + sizeof buf);
            off += utf8_encode(ev.x + ' ', buf + off, buf + sizeof buf);
            off += utf8_encode(ev.y + ' ', buf + off, buf + sizeof buf);
            nss_term_answerback(term, CSI"%s%s",
                    nss_term_inmode(term)->keyboard_mapping == nss_km_sco ? ">M" : "M", buf);
            break;
        case nss_mouse_format_uxvt:
            nss_term_answerback(term, CSI"%"PRIu8";%"PRIu16";%"PRIu16"M", ev.button + ' ', ev.x + 1, ev.y + 1);
            break;
        case nss_mouse_format_default:
            if (ev.x > 222 || ev.y > 222) return;
            nss_term_answerback(term, CSI"%s%c%c%c",
                    nss_term_inmode(term)->keyboard_mapping == nss_km_sco ? ">M" : "M",
                    ev.button + ' ', ev.x + 1 + ' ', ev.y + 1 + ' ');
        }

        loc->x = ev.x;
        loc->y = ev.y;
    /* Scroll view */
    } else if (ev.event == nss_me_press && (ev.button == 3 || ev.button == 4)) {
        nss_term_scroll_view(term, (2 *(ev.button == 3) - 1) * nss_config_integer(NSS_ICONFIG_SCROLL_AMOUNT));
    /* Select */
    } else if ((ev.event == nss_me_press && ev.button == 0) ||
               (ev.event == nss_me_motion && ev.mask & nss_ms_button_1 &&
                    (loc->state == nss_sstate_progress || loc->state == nss_sstate_pressed)) ||
               (ev.event == nss_me_release && ev.button == 0 &&
                    (loc->state == nss_sstate_progress))) {

        adj_coords(nss_term_window(term), &ev.x, &ev.y);
        change_selection(term, ev.event + 1, ev.x, ev.y, ev.mask & nss_mm_mod1);

        if (ev.event == nss_me_release) {
            loc->targ = nss_term_select_to_clipboard(term) ? nss_ct_clipboard : nss_ct_primary;
            nss_window_set_clip(nss_term_window(term), selection_data(term), NSS_TIME_NOW, loc->targ);
        }
    /* Paste */
    } else if (ev.button == 1 && ev.event == nss_me_release) {
        nss_window_paste_clip(nss_term_window(term), nss_ct_primary);
    }
}
