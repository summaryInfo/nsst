/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

#define _DEFAULT_SOURCE

#include "feature.h"

#include "screen.h"
#include "tty.h"

#include <assert.h>
#include <stdio.h>

#define CBUF_STEP(c,m) ((c) ? MIN(4 * (c) / 3, m) : MIN(16, m))
#define PRINT_BLOCK_SIZE 256

void free_screen(struct screen *scr) {
    free_printer(&scr->printer);
    free_selection(&scr->sstate);

#if USE_URI
    uri_unref(scr->sgr.uri);
    uri_unref(scr->saved_sgr.uri);
    uri_unref(scr->back_saved_sgr.uri);
#endif

    screen_free_scrollback(scr, 0);

    for (ssize_t i = 0; i < screen_height(scr); i++) {
        free_line(scr->screen[i]);
        free_line(scr->back_screen[i]);
    }

    free(scr->screen);
    free(scr->back_screen);
    free(scr->temp_screen);
    free(scr->tabs);
    free(scr->predec_buf);
}


/* Damage terminal screen, relative to view
 * Faster version for whole lines */
void screen_damage_lines(struct screen *scr, ssize_t ys, ssize_t yd) {
    struct line_offset vpos = screen_view(scr);
    screen_advance_iter(scr, &vpos, ys);
    for (ssize_t i = ys; i < yd; i++, screen_inc_iter(scr, &vpos))
        screen_line_at(scr, vpos).line->force_damage = 1;
}

void screen_damage_selection(struct screen *scr) {
    struct line_offset vpos = screen_view(scr);
    struct line *prev = NULL;
    for (ssize_t i = 0; i < scr->height; i++) {
        struct line_view v = screen_line_at(scr, vpos);
        if (prev != v.line)
            selection_damage(&scr->sstate, v.line);
        screen_inc_iter(scr, &vpos);
        prev = v.line;
    }
}

void screen_damage_uri(struct screen *scr, uint32_t uri) {
    if (!uri) return;

    struct line_offset vpos = screen_view(scr);
    for (ssize_t i = 0; i < 0 + scr->height; i++) {
        struct line_view line = screen_line_at(scr, vpos);
        if (line.line) {
            for (ssize_t j = 0; j <  MIN(scr->width, line.width); j++) {
                if (line_view_attr(line, line.cell[j].attrid).uri == uri)
                    line.cell[j].drawn = 0;
            }
            screen_inc_iter(scr, &vpos);
        }
    }
}

inline static struct line *line_at(struct screen *scr, ssize_t y) {
    return y >= 0 ? scr->screen[y] : scr->scrollback[(scr->sb_top + scr->sb_caps + y + 1) % scr->sb_caps];
}

struct line *screen_paragraph_at(struct screen *scr, ssize_t y) {
    return y < -scr->sb_limit || y >= scr->height ? NULL : line_at(scr, y);
}

void screen_unwrap_cursor_line(struct screen *scr) {
    if (scr->c.y - 1 >= -scr->sb_limit)
        line_at(scr, scr->c.y - 1)->wrapped = 0;
}

struct line_view screen_line_at(struct screen *scr, struct line_offset pos) {
    if (pos.line >= -scr->sb_limit && pos.line < scr->height) {
        struct line *ln = line_at(scr, pos.line);
        ssize_t wid = line_width(ln, pos.offset, scr->width);
        return (struct line_view) {
            .width = wid - pos.offset,
            .wrapped = ln->wrapped || wid < ln->size,
            .cell = ln->cell + pos.offset,
            .line = ln,
        };
    } else return (struct line_view){ 0 };
}

ssize_t screen_inc_iter(struct screen *scr, struct line_offset *pos) {
    struct line *ln;
    if (scr->mode.rewrap) {
        if (pos->line >= scr->height) return 1;
        ln = line_at(scr, pos->line);
        pos->offset = line_width(ln, pos->offset, scr->width);
        if (pos->offset >= ln->size) {
            if (pos->line + 1 >= scr->height) return 1;
            pos->offset = 0;
            pos->line++;
        }
        return 0;
    } else {
        ssize_t new = MIN(1 + pos->line, scr->height - 1);
        ssize_t amount = new != pos->line;
        *pos = (struct line_offset) { new, 0 };
        return amount;
    }
}

ssize_t screen_advance_iter(struct screen *scr, struct line_offset *pos, ssize_t amount) {
    struct line *ln;
    if (scr->mode.rewrap) {
        // Re-wraping is enabled
        if (amount < 0) {
            if (pos->line - 1 < -scr->sb_limit) return amount;
            ln = line_at(scr, pos->line);
            // TODO Little optimization
            amount += line_segments(ln, 0, scr->width) - line_segments(ln, pos->offset, scr->width);
            pos->offset = 0;
            while (amount < 0) {
                if (pos->line - 1 < -scr->sb_limit) return amount;
                ln = line_at(scr, --pos->line);
                amount += line_segments(ln, 0, scr->width);
            }
        }
        if (amount > 0) {
            if (pos->line >= scr->height) return amount;
            ln = line_at(scr, pos->line);
            while (pos->line < scr->height && amount) {
                if ((pos->offset = line_width(ln, pos->offset, scr->width)) >= ln->size) {
                    pos->offset = 0;
                    if (pos->line + 1 >= scr->height) break;
                    ln = line_at(scr, ++pos->line);
                }
                amount--;
            }
        }
    } else {
        ssize_t new = MAX(-scr->sb_limit, MIN(amount + pos->line, scr->height - 1));
        amount -= new - pos->line;
        *pos = (struct line_offset) { new, 0 };
    }
    return amount;
}

struct line_offset screen_view(struct screen *scr) {
    return scr->view_pos;
}

struct line_offset screen_line_iter(struct screen *scr, ssize_t y) {
    struct line_offset pos = {0, 0};
    screen_advance_iter(scr, &pos, y);
    return pos;
}

void screen_reset_view(struct screen *scr, bool damage) {
    if (scr->view_pos.line) {
        scr->prev_c_view_changed |= !scr->view_pos.line;
        scr->view_pos = (struct line_offset){0};
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

inline static struct line *screen_realloc_line(struct screen *scr, struct line *line, ssize_t width) {
    struct line *new = realloc_line(line, width);

    if (new->selection_index)
        selection_relocated(&scr->sstate, new);
    return new;
}

inline static void screen_adjust_line(struct screen *scr, struct line **pline, int16_t size) {
    struct line *line = *pline;

    assert(line->size <= line->caps);
    if (size <= line->size) return;

    if (size > line->caps)
        line = screen_realloc_line(scr, line, size);

    struct cell c = MKCELL(0, line->pad_attrid);
    for (ssize_t i = line->size; i < size; i++)
        line->cell[i] = c;

    line->size = size;
    *pline = line;
}

void screen_free_scrollback(struct screen *scr, ssize_t max_size) {
    if (scr->scrollback) {
        selection_clear(&scr->sstate);
        screen_reset_view(scr, 0);
    }

    for (ssize_t i = 1; i <= (scr->sb_caps == scr->sb_max_caps ? scr->sb_caps : scr->sb_limit); i++) {
        struct line *line = line_at(scr, -i);
        if (line->selection_index)
            selection_clear(&scr->sstate);
        free_line(line);
    }
    free(scr->scrollback);

    scr->scrollback = NULL;
    scr->sb_max_caps = max_size;
    scr->sb_caps = 0;
    scr->sb_limit = 0;
    scr->sb_top = -1;
}

void screen_scroll_view(struct screen *scr, int16_t amount) {
    bool old_viewr = !scr->view_pos.line;

    ssize_t delta = screen_advance_iter(scr, &scr->view_pos, -amount) + amount;
    if (scr->view_pos.line > 0) {
        delta += scr->view_pos.line;
        scr->view_pos.line = 0;
    }

    if (delta > 0) /* View down, image up */ {
        window_shift(scr->win, 0, delta, scr->height - delta);
        screen_damage_lines(scr, 0, delta);
    } else if (delta < 0) /* View down, image up */ {
        window_shift(scr->win, -delta, 0, scr->height + delta);
        screen_damage_lines(scr, scr->height + delta, scr->height);
    }

   selection_view_scrolled(&scr->sstate, scr);
    scr->prev_c_view_changed |= old_viewr != !scr->view_pos.line;
}

ssize_t screen_append_history(struct screen *scr, struct line *line, bool opt) {
    ssize_t res = 0;
    if (scr->sb_max_caps > 0) {
        struct line *tmp;
        /* If last line in history is wrapped concat current line to it */
        if (scr->mode.rewrap && scr->sb_limit &&
                line_at(scr, -1)->wrapped && (tmp = screen_concat_line(scr, line_at(scr, -1), line, opt))) {

            scr->scrollback[scr->sb_top] = tmp;
        } else {
            // TODO Need to damage screen if selection is reset

            /* Optimize line */
            line = screen_concat_line(scr, line, NULL, opt);

            if (scr->sb_limit == scr->sb_max_caps) {
                /* If view points to the line that is to be freed, scroll it down */
                if (scr->view_pos.line == -scr->sb_max_caps) {
                    if (scr->mode.rewrap)
                        res = line_segments(line_at(scr, -scr->sb_limit), scr->view_pos.offset, scr->width);
                    else
                        res = 1;
                    scr->view_pos.line++;
                    scr->view_pos.offset = 0;
                    scr->prev_c_view_changed |= !scr->view_pos.line;
                }

                /* We reached maximal number of saved lines,
                 * now scr->scrollback functions as true cyclic buffer */
                scr->sb_top = (scr->sb_top + 1) % scr->sb_caps;
                SWAP(line, scr->scrollback[scr->sb_top]);

                if (line->selection_index)
                    selection_clear(&scr->sstate);
                free_line(line);
            } else {
                /* More lines can be saved, scr->scrollback is not cyclic yet */

                /* Adjust capacity as needed */
                if (scr->sb_limit + 1 >= scr->sb_caps) {
                    ssize_t new_cap = CBUF_STEP(scr->sb_caps, scr->sb_max_caps);
                    struct line **new = realloc(scr->scrollback, new_cap * sizeof(*new));
                    if (!new) {
                        if (line->selection_index)
                            selection_clear(&scr->sstate);
                        free_line(line);
                        return res;
                    }
                    scr->sb_caps = new_cap;
                    scr->scrollback = new;
                }

                /* And just save line */
                scr->sb_limit++;
                scr->sb_top = (scr->sb_top + 1) % scr->sb_caps;
                scr->scrollback[scr->sb_top] = line;
            }

            if (scr->view_pos.line)
                scr->view_pos.line--;
        }
    } else {
        if (line->selection_index)
            selection_clear(&scr->sstate);
        free_line(line);
    }
    return res;
}

static void resize_tabs(struct screen *scr, int16_t width) {
    bool *new_tabs = realloc(scr->tabs, width * sizeof(*scr->tabs));
    if (!new_tabs) die("Can't alloc tabs");
    scr->tabs = new_tabs;

    if (width > scr->width) {
        memset(new_tabs + scr->width, 0, (width - scr->width) * sizeof(new_tabs[0]));
        ssize_t tab = scr->width ? scr->width - 1: 0, tabw = window_cfg(scr->win)->tab_width;
        while (tab > 0 && !new_tabs[tab]) tab--;
        while ((tab += tabw) < width) new_tabs[tab] = 1;
    }
}

void screen_resize(struct screen *scr, int16_t width, int16_t height) {
#if USE_URI
    // Reset active URL
    window_set_active_uri(scr->win, EMPTY_URI, 0);
#endif

    // Resize predecode buffer
    uint32_t *newpb = realloc(scr->predec_buf, width*sizeof(*newpb));
    if (!newpb) die("Can't allocate predecode buffer");
    scr->predec_buf = newpb;

    resize_tabs(scr, width);

    // Ensure that scr->back_screen is altscreen
    if (scr->mode.altscreen) {
        SWAP(scr->back_saved_c,scr->saved_c);
        SWAP(scr->back_screen,scr->screen);
        SWAP(scr->back_saved_sgr,scr->saved_sgr);
    }

    bool cur_moved = scr->c.x == scr->width - 1 && scr->c.pending;

    // Resize temporary screen buffer
    struct line **new_tmpsc = realloc(scr->temp_screen, height*sizeof(*new_tmpsc));
    if (!new_tmpsc) die("Can't allocate new temporary screen buffer");
    scr->temp_screen = new_tmpsc;

    struct attr dflt_sgr = ATTR_DEFAULT;

    { // Resize altscreen

        for (ssize_t i = height; i < scr->height; i++) {
            struct line *line = scr->back_screen[i];
            if (line->selection_index)
                selection_clear(&scr->sstate);
            free_line(line);
        }

        struct line **new_back = realloc(scr->back_screen, height * sizeof(scr->back_screen[0]));
        if (!new_back) die("Can't allocate lines");
        scr->back_screen = new_back;

        ssize_t minh = MIN(scr->height, height);
        for (ssize_t i = 0; i < minh; i++)
            scr->back_screen[i] = screen_realloc_line(scr, scr->back_screen[i], width);

        for (ssize_t i = scr->height; i < height; i++)
            scr->back_screen[i] = create_line(dflt_sgr, width);

        // Adjust altscreen saved cursor position

        scr->back_saved_c.x = MIN(MAX(scr->back_saved_c.x, 0), width - 1);
        scr->back_saved_c.y = MIN(MAX(scr->back_saved_c.y, 0), height - 1);
        if (scr->back_saved_c.pending) scr->back_saved_c.x = width - 1;

        if (scr->mode.altscreen) {
            scr->c.x = MIN(MAX(scr->c.x, 0), width - 1);
            scr->c.y = MIN(MAX(scr->c.y, 0), height - 1);
            if (scr->c.pending) scr->c.x = width - 1;
        }
    }


    // Clear mouse selection
    // TODO Keep non-rectangular selection
    selection_clear(&scr->sstate);

    // Find line of bottom left cell
    struct line_offset lower_left = scr->view_pos;
    screen_advance_iter(scr, &lower_left, scr->height - 1);
    bool to_top = scr->view_pos.line <= -scr->sb_limit;
    bool to_bottom = !scr->view_pos.line;
    bool ll_translated = to_top || to_bottom;


    {  // Resize main screen

        struct line **new_lines = scr->screen;
        ssize_t nnlines = scr->height, cursor_par = 0, new_cur_par = 0;

        if (scr->width != width && scr->width && scr->mode.rewrap) {
            ssize_t lline = 0, loff = 0, y= 0;
            nnlines = 0;

            // If first line of screen is continuation line,
            // start with fist line of scrollback
            if (scr->sb_limit && line_at(scr, -1)->wrapped) {
                loff = line_at(scr, -1)->size;
                y = -1;
            }

            bool cset = scr->mode.altscreen, csset = 0, aset = 0;
            ssize_t par_start = y, approx_cy = 0, dlta = 0;

            scr->screen[scr->height - 1]->wrapped = 0;

            for (ssize_t i = 0; i < scr->height; i++) {
                // Calculate new apporoximate cursor y
                if (!aset && i == scr->c.y) {
                    approx_cy = nnlines + (loff + scr->c.x) / width,
                    new_cur_par = nnlines;
                    cursor_par = par_start;
                    aset = 1;
                }
                // Calculate new line number
                if (!scr->screen[i]->wrapped) {
                    ssize_t len = line_length(scr->screen[i]);
                    scr->screen[i]->size = len;
                    if (y && !nnlines) {
                        dlta = (len + loff + width - 1)/width -
                                (len + loff - line_at(scr, -1)->size + width - 1)/width;
                    }
                    nnlines += (len + loff + width - 1)/width;
                    lline++;
                    loff = 0;
                    par_start = i + 1;
                } else loff += scr->width;
            }

            new_cur_par -= dlta;

            // Pop lines from scrollback so cursor row won't be changed
            while (y - 1 > -scr->sb_limit && new_cur_par < cursor_par) {
                struct line *line = line_at(scr, --y);
                ssize_t delta = (line->size + width - 1) / width;
                new_cur_par += delta;
                approx_cy += delta;
                nnlines += delta;
            }

            nnlines = height + 1;
            new_lines = calloc(nnlines, sizeof(*new_lines));
            if (!new_lines) die("Can't allocate line");
            new_lines[0] = create_line(dflt_sgr, width);

            ssize_t y2 = y, dy = 0;
            for (ssize_t dx = 0; y < scr->height; y++) {
                struct line *line = line_at(scr, y);
                ssize_t len = line_length(line);
                if (!ll_translated && lower_left.line == y) {
                    lower_left.line = dy;
                    lower_left.offset = dx;
                    ll_translated = 1;
                }
                if (cursor_par == y) new_cur_par = dy;

                uint32_t previd = ATTRID_DEFAULT, newid = 0;
                for (ssize_t x = 0; x < len; x++) {
                    // If last character of line is wide, soft wrap
                    if (dx == width - 1 && cell_wide(&line->cell[x])) {
                        if (dy == nnlines - 1) {
                            new_lines = realloc(new_lines, (nnlines *= 2) * sizeof *new_lines);
                            assert(new_lines);
                        }
                        new_lines[dy]->wrapped = 1;
                        new_lines[++dy] = create_line(dflt_sgr, width);
                        dx = 0;
                    }
                    // Calculate new cursor...
                    if (!cset && scr->c.y == y && x == scr->c.x) {
                        scr->c.y = dy;
                        scr->c.x = dx;
                        cset = 1;
                    }
                    // ..and saved cursor position
                    if (!csset && scr->saved_c.y == y && x == scr->saved_c.x) {
                        scr->saved_c.y = dy;
                        scr->saved_c.x = dx;
                        csset = 1;
                    }

                    assert(dx < new_lines[dy]->caps);
                    assert(dx == new_lines[dy]->size);

                    // Copy cell
                    struct cell c = line->cell[x];
                    if (c.attrid) {
                        if (c.attrid != previd)
                            newid = alloc_attr(new_lines[dy], line->attrs->data[c.attrid - 1]);
                        c.attrid = newid;
                    };
                    new_lines[dy]->cell[dx++] = c;
                    new_lines[dy]->size++;

                    // Advance line, soft wrap
                    if (dx == width && (x < len - 1 || line->wrapped)) {
                        if (dy == nnlines - 1) {
                            new_lines = realloc(new_lines, (nnlines *= 2) * sizeof *new_lines);
                            assert(new_lines);
                        }
                        new_lines[dy]->wrapped = 1;
                        new_lines[dy]->pad_attrid = alloc_attr(new_lines[dy], attr_pad(line));
                        new_lines[++dy] = create_line(dflt_sgr, width);
                        dx = 0;
                    }
                }
                // If cursor is to the right of line end, need to check separately
                if (!cset && scr->c.y == y && scr->c.x >= len) {
                    scr->c.y = dy;
                    scr->c.x = MIN(width - 1, scr->c.x - len + dx);
                    cset = 1;
                }
                if (!csset && scr->saved_c.y == y && scr->saved_c.x >= len) {
                    scr->saved_c.y = dy;
                    scr->saved_c.x = MIN(width - 1, scr->saved_c.x - len + dx);
                    csset = 1;
                }
                // Advance line, hard wrap
                if (!line->wrapped) {
                    if (dy == nnlines - 1) {
                        new_lines = realloc(new_lines, (nnlines *= 2) * sizeof *new_lines);
                        assert(new_lines);
                    }
                    new_lines[dy]->pad_attrid = alloc_attr(new_lines[dy], attr_pad(line));
                    new_lines[++dy] = create_line(dflt_sgr, width);
                    dx = 0;
                }

                // Pop from scrollback
                if (y < 0) {
                    struct line **pline = &scr->scrollback[(scr->sb_top + scr->sb_caps + y + 1) % scr->sb_caps];
                    assert(line == *pline);
                    *pline = NULL;
                }

                assert(!line->selection_index);
                free_line(line);
            }

            // Update scrollback data
            if (scr->sb_limit) {
                scr->sb_top = (scr->sb_top + y2 + scr->sb_caps) % scr->sb_caps;
                scr->sb_limit += y2;
            }
            if (!ll_translated) lower_left.line -= y2;

            nnlines = dy + 1;
        }

        // Push extra lines from top back to scrollback
        ssize_t start = 0, scrolled = 0;
        while (scr->c.y > height - 1 || new_cur_par > cursor_par) {
            if (scr->sb_limit && line_at(scr, -1)->wrapped) {
                if (lower_left.line >= 0) {
                    if (!lower_left.line) lower_left.offset += line_at(scr, -1)->size;
                    lower_left.line--;
                }
            } else lower_left.line--;

            scrolled = screen_append_history(scr, new_lines[start++], scr->mode.minimize_scrollback);
            new_cur_par--;
            scr->c.y--;
            scr->saved_c.y--;
        }

        ssize_t minh = MIN(nnlines - start, height);

        // Resize lines if re-wraping is disabled
        if (!scr->mode.cut_lines) {
            if (scrolled) {
                window_shift(scr->win, scrolled, 0, scr->height - scrolled);
                screen_damage_lines(scr, scr->height - scrolled, scr->height);
            } else if (start && !scr->view_pos.line) {
                window_shift(scr->win, start, 0, height - start);
            }
            for (ssize_t i = 0; i < minh; i++) {
                if (new_lines[i + start]->caps < width || scr->mode.cut_lines) {
                    new_lines[i + start] = screen_realloc_line(scr, new_lines[i + start], width);
                }
            }
        }

        // Adjust cursor
        scr->saved_c.y = MAX(MIN(scr->saved_c.y, height - 1), 0);
        if (scr->saved_c.pending) scr->saved_c.x = width - 1;
        if (!scr->mode.altscreen) {
            scr->c.y = MAX(MIN(scr->c.y, height - 1), 0);
            if (scr->c.pending) scr->c.x = width - 1;
        }

        // Free extra lines from bottom
        for (ssize_t i = start + height; i < nnlines; i++) {
            assert(!new_lines[i]->selection_index);
            free_line(new_lines[i]);
        }

        // Free old line buffer
        if (new_lines != scr->screen) free(scr->screen);

        // Resize line buffer
        // That 'if' is not strictly necessary,
        // but causes UBSAN warning
        if (new_lines) memmove(new_lines, new_lines + start, minh * sizeof(*new_lines));
        new_lines = realloc(new_lines, height * sizeof(*new_lines));
        if (!new_lines)  die("Can't allocate lines");

        // Allocate new empty lines
        for (ssize_t i = minh; i < height; i++)
            new_lines[i] = create_line(dflt_sgr, width);

        scr->screen = new_lines;
    }

    // Set state
    scr->width = width;
    scr->height = height;
    scr->left = 0;
    scr->top = 0;
    scr->right = width - 1;
    scr->bottom = height - 1;

    {  // Fixup view

        // Reposition view
        if (scr->mode.rewrap && !scr->mode.altscreen) {
            if (to_bottom) {
                // Stick to bottom
                scr->view_pos.offset = 0;
                scr->view_pos.line = 0;
            } else if (to_top) {
                // Stick to top
                scr->view_pos.offset = 0;
                scr->view_pos.line = -scr->sb_limit;
            } else {
                // Keep line of lower left view cell at the bottom
                lower_left.offset -= lower_left.offset % width;
                ssize_t hei = height;
                if (lower_left.line >= scr->height) {
                    hei = MAX(0, hei - (lower_left.line - scr->height));
                    lower_left.line = height - 1;
                }
                screen_advance_iter(scr, &lower_left, 1 - hei);
                scr->view_pos = lower_left;
                if (scr->view_pos.line >= 0 || scr->view_pos.line < -scr->sb_limit)
                    scr->view_pos.offset = 0;
            }
            scr->view_pos.line = MAX(MIN(0, scr->view_pos.line), -scr->sb_limit);
        }
    }

    // Damage screen
    if (!scr->mode.altscreen && scr->mode.rewrap) {
        // Just damage everything if re-wraping is enabled
        screen_damage_lines(scr, 0, scr->height);
    } else if (cur_moved) {
        scr->back_screen[scr->c.y]->cell[scr->c.x].drawn = 0;
        scr->back_screen[scr->c.y]->cell[MAX(scr->c.x - 1, 0)].drawn = 0;
    }

    if (scr->mode.altscreen) {
        SWAP(scr->back_saved_c, scr->saved_c);
        SWAP(scr->back_screen, scr->screen);
        SWAP(scr->back_saved_sgr, scr->saved_sgr);
    }

}

bool screen_redraw(struct screen *scr, bool blink_commited) {
    bool c_hidden = scr->mode.hide_cursor || scr->view_pos.line;

    if (scr->c.x != scr->prev_c_x || scr->c.y != scr->prev_c_y ||
            scr->prev_c_hidden != c_hidden || scr->prev_c_view_changed || !blink_commited) {
        if (!c_hidden) screen_damage_cursor(scr);
        if ((!scr->prev_c_hidden || scr->prev_c_view_changed) &&
                scr->prev_c_y < scr->height && scr->prev_c_x < scr->screen[scr->prev_c_y]->size)
            scr->screen[scr->prev_c_y]->cell[scr->prev_c_x].drawn = 0;
    }

    scr->prev_c_x = scr->c.x;
    scr->prev_c_y = scr->c.y;
    scr->prev_c_hidden = c_hidden;
    scr->prev_c_view_changed = 0;

    if (scr->scroll_damage) {
        screen_damage_lines(scr, 0, scr->height);
        scr->scroll_damage = 0;
    }

    struct line *cl = scr->screen[scr->c.y];
    bool cursor = !c_hidden && (scr->c.x >= cl->size || !cl->cell[scr->c.x].drawn || cl->force_damage);

    if (cursor) {
        // Cursor cannot be drawn if it is beyond the end of the line,
        // so we need to adjust line size.
        screen_adjust_line(scr, &scr->screen[scr->c.y], scr->c.x + 1);
    }

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

    for (int16_t i = *ys; i < *ye; i++)
        screen_adjust_line(scr, &scr->screen[i], *xe);
}

FORCEINLINE
inline static void screen_erase_pre(struct screen *scr, int16_t *xs, int16_t *ys, int16_t *xe, int16_t *ye, bool origin, bool is_erase) {
    if (origin) screen_rect_pre(scr, xs, ys, xe, ye);
    else {
        *xs = MAX(0, MIN(*xs, scr->width - 1));
        *xe = MAX(0, MIN(*xe, scr->width));
        *ys = MAX(0, MIN(*ys, scr->height - 1));
        *ye = MAX(0, MIN(*ye, scr->height));

        if (is_erase) {
            for (int16_t i = *ys; i < *ye; i++) {
                assert(scr->screen[i]->size <= scr->screen[i]->caps);
                if (scr->screen[i]->size <= *xe) {
                    scr->screen[i]->force_damage |= !*xs;
                    scr->screen[i]->size = MIN(*xs, scr->screen[i]->size);
                }
            }
        } else {
            for (int16_t i = *ys; i < *ye; i++)
                screen_adjust_line(scr, &scr->screen[i], *xe);
        }
    }

    if (!scr->view_pos.line) window_delay_redraw(scr->win);

    if (!selection_active(&scr->sstate)) return;
    for (ssize_t y = *ys; y < *ye; y++) {
        if (selection_intersects(&scr->sstate, scr->screen[y], *xs, *xe)) {
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
        struct line *line = scr->screen[ys];
        for (int16_t i = xs; i < xe; i++) {
            uint32_t ch = i >= line->size ? 0 : line->cell[i].ch;
            struct attr attr = attr_at(line, i);
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
            if (first || line->cell[i].ch || !attr_eq(&attr, &(struct attr){ .fg = attr.fg, .bg = attr.bg, .ul = attr.ul }))
                trm += ch + spc, spc = 0;
            else if (!line->cell[i].ch && notrim) spc += ' ';

            res += ch;
            first = notrim;
        }
        if (!notrim) spc = first = 0;
    }

    if (!notrim) res = trm;
    return mode.positive ? res : -res;
}

void screen_reverse_sgr(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye, struct attr *attr) {
    screen_erase_pre(scr, &xs, &ys, &xe, &ye, 1, 0);
    uint32_t mask = attr_mask(attr);

    bool rect = scr->mode.attr_ext_rectangle;
    for (; ys < ye; ys++) {
        struct line *line = scr->screen[ys];
        // TODO Optimize
        for (int16_t i = xs; i < (rect || ys == ye - 1 ? xe : screen_max_ox(scr)); i++) {
            struct attr newa = attr_at(line, i);
            attr_mask_set(&newa, attr_mask(&newa) ^ mask);
            line->cell[i].attrid = alloc_attr(line, newa);
            line->cell[i].drawn = 0;
        }
        if (!rect) xs = screen_min_ox(scr);
    }
}

void screen_apply_sgr(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye, struct attr *mask, struct attr *attr) {
    screen_erase_pre(scr, &xs, &ys, &xe, &ye, 1, 0);
    uint32_t mmsk = attr_mask(mask);
    uint32_t amsk = attr_mask(attr) & mmsk;

    bool rect = scr->mode.attr_ext_rectangle;
    for (; ys < ye; ys++) {
        int16_t xend = rect || ys == ye - 1 ? xe : screen_max_ox(scr);
        screen_adjust_line(scr, &scr->screen[ys], xend);
        struct line *line = scr->screen[ys];
        // TODO Optimize
        for (int16_t i = xs; i < xend; i++) {
            struct attr newa = attr_at(line, i);
            attr_mask_set(&newa, (attr_mask(&newa) & ~mmsk) | amsk);
            if (mask->fg) newa.fg = attr->fg;
            if (mask->bg) newa.bg = attr->bg;
            if (mask->ul) newa.ul = attr->ul;

            line->cell[i].attrid = alloc_attr(line, newa);
            line->cell[i].drawn = 0;
        }
        if (!rect) xs = screen_min_ox(scr);
    }

}

struct attr screen_common_sgr(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye) {
    screen_rect_pre(scr, &xs, &ys, &xe, &ye);

    struct attr common = attr_at(scr->screen[ys], xs);
    bool has_common_fg = 1, has_common_bg = 1, has_common_ul = 1;

    for (; ys < ye; ys++) {
        struct line *line = scr->screen[ys];
        for (int16_t i = xs; i < xe; i++) {
            struct attr attr = attr_at(line, i);
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
            screen_adjust_line(scr, &scr->screen[ys], xe);
            screen_adjust_line(scr, &scr->screen[yd], xd + (xe - xs));
            struct line *sl = scr->screen[ys], *dl = scr->screen[yd];
            dl->wrapped = 0; // Reset line wrapping state
            copy_line(dl, xd, sl, xs, xe - xs);
        }
    } else {
        for (yd += ye - ys; ys < ye; ye--, yd--) {
            screen_adjust_line(scr, &scr->screen[ye - 1], xe);
            screen_adjust_line(scr, &scr->screen[yd - 1], xd + (xe - xs));
            struct line *sl = scr->screen[ye - 1], *dl = scr->screen[yd - 1];
            dl->wrapped = 0; // Reset line wrapping state
            copy_line(dl, xd, sl, xs, xe - xs);
        }
    }
}

void screen_fill(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye, bool origin, uint32_t ch) {
    bool is_erase = !ch && scr->sgr.bg == indirect_color(SPECIAL_BG) &&
                    !scr->sgr.blink && !scr->sgr.reverse && !scr->sgr.protected &&
                    scr->sgr.uri == EMPTY_URI;

    screen_erase_pre(scr, &xs, &ys, &xe, &ye, origin, is_erase);

    for (; ys < ye; ys++) {
        struct line *line = scr->screen[ys];
        // Reset line wrapping state
        line->wrapped = 0;
        struct cell c = {
            .attrid = alloc_attr(line, scr->sgr),
            .ch = compact(ch),
        };
        fill_cells(line->cell + xs, c, xe - xs);
    }
}

void screen_erase(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye, bool origin) {
    screen_fill(scr, xs, ys, xe, ye, origin, 0);
}

void screen_protective_erase(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye, bool origin) {
    screen_erase_pre(scr, &xs, &ys, &xe, &ye, origin, 0);

    for (; ys < ye; ys++) {
        struct line *line = scr->screen[ys];
        // Reset line wrapping state
        struct cell c = { .attrid = alloc_attr(line, scr->sgr) };
        for (int16_t i = xs; i < xe; i++)
            if (!attr_at(line, i).protected) line->cell[i] = c;
    }
}

void screen_selective_erase(struct screen *scr, int16_t xs, int16_t ys, int16_t xe, int16_t ye, bool origin) {
    screen_erase_pre(scr, &xs, &ys, &xe, &ye, origin, 0);

    for (; ys < ye; ys++) {
        struct line *line = scr->screen[ys];
        // Reset line wrapping state
        for (ssize_t i = xs; i < xe; i++)
            if (!attr_at(line, i).protected)
                line->cell[i] = MKCELL(0, line->cell[i].attrid);
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
        scr->c.x = MIN(scr->c.x, scr->width - 1);
        scr->c.y = MIN(scr->c.y, scr->height - 1);
    }
}

void screen_swap_screen(struct screen *scr, bool damage) {
    selection_clear(&scr->sstate);
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
        struct line *line = scr->screen[i];
        adjust_wide_left(line, left);
        adjust_wide_right(line, right - 1);
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

void screen_scroll(struct screen *scr, int16_t top, int16_t amount, bool save) {
    ssize_t left = screen_min_x(scr), right = screen_max_x(scr), bottom = screen_max_y(scr);

    if (left == 0 && right == scr->width) { // Fast scrolling without margins
        if (amount > 0) { /* up */
            amount = MIN(amount, (bottom - top));
            ssize_t rest = (bottom - top) - amount;

            if (save && !scr->mode.altscreen && screen_min_y(scr) == top) {
                ssize_t scrolled = 0;
                for (ssize_t i = 0; i < amount; i++)
                    scrolled -= screen_append_history(scr, scr->screen[top + i], scr->mode.minimize_scrollback);

                memmove(scr->screen + top, scr->screen + top + amount, rest * sizeof(*scr->screen));
                for (ssize_t i = 0; i < amount; i++)
                    scr->screen[bottom - 1 - i] = create_line(scr->sgr, scr->width);

                if (scrolled < 0) /* View down, image up */ {
                    screen_damage_lines(scr, scr->height + scrolled, scr->height);
                    window_shift(scr->win, -scrolled, 0, scr->height + scrolled);
                    selection_view_scrolled(&scr->sstate, scr);
                }
            } else {
                screen_erase(scr, 0, top, scr->width, top + amount, 0);

                memcpy(scr->temp_screen, scr->screen + top, amount*sizeof(*scr->temp_screen));
                memmove(scr->screen + top, scr->screen + top + amount, rest*sizeof(scr->temp_screen));
                memcpy(scr->screen + top + rest, scr->temp_screen, amount*sizeof(*scr->temp_screen));
            }

            scr->scroll_damage = 1;
        } else { /* down */
            amount = MAX(amount, -(bottom - top));
            ssize_t rest = (bottom - top) + amount;

            screen_erase(scr, 0, bottom + amount, scr->width, bottom, 0);

            memcpy(scr->temp_screen, scr->screen + bottom + amount, -amount*sizeof(*scr->temp_screen));
            memmove(scr->screen + top - amount, scr->screen + top, rest*sizeof(scr->temp_screen));
            memcpy(scr->screen + top, scr->temp_screen, -amount*sizeof(*scr->temp_screen));

        }

        if (amount) {
            // Update position of selection, if scrolled
            selection_scrolled(&scr->sstate, scr, amount, top, bottom);

            scr->scroll_damage = 1;

            if (!scr->view_pos.line)
                window_delay_redraw(scr->win);
        }

    } else { // Slow scrolling with margins
        for (ssize_t i = top; i < bottom; i++) {
            struct line *line = scr->screen[i];
            adjust_wide_left(line, left);
            adjust_wide_right(line, right - 1);
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

    if (scr->mode.smooth_scroll && (scr->scrolled += abs(amount)) > window_cfg(scr->win)->smooth_scroll_step) {
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

        screen_adjust_line(scr, &scr->screen[scr->c.y], screen_max_x(scr));
        struct line *line = scr->screen[scr->c.y];

        adjust_wide_left(line, scr->c.x);
        adjust_wide_right(line, scr->c.x);

        memmove(line->cell + scr->c.x + n, line->cell + scr->c.x,
                (screen_max_x(scr) - scr->c.x - n) * sizeof(struct cell));
        for (int16_t i = scr->c.x + n; i < screen_max_x(scr); i++)
            line->cell[i].drawn = 0;

        screen_erase(scr, scr->c.x, scr->c.y, scr->c.x + n, scr->c.y + 1, 0);
        if (selection_intersects(&scr->sstate, line, screen_max_x(scr) - n, screen_max_x(scr))) {
            screen_damage_selection(scr);
            selection_clear(&scr->sstate);
        }
    }

    screen_reset_pending(scr);
}

void screen_delete_cells(struct screen *scr, int16_t n) {
    // Do not check top/bottom margins, DCH should work outside them
    if (scr->c.x >= screen_min_x(scr) && scr->c.x < screen_max_x(scr)) {
        struct line *line = scr->screen[scr->c.y];

        // TODO Shrink line
        // We can optimize this code by avoiding allocation and movement of empty cells
        // and just shrink the line.
        screen_adjust_line(scr, &scr->screen[scr->c.y], screen_max_x(scr));

        n = MIN(n, screen_max_x(scr) - scr->c.x);
        if (n <= 0) return;

        adjust_wide_left(line, scr->c.x);
        adjust_wide_right(line, scr->c.x + n - 1);

        memmove(line->cell + scr->c.x, line->cell + scr->c.x + n,
                (screen_max_x(scr) - scr->c.x - n) * sizeof(struct cell));
        for (int16_t i = scr->c.x; i < screen_max_x(scr) - n; i++)
            line->cell[i].drawn = 0;

        screen_erase(scr, screen_max_x(scr) - n, scr->c.y, screen_max_x(scr), scr->c.y + 1, 0);
        if (selection_intersects(&scr->sstate, line, scr->c.x, scr->c.x + n)) {
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

void screen_index(struct screen *scr) {
    if (scr->c.y == screen_max_y(scr) - 1 && screen_cursor_in_region(scr)) {
        screen_scroll(scr, screen_min_y(scr), 1, 1);

        screen_reset_pending(scr);
    } else if (scr->c.y != screen_max_y(scr) - 1)
        screen_move_to(scr, scr->c.x, scr->c.y + 1);
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
    scr->mode.rewrap = cfg->rewrap;
    scr->mode.cut_lines = cfg->cut_lines;
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

void screen_print_line(struct screen *scr, ssize_t y) {
    if (!printer_is_available(&scr->printer)) return;

    uint8_t buf[PRINT_BLOCK_SIZE];
    uint8_t *pbuf = buf, *pend = buf + PRINT_BLOCK_SIZE;

    struct attr prev = {0};
    struct line *line = line_at(scr, y);

    for (int16_t i = 0; i < line_length(line); i++) {
        struct cell c = line->cell[i];
        struct attr attr = attr_at(line, i);

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

void screen_print_screen(struct screen *scr, bool force_ext) {
    if (!printer_is_available(&scr->printer)) return;

    force_ext |= scr->mode.print_extend;

    int16_t top = force_ext ? 0 : screen_min_y(scr);
    int16_t bottom = (force_ext ? scr->height : screen_max_y(scr)) - 1;

    while (top < bottom)
        screen_print_line(scr, top++);

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
    return MIN(end, start + (ffsll(b) - sizeof(uint64_t) + 1)/sizeof(uint64_t));
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

    ssize_t max_cx = scr->c.x + totalw, cx = scr->c.x;
    ssize_t max_tx = screen_max_x(scr);
    struct line *line = scr->screen[scr->c.y];
    struct cell *cell = &line->cell[cx];

    if (max_cx < line->size)
        adjust_wide_right(line, max_cx - 1);

    // Shift characters to the left if insert mode is enabled
    if (scr->mode.insert && max_cx < max_tx && cx < line->size) {
        ssize_t max_new_size = MIN(max_tx, line->size + totalw);
        if (line->size < max_new_size) {
            screen_adjust_line(scr, &line, max_new_size);
            scr->screen[scr->c.y] = line;
        }

        for (struct cell *c = cell + totalw; c - line->cell < max_tx; c++) c->drawn = 0;
        memmove(cell + totalw, cell, (max_tx - max_cx)*sizeof(*cell));
        max_cx = MAX(max_cx, max_tx);
    } else {
        if (line->size < max_cx) {
            screen_adjust_line(scr, &line, max_cx);
            scr->screen[scr->c.y] = line;
        }
    }

    // Clear selection if writing over it
    if (selection_intersects(&scr->sstate, line, cx, scr->mode.insert ? max_tx : max_cx)) {
        screen_damage_selection(scr);
        selection_clear(&scr->sstate);
    }

    if (scr->mode.margin_bell) {
        ssize_t bcol = screen_max_x(scr) - window_cfg(scr->win)->margin_bell_column;
        if (cx < bcol && max_cx >= bcol)
            window_bell(scr->win, scr->mbvol);
    }

    // Erase overwritten parts of wide characters
    adjust_wide_left(line, cx);

    cx += totalw;

    scr->c.pending = cx == max_tx;
    scr->c.x = cx - scr->c.pending;

    // Writing to the line resets its wrapping state
    line->wrapped = 0;

    // Allocate color for cell
    uint32_t attrid = alloc_attr(line, *screen_sgr(scr));

    // Put charaters
    for (uint32_t *p = bstart; p < bend; p++)
        *cell++ = MKCELL(*p, attrid);

    if (gconfig.trace_characters) {
        for (uint32_t *p = bstart; p < bend; p++) {
            info("Char: (%x) '%lc' ", *p, *p);
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
