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
    enum nss_line_mode {
        nss_lm_dirty = 1 << 0,
        nss_lm_wrapped = 1 << 1,
    } mode;
    nss_cell_t cell[];
} nss_line_t;

#define UTF8_MAX_LEN 4
#define UTF_INVAL 0xfffd
#define TTY_MAX_WRITE 256
#define NSS_FD_BUF_SZ 256
#define INIT_TAB_SIZE 8

struct nss_term {
    int16_t cur_x;
    int16_t cur_y;
    nss_cell_t cur;

    int16_t width;
    int16_t height;

    nss_window_t *win;

    uint8_t *tabs;
    nss_line_t *view;
    nss_line_t **back_screen;
    nss_line_t **display;

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

    //TODO Put parser state here

    pid_t child;
    int fd;
    // Make this just 4 bytes for incomplete utf-8
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

static _Bool utf8_decode(nss_term_t *term, uint32_t *res, const uint8_t **buf, const uint8_t *end) {
    if (*buf >= end) return 0;
    uint32_t part = *(*buf)++;
    uint8_t len = 0, i = 0x80;
    if (part > 0xf7) {
        *res = UTF_INVAL;
    } else {
        while (part & i) {
            len++;
            part &= ~i;
            i /= 2;
        }
        if (len == 1) {
            part = UTF_INVAL;
        }  else if (len > 1) {
            uint8_t i = --len;
            if (end - *buf < i) return 0;
            while (i--) {
                if ((**buf & 0xc0) != 0x80) {
                    part = UTF_INVAL;
                    goto end;
                }
                part = (part << 6) + (*(*buf)++ & ~0xc0);
            }
            if(part >= 0xd800 && part < 0xe000 &&
                    part >= (uint32_t[]){0x80, 0x800, 0x10000, 0x11000}[len - 1])
                part = UTF_INVAL;
        }
    }
end:
    *res = part;
    return 1;
}

static nss_line_t *create_line(nss_line_t *prev, nss_line_t *next, size_t width) {
    nss_line_t *line = malloc(sizeof(nss_line_t) + width*sizeof(line->cell[0]));
    if (!line) {
        warn("Can't allocate line");
        return NULL;
    }

    line->width = width;
    line->mode = nss_lm_dirty;

    // TODO Set this to default bg/fg
    for (size_t i = 0; i < width; i++)
        line->cell[i] = NSS_MKCELL(7, 0, 0, ' ');

    line->prev = prev;
    if (prev) prev->next = line;
    line->next = next;
    if (next) next->prev = line;
    return line;
}

static nss_line_t *resize_line(nss_line_t *line, size_t width) {
    info("Resize: %zu", width);
    nss_line_t *new = realloc(line, sizeof(nss_line_t) + width*sizeof(line->cell[0]));
    if (!new) {
        warn("Can't allocate line");
        return line;
    }

    // TODO Set this to default bg/fg
    for (size_t i = new->width; i < width; i++)
        new->cell[i] = NSS_MKCELL(7, 0, 0, ' ');
    if (new->prev) new->prev->next = new;
    if (new->next) new->next->prev = new;

    new->width = width;

    return new;
}

static void term_moveto(nss_term_t *term, int16_t x, int16_t y) {
    term->cur_x = MIN(MAX(x, 0), term->width);
    term->cur_y = MIN(MAX(y, 0), term->height - 1);
    //TODO cursor origin
}

void nss_term_scroll_view(nss_term_t *term, int16_t amount) {
    nss_line_t *old_view = term->view;
    if (amount > 0) {
        do {
            if (!term->view->next || term->view == term->display[0]) break;
            term->view = term->view->next;
        } while (--amount);
    } else if (amount < 0) {
        do {
            if (!term->view->prev) break;
            term->view = term->view->prev;
        } while (++amount);
    }
    if (term->view != old_view) {
        nss_term_redraw(term, (nss_rect_t) {0, 0, term->width, term->height}, 1);
        nss_window_draw_commit(term->win);
    }
}

static void term_scroll(nss_term_t *term, int16_t amount) {
    _Bool inview = term->display[0] == term->view;
    if (amount > 0) { //up
        amount = MIN(amount, term->height);
        size_t rest = term->height - amount;
        nss_line_t *prev = term->display[term->height - 1]->prev;

        memmove(term->display, term->display + amount, rest * sizeof(*term->display));

        while (rest < (size_t)term->height)
            term->display[rest++] = prev = create_line(prev, NULL, term->width);
    } else if (amount < 0) { //down
        // Scroll down doesn't affect history now

        amount = MIN(-amount, term->height);
        size_t rest = term->height - amount;
        nss_line_t *last = term->display[rest]->prev;

        if (last) last->next = NULL;
        last = rest ? term->display[0] : NULL;
        nss_line_t *first = term->display[0]->prev;

        for (size_t j = rest; j < (size_t)term->height; j++)
            free(term->display[j]);

        memmove(term->display + amount, term->display, rest * sizeof(*term->display));

        for (size_t j = 0; j < (size_t)amount; j++)
            term->display[j] = first =  create_line(first, NULL, term->width);

        if (first) first->next = last;
        if (last) last->prev = first;
    }
    if (inview) term->view = term->display[0];
    nss_term_redraw(term, (nss_rect_t) {0, 0, term->width, term->height}, 1);
}

static void term_newline(nss_term_t *term, _Bool cr) {
    if (term->cur_y == term->height - 1) {
        term_scroll(term, 1);
        term_moveto(term, cr ? 0 : term->cur_x, term->cur_y);
    } else
        term_moveto(term, cr ? 0 : term->cur_x, term->cur_y + 1);
}

static void term_putchar(nss_term_t *term, uint32_t ch) {
    if (term->mode & nss_tm_wrap && term->cur_x >= term->width) {
        term->display[term->cur_y]->mode |= nss_lm_wrapped;
        term_newline(term, 1);
    }
    term->cur_x++;
    nss_cell_t cell = term->cur;
    cell.ch |= ch;
    term->display[term->cur_y]->cell[term->cur_x - 1] = cell;
    nss_term_redraw(term, (nss_rect_t) {term->cur_x - 1, term->cur_y, 2, 1}, 1);
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
    if (term->mode & nss_tm_utf8 && ch > 0x7f) return 0;//TODO I suppose this doesn't work
    switch (ch) {
    case '\t': // HT
        term_tabs(term, 1);
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
    uint32_t ch;
    while (utf8_decode(term, &ch, &start, end)) {

        uint8_t bufs[5];
        bufs[utf8_encode(ch, bufs)] = '\0';
        info("Decoded: 0x%"PRIx32", %s", ch, bufs);

        nss_term_redraw(term, (nss_rect_t) { MIN(term->cur_x, term->width - 1), term->cur_y, 1, 1}, 0);
        if (!term_handle(term, ch))
            term_putchar(term, ch);
        else
            nss_term_redraw(term, (nss_rect_t) { MIN(term->cur_x, term->width - 1), term->cur_y, 1, 1}, 1);
        nss_window_draw_commit(term->win);

        info("Current line length: %zu, %"PRId16" %"PRId16 , term->width, term->cur_x, term->cur_y);
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
    term_scroll(term, 0);

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

nss_term_t *nss_create_term(nss_window_t *win, int16_t width, int16_t height) {
    nss_term_t *term = malloc(sizeof(nss_term_t));

    term->width = width;
    term->height = height;
    term->win = win;
    term->mode = nss_tm_wrap | nss_tm_visible | nss_tm_utf8 | nss_tm_altscreen;

    term->fd_buf_pos = 0;
    term->child = 0;
    term->fd = -1;

    term->cur_x = 0;
    term->cur_y = 0;
    term->cur = NSS_MKCELL(7, 0, 0, 0);

    term->tabs = calloc(width, sizeof(term->tabs[0]));
    if(!term->tabs) die("Can't alloc tabs");
    term_reset_tabs(term);

    //term->back_screen;

    nss_attrs_t test[] = {
        nss_attrib_italic | nss_attrib_bold,
        nss_attrib_italic | nss_attrib_underlined,
        nss_attrib_strikethrough,
        nss_attrib_underlined | nss_attrib_inverse,
        0
    };

    info("Term w=%"PRId16" h=%"PRId16, width, height);

    //Termporary
    width = MAX('~' - '!' + 1, width);

    term->display = malloc(sizeof(term->display[0]) * term->height);
    nss_line_t *line = NULL;
    for(size_t i = 0; i < (size_t)term->height; i++) {
        term->display[i] = line = create_line(line, NULL, width);
        if(!line) die("Can't create line");
    }

    term->view = term->display[0];

    for (size_t k = 0; k < 5; k++) {
        for (size_t i = 0; i <= (size_t)('~' - '!'); i++)
            term->display[k]->cell[i] = NSS_MKCELL(7, 0, test[k], i + '!');
    }
    term->display[0]->cell[13] = NSS_MKCELL(3, 5, test[3], 'A');
    term->display[1]->cell[16] = NSS_MKCELL(4, 6, test[2], 'A');

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

    cursor &= term->view == term->display[0];

    if (intersect_with(&damage, &(nss_rect_t) {0, 0, term->width, term->height})) {
        //Clear undefined areas
        nss_window_clear(term->win, 1, &damage);

        nss_line_t *line = term->view;
        size_t j = 0;
        for (; line && j < (size_t)damage.y; j++, line = line->next);
        for (; line && j < (size_t)damage.height + damage.y; j++, line = line->next) {
            if (line->width > (size_t)damage.x)
                nss_window_draw(term->win, damage.x, j,
                        MIN(line->width - damage.x, damage.width), line->cell + damage.x);
        }

        if (cursor && damage.x <= term->cur_x && term->cur_x <= damage.x + damage.width &&
                damage.y <= term->cur_y && term->cur_y <= damage.y + damage.height) {
            int16_t cx = MIN(term->cur_x, term->width - 1);
            nss_window_draw_cursor(term->win, cx, term->cur_y, &term->display[term->cur_y]->cell[cx]);
        }
        nss_window_update(term->win, 1, &damage);
    }
}

void nss_term_resize(nss_term_t *term, int16_t width, int16_t height) {

    info("Resize: w=%"PRId16" h=%"PRId16, width, height);

    for(size_t i = height; i < (size_t)term->height; i++)
        free(term->display[i]);
    nss_line_t **new = realloc(term->display, height * sizeof(term->display[0]));
    if (!new) die("Can't create lines");

    term->display = new;

    for(size_t i = term->height; i < (size_t)height; i++)
        term->display[i] = create_line(term->display[i - 1], NULL, width);

    term->display[height - 1]->next = NULL;

    _Bool inview = term->display[0] == term->view;
    for (size_t i = 0; i < (size_t)MIN(height, term->height); i++)
        if (term->display[i]->width < (size_t)width)
            term->display[i] = resize_line(term->display[i], width);
    if (inview) term->view = term->display[0];

    uint8_t *newtabs = realloc(term->tabs, width);
    if (!newtabs) die("Can't alloc tabs");
    term->tabs = newtabs;
    if(width > term->width) {
        memset(newtabs + term->width, 0, (width - term->width) * sizeof(newtabs[0]));
        int16_t tab = term->width;
        while(tab > 0 && !newtabs[tab]) tab--;
        while ((tab += INIT_TAB_SIZE) < width) newtabs[tab] = 1;
    }

    _Bool cur_moved = term->cur_x == term->width;

    term->height = height;
    term->width = width;

    int16_t ncx = MIN(term->cur_x, width);
    int16_t ncy = MIN(term->cur_y, height - 1);

    cur_moved |= term->cur_x != ncx || term->cur_y >= ncy;
    term_moveto(term, ncx, ncy);

    if (cur_moved) nss_term_redraw(term, (nss_rect_t){term->cur_x - 1, term->cur_y, 2, 1}, 1);

   // if (term->view == term->display[0] && term->cur_y > height - 1)
    //        term_scroll(term, term->cur_y - height - 1);
    nss_window_draw_commit(term->win);

    // Pixel sizes are unused
    // TODO: Set them correctly
    struct winsize wsz;
    wsz.ws_col = width;
    wsz.ws_row = height;
    wsz.ws_xpixel = nss_window_get(term->win, nss_wc_width);
    wsz.ws_ypixel = nss_window_get(term->win, nss_wc_height);

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
    nss_line_t *line = term->display[term->height - 1];
    while (line) {
        nss_line_t *next = line->prev;
        // TODO: Deref all attribs in line here
        free(line);
        line = next;
    }
    free(term->tabs);
    free(term);
}
