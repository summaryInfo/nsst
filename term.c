#define _XOPEN_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

//For openpty() funcion
#if   defined(__linux)
 #include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
 #include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
 #include <libutil.h>
#endif

#include "term.h"
#include "window.h"

typedef struct nss_line {
    struct nss_line *next, *prev;
    int16_t width;
    int16_t wrap_at;
    uint16_t extra_size;
    uint16_t extra_caps;
    nss_color_t *extra;
    nss_cell_t cell[];
} nss_line_t;

#define TTY_MAX_WRITE 256
#define NSS_FD_BUF_SZ 512
#define ESC_MAX_PARAM 16
#define ESC_MAX_INTERM 2
#define ESC_MAX_STR 512
#define MAX_REPORT 256

#define IS_C1(c) ((c) < 0xa0 && (c) >= 0x80)
#define IS_C0(c) ((c) < 0x20)
#define IS_DEL(c) ((c) == 0x7f)
#define IS_STREND(c) (IS_C1(c) || (c) == 0x1b || (c) == 0x1a || (c) == 0x18 || (c) == 0x07)
#define ENABLE_IF(c, m, f) { if (c) { (m) |= (f); } else { (m) &= ~(f); }}

#define MAX_EXTRA_PALETTE (0x10000 - NSS_PALETTE_SIZE)
#define CAPS_INC_STEP(sz) MIN(MAX_EXTRA_PALETTE, (sz) ? 8*(sz)/5 : 4)


typedef struct nss_cursor {
    int16_t x;
    int16_t y;
    nss_cell_t cel;
    nss_color_t fg;
    nss_color_t bg;
    // Shift state
    uint8_t gl;
    uint8_t gr;
    uint8_t gl_ss;
    enum nss_char_set {
        nss_cs_dec_ascii,
        nss_cs_dec_sup,
        nss_cs_dec_graph,
        nss_cs_british, // Also latin-1
        nss_cs_french_canadian,
        nss_cs_finnish,
        nss_cs_german,
        nss_cs_dutch,
        nss_cs_itallian,
        nss_cs_swiss,
        nss_cs_swedish,
        nss_cs_norwegian_dannish,
        nss_cs_french,
        nss_cs_spannish,
        nss_cs_max,
    } gn[4];

    _Bool origin;
    _Bool extra_color;
} nss_cursor_t;

struct nss_term {
    nss_line_t **screen;
    nss_line_t **back_screen;
    nss_rect_t *render_buffer;
    nss_line_t *view;
    nss_line_t *scrollback;
    nss_line_t *scrollback_top;
    int32_t scrollback_limit;
    int32_t scrollback_size;

    nss_cursor_t c;
    nss_cursor_t cs;
    nss_cursor_t back_cs;
    uint32_t prev_ch;

    int16_t prev_mouse_x;
    int16_t prev_mouse_y;
    uint8_t prev_mouse_button;
    int16_t prev_c_x;
    int16_t prev_c_y;

    int16_t width;
    int16_t height;
    int16_t top;
    int16_t bottom;
    uint8_t *tabs;

    struct timespec lastscroll;

    enum nss_term_mode {
        nss_tm_echo = 1 << 0,
        nss_tm_crlf = 1 << 1,
        nss_tm_wrap = 1 << 3,
        nss_tm_visible = 1 << 4,
        nss_tm_focused = 1 << 5,
        nss_tm_altscreen = 1 << 6,
        nss_tm_utf8 = 1 << 7,
        //nss_tm_force_redraw = 1 << 8,
        nss_tm_insert = 1 << 9,
        nss_tm_sixel = 1 << 10,
        nss_tm_8bit = 1 << 11,
        nss_tm_use_protected_area_semantics = 1 << 12,
        nss_tm_disable_altscreen = 1 << 13,
        nss_tm_track_focus = 1 << 14,
        nss_tm_hide_cursor = 1<< 15,
        nss_tm_enable_nrcs = 1 << 16,
        nss_tm_132_preserve_display = 1 << 17,
        nss_tm_scoll_on_output = 1 << 18,
        nss_tm_dont_scroll_on_input = 1 << 19,
        nss_tm_mouse_x10 = 1 << 20,
        nss_tm_mouse_button = 1 << 21,
        nss_tm_mouse_motion = 1 << 22,
        nss_tm_mouse_many = 1 << 23,
        nss_tm_mouse_format_sgr = 1 << 24,
        nss_tm_mouse_mask =
            nss_tm_mouse_x10 | nss_tm_mouse_button |
            nss_tm_mouse_motion | nss_tm_mouse_many,
    } mode;

    struct nss_escape {
        enum nss_escape_state {
            nss_es_ground = 0,
            nss_es_escape = 1 << 0,
            nss_es_intermediate = 1 << 1,
            nss_es_string = 1 << 2,
            nss_es_gotfirst = 1 << 3,
            nss_es_defer = 1 << 4,
            nss_es_ignore = 1 << 5,
            nss_es_csi = 1 << 6,
            nss_es_dcs = 1 << 7,
            nss_es_osc = 1 << 8,
        } state;
        uint8_t private;
        size_t param_idx;
        uint32_t param[ESC_MAX_PARAM];
        size_t interm_idx;
        uint8_t interm[ESC_MAX_INTERM];
        uint8_t final;
        size_t str_idx;
        uint8_t str[ESC_MAX_STR + 1];
    } esc;

    nss_window_t *win;
    nss_color_t *palette;
    pid_t child;
    int fd;
    // Make this just 4 bytes for incomplete utf-8
    size_t fd_buf_pos;
    uint8_t fd_buf[NSS_FD_BUF_SZ];
};

static void term_answerback(nss_term_t *term, const char *str, ...);

static void sigchld_fn(int arg) {
    int status;
    pid_t pid = waitpid(-1, &status, WNOHANG);
    if (pid < 0) {
        // Thats unsafe
        warn("Child wait failed");
        return;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status))
        info("Child exited with status: %d", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        info("Child terminated due to the signal: %d", WTERMSIG(status));
}

static void exec_shell(const char *cmd, const char **args) {

    const struct passwd *pw;
    errno = 0;
    if (!(pw = getpwuid(getuid()))) {
        if (errno) die("getpwuid(): %s", strerror(errno));
        else die("I don't know you");
    }

    const char *sh = cmd;
    if (!(sh = getenv("SHELL")))
        sh = pw->pw_shell[0] ? pw->pw_shell : cmd;

    if (args) cmd = args[0];
    else cmd = sh;

    const char *def[] = {cmd, NULL};
    if (!args) args = def;

    unsetenv("COLUMNS");
    unsetenv("LINES");
    unsetenv("TERMCAP");
    setenv("LOGNAME", pw->pw_name, 1);
    setenv("USER", pw->pw_name, 1);
    setenv("SHELL", sh, 1);
    setenv("HOME", pw->pw_dir, 1);
    setenv("TERM", nss_config_string(nss_config_term_name, "xterm"), 1);

    signal(SIGCHLD, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGALRM, SIG_DFL);

    execvp(cmd, (char *const *)args);
    _exit(1);
}

int tty_open(nss_term_t *term, const char *cmd, const char **args) {
    int slave, master;
    if (openpty(&master, &slave, NULL, NULL, NULL) < 0) {
        warn("Can't create pseudo terminal");
        term->fd = -1;
        return -1;
    }

    pid_t pid;
    switch ((pid = fork())) {
    case -1:
        close(slave);
        close(master);
        warn("Can't fork");
        term->fd = -1;
        return -1;
    case 0:
        setsid();
        errno = 0;
        if (ioctl(slave, TIOCSCTTY, NULL) < 0)
            die("Can't make tty controlling");
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        close(slave);
        close(master);

        exec_shell(cmd, args);
        break;
    default:
        close(slave);
        signal(SIGCHLD, sigchld_fn);
    }
    term->child = pid;
    term->fd = master;

    return master;
}

static nss_cid_t alloc_color(nss_line_t *line, nss_color_t col) {
    static nss_cid_t *buf = NULL, buf_len = 0, *new;
    if (line->extra_size + 1 >= line->extra_caps) {
        if (line->extra) {
            if (buf_len < line->extra_size) {
                new = realloc(buf, line->extra_size * sizeof(nss_cid_t));
                if (!new) return NSS_SPECIAL_BG;
                buf = new, buf_len = line->extra_size;
            }
            memset(buf, 0, buf_len * sizeof(nss_cid_t));
            for (int16_t i = 0; i < line->width; i++) {
                if (line->cell[i].fg >= NSS_PALETTE_SIZE)
                    buf[line->cell[i].fg - NSS_PALETTE_SIZE] = 0xFFFF;
                if (line->cell[i].bg >= NSS_PALETTE_SIZE)
                    buf[line->cell[i].bg - NSS_PALETTE_SIZE] = 0xFFFF;
            }
            int16_t k = 0;
            for (nss_cid_t i = 0; i < line->extra_size; i++) {
                if (buf[i] == 0xFFFF) {
                    line->extra[k] = line->extra[i];
                    for (nss_cid_t j = i + 1; j < line->extra_size; j++)
                        if (line->extra[i] == line->extra[j]) buf[j] = k;
                    buf[i] = k++;
                }
            }
            line->extra_size = k;

            for (int16_t i = 0; i < line->width; i++) {
                if (line->cell[i].fg >= NSS_PALETTE_SIZE && buf[line->cell[i].fg - NSS_PALETTE_SIZE] != 0xFFFF)
                        line->cell[i].fg = buf[line->cell[i].fg - NSS_PALETTE_SIZE] + NSS_PALETTE_SIZE;
                if (line->cell[i].bg >= NSS_PALETTE_SIZE && buf[line->cell[i].bg - NSS_PALETTE_SIZE] != 0xFFFF)
                        line->cell[i].bg = buf[line->cell[i].bg - NSS_PALETTE_SIZE] + NSS_PALETTE_SIZE;
            }
        }
        if (line->extra_size + 1 >= line->extra_caps) {
            if (line->extra_caps == MAX_EXTRA_PALETTE) return NSS_SPECIAL_BG;
            nss_color_t *new = realloc(line->extra, CAPS_INC_STEP(line->extra_caps) * sizeof(nss_color_t));
            if (!new) return NSS_SPECIAL_BG;
            line->extra = new;
            line->extra_caps = CAPS_INC_STEP(line->extra_caps);
        }
    }
    line->extra[line->extra_size++] = col;
    return NSS_PALETTE_SIZE + line->extra_size - 1;
}

static nss_cell_t fixup_color(nss_line_t *line, nss_cursor_t *cur) {
    nss_cell_t cel = cur->cel;
    if (cel.fg >= NSS_PALETTE_SIZE)
        cel.fg = alloc_color(line, cur->fg);
    if (cel.bg >= NSS_PALETTE_SIZE)
        cel.bg = alloc_color(line, cur->bg);
    return cel;
}

static nss_line_t *term_create_line(nss_term_t *term, size_t width) {
    nss_line_t *line = malloc(sizeof(*line) + width * sizeof(line->cell[0]));
    if (line) {
        line->width = width;
        line->wrap_at = 0;
        line->extra_caps = 0;
        line->extra_size = 0;
        line->next = line->prev = NULL;
        line->extra = NULL;
        nss_cell_t cel = fixup_color(line, &term->c);
        for (size_t i = 0; i < width; i++)
            line->cell[i] = cel;
    } else warn("Can't allocate line");
    return line;
}

static void term_free_line(nss_term_t *term, nss_line_t *line) {
    free(line->extra);
    free(line);
}

static void term_line_dirt(nss_line_t *line) {
    for (int16_t i = 0; i < line->width; i++)
        CELL_ATTR_CLR(line->cell[i], nss_attrib_drawn);
}

void nss_term_invalidate_screen(nss_term_t *term) {
    int16_t i = 0;
    nss_line_t *view = term->view;
    for (; view && i < term->height; i++) {
        term_line_dirt(view);
        view = view->next;
    }
    for (int16_t j = 0; j < term->height; j++)
        term_line_dirt(term->screen[j - i]);
}


void nss_term_scroll_view(nss_term_t *term, int16_t amount) {
    if (term->mode & nss_tm_altscreen) return;
    int16_t scrolled = 0;
    if (amount > 0) {
        if (!term->view && term->scrollback) {
            term->view = term->scrollback;
            term_line_dirt(term->scrollback);
            scrolled++;
        }
        if (term->view) {
            while (scrolled < amount && term->view->prev) {
                term->view = term->view->prev;
                scrolled++;
                if (term->view)
                    term_line_dirt(term->view);
            }
        }

        nss_window_shift(term->win, 0, scrolled, term->height - scrolled);
    } else if (amount < 0) {
        while (scrolled < -amount && term->view)
            term->view = term->view->next, scrolled++;

        nss_line_t *line = term->view;
        size_t y0 = 0;
        for (; line && y0 < (size_t)(term->height - scrolled); y0++)
            line = line->next;
        if (y0 < (size_t)(term->height - scrolled)) {
            y0 = term->height - scrolled - y0;
            for(size_t y = y0; y < y0 + scrolled; y++)
                term_line_dirt(term->screen[y]);
        } else {
            for (; line && y0 < (size_t)term->height; y0++) {
                term_line_dirt(line);
                line = line->next;
            }
            for(size_t y = 0; y < (size_t)term->height - y0; y++)
                term_line_dirt(term->screen[y]);
        }

        nss_window_shift(term->win, scrolled, 0, term->height - scrolled);
    }
}

static void term_append_history(nss_term_t *term, nss_line_t *line) {
    if (term->scrollback_limit == 0) {
        term_free_line(term,line);
    } else {
        if (term->scrollback) term->scrollback->next = line;
        else term->scrollback_top = line;
        line->prev = term->scrollback;
        line->next = NULL;
        term->scrollback = line;

        if (term->scrollback_limit >= 0 && ++term->scrollback_size > term->scrollback_limit) {
            if (term->scrollback_top == term->view) {
                // TODO Dont invalidate whole screen
                term->view = term->scrollback_top->next;
                nss_term_invalidate_screen(term);
            }
            nss_line_t *next = term->scrollback_top->next;
            term_free_line(term, term->scrollback_top);

            if (next) next->prev = NULL;
            else term->scrollback = NULL;
            term->scrollback_top = next;
            term->scrollback_size = term->scrollback_limit;
        }
    }
}

static void term_erase(nss_term_t *term, int16_t xs, int16_t ys, int16_t xe, int16_t ye) {
    if (ye < ys) SWAP(int16_t, ye, ys);
    if (xe < xs) SWAP(int16_t, xe, xs);

    xs = MAX(0, MIN(xs, term->width));
    xe = MAX(0, MIN(xe, term->width));
    ys = MAX(0, MIN(ys, term->height));
    ye = MAX(0, MIN(ye, term->height));

    for (; ys < ye; ys++) {
        nss_line_t *line = term->screen[ys];
        nss_cell_t cell = fixup_color(line, &term->c);
        CELL_ATTR_ZERO(cell);
        for(int16_t i = xs; i < xe; i++)
            line->cell[i] = cell;
    }
}

static nss_line_t *term_realloc_line(nss_term_t *term, nss_line_t *line, size_t width) {
    nss_line_t *new = realloc(line, sizeof(*new) + width * sizeof(new->cell[0]));
    if (!new) die("Can't create lines");

    nss_cell_t cell = fixup_color(line, &term->c);
    CELL_ATTR_ZERO(cell);

    for(size_t i = new->width; i < width; i++)
        new->cell[i] = cell;

    new->width = width;
    return new;
}

static void term_protective_erase(nss_term_t *term, int16_t xs, int16_t ys, int16_t xe, int16_t ye) {
    if (ye < ys) SWAP(int16_t, ye, ys);
    if (xe < xs) SWAP(int16_t, xe, xs);

    xs = MAX(0, MIN(xs, term->width));
    xe = MAX(0, MIN(xe, term->width));
    ys = MAX(0, MIN(ys, term->height));
    ye = MAX(0, MIN(ye, term->height));

    for (; ys < ye; ys++) {
        nss_line_t *line = term->screen[ys];
        nss_cell_t cell = fixup_color(line, &term->c);
        CELL_ATTR_ZERO(cell);
        for(int16_t i = xs; i < xe; i++)
            if (!(CELL_ATTR(line->cell[i]) & nss_attrib_protected))
                line->cell[i] = cell;
    }
}

static void term_selective_erase(nss_term_t *term, int16_t xs, int16_t ys, int16_t xe, int16_t ye) {
    if (ye < ys) SWAP(int16_t, ye, ys);
    if (xe < xs) SWAP(int16_t, xe, xs);

    xs = MAX(0, MIN(xs, term->width));
    xe = MAX(0, MIN(xe, term->width));
    ys = MAX(0, MIN(ys, term->height));
    ye = MAX(0, MIN(ye, term->height));

    for (; ys < ye; ys++) {
        nss_line_t *line = term->screen[ys];
        for(int16_t i = xs; i < xe; i++)
            if (!(CELL_ATTR(line->cell[i]) & nss_attrib_protected))
                line->cell[i] = MKCELLWITH(line->cell[i], ' ');
    }
}

static void term_adjust_wide_before(nss_term_t *term, int16_t x, int16_t y, _Bool left, _Bool right) {
    if (x < 0 || x > term->screen[y]->width - 1) return;
    nss_cell_t *cell = &term->screen[y]->cell[x];
    if (left && x > 0 && CELL_ATTR(cell[-1]) & nss_attrib_wide) {
        cell[-1] = MKCELLWITH(cell[-1], ' ');
        CELL_ATTR_CLR(cell[-1], nss_attrib_wide);
    }
    if (right && x < term->screen[y]->width && CELL_ATTR(cell[0]) & nss_attrib_wide) {
        cell[1] = MKCELLWITH(cell[1], ' ');
    }
}

static void term_move_to(nss_term_t *term, int16_t x, int16_t y) {
    term->c.x = MIN(MAX(x, 0), term->width - 1);
    if (term->c.origin)
        term->c.y = MIN(MAX(y, term->top), term->bottom);
    else
        term->c.y = MIN(MAX(y, 0), term->height - 1);
}

static void term_move_to_abs(nss_term_t *term, int16_t x, int16_t y) {
    term_move_to(term, x, (term->c.origin ? term->top : 0) + y);
}

static void term_cursor_mode(nss_term_t *term, _Bool mode) {
    if (mode) /* save */ {
        term->cs = term->c;
    } else /* restore */ {
        term->c = term->cs;
        term->c.x = MIN(term->c.x, term->width);
        term->c.y = MIN(term->c.y, term->height - 1);
    }
}

static void term_swap_screen(nss_term_t *term) {
    term->mode ^= nss_tm_altscreen;
    SWAP(nss_cursor_t, term->back_cs, term->cs);
    SWAP(nss_line_t **, term->back_screen, term->screen);
    term->view = NULL;
}

static void term_scroll(nss_term_t *term, int16_t top, int16_t bottom, int16_t amount, _Bool save) {
    if (term->prev_c_y >= 0 && top <= term->prev_c_y && term->prev_c_y <= bottom) {
        CELL_ATTR_CLR(term->screen[term->prev_c_y]->cell[term->prev_c_x], nss_attrib_drawn);
        if (amount >= 0) term->prev_c_y = MAX(0, term->prev_c_y - MIN(amount, (bottom - top + 1)));
        else term->prev_c_y = MIN(term->height - 1, term->prev_c_y + MIN(-amount, (bottom - top + 1)));
    }

    if (amount > 0) { /* up */
        amount = MIN(amount, (bottom - top + 1));
        size_t rest = (bottom - top + 1) - amount;

        if (save && !(term->mode & nss_tm_altscreen) && term->top == top) {
            for (size_t i = 0; i < (size_t)amount; i++) {
                term_append_history(term, term->screen[top + i]);
                term->screen[top + i] = term_create_line(term, term->width);
            }
        } else term_erase(term, 0, top, term->width, top + amount);

        for (size_t i = 0; i < rest; i++)
            SWAP(nss_line_t *, term->screen[top + i], term->screen[top + amount + i]);

    } else { /* down */
        amount = MAX(amount, -(bottom - top + 1));
        size_t rest = (bottom - top + 1) + amount;

        for (size_t i = 0; i < rest; i++)
            SWAP(nss_line_t *, term->screen[bottom - i], term->screen[bottom + amount - i]);

        term_erase(term, 0, top, term->width, top - amount);
    }

    if (!term->view) {
            struct timespec cur;
        clock_gettime(CLOCK_MONOTONIC, &cur);
        if (TIMEDIFF(term->lastscroll, cur) > NSS_TERM_SCROLL_DELAY/2) {
            if (amount > 0)
                nss_window_shift(term->win, top + amount, top, bottom + 1 - top - amount);
            else if (amount < 0)
                nss_window_shift(term->win, top, top - amount, bottom + 1 - top + amount);
            term->lastscroll = cur;
        } else {
            nss_term_invalidate_screen(term);
        }
    } else nss_term_invalidate_screen(term);
}

struct timespec *nss_term_last_scroll_time(nss_term_t *term) {
    return &term->lastscroll;
}

static void term_set_tb_margins(nss_term_t *term, int16_t top, int16_t bottom) {
    if (top < bottom) {
        term->top = MAX(0, MIN(term->height - 1, top));
        term->bottom = MAX(0, MIN(term->height - 1, bottom));
    } else {
        term->top = 0;
        term->bottom = term->height - 1;
    }
}

static void term_insert_cells(nss_term_t *term, int16_t n) {
    int16_t cx = MIN(term->c.x, term->width - 1);
    n = MAX(0, MIN(n, term->width - cx));
    nss_line_t *line = term->screen[term->c.y];
    term_adjust_wide_before(term, cx, term->c.y, 1, 1);
    memmove(line->cell + cx + n, line->cell + cx, (line->width - cx - n) * sizeof(nss_cell_t));
    for (int16_t i = cx + n; i < term->width; i++)
        CELL_ATTR_CLR(line->cell[i], nss_attrib_drawn);
    term_erase(term, cx, term->c.y, cx + n, term->c.y + 1);
    term_move_to(term, term->c.x, term->c.y);
}

static void term_delete_cells(nss_term_t *term, int16_t n) {
    int16_t cx = MIN(term->c.x, term->width - 1);
    n = MAX(0, MIN(n, term->width - cx));
    nss_line_t *line = term->screen[term->c.y];
    term_adjust_wide_before(term, cx, term->c.y, 1, 0);
    term_adjust_wide_before(term, cx + n - 1, term->c.y, 0, 1);
    memmove(line->cell + cx, line->cell + cx + n, (term->width - cx - n) * sizeof(nss_cell_t));
    for (int16_t i = cx; i < term->width - n; i++)
        CELL_ATTR_CLR(line->cell[i], nss_attrib_drawn);
    term_erase(term, term->width - n, term->c.y, term->width, term->c.y + 1);
    term_move_to(term, term->c.x, term->c.y);
}

static void term_insert_lines(nss_term_t *term, int16_t n) {
    if (term->top <= term->c.y && term->c.y <= term->bottom)
        term_scroll(term, term->c.y, term->bottom, -n, 0);
    term_move_to(term, 0, term->c.y);
}

static void term_delete_lines(nss_term_t *term, int16_t n) {
    if (term->top <= term->c.y && term->c.y <= term->bottom)
        term_scroll(term, term->c.y, term->bottom, n, 0);
    term_move_to(term, 0, term->c.y);
}

static void term_index(nss_term_t *term, _Bool cr) {
    if (term->c.y == term->bottom) {
        term_scroll(term,  term->top, term->bottom, 1, 1);
        term_move_to(term, cr ? 0 : term->c.x, term->c.y);
    } else {
        term_move_to(term, cr ? 0 : term->c.x, term->c.y + 1);
    }
}

static void term_rindex(nss_term_t *term, _Bool cr) {
    if (term->c.y == term->top) {
        term_scroll(term,  term->top, term->bottom, -1, 1);
        term_move_to(term, cr ? 0 : term->c.x, term->c.y);
    } else {
        term_move_to(term, cr ? 0 : term->c.x, term->c.y - 1);
    }
}

static void term_tabs(nss_term_t *term, int16_t n) {
    if (n >= 0) {
        while(term->c.x < term->width - 1 && n--) {
            do term->c.x++;
            while(term->c.x < term->width - 1 && !term->tabs[term->c.x]);
        }
    } else {
        while(term->c.x > 0 && n++) {
            do term->c.x--;
            while(term->c.x > 0 && !term->tabs[term->c.x]);
        }
    }
}

static void term_reset(nss_term_t *term, _Bool hard) {
    for(size_t i = 0; i < NSS_PALETTE_SIZE; i++)
        term->palette[i] = nss_config_color(nss_config_color_0 + i);
    uint32_t args[] = {
        term->palette[NSS_SPECIAL_BG], term->palette[NSS_SPECIAL_FG],
        term->palette[NSS_SPECIAL_CURSOR_BG], term->palette[NSS_SPECIAL_CURSOR_FG],
        nss_config_integer(nss_config_cursor_shape, 0, 6),
        nss_config_integer(nss_config_appkey, 0, 1),
        nss_config_integer(nss_config_appcursor, 0, 1),
        nss_config_integer(nss_config_numlock, 0, 1), 0,
        nss_config_integer(nss_config_has_meta, 0, 1),
        nss_config_integer(nss_config_meta_escape, 0, 1),
        nss_config_integer(nss_config_backspace_is_delete, 0, 1),
        nss_config_integer(nss_config_delete_is_delete, 0, 1),
        nss_config_integer(nss_config_reverse_video, 0, 1), 0};
    nss_window_set(term->win,
            nss_wc_background | nss_wc_foreground |
            nss_wc_cursor_background | nss_wc_cursor_foreground |
            nss_wc_cursor_type | nss_wc_appkey |
            nss_wc_appcursor | nss_wc_numlock |
            nss_wc_keylock | nss_wc_has_meta |
            nss_wc_meta_escape | nss_wc_bs_del |
            nss_wc_del_del | nss_wc_reverse | nss_wc_mouse, args);

    int16_t cx = term->c.x, cy =term->c.y;

    term->c = term->back_cs = term->cs = (nss_cursor_t) {
        .cel = MKCELL(NSS_SPECIAL_FG, NSS_SPECIAL_BG, 0, ' '),
        .fg = nss_config_color(nss_config_fg),
        .bg = nss_config_color(nss_config_bg),
        .gl = 0, .gl_ss = 0, .gr = 2,
        .gn = {nss_cs_dec_ascii, nss_cs_british, nss_cs_british, nss_cs_british}
    };

    term->mode &= nss_tm_focused | nss_tm_visible;
    if (nss_config_integer(nss_config_utf8, 0, 1)) term->mode |= nss_tm_utf8;
    if (!nss_config_integer(nss_config_allow_altscreen, 0, 1)) term->mode |= nss_tm_disable_altscreen;
    if (nss_config_integer(nss_config_init_wrap, 0, 1)) term->mode |= nss_tm_wrap;
    if (!nss_config_integer(nss_config_scroll_on_input, 0, 1)) term->mode |= nss_tm_dont_scroll_on_input;
    if (nss_config_integer(nss_config_scroll_on_output, 0, 1)) term->mode |= nss_tm_scoll_on_output;

    term->top = 0;
    term->bottom = term->height - 1;

    if (hard) {
        memset(term->tabs, 0, term->width * sizeof(term->tabs[0]));
        size_t tabw = nss_config_integer(nss_config_tab_width, 1, 10000);
        for(size_t i = tabw; i < (size_t)term->width; i += tabw)
            term->tabs[i] = 1;

        for(size_t i = 0; i < 2; i++) {
            term_cursor_mode(term, 1);
            term_erase(term, 0, 0, term->width, term->height);
            term_swap_screen(term);
        }
        for (nss_line_t *tmp, *line = term->scrollback; line; line = tmp) {
            tmp = line->prev;
            term_free_line(term, line);
        }

        term->view = NULL;
        term->scrollback_top = NULL;
        term->scrollback = NULL;
        term->scrollback_size = 0;
        nss_window_set_title(term->win, NULL);

        // Hmm?..
        // term->mode |= nss_tm_echo;
    } else {
        term->c.x = cx;
        term->c.y = cy;
    }

}

#define GRAPH0_BASE 0x41
#define GRAPH0_SIZE 62

static uint32_t nrcs_translate(uint8_t set, uint32_t ch, _Bool nrcs) {
    static const unsigned *trans[nss_cs_max] = {
        /* [0x23] [0x40] [0x5B 0x5C 0x5D 0x5E 0x5F 0x60] [0x7B 0x7C 0x7D 0x7E] */
        [nss_cs_british] =           U"£@[\\]^_`{|}~",
        [nss_cs_dutch] =             U"£¾\u0133½|^_`¨f¼´",
        [nss_cs_finnish] =           U"#@ÄÖÅÜ_éäöåü",
        [nss_cs_french] =            U"£à°ç§^_`éùè¨",
        [nss_cs_swiss] =             U"ùàéçêîèôäöüû",
        [nss_cs_french_canadian] =   U"#àâçêî_ôéùèû",
        [nss_cs_german] =            U"#§ÄÖÜ^_`äöüß",
        [nss_cs_itallian] =          U"£§°çé^_ùàòèì",
        [nss_cs_norwegian_dannish] = U"#ÄÆØÅÜ_äæøåü",
        [nss_cs_spannish] =          U"£§¡Ñ¿^_`°ñç~",
        [nss_cs_swedish] =           U"#ÉÆØÅÜ_éæøåü",
    };
    static const unsigned *graph = U" ◆▒␉␌␍␊°±␤␋┘┐┌└┼⎺⎻─⎼⎽├┤┴┬│≤≥π≠£·";
    if (set == nss_cs_dec_ascii) /* do nothing */;
    else if (set == nss_cs_dec_graph) {
        if (0x5F <= ch && ch <= 0x7E)
            return graph[ch - 0x5F];
    } else if (!nrcs && set == nss_cs_british) { /* latin-1 */
        return ch + 0x80;
    } else if (set == nss_cs_dec_sup) {
        ch += 0x80;
        if (ch == 0xA8) ch = U'¤';
        else if (ch == 0xD7) ch = U'Œ';
        else if (ch == 0xDD) ch = U'Ÿ';
        else if (ch == 0xF7) ch = U'œ';
        else if (ch == 0xFD) ch = U'ÿ';
    } else if (trans[set]){
        if (ch == 0x23) return trans[set][0];
        if (ch == 0x40) return trans[set][1];
        if (0x5B <= ch && ch <= 0x60)
            return trans[set][2 + ch - 0x5B];
        if (0x7B <= ch && ch <= 0x7E)
            return trans[set][8 + ch - 0x7B];
    }
    return ch;

    /*
     * Where these symbols did come from?
        "↑", "↓", "→", "←", "█", "▚", "☃",      // A - G
    */
}

static void term_set_cell(nss_term_t *term, int16_t x, int16_t y, uint32_t ch) {

    // In theory this should be disabled while in UTF-8 mode, but
    // in practive applications use these symbols, so keep translating
    // Need to make this configuration option

    if (!(term->mode & nss_tm_utf8) || nss_config_integer(nss_config_allow_nrcs, 0, 1)) {
        if (ch < 0x80) {
            if (term->c.gn[term->c.gl_ss] != nss_cs_dec_ascii) {
                ch = nrcs_translate(term->c.gn[term->c.gl_ss], ch, term->mode & nss_tm_enable_nrcs);
            }
        } else if (ch < 0x100) {
            if (term->c.gn[term->c.gr] != nss_cs_british || term->mode & nss_tm_enable_nrcs)
                ch = nrcs_translate(term->c.gn[term->c.gr], ch - 0x80, term->mode & nss_tm_enable_nrcs);
        }
    }

    nss_line_t *line = term->screen[y];
    nss_cell_t cel = fixup_color(line, &term->c);
    CELL_CHAR_SET(cel, ch);

    line->cell[x] = cel;

    term->c.gl_ss = term->c.gl; // Reset single shift
    term->prev_ch = ch; // For REP CSI Ps b
}

static void term_escape_da(nss_term_t *term, uint8_t mode) {
    switch (mode) {
    case '=':
        term_answerback(term, "\x90!|00000000\x9C");
        break;
    case '>':
        /*
         * 0 - VT100
         * 1 - VT220
         * 2 - VT240
         * 18 - VT330
         * 19 - VT340
         * 24 - VT320
         * 41 - VT420
         * 61 - VT510
         * 64 - VT520
         * 65 - VT525
         */
        term_answerback(term, "\x9B>1;10;0c");
        break;
    default:
        /*
         * ?1;2 - VT100
         * ?1;0 - VT101
         * ?6 - VT102
         * ?62;... - VT220
         * ?63;... - VT320
         * ?64;... - VT420
         *  where ... is
         *       1 - 132-columns
         *       2 - Printer
         *       3 - ReGIS graphics
         *       4 - Sixel graphics
         *       6 - Selective erase
         *       8 - User-defined keys
         *       9 - National Replacement Character sets
         *       15 - Technical characters
         *       16 - Locator port
         *       17 - Terminal state interrogation
         *       18 - User windows
         *       21 - Horizontal scrolling
         *       22 - ANSI color
         *       28 - Rectangular editing
         *       29 - ANSI text locator (i.e., DEC Locator mode).
         */
         term_answerback(term, "\x9B?62;1;2;6;9;22c");
    }
}

#define ESC_DUMP_MAX 1024

static void term_escape_dump(nss_term_t *term) {
    char buf[ESC_DUMP_MAX] = "^[";
    size_t pos = 2;
    int w = 0;
    if (term->esc.state & (nss_es_csi | nss_es_dcs | nss_es_osc)) {
        if (term->esc.state & nss_es_csi) buf[pos++] = '[';
        else if (term->esc.state & nss_es_dcs) buf[pos++] = 'P';
        else if (term->esc.state & nss_es_osc) buf[pos++] = ']';
        if (term->esc.private)
            buf[pos++] = term->esc.private;
        for (ssize_t i = 0; i <= (ssize_t)term->esc.param_idx - (!!(term->esc.state & nss_es_osc)); i++) {
            snprintf(buf + pos, ESC_DUMP_MAX - pos, "%"PRIu32"%n", term->esc.param[i], &w);
            pos += w;
            if (i < (ssize_t)term->esc.param_idx) buf[pos++] = ';';
        }
    }
    for (size_t i = 0; i < term->esc.interm_idx; i++)
        buf[pos++] = term->esc.interm[i];
    if (!(term->esc.state & nss_es_osc))
        buf[pos++] = term->esc.final;
    if (term->esc.state & (nss_es_string | nss_es_defer)) {
        memcpy(buf + pos, term->esc.str, term->esc.str_idx);
        pos += term->esc.str_idx;
        buf[pos++] = '^';
        buf[pos++] = '[';
        buf[pos++] = '\\';
    }
    buf[pos] = 0;
    warn("%s", buf);
}

static void term_escape_dsr(nss_term_t *term) {
    if (term->esc.private == '?') {
        switch(term->esc.param[0]) {
        case 15: /* Printer status -- Has no printer */
            term_answerback(term, "\x9B?13n"); //TODO Has printer -- 10n
            break;
        case 25: /* User defined keys lock -- Locked */
            term_answerback(term, "\x9B?21n"); //TODO Unlocked - ?20n
            break;
        case 26: /* Keyboard Language -- Unknown */
            term_answerback(term, "\x9B?27;0n"); //TODO Print proper keylayout
        }
    } else {
        switch(term->esc.param[0]) {
        case 5: /* Health report -- OK */
            term_answerback(term, "\x9B%dn", 0);
            break;
        case 6: /* Cursor position -- Y;X */
            term_answerback(term, "\x9B%"PRIu16";%"PRIu16"R",
                    (term->c.origin ? -term->top : 0) + term->c.y + 1, MIN(term->c.x, term->width - 1) + 1);
            break;
        }
    }
}

static void term_escape_dcs(nss_term_t *term) {
    warn("DCS"); /* yet nothing here */
}

static nss_color_t color_parse(char *str, char *end) {
    uint64_t val = 0;
    ptrdiff_t sz = end - str;
    if (*str != '#') return 0;
    while (++str < end) {
        if (*str - '0' < 10)
            val = (val << 4) + *str - '0';
        else if (*str - 'A' < 6)
            val = (val << 4) + 10 + *str - 'A';
        else if (*str - 'a' < 6)
            val = (val << 4) + 10 + *str - 'a';
        else return 0;
    }
    nss_color_t col = 0xFF000000;
    switch (sz) {
    case 4:
        for (size_t i = 0; i < 3; i++) {
            col |= (val & 0xF) << (8*i + 4);
            val >>= 4;
        }
        break;
    case 7:
        col |= val;
        break;
    case 10:
        for (size_t i = 0; i < 3; i++) {
            col |= ((val >> 4) & 0xFF) << 8*i;
            val >>= 12;
        }
        break;
    case 13:
        for (size_t i = 0; i < 3; i++) {
            col |= ((val >> 8) & 0xFF) << 8*i;
            val >>= 16;
        }
        break;
    default:
        return 0;
    }
    return col;
}

static void term_escape_osc(nss_term_t *term) {
    switch(term->esc.param[0]) {
        uint32_t args[2];
    case 0: /* Change window icon name and title */
    case 1: /* Change window icon name */
    case 2: /* Change window title */
        nss_window_set_title(term->win, (char *)term->esc.str);
        break;
    case 4: /* Set color */ {
        char *pstr = (char *)term->esc.str, *pnext, *s_end;
        char *pend = pstr + term->esc.str_idx;
        while(pstr < pend && (pnext = strchr(pstr, ';'))) {
            *pnext = '\0';
            errno = 0;
            unsigned long idx = strtoul(pstr, &s_end, 10);

            char *parg  = pnext + 1;
            if ((pnext = strchr(parg, ';'))) *pnext = '\0';
            else pnext = pend - 1;

            if (!errno && !*s_end && s_end != pstr && idx < NSS_PALETTE_SIZE - NSS_SPECIAL_COLORS) {

                nss_color_t col = color_parse(parg, pnext);
                if (col) term->palette[idx] = col;
                else if (parg[0] == '?' && parg[1] == '\0')
                    term_answerback(term, "\2354;#%06X\234", term->palette[idx] & 0x00FFFFFF);
                else term_escape_dump(term);
            }
            pstr = pnext + 1;
        }
        if (pstr < pend && !pnext) {
            for (size_t i = 0; i < term->esc.str_idx; i++)
                if (!term->esc.str[i]) term->esc.str[i] = ';';
            term_escape_dump(term);
        }
        break;
    }
    case 5: /* Set special color */
    case 6: /* Enable/disable special color */
        term_escape_dump(term);
        break;
    case 10: /* Set VT100 foreground color */ {
        nss_color_t col = color_parse((char *)term->esc.str, (char *)term->esc.str + term->esc.str_idx);
        if (col) {
            term->palette[NSS_SPECIAL_FG] = col;
            nss_window_set(term->win, nss_wc_foreground, &col);
        } else term_escape_dump(term);
        break;
    }
    case 11: /* Set VT100 background color */ {
        nss_color_t col = color_parse((char *)term->esc.str, (char *)term->esc.str + term->esc.str_idx);
        if (col) {
            args[0] = args[1] = col;
            nss_color_t def = term->palette[NSS_SPECIAL_BG];
            col = (col & 0x00FFFFFF) | (0xFF000000 & def); // Keep alpha
            term->palette[NSS_SPECIAL_CURSOR_BG] = term->palette[NSS_SPECIAL_BG] = col;
            nss_window_set(term->win, nss_wc_background | nss_wc_cursor_background, args);
        } else term_escape_dump(term);
        break;
    }
    case 12: /* Set Cursor color */ {
        nss_color_t col = color_parse((char *)term->esc.str, (char *)term->esc.str + term->esc.str_idx);
        if (col) {
            term->palette[NSS_SPECIAL_CURSOR_FG] = col;
            nss_window_set(term->win, nss_wc_background, &col);
        } else term_escape_dump(term);
        break;
    }
    case 13: /* Set Mouse foreground color */
    case 14: /* Set Mouse background color */
    case 17: /* Set Highlight background color */
    case 19: /* Set Highlight foreground color */
    case 50: /* Set Font */
    case 52: /* Manipulate selecion data */
        term_escape_dump(term);
        break;
    case 104: /* Reset color */ {
        if (term->esc.str_idx) {
            char *pstr = (char *)term->esc.str, *pnext, *s_end;
            while((pnext = strchr(pstr, ';'))) {
                *pnext = '\0';
                errno = 0;
                unsigned long idx = strtoul(pstr, &s_end, 10);
                if (!errno && !*s_end && s_end != pstr && idx < NSS_PALETTE_SIZE - NSS_SPECIAL_COLORS) {
                    term->palette[idx] = nss_config_color(nss_config_color_0 + idx);
                } else term_escape_dump(term);
                pstr = pnext + 1;
            }
        } else {
            for(size_t i = 0; i < NSS_PALETTE_SIZE - NSS_SPECIAL_COLORS; i++)
                term->palette[i] = nss_config_color(nss_config_color_0 + i);
        }
        break;
    }
    case 105: /* Reset special color */
    case 106: /* Enable/disable special color */
        term_escape_dump(term);
        break;
    case 110: /*Reset  VT100 foreground color */
        args[0] = term->palette[NSS_SPECIAL_FG] = nss_config_color(nss_config_fg);
        nss_window_set(term->win, nss_wc_foreground, args);
        break;
    case 111: /*Reset  VT100 background color */
        args[0] = term->palette[NSS_SPECIAL_BG] = nss_config_color(nss_config_bg);
        args[1] = term->palette[NSS_SPECIAL_CURSOR_BG] = nss_config_color(nss_config_cursor_bg);
        nss_window_set(term->win, nss_wc_background | nss_wc_cursor_background, args);
        break;
    case 112: /*Reset  Cursor color */
        args[0] = term->palette[NSS_SPECIAL_CURSOR_FG] = nss_config_color(nss_config_cursor_fg);
        nss_window_set(term->win, nss_wc_cursor_foreground, args);
        break;
    case 113: /*Reset  Mouse foreground color */
    case 114: /*Reset  Mouse background color */
    case 117: /*Reset  Highlight background color */
    case 119: /*Reset  Highlight foreground color */
        term_escape_dump(term);
        break;
    default:
        term_escape_dump(term);
    }
}

_Bool nss_term_is_altscreen(nss_term_t *term) {
    return term->mode & nss_tm_altscreen;
}
_Bool nss_term_is_utf8(nss_term_t *term) {
    return term->mode & nss_tm_utf8;
}

static size_t term_define_color(nss_term_t *term, size_t arg, _Bool foreground) {
    if (term->esc.param_idx - arg > 1) {
        if (term->esc.param[arg] == 2 && term->esc.param_idx - arg > 2) {
            if (term->esc.param[arg + 1] > 255 || term->esc.param[arg + 2] > 255 || term->esc.param[arg + 3] > 255)
                term_escape_dump(term);
            nss_color_t col = 0xFF;
            for (size_t i = 1; i < 4; i++)
                col = (col << 8) + MIN(term->esc.param[arg + i], 255);
            if (foreground) {
                term->c.cel.fg = 0xFFFF;
                term->c.fg = col;
            } else {
                term->c.cel.bg = 0xFFFF;
                term->c.bg = col;
            }
        }
        return 4;
    } else if (term->esc.param[arg] == 5 && term->esc.param_idx - arg > 0) {
        if (term->esc.param[arg + 1] < NSS_PALETTE_SIZE - NSS_SPECIAL_COLORS) {
            if (foreground)
                term->c.cel.fg = term->esc.param[arg + 1];
            else
                term->c.cel.bg = term->esc.param[arg + 1];
        } else term_escape_dump(term);
        return 2;
    }
    term_escape_dump(term);
    return 0;
}

static void term_escape_sgr(nss_term_t *term) {
    for(uint32_t i = 0; i < term->esc.param_idx + 1; i++) {
        uint32_t p = term->esc.param[i];
        switch(p) {
        case 0:
            CELL_ATTR_CLR(term->c.cel, nss_attrib_blink | nss_attrib_bold |
                    nss_attrib_faint | nss_attrib_inverse | nss_attrib_invisible |
                    nss_attrib_italic | nss_attrib_underlined | nss_attrib_strikethrough);
            term->c.cel.bg = NSS_SPECIAL_BG;
            term->c.cel.fg = NSS_SPECIAL_FG;
            break;
        case 1:
            CELL_ATTR_SET(term->c.cel, nss_attrib_bold);
            break;
        case 2:
            CELL_ATTR_SET(term->c.cel, nss_attrib_faint);
            break;
        case 3:
            CELL_ATTR_SET(term->c.cel, nss_attrib_italic);
            break;
        case 4:
            CELL_ATTR_SET(term->c.cel, nss_attrib_underlined);
            break;
        case 5:
        case 6:
            CELL_ATTR_SET(term->c.cel, nss_attrib_blink);
            break;
        case 7:
            CELL_ATTR_SET(term->c.cel, nss_attrib_inverse);
            break;
        case 8:
            CELL_ATTR_SET(term->c.cel, nss_attrib_invisible);
            break;
        case 9:
            CELL_ATTR_SET(term->c.cel, nss_attrib_strikethrough);
            break;
        case 21:
            /* actually double underlind */
            CELL_ATTR_SET(term->c.cel, nss_attrib_underlined);
            break;
        case 22:
            CELL_ATTR_CLR(term->c.cel, nss_attrib_bold | nss_attrib_faint);
            break;
        case 23:
            CELL_ATTR_CLR(term->c.cel, nss_attrib_italic);
            break;
        case 24:
            CELL_ATTR_CLR(term->c.cel, nss_attrib_underlined);
            break;
        case 25:
        case 26:
            CELL_ATTR_CLR(term->c.cel, nss_attrib_blink);
            break;
        case 27:
            CELL_ATTR_CLR(term->c.cel, nss_attrib_inverse);
            break;
        case 28:
            CELL_ATTR_CLR(term->c.cel, nss_attrib_invisible);
            break;
        case 29:
            CELL_ATTR_CLR(term->c.cel, nss_attrib_strikethrough);
            break;
        case 38:
            i += term_define_color(term, i + 1, 1);
            break;
        case 39:
            term->c.cel.fg = NSS_SPECIAL_FG;
            break;
        case 48:
            i += term_define_color(term, i + 1, 0);
            break;
        case 49:
            term->c.cel.bg = NSS_SPECIAL_BG;
            break;
        default:
            if (30 <= p && p <= 37) {
                term->c.cel.fg = p - 30;
            } else if (40 <= p && p <= 47) {
                term->c.cel.bg = p - 40;
            } else if (90 <= p && p <= 97) {
                term->c.cel.fg = 8 + p - 90;
            } else if (100 <= p && p <= 107) {
                term->c.cel.bg = 8 + p - 100;
            } else term_escape_dump(term);
        }
    }
}
static void term_escape_setmode(nss_term_t *term, _Bool set) {
    if (term->esc.private == '?') {
        for(uint32_t i = 0; i < term->esc.param_idx + 1; i++) {
            uint32_t arg = set;
            switch(term->esc.param[i]) {
            case 1: /* DECCKM */
                nss_window_set(term->win, nss_wc_appcursor, &arg);
                break;
            case 2: /* DECANM */
                // TODO
                term_escape_dump(term);
                break;
            case 3: /* DECCOLM */
                //Just clear screen
                if (!(term->mode & nss_tm_132_preserve_display))
                    term_erase(term, 0, 0, term->width, term->height);
                term_set_tb_margins(term, 0, 0);
                term_move_to(term, 0, 0);
                break;
            case 4: /* DECSCLM */
                // IGNORE
                break;
            case 5: /* DECCNM */
                nss_window_set(term->win, nss_wc_reverse, &arg);
                break;
            case 6: /* DECCOM */
                term->c.origin = set;
                term_move_to_abs(term, 0, 0);
                break;
            case 7: /* DECAWM */
                ENABLE_IF(set, term->mode, nss_tm_wrap);
                break;
            case 8: /* DECARM */
                // IGNORE
                break;
            case 9: /* X10 Mouse tracking */
                arg = 0;
                nss_window_set(term->win, nss_wc_mouse, &arg);
                term->mode &= ~nss_tm_mouse_mask;
                ENABLE_IF(set, term->mode, nss_tm_mouse_x10);
                break;
            case 12: /* Start blinking cursor */
            case 13:
                // IGNORE
                break;
            case 18: /* DECPFF */ // TODO MC
                term_escape_dump(term);
                break;
            case 19: /* DECREX */ // TODO MC
                term_escape_dump(term);
                break;
            case 25: /* DECTCEM */
                ENABLE_IF(!set, term->mode, nss_tm_hide_cursor);
                break;
            case 42: /* DECNRCM */
                ENABLE_IF(set, term->mode, nss_tm_enable_nrcs);
                break;
            case 45: /* Reverse wrap */
                term_escape_dump(term);
                break;
            case 47: /* Enable altscreen */
                if (term->mode & nss_tm_disable_altscreen) break;
                if (set ^ !!(term->mode & nss_tm_altscreen))
                    term_swap_screen(term);
                break;
            case 66: /* DECNKM */
                nss_window_set(term->win, nss_wc_appkey, &arg);
                break;
            case 67: /* DECBKM */
                arg = !set;
                nss_window_set(term->win, nss_wc_bs_del, &arg);
                break;
            case 69: /* DECLRMM */ //TODO DECBI/DECFI
                term_escape_dump(term);
                break;
            case 80: /* DECSDM */ //TODO SIXEL
                term_escape_dump(term);
                break;
            case 95: /* DECNCSM */
                ENABLE_IF(set, term->mode, nss_tm_132_preserve_display);
                break;
            case 1000: /* X11 Mouse tracking */
                arg = 0;
                nss_window_set(term->win, nss_wc_mouse, &arg);
                term->mode &= ~nss_tm_mouse_mask;
                ENABLE_IF(set, term->mode, nss_tm_mouse_button);
                break;
            case 1001: /* Highlight mouse tracking */
                // IGNORE
                break;
            case 1002: /* Cell motion mouse tracking on keydown */
                arg = 0;
                nss_window_set(term->win, nss_wc_mouse, &arg);
                term->mode &= ~nss_tm_mouse_mask;
                ENABLE_IF(set, term->mode, nss_tm_mouse_motion);
                break;
            case 1003: /* All motion mouse tracking */
                nss_window_set(term->win, nss_wc_mouse, &arg);
                term->mode &= ~nss_tm_mouse_mask;
                ENABLE_IF(set, term->mode, nss_tm_mouse_many);
                break;
            case 1004: /* Focus in/out events */
                ENABLE_IF(set, term->mode, nss_tm_track_focus);
                break;
            case 1005: /* UTF-8 mouse tracking */
                // IGNORE
                break;
            case 1006: /* SGR mouse tracking */
                ENABLE_IF(set, term->mode, nss_tm_mouse_format_sgr);
                break;
            case 1010: /* Scroll to bottom on output */
                ENABLE_IF(set, term->mode, nss_tm_scoll_on_output);
                break;
            case 1011: /* Scroll to bottom on keypress */
                ENABLE_IF(!set, term->mode, nss_tm_dont_scroll_on_input);
                break;
            case 1015: /* Urxvt mouse tracking */
                // IGNORE
                break;
            case 1034: /* Interpret meta */
                nss_window_set(term->win, nss_wc_has_meta, &arg);
                break;
            case 1035: /* Numlock */
                nss_window_set(term->win, nss_wc_numlock, &arg);
                break;
            case 1036: /* Meta sends escape */
                nss_window_set(term->win, nss_wc_meta_escape, &arg);
                break;
            case 1037: /* Backspace is delete */
                nss_window_set(term->win, nss_wc_del_del, &arg);
                break;
            case 1046: /* Allow altscreen */
                ENABLE_IF(!set, term->mode, nss_tm_disable_altscreen);
                break;
            case 1047: /* Enable altscreen and clear screen */
                if (term->mode & nss_tm_disable_altscreen) break;
                if (set) term_erase(term, 0, 0, term->width, term->height);
                if (!!(term->mode & nss_tm_altscreen) ^ set)
                    term_swap_screen(term);
                break;
            case 1048: /* Save cursor  */
                term_cursor_mode(term, set);
                break;
            case 1049: /* Save cursor and switch to altscreen */
                if (term->mode & nss_tm_disable_altscreen) break;
                if (set) term_erase(term, 0, 0, term->width, term->height);
                if (!(term->mode & nss_tm_altscreen) && set) {
                    term_cursor_mode(term, 1);
                    term_swap_screen(term);
                } else if (term->mode & nss_tm_altscreen && !set) {
                    term_swap_screen(term);
                    term_cursor_mode(term, 0);
                }
                break;
            case 2004: /* Bracketed paste */
                term_escape_dump(term);
                break;
            default:
                term_escape_dump(term);
            }
        }
    } else {
        for(uint32_t i = 0; i < term->esc.param_idx + 1; i++) {
            uint32_t arg = set;
            switch(term->esc.param[i]) {
            case 0: /* Default - nothing */
                break;
            case 2: /* KAM */
                nss_window_set(term->win, nss_wc_keylock, &arg);
                break;
            case 4: /* IRM */
                ENABLE_IF(set, term->mode, nss_tm_insert);
                break;
            case 12: /* SRM */
                ENABLE_IF(set, term->mode, nss_tm_echo);
                break;
            case 20: /* LNM */
                ENABLE_IF(set, term->mode, nss_tm_crlf);
                break;
            default:
                term_escape_dump(term);
            }
        }
    }
}

static void term_escape_csi(nss_term_t *term) {
    //DUMP
    //term_escape_dump(term);

#define PARAM(i, d) (term->esc.param[i] ? term->esc.param[i] : (d))
    if (!term->esc.interm_idx) {
        switch (term->esc.final) {
        case '@': /* ICH */
            term_insert_cells(term, PARAM(0, 1));
            term_move_to(term, term->c.x, term->c.y);
            break;
        case 'A': /* CUU */
            term_move_to(term, term->c.x, term->c.y - PARAM(0, 1));
            break;
        case 'B': /* CUD */
        case 'e': /* VPR */
            term_move_to(term, term->c.x, term->c.y + PARAM(0, 1));
            break;
        case 'C': /* CUF */
            term_move_to(term, MIN(term->c.x, term->width - 1) + PARAM(0, 1), term->c.y);
            break;
        case 'D': /* CUB */
            term_move_to(term, MIN(term->c.x, term->width - 1) - PARAM(0, 1), term->c.y);
            break;
        case 'E': /* CNL */
            term_move_to(term, 0, term->c.y + PARAM(0, 1));
            break;
        case 'F': /* CPL */
            term_move_to(term, 0, term->c.y - PARAM(0, 1));
            break;
        case '`': /* HPA */
        case 'G': /* CHA */
            term_move_to(term, PARAM(0, 1) - 1, term->c.y);
            break;
        case 'H': /* CUP */
        case 'f': /* HVP */
            term_move_to_abs(term, PARAM(1, 1) - 1, PARAM(0, 1) - 1);
            break;
        case 'I': /* CHT */
            term_tabs(term, PARAM(0, 1));
            break;
        case 'J': /* ED */ /* ? DECSED */ {
            void (*erase)(nss_term_t *, int16_t, int16_t, int16_t, int16_t) = term_erase;
            if(term->esc.private == '?')
                erase = term_selective_erase;
            else if(term->mode & nss_tm_use_protected_area_semantics)
                erase = term_protective_erase;
            switch(PARAM(0, 0)) {
            case 0: /* Below */
                term_adjust_wide_before(term, term->c.x, term->c.y, 1, 0);
                erase(term, term->c.x, term->c.y, term->width, term->c.y + 1);
                erase(term, 0, term->c.y + 1, term->width, term->height);
                break;
            case 1: /* Above */
                term_adjust_wide_before(term, term->c.x, term->c.y, 0, 1);
                erase(term, 0, term->c.y, term->c.x + 1, term->c.y + 1);
                erase(term, 0, 0, term->width, term->c.y);
                break;
            case 2: /* All */
                erase(term, 0, 0, term->width, term->height);
                break;
            case 3: /* Saved Lines?, xterm */
            default:
                term_escape_dump(term);
            }
            term_move_to(term, term->c.x, term->c.y);
            break;
        }
        case 'K': /* EL */ /* ? DECSEL */ {
            void (*erase)(nss_term_t *, int16_t, int16_t, int16_t, int16_t) = term_erase;
            if(term->esc.private == '?')
                erase = term_selective_erase;
            else if(term->mode & nss_tm_use_protected_area_semantics)
                erase = term_protective_erase;
            switch(PARAM(0, 0)) {
            case 0: /* To the right */
                term_adjust_wide_before(term, term->c.x, term->c.y, 1, 0);
                erase(term, term->c.x, term->c.y, term->width, term->c.y + 1);
                break;
            case 1: /* To the left */
                term_adjust_wide_before(term, term->c.x, term->c.y, 0, 1);
                erase(term, 0, term->c.y, term->c.x + 1, term->c.y + 1);
                break;
            case 2: /* Whole */
                erase(term, 0, term->c.y, term->width, term->c.y + 1);
                break;
            default:
                term_escape_dump(term);
            }
            term_move_to(term, term->c.x, term->c.y);
            break;
        }
        case 'L': /* IL */
            term_insert_lines(term, PARAM(0, 1));
            break;
        case 'M': /* DL */
            term_delete_lines(term, PARAM(0, 1));
            break;
        case 'P': /* DCH */
            term_delete_cells(term, PARAM(0, 1));
            term_move_to(term, term->c.x, term->c.y);
            break;
        case 'S': /* SU */ /* ? Set graphics attributes, xterm */
            if (!term->esc.private)
                term_scroll(term, term->top, term->bottom, PARAM(0, 1), 0);
            else term_escape_dump(term);
            break;
        case 'T': /* SD */ /* > Reset title, xterm */
            if (term->esc.private) {
                term_escape_dump(term);
                break;
            } /* fallthrough */
        case '^': /* SD */
            term_scroll(term, term->top, term->bottom, -PARAM(0, 1), 0);
            break;
        case 'X': /* ECH */
            term_protective_erase(term, term->c.x, term->c.y, term->c.x + PARAM(0, 1), term->c.y + 1);
            term_move_to(term, term->c.x, term->c.y);
            break;
        case 'Z': /* CBT */
            term_tabs(term, -PARAM(0, 1));
            break;
        case 'a': /* HPR */
            term_move_to(term, MIN(term->c.x, term->width - 1) + PARAM(0, 1), term->c.y + PARAM(1, 0));
            break;
        case 'b': /* REP */
            term_escape_dump(term);
            break;
        case 'c': /* Primary DA */ /* > Secondary DA */ /* = Tertinary DA */
            term_escape_da(term, term->esc.private);
            break;
        case 'd': /* VPA */
            term_move_to_abs(term, term->c.x, PARAM(0, 1) - 1);
            break;
        case 'g': /* TBC */
            switch (PARAM(0, 0)) {
            case 0:
                term->tabs[MIN(term->c.x, term->width - 1)] = 0;
                break;
            case 3:
                memset(term->tabs, 0, term->width * sizeof(term->tabs[0]));
                break;
            }
            break;
        case 'h': /* SM */ /* ? DECSET */
            term_escape_setmode(term, 1);
            break;
        case 'i': /* MC */ /* ? MC */
            term_escape_dump(term);
            break;
        case 'l': /* RM */ /* ? DECRST */
            term_escape_setmode(term, 0);
            break;
        case 'm': /* SGR */
            term_escape_sgr(term);
            break;
        case 'n': /* DSR */ /* ? DSR */ /* > Disable key modifires, xterm */
            term_escape_dsr(term);
            break;
        case 'p': /* > Set pointer mode, xterm */
            term_escape_dump(term);
            break;
        case 'q': /* DECLL */
            term_escape_dump(term);
            break;
        case 'r': /* DECSTBM */ /* ? Restore DEC privite mode */
            if (!term->esc.private) {
                term_set_tb_margins(term, PARAM(0, 1) - 1, PARAM(1, (size_t)term->height) - 1);
                //term_move_to_abs(term, 0, 0);
            } else term_escape_dump(term);
            break;
        case 's': /* DECSLRM/(SCOSC) */ /* ? Save DEC privite mode */
            if (!term->esc.private) {
                term_cursor_mode(term, 1);
            } else term_escape_dump(term);
            break;
        case 't': /* Window operations, xterm */ /* > Title mode, xterm */
            term_escape_dump(term);
            break;
        case 'u': /* (SCORC) */
            term_cursor_mode(term, 0);
            break;
        case 'x': /* DECREQTPARAM */
            term_escape_dump(term);
            break;
        default:
            term_escape_dump(term);
        }
    } else if (term->esc.interm_idx == 1) {
        switch(term->esc.interm[0]) {
        case ' ':
            switch(term->esc.final) {
            case '@': /* SL */
                term_escape_dump(term);
                break;
            case 'A': /* SR */
                term_escape_dump(term);
                break;
            case 't': /* DECSWBV */
                term_escape_dump(term);
                break;
            case 'q': /* DECSCUSR */ {
                uint32_t arg;
                switch(PARAM(0, 1)) {
                case 1: /* Blinking block */
                case 2: /* Steady block */
                    arg = nss_cursor_block;
                    nss_window_set(term->win, nss_wc_cursor_type, &arg);
                    break;
                case 3: /* Blinking underline */
                case 4: /* Steady underline */
                    arg = nss_cursor_underline;
                    nss_window_set(term->win, nss_wc_cursor_type, &arg);
                    break;
                case 5: /* Blinking bar */
                case 6: /* Steady bar */
                    arg = nss_cursor_bar;
                    nss_window_set(term->win, nss_wc_cursor_type, &arg);
                }
                break;
            }
            default:
                term_escape_dump(term);
            }
            break;
        case '!':
            switch(term->esc.final) {
            case 'p': /* DECSTR */
                term_reset(term, 0);
                break;
            default:
                term_escape_dump(term);
            }
            break;
        case '"':
            switch(term->esc.final) {
            case 'p': /* DECSCL */
                term_reset(term, 1);
                switch(PARAM(0, 62)) { /* TODO Compatablility restrictions */
                case 61: /* VT100 */
                    break;
                case 62: /* VT200 */
                    break;
                case 63: /* VT300 */
                    break;
                case 64: /* VT400 */
                    break;
                case 65: /* VT500 */
                    break;
                default:
                    term_escape_dump(term);
                }
                switch(PARAM(1, 2)) {
                case 2:
                    term->mode |= nss_tm_8bit;
                    break;
                case 1:
                    term->mode &= ~nss_tm_8bit;
                    break;
                default:
                    term_escape_dump(term);
                }
                break;
            case 'q': /* DECSCA */
                switch(PARAM(0, 2)) {
                case 1:
                    CELL_ATTR_SET(term->c.cel, nss_attrib_protected);
                    break;
                case 2:
                    CELL_ATTR_CLR(term->c.cel, nss_attrib_protected);
                    break;
                }
                term->mode &= ~nss_tm_use_protected_area_semantics;
                break;
            default:
                term_escape_dump(term);
            }
            break;
        case '#':
            switch(term->esc.final) {
            case 'y': /* XTCHECKSUM */
                term_escape_dump(term);
                break;
            case 'p': /* XTPUSHSGR */
            case '{':
                term_escape_dump(term);
                break;
            case '|': /* XTREPORTSGR */
                term_escape_dump(term);
                break;
            case 'q': /* XTPOPSGR */
            case '}':
                term_escape_dump(term);
                break;
            default:
                term_escape_dump(term);
            }
            break;
        case '$':
            switch(term->esc.final) {
            case 'p': /* DECRQM */ /* ? DECRQM */
                term_escape_dump(term);
                break;
            case 'r': /* DECCARA */
                term_escape_dump(term);
                break;
            case 't': /* DECRARA */
                term_escape_dump(term);
                break;
            case 'v': /* DECCRA */
                term_escape_dump(term);
                break;
            case 'w': /* DECRQPSR */
                term_escape_dump(term);
                break;
            case 'x': /* DECFRA */
                term_escape_dump(term);
                break;
            case 'z': /* DECERA */
                term_escape_dump(term);
                break;
            case '{': /* DECSERA */
                term_escape_dump(term);
                break;
            default:
                term_escape_dump(term);
            }
            break;
        case '\'':
            switch(term->esc.final) {
            case 'w': /* DECEFR */
                term_escape_dump(term);
                break;
            case 'z': /* DECELR */
                term_escape_dump(term);
                break;
            case '{': /* DECSLE */
                term_escape_dump(term);
                break;
            case '|': /* DECRQLP */
                term_escape_dump(term);
                break;
            case '}': /* DECIC */
                term_escape_dump(term);
                break;
            case '~': /* DECDC */
                term_escape_dump(term);
                break;
            default:
                term_escape_dump(term);
            }
            break;
        case '*':
            switch(term->esc.final) {
            case 'x': /* DECSACE */
                term_escape_dump(term);
                break;
            case 'y': /* DECRQCRA */
                term_escape_dump(term);
                break;
            case '|': /* DECSNLS */
                term_escape_dump(term);
                break;
            default:
                term_escape_dump(term);
            }
            break;
        }
    } else term_escape_dump(term);

#undef PARAM

}

static void term_escape_esc(nss_term_t *term) {
    //DUMP
    //term_escape_dump(term);
    if (term->esc.interm_idx == 0) {
        uint32_t arg;
        switch(term->esc.final) {
        case 'D': /* IND */
            term_index(term, 0);
            break;
        case 'E': /* NEL */
            term_index(term, 1);
            break;
        case 'F': /* HP Home Down */
            term_move_to(term, 0, term->height - 1);
            break;
        case 'H': /* HTS */
            term->tabs[MIN(term->c.x, term->width - 1)] = 1;
            break;
        case 'M': /* RI */
            term_rindex(term, 0);
            break;
        case 'N': /* SS2 */
            term->c.gl_ss = 2;
            break;
        case 'O': /* SS3 */
            term->c.gl_ss = 3;
            break;
        case 'V': /* SPA */
            CELL_ATTR_SET(term->c.cel, nss_attrib_protected);
            term->mode |= nss_tm_use_protected_area_semantics;
            break;
        case 'W': /* EPA */
            CELL_ATTR_CLR(term->c.cel, nss_attrib_protected);
            term->mode |= nss_tm_use_protected_area_semantics;
            break;
        case 'Z': /* DECID */
            term_escape_da(term, 0);
            break;
        case '\\': /* ST */
            /* do nothing */
            break;
        case '6': /* DECBI */
            term_escape_dump(term);
            break;
        case '7': /* DECSC */
            term_cursor_mode(term, 1);
            break;
        case '8': /* DECRC */
            term_cursor_mode(term, 0);
            break;
        case '9': /* DECFI */
            term_escape_dump(term);
            break;
        case '=': /* DECKPAM */
            arg = 1;
            nss_window_set(term->win, nss_wc_appkey, &arg);
            break;
        case '>': /* DECKPNM */
            arg = 0;
            nss_window_set(term->win, nss_wc_appkey, &arg);
            break;
        case 'c': /* RIS */
            term_reset(term, 1);
            break;
        case 'l': /* HP Memory lock */
            term_set_tb_margins(term, term->c.y, term->height - 1);
            break;
        case 'm': /* HP Memory unlock */
            term_set_tb_margins(term, 0, term->height - 1);
            break;
        case 'n': /* LS2 */
            term->c.gl = term->c.gl_ss = 2;
            break;
        case 'o': /* LS3 */
            term->c.gl = term->c.gl_ss = 3;
            break;
        case '|': /* LS3R */
            term->c.gr = 3;
            break;
        case '}': /* LS2R */
            term->c.gr = 2;
            break;
        case '~': /* LS1R */
            term->c.gr = 1;
            break;
        default:
            term_escape_dump(term);
        }
    } else if (term->esc.interm_idx == 1) {
        switch (term->esc.interm[0]) {
        case ' ':
            switch (term->esc.final) {
            case 'F': /* S7C1T */
                term->mode &= ~nss_tm_8bit;
                break;
            case 'G': /* S8C1T */
                term->mode |= nss_tm_8bit;
                break;
            case 'L': /* ANSI_LEVEL_1 */
            case 'M': /* ANSI_LEVEL_2 */
                term->c.gn[1] = nss_cs_dec_sup;
                term->c.gr = 1;
                /* fallthrough */
            case 'N': /* ANSI_LEVEL_3 */
                term->c.gn[0] = nss_cs_dec_ascii;
                term->c.gl = term->c.gl_ss = 0;
                break;
            default:
                term_escape_dump(term);
            }
            break;
        case '#':
            switch (term->esc.final) {
            case '3': /* DECDHL */
            case '4': /* DECDHL */
            case '5': /* DECSWL */
            case '6': /* DECDWL */
                term_escape_dump(term);
                break;
            case '8': /* DECALN*/
                for (int16_t i = 0; i < term->height; i++)
                    for(int16_t j = 0; j < term->width; j++)
                        term_set_cell(term, j, i, 'E');
                break;
            default:
                term_escape_dump(term);
            }
            break;
        case '%':
            switch (term->esc.final) {
            case '@': term->mode &= ~nss_tm_utf8; break;
            case 'G': term->mode |= nss_tm_utf8; break;
            default:
                term_escape_dump(term);
            }
            break;
        case '(': /* GZD4 */
        case ')': /* G1D4 */
        case '*': /* G2D4 */
        case '+': /* G3D4 */ {
            enum nss_char_set *set = &term->c.gn[term->esc.interm[0] - '('];
            switch (term->esc.final) {
            case 'A': *set = nss_cs_british; break;
            case 'B': *set = nss_cs_dec_ascii; break;
            case 'C': case '5': *set = nss_cs_finnish; break;
            case 'H': case '7': *set = nss_cs_swedish; break;
            case 'K': *set = nss_cs_german; break;
            case 'Q': case '9': *set = nss_cs_french_canadian; break;
            case 'R': case 'f': *set = nss_cs_french; break;
            case 'Y': *set = nss_cs_itallian; break;
            case 'Z': *set = nss_cs_spannish; break;
            case '4': *set = nss_cs_dutch; break;
            case '=': *set = nss_cs_swiss; break;
            case '`': case 'E': case '6': *set = nss_cs_norwegian_dannish; break;
            case '0': *set = nss_cs_dec_graph; break;
            case '<': *set = nss_cs_dec_sup; break;
            default:
                term_escape_dump(term);
            }
            break;
        }
        case '-': /* G1D6 */
        case '.': /* G2D6 */
        case '/': /* G3D6 */ {
            enum nss_char_set *set = &term->c.gn[term->esc.interm[0] - '-'];
            switch(term->esc.final) {
            case 'A': *set = nss_cs_british; term->mode &= ~nss_tm_enable_nrcs; break;
            default:
                term_escape_dump(term);
            };
            break;
        }
        default:
            term_escape_dump(term);
        }
    } else {
        switch (term->esc.interm[0]) {
        case '(': /* GZD4 */
        case ')': /* G1D4 */
        case '*': /* G2D4 */
        case '+': /* G3D4 */
            term_escape_dump(term);
            break;
        default:
            term_escape_dump(term);
        }
    }
}

static void term_escape_reset(nss_term_t *term) {
    memset(&term->esc, 0, sizeof(term->esc));
}


static void term_escape_control(nss_term_t *term, uint32_t ch) {
    // DUMP
    //if (IS_C0(ch) || IS_DEL(ch)) {
    //    if (ch != 0x1B) warn("^%c", ch ^ 0x40);
    //} else warn("^[%c", ch ^ 0xC0);

    switch (ch) {
    case 0x00: /* NUL (IGNORE) */
    case 0x01: /* SOH (IGNORE) */
    case 0x02: /* STX (IGNORE) */
    case 0x03: /* ETX (IGNORE) */
    case 0x04: /* EOT (IGNORE) */
        return;
    case 0x05: /* ENQ */
        term_answerback(term, "%s", nss_config_string(nss_config_answerback_string, ""));
        break;
    case 0x06: /* ACK (IGNORE) */
        return;
    case 0x07: /* BEL */
        if (term->esc.state & nss_es_string) {
            if (term->esc.state & nss_es_ignore)
                /* do nothing */;
            else if (term->esc.state & nss_es_dcs)
                term_escape_dcs(term);
            else if (term->esc.state & nss_es_osc)
                term_escape_osc(term);
        }
        else /* term_bell() -- TODO */;
        break;
    case 0x08: /* BS */
        term_move_to(term, MIN(term->c.x, term->width - 1) - 1, term->c.y);
        return;
    case 0x09: /* HT */
        term_tabs(term, 1);
        return;
    case 0x0a: /* LF */
    case 0x0b: /* VT */
    case 0x0c: /* FF */
        term_index(term, term->mode & nss_tm_crlf);
        return;
    case 0x0d: /* CR */
        term_move_to(term, 0, term->c.y);
        return;
    case 0x0e: /* SO/LS1 */
        term->c.gl = term->c.gl_ss = 1;
        return;
    case 0x0f: /* SI/LS0 */
        term->c.gl = term->c.gl_ss = 0;
        return;
    case 0x10: /* DLE (IGNORE) */
        return;
    case 0x11: /* XON (IGNORE) */
    case 0x12: /* DC2 (IGNORE) */
    case 0x13: /* XOFF (IGNORE) */
    case 0x14: /* DC4 (IGNORE) */
    case 0x15: /* NAK (IGNORE) */
    case 0x16: /* SYN (IGNORE) */
    case 0x17: /* ETB (IGNORE) */
        return;
    case 0x18: /* CAN */
        break;
    case 0x19: /* EM (IGNORE) */
        return;
    case 0x1a: /* SUB */
        term_move_to(term, term->c.x, term->c.y);
        term_set_cell(term, term->c.x, term->c.y, '?');
        break;
    case 0x1b: /* ESC */
        if (term->esc.state & nss_es_string) {
            term->esc.state &= (nss_es_osc | nss_es_dcs | nss_es_ignore);
            term->esc.state |= nss_es_escape | nss_es_defer;
            return;
        }
        term_escape_reset(term);
        term->esc.state = nss_es_escape;
        return;
    case 0x1c: /* FS (IGNORE) */
    case 0x1d: /* GS (IGNORE) */
    case 0x1e: /* RS (IGNORE) */
    case 0x1f: /* US (IGNORE) */
    case 0x7f: /* DEL (IGNORE) */
        return;
    case 0x80: /* PAD */
    case 0x81: /* HOP */
    case 0x82: /* BPH */
    case 0x83: /* NBH */
        warn("^%"PRIx32, ch ^ 0xC0);
        break;
    case 0x84: /* IND - Index */
        term_index(term, 0);
        break;
    case 0x85: /* NEL -- Next line */
        term_index(term, 1);
        break;
    case 0x86: /* SSA */
    case 0x87: /* ESA */
        warn("^%"PRIx32, ch ^ 0xC0);
        break;
    case 0x88: /* HTS -- Horizontal tab stop */
        term->tabs[MIN(term->c.x, term->width - 1)] = 1;
        break;
    case 0x89: /* HTJ */
    case 0x8a: /* VTS */
    case 0x8b: /* PLD */
    case 0x8c: /* PLU */
        warn("^%"PRIx32, ch ^ 0xC0);
        break;
    case 0x8d: /* RI - Reverse Index */
        term_rindex(term, 0);
        break;
    case 0x8e: /* SS2 - Single Shift 2 */
        term->c.gl_ss = 2;
        break;
    case 0x8f: /* SS3 - Single Shift 3 */
        term->c.gl_ss = 3;
        break;
    case 0x90: /* DCS -- Device Control String */
        term_escape_reset(term);
        term->esc.state = nss_es_dcs | nss_es_escape;
        return;
    case 0x91: /* PU1 */
    case 0x92: /* PU2 */
    case 0x93: /* STS */
    case 0x94: /* CCH */
    case 0x95: /* MW */
        warn("^%"PRIx32, ch ^ 0xC0);
        break;
    case 0x96: /* SPA - Start of Protected Area */
        CELL_ATTR_SET(term->c.cel, nss_attrib_protected);
        term->mode |= nss_tm_use_protected_area_semantics;
        break;
    case 0x97: /* EPA - End of Protected Area */
        CELL_ATTR_CLR(term->c.cel, nss_attrib_protected);
        term->mode |= nss_tm_use_protected_area_semantics;
        break;
    case 0x98: /* SOS - Start Of String */
        term_escape_reset(term);
        term->esc.state = nss_es_ignore | nss_es_string;
        return;
    case 0x99: /* SGCI */
        warn("^%"PRIx32, ch ^ 0xC0);
        break;
    case 0x9a: /* DECID -- Identify Terminal */
        term_escape_da(term, 0);
        break;
    case 0x9b: /* CSI - Control Sequence Introducer */
        term_escape_reset(term);
        term->esc.state = nss_es_csi | nss_es_escape;
        break;
    case 0x9c: /* ST - String terminator */
        if (term->esc.state & nss_es_string) {
            if (term->esc.state & nss_es_ignore)
                /* do nothing */;
            else if (term->esc.state & nss_es_dcs)
                term_escape_dcs(term);
            else if (term->esc.state & nss_es_osc)
                term_escape_osc(term);
        }
        break;
    case 0x9d: /* OSC -- Operating System Command */
        term_escape_reset(term);
        term->esc.state = nss_es_osc | nss_es_string;
        return;
    case 0x9e: /* PM -- Privacy Message */
    case 0x9f: /* APC -- Application Program Command */
        term_escape_reset(term);
        term->esc.state = nss_es_string | nss_es_ignore;
        return;
    }
    term_escape_reset(term);
}

static void term_putchar(nss_term_t *term, uint32_t ch) {
    int16_t width = wcwidth(ch);

    if (width < 0 && !(IS_C0(ch) || IS_DEL(ch) || IS_C1(ch)))
        ch = UTF_INVAL, width =1;

    //info("UTF %"PRIx32" '%s'", ch, buf);

    if (term->esc.state & nss_es_string && !IS_STREND(ch)) {
        if (term->esc.state & nss_es_ignore) return;
        if (term->esc.state & nss_es_dcs) {
            if (IS_DEL(ch)) return;
        } else if (term->esc.state & nss_es_osc) {
            if (IS_C0(ch)) return;
            if (!(term->esc.state & nss_es_gotfirst)) {
                if ('0' <= ch && ch <= '9') {
                    term->esc.param[term->esc.param_idx] *= 10;
                    term->esc.param[term->esc.param_idx] += ch - '0';
                } else if (ch == ';') {
                    if (term->esc.state & nss_es_intermediate) {
                        term->esc.param_idx++;
                        term->esc.state |= nss_es_gotfirst;
                    } else term->esc.state |= nss_es_ignore;
                } else {
                    if (term->esc.state & nss_es_intermediate) {
                        term->esc.state |= nss_es_ignore;
                    } else if (ch == 'l') {
                        term->esc.param[term->esc.param_idx++] = 1;
                    } else if (ch == 'L') {
                        term->esc.param[term->esc.param_idx++] = 2;
                    }
                }
                term->esc.state |= nss_es_intermediate;
                return;
            }
        }

        uint8_t buf[UTF8_MAX_LEN + 1];
        size_t char_len = utf8_encode(ch, buf, buf + UTF8_MAX_LEN);
        buf[char_len] = '\0';

        if (term->esc.str_idx + char_len > ESC_MAX_STR) return;
        memcpy(term->esc.str + term->esc.str_idx, buf, char_len + 1);
        term->esc.str_idx += char_len;
        return;
    } else if (IS_C0(ch) || IS_C1(ch) || (IS_DEL(ch) && !term->esc.state && term->c.gn[term->c.gl_ss])) {
        // We should print DEL if 96 character set is assigned to GL and we are at ground state
        if (!IS_DEL(ch) || term->esc.state || term->c.gn[term->c.gl_ss] !=
                nss_cs_british || (term->mode & nss_tm_enable_nrcs)) {
            if (!IS_STREND(ch) && (term->esc.state & nss_es_dcs)) return;
            term_escape_control(term, ch);
            return;
        }
    } else if (term->esc.state & nss_es_escape) {
        if (term->esc.state & nss_es_defer) {
            if (ch == 0x5c) { /* ST */
                if (term->esc.state & nss_es_ignore)
                    /* do nothing */;
                else if (term->esc.state & nss_es_dcs)
                    term_escape_dcs(term);
                else if (term->esc.state & nss_es_osc)
                    term_escape_osc(term);
            }
            term_escape_reset(term);
            term->esc.state = nss_es_escape;
        }

        if (0x20 <= ch && ch <= 0x2f) {
            term->esc.state |= nss_es_intermediate;
            if (term->esc.interm_idx < ESC_MAX_INTERM)
                term->esc.interm[term->esc.interm_idx++] = ch;
            else term->esc.state |= nss_es_ignore;
        } else {
            if (term->esc.state & (nss_es_csi | nss_es_dcs)) {
                if (0x30 <= ch && ch <= 0x39) { /* 0-9 */
                    if (term->esc.state & nss_es_intermediate)
                        term->esc.state |= nss_es_ignore;
                    if (term->esc.param_idx < ESC_MAX_PARAM) {
                        term->esc.param[term->esc.param_idx] *= 10;
                        term->esc.param[term->esc.param_idx] += ch - 0x30;
                    }
                } else if (ch == 0x3a) { /* : */
                    term->esc.state |= nss_es_ignore;
                } else if (ch == 0x3b) { /* ; */
                    if (term->esc.state & nss_es_intermediate)
                        term->esc.state |= nss_es_ignore;
                    else if (term->esc.param_idx < ESC_MAX_PARAM)
                        term->esc.param[++term->esc.param_idx] = 0;
                } else if (0x3c <= ch && ch <= 0x3f) {
                    if (term->esc.state & (nss_es_gotfirst | nss_es_intermediate))
                        term->esc.state |= nss_es_ignore;
                    else term->esc.private = ch;
                } else if (0x40 <= ch && ch <= 0x7e) {
                    term->esc.final = ch;
                    if (term->esc.state & nss_es_csi) {
                        if (!(term->esc.state & nss_es_ignore))
                            term_escape_csi(term);
                        else term_escape_dump(term);
                        term_escape_reset(term);
                    } else
                        term->esc.state |= nss_es_string | nss_es_gotfirst;
                    return;
                }
                term->esc.state |= nss_es_gotfirst;
            } else {
                if (ch == 0x50) /* DCS */
                    term->esc.state |= nss_es_dcs;
                else if (ch == 0x5b) /* CSI */
                    term->esc.state |= nss_es_csi;
                else if (ch == 0x58 || ch == 0x5e || ch == 0x5f) /* SOS, APC, PM */
                    term->esc.state |= nss_es_string | nss_es_ignore;
                else if (ch == 0x5d) /* OSC */
                    term->esc.state |= nss_es_osc | nss_es_string;
                else if (ch == 'k') { /* old set title */
                    term->esc.param[0] = 2;
                    term->esc.param_idx = 1;
                    term->esc.state |= nss_es_osc | nss_es_string | nss_es_gotfirst;
                } else if (0x30 <= ch && ch <= 0x7e) {
                    term->esc.final = ch;
                    if (!(term->esc.state & nss_es_ignore))
                        term_escape_esc(term);
                    else term_escape_dump(term);
                    term_escape_reset(term);
                }
            }
        }
        return;
    }

    //DUMP
    //info("%c (%u)", ch, ch);

    if (term->mode & nss_tm_wrap && term->c.x + width - 1 > term->width - 1) {
        term->screen[term->c.y]->wrap_at = term->c.x;
        term_index(term, 1);
    }

    nss_cell_t *cell = &term->screen[term->c.y]->cell[term->c.x];

    if (term->mode & nss_tm_insert && term->c.x + width < term->width) {
        for (nss_cell_t *c = cell + width; c - term->screen[term->c.y]->cell < term->width; c++)
            CELL_ATTR_CLR(*c, nss_attrib_drawn);
        memmove(cell + width, cell, term->screen[term->c.y]->width - term->c.x - width);
    }

    term_adjust_wide_before(term, MIN(term->c.x, term->width - 1), term->c.y, 1, 0);
    term_adjust_wide_before(term, MIN(term->c.x, term->width - 1) + width - 1, term->c.y, 0, 1);

    term_set_cell(term, MIN(term->c.x, term->width - 1), term->c.y, ch);

    if (width > 1) {
        cell[1] = fixup_color(term->screen[term->c.y], &term->c);
        CELL_ATTR_SET(cell[0], nss_attrib_wide);
    }

    term->c.x += width;
}

static ssize_t term_write(nss_term_t *term, const uint8_t *buf, size_t len, _Bool show_ctl) {
    const uint8_t *end = buf + len, *start = buf;

    term->prev_c_x = MIN(term->c.x, term->width - 1);
    term->prev_c_y = term->c.y;

    while (start < end) {
        uint32_t ch;
        if (!(term->mode & nss_tm_utf8) || (term->mode & nss_tm_sixel))  ch = *start++;
        else if (!utf8_decode(&ch, &start, end)) break;

        if (show_ctl) {
            if (IS_C1(ch)) {
                term_putchar(term, '^');
                term_putchar(term, '[');
                ch ^= 0xc0;
            } else if ((IS_C0(ch) || IS_DEL(ch)) && ch != '\n' && ch != '\t' && ch != '\r') {
                term_putchar(term, '^');
                ch ^= 0x40;
            }
        }
        term_putchar(term, ch);
    }

    if (MIN(term->c.x, term->width - 1) != term->prev_c_x || term->c.y != term->prev_c_y) {
        CELL_ATTR_CLR(term->screen[term->c.y]->cell[MIN(term->c.x, term->width - 1)], nss_attrib_drawn);
        CELL_ATTR_CLR(term->screen[term->prev_c_y]->cell[term->prev_c_x], nss_attrib_drawn);
    }

    return start - buf;
}

ssize_t nss_term_read(nss_term_t *term) {
    if (term->fd == -1) return -1;

    if (term->mode & nss_tm_scoll_on_output && term->view) {
        term->view = NULL;
        nss_term_invalidate_screen(term);
    }

    ssize_t res;
    if ((res = read(term->fd, term->fd_buf + term->fd_buf_pos,
            NSS_FD_BUF_SZ - term->fd_buf_pos)) < 0) {
        warn("Can't read from tty");
        nss_term_hang(term);
        return -1;
    }
    term->fd_buf_pos += res;
    ssize_t written = term_write(term, term->fd_buf, term->fd_buf_pos, 0);

    term->fd_buf_pos -= written;
    if (term->fd_buf_pos > 0)
        memmove(term->fd_buf, term->fd_buf + written, term->fd_buf_pos);
    return res;
}

static void tty_write_raw(nss_term_t *term, const uint8_t *buf, size_t len) {
    ssize_t res;
    size_t lim = TTY_MAX_WRITE;
    struct pollfd pfd = {
        .events = POLLIN | POLLOUT,
        .fd = term->fd
    };
    while (len) {
        if (poll(&pfd, 1, -1) < 0 && errno != EINTR) {
            warn("Can't poll tty");
            nss_term_hang(term);
            return;
        }
        if (pfd.revents & POLLOUT) {
            if ((res = write(term->fd, buf, MIN(lim, len))) < 0) {
                warn("Can't read from tty");
                nss_term_hang(term);
                return;
            }

            if (res < (ssize_t)len) {
                if (len < lim)
                    lim = nss_term_read(term);
                len -= res;
                buf += res;
            } else break;
        }
        if (pfd.revents & POLLIN)
            lim = nss_term_read(term);
    }

}

void term_tty_write(nss_term_t *term, const uint8_t *buf, size_t len) {
    if (term->fd == -1) return;

    const uint8_t *next;

    if (!(term->mode & nss_tm_crlf))
        tty_write_raw(term, buf, len);
    else while (len) {
        if (*buf == '\r') {
            next = buf + 1;
            tty_write_raw(term, (uint8_t *)"\r\n", 2);
        } else {
            next = memchr(buf , '\r', len);
            if (!next) next = buf + len;
            tty_write_raw(term, buf, next - buf);
        }
        len -= next - buf;
        buf = next;
    }
}

static size_t term_encode_c1(nss_term_t *term, const uint8_t *in, uint8_t *out) {
    uint8_t *fmtp = out;
    for (uint8_t *it = (uint8_t *)in; *it && fmtp - out < MAX_REPORT - 1; it++) {
        if ((*it > 0x7F) && !(term->mode & nss_tm_8bit)) {
            *fmtp++ = 0x1B;
            *fmtp++ = *it ^ 0xC0;
        } else {
            if ((*it > 0x7F) && term->mode & nss_tm_utf8)
                *fmtp++ = 0xC2;
            *fmtp++ = *it;
        }
    }
    *fmtp = 0x00;
    return fmtp - out;
}

static void term_answerback(nss_term_t *term, const char *str, ...) {
    uint8_t fmt[MAX_REPORT], csi[MAX_REPORT];
    term_encode_c1(term, (const uint8_t *)str, fmt);
    va_list vl;
    va_start(vl, str);
    vsnprintf((char *)csi, sizeof(csi), (char *)fmt, vl);
    va_end(vl);

    term_tty_write(term, csi, (uint8_t *)memchr(csi, 0, MAX_REPORT) - csi);
}

void nss_term_sendkey(nss_term_t *term, const char *str) {
    if (term->mode & nss_tm_echo)
        term_write(term, (uint8_t *)str, strlen(str), 1);

    if (!(term->mode & nss_tm_dont_scroll_on_input) && term->view) {
        term->view = NULL;
        nss_term_invalidate_screen(term);
    }
    uint8_t rep[MAX_REPORT];
    size_t len = term_encode_c1(term, (const uint8_t *)str, rep);

    term_tty_write(term, rep, len);
}

void nss_term_sendbreak(nss_term_t *term) {
    if (tcsendbreak(term->fd, 0))
        warn("Can't send break");
}

void nss_term_hang(nss_term_t *term) {
    if(term->fd >= 0) {
        close(term->fd);
        term->fd = -1;
    }
    kill(term->child, SIGHUP);
}

int nss_term_fd(nss_term_t *term) {
    return term->fd;
}

static void term_resize(nss_term_t *term, int16_t width, int16_t height) {
    _Bool cur_moved = term->c.x == term->width;

    // Free extra lines, scrolling screen upwards

    if (term->height > height) {
        if (term->mode & nss_tm_altscreen)
            SWAP(nss_line_t **, term->screen, term->back_screen);

        int16_t delta = MAX(0, term->c.y - height + 1);

        if (delta) nss_term_invalidate_screen(term);

        for (int16_t i = height; i < term->height; i++) {
            if (i < height + delta)
                term_append_history(term, term->screen[i - height]);
            else
                term_free_line(term, term->screen[i]);
            term_free_line(term, term->back_screen[i]);
        }

        memmove(term->screen, term->screen + delta, (term->height - delta)* sizeof(term->screen[0]));

        if (term->mode & nss_tm_altscreen)
            SWAP(nss_line_t **, term->screen, term->back_screen);
    }

    // Resize screens

    nss_line_t **new = realloc(term->screen, height * sizeof(term->screen[0]));
    nss_line_t **new_back = realloc(term->back_screen, height * sizeof(term->back_screen[0]));
    nss_rect_t *new_rend = realloc(term->render_buffer, height * sizeof(term->render_buffer[0]));

    if (!new) die("Can't create lines");
    if (!new_back) die("Can't create lines");
    if (!new_rend) die("Can't create lines");

    term->screen = new;
    term->back_screen = new_back;
    term->render_buffer = new_rend;

    // Create new lines

    if (height > term->height) {
        for (int16_t i = term->height; i < height; i++) {
            term->screen[i] = term_create_line(term, width);
            term->back_screen[i] = term_create_line(term, width);
        }
    }

    // Resize tabs

    uint8_t *new_tabs = realloc(term->tabs, width * sizeof(*new_tabs));
    if (!new_tabs) die("Can't alloc tabs");
    term->tabs = new_tabs;

    if(width > term->width) {
        memset(new_tabs + term->width, 0, (width - term->width) * sizeof(new_tabs[0]));
        int16_t tab = term->width, tabw = nss_config_integer(nss_config_tab_width, 1, 10000);
        while (tab > 0 && !new_tabs[tab]) tab--;
        while ((tab += tabw) < width) new_tabs[tab] = 1;
    }

    // Set parameters

    int16_t minh = MIN(height, term->height);

    term->width = width;
    term->height = height;

    // Clear new regions

    nss_line_t *view = term->view;
    for (size_t i = 0; i < 2; i++) {
        for (int16_t i = 0; i < minh; i++)
            if (term->screen[i]->width < width)
                term->screen[i] = term_realloc_line(term, term->screen[i], width);
        term_swap_screen(term);
    }
    term->view = view;

    // Reset scroll region

    term->top = 0;
    term->bottom = height - 1;
    term_move_to(term, term->c.x, term->c.y);
    if (cur_moved) {
        CELL_ATTR_CLR(term->screen[term->c.y]->cell[MIN(term->c.x, term->width - 1)], nss_attrib_drawn);
        CELL_ATTR_CLR(term->screen[term->c.y]->cell[MAX(term->c.x - 1, 0)], nss_attrib_drawn);
    }

}

nss_term_t *nss_create_term(nss_window_t *win, int16_t width, int16_t height) {
    nss_term_t *term = calloc(1, sizeof(nss_term_t));
    term->palette = nss_create_palette();
    term->win = win;
    term->scrollback_limit = nss_config_integer(nss_config_history_lines, -1, 1000000);

    term->mode = nss_tm_visible;
    if (nss_config_integer(nss_config_utf8, 0, 1)) term->mode |= nss_tm_utf8;
    if (!nss_config_integer(nss_config_allow_altscreen, 0, 1)) term->mode |= nss_tm_disable_altscreen;
    if (nss_config_integer(nss_config_init_wrap, 0, 1)) term->mode |= nss_tm_wrap;
    if (!nss_config_integer(nss_config_scroll_on_input, 0, 1)) term->mode |= nss_tm_dont_scroll_on_input;
    if (nss_config_integer(nss_config_scroll_on_output, 0, 1)) term->mode |= nss_tm_scoll_on_output;

    term->c = term->back_cs = term->cs = (nss_cursor_t) {
        .cel = MKCELL(NSS_SPECIAL_FG, NSS_SPECIAL_BG, 0, ' '),
        .gl = 0, .gl_ss = 0, .gr = 2,
        .gn = {nss_cs_dec_ascii, nss_cs_british, nss_cs_british, nss_cs_british}
    };

    memset(term->tabs, 0, term->width * sizeof(term->tabs[0]));
    size_t tabw = nss_config_integer(nss_config_tab_width, 1, 10000);
    for(size_t i = tabw; i < (size_t)term->width; i += tabw)
        term->tabs[i] = 1;

    for(size_t i = 0; i < 2; i++) {
        term_cursor_mode(term, 1);
        term_erase(term, 0, 0, term->width, term->height);
        term_swap_screen(term);
    }

    clock_gettime(CLOCK_MONOTONIC, &term->lastscroll);

    term_resize(term, width, height);

    if (tty_open(term, nss_config_string(nss_config_shell, "/bin/sh"), NULL) < 0) {
        warn("Can't create tty");
        nss_free_term(term);
        return NULL;
    }

    return term;
}

void nss_term_redraw(nss_term_t *term, nss_rect_t damage, _Bool cursor) {
    if (!(term->mode & nss_tm_visible)) return;

    if (intersect_with(&damage, &(nss_rect_t) {0, 0, term->width, term->height})) {
        //Clear undefined areas
        nss_window_clear(term->win, 1, &damage);

        int16_t y0 = 0;
        nss_line_t *view = term->view;
        for (; view && y0 < damage.y; y0++, view = view->next);
        for (; view && y0 < damage.height + damage.y; y0++, view = view->next) {
            if (view->width > damage.x) {
                int16_t xs = damage.x, w = MIN(view->width - damage.x, damage.width);
                if (xs > 0 && CELL_ATTR(view->cell[xs - 1]) & nss_attrib_wide) w++, xs--;
                if (CELL_ATTR(view->cell[xs]) & nss_attrib_wide) w++;
                nss_window_draw(term->win, xs, y0, w, view->cell + xs, term->palette, view->extra);
            }
        }
        for (int16_t y = 0; y < term->height && y + y0 < damage.height + damage.y; y++) {
            nss_line_t *line = term->screen[y];
            if (line->width > damage.x) {
                int16_t xs = damage.x, w = MIN(line->width - damage.x, damage.width);
                if (xs > 0 && CELL_ATTR(line->cell[xs - 1]) & nss_attrib_wide) w++, xs--;
                if (CELL_ATTR(line->cell[xs]) & nss_attrib_wide) w++;
                nss_window_draw(term->win, xs, y0 + y, w, line->cell + xs, term->palette, line->extra);
            }
        }
        if (cursor && !(term->mode & nss_tm_hide_cursor) && !term->view && damage.x <= term->c.x && term->c.x <= damage.x + damage.width &&
                damage.y <= term->c.y && term->c.y <= damage.y + damage.height) {
            int16_t cx = MIN(term->c.x, term->width - 1);
            nss_window_draw_cursor(term->win, term->c.x, term->c.y, &term->screen[term->c.y]->cell[cx], term->palette, term->screen[term->c.y]->extra);
        }
    }
}

void nss_term_redraw_dirty(nss_term_t *term, _Bool cursor) {
    if (!(term->mode & nss_tm_visible)) return;

    size_t rbuf_clear = 0, drawn = 0;

    int16_t y0 = 0;
    nss_line_t *view = term->view;
    for (; view && y0 < term->height; y0++, view = view->next) {
        drawn += nss_window_draw(term->win, 0, y0, MIN(view->width, term->width), view->cell, term->palette, view->extra);
        if (term->width > view->width) {
            term->render_buffer[rbuf_clear++] = (nss_rect_t){ view->width, y0, term->width - view->width, 1};
            drawn += term->width - view->width;
        }
    }

    cursor &= !(CELL_ATTR(term->screen[term->c.y]->cell[MIN(term->c.x, term->width - 1)]) & nss_attrib_drawn);

    for (int16_t y = 0; y + y0 < term->height; y++) {
        nss_line_t *line = term->screen[y];
        drawn += nss_window_draw(term->win, 0, y0 + y, term->width, line->cell, term->palette, line->extra);
    }

    if (cursor && !term->view && !(term->mode & nss_tm_hide_cursor)) {
        int16_t cx = MIN(term->c.x, term->width - 1);
        nss_window_draw_cursor(term->win, term->c.x, term->c.y,
                &term->screen[term->c.y]->cell[cx], term->palette, term->screen[term->c.y]->extra);
        drawn++;
    }

    if (rbuf_clear) nss_window_clear(term->win, rbuf_clear, term->render_buffer);
    if (drawn) nss_window_update(term->win, 1, &(nss_rect_t){0, 0, term->width, term->height});
}

void nss_term_resize(nss_term_t *term, int16_t width, int16_t height) {
    term_resize(term, width, height);

    struct winsize wsz = {
        .ws_col = width,
        .ws_row = height,
        .ws_xpixel = nss_window_get(term->win, nss_wc_width),
        .ws_ypixel = nss_window_get(term->win, nss_wc_height)
    };

    if (ioctl(term->fd, TIOCSWINSZ, &wsz) < 0)
        warn("Can't change tty size");

    // Add delay to remove flickering
    clock_gettime(CLOCK_MONOTONIC, &term->lastscroll);
}

void nss_term_focus(nss_term_t *term, _Bool focused) {
    ENABLE_IF(focused, term->mode, nss_tm_focused);
    if (term->mode & nss_tm_track_focus)
        term_answerback(term, focused ? "\x9BI" : "\x9BO");
    CELL_ATTR_CLR(term->screen[term->c.y]->cell[term->c.x], nss_attrib_drawn);
}

void nss_term_visibility(nss_term_t *term, _Bool visible) {
    ENABLE_IF(visible, term->mode, nss_tm_visible);
    if (visible) nss_term_invalidate_screen(term);
}

_Bool nss_term_mouse(nss_term_t *term, int16_t x, int16_t y, nss_mouse_state_t mask, nss_mouse_event_t event, uint8_t button) {
    if (term->mode & nss_tm_mouse_x10 && button > 2) return 0;
    if (!(term->mode & nss_tm_mouse_mask)) return 0;

    if (event == nss_me_motion) {
        if (!(term->mode & (nss_tm_mouse_many | nss_tm_mouse_motion))) return 0;
        if (term->mode & nss_tm_mouse_button && term->prev_mouse_button == 3) return 0;
        if (x == term->prev_mouse_x && y == term->prev_mouse_y) return 0;
        button = term->prev_mouse_button + 32;
    } else {
        if (button > 6) button += 128 - 7;
        else if (button > 2) button += 64 - 3;
        if (event == nss_me_release) {
            if (term->mode & nss_tm_mouse_x10) return 0;
            /* Don't report wheel relese events */
            if (button == 64 || button == 65) return 0;
            if (!(term->mode & nss_tm_mouse_format_sgr)) button = 3;
        }
        term->prev_mouse_button = button;
    }

    if (!(term->mode & nss_tm_mouse_x10)) {
        if (mask & nss_ms_shift) button |= 4;
        if (mask & nss_ms_mod_1) button |= 8;
        if (mask & nss_ms_control) button |= 16;
    }


    if (term->mode & nss_tm_mouse_format_sgr) {
        term_answerback(term, "\x9B<%"PRIu8";%"PRIu16";%"PRIu16"%c",
                button, x + 1, y + 1, event == nss_me_release ? 'm' : 'M');
    } else {
        if (x >= 223 || y >= 223) return 0;
        term_answerback(term, "\x9BM%c%c%c", button + ' ', x + 1 + ' ', y + 1 + ' ');
    }

    term->prev_mouse_x = x;
    term->prev_mouse_y = y;
    return 1;
}

void nss_free_term(nss_term_t *term) {
    nss_term_hang(term);
    for (size_t i = 0; i < (size_t)term->height; i++) {
        term_free_line(term, term->screen[i]);
        term_free_line(term, term->back_screen[i]);
    }
    free(term->screen);
    free(term->back_screen);
    free(term->render_buffer);

    nss_line_t *next, *line = term->scrollback;
    while (line) {
        next = line->prev;
        term_free_line(term, line);
        line = next;
    }

    free(term->tabs);
    free(term->palette);
    free(term);
}
