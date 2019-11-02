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
#include <wchar.h>
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
#define IS_C0(c) (((c) < 0x20) || (c) == 0x7f)

typedef struct nss_cursor {
    int16_t x;
    int16_t y;
    nss_cell_t cel;
    // Shift state
    uint8_t gl;
    uint8_t gr;
    uint8_t gl_ss;
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

    int16_t width;
    int16_t height;
    int16_t top;
    int16_t bottom;
    uint8_t *tabs;

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
        nss_tm_force_redraw = 1 << 8,
        nss_tm_insert = 1 << 9,
        nss_tm_sixel = 1 << 10,
        nss_tm_8bit = 1 << 11,
    } mode;

    enum nss_char_set {
        nss_cs_dec_ascii,
        nss_cs_dec_sup,
        nss_cs_dec_graph,
        nss_cs_british,
        nss_cs_dutch,
        nss_cs_finnish,
        nss_cs_french,
        nss_cs_french_canadian,
        nss_cs_german,
        nss_cs_itallian,
        nss_cs_norwegian_dannish,
        nss_cs_spannish,
        nss_cs_swedish,
        nss_cs_swiss,
        nss_cs_max,
    } charset[4];

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

    nss_cell_t cell = term->c.cel;
    NSS_CELL_ATTRS_ZERO(cell); //@@ ???
    for (; ys < ye; ys++)
        for(int16_t i = xs; i < xe; i++)
            term->screen[ys]->cell[i] = cell;
}

static void term_move_to(nss_term_t *term, int16_t x, int16_t y) {
    term->c.x = MIN(MAX(x, 0), term->width - 1);
    if (term->c.origin)
        term->c.y = MIN(MAX(y, term->top), term->bottom);
    else
        term->c.y = MIN(MAX(y, 0), term->height - 1);
}

static void term_move_to_abs(nss_term_t *term, int16_t x, int16_t y) {
    term_move_to(term, x, y + (term->c.origin ? term->top : 0));
}

static void term_cursor_mode(nss_term_t *term, _Bool mode) {
    if (mode) { //save
       term->cs = term->c;
    } else { //restore
       term->c = term->cs;
    }
    // Should it reset wrap-next state?
    term_move_to(term, term->c.x, term->c.y);
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

static void term_set_tb_margins(nss_term_t *term, int16_t top, int16_t bottom) {
    top = MAX(0, MIN(term->height - 1, top));
    bottom = MAX(0, MIN(term->height - 1, bottom));
    if (top > bottom) SWAP(int16_t, top, bottom);
    term->top = top;
    term->bottom = bottom;
}

static void term_insert_cells(nss_term_t *term, int16_t n) {
    int16_t cx = MIN(term->c.x, term->width - 1);
    n = MAX(0, MIN(n, term->width - cx));
    nss_line_t *line = term->screen[term->c.y];
    memmove(line->cell + cx + n, line->cell + cx, (term->width - cx - n) * sizeof(nss_cell_t));
    term_clear_region(term, cx, term->c.y, term->width, term->c.y + 1);
}

static void term_delete_cells(nss_term_t *term, int16_t n) {
    int16_t cx = MIN(term->c.x, term->width - 1);
    n = MAX(0, MIN(n, term->width - cx));
    nss_line_t *line = term->screen[term->c.y];
    memmove(line->cell + cx, line->cell + cx + n, (term->width - cx - n) * sizeof(nss_cell_t));
    term_clear_region(term, term->width - n, term->c.y, term->width, term->c.y + 1);
}

static void term_insert_lines(nss_term_t *term, int16_t n) {
    if (term->top <= term->c.y && term->c.y <= term->bottom)
        term_scroll(term, term->c.y, term->bottom, n, 0);
}

static void term_delete_lines(nss_term_t *term, int16_t n) {
    if (term->top <= term->c.y && term->c.y <= term->bottom)
        term_scroll(term, term->c.y, term->bottom, -n, 0);
}

static void term_newline(nss_term_t *term, _Bool cr) {
    if (term->c.y == term->bottom) {
        term_scroll(term,  term->top, term->bottom, 1, 1);
        term_move_to(term, cr ? 0 : term->c.x, term->c.y);
    } else {
        term_move_to(term, cr ? 0 : term->c.x, term->c.y + 1);
    }
}

static void term_reset_tabs(nss_term_t *term) {
    memset(term->tabs, 0, term->width * sizeof(term->tabs[0]));
    for(size_t i = INIT_TAB_SIZE; i < (size_t)term->width; i += INIT_TAB_SIZE)
        term->tabs[i] = 1;
}

static void term_tabs(nss_term_t *term, int16_t n) {
    if (n >= 0) {
        if(term->c.x < term->width) term->c.x++;
        while(n--)
            while(term->c.x < term->width && !term->tabs[term->c.x])
                term->c.x++;
    } else {
        if(term->c.x > 0) term->c.x--;
        while(n++)
            while(term->c.x > 0 && !term->tabs[term->c.x])
                term->c.x--;
    }
    term->c.x = MIN(term->c.x, term->width - 1);
}

static void term_reset(nss_term_t *term) {
    term->charset[0] = nss_cs_dec_ascii;
    term->charset[1] = nss_cs_dec_sup;
    term->charset[2] = nss_cs_dec_ascii;
    term->charset[3] = nss_cs_dec_ascii;

    term->c = (nss_cursor_t) { .cel = NSS_MKCELL(7, 0, 0, ' ') };
    term->mode = nss_tm_wrap | nss_tm_visible | nss_tm_utf8;
    term->top = 0;
    term->bottom = term->height - 1;
    term_reset_tabs(term);
    for(size_t i = 0; i < 2; i++) {
        term_cursor_mode(term, 1);
        term_clear_region(term, 0, 0, term->width, term->height);
        term_swap_screen(term);
    }
}

#define GRAPH0_BASE 0x41
#define GRAPH0_SIZE 62

static uint32_t nrcs_translate(uint8_t set, uint32_t ch) {
    static const int32_t *trans[nss_cs_max] = {
        /* [0x23] [0x40] [0x5b 0x5c 0x5d 0x5e 0x5f 0x60] [0x7b 0x7c 0x7d 0x7e] */
        [nss_cs_british] =           L"£@[\\]^_`{|}~",
        [nss_cs_dutch] =             L"£¾\u0133½|^_`¨f¼´",
        [nss_cs_finnish] =           L"#@ÄÖÅÜ_éäöåü",
        [nss_cs_french] =            L"£à°ç§^_`éùè¨",
        [nss_cs_swiss] =             L"ùàéçêîèôäöüû",
        [nss_cs_french_canadian] =   L"#àâçêî_ôéùèû",
        [nss_cs_german] =            L"#§ÄÖÜ^_`äöüß",
        [nss_cs_itallian] =          L"£§°çé^_ùàòèì",
        [nss_cs_norwegian_dannish] = L"#ÄÆØÅÜ_äæøåü",
        [nss_cs_spannish] =          L"£§¡Ñ¿^_`°ñç~",
        [nss_cs_swedish] =           L"#ÉÆØÅÜ_éæøåü",
    };
    static const int32_t *graph = L" ◆▒␉␌␍␊°±␤␋┘┐┌└┼⎺⎻─⎼⎽├┤┴┬│≤≥π≠£·";
    if (set == nss_cs_dec_graph) {
        if (0x5f <= ch && ch <= 0x7e)
            return graph[ch - 0x5f];
    } else if (set == nss_cs_dec_sup) {
        return ch + 0x80;
    } else if (trans[set]){
        if (ch == 0x23) return trans[set][0];
        if (ch == 0x40) return trans[set][1];
        if (0x5b <= ch && ch <= 0x60)
            return trans[set][2 + ch - 0x5b];
        if (0x7b <= ch && ch <= 0x6e)
            return trans[set][8 + ch - 0x7b];
    }
    return ch;

    /*
     * What are that symbols?
        "↑", "↓", "→", "←", "█", "▚", "☃",      // A - G
    */
}

static void term_set_cell(nss_term_t *term, int16_t x, int16_t y, nss_cell_t cel, uint32_t ch) {
    if (ch < 0x80) {
        if (term->c.gl_ss != nss_cs_dec_ascii)
            ch = nrcs_translate(term->c.gl_ss, ch);
    } else if (ch < 0x100) {
        if (term->c.gr != nss_cs_dec_sup)
            ch = nrcs_translate(term->c.gr, ch - 0x80);
    }
    term->c.gl_ss = term->c.gl;

    nss_cell_t *cell = &term->screen[y]->cell[x];
    if (NSS_CELL_ATTRS(*cell) & nss_attrib_wide) {
        if ((size_t)x + 1 < term->screen[y]->width) {
            cell[1] = NSS_MKCELLWITH(cell[1], ' ');
            NSS_CELL_ATTRCLR(cell[1], nss_attrib_wdummy);
        }
    } else if (NSS_CELL_ATTRS(*cell) & nss_attrib_wdummy) {
        if (x > 0) {
            cell[-1] = NSS_MKCELLWITH(cell[-1], ' ');
            NSS_CELL_ATTRCLR(cell[-1], nss_attrib_wide);
        }
    }

    term->screen[y]->mode |= nss_lm_dirty;
    cell[0] = NSS_MKCELLWITH(cel, ch);
}


static void term_escape_dcs(nss_term_t *term) { /* yet nothing here */}
static void term_escape_osc(nss_term_t *term) { /* yet nothing here */}

static void term_escape_csi(nss_term_t *term) {
    switch (term->esc.final) {
    }
}

static void term_escape_esc(nss_term_t *term) {
    if (term->esc.interm_idx == 0) {
            uint32_t arg;
        switch(term->esc.final) {
        case 'D': /* IND */
            term_newline(term, 0);
            break;
        case 'E': /* NEL */
            term_newline(term, 1);
            break;
        case 'H': /* HTS */
            term->tabs[MIN(term->c.x, term->width - 1)] = 1;
            break;
        case 'M': /* RI */
            if (term->c.y == term->top)
                term_scroll(term, term->top, term->bottom, -1, 0);
            else
                term_move_to(term, term->c.x, term->c.y - 1);
            break;
        case 'N': /* SS2 */
            term->c.gl_ss = 2;
            break;
        case 'O': /* SS3 */
            term->c.gl_ss = 3;
            break;
        case 'V': /* SPA */
            NSS_CELL_ATTRSET(term->c.cel, nss_attrib_protected);
            break;
        case 'W': /* EPA */
            NSS_CELL_ATTRCLR(term->c.cel, nss_attrib_protected);
            break;
        case 'Z': /* DECID */
            nss_term_write(term, NSS_TERM_DECID, strlen((const char *)NSS_TERM_DECID), 0);
            break;
        case '6': /* DECBI */
            warn("Unknown escape sequence"); //TODO Dump
            break;
        case '7': /* DECSC */
            term_cursor_mode(term, 1);
            break;
        case '8': /* DECRC */
            term_cursor_mode(term, 1);
            break;
        case '9': /* DECFI */
            warn("Unknown escape sequence"); //TODO Dump
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
            term_reset(term);
            // @TODO Reset colors and title
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
        default: warn("Unknown escape sequence"); //TODO Dump
        }
    } else if (term->esc.interm_idx == 1) {
        switch (term->esc.interm[0]) {
        case ' ':
            switch (term->esc.final) {
                uint32_t arg;
            case 'F': /* S8C1T */
                arg = 1;
                nss_window_set(term->win, nss_wc_8bit, &arg);
                term->mode |= nss_tm_8bit;
                break;
            case 'G': /* S7C1T */
                arg = 0;
                nss_window_set(term->win, nss_wc_8bit, &arg);
                term->mode &= ~nss_tm_8bit;
                break;
            case 'L': case 'M': case 'N':
            default: warn("Unknown escape seqence");
            }
            break;
        case '#':
            switch (term->esc.final) {
            case '3': /* DECDHL */
            case '4': /* DECDHL */
            case '5': /* DECSWL */
            case '6': /* DECDWL */
                warn("Double height/width not supported");
                break;
            case '8': /* DECALN*/
                for (int16_t i = 0; i < term->height; i++)
                    for(int16_t j = 0; j < term->width; j++)
                        term_set_cell(term, j, i, term->c.cel, 'E');
                break;
            default: warn("Unknown escape sequence"); //TODO Dump
            }
            break;
        case '%':
            switch (term->esc.final) {
            case '@': term->mode &= ~nss_tm_utf8; break;
            case 'G': term->mode |= nss_tm_utf8; break;
            default: warn ("Unknown escape sequence");
            }
            break;
        case '(': /* Designate G0 with 94 charset */
        case ')': /* Designate G1 with 94 charset */
        case '*': /* Designate G2 with 94 charset */
        case '+': /* Designate G3 with 94 charset */ {
            enum nss_char_set *set = &term->charset[term->esc.interm[0] - ')'];
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
                warn("Charset is not supported");
            }
            break;
        }
        case '-': /* Designate G1 withh 96 charset */
        case '.': /* Designate G2 withh 96 charset */
        case '/': /* Designate G3 withh 96 charset */
            warn("Charset is not supported");
            break;
        default:
            warn ("Unknown escape sequence");
        }
    } else {
        switch (term->esc.interm[0]) {
        case '(': /* Designate G0 with 94 charset */
        case ')': /* Designate G1 with 94 charset */
        case '*': /* Designate G2 with 94 charset */
        case '+': /* Designate G3 with 94 charset */
            warn("Charset is not supported");
            break;
        default:
            warn("Unknown escape sequence"); //TODO Dump
        }
    }
}

static void term_escape_reset(nss_term_t *term) {
    memset(&term->esc, 0, sizeof(term->esc));
}

static void term_escape_control(nss_term_t *term, uint32_t ch) {
    switch (ch) {
    case '\t': /* HT */
        term_tabs(term, 1);
        return;
    case '\b': /* BS */
        term_move_to(term, term->c.x - 1, term->c.y);
        return;
    case '\r': /* CR */
        term_move_to(term, 0, term->c.y);
        return;
    case '\f': /* FF */
    case '\v': /* VT */
    case '\n': /* LF */
        term_newline(term, term->mode & nss_tm_crlf);
        return;
    case '\a': /* BEL */
        if (term->esc.state & nss_es_string) {
            if (term->esc.state & nss_es_dcs)
                term_escape_dcs(term);
            else if (term->esc.state & nss_es_osc)
                term_escape_osc(term);
        }
        else /* term_bell() -- TODO */;
        break;
    case '\e': /* ESC */
        if (term->esc.state & nss_es_string) {
            term->esc.state &= (nss_es_osc | nss_es_dcs | nss_es_ignore);
            term->esc.state |= nss_es_escape | nss_es_defer;
            return;
        }
        term_escape_reset(term);
        term->esc.state = nss_es_escape;
        return;
    case '\016': /* S0/LS0 */
        term->c.gl = term->c.gl_ss = 0;
        return;
    case '\017': /* SI/LS1 */
        term->c.gl = term->c.gl_ss = 1;
        return;
    case '\032': /* SUB */
        term_set_cell(term, MIN(term->c.x, term->width - 1),
                      term->c.y, term->c.cel, '?');
    case '\030': /* CAN */
        break;
    case '\005': /* ENQ (IGNORE) */
    case '\000': /* NUL (IGNORE) */
    case '\021': /* XON (IGNORE) */
    case '\023': /* XOFF (IGNORE) */
    case 0x7f:   /* DEL (IGNORE) */
        return;
    case 0x80:   /* PAD - TODO */
    case 0x81:   /* HOP - TODO */
    case 0x82:   /* BPH - TODO */
    case 0x83:   /* NBH - TODO */
        warn("Unknown control character");
        break;
    case 0x84:   /* IND - Index */
        term_newline(term, 0);
        break;
    case 0x85:   /* NEL -- Next line */
        term_newline(term, 1);
        break;
    case 0x86:   /* SSA - TODO */
    case 0x87:   /* ESA - TODO */
        warn("Unknown control character");
        break;
    case 0x88:   /* HTS -- Horizontal tab stop */
        term->tabs[MIN(term->c.x, term->width - 1)] = 1;
        break;
    case 0x89:   /* HTJ - TODO */
    case 0x8a:   /* VTS - TODO */
    case 0x8b:   /* PLD - TODO */
    case 0x8c:   /* PLU - TODO */
        warn("Unknown control character");
        break;
    case 0x8d:   /* RI - Reverse Index */
        if (term->c.y == term->top)
            term_scroll(term, term->top, term->bottom, -1, 0);
        else
            term_move_to(term, term->c.x, term->c.y - 1);
        break;

    case 0x8e:   /* SS2 - Single Shift 2 */
        term->c.gl_ss = 2;
        break;
    case 0x8f:   /* SS3 - Single Shift 3 */
        term->c.gl_ss = 3;
        break;
    case 0x91:   /* PU1 - TODO */
    case 0x92:   /* PU2 - TODO */
    case 0x93:   /* STS - TODO */
    case 0x94:   /* CCH - TODO */
    case 0x95:   /* MW - TODO */
        warn("Unknown control character");
        break;
    case 0x96:   /* SPA - Start of Protected Area */
        NSS_CELL_ATTRSET(term->c.cel, nss_attrib_protected);
        break;
    case 0x97:   /* EPA - End of Protected Area */
        NSS_CELL_ATTRCLR(term->c.cel, nss_attrib_protected);
        break;
    case 0x98:   /* SOS - Start Of String */
        term_escape_reset(term);
        term->esc.state = nss_es_ignore | nss_es_string;
        return;
    case 0x99:   /* SGCI - TODO */
        warn("Unknown control character");
        break;
    case 0x9a:   /* DECID -- Identify Terminal */
        nss_term_write(term, NSS_TERM_DECID, strlen((const char *)NSS_TERM_DECID), 0);
        break;
    case 0x9b:   /* CSI - Control Sequence Introducer */
        term_escape_reset(term);
        term->esc.state = nss_es_csi | nss_es_escape;
        break;
    case 0x9c:   /* ST - String terminator */
        if (term->esc.state & nss_es_string) {
            if (term->esc.state & nss_es_dcs)
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
        width = 1, ch = UTF_INVAL;

    size_t char_len = utf8_encode(ch, buf, buf + UTF8_MAX_LEN);
    buf[char_len] = '\0';

    info("UTF %"PRIx32" '%s'", ch, buf);

    if (term->esc.state & nss_es_string && !(ch < 0x100 && (IS_C1(ch) || strchr("\033\032\030\a", ch)))) {
        if (term->esc.state & nss_es_dcs && ch == 0x7f) return;
        if (term->esc.state & nss_es_osc && IS_C0(ch) && ch != 0x7f) return;
        if (term->esc.state & nss_es_ignore) return;
        /* It might be useful to stop string mode here V */
        if (term->esc.str_idx + char_len > ESC_MAX_STR) return;
        memcpy(term->esc.str, buf, char_len + 1);
        term->esc.str_idx += char_len;
    } else if (IS_C0(ch) || IS_C1(ch)) {
        if (term->esc.state & nss_es_dcs && IS_C0(ch) && !strchr("\033\032\030\a", ch)) return;
        info("Here");
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
                    else if (term->esc.param_idx < ESC_MAX_PARAM) {
                        term->esc.param[term->esc.param_idx++] = 0;
                    }
                } else if (0x3c <= ch && ch <= 0x3f) {
                    if (term->esc.state & (nss_es_gotfirst | nss_es_intermediate))
                        term->esc.state |= nss_es_ignore;
                    else term->esc.private = ch;
                } else if (0x40 <= ch && ch <= 0x7e) {
                    term->esc.final = ch;
                    if (term->esc.state & nss_es_csi) {
                        if (!(term->esc.state & nss_es_ignore))
                            term_escape_csi(term);
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
                    memcpy(term->esc.str, "0;", 3);
                    term->esc.str_idx = 2;
                    term->esc.state |= nss_es_osc | nss_es_string;
                } else if (0x30 <= ch && ch <= 0x7e) {
                    term->esc.final = ch;
                    if (!(term->esc.state & nss_es_ignore))
                        term_escape_esc(term);
                    term_escape_reset(term);
                }
            }
        }
        return;
    }

    if (!width) return;

    if (term->mode & nss_tm_wrap && term->c.x + width - 1 >= term->width) {
        term->screen[term->c.y]->mode |= nss_lm_wrapped;
        term_newline(term, 1);
    }

    nss_cell_t *cell = &term->screen[term->c.y]->cell[term->c.x];

    if (term->mode & nss_tm_insert && term->c.x + width < term->width) {
        // TODO Check wide chars here too
        memmove(cell + width, cell, term->screen[term->c.y]->width - term->c.x - width);
    }

    // NOTE Do I need to wrap here again?
    term_set_cell(term, term->c.x, term->c.y, term->c.cel, ch);

    if (width > 1) {
        cell[1] = NSS_MKCELLWITH(term->c.cel, ' ');
        NSS_CELL_ATTRSET(cell[0], nss_attrib_wide);
        NSS_CELL_ATTRSET(cell[1], nss_attrib_wdummy);
    }

    term->c.x += width;
}

static ssize_t term_write(nss_term_t *term, const uint8_t *buf, size_t len, _Bool show_ctl) {
    const uint8_t *end = buf + len, *start = buf;

    nss_term_redraw(term, (nss_rect_t) { MIN(term->c.x, term->width - 1), term->c.y, 2, 1}, 0);

    while (start < end) {
        uint32_t ch;
        if (!(term->mode & nss_tm_utf8) || (term->mode & nss_tm_sixel))  ch = *buf++;
        else if (!utf8_decode(&ch, &start, end))  break;

        if (show_ctl) {
            if (IS_C1(ch)) {
                term_putchar(term, '^');
                term_putchar(term, '[');
            } else if (IS_C0(ch) && ch != '\n' && ch != '\t' && ch != '\r') {
                ch ^= 0x40;
                term_putchar(term, '^');
            }
        }
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
    if ((res = read(term->fd, term->fd_buf + term->fd_buf_pos,
            NSS_FD_BUF_SZ - term->fd_buf_pos)) < 0) {
        warn("Can't read from tty");
        nss_term_hang(term);
    }
    term->fd_buf_pos += res;
    ssize_t disp = term_write(term, term->fd_buf, term->fd_buf_pos, 0);

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
    int16_t ox = term->c.x, oy = term->c.y;

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
    term_move_to(term, term->c.x, term->c.y);

    // Clear new regions

    for (size_t i = 0; i < 2; i++) {
        if (height > oh) term_clear_region(term, 0, oh, width, height);
        SWAP(nss_line_t **, term->screen, term->back_screen);
    }

    if(ow == ox || ox != term->c.x || oy != term->c.y)
        term->mode |= nss_tm_force_redraw;
}

nss_term_t *nss_create_term(nss_window_t *win, int16_t width, int16_t height) {
    nss_term_t *term = calloc(1, sizeof(nss_term_t));

    term->win = win;
    term->scrollback_limit = -1;
    clock_gettime(CLOCK_MONOTONIC, &term->draw_time);

    term->charset[1] = nss_cs_dec_sup;
    term->c = term->cs = term->back_cs = (nss_cursor_t) { .cel = NSS_MKCELL(7, 0, 0, ' ') };
    term->mode = nss_tm_wrap | nss_tm_visible | nss_tm_utf8;

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
        if (cursor && !term->view && damage.x <= term->c.x && term->c.x <= damage.x + damage.width &&
                damage.y <= term->c.y && term->c.y <= damage.y + damage.height) {
            int16_t cx = MIN(term->c.x, term->width - 1);
            nss_window_draw_cursor(term->win, term->c.x, term->c.y, &term->screen[term->c.y]->cell[cx]);
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
        int16_t cx = MIN(term->c.x, term->width - 1);
        nss_window_draw_cursor(term->win, term->c.x, term->c.y, &term->screen[term->c.y]->cell[cx]);
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
    nss_term_redraw(term, (nss_rect_t) {term->c.x, term->c.y, 1, 1}, 1);
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
