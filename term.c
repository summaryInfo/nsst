#define _XOPEN_SOURCE
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>

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
    enum nss_line_mode {
        nss_lm_dirty = 1 << 0,
        nss_lm_wrapped = 1 << 1,
        nss_lm_blink = 1 << 1,
    } mode;
    nss_cell_t cell[];
} nss_line_t;

#define TTY_MAX_WRITE 256
#define NSS_FD_BUF_SZ 256
#define INIT_TAB_SIZE 8

#define IS_C1(c) ((c) < 0x100 && (c) >= 0x80)
#define IS_C0(c) (((c) < 0x20 && (c) >= 0) || (c) == 0x7f)

struct nss_term {
    nss_line_t **screen;
    int16_t saved_x;
    int16_t saved_y;
    nss_cell_t saved;
    _Bool saved_origin;

    nss_line_t **back_screen;
    int16_t back_saved_x;
    int16_t back_saved_y;
    nss_cell_t back_saved;
    _Bool back_saved_origin;

    int16_t cur_x;
    int16_t cur_y;
    nss_cell_t cur;
    _Bool cur_origin;

    int16_t width;
    int16_t height;
    int16_t top;
    int16_t bottom;

    uint8_t *tabs;
    nss_line_t *view;
    nss_line_t *scrollback;
    nss_line_t *scrollback_top;
    int32_t scrollback_limit;
    int32_t scrollback_size;

    struct timespec draw_time;

    enum nss_term_mode {
        nss_tm_echo = 1 << 0,
        nss_tm_crlf = 1 << 1,
        nss_tm_lock = 1 << 2,
        nss_tm_wrap = 1 << 3,
        nss_tm_visible = 1 << 4,
        nss_tm_focused = 1 << 5,
        nss_tm_altscreen = 1 << 6,
        nss_tm_utf8 = 1 << 7,
        nss_tm_force_redraw = 1 << 8
    } mode;


#define ESC_MAX_PARAM 16
#define ESC_MAX_INTERM 2
#define ESC_MAX_STR 256

    struct nss_escape {
        enum nss_escape_state {
            nss_es_ground,
            nss_es_escape,
            nss_es_dcs_0,
            nss_es_dcs_1,
            nss_es_dcs_2,
            nss_es_dcs_S,
            nss_es_dcs_E,
            nss_es_osc,
            nss_es_str,
            nss_es_csi_0,
            nss_es_csi_1,
            nss_es_csi_2,
            nss_es_csi_E,
        } state;
        uint8_t priv;
        uint8_t interm[ESC_MAX_INTERM];
        uint8_t func;
        uint32_t param[ESC_MAX_PARAM];
        uint8_t str[ESC_MAX_STR];
    } esc;

    nss_window_t *win;
    pid_t child;
    int fd;
    // Make this just 4 bytes for incomplete utf-8
    uint8_t fd_buf[NSS_FD_BUF_SZ];
    size_t fd_buf_pos;
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
    // TODO - Use fixed program for testing
    //if (!(sh = getenv("SHELL")))
    //    sh = pw->pw_shell[0] ? pw->pw_shell : cmd;

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

static nss_line_t *create_line(size_t width) {
    nss_line_t *line = malloc(sizeof(*line) + width * sizeof(line->cell[0]));
    if (line) {
        line->width = width;
        line->mode = nss_lm_dirty;
        line->next = line->prev = NULL;
    } else warn("Can't allocate line");
    return line;
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
        free(term->scrollback_top);
        if (next) next->prev = NULL;
        term->scrollback_top = next;
        term->scrollback_size = term->scrollback_limit;
    }
}

static void term_clear_region(nss_term_t *term, int16_t xs, int16_t ys, int16_t xe, int16_t ye) {
    xs = MAX(0, MIN(xs, term->width));
    xe = MAX(0, MIN(xe, term->width));
    ys = MAX(term->top, MIN(ys, term->bottom + 1));
    ye = MAX(term->top, MIN(ye, term->bottom + 1));

    if (ye < ys) SWAP(int16_t, ye, ys);
    if (xe < xs) SWAP(int16_t, xe, xs);

    nss_cell_t cell = term->cur;
    NSS_CELL_ATTRS_ZERO(cell);
    for (; ys < ye; ys++)
        for(int16_t i = xs; i < xe; i++)
            term->screen[ys]->cell[i] = cell;
}

static void term_move_to(nss_term_t *term, int16_t x, int16_t y) {
    term->cur_x = MIN(MAX(x, 0), term->width);
    if (term->cur_origin)
        term->cur_y = MIN(MAX(y, term->top), term->bottom);
    else
        term->cur_y = MIN(MAX(y, 0), term->height - 1);
}

static void term_cursor_mode(nss_term_t *term, _Bool mode) {
    if (mode) { //save
       term->saved = term->cur;
       term->saved_x = term->cur_x;
       term->saved_y = term->cur_y;
       term->saved_origin = term->cur_origin;
    } else { //restore
       term->cur = term->saved;
       term->cur_x = term->saved_x;
       term->cur_y = term->saved_y;
       term->cur_origin = term->saved_origin;
    }
    term_move_to(term, term->cur_x, term->cur_y);
}

static void term_swap_screen(nss_term_t *term) {

    term->mode ^= nss_tm_altscreen;

    SWAP(int16_t, term->back_saved_x, term->saved_x);
    SWAP(int16_t, term->back_saved_y, term->saved_y);
    SWAP(nss_cell_t, term->back_saved, term->saved);
    SWAP(_Bool, term->back_saved_origin, term->saved_origin);

    SWAP(nss_line_t **, term->back_screen, term->screen);
    term->view = NULL;
}

void nss_term_scroll_view(nss_term_t *term, int16_t amount) {
    nss_line_t *old_view = term->view;
    if (amount > 0) {
        if (!term->view) {
            term->view = term->scrollback;
            amount--;
        }
        while (amount-- && term->view->prev)
            term->view = term->view->prev;
    } else if (amount < 0) {
        while (amount++ && term->view)
            term->view = term->view->next;
    }
    if (term->view != old_view) {
        // @REDRAW
        nss_term_redraw(term, (nss_rect_t) {0, 0, term->width, term->height}, 1);
        nss_window_draw_commit(term->win);
    }
}

static void term_scroll(nss_term_t *term, int16_t top, int16_t bottom, int16_t amount, _Bool save) {
    if (amount > 0) { /* up */
        amount = MIN(amount, (bottom - top + 1));
        size_t rest = (bottom - top + 1) - amount;

        if (save && !(term->mode & nss_tm_altscreen)) {
            for (size_t i = 0; i < (size_t)amount; i++) {
                term_append_history(term, term->screen[top + i]);
                term->screen[top + i] = create_line(term->width);
            }
        }

        for (size_t i = 0; i < rest; i++)
            SWAP(nss_line_t *, term->screen[i], term->screen[amount + i]);

        term_clear_region(term, 0, bottom + 1 - amount, term->width, bottom + 1);

    } else { /* down */
        amount = MIN(-amount, bottom - top + 1);
        size_t rest = bottom - top + 1 - amount;

        for (size_t i = 0; i < rest; i++)
            SWAP(nss_line_t *, term->screen[bottom - i], term->screen[bottom - amount - i]);

        term_clear_region(term, 0, top, term->width, top + amount);
    }

    term->view = NULL;
    term->mode |= nss_tm_force_redraw;
}

static void term_insert_cells(nss_term_t *term, int16_t n) {
    int16_t cx = MIN(term->cur_x, term->width - 1);
    n = MAX(0, MIN(n, term->width - cx));
    nss_line_t *line = term->screen[term->cur_y];
    memmove(line->cell + cx + n, line->cell + cx, (term->width - cx - n) * sizeof(nss_cell_t));
    term_clear_region(term, cx, term->cur_y, term->width, term->cur_y + 1);
}

static void term_delete_cells(nss_term_t *term, int16_t n) {
    int16_t cx = MIN(term->cur_x, term->width - 1);
    n = MAX(0, MIN(n, term->width - cx));
    nss_line_t *line = term->screen[term->cur_y];
    memmove(line->cell + cx, line->cell + cx + n, (term->width - cx - n) * sizeof(nss_cell_t));
    term_clear_region(term, term->width - n, term->cur_y, term->width, term->cur_y + 1);
}

static void term_insert_lines(nss_term_t *term, int16_t n) {
    if (term->cur_y >= term->top && term->cur_y <= term->bottom)
        term_scroll(term, term->cur_y, term->bottom, n, 0);
}

static void term_delete_lines(nss_term_t *term, int16_t n) {
    if (term->cur_y >= term->top && term->cur_y <= term->bottom)
        term_scroll(term, term->cur_y, term->bottom, -n, 0);
}

static void term_newline(nss_term_t *term, _Bool cr) {
    if (term->cur_y == term->height - 1) {
        term_scroll(term,  term->top, term->bottom, 1, 1);
        term_move_to(term, cr ? 0 : term->cur_x, term->cur_y);
    } else {
        term_move_to(term, cr ? 0 : term->cur_x, term->cur_y + 1);
    }
}

static void term_putchar(nss_term_t *term, uint32_t ch) {
    if (term->mode & nss_tm_wrap && term->cur_x >= term->width) {
        term->screen[term->cur_y]->mode |= nss_lm_wrapped;
        term_newline(term, 1);
    }
    term->screen[term->cur_y]->cell[term->cur_x++] = NSS_MKCELLWITH(term->cur, ch);
    term->screen[term->cur_y]->mode |= nss_lm_dirty;
}

static void term_reset_tabs(nss_term_t *term) {
    memset(term->tabs, 0, term->width * sizeof(term->tabs[0]));
    for(size_t i = INIT_TAB_SIZE; i < (size_t)term->width; i += INIT_TAB_SIZE)
        term->tabs[i] = 1;
}

static void term_tabs(nss_term_t *term, int16_t n) {
    if (n >= 0) {
        if(term->cur_x < term->width) term->cur_x++;
        while(n--)
            while(term->cur_x < term->width && !term->tabs[term->cur_x])
                term->cur_x++;
    } else {
        if(term->cur_x > 0) term->cur_x--;
        while(n++)
            while(term->cur_x > 0 && !term->tabs[term->cur_x])
                term->cur_x--;
    }
}

static _Bool term_handle(nss_term_t *term, uint32_t ch) {
    switch (ch) {
    case '\t': // HT
        term_tabs(term, 1);
        return 1;
    case '\b': // BS
        term_move_to(term, term->cur_x - 1, term->cur_y);
        return 1;
    case '\r': // CR
        term_move_to(term, 0, term->cur_y);
        return 1;
    case '\f': // FF
    case '\v': // VT
    case '\n': // LF
        term_newline(term, term->mode & nss_tm_crlf);
        return 1;
    case '\a': // BEL
        //Ignore for now
        return 1;
    case '\e': // ESC
        //Ignore for now
        return 1;
    case '\016': // S0/LS0
    case '\017': // SI/LS1
        //Ignore for now
        return 1;
    case '\032': // SUB
        //Ignore for now
    case '\030': // CAN
        //Ignore for now
        return 1;
    case '\005': // ENQ (IGNORE)
    case '\000': // NUL (IGNORE)
    case '\021': // XON (IGNORE)
    case '\023': // XOFF (IGNORE)
    case 0x7f:   // DEL (IGNORE)
        return 1;
    case 0x80:   // PAD - TODO
    case 0x81:   // HOP - TODO
    case 0x82:   // BPH - TODO
    case 0x83:   // NBH - TODO
        return 1;
    case 0x84:   // IND - Index
        term_newline(term, 0);
        return 1;
    case 0x85:   // NEL -- Next line
        term_newline(term, 1);
        return 1;
    case 0x86:   // SSA - TODO
    case 0x87:   // ESA - TODO
        return 1;
    case 0x88:   // HTS -- Horizontal tab stop
        if(term->cur_x < term->width)
            term->tabs[term->cur_x] = 1;
        return 1;
    case 0x89:   // HTJ - TODO
    case 0x8a:   // VTS - TODO
    case 0x8b:   // PLD - TODO
    case 0x8c:   // PLU - TODO
    case 0x8d:   // RI - TODO
    case 0x8e:   // SS2 - TODO
    case 0x8f:   // SS3 - TODO
    case 0x91:   // PU1 - TODO
    case 0x92:   // PU2 - TODO
    case 0x93:   // STS - TODO
    case 0x94:   // CCH - TODO
    case 0x95:   // MW - TODO
    case 0x96:   // SPA - TODO
    case 0x97:   // EPA - TODO
    case 0x98:   // SOS - TODO
    case 0x99:   // SGCI - TODO
        return 1;
    case 0x9a:   // DECID -- Identify Terminal
        //Ignore for now
        return 1;
    case 0x9b:   // CSI - TODO
    case 0x9c:   // ST - TODO
        return 1;
    case 0x90:   // DCS -- Device Control String
    case 0x9d:   // OSC -- Operating System Command
    case 0x9e:   // PM -- Privacy Message
    case 0x9f:   // APC -- Application Program Command
        //Ignore for now
        return 1;
    default:
        return 0;
    }
}

static ssize_t term_write(nss_term_t *term, const uint8_t *buf, size_t len, _Bool show_ctl) {
    const uint8_t *end = buf + len, *start = buf;

    nss_term_redraw(term, (nss_rect_t) { MIN(term->cur_x, term->width - 1), term->cur_y, 2, 1}, 0);

    while (start < end) {
        uint32_t ch;
        // consider sixel here
        if (!(term->mode & nss_tm_utf8))  ch = *buf++;
        else if (!utf8_decode(&ch, &start, end))  break;

        info("UTF %"PRIx32, ch);

        // Parse here
        // TODO Redo drawing to damage based

        if (!term_handle(term, ch))
            term_putchar(term, ch);
    }

    struct timespec cur;
    clock_gettime(CLOCK_MONOTONIC, &cur);
    long long ms_diff = ((cur.tv_sec - term->draw_time.tv_sec) * 1000000000 +
            (cur.tv_nsec - term->draw_time.tv_nsec)) / 1000;
    if (ms_diff > (1000000/NSS_TERM_FPS)) {
        term->draw_time = cur;
        nss_term_redraw_dirty(term, 1);
        nss_window_draw_commit(term->win);
    }

    return start - buf;
}

ssize_t nss_term_read(nss_term_t *term) {
    if (term->fd == -1) return -1;
    ssize_t res;
    if ((res = read(term->fd, term->fd_buf + term->fd_buf_pos, NSS_FD_BUF_SZ - term->fd_buf_pos)) < 0) {
        warn("Can't read from tty");
        nss_term_hang(term);
    }
    term->fd_buf_pos += res;
    ssize_t disp = term_write(term, term->fd_buf, term->fd_buf_pos, 0);
    info("Disp: %zd", disp);
    term->fd_buf_pos -= disp;
    if (term->fd_buf_pos > 0)
        memmove(term->fd_buf, term->fd_buf + disp, term->fd_buf_pos);
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
            break;
        }
        if (pfd.revents & POLLOUT) {
            if ((res = write(term->fd, buf, MIN(lim, len))) < 0) {
                warn("Can't read from tty");
                nss_term_hang(term);
                break;
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

    if (term->view) term->mode |= nss_tm_force_redraw;
    term->view = NULL;

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
    int16_t ow = term->width, oh = term->height;
    int16_t ox = term->cur_x, oy = term->cur_y;

    // Free extra lines, scrolling screen upwards

    if (term->height > height) {
        if (term->mode & nss_tm_altscreen)
            SWAP(nss_line_t **, term->screen, term->back_screen);

        int16_t delta = MAX(0, term->cur_y - height + 1);

        if (delta) term->mode |= nss_tm_force_redraw;

        for (int16_t i = height; i < term->height; i++) {
            if (i < height + delta)
                term_append_history(term, term->screen[i - height]);
            else
                free(term->screen[i]);
            free(term->back_screen[i]);
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
            term->screen[i] = create_line(width);
            term->back_screen[i] = create_line(width);
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

    for (size_t i = 0; i < minh; i++) {
        if (term->screen[i]->width < (size_t)width) {
            info("Resize to %hd, whilst having %zd", width, term->screen[i]->width);
            nss_line_t *new = realloc(term->screen[i], sizeof(*new) + width * sizeof(new->cell[0]));
            if (!new) die("Can't create lines");
            new->mode |= nss_lm_dirty;
            term->screen[i] = new;
            term_clear_region(term, new->width, i, width, i + 1);
            new->width = width;
        }
        if (term->back_screen[i]->width < (size_t)width) {
            nss_line_t *new = realloc(term->back_screen[i], sizeof(*new) + width * sizeof(new->cell[0]));
            if (!new) die("Can't create lines");
            new->mode |= nss_lm_dirty;
            term->back_screen[i] = new;
            term_clear_region(term, new->width, i, width, i + 1);
            new->width = width;
        }
    }

    // Reset scroll region

    term->top = 0;
    term->bottom = height - 1;
    term_move_to(term, term->cur_x, term->cur_y);

    // Clear new regions

    for (size_t i = 0; i < 2; i++) {
        if (height > oh) term_clear_region(term, 0, oh, width, height);
        SWAP(nss_line_t **, term->screen, term->back_screen);
    }

    if(ow == ox || ox != term->cur_x || oy != term->cur_y)
        term->mode |= nss_tm_force_redraw;
}

nss_term_t *nss_create_term(nss_window_t *win, int16_t width, int16_t height) {
    nss_term_t *term = calloc(1, sizeof(nss_term_t));

    term->win = win;
    term->mode = nss_tm_wrap | nss_tm_visible | nss_tm_utf8;
    term->fd = -1;
    term->scrollback_limit = -1;
    term->cur = term->back_saved = term->saved = NSS_MKCELL(7, 0, 0, ' ');
    clock_gettime(CLOCK_MONOTONIC, &term->draw_time);

                //    +-- This is temoporal
                //    |
                //    V
    term_resize(term, MAX('~' - '!' + 1, width), height);

    if (tty_open(term, "./testcmd", NULL) < 0) {
        warn("Can't create tty");
        nss_free_term(term);
        return NULL;
    }

    { /* Sample screen */

        info("Term w=%"PRId16" h=%"PRId16, width, height);

        nss_attrs_t test[] = {
            nss_attrib_italic | nss_attrib_bold,
            nss_attrib_italic | nss_attrib_underlined,
            nss_attrib_strikethrough | nss_attrib_blink,
            nss_attrib_underlined | nss_attrib_inverse,
            0
        };
        for (size_t k = 0; k < (size_t)MIN(5,term->height); k++) {
            for (size_t i = 0; i <= (size_t)('~' - '!'); i++)
                term->screen[k]->cell[i] = NSS_MKCELL(7, 0, test[k], i + '!');
        }
        term->screen[2]->mode |= nss_lm_blink;
        term->screen[1]->mode |= nss_lm_blink;
        term->screen[0]->cell[13] = NSS_MKCELL(3, 5, test[3], 'A');
        term->screen[1]->cell[16] = NSS_MKCELL(4, 6, test[2], 'A');
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
            if (view->width > (size_t)damage.x)
                nss_window_draw(term->win, damage.x, y0,
                        MIN(view->width - damage.x, damage.width), view->cell + damage.x);
        }
        for (int16_t y = 0; y < term->height && y + y0 < damage.height + damage.y; y++) {
            if (term->screen[y]->width > (size_t)damage.x)
                nss_window_draw(term->win, damage.x, y0 + y,
                        MIN(term->screen[y]->width - damage.x, damage.width), term->screen[y]->cell + damage.x);
        }
        if (cursor && !term->view && damage.x <= term->cur_x && term->cur_x <= damage.x + damage.width &&
                damage.y <= term->cur_y && term->cur_y <= damage.y + damage.height) {
            int16_t cx = MIN(term->cur_x, term->width - 1);
            nss_window_draw_cursor(term->win, term->cur_x, term->cur_y, &term->screen[term->cur_y]->cell[cx]);
        }

        nss_window_update(term->win, 1, &damage);
    }
}

void nss_term_redraw_dirty(nss_term_t *term, _Bool cursor) {
    if (!(term->mode & nss_tm_visible)) return;

    int16_t y0 = 0;
    nss_line_t *view = term->view;
    for (; view && y0 < term->height; y0++, view = view->next) {
        if (view->mode & (nss_lm_dirty | nss_lm_blink) || term->mode & nss_tm_force_redraw)
            nss_window_draw(term->win, 0, y0, term->width, view->cell);
        view->mode &= ~nss_lm_dirty;
    }

    for (int16_t y = 0; y + y0 < term->height; y++) {
        if (term->screen[y]->mode & (nss_lm_dirty | nss_lm_blink) || term->mode & nss_tm_force_redraw)
            nss_window_draw(term->win, 0, y0 + y, term->width, term->screen[y]->cell);
        term->screen[y]->mode &= ~nss_lm_dirty;
    }

    term->mode &= ~nss_tm_force_redraw;

    if (cursor && !term->view) {
        int16_t cx = MIN(term->cur_x, term->width - 1);
        nss_window_draw_cursor(term->win, term->cur_x, term->cur_y, &term->screen[term->cur_y]->cell[cx]);
    }

    nss_window_update(term->win, 1, &(nss_rect_t){0, 0, term->width, term->height});
}

void nss_term_resize(nss_term_t *term, int16_t width, int16_t height) {
    info("Resize: w=%"PRId16" h=%"PRId16, width, height);

    term_resize(term, width, height);

    nss_term_redraw_dirty(term, 1);
    nss_window_draw_commit(term->win);

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
    nss_term_redraw(term, (nss_rect_t) {term->cur_x, term->cur_y, 1, 1}, 1);
    nss_window_draw_commit(term->win);
}

void nss_term_visibility(nss_term_t *term, _Bool visible) {
    if (visible) {
        term->mode |= nss_tm_visible;
        nss_term_redraw(term, (nss_rect_t) {0, 0, term->width, term->height}, 1);
    } else term->mode &= ~nss_tm_visible;
}

void nss_free_term(nss_term_t *term) {
    nss_term_hang(term);
    for (size_t i = 0; i < (size_t)term->height; i++) {
        free(term->screen[i]);
        free(term->back_screen[i]);
    }
    free(term->screen);
    free(term->back_screen);

    nss_line_t *next, *line = term->scrollback;
    while (line) {
        next = line->prev;
        // TODO: Deref all attribs in line here
        free(line);
        line = next;
    }

    free(term->tabs);
    free(term);
}
