#ifndef TERM_H_
#define TERM_H_ 1

#include <sys/types.h>
#include "window.h"
#include "util.h"

typedef struct nss_line {
    struct nss_line *next, *prev;
    int16_t width;
    int16_t wrap_at;
    uint16_t extra_size;
    uint16_t extra_caps;
    nss_color_t *extra;
    nss_cell_t cell[];
} nss_line_t;

typedef struct nss_input_mode nss_input_mode_t;
typedef struct nss_term nss_term_t;

nss_term_t *nss_create_term(nss_window_t *win, nss_input_mode_t *mode, int16_t width, int16_t height);
void nss_free_term(nss_term_t *term);
void nss_term_redraw_dirty(nss_term_t *term, _Bool cursor);
void nss_term_resize(nss_term_t *term, int16_t width, int16_t height);
void nss_term_visibility(nss_term_t *term, _Bool visible);
void nss_term_focus(nss_term_t *term, _Bool focused);
_Bool nss_term_mouse(nss_term_t *term, int16_t x, int16_t y, nss_mouse_state_t mask, nss_mouse_event_t event, uint8_t button);
void nss_term_sendkey(nss_term_t *term, const char *str, _Bool encode);
void nss_term_sendbreak(nss_term_t *term);
void nss_term_scroll_view(nss_term_t *term, int16_t amount);
ssize_t nss_term_read(nss_term_t *term);
int nss_term_fd(nss_term_t *term);
void nss_term_hang(nss_term_t *term);
_Bool nss_term_is_altscreen(nss_term_t *term);
_Bool nss_term_is_utf8(nss_term_t *term);
void nss_term_damage(nss_term_t *term, nss_rect_t damage);

#endif
