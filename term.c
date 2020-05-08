/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#include "feature.h"

#define _XOPEN_SOURCE 700

#include "config.h"
#include "input.h"
#include "nrcs.h"
#include "term.h"
#include "window.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

//For openpty() funcion
#if   defined(__linux)
#   include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
#   include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#   include <libutil.h>
#endif

#define TTY_MAX_WRITE 256
#define NSS_FD_BUF_SZ 512
#define ESC_MAX_PARAM 32
#define ESC_MAX_STR 512
#define ESC_DUMP_MAX 1024
#define MAX_REPORT 256
#define SEL_INIT_SIZE 32

#define CSI "\x9B"
#define OSC "\x9D"
#define DCS "\x90"
#define ESC "\x1B"
#define ST "\x9C"

#define IS_C1(c) ((c) < 0xa0 && (c) >= 0x80)
#define IS_C0(c) ((c) < 0x20)
#define IS_DEL(c) ((c) == 0x7f)
#define IS_STREND(c) (IS_C1(c) || (c) == 0x1b || (c) == 0x1a || (c) == 0x18 || (c) == 0x07)
#define ENABLE_IF(c, m, f) { if (c) { (m) |= (f); } else { (m) &= ~(f); }}

#define MAX_EXTRA_PALETTE (0x10000 - NSS_PALETTE_SIZE)
#define CAPS_INC_STEP(sz) MIN(MAX_EXTRA_PALETTE, (sz) ? 8*(sz)/5 : 4)
#define PARAM(i, d) (term->esc.param[i] > 0 ? (param_t)term->esc.param[i] : (d))
#define CHK_VT(v) { if (term->vt_level < (v)) break; }

#define C(c) ((c) & 0x3F)
#define P(p) ((p) ? ((((p) & 3) + 1) << 6) : 0)
#define E(c) ((c) & 0x7F)
#define I0(i) ((i) ? (((i) & 0xF) + 1) << 9 : 0)
#define I1(i) (I0(i) << 5)
#define C_MASK (0x3F)
#define P_MASK (7 << 6)
#define E_MASK (0x7F)
#define I0_MASK (0x1F << 9)
#define I1_MASK (0x1F << 14)

static void term_answerback(nss_term_t *term, const char *str, ...);
static void term_scroll_selection(nss_term_t *term, nss_coord_t amount);
static void term_change_selection(nss_term_t *term, uint8_t state, nss_coord_t x, nss_color_t y, _Bool rectangular);

typedef struct nss_cursor {
    nss_coord_t x;
    nss_coord_t y;
    nss_cell_t cel;
    nss_color_t fg;
    nss_color_t bg;
    // Shift state
    uint8_t gl;
    uint8_t gr;
    uint8_t gl_ss;
    enum nss_char_set gn[4];

    _Bool origin;
} nss_cursor_t;

typedef struct nss_selected {
    nss_coord_t x0;
    ssize_t y0;
    nss_coord_t x1;
    ssize_t y1;
    _Bool rect;
} nss_selected_t;

typedef struct nss_visual_selection {
    nss_selected_t r;
    nss_selected_t n;

    enum {
        nss_ssnap_none,
        nss_ssnap_word,
        nss_ssnap_line,
    } snap;
    enum {
        nss_sstate_none,
        nss_sstate_pressed = nss_me_press + 1,
        nss_sstate_released = nss_me_release + 1,
        nss_sstate_progress = nss_me_motion + 1,
    } state;

    struct timespec click0;
    struct timespec click1;
    nss_clipboard_target_t targ;
} nss_visual_selection_t;


struct nss_term {
    nss_line_t **screen;
    nss_line_t **back_screen;
    nss_line_t *view;
    nss_line_t *scrollback;
    nss_line_t *scrollback_top;
    ssize_t scrollback_limit;
    ssize_t scrollback_size;
    ssize_t scrollback_pos;

    nss_cursor_t c;
    nss_cursor_t cs;
    nss_cursor_t back_cs;
    nss_cursor_t vt52c;

    /* Last written character
     * Used for REP */
    nss_char_t prev_ch;

    nss_coord_t prev_mouse_x;
    nss_coord_t prev_mouse_y;
    uint8_t prev_mouse_button;

    nss_visual_selection_t vsel;

    /* Previous cursor state
     * Used for effective cursor invalidation */
    nss_coord_t prev_c_x;
    nss_coord_t prev_c_y;
    _Bool prev_c_hidden;
    _Bool prev_c_view_changed;

    /* OSC 52 character description
     * of selection being pasted from */
    uint8_t paste_from;

    nss_coord_t width;
    nss_coord_t height;
    nss_coord_t top;
    nss_coord_t bottom;
    _Bool *tabs;

    enum nss_term_mode {
        nss_tm_echo = 1 << 0,
        nss_tm_crlf = 1 << 1,
        nss_tm_132cols = 1 << 2,
        nss_tm_wrap = 1 << 3,
        nss_tm_visible = 1 << 4,
        nss_tm_focused = 1 << 5,
        nss_tm_altscreen = 1 << 6,
        nss_tm_utf8 = 1 << 7,
        nss_tm_reverse_video = 1 << 8,
        nss_tm_insert = 1 << 9,
        nss_tm_sixel = 1 << 10,
        nss_tm_8bit = 1 << 11,
        nss_tm_protected = 1 << 12,
        nss_tm_disable_altscreen = 1 << 13,
        nss_tm_track_focus = 1 << 14,
        nss_tm_hide_cursor = 1<< 15,
        nss_tm_enable_nrcs = 1 << 16,
        nss_tm_132_preserve_display = 1 << 17,
        nss_tm_scroll_on_output = 1 << 18,
        nss_tm_dont_scroll_on_input = 1 << 19,
        nss_tm_mouse_x10 = 1 << 20,
        nss_tm_mouse_button = 1 << 21,
        nss_tm_mouse_motion = 1 << 22,
        nss_tm_mouse_many = 1 << 23,
        nss_tm_mouse_format_sgr = 1 << 24,
        nss_tm_alternate_scroll = 1LL << 36,
        nss_tm_mouse_mask =
            nss_tm_mouse_x10 | nss_tm_mouse_button |
            nss_tm_mouse_motion | nss_tm_mouse_many,
        nss_tm_print_extend = 1 << 25,
        nss_tm_print_form_feed = 1 << 26,
        nss_tm_print_enabled = 1 << 27,
        nss_tm_print_auto = 1 << 28,
        nss_tm_title_set_utf8 = 1 << 29,
        nss_tm_title_query_utf8 = 1 << 30,
        nss_tm_title_set_hex = 1 << 31,
        nss_tm_title_query_hex = 1LL << 32,
        nss_tm_bracketed_paste = 1LL << 33,
        nss_tm_keep_selection = 1LL << 34,
        nss_tm_keep_clipboard = 1LL << 35,
        nss_tm_select_to_clipboard = 1LL << 36,
    } mode, vt52mode;

    struct nss_escape {
        enum nss_escape_state {
            esc_ground,
            esc_esc_entry, esc_esc_1, esc_esc_2, esc_esc_ignore,
            esc_csi_entry, esc_csi_0, esc_csi_1, esc_csi_2, esc_csi_ignore,
            esc_dcs_entry, esc_dcs_0, esc_dcs_1, esc_dcs_2,
            esc_osc_entry, esc_osc_1, esc_osc_2, esc_osc_string,
            esc_dcs_string,
            esc_ign_entry, esc_ign_string,
            esc_vt52_entry, esc_vt52_cup_0, esc_vt52_cup_1,
        } state, old_state;
        param_t selector;
        param_t old_selector;
        size_t i;
        int32_t param[ESC_MAX_PARAM];
        uint32_t subpar_mask;
        size_t si;
        uint8_t str[ESC_MAX_STR + 1];
    } esc;

    uint16_t vt_version;
    uint16_t vt_level;

    nss_window_t *win;
    nss_input_mode_t inmode;
    nss_color_t *palette;
    pid_t child;
    int fd;
    int printerfd;

    size_t fd_pos;
    uint8_t fd_buf[NSS_FD_BUF_SZ];
};

static void handle_chld(int arg) {
    int status;
    pid_t pid = waitpid(-1, &status, WNOHANG);
    if (pid < 0) {
        // Thats unsafe
        warn("Child wait failed");
        return;
    }

    // TODO Need to hang terminal here

    if (WIFEXITED(status) && WEXITSTATUS(status))
        info("Child exited with status: %d", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        info("Child terminated due to the signal: %d", WTERMSIG(status));

    (void)arg;
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
    setenv("TERM", nss_config_string(NSS_SCONFIG_TERM_NAME), 1);

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


    /* If IUTF8 is defined, enable it by default,
     * when terminal itself is in UTF-8 mode
     */
#ifdef IUTF8
    if (nss_config_integer(NSS_ICONFIG_UTF8)) {
        struct termios tio;
        tcgetattr(slave, &tio);
            tio.c_iflag |= IUTF8;
        tcsetattr(slave, TCSANOW, &tio);
    }
#endif

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
        int fl = fcntl(master, F_GETFD);
        if (fl >= 0)
            fcntl(master, F_SETFD, fl | FD_CLOEXEC);
        signal(SIGCHLD, handle_chld);
    }
    term->child = pid;
    term->fd = master;

    return master;
}

static _Bool optimize_line_palette(nss_line_t *line) {
    // Buffer here causes a leak in theory
    static nss_cid_t *buf = NULL, buf_len = 0, *new;

    if (!line) {
        free(buf);
        buf = NULL;
        buf_len = 0;
        return 0;
    }

    if (line->pal) {
        if (buf_len < line->pal->size) {
            new = realloc(buf, line->pal->size * sizeof(nss_cid_t));
            if (!new) return 0;
            buf = new, buf_len = line->pal->size;
        }
        memset(buf, 0, buf_len * sizeof(nss_cid_t));
        for (nss_coord_t i = 0; i < line->width; i++) {
            if (line->cell[i].fg >= NSS_PALETTE_SIZE)
                buf[line->cell[i].fg - NSS_PALETTE_SIZE] = 0xFFFF;
            if (line->cell[i].bg >= NSS_PALETTE_SIZE)
                buf[line->cell[i].bg - NSS_PALETTE_SIZE] = 0xFFFF;
        }
        nss_coord_t k = 0;
        for (nss_cid_t i = 0; i < line->pal->size; i++) {
            if (buf[i] == 0xFFFF) {
                line->pal->data[k] = line->pal->data[i];
                for (nss_cid_t j = i + 1; j < line->pal->size; j++)
                    if (line->pal->data[i] == line->pal->data[j]) buf[j] = k;
                buf[i] = k++;
            }
        }
        line->pal->size = k;

        for (nss_coord_t i = 0; i < line->width; i++) {
            if (line->cell[i].fg >= NSS_PALETTE_SIZE && buf[line->cell[i].fg - NSS_PALETTE_SIZE] != 0xFFFF)
                    line->cell[i].fg = buf[line->cell[i].fg - NSS_PALETTE_SIZE] + NSS_PALETTE_SIZE;
            if (line->cell[i].bg >= NSS_PALETTE_SIZE && buf[line->cell[i].bg - NSS_PALETTE_SIZE] != 0xFFFF)
                    line->cell[i].bg = buf[line->cell[i].bg - NSS_PALETTE_SIZE] + NSS_PALETTE_SIZE;
        }
    }

    return 1;
}

static nss_cid_t alloc_color(nss_line_t *line, nss_color_t col) {
    if (line->pal) {
        if (line->pal->size > 0 && line->pal->data[line->pal->size - 1] == col)
            return NSS_PALETTE_SIZE + line->pal->size - 1;
        if (line->pal->size > 1 && line->pal->data[line->pal->size - 2] == col)
            return NSS_PALETTE_SIZE + line->pal->size - 2;
    }

    if (!line->pal || line->pal->size + 1 >= line->pal->caps) {
        if (!optimize_line_palette(line))
            return NSS_SPECIAL_BG;
        if (!line->pal || line->pal->size + 1 >= line->pal->caps) {
            if (line->pal && line->pal->caps == MAX_EXTRA_PALETTE) return NSS_SPECIAL_BG;
            nss_line_palette_t *new = realloc(line->pal, sizeof(nss_line_palette_t) + CAPS_INC_STEP(line->pal ? line->pal->caps : 0) * sizeof(nss_color_t));
            if (!new) return NSS_SPECIAL_BG;
            if (!line->pal) new->size = 0;
            new->caps = CAPS_INC_STEP(line->pal ? new->caps : 0);
            line->pal = new;
        }
    }

    line->pal->data[line->pal->size++] = col;
    return NSS_PALETTE_SIZE + line->pal->size - 1;
}

static nss_cell_t fixup_color(nss_line_t *line, nss_cursor_t *cur) {
    nss_cell_t cel = cur->cel;
    if (cel.fg >= NSS_PALETTE_SIZE)
        cel.fg = alloc_color(line, cur->fg);
    if (cel.bg >= NSS_PALETTE_SIZE)
        cel.bg = alloc_color(line, cur->bg);
    return cel;
}

static nss_line_t *term_create_line(nss_term_t *term, nss_coord_t width) {
    nss_line_t *line = malloc(sizeof(*line) + (size_t)width * sizeof(line->cell[0]));
    if (line) {
        line->width = width;
        line->wrap_at = 0;
        line->pal = NULL;
        line->next = line->prev = NULL;
        nss_cell_t cel = fixup_color(line, &term->c);
        for (nss_coord_t i = 0; i < width; i++)
            line->cell[i] = cel;
    } else warn("Can't allocate line");
    return line;
}

static nss_line_t *term_realloc_line(nss_term_t *term, nss_line_t *line, nss_coord_t width) {
    nss_line_t *new = realloc(line, sizeof(*new) + (size_t)width * sizeof(new->cell[0]));
    if (!new) die("Can't create lines");

    if (width > new->width) {
        nss_cell_t cell = fixup_color(new, &term->c);
        cell.attr = 0;

        for(nss_coord_t i = new->width; i < width; i++)
            new->cell[i] = cell;
    }

    new->width = width;
    return new;
}

static void term_free_line(nss_line_t *line) {
    free(line->pal);
    free(line);
}

static void term_line_dirt(nss_line_t *line) {
    for (nss_coord_t i = 0; i < line->width; i++)
        line->cell[i].attr &= ~nss_attrib_drawn;
}

inline static void term_put_cell(nss_term_t *term, nss_coord_t x, nss_coord_t y, nss_char_t ch) {
    term->screen[y]->cell[x] = MKCELLWITH(fixup_color(term->screen[y], &term->c), ch);
}

_Bool nss_term_is_utf8(nss_term_t *term) {
    return term->mode & nss_tm_utf8;
}

_Bool nss_term_is_nrcs_enabled(nss_term_t *term) {
    return !!(term->mode & nss_tm_enable_nrcs);
}

nss_input_mode_t *nss_term_inmode(nss_term_t *term) {
    return &term->inmode;
}

int nss_term_fd(nss_term_t *term) {
    return term->fd;
}

void nss_term_damage(nss_term_t *term, nss_rect_t damage) {
    if (intersect_with(&damage, &(nss_rect_t) {0, 0, term->width, term->height})) {
        nss_line_iter_t it = make_line_iter(term->view, term->screen, damage.y, damage.y + damage.height);
        for (nss_line_t *line; (line = line_iter_next(&it));)
            for (nss_coord_t j = damage.x; j <  MIN(damage.x + damage.width, line->width); j++)
                line->cell[j].attr &= ~nss_attrib_drawn;
    }
}

void nss_term_redraw_dirty(nss_term_t *term, _Bool cursor) {
    if (!(term->mode & nss_tm_visible)) return;

    if (MIN(term->c.x, term->width - 1) != term->prev_c_x || term->c.y != term->prev_c_y || term->prev_c_view_changed) {
        if (!(term->mode & nss_tm_hide_cursor) && !term->view)
            term->screen[term->c.y]->cell[MIN(term->c.x, term->width - 1)].attr &= ~nss_attrib_drawn;
        if ((term->prev_c_view_changed || !term->prev_c_hidden) && term->prev_c_y < term->height && term->prev_c_x < term->width)
            term->screen[term->prev_c_y]->cell[term->prev_c_x].attr &= ~nss_attrib_drawn;
    }

    term->prev_c_x = MIN(term->c.x, term->width - 1);
    term->prev_c_y = term->c.y;
    term->prev_c_hidden = (term->mode & nss_tm_hide_cursor) || term->view;
    term->prev_c_view_changed = 0;

    cursor &= !term->prev_c_hidden;
    cursor &= !(term->screen[term->c.y]->cell[MIN(term->c.x, term->width - 1)].attr & nss_attrib_drawn);

    nss_line_iter_t it = make_line_iter(term->view, term->screen, 0, term->height);

    nss_window_submit_screen(term->win, &it, term->palette, term->c.x, term->c.y, cursor);
}

static void term_reset_view(nss_term_t *term, _Bool damage) {
    term->prev_c_view_changed |= !!term->view;
    ssize_t old_spos = term->scrollback_pos;
    term->view = NULL;
    term->scrollback_pos = 0;
    if (term->vsel.state == nss_sstate_progress)
        term_change_selection(term, nss_sstate_progress, term->vsel.r.x1, term->vsel.r.y1 + old_spos, term->vsel.r.rect);
    if (damage) nss_term_damage(term, (nss_rect_t){0, 0, term->width, term->height});
}

void nss_term_scroll_view(nss_term_t *term, nss_coord_t amount) {
    if (term->mode & nss_tm_altscreen) {
        if (term->mode & nss_tm_alternate_scroll)
            term_answerback(term, CSI"%d%c", abs(amount), amount > 0 ? 'A' : 'D');
        return;
    }

    _Bool ini_view = term->view;
    ssize_t old_spos = term->scrollback_pos;

    nss_coord_t scrolled = 0;

    if (amount > 0) {
        if (!term->view && term->scrollback)
            term->view = term->scrollback, scrolled++;

        if (term->view) {
            while (scrolled < amount && term->view->prev)
                term->view = term->view->prev, scrolled++;
        }

        nss_line_iter_t it = make_line_iter(term->view, term->screen, 0, scrolled);
        for (nss_line_t *line; (line = line_iter_next(&it));)
                term_line_dirt(line);

        term->scrollback_pos += scrolled;
        nss_window_shift(term->win, 0, scrolled, term->height - scrolled, 0);
    } else if (amount < 0) {

        while (term->view && scrolled < -amount)
            term->view = term->view->next, scrolled++;

        nss_line_iter_t it = make_line_iter(term->view, term->screen, term->height - scrolled, term->height);
        for (nss_line_t *line; (line = line_iter_next(&it));)
                term_line_dirt(line);

        term->scrollback_pos -= scrolled;
        nss_window_shift(term->win, scrolled, 0, term->height - scrolled, 0);
    }

    if (term->vsel.state == nss_sstate_progress)
        term_change_selection(term, nss_sstate_progress, term->vsel.r.x1, term->vsel.r.y1 + old_spos, term->vsel.r.rect);
    term->prev_c_view_changed |= ini_view != !!term->view;
}

inline static nss_coord_t line_length(nss_line_t *line) {
    nss_coord_t max_x = line->width;
    if (!line->wrap_at)
        while(max_x > 0 && !line->cell[max_x - 1].ch) max_x--;
    else max_x = line->wrap_at;
    return max_x;
}


static void term_append_history(nss_term_t *term, nss_line_t *line) {
    if (term->scrollback_limit == 0) {
        term_free_line(line);
    } else {
        line = term_realloc_line(term, line, line_length(line));
        if (line->pal) {
            optimize_line_palette(line);
            nss_line_palette_t *pal = realloc(line->pal, sizeof(nss_line_palette_t) + sizeof(nss_color_t)*(line->pal->size));
            if (pal) {
                line->pal = pal;
                pal->caps = pal->size;
            }
        }

        if (term->scrollback) term->scrollback->next = line;
        else term->scrollback_top = line;
        line->prev = term->scrollback;
        line->next = NULL;
        term->scrollback = line;

        if (term->scrollback_limit >= 0 && ++term->scrollback_size > term->scrollback_limit) {
            if (term->scrollback_top == term->view)
                nss_term_scroll_view(term, -1);
            nss_line_t *next = term->scrollback_top->next;
            term_free_line(term->scrollback_top);

            if (next) next->prev = NULL;
            else term->scrollback = NULL;
            term->scrollback_top = next;
            term->scrollback_size = term->scrollback_limit;
        } else if (term->view)
            term->scrollback_pos++;

    }
}


inline static void term_clear_selection_on_erase(nss_term_t *term, nss_coord_t xs, nss_coord_t ys, nss_coord_t xe, nss_coord_t ye) {
    if (term->vsel.state == nss_sstate_none) return;

#define RECT_INTRS(x10, x11, y10, y11) \
    ((MAX(xs, x10) <= MIN(xe - 1, x11)) && (MAX(ys, y10) <= MIN(ye - 1, y11)))

    if (term->vsel.r.rect || term->vsel.n.y0 == term->vsel.n.y1) {
        if (RECT_INTRS(term->vsel.n.x0, term->vsel.n.x1, term->vsel.n.y0, term->vsel.n.y1))
            nss_term_clear_selection(term);
    } else {
        if (RECT_INTRS(term->vsel.n.x0, term->width - 1, term->vsel.n.y0, term->vsel.n.y0))
            nss_term_clear_selection(term);
        if (term->vsel.n.y1 - term->vsel.n.y0 > 1)
            if (RECT_INTRS(0, term->width - 1, term->vsel.n.y0 + 1, term->vsel.n.y1 - 1))
                nss_term_clear_selection(term);
        if (RECT_INTRS(0, term->vsel.n.x1, term->vsel.n.y1, term->vsel.n.y1))
            nss_term_clear_selection(term);
    }

#undef RECT_INTRS
}

static void term_fill(nss_term_t *term, nss_coord_t xs, nss_coord_t ys, nss_coord_t xe, nss_coord_t ye, nss_char_t ch) {

    if (ye < ys) SWAP(nss_coord_t, ye, ys);
    if (xe < xs) SWAP(nss_coord_t, xe, xs);

    xs = MAX(0, MIN(xs, term->width));
    xe = MAX(0, MIN(xe, term->width));
    ys = MAX(0, MIN(ys, term->height));
    ye = MAX(0, MIN(ye, term->height));

    term_clear_selection_on_erase(term, xs, xe, ys, ye);

    for (; ys < ye; ys++) {
        nss_line_t *line = term->screen[ys];
        nss_cell_t cell = fixup_color(line, &term->c);
        cell.ch = ch;
        cell.attr = 0;
        for(nss_coord_t i = xs; i < xe; i++)
            line->cell[i] = cell;
    }
}

static void term_erase(nss_term_t *term, nss_coord_t xs, nss_coord_t ys, nss_coord_t xe, nss_coord_t ye) {
    term_fill(term, xs, ys, xe, ye, 0);
}

static void term_protective_erase(nss_term_t *term, nss_coord_t xs, nss_coord_t ys, nss_coord_t xe, nss_coord_t ye) {
    if (ye < ys) SWAP(nss_coord_t, ye, ys);
    if (xe < xs) SWAP(nss_coord_t, xe, xs);

    xs = MAX(0, MIN(xs, term->width));
    xe = MAX(0, MIN(xe, term->width));
    ys = MAX(0, MIN(ys, term->height));
    ye = MAX(0, MIN(ye, term->height));

    term_clear_selection_on_erase(term, xs, xe, ys, ye);

    for (; ys < ye; ys++) {
        nss_line_t *line = term->screen[ys];
        nss_cell_t cell = fixup_color(line, &term->c);
        cell.attr = 0;
        for(nss_coord_t i = xs; i < xe; i++)
            if (!(line->cell[i].attr & nss_attrib_protected))
                line->cell[i] = cell;
    }
}

static void term_selective_erase(nss_term_t *term, nss_coord_t xs, nss_coord_t ys, nss_coord_t xe, nss_coord_t ye) {
    if (ye < ys) SWAP(nss_coord_t, ye, ys);
    if (xe < xs) SWAP(nss_coord_t, xe, xs);

    xs = MAX(0, MIN(xs, term->width));
    xe = MAX(0, MIN(xe, term->width));
    ys = MAX(0, MIN(ys, term->height));
    ye = MAX(0, MIN(ye, term->height));

    term_clear_selection_on_erase(term, xs, xe, ys, ye);

    for (; ys < ye; ys++) {
        nss_line_t *line = term->screen[ys];
        for(nss_coord_t i = xs; i < xe; i++)
            if (!(line->cell[i].attr & nss_attrib_protected))
                line->cell[i] = MKCELLWITH(line->cell[i], 0);
    }
}

static void term_adjust_wide_before(nss_term_t *term, nss_coord_t x, nss_coord_t y, _Bool left, _Bool right) {
    if (x < 0 || x > term->screen[y]->width - 1) return;
    nss_cell_t *cell = &term->screen[y]->cell[x];
    if (left && x > 0 && cell[-1].attr & nss_attrib_wide) {
        cell[-1] = MKCELLWITH(cell[-1], 0);
        cell[-1].attr &= ~nss_attrib_wide;
    }
    if (right && x < term->screen[y]->width && cell[0].attr & nss_attrib_wide) {
        cell[1] = MKCELLWITH(cell[1], 0);
    }
}

static void term_move_to(nss_term_t *term, nss_coord_t x, nss_coord_t y) {
    term->c.x = MAX(0, MIN(x, term->width - 1));
    if (term->c.origin)
        term->c.y = MIN(MAX(y, term->top), term->bottom);
    else
        term->c.y = MIN(MAX(y, 0), term->height - 1);
}

static void term_move_to_abs(nss_term_t *term, nss_coord_t x, nss_coord_t y) {
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

static void term_swap_screen(nss_term_t *term, _Bool damage) {
    term->mode ^= nss_tm_altscreen;
    SWAP(nss_cursor_t, term->back_cs, term->cs);
    SWAP(nss_line_t **, term->back_screen, term->screen);
    term_reset_view(term, damage);
    if (damage) nss_term_clear_selection(term);
}

static void term_scroll(nss_term_t *term, nss_coord_t top, nss_coord_t bottom, nss_coord_t amount, _Bool save) {
    if (term->prev_c_y >= 0 && top <= term->prev_c_y && term->prev_c_y <= bottom) {
        term->screen[term->prev_c_y]->cell[term->prev_c_x].attr &= ~nss_attrib_drawn;
        if (amount >= 0) term->prev_c_y = MAX(0, term->prev_c_y - MIN(amount, (bottom - top + 1)));
        else term->prev_c_y = MIN(term->height - 1, term->prev_c_y + MIN(-amount, (bottom - top + 1)));
    }

    if (amount > 0) { /* up */
        amount = MIN(amount, (bottom - top + 1));
        nss_coord_t rest = (bottom - top + 1) - amount;

        if (save && !(term->mode & nss_tm_altscreen) && term->top == top) {
            for (nss_coord_t i = 0; i < amount; i++) {
                term_append_history(term, term->screen[top + i]);
                term->screen[top + i] = term_create_line(term, term->width);
            }
        } else term_erase(term, 0, top, term->width, top + amount);

        for (nss_coord_t i = 0; i < rest; i++)
            SWAP(nss_line_t *, term->screen[top + i], term->screen[top + amount + i]);

    } else { /* down */
        amount = MAX(amount, -(bottom - top + 1));
        nss_coord_t rest = (bottom - top + 1) + amount;

        for (nss_coord_t i = 0; i < rest; i++)
            SWAP(nss_line_t *, term->screen[bottom - i], term->screen[bottom + amount - i]);

        term_erase(term, 0, top, term->width, top - amount);
    }

    if (!term->view) {
        if (amount > 0)
            nss_window_shift(term->win, top + amount, top, bottom + 1 - top - amount, 1);
        else if (amount < 0)
            nss_window_shift(term->win, top, top - amount, bottom + 1 - top + amount, 1);
    } else {
        nss_term_damage(term, (nss_rect_t){ .y = top - MAX(0, amount),
                .width = term->width, .height = bottom + 1 - top - abs(amount) });
    }

    term_scroll_selection(term, amount);
}

static void term_set_tb_margins(nss_term_t *term, nss_coord_t top, nss_coord_t bottom) {
    if (top < bottom) {
        term->top = MAX(0, MIN(term->height - 1, top));
        term->bottom = MAX(0, MIN(term->height - 1, bottom));
    } else {
        term->top = 0;
        term->bottom = term->height - 1;
    }
}

static void term_insert_cells(nss_term_t *term, nss_coord_t n) {
    nss_coord_t cx = MIN(term->c.x, term->width - 1);
    n = MAX(0, MIN(n, term->width - cx));
    nss_line_t *line = term->screen[term->c.y];
    term_adjust_wide_before(term, cx, term->c.y, 1, 1);
    memmove(line->cell + cx + n, line->cell + cx, (line->width - cx - n) * sizeof(nss_cell_t));
    for (nss_coord_t i = cx + n; i < term->width; i++)
        line->cell[i].attr &= ~nss_attrib_drawn;
    term_erase(term, cx, term->c.y, cx + n, term->c.y + 1);
    term_move_to(term, term->c.x, term->c.y);
}

static void term_delete_cells(nss_term_t *term, nss_coord_t n) {
    nss_coord_t cx = MIN(term->c.x, term->width - 1);
    n = MAX(0, MIN(n, term->width - cx));
    nss_line_t *line = term->screen[term->c.y];
    term_adjust_wide_before(term, cx, term->c.y, 1, 0);
    term_adjust_wide_before(term, cx + n - 1, term->c.y, 0, 1);
    memmove(line->cell + cx, line->cell + cx + n, (term->width - cx - n) * sizeof(nss_cell_t));
    for (nss_coord_t i = cx; i < term->width - n; i++)
        line->cell[i].attr &= ~nss_attrib_drawn;
    term_erase(term, term->width - n, term->c.y, term->width, term->c.y + 1);
    term_move_to(term, term->c.x, term->c.y);
}

static void term_insert_lines(nss_term_t *term, nss_coord_t n) {
    if (term->top <= term->c.y && term->c.y <= term->bottom)
        term_scroll(term, term->c.y, term->bottom, -n, 0);
    term_move_to(term, 0, term->c.y);
}

static void term_delete_lines(nss_term_t *term, nss_coord_t n) {
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

static void term_tabs(nss_term_t *term, nss_coord_t n) {
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

static inline void term_esc_start(nss_term_t *term) {
    term->esc.old_selector = term->esc.selector;
    term->esc.old_state = term->esc.state;
    term->esc.selector = 0;
}

static inline void term_esc_start_seq(nss_term_t *term) {
    for (size_t i = 0; i <= term->esc.i; i++)
        term->esc.param[i] = -1;
    term->esc.i = 0;
    term->esc.subpar_mask = 0;
    term->esc.selector = 0;
    term->esc.old_selector = 0;
    term->esc.old_state = 0;

}
static inline void term_esc_start_string(nss_term_t *term) {
    term->esc.si = 0;
    term->esc.str[0] = 0;

    term->esc.selector = 0;
    term->esc.old_selector = 0;
    term->esc.old_state = 0;
}

static void term_esc_dump(nss_term_t *term, _Bool use_info) {
    if (use_info && nss_config_integer(NSS_ICONFIG_LOG_LEVEL) < 3) return;

    char buf[ESC_DUMP_MAX] = "^[";
    size_t pos = 2;
    switch(term->esc.state) {
        case esc_esc_entry:
        case esc_esc_1:
            buf[pos++] = 0x20 + ((term->esc.selector & I0_MASK) >> 9) - 1;
        case esc_esc_2:
            buf[pos++] = 0x20 + ((term->esc.selector & I1_MASK) >> 14) - 1;
            buf[pos++] = E_MASK & term->esc.selector;
            break;
        case esc_csi_entry:
        case esc_csi_0:
        case esc_csi_1:
        case esc_csi_2:
        case esc_dcs_string:
            buf[pos++] = term->esc.state == esc_dcs_string ? 'P' :'[';
            if (term->esc.selector & P_MASK)
                buf[pos++] = '<' + ((term->esc.selector & P_MASK) >> 6) - 1;
            for (size_t i = 0; i <= term->esc.i; i++) {
                if (term->esc.param[i] >= 0)
                    pos += snprintf(buf + pos, ESC_DUMP_MAX - pos, "%"PRId32, term->esc.param[i]);
                if (i < term->esc.i) buf[pos++] = term->esc.subpar_mask & (1 << (i + 1)) ? ':' : ';' ;
            }
            if (term->esc.selector & I0_MASK)
                buf[pos++] = 0x20 + ((term->esc.selector & I0_MASK) >> 9) - 1;
            if (term->esc.selector & I1_MASK)
                buf[pos++] = 0x20 + ((term->esc.selector & I1_MASK) >> 14) - 1;
            buf[pos++] = (C_MASK & term->esc.selector) + 0x40;
            if (term->esc.state == esc_dcs_string) {
                strncat(buf + pos, (char *)term->esc.str, ESC_DUMP_MAX - pos);
                pos += term->esc.si - 1;
                buf[pos++] = '^';
                buf[pos++] = '[';
                buf[pos++] = '\\';
            }
            break;
        case esc_osc_string:
            warn("^[]%u;%s^[\\", term->esc.selector, term->esc.str);
        default:
            return;
    }
    buf[pos] = 0;
    if (use_info)
        info("%s", buf);
    else
        warn("%s", buf);
}

static void term_load_config(nss_term_t *term) {
    term->c = term->back_cs = term->cs = (nss_cursor_t) {
        .cel = MKCELL(NSS_SPECIAL_FG, NSS_SPECIAL_BG, 0, 0),
        .fg = nss_config_color(NSS_CCONFIG_FG),
        .bg = nss_config_color(NSS_CCONFIG_BG),
        .gl = 0, .gl_ss = 0, .gr = 2,
        .gn = {nss_94cs_ascii, nss_94cs_ascii, nss_94cs_ascii, nss_94cs_ascii}
    };

    for(size_t i = 0; i < NSS_PALETTE_SIZE; i++)
        term->palette[i] = nss_config_color(NSS_CCONFIG_COLOR_0 + i);
    if (nss_config_integer(NSS_ICONFIG_REVERSE_VIDEO)) {
        SWAP(nss_color_t, term->palette[NSS_SPECIAL_BG], term->palette[NSS_SPECIAL_FG]);
        SWAP(nss_color_t, term->palette[NSS_SPECIAL_CURSOR_BG], term->palette[NSS_SPECIAL_CURSOR_FG]);
        term->mode |= nss_tm_reverse_video;
    }

    if (nss_config_integer(NSS_ICONFIG_UTF8)) term->mode |= nss_tm_utf8 | nss_tm_title_query_utf8 | nss_tm_title_set_utf8;
    if (!nss_config_integer(NSS_ICONFIG_ALLOW_ALTSCREEN)) term->mode |= nss_tm_disable_altscreen;
    if (nss_config_integer(NSS_ICONFIG_INIT_WRAP)) term->mode |= nss_tm_wrap;
    if (!nss_config_integer(NSS_ICONFIG_SCROLL_ON_INPUT)) term->mode |= nss_tm_dont_scroll_on_input;
    if (nss_config_integer(NSS_ICONFIG_SCROLL_ON_OUTPUT)) term->mode |= nss_tm_scroll_on_output;
    if (nss_config_integer(NSS_ICONFIG_ALLOW_NRCS)) term->mode |= nss_tm_enable_nrcs;
    if (nss_config_integer(NSS_ICONFIG_KEEP_CLIPBOARD)) term->mode |= nss_tm_keep_clipboard;
    if (nss_config_integer(NSS_ICONFIG_KEEP_SELECTION)) term->mode |= nss_tm_keep_selection;
    if (nss_config_integer(NSS_ICONFIG_SELECT_TO_CLIPBOARD)) term->mode |= nss_tm_select_to_clipboard;
}

static void term_reset_margins(nss_term_t *term) {
    term->top = 0;
    term->bottom = term->height - 1;
}

static void term_reset(nss_term_t *term, _Bool hard) {

    term->mode &= nss_tm_focused | nss_tm_visible;
    term->inmode = nss_config_input_mode();

    nss_coord_t cx = term->c.x, cy = term->c.y;

    term_load_config(term);
    term_reset_margins(term);

    nss_window_set_mouse(term->win, 0);
    nss_window_set_cursor(term->win, nss_config_integer(NSS_ICONFIG_CURSOR_SHAPE));
    nss_window_set_colors(term->win, term->palette[NSS_SPECIAL_BG], term->palette[NSS_SPECIAL_CURSOR_FG]);

    if (hard) {
        memset(term->tabs, 0, term->width * sizeof(term->tabs[0]));
        nss_coord_t tabw = nss_config_integer(NSS_ICONFIG_TAB_WIDTH);
        for(nss_coord_t i = tabw; i < term->width; i += tabw)
            term->tabs[i] = 1;

        for(size_t i = 0; i < 2; i++) {
            term_cursor_mode(term, 1);
            term_erase(term, 0, 0, term->width, term->height);
            term_swap_screen(term, 0);
        }
        for (nss_line_t *tmp, *line = term->scrollback; line; line = tmp) {
            tmp = line->prev;
            term_free_line(line);
        }

        term->vt_level = term->vt_version / 100;

        term->scrollback_top = NULL;
        term->scrollback = NULL;
        term->scrollback_size = 0;
        term_reset_view(term, 0);

        nss_window_set_title(term->win, NULL, term->mode & nss_tm_title_set_utf8);
        nss_window_set_icon_name(term->win, NULL, term->mode & nss_tm_title_set_utf8);

        // Hmm?..
        // term->mode |= nss_tm_echo;
    } else {
        term->c.x = cx;
        term->c.y = cy;
    }

    term->esc.state = esc_ground;
}

nss_term_t *nss_create_term(nss_window_t *win, nss_coord_t width, nss_coord_t height) {
    nss_term_t *term = calloc(1, sizeof(nss_term_t));

    term->palette = malloc(NSS_PALETTE_SIZE * sizeof(nss_color_t));
    term->win = win;

    term->inmode = nss_config_input_mode();
    term->mode = nss_tm_visible;
    term->printerfd = -1;
    term->scrollback_limit = nss_config_integer(NSS_ICONFIG_HISTORY_LINES);
    term->vt_version = nss_config_integer(NSS_ICONFIG_VT_VERION);
    term->vt_level = term->vt_version / 100;

    term_load_config(term);

    for(size_t i = 0; i < 2; i++) {
        term_cursor_mode(term, 1);
        term_erase(term, 0, 0, term->width, term->height);
        term_swap_screen(term, 0);
    }

    if (tty_open(term, nss_config_string(NSS_SCONFIG_SHELL), nss_config_argv()) < 0) {
        warn("Can't create tty");
        nss_free_term(term);
        return NULL;
    }

    nss_term_resize(term, width, height);

    const char *printer_path = nss_config_string(NSS_SCONFIG_PRINTER);
    if (printer_path) {
        if (printer_path[0] == '-' && !printer_path[1])
            term->printerfd = STDOUT_FILENO;
        else
            term->printerfd = open(printer_path, O_WRONLY | O_CREAT, 0660);
    }

    return term;
}

static void term_dispatch_da(nss_term_t *term, param_t mode) {
    if (PARAM(0, 0)) return;
    switch (mode) {
    case P('='):
        CHK_VT(4);
        term_answerback(term, DCS"!|00000000"ST);
        break;
    case P('>'): {
        param_t ver = 0;
        switch(term->vt_version) {
        case 100: ver = 0; break;
        case 220: ver = 1; break;
        case 240: ver = 2; break;
        case 330: ver = 18; break;
        case 340: ver = 19; break;
        case 320: ver = 24; break;
        case 420: ver = 41; break;
        case 510: ver = 61; break;
        case 520: ver = 64; break;
        case 525: ver = 65; break;
        default:
            ver = term->vt_level * 100 + 20;
        }
        term_answerback(term, CSI">%"PRIu32";10;0c", ver);
        break;
    }
    default:
        if (term->vt_version < 200) {
            switch(term->vt_level) {
            case 125: term_answerback(term, CSI"?12;2;0;10c"); break;
            case 102: term_answerback(term, CSI"?6c"); break;
            case 101: term_answerback(term, CSI"?1;0c"); break;
            default: term_answerback(term, CSI"?1;2c");
            }
        } else {
            /*1 - 132-columns
             *2 - Printer
             *3 - ReGIS graphics
             *4 - Sixel graphics
             *6 - Selective erase
             *8 - User-defined keys
             *9 - National Replacement Character sets
             *15 - Technical characters
             *16 - Locator port
             *17 - Terminal state interrogation
             *18 - User windows
             *21 - Horizontal scrolling
             *22 - ANSI color
             *28 - Rectangular editing
             *29 - ANSI text locator (i.e., DEC Locator mode).
             */
            term_answerback(term, CSI"?%u;1;2;6%s;9;22c",
                    60 + term->vt_version/100,
                    term->inmode.keyboard_mapping == nss_km_vt220 ? ";8" : "");
        }
    }
}

static void term_dispatch_dsr(nss_term_t *term) {
    if (term->esc.selector & P_MASK) {
        switch(term->esc.param[0]) {
        case 6: /* Cursor position -- Y;X */
            term_answerback(term, CSI"%"PRIu16";%"PRIu16"%sR",
                    (term->c.origin ? -term->top : 0) + term->c.y + 1,
                    MIN(term->c.x, term->width - 1) + 1,
                    term->vt_level >= 4 ? ";1" : "");
        case 15: /* Printer status -- Has no printer */
            CHK_VT(2);
            term_answerback(term, CSI"?13n"); //TODO Has printer -- 10
            break;
        case 25: /* User defined keys lock -- Locked */
            CHK_VT(2);
            term_answerback(term, CSI"?21n"); //TODO Unlocked - 20
            break;
        case 26: /* Keyboard language -- North American */
            CHK_VT(2);
            term_answerback(term, CSI"?27;1%sn",
                    term->vt_level >= 4 ? ";0;0" : // ready, LK201
                    term->vt_level >= 3 ? ";0" : ""); // ready
            break;
        case 62: /* DECMSR, Macro space -- No data, no space for macros */
            CHK_VT(4);
            //TODO Why is it hex?
            term_answerback(term, CSI"0000*{");
            break;
        case 63: /* DECCKSR, Memory checksum -- 0000 (hex) */
            CHK_VT(4);
            term_answerback(term, DCS"%"PRId16"!~0000"ST, term->esc.param[1]);
            break;
        case 75: /* Data integrity -- Ready, no errors */
            CHK_VT(4);
            term_answerback(term, CSI"?70n");
            break;
        case 85: /* Multi-session configuration -- Not configured */
            CHK_VT(4);
            term_answerback(term, CSI"?83n");
        }
    } else {
        switch(term->esc.param[0]) {
        case 5: /* Health report -- OK */
            term_answerback(term, CSI"0n");
            break;
        case 6: /* Cursor position -- Y;X */
            term_answerback(term, CSI"%"PRIu16";%"PRIu16"R",
                    (term->c.origin ? -term->top : 0) + term->c.y + 1,
                    MIN(term->c.x, term->width - 1) + 1);
            break;
        }
    }
}

static void term_dispatch_dcs(nss_term_t *term) {
    /* yet nothing here */
    term_esc_dump(term, 0);

    term->esc.state = esc_ground;
}

static nss_clipboard_target_t decode_target(uint8_t targ, _Bool mode) {
    switch(targ) {
    case 'p': return nss_ct_primary;
    case 'q': return nss_ct_secondary;
    case 'c': return nss_ct_clipboard;
    case 's': return mode ? nss_ct_clipboard : nss_ct_primary;
    default: return -1;
    }
}

static void term_dispatch_osc(nss_term_t *term) {
    term_esc_dump(term, 1);
    switch (term->esc.selector) {
    case 0: /* Change window icon name and title */
    case 1: /* Change window icon name */
    case 2: /* Change window title */
        if (term->mode & nss_tm_title_set_hex) {
            uint8_t *end = hex_decode(term->esc.str, term->esc.str, term->esc.str + term->esc.si);
            if (*end) {
                term_esc_dump(term, 0);
                break;
            } else *(end = term->esc.str + (end - term->esc.str)/2) = '\0';
        }
        if (!(term->mode & nss_tm_title_set_utf8) && (term->mode & nss_tm_utf8)) {
            uint8_t *dst = term->esc.str;
            const uint8_t *ptr = dst;
            nss_char_t val = 0;
            while (*ptr && utf8_decode(&val, &ptr, term->esc.str + term->esc.si))
                *dst++ = val;
            *dst = '\0';
        } else if (term->mode & nss_tm_title_set_utf8 && !(term->mode & nss_tm_utf8)) {
            uint8_t *ds = malloc((term->esc.si + 1)*sizeof(uint8_t));
            if (!ds) break;

            memcpy(ds, term->esc.str, term->esc.si + 1);
            uint8_t *dst = term->esc.str, *src = ds;
            term->esc.si = 0;
            while (*src) {
                nss_char_t val = *src++;
                dst += utf8_encode(val, dst, term->esc.str + ESC_MAX_STR);
            }
            *dst = '\0';
            free(ds);
        }
        // If it's not UTF-8, assume Latin-1
        if (term->esc.selector < 2)
            nss_window_set_icon_name(term->win, (char *)term->esc.str, term->mode & nss_tm_utf8);
        if (term->esc.selector & 2)
            nss_window_set_title(term->win, (char *)term->esc.str, term->mode & nss_tm_utf8);
        break;
    case 4: /* Set color */ {
        char *pstr = (char *)term->esc.str, *pnext = NULL, *s_end;
        char *pend = pstr + term->esc.si;
        while(pstr < pend && (pnext = strchr(pstr, ';'))) {
            *pnext = '\0';
            errno = 0;
            unsigned long idx = strtoul(pstr, &s_end, 10);

            char *parg  = pnext + 1;
            if ((pnext = strchr(parg, ';'))) *pnext = '\0';
            else pnext = pend;

            if (!errno && !*s_end && s_end != pstr && idx < NSS_PALETTE_SIZE - NSS_SPECIAL_COLORS) {

                nss_color_t col = parse_color((uint8_t *)parg, (uint8_t *)pnext);
                if (col) term->palette[idx] = col;
                else if (parg[0] == '?' && parg[1] == '\0')
                    term_answerback(term, OSC"4;#%06X"ST, term->palette[idx] & 0x00FFFFFF);
                else term_esc_dump(term, 0);
            }
            pstr = pnext + 1;
        }
        if (pstr < pend && !pnext) {
            for (size_t i = 0; i < term->esc.si; i++)
                if (!term->esc.str[i]) term->esc.str[i] = ';';
            term_esc_dump(term, 0);
        }
        break;
    }
    case 5: /* Set special color */
    case 6: /* Enable/disable special color */
        term_esc_dump(term, 0);
        break;
    case 10: /* Set VT100 foreground color */ {
        nss_color_t col = parse_color(term->esc.str, term->esc.str + term->esc.si);
        if (col) {
            if (term->mode & nss_tm_reverse_video) {
                term->palette[NSS_SPECIAL_BG] = col;
                nss_window_set_colors(term->win, col, 0);
            } else term->palette[NSS_SPECIAL_FG] = col;
        } else term_esc_dump(term, 0);
        break;
    }
    case 11: /* Set VT100 background color */ {
        nss_color_t col = parse_color(term->esc.str, term->esc.str + term->esc.si);
        if (col) {
            nss_color_t def = term->palette[NSS_SPECIAL_BG];
            col = (col & 0x00FFFFFF) | (0xFF000000 & def); // Keep alpha
            if (!(term->mode & nss_tm_reverse_video)) {
                term->palette[NSS_SPECIAL_CURSOR_BG] = term->palette[NSS_SPECIAL_BG] = col;
                nss_window_set_colors(term->win, col, 0);
            } else  {
                term->palette[NSS_SPECIAL_CURSOR_FG] = term->palette[NSS_SPECIAL_FG] = col;
                nss_window_set_colors(term->win, 0, col);
            }
        } else term_esc_dump(term, 0);
        break;
    }
    case 12: /* Set Cursor color */ {
        nss_color_t col = parse_color(term->esc.str, term->esc.str + term->esc.si);
        if (col) {
            if (!(term->mode & nss_tm_reverse_video)) {
                term->palette[NSS_SPECIAL_CURSOR_FG] = col;
                nss_window_set_colors(term->win, 0, col);
            } else term->palette[NSS_SPECIAL_CURSOR_BG] = col;
        } else term_esc_dump(term, 0);
        break;
    }
    case 13: /* Set Mouse foreground color */
    case 14: /* Set Mouse background color */
    case 17: /* Set Highlight background color */
    case 19: /* Set Highlight foreground color */
    case 50: /* Set Font */
        term_esc_dump(term, 0);
        break;
    case 52: /* Manipulate selecion data */ {
        if (!nss_config_integer(NSS_ICONFIG_ALLOW_WINDOW_OPS)) break;

        nss_clipboard_target_t ts[nss_ct_MAX] = {0};
        _Bool toclip = term->mode & nss_tm_select_to_clipboard;
        uint8_t *parg = term->esc.str, *end = term->esc.str + term->esc.si, letter = 0;
        for(; parg < end && *parg !=  ';'; parg++) {
            if (strchr("pqsc", *parg)) {
                ts[decode_target(*parg, toclip)] = 1;
                if (!letter) letter = *parg;
            }
        }
        if (parg++ < end) {
            if (!letter) ts[decode_target((letter = 'c'), toclip)] = 1;
            if (!strcmp("?", (char*)parg)) {
                term->paste_from = letter;
                nss_window_paste_clip(term->win, decode_target(letter, toclip));
            } else {
                if (base64_decode(parg, parg, end) != end) parg = NULL;
                for (size_t i = 0; i < nss_ct_MAX; i++) {
                    if (ts[i]) {
                        if (i == term->vsel.targ) term->vsel.targ = -1;
                        nss_window_set_clip(term->win, parg, NSS_TIME_NOW, i);
                    }
                }
            }
        } else term_esc_dump(term, 0);
        break;
    }
    case 104: /* Reset color */ {
        if (term->esc.si) {
            char *pstr = (char *)term->esc.str, *pnext, *s_end;
            while((pnext = strchr(pstr, ';'))) {
                *pnext = '\0';
                errno = 0;
                unsigned long idx = strtoul(pstr, &s_end, 10);
                if (!errno && !*s_end && s_end != pstr && idx < NSS_PALETTE_SIZE - NSS_SPECIAL_COLORS) {
                    term->palette[idx] = nss_config_color(NSS_CCONFIG_COLOR_0 + idx);
                } else term_esc_dump(term, 0);
                pstr = pnext + 1;
            }
        } else {
            for(size_t i = 0; i < NSS_PALETTE_SIZE - NSS_SPECIAL_COLORS; i++)
                term->palette[i] = nss_config_color(NSS_CCONFIG_COLOR_0 + i);
        }
        break;
    }
    case 105: /* Reset special color */
    case 106: /* Enable/disable special color */
        term_esc_dump(term, 0);
        break;
    case 110: /*Reset  VT100 foreground color */
        if (!(term->mode & nss_tm_reverse_video)) {
            nss_window_set_colors(term->win, 0, term->palette[NSS_SPECIAL_CURSOR_FG] = nss_config_color(NSS_CCONFIG_FG));
        } else term->palette[NSS_SPECIAL_CURSOR_BG] = nss_config_color(NSS_CCONFIG_FG);
        break;
    case 111: /*Reset  VT100 background color */
        if (!(term->mode & nss_tm_reverse_video)) {
            term->palette[NSS_SPECIAL_CURSOR_BG] = nss_config_color(NSS_CCONFIG_CURSOR_BG);
            nss_window_set_colors(term->win, term->palette[NSS_SPECIAL_BG] = nss_config_color(NSS_CCONFIG_BG), 0);
        } else {
            term->palette[NSS_SPECIAL_CURSOR_FG] = nss_config_color(NSS_CCONFIG_CURSOR_FG);
            nss_window_set_colors(term->win, 0, term->palette[NSS_SPECIAL_FG] = nss_config_color(NSS_CCONFIG_FG));
        }
        break;
    case 112: /*Reset  Cursor color */
        if (!(term->mode & nss_tm_reverse_video)) {
            nss_window_set_colors(term->win, 0, term->palette[NSS_SPECIAL_CURSOR_FG] = nss_config_color(NSS_CCONFIG_CURSOR_FG));
        } else term->palette[NSS_SPECIAL_CURSOR_BG] = nss_config_color(NSS_CCONFIG_CURSOR_FG);
        break;
    case 113: /*Reset  Mouse foreground color */
    case 114: /*Reset  Mouse background color */
    case 117: /*Reset  Highlight background color */
    case 119: /*Reset  Highlight foreground color */
        term_esc_dump(term, 0);
        break;
    case 13001: {
        char *end = (char *)term->esc.str;
        errno = 0;
        unsigned long res = strtoul(end, &end, 10);
        if (res > 255 || errno || *end) {
            term_esc_dump(term, 0);
            break;
        }
        term->palette[NSS_SPECIAL_BG] &= 0x00FFFFFF;
        term->palette[NSS_SPECIAL_BG] |= res << 24;
        nss_window_set_colors(term->win, term->palette[NSS_SPECIAL_BG], 0);
        break;
    }
    default:
        term_esc_dump(term, 0);
    }

    term->esc.state = esc_ground;
}

static size_t term_define_color(nss_term_t *term, size_t arg, _Bool foreground) {
    size_t argc = arg + 1 < ESC_MAX_PARAM;
    _Bool subpars = arg && (term->esc.subpar_mask >> (arg + 1)) & 1;
    if (subpars)
        for (size_t i = arg + 1; i < ESC_MAX_PARAM &&
                (term->esc.subpar_mask >> i) & 1; i++) argc++;
    else
        for (size_t i = arg + 1; i < ESC_MAX_PARAM &&
                !((term->esc.subpar_mask >> i) & 1); i++) argc++;
    if (argc > 0) {
        if (term->esc.param[arg] == 2 && argc > 3) {
            nss_color_t col = 0xFF;
            _Bool space = subpars && argc > 4;
            _Bool wrong = 0;
            for (size_t i = 1 + space; i < 4U + space; i++) {
                wrong |= term->esc.param[arg + i] > 255;
                col = (col << 8) + MIN(MAX(0, term->esc.param[arg + i]), 0xFF);
            }
            if (wrong) term_esc_dump(term, 0);
            if (foreground) {
                term->c.cel.fg = 0xFFFF;
                term->c.fg = col;
            } else {
                term->c.cel.bg = 0xFFFF;
                term->c.bg = col;
            }
            if (!subpars) argc = MIN(argc, 4);
        } else if (term->esc.param[arg] == 5 && argc > 1) {
            if (term->esc.param[arg + 1] < NSS_PALETTE_SIZE - NSS_SPECIAL_COLORS && term->esc.param[arg + 1] >= 0) {
                if (foreground)
                    term->c.cel.fg = term->esc.param[arg + 1];
                else
                    term->c.cel.bg = term->esc.param[arg + 1];
            } else term_esc_dump(term, 0);
            if (!subpars) argc = MIN(argc, 2);
        } else {
            if (!subpars) argc = MIN(argc, 1);
             term_esc_dump(term, 0);
         }
    } else term_esc_dump(term, 0);
    return argc;
}

static void term_dispatch_sgr(nss_term_t *term) {
    size_t argc = term->esc.i + (term->esc.param[term->esc.i] > 0);
    if (!argc) argc++;
    for(size_t i = 0; i < argc; i++) {
        param_t p = MAX(0, term->esc.param[i]);
        if ((term->esc.subpar_mask >> i) & 1) return;
        switch(p) {
        case 0:
            term->c.cel.attr &= ~(nss_attrib_blink | nss_attrib_bold |
                    nss_attrib_faint | nss_attrib_inverse | nss_attrib_invisible |
                    nss_attrib_italic | nss_attrib_underlined | nss_attrib_strikethrough);
            term->c.cel.bg = NSS_SPECIAL_BG;
            term->c.cel.fg = NSS_SPECIAL_FG;
            break;
        case 1:
            term->c.cel.attr |= nss_attrib_bold;
            break;
        case 2:
            term->c.cel.attr |= nss_attrib_faint;
            break;
        case 3:
            term->c.cel.attr |= nss_attrib_italic;
            break;
        case 4:
            if (i < term->esc.i && (term->esc.subpar_mask >> (i + 1)) & 1) {
                if (term->esc.param[i + 1] <= 0)
                    term->c.cel.attr &= ~nss_attrib_underlined;
                else term->c.cel.attr |= nss_attrib_underlined;
                i++;
            } else term->c.cel.attr |= nss_attrib_underlined;
            break;
        case 5: case 6:
            term->c.cel.attr |= nss_attrib_blink;
            break;
        case 7:
            term->c.cel.attr |= nss_attrib_inverse;
            break;
        case 8:
            term->c.cel.attr |= nss_attrib_invisible;
            break;
        case 9:
            term->c.cel.attr |= nss_attrib_strikethrough;
            break;
        case 21:
            /* actually double underlind */
            term->c.cel.attr |= nss_attrib_underlined;
            break;
        case 22:
            term->c.cel.attr &= ~(nss_attrib_bold | nss_attrib_faint);
            break;
        case 23:
            term->c.cel.attr &= ~nss_attrib_italic;
            break;
        case 24:
            term->c.cel.attr &= ~nss_attrib_underlined;
            break;
        case 25: case 26:
            term->c.cel.attr &= ~nss_attrib_blink;
            break;
        case 27:
            term->c.cel.attr &= ~nss_attrib_inverse;
            break;
        case 28:
            term->c.cel.attr &= ~nss_attrib_invisible;
            break;
        case 29:
            term->c.cel.attr &= ~nss_attrib_strikethrough;
            break;
        case 30: case 31: case 32: case 33:
        case 34: case 35: case 36: case 37:
            term->c.cel.fg = p - 30;
            break;
        case 38:
            i += term_define_color(term, i + 1, 1);
            break;
        case 39:
            term->c.cel.fg = NSS_SPECIAL_FG;
            break;
        case 40: case 41: case 42: case 43:
        case 44: case 45: case 46: case 47:
            term->c.cel.bg = p - 40;
            break;
        case 48:
            i += term_define_color(term, i + 1, 0);
            break;
        case 49:
            term->c.cel.bg = NSS_SPECIAL_BG;
            break;
        case 90: case 91: case 92: case 93:
        case 94: case 95: case 96: case 97:
            term->c.cel.fg = p - 90;
            break;
        case 100: case 101: case 102: case 103:
        case 104: case 105: case 106: case 107:
            term->c.cel.bg = p - 100;
            break;
        default:
            term_esc_dump(term, 0);
        }
    }
}

static void term_dispatch_srm(nss_term_t *term, _Bool set) {
    if (term->esc.selector & P_MASK) {
        for(size_t i = 0; i <= term->esc.i; i++) {
            switch(PARAM(i, 0)) {
            case 0: /* Default - nothing */
                break;
            case 1: /* DECCKM */
                term->inmode.appcursor = set;
                break;
            case 2: /* DECANM */
                if (!set) {
                    term->vt52c = term->c;
                    term->vt52mode = term->mode;
                    term->inmode.keyboad_vt52 = 1;
                    term->vt_level = 0;
                    term->c.gl_ss = term->c.gl = 0;
                    term->c.gr = 2,
                    term->c.gn[0] = term->c.gn[2] = term->c.gn[3] = nss_94cs_ascii;
                    term->c.gn[1] = nss_94cs_dec_graph;
                    term->mode &= nss_tm_visible | nss_tm_focused | nss_tm_reverse_video;
                    term_esc_start_seq(term);
                }
                break;
            case 3: /* DECCOLM */
                //Just clear screen
                if (!(term->mode & nss_tm_132cols)) {
                    if (!(term->mode & nss_tm_132_preserve_display))
                        term_erase(term, 0, 0, term->width, term->height);
                    term_move_to(term, 0, 0);
                    term_reset_margins(term);
                }
                break;
            case 4: /* DECSCLM */
                // IGNORE
                break;
            case 5: /* DECCNM */
                if (set ^ !!(term->mode & nss_tm_reverse_video)) {
                    SWAP(nss_color_t, term->palette[NSS_SPECIAL_BG], term->palette[NSS_SPECIAL_FG]);
                    SWAP(nss_color_t, term->palette[NSS_SPECIAL_CURSOR_BG], term->palette[NSS_SPECIAL_CURSOR_FG]);
                    nss_window_set_colors(term->win, term->palette[NSS_SPECIAL_BG], term->palette[NSS_SPECIAL_CURSOR_FG]);
                }
                ENABLE_IF(set, term->mode, nss_tm_reverse_video);
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
                nss_window_set_mouse(term->win, 0);
                term->mode &= ~nss_tm_mouse_mask;
                ENABLE_IF(set, term->mode, nss_tm_mouse_x10);
                break;
            case 10: /* Show toolbar */
                // IGNORE - There is no toolbar
                break;
            case 12: /* Start blinking cursor */
            case 13:
            case 14: /* Enable XOR of controll sequence and menu for blinking */
                // IGNORE
                break;
            case 18: /* DECPFF */
                ENABLE_IF(set, term->mode, nss_tm_print_form_feed);
                break;
            case 19: /* DECREX */
                ENABLE_IF(set, term->mode, nss_tm_print_extend);
                break;
            case 25: /* DECTCEM */
                if (set ^ !!(term->mode & nss_tm_hide_cursor))
                    term->screen[term->c.y]->cell[MIN(term->c.x, term->width -1 )].attr &= ~nss_attrib_drawn;
                ENABLE_IF(!set, term->mode, nss_tm_hide_cursor);
                break;
            case 30: /* Show scrollbar */
                // IGNORE - There is no scrollbar
                break;
            case 40: /* 132COLS */
                ENABLE_IF(set, term->mode, nss_tm_132cols);
                break;
            case 42: /* DECNRCM */
                CHK_VT(3);
                ENABLE_IF(set, term->mode, nss_tm_enable_nrcs);
                break;
            //case 45: /* Reverse wrap */
            //    break;
            case 47: /* Enable altscreen */
                if (term->mode & nss_tm_disable_altscreen) break;
                if (set ^ !!(term->mode & nss_tm_altscreen)) {
                    term_swap_screen(term, 1);
                }
                break;
            case 66: /* DECNKM */
                term->inmode.appkey = set;
                break;
            case 67: /* DECBKM */
                term->inmode.backspace_is_del = !set;
                break;
            case 69: /* DECLRMM */ //TODO
                CHK_VT(4);
                term_esc_dump(term, 0);
                break;
            //case 80: /* DECSDM */ //TODO SIXEL
            //    break;
            case 95: /* DECNCSM */
                CHK_VT(5);
                ENABLE_IF(set, term->mode, nss_tm_132_preserve_display);
                break;
            case 1000: /* X11 Mouse tracking */
                nss_window_set_mouse(term->win, 0);
                term->mode &= ~nss_tm_mouse_mask;
                ENABLE_IF(set, term->mode, nss_tm_mouse_button);
                break;
            case 1001: /* Highlight mouse tracking */
                // IGNORE
                break;
            case 1002: /* Cell motion mouse tracking on keydown */
                nss_window_set_mouse(term->win, 0);
                term->mode &= ~nss_tm_mouse_mask;
                ENABLE_IF(set, term->mode, nss_tm_mouse_motion);
                break;
            case 1003: /* All motion mouse tracking */
                nss_window_set_mouse(term->win, set);
                term->mode &= ~nss_tm_mouse_mask;
                ENABLE_IF(set, term->mode, nss_tm_mouse_many);
                break;
            case 1004: /* Focus in/out events */
                ENABLE_IF(set, term->mode, nss_tm_track_focus);
                break;
            case 1005: /* UTF-8 mouse tracking */
                // IGNORE
                break;
            case 1007: /* Alternate scroll */
                ENABLE_IF(set, term->mode, nss_tm_alternate_scroll);
                break;
            case 1006: /* SGR mouse tracking */
                ENABLE_IF(set, term->mode, nss_tm_mouse_format_sgr);
                break;
            case 1010: /* Scroll to bottom on output */
                ENABLE_IF(set, term->mode, nss_tm_scroll_on_output);
                break;
            case 1011: /* Scroll to bottom on keypress */
                ENABLE_IF(!set, term->mode, nss_tm_dont_scroll_on_input);
                break;
            case 1015: /* Urxvt mouse tracking */
                // IGNORE
                break;
            case 1034: /* Interpret meta */
                term->inmode.has_meta = set;
                break;
            case 1035: /* Numlock */
                term->inmode.allow_numlock = set;
                break;
            case 1036: /* Meta sends escape */
                term->inmode.meta_escape = set;
                break;
            case 1037: /* Backspace is delete */
                term->inmode.backspace_is_del = set;
                break;
            case 1040: /* Don't clear X11 PRIMARY selection */
                ENABLE_IF(!set, term->mode, nss_tm_keep_selection);
                break;
            case 1041: /* Use CLIPBOARD instead of PRIMARY */
                ENABLE_IF(!set, term->mode, nss_tm_select_to_clipboard);
                break;
            case 1044: /* Don't clear X11 CLIPBOARD selection */
                ENABLE_IF(!set, term->mode, nss_tm_keep_clipboard);
                break;
            case 1046: /* Allow altscreen */
                ENABLE_IF(!set, term->mode, nss_tm_disable_altscreen);
                break;
            case 1047: /* Enable altscreen and clear screen */
                if (term->mode & nss_tm_disable_altscreen) break;
                if (set == !(term->mode & nss_tm_altscreen))
                    term_swap_screen(term, !set);
                if (set) term_erase(term, 0, 0, term->width, term->height);
                break;
            case 1048: /* Save cursor  */
                term_cursor_mode(term, set);
                break;
            case 1049: /* Save cursor and switch to altscreen */
                if (term->mode & nss_tm_disable_altscreen) break;
                if (set == !(term->mode & nss_tm_altscreen)) {
                    if (set) term_cursor_mode(term, 1);
                    term_swap_screen(term, !set);
                    if (!set) term_cursor_mode(term, 0);
                }
                if (set) term_erase(term, 0, 0, term->width, term->height);
                break;
            case 1051: /* SUN function keys */
                term->inmode.keyboard_mapping = set ? nss_km_sun : nss_km_default;
                break;
            case 1052: /* HP function keys */
                term->inmode.keyboard_mapping = set ? nss_km_hp : nss_km_default;
                break;
            case 1053: /* SCO function keys */
                term->inmode.keyboard_mapping = set ? nss_km_sco : nss_km_default;
                break;
            case 1060: /* legacy xterm function keys */
                term->inmode.keyboard_mapping = set ? nss_km_legacy : nss_km_default;
                break;
            case 1061: /* vt220 function keys */
                term->inmode.keyboard_mapping = set ? nss_km_vt220 : nss_km_default;
                break;
            case 2004: /* Bracketed paste */
                ENABLE_IF(set, term->mode, nss_tm_bracketed_paste);
                break;
            default:
                term_esc_dump(term, 0);
            }
        }
    } else {
        for(size_t i = 0; i <= term->esc.i; i++) {
            switch(PARAM(i, 0)) {
            case 0: /* Default - nothing */
                break;
            case 2: /* KAM */
                term->inmode.keylock = set;
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
                term_esc_dump(term, 0);
            }
        }
    }
}

static void term_print_char(nss_term_t *term, nss_char_t ch) {
    uint8_t buf[5];
    if (write(term->printerfd, buf, utf8_encode(ch, buf, buf + 5)) < 0) {
        warn("Printer error");
        if (term->printerfd != STDOUT_FILENO)
            close(term->printerfd);
        term->printerfd = -1;
    }
}

static void term_print_line(nss_term_t *term, nss_line_t *line) {
    if (term->printerfd < 0) return;

    for (nss_coord_t i = 0; i < MIN(line->width, term->width); i++)
        term_print_char(term, line->cell[i].ch);
    term_print_char(term, '\n');
}

static void term_print_screen(nss_term_t *term, _Bool ext) {
    if (term->printerfd < 0) return;

    nss_coord_t top = ext ? 0 : term->top;
    nss_coord_t bottom = ext ? term->height - 1 : term->bottom;

    while(top < bottom) term_print_line(term, term->screen[top++]);
    if (term->mode & nss_tm_print_form_feed)
        term_print_char(term, '\f');
}

static void term_dispatch_mc(nss_term_t *term) {
    if (term->esc.selector & P_MASK) {
        switch(PARAM(0, 0)) {
        case 1: /* Print current line */
            if (term->printerfd < 0) break;
            term_print_line(term, term->screen[term->c.y]);
            break;
        case 4: /* Disable autoprint */
            term->mode &= ~nss_tm_print_auto;
            break;
        case 5: /* Enable autoprint */
            term->mode |= nss_tm_print_auto;
            break;
        case 11: /* Print scrollback and screen */
            if (term->printerfd < 0) break;
            for (nss_line_t *line = term->scrollback_top; line; line = line->next)
                term_print_line(term, line);
        case 10: /* Print screen */
            term_print_screen(term, 1);
            break;
        default:
            term_esc_dump(term, 0);
        }
    } else {
        switch(PARAM(0, 0)) {
        case 0: /* Print screen */
            term_print_screen(term, term->mode & nss_tm_print_extend);
            break;
        case 4: /* Disable printer */
            term->mode &= ~nss_tm_print_enabled;
            break;
        case 5: /* Enable printer */
            term->mode |= nss_tm_print_enabled;
            break;
        default:
            term_esc_dump(term, 0);
        }

    }
}

static void term_dispatch_tmode(nss_term_t *term, _Bool set) {
    size_t pnum = term->esc.i + (term->esc.param[term->esc.i] >= 0);
    if (!pnum) return;
    for (size_t i = 0; i < pnum; i++) {
        switch(term->esc.param[i]) {
        case 0:
            ENABLE_IF(set, term->mode, nss_tm_title_set_hex);
            break;
        case 1:
            ENABLE_IF(set, term->mode, nss_tm_title_query_hex);
            break;
        case 2:
            ENABLE_IF(set, term->mode, nss_tm_title_set_utf8);
            break;
        case 3:
            ENABLE_IF(set, term->mode, nss_tm_title_query_utf8);
            break;
        default:
            term_esc_dump(term, 0);
        }
    }
}

static void term_dispatch_csi(nss_term_t *term) {
    term_esc_dump(term, 1);

    // Only SGR is allowed to have subparams
    if (term->esc.subpar_mask && term->esc.selector != C('m')) return;

    switch (term->esc.selector) {
    case C('@'): /* ICH */
        term_insert_cells(term, PARAM(0, 1));
        term_move_to(term, term->c.x, term->c.y);
        break;
    case C('A'): /* CUU */
        term_move_to(term, term->c.x, term->c.y - PARAM(0, 1));
        break;
    case C('B'): /* CUD */
    case C('e'): /* VPR */
        term_move_to(term, term->c.x, term->c.y + PARAM(0, 1));
        break;
    case C('C'): /* CUF */
        term_move_to(term, MIN(term->c.x, term->width - 1) + PARAM(0, 1), term->c.y);
        break;
    case C('D'): /* CUB */
        term_move_to(term, MIN(term->c.x, term->width - 1) - PARAM(0, 1), term->c.y);
        break;
    case C('E'): /* CNL */
        term_move_to(term, 0, term->c.y + PARAM(0, 1));
        break;
    case C('F'): /* CPL */
        term_move_to(term, 0, term->c.y - PARAM(0, 1));
        break;
    case C('`'): /* HPA */
    case C('G'): /* CHA */
        term_move_to(term, PARAM(0, 1) - 1, term->c.y);
        break;
    case C('H'): /* CUP */
    case C('f'): /* HVP */
        term_move_to_abs(term, PARAM(1, 1) - 1, PARAM(0, 1) - 1);
        break;
    case C('I'): /* CHT */
        term_tabs(term, PARAM(0, 1));
        break;
    case C('J') | P('?'): /* DECSED */
    case C('J'): /* ED */ {
        void (*erase)(nss_term_t *, nss_coord_t, nss_coord_t, nss_coord_t, nss_coord_t) = term_erase;
        if (term->esc.selector & P_MASK)
            erase = term_selective_erase;
        else if (term->mode & nss_tm_protected)
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
        case 3:
            /* UNIMPLEMENTED - Erase scrollback, xterm */
        default:
            term_esc_dump(term, 0);
        }
        term_move_to(term, term->c.x, term->c.y);
        break;
    }
    case C('K') | P('?'): /* DECSEL */
    case C('K'): /* EL */ {
        void (*erase)(nss_term_t *, nss_coord_t, nss_coord_t, nss_coord_t, nss_coord_t) = term_erase;
        if (term->esc.selector & P_MASK)
            erase = term_selective_erase;
        else if(term->mode & nss_tm_protected)
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
            term_esc_dump(term, 0);
        }
        term_move_to(term, term->c.x, term->c.y);
        break;
    }
    case C('L'): /* IL */
        term_insert_lines(term, PARAM(0, 1));
        break;
    case C('M'): /* DL */
        term_delete_lines(term, PARAM(0, 1));
        break;
    case C('P'): /* DCH */
        term_delete_cells(term, PARAM(0, 1));
        term_move_to(term, term->c.x, term->c.y);
        break;
    //case C('S') | P('>'): /* Set graphics attributes, xterm */ //TODO SIXEL
    //    break;
    case C('S'): /* SU */
        term_scroll(term, term->top, term->bottom, PARAM(0, 1), 0);
        break;
    case C('T') | P('>'): /* Reset title, xterm */
        term_dispatch_tmode(term, 0);
        break;
    case C('T'): /* SD */
    case C('^'): /* SD */
        term_scroll(term, term->top, term->bottom, -PARAM(0, 1), 0);
        break;
    case C('X'): /* ECH */
        term_protective_erase(term, term->c.x, term->c.y, term->c.x + PARAM(0, 1), term->c.y + 1);
        term_move_to(term, term->c.x, term->c.y);
        break;
    case C('Z'): /* CBT */
        term_tabs(term, -PARAM(0, 1));
        break;
    case C('a'): /* HPR */
        term_move_to(term, MIN(term->c.x, term->width - 1) + PARAM(0, 1), term->c.y + PARAM(1, 0));
        break;
    //case C('b'): /* REP */
    //    break;
    case C('c'): /* Primary DA */
    case C('c') | P('>'): /* Secondary DA */
    case C('c') | P('='): /* Tertinary DA */
        term_dispatch_da(term, term->esc.selector & P_MASK);
        break;
    case C('d'): /* VPA */
        term_move_to_abs(term, term->c.x, PARAM(0, 1) - 1);
        break;
    case C('g'): /* TBC */
        switch (PARAM(0, 0)) {
        case 0:
            term->tabs[MIN(term->c.x, term->width - 1)] = 0;
            break;
        case 3:
            memset(term->tabs, 0, term->width * sizeof(term->tabs[0]));
            break;
        }
        break;
    case C('h'): /* SM */
    case C('h') | P('?'): /* DECSET */
        term_dispatch_srm(term, 1);
        break;
    case C('i'): /* MC */
    case C('i') | P('?'): /* MC */
        term_dispatch_mc(term);
        break;
    case C('l'): /* RM */
    case C('l') | P('?'):/* DECRST */
        term_dispatch_srm(term, 0);
        break;
    case C('m') | P('>'): /* Modify keys, xterm */ {
        nss_input_mode_t mode = nss_config_input_mode();
        param_t p = PARAM(0, 0), inone = !term->esc.i && term->esc.param[0] < 0;
        if (term->esc.i > 0 && term->esc.param[1] >= 0) {
            switch(p) {
            case 0:
                term->inmode.modkey_legacy_allow_keypad = PARAM(1, 0) & 1;
                term->inmode.modkey_legacy_allow_edit_keypad = PARAM(1, 0) & 2;
                term->inmode.modkey_legacy_allow_function = PARAM(1, 0) & 4;
                term->inmode.modkey_legacy_allow_misc = PARAM(1, 0) & 8;
                break;
            case 1:
                term->inmode.modkey_cursor = PARAM(1, 0) + 1;
                break;
            case 2:
                term->inmode.modkey_fn = PARAM(1, 0) + 1;
                break;
            case 3:
                term->inmode.modkey_keypad = PARAM(1, 0) + 1;
                break;
            case 4:
                term->inmode.modkey_other = PARAM(1, 0);
                break;
            }
        } else {
            if (inone || p == 0) {
                term->inmode.modkey_legacy_allow_keypad = mode.modkey_legacy_allow_keypad;
                term->inmode.modkey_legacy_allow_edit_keypad = mode.modkey_legacy_allow_edit_keypad;
                term->inmode.modkey_legacy_allow_function = mode.modkey_legacy_allow_function;
                term->inmode.modkey_legacy_allow_misc = mode.modkey_legacy_allow_misc;
            }
            if (inone || p == 1) term->inmode.modkey_cursor = mode.modkey_cursor;
            if (inone || p == 2) term->inmode.modkey_fn = mode.modkey_fn;
            if (inone || p == 3) term->inmode.modkey_keypad = mode.modkey_keypad;
            if (inone || p == 4) term->inmode.modkey_other = mode.modkey_other;
        }
        break;
    }
    case C('m'): /* SGR */
        term_dispatch_sgr(term);
        break;
    case C('n') | P('>'): /* Disable key modifires, xterm */ {
            param_t p = term->esc.param[0];
            if (p == 0) {
                term->inmode.modkey_legacy_allow_keypad = 0;
                term->inmode.modkey_legacy_allow_edit_keypad = 0;
                term->inmode.modkey_legacy_allow_function = 0;
                term->inmode.modkey_legacy_allow_misc = 0;
            }
            if (p == 1) term->inmode.modkey_cursor = 0;
            if (p == 2) term->inmode.modkey_fn = 0;
            if (p == 3) term->inmode.modkey_keypad = 0;
            if (p == 4) term->inmode.modkey_other = 0;
            break;
    }
    case C('n') | P('?'): /* DECDSR */
    case C('n'):
        term_dispatch_dsr(term);
        break;
    //case C('p') | P('>'): /* Set pointer mode, xterm */
    //    break;
    //case C('q'): /* DECLL */
    //    break;
    case C('r'): /* DECSTBM */
        term_set_tb_margins(term, PARAM(0, 1) - 1, PARAM(1, (size_t)term->height) - 1);
        term_move_to(term, 0, 0);
        break;
    //case C('r') | P('?'): /* Restore DEC privite mode */
    //    break
    case C('s'): /* DECSLRM/(SCOSC) */
        term_cursor_mode(term, 1);
        break;
    //case C('s') | P('?'): /* Save DEC privite mode */
    //    break
    //case C('t'): /* Window operations, xterm */
    //    break
    case C('t') | P('>'):/* Set title mode, xterm */
        term_dispatch_tmode(term, 1);
        break;
    case C('u'): /* (SCORC) */
        term_cursor_mode(term, 0);
        break;
    case C('x'): /* DECREQTPARAM */
        if (term->vt_version < 200) {
            param_t p = PARAM(0, 0);
            if (p < 2)
                term_answerback(term, CSI"%d;1;1;128;128;1;0x", p + 2);
        }
        break;
    //case C('@') | I0(' '): /* SL */
    //    break;
    //case C('A') | I0(' '): /* SR */
    //    break;
    //case C('t') | I0(' '): /* DECSWBV */
    //    break;
    case C('q') | I0(' '): /* DECSCUSR */ {
        switch(PARAM(0, 1)) {
        case 1: /* Blinking block */
        case 2: /* Steady block */
            nss_window_set_cursor(term->win, nss_cursor_block);
            break;
        case 3: /* Blinking underline */
        case 4: /* Steady underline */
            nss_window_set_cursor(term->win, nss_cursor_underline);
            break;
        case 5: /* Blinking bar */
        case 6: /* Steady bar */
            nss_window_set_cursor(term->win, nss_cursor_bar);
        }
        break;
    }
    case C('p') | I0('!'): /* DECSTR */
        term_reset(term, 0);
        break;
    case C('p') | I0('"'): /* DECSCL */
        if (term->vt_version < 200) break;

        term_reset(term, 0);
        param_t p = PARAM(0, 65) - 60;
        if (p && p <= term->vt_version/100)
            term->vt_level = p;
        if (p > 1) switch(PARAM(1, 2)) {
        case 2:
            term->mode |= nss_tm_8bit;
            break;
        case 1:
            term->mode &= ~nss_tm_8bit;
            break;
        default:
            term_esc_dump(term, 0);
        }
        break;
    case C('q') | I0('"'): /* DECSCA */
        switch(PARAM(0, 2)) {
        case 1:
            term->c.cel.attr |= nss_attrib_protected;
            break;
        case 0: case 2:
            term->c.cel.attr &= ~nss_attrib_protected;
            break;
        }
        term->mode &= ~nss_tm_protected;
        break;
    //case C('y') | I0('#'): /* XTCHECKSUM */
    //    break;
    //case C('p') | I0('#'): /* XTPUSHSGR */
    //case C('{') | I0('#'):
    //    break;
    //case C('|') | I0('#'): /* XTREPORTSGR */
    //    break;
    //case C('q') | I0('#'): /* XTPOPSGR */
    //case C('}') | I0('#'):
    //    break;
    //case C('p') | I0('$'): /* DECRQM */
    //    break;
    //case C('p') | P('?') | I('$'): /* DECRQM */
    //    break;
    //case C('r') | I0('$'): /* DECCARA */
    //    break;
    //case C('t') | I0('$'): /* DECRARA */
    //    break;
    //case C('v') | I0('$'): /* DECCRA */
    //    break;
    //case C('w') | I0('$'): /* DECRQPSR */
    //    break;
    case C('x') | I0('$'): /* DECFRA */
        term_fill(term, PARAM(1,1) - 1, PARAM(2,1) - 1, PARAM(3,1), PARAM(4,1), PARAM(0, 0));
        term_move_to(term, term->c.x, term->c.y);
        break;
    case C('z') | I0('$'): /* DECERA */
        term_erase(term, PARAM(0,1) - 1, PARAM(1,1) - 1, PARAM(2,1), PARAM(3,1));
        term_move_to(term, term->c.x, term->c.y);
        break;
    case C('{') | I0('$'): /* DECSERA */
        term_selective_erase(term, PARAM(0,1) - 1, PARAM(1,1) - 1, PARAM(2,1), PARAM(3,1));
        term_move_to(term, term->c.x, term->c.y);
        break;
    //case C('w') | I0('\''): /* DECEFR */
    //    break;
    //case C('z') | I0('\''): /* DECELR */
    //    break;
    //case C('{') | I0('\''): /* DECSLE */
    //    break;
    //case C('|') | I0('\''): /* DECRQLP */
    //    break;
    //case C('}') | I0('\''): /* DECIC */
    //    break;
    //case C('~') | I0('\''): /* DECDC */
    //    break;
    //case C('x') | I0('*'): /* DECSACE */
    //    break;
    //case C('y') | I0('*'): /* DECRQCRA */
    //    break;
    //case C('|') | I0('*'): /* DECSNLS */
    //    break;
    default:
        term_esc_dump(term, 0);
    }

    term->esc.state = esc_ground;
}

static enum nss_char_set parse_nrcs(param_t selector, _Bool is96, uint16_t vt_level, _Bool nrcs) {
    #define NRC {if (!nrcs) return -1;}
    selector &= (I1_MASK | E_MASK);
    if (!is96) {
        switch (vt_level) {
        default:
            switch (selector) {
            case E('4') | I1('"'): return nss_94cs_dec_hebrew;
            case E('?') | I1('"'): return nss_94cs_dec_greek;
            case E('0') | I1('%'): return nss_94cs_dec_turkish;
            case E('=') | I1('%'): NRC; return nss_nrcs_hebrew;
            case E('>') | I1('"'): NRC; return nss_nrcs_greek;
            case E('2') | I1('%'): NRC; return nss_nrcs_turkish;
            case E('4') | I1('&'): NRC; return nss_nrcs_cyrillic;
            }
        case 4: case 3:
            switch (selector) {
            case E('5') | I1('%'): return nss_94cs_dec_sup_graph;
            case E('`'): NRC; return nss_nrcs_norwegian_dannish3;
            case E('9'): NRC; return nss_nrcs_french_canadian2;
            case E('>'): return nss_94cs_dec_tech;
            case E('6') | I1('%'): NRC; return nss_nrcs_portuguese;
            }
        case 2:
            switch (selector) {
            case E('C'): NRC; return nss_nrcs_finnish;
            case E('5'): NRC; return nss_nrcs_finnish2;
            case E('H'): NRC; return nss_nrcs_swedish;
            case E('7'): NRC; return nss_nrcs_swedish2;
            case E('K'): NRC; return nss_nrcs_german;
            case E('Q'): NRC; return nss_nrcs_french_canadian;
            case E('R'): NRC; return nss_nrcs_french;
            case E('f'): NRC; return nss_nrcs_french2;
            case E('Y'): NRC; return nss_nrcs_itallian;
            case E('Z'): NRC; return nss_nrcs_spannish;
            case E('4'): NRC; return nss_nrcs_dutch;
            case E('='): NRC; return nss_nrcs_swiss;
            case E('E'): NRC; return nss_nrcs_norwegian_dannish;
            case E('6'): NRC; return nss_nrcs_norwegian_dannish2;
            case E('<'): return nss_94cs_dec_sup;
            }
        case 1:
            switch (selector) {
            case E('A'): return nss_94cs_british;
            case E('B'): return nss_94cs_ascii;
            case E('0'): return nss_94cs_dec_graph;
            case E('1'): if (vt_level != 1) break;
                         return nss_94cs_dec_altchars;
            case E('2'): if (vt_level != 1) break;
                         return nss_94cs_dec_altgraph;
            }
        case 0: break;
        }
    } else {
        switch (vt_level) {
        default:
            switch (selector) {
            case E('F'): return nss_96cs_greek;
            case E('H'): return nss_96cs_hebrew;
            case E('L'): return nss_96cs_latin_cyrillic;
            case E('M'): return nss_96cs_latin_5;
            }
        case 4: case 3:
            switch (selector) {
            case E('A'): return nss_96cs_latin_1;
            }
        case 2: case 1: case 0:
            break;
        }
    }
    return -1;
#undef NRC
}

static void term_dispatch_esc(nss_term_t *term) {
    if (term->esc.selector != E('['))
        term_esc_dump(term, 1);

    switch(term->esc.selector) {
    case E('D'): /* IND */
        term_index(term, 0);
        break;
    case E('E'): /* NEL */
        term_index(term, 1);
        break;
    case E('F'): /* HP Home Down */
        term_move_to(term, 0, term->height - 1);
        break;
    case E('H'): /* HTS */
        term->tabs[MIN(term->c.x, term->width - 1)] = 1;
        break;
    case E('M'): /* RI */
        term_rindex(term, 0);
        break;
    case E('N'): /* SS2 */
        term->c.gl_ss = 2;
        break;
    case E('O'): /* SS3 */
        term->c.gl_ss = 3;
        break;
    case E('P'): /* DCS */
        term->esc.state = esc_dcs_entry;
        return;
    case E('V'): /* SPA */
        term->c.cel.attr |= nss_attrib_protected;
        term->mode |= nss_tm_protected;
        break;
    case E('W'): /* EPA */
        term->c.cel.attr &= ~nss_attrib_protected;
        term->mode |= nss_tm_protected;
        break;
    case E('Z'): /* DECID */
        term_dispatch_da(term, 0);
        break;
    case E('['): /* CSI */
        term->esc.state = esc_csi_entry;
        return;
    case E('\\'): /* ST */
        if (term->esc.old_state == esc_dcs_string)
            term_dispatch_dcs(term);
        else if(term->esc.old_state == esc_osc_string)
            term_dispatch_osc(term);
        break;
    case E(']'): /* OSC */
        term->esc.state = esc_osc_entry;
        return;
    case E('X'): /* SOS */
    case E('^'): /* PM */
    case E('_'): /* APC */
        term->esc.state = esc_ign_entry;
        return;
    //case E('6'): /* DECBI */
    //    CHK_VT(4);
    //    break;
    case E('7'): /* DECSC */
        term_cursor_mode(term, 1);
        break;
    case E('8'): /* DECRC */
        term_cursor_mode(term, 0);
        break;
    //case E('9'): /* DECFI */
    //    CHK_VT(4);
    //    break;
    case E('='): /* DECKPAM */
        term->inmode.appkey = 1;
        break;
    case E('>'): /* DECKPNM */
        term->inmode.appkey = 0;
        break;
    case E('c'): /* RIS */
        term_reset(term, 1);
        break;
    case E('k'): /* Old style title */
        term->esc.state = esc_osc_string;
        term->esc.selector = 2;
        return;
    case E('l'): /* HP Memory lock */
        term_set_tb_margins(term, term->c.y, term->bottom);
        break;
    case E('m'): /* HP Memory unlock */
        term_set_tb_margins(term, 0, term->bottom);
        break;
    case E('n'): /* LS2 */
        term->c.gl = term->c.gl_ss = 2;
        break;
    case E('o'): /* LS3 */
        term->c.gl = term->c.gl_ss = 3;
        break;
    case E('|'): /* LS3R */
        term->c.gr = 3;
        break;
    case E('}'): /* LS2R */
        term->c.gr = 2;
        break;
    case E('~'): /* LS1R */
        term->c.gr = 1;
        break;
    case E('F') | I0(' '): /* S7C1T */
        CHK_VT(2);
        term->mode &= ~nss_tm_8bit;
        break;
    case E('G') | I0(' '): /* S8C1T */
        CHK_VT(2);
        term->mode |= nss_tm_8bit;
        break;
    case E('L') | I0(' '): /* ANSI_LEVEL_1 */
    case E('M') | I0(' '): /* ANSI_LEVEL_2 */
        term->c.gn[1] = nss_94cs_ascii;
        term->c.gr = 1;
        /* fallthrough */
    case E('N') | I0(' '): /* ANSI_LEVEL_3 */
        term->c.gn[0] = nss_94cs_ascii;
        term->c.gl = term->c.gl_ss = 0;
        break;
    //case E('3') | I0('#'): /* DECDHL */
    //case E('4') | I0('#'): /* DECDHL */
    //case E('5') | I0('#'): /* DECSWL */
    //case E('6') | I0('#'): /* DECDWL */
    //    break;
    case E('8') | I0('#'): /* DECALN*/
        term_reset_margins(term);
        nss_term_clear_selection(term);
        term->c.x = term->c.y = 0;
        term->c.cel.attr = 0;
        term->c.cel.fg = NSS_SPECIAL_FG;
        term->c.cel.bg = NSS_SPECIAL_BG;
        for (nss_coord_t i = 0; i < term->height; i++)
            for(nss_coord_t j = 0; j < term->width; j++)
                term_put_cell(term, j, i, 'E');
        break;
    case E('@') | I0('%'): /* Disable UTF-8 */
        term->mode &= ~nss_tm_utf8;
        break;
    case E('G') | I0('%'): /* Eable UTF-8 */
    case E('8') | I0('%'):
        term->mode |= nss_tm_utf8;
        break;
    default: {
        /* Decode select charset */
        enum nss_char_set set;
        switch(term->esc.selector & I0_MASK) {
        case I0('*'): /* G2D4 */
        case I0('+'): /* G3D4 */
        case I0('('): /* GZD4 */
        case I0(')'): /* G1D4 */
            if ((set = parse_nrcs(term->esc.selector, 0, term->vt_level, term->mode & nss_tm_enable_nrcs)) > 0)
                term->c.gn[((term->esc.selector & I0_MASK) - I0('(')) >> 9] = set;
            break;
        case I0('-'): /* G1D6 */
        case I0('.'): /* G2D6 */
        case I0('/'): /* G3D6 */
            if ((set = parse_nrcs(term->esc.selector, 1, term->vt_level, term->mode & nss_tm_enable_nrcs)) > 0)
                term->c.gn[1 + (((term->esc.selector & I0_MASK) - I0('-')) >> 9)] = set;
            break;
        default:
            // If we got unknown C1
            if (term->esc.state == esc_ground)
                term->esc.state = esc_esc_entry;
            term_esc_dump(term, 0);
        }
    }
    }

    term->esc.state = esc_ground;
}

static void term_dispatch_c0(nss_term_t *term, nss_char_t ch) {
    if (ch != 0x1B) debug("^%c", ch ^ 0x40);

    switch (ch) {
    case 0x00: /* NUL (IGNORE) */
    case 0x01: /* SOH (IGNORE) */
    case 0x02: /* STX (IGNORE) */
    case 0x03: /* ETX (IGNORE) */
    case 0x04: /* EOT (IGNORE) */
        break;
    case 0x05: /* ENQ */
        term_answerback(term, "%s", nss_config_string(NSS_SCONFIG_ANSWERBACK_STRING));
        break;
    case 0x06: /* ACK (IGNORE) */
        break;
    case 0x07: /* BEL */
        if (term->esc.state == esc_dcs_string)
            term_dispatch_dcs(term);
        else if (term->esc.state == esc_osc_string)
            term_dispatch_osc(term);
        else {}/* term_bell() -- TODO */;
        break;
    case 0x08: /* BS */
        term_move_to(term, MIN(term->c.x, term->width - 1) - 1, term->c.y);
        break;
    case 0x09: /* HT */
        term_tabs(term, 1);
        break;
    case 0x0a: /* LF */
    case 0x0b: /* VT */
    case 0x0c: /* FF */
        if ((term->mode & (nss_tm_print_enabled | nss_tm_print_auto)) == nss_tm_print_auto)
            term_print_line(term, term->screen[term->c.y]);
        term_index(term, term->mode & nss_tm_crlf);
        break;
    case 0x0d: /* CR */
        term_move_to(term, 0, term->c.y);
        break;
    case 0x0e: /* SO/LS1 */
        term->c.gl = term->c.gl_ss = 1;
        if (!term->vt_level) term->esc.state = esc_ground;
        break;
    case 0x0f: /* SI/LS0 */
        term->c.gl = term->c.gl_ss = 0;
        if (!term->vt_level) term->esc.state = esc_ground;
        break;
    case 0x10: /* DLE (IGNORE) */
    case 0x11: /* XON (IGNORE) */
    case 0x12: /* DC2 (IGNORE) */
    case 0x13: /* XOFF (IGNORE) */
    case 0x14: /* DC4 (IGNORE) */
    case 0x15: /* NAK (IGNORE) */
    case 0x16: /* SYN (IGNORE) */
    case 0x17: /* ETB (IGNORE) */
        break;
    case 0x1a: /* SUB */
        term_move_to(term, term->c.x, term->c.y);
        // Clear selection when selected cell is overwritten
        if (nss_term_is_selected(term, term->c.x, term->c.y))
            nss_term_clear_selection(term);
        term_put_cell(term, term->c.x, term->c.y, '?');
    case 0x18: /* CAN */
        term->esc.state = esc_ground;
        break;
    case 0x19: /* EM (IGNORE) */
        break;
    case 0x1b: /* ESC */
        term->esc.state = term->vt_level ? esc_esc_entry : esc_vt52_entry;
        break;
    case 0x1c: /* FS (IGNORE) */
    case 0x1d: /* GS (IGNORE) */
    case 0x1e: /* RS (IGNORE) */
    case 0x1f: /* US (IGNORE) */
        break;
    }
}

static void term_dispatch_vt52(nss_term_t *term, nss_char_t ch) {
    switch (ch) {
    case '<':
        if (term->vt_version >= 100) {
            term->inmode.keyboad_vt52 = 0;
            term->vt_level = 1;
            term->mode = term->vt52mode;
            term->c.gl = term->vt52c.gl;
            term->c.gr = term->vt52c.gr;
            term->c.gl_ss = term->vt52c.gl_ss;
            term->c.gn[0] = term->vt52c.gn[0];
            term->c.gn[1] = term->vt52c.gn[1];
            term->c.gn[2] = term->vt52c.gn[2];
            term->c.gn[3] = term->vt52c.gn[3];
        }
        break;
    case '=':
        term->inmode.appkey = 1;
        break;
    case '>':
        term->inmode.appkey = 0;
        break;
    case 'A':
        term_move_to(term, term->c.x, term->c.y - 1);
        break;
    case 'B':
        term_move_to(term, term->c.x, term->c.y + 1);
        break;
    case 'C':
        term_move_to(term, term->c.x + 1, term->c.y);
        break;
    case 'D':
        term_move_to(term, MIN(term->c.x, term->width - 1) - 1, term->c.y);
        break;
    case 'F':
        term->c.gl = term->c.gl_ss = 1;
        break;
    case 'G':
        term->c.gl = term->c.gl_ss = 0;
        break;
    case 'H':
        term_move_to_abs(term, 0, 0);
        break;
    case 'I':
        term_rindex(term, 0);
        break;
    case 'J':
        term_adjust_wide_before(term, term->c.x, term->c.y, 1, 0);
        term_erase(term, term->c.x, term->c.y, term->width, term->c.y + 1);
        term_erase(term, 0, term->c.y + 1, term->width, term->height);
        break;
    case 'K':
        term_adjust_wide_before(term, term->c.x, term->c.y, 1, 0);
        term_erase(term, term->c.x, term->c.y, term->width, term->c.y + 1);
        break;
    case 'V': /* Print cursor line */
        if (term->printerfd < 0) break;
        term_print_line(term, term->screen[term->c.y]);
        break;
    case 'W': /* Enable printer */
        term->mode |= nss_tm_print_enabled;
        break;
    case 'X': /* Disable printer */
        term->mode &= ~nss_tm_print_enabled;
        break;
    case 'Y':
        term->esc.state = esc_vt52_cup_0;
        return;
    case 'Z':
        term_answerback(term, ESC"/Z");
        break;
    case ']': /* Print screen */
        term_print_screen(term, term->mode & nss_tm_print_extend);
        break;
    case '^': /* Autoprint on */
        term->mode |= nss_tm_print_auto;
        break;
    case '_': /* Autoprint off */
        term->mode &= ~nss_tm_print_auto;
        break;
    default:
        warn("^[%c", ch);
    }

    term->esc.state = esc_ground;
}

static void term_dispatch_vt52_cup(nss_term_t *term) {
    term_move_to_abs(term, term->esc.param[1], term->esc.param[0]);
    term->esc.state = esc_ground;
}

static void term_putchar(nss_term_t *term, nss_char_t ch) {
    //info("UTF %"PRIx32" '%s'", ch, buf);

    // TODO More sophisticated filtering
    if (term->mode & nss_tm_print_enabled)
        term_print_char(term, ch);

    if (IS_C1(ch) && (term->vt_level > 1)) {
        term_esc_start(term);
        term->esc.selector = E(ch ^ 0xC0);
        term_dispatch_esc(term);
        return;
    }

    if ((term->esc.state != esc_ground || !term->vt_level) && term->esc.state !=
            esc_dcs_string && term->esc.state != esc_osc_string) ch &= 0x7F;

    switch (term->esc.state) {
    case esc_esc_entry:
        term_esc_start(term);
    case esc_esc_1:
        if (0x20 <= ch && ch <= 0x2F) {
            term->esc.selector |= term->esc.state ==
                    esc_esc_entry ? I0(ch) : I1(ch);
            term->esc.state++;
        } else
    case esc_esc_2:
        if (0x30 <= ch && ch <= 0x7E) {
            term->esc.selector |= E(ch);
            term_dispatch_esc(term);
        } else
    case esc_esc_ignore:
        if (ch <= 0x1F)
            term_dispatch_c0(term, ch);
        else if (ch == 0x7F)
            /* ignore */;
        else if (0x30 <= ch && ch <= 0x7E)
            term->esc.state = esc_ground;
        else
            term->esc.state = esc_esc_ignore;
        break;
    case esc_dcs_entry:
        term_esc_start_string(term);
    case esc_csi_entry:
        term_esc_start_seq(term);
        term->esc.state++;
        if (0x3C <= ch && ch <= 0x3F)
            term->esc.selector |= P(ch);
        else
    case esc_csi_0:
    case esc_dcs_0:
        if (0x30 <= ch && ch <= 0x39)
            term->esc.param[term->esc.i] = (ch - 0x30) +
                MAX(term->esc.param[term->esc.i], 0) * 10;
        else if (ch == 0x3B) {
            if (term->esc.i < ESC_MAX_PARAM - 1)
                term->esc.param[++term->esc.i] = -1;
        } else if (ch == 0x3A) {
            if (term->esc.i < ESC_MAX_PARAM - 1) {
                term->esc.param[++term->esc.i] = -1;
                term->esc.subpar_mask |= 1 << term->esc.i;
            }
        } else
    case esc_csi_1:
    case esc_dcs_1:
        if (0x20 <= ch && ch <= 0x2F) {
            term->esc.selector |= (term->esc.state == esc_csi_0 ||
                    term->esc.state == esc_dcs_0) ? I0(ch) : I1(ch);
            term->esc.state++;
        } else
    case esc_csi_2:
    case esc_dcs_2:
        if (0x40 <= ch && ch <= 0x7E) {
            term->esc.selector |= C(ch);
            if (esc_dcs_entry <= term->esc.state && term->esc.state <= esc_dcs_2)
                term->esc.state = esc_dcs_string;
            else
                term_dispatch_csi(term);
        } else
    case esc_csi_ignore:
        if (ch <= 0x1F) {
            if (esc_dcs_entry > term->esc.state || term->esc.state > esc_dcs_2)
                term_dispatch_c0(term, ch);
        } else if (ch == 0x7F)
            /* ignore */;
        else if (esc_dcs_entry <= term->esc.state && term->esc.state <= esc_dcs_2)
            term->esc.state = esc_ign_string;
        else if (0x40 <= ch && ch <= 0x7E)
            term->esc.state = esc_ground;
        else
            term->esc.state = esc_csi_ignore;
        break;
    case esc_osc_entry:
        term_esc_start_string(term);
        term->esc.state++;
        if (ch == 'l' || ch == 'L') {
            term->esc.selector = 1 + (ch == 'L');
            term->esc.state = esc_osc_2;
        } else
    case esc_osc_1:
        if (0x30 <= ch && ch <= 0x39)
            term->esc.selector = (ch - 0x30) + term->esc.selector * 10;
        else
    case esc_osc_2:
        if(ch == 0x3B)
            term->esc.state = esc_osc_string;
        else
    case esc_ign_string:
        if (ch == 0x1b || ch == 0x1a || ch == 0x18 || ch == 0x07)
            term_dispatch_c0(term, ch);
        else
            term->esc.state = esc_ign_string;
        break;
    case esc_ign_entry:
        term_esc_start_string(term);
        term->esc.state = esc_ign_string;
        if (ch == 0x1b || ch == 0x1a || ch == 0x18 || ch == 0x07)
            term_dispatch_c0(term, ch);
        break;
    case esc_osc_string:
        if (ch <= 0x1F && ch != 0x1B && ch != 0x1A && ch != 0x18 && ch != 0x07)
            /* ignore */;
        else
    case esc_dcs_string:
        if (ch == 0x7F && term->esc.state != esc_osc_string)
            /* ignore */;
        else if (ch == 0x1b || ch == 0x1a || ch == 0x18 || ch == 0x07)
            term_dispatch_c0(term, ch);
        else {
            uint8_t buf[UTF8_MAX_LEN + 1];
            size_t char_len = utf8_encode(ch, buf, buf + UTF8_MAX_LEN);
            buf[char_len] = '\0';

            /* TODO Don't ignore long strings but process them to free buffer
             * (need some complicated code to process incomplete OSC/DCS strings)
             * This is only relevant to OSC 52, SIXEL, DECUDK and DECDLD
             * The latter three is not implemented yet
             * so nss_window_append_clip
             *      -- it appends to selection data if only we still own selection
             *         and just ignores data otherwise
             *      -- also, it shouldn't own passed buffer to decode base64 in-place */

            if (term->esc.si + char_len + 1 > ESC_MAX_STR) return;
            memcpy(term->esc.str + term->esc.si, buf, char_len + 1);
            term->esc.si += char_len;
        }
        break;
    case esc_vt52_entry:
        if (ch <= 0x1F)
            term_dispatch_c0(term, ch);
        else
            term_dispatch_vt52(term, ch);
        break;
    case esc_vt52_cup_0:
        term_esc_start_seq(term);
    case esc_vt52_cup_1:
        if (ch <= 0x1F)
            term_dispatch_c0(term, ch);
        else {
            term->esc.param[term->esc.i++] = ch - ' ';
            if (term->esc.state == esc_vt52_cup_1)
                term_dispatch_vt52_cup(term);
            else term->esc.state++;
        }
        break;
    case esc_ground:;
        uint8_t glv = term->c.gn[term->c.gl_ss];
        if (ch <= 0x1F)
            term_dispatch_c0(term, ch);
        else if (ch == 0x7F && (!(term->mode & nss_tm_enable_nrcs) && (glv == nss_96cs_latin_1 || glv == nss_94cs_british)))
            /* ignore */;
        else {
            // 'print' state

            nss_coord_t width = wcwidth(ch);
            if (width < 0) /*ch = UTF_INVAL,*/ width = 1;
            // Ignore zero-width characters
            else if (width == 0) return;

            info("%c (%u)", ch, ch);

            // Wrap line if needed
            if (term->mode & nss_tm_wrap) {
                if (term->c.x + width > term->width) {
                    term->screen[term->c.y]->wrap_at = term->c.x;
                    if ((term->mode & (nss_tm_print_enabled | nss_tm_print_auto)) == nss_tm_print_auto)
                        term_print_line(term, term->screen[term->c.y]);
                    term_index(term, 1);
                }
            } else term->c.x = MIN(term->c.x, term->width - width);

            // Shift characters to the left if insert mode is enabled
            nss_cell_t *cell = &term->screen[term->c.y]->cell[term->c.x];
            if (term->mode & nss_tm_insert && term->c.x + width < term->width) {
                for (nss_cell_t *c = cell + width; c - term->screen[term->c.y]->cell < term->width; c++)
                    c->attr &= ~nss_attrib_drawn;
                memmove(cell + width, cell, term->screen[term->c.y]->width - term->c.x - width);
            }

            // Erase overwritten parts of wide characters
            term_adjust_wide_before(term, term->c.x, term->c.y, 1, 0);
            term_adjust_wide_before(term, term->c.x + width - 1, term->c.y, 0, 1);

            // Decode nrcs

            // In theory this should be disabled while in UTF-8 mode, but
            // in practive applications use these symbols, so keep translating
            if (!(term->mode & nss_tm_utf8) || nss_config_integer(NSS_ICONFIG_ALLOW_CHARSETS))
                ch = nrcs_decode(term->c.gn[term->c.gl_ss], term->c.gn[term->c.gr], ch, term->mode & nss_tm_enable_nrcs);

            // Clear selection when selected cell is overwritten
            if (nss_term_is_selected(term, term->c.x, term->c.y))
                nss_term_clear_selection(term);

            // Put character itself
            term_put_cell(term, term->c.x, term->c.y, ch);

            // Put dummy character to the left of wide
            if (width > 1) {
                cell[1] = fixup_color(term->screen[term->c.y], &term->c);
                cell[0].attr |= nss_attrib_wide;
            }

            term->c.gl_ss = term->c.gl; // Reset single shift
            term->prev_ch = ch; // For REP CSI Ps b

            term->c.x += width;
        }
        break;
    }
}

static ssize_t term_write(nss_term_t *term, const uint8_t *buf, size_t len, _Bool show_ctl) {
    const uint8_t *end = buf + len, *start = buf;

    while (start < end) {
        nss_char_t ch;
        // Try to handle unencoded C1 bytes even if UTF-8 is enabled
        if (!(term->mode & nss_tm_utf8) || IS_C1(*start)) ch = *start++;
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

    return start - buf;
}

ssize_t nss_term_read(nss_term_t *term) {
    if (term->fd == -1) return -1;

    if (term->mode & nss_tm_scroll_on_output && term->view)
        term_reset_view(term, 1);

    ssize_t inc = read(term->fd, term->fd_buf + term->fd_pos, sizeof(term->fd_buf) - term->fd_pos);
    if (inc < 0) {
        warn("Can't read from tty");
        nss_term_hang(term);
        return -1;
    }
    term->fd_pos += inc;

    ssize_t dec = term_write(term, term->fd_buf, term->fd_pos, 0);
    term->fd_pos -= dec;
    if (term->fd_pos)
        memmove(term->fd_buf, term->fd_buf + dec, term->fd_pos);
    return inc;
}

static void tty_write_raw(nss_term_t *term, const uint8_t *buf, ssize_t len) {
    ssize_t res, lim = TTY_MAX_WRITE;
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
        if (IS_C1(*it) && (!(term->mode & nss_tm_8bit ) || term->vt_level < 2)) {
            *fmtp++ = 0x1B;
            *fmtp++ = *it ^ 0xC0;
        } else if ((*it > 0x7F) && term->mode & nss_tm_utf8) {
            *fmtp++ = 0xC0 | (*it >> 6);
            *fmtp++ = 0x80 | (*it & 0x3F);
        } else {
            *fmtp++ = *it;
        }
    }
    *fmtp = 0x00;
    return fmtp - out;
}

void term_answerback(nss_term_t *term, const char *str, ...) {
    static uint8_t fmt[MAX_REPORT], csi[MAX_REPORT];
    va_list vl;
    va_start(vl, str);
    term_encode_c1(term, (const uint8_t *)str, fmt);
    ssize_t res = vsnprintf((char *)csi, sizeof(csi), (char *)fmt, vl);
    va_end(vl);
    term_tty_write(term, csi, res);
}

/* If len == 0 encodes C1 controls and determines length by NUL character */
void nss_term_sendkey(nss_term_t *term, const uint8_t *str, size_t len) {
    _Bool encode = !len;
    if (!len) len = strlen((char *)str);

    if (term->mode & nss_tm_echo)
        term_write(term, str, len, 1);

    if (!(term->mode & nss_tm_dont_scroll_on_input) && term->view)
        term_reset_view(term, 1);
    uint8_t rep[MAX_REPORT];

    if (encode) len = term_encode_c1(term, str, rep);

    term_tty_write(term, encode ? rep : str, len);
}

void nss_term_sendbreak(nss_term_t *term) {
    if (tcsendbreak(term->fd, 0))
        warn("Can't send break");
}

void nss_term_toggle_numlock(nss_term_t *term) {
    term->inmode.allow_numlock = !term->inmode.allow_numlock;
}

void nss_term_resize(nss_term_t *term, nss_coord_t width, nss_coord_t height) {
    // Notify application

    int16_t wwidth, wheight;
    nss_window_get_dim(term->win, &wwidth, &wheight);

    struct winsize wsz = {
        .ws_col = width,
        .ws_row = height,
        .ws_xpixel = wwidth,
        .ws_ypixel = wheight
    };

    if (ioctl(term->fd, TIOCSWINSZ, &wsz) < 0) {
        warn("Can't change tty size");
        nss_term_hang(term);
    }

    _Bool cur_moved = term->c.x == term->width;

    // Free extra lines, scrolling screen upwards

    if (term->height > height) {
        if (term->mode & nss_tm_altscreen)
            SWAP(nss_line_t **, term->screen, term->back_screen);

        nss_coord_t delta = MAX(0, term->c.y - height + 1);

        for (nss_coord_t i = height; i < term->height; i++) {
            if (i < height + delta)
                term_append_history(term, term->screen[i - height]);
            else
                term_free_line(term->screen[i]);
            term_free_line(term->back_screen[i]);
        }

        memmove(term->screen, term->screen + delta, (term->height - delta)* sizeof(term->screen[0]));
        if (delta) nss_window_shift(term->win, delta, 0, term->height - delta, 0);

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
        for (nss_coord_t i = term->height; i < height; i++) {
            term->screen[i] = term_create_line(term, width);
            term->back_screen[i] = term_create_line(term, width);
        }
    }

    // Resize tabs

    _Bool *new_tabs = realloc(term->tabs, width * sizeof(*term->tabs));
    if (!new_tabs) die("Can't alloc tabs");
    term->tabs = new_tabs;

    if(width > term->width) {
        memset(new_tabs + term->width, 0, (width - term->width) * sizeof(new_tabs[0]));
        nss_coord_t tab = term->width ? term->width - 1: 0, tabw = nss_config_integer(NSS_ICONFIG_TAB_WIDTH);
        while (tab > 0 && !new_tabs[tab]) tab--;
        while ((tab += tabw) < width) new_tabs[tab] = 1;
    }

    // Set parameters

    nss_coord_t minh = MIN(height, term->height);
    nss_coord_t minw = MIN(width, term->width);
    nss_coord_t dx = width - term->width;
    nss_coord_t dy = height - term->height;

    term->width = width;
    term->height = height;

    // Clear new regions

    nss_line_t *view = term->view;
    ssize_t scrollback_pos = term->scrollback_pos;
    for (size_t j = 0; j < 2; j++) {
        // Reallocate line if it is not wide enough
        for (nss_coord_t i = 0; i < minh; i++)
            if (term->screen[i]->width < width)
                term->screen[i] = term_realloc_line(term, term->screen[i], width);
        term_swap_screen(term, 0);
    }
    term->view = view;
    term->scrollback_pos = scrollback_pos;

    // Reset scroll region

    term_reset_margins(term);
    term_move_to(term, term->c.x, term->c.y);
    if (cur_moved) {
        term->screen[term->c.y]->cell[MIN(term->c.x, term->width - 1)].attr &= ~nss_attrib_drawn;
        term->screen[term->c.y]->cell[MAX(term->c.x - 1, 0)].attr &= ~nss_attrib_drawn;
    }

    // Damage screen

    if (!(term->mode & nss_tm_altscreen)) {
        if (dy > 0) nss_term_damage(term, (nss_rect_t) { 0, minh, minw, dy });
        if (dx > 0) nss_term_damage(term, (nss_rect_t) { minw, 0, dx, height });
    }
}

_Bool nss_term_paste_need_encode(nss_term_t *term) {
    return term->paste_from;
}

void nss_term_paste_begin(nss_term_t *term) {
    /* If paste_from is not 0 application have requested
     * OSC 52 selection contents reply */
    if (term->paste_from)
        term_answerback(term, OSC"52;%c;", term->paste_from);
    /* Otherwize it's just paste (bracketed or not) */
    else if (term->mode & nss_tm_bracketed_paste)
        term_answerback(term, CSI"200~");
}

void nss_term_paste_end(nss_term_t *term) {
    /* If paste_from is not 0 application have requested
     * OSC 52 selection contents reply
     *
     * Actually there's a race condition
     * if user have requested paste and
     * before data have arrived an application
     * uses OSC 52. But it's really hard to deal with
     *
     * Probably creating the queue of paste
     * would be a valid solution
     *
     * But this race isn't that destructive
     * rare to deal with
     */
    if (term->paste_from) {
        term_answerback(term, ST);
        term->paste_from = 0;
    } else if (term->mode & nss_tm_bracketed_paste)
        term_answerback(term, CSI"201~");
}

_Bool nss_term_keep_clipboard(nss_term_t *term) {
    return term->mode & nss_tm_keep_clipboard;
}

void nss_term_focus(nss_term_t *term, _Bool focused) {
    ENABLE_IF(focused, term->mode, nss_tm_focused);
    if (term->mode & nss_tm_track_focus)
        term_answerback(term, focused ? CSI"I" : CSI"O");
    term->screen[term->c.y]->cell[MIN(term->c.x, term->width - 1)].attr &= ~nss_attrib_drawn;
}

void nss_term_visibility(nss_term_t *term, _Bool visible) {
    ENABLE_IF(visible, term->mode, nss_tm_visible);
    nss_term_damage(term, (nss_rect_t){0, 0, term->width, term->height});
}

inline static size_t descomose_selection(nss_rect_t dst[static 3], nss_visual_selection_t *sel, nss_rect_t bound, ssize_t pos) {
    size_t count = 0;
    if (sel->state != nss_sstate_none && sel->state != nss_sstate_pressed) {
        nss_coord_t x0 = sel->n.x0, x1 = sel->n.x1 + 1;
        ssize_t y0 = sel->n.y0 + pos, y1 = sel->n.y1 + 1 + pos;
        if (sel->n.rect || y1 - y0 == 1) {
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

static void term_update_selection(nss_term_t *term, nss_visual_selection_t *old) {
    // There could be at most 6 difference rectangles

    nss_rect_t d_old[4] = {{0}}, d_new[4] = {{0}}, d_diff[16] = {{0}};
    size_t sz_old = 0, sz_new = 0, count = 0;
    nss_rect_t *res = d_diff, bound = {0, 0, term->width, term->height};

    sz_old = descomose_selection(d_old, old, bound, term->scrollback_pos);
    sz_new = descomose_selection(d_new, &term->vsel, bound, term->scrollback_pos);

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

void nss_term_clear_selection(nss_term_t *term) {
    nss_visual_selection_t old = term->vsel;

    term->vsel.state = nss_sstate_none;

    term_update_selection(term, &old);

    if (term->vsel.targ > 0) {
        if (term->mode & nss_tm_keep_selection) return;

        nss_window_set_clip(term->win, NULL, NSS_TIME_NOW, term->vsel.targ);
        term->vsel.targ = -1;
    }
}

static void term_scroll_selection(nss_term_t *term, nss_coord_t amount) {
    if (term->vsel.state == nss_sstate_none) return;

    // Clear sellection if it is going to be split by scroll
    if ((term->top <= term->vsel.n.y0 && term->vsel.n.y0 <= term->bottom) ||
            (term->top <= term->vsel.n.y1 && term->vsel.n.y1 <= term->bottom))
        nss_term_clear_selection(term);

    // Scroll and cut off scroll off lines

    term->vsel.r.y0 -= amount;
    term->vsel.r.y0 -= amount;
    term->vsel.n.y1 -= amount;
    term->vsel.n.y1 -= amount;

    _Bool swapped = term->vsel.r.y0 > term->vsel.r.y1;

    if (swapped) {
        SWAP(ssize_t, term->vsel.r.y0, term->vsel.r.y1);
        SWAP(ssize_t, term->vsel.r.x0, term->vsel.r.x1);
    }

    if (term->vsel.r.y0 < term->top) {
        term->vsel.r.y0 = term->top;
        term->vsel.n.y0 = term->top;
        if (!term->vsel.r.rect) {
            term->vsel.r.x0 = 0;
            term->vsel.n.x0 = 0;
        }
    }

    if (term->vsel.r.y1 > term->bottom) {
        term->vsel.r.y1 = term->bottom;
        term->vsel.n.y1 = term->bottom;
        if (!term->vsel.r.rect) {
            term->vsel.r.x1 = term->screen[term->bottom]->width - 1;
            term->vsel.n.x1 = term->screen[term->bottom]->width - 1;
        }
    }

    if (term->vsel.r.y0 > term->vsel.r.y1)
        nss_term_clear_selection(term);

    if (swapped) {
        SWAP(ssize_t, term->vsel.r.y0, term->vsel.r.y1);
        SWAP(ssize_t, term->vsel.r.x0, term->vsel.r.x1);
    }
 }

inline static _Bool is_separator(nss_char_t ch) {
        if (!ch) return 1;
        uint8_t cbuf[UTF8_MAX_LEN + 1];
        cbuf[utf8_encode(ch, cbuf, cbuf + UTF8_MAX_LEN)] = '\0';
        return strstr(nss_config_string(NSS_SCONFIG_WORD_SEPARATORS), (char *)cbuf);
}

static void selection_normalize(nss_visual_selection_t *sel) {
    sel->n.x0 = sel->r.x0, sel->n.y0 = sel->r.y0;
    sel->n.x1 = sel->r.x1, sel->n.y1 = sel->r.y1;
    sel->r.rect = sel->n.rect;
    if (sel->n.y1 <= sel->n.y0) {
        if (sel->n.y1 < sel->n.y0) {
            SWAP(nss_coord_t, sel->n.y0, sel->n.y1);
            SWAP(nss_coord_t, sel->n.x0, sel->n.x1);
        } else if (sel->n.x1 < sel->n.x0) {
            SWAP(nss_coord_t, sel->n.x0, sel->n.x1);
        }
    }
    if (sel->n.rect && sel->n.x1 < sel->n.x0)
            SWAP(nss_coord_t, sel->n.x0, sel->n.x1);
}

static void term_snap_selection(nss_term_t *term) {
    selection_normalize(&term->vsel);

    if (term->vsel.snap == nss_ssnap_line) {
        term->vsel.state = nss_sstate_progress;

        nss_line_iter_t it = make_line_iter(term->view, term->screen,
                term->vsel.n.y0, term->scrollback_pos + term->height);
        term->vsel.n.x0 = 0;
        term->vsel.n.x1 = term->width - 1;

        nss_line_t *line;
        term->vsel.n.y0++;
        do line = line_iter_prev(&it), term->vsel.n.y0--;
        while (line && line->wrap_at);

        it = make_line_iter(term->view, term->screen,
                term->vsel.n.y1, term->scrollback_pos + term->height);
        line = line_iter_next(&it);
        if (!line) return;

        while (line && line->wrap_at)
            line = line_iter_prev(&it), term->vsel.n.y1++;

    } else if (term->vsel.snap == nss_ssnap_word) {
        term->vsel.state = nss_sstate_progress;

        nss_line_iter_t it = make_line_iter(term->view, term->screen,
                term->scrollback_pos + term->vsel.n.y0, term->height + term->scrollback_pos);
        nss_line_t *line = line_iter_next(&it); line_iter_prev(&it);

        if (!line) return;

        term->vsel.n.x0 = MIN(term->vsel.n.x0, line->width - 1);
        _Bool first = 1, cat = is_separator(line->cell[term->vsel.n.x0].ch);
        if (term->vsel.n.x0 >= 0) do {
            if (!first) {
                term->vsel.n.x0 = line->wrap_at;
                term->vsel.n.y0--;
            } else first = 0;
            while(term->vsel.n.x0 > 0 &&
                    cat == is_separator(line->cell[term->vsel.n.x0 - 1].ch)) term->vsel.n.x0--;
            if (cat != is_separator(line->cell[0].ch)) break;
        } while((line = line_iter_prev(&it)) && line->wrap_at);

        it = make_line_iter(term->view, term->screen,
                term->scrollback_pos + term->vsel.n.y1, term->height + term->scrollback_pos);
        line = line_iter_next(&it);

        if (!line) return;

        term->vsel.n.x1 = MAX(term->vsel.n.x1, 0);
        first = 1, cat = is_separator(line->cell[term->vsel.n.x1].ch);
        ssize_t line_len = line->wrap_at ? line->wrap_at : line->width;
        if (term->vsel.n.x1 < line->width) do {
            if (!first) {
                if (cat != is_separator(line->cell[0].ch)) break;
                term->vsel.n.x1 = 0;
                term->vsel.n.y1++;
                line_len = line->wrap_at;
            } else first = 0;
            while(term->vsel.n.x1 < line_len - 1 &&
                    cat == is_separator(line->cell[term->vsel.n.x1 + 1].ch)) term->vsel.n.x1++;
            if (cat != is_separator(line->cell[line_len - 1].ch)) break;
        } while(line->wrap_at && (line = line_iter_next(&it)));
    }
}

_Bool nss_term_is_selected(nss_term_t *term, nss_coord_t x, nss_coord_t y) {
    if (term->vsel.state == nss_sstate_none || term->vsel.state == nss_sstate_pressed) return 0;

    y -= term->scrollback_pos;

    if (term->vsel.n.rect) {
        return (term->vsel.n.x0 <= x && x <= term->vsel.n.x1) &&
                (term->vsel.n.y0 <= y && y <= term->vsel.n.y1);
    } else {
        return (term->vsel.n.y0 <= y && y <= term->vsel.n.y1) &&
                !(term->vsel.n.y0 == y && x < term->vsel.n.x0) &&
                !(term->vsel.n.y1 == y && x > term->vsel.n.x1);
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

static uint8_t *term_selection_data(nss_term_t *term) {
    if (term->vsel.state == nss_sstate_released) {
        uint8_t *res = malloc(SEL_INIT_SIZE * sizeof(*res));
        if (!res) return NULL;
        size_t pos = 0, cap = SEL_INIT_SIZE;

        nss_line_t *line;
        _Bool inview = !term->view && term->scrollback;
        nss_line_iter_t it = make_line_iter(inview ? term->scrollback : term->view, term->screen,
                term->vsel.n.y0 + term->scrollback_pos + inview, term->scrollback_pos + term->vsel.n.y1 + inview + 1);
        if (term->vsel.n.rect || term->vsel.n.y0 == term->vsel.n.y1) {
            while ((line = line_iter_next(&it)))
                append_line(&pos, &cap, &res, line, term->vsel.n.x0, term->vsel.n.x1 + 1);
        } else {
            while ((line = line_iter_next(&it))) {
                if (line_iter_y(&it) == term->vsel.n.y0 + term->scrollback_pos + inview)
                    append_line(&pos, &cap, &res, line, term->vsel.n.x0, line->width);
                else if(line_iter_y(&it) == term->vsel.n.y1 + term->scrollback_pos + inview && term->vsel.n.y1 != term->vsel.n.y0)
                    append_line(&pos, &cap, &res, line, 0, term->vsel.n.x1 + 1);
                else
                    append_line(&pos, &cap, &res, line, 0, line->width);
            }
        }
        res[pos -= !!pos] = '\0';
        return res;
    } else return NULL;
}

static void term_change_selection(nss_term_t *term, uint8_t state, nss_coord_t x, nss_color_t y, _Bool rectangular) {
    nss_visual_selection_t old = term->vsel;

    if (state == nss_sstate_pressed) {
        term->vsel.r.x0 = x;
        term->vsel.r.y0 = y - term->scrollback_pos;

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        if (TIMEDIFF(term->vsel.click1, now) < nss_config_integer(NSS_ICONFIG_TRIPLE_CLICK_TIME)*(SEC/1000))
            term->vsel.snap = nss_ssnap_line;
        else if (TIMEDIFF(term->vsel.click0, now) < nss_config_integer(NSS_ICONFIG_DOUBLE_CLICK_TIME)*(SEC/1000))
            term->vsel.snap = nss_ssnap_word;
        else
            term->vsel.snap = nss_ssnap_none;

        term->vsel.click1 = term->vsel.click0;
        term->vsel.click0 = now;
    }

    term->vsel.state = state;
    term->vsel.r.rect = rectangular;
    term->vsel.r.x1 = x;
    term->vsel.r.y1 = y - term->scrollback_pos;

    term_snap_selection(term);
    term_update_selection(term, &old);
}

_Bool nss_term_mouse(nss_term_t *term, nss_coord_t x, nss_coord_t y, nss_mouse_state_t mask, nss_mouse_event_t event, uint8_t button) {
    x = MIN(MAX(0, x), term->width - 1);
    y = MIN(MAX(0, y), term->height - 1);

    // TODO: Force selection

    /* Scroll view */
    if (event == nss_me_press && !(term->mode & nss_tm_altscreen) && (button == 3 || button == 4) && !(mask & nss_ms_modifer_mask)) {
        nss_term_scroll_view(term, (2 *(button == 3) - 1) * nss_config_integer(NSS_ICONFIG_SCROLL_AMOUNT));
    /* Report mouse */
    } else if (term->mode & nss_tm_mouse_mask) {
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
            term_answerback(term, CSI"<%"PRIu8";%"PRIu16";%"PRIu16"%c",
                    button, x + 1, y + 1, event == nss_me_release ? 'm' : 'M');
        } else {
            if (x >= 223 || y >= 223) return 0;
            term_answerback(term, CSI"%s%c%c%c",
                    term->inmode.keyboard_mapping == nss_km_sco ? ">M" : "M", button + ' ', x + 1 + ' ', y + 1 + ' ');
        }

        term->prev_mouse_x = x;
        term->prev_mouse_y = y;
        return 1;
    /* Or else select */
    } else if ((event == nss_me_press && button == 0) ||
               (event == nss_me_motion && mask & nss_ms_button_1) ||
               (event == nss_me_release && button == 0)) {

        if (event == nss_me_motion && term->vsel.state != nss_sstate_progress &&
                term->vsel.state != nss_sstate_pressed) return 0;
        if (event == nss_me_release && term->vsel.state != nss_sstate_progress) return 0;

        term_change_selection(term, event + 1, x, y, mask & nss_mm_mod1);

        if (event == nss_me_release) {
            term->vsel.targ = term->mode & nss_tm_select_to_clipboard ? nss_ct_clipboard : nss_ct_primary;
            nss_window_set_clip(term->win, term_selection_data(term), NSS_TIME_NOW, term->vsel.targ);
        }

        return 1;
    } else if (button == 1 && event == nss_me_release) {
        nss_window_paste_clip(term->win, nss_ct_primary);
        return 1;
    }

    return 0;
}

void nss_term_hang(nss_term_t *term) {
    if(term->fd >= 0) {
        close(term->fd);
        if (term->printerfd != STDOUT_FILENO)
            close(term->printerfd);
        term->fd = -1;
    }
    kill(term->child, SIGHUP);
}

void nss_free_term(nss_term_t *term) {
    nss_term_hang(term);
    for (nss_coord_t i = 0; i < term->height; i++) {
        term_free_line(term->screen[i]);
        term_free_line(term->back_screen[i]);
    }
    free(term->screen);
    free(term->back_screen);

    nss_line_t *next, *line = term->scrollback;
    while (line) {
        next = line->prev;
        term_free_line(line);
        line = next;
    }

    free(term->tabs);
    free(term->palette);
    free(term);
}
