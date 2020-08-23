/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#include "feature.h"

#define _XOPEN_SOURCE 700
#include <assert.h>

#include "config.h"
#include "input.h"
#include "mouse.h"
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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

//For openpty() function
#if   defined(__linux)
#   include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
#   include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#   include <libutil.h>
#endif

#define TTY_MAX_WRITE 256
#define FD_BUF_SIZE 4096
#define ESC_MAX_PARAM 32
#define ESC_MAX_STR 256
#define ESC_MAX_LONG_STR 0x10000000
#define ESC_DUMP_MAX 768
#define MAX_REPORT 1024
#define SGR_BUFSIZ 64
#define UDK_MAX 37
#define MAX_LINE_LEN 16384

#define CSI "\x9B"
#define OSC "\x9D"
#define DCS "\x90"
#define ESC "\x1B"
#define ST "\x9C"

#define IS_C1(c) ((uint32_t)(c) - 0x80U < 0x20U)
#define IS_C0(c) ((c) < 0x20)
#define IS_DEL(c) ((c) == 0x7f)
#define IS_STREND(c) (IS_C1(c) || (c) == 0x1b || (c) == 0x1a || (c) == 0x18 || (c) == 0x07)

#define TABSR_INIT_CAP 48
#define TABSR_CAP_STEP(x) (4*(x)/3)
#define TABSR_MAX_ENTRY 6

#define MAX_EXTRA_PALETTE (0x10000 - PALETTE_SIZE)
#define CAPS_INC_STEP(sz) MIN(MAX_EXTRA_PALETTE, (sz) ? 8*(sz)/5 : 4)
#define CBUF_STEP(c,m) ((c) ? MIN(4 * (c) / 3, m) : MIN(16, m))
#define STR_CAP_STEP(x) (4*(x)/3)
#define PARAM(i, d) (term->esc.param[i] > 0 ? (uparam_t)term->esc.param[i] : (uparam_t)(d))
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

struct cursor {
    ssize_t x;
    ssize_t y;
    struct cell cel;
    color_t fg;
    color_t bg;
    // Shift state
    uint8_t gl : 2;
    uint8_t gr : 2;
    uint8_t gl_ss : 2;
    enum charset upcs;
    enum charset gn[4];

    bool origin : 1;
    bool pending : 1;
};

enum mode_status {
    modstate_unrecognized,
    modstate_enabled,
    modstate_disabled,
    modstate_aways_enabled,
    modstate_aways_disabled,
};

struct term_mode {
    bool echo : 1;
    bool crlf : 1;
    bool wrap : 1;
    bool focused : 1;
    bool altscreen : 1;
    bool altscreen_scroll : 1;
    bool disable_altscreen : 1;
    bool utf8 : 1;
    bool reverse_video : 1;
    bool insert : 1;
    bool sixel : 1;
    bool eight_bit : 1;
    bool protected : 1;
    bool track_focus : 1;
    bool hide_cursor : 1;
    bool enable_nrcs : 1;
    bool scroll_on_output : 1;
    bool no_scroll_on_input : 1;
    bool columns_132 : 1;
    bool preserve_display_132 : 1;
    bool disable_columns_132 : 1;
    bool print_extend : 1;
    bool print_form_feed : 1;
    bool print_enabled : 1;
    bool print_auto : 1;
    bool title_set_utf8 : 1;
    bool title_query_utf8 : 1;
    bool title_set_hex : 1;
    bool title_query_hex : 1;
    bool bracketed_paste : 1;
    bool keep_selection : 1;
    bool keep_clipboard : 1;
    bool select_to_clipboard : 1;
    bool reverse_wrap : 1;
    bool led_num_lock : 1;
    bool led_caps_lock : 1;
    bool led_scroll_lock : 1;
    bool attr_ext_rectangle : 1;
    bool lr_margins : 1;
    bool smooth_scroll : 1;
    bool xterm_more_hack : 1;
    bool allow_change_font : 1;
    bool paste_quote : 1;
    bool paste_literal_nl : 1;
    bool margin_bell : 1;
    bool bell_raise : 1;
    bool bell_urgent : 1;
};

struct checksum_mode {
    bool positive : 1;
    bool no_attr : 1;
    bool no_trim : 1;
    bool no_implicit : 1;
    bool wide : 1;
    bool eight_bit : 1;
};

struct term {
    struct line **screen;
    struct line **back_screen;

    /* Cyclic buffer for
     * saved lines */
    struct line **scrollback;
    ssize_t sb_top;
    ssize_t sb_limit;
    ssize_t sb_caps;
    ssize_t sb_max_caps;
    /* offset in scrollback lines */
    struct line_offset view_pos;
    /* virtual line number */
    ssize_t view;

    struct cursor c;
    struct cursor cs;
    struct cursor back_cs;
    struct cursor vt52c;

    ssize_t width;
    ssize_t height;
    ssize_t top;
    ssize_t bottom;
    ssize_t left;
    ssize_t right;
    bool *tabs;

    /* Last written character
     * Used for REP */
    term_char_t prev_ch;

    /* OSC 52 character description
     * of selection being pasted from */
    uint8_t paste_from;

    /* Mouse and selection state */
    struct mouse_state mstate;
    struct keyboard_state kstate;

    /* Previous cursor state
     * Used for effective cursor invalidation */
    ssize_t prev_c_x;
    ssize_t prev_c_y;
    bool prev_c_hidden;
    bool prev_c_view_changed;

    struct term_mode mode;
    struct term_mode vt52mode;

    struct checksum_mode checksum_mode;

    uint8_t bvol;
    uint8_t mbvol;

    /* This is compressed bit array,
     * that stores encoded modes for
     * XTSAVE/XTRESTORE
     * [0-96] : 12 bytes;
     * [1000-1063] : 8 bytes;
     * [2000-2007] : 1 byte */
    uint8_t saved_modbits[21];
    enum mouse_mode saved_mouse_mode;
    enum mouse_format saved_mouse_format;
    uint8_t saved_keyboard_type;

    struct escape {
        enum escape_state {
            esc_ground,
            esc_esc_entry, esc_esc_1, esc_esc_2, esc_esc_ignore,
            esc_csi_entry, esc_csi_0, esc_csi_1, esc_csi_2, esc_csi_ignore,
            esc_dcs_entry, esc_dcs_0, esc_dcs_1, esc_dcs_2,
            esc_osc_entry, esc_osc_1, esc_osc_2, esc_osc_string,
            esc_dcs_string,
            esc_ign_entry, esc_ign_string,
            esc_vt52_entry, esc_vt52_cup_0, esc_vt52_cup_1,
        } state, old_state;
        uparam_t selector;
        uparam_t old_selector;

        size_t i;
        iparam_t param[ESC_MAX_PARAM];
        uint32_t subpar_mask;

        size_t str_len;
        // Short strings are not allocated
        uint8_t str_data[ESC_MAX_STR + 1];
        // Long strings are
        uint8_t *str_ptr;
        size_t str_cap;
    } esc;

    uint16_t vt_version;
    uint16_t vt_level;

    struct window *win;
    color_t *palette;

    pid_t child;
    int fd;
    int printerfd;

    uint8_t *fd_start;
    uint8_t *fd_end;
    uint8_t fd_buf[FD_BUF_SIZE];

    int32_t predec_buf[FD_BUF_SIZE];
};

/* Default termios, initialized from main */
static struct termios dtio;

static void handle_chld(int arg) {
    int status;
    char str[128];
    ssize_t len = 0;

    pid_t pid = waitpid(-1, &status, WNOHANG);
    uint32_t loglevel = iconf(ICONF_LOG_LEVEL);

    if (pid < 0) {
        if (loglevel > 1) len = snprintf(str, sizeof str,
                "[\033[33;1mWARN\033[m] Child wait failed");
    } else if (WIFEXITED(status) && WEXITSTATUS(status)) {
        if (loglevel > 2) len = snprintf(str, sizeof str,
                "[\033[32;1mINFO\033[m] Child exited with status: %d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        if (loglevel > 2) len = snprintf(str, sizeof str,
                "[\033[32;1mINFO\033[m] Child terminated due to the signal: %d\n", WTERMSIG(status));
    }

    if (len) write(STDERR_FILENO, str, len);

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
    setenv("TERM", sconf(SCONF_TERM_NAME), 1);

    signal(SIGCHLD, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGALRM, SIG_DFL);

    // Disable job control signals by default
    // like login does
#ifdef SIGTSTP
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
#endif

    execvp(cmd, (char *const *)args);
    _exit(1);
}

void init_default_termios(void) {
    /* Use stdin as base configuration */
    if (tcgetattr(STDIN_FILENO, &dtio) < 0)
        memset(&dtio, 0, sizeof(dtio));

    /* Setup keys */

    /* Disable everything */
    for (size_t i = 0; i < NCCS; i++)
#ifdef _POSIX_VDISABLE
        dtio.c_cc[i] = _POSIX_VDISABLE;
#else
        dtio.c_cc[i] = 255;
#endif

    /* Then enable all known */

#ifdef CINTR
    dtio.c_cc[VINTR] = CINTR;
#else
    dtio.c_cc[VINTR] = '\003';
#endif

#ifdef CQUIT
    dtio.c_cc[VQUIT] = CQUIT;
#else
    dtio.c_cc[VQUIT] = '\034';
#endif

#ifdef CERASE
    dtio.c_cc[VERASE] = CERASE;
#elif defined(__linux__)
    dtio.c_cc[VERASE] = '\177';
#else
    dtio.c_cc[VERASE] = '\010';
#endif

#ifdef CKILL
    dtio.c_cc[VKILL] = CKILL;
#else
    dtio.c_cc[VKILL] = '\025';
#endif

#ifdef CEOF
    dtio.c_cc[VEOF] = CEOF;
#else
    dtio.c_cc[VEOF] = '\004';
#endif

#ifdef CSTART
    dtio.c_cc[VSTART] = CSTART;
#else
    dtio.c_cc[VSTART] = '\021';
#endif

#ifdef CSTOP
    dtio.c_cc[VSTOP] = CSTOP;
#else
    dtio.c_cc[VSTOP] = '\023';
#endif

#ifdef CSUSP
    dtio.c_cc[VSUSP] = CSUSP;
#else
    dtio.c_cc[VSUSP] = '\032';
#endif

#ifdef VERASE2
    dtio.c_cc[VERASE2] = CERASE2;
#endif

#ifdef VDSUSP
#   ifdef CDSUSP
    dtio.c_cc[VDSUSP] = CDSUSP;
#   else
    dtio.c_cc[VDSUSP] = '\031';
#   endif
#endif

#ifdef VREPRINT
#   ifdef CRPRNT
    dtio.c_cc[VREPRINT] = CRPRNT;
#   else
    dtio.c_cc[VREPRINT] = '\022';
#   endif
#endif

#ifdef VDISCRD
#   ifdef CFLUSH
    dtio.c_cc[VDISCRD] = CFLUSH;
#   else
    dtio.c_cc[VDISCRD] = '\017';
#   endif
#elif defined(VDISCARD)
#   ifdef CFLUSH
    dtio.c_cc[VDISCARD] = CFLUSH;
#   else
    dtio.c_cc[VDISCARD] = '\017';
#   endif
#endif

#ifdef VWERSE
    dtio.c_cc[VWERSE] = CWERASE;
#elif defined(VWERASE)
    dtio.c_cc[VWERASE] = '\027';
#endif

#ifdef VLNEXT
#   ifdef CLNEXT
    dtio.c_cc[VLNEXT] = CLNEXT;
#   else
    dtio.c_cc[VLNEXT] = '\026';
#   endif
#endif

#ifdef VSTATUS
    dtio.c_cc[VSTATUS] = CSTATUS;
#endif

#if VMIN != VEOF
    dtio.c_cc[VMIN] = 1;
#endif

#if VTIME != VEOL
    dtio.c_cc[VTIME] = 0;
#endif

    /* Input modes */
#ifdef IMAXBEL
    dtio.c_iflag = BRKINT | IGNPAR | ICRNL | IMAXBEL | IXON;
#else
    dtio.c_iflag = BRKINT | IGNPAR | ICRNL | IXON;
#endif

    /* Output modes */
#ifdef ONLCR
    dtio.c_oflag = OPOST | ONLCR;
#else
    dtio.c_oflag = OPOST;
#endif

    /* Control modes */
    dtio.c_cflag = CS8 | CREAD;

    /* Local modes */
#if defined (ECHOCTL) && defined (ECHOKE)
    dtio.c_lflag = ISIG | ICANON | IEXTEN | ECHO | ECHOCTL | ECHOKE | ECHOE | ECHOK;
#else
    dtio.c_lflag = ISIG | ICANON | IEXTEN | ECHO | ECHOE | ECHOK;
#endif

    /* Find and set max I/O baud rate */
    int rate =
#if defined(B230400)
            B230400;
#elif defined(B115200)
            B115200;
#elif defined(B57600)
            B57600;
#elif defined(B38400)
            B38400;
#elif defined(B19200)
            B19200;
#else
            B9600;
#endif
    cfsetispeed(&dtio, rate);
    cfsetospeed(&dtio, rate);

}

static int tty_open(struct term *term, const char *cmd, const char **args) {
    /* Configure PTY */

    struct termios tio = dtio;

    tio.c_cc[VERASE] = iconf(ICONF_BACKSPACE_IS_DELETE) ? '\177' : '\010';

    /* If IUTF8 is defined, enable it by default,
     * when terminal itself is in UTF-8 mode */
#ifdef IUTF8
    if (iconf(ICONF_UTF8))
        tio.c_iflag |= IUTF8;
#endif

    int slave, master;
    if (openpty(&master, &slave, NULL, &tio, NULL) < 0) {
        warn("Can't create pseudo terminal");
        term->fd = -1;
        return -1;
    }

    int fl = fcntl(master, F_GETFL);
    if (fl >= 0) fcntl(master, F_SETFL, fl | O_NONBLOCK | O_CLOEXEC);

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
        exec_shell(cmd, args);
        break;
    default:
        close(slave);
        sigaction(SIGCHLD, &(struct sigaction){
                .sa_handler = handle_chld, .sa_flags = SA_RESTART}, NULL);
    }
    term->child = pid;
    term->fd = master;

    return master;
}

static bool optimize_line_palette(struct line *line) {
    // Buffer here causes a leak in theory
    static color_id_t *buf = NULL, buf_len = 0, *new;

    if (!line) {
        free(buf);
        buf = NULL;
        buf_len = 0;
        return 0;
    }

    if (line->pal) {
        if (buf_len < line->pal->size) {
            if (!(new = realloc(buf, line->pal->size * sizeof(color_id_t)))) return 0;
            memset(new + buf_len, 0xFF, (line->pal->size - buf_len) * sizeof(color_id_t));
            buf_len = line->pal->size, buf = new;
        }
        color_id_t k = PALETTE_SIZE, *pbuf = buf - PALETTE_SIZE;
        for (ssize_t i = 0; i < line->width; i++) {
            struct cell *cel = &line->cell[i];
            if (cel->fg >= PALETTE_SIZE) pbuf[cel->fg] = 0;
            if (cel->bg >= PALETTE_SIZE) pbuf[cel->bg] = 0;
        }
        color_t *pal = line->pal->data - PALETTE_SIZE;
        for (color_id_t i = PALETTE_SIZE; i < line->pal->size + PALETTE_SIZE; i++) {
            if (!pbuf[i]) {
                pal[k] = pal[i];
                for (color_id_t j = i + 1; j < line->pal->size + PALETTE_SIZE; j++)
                    if (pal[k] == pal[j]) pbuf[j] = k;
                pbuf[i] = k++;
            }
        }
        line->pal->size = k - PALETTE_SIZE;

        for (ssize_t i = 0; i < line->width; i++) {
            struct cell *cel = &line->cell[i];
            if (cel->fg >= PALETTE_SIZE) cel->fg = pbuf[cel->fg];
            if (cel->bg >= PALETTE_SIZE) cel->bg = pbuf[cel->bg];
        }
    }

    return 1;
}

static color_id_t alloc_color(struct line *line, color_t col) {
    if (line->pal) {
        if (line->pal->size > 0 && line->pal->data[line->pal->size - 1] == col)
            return PALETTE_SIZE + line->pal->size - 1;
        if (line->pal->size > 1 && line->pal->data[line->pal->size - 2] == col)
            return PALETTE_SIZE + line->pal->size - 2;
    }

    if (!line->pal || line->pal->size + 1 > line->pal->caps) {
        if (!optimize_line_palette(line)) return SPECIAL_BG;
        if (!line->pal || line->pal->size + 1 >= line->pal->caps) {
            if (line->pal && line->pal->caps == MAX_EXTRA_PALETTE) return SPECIAL_BG;
            size_t newc = CAPS_INC_STEP(line->pal ? line->pal->caps : 0);
            struct line_palette *new = realloc(line->pal, sizeof(struct line_palette) + newc * sizeof(color_t));
            if (!new) return SPECIAL_BG;
            if (!line->pal) new->size = 0;
            new->caps = newc;
            line->pal = new;
        }
    }

    line->pal->data[line->pal->size++] = col;
    return PALETTE_SIZE + line->pal->size - 1;
}

inline static struct cell fixup_color(struct line *line, struct cursor *cur) {
    struct cell cel = cur->cel;
    if (__builtin_expect(cel.bg >= PALETTE_SIZE, 0))
        cel.bg = alloc_color(line, cur->bg);
    if (__builtin_expect(cel.fg >= PALETTE_SIZE, 0))
        cel.fg = alloc_color(line, cur->fg);
    return cel;
}

inline static void copy_cell(struct line *dst, ssize_t dx, struct line *src, ssize_t sx, bool dmg) {
    struct cell cel = src->cell[sx];
    if (cel.fg >= PALETTE_SIZE) cel.fg = alloc_color(dst,
            src->pal->data[cel.fg - PALETTE_SIZE]);
    if (cel.bg >= PALETTE_SIZE) cel.bg = alloc_color(dst,
            src->pal->data[cel.bg - PALETTE_SIZE]);
    if (dmg) cel.attr &= ~attr_drawn;
    dst->cell[dx] = cel;
}

static struct line *term_create_line(struct term *term, int16_t width) {
    struct line *line = malloc(sizeof(*line) + (size_t)width * sizeof(line->cell[0]));
    if (line) {
        line->width = width;
        line->pal = NULL;
        line->wrapped = 0;
        line->force_damage = 0;
        struct cell cel = fixup_color(line, &term->c);
        for (ssize_t i = 0; i < width; i++)
            line->cell[i] = cel;
    } else warn("Can't allocate line");
    return line;
}

static struct line *term_realloc_line(struct term *term, struct line *line, int16_t width) {
    struct line *new = realloc(line, sizeof(*new) + (size_t)width * sizeof(new->cell[0]));
    if (!new) die("Can't create lines");

    if (width > new->width) {
        struct cell cell = fixup_color(new, &term->c);
        cell.attr = 0;

        for (ssize_t i = new->width; i < width; i++)
            new->cell[i] = cell;
    }

    new->width = width;
    return new;
}

static void term_free_line(struct line *line) {
    if (line) free(line->pal);
    free(line);
}

inline static int16_t line_length(struct line *line) {
    int16_t max_x = line->width;
    if (!line->wrapped)
        while (max_x > 0 && !line->cell[max_x - 1].ch) max_x--;
    return max_x;
}

inline static void term_put_cell(struct term *term, int16_t x, int16_t y, term_char_t ch) {
    struct line *line = term->screen[y];

    // Writing to the line resets its wrapping state
    line->wrapped = 0;

    line->cell[x] = MKCELLWITH(fixup_color(line, &term->c), ch);
}

void term_damage(struct term *term, struct rect damage) {
    if (intersect_with(&damage, &(struct rect) {0, 0, term->width, term->height})) {
        struct line_offset vpos = term_get_view(term);
        term_line_next(term, &vpos, damage.y);
        for (ssize_t i = damage.y; i < damage.y + damage.height; i++) {
            struct line_view line = term_line_at(term, vpos);
            for (ssize_t j = damage.x; j <  MIN(damage.x + damage.width, line.width); j++)
                line.cell[j].attr &= ~attr_drawn;
            if (damage.x >= line.width) {
                if (line.width) line.cell[line.width - 1].attr &= ~attr_drawn;
                else line.line->force_damage = 1;
            }
            term_line_next(term, &vpos, 1);
        }
    }
}

void term_damage_lines(struct term *term, ssize_t ys, ssize_t yd) {
    struct line_offset vpos = term_get_view(term);
    term_line_next(term, &vpos, ys);
    for (ssize_t i = ys; i < yd; i++, term_line_next(term, &vpos, 1))
        term_line_at(term, vpos).line->force_damage = 1;
}

inline static struct line *line_at(struct term *term, ssize_t y) {
    return y >= 0 ? term->screen[y] :
            term->scrollback[(term->sb_top + term->sb_caps + y + 1) % term->sb_caps];
}

inline static bool is_wide(struct line *ln, ssize_t x) {
    return ln->cell[x].attr & attr_wide;
}

inline static ssize_t line_wid(struct line *ln, ssize_t off, ssize_t w) {
    off += w;
    if (off - 1 < ln->width)
        off -= is_wide(ln, off - 1);
    return MIN(off, ln->width);

}

inline static ssize_t line_segments(struct line *ln, ssize_t off, ssize_t w) {
    ssize_t n = off < ln->width || (!ln->width && !off);
    while ((off = line_wid(ln, off, w)) < ln->width) n++;
    return n;
}

struct line_view term_line_at(struct term *term, struct line_offset pos) {
    if (pos.line >= -term->sb_limit && pos.line < term->height) {
        struct line *ln = line_at(term, pos.line);
        ssize_t wid = line_wid(ln, pos.offset, term->width);
        return (struct line_view) {
            .width = wid - pos.offset,
            .wrapped = ln->wrapped || wid < ln->width,
            .cell = ln->cell + pos.offset,
            .line = ln,
        };
    } else return (struct line_view){0};
}


ssize_t term_line_next(struct term *term, struct line_offset *pos, ssize_t amount) {
    struct line *ln;
    if (iconf(ICONF_REWRAP)) {
        // Rewrapping is enabled
        if (amount < 0) {
            if (pos->line - 1 < -term->sb_limit) return amount;
            ln = line_at(term, pos->line);
            // TODO Litle optimization
            amount += line_segments(ln, 0, term->width) - line_segments(ln, pos->offset, term->width);
            pos->offset = 0;
            while (amount < 0) {
                if (pos->line - 1 < -term->sb_limit) return amount;
                ln = line_at(term, --pos->line);
                amount += line_segments(ln, 0, term->width);
            }
        }
        if (amount > 0) {
            if (pos->line >= term->height) return amount;
            ln = line_at(term, pos->line);
            while (pos->line < term->height && amount) {
                if ((pos->offset = line_wid(ln, pos->offset, term->width)) >= ln->width) {
                    pos->offset = 0;
                    if (pos->line + 1 >= term->height) break;
                    ln = line_at(term, ++pos->line);
                }
                amount--;
            }
        }
    } else {
        ssize_t new = MAX(-term->sb_limit, MIN(amount + pos->line, term->height - 1));
        amount -= new - pos->line;
        *pos = (struct line_offset) { new, 0 };
    }
    return amount;
}

bool is_last_line(struct line_view line) {
    return !iconf(ICONF_REWRAP) || line.cell - line.line->cell + line.width >= line.line->width;
}

struct line_offset term_get_view(struct term *term) {
    return term->view_pos;
}

struct line_offset term_get_line_pos(struct term *term, ssize_t y) {
    struct line_offset pos = {0, 0};
    term_line_next(term, &pos, y);
    return pos;
}

static void term_reset_view(struct term *term, bool damage) {
    term->prev_c_view_changed |= !term->view_pos.line;
    ssize_t old_view = term->view;
    term->view_pos = (struct line_offset){0};
    term->view = 0;
    mouse_scroll_view(term, -old_view);
    if (damage) term_damage_lines(term, 0, term->height);
}

static void term_free_scrollback(struct term *term) {
    if (!term->scrollback) return;

    term_reset_view(term, 0);
    for (ssize_t i = 1; i <= (term->sb_caps == term->sb_max_caps ? term->sb_caps : term->sb_limit); i++)
        term_free_line(line_at(term, -i));
    free(term->scrollback);

    term->scrollback = NULL;
    term->sb_caps = 0;
    term->sb_limit = 0;
    term->sb_top = -1;
}

void term_scroll_view(struct term *term, int16_t amount) {
    if (term->mode.altscreen) {
        if (term->mode.altscreen_scroll)
            term_answerback(term, CSI"%d%c", abs(amount), amount > 0 ? 'A' : 'D');
        return;
    }

    bool old_viewr = !term->view_pos.line;

    ssize_t delta = term_line_next(term, &term->view_pos, -amount) + amount;
    if (term->view_pos.line > 0) {
        delta += term->view_pos.line;
        term->view_pos.line = 0;
    }

    term->view += delta;

    if (delta > 0) /* View down, image up */ {
        window_shift(term->win, 0, 0, 0, delta, term->width, term->height - delta, 0);
        term_damage_lines(term, 0, delta);
    } else if (delta < 0) /* View down, image up */ {
        window_shift(term->win, 0, -delta, 0, 0, term->width, term->height + delta, 0);
        term_damage_lines(term, term->height + delta, term->height);
    }

    mouse_scroll_view(term, delta);
    term->prev_c_view_changed |= old_viewr != !term->view_pos.line;
}

static ssize_t term_append_history(struct term *term, struct line *line, bool opt) {
    ssize_t res = 0;
    if (term->sb_max_caps > 0) {
        ssize_t llen = MAX(line_length(line), 1);

        /* If last line in history is wrapped concat current line to it */
        if (iconf(ICONF_REWRAP) && term->sb_limit &&
                line_at(term, -1)->wrapped && llen + line_at(term, -1)->width < MAX_LINE_LEN) {
            struct line **ptop = &term->scrollback[term->sb_top];
            ssize_t oldw = (*ptop)->width;
            *ptop = term_realloc_line(term, *ptop, oldw + llen);
            for (ssize_t k = 0; k < llen; k++)
                copy_cell(*ptop, oldw + k, line, k, 0);

            (*ptop)->wrapped = line->wrapped;

            /* Minimize line size to save memory */
            if (opt && (*ptop)->pal) {
                optimize_line_palette((*ptop));
                struct line_palette *pal = realloc((*ptop)->pal, sizeof(struct line_palette) + sizeof(color_t)*((*ptop)->pal->size));
                if (pal) {
                    (*ptop)->pal = pal;
                    pal->caps = pal->size;
                }
            }

            term_free_line(line);

            if (term->view) term->view++;

        } else {

            /* Minimize line size to save memory */
            if (opt) {
                if (!line->wrapped)
                    line = term_realloc_line(term, line, llen);
                if (line->pal) {
                    optimize_line_palette(line);
                    struct line_palette *pal = realloc(line->pal, sizeof(struct line_palette) + sizeof(color_t)*(line->pal->size));
                    if (pal) {
                        line->pal = pal;
                        pal->caps = pal->size;
                    }
                }
            }

            if (term->sb_limit == term->sb_max_caps) {
                /* If view points to the line that is to be freed, scroll it down */
                if (term->view_pos.line == -term->sb_limit) {
                    if (iconf(ICONF_REWRAP))
                        res = line_segments(line_at(term, -term->sb_limit), term->view_pos.offset, term->width);
                    else
                        res = 1;
                    term->view -= res;
                    term->view_pos.line++;
                    term->view_pos.offset = 0;
                    term->prev_c_view_changed |= !term->view_pos.line;
                }

                /* We reached maximal number of saved lines,
                 * now term->scrollback functions as true cyclic buffer */
                term->sb_top = (term->sb_top + 1) % term->sb_caps;
                SWAP(struct line *, line, term->scrollback[term->sb_top]);
                term_free_line(line);
            } else {
                /* More lines can be saved, term->scrollback is not cyclic yet */

                /* Adjust capacity as needed */
                if (term->sb_limit + 1 >= term->sb_caps) {
                    ssize_t new_cap = CBUF_STEP(term->sb_caps, term->sb_max_caps);
                    struct line **new = realloc(term->scrollback, new_cap * sizeof(*new));
                    if (!new) {
                        term_free_line(line);
                        return res;
                    }
                    term->sb_caps = new_cap;
                    term->scrollback = new;
                }

                /* And just save line */
                term->sb_limit++;
                term->sb_top = (term->sb_top + 1) % term->sb_caps;
                term->scrollback[term->sb_top] = line;
            }

            if (term->view_pos.line) {
                term->view_pos.line--;
                term->view++;
            }
        }
    } else term_free_line(line);
    return res;
}

void term_resize(struct term *term, int16_t width, int16_t height) {

    // First try to read from tty to empty out input queue
    // since this is input from program not yet aware about resize
    term_read(term);

    bool cur_moved = term->c.x == term->width - 1 && term->c.pending;

    // Ensure that term->back_screen is altscreen
    if (term->mode.altscreen) {
        SWAP(struct cursor, term->back_cs, term->cs);
        SWAP(struct line **, term->back_screen, term->screen);
    }

    { // Notify application

        int16_t wwidth, wheight;
        window_get_dim(term->win, &wwidth, &wheight);

        struct winsize wsz = {
            .ws_col = width,
            .ws_row = height,
            .ws_xpixel = wwidth,
            .ws_ypixel = wheight
        };

        if (ioctl(term->fd, TIOCSWINSZ, &wsz) < 0) {
            warn("Can't change tty size");
            term_hang(term);
        }
    }

    { // Resize tabs

        bool *new_tabs = realloc(term->tabs, width * sizeof(*term->tabs));
        if (!new_tabs) die("Can't alloc tabs");
        term->tabs = new_tabs;

        if (width > term->width) {
            memset(new_tabs + term->width, 0, (width - term->width) * sizeof(new_tabs[0]));
            ssize_t tab = term->width ? term->width - 1: 0, tabw = iconf(ICONF_TAB_WIDTH);
            while (tab > 0 && !new_tabs[tab]) tab--;
            while ((tab += tabw) < width) new_tabs[tab] = 1;
        }
    }

    { // Resize altscreen

        for (ssize_t i = height; i < term->height; i++)
            term_free_line(term->back_screen[i]);

        struct line **new_back = realloc(term->back_screen, height * sizeof(term->back_screen[0]));
        if (!new_back) die("Can't allocate lines");
        term->back_screen = new_back;

        for (ssize_t i = 0; i < MIN(term->height, height); i++)
            term->back_screen[i] = term_realloc_line(term, term->back_screen[i], width);

        for (ssize_t i = term->height; i < height; i++)
            if (!(term->back_screen[i] = term_create_line(term, width)))
                die("Can't allocate lines");

        // Adjust altscreen saved cursor position

        term->back_cs.x = MIN(MAX(term->back_cs.x, 0), width - 1);
        term->back_cs.y = MIN(MAX(term->back_cs.y, 0), height - 1);
        if (term->back_cs.pending) term->back_cs.x = width - 1;
    }

    // Clear mouse selection
    // TODO Keep non-rectangular selection
    mouse_clear_selection(term);

    // Find line of bottom left cell
    struct line_offset lower_left = term->view_pos;
    term_line_next(term, &lower_left, term->height - 1);
    bool ll_translated = 0;
    bool to_top = term->view_pos.line <= -term->sb_limit;
    bool to_bottom = !term->view_pos.line;

    {  // Resize main screen

        struct line **new_lines = term->screen;
        ssize_t nnlines = term->height, cursor_par = 0, new_cur_par = 0;

        if (term->width != width && term->width && iconf(ICONF_REWRAP)) {
            ssize_t lline = 0, loff = 0, y= 0;
            nnlines = 0;

            // If first line of screen is continuation line,
            // start with fist line of scrollback
            if (term->sb_limit && line_at(term, -1)->wrapped) {
                loff = line_at(term, -1)->width;
                y = -1;
            }

            bool cset = 0, csset = 0, aset = 0;
            ssize_t par_start = y, approx_cy = 0, dlta = 0;

            term->screen[term->height - 1]->wrapped = 0;

            for (ssize_t i = 0; i < term->height; i++) {
                // Calculate new apporiximate cursor y
                if (!aset && i == term->c.y) {
                    approx_cy = nnlines + (loff + term->c.x) / width,
                    new_cur_par = nnlines;
                    cursor_par = par_start;
                    aset = 1;
                }
                // Calculate new line number
                if (!term->screen[i]->wrapped) {
                    ssize_t len = line_length(term->screen[i]);
                    term->screen[i]->width = len;
                    if (y && !nnlines) {
                        dlta = (len + loff + width - 1)/width -
                                (len + loff - line_at(term, -1)->width + width - 1)/width;
                    }
                    nnlines += (len + loff + width - 1)/width;
                    lline++;
                    loff = 0;
                    par_start = i + 1;
                } else loff += term->width;
            }

            new_cur_par -= dlta;

            // Pop lines from scrollback so cursor row won't be changed
            while (y - 1 > -term->sb_limit && new_cur_par < cursor_par) {
                struct line *line = line_at(term, --y);
                ssize_t delta = (line->width + width - 1) / width;
                new_cur_par += delta;
                approx_cy += delta;
                nnlines += delta;
            }

            nnlines = MAX(nnlines * 2, MAX(MAX(new_cur_par - cursor_par, approx_cy - height - 1), 0) + height * 2);

            new_lines = calloc(nnlines, sizeof(*new_lines));
            if (!new_lines || !(new_lines[0] = term_create_line(term, width)))
                die("Can't allocate line");

            ssize_t y2 = y, dy = 0;
            par_start = 0;
            for (ssize_t dx = 0; y < term->height; y++) {
                struct line *line = line_at(term, y);
                ssize_t len = line->width;
                if (!ll_translated && lower_left.line == y) {
                    lower_left.line = dy;
                    lower_left.offset = dx;
                    ll_translated = 1;
                }
                if (cursor_par == y) new_cur_par = dy;
                for (ssize_t x = 0; x < len; x++) {
                    // If last character of line is wide, soft wrap
                    if (dx == width - 1 && line->cell[x].attr & attr_wide) {
                        new_lines[dy]->wrapped = 1;
                        if (dy < nnlines - 1 && !(new_lines[++dy] = term_create_line(term, width)))
                            die("Can't allocate line");
                        dx = 0;
                    }
                    // Calculate new cursor...
                    if (!cset && term->c.y == y && x == term->c.x) {
                        term->c.y = dy;
                        term->c.x = dx;
                        cset = 1;
                    }
                    // ..and saved cursor position
                    if (!csset && term->cs.y == y && x == term->cs.x) {
                        term->cs.y = dy;
                        term->cs.x = dx;
                        csset = 1;
                    }
                    copy_cell(new_lines[dy], dx, line, x, 0);
                    // Advance line, soft wrap
                    if (++dx == width && (x < len - 1 || line->wrapped)) {
                        new_lines[dy]->wrapped = 1;
                        if (dy < nnlines - 1 && !(new_lines[++dy] = term_create_line(term, width)))
                            die("Can't allocate line");
                        dx = 0;
                    }
                }
                // If cursor is to the right of line end, need to check separately
                if (!cset && term->c.y == y && term->c.x >= len) {
                    term->c.y = dy;
                    term->c.x = MIN(width - 1, term->c.x - len + dx);
                    cset = 1;
                }
                if (!csset && term->cs.y == y && term->cs.x >= len) {
                    term->cs.y = dy;
                    term->cs.x = MIN(width - 1, term->cs.x - len + dx);
                    csset = 1;
                }
                // Advance line, hard wrap
                if (!line->wrapped) {
                    if (dy < nnlines - 1 && !(new_lines[++dy] = term_create_line(term, width)))
                        die("Can't allocate line");
                    par_start = dy;
                    dx = 0;
                }

                // Pop from scrollback
                if (y < 0) term->scrollback[(term->sb_top + term->sb_caps + y + 1) % term->sb_caps] = NULL;
                term_free_line(line);
            }

            // Update scrollback data
            if (term->sb_limit) {
                term->sb_top = (term->sb_top + y2 + term->sb_caps) % term->sb_caps;
                term->sb_limit += y2;
            }
            if (!ll_translated) lower_left.line -= y2;

            if (dy < nnlines) dy++;

            nnlines = dy;
        }

        // Push extra lines from top back to scrollback
        ssize_t start = 0, scrolled = 0;
        while (term->c.y > height - 1 || new_cur_par > cursor_par) {
            if (term->sb_limit && line_at(term, -1)->wrapped) {
                if (lower_left.line >= 0) {
                    if (!lower_left.line) lower_left.offset += line_at(term, -1)->width;
                    lower_left.line--;
                }
            } else lower_left.line--;

            scrolled = term_append_history(term, new_lines[start++], 1);
            new_cur_par--;
            term->c.y--;
            term->cs.y--;
        }

        ssize_t minh = MIN(nnlines - start, height);

        // Resize lines if rewrapping is disabled
        if (!iconf(ICONF_REWRAP)) {
            if (scrolled) {
                window_shift(term->win, 0, scrolled, 0, 0, term->width, term->height - scrolled, 0);
                term_damage_lines(term, term->height - scrolled, term->height);
            } else if (start && !term->view_pos.line) {
                window_shift(term->win, 0, start, 0, 0, MIN(term->width, width), height - start, 0);
            }
            for (ssize_t i = 0; i < minh; i++) {
                if (new_lines[i + start]->width < width || iconf(ICONF_CUT_LINES)) {
                    new_lines[i + start] = term_realloc_line(term, new_lines[i + start], width);
                }
            }
        }

        // Adjust cursor
        term->cs.y = MAX(MIN(term->cs.y, height - 1), 0);
        if (term->cs.pending) term->cs.x = width - 1;
        term->c.y = MAX(MIN(term->c.y, height - 1), 0);
        if (term->c.pending) term->c.x = width - 1;

        // Free extra lines from bottom
        for (ssize_t i = start + height; i < nnlines; i++)
            term_free_line(new_lines[i]);

        // Free old line buffer
        if (new_lines != term->screen) free(term->screen);

        // Resize line buffer
        memmove(new_lines, new_lines + start, minh * sizeof(*new_lines));
        new_lines = realloc(new_lines, height * sizeof(*new_lines));
        if (!new_lines)  die("Can't allocate lines");

        // Allocate new empty lines
        for (ssize_t i = minh; i < height; i++)
            if (!(new_lines[i] = term_create_line(term, width)))
                die("Can't allocate lines");

        term->screen = new_lines;
    }

    int16_t minh = MIN(height, term->height);
    int16_t minw = MIN(width, term->width);
    int16_t dx = width - term->width;
    int16_t dy = height - term->height;

    // Set state
    term->width = width;
    term->height = height;
    term->left = term->top = 0;
    term->right = width - 1;
    term->bottom = height - 1;

    {  // Fixup view

        // Reposition view
        if (iconf(ICONF_REWRAP)) {
            if (to_bottom) {
                // Stick to bottom
                term->view_pos.offset = 0;
                term->view_pos.line = 0;
            } else if (to_top) {
                // Stick to top
                term->view_pos.offset = 0;
                term->view_pos.line = -term->sb_limit;
            } else {
                // Keep line of lower left view cell at the bottom
                lower_left.offset -= lower_left.offset % width;
                ssize_t hei = height;
                if (lower_left.line >= term->height) {
                    hei = MAX(0, hei - (lower_left.line - term->height));
                    lower_left.line = height - 1;
                }
                term_line_next(term, &lower_left, 1 - hei);
                term->view_pos = lower_left;
                if (term->view_pos.line >= 0 || term->view_pos.line < -term->sb_limit)
                    term->view_pos.offset = 0;
            }
            term->view_pos.line = MAX(MIN(0, term->view_pos.line), -term->sb_limit);

            // Restore view offset
            term->view = 0;
            ssize_t vline = term->view_pos.line;
            while (vline++ < 0) term->view += line_segments(line_at(term, vline - 1), 0, term->width);
        }
    }

    // Damage screen
    if (!term->mode.altscreen && iconf(ICONF_REWRAP)) {
        // Just damage everything if rewrapping is enabled
        term_damage_lines(term, 0, term->height);
    } else {
        if (!term->mode.altscreen && !iconf(ICONF_REWRAP)) {
            // Damage changed parts
            if (dy > 0) term_damage(term, (struct rect) { 0, minh, minw, dy });
            if (dx > 0) term_damage(term, (struct rect) { minw, 0, dx, height });
        }
        if (cur_moved){
            term->back_screen[term->c.y]->cell[term->c.x].attr &= ~attr_drawn;
            term->back_screen[term->c.y]->cell[MAX(term->c.x - 1, 0)].attr &= ~attr_drawn;
        }
    }

    if (term->mode.altscreen) {
        SWAP(struct cursor, term->back_cs, term->cs);
        SWAP(struct line **, term->back_screen, term->screen);
    }

}

bool term_is_cursor_enabled(struct term *term) {
    return !term->mode.hide_cursor && !term->view_pos.line;
}

bool term_redraw(struct term *term) {
    bool c_hidden = !term_is_cursor_enabled(term);

    if (term->c.x != term->prev_c_x || term->c.y != term->prev_c_y ||
            term->prev_c_hidden != c_hidden || term->prev_c_view_changed) {
        if (!c_hidden) term->screen[term->c.y]->cell[term->c.x].attr &= ~attr_drawn;
        if ((!term->prev_c_hidden || term->prev_c_view_changed) && term->prev_c_y < term->height && term->prev_c_x < term->width)
            term->screen[term->prev_c_y]->cell[term->prev_c_x].attr &= ~attr_drawn;
    }

    term->prev_c_x = term->c.x;
    term->prev_c_y = term->c.y;
    term->prev_c_hidden = c_hidden;
    term->prev_c_view_changed = 0;

    struct line *cl = term->screen[term->c.y];

    bool cursor = !term->prev_c_hidden && (!(cl->cell[term->c.x].attr & attr_drawn) || cl->force_damage);

    return window_submit_screen(term->win, term->palette, term->c.x, term->c.y, cursor, term->c.pending);
}


/* Get min/max column/row */
int16_t term_max_y(struct term *term) {
    return term->bottom + 1;
}

int16_t term_min_y(struct term *term) {
    return term->top;
}

int16_t term_max_x(struct term *term) {
    return term->mode.lr_margins ? term->right + 1 : term->width;
}

int16_t term_min_x(struct term *term) {
    return term->mode.lr_margins ? term->left : 0;
}

int16_t term_width(struct term *term) {
    return term->width;
}

int16_t term_height(struct term *term) {
    return term->height;
}

/* Get min/max column/row WRT origin */
inline static int16_t term_max_ox(struct term *term) {
    return (term->mode.lr_margins && term->c.origin) ? term->right + 1: term->width;
}

inline static int16_t term_min_ox(struct term *term) {
    return (term->mode.lr_margins && term->c.origin) ? term->left : 0;
}

inline static int16_t term_max_oy(struct term *term) {
    return term->c.origin ? term->bottom + 1: term->height;
}

inline static int16_t term_min_oy(struct term *term) {
    return term->c.origin ? term->top : 0;
}

inline static void term_esc_start(struct term *term) {
    term->esc.selector = 0;
}

inline static void term_esc_start_seq(struct term *term) {
    for (size_t i = 0; i <= term->esc.i; i++)
        term->esc.param[i] = -1;
    term->esc.i = 0;
    term->esc.subpar_mask = 0;
    term->esc.selector = 0;
}

inline static uint8_t *term_esc_str(struct term *term) {
    return term->esc.str_ptr ? term->esc.str_ptr : term->esc.str_data;
}

inline static void term_esc_start_string(struct term *term) {
    term->esc.str_len = 0;
    term->esc.str_data[0] = 0;
    term->esc.str_cap = ESC_MAX_STR;
    term->esc.selector = 0;
}

inline static void term_esc_finish_string(struct term *term) {
    free(term->esc.str_ptr);
    term->esc.str_ptr = NULL;
}

static void term_esc_dump(struct term *term, bool use_info) {
    if (use_info && !iconf(ICONF_TRACE_CONTROLS)) return;

    char *pref = use_info ? "Seq: " : "Unrecognized ";

    char buf[ESC_DUMP_MAX] = "^[";
    size_t pos = 2;
    switch (term->esc.state) {
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
            for (size_t i = 0; i < term->esc.i; i++) {
                pos += snprintf(buf + pos, ESC_DUMP_MAX - pos, "%d", term->esc.param[i]);
                if (i < term->esc.i - 1) buf[pos++] = term->esc.subpar_mask & (1 << (i + 1)) ? ':' : ';' ;
            }
            if (term->esc.selector & I0_MASK)
                buf[pos++] = 0x20 + ((term->esc.selector & I0_MASK) >> 9) - 1;
            if (term->esc.selector & I1_MASK)
                buf[pos++] = 0x20 + ((term->esc.selector & I1_MASK) >> 14) - 1;
            buf[pos++] = (C_MASK & term->esc.selector) + 0x40;
            if (term->esc.state != esc_dcs_string) break;

            buf[pos] = 0;
            (use_info ? info : warn)("%s%s%s^[\\", pref, buf, term_esc_str(term));
            return;
        case esc_osc_string:
            (use_info ? info : warn)("%s^[]%u;%s^[\\", pref, term->esc.selector, term_esc_str(term));
        default:
            return;
    }
    buf[pos] = 0;
    (use_info ? info : warn)("%s%s", pref, buf);
}

static size_t term_decode_color(struct term *term, size_t arg, color_t *rcol, color_id_t *rcid, color_id_t *valid) {
    *valid = 0;
    size_t argc = arg + 1 < ESC_MAX_PARAM;
    bool subpars = arg && (term->esc.subpar_mask >> (arg + 1)) & 1;
    for (size_t i = arg + 1; i < ESC_MAX_PARAM &&
        !subpars ^ ((term->esc.subpar_mask >> i) & 1); i++) argc++;
    if (argc > 0) {
        if (term->esc.param[arg] == 2 && argc > 3) {
            color_t col = 0xFF;
            bool wrong = 0, space = subpars && argc > 4;
            for (size_t i = 1 + space; i < 4U + space; i++) {
                wrong |= term->esc.param[arg + i] > 255;
                col = (col << 8) + MIN(MAX(0, term->esc.param[arg + i]), 0xFF);
            }
            if (wrong) term_esc_dump(term, 1);
            *rcol = col;
            *rcid = 0xFFFF;
            *valid = 1;
            if (!subpars) argc = MIN(argc, 4);
        } else if (term->esc.param[arg] == 5 && argc > 1) {
            if (term->esc.param[arg + 1] < PALETTE_SIZE - SPECIAL_PALETTE_SIZE && term->esc.param[arg + 1] >= 0) {
                *rcid = term->esc.param[arg + 1];
                *valid = 1;
            } else term_esc_dump(term, 0);
            if (!subpars) argc = MIN(argc, 2);
        } else {
            if (!subpars) argc = MIN(argc, 1);
             term_esc_dump(term, 0);
         }
    } else term_esc_dump(term, 0);
    return argc;
}

static void term_decode_sgr(struct term *term, size_t i, struct cell *mask, struct cell *val, color_t *fg, color_t *bg) {
#define SET(f) (mask->attr |= (f), val->attr |= (f))
#define RESET(f) (mask->attr |= (f), val->attr &= ~(f))
#define SETFG(f) (mask->fg = 1, val->fg = (f))
#define SETBG(f) (mask->bg = 1, val->bg = (f))
    do {
        uparam_t par = PARAM(i, 0);
        if ((term->esc.subpar_mask >> i) & 1) return;
        switch (par) {
        case 0:
            RESET(0xFF);
            SETFG(SPECIAL_FG);
            SETBG(SPECIAL_BG);
            break;
        case 1:  SET(attr_bold); break;
        case 2:  SET(attr_faint); break;
        case 3:  SET(attr_italic); break;
        case 21: /* <- should be double underlind */
        case 4:
            if (i < term->esc.i && (term->esc.subpar_mask >> (i + 1)) & 1 &&
                term->esc.param[++i] <= 0) RESET(attr_underlined);
            else SET(attr_underlined);
            break;
        case 5:  /* <- should be slow blink */
        case 6:  SET(attr_blink); break;
        case 7:  SET(attr_inverse); break;
        case 8:  SET(attr_invisible); break;
        case 9:  SET(attr_strikethrough); break;
        case 22: RESET(attr_faint | attr_bold); break;
        case 23: RESET(attr_italic); break;
        case 24: RESET(attr_underlined); break;
        case 25: /* <- should be slow blink reset */
        case 26: RESET(attr_blink); break;
        case 27: RESET(attr_inverse); break;
        case 28: RESET(attr_invisible); break;
        case 29: RESET(attr_strikethrough); break;
        case 30: case 31: case 32: case 33:
        case 34: case 35: case 36: case 37:
            SETFG(par - 30); break;
        case 38:
            i += term_decode_color(term, i + 1, fg, &val->fg, &mask->fg);
            break;
        case 39: SETFG(SPECIAL_FG); break;
        case 40: case 41: case 42: case 43:
        case 44: case 45: case 46: case 47:
            SETBG(par - 40); break;
        case 48:
            i += term_decode_color(term, i + 1, bg, &val->bg, &mask->bg);
            break;
        case 49: SETBG(SPECIAL_BG); break;
        case 90: case 91: case 92: case 93:
        case 94: case 95: case 96: case 97:
            SETFG(par - 90); break;
        case 100: case 101: case 102: case 103:
        case 104: case 105: case 106: case 107:
            SETBG(par - 100); break;
        default:
            term_esc_dump(term, 0);
        }
    } while (++i < term->esc.i);
#undef SET
#undef RESET
#undef SETFG
#undef SETBG
}


inline static void term_rect_pre(struct term *term, int16_t *xs, int16_t *ys, int16_t *xe, int16_t *ye) {
    *xs = MAX(term_min_oy(term), MIN(*xs, term_max_ox(term) - 1));
    *xe = MAX(term_min_oy(term), MIN(*xe, term_max_ox(term)));
    *ys = MAX(term_min_oy(term), MIN(*ys, term_max_oy(term) - 1));
    *ye = MAX(term_min_oy(term), MIN(*ye, term_max_oy(term)));
}

inline static void term_erase_pre(struct term *term, int16_t *xs, int16_t *ys, int16_t *xe, int16_t *ye, bool origin) {
    if (origin) term_rect_pre(term, xs, ys, xe, ye);
    else {
        *xs = MAX(0, MIN(*xs, term->width - 1));
        *xe = MAX(0, MIN(*xe, term->width));
        *ys = MAX(0, MIN(*ys, term->height - 1));
        *ye = MAX(0, MIN(*ye, term->height));
    }

    window_delay(term->win);
    mouse_selection_erase(term, (struct rect){ *xs, *ys, *xe - *xs, *ye - *ys});
}

static uint16_t term_checksum(struct term *term, int16_t xs, int16_t ys, int16_t xe, int16_t ye) {
    term_rect_pre(term, &xs, &ys, &xe, &ye);

    // TODO Test this thing

    uint32_t res = 0, spc = 0, trm = 0;
    enum charset gr = term->c.gn[term->c.gr];
    bool first = 1, notrim = term->checksum_mode.no_trim;

    for (; ys < ye; ys++) {
        struct line *line = term->screen[ys];
        for (int16_t i = xs; i < xe; i++) {
            term_char_t ch = line->cell[i].ch;
            uint32_t attr = line->cell[i].attr;
            if (!(term->checksum_mode.no_implicit) && !ch) ch = ' ';

            if (!(term->checksum_mode.wide)) {
                if (ch > 0x7F && gr != cs94_ascii) {
                    nrcs_encode(gr, &ch, term->mode.enable_nrcs);
                    if (!(term->checksum_mode.eight_bit) && ch < 0x80) ch |= 0x80;
                }
                ch &= 0xFF;
            }
            if (!(term->checksum_mode.no_attr)) {
                if (attr & attr_underlined) ch += 0x10;
                if (attr & attr_inverse) ch += 0x20;
                if (attr & attr_blink) ch += 0x40;
                if (attr & attr_bold) ch += 0x80;
                if (attr & attr_italic) ch += 0x100;
                if (attr & attr_faint) ch += 0x200;
                if (attr & attr_strikethrough) ch += 0x400;
                if (attr & attr_invisible) ch += 0x800;
            }
            if (first || line->cell[i].ch || (attr & 0xFF))
                trm += ch + spc, spc = 0;
            else if (!line->cell[i].ch && notrim) spc += ' ';

            res += ch;
            first = notrim;
        }
        if (!notrim) spc = first = 0;
    }

    if (!notrim) res = trm;
    return term->checksum_mode.positive ? res : -res;
}

static void term_reverse_sgr(struct term *term, int16_t xs, int16_t ys, int16_t xe, int16_t ye) {
    term_erase_pre(term, &xs, &ys, &xe, &ye, 1);
    color_t fg = 0, bg = 0;
    struct cell mask = {0}, val = {0};
    term_decode_sgr(term, 4, &mask, &val, &fg, &bg);

    bool rect = term->mode.attr_ext_rectangle;
    for (; ys < ye; ys++) {
        struct line *line = term->screen[ys];
        for (int16_t i = xs; i < (rect || ys == ye - 1 ? xe : term_max_ox(term)); i++) {
            line->cell[i].attr ^= mask.attr;
            line->cell[i].attr &= ~attr_drawn;
        }
        if (!rect) xs = term_min_ox(term);
    }
}

static void term_encode_sgr(char *dst, char *end, struct cell cel, color_t fg, color_t bg) {
    // Maximal sequence is 0;1;2;3;4;6;7;8;9;38:2:255:255:255;48:2:255:255:255
    // 64 byte buffer is enough
#define FMT(...) dst += snprintf(dst, end - dst, __VA_ARGS__)
    // Reset everything
    FMT("0");

    // Encode attributes
    if (cel.attr & attr_bold) FMT(";1");
    if (cel.attr & attr_faint) FMT(";2");
    if (cel.attr & attr_italic) FMT(";3");
    if (cel.attr & attr_underlined) FMT(";4");
    if (cel.attr & attr_blink) FMT(";6");
    if (cel.attr & attr_inverse) FMT(";7");
    if (cel.attr & attr_invisible) FMT(";8");
    if (cel.attr & attr_strikethrough) FMT(";9");

    // Encode foreground color
    if (cel.fg < 8) FMT(";%d", 30 + cel.fg);
    else if (cel.fg < 16) FMT(";%d", 90 + cel.fg - 8);
    else if (cel.fg < PALETTE_SIZE - SPECIAL_PALETTE_SIZE) FMT(";38:5:%d", cel.fg);
    else if (cel.fg == SPECIAL_FG) /* FMT(";39") -- default, skip */;
    else FMT(";38:2:%d:%d:%d", fg >> 16 & 0xFF, fg >>  8 & 0xFF, fg >>  0 & 0xFF);

    // Encode background color
    if (cel.bg < 8) FMT(";%d", 40 + cel.bg);
    else if (cel.bg < 16) FMT(";%d", 100 + cel.bg - 8);
    else if (cel.bg < PALETTE_SIZE - SPECIAL_PALETTE_SIZE) FMT(";48:5:%d", cel.bg);
    else if (cel.bg == SPECIAL_BG) /* FMT(";49") -- default, skip */;
    else FMT(";48:2:%d:%d:%d", bg >> 16 & 0xFF, bg >>  8 & 0xFF, bg >>  0 & 0xFF);
#undef FMT
}

static void term_report_sgr(struct term *term, int16_t xs, int16_t ys, int16_t xe, int16_t ye) {
    term_rect_pre(term, &xs, &ys, &xe, &ye);

    if (ys >= ye || xs >= xe) {
        // Invalid rectangle
        term_answerback(term, CSI"0m");
        return;
    }

    struct cell common = term->screen[ys]->cell[xs];
    color_t common_fg = 0, common_bg = 0;
    bool has_common_fg = 1, has_common_bg = 1;
    bool true_fg = 0, true_bg = 0;

    if (common.fg >= PALETTE_SIZE && term->screen[ys]->pal)
        common_fg = term->screen[ys]->pal->data[common.fg - PALETTE_SIZE], true_fg = 1;
    if (common.bg >= PALETTE_SIZE && term->screen[ys]->pal)
        common_bg = term->screen[ys]->pal->data[common.bg - PALETTE_SIZE], true_bg = 1;

    for (; ys < ye; ys++) {
        struct line *line = term->screen[ys];
        for (int16_t i = xs; i < xe; i++) {
            has_common_fg &= (common.fg == line->cell[i].fg || (true_fg && line->cell[i].fg >= PALETTE_SIZE &&
                    common_fg == line->pal->data[line->cell[i].fg - PALETTE_SIZE]));
            has_common_bg &= (common.bg == line->cell[i].bg || (true_bg && line->cell[i].bg >= PALETTE_SIZE &&
                    common_bg == line->pal->data[line->cell[i].bg - PALETTE_SIZE]));
            common.attr &= line->cell[i].attr;
        }
    }

    if (!has_common_bg) common.bg = SPECIAL_BG;
    if (!has_common_fg) common.fg = SPECIAL_FG;

    char sgr[SGR_BUFSIZ];
    term_encode_sgr(sgr, sgr + sizeof sgr, common, common_fg, common_bg);
    term_answerback(term, CSI"%sm", sgr);
}

static void term_apply_sgr(struct term *term, int16_t xs, int16_t ys, int16_t xe, int16_t ye) {
    term_erase_pre(term, &xs, &ys, &xe, &ye, 1);
    color_t fg = 0, bg = 0;
    struct cell mask = {0}, val = {0};
    term_decode_sgr(term, 4, &mask, &val, &fg, &bg);

    mask.attr |= attr_drawn;
    val.attr &= ~attr_drawn;
    bool rect = term->mode.attr_ext_rectangle;
    for (; ys < ye; ys++) {
        struct line *line = term->screen[ys];
        if (val.fg >= PALETTE_SIZE) val.fg = alloc_color(line, fg);
        if (val.bg >= PALETTE_SIZE) val.bg = alloc_color(line, bg);
        for (int16_t i = xs; i < (rect || ys == ye - 1 ? xe : term_max_ox(term)); i++) {
            struct cell *cel = &line->cell[i];
            cel->attr = (cel->attr & ~mask.attr) | (val.attr & mask.attr);
            if (mask.fg) cel->fg = val.fg;
            if (mask.bg) cel->bg = val.bg;
        }
        if (!rect) xs = term_min_ox(term);
    }
}

static void term_copy(struct term *term, int16_t xs, int16_t ys, int16_t xe, int16_t ye, int16_t xd, int16_t yd, bool origin) {
    if (ye < ys) SWAP(int16_t, ye, ys);
    if (xe < xs) SWAP(int16_t, xe, xs);

    if (origin) {
        xs = MAX(term_min_ox(term), MIN(xs, term_max_ox(term) - 1));
        ys = MAX(term_min_oy(term), MIN(ys, term_max_oy(term) - 1));
        xd = MAX(term_min_ox(term), MIN(xd, term_max_ox(term) - 1));
        yd = MAX(term_min_oy(term), MIN(yd, term_max_oy(term) - 1));
        xe = MAX(term_min_ox(term), MIN(MIN(xe - xs + xd, term_max_ox(term)) - xd + xs, term_max_ox(term)));
        ye = MAX(term_min_oy(term), MIN(MIN(ye - ys + yd, term_max_oy(term)) - yd + ys, term_max_oy(term)));
    } else {
        xs = MAX(0, MIN(xs, term->width - 1));
        ys = MAX(0, MIN(ys, term->height - 1));
        xd = MAX(0, MIN(xd, term->width - 1));
        yd = MAX(0, MIN(yd, term->height - 1));
        xe = MAX(0, MIN(MIN(xe - xs + xd, term->width) - xd + xs, term->width));
        ye = MAX(0, MIN(MIN(ye - ys + yd, term->height) - yd + ys, term->height));
    }

    if (xs >= xe || ys >= ye) return;

    bool dmg = !!term->view_pos.line || !window_shift(term->win, xs, ys, xd, yd, xe - xs, ye - ys, 1);

    if (yd < ys || (yd == ys && xd < xs)) {
        for (; ys < ye; ys++, yd++) {
            struct line *sl = term->screen[ys], *dl = term->screen[yd];
            // Reset line wrapping state
            dl->wrapped = 0;
            for (int16_t x1 = xs, x2 = xd; x1 < xe; x1++, x2++)
                copy_cell(dl, x2, sl, x1, dmg);
        }
    } else {
        for (yd += ye - ys, xd += xe - xs; ys < ye; ye--, yd--) {
            struct line *sl = term->screen[ye - 1], *dl = term->screen[yd - 1];
            // Reset line wrapping state
            dl->wrapped = 0;
            for (int16_t x1 = xe, x2 = xd; x1 > xs; x1--, x2--)
                copy_cell(dl, x2 - 1, sl, x1 - 1, dmg);
        }
    }
}

static void term_fill(struct term *term, int16_t xs, int16_t ys, int16_t xe, int16_t ye, bool origin, term_char_t ch) {
    term_erase_pre(term, &xs, &ys, &xe, &ye, origin);

    for (; ys < ye; ys++) {
        struct line *line = term->screen[ys];
        // Reset line wrapping state
        line->wrapped = 0;
        struct cell cell = fixup_color(line, &term->c);
        cell.ch = ch;
        cell.attr = 0;
        for (int16_t i = xs; i < xe; i++)
            line->cell[i] = cell;
    }
}

static void term_erase(struct term *term, int16_t xs, int16_t ys, int16_t xe, int16_t ye, bool origin) {
    term_fill(term, xs, ys, xe, ye, origin, 0);
}

static void term_protective_erase(struct term *term, int16_t xs, int16_t ys, int16_t xe, int16_t ye, bool origin) {
    term_erase_pre(term, &xs, &ys, &xe, &ye, origin);

    for (; ys < ye; ys++) {
        struct line *line = term->screen[ys];
        // Reset line wrapping state
        line->wrapped = 0;
        struct cell cell = fixup_color(line, &term->c);
        cell.attr = 0;
        for (int16_t i = xs; i < xe; i++)
            if (!(line->cell[i].attr & attr_protected))
                line->cell[i] = cell;
    }
}

static void term_selective_erase(struct term *term, int16_t xs, int16_t ys, int16_t xe, int16_t ye, bool origin) {
    term_erase_pre(term, &xs, &ys, &xe, &ye, origin);

    for (; ys < ye; ys++) {
        struct line *line = term->screen[ys];
        // Reset line wrapping state
        line->wrapped = 0;
        for (int16_t i = xs; i < xe; i++)
            if (!(line->cell[i].attr & attr_protected))
                line->cell[i] = MKCELLWITH(line->cell[i], 0);
    }
}


inline static void term_adjust_wide_left(struct term *term, int16_t x, int16_t y) {
    if (x < 1) return;
    struct cell *cell = &term->screen[y]->cell[x - 1];
    if (cell->attr & attr_wide) {
        cell->attr &= ~(attr_wide|attr_drawn);
        cell->ch = 0;
    }
}

inline static void term_adjust_wide_right(struct term *term, int16_t x, int16_t y) {
    if (x >= term->screen[y]->width - 1) return;
    struct cell *cell = &term->screen[y]->cell[x + 1];
    if (cell[-1].attr & attr_wide) {
        cell->attr &= ~attr_drawn;
        cell->ch = 0;
    }
}

inline static void term_reset_pending(struct term *term) {
    term->c.pending = 0;
}

static void term_move_to(struct term *term, int16_t x, int16_t y) {
    term->c.x = MIN(MAX(x, 0), term->width - 1);
    term->c.y = MIN(MAX(y, 0), term->height - 1);
    term_reset_pending(term);
}


static void term_bounded_move_to(struct term *term, int16_t x, int16_t y) {
    term->c.x = MIN(MAX(x, term_min_x(term)), term_max_x(term) - 1);
    term->c.y = MIN(MAX(y, term_min_y(term)), term_max_y(term) - 1);
    term_reset_pending(term);
}

static void term_move_left(struct term *term, int16_t amount) {
    int16_t x = term->c.x, y = term->c.y,
        first_left = x < term_min_x(term) ? 0 : term_min_x(term);


    // This is a hack that allows using proper line editing with reverse wrap
    // mode while staying compatible with VT100 wrapping mode
    if (term->mode.reverse_wrap) x += term->c.pending;

    if (amount > x - first_left && term->mode.wrap && term->mode.reverse_wrap) {
        bool in_tbm = term_min_y(term) <= term->c.y && term->c.y < term_max_y(term);
        int16_t height = in_tbm ? term_max_y(term) - term_min_y(term) : term->height;
        int16_t top = in_tbm ? term_min_y(term) : 0;

        amount -= x - first_left;
        x = term_max_x(term);
        y -= 1 + amount/(term_max_x(term) - term_min_x(term));
        amount %= term_max_x(term) - term_min_x(term);

        y = (y - top) % height + top;
        if (y < top) y += height;
    }

    (term->c.x >= term_min_x(term) ? term_bounded_move_to : term_move_to)(term, x - amount, y);
}

static void term_cursor_mode(struct term *term, bool mode) {
    if (mode) /* save */ {
        term->cs = term->c;
    } else /* restore */ {
        term->c = term->cs;
        term->c.x = MIN(term->c.x, term->width - 1);
        term->c.y = MIN(term->c.y, term->height - 1);
    }
}

static void term_swap_screen(struct term *term, bool damage) {
    term->mode.altscreen ^= 1;
    SWAP(struct cursor, term->back_cs, term->cs);
    SWAP(struct line **, term->back_screen, term->screen);
    term_reset_view(term, damage);
    if (damage) mouse_clear_selection(term);
}

static void term_scroll_horizontal(struct term *term, int16_t left, int16_t amount) {
    int16_t top = term_min_y(term), right = term_max_x(term), bottom = term_max_y(term);

    if (term->prev_c_y >= 0 && top <= term->prev_c_y && term->prev_c_y < bottom &&
        left <= term->prev_c_y && term->prev_c_x < right) {
        term->screen[term->prev_c_y]->cell[term->prev_c_x].attr &= ~attr_drawn;
        term->prev_c_x = MAX(left, MIN(right, term->prev_c_x - amount));
    }

    for (int16_t i = top; i < bottom; i++) {
        term_adjust_wide_left(term, left, i);
        term_adjust_wide_right(term, right - 1, i);
    }

    if (amount > 0) { /* left */
        amount = MIN(amount, right - left);
        term_copy(term, left + amount, top, right, bottom, left, top, 0);
        term_erase(term, right - amount, top, right, bottom, 0);
    } else { /* right */
        amount = MIN(-amount, right - left);
        term_copy(term, left, top, right - amount, bottom, left + amount, top, 0);
        term_erase(term, left, top, left + amount, bottom, 0);
    }
}

static void term_scroll(struct term *term, int16_t top, int16_t amount, bool save) {
    int16_t left = term_min_x(term), right = term_max_x(term), bottom = term_max_y(term);

    if (left == 0 && right == term->width) { // Fast scrolling without margins
        if (top <= term->prev_c_y && term->prev_c_y < bottom) {
            term->screen[term->prev_c_y]->cell[term->prev_c_x].attr &= ~attr_drawn;
            if (amount >= 0) term->prev_c_y = MAX(top, term->prev_c_y - amount);
            else term->prev_c_y = MIN(bottom, term->prev_c_y - amount);
        }

        if (amount > 0) { /* up */
            amount = MIN(amount, (bottom - top));
            int16_t rest = (bottom - top) - amount;

            if (save && !term->mode.altscreen && term->top == top) {
                ssize_t scrolled = 0;
                for (int16_t i = 0; i < amount; i++) {
                    scrolled -= term_append_history(term, term->screen[top + i],  1);
                    term->screen[top + i] = term_create_line(term, term->width);
                }

                if (scrolled < 0) /* View down, image up */ {
                    term_damage_lines(term, term->height + scrolled, term->height);
                    window_shift(term->win, 0, -scrolled, 0, 0, term->width, term->height + scrolled, 0);
                    mouse_scroll_view(term, scrolled);
                }
            } else term_erase(term, 0, top, term->width, top + amount, 0);

            for (int16_t i = 0; i < rest; i++)
                SWAP(struct line *, term->screen[top + i], term->screen[top + amount + i]);

            if (term->view_pos.line || !window_shift(term->win,
                    0, top + amount, 0, top, term->width, bottom - top - amount, 1)) {
                for (int16_t i = top; i < bottom - amount; i++)
                     term->screen[i]->force_damage = 1;
            }
        } else { /* down */
            amount = MAX(amount, -(bottom - top));
            int16_t rest = (bottom - top) + amount;

            term_erase(term, 0, bottom + amount, term->width, bottom, 0);

            for (int16_t i = 1; i <= rest; i++)
                SWAP(struct line *, term->screen[bottom - i], term->screen[bottom + amount - i]);

            if (term->view_pos.line || !window_shift(term->win, 0, top,
                    0, top - amount, term->width, bottom - top + amount, 1)) {
                for (int16_t i = top - amount; i < bottom; i++)
                     term->screen[i]->force_damage = 1;
            }
        }
    } else { // Slow scrolling with margins

        if (term->prev_c_y >= 0 && top <= term->prev_c_y && term->prev_c_y < bottom &&
            left <= term->prev_c_y && term->prev_c_x < right) {
            term->screen[term->prev_c_y]->cell[term->prev_c_x].attr &= ~attr_drawn;
            term->prev_c_y = MAX(top, MIN(bottom, term->prev_c_y - amount));
        }

        for (int16_t i = top; i < bottom; i++) {
            term_adjust_wide_left(term, left, i);
            term_adjust_wide_right(term, right - 1, i);
        }

        if (amount > 0) { /* up */
            amount = MIN(amount, bottom - top);

            if (save && !term->mode.altscreen && term->top == top) {
                ssize_t scrolled = 0;
                for (int16_t i = 0; i < amount; i++) {
                    struct line *ln = term_create_line(term, line_length(term->screen[top + i]));
                    for (ssize_t k = term_min_x(term); k < MIN(term_max_x(term), ln->width); k++)
                        copy_cell(ln, k, term->screen[top + i], k, 0);
                    scrolled -= term_append_history(term, ln, 0);
                }
                if (scrolled < 0) /* View down, image up */ {
                    term_damage_lines(term, term->height + scrolled, term->height);
                    window_shift(term->win, 0, -scrolled, 0, 0, term->width, term->height + scrolled, 0);
                    mouse_scroll_view(term, scrolled);
                }
            }

            term_copy(term, left, top + amount, right, bottom, left, top, 0);
            term_erase(term, left, bottom - amount, right, bottom, 0);
        } else { /* down */
            amount = MIN(-amount, bottom - top);
            term_copy(term, left, top, right, bottom - amount, left, top + amount, 0);
            term_erase(term, left, top, right, top + amount, 0);
        }
    }
    mouse_scroll_selection(term, amount, save);
}

static void term_set_tb_margins(struct term *term, int16_t top, int16_t bottom) {
    if (top < bottom) {
        term->top = MAX(0, MIN(term->height - 1, top));
        term->bottom = MAX(0, MIN(term->height - 1, bottom));
    } else {
        term->top = 0;
        term->bottom = term->height - 1;
    }
}

static void term_set_lr_margins(struct term *term, int16_t left, int16_t right) {
    if (left < right) {
        term->left = MAX(0, MIN(term->width - 1, left));
        term->right = MAX(0, MIN(term->width - 1, right));
    } else {
        term->left = 0;
        term->right = term->width - 1;
    }
}

static void term_reset_margins(struct term *term) {
    term->top = 0;
    term->left = 0;
    term->bottom = term->height - 1;
    term->right = term->width - 1;
}

inline static bool term_cursor_in_region(struct term *term) {
    return term->c.x >= term_min_x(term) && term->c.x < term_max_x(term) &&
            term->c.y >= term_min_y(term) && term->c.y < term_max_y(term);
}

static void term_insert_cells(struct term *term, int16_t n) {
    if (term_cursor_in_region(term)) {
        n = MAX(term_min_x(term), MIN(n, term_max_x(term) - term->c.x));

        struct line *line = term->screen[term->c.y];

        term_adjust_wide_left(term, term->c.x, term->c.y);
        term_adjust_wide_right(term, term->c.x, term->c.y);

        memmove(line->cell + term->c.x + n, line->cell + term->c.x,
                (term_max_x(term) - term->c.x - n) * sizeof(struct cell));
        for (int16_t i = term->c.x + n; i < term_max_x(term); i++)
            line->cell[i].attr &= ~attr_drawn;

        term_erase(term, term->c.x, term->c.y, term->c.x + n, term->c.y + 1, 0);
    }

    term_reset_pending(term);
}

static void term_delete_cells(struct term *term, int16_t n) {
    // Do not check top/bottom margins, DCH sould work outside them
    if (term->c.x >= term_min_x(term) && term->c.x < term_max_x(term)) {
        n = MAX(0, MIN(n, term_max_x(term) - term->c.x));

        struct line *line = term->screen[term->c.y];

        term_adjust_wide_left(term, term->c.x, term->c.y);
        term_adjust_wide_right(term, term->c.x + n - 1, term->c.y);

        memmove(line->cell + term->c.x, line->cell + term->c.x + n,
                (term_max_x(term) - term->c.x - n) * sizeof(struct cell));
        for (int16_t i = term->c.x; i < term_max_x(term) - n; i++)
            line->cell[i].attr &= ~attr_drawn;

        term_erase(term, term_max_x(term) - n, term->c.y, term_max_x(term), term->c.y + 1, 0);
    }

    term_reset_pending(term);
}

static void term_insert_lines(struct term *term, int16_t n) {
    if (term_cursor_in_region(term))
        term_scroll(term, term->c.y, -n, 0);
    term_move_to(term, term_min_x(term), term->c.y);
}

static void term_delete_lines(struct term *term, int16_t n) {
    if (term_cursor_in_region(term))
        term_scroll(term, term->c.y, n, 0);
    term_move_to(term, term_min_x(term), term->c.y);
}

static void term_insert_columns(struct term *term, int16_t n) {
    if (term_cursor_in_region(term))
        term_scroll_horizontal(term, term->c.x, -n);
}

static void term_delete_columns(struct term *term, int16_t n) {
    if (term_cursor_in_region(term))
        term_scroll_horizontal(term, term->c.x, n);
}

static void term_index_horizonal(struct term *term) {
    if (term->c.x == term_max_x(term) - 1 && term_cursor_in_region(term)) {
        term_scroll_horizontal(term, term_min_x(term), 1);
        term_reset_pending(term);
    } else if (term->c.x != term_max_x(term) - 1)
        term_move_to(term, term->c.x + 1, term->c.y);
}

static void term_rindex_horizonal(struct term *term) {
    if (term->c.x == term_min_x(term) && term_cursor_in_region(term)) {
        term_scroll_horizontal(term, term_min_x(term), -1);
        term_reset_pending(term);
    } else if (term->c.x != term_min_x(term))
        term_move_to(term, term->c.x - 1, term->c.y);
}

static void term_index(struct term *term) {
    if (term->c.y == term_max_y(term) - 1 && term_cursor_in_region(term)) {
        term_scroll(term, term_min_y(term), 1, 1);
        term_reset_pending(term);
    } else if (term->c.y != term_max_y(term) - 1)
        term_move_to(term, term->c.x, term->c.y + 1);
}

static void term_rindex(struct term *term) {
    if (term->c.y == term_min_y(term) && term_cursor_in_region(term)) {
        term_scroll(term,  term_min_y(term), -1, 1);
        term_reset_pending(term);
    } else if (term->c.y != term_min_y(term))
        term_move_to(term, term->c.x, term->c.y - 1);
}

static void term_cr(struct term *term) {
    term_move_to(term, term->c.x < term_min_x(term) ?
            term_min_ox(term) : term_min_x(term), term->c.y);
}

static void term_print_string(struct term *term, uint8_t *str, ssize_t size) {
    ssize_t wri = 0, res;
    do {
        res = write(term->printerfd, str, size);
        if (res < 0) {
            warn("Printer error");
            if (term->printerfd != STDOUT_FILENO)
                close(term->printerfd);
            term->printerfd = -1;
            break;
        }
        wri += res;
    } while(wri < size);
}

static void term_print_char(struct term *term, term_char_t ch) {
    uint8_t buf[5] = {ch};
    size_t sz = 1;
    if (term->mode.utf8 && ch >= 0xA0 && term->esc.state == esc_ground)
        sz = utf8_encode(ch, buf, buf + 5);
    term_print_string(term, buf, sz);
}

static void term_print_line(struct term *term, struct line *line) {
    if (term->printerfd < 0) return;

    // TODO Print with SGR
    for (int16_t i = 0; i < MIN(line->width, term->width); i++)
        term_print_char(term, line->cell[i].ch);
    term_print_char(term, '\n');
}

static void term_print_screen(struct term *term, bool ext) {
    if (term->printerfd < 0) return;

    int16_t top = ext ? 0 : term->top;
    int16_t bottom = ext ? term->height - 1 : term->bottom;

    while (top < bottom) term_print_line(term, term->screen[top++]);
    if (term->mode.print_form_feed)
        term_print_char(term, '\f');
}

inline static void term_do_wrap(struct term *term) {
    term->screen[term->c.y]->wrapped = 1;
    if (term->mode.print_auto && !term->mode.print_enabled)
        term_print_line(term, term->screen[term->c.y]);
    term_index(term);
    term_cr(term);
}

static void term_tabs(struct term *term, int16_t n) {
    //TODO CHT is not affected by DECCOM but CBT is?

    if (n >= 0) {
        if (term->mode.xterm_more_hack && term->c.pending)
            term_do_wrap(term);
        while (term->c.x < term_max_x(term) - 1 && n--) {
            do term->c.x++;
            while (term->c.x < term_max_x(term) - 1 && !term->tabs[term->c.x]);
        }
    } else {
        while (term->c.x > term_min_ox(term) && n++) {
            do term->c.x--;
            while (term->c.x > term_min_ox(term) && !term->tabs[term->c.x]);
        }
    }
}

void term_set_reverse(struct term *term, bool set) {
    if (set ^ term->mode.reverse_video) {
        SWAP(color_t, term->palette[SPECIAL_BG], term->palette[SPECIAL_FG]);
        SWAP(color_t, term->palette[SPECIAL_CURSOR_BG], term->palette[SPECIAL_CURSOR_FG]);
        SWAP(color_t, term->palette[SPECIAL_SELECTED_BG], term->palette[SPECIAL_SELECTED_FG]);
        mouse_damage_selection(term);
        window_set_colors(term->win, term->palette[SPECIAL_BG], term->palette[SPECIAL_CURSOR_FG]);
    }
    term->mode.reverse_video = set;
}

static void term_set_vt52(struct term *term, bool set) {
    if (set) {
        term->kstate.keyboad_vt52 = 1;
        term->vt_level = 0;
        term->vt52c = term->c;
        term->c = (struct cursor) {
            .cel = term->c.cel, .fg = term->c.fg, .bg = term->c.bg,
            .gl = 0, .gl_ss = 0, .gr = 2,
            .gn = {cs94_ascii, cs94_ascii, cs94_ascii, cs94_dec_graph}
        };
        term->vt52mode = term->mode;
        term->mode = (struct term_mode) {
            .focused = term->mode.focused,
            .reverse_video = term->mode.reverse_video
        };
        term_esc_start_seq(term);
    } else {
        term->kstate.keyboad_vt52 = 0;
        term->vt_level = 1;
        term->mode = term->vt52mode;
        term->vt52c.x = term->c.x;
        term->vt52c.y = term->c.y;
        term->vt52c.pending = term->c.pending;
        term->c = term->vt52c;
    }
}

static void term_load_config(struct term *term) {

    term->mstate = (struct mouse_state) {0};

    term->mode = (struct term_mode) {
        .focused = term->mode.focused,
        .utf8 = iconf(ICONF_UTF8),
        .title_query_utf8 = iconf(ICONF_UTF8),
        .title_set_utf8 = iconf(ICONF_UTF8),
        .disable_altscreen = !iconf(ICONF_ALLOW_ALTSCREEN),
        .wrap = iconf(ICONF_INIT_WRAP),
        .no_scroll_on_input = !iconf(ICONF_SCROLL_ON_INPUT),
        .scroll_on_output = iconf(ICONF_SCROLL_ON_OUTPUT),
        .enable_nrcs = iconf(ICONF_ALLOW_NRCS),
        .keep_clipboard = iconf(ICONF_KEEP_CLIPBOARD),
        .keep_selection = iconf(ICONF_KEEP_SELECTION),
        .select_to_clipboard = iconf(ICONF_SELECT_TO_CLIPBOARD),
        .bell_raise = iconf(ICONF_RAISE_ON_BELL),
        .bell_urgent = iconf(ICONF_URGENT_ON_BELL),
    };

    for (size_t i = 0; i < PALETTE_SIZE; i++)
        term->palette[i] = cconf(CCONF_COLOR_0 + i);
    term_set_reverse(term, iconf(ICONF_REVERSE_VIDEO));

    term->kstate = (struct keyboard_state) {
        .appcursor = iconf(ICONF_APPCURSOR),
        .appkey = iconf(ICONF_APPKEY),
        .backspace_is_del = iconf(ICONF_BACKSPACE_IS_DELETE),
        .delete_is_del = iconf(ICONF_DELETE_IS_DELETE),
        .fkey_inc_step = iconf(ICONF_FKEY_INCREMENT),
        .has_meta = iconf(ICONF_HAS_META),
        .keyboard_mapping = iconf(ICONF_MAPPING),
        .keylock = iconf(ICONF_LOCK),
        .meta_escape = iconf(ICONF_META_IS_ESC),
        .modkey_cursor = iconf(ICONF_MODIFY_CURSOR),
        .modkey_fn = iconf(ICONF_MODIFY_FUNCTION),
        .modkey_keypad = iconf(ICONF_MODIFY_KEYPAD),
        .modkey_other = iconf(ICONF_MODIFY_OTHER),
        .modkey_other_fmt = iconf(ICONF_MODIFY_OTHER_FMT),
        .modkey_legacy_allow_edit_keypad = iconf(ICONF_MALLOW_EDIT),
        .modkey_legacy_allow_function = iconf(ICONF_MALLOW_FUNCTION),
        .modkey_legacy_allow_keypad = iconf(ICONF_MALLOW_KEYPAD),
        .modkey_legacy_allow_misc = iconf(ICONF_MALLOW_MISC),
        .allow_numlock = iconf(ICONF_NUMLOCK),
    };

    term->c = term->back_cs = term->cs = (struct cursor) {
        .cel = MKCELL(SPECIAL_FG, SPECIAL_BG, 0, 0),
        .fg = cconf(CCONF_FG),
        .bg = cconf(CCONF_BG),
        .gl = 0, .gl_ss = 0, .gr = 2,
        .upcs = cs94_dec_sup,
        .gn = {cs94_ascii, cs94_ascii, cs94_ascii, cs94_ascii}
    };

    term->vt_level = term->vt_version / 100;
    if (!term->vt_level) term_set_vt52(term, 1);

    switch(iconf(ICONF_BELL_VOLUME)) {
    case 0: term->bvol = 0; break;
    case 1: term->bvol = iconf(ICONF_BELL_LOW_VOLUME); break;
    case 2: term->bvol = iconf(ICONF_BELL_HIGH_VOLUME);
    }

    switch(iconf(ICONF_MARGIN_BELL_VOLUME)) {
    case 0: term->mbvol = 0; break;
    case 1: term->mbvol = iconf(ICONF_MARGIN_BELL_LOW_VOLUME); break;
    case 2: term->mbvol = iconf(ICONF_MARGIN_BELL_HIGH_VOLUME);
    }

}

static void term_reset_tabs(struct term *term) {
    memset(term->tabs, 0, term->width * sizeof(term->tabs[0]));
    int16_t tabw = iconf(ICONF_TAB_WIDTH);
    for (int16_t i = tabw; i < term->width; i += tabw)
        term->tabs[i] = 1;
}

static void term_request_resize(struct term *term, int16_t w, int16_t h, bool in_cells) {
    int16_t cur_w, cur_h, scr_w, scr_h;
    window_get_dim(term->win, &cur_w, &cur_h);
    window_get_dim_ext(term->win, dim_screen_size, &scr_w, &scr_h);

    if (in_cells) {
        int16_t ce_w, ce_h, bo_w, bo_h;
        window_get_dim_ext(term->win, dim_cell_size, &ce_w, &ce_h);
        window_get_dim_ext(term->win, dim_border, &bo_w, &bo_h);
        if (w > 0) w = w * ce_w + bo_w * 2;
        if (h > 0) h = h * ce_h + bo_h * 2;
    }

    w = !w ? scr_w : w < 0 ? cur_w : w;
    h = !h ? scr_h : h < 0 ? cur_h : h;

    window_resize(term->win, w, h);
}

static void term_set_132(struct term *term, bool set) {
    term_reset_margins(term);
    term_move_to(term, term_min_ox(term), term_min_oy(term));
    if (!(term->mode.preserve_display_132))
        term_erase(term, 0, 0, term->width, term->height, 0);
    if (iconf(ICONF_ALLOW_WINDOW_OPS))
        term_request_resize(term, set ? 132 : 80, 24, 1);
    term->mode.columns_132 = set;
}

static void term_do_reset(struct term *term, bool hard) {
    if (term->mode.columns_132) term_set_132(term, 0);
    if (term->mode.altscreen) term_swap_screen(term, 1);

    int16_t cx = term->c.x, cy = term->c.y;
    bool cpending = term->c.pending;

    term_load_config(term);
    term_reset_margins(term);
    term_reset_tabs(term);
    keyboard_reset_udk(term);

    window_set_mouse(term->win, 0);
    window_set_cursor(term->win, iconf(ICONF_CURSOR_SHAPE));
    window_set_colors(term->win, term->palette[SPECIAL_BG], term->palette[SPECIAL_CURSOR_FG]);

    if (hard) {
        term_cursor_mode(term, 1);
        term->back_cs = term->cs;
        term_erase(term, 0, 0, term->width, term->height, 0);

        term_free_scrollback(term);

        term->vt_level = term->vt_version / 100;

        window_set_title(term->win, target_icon_label | target_title, NULL, term->mode.title_set_utf8);
    } else {
        term->c.x = cx;
        term->c.y = cy;
        term->c.pending = cpending;
    }

    term->esc.state = esc_ground;
}

void term_reset(struct term *term) {
    term_do_reset(term, 1);
}

struct term *create_term(struct window *win, int16_t width, int16_t height) {
    struct term *term = calloc(1, sizeof(struct term));

    term->palette = malloc(PALETTE_SIZE * sizeof(color_t));
    term->win = win;

    term->printerfd = -1;
    term->sb_max_caps = iconf(ICONF_HISTORY_LINES);
    term->vt_version = iconf(ICONF_VT_VERION);
    term->fd_start = term->fd_end = term->fd_buf;
    term->sb_top = -1;

    term_load_config(term);

    for (size_t i = 0; i < 2; i++) {
        term_cursor_mode(term, 1);
        term_erase(term, 0, 0, term->width, term->height, 0);
        term_swap_screen(term, 0);
    }

    if (tty_open(term, sconf(SCONF_SHELL), sconf_argv()) < 0) {
        warn("Can't create tty");
        free_term(term);
        return NULL;
    }

    term_resize(term, width, height);

    const char *printer_path = sconf(SCONF_PRINTER);
    if (printer_path) {
        if (printer_path[0] == '-' && !printer_path[1])
            term->printerfd = STDOUT_FILENO;
        else
            term->printerfd = open(printer_path, O_WRONLY | O_CREAT, 0660);
    }

    return term;
}

static void term_dispatch_da(struct term *term, uparam_t mode) {
    switch (mode) {
    case P('='): /* Tertinary DA */
        CHK_VT(4);
        /* DECREPTUI */
        term_answerback(term, DCS"!|00000000"ST);
        break;
    case P('>'): /* Secondary DA */ {
        uparam_t ver = 0;
        switch (term->vt_version) {
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
        }
        term_answerback(term, CSI">%d;%d;0c", ver, NSST_VERSION);
        break;
    }
    default: /* Primary DA */
        if (term->vt_version < 200) {
            switch (term->vt_version) {
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
            term_answerback(term, CSI"?%u;1;2;6%s;9%sc",
                    60 + term->vt_version/100,
                    term->kstate.keyboard_mapping == keymap_vt220 ? ";8" : "",
                    term->vt_level >= 4 ? ";21;22;28" : ";22");
        }
    }
}

static void term_dispatch_dsr(struct term *term) {
    if (term->esc.selector & P_MASK) {
        switch (term->esc.param[0]) {
        case 6: /* DECXCPR -- CSI ? Py ; Px ; R ; 1  */
            term_answerback(term, CSI"%d;%d%sR",
                    term->c.y - term_min_oy(term) + 1,
                    term->c.x - term_min_ox(term) + 1,
                    term->vt_level >= 4 ? ";1" : "");
            break;
        case 15: /* Printer status -- Has printer*/
            CHK_VT(2);
            term_answerback(term, term->printerfd >= 0 ? CSI"?10n" : CSI"?13n");
            break;
        case 25: /* User defined keys lock */
            CHK_VT(2);
            term_answerback(term, CSI"?%dn", 20 + term->kstate.udk_locked);
            break;
        case 26: /* Keyboard language -- North American */
            CHK_VT(2);
            term_answerback(term, CSI"?27;1%sn",
                    term->vt_level >= 4 ? ";0;0" : // ready, LK201
                    term->vt_level >= 3 ? ";0" : ""); // ready
            break;
        case 53: /* Report locator status */
        case 55:
            CHK_VT(4);
            term_answerback(term, CSI"?53n"); // Locator available
            break;
        case 56: /* Report locator type */
            CHK_VT(4);
            term_answerback(term, CSI"?57;1n"); // Mouse
            break;
        case 62: /* DECMSR, Macro space -- No data, no space for macros */
            CHK_VT(4);
            term_answerback(term, CSI"0*{");
            break;
        case 63: /* DECCKSR, Memory checksum -- 0000 (hex) */
            CHK_VT(4);
            term_answerback(term, DCS"%d!~0000"ST, PARAM(1, 0));
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
        switch (term->esc.param[0]) {
        case 5: /* Health report -- OK */
            term_answerback(term, CSI"0n");
            break;
        case 6: /* CPR -- CSI Py ; Px R */
            term_answerback(term, CSI"%d;%dR",
                    term->c.y - term_min_oy(term) + 1,
                    term->c.x - term_min_ox(term) + 1);
            break;
        }
    }
}

inline static void term_parse_cursor_report(struct term *term) {
    char *dstr = (char *)term_esc_str(term);

    // Cursor Y
    ssize_t y = strtoul(dstr, &dstr, 10);
    if (!dstr || *dstr++ != ';') goto err;

    // Cursor X
    ssize_t x = strtoul(dstr, &dstr, 10);
    if (!dstr || *dstr++ != ';') goto err;

    // Page, always '1'
    if (*dstr++ != '1' || *dstr++ != ';') goto err;

    // SGR
    char sgr0 = *dstr++, sgr1 = 0x40;
    if ((sgr0 & 0xD0) != 0x40) goto err;

    // Optional extended byte
    if (sgr0 & 0x20) sgr1 = *dstr++;
    if ((sgr1 & 0xF0) != 0x40 || *dstr++ != ';') goto err;

    // Protection
    char prot = *dstr++;
    if ((prot & 0xFE) != 0x40 || *dstr++ != ';') goto err;

    // Flags
    char flags = *dstr++;
    if ((flags & 0xF0) != 0x40 || *dstr++ != ';') goto err;

    // GL
    unsigned long gl = strtoul(dstr, &dstr, 10);
    if (!dstr || *dstr++ != ';'|| gl > 3) goto err;

    // GR
    unsigned long gr = strtoul(dstr, &dstr, 10);
    if (!dstr || *dstr++ != ';' || gr > 3) goto err;

    // G0 - G3 sizes
    char c96 = *dstr++;
    if ((flags & 0xF0) != 0x40 || *dstr++ != ';') goto err;

    // G0 - G3
    enum charset gn[4];
    for (size_t i = 0; i < 4; i++) {
        uparam_t sel = 0;
        char c;
        if ((c = *dstr++) < 0x30) {
            sel |= I1(c);
            if ((c = *dstr++) < 0x30) goto err;
        }
        sel |= E(c);
        if ((gn[i] = nrcs_parse(sel, (c96 >> i) & 1,
                term->vt_level, term->mode.enable_nrcs)) == -1U) goto err;
    }

    // Everything is OK, load

    term->c = (struct cursor) {
        .x = MIN(x, term->width - 1),
        .y = MIN(y, term->height - 1),
        .cel = (struct cell) {
            .fg = term->c.cel.fg,
            .bg = term->c.cel.bg,
        },
        .fg = term->c.fg,
        .bg = term->c.bg,
        .origin = flags & 1,
        .pending = !!(flags & 8),
        .gn = { gn[0], gn[1], gn[2], gn[3] },
        .gl = gl,
        .gr = gr,
        .upcs = term->c.upcs,
        .gl_ss = flags & 4 ? 3 : flags & 2 ? 2 : term->c.gl
    };

    if (sgr0 & 1) term->c.cel.attr |= attr_bold;
    if (sgr0 & 2) term->c.cel.attr |= attr_underlined;
    if (sgr0 & 4) term->c.cel.attr |= attr_blink;
    if (sgr0 & 8) term->c.cel.attr |= attr_inverse;
    if (sgr1 & 1) term->c.cel.attr |= attr_italic;
    if (sgr1 & 2) term->c.cel.attr |= attr_faint;
    if (sgr1 & 4) term->c.cel.attr |= attr_strikethrough;
    if (sgr1 & 8) term->c.cel.attr |= attr_invisible;
    if (prot & 1) term->c.cel.attr |= attr_protected;

    return;
err:
    term_esc_dump(term, 0);
}

inline static void term_parse_tabs_report(struct term *term) {
    memset(term->tabs, 0, term->width*sizeof(*term->tabs));
    uint8_t *dstr = term_esc_str(term);
    uint8_t *dend = dstr + term->esc.str_len;
    for (ssize_t tab = 0; dstr <= dend; dstr++) {
        if (*dstr == '/' || dstr == dend) {
            if (tab - 1 < term->width) term->tabs[tab - 1] = 1;
            tab = 0;
        } else if (isdigit(*dstr)) {
            tab = 10 * tab + *dstr - '0';
        } else  term_esc_dump(term, 0);
    }
}

static void term_dispatch_dcs(struct term *term) {
    // Fixup parameter count
    term->esc.i += term->esc.param[term->esc.i] >= 0;

    if (term->esc.state != esc_dcs_string) {
        term->esc.selector = term->esc.old_selector;
        term->esc.state = term->esc.old_state;
    }

    term_esc_dump(term, 1);

    // Only SGR is allowed to have subparams
    if (term->esc.subpar_mask) return;

    uint8_t *dstr = term_esc_str(term);
    uint8_t *dend = dstr + term->esc.str_len;

    switch (term->esc.selector) {
    case C('s') | P('='): /* iTerm2 syncronous updates */
        switch (PARAM(0,0)) {
        case 1: /* Begin syncronous update */
            window_set_sync(term->win, 1);
            break;
        case 2: /* End syncronous update */
            window_set_sync(term->win, 0);
            break;
        default:
            term_esc_dump(term, 0);
        }
        break;
    case C('q') | I0('$'): /* DECRQSS -> DECRPSS */ {
        if (term->esc.str_len && term->esc.str_len < 3) {
            uint16_t id = *dstr | dstr[1] << 8;
            switch(id) {
            case 'm': /* -> SGR */ {
                char sgr[SGR_BUFSIZ];
                term_encode_sgr(sgr, sgr + sizeof sgr, term->c.cel, term->c.fg, term->c.bg);
                term_answerback(term, DCS"1$r%sm"ST, sgr);
                break;
            }
            case 'r': /* -> DECSTBM */
                term_answerback(term, DCS"1$r%d;%dr"ST, term->top + 1, term->bottom + 1);
                break;
            case 's': /* -> DECSLRM */
                term_answerback(term, term->vt_level >= 4 ? DCS"1$r%d;%ds"ST :
                        DCS"0$r"ST, term->left + 1, term->right + 1);
                break;
            case 't': /* -> DECSLPP */
                // Can't report less than 24 lines
                term_answerback(term, DCS"1$r%dt"ST, MAX(term->height, 24));
                break;
            case '|' << 8 | '$': /* -> DECSCPP */
                // It should be either 80 or 132 despite actual column count
                // New apps use ioctl(TIOGWINSZ, ...) instead
                term_answerback(term, DCS"1$r%d$|"ST, term->mode.columns_132 ? 132 : 80);
                break;
            case 'q' << 8 | '"': /* -> DECSCA */
                term_answerback(term, DCS"1$r%d\"q"ST, term->mode.protected ? 2 :
                        term->c.cel.attr & attr_protected ? 1 : 2);
                break;
            case 'q' << 8 | ' ': /* -> DECSCUSR */
                term_answerback(term, DCS"1$r%d q"ST, window_get_cursor(term->win));
                break;
            case '|' << 8 | '*': /* -> DECSLNS */
                term_answerback(term, DCS"1$r%d*|"ST, term->height);
                break;
            case 'x' << 8 | '*': /* -> DECSACE */
                term_answerback(term, term->vt_level < 4 ? DCS"0$r"ST :
                        DCS"1$r%d*x"ST, term->mode.attr_ext_rectangle + 1);
                break;
            case 'p' << 8 | '"': /* -> DECSCL */
                term_answerback(term, DCS"1$r%d%s\"p"ST, 60 + MAX(term->vt_level, 1),
                        term->vt_level >= 2 ? (term->mode.eight_bit ? ";2" : ";1") : "");
                break;
            case 't' << 8 | ' ': /* -> DECSWBV */ {
                uparam_t val = 8;
                if (term->bvol == iconf(ICONF_BELL_LOW_VOLUME)) val = 4;
                else if (!term->bvol) val = 0;
                term_answerback(term, DCS"1$r%d t"ST, val);
                break;
            }
            case 'u' << 8 | ' ': /* -> DECSMBV */ {
                uparam_t val = 8;
                if (term->mbvol == iconf(ICONF_MARGIN_BELL_LOW_VOLUME)) val = 4;
                else if (!term->mbvol) val = 0;
                term_answerback(term, DCS"1$r%d u"ST, val);
                break;
            }
            default:
                // Invalid request
                term_answerback(term, DCS"0$r"ST);
                term_esc_dump(term, 0);
            }
        } else term_esc_dump(term, 0);
        break;
    }
    case C('t') | I0('$'): /* DECRSPS */
        switch(PARAM(0, 0)) {
        case 1: /* <- DECCIR */
            term_parse_cursor_report(term);
            break;
        case 2: /* <- DECTABSR */
            term_parse_tabs_report(term);
            break;
        default:
            term_esc_dump(term, 0);
        }
        break;
    case C('|'): /* DECUDK */
        keyboard_set_udk(term, dstr, dend, !PARAM(0, 0), !PARAM(1, 0));
        break;
    case C('u') | I0('!'): /* DECAUPSS */ {
        uint32_t sel = 0;
        if (term->esc.str_len == 1 && *dstr > 0x2F && *dstr < 0x7F) {
            sel = E(*dstr);
        } else if (term->esc.str_len == 2 && *dstr >= 0x20 && *dstr < 0x30 && dstr[1] > 0x2F && dstr[1] < 0x7F) {
            sel = E(dstr[1]) | I0(dstr[0]);
        } else {
            term_esc_dump(term, 0);
            break;
        }
        enum charset cs = nrcs_parse(sel, 0, term->vt_level, term->mode.enable_nrcs);
        if (cs == -1U) cs = nrcs_parse(E(*dstr), 1, term->vt_level, term->mode.enable_nrcs);
        if (cs != -1U) term->c.upcs = cs;
        break;
    }
    case C('q') | I0('+'): /* XTGETTCAP */ {
        // TODO Termcap: Support proper tcap db
        // for now, just implement Co/colors
        bool valid = 0;
        if (!strcmp((char *)dstr, "436F") || // "Co"
                !strcmp((char *)dstr, "636F6C6F7266")) { // "colors"
            uint8_t tmp[16];
            int len = snprintf((char *)tmp, sizeof tmp, "%d", PALETTE_SIZE - SPECIAL_PALETTE_SIZE);
            *dend = '=';
            hex_encode(dend + 1, tmp, tmp + len);
            valid = 1;
        }
        term_answerback(term, DCS"%d+r%s"ST, valid, dstr);
        break;
    }
    //case C('p') | I0('+'): /* XTSETTCAP */ // TODO Termcap
    //    break;
    default:
        term_esc_dump(term, 0);
    }

    term_esc_finish_string(term);
    term->esc.old_state = 0;
    term->esc.state = esc_ground;
}

static enum clip_target decode_target(uint8_t targ, bool mode) {
    switch (targ) {
    case 'p': return clip_primary;
    case 'q': return clip_secondary;
    case 'c': return clip_clipboard;
    case 's': return mode ? clip_clipboard : clip_primary;
    default:
        return -1;
    }
}

static uint32_t selector_to_cid(uint32_t sel, bool rev) {
    switch(sel) {
    case 10: return rev ? SPECIAL_BG : SPECIAL_FG; break;
    case 11: return rev ? SPECIAL_FG : SPECIAL_BG; break;
    case 12: return rev ? SPECIAL_CURSOR_BG : SPECIAL_CURSOR_FG; break;
    case 17: return rev ? SPECIAL_SELECTED_FG : SPECIAL_SELECTED_BG; break;
    case 19: return rev ? SPECIAL_SELECTED_BG : SPECIAL_SELECTED_FG; break;
    }
    warn("Unreachable");
    return 0;
}

static void term_colors_changed(struct term *term, uint32_t sel, color_t col) {
    switch(sel) {
    case 10:
        if(term->mode.reverse_video)
            window_set_colors(term->win, col, 0);
        break;
    case 11:
        if(!(term->mode.reverse_video))
            window_set_colors(term->win, term->palette[SPECIAL_CURSOR_BG] = col, 0);
        else
            window_set_colors(term->win, 0, term->palette[SPECIAL_CURSOR_FG] = col);
        break;
    case 12:
        if(!(term->mode.reverse_video))
            window_set_colors(term->win, 0, col);
        break;
    case 17: case 19:
        mouse_damage_selection(term);
    }
}

static void term_do_set_color(struct term *term, uint32_t sel, uint8_t *dstr, uint8_t *dend) {
    color_t col;
    color_id_t cid = selector_to_cid(sel, term->mode.reverse_video);

    if (!strcmp((char *)dstr, "?")) {
        col = term->palette[cid];

        term_answerback(term, OSC"%d;rgb:%04x/%04x/%04x"ST, sel,
               ((col >> 16) & 0xFF) * 0x101,
               ((col >>  8) & 0xFF) * 0x101,
               ((col >>  0) & 0xFF) * 0x101);
    } else if ((col = parse_color(dstr, dend))) {
        term->palette[cid] = col = (col & 0x00FFFFFF) | (0xFF000000 & term->palette[cid]); // Keep alpha

        term_colors_changed(term, sel, col);

    } else term_esc_dump(term, 0);
}

static void term_do_reset_color(struct term *term) {
    color_id_t cid = selector_to_cid(term->esc.selector - 100, term->mode.reverse_video);

    term->palette[cid] = cconf(CCONF_COLOR_0 + selector_to_cid(term->esc.selector - 100, 0));

    term_colors_changed(term, term->esc.selector - 100, term->esc.selector == 111 ?
            cconf(CCONF_CURSOR_BG) : term->palette[cid]);
}

inline static bool is_osc_state(uint32_t state) {
    return state == esc_osc_string || state == esc_osc_1 || state == esc_osc_2;
}

static void term_dispatch_osc(struct term *term) {
    if (!is_osc_state(term->esc.state)) {
        term->esc.state = term->esc.old_state;
        term->esc.selector = term->esc.old_selector;
    }
    term_esc_dump(term, 1);

    uint8_t *dstr = term_esc_str(term);
    uint8_t *dend = dstr + term->esc.str_len;

    switch (term->esc.selector) {
        color_t col;
    case 0: /* Change window icon name and title */
    case 1: /* Change window icon name */
    case 2: /* Change window title */ {
        if (term->mode.title_set_hex) {
            if (*hex_decode(dstr, dstr, dend)) {
                term_esc_dump(term, 0);
                break;
            }
            dend = memchr(dstr, 0, ESC_MAX_STR);
        }
        uint8_t *res = NULL;
        if (!(term->mode.title_set_utf8) && term->mode.utf8) {
            uint8_t *dst = dstr;
            const uint8_t *ptr = dst;
            term_char_t val = 0;
            while (*ptr && utf8_decode(&val, &ptr, dend))
                *dst++ = val;
            *dst = '\0';
        } else if (term->mode.title_set_utf8 && !(term->mode.utf8)) {
            res = malloc(term->esc.str_len * 2 + 1);
            uint8_t *ptr = res, *src = dstr;
            if (res) {
                while (*src) ptr += utf8_encode(*src++, ptr, res + term->esc.str_len * 2);
                *ptr = '\0';
                dstr = res;
            }
        }
        window_set_title(term->win, 3 - term->esc.selector, (char *)dstr, term->mode.utf8);
        free(res);
        break;
    }
    case 4: /* Set color */
    case 5: /* Set special color */ {
        uint8_t *pstr = dstr, *pnext = NULL, *s_end;
        while (pstr < dend && (pnext = memchr(pstr, ';', dend - pstr))) {
            *pnext = '\0';
            errno = 0;
            unsigned long idx = strtoul((char *)pstr, (char **)&s_end, 10);
            if (term->esc.selector == 5) idx += SPECIAL_BOLD;

            *pnext = ';';
            uint8_t *parg  = pnext + 1;
            if ((pnext = memchr(parg, ';', dend - parg))) *pnext = '\0';
            else pnext = dend;

            if (!errno && s_end == parg - 1 && idx < PALETTE_SIZE - SPECIAL_PALETTE_SIZE + 5) {
                if (parg[0] == '?' && parg[1] == '\0')
                    term_answerback(term, OSC"%d;%d;rgb:%04x/%04x/%04x"ST,
                            term->esc.selector, idx - (term->esc.selector == 5) * SPECIAL_BOLD,
                            ((term->palette[idx] >> 16) & 0xFF) * 0x101,
                            ((term->palette[idx] >>  8) & 0xFF) * 0x101,
                            ((term->palette[idx] >>  0) & 0xFF) * 0x101);
                else if ((col = parse_color(parg, pnext)))
                    term->palette[idx] = col;
                else {
                    if (pnext != dend) *pnext = ';';
                    term_esc_dump(term, 0);
                }
            }
            if (pnext != dend) *pnext = ';';
            pstr = pnext + 1;
        }
        if (pstr < dend && !pnext) term_esc_dump(term, 0);
        break;
    }
    case 104: /* Reset color */
    case 105: /* Reset special color */ {
        if (term->esc.str_len) {
            uint8_t *pnext, *s_end;
            do {
                pnext = memchr(dstr, ';', dend - dstr);
                if (!pnext) pnext = dend;
                else *pnext = '\0';
                errno = 0;
                unsigned long idx = strtoul((char *)dstr, (char **)&s_end, 10);
                if (term->esc.selector == 105) idx += SPECIAL_BOLD;
                if (!errno && !*s_end && s_end != dstr && idx < PALETTE_SIZE - SPECIAL_PALETTE_SIZE + 5)
                    term->palette[idx] = cconf(CCONF_COLOR_0 + idx);
                else term_esc_dump(term, 0);
                if (pnext != dend) *pnext = ';';
                dstr = pnext + 1;
            } while (pnext != dend);
        } else {
            for (size_t i = 0; i < PALETTE_SIZE - SPECIAL_PALETTE_SIZE + 5; i++)
                term->palette[i] = cconf(CCONF_COLOR_0 + i);
        }
        break;
    }
    case 6:
    case 106: /* Enable/disable special color */ {
        // IMPORTANT: this option affects all instances
        ssize_t n;
        uparam_t idx, val;
        if (sscanf((char *)dstr, "%"SCNparam";%"SCNparam"%zn", &idx, &val, &n) == 2 && n == dend - dstr && idx < 5)
            iconf_set(ICONF_SPEICAL_BOLD + idx, !!val);
        else term_esc_dump(term, 0);
        break;
    }
    case 10: /* Set VT100 foreground color */ {
        // OSC 10 can also have second argument that works as OSC 11
        uint8_t *str2;
        if ((str2 = memchr(dstr, ';', dend - dstr))) {
            *str2 = '\0';
            term_do_set_color(term, 10, dstr, str2);
            *str2 = ';';
            dstr = str2 + 1;
            term->esc.selector++;
        }
    }
    case 11: /* Set VT100 background color */
    case 12: /* Set Cursor color */
    case 17: /* Set Highlight background color */
    case 19: /* Set Highlight foreground color */
        term_do_set_color(term, term->esc.selector, dstr, dend);
        break;
    case 110: /*Reset  VT100 foreground color */
    case 111: /*Reset  VT100 background color */
    case 112: /*Reset  Cursor color */
    case 117: /*Reset  Highlight background color */
    case 119: /*Reset  Highlight foreground color */
        term_do_reset_color(term);
        break;
    case 52: /* Manipulate selecion data */ {
        if (!iconf(ICONF_ALLOW_WINDOW_OPS)) break;

        enum clip_target ts[clip_MAX] = {0};
        bool toclip = term->mode.select_to_clipboard;
        uint8_t *parg = dstr, letter = 0;
        for (; parg < dend && *parg !=  ';'; parg++) {
            if (strchr("pqsc", *parg)) {
                ts[decode_target(*parg, toclip)] = 1;
                if (!letter) letter = *parg;
            }
        }
        if (parg++ < dend) {
            if (!letter) ts[decode_target((letter = 's'), toclip)] = 1;
            if (!strcmp("?", (char*)parg)) {
                term->paste_from = letter;
                window_paste_clip(term->win, decode_target(letter, toclip));
            } else {
                if (base64_decode(parg, parg, dend) != dend) parg = NULL;
                for (size_t i = 0; i < clip_MAX; i++) {
                    if (ts[i]) {
                        if (i == term->mstate.targ) term->mstate.targ = -1;
                        window_set_clip(term->win, parg ? (uint8_t *)strdup((char *)parg) : parg, CLIP_TIME_NOW, i);
                    }
                }
            }
        } else term_esc_dump(term, 0);
        break;
    }
    case 13001: /* Select background alpha */ {
        errno = 0;
        unsigned long res = strtoul((char *)dstr, (char **)&dstr, 10);
        if (res > 255 || errno || *dstr) {
            term_esc_dump(term, 0);
            break;
        }
        term->palette[SPECIAL_BG] = (term->palette[SPECIAL_BG] & 0x00FFFFFF) | res << 24;
        window_set_colors(term->win, term->palette[SPECIAL_BG], 0);
        break;
    }
    //case 50: /* Set Font */ // TODO OSC 50
    //    break;
    //case 13: /* Set Mouse foreground color */ // TODO Pointer
    //    break;
    //case 14: /* Set Mouse background color */ // TODO Pointer
    //    break;
    //case 113: /*Reset  Mouse foreground color */ // TODO Pointer
    //    break;
    //case 114: /*Reset  Mouse background color */ // TODO Pointer
    //    break;
    default:
        term_esc_dump(term, 0);
    }

    term_esc_finish_string(term);
    term->esc.old_state = 0;
    term->esc.state = esc_ground;
}

static bool term_srm(struct term *term, bool private, uparam_t mode, bool set) {
    if (private) {
        switch (mode) {
        case 0: /* Default - nothing */
            break;
        case 1: /* DECCKM */
            term->kstate.appcursor = set;
            break;
        case 2: /* DECANM */
            if (!set) term_set_vt52(term, 1);
            break;
        case 3: /* DECCOLM */
            if (!(term->mode.disable_columns_132))
                term_set_132(term, set);
            break;
        case 4: /* DECSCLM */
            term->mode.smooth_scroll = set;
            break;
        case 5: /* DECSCNM */
            term_set_reverse(term, set);
            break;
        case 6: /* DECCOM */
            term->c.origin = set;
            term_move_to(term, term_min_ox(term), term_min_oy(term));
            break;
        case 7: /* DECAWM */
            term->mode.wrap = set;
            term_reset_pending(term);
            break;
        case 8: /* DECARM */
            // IGNORE
            break;
        case 9: /* X10 Mouse tracking */
            window_set_mouse(term->win, 0);
            term->mstate.mouse_mode = set ? mouse_mode_x10 : mouse_mode_none;
            break;
        case 10: /* Show toolbar */
            // IGNORE - There is no toolbar
            break;
        case 12: /* Start blinking cursor */
            window_set_cursor(term->win, ((window_get_cursor(term->win) + 1) & ~1) - set);
            break;
        case 13: /* Start blinking cursor (menu item) */
        case 14: /* Enable XOR of controll sequence and menu for blinking */
            // IGNORE
            break;
        case 18: /* DECPFF */
            term->mode.print_form_feed = set;
            break;
        case 19: /* DECREX */
            term->mode.print_extend = set;
            break;
        case 25: /* DECTCEM */
            if (set ^ term->mode.hide_cursor)
                term->screen[term->c.y]->cell[term->c.x].attr &= ~attr_drawn;
            term->mode.hide_cursor = !set;
            break;
        case 30: /* Show scrollbar */
            // IGNORE - There is no scrollbar
            break;
        case 35: /* URXVT Allow change font */
            term->mode.allow_change_font = set;
            break;
        case 40: /* 132COLS */
            term->mode.disable_columns_132 = !set;
            break;
        case 41: /* XTerm more(1) hack */
            term->mode.xterm_more_hack = set;
            break;
        case 42: /* DECNRCM */
            CHK_VT(3);
            term->mode.enable_nrcs = set;
            break;
        case 44: /* Margin bell */
            term->mode.margin_bell = set;
            break;
        case 45: /* Reverse wrap */
            term->mode.reverse_wrap = set;
            break;
        case 47: /* Enable altscreen */
            if (term->mode.disable_altscreen) break;
            if (set ^ term->mode.altscreen)
                term_swap_screen(term, 1);
            break;
        case 66: /* DECNKM */
            term->kstate.appkey = set;
            break;
        case 67: /* DECBKM */
            term->kstate.backspace_is_del = !set;
            break;
        case 69: /* DECLRMM */
            CHK_VT(4);
            term->mode.lr_margins = set;
            break;
        //case 80: /* DECSDM */ //TODO SIXEL
        //    break;
        case 95: /* DECNCSM */
            CHK_VT(5);
            term->mode.preserve_display_132 = set;
            break;
        case 1000: /* X11 Mouse tracking */
            window_set_mouse(term->win, 0);
            term->mstate.mouse_mode = set ? mouse_mode_button : mouse_mode_none;
            break;
        case 1001: /* Highlight mouse tracking */
            // IGNORE
            break;
        case 1002: /* Cell motion mouse tracking on keydown */
            window_set_mouse(term->win, 0);
            term->mstate.mouse_mode = set ? mouse_mode_drag : mouse_mode_none;
            break;
        case 1003: /* All motion mouse tracking */
            window_set_mouse(term->win, set);
            term->mstate.mouse_mode = set ? mouse_mode_motion : mouse_mode_none;
            break;
        case 1004: /* Focus in/out events */
            term->mode.track_focus = set;
            if (set) term_answerback(term, term->mode.focused ? CSI"I" : CSI"O");
            break;
        case 1005: /* UTF-8 mouse format */
            term->mstate.mouse_format = set ? mouse_format_utf8 : mouse_format_default;
            break;
        case 1006: /* SGR mouse format */
            term->mstate.mouse_format = set ? mouse_format_sgr : mouse_format_default;
            break;
        case 1007: /* Alternate scroll */
            term->mode.altscreen_scroll = set;
            break;
        case 1010: /* Scroll to bottom on output */
            term->mode.scroll_on_output = set;
            break;
        case 1011: /* Scroll to bottom on keypress */
            term->mode.no_scroll_on_input = !set;
            break;
        case 1015: /* Urxvt mouse format */
            term->mstate.mouse_format = set ? mouse_format_uxvt : mouse_format_default;
            break;
        case 1016: /* SGR mouse format with pixel coordinates */
            term->mstate.mouse_format = set ? mouse_format_pixel : mouse_format_default;
            break;
        case 1034: /* Interpret meta */
            term->kstate.has_meta = set;
            break;
        case 1035: /* Numlock */
            term->kstate.allow_numlock = set;
            break;
        case 1036: /* Meta sends escape */
            term->kstate.meta_escape = set;
            break;
        case 1037: /* Backspace is delete */
            term->kstate.backspace_is_del = set;
            break;
        case 1040: /* Don't clear X11 PRIMARY selection */
            term->mode.keep_selection = set;
            break;
        case 1041: /* Use CLIPBOARD instead of PRIMARY */
            term->mode.select_to_clipboard = set;
            break;
        case 1042: /* Urgency on bell */
            term->mode.bell_urgent = set;
            break;
        case 1043: /* Raise window on bell */
            term->mode.bell_raise = set;
            break;
        case 1044: /* Don't clear X11 CLIPBOARD selection */
            term->mode.keep_clipboard = set;
            break;
        case 1046: /* Allow altscreen */
            if (!set && term->mode.altscreen)
                term_swap_screen(term, 1);
            term->mode.disable_altscreen = !set;
            break;
        case 1047: /* Enable altscreen and clear screen */
            if (term->mode.disable_altscreen) break;
            if (set == !term->mode.altscreen) term_swap_screen(term, !set);
            if (set) term_erase(term, 0, 0, term->width, term->height, 0);
            break;
        case 1048: /* Save cursor  */
            term_cursor_mode(term, set);
            break;
        case 1049: /* Save cursor and switch to altscreen */
            if (term->mode.disable_altscreen) break;
            if (set == !term->mode.altscreen) {
                if (set) term_cursor_mode(term, 1);
                term_swap_screen(term, !set);
                if (!set) term_cursor_mode(term, 0);
            }
            if (set) term_erase(term, 0, 0, term->width, term->height, 0);
            break;
        case 1050: /* termcap function keys */
            // TODO Termcap
            break;
        case 1051: /* SUN function keys */
            term->kstate.keyboard_mapping = set ? keymap_sun : keymap_default;
            break;
        case 1052: /* HP function keys */
            term->kstate.keyboard_mapping = set ? keymap_hp : keymap_default;
            break;
        case 1053: /* SCO function keys */
            term->kstate.keyboard_mapping = set ? keymap_sco : keymap_default;
            break;
        case 1060: /* Legacy xterm function keys */
            term->kstate.keyboard_mapping = set ? keymap_legacy : keymap_default;
            break;
        case 1061: /* VT220 function keys */
            term->kstate.keyboard_mapping = set ? keymap_vt220 : keymap_default;
            break;
        case 2004: /* Bracketed paste */
            term->mode.bracketed_paste = set;
            break;
        case 2005: /* Paste quote */
            term->mode.paste_quote = set;
            break;
        case 2006: /* Paste literal NL */
            term->mode.paste_literal_nl = set;
            break;
        default:
            return 0;
        }
    } else {
        switch (mode) {
        case 0: /* Default - nothing */
            break;
        case 2: /* KAM */
            term->kstate.keylock = set;
            break;
        case 4: /* IRM */
            term->mode.insert = set;
            break;
        case 12: /* SRM */
            term->mode.echo = set;
            break;
        case 20: /* LNM */
            term->mode.crlf = set;
            break;
        default:
            term_esc_dump(term, 0);
        }
    }
    return 1;
}

static enum mode_status term_get_mode(struct term *term, bool private, uparam_t mode) {
    enum mode_status val = modstate_unrecognized;
#define MODSTATE(x) (modstate_enabled + !(x))
    if (private) {
        switch(mode) {
        case 1: /* DECCKM */
            val = MODSTATE(term->kstate.appcursor);
            break;
        case 2: /* DECANM */
            val = modstate_disabled;
            break;
        case 3: /* DECCOLM */
            val = MODSTATE(term->mode.columns_132);
            break;
        case 4: /* DECSCLM */
            val = MODSTATE(term->mode.smooth_scroll);
            break;
        case 5: /* DECCNM */
            val = MODSTATE(term->mode.reverse_video);
            break;
        case 6: /* DECCOM */
            val = MODSTATE(term->c.origin);
            break;
        case 7: /* DECAWM */
            val = MODSTATE(term->mode.wrap);
            break;
        case 8: /* DECARM */
            val = modstate_aways_disabled;
            break;
        case 9: /* X10 Mouse */
            val = MODSTATE(term->mstate.mouse_mode == mouse_mode_x10);
            break;
        case 10: /* Show toolbar */
            val = modstate_aways_disabled;
            break;
        case 12: /* Start blinking cursor */
            val = MODSTATE(window_get_cursor(term->win) & 1);
            break;
        case 13: /* Start blinking cursor (menu item) */
        case 14: /* Enable XORG of control sequence and menu for blinking */
            val = modstate_aways_disabled;
            break;
        case 18: /* DECPFF */
            val = MODSTATE(term->mode.print_form_feed);
            break;
        case 19: /* DECREX */
            val = MODSTATE(term->mode.print_extend);
            break;
        case 25: /* DECTCEM */
            val = MODSTATE(!term->mode.hide_cursor);
            break;
        case 30: /* Show scrollbar */
            val = modstate_aways_disabled;
            break;
        case 35: /* URXVT Allow change font */
            val = MODSTATE(term->mode.allow_change_font);
            break;
        case 40: /* 132COLS */
            val = MODSTATE(!term->mode.disable_columns_132);
            break;
        case 41: /* XTerm more(1) hack */
            val = MODSTATE(term->mode.xterm_more_hack);
            break;
        case 42: /* DECNRCM */
            val = MODSTATE(term->mode.enable_nrcs);
            break;
        case 44: /* Margin bell */
            val = MODSTATE(term->mode.margin_bell);
            break;
        case 45: /* Reverse wrap */
            val = MODSTATE(term->mode.reverse_wrap);
            break;
        case 47: /* Enable altscreen */
            val = MODSTATE(term->mode.altscreen);
            break;
        case 66: /* DECNKM */
            val = MODSTATE(term->kstate.appkey);
            break;
        case 67: /* DECBKM */
            val = MODSTATE(!term->kstate.backspace_is_del);
            break;
        case 69: /* DECLRMM */
            val = MODSTATE(term->mode.lr_margins);
            break;
        case 80: /* DECSDM */ //TODO SIXEL
            val = modstate_aways_disabled;
            break;
        case 95: /* DECNCSM */
            val = MODSTATE(term->mode.preserve_display_132);
            break;
        case 1000: /* X11 Mouse tracking */
            val = MODSTATE(term->mstate.mouse_mode == mouse_mode_x10);
            break;
        case 1001: /* Highlight mouse tracking */
            val = modstate_aways_disabled;
            break;
        case 1002: /* Cell motion tracking on keydown */
            val = MODSTATE(term->mstate.mouse_mode == mouse_mode_drag);
            break;
        case 1003: /* All motion mouse tracking */
            val = MODSTATE(term->mstate.mouse_mode == mouse_mode_motion);
            break;
        case 1004: /* Focus in/out events */
            val = MODSTATE(term->mode.track_focus);
            break;
        case 1005: /* UTF-8 mouse tracking */
            val = MODSTATE(term->mstate.mouse_format == mouse_format_utf8);
            break;
        case 1006: /* SGR Mouse tracking */
            val = MODSTATE(term->mstate.mouse_format == mouse_format_sgr);
            break;
        case 1007: /* Alternate scroll */
            val = MODSTATE(term->mode.altscreen_scroll);
            break;
        case 1010: /* Scroll to bottom on output */
            val = MODSTATE(term->mode.scroll_on_output);
            break;
        case 1011: /* Scroll to bottom on keypress */
            val = MODSTATE(!term->mode.no_scroll_on_input);
            break;
        case 1015: /* Urxvt mouse tracking */
            val = MODSTATE(term->mstate.mouse_format == mouse_format_uxvt);
            break;
        case 1016: /* SGR with pixels, XTerm */
            val = MODSTATE(term->mstate.mouse_format == mouse_format_pixel);
            break;
        case 1034: /* Interpret meta */
            val = MODSTATE(term->kstate.has_meta);
            break;
        case 1035: /* Numlock */
            val = MODSTATE(term->kstate.allow_numlock);
            break;
        case 1036: /* Meta sends escape */
            val = MODSTATE(term->kstate.meta_escape);
            break;
        case 1037: /* Backspace is delete */
            val = MODSTATE(term->kstate.backspace_is_del);
            break;
        case 1040: /* Don't clear X11 PRIMARY selecion */
            val = MODSTATE(term->mode.keep_selection);
            break;
        case 1041: /* Use CLIPBOARD instead of PRIMARY */
            val = MODSTATE(term->mode.select_to_clipboard);
            break;
        case 1042: /* Urgency on bell */
            val = MODSTATE(term->mode.bell_urgent);
            break;
        case 1043: /* Raise window on bell */
            val = MODSTATE(term->mode.bell_raise);
            break;
        case 1044: /* Don't clear X11 CLIPBOARD */
            val = MODSTATE(term->mode.keep_clipboard);
            break;
        case 1046: /* Allow altscreen */
            val = MODSTATE(!term->mode.disable_altscreen);
            break;
        case 1047: /* Enable altscreen and clear screen */
            val = MODSTATE(term->mode.altscreen);
            break;
        case 1048: /* Save cursor */
            val = modstate_aways_enabled;
            break;
        case 1049: /* Save cursor and switch to altscreen */
            val = MODSTATE(term->mode.altscreen);
            break;
        case 1050: /* termcap function keys */
            val = modstate_aways_disabled; //TODO Termcap
            break;
        case 1051: /* SUN function keys */
            val = MODSTATE(term->kstate.keyboard_mapping == keymap_sun);
            break;
        case 1052: /* HP function keys */
            val = MODSTATE(term->kstate.keyboard_mapping == keymap_hp);
            break;
        case 1053: /* SCO function keys */
            val = MODSTATE(term->kstate.keyboard_mapping == keymap_sco);
            break;
        case 1060: /* Legacy xterm function keys */
            val = MODSTATE(term->kstate.keyboard_mapping == keymap_legacy);
            break;
        case 1061: /* VT220 function keys */
            val = MODSTATE(term->kstate.keyboard_mapping == keymap_vt220);
            break;
        case 2004: /* Bracketed paste */
            val = MODSTATE(term->mode.bracketed_paste);
            break;
        case 2005: /* Paste literal NL */
            val = MODSTATE(term->mode.paste_literal_nl);
            break;
        case 2006: /* Paste quote */
            val = MODSTATE(term->mode.paste_quote);
            break;
        default:
            term_esc_dump(term, 0);
        }
    } else {
        switch(mode) {
        case 1: /* GATM */
        case 5: /* SRTM */
        case 7: /* VEM */
        case 10: /* HEM */
        case 11: /* PUM */
        case 13: /* FEAM */
        case 14: /* FETM */
        case 15: /* MATM */
        case 16: /* TTM */
        case 17: /* SATM */
        case 18: /* TSM */
        case 19: /* EBM */
            val = modstate_aways_disabled; /* always reset */
            break;
        case 2: /* KAM */
            val = MODSTATE(term->kstate.keylock); /* reset/set */
            break;
        case 3: /* CRM */
            val = modstate_disabled; /* reset */
            break;
        case 4: /* IRM */
            val = MODSTATE(term->mode.insert); /* reset/set */
            break;
        case 12: /* SRM */
            val = MODSTATE(term->mode.echo); /* reset/set */
            break;
        case 20: /* LNM */
            val = MODSTATE(term->mode.crlf); /* reset/set */
            break;
        default:
            term_esc_dump(term, 0);
        }
    }
#undef MODSTATE
    return val;
}

static void term_dispatch_mc(struct term *term) {
    if (term->esc.selector & P_MASK) {
        switch (PARAM(0, 0)) {
        case 1: /* Print current line */
            if (term->printerfd < 0) break;
            term_print_line(term, term->screen[term->c.y]);
            break;
        case 4: /* Disable autoprint */
        case 5: /* Enable autoprint */
            term->mode.print_auto = term->esc.param[0] == 5;
            break;
        case 11: /* Print scrollback and screen */
            if (term->printerfd < 0) break;
            for (ssize_t i = 1; i <= term->sb_limit; i++)
                term_print_line(term, line_at(term, -i));
        case 10: /* Print screen */
            term_print_screen(term, 1);
            break;
        default:
            term_esc_dump(term, 0);
        }
    } else {
        switch (PARAM(0, 0)) {
        case 0: /* Print screen */
            term_print_screen(term, term->mode.print_extend);
            break;
        case 4: /* Disable printer */
        case 5: /* Enable printer */
            term->mode.print_enabled = term->esc.param[0] == 5;
            break;
        default:
            term_esc_dump(term, 0);
        }

    }
}

static void term_dispatch_tmode(struct term *term, bool set) {
    for (size_t i = 0; i < term->esc.i; i++) {
        switch (term->esc.param[i]) {
        case 0:
            term->mode.title_set_hex = set;
            break;
        case 1:
            term->mode.title_query_hex = set;
            break;
        case 2:
            term->mode.title_set_utf8 = set;
            break;
        case 3:
            term->mode.title_query_utf8 = set;
            break;
        default:
            term_esc_dump(term, 0);
        }
    }
}

static void term_precompose_at_cursor(struct term *term, term_char_t ch) {
    struct cell *cel = &term->screen[term->c.y]->cell[term->c.x];
    if (term->c.x) cel--;
    if (!cel->ch && term->c.x > 1 && cel[-1].attr & attr_wide) cel--;
    ch = try_precompose(cel->ch, ch);
    if (cel->ch != ch) *cel = MKCELLWITH(*cel, ch);
}

static void term_putchar(struct term *term, term_char_t ch) {
    // 'print' state

    term->prev_ch = ch; // For REP CSI

    int16_t width = wcwidth(ch);
    if (width < 0) width = 1;
    else if (width > 1) width = 2;
    else if(!width) {
        term_precompose_at_cursor(term, ch);
        return;
    }

    if (iconf(ICONF_TRACE_CHARACTERS))
        info("Char: (%x) '%lc' ", ch, ch);

    // Wrap line if needed
    if (term->mode.wrap) {
        if (term->c.pending || (width == 2 && term->c.x == term_max_x(term) - 1))
            term_do_wrap(term);
    } else term->c.x = MIN(term->c.x, term_max_x(term) - width);

    struct line *line = term->screen[term->c.y];
    struct cell *cell = &line->cell[term->c.x];
    int16_t maxsx = term->c.x + width;

    // Shift characters to the left if insert mode is enabled
    if (term->mode.insert && term->c.x + width < term_max_x(term)) {
        for (struct cell *c = cell + width; c - line->cell < term_max_x(term); c++)
            c->attr &= ~attr_drawn;
        memmove(cell + width, cell, (term_max_x(term) - term->c.x - width)*sizeof(*cell));
        maxsx = MAX(maxsx, term_max_x(term));
    }

    // Erase overwritten parts of wide characters
    term_adjust_wide_left(term, term->c.x, term->c.y);
    term_adjust_wide_right(term, term->c.x + width - 1, term->c.y);

    // Clear selection when selected cell is overwritten
    if (mouse_is_selected_2(term, term->c.x, maxsx, term->c.y))
        mouse_clear_selection(term);

    // Put character itself
    term_put_cell(term, term->c.x, term->c.y, ch);

    // TODO Print multiple lines

    // Put dummy character to the left of wide
    if (__builtin_expect(width > 1, 0)) {
        // Don't need to call fixup_color,
        // it is already done
        cell[1] = term->c.cel;
        cell[0].attr |= attr_wide;
    }

    if (term->mode.margin_bell) {
        int16_t bcol = term->right - iconf(ICONF_MARGIN_BELL_COLUMN);
        if (term->c.x < bcol && term->c.x + width >= bcol)
            window_bell(term->win, term->mbvol);
    }

    term->c.pending = term->c.x + width == term_max_x(term);
    term->c.x += width - term->c.pending;
}

static void term_putstr(struct term *term, ssize_t count, ssize_t totalwidth) {

    // TODO Only handles enabled term->mode.wrap

    if (term->c.pending || (term->c.x == term_max_x(term) - 1 && term->predec_buf[0] < 0))
        term_do_wrap(term);

    struct line *line = term->screen[term->c.y];
    struct cell *cell = &line->cell[term->c.x];
    int16_t maxsx = term->c.x + totalwidth;

    // Shift characters to the left if insert mode is enabled
    if (term->mode.insert && term->c.x + totalwidth < term_max_x(term)) {
        for (struct cell *c = cell + totalwidth; c - line->cell < term_max_x(term); c++)
            c->attr &= ~attr_drawn;
        memmove(cell + totalwidth, cell, (term_max_x(term) - term->c.x - totalwidth)*sizeof(*cell));
        maxsx = MAX(maxsx, term_max_x(term));
    }

    // Clear selection if writing over it
    if (mouse_is_selected_2(term, term->c.x, maxsx, term->c.y))
        mouse_clear_selection(term);

    // Erase overwritten parts of wide characters
    term_adjust_wide_left(term, term->c.x, term->c.y);
    term_adjust_wide_right(term, term->c.x + totalwidth - 1, term->c.y);

    if (term->mode.margin_bell) {
        int16_t bcol = term->right - iconf(ICONF_MARGIN_BELL_COLUMN);
        if (term->c.x < bcol && term->c.x + totalwidth >= bcol)
            window_bell(term->win, term->mbvol);
    }

    if (iconf(ICONF_TRACE_CHARACTERS)) {
        for (ssize_t i = 0; i < count; i++)
            info("Char: (%x) '%lc' ", term->predec_buf[i], term->predec_buf[i]);
    }

    // Writing to the line resets its wrapping state
    line->wrapped = 0;

    // Allocate color for cell
    struct cell cur = fixup_color(line, &term->c);

    // Put charaters
    for (ssize_t i = 0; i < count; i++) {
        cur.ch = abs(term->predec_buf[i]);
        line->cell[term->c.x] = cur;

        // Put dummy character to the left of wide
        if (__builtin_expect(term->predec_buf[i] < 0, 0)) {
            line->cell[term->c.x].attr |= attr_wide;
            line->cell[++term->c.x] = term->c.cel;
        }

        term->c.x++;
    }

    term->c.pending = term->c.x == term_max_x(term);
    term->c.x -= term->c.pending;
    term->prev_ch = term->predec_buf[count - 1]; // For REP CSI
}

static void term_dispatch_window_op(struct term *term) {
    uparam_t pa = PARAM(0, 24);
    // Only title operations allowed by default
    if (!iconf(ICONF_ALLOW_WINDOW_OPS) &&
            (pa < 20 || pa > 23)) return;

    switch (pa) {
    case 1: /* Undo minimize */
        window_action(term->win, action_restore_minimized);
        break;
    case 2: /* Minimize */
        window_action(term->win, action_minimize);
        break;
    case 3: /* Move */
        window_move(term->win, PARAM(1,0), PARAM(2,0));
        break;
    case 4: /* Resize */
    case 8: /* Resize (in cell units) */
        term_request_resize(term, term->esc.param[2], term->esc.param[1], pa == 8);
        break;
    case 5: /* Raise */
        window_action(term->win, action_raise);
        break;
    case 6: /* Lower */
        window_action(term->win, action_lower);
        break;
    case 7: /* Refresh */
        term_damage_lines(term, 0, term->height);
        break;
    case 9: /* Maximize operations */ {
        enum window_action act = -1;

        switch(PARAM(1, 0)) {
        case 0: /* Undo maximize */
            act = action_restore;
            break;
        case 1: /* Maximize */
            act = action_maximize;
            break;
        case 2: /* Maximize vertically */
            act = action_maximize_height;
            break;
        case 3: /* Maximize horizontally */
            act = action_maximize_width;
            break;
        default:
            term_esc_dump(term, 0);
        }
        if (act >= 0) window_action(term->win, act);
        break;
    }
    case 10: /* Fullscreen operations */ {
        enum window_action act = -1;

        switch(PARAM(1, 0)) {
        case 0: /* Undo fullscreen */
            act = action_restore;
            break;
        case 1: /* Fullscreen */
            act = action_fullscreen;
            break;
        case 2: /* Toggle fullscreen */
            act = action_toggle_fullscreen;
            break;
        default:
            term_esc_dump(term, 0);
        }
        if (act >= 0) window_action(term->win, act);
        break;
    }
    case 11: /* Report state */
        term_answerback(term, CSI"%dt", 1 + !window_is_mapped(term->win));
        break;
    case 13: /* Report position opetations */
        switch(PARAM(1,0)) {
            int16_t x, y;
        case 0: /* Report window position */
            window_get_dim_ext(term->win, dim_window_position, &x, &y);
            term_answerback(term, CSI"3;%d;%dt", x, y);
            break;
        case 2: /* Report grid position */
            window_get_dim_ext(term->win, dim_grid_position, &x, &y);
            term_answerback(term, CSI"3;%d;%dt", x, y);
            break;
        default:
            term_esc_dump(term, 0);
        }
        break;
    case 14: /* Report size operations */
        switch(PARAM(1,0)) {
            int16_t x, y;
        case 0: /* Report grid size */
            window_get_dim_ext(term->win, dim_grid_size, &x, &y);
            term_answerback(term, CSI"4;%d;%dt", y, x);
            break;
        case 2: /* Report window size */
            window_get_dim(term->win, &x, &y);
            term_answerback(term, CSI"4;%d;%dt", y, x);
            break;
        default:
            term_esc_dump(term, 0);
        }
        break;
    case 15: /* Report screen size */ {
        int16_t x, y;
        window_get_dim_ext(term->win, dim_screen_size, &x, &y);
        term_answerback(term, CSI"5;%d;%dt", y, x);
        break;
    }
    case 16: /* Report cell size */ {
        int16_t x, y;
        window_get_dim_ext(term->win, dim_cell_size, &x, &y);
        term_answerback(term, CSI"6;%d;%dt", y, x);
        break;
    }
    case 18: /* Report grid size (in cell units) */
        term_answerback(term, CSI"8;%d;%dt", term->height, term->width);
        break;
    case 19: /* Report screen size (in cell units) */ {
        int16_t s_w, s_h, c_w, c_h, b_w, b_h;
        window_get_dim_ext(term->win, dim_screen_size, &s_w, &s_h);
        window_get_dim_ext(term->win, dim_cell_size, &c_w, &c_h);
        window_get_dim_ext(term->win, dim_border, &b_w, &b_h);
        term_answerback(term, CSI"9;%d;%dt", (s_h - 2*b_h)/c_h, (s_w - 2*b_w)/c_w);
        break;
    }
    case 20: /* Report icon label */
    case 21: /* Report title */ {
        bool tutf8;
        uint8_t *res = NULL, *res2 = NULL, *tit = NULL;
        window_get_title(term->win, pa == 20 ? target_icon_label : target_title, (char **)&tit, &tutf8);
        if (!tit) {
            warn("Can't get title");
            term_answerback(term, OSC"%c"ST, pa == 20 ? 'L' : 'l');
            break;
        }
        size_t tlen = strlen((const char *)tit);
        uint8_t *title = tit, *tmp = tit, *end = tit + tlen;

        if (!term->mode.title_query_utf8 && tutf8) {
            uint32_t u;
            while (utf8_decode(&u, (const uint8_t **)&title, end)) *tmp++ = u;
            *tmp = '\0';
            tlen = tmp - tit;
        } else if (term->mode.title_query_utf8 && !tutf8) {
            if ((tmp = res2 = malloc(2 * tlen + 1))) {
                while (*title) tmp += utf8_encode(*title++, tmp, res2 + 2 * tlen);
                *tmp = '\0';
                tlen = tmp - res2;
                title = res2;
            }
        }
        if (term->mode.title_query_hex) {
            if ((res = malloc(2 * tlen + 1))) {
                hex_encode(res, title, title + tlen);
                title = res;
            }
        }
        term_answerback(term, OSC"%c%s"ST, pa == 20 ? 'L' : 'l', title);
        free(res);
        free(tit);
        free(res2);
        break;
    }
    case 22: /* Save */
        switch (PARAM(1, 0)) {
        case 0: /* Title and icon label */
        case 1: /* Icon label */
        case 2: /* Title */
            window_push_title(term->win, 3 - PARAM(1, 0));
            break;
        default:
            term_esc_dump(term, 0);
        }
        break;
    case 23: /* Restore */
        switch (PARAM(1, 0)) {
        case 0: /* Title and icon label */
        case 1: /* Icon label */
        case 2: /* Title */
            window_pop_title(term->win, 3 - PARAM(1, 0));
            break;
        default:
            term_esc_dump(term, 0);
        }
        break;
    case 0:
    case 12:
    case 17: /* Invalid */
        term_esc_dump(term, 0);
        break;
    default: /* Resize window to PARAM(0, 24) lines */
        term_request_resize(term, -1, PARAM(0, 24), 1);
    }
}

static void term_report_cursor(struct term *term) {
    char csgr[3] = { 0x40, 0, 0 };
    if (term->c.cel.attr & attr_bold) csgr[0] |= 1;
    if (term->c.cel.attr & attr_underlined) csgr[0] |= 2;
    if (term->c.cel.attr & attr_blink) csgr[0] |= 4;
    if (term->c.cel.attr & attr_inverse) csgr[0] |= 8;

    if (iconf(ICONF_EXTENDED_CIR)) {
        csgr[0] |= 0x20;
        csgr[1] |= 0x40;
        // Extended byte
        if (term->c.cel.attr & attr_italic) csgr[1] |= 1;
        if (term->c.cel.attr & attr_faint) csgr[1] |= 2;
        if (term->c.cel.attr & attr_strikethrough) csgr[1] |= 4;
        if (term->c.cel.attr & attr_invisible) csgr[1] |= 8;
    }

    char cflags = 0x40;
    if (term->c.origin) cflags |= 1; // origin
    if (term->c.gl_ss == 2 && term->c.gl != 2) cflags |= 2; // ss2
    if (term->c.gl_ss == 3 && term->c.gl != 3) cflags |= 4; // ss3
    if (term->c.pending) cflags |= 8; // pending wrap

    char cg96 = 0x40;
    if (nrcs_is_96(term->c.gn[0])) cg96 |= 1;
    if (nrcs_is_96(term->c.gn[1])) cg96 |= 2;
    if (nrcs_is_96(term->c.gn[2])) cg96 |= 4;
    if (nrcs_is_96(term->c.gn[3])) cg96 |= 8;

    term_answerback(term, DCS"1$u%d;%d;1;%s;%c;%c;%d;%d;%c;%s%s%s%s"ST,
        /* line */ term->c.y + 1,
        /* column */ term->c.x + 1,
        /* attributes */ csgr,
        /* cell protection */ 0x40 + !!(term->c.cel.attr & attr_protected),
        /* flags */ cflags,
        /* gl */ term->c.gl,
        /* gr */ term->c.gr,
        /* cs size */ cg96,
        /* g0 */ nrcs_unparse(term->c.gn[0]),
        /* g1 */ nrcs_unparse(term->c.gn[1]),
        /* g2 */ nrcs_unparse(term->c.gn[2]),
        /* g3 */ nrcs_unparse(term->c.gn[3]));
}

static void term_report_tabs(struct term *term) {
    size_t caps = TABSR_INIT_CAP, len = 0;
    char *tabs = malloc(caps), *tmp;

    for (int16_t i = 0; tabs && i < term->width; i++) {
        if (term->tabs[i]) {
            if (len + TABSR_MAX_ENTRY > caps) {
                tmp = realloc(tabs, caps = TABSR_CAP_STEP(caps));
                if (!tmp) {
                    free(tabs);
                    tabs = NULL;
                    break;
                }
                tabs = tmp;
            }
            len += snprintf(tabs + len, caps, len ? "/%d" : "%d", i + 1);
        }
    }

    if (!tabs) tabs = "";
    term_answerback(term, DCS"2$u%s"ST, tabs);
}

// Utility functions for XTSAVE/XTRESTORE

inline static void store_mode(uint8_t modbits[], uparam_t mode, bool val) {
    if (mode < 96) modbits += mode / 8;
    else if (1000 <= mode && mode < 1064) modbits += mode / 8 - 113;
    else if (2000 <= mode && mode < 2007) modbits += 20;
    else {
        warn("Can't save mode %d", mode);
        return;
    }
    if (val) *modbits |= 1 << (mode % 8);
    else *modbits &= ~(1 << (mode % 8));
}

inline static bool load_mode(uint8_t modbits[], uparam_t mode) {
    if (mode < 96) modbits += mode / 8;
    else if (1000 <= mode && mode < 1064) modbits += mode / 8 - 113;
    else if (2000 <= mode && mode < 2007) modbits += 20;
    else {
        warn("Can't restore mode %d", mode);
        return 0;
    }
    return (*modbits >> (mode % 8)) & 1;
}

static void term_dispatch_csi(struct term *term) {
    // Fixup parameter count
    term->esc.i += term->esc.param[term->esc.i] >= 0;

    term_esc_dump(term, 1);

    // Only SGR is allowed to have subparams
    if (term->esc.subpar_mask && term->esc.selector != C('m')) return;

    switch (term->esc.selector) {
    case C('@'): /* ICH */
        term_insert_cells(term, PARAM(0, 1));
        break;
    case C('@') | I0(' '): /* SL */
        if (term_cursor_in_region(term))
            term_scroll_horizontal(term, term_min_x(term), PARAM(0, 1));
        break;
    case C('A'): /* CUU */
        (term->c.y >= term_min_y(term) ? term_bounded_move_to : term_move_to)
                (term, term->c.x, term->c.y - PARAM(0, 1));
        break;
    case C('A') | I0(' '): /* SR */
        if (term_cursor_in_region(term))
            term_scroll_horizontal(term, term_min_x(term), -PARAM(0, 1));
        break;
    case C('B'): /* CUD */
        (term->c.y < term_max_y(term) ? term_bounded_move_to : term_move_to)
                (term, term->c.x, term->c.y + PARAM(0, 1));
        break;
    case C('e'): /* VPR */
        (term->c.origin ? term_bounded_move_to : term_move_to)
                (term, term->c.x, term->c.y + PARAM(0, 1));
        break;
    case C('C'): /* CUF */
        (term->c.x < term_max_x(term) ? term_bounded_move_to : term_move_to)
                (term, term->c.x + PARAM(0, 1),  term->c.y);
        break;
    case C('D'): /* CUB */
        term_move_left(term, PARAM(0, 1));
        break;
    case C('E'): /* CNL */
        (term->c.y < term_max_y(term) ? term_bounded_move_to : term_move_to)
                (term, term->c.x, term->c.y + PARAM(0, 1));
        term_cr(term);
        break;
    case C('F'): /* CPL */
        (term->c.y >= term_min_y(term) ? term_bounded_move_to : term_move_to)
                (term, term->c.x, term->c.y - PARAM(0, 1));
        term_cr(term);
        break;
    case C('`'): /* HPA */
    case C('G'): /* CHA */
        (term->c.origin ? term_bounded_move_to : term_move_to)
                (term, term_min_ox(term) + PARAM(0, 1) - 1, term->c.y);
        break;
    case C('H'): /* CUP */
    case C('f'): /* HVP */
        (term->c.origin ? term_bounded_move_to : term_move_to)
                (term, term_min_ox(term) + PARAM(1, 1) - 1, term_min_oy(term) + PARAM(0, 1) - 1);
        break;
    case C('I'): /* CHT */
        term_tabs(term, PARAM(0, 1));
        break;
    case C('J') | P('?'): /* DECSED */
    case C('J'): /* ED */ {
        void (*erase)(struct term *, int16_t, int16_t, int16_t, int16_t, bool) =
                term->esc.selector & P_MASK ? (term->mode.protected ? term_erase : term_selective_erase) :
                term->mode.protected ? term_protective_erase : term_erase;
        switch (PARAM(0, 0)) {
        case 0: /* Below */
            term_adjust_wide_left(term, term->c.x, term->c.y);
            erase(term, term->c.x, term->c.y, term->width, term->c.y + 1, 0);
            erase(term, 0, term->c.y + 1, term->width, term->height, 0);
            break;
        case 1: /* Above */
            term_adjust_wide_right(term, term->c.x, term->c.y);
            erase(term, 0, term->c.y, term->c.x + 1, term->c.y + 1, 0);
            erase(term, 0, 0, term->width, term->c.y, 0);
            break;
        case 2: /* All */
            erase(term, 0, 0, term->width, term->height, 0);
            break;
        case 3: /* Scrollback */
            if (iconf(ICONF_ALLOW_ERASE_SCROLLBACK) && !term->mode.altscreen) {
                term_free_scrollback(term);
                break;
            }
        default:
            term_esc_dump(term, 0);
        }
        term_reset_pending(term);
        break;
    }
    case C('K') | P('?'): /* DECSEL */
    case C('K'): /* EL */ {
        void (*erase)(struct term *, int16_t, int16_t, int16_t, int16_t, bool) =
                term->esc.selector & P_MASK ? (term->mode.protected ? term_erase : term_selective_erase) :
                term->mode.protected ? term_protective_erase : term_erase;
        switch (PARAM(0, 0)) {
        case 0: /* To the right */
            term_adjust_wide_left(term, term->c.x, term->c.y);
            erase(term, term->c.x, term->c.y, term->width, term->c.y + 1, 0);
            break;
        case 1: /* To the left */
            term_adjust_wide_right(term, term->c.x, term->c.y);
            erase(term, 0, term->c.y, term->c.x + 1, term->c.y + 1, 0);
            break;
        case 2: /* Whole */
            erase(term, 0, term->c.y, term->width, term->c.y + 1, 0);
            break;
        default:
            term_esc_dump(term, 0);
        }
        term_reset_pending(term);
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
        break;
    case C('S'): /* SU */
        term_scroll(term, term_min_y(term), PARAM(0, 1), 0);
        break;
    case C('T') | P('>'): /* XTRMTITLE */
        term_dispatch_tmode(term, 0);
        break;
    case C('T'): /* SD */
    case C('^'): /* SD */
        term_scroll(term, term_min_y(term), -PARAM(0, 1), 0);
        break;
    case C('X'): /* ECH */
        (term->mode.protected ? term_protective_erase : term_erase)
                (term, term->c.x, term->c.y, term->c.x + PARAM(0, 1), term->c.y + 1, 0);
        term_reset_pending(term);
        break;
    case C('Z'): /* CBT */
        term_tabs(term, -PARAM(0, 1));
        break;
    case C('a'): /* HPR */
        (term->c.origin ? term_bounded_move_to : term_move_to)
                (term, term->c.x + PARAM(0, 1), term->c.y + PARAM(1, 0));
        break;
    case C('b'): /* REP */
        for (uparam_t i = PARAM(0, 1); i > 0; i--)
            term_putchar(term, term->prev_ch);
        break;
    case C('c'): /* DA1 */
    case C('c') | P('>'): /* DA2 */
    case C('c') | P('='): /* DA3 */
        if (PARAM(0, 0)) break;
        term_dispatch_da(term, term->esc.selector & P_MASK);
        break;
    case C('d'): /* VPA */
        (term->c.origin ? term_bounded_move_to : term_move_to)
                (term, term->c.x, term_min_oy(term) + PARAM(0, 1) - 1);
        break;
    case C('g'): /* TBC */
        switch (PARAM(0, 0)) {
        case 0:
            term->tabs[term->c.x] = 0;
            break;
        case 3:
            memset(term->tabs, 0, term->width * sizeof(term->tabs[0]));
            break;
        default:
            term_esc_dump(term, 0);
        }
        break;
    case C('h'): /* SM */
    case C('h') | P('?'): /* DECSET */
        for (size_t i = 0; i < term->esc.i; i++)
            if (!term_srm(term, term->esc.selector & P_MASK, PARAM(i, 0), 1))
                term_esc_dump(term, 0);
        break;
    case C('i'): /* MC */
    case C('i') | P('?'): /* MC */
        term_dispatch_mc(term);
        break;
    case C('l'): /* RM */
    case C('l') | P('?'):/* DECRST */
        for (size_t i = 0; i < term->esc.i; i++)
            if (!term_srm(term, term->esc.selector & P_MASK, PARAM(i, 0), 0))
                term_esc_dump(term, 0);
        break;
    case C('m') | P('>'): /* XTMODKEYS */ {
        uparam_t p = PARAM(0, 0), inone = !term->esc.i && term->esc.param[0] < 0;
        if (term->esc.i > 0 && term->esc.param[1] >= 0) {
            switch (p) {
            case 0:
                term->kstate.modkey_legacy_allow_keypad = PARAM(1, 0) & 1;
                term->kstate.modkey_legacy_allow_edit_keypad = PARAM(1, 0) & 2;
                term->kstate.modkey_legacy_allow_function = PARAM(1, 0) & 4;
                term->kstate.modkey_legacy_allow_misc = PARAM(1, 0) & 8;
                break;
            case 1:
                term->kstate.modkey_cursor = PARAM(1, 0) + 1;
                break;
            case 2:
                term->kstate.modkey_fn = PARAM(1, 0) + 1;
                break;
            case 3:
                term->kstate.modkey_keypad = PARAM(1, 0) + 1;
                break;
            case 4:
                term->kstate.modkey_other = PARAM(1, 0);
                break;
            }
        } else {
            if (inone || p == 0) {
                term->kstate.modkey_legacy_allow_keypad = iconf(ICONF_MALLOW_KEYPAD);
                term->kstate.modkey_legacy_allow_edit_keypad = iconf(ICONF_MALLOW_EDIT);
                term->kstate.modkey_legacy_allow_function = iconf(ICONF_MALLOW_FUNCTION);
                term->kstate.modkey_legacy_allow_misc = iconf(ICONF_MALLOW_MISC);
            }
            if (inone || p == 1) term->kstate.modkey_cursor = iconf(ICONF_MODIFY_CURSOR);
            if (inone || p == 2) term->kstate.modkey_fn = iconf(ICONF_MODIFY_FUNCTION);
            if (inone || p == 3) term->kstate.modkey_keypad = iconf(ICONF_MODIFY_KEYPAD);
            if (inone || p == 4) term->kstate.modkey_other = iconf(ICONF_MODIFY_OTHER);
        }
        break;
    }
    case C('m'): /* SGR */
        term_decode_sgr(term, 0, &(struct cell){0}, &term->c.cel, &term->c.fg, &term->c.bg);
        break;
    case C('n') | P('>'): /* Disable key modifires, xterm */ {
            uparam_t p = term->esc.param[0];
            if (p == 0) {
                term->kstate.modkey_legacy_allow_keypad = 0;
                term->kstate.modkey_legacy_allow_edit_keypad = 0;
                term->kstate.modkey_legacy_allow_function = 0;
                term->kstate.modkey_legacy_allow_misc = 0;
            }
            if (p == 1) term->kstate.modkey_cursor = 0;
            if (p == 2) term->kstate.modkey_fn = 0;
            if (p == 3) term->kstate.modkey_keypad = 0;
            if (p == 4) term->kstate.modkey_other = 0;
            break;
    }
    case C('n') | P('?'): /* DECDSR */
    case C('n'):
        term_dispatch_dsr(term);
        break;
    case C('q'): /* DECLL */
        for (uparam_t i = 0; i < term->esc.i; i++) {
            switch (PARAM(i, 0)) {
            case 1: term->mode.led_num_lock = 1; break;
            case 2: term->mode.led_caps_lock = 1; break;
            case 3: term->mode.led_scroll_lock = 1; break;
            case 0: term->mode.led_caps_lock = 0;
                    term->mode.led_scroll_lock = 0; /* fallthrough */
            case 21: term->mode.led_num_lock = 0; break;
            case 22: term->mode.led_caps_lock = 0; break;
            case 23: term->mode.led_scroll_lock = 0; break;
            default:
                term_esc_dump(term, 0);
            }
        }
        break;
    case C('r'): /* DECSTBM */
        term_set_tb_margins(term, PARAM(0, 1) - 1, PARAM(1, term->height) - 1);
        term_move_to(term, term_min_ox(term), term_min_oy(term));
        break;
    case C('s'): /* DECSLRM/(SCOSC) */
        if (term->mode.lr_margins) {
            term_set_lr_margins(term, PARAM(0, 1) - 1, PARAM(1, term->width) - 1);
            term_move_to(term, term_min_ox(term), term_min_oy(term));
        } else {
            term_cursor_mode(term, 1);
        }
        break;
    case C('t'): /* XTWINOPS, xterm */
        term_dispatch_window_op(term);
        break;
    case C('t') | P('>'):/* XTSMTITLE */
        term_dispatch_tmode(term, 1);
        break;
    case C('u'): /* (SCORC) */
        term_cursor_mode(term, 0);
        break;
    case C('x'): /* DECREQTPARAM */
        if (term->vt_version < 200) {
            uparam_t p = PARAM(0, 0);
            if (p < 2) term_answerback(term, CSI"%d;1;1;128;128;1;0x", p + 2);
        }
        break;
    case C('q') | I0(' '): /* DECSCUSR */ {
        enum cursor_type csr = PARAM(0, 1);
        if (csr < 7) window_set_cursor(term->win, csr);
        break;
    }
    case C('p') | I0('!'): /* DECSTR */
        term_do_reset(term, 0);
        break;
    case C('p') | I0('"'): /* DECSCL */
        if (term->vt_version < 200) break;

        term_do_reset(term, 0);
        uparam_t p = PARAM(0, 65) - 60;
        if (p && p <= term->vt_version/100)
            term->vt_level = p;
        if (p > 1) switch (PARAM(1, 2)) {
        case 2: term->mode.eight_bit = 1; break;
        case 1: term->mode.eight_bit = 0; break;
        default:
            term_esc_dump(term, 0);
        }
        break;
    case C('q') | I0('"'): /* DECSCA */
        switch (PARAM(0, 2)) {
        case 1:
            term->c.cel.attr |= attr_protected;
            break;
        case 0: case 2:
            term->c.cel.attr &= ~attr_protected;
            break;
        }
        term->mode.protected = 0;
        break;
    case C('p') | I0('$'): /* RQM -> RPM */
        CHK_VT(3);
        term_answerback(term, CSI"%d;%d$y", PARAM(0, 0), term_get_mode(term, 0, PARAM(0, 0)));
        break;
    case C('p') | P('?') | I0('$'): /* DECRQM -> DECRPM */
        CHK_VT(3);
        term_answerback(term, CSI"?%d;%d$y", PARAM(0, 0), term_get_mode(term, 1, PARAM(0, 0)));
        break;
    case C('r') | I0('$'): /* DECCARA */
        CHK_VT(4);
        term_apply_sgr(term, term_min_ox(term) + PARAM(1, 1) - 1, term_min_oy(term) + PARAM(0, 1) - 1,
                term_min_ox(term) + PARAM(3, term_max_ox(term) - term_min_ox(term)),
                term_min_oy(term) + PARAM(2, term_max_oy(term) - term_min_oy(term)));
        break;
    case C('t') | I0('$'): /* DECRARA */
        CHK_VT(4);
        term_reverse_sgr(term, term_min_ox(term) + PARAM(1, 1) - 1, term_min_oy(term) + PARAM(0, 1) - 1,
                term_min_ox(term) + PARAM(3, term_max_ox(term) - term_min_ox(term)),
                term_min_oy(term) + PARAM(2, term_max_oy(term) - term_min_oy(term)));
        break;
    case C('v') | I0('$'): /* DECCRA */
        CHK_VT(4);
        term_copy(term, term_min_ox(term) + PARAM(1, 1) - 1, term_min_oy(term) + PARAM(0, 1) - 1,
                term_min_ox(term) + PARAM(3, term_max_ox(term) - term_min_ox(term)),
                term_min_oy(term) + PARAM(2, term_max_oy(term) - term_min_oy(term)),
                term_min_ox(term) + PARAM(6, 1) - 1, term_min_oy(term) + PARAM(5, 1) - 1, 1);
        break;
    case C('|') | I0('#'): /* XTREPORTSGR */
        CHK_VT(4);
        term_report_sgr(term, term_min_ox(term) + PARAM(1, 1) - 1, term_min_oy(term) + PARAM(0, 1) - 1,
                term_min_ox(term) + PARAM(3, term_max_ox(term) - term_min_ox(term)),
                term_min_oy(term) + PARAM(2, term_max_oy(term) - term_min_oy(term)));
        break;
    case C('w') | I0('$'): /* DECRQPSR */
        switch(PARAM(0, 0)) {
        case 1: /* -> DECCIR */
            term_report_cursor(term);
            break;
        case 2: /* -> DECTABSR */
            term_report_tabs(term);
            break;
        default:
            term_esc_dump(term, 0);
        }
        break;
    case C('x') | I0('$'): /* DECFRA */
        CHK_VT(4);
        term_fill(term, term_min_ox(term) + PARAM(2, 1) - 1, term_min_oy(term) + PARAM(1, 1) - 1,
                term_min_ox(term) + PARAM(4, term_max_ox(term) - term_min_ox(term)),
                term_min_oy(term) + PARAM(3, term_max_oy(term) - term_min_oy(term)), 1, PARAM(0, 0));
        break;
    case C('z') | I0('$'): /* DECERA */
        CHK_VT(4);
        (term->mode.protected ? term_protective_erase : term_erase)
                (term, term_min_ox(term) + PARAM(1, 1) - 1, term_min_oy(term) + PARAM(0, 1) - 1,
                term_min_ox(term) + PARAM(3, term_max_ox(term) - term_min_ox(term)),
                term_min_oy(term) + PARAM(2, term_max_oy(term) - term_min_oy(term)), 1);
        break;
    case C('{') | I0('$'): /* DECSERA */
        CHK_VT(4);
        (term->mode.protected ? term_erase : term_selective_erase)
                (term, term_min_ox(term) + PARAM(1, 1) - 1, term_min_oy(term) + PARAM(0, 1) - 1,
                term_min_ox(term) + PARAM(3, term_max_ox(term) - term_min_ox(term)),
                term_min_oy(term) + PARAM(2, term_max_oy(term) - term_min_oy(term)), 1);
        break;
    case C('y') | I0('*'): /* DECRQCRA */
        CHK_VT(4);
        uint16_t sum = term_checksum(term, term_min_ox(term) + PARAM(3, 1) - 1, term_min_oy(term) + PARAM(2, 1) - 1,
                term_min_ox(term) + PARAM(5, term_max_ox(term) - term_min_ox(term)),
                term_min_oy(term) + PARAM(4, term_max_oy(term) - term_min_oy(term)));
        // DECRPCRA
        term_answerback(term, DCS"%d!~%04X"ST, PARAM(0, 0), sum);
        break;
    case C('y') | I0('#'): /* XTCHECKSUM */;
        p = PARAM(0, 0);
        term->checksum_mode = (struct checksum_mode) {
            .positive = p & 1,
            .no_attr = p & 2,
            .no_trim = p & 4,
            .no_implicit = p & 8,
            .wide = p & 16,
            .eight_bit = p & 32,
        };
        break;
    case C('}') | I0('\''): /* DECIC */
        CHK_VT(4);
        term_insert_columns(term, PARAM(0, 1));
        break;
    case C('~') | I0('\''): /* DECDC */
        CHK_VT(4);
        term_delete_columns(term, PARAM(0, 1));
        break;
    case C('x') | I0('*'): /* DECSACE */
        CHK_VT(4);
        switch (PARAM(0, 1)) {
        case 1: term->mode.attr_ext_rectangle = 0; break;
        case 2: term->mode.attr_ext_rectangle = 1; break;
        default: term_esc_dump(term, 0);
        }
        break;
    case C('|') | I0('$'): /* DECSCPP */
        if (iconf(ICONF_ALLOW_WINDOW_OPS))
            term_request_resize(term, PARAM(0, 80), -1, 1);
        break;
    case C('|') | I0('*'): /* DECSNLS */
        if (iconf(ICONF_ALLOW_WINDOW_OPS))
            term_request_resize(term, -1, PARAM(0, 24), 1);
    case C('W') | P('?'): /* DECST8C */
        if (PARAM(0, 5) == 5) term_reset_tabs(term);
        else term_esc_dump(term, 0);
        break;
    case C('s') | P('?'): /* XTSAVE */
        for (size_t i = 0; i < term->esc.i; i++) {
            uparam_t mode = PARAM(i, 0);
            enum mode_status val = term_get_mode(term, 1, mode);
            if (val == modstate_enabled || val == modstate_disabled) {
                switch(mode) {
                case 1005: case 1006: case 1015: case 1016:
                    term->saved_mouse_format = term->mstate.mouse_format;
                    break;
                case 9: case 1000: case 1001:
                case 1002: case 1003:
                    term->saved_mouse_mode = term->mstate.mouse_mode;
                    break;
                case 1050: case 1051: case 1052:
                case 1053: case 1060: case 1061:
                    term->saved_keyboard_type = term->kstate.keyboard_mapping;
                    break;
                case 1048:
                    term_cursor_mode(term, 1);
                    break;
                case 1047: case 1049:
                    mode = 47;
                default:
                    store_mode(term->saved_modbits, mode, val == modstate_enabled);
                }
            }
        }
        break;
    case C('r') | P('?'): /* XTRESTORE */
        for (size_t i = 0; i < term->esc.i; i++) {
            uparam_t mode = PARAM(i, 0);
            switch(mode) {
            case 1005: case 1006: case 1015: case 1016:
                term->mstate.mouse_format = term->saved_mouse_format;
                break;
            case 9: case 1000: case 1001:
            case 1002: case 1003:
                term->mstate.mouse_mode = term->saved_mouse_mode;
                window_set_mouse(term->win, term->mstate.mouse_mode == mouse_mode_motion);
                break;
            case 1050: case 1051: case 1052:
            case 1053: case 1060: case 1061:
                term->kstate.keyboard_mapping = term->saved_keyboard_type;
                break;
            case 1048:
                term_cursor_mode(term, 0);
                break;
            case 1047: case 1049:
                mode = 47;
            default:
                term_srm(term, 1, mode, load_mode(term->saved_modbits, mode));
            }
        }
        break;
    case C('u') | I0('&'): /* DECRQUPSS */
        term_answerback(term, DCS"%d!u%s"ST,
                term->c.upcs > cs96_latin_1, nrcs_unparse(term->c.upcs));
        break;
    case C('t') | I0(' '): /* DECSWBV */
        switch (PARAM(0, 1)) {
        case 1:
            term->bvol = 0;
            break;
        case 2: case 3: case 4:
            term->bvol = iconf(ICONF_BELL_LOW_VOLUME);
            break;
        default:
            term->bvol = iconf(ICONF_BELL_HIGH_VOLUME);
        }
        break;
    case C('u') | I0(' '): /* DECSMBV */
        switch (PARAM(0, 8)) {
        case 1:
            term->mbvol = 0;
            break;
        case 2: case 3: case 4:
            term->mbvol = iconf(ICONF_MARGIN_BELL_LOW_VOLUME);
            break;
        default:
            term->mbvol = iconf(ICONF_MARGIN_BELL_HIGH_VOLUME);
        }
        break;
    case C('w') | I0('\''): /* DECEFR */ {
        int16_t x, y;
        window_get_pointer(term->win, &x, &y, NULL);
        mouse_set_filter(term, PARAM(1, y), PARAM(0, x), PARAM(3, y), PARAM(4, x));
        break;
    }
    case C('z') | I0('\''): /* DECELR */
        switch(PARAM(0, 0)) {
        case 0:
            term->mstate.locator_enabled = 0;
            break;
        case 1:
            term->mstate.locator_oneshot = 1;
        case 2:
            term->mstate.locator_enabled = 1;
            break;
        default:
            term_esc_dump(term, 0);
        }
        switch(PARAM(1, 2)) {
        case 2:
            term->mstate.locator_pixels = 0;
            break;
        case 1:
            term->mstate.locator_pixels = 1;
            break;
        default:
            term_esc_dump(term, 0);
        }
        break;
    case C('{') | I0('\''): /* DECSLE */
        term->esc.i += !term->esc.i;
        for (size_t i = 0; i < term->esc.i; i++) {
            switch(PARAM(i, 0)) {
            case 0: /* Only explicit requests */
                term->mstate.locator_report_press = 0;
            case 4: /* Disable up */
                term->mstate.locator_report_release = 0;
                break;
            case 1: /* Enable down */
                term->mstate.locator_report_press = 1;
                break;
            case 2: /* Disable down */
                term->mstate.locator_report_press = 0;
                break;
            case 3: /* Enable up */
                term->mstate.locator_report_release = 1;
                break;
            default:
                term_esc_dump(term, 0);
            }
        }
        break;
    case C('|') | I0('\''): /* DECRQLP */ {
        int16_t x, y;
        uint32_t mask;
        window_get_pointer(term->win, &x, &y, &mask);
        mouse_report_locator(term, 1, x, y, mask);
        break;
    }
    //case C('p') | P('>'): /* XTSMPOINTER */ // TODO Pointer
    //    break;
    //case C('S') | P('?'): /* XTSMSGRAPHICS */ // TODO SIXEL
    //    break;
    //case C('S') | P('>'): /* Set graphics attributes, xterm */ //TODO SIXEL
    //    break;
    default:
        term_esc_dump(term, 0);
    }

    term->esc.state = esc_ground;
}

static void term_dispatch_esc(struct term *term) {
    if (iconf(ICONF_TRACE_CONTROLS)) {
        if (term->esc.selector != E('[') && term->esc.selector != E('P') &&
                term->esc.selector != E(']'))
            term_esc_dump(term, 1);
    }

    switch (term->esc.selector) {
    case E('D'): /* IND */
        term_index(term);
        break;
    case E('E'): /* NEL */
        term_index(term);
        term_cr(term);
        break;
    case E('F'): /* HP Home Down */
        term_move_to(term, term_min_ox(term), term_max_oy(term));
        break;
    case E('H'): /* HTS */
        term->tabs[term->c.x] = 1;
        break;
    case E('M'): /* RI */
        term_rindex(term);
        break;
    case E('N'): /* SS2 */
        term->c.gl_ss = 2;
        break;
    case E('O'): /* SS3 */
        term->c.gl_ss = 3;
        break;
    case E('P'): /* DCS */
        term->esc.state = esc_dcs_entry;
        term->esc.old_state = 0;
        return;
    case E('V'): /* SPA */
        term->c.cel.attr |= attr_protected;
        term->mode.protected = 1;
        break;
    case E('W'): /* EPA */
        term->c.cel.attr &= ~attr_protected;
        term->mode.protected = 1;
        break;
    case E('Z'): /* DECID */
        term_dispatch_da(term, 0);
        break;
    case E('['): /* CSI */
        term->esc.state = esc_csi_entry;
        term->esc.old_state = 0;
        return;
    case E('\\'): /* ST */
        if (term->esc.old_state == esc_dcs_string)
            term_dispatch_dcs(term);
        else if (is_osc_state(term->esc.old_state))
            term_dispatch_osc(term);
        break;
    case E(']'): /* OSC */
        term->esc.old_state = 0;
        term->esc.state = esc_osc_entry;
        return;
    case E('X'): /* SOS */
    case E('^'): /* PM */
    case E('_'): /* APC */
        term->esc.old_state = 0;
        term->esc.state = esc_ign_entry;
        return;
    case E('6'): /* DECBI */
        CHK_VT(4);
        term_rindex_horizonal(term);
        break;
    case E('7'): /* DECSC */
        term_cursor_mode(term, 1);
        break;
    case E('8'): /* DECRC */
        term_cursor_mode(term, 0);
        break;
    case E('9'): /* DECFI */
        CHK_VT(4);
        term_index_horizonal(term);
        break;
    case E('='): /* DECKPAM */
        term->kstate.appkey = 1;
        break;
    case E('>'): /* DECKPNM */
        term->kstate.appkey = 0;
        break;
    case E('c'): /* RIS */
        term_do_reset(term, 1);
        break;
    case E('k'): /* Old style title */
        term->esc.state = esc_osc_string;
        term->esc.selector = 2;
        term->esc.old_state = 0;
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
        term->mode.eight_bit = 0;
        break;
    case E('G') | I0(' '): /* S8C1T */
        CHK_VT(2);
        term->mode.eight_bit = 1;
        break;
    case E('L') | I0(' '): /* ANSI_LEVEL_1 */
    case E('M') | I0(' '): /* ANSI_LEVEL_2 */
        term->c.gn[1] = cs94_ascii;
        term->c.gr = 1;
        /* fallthrough */
    case E('N') | I0(' '): /* ANSI_LEVEL_3 */
        term->c.gn[0] = cs94_ascii;
        term->c.gl = term->c.gl_ss = 0;
        break;
    //case E('3') | I0('#'): /* DECDHL */
    //case E('4') | I0('#'): /* DECDHL */
    //case E('5') | I0('#'): /* DECSWL */
    //case E('6') | I0('#'): /* DECDWL */
    //    break;
    case E('8') | I0('#'): /* DECALN*/
        term_reset_margins(term);
        mouse_clear_selection(term);
        term_move_to(term, 0, 0);
        for (ssize_t i = 0; i < term->height; i++)
            for (ssize_t j = 0; j < term->width; j++)
                term_put_cell(term, j, i, 'E');
        break;
    case E('@') | I0('%'): /* Disable UTF-8 */
        term->mode.utf8 = 0;
        break;
    case E('G') | I0('%'): /* Eable UTF-8 */
    case E('8') | I0('%'):
        term->mode.utf8 = 1;
        break;
    default: {
        /* Decode select charset */
        enum charset set;
        switch (term->esc.selector & I0_MASK) {
        case I0('*'): /* G2D4 */
        case I0('+'): /* G3D4 */
        case I0('('): /* GZD4 */
        case I0(')'): /* G1D4 */
            if ((set = nrcs_parse(term->esc.selector, 0, term->vt_level, term->mode.enable_nrcs)) > 0)
                term->c.gn[((term->esc.selector & I0_MASK) - I0('(')) >> 9] = set;
            break;
        case I0('-'): /* G1D6 */
        case I0('.'): /* G2D6 */
        case I0('/'): /* G3D6 */
            if ((set = nrcs_parse(term->esc.selector, 1, term->vt_level, term->mode.enable_nrcs)) > 0)
                term->c.gn[1 + (((term->esc.selector & I0_MASK) - I0('-')) >> 9)] = set;
            break;
        default:
            term_esc_dump(term, 0);
        }
    }
    }

    term_esc_finish_string(term);
    term->esc.old_state = 0;
    term->esc.state = esc_ground;
}

static void term_dispatch_c0(struct term *term, term_char_t ch) {
    if (iconf(ICONF_TRACE_CONTROLS) && ch != 0x1B)
        info("Seq: ^%c", ch ^ 0x40);

    switch (ch) {
    case 0x00: /* NUL (IGNORE) */
    case 0x01: /* SOH (IGNORE) */
    case 0x02: /* STX (IGNORE) */
    case 0x03: /* ETX (IGNORE) */
    case 0x04: /* EOT (IGNORE) */
        break;
    case 0x05: /* ENQ */
        term_answerback(term, "%s", sconf(SCONF_ANSWERBACK_STRING));
        break;
    case 0x06: /* ACK (IGNORE) */
        break;
    case 0x07: /* BEL */
        if (term->esc.state == esc_dcs_string)
            term_dispatch_dcs(term);
        else if (is_osc_state(term->esc.state))
            term_dispatch_osc(term);
        else window_bell(term->win, term->bvol);
        break;
    case 0x08: /* BS */
        term_move_left(term, 1);
        break;
    case 0x09: /* HT */
        term_tabs(term, 1);
        break;
    case 0x0a: /* LF */
    case 0x0b: /* VT */
    case 0x0c: /* FF */
        if (!term->mode.print_enabled && term->mode.print_auto)
            term_print_line(term, term->screen[term->c.y]);
        term_index(term);
        if (term->mode.crlf) term_cr(term);
        break;
    case 0x0d: /* CR */
        term_cr(term);
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
        term_reset_pending(term);
        // Clear selection when selected cell is overwritten
        if (mouse_is_selected(term, term->c.x, term->c.y))
            mouse_clear_selection(term);
        term_put_cell(term, term->c.x, term->c.y, '?');
    case 0x18: /* CAN */
        term_esc_finish_string(term);
        term->esc.state = esc_ground;
        break;
    case 0x19: /* EM (IGNORE) */
        break;
    case 0x1b: /* ESC */
        term->esc.old_selector = term->esc.selector;
        term->esc.old_state = term->esc.state;
        term->esc.state = term->vt_level ? esc_esc_entry : esc_vt52_entry;
        break;
    case 0x1c: /* FS (IGNORE) */
    case 0x1d: /* GS (IGNORE) */
    case 0x1e: /* RS (IGNORE) */
    case 0x1f: /* US (IGNORE) */
        break;
    }
}

static void term_dispatch_vt52(struct term *term, term_char_t ch) {
    switch (ch) {
    case '<':
        if (term->vt_version >= 100)
            term_set_vt52(term, 0);
        break;
    case '=':
        term->kstate.appkey = 1;
        break;
    case '>':
        term->kstate.appkey = 0;
        break;
    case 'A':
        (term->c.origin ? term_bounded_move_to : term_move_to)
                (term, term->c.x, term->c.y - 1);
        break;
    case 'B':
        (term->c.origin ? term_bounded_move_to : term_move_to)
                (term, term->c.x, term->c.y + 1);
        break;
    case 'C':
        (term->c.origin ? term_bounded_move_to : term_move_to)
                (term, term->c.x + 1, term->c.y);
        break;
    case 'D':
        (term->c.origin ? term_bounded_move_to : term_move_to)
                (term, term->c.x - 1, term->c.y);
        break;
    case 'F':
        term->c.gl = term->c.gl_ss = 1;
        break;
    case 'G':
        term->c.gl = term->c.gl_ss = 0;
        break;
    case 'H':
        term_move_to(term, term_min_ox(term), term_min_oy(term));
        break;
    case 'I':
        term_rindex(term);
        break;
    case 'J':
        term_adjust_wide_left(term, term->c.x, term->c.y);
        term_erase(term, term->c.x, term->c.y, term->width, term->c.y + 1, 0);
        term_erase(term, 0, term->c.y + 1, term->width, term->height, 0);
        break;
    case 'K':
        term_adjust_wide_left(term, term->c.x, term->c.y);
        term_erase(term, term->c.x, term->c.y, term->width, term->c.y + 1, 0);
        break;
    case 'V': /* Print cursor line */
        if (term->printerfd < 0) break;
        term_print_line(term, term->screen[term->c.y]);
        break;
    case 'W': /* Enable printer */
        term->mode.print_enabled = 1;
        break;
    case 'X': /* Disable printer */
        term->mode.print_enabled = 0;
        break;
    case 'Y':
        term->esc.state = esc_vt52_cup_0;
        return;
    case 'Z':
        term_answerback(term, ESC"/Z");
        break;
    case ']': /* Print screen */
        term_print_screen(term, term->mode.print_extend);
        break;
    case '^': /* Autoprint on */
        term->mode.print_auto = 1;
        break;
    case '_': /* Autoprint off */
        term->mode.print_auto = 0;
        break;
    default:
        warn("Unrecognized ^[%c", ch);
    }

    term->esc.state = esc_ground;
}

static void term_dispatch_vt52_cup(struct term *term) {
    (term->c.origin ? term_bounded_move_to : term_move_to)
            (term, term_min_ox(term) + term->esc.param[1], term_min_oy(term) + term->esc.param[0]);
    term->esc.state = esc_ground;
}

static _Bool term_dispatch(struct term *term, term_char_t ch) {
    if (term->mode.print_enabled)
        term_print_char(term, ch);

    // C1 controls are interpreted in all states, try them before others
    if (IS_C1(ch) && term->vt_level > 1) {
        term->esc.old_selector = term->esc.selector;
        term->esc.old_state = term->esc.state;
        term->esc.state = esc_esc_entry;
        term->esc.selector = E(ch ^ 0xC0);
        term_dispatch_esc(term);
        return 1;
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
        if (ch == 0x3B)
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
            // Don't encode UTF-8 back, now we operate on bytes
            if (term->esc.str_len + 1 >= term->esc.str_cap) {
                size_t new_cap = STR_CAP_STEP(term->esc.str_cap);
                if (new_cap > ESC_MAX_LONG_STR) break;
                uint8_t *new = realloc(term->esc.str_ptr, new_cap + 1);
                if (!new) break;
                if (!term->esc.str_ptr) memcpy(new, term->esc.str_data, term->esc.str_len);
                term->esc.str_ptr = new;
                term->esc.str_cap = new_cap;
            }

            term_esc_str(term)[term->esc.str_len++] = ch;
            term_esc_str(term)[term->esc.str_len] = '\0';
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
        else if (ch == 0x7F && (!term->mode.enable_nrcs &&
                (glv == cs96_latin_1 || glv == cs94_british))) // TODO Why???
            /* ignore */;
        else  {
            // Decode UTF-8 only in ground state
            if (term->mode.utf8 && ch >= 0xA0) {
                term->fd_start--;
                if (!utf8_decode(&ch, (const uint8_t **)&term->fd_start, term->fd_end)) return 0;
            }

            // TODO Remove this codepath (and term_putchar)
            // Decode nrcs

            // In theory this should be disabled while in UTF-8 mode, but
            // in practive applications use these symbols, so keep translating.
            // But decode only allow only DEC Graph in GL, unless configured otherwise
            if (term->mode.utf8 && !iconf(ICONF_FORCE_UTF8_NRCS))
                ch = nrcs_decode_fast(glv, ch);
            else
                ch = nrcs_decode(glv, term->c.gn[term->c.gr], term->c.upcs, ch, term->mode.enable_nrcs);

            term_putchar(term, ch);

            term->c.gl_ss = term->c.gl; // Reset single shift
        }
    }
    return 1;
}

static ssize_t term_refill(struct term *term) {
    if (term->fd == -1) return -1;

    ssize_t inc, sz = term->fd_end - term->fd_start;

    if (term->fd_start != term->fd_buf) {
        memmove(term->fd_buf, term->fd_start, sz);
        term->fd_end = term->fd_buf + sz;
        term->fd_start = term->fd_buf;
    }

    if ((inc = read(term->fd, term->fd_end, sizeof(term->fd_buf) - sz)) < 0) {
        if (errno != EAGAIN) {
            warn("Can't read from tty");
            term_hang(term);
            return -1;
        }
        inc = 0;
    }

    term->fd_end += inc;
    return inc;
}

void term_read(struct term *term) {
    if (term_refill(term) <= 0) return;

    if (term->mode.scroll_on_output && term->view_pos.line)
        term_reset_view(term, 1);

    while (term->fd_start < term->fd_end) {
        term_char_t ch = *term->fd_start;

        // If we are in ground state try to print all
        // sequential graphical charactes at once
        // TODO Allow fast printing without term->mode.wrap
        if (term->esc.state == esc_ground && term->mode.wrap && ((ch > 0x1F && ch < 0x80) || ch >= 0xA0)) {

            // Count maximal with to be printed at once
            ssize_t buf = 0, totw = 0, maxw = term_max_x(term) - term_min_x(term);
            if (!term->c.pending) {
                if (term->c.x >= term_max_x(term)) maxw = term->width - term->c.x;
                else maxw = term_max_x(term) - term->c.x;
            }

            uint8_t *blk_start = term->fd_start;

            do {
                ch = *term->fd_start;

                // If we have encountered control character, break
                if (ch < 0x20 || (ch > 0x7F && ch < 0xA0)) break;

                uint8_t *char_start = term->fd_start;
                if (term->mode.utf8 && ch >= 0xA0) {
                    if (!utf8_decode(&ch, (const uint8_t **)&term->fd_start, term->fd_end)) {
                        // If we encountered partial UTF-8,
                        // print all we have and return
                        term_putstr(term, buf, totw);
                        return;
                    }
                } else term->fd_start++;

                uint8_t glv = term->c.gn[term->c.gl_ss];

                // Skip DEL char if not 96 set
                if (ch == 0x7F && (glv > cs96_latin_1 || (!term->mode.enable_nrcs &&
                        (glv == cs96_latin_1 || glv == cs94_british)))) continue;

                // Decode nrcs
                // In theory this should be disabled while in UTF-8 mode, but
                // in practive applications use these symbols, so keep translating.
                // But decode only allow only DEC Graph in GL, unless configured otherwise
                if (term->mode.utf8 && !iconf(ICONF_FORCE_UTF8_NRCS))
                    ch = nrcs_decode_fast(glv, ch);
                else
                    ch = nrcs_decode(glv, term->c.gn[term->c.gr], term->c.upcs, ch, term->mode.enable_nrcs);
                term->c.gl_ss = term->c.gl; // Reset single shift

                int16_t wid = wcwidth(ch);
                if(!wid) {
                    // Don't put zero-width charactes
                    // to predecode buffer
                    if (!buf) term_precompose_at_cursor(term, ch);
                    else term->predec_buf[buf - 1] = try_precompose(term->predec_buf[buf - 1], ch);
                } else {
                    // Don't include char if its too wide
                    if (totw + wid > maxw && !(term->c.x == term_max_x(term) - 1 && !totw)) {
                        term->fd_start = char_start;
                        break;
                    }
                    // Wide cells are indicated as
                    // negative in predecode buffer
                    if (wid >= 2) {
                        ch = -ch;
                        totw++;
                    }

                    totw++;
                    term->predec_buf[buf++] = ch;
                }
            } while(totw < maxw && buf < FD_BUF_SIZE && term->fd_start < term->fd_end);

            if (term->mode.print_enabled)
                term_print_string(term, blk_start, term->fd_start - blk_start);

            term_putstr(term, buf, totw);
        } else {
            // Slow path, all controls
            // are processed here
            if (!term_dispatch(term, *term->fd_start++)) break;
        }
    }
}

inline static void tty_write_raw(struct term *term, const uint8_t *buf, ssize_t len) {
    ssize_t res, lim = TTY_MAX_WRITE;
    struct pollfd pfd = {
        .events = POLLIN | POLLOUT,
        .fd = term->fd
    };
    while (len) {
        if (poll(&pfd, 1, -1) < 0 && errno != EINTR) {
            warn("Can't poll tty");
            term_hang(term);
            return;
        }
        if (pfd.revents & POLLOUT) {
            if ((res = write(term->fd, buf, MIN(lim, len))) < 0) {
                warn("Can't write to from tty");
                term_hang(term);
                return;
            }

            if (res < (ssize_t)len) {
                if (len < lim)
                    lim = term_refill(term);
                len -= res;
                buf += res;
            } else break;
        }
        if (pfd.revents & POLLIN)
            lim = term_refill(term);
    }
}

inline static void term_tty_write(struct term *term, const uint8_t *buf, size_t len) {
    if (term->fd == -1) return;

    const uint8_t *next;

    if (!term->mode.crlf)
        tty_write_raw(term, buf, len);
    else while (len) {
        if (*buf == '\r') {
            next = buf + 1;
            tty_write_raw(term, (const uint8_t *)"\r\n", 2);
        } else {
            next = memchr(buf , '\r', len);
            if (!next) next = buf + len;
            tty_write_raw(term, buf, next - buf);
        }
        len -= next - buf;
        buf = next;
    }
}

static size_t term_encode_c1(struct term *term, const uint8_t *in, uint8_t *out) {
    uint8_t *fmtp = out;
    for (uint8_t *it = (uint8_t *)in; *it && fmtp - out < MAX_REPORT - 1; it++) {
        if (IS_C1(*it) && (term->mode.utf8 || !term->mode.eight_bit || term->vt_level < 2)) {
            *fmtp++ = 0x1B;
            *fmtp++ = *it ^ 0xC0;
            // Theoretically we can use C1 encoded as UTF-8 if term->mode.utf8
            // but noone understands that format
            //*fmtp++ = 0xC0 | (*it >> 6);
            //*fmtp++ = 0x80 | (*it & 0x3F);
        } else {
            *fmtp++ = *it;
        }
    }
    *fmtp = 0x00;
    return fmtp - out;
}

void term_answerback(struct term *term, const char *str, ...) {
    static uint8_t fmt[MAX_REPORT], csi[MAX_REPORT];
    va_list vl;
    va_start(vl, str);
    term_encode_c1(term, (const uint8_t *)str, fmt);
    ssize_t res = vsnprintf((char *)csi, sizeof(csi), (char *)fmt, vl);
    va_end(vl);
    term_tty_write(term, csi, res);

    if (iconf(ICONF_TRACE_INPUT)) {
        ssize_t j = MAX_REPORT;
        for (size_t i = res; i; i--) {
            if (IS_C0(csi[i - 1]) || IS_DEL(csi[i - 1]))
                csi[--j] = csi[i - 1] ^ 0x40, csi[--j] = '^';
            else if (IS_C1(csi[i - 1]))
                csi[--j] = csi[i - 1] ^ 0xC0, csi[--j] = '[', csi[--j] = '^';
            else
                csi[--j] = csi[i - 1];
        }
        info("Rep: %s", csi + j);
    }
}

/* If len == 0 encodes C1 controls and determines length by NUL character */
void term_sendkey(struct term *term, const uint8_t *str, size_t len) {
    bool encode = !len;
    if (!len) len = strlen((char *)str);

    const uint8_t *end = len + str, *start = str;
    if (term->mode.echo) {
        while (*start < *end) {
            term_char_t ch = *start;
            // Try to handle unencoded C1 bytes even if UTF-8 is enabled
            if (term->mode.utf8 && ch >= 0xA0 && term->esc.state == esc_ground) {
                if (!utf8_decode(&ch, &start, end)) break;
            } else term->fd_start++;

            if (IS_C1(ch)) {
                term_dispatch(term, '^');
                term_dispatch(term, '[');
                ch ^= 0xC0;
            } else if ((IS_C0(ch) || IS_DEL(ch)) && ch != '\n' && ch != '\t' && ch != '\r') {
                term_dispatch(term, '^');
                ch ^= 0x40;
            }
            term_dispatch(term, ch);
        }
    }

    if (!(term->mode.no_scroll_on_input) && term->view_pos.line)
        term_reset_view(term, 1);

    uint8_t rep[MAX_REPORT];

    if (encode) len = term_encode_c1(term, str, rep);

    term_tty_write(term, encode ? rep : str, len);
}

void term_break(struct term *term) {
    if (tcsendbreak(term->fd, 0))
        warn("Can't send break");
}

void term_toggle_numlock(struct term *term) {
    term->kstate.allow_numlock = !term->kstate.allow_numlock;
}

bool term_is_paste_requested(struct term *term) {
    return term->paste_from;
}

void term_paste_begin(struct term *term) {
    /* If paste_from is not 0 application have requested
     * OSC 52 selection contents reply */
    if (term->paste_from)
        term_answerback(term, OSC"52;%c;", term->paste_from);
    /* Otherwise it's just paste (bracketed or not) */
    else if (term->mode.bracketed_paste)
        term_answerback(term, CSI"200~");
}

void term_paste_end(struct term *term) {
    /* If paste_from is not 0 application have requested
     * OSC 52 selection contents reply
     *
     * Actually there's a race condition
     * if user have requested paste and
     * before data have arrived an application
     * uses OSC 52. But it's really hard to deal with
     *
     * Probably creating the queue of paste requests
     * would be a valid solution
     *
     * But this race isn't that destructive and
     * rather rare to deal with
     */
    if (term->paste_from) {
        term_answerback(term, ST);
        term->paste_from = 0;
    } else if (term->mode.bracketed_paste)
        term_answerback(term, CSI"201~");
}

bool term_is_keep_clipboard_enabled(struct term *term) {
    return term->mode.keep_clipboard;
}

bool term_is_keep_selection_enabled(struct term *term) {
    return term->mode.keep_selection;
}

bool term_is_select_to_clipboard_enabled(struct term *term) {
    return term->mode.select_to_clipboard;
}

bool term_is_utf8_enabled(struct term *term) {
    return term->mode.utf8;
}

bool term_is_paste_nl_enabled(struct term *term) {
    return term->mode.paste_literal_nl;
}

bool term_is_paste_quote_enabled(struct term *term) {
    return term->mode.paste_quote;
}

bool term_is_nrcs_enabled(struct term *term) {
    return term->mode.enable_nrcs;
}

bool term_is_bell_urgent_enabled(struct term *term) {
    return term->mode.bell_urgent;
}

bool term_is_bell_raise_enabled(struct term *term) {
    return term->mode.bell_raise;
}

struct keyboard_state *term_get_kstate(struct term *term) {
    return &term->kstate;
}

struct mouse_state *term_get_mstate(struct term *term) {
    return &term->mstate;
}

struct window *term_window(struct term *term) {
    return term->win;
}

int term_fd(struct term *term) {
    return term->fd;
}

bool term_is_reverse(struct term *term) {
    return term->mode.reverse_video;
}

ssize_t term_view(struct term *term) {
    return term->view;
}

void term_handle_focus(struct term *term, bool set) {
    term->mode.focused = set;
    if (term->mode.track_focus)
        term_answerback(term, set ? CSI"I" : CSI"O");
    term->screen[term->c.y]->cell[term->c.x].attr &= ~attr_drawn;
}

void term_hang(struct term *term) {
    if (term->fd >= 0) {
        close(term->fd);
        if (term->printerfd != STDOUT_FILENO)
            close(term->printerfd);
        term->fd = -1;
    }
    kill(term->child, SIGHUP);
}

void free_term(struct term *term) {
    term_hang(term);

    term_free_scrollback(term);

    for (ssize_t i = 0; i < term->height; i++) {
        term_free_line(term->screen[i]);
        term_free_line(term->back_screen[i]);
    }

    free(term->screen);
    free(term->back_screen);

    free(term->tabs);
    free(term->palette);
    free(term);
}
