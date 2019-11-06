#ifndef TERM_H_
#define TERM_H_ 1

#include <sys/types.h>
#include "window.h"
#include "util.h"

#define NSS_TERM_NAME "st"
#define NSS_TERM_FPS 60
#define NSS_TERM_SCROLL_DELAY (1000000/240)
#define NSS_TERM_REDRAW_RATE (1000000/NSS_TERM_FPS)

typedef struct nss_term nss_term_t;

nss_term_t *nss_create_term(nss_window_t *win, nss_palette_t pal, int16_t width, int16_t height);
void nss_free_term(nss_term_t *term);
void nss_term_redraw(nss_term_t *term, nss_rect_t rect, _Bool cursor);
void nss_term_redraw_dirty(nss_term_t *term, _Bool cursor);
void nss_term_resize(nss_term_t *term, int16_t width, int16_t height);
void nss_term_visibility(nss_term_t *term, _Bool visible);
void nss_term_focus(nss_term_t *term, _Bool focused);
_Bool nss_term_mouse(nss_term_t *term, int16_t x, int16_t y, nss_mouse_state_t mask, nss_mouse_event_t event, uint8_t button);
void nss_term_answerback(nss_term_t *term, const char *str, ...);
void nss_term_sendkey(nss_term_t *term, const char *str);
void nss_term_scroll_view(nss_term_t *term, int16_t amount);
ssize_t nss_term_read(nss_term_t *term);
int nss_term_fd(nss_term_t *term);
void nss_term_hang(nss_term_t *term);
struct timespec *nss_term_last_scroll_time(nss_term_t *term);
_Bool nss_term_is_altscreen(nss_term_t *term);
_Bool nss_term_is_utf8(nss_term_t *term);

#endif
