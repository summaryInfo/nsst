#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <inttypes.h>

#include "term.h"
#include "window.h"

typedef struct nss_line {
    struct nss_line *next, *prev;
    size_t width;
    _Bool dirty;
    nss_cell_t cell[];
} nss_line_t;

typedef enum nss_term_state {
    nss_state_focused = 1 << 0,
    nss_state_visible = 1 << 1,
    nss_state_wrap = 1 << 2,
    nss_state_moving_up = 1 << 3,
} nss_term_state_t;

#define UTF8_MAX_LEN 4
#define UTF_INVAL 0xfffd

struct nss_term {
    int16_t cur_x;
    int16_t cur_y;
    uint8_t cur_attrs;
    nss_short_cid_t cur_fg;
    nss_short_cid_t cur_bg;

    int16_t width;
    int16_t height;
    _Bool focused;
    _Bool visible;

    nss_window_t *win;
    nss_context_t *con;
    nss_line_t *screen;
    nss_line_t *cur_line;

    uint32_t utf_part;
    int8_t utf_rest;

    enum nss_term_mode {
		nss_tm_echo = 1 << 0,
		nss_tm_crlf = 1 << 1,
		nss_tm_lock = 1 << 2,
		nss_tm_wrap = 1 << 3,
		nss_tm_dyn_wrap = 1 << 4,
    } mode;

	pid_t child;
	int fd;
};

// Need term_mouse, term_pollfd
// erase_line erase_cell erase_screen
// delete_line delete_cell
// insert_line insert_cell
// copy_line copy_cell copy_screen

static nss_line_t *create_line(nss_line_t *prev, nss_line_t *next, size_t width) {
    nss_line_t *line = malloc(sizeof(nss_line_t) + width*sizeof(line->cell[0]));
    if (!line) die("Can't allocate line");
    memset(line->cell, 0, width*sizeof(line->cell[0]));
    line->width = width;
    line->dirty = 1;

    line->prev = prev;
    if (prev) prev->next = line;
    line->next = next;
    if (next) next->prev = line;
    return line;
}

static nss_line_t *resize_line(nss_line_t *line, size_t width) {
    nss_line_t *new = malloc(sizeof(nss_line_t) + width*sizeof(line->cell[0]));
    if (!line) die("Can't allocate line");
    memcpy(new, line, sizeof(nss_line_t) + MIN(width, line->width)*sizeof(line->cell[0]));
    if (width > line->width)
        memset(new->cell + line->width, 0, (width - line->width)*sizeof(line->cell[0]));
    new->width = width;

	if (line->prev) line->prev->next = new;
	if (line->next) line->next->prev = new;

    return new;
}

static size_t utf8_encode(uint32_t u, uint8_t *buf) {
    static const uint32_t utf8_min[] = {0x80, 0x800, 0x10000, 0x11000};
    static const uint8_t utf8_mask[] = {0x00, 0xc0, 0xe0, 0xf0};
    if(u > 0x10ffff) u = UTF_INVAL;
    size_t i = 0, j;
    while(u > utf8_min[i++]);
    for(j = i; j > 1; j--) {
        buf[j - 1] = (u & 0x3f) | 0x80;
        u >>= 6;
    }
    buf[0] = u | utf8_mask[i - 1];
    return i;
}

static _Bool utf8_decode(nss_term_t *term, uint32_t *res, const uint8_t **buf) {
    if (**buf >= 0x80 && **buf < 0xc0) {
        uint8_t ch = *(*buf)++;
        if(!term->utf_rest) {
            *res = UTF_INVAL;
            return 1;
        }
        term->utf_part <<= 6;
        term->utf_part |= ch & ~0xc0;
        if(!--term->utf_rest) {
            *res = term->utf_part;
            return 1;
        }
        return 0;
    }
    if (term->utf_rest) {
        term->utf_rest = 0;
        *res = UTF_INVAL;
        return 1;
    }
    term->utf_part = *(*buf)++;
    if (term->utf_part < 0x80) {
        *res = term->utf_part;
        return 1;
    }
    if (term->utf_part > 0xf7) {
        *res = UTF_INVAL;
        return 1;
    }
    term->utf_part &= ~0x80;
    uint32_t i = 0x40;
    do {
        term->utf_part &= ~i;
        term->utf_rest++;
    } while((i /= 2) & term->utf_part);
    info("Rest: %"PRIi8, term->utf_rest);

    return 0;
}

static _Bool utf8_check(uint32_t u, size_t len) {
    static const uint32_t utf8_min[] = {0x80, 0x800, 0x10000, 0x11000};
    if (len > 4 || u > 0x10ffff) return 0;
    if (u >= 0xd800 && u < 0xe000) return 0;
    if (len > 1 && u < utf8_min[len - 2]) return 0;
    return 1;
}


void nss_term_write(nss_term_t *term, const uint8_t *buf, size_t len) {
    const uint8_t *end = buf + len, *start = buf;
    uint32_t ch;
    while (start < end) {
        if (utf8_decode(term, &ch, &start)) {

            uint8_t bufs[5];
            bufs[utf8_encode(ch, bufs)] = '\0';
            info("Decoded: 0x%"PRIx32", %s", ch, bufs);
            if(!utf8_check(ch, start - buf)) ch = UTF_INVAL;
            // Write it here
            term->cur_line->cell[term->cur_x] = NSS_MKCELL(term->cur_fg, term->cur_bg, term->cur_attrs, ch);
            nss_window_draw(term->con, term->win, term->cur_x, term->cur_y, &term->cur_line->cell[term->cur_x], 1);
            nss_window_update(term->con, term->win, 1, &(nss_rect_t) {term->cur_x, term->cur_y, 1, 1});
            if(term->cur_x >= term->width) {
                term->cur_y++;
                term->cur_x = 0;
                if (!term->cur_line->next)
                    term->cur_line->next = create_line(term->cur_line, NULL, term->width);
                term->cur_line = term->cur_line->next;
            } else {
                nss_line_t *line = term->cur_line;
                if((size_t)term->cur_x == term->cur_line->width)
                    term->cur_line = resize_line(line, term->width);
                if(line == term->screen)
                    term->screen = term->cur_line;
                term->cur_x++;
            }
            nss_window_draw(term->con, term->win, term->cur_x, term->cur_y, &term->cur_line->cell[term->cur_x], 1);
            nss_window_draw_cursor(term->con, term->win, term->cur_x, term->cur_y);
            nss_window_update(term->con, term->win, 1, &(nss_rect_t) {term->cur_x, term->cur_y, 1, 1});
            nss_window_draw_commit(term->con, term->win);

            buf = start;
        }
    }
}

int nss_term_fd(nss_term_t *term) {
    return 0; //TODO
}

nss_term_t *nss_create_term(nss_context_t *con, nss_window_t *win, int16_t width, int16_t height) {
    nss_term_t *term = malloc(sizeof(nss_term_t));

    term->width = width;
    term->height = height;
    term->focused = 1;
    term->visible = 1;
    term->win = win;
    term->con = con;

    term->utf_part = 0;
    term->utf_rest = 0;

    term->cur_x = 0;
    term->cur_y = 0;
    term->cur_attrs = 0;
    term->cur_fg = 7;
    term->cur_bg = 0;

    nss_attrs_t test[] = {
        nss_attrib_italic | nss_attrib_bold,
        nss_attrib_italic | nss_attrib_underlined,
        nss_attrib_strikethrough,
        nss_attrib_underlined | nss_attrib_inverse,
        0
    };

    nss_line_t *line = NULL;

    for (size_t k = 0; k < 5; k++) {
        line = create_line(line, NULL, '~' - '!');
        if (!line->prev) term->screen = line;
        for (size_t i = '!'; i <= '~'; i++) {
            line->cell[i - '!'] = NSS_MKCELL(7, 0, test[k], i);
        }
    }
    line->cell[13] = NSS_MKCELL(3, 5, test[3], 'A');
    line->prev->cell[16] = NSS_MKCELL(4, 6, test[2], 'A');
    term->cur_line = term->screen;

    return term;
}

#define ALLOC_STEP 16

void nss_term_redraw(nss_term_t *term, nss_rect_t damage) {
    if (!term->visible) return;

    //Clear undefined areas
    nss_window_clear(term->con, term->win, 1, &damage);

    nss_line_t *line = term->screen;
    size_t j = 0;
    for (; line && j < (size_t)damage.y; j++, line = line->next) {}
    for (; line && j < (size_t)damage.height + damage.y; j++, line = line->next) {
        if ((size_t)damage.x < line->width) {
            nss_window_draw(term->con, term->win, damage.x, j, line->cell + damage.x, MIN(line->width - damage.x, damage.width));
            info("Draw: x=%d..%d y=%d", damage.x, damage.x + MIN(line->width - damage.x, damage.width), j);
            if (line == term->cur_line && damage.x <= term->cur_x && term->cur_x < damage.x + damage.width) {
                nss_window_draw_cursor(term->con, term->win, term->cur_x, term->cur_y);
            }
        }
    }
    nss_window_draw_commit(term->con, term->win);
}

void nss_term_resize(nss_term_t *term, int16_t width, int16_t height) {
    term->width = width;
    term->height = height;
}

void nss_term_focus(nss_term_t *term, _Bool focused) {
    int16_t cx = term->cur_x, cy = term->cur_y;
    term->focused = focused;
    nss_window_draw(term->con, term->win, cx, cy, &term->cur_line->cell[cx], 1);
    nss_window_draw_cursor(term->con, term->win, cx, cy);
    nss_window_update(term->con, term->win, 1, &(nss_rect_t) {cx, cy, 1, 1});
    nss_window_draw_commit(term->con, term->win);
}

void nss_term_visibility(nss_term_t *term, _Bool visible) {
    term->visible = visible;
}

void nss_free_term(nss_term_t *term) {
    nss_line_t *line = term->screen;
    while (line->prev)
        line = line->prev;
    while (line) {
        nss_line_t *next = line->next;
        // TODO: Deref all attribs in line here
        free(line);
        line = next;
    }
    free(term);
}
