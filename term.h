#ifndef TERM_H_
#define TERM_H_ 1

#include "window.h"
#include "util.h"

typedef struct nss_term nss_term_t;

nss_term_t *nss_create_term(void);
void nss_free_term(nss_term_t *term);
void nss_term_redraw(nss_context_t *con, nss_window_t *win, nss_term_t *term, nss_rect_t rect);
void nss_term_get_cursor(nss_term_t *term, int16_t *cursor_x, int16_t *cursor_y);

#endif
