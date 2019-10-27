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
#define TTY_MAX_WRITE 256
#define NSS_FD_BUF_SZ 256

struct nss_term {
    int16_t cur_x;
    int16_t cur_y;
    uint8_t cur_attrs;
    nss_short_cid_t cur_fg;
    nss_short_cid_t cur_bg;

    int16_t width;
    int16_t height;

    nss_window_t *win;
    nss_context_t *con;
    nss_line_t *screen;
    nss_line_t *cur_line;
    nss_line_t *altscreen;

    uint32_t utf_part;
    int8_t utf_rest;

    enum nss_term_mode {
        nss_tm_echo = 1 << 0, //This is done by driver, done
        nss_tm_crlf = 1 << 1, //This also, and non-printable to printable conversion, done
        nss_tm_lock = 1 << 2,
        nss_tm_wrap = 1 << 3, //Done
        nss_tm_visible = 1 << 4,
        nss_tm_focused = 1 << 5,
        nss_tm_altscreen = 1 << 6,
        nss_tm_utf8 = 1 << 7,
    } mode;

    pid_t child;
    int fd;
    uint8_t fd_buf[NSS_FD_BUF_SZ];
    size_t fd_buf_pos;
};

// Need term_mouse
// erase_line erase_cell erase_screen
// delete_line delete_cell
// insert_line insert_cell
// copy_line copy_cell copy_screen


static void sigchld_fn(int arg) {
    int status;
    pid_t pid = waitpid(-1, &status, WNOHANG);
    if (pid < 0) {
        // Thats unsafe
        warn("Child wait failed");
        return;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status))
        warn("Child exited with status: %d", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        warn("Child terminated due to the signal: %d", WTERMSIG(status));
}

static void exec_shell(char *cmd, char **args) {

    const struct passwd *pw;
    errno = 0;
    if (!(pw = getpwuid(getuid()))) {
        if (errno) die("getpwuid(): %s", strerror(errno));
        else die("I don't know you");
    }

    char *sh = cmd;
    // TODO
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

static size_t utf8_encode(uint32_t u, uint8_t *buf) {
    static const uint32_t utf8_min[] = {0x80, 0x800, 0x10000, 0x11000};
    static const uint8_t utf8_mask[] = {0x00, 0xc0, 0xe0, 0xf0};
    if (u > 0x10ffff) u = UTF_INVAL;
    size_t i = 0, j;
    while (u > utf8_min[i++]);
    for (j = i; j > 1; j--) {
        buf[j - 1] = (u & 0x3f) | 0x80;
        u >>= 6;
    }
    buf[0] = u | utf8_mask[i - 1];
    return i;
}

static _Bool utf8_decode(nss_term_t *term, uint32_t *res, const uint8_t **buf) {
    if (**buf >= 0x80 && **buf < 0xc0) {
        uint8_t ch = *(*buf)++;
        if (!term->utf_rest) {
            *res = UTF_INVAL;
            return 1;
        }
        term->utf_part <<= 6;
        term->utf_part |= ch & ~0xc0;
        if (!--term->utf_rest) {
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
    } while ((i /= 2) & term->utf_part);
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

static nss_line_t *create_line(nss_line_t *prev, nss_line_t *next, size_t width) {
    nss_line_t *line = malloc(sizeof(nss_line_t) + width*sizeof(line->cell[0]));
    if (!line) {
        warn("Can't allocate line");
        return NULL;
    }
    for (size_t i = 0; i < width; i++) {
        line->cell[i].ch = 0;
        line->cell[i].bg = 0;
        line->cell[i].fg = 7;
    }
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
    if (!line) {
        return NULL;
        warn("Can't allocate line");
    }
    memcpy(new, line, sizeof(nss_line_t) + MIN(width, line->width)*sizeof(line->cell[0]));
    if (width > line->width) {
        for (size_t i = line->width; i < width; i++) {
            new->cell[i].ch = 0;
            new->cell[i].bg = 0;
            new->cell[i].fg = 7;
        }
    }
    new->width = width;

    if (line->prev) line->prev->next = new;
    if (line->next) line->next->prev = new;

    free(line);

    return new;
}

static void term_moveto(nss_term_t *term, int16_t x, int16_t y) {
    x = MIN(MAX(x, 0), term->width - 1);
    y = MIN(MAX(y, 0), term->height - 1);
    //TODO cursor origin
    term->cur_x = x;
    term->cur_y = y;
}

static void term_adjline(nss_term_t *term) {
    if (term->cur_line->width < (size_t)term->width) {
        nss_line_t *line = term->cur_line;
        term->cur_line = resize_line(line, term->width);
        if (line == term->screen)
            term->screen = term->cur_line;
        if (line == term->altscreen)
            term->altscreen = term->cur_line;
    }
}

static void term_adjscreen(nss_term_t *term, _Bool force) {
    // Scroll terminal display position to screen
    // Something messy is happenning
    // if not force
        // if cursor on last line -> scroll up
    // else scroll to cursor

    // TODO: Create sceen line index array

    nss_line_t *new_screen = term->cur_line;
    int16_t new_y = 0;
    for (size_t i = term->height - 1; i && new_screen->prev; i--) {
        new_screen = new_screen->prev;
        new_y++;
    }
    if (force || term->cur_y > term->height - 1) {
        if (term->mode & nss_tm_altscreen) {
            nss_line_t *ln = term->screen->prev;
            while (ln) {
                nss_line_t *tmp = ln->prev;
                free(ln);
                ln = tmp;
            }
        }
        if (term->screen != new_screen) {
            term->screen = new_screen;
            term->cur_y = new_y;
            nss_term_redraw(term, (nss_rect_t) {0, 0, term->width, term->height}, 1);
        }
    }
}

static void term_newline(nss_term_t *term, _Bool cr) {
    if (!term->cur_line->next) {
        term->cur_line->next = create_line(term->cur_line, NULL, term->width);
        if (term->mode & nss_tm_altscreen) {
            nss_line_t *top = term->screen;
            while (top->prev) top = top->prev;
            if (top->next) top->next->prev = NULL;
            if (top == term->screen) term->screen = top->next;
            free(top);
        }
    }
    term->cur_line = term->cur_line->next;
    term_moveto(term, 0, term->cur_y + 1);
    term_adjscreen(term, 0);
}

static void term_putchar(nss_term_t *term, uint32_t ch) {
    if (term->mode & nss_tm_wrap) {
        if (term->cur_x == term->width)
            term_newline(term, 1);
        term->cur_x++;
    }
    term_adjline(term);

    term->cur_line->cell[term->cur_x - 1] = NSS_MKCELL(term->cur_fg, term->cur_bg, term->cur_attrs, ch);
    nss_term_redraw(term, (nss_rect_t) {term->cur_x - 1, term->cur_y, 2, 1}, 1);
}

static _Bool term_handle(nss_term_t *term, uint32_t ch) {
    if (term->mode & nss_tm_utf8 && ch > 0x7f) return 0;
    switch (ch) {
    case '\t': // HT
        //Ignore for now
        return 1; 
    case '\b': // BS
        term_moveto(term, term->cur_x - 1, term->cur_y); 
        return 1;
    case '\r': // CR
        term_moveto(term, 0, term->cur_y);
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
    case 0x84:   // IND - TODO
        return 1;
    case 0x85:   // NEL -- Next line
        return 1;
    case 0x86:   // SSA - TODO 
    case 0x87:   // ESA - TODO 
        return 1;
    case 0x88:   // HTS -- Horizontal tab stop
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
        return 1;
    case 0x9b:   // CSI - TODO
    case 0x9c:   // ST - TODO
        return 1;
    case 0x90:   // DCS -- Device Control String
    case 0x9d:   // OSC -- Operating System Command
    case 0x9e:   // PM -- Privacy Message
    case 0x9f:   // APC -- Application Program Command
        return 1;
    default:
        return 0;
    }
}

static ssize_t term_write(nss_term_t *term, const uint8_t *buf, size_t len, _Bool show_ctl) {
    const uint8_t *end = buf + len, *start = buf, *prev = buf;
    uint32_t ch;
    while (start < end) {
        if (utf8_decode(term, &ch, &start)) {
            uint8_t bufs[5];
            bufs[utf8_encode(ch, bufs)] = '\0';
            if (!utf8_check(ch, start - prev)) ch = UTF_INVAL;
            info("Decoded: 0x%"PRIx32", %s", ch, bufs);
            nss_term_redraw(term, (nss_rect_t) { term->cur_x, term->cur_y, 1, 1}, 0);
            if (ch > 0xff || !term_handle(term, ch))
                term_putchar(term, ch);
            else
                nss_term_redraw(term, (nss_rect_t) { term->cur_x, term->cur_y, 1, 1}, 1);
            //nss_window_draw_commit(term->con, term->win);
            info("Current line length: %zu", term->cur_line->width);

            prev = start;
        }
    }
    return start - buf;
}

ssize_t nss_term_read(nss_term_t *term) {
    if (term->fd == -1) return -1;
    ssize_t res;
    if ((res = read(term->fd, term->fd_buf + term->fd_buf_pos, NSS_FD_BUF_SZ - term->fd_buf_pos)) < 0) {
        warn("Can't read from tty");
        close(term->fd);
        term->fd = -1;
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
            close(term->fd);
            term->fd = -1;
            nss_term_hang(term);
        }
        if (pfd.revents & POLLOUT) {
            if ((res = write(term->fd, buf, MIN(lim, len))) < 0) {
                warn("Can't read from tty");
                close(term->fd);
                nss_term_hang(term);
                term->fd = -1;
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
    term_adjscreen(term, 1);

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
    kill(term->child, SIGHUP);
}

int nss_term_fd(nss_term_t *term) {
    return term->fd;
}

nss_term_t *nss_create_term(nss_context_t *con, nss_window_t *win, int16_t width, int16_t height) {
    nss_term_t *term = malloc(sizeof(nss_term_t));

    term->width = width;
    term->height = height;
    term->win = win;
    term->con = con;
    term->mode = nss_tm_wrap | nss_tm_visible | nss_tm_utf8;

    term->utf_part = 0;
    term->utf_rest = 0;

    term->fd_buf_pos = 0;
    term->child = 0;
    term->fd = -1;

    term->cur_x = 0;
    term->cur_y = 0;
    term->cur_attrs = 0;
    term->cur_fg = 7;
    term->cur_bg = 0;
    term->altscreen = create_line(NULL, NULL, width);

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

    if (tty_open(term, "./testcmd", NULL) < 0) {
        warn("Can't create tty");
        nss_free_term(term);
        return NULL;
    }

    return term;
}

#define ALLOC_STEP 16

void nss_term_redraw(nss_term_t *term, nss_rect_t damage, _Bool cursor) {
    if (!(term->mode & nss_tm_visible)) return;
    if (intersect_with(&damage, &(nss_rect_t) {0, 0, term->width, term->height})) {
        //Clear undefined areas
        nss_window_clear(term->con, term->win, 1, &damage);

        nss_line_t *line = term->screen;
        size_t j = 0;
        for (; line && j < (size_t)damage.y; j++, line = line->next);
        for (; line && j < (size_t)damage.height + damage.y; j++, line = line->next) {
            if ((size_t)damage.x < line->width) {
                nss_window_draw(term->con, term->win, damage.x, j, MIN(line->width - damage.x, damage.width), line->cell + damage.x);
                if (cursor && line == term->cur_line && damage.x <= term->cur_x && term->cur_x <= damage.x + damage.width) {
                    int16_t cx = MIN(term->cur_x, term->width - 1);
                    nss_window_draw_cursor(term->con, term->win, cx, term->cur_y, &term->cur_line->cell[cx]);
                }
            }
        }
        nss_window_update(term->con, term->win, 1, &damage);
        nss_window_draw_commit(term->con, term->win);
    }
}

void nss_term_resize(nss_term_t *term, int16_t width, int16_t height) {
    term->width = width;
    term->height = height;
    info("Resize: w=%"PRId16" h=%"PRId16, width, height);
    info("Cursor: x=%"PRId16" y=%"PRId16, term->cur_x, term->cur_y);

    //TODO Resize
    term_adjscreen(term, 0);

    // Pixel sizes are unused
    // TODO: Set them correctly
    struct winsize wsz;
    wsz.ws_xpixel = wsz.ws_col = width;
    wsz.ws_ypixel = wsz.ws_row = height;
    if (ioctl(term->fd, TIOCSWINSZ, &wsz) < 0)
        warn("Can't change tty size");
}

void nss_term_focus(nss_term_t *term, _Bool focused) {
    if (focused) term->mode |= nss_tm_focused;
    else term->mode &= ~nss_tm_focused;
    nss_term_redraw(term, (nss_rect_t) {term->cur_x, term->cur_y, 1, 1}, 1);
    nss_window_draw_commit(term->con, term->win);
}

void nss_term_visibility(nss_term_t *term, _Bool visible) {
    if (visible) {
        term->mode |= nss_tm_visible; 
        nss_term_redraw(term, (nss_rect_t) {0, 0, term->width, term->height}, 1);
    } else term->mode &= ~nss_tm_visible;
}

void nss_free_term(nss_term_t *term) {
    if (term->fd >= 0) close(term->fd);
    nss_term_hang(term);
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
