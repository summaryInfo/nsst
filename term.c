
#include <stdlib.h>
#include <string.h>
#include <assert.h>

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

struct nss_term {
    int16_t cursor_x;
    int16_t cursor_y;
    int16_t width;
    int16_t height;
    _Bool focused;
    _Bool visible;

    nss_window_t *win;
    nss_context_t *con;
    nss_line_t *screen;
    nss_line_t *current_line;

    //nss_rect_t clip;
};

#define NSS_TERM_MIN_ATTRIBS 8

nss_line_t *create_line(nss_line_t *prev, nss_line_t *next, size_t width) {
    nss_line_t *line = malloc(sizeof(nss_line_t) + width*sizeof(line->cell[0]));
    memset(line->cell, 0, width*sizeof(line->cell[0]));
    line->width = width;
    line->dirty = 1;

    line->prev = prev;
    if (prev) prev->next = line;
    line->next = next;
    if (next) next->prev = line;
    return line; 
}

// Need utf8_decode, utf8_encode

void nss_term_write(nss_term_t *term, size_t len, const char* data) {

}

nss_term_t *nss_create_term(nss_context_t *con, nss_window_t *win, int16_t width, int16_t height) {
    nss_term_t *term = malloc(sizeof(nss_term_t));

    term->width = width;
    term->height = height;
    term->cursor_x = 0;
    term->cursor_y = 0;
    term->focused = 1;
    term->visible = 1;
    term->win = win;
    term->con = con;
    //term->clip = (nss_rect_t) {0,0,127-33,5};


    nss_attrs_t test[] = {
        nss_attrib_italic | nss_attrib_bold,
        nss_attrib_italic | nss_attrib_underlined,
        nss_attrib_strikethrough,
        nss_attrib_underlined | nss_attrib_inverse,
        0
    };

    nss_cid_t fg = nss_color_find(0xffffffff);
    nss_cid_t bg = nss_color_find(0xff000000);
    
    nss_line_t *line = NULL;

    for (size_t k = 0; k < 5; k++) {
        line = create_line(line, NULL, '~' - '!');
        if (!line->prev) term->screen = line;
        for (size_t i = '!'; i <= '~'; i++) {
            line->cell[i - '!'] = NSS_MKCELL(fg, bg, test[k], i);
        }
    }
    line->cell[13] = NSS_MKCELL(nss_color_find(0xffff0000), nss_color_find(0xff00ff00), test[3], 'A');
    line->prev->cell[16] = NSS_MKCELL(nss_color_find(0xffff0000), nss_color_find(0xff00ff00), test[2], 'A');
    term->current_line = term->screen;

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
            if (line == term->current_line && damage.x <= term->cursor_x && term->cursor_x < damage.x + damage.width) {
                nss_window_draw_cursor(term->con, term->win, term->cursor_x, term->cursor_y);
            }
        }
    }
    nss_window_draw_commit(term->con, term->win);
}

void nss_term_resize(nss_term_t *term, int16_t width, int16_t height) {
    term->width = width;
    term->height = height;
    //TODO: Move cursor
}

void nss_term_focus(nss_term_t *term, _Bool focused) {
    int16_t cx = term->cursor_x, cy = term->cursor_y;
    term->focused = focused;
    nss_window_draw(term->con, term->win, cx, cy, &term->current_line->cell[cx], 1);
    nss_window_draw_cursor(term->con, term->win, cx, cy);
    nss_window_update(term->con, term->win, 1, &(nss_rect_t) {cx, cy, 1, 1});
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
