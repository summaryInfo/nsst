#define _XOPEN_SOURCE
#include <assert.h>
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
    size_t width;
    size_t wrap_at;
    enum nss_line_mode {
        nss_lm_dirty = 1 << 0,
        nss_lm_blink = 1 << 1,
    } mode;
    nss_cell_t cell[];
} nss_line_t;

#define TTY_MAX_WRITE 256
#define NSS_FD_BUF_SZ 256
#define INIT_TAB_SIZE 8

#define IS_C1(c) ((c) < 0xa0 && (c) >= 0x80)
#define IS_C0(c) (((c) < 0x20) || (c) == 0x7f)
#define IS_STREND(c) (IS_C1(c) || (c) == 0x1b || (c) == 0x1a || (c) == 0x18 || (c) == 0x07)

typedef struct nss_cursor {
    int16_t x;
    int16_t y;
    nss_cell_t cel;
    // Shift state
    uint8_t gl;
    uint8_t gr;
    uint8_t gl_ss;
    enum nss_char_set {
        nss_cs_dec_ascii,
        nss_cs_dec_sup,
        nss_cs_dec_graph,
        nss_cs_british,
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
} nss_cursor_t;

struct nss_term {
    nss_line_t **screen;
    nss_line_t **back_screen;
    nss_line_t *view;
    nss_line_t *scrollback;
    nss_line_t *scrollback_top;
    int32_t scrollback_limit;
    int32_t scrollback_size;

    nss_cursor_t c;
    nss_cursor_t cs;
    nss_cursor_t back_cs;
    uint32_t prev_ch;

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
        nss_tm_force_redraw = 1 << 8,
        nss_tm_insert = 1 << 9,
        nss_tm_sixel = 1 << 10,
        nss_tm_8bit = 1 << 11,
        nss_tm_disable_altscreen = 1 << 13,
        nss_tm_track_focus = 1 << 14,
        nss_tm_hide_cursor = 1<< 15,
        nss_tm_enable_nrcs = 1 << 16,
        nss_tm_132_preserve_display = 1 << 17,
        nss_tm_scoll_on_output = 1 << 18,
        nss_tm_dont_scroll_on_input = 1 << 19,
        nss_tm_use_protected_area_semantics = 1 << 12
    } mode;

#define ESC_MAX_PARAM 16
#define ESC_MAX_INTERM 2
#define ESC_MAX_STR 256

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
    nss_palette_t pal;
    pid_t child;
    int fd;
    // Make this just 4 bytes for incomplete utf-8
    size_t fd_buf_pos;
    uint8_t fd_buf[NSS_FD_BUF_SZ];
};

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

static void exec_shell(char *cmd, char **args) {

    const struct passwd *pw;
    errno = 0;
    if (!(pw = getpwuid(getuid()))) {
        if (errno) die("getpwuid(): %s", strerror(errno));
        else die("I don't know you");
    }

    char *sh = cmd;
    if (!(sh = getenv("SHELL")))
        sh = pw->pw_shell[0] ? pw->pw_shell : cmd;

    if (args) cmd = args[0];
    else cmd = sh;

    char *def[] = {cmd, NULL};
    if (!args) args = def;

    unsetenv("COLUMNS");
    unsetenv("LINES");
    unsetenv("TERMCAP");
    setenv("LOGNAME", pw->pw_name, 1);
    setenv("USER", pw->pw_name, 1);
    setenv("SHELL", sh, 1);
    setenv("HOME", pw->pw_dir, 1);
    setenv("TERM", NSS_TERM_NAME, 1);

    signal(SIGCHLD, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGALRM, SIG_DFL);

    execvp(cmd, args);
    _exit(1);
}

int tty_open(nss_term_t *term, char *cmd, char **args) {
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

static nss_line_t *term_create_line(nss_term_t *term, size_t width) {
    nss_line_t *line = malloc(sizeof(*line) + width * sizeof(line->cell[0]));
    if (line) {
        line->width = width;
        line->mode = nss_lm_dirty;
        line->next = line->prev = NULL;
        line->wrap_at = 0;
        for (size_t i = 0; i < width; i++) {
            nss_color_ref(term->pal, term->c.cel.fg);
            nss_color_ref(term->pal, term->c.cel.bg);
            line->cell[i] = NSS_MKCELLWITH(term->c.cel, ' ');
        }
    } else warn("Can't allocate line");
    return line;
}

static void term_free_line(nss_term_t *term, nss_line_t *line) {
    for(size_t i = 0; i < line->width; i++) {
        nss_color_free(term->pal, line->cell[i].fg);
        nss_color_free(term->pal, line->cell[i].bg);
    }
    free(line);
}

void nss_term_scroll_view(nss_term_t *term, int16_t amount) {
    if (term->mode & nss_tm_altscreen) return;
    int16_t scrolled = 0, y0, yd, yr;
    if (amount > 0) {
        if (!term->view && term->scrollback) {
            term->view = term->scrollback;
            scrolled++;
        }
        if (term->view)
            while (scrolled < amount && term->view->prev)
                term->view = term->view->prev, scrolled++;
        y0 = 0, yd = scrolled, yr = 0;
    } else if (amount < 0) {
        while (scrolled < -amount && term->view)
            term->view = term->view->next, scrolled++;
        y0 = scrolled, yd = 0, yr = term->height - scrolled;
    }
    if (scrolled) {
        nss_window_shift(term->win, y0, yd, term->height - scrolled);
        nss_term_redraw(term, (nss_rect_t) {0, yr, term->width, scrolled}, 1);
        nss_window_update(term->win, 1, &(nss_rect_t){0, 0, term->width, term->height});
        nss_window_draw_commit(term->win);
    }
}

static void term_append_history(nss_term_t *term, nss_line_t *line) {
    if (term->scrollback)
        term->scrollback->next = line;
    else
        term->scrollback_top = line;
    line->prev = term->scrollback;
    line->next = NULL;
    term->scrollback = line;

    if (term->scrollback_limit >= 0 && ++term->scrollback_size > term->scrollback_limit) {
        if (term->scrollback_top == term->view) {
            term->view = term->scrollback_top->next;
            term->mode |= nss_tm_force_redraw;
        }
        if (term->scrollback_top == term->scrollback)
            term->scrollback = NULL;
        nss_line_t *next = term->scrollback_top->next;
        term_free_line(term, term->scrollback_top);
        if (next) next->prev = NULL;
        term->scrollback_top = next;
        term->scrollback_size = term->scrollback_limit;
    }
}

static void term_erase(nss_term_t *term, int16_t xs, int16_t ys, int16_t xe, int16_t ye) {
    if (ye < ys) SWAP(int16_t, ye, ys);
    if (xe < xs) SWAP(int16_t, xe, xs);

    xs = MAX(0, MIN(xs, term->width));
    xe = MAX(0, MIN(xe, term->width));
    ys = MAX(0, MIN(ys, term->height));
    ye = MAX(0, MIN(ye, term->height));

    nss_cell_t cell = term->c.cel;
    NSS_CELL_ATTRS_ZERO(cell);
    for (; ys < ye; ys++) {
        nss_line_t *line = term->screen[ys];
        line->mode |= nss_lm_dirty;
        for(int16_t i = xs; i < xe; i++) {
            nss_color_ref(term->pal, cell.fg);
            nss_color_ref(term->pal, cell.bg);
            nss_color_free(term->pal, line->cell[i].fg);
            nss_color_free(term->pal, line->cell[i].bg);
            line->cell[i] = cell;
        }
    }
}

static nss_line_t *term_realloc_line(nss_term_t *term, nss_line_t *line, size_t width) {
    nss_line_t *new = realloc(line, sizeof(*new) + width * sizeof(new->cell[0]));
    if (!new) die("Can't create lines");

    nss_cell_t cell = term->c.cel;
    NSS_CELL_ATTRS_ZERO(cell);
    new->mode |= nss_lm_dirty;

    for(size_t i = new->width; i < width; i++) {
        nss_color_ref(term->pal, cell.fg);
        nss_color_ref(term->pal, cell.bg);
        new->cell[i] = cell;
    }

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

    nss_cell_t cell = term->c.cel;
    NSS_CELL_ATTRS_ZERO(cell);
    for (; ys < ye; ys++) {
        nss_line_t *line = term->screen[ys];
        line->mode |= nss_lm_dirty;
        for(int16_t i = xs; i < xe; i++) {
            if (!(NSS_CELL_ATTRS(term->screen[ys]->cell[i]) & nss_attrib_protected)) {
                nss_color_ref(term->pal, cell.fg);
                nss_color_ref(term->pal, cell.bg);
                nss_color_free(term->pal, line->cell[i].fg);
                nss_color_free(term->pal, line->cell[i].bg);
                line->cell[i] = cell;
            }
        }
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
        line->mode |= nss_lm_dirty;
        for(int16_t i = xs; i < xe; i++) {
            if (!(NSS_CELL_ATTRS(line->cell[i]) & nss_attrib_protected))
                line->cell[i] = NSS_MKCELLWITH(line->cell[i], ' ');
        }
    }
}

static void term_adjust_wide_before(nss_term_t *term, int16_t x, int16_t y, _Bool left, _Bool right) {
    if (x < 0 || (size_t)x > term->screen[y]->width - 1) return;
    nss_cell_t *cell = &term->screen[y]->cell[x];
    if (left && NSS_CELL_ATTRS(*cell) & nss_attrib_wdummy && x > 0) {
        cell[-1] = NSS_MKCELLWITH(cell[-1], ' ');
        NSS_CELL_ATTRCLR(cell[-1], nss_attrib_wide);
    }
    if (right && NSS_CELL_ATTRS(*cell) & nss_attrib_wide
            && (size_t)x < term->screen[y]->width) {
        cell[1] = NSS_MKCELLWITH(cell[1], ' ');
        NSS_CELL_ATTRCLR(cell[1], nss_attrib_wdummy);
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
        nss_color_free(term->pal, term->cs.cel.fg);
        nss_color_free(term->pal, term->cs.cel.bg);
        term->cs = term->c;
    } else /* restore */ {
        nss_color_ref(term->pal, term->cs.cel.fg);
        nss_color_ref(term->pal, term->cs.cel.bg);
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
        if (amount > 0) {
            nss_window_shift(term->win, top + amount, top, bottom + 1 - top - amount);
            for (size_t i = 0; i < (size_t)amount; i++) term->screen[bottom - i]->mode |= nss_lm_dirty;
        } else if (amount < 0) {
            nss_window_shift(term->win, top, top - amount, bottom + 1 - top + amount);
            for (size_t i = 0; i < (size_t)-amount; i++) term->screen[top + i]->mode |= nss_lm_dirty;
        }
        clock_gettime(CLOCK_MONOTONIC, &term->lastscroll);
    } else  term->mode |= nss_tm_force_redraw;
    term->view = NULL;
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
    for (int16_t i = 0; i < n; i ++) {
        nss_color_free(term->pal, line->cell[line->width - 1 - i].fg);
        nss_color_free(term->pal, line->cell[line->width - 1 - i].bg);
        nss_color_ref(term->pal, line->cell[cx + i].fg);
        nss_color_ref(term->pal, line->cell[cx + i].bg);
    }
    memmove(line->cell + cx + n, line->cell + cx, (line->width - cx - n) * sizeof(nss_cell_t));
    term_erase(term, cx, term->c.y, cx + n, term->c.y + 1);
    term_move_to(term, term->c.x, term->c.y);
}

static void term_delete_cells(nss_term_t *term, int16_t n) {
    int16_t cx = MIN(term->c.x, term->width - 1);
    n = MAX(0, MIN(n, term->width - cx));
    nss_line_t *line = term->screen[term->c.y];
    term_adjust_wide_before(term, cx, term->c.y, 1, 0);
    term_adjust_wide_before(term, cx + n - 1, term->c.y, 0, 1);
    for (int16_t i = 0; i < n; i ++) {
        nss_color_free(term->pal, line->cell[cx + i].fg);
        nss_color_free(term->pal, line->cell[cx + i].bg);
        nss_color_ref(term->pal, line->cell[line->width - 1 - i].fg);
        nss_color_ref(term->pal, line->cell[line->width - 1 - i].bg);
    }
    memmove(line->cell + cx, line->cell + cx + n, (term->width - cx - n) * sizeof(nss_cell_t));
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
    nss_color_ref(term->pal, NSS_SPECIAL_FG);
    nss_color_ref(term->pal, NSS_SPECIAL_BG);
    nss_color_free(term->pal, term->c.cel.fg);
    nss_color_free(term->pal, term->c.cel.bg);
    term->c = (nss_cursor_t) {
        .cel = NSS_MKCELL(NSS_SPECIAL_FG, NSS_SPECIAL_BG, 0, ' '),
        .gl = 0, .gl_ss = 0, .gr = 2,
        .gn = {nss_cs_dec_ascii, nss_cs_dec_sup, nss_cs_dec_sup, nss_cs_dec_sup}
    };
    term->mode = nss_tm_wrap | nss_tm_visible | nss_tm_utf8;
    term->top = 0;
    term->bottom = term->height - 1;
    memset(term->tabs, 0, term->width * sizeof(term->tabs[0]));

    for(size_t i = INIT_TAB_SIZE; i < (size_t)term->width; i += INIT_TAB_SIZE)
        term->tabs[i] = 1;

    for(size_t i = 0; i < 2; i++) {
        term_cursor_mode(term, 1);
        term_erase(term, 0, 0, term->width, term->height);
        term_swap_screen(term);
    }

    uint32_t args[] = { 0, 0, 1, 0, 0 };
    nss_window_set(term->win, nss_wc_appkey | nss_wc_appcursor |
                   nss_wc_numlock | nss_wc_keylock | nss_wc_8bit, args);
    nss_window_set_title(term->win, NULL);
}

#define GRAPH0_BASE 0x41
#define GRAPH0_SIZE 62

static uint32_t nrcs_translate(uint8_t set, uint32_t ch, _Bool nrcs) {
    static const unsigned *trans[nss_cs_max] = {
        /* [0x23] [0x40] [0x5b 0x5c 0x5d 0x5e 0x5f 0x60] [0x7b 0x7c 0x7d 0x7e] */
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
        if (0x5f <= ch && ch <= 0x7e)
            return graph[ch - 0x5f];
    } else if (set == nss_cs_dec_sup || (!nrcs && set == nss_cs_british)) {
        return ch + 0x80;
    } else if (trans[set]){
        if (ch == 0x23) return trans[set][0];
        if (ch == 0x40) return trans[set][1];
        if (0x5b <= ch && ch <= 0x60)
            return trans[set][2 + ch - 0x5b];
        if (0x7b <= ch && ch <= 0x7e)
            return trans[set][8 + ch - 0x7b];
    }
    return ch;

    /*
     * Where these symbols did come from?
        "↑", "↓", "→", "←", "█", "▚", "☃",      // A - G
    */
}

static void term_set_cell(nss_term_t *term, int16_t x, int16_t y, nss_cell_t cel, uint32_t ch) {

    if (ch < 0x80) {
        if (term->c.gn[term->c.gl_ss] != nss_cs_dec_ascii) {
            ch = nrcs_translate(term->c.gn[term->c.gl_ss], ch, term->mode & nss_tm_enable_nrcs);
        }
    } else if (ch < 0x100) {
        if (term->c.gn[term->c.gr] != nss_cs_dec_sup)
            ch = nrcs_translate(term->c.gn[term->c.gr], ch - 0x80, term->mode & nss_tm_enable_nrcs);
    }

    nss_cell_t *cell = &term->screen[y]->cell[x];

    term->screen[y]->mode |= nss_lm_dirty;

    nss_color_free(term->pal, cell->fg);
    nss_color_free(term->pal, cell->bg);
    nss_color_ref(term->pal, cel.fg);
    nss_color_ref(term->pal, cel.bg);
    cell[0] = NSS_MKCELLWITH(cel, ch);

    if(NSS_CELL_ATTRS(cell[0]) & nss_attrib_blink)
        term->screen[y]->mode |= nss_lm_blink;

    term->c.gl_ss = term->c.gl; // Reset single shift
    term->prev_ch = ch; // For REP CSI Ps b
}

#define MAX_REPORT 256
static void term_answerback(nss_term_t *term, const char *str, ...) {
    va_list vl;
    va_start(vl, str);
    static uint8_t fmt[MAX_REPORT], csi[MAX_REPORT];
    uint8_t *fmtp = fmt;
    for (uint8_t *it = (uint8_t *)str; *it && fmtp - fmt < MAX_REPORT; it++) {
        if (IS_C1(*it) && !(term->mode & nss_tm_8bit)) {
            *fmtp++ = 0x1B;
            *fmtp++ = *it ^ 0xC0;
        } else {
            if (IS_C1(*it) && term->mode & nss_tm_utf8)
                *fmtp++ = 0xC2;
            *fmtp++ = *it;
        }
    }
    *fmtp = 0x00;
    vsnprintf((char *)csi, sizeof(csi), (char *)fmt, vl);
    va_end(vl);
    nss_term_write(term, csi, (uint8_t *)memchr(csi, 0, MAX_REPORT) - csi, 0);
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
        for (size_t i = 0; i <= term->esc.param_idx - (!!(term->esc.state & nss_es_osc)); i++) {
            snprintf(buf + pos, ESC_DUMP_MAX - pos, "%"PRIu32"%n", term->esc.param[i], &w);
            pos += w;
            if (i < term->esc.param_idx) buf[pos++] = ';';
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

static nss_cid_t term_define_color(nss_term_t *term, uint32_t *args, size_t *len) {
    nss_cid_t cid = NSS_SPECIAL_BG;
    if (*len) switch(args[0]) {
    case 2: {
        if (*len > 3) {
            if (args[1] > 255 || args[2] > 255 || args[3] > 255)
                term_escape_dump(term);
            uint32_t c = 0xff;
            c = (c << 8) + MIN(args[3], 255);
            c = (c << 8) + MIN(args[2], 255);
            c = (c << 8) + MIN(args[1], 255);
            cid = nss_color_find(term->pal, c);
            *len += 4;
        } else term_escape_dump(term);
        break;
    }
    case 5:
        if (*len > 1) {
            cid = args[1];
            *len += 2;
        } else term_escape_dump(term);
        break;
    default:
        term_escape_dump(term);
    } else term_escape_dump(term);
    return cid;
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

static nss_color_t term_color_parse(nss_term_t *term) {
    size_t n = term->esc.str_idx;
    uint64_t val = 0;
    if (term->esc.str[0] != '#') return 0;
    for (size_t i = 1; i < n; i++) {
        if (term->esc.str[i] - '0' < 10)
            val = (val << 4) + term->esc.str[i] - '0';
        else if (term->esc.str[i] - 'A' < 6)
            val = (val << 4) + 10 + term->esc.str[i] - 'A';
        else if (term->esc.str[i] - 'a' < 6)
            val = (val << 4) + 10 + term->esc.str[i] - 'a';
        else return 0;
    }
    nss_color_t col = 0xFF;
    switch (n) {
    case 4:
        for (size_t i = 0; i < 3; i++) {
            col = (col << 8) | ((val & 0xF) << 4);
            val >>= 4;
        }
        break;
    case 7:
        col = val | 0xFF000000;
        break;
    case 10:
        for (size_t i = 0; i < 3; i++) {
            col = (col << 8) | (val & 0xFF);
            val >>= 12;
        }
    case 13:
        for (size_t i = 0; i < 3; i++) {
            col = (col << 8) | (val & 0xFF);
            val >>= 16;
        }
    }
    return col;
}

static void term_escape_osc(nss_term_t *term) {
    nss_color_t col = term_color_parse(term);
    switch(term->esc.param[0]) {
        uint32_t args[2];
    case 0: /* Change window icon name and title */
    case 1: /* Change window icon name */
    case 2: /* Change window title */
        nss_window_set_title(term->win, (char *)term->esc.str);
        break;
    case 4: /* Set color */
        if(col && term->esc.param_idx > 1 && term->esc.param[1] < NSS_PALETTE_SIZE - NSS_SPECIAL_COLORS) {
            nss_color_set(term->pal, term->esc.param[1], col);
        } else term_escape_dump(term);
        break;
    case 5: /* Set special color */
    case 6: /* Enable/disable special color */
        term_escape_dump(term);
        break;
    case 10: /* Set VT100 foreground color */
        if (col) {
            args[0] = NSS_SPECIAL_FG;
            nss_color_set(term->pal, NSS_SPECIAL_FG, col);
            nss_window_set(term->win, nss_wc_foreground, args);
        } else term_escape_dump(term);
        break;
    case 11: /* Set VT100 background color */
        if (col) {
            args[0] = NSS_SPECIAL_BG;
            args[1] = NSS_SPECIAL_CURSOR_BG;
            nss_color_t def = nss_color_get(term->pal, NSS_SPECIAL_BG);
            col = (col & 0x00FFFFFF) | (0xFF000000 & def); // Keep alpha
            nss_color_set(term->pal, NSS_SPECIAL_BG, col);
            nss_color_set(term->pal, NSS_SPECIAL_CURSOR_BG, col);
            nss_window_set(term->win, nss_wc_background | nss_wc_cursor_background, args);
        } else term_escape_dump(term);
        break;
    case 12: /* Set Cursor color */
        if (col) {
            args[0] = term_color_parse(term);
            nss_color_set(term->pal, NSS_SPECIAL_CURSOR_FG, args[0]);
            nss_window_set(term->win, nss_wc_background, args);
        } else term_escape_dump(term);
        break;
    case 13: /* Set Mouse foreground color */
    case 14: /* Set Mouse background color */
    case 17: /* Set Highlight background color */
    case 19: /* Set Highlight foreground color */
    case 50: /* Set Font */
    case 52: /* Manipulate selecion data */
        term_escape_dump(term);
        break;
    case 104: /* Reset color */
        if(term->esc.param_idx > 1 && term->esc.param[1] < NSS_PALETTE_SIZE - NSS_SPECIAL_COLORS)
            nss_color_set(term->pal, term->esc.param[1], nss_color_get(NSS_DEFAULT_PALETTE, term->esc.param[1]));
        else term_escape_dump(term);
        break;
    case 105: /* Reset special color */
    case 106: /* Enable/disable special color */
        term_escape_dump(term);
        break;
    case 110: /*Reset  VT100 foreground color */
        args[0] = NSS_SPECIAL_FG;
        nss_color_set(term->pal, NSS_SPECIAL_FG, nss_color_get(NSS_DEFAULT_PALETTE, NSS_SPECIAL_FG));
        nss_window_set(term->win, nss_wc_background, args);
        break;
    case 111: /*Reset  VT100 background color */
        args[0] = NSS_SPECIAL_BG;
        args[1] = NSS_SPECIAL_CURSOR_BG;
        nss_color_set(term->pal, NSS_SPECIAL_BG, nss_color_get(NSS_DEFAULT_PALETTE, NSS_SPECIAL_BG));
        nss_color_set(term->pal, NSS_SPECIAL_CURSOR_BG, nss_color_get(NSS_DEFAULT_PALETTE, NSS_SPECIAL_CURSOR_BG));
        nss_window_set(term->win, nss_wc_background | nss_wc_cursor_background, args);
        break;
    case 112: /*Reset  Cursor color */
        args[0] = NSS_SPECIAL_CURSOR_BG;
        nss_color_set(term->pal, NSS_SPECIAL_CURSOR_FG, nss_color_get(NSS_DEFAULT_PALETTE, NSS_SPECIAL_CURSOR_FG));
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

static void term_escape_sgr(nss_term_t *term) {

#define SET(x) NSS_CELL_ATTRSET(term->c.cel, (x))
#define CLR(x) NSS_CELL_ATTRCLR(term->c.cel, (x))

    size_t len;
    for(uint32_t i = 0; i < term->esc.param_idx + 1; i++) {
        uint32_t p = term->esc.param[i];
        switch(p) {
        case 0:
            CLR(nss_attrib_blink | nss_attrib_bold | nss_attrib_faint |
                    nss_attrib_inverse | nss_attrib_invisible | nss_attrib_italic |
                    nss_attrib_underlined | nss_attrib_strikethrough);
            nss_color_ref(term->pal, NSS_SPECIAL_BG);
            nss_color_ref(term->pal, NSS_SPECIAL_FG);
            nss_color_free(term->pal, term->c.cel.bg);
            nss_color_free(term->pal, term->c.cel.fg);
            term->c.cel.bg = NSS_SPECIAL_BG;
            term->c.cel.fg = NSS_SPECIAL_FG;
            break;
        case 1:
            SET(nss_attrib_bold);
            break;
        case 2:
            SET(nss_attrib_faint);
            break;
        case 3:
            SET(nss_attrib_italic);
            break;
        case 4:
            SET(nss_attrib_underlined);
            break;
        case 5:
        case 6:
            SET(nss_attrib_blink);
            break;
        case 7:
            SET(nss_attrib_inverse);
            break;
        case 8:
            SET(nss_attrib_invisible);
            break;
        case 9:
            SET(nss_attrib_strikethrough);
            break;
        case 21:
            /* actually double underlind */
            SET(nss_attrib_underlined);
            break;
        case 22:
            CLR(nss_attrib_bold | nss_attrib_faint);
            break;
        case 23:
            CLR(nss_attrib_italic);
            break;
        case 24:
            CLR(nss_attrib_underlined);
            break;
        case 25:
        case 26:
            CLR(nss_attrib_blink);
            break;
        case 27:
            CLR(nss_attrib_inverse);
            break;
        case 28:
            CLR(nss_attrib_invisible);
            break;
        case 29:
            CLR(nss_attrib_strikethrough);
            break;
        case 38:
            len = term->esc.param_idx - i;
            nss_color_free(term->pal, term->c.cel.fg);
            term->c.cel.fg = term_define_color(term, &term->esc.param[i + 1], &len);
            i += len - term->esc.param_idx + i;
            break;
        case 39:
            nss_color_ref(term->pal, NSS_SPECIAL_BG);
            nss_color_free(term->pal, term->c.cel.fg);
            term->c.cel.fg = NSS_SPECIAL_FG;
            break;
        case 48:
            len = term->esc.param_idx - i;
            nss_color_free(term->pal, term->c.cel.bg);
            term->c.cel.bg = term_define_color(term, &term->esc.param[i + 1], &len);
            i += len - term->esc.param_idx + i;
            break;
        case 49:
            nss_color_ref(term->pal, NSS_SPECIAL_BG);
            nss_color_free(term->pal, term->c.cel.bg);
            term->c.cel.bg = NSS_SPECIAL_BG;
            break;
        default:
            if (30 <= p && p <= 37) {
                nss_color_free(term->pal, term->c.cel.fg);
                term->c.cel.fg = p - 30;
                nss_color_ref(term->pal, term->c.cel.fg);
            } else if (40 <= p && p <= 47) {
                nss_color_free(term->pal, term->c.cel.bg);
                term->c.cel.bg = p - 40;
                nss_color_ref(term->pal, term->c.cel.bg);
            } else if (90 <= p && p <= 97) {
                nss_color_free(term->pal, term->c.cel.fg);
                term->c.cel.fg = 8 + p - 90;
                nss_color_ref(term->pal, term->c.cel.fg);
            } else if (100 <= p && p <= 107) {
                nss_color_free(term->pal, term->c.cel.bg);
                term->c.cel.bg = 8 + p - 100;
                nss_color_ref(term->pal, term->c.cel.bg);
            } else term_escape_dump(term);
        }
    }

#undef SET
#undef CLR
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
                if (!(term->mode & nss_tm_132_preserve_display)) {
                    term_erase(term, 0, 0, term->width, term->height);
                    term_move_to(term, 0, 0);
                }
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
                if (set) term->mode |= nss_tm_wrap;
                else term->mode &= ~nss_tm_wrap;
                break;
            case 8: /* DECARM */
                // IGNORE
                break;
            case 9: /* X10 Mouse tracking */
                term_escape_dump(term);
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
                if (!set) term->mode |= nss_tm_hide_cursor;
                else term->mode &= ~nss_tm_hide_cursor;
                break;
            case 42: /* DECNRCM */
                if (set) term->mode |= nss_tm_enable_nrcs;
                else term->mode &= ~nss_tm_enable_nrcs;
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
            case 67: /* DECBKM */ // TODO DECUDK
                term_escape_dump(term);
                break;
            case 69: /* DECLRMM */ //TODO DECBI/DECFI
                term_escape_dump(term);
                break;
            case 80: /* DECSDM */ //TODO SIXEL
                term_escape_dump(term);
                break;
            case 95: /* DECNCSM */
                if (set) term->mode |= nss_tm_132_preserve_display;
                else term->mode &= ~nss_tm_132_preserve_display;
                break;
            case 1000: /* X11 Mouse tracking */
                term_escape_dump(term);
                break;
            case 1001: /* Highlight mouse tracking */
                // IGNORE
                break;
            case 1002: /* Cell motion mouse tracking */
                term_escape_dump(term);
                break;
            case 1003: /* All motion mouse tracking */
                term_escape_dump(term);
                break;
            case 1004: /* Focus in/out events */
                if (set) term->mode |= nss_tm_track_focus;
                else  term->mode &= ~nss_tm_track_focus;
                break;
            case 1005: /* UTF-8 mouse tracking */
                // IGNORE
                break;
            case 1006: /* SGR mouse tracking */
                term_escape_dump(term);
                break;
            case 1010: /* Scroll to bottom on output */
                if (set) term->mode |= nss_tm_scoll_on_output;
                else  term->mode &= ~nss_tm_scoll_on_output;
                break;
            case 1011: /* Scroll to bottom on keypress */
                if (!set) term->mode |= nss_tm_dont_scroll_on_input;
                else  term->mode &= ~nss_tm_dont_scroll_on_input;
                break;
            case 1015: /* Urxvt mouse tracking */
                // IGNORE
                break;
            case 1034: /* Interpret meta */
                nss_window_set(term->win, nss_wc_8bit, &arg);
                break;
            case 1046: /* Allow altscreen */
                if (!set) term->mode |= nss_tm_disable_altscreen;
                else  term->mode &= ~nss_tm_disable_altscreen;
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
                if (set) term->mode |= nss_tm_insert;
                else  term->mode &= ~nss_tm_insert;
                break;
            case 12: /* SRM */
                if (set) term->mode |= nss_tm_echo;
                else  term->mode &= ~nss_tm_echo;
                break;
            case 20: /* LNM */
                if (set) term->mode |= nss_tm_crlf;
                else  term->mode &= ~nss_tm_crlf;
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
                    uint32_t arg;
                case 2:
                    arg = 1;
                    nss_window_set(term->win, nss_wc_8bit, &arg);
                    term->mode |= nss_tm_8bit;
                    break;
                case 1:
                    arg = 0;
                    nss_window_set(term->win, nss_wc_8bit, &arg);
                    term->mode &= ~nss_tm_8bit;
                    break;
                default:
                    term_escape_dump(term);
                }
                term_reset(term, 1);
                break;
            case 'q': /* DECSCA */
                switch(PARAM(0, 2)) {
                case 1:
                    NSS_CELL_ATTRSET(term->c.cel, nss_attrib_protected);
                    break;
                case 2:
                    NSS_CELL_ATTRCLR(term->c.cel, nss_attrib_protected);
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
            NSS_CELL_ATTRSET(term->c.cel, nss_attrib_protected);
            term->mode |= nss_tm_use_protected_area_semantics;
            break;
        case 'W': /* EPA */
            NSS_CELL_ATTRCLR(term->c.cel, nss_attrib_protected);
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
                uint32_t arg;
            case 'F': /* S7C1T */
                arg = 0;
                nss_window_set(term->win, nss_wc_8bit, &arg);
                term->mode &= ~nss_tm_8bit;
                break;
            case 'G': /* S8C1T */
                arg = 1;
                nss_window_set(term->win, nss_wc_8bit, &arg);
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
                        term_set_cell(term, j, i, term->c.cel, 'E');
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
        case '/': /* G3D6 */
            term_escape_dump(term);
            break;
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
    //if (IS_C0(ch)) {
    //    if (ch != 0x1B) warn("^%c", ch ^ 0x40);
    //} else warn("^[%c", ch ^ 0xC0);

    switch (ch) {
    case 0x00: /* NUL (IGNORE) */
        return;
    case 0x05: /* ENQ */
        term_escape_da(term, 0);
        break;
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
    case 0x11: /* XON (IGNORE) */
    case 0x13: /* XOFF (IGNORE) */
        return;
    case 0x18: /* CAN */
        break;
    case 0x1a: /* SUB */
        term_set_cell(term, MIN(term->c.x, term->width - 1),
                      term->c.y, term->c.cel, '?');
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
    case 0x7f:   /* DEL (IGNORE) */
        return;
    case 0x80:   /* PAD */
    case 0x81:   /* HOP */
    case 0x82:   /* BPH */
    case 0x83:   /* NBH */
        warn("Unknown control character ^%"PRIx32, ch ^ 0xC0);
        break;
    case 0x84:   /* IND - Index */
        term_index(term, 0);
        break;
    case 0x85:   /* NEL -- Next line */
        term_index(term, 1);
        break;
    case 0x86:   /* SSA */
    case 0x87:   /* ESA */
        warn("Unknown control character ^%"PRIx32, ch ^ 0xC0);
        break;
    case 0x88:   /* HTS -- Horizontal tab stop */
        term->tabs[MIN(term->c.x, term->width - 1)] = 1;
        break;
    case 0x89:   /* HTJ */
    case 0x8a:   /* VTS */
    case 0x8b:   /* PLD */
    case 0x8c:   /* PLU */
        warn("Unknown control character ^%"PRIx32, ch ^ 0xC0);
        break;
    case 0x8d:   /* RI - Reverse Index */
        term_rindex(term, 0);
        break;
    case 0x8e:   /* SS2 - Single Shift 2 */
        term->c.gl_ss = 2;
        break;
    case 0x8f:   /* SS3 - Single Shift 3 */
        term->c.gl_ss = 3;
        break;
    case 0x91:   /* PU1 */
    case 0x92:   /* PU2 */
    case 0x93:   /* STS */
    case 0x94:   /* CCH */
    case 0x95:   /* MW */
        warn("Unknown control character ^%"PRIx32, ch ^ 0xC0);
        break;
    case 0x96:   /* SPA - Start of Protected Area */
        NSS_CELL_ATTRSET(term->c.cel, nss_attrib_protected);
        term->mode |= nss_tm_use_protected_area_semantics;
        break;
    case 0x97:   /* EPA - End of Protected Area */
        NSS_CELL_ATTRCLR(term->c.cel, nss_attrib_protected);
        term->mode |= nss_tm_use_protected_area_semantics;
        break;
    case 0x98:   /* SOS - Start Of String */
        term_escape_reset(term);
        term->esc.state = nss_es_ignore | nss_es_string;
        return;
    case 0x99:   /* SGCI */
        warn("Unknown control character ^%"PRIx32, ch ^ 0xC0);
        break;
    case 0x9a:   /* DECID -- Identify Terminal */
        term_escape_da(term, 0);
        break;
    case 0x9b:   /* CSI - Control Sequence Introducer */
        term_escape_reset(term);
        term->esc.state = nss_es_csi | nss_es_escape;
        break;
    case 0x9c:   /* ST - String terminator */
        if (term->esc.state & nss_es_string) {
            if (term->esc.state & nss_es_ignore)
                /* do nothing */;
            else if (term->esc.state & nss_es_dcs)
                term_escape_dcs(term);
            else if (term->esc.state & nss_es_osc)
                term_escape_osc(term);
        }
        break;
    case 0x90:   /* DCS -- Device Control String */
        term_escape_reset(term);
        term->esc.state = nss_es_dcs | nss_es_escape;
        return;
    case 0x9d:   /* OSC -- Operating System Command */
        term_escape_reset(term);
        term->esc.state = nss_es_osc | nss_es_string;
        return;
    case 0x9e:   /* PM -- Privacy Message */
    case 0x9f:   /* APC -- Application Program Command */
        term_escape_reset(term);
        term->esc.state = nss_es_string | nss_es_ignore;
        return;
    }
    term_escape_reset(term);
}

static void term_putchar(nss_term_t *term, uint32_t ch) {
    int16_t width = wcwidth(ch);
    uint8_t buf[UTF8_MAX_LEN + 1];

    if (width < 0 && !(IS_C0(ch) || IS_C1(ch)))
        ch = UTF_INVAL, width =1;

    size_t char_len = utf8_encode(ch, buf, buf + UTF8_MAX_LEN);
    buf[char_len] = '\0';

    //info("UTF %"PRIx32" '%s'", ch, buf);

    if (term->esc.state & nss_es_string && !IS_STREND(ch)) {
        if (term->esc.state & nss_es_ignore) return;
        if (term->esc.state & nss_es_dcs) {
            if (ch == 0x7f) return;
        } else if (term->esc.state & nss_es_osc) {
            if (IS_C0(ch) && ch != 0x7f) return;
            if (!(term->esc.state & nss_es_gotfirst)) {
                if ('0' <= ch && ch <= '9') {
                    term->esc.param[term->esc.param_idx] *= 10;
                    term->esc.param[term->esc.param_idx] += ch - '0';
                    return;
                } else if (ch == ';') {
                    term->esc.param[++term->esc.param_idx] = 0;
                    return;
                } else {
                    term->esc.state |= nss_es_gotfirst;
                    if (ch == 'I') {
                        term->esc.param[term->esc.param_idx++] = 1;
                        return;
                    } else if (ch == 'l' || ch == 'L') {
                        term->esc.param[term->esc.param_idx++] = 2;
                        return;
                    }
                }
            }
        }
        if (term->esc.str_idx + char_len > ESC_MAX_STR) return;
        memcpy(term->esc.str + term->esc.str_idx, buf, char_len + 1);
        term->esc.str_idx += char_len;
        return;
    } else if (IS_C0(ch) || IS_C1(ch)) {
        if (!IS_STREND(ch) && (term->esc.state & nss_es_dcs)) return;
        term_escape_control(term, ch);
        return;
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
        for(size_t i = 0; i < (size_t)width; i ++) {
            nss_color_free(term->pal, cell[term->screen[term->c.y]->width - 1 - i].fg);
            nss_color_free(term->pal, cell[term->screen[term->c.y]->width - 1 - i].bg);
            nss_color_ref(term->pal, cell[i].fg);
            nss_color_ref(term->pal, cell[i].bg);
        }
        memmove(cell + width, cell, term->screen[term->c.y]->width - term->c.x - width);
    }

    term_adjust_wide_before(term, MIN(term->c.x, term->width - 1), term->c.y, 1, 0);
    term_adjust_wide_before(term, MIN(term->c.x, term->width - 1) + width - 1, term->c.y, 0, 1);

    term_set_cell(term, MIN(term->c.x, term->width - 1), term->c.y, term->c.cel, ch);

    if (width > 1) {
        nss_color_ref(term->pal, term->c.cel.fg);
        nss_color_ref(term->pal, term->c.cel.bg);
        nss_color_free(term->pal, cell[1].fg);
        nss_color_free(term->pal, cell[1].bg);
        cell[1] = NSS_MKCELLWITH(term->c.cel, ' ');
        NSS_CELL_ATTRSET(cell[0], nss_attrib_wide);
        NSS_CELL_ATTRSET(cell[1], nss_attrib_wdummy);
    }

    term->c.x += width;
}

static ssize_t term_write(nss_term_t *term, const uint8_t *buf, size_t len, _Bool show_ctl) {
    const uint8_t *end = buf + len, *start = buf;

    term->screen[term->c.y]->mode |= nss_lm_dirty;

    while (start < end) {
        uint32_t ch;
        if (!(term->mode & nss_tm_utf8) || (term->mode & nss_tm_sixel))  ch = *start++;
        else if (!utf8_decode(&ch, &start, end)) break;

        if (show_ctl) {
            if (IS_C1(ch)) {
                term_putchar(term, '^');
                term_putchar(term, '[');
                ch ^= 0xc0;
            } else if (IS_C0(ch) && ch != '\n' && ch != '\t' && ch != '\r') {
                term_putchar(term, '^');
                ch ^= 0x40;
            }
        }
        term_putchar(term, ch);
    }

    term->screen[term->c.y]->mode |= nss_lm_dirty;

    return start - buf;
}

ssize_t nss_term_read(nss_term_t *term) {
    if (term->fd == -1) return -1;

    if (term->mode & nss_tm_scoll_on_output && term->view) {
        term->mode |= nss_tm_force_redraw;
        term->view = NULL;
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

void nss_term_write(nss_term_t *term, const uint8_t *buf, size_t len, _Bool do_echo) {
    if (term->fd == -1) return;

    if (!(term->mode | nss_tm_dont_scroll_on_input) && term->view && do_echo) {
        term->mode |= nss_tm_force_redraw;
        term->view = NULL;
    }

    const uint8_t *next;

    if (do_echo && term->mode & nss_tm_echo)
        term_write(term, buf, len, 1);

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

        if (delta) term->mode |= nss_tm_force_redraw;

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

    if (!new) die("Can't create lines");
    if (!new_back) die("Can't create lines");

    term->screen = new;
    term->back_screen = new_back;

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
        int16_t tab = term->width;
        while (tab > 0 && !new_tabs[tab]) tab--;
        while ((tab += INIT_TAB_SIZE) < width) new_tabs[tab] = 1;
    }

    // Set parameters

    size_t minh = MIN(height, term->height);

    term->width = width;
    term->height = height;

    // Clear new regions

    nss_line_t *view = term->view;
    for (size_t i = 0; i < 2; i++) {
        for (size_t i = 0; i < minh; i++)
            if (term->screen[i]->width < (size_t)width)
                term->screen[i] = term_realloc_line(term, term->screen[i], width);
        term_swap_screen(term);
    }
    term->view = view;

    // Reset scroll region

    term->top = 0;
    term->bottom = height - 1;
    term_move_to(term, term->c.x, term->c.y);
    if (cur_moved)
        term->screen[term->c.y]->mode |= nss_lm_dirty;
}

nss_term_t *nss_create_term(nss_window_t *win, nss_palette_t pal, int16_t width, int16_t height) {
    nss_term_t *term = calloc(1, sizeof(nss_term_t));
    term->win = win;
    term->pal = pal;
    term->scrollback_limit = -1;
    clock_gettime(CLOCK_MONOTONIC, &term->lastscroll);

    term_resize(term, width, height);
    term_reset(term, 0);

    // TODO Config
    if (tty_open(term, "./testcmd", NULL) < 0) {
        warn("Can't create tty");
        nss_free_term(term);
        return NULL;
    }

    return term;
}

void nss_term_redraw(nss_term_t *term, nss_rect_t damage, _Bool cursor) {
    if (intersect_with(&damage, &(nss_rect_t) {0, 0, term->width, term->height})) {
        //Clear undefined areas
        nss_window_clear(term->win, 1, &damage);

        int16_t y0 = 0;
        nss_line_t *view = term->view;
        for (; view && y0 < damage.y; y0++, view = view->next);
        for (; view && y0 < damage.height + damage.y; y0++, view = view->next) {
            if (view->width > (size_t)damage.x) {
                int16_t xs = damage.x, w = MIN(view->width - damage.x, damage.width);
                if (NSS_CELL_ATTRS(view->cell[xs]) & nss_attrib_wdummy) w++, xs--;
                if (NSS_CELL_ATTRS(view->cell[xs]) & nss_attrib_wide) w++;
                nss_window_draw(term->win, xs, y0, w, view->cell + xs);
            }
        }
        for (int16_t y = 0; y < term->height && y + y0 < damage.height + damage.y; y++) {
            if (term->screen[y]->width > (size_t)damage.x) {
                int16_t xs = damage.x, w = MIN(term->screen[y]->width - damage.x, damage.width);
                if (NSS_CELL_ATTRS(term->screen[y]->cell[xs]) & nss_attrib_wdummy) w++, xs--;
                if (NSS_CELL_ATTRS(term->screen[y]->cell[xs]) & nss_attrib_wide) w++;
                nss_window_draw(term->win, xs, y0 + y, w, term->screen[y]->cell + xs);
            }
        }
        if (cursor && !(term->mode & nss_tm_hide_cursor) && !term->view && damage.x <= term->c.x && term->c.x <= damage.x + damage.width &&
                damage.y <= term->c.y && term->c.y <= damage.y + damage.height) {
            int16_t cx = MIN(term->c.x, term->width - 1);
            nss_window_draw_cursor(term->win, term->c.x, term->c.y, &term->screen[term->c.y]->cell[cx]);
        }
    }
}

void nss_term_redraw_dirty(nss_term_t *term, _Bool cursor) {

    int16_t y0 = 0;
    nss_line_t *view = term->view;
    for (; view && y0 < term->height; y0++, view = view->next) {
        if (view->mode & (nss_lm_dirty | nss_lm_blink))
            nss_window_draw(term->win, 0, y0, term->width, view->cell);
        view->mode &= ~nss_lm_dirty;
    }

    for (int16_t y = 0; y + y0 < term->height; y++) {
        if (term->screen[y]->mode & (nss_lm_dirty | nss_lm_blink) || (term->mode & nss_tm_force_redraw))
            nss_window_draw(term->win, 0, y0 + y, term->width, term->screen[y]->cell);
        term->screen[y]->mode &= ~nss_lm_dirty;
    }

    term->mode &= ~nss_tm_force_redraw;

    if (cursor && !term->view && !(term->mode & nss_tm_hide_cursor)) {
        int16_t cx = MIN(term->c.x, term->width - 1);
        nss_window_draw_cursor(term->win, term->c.x, term->c.y, &term->screen[term->c.y]->cell[cx]);
    }

    nss_window_update(term->win, 1, &(nss_rect_t){0, 0, term->width, term->height});
}

void nss_term_resize(nss_term_t *term, int16_t width, int16_t height) {
    info("Resize: w=%"PRId16" h=%"PRId16, width, height);

    term_resize(term, width, height);

    struct winsize wsz = {
        .ws_col = width,
        .ws_row = height,
        .ws_xpixel = nss_window_get(term->win, nss_wc_width),
        .ws_ypixel = nss_window_get(term->win, nss_wc_height)
    };

    if (ioctl(term->fd, TIOCSWINSZ, &wsz) < 0)
        warn("Can't change tty size");
}

void nss_term_focus(nss_term_t *term, _Bool focused) {
    if (focused) term->mode |= nss_tm_focused;
    else term->mode &= ~nss_tm_focused;
    if (term->mode & nss_tm_track_focus)
        term_answerback(term, focused ? "\x9BI" : "\x9BO");
    nss_rect_t damage = {MIN(term->c.x, term->width - 1), term->c.y, 1, 1};
    nss_term_redraw(term, damage, 1);
    nss_window_update(term->win, 1, &damage);
    nss_window_draw_commit(term->win);
}

void nss_term_visibility(nss_term_t *term, _Bool visible) {
    info("Visibliltiy: %d", visible);
    if (visible) {
        term->mode |= nss_tm_visible;
        nss_rect_t damage = {0, 0, term->width, term->height};
        nss_term_redraw(term, damage, 1);
        nss_window_update(term->win, 1, &damage);
    } else term->mode &= ~nss_tm_visible;
}

void nss_free_term(nss_term_t *term) {
    nss_term_hang(term);
    for (size_t i = 0; i < (size_t)term->height; i++) {
        term_free_line(term, term->screen[i]);
        term_free_line(term, term->back_screen[i]);
    }
    free(term->screen);
    free(term->back_screen);

    nss_line_t *next, *line = term->scrollback;
    while (line) {
        next = line->prev;
        term_free_line(term, line);
        line = next;
    }

    free(term->tabs);
    free(term);
}
