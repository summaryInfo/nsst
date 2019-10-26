#ifndef TERM_H_
#define TERM_H_ 1

#include <sys/types.h>
#include "window.h"
#include "util.h"

#define NSS_TERM_NAME "st"

typedef struct nss_term nss_term_t;

nss_term_t *nss_create_term(nss_context_t *con, nss_window_t *win, int16_t width, int16_t height);
void nss_free_term(nss_term_t *term);
void nss_term_redraw(nss_term_t *term, nss_rect_t rect);
void nss_term_resize(nss_term_t *term, int16_t width, int16_t height);
void nss_term_visibility(nss_term_t *term, _Bool visible);
void nss_term_focus(nss_term_t *term, _Bool focused);
void nss_term_write(nss_term_t *term, const uint8_t *buf, size_t len, _Bool do_echo);
ssize_t nss_term_read(nss_term_t *term);
int nss_term_fd(nss_term_t *term);
void nss_term_hang(nss_term_t *term);

#endif
