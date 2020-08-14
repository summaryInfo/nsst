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

//For openpty() function
#if   defined(__linux)
#   include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
#   include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#   include <libutil.h>
#endif

#define TTY_MAX_WRITE 256
#define NSS_FD_BUF_SZ 4096
#define ESC_MAX_PARAM 32
#define ESC_MAX_STR 256
#define ESC_MAX_LONG_STR 0x10000000
#define ESC_DUMP_MAX 768
#define MAX_REPORT 1024
#define SEL_INIT_SIZE 32
#define SGR_BUFSIZ 64

#define CSI "\x9B"
#define OSC "\x9D"
#define DCS "\x90"
#define ESC "\x1B"
#define ST "\x9C"

#define IS_C1(c) ((uint32_t)(c) - 0x80U < 0x20U)
#define IS_C0(c) ((c) < 0x20)
#define IS_DEL(c) ((c) == 0x7f)
#define IS_STREND(c) (IS_C1(c) || (c) == 0x1b || (c) == 0x1a || (c) == 0x18 || (c) == 0x07)
#define ENABLE_IF(c, m, f) { if (c) { (m) |= (f); } else { (m) &= ~(f); }}

#define TABSR_INIT_CAP 48
#define TABSR_CAP_STEP(x) (4*(x)/3)
#define TABSR_MAX_ENTRY 6

#define MAX_EXTRA_PALETTE (0x10000 - NSS_PALETTE_SIZE)
#define CAPS_INC_STEP(sz) MIN(MAX_EXTRA_PALETTE, (sz) ? 8*(sz)/5 : 4)
#define CBUF_STEP(c,m) ((c) ? MIN(4 * (c) / 3, m) : MIN(16, m))
#define STR_CAP_STEP(x) (4*(x)/3)
#define PARAM(i, d) (term->esc.param[i] > 0 ? (param_t)term->esc.param[i] : (param_t)(d))
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
    _Bool pending;
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

    /* Cyclic buffer for
     * saved lines */
    nss_line_t **scrollback;
    ssize_t view;
    ssize_t sb_top;
    ssize_t sb_limit;
    ssize_t sb_caps;
    ssize_t sb_max_caps;

    nss_cursor_t c;
    nss_cursor_t cs;
    nss_cursor_t back_cs;
    nss_cursor_t vt52c;

    nss_coord_t width;
    nss_coord_t height;
    nss_coord_t top;
    nss_coord_t bottom;
    nss_coord_t left;
    nss_coord_t right;
    _Bool *tabs;

    /* Last written character
     * Used for REP */
    nss_char_t prev_ch;

    /* OSC 52 character description
     * of selection being pasted from */
    uint8_t paste_from;

    /* Previous mouse state
     * used reduce duplicated
     * mouse events */
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

    enum nss_term_mode {
        nss_tm_echo                 = 1LL << 0,
        nss_tm_crlf                 = 1LL << 1,
        nss_tm_132cols              = 1LL << 2,
        nss_tm_wrap                 = 1LL << 3,
        nss_tm_focused              = 1LL << 4,
        nss_tm_altscreen            = 1LL << 5,
        nss_tm_utf8                 = 1LL << 6,
        nss_tm_reverse_video        = 1LL << 7,
        nss_tm_insert               = 1LL << 8,
        nss_tm_sixel                = 1LL << 9,
        nss_tm_8bit                 = 1LL << 10,
        nss_tm_protected            = 1LL << 11,
        nss_tm_disable_altscreen    = 1LL << 12,
        nss_tm_track_focus          = 1LL << 13,
        nss_tm_hide_cursor          = 1LL << 14,
        nss_tm_enable_nrcs          = 1LL << 15,
        nss_tm_132_preserve_display = 1LL << 16,
        nss_tm_scroll_on_output     = 1LL << 17,
        nss_tm_dont_scroll_on_input = 1LL << 18,
        nss_tm_mouse_x10            = 1LL << 19,
        nss_tm_mouse_button         = 1LL << 20,
        nss_tm_mouse_motion         = 1LL << 21,
        nss_tm_mouse_many           = 1LL << 22,
        nss_tm_mouse_format_sgr     = 1LL << 23,
        nss_tm_print_extend         = 1LL << 24,
        nss_tm_print_form_feed      = 1LL << 25,
        nss_tm_print_enabled        = 1LL << 26,
        nss_tm_print_auto           = 1LL << 27,
        nss_tm_title_set_utf8       = 1LL << 28,
        nss_tm_title_query_utf8     = 1LL << 29,
        nss_tm_title_set_hex        = 1LL << 30,
        nss_tm_title_query_hex      = 1LL << 31,
        nss_tm_bracketed_paste      = 1LL << 32,
        nss_tm_keep_selection       = 1LL << 33,
        nss_tm_keep_clipboard       = 1LL << 34,
        nss_tm_select_to_clipboard  = 1LL << 35,
        nss_tm_reverse_wrap         = 1LL << 36,
        nss_tm_led_num_lock         = 1LL << 37,
        nss_tm_led_caps_lock        = 1LL << 38,
        nss_tm_led_scroll_lock      = 1LL << 39,
        nss_tm_alternate_scroll     = 1LL << 40,
        nss_tm_attr_ext_rectangle   = 1LL << 41,
        nss_tm_lr_margins           = 1LL << 42,
        nss_tm_disable_132cols      = 1LL << 43,
        nss_tm_smooth_scroll        = 1LL << 44,
        nss_tm_xterm_more_hack      = 1LL << 45,
        nss_tm_allow_change_font    = 1LL << 46,

        // Need to modify XTCHECKSUM aswell if theses values gets modified
        nss_tm_cksm_positive        = 1LL << 48,
        nss_tm_cksm_no_attr         = 1LL << 49,
        nss_tm_cksm_no_trim         = 1LL << 50,
        nss_tm_cksm_no_implicit     = 1LL << 51,
        nss_tm_cksm_wide            = 1LL << 52,
        nss_tm_cksm_8bit            = 1LL << 53,

        nss_tm_cksm_mask =
            nss_tm_cksm_positive | nss_tm_cksm_no_attr |
            nss_tm_cksm_no_trim | nss_tm_cksm_no_implicit |
            nss_tm_cksm_wide | nss_tm_cksm_8bit,
        nss_tm_mouse_mask =
            nss_tm_mouse_x10 | nss_tm_mouse_button |
            nss_tm_mouse_motion | nss_tm_mouse_many,
        nss_tm_print_mask =
            nss_tm_print_auto | nss_tm_print_enabled
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
        size_t str_cap;
        // Short strings are not allocated
        uint8_t str_data[ESC_MAX_STR + 1];
        // Long strings are
        uint8_t *str_ptr;
    } esc;

    uint16_t vt_version;
    uint16_t vt_level;

    nss_window_t *win;
    nss_input_mode_t inmode;
    nss_color_t *palette;

    pid_t child;
    int fd;
    int printerfd;

    uint8_t *fd_start;
    uint8_t *fd_end;
    uint8_t fd_buf[NSS_FD_BUF_SZ];
};

/* Default termios, initialized from main */
static struct termios dtio;

static void term_answerback(nss_term_t *term, const char *str, ...);
static void term_scroll_selection(nss_term_t *term, nss_coord_t amount, _Bool save);
static void term_change_selection(nss_term_t *term, uint8_t state, nss_coord_t x, nss_color_t y, _Bool rectangular);
static void term_update_selection(nss_term_t *term, uint8_t oldstate, nss_selected_t old);

inline static nss_line_iter_t make_screen_iter(nss_term_t *term, ssize_t ymin, ssize_t ymax) {
    if (ymin > ymax) SWAP(ssize_t, ymin, ymax);
    return (nss_line_iter_t) {
        term->screen,
        term->scrollback,
        term->sb_top,
        term->sb_limit,
        MAX(-term->sb_limit, ymin),
        MIN(term->height, ymax),
        MAX(-term->sb_limit, ymin),
    };
}

inline static void line_iter_inc(nss_line_iter_t *it, ssize_t delta) {
    it->_y += delta;
}

inline static nss_line_t *term_line_at(nss_term_t *term, ssize_t y) {
    return y >= 0 ? term->screen[y] :
        term->scrollback[(term->sb_top + term->sb_limit + y + 1) % term->sb_limit];
}

inline static nss_line_t *line_iter_ref(nss_line_iter_t *it) {
    if (it->_y >= it->_y_max || it->_y < it->_y_min) return NULL;
    return (it->_y >= 0) ? it->_screen[it->_y] :
        it->_scrollback[(it->_sb_0 + it->_y + 1 + it->_sb_limit) % it->_sb_limit];
}

inline static nss_line_t *line_iter_prev(nss_line_iter_t *it) {
    line_iter_inc(it, -1);
    return line_iter_ref(it);
}

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

void nss_setup_default_termios(void) {
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

static int tty_open(nss_term_t *term, const char *cmd, const char **args) {
    int slave, master;
    if (openpty(&master, &slave, NULL, NULL, NULL) < 0) {
        warn("Can't create pseudo terminal");
        term->fd = -1;
        return -1;
    }

    /* Configure PTY */

    struct termios tio = dtio;

    tio.c_cc[VERASE] = nss_config_input_mode().backspace_is_del ? '\177' : '\010';

    /* If IUTF8 is defined, enable it by default,
     * when terminal itself is in UTF-8 mode */
#ifdef IUTF8
    if (nss_config_integer(NSS_ICONFIG_UTF8))
        tio.c_iflag |= IUTF8;
#endif

    tcsetattr(slave, TCSANOW, &tio);

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
        sigaction(SIGCHLD, &(struct sigaction){
                .sa_handler = handle_chld, .sa_flags = SA_RESTART}, NULL);
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
            if (!(new = realloc(buf, line->pal->size * sizeof(nss_cid_t)))) return 0;
            memset(new + buf_len, 0xFF, (line->pal->size - buf_len) * sizeof(nss_cid_t));
            buf_len = line->pal->size, buf = new;
        }
        nss_cid_t k = NSS_PALETTE_SIZE, *pbuf = buf - NSS_PALETTE_SIZE;
        for (nss_coord_t i = 0; i < line->width; i++) {
            nss_cell_t *cel = &line->cell[i];
            if (cel->fg >= NSS_PALETTE_SIZE) pbuf[cel->fg] = 0;
            if (cel->bg >= NSS_PALETTE_SIZE) pbuf[cel->bg] = 0;
        }
        nss_color_t *pal = line->pal->data - NSS_PALETTE_SIZE;
        for (nss_cid_t i = NSS_PALETTE_SIZE; i < line->pal->size + NSS_PALETTE_SIZE; i++) {
            if (!pbuf[i]) {
                pal[k] = pal[i];
                for (nss_cid_t j = i + 1; j < line->pal->size + NSS_PALETTE_SIZE; j++)
                    if (pal[k] == pal[j]) pbuf[j] = k;
                pbuf[i] = k++;
            }
        }
        line->pal->size = k - NSS_PALETTE_SIZE;

        for (nss_coord_t i = 0; i < line->width; i++) {
            nss_cell_t *cel = &line->cell[i];
            if (cel->fg >= NSS_PALETTE_SIZE) cel->fg = pbuf[cel->fg];
            if (cel->bg >= NSS_PALETTE_SIZE) cel->bg = pbuf[cel->bg];
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

    if (!line->pal || line->pal->size + 1 > line->pal->caps) {
        if (!optimize_line_palette(line)) return NSS_SPECIAL_BG;
        if (!line->pal || line->pal->size + 1 >= line->pal->caps) {
            if (line->pal && line->pal->caps == MAX_EXTRA_PALETTE) return NSS_SPECIAL_BG;
            size_t newc = CAPS_INC_STEP(line->pal ? line->pal->caps : 0);
            nss_line_palette_t *new = realloc(line->pal, sizeof(nss_line_palette_t) + newc * sizeof(nss_color_t));
            if (!new) return NSS_SPECIAL_BG;
            if (!line->pal) new->size = 0;
            new->caps = newc;
            line->pal = new;
        }
    }

    line->pal->data[line->pal->size++] = col;
    return NSS_PALETTE_SIZE + line->pal->size - 1;
}

inline static nss_cell_t fixup_color(nss_line_t *line, nss_cursor_t *cur) {
    nss_cell_t cel = cur->cel;
    if (__builtin_expect(cel.bg >= NSS_PALETTE_SIZE, 0))
        cel.bg = alloc_color(line, cur->bg);
    if (__builtin_expect(cel.fg >= NSS_PALETTE_SIZE, 0))
        cel.fg = alloc_color(line, cur->fg);
    return cel;
}

static nss_line_t *term_create_line(nss_term_t *term, nss_coord_t width) {
    nss_line_t *line = malloc(sizeof(*line) + (size_t)width * sizeof(line->cell[0]));
    if (line) {
        line->width = width;
        line->wrap_at = 0;
        line->pal = NULL;
        line->force_damage = 0;
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

        for (nss_coord_t i = new->width; i < width; i++)
            new->cell[i] = cell;
    }

    new->width = width;
    return new;
}

static void term_free_line(nss_line_t *line) {
    free(line->pal);
    free(line);
}

inline static void term_put_cell(nss_term_t *term, nss_coord_t x, nss_coord_t y, nss_char_t ch) {
    term->screen[y]->cell[x] = MKCELLWITH(fixup_color(term->screen[y], &term->c), ch);
}

_Bool nss_term_is_cursor_enabled(nss_term_t *term) {
    return !(term->mode & nss_tm_hide_cursor) && !term->view;
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

void term_damage(nss_term_t *term, nss_rect_t damage) {
    if (intersect_with(&damage, &(nss_rect_t) {0, 0, term->width, term->height})) {
        nss_line_iter_t it = make_screen_iter(term, damage.y - term->view, damage.y + damage.height - term->view);
        for (nss_line_t *line; (line = line_iter_next(&it));)
            for (nss_coord_t j = damage.x; j <  MIN(damage.x + damage.width, line->width); j++)
                line->cell[j].attr &= ~nss_attrib_drawn;
    }
}

void nss_term_damage_lines(nss_term_t *term, nss_coord_t ys, nss_coord_t yd) {
    nss_line_iter_t it = make_screen_iter(term, ys - term->view, yd - term->view);
    for (nss_line_t *line; (line = line_iter_next(&it));) line->force_damage = 1;
}

_Bool nss_term_redraw_dirty(nss_term_t *term) {
    _Bool c_hidden = (term->mode & nss_tm_hide_cursor) || term->view;

    if (term->c.x != term->prev_c_x || term->c.y != term->prev_c_y ||
            term->prev_c_hidden != c_hidden || term->prev_c_view_changed) {
        if (!c_hidden) term->screen[term->c.y]->cell[term->c.x].attr &= ~nss_attrib_drawn;
        if ((!term->prev_c_hidden || term->prev_c_view_changed) && term->prev_c_y < term->height && term->prev_c_x < term->width)
            term->screen[term->prev_c_y]->cell[term->prev_c_x].attr &= ~nss_attrib_drawn;
    }

    //TODO Fix margin cursor

    term->prev_c_x = term->c.x;
    term->prev_c_y = term->c.y;
    term->prev_c_hidden = c_hidden;
    term->prev_c_view_changed = 0;

    _Bool cursor = !term->prev_c_hidden &&
            !(term->screen[term->prev_c_y]->cell[term->prev_c_x].attr & nss_attrib_drawn);

    nss_line_iter_t it = make_screen_iter(term, -term->view, term->height - term->view);

    return nss_window_submit_screen(term->win, &it, term->palette, term->c.x, term->c.y, cursor, term->c.pending);
}

static void term_reset_view(nss_term_t *term, _Bool damage) {
    term->prev_c_view_changed |= !!term->view;
    ssize_t old_view = term->view;
    term->view = 0;
    if (term->vsel.state == nss_sstate_progress)
        term_change_selection(term, nss_sstate_progress, term->vsel.r.x1, term->vsel.r.y1 + old_view, term->vsel.r.rect);
    if (damage) nss_term_damage_lines(term, 0, term->height);
}

void nss_term_scroll_view(nss_term_t *term, nss_coord_t amount) {
    if (term->mode & nss_tm_altscreen) {
        if (term->mode & nss_tm_alternate_scroll)
            term_answerback(term, CSI"%d%c", abs(amount), amount > 0 ? 'A' : 'D');
        return;
    }

    ssize_t old_view = term->view;

    term->view = MAX(MIN(term->view + amount, term->sb_limit), 0);

    ssize_t delta = term->view - old_view;
    if (delta > 0) { //Up
        nss_term_damage_lines(term, 0, delta);
        nss_window_shift(term->win, 0, delta, term->height - delta, 0);
    } else if (delta < 0) { // Down
        nss_term_damage_lines(term, term->height + delta, term->height);
        nss_window_shift(term->win, -delta, 0, term->height + delta, 0);
    }

    if (term->vsel.state == nss_sstate_progress)
        term_change_selection(term, nss_sstate_progress,
                term->vsel.r.x1, term->vsel.r.y1 + old_view, term->vsel.r.rect);
    term->prev_c_view_changed |= !old_view ^ !term->view;
}

inline static nss_coord_t line_length(nss_line_t *line) {
    nss_coord_t max_x = line->width;
    if (!line->wrap_at)
        while (max_x > 0 && !line->cell[max_x - 1].ch) max_x--;
    else max_x = line->wrap_at;
    return max_x;
}

static void term_append_history(nss_term_t *term, nss_line_t *line) {
    if (term->sb_max_caps > 0) {
        /* Minimize line size to save memory */
        line = term_realloc_line(term, line, line_length(line));
        if (line->pal) {
            optimize_line_palette(line);
            nss_line_palette_t *pal = realloc(line->pal, sizeof(nss_line_palette_t) + sizeof(nss_color_t)*(line->pal->size));
            if (pal) {
                line->pal = pal;
                pal->caps = pal->size;
            }
        }

        if (term->sb_limit == term->sb_max_caps) {
            /* If view points to the line that is to be freed, scroll it down */
            if (term->view == term->sb_limit) nss_term_scroll_view(term, -1);

            /* We reached maximal number of saved lines,
             * now term->scrollback functions as true cyclic buffer */
            term->sb_top = (term->sb_limit + term->sb_top + 1) % term->sb_limit;
            SWAP(nss_line_t *, line, term->scrollback[term->sb_top]);
            term_free_line(line);
        } else {
            /* More lines can be saved, term->scrollback is not cyclic yet */

            /* Adjust capacity as needed */
            if (term->sb_limit + 1 >= term->sb_caps) {
                ssize_t new_cap = CBUF_STEP(term->sb_caps, term->sb_max_caps);
                nss_line_t **new = realloc(term->scrollback, new_cap * sizeof(*new));
                if (!new) {
                    term_free_line(line);
                    return;
                }
                term->sb_caps = new_cap;
                term->scrollback = new;
            }

            /* And just save line */
            term->sb_top = term->sb_limit++;
            term->scrollback[term->sb_top] = line;
        }

        if (term->view) term->view++;
    } else term_free_line(line);
}

/* Get min/max column/row */
inline static nss_coord_t term_max_y(nss_term_t *term) {
    return term->bottom + 1;
}
inline static nss_coord_t term_min_y(nss_term_t *term) {
    return term->top;
}
inline static nss_coord_t term_max_x(nss_term_t *term) {
    return term->mode & nss_tm_lr_margins ? term->right + 1 : term->width;
}
inline static nss_coord_t term_min_x(nss_term_t *term) {
    return term->mode & nss_tm_lr_margins ? term->left : 0;
}

/* Get min/max column/row WRT origin */
inline static nss_coord_t term_max_ox(nss_term_t *term) {
    return (term->mode & nss_tm_lr_margins && term->c.origin) ? term->right + 1: term->width;
}
inline static nss_coord_t term_min_ox(nss_term_t *term) {
    return (term->mode & nss_tm_lr_margins && term->c.origin) ? term->left : 0;
}
inline static nss_coord_t term_max_oy(nss_term_t *term) {
    return term->c.origin ? term->bottom + 1: term->height;
}
inline static nss_coord_t term_min_oy(nss_term_t *term) {
    return term->c.origin ? term->top : 0;
}

inline static void term_esc_start(nss_term_t *term) {
    term->esc.selector = 0;
}
inline static void term_esc_start_seq(nss_term_t *term) {
    for (size_t i = 0; i <= term->esc.i; i++)
        term->esc.param[i] = -1;
    term->esc.i = 0;
    term->esc.subpar_mask = 0;
    term->esc.selector = 0;
}
inline static void term_esc_start_string(nss_term_t *term) {
    term->esc.si = 0;
    term->esc.str_data[0] = 0;
    term->esc.str_cap = ESC_MAX_STR;
    term->esc.selector = 0;
}
inline static void term_esc_finish_string(nss_term_t *term) {
    free(term->esc.str_ptr);
    term->esc.str_ptr = NULL;
}

static void term_esc_dump(nss_term_t *term, _Bool use_info) {
    if (use_info && nss_config_integer(NSS_ICONFIG_LOG_LEVEL) < 3) return;

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
                pos += snprintf(buf + pos, ESC_DUMP_MAX - pos, "%"PRId32, term->esc.param[i]);
                if (i < term->esc.i - 1) buf[pos++] = term->esc.subpar_mask & (1 << (i + 1)) ? ':' : ';' ;
            }
            if (term->esc.selector & I0_MASK)
                buf[pos++] = 0x20 + ((term->esc.selector & I0_MASK) >> 9) - 1;
            if (term->esc.selector & I1_MASK)
                buf[pos++] = 0x20 + ((term->esc.selector & I1_MASK) >> 14) - 1;
            buf[pos++] = (C_MASK & term->esc.selector) + 0x40;
            if (term->esc.state != esc_dcs_string) break;

            buf[pos] = 0;
            (use_info ? info : warn)("%s%s^[\\", buf, term->esc.str_ptr ? term->esc.str_ptr : term->esc.str_data);
            return;
        case esc_osc_string:
            (use_info ? info : warn)("^[]%u;%s^[\\", term->esc.selector, term->esc.str_ptr ? term->esc.str_ptr : term->esc.str_data);
        default:
            return;
    }
    buf[pos] = 0;
    (use_info ? info : warn)("%s", buf);
}

static size_t term_decode_color(nss_term_t *term, size_t arg, nss_color_t *rcol, nss_cid_t *rcid, nss_cid_t *valid) {
    *valid = 0;
    size_t argc = arg + 1 < ESC_MAX_PARAM;
    _Bool subpars = arg && (term->esc.subpar_mask >> (arg + 1)) & 1;
    for (size_t i = arg + 1; i < ESC_MAX_PARAM &&
        !subpars ^ ((term->esc.subpar_mask >> i) & 1); i++) argc++;
    if (argc > 0) {
        if (term->esc.param[arg] == 2 && argc > 3) {
            nss_color_t col = 0xFF;
            _Bool wrong = 0, space = subpars && argc > 4;
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
            if (term->esc.param[arg + 1] < NSS_PALETTE_SIZE - NSS_SPECIAL_COLORS && term->esc.param[arg + 1] >= 0) {
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

static void term_decode_sgr(nss_term_t *term, size_t i, nss_cell_t *mask, nss_cell_t *val, nss_color_t *fg, nss_color_t *bg) {
#define SET(f) (mask->attr |= (f), val->attr |= (f))
#define RESET(f) (mask->attr |= (f), val->attr &= ~(f))
#define SETFG(f) (mask->fg = 1, val->fg = (f))
#define SETBG(f) (mask->bg = 1, val->bg = (f))
    do {
        param_t par = PARAM(i, 0);
        if ((term->esc.subpar_mask >> i) & 1) return;
        switch (par) {
        case 0:
            RESET(0xFF);
            SETFG(NSS_SPECIAL_FG);
            SETBG(NSS_SPECIAL_BG);
            break;
        case 1:  SET(nss_attrib_bold); break;
        case 2:  SET(nss_attrib_faint); break;
        case 3:  SET(nss_attrib_italic); break;
        case 21: /* <- should be double underlind */
        case 4:
            if (i < term->esc.i && (term->esc.subpar_mask >> (i + 1)) & 1 &&
                term->esc.param[++i] <= 0) RESET(nss_attrib_underlined);
            else SET(nss_attrib_underlined);
            break;
        case 5:  /* <- should be slow blink */
        case 6:  SET(nss_attrib_blink); break;
        case 7:  SET(nss_attrib_inverse); break;
        case 8:  SET(nss_attrib_invisible); break;
        case 9:  SET(nss_attrib_strikethrough); break;
        case 22: RESET(nss_attrib_faint | nss_attrib_bold); break;
        case 23: RESET(nss_attrib_italic); break;
        case 24: RESET(nss_attrib_underlined); break;
        case 25: /* <- should be slow blink reset */
        case 26: RESET(nss_attrib_blink); break;
        case 27: RESET(nss_attrib_inverse); break;
        case 28: RESET(nss_attrib_invisible); break;
        case 29: RESET(nss_attrib_strikethrough); break;
        case 30: case 31: case 32: case 33:
        case 34: case 35: case 36: case 37:
            SETFG(par - 30); break;
        case 38:
            i += term_decode_color(term, i + 1, fg, &val->fg, &mask->fg);
            break;
        case 39: SETFG(NSS_SPECIAL_FG); break;
        case 40: case 41: case 42: case 43:
        case 44: case 45: case 46: case 47:
            SETBG(par - 40); break;
        case 48:
            i += term_decode_color(term, i + 1, bg, &val->bg, &mask->bg);
            break;
        case 49: SETBG(NSS_SPECIAL_BG); break;
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


inline static void term_rect_pre(nss_term_t *term, nss_coord_t *xs, nss_coord_t *ys, nss_coord_t *xe, nss_coord_t *ye) {
    *xs = MAX(term_min_oy(term), MIN(*xs, term_max_ox(term) - 1));
    *xe = MAX(term_min_oy(term), MIN(*xe, term_max_ox(term)));
    *ys = MAX(term_min_oy(term), MIN(*ys, term_max_oy(term) - 1));
    *ye = MAX(term_min_oy(term), MIN(*ye, term_max_oy(term)));
}

inline static void term_erase_pre(nss_term_t *term, nss_coord_t *xs, nss_coord_t *ys, nss_coord_t *xe, nss_coord_t *ye, _Bool origin) {
    if (origin) term_rect_pre(term, xs, ys, xe, ye);
    else {
        *xs = MAX(0, MIN(*xs, term->width - 1));
        *xe = MAX(0, MIN(*xe, term->width));
        *ys = MAX(0, MIN(*ys, term->height - 1));
        *ye = MAX(0, MIN(*ye, term->height));
    }

    nss_window_delay(term->win);

    if (term->vsel.state == nss_sstate_none) return;

#define RECT_INTRS(x10, x11, y10, y11) \
    ((MAX(*xs, x10) <= MIN(*xe - 1, x11)) && (MAX(*ys, y10) <= MIN(*ye - 1, y11)))

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

static uint16_t term_checksum(nss_term_t *term, nss_coord_t xs, nss_coord_t ys, nss_coord_t xe, nss_coord_t ye) {
    term_rect_pre(term, &xs, &ys, &xe, &ye);

    // TODO Test this thing

    uint32_t res = 0, spc = 0, trm = 0;
    enum nss_char_set gr = term->c.gn[term->c.gr];
    _Bool first = 1, notrim = term->mode & nss_tm_cksm_no_trim;

    for (; ys < ye; ys++) {
        nss_line_t *line = term->screen[ys];
        for (nss_coord_t i = xs; i < xe; i++) {
            nss_char_t ch = line->cell[i].ch;
            uint32_t attr = line->cell[i].attr;
            if (!(term->mode & nss_tm_cksm_no_implicit) && !ch) ch = ' ';

            if (!(term->mode & nss_tm_cksm_wide)) {
                if (ch > 0x7F && gr != nss_94cs_ascii) {
                    nrcs_encode(gr, &ch, term->mode & nss_tm_enable_nrcs);
                    if (!(term->mode & nss_tm_cksm_8bit) && ch < 0x80) ch |= 0x80;
                }
                ch &= 0xFF;
            }
            if (!(term->mode & nss_tm_cksm_no_attr)) {
                if (attr & nss_attrib_underlined) ch += 0x10;
                if (attr & nss_attrib_inverse) ch += 0x20;
                if (attr & nss_attrib_blink) ch += 0x40;
                if (attr & nss_attrib_bold) ch += 0x80;
                if (attr & nss_attrib_italic) ch += 0x100;
                if (attr & nss_attrib_faint) ch += 0x200;
                if (attr & nss_attrib_strikethrough) ch += 0x400;
                if (attr & nss_attrib_invisible) ch += 0x800;
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
    return term->mode & nss_tm_cksm_positive ? res : -res;
}

static void term_reverse_sgr(nss_term_t *term, nss_coord_t xs, nss_coord_t ys, nss_coord_t xe, nss_coord_t ye) {
    term_erase_pre(term, &xs, &ys, &xe, &ye, 1);
    nss_color_t fg = 0, bg = 0;
    nss_cell_t mask = {0}, val = {0};
    term_decode_sgr(term, 4, &mask, &val, &fg, &bg);

    _Bool rect = term->mode & nss_tm_attr_ext_rectangle;
    for (; ys < ye; ys++) {
        nss_line_t *line = term->screen[ys];
        for (nss_coord_t i = xs; i < (rect || ys == ye - 1 ? xe : term_max_ox(term)); i++) {
            line->cell[i].attr ^= mask.attr;
            line->cell[i].attr &= ~nss_attrib_drawn;
        }
        if (!rect) xs = term_min_ox(term);
    }
}

static void term_encode_sgr(char *dst, char *end, nss_cell_t cel, nss_color_t fg, nss_color_t bg) {
    // Maximal sequence is 0;1;2;3;4;6;7;8;9;38:2:255:255:255;48:2:255:255:255
    // 64 byte buffer is enough
#define FMT(...) dst += snprintf(dst, end - dst, __VA_ARGS__)
    // Reset everything
    FMT("0");

    // Encode attributes
    if (cel.attr & nss_attrib_bold) FMT(";1");
    if (cel.attr & nss_attrib_faint) FMT(";2");
    if (cel.attr & nss_attrib_italic) FMT(";3");
    if (cel.attr & nss_attrib_underlined) FMT(";4");
    if (cel.attr & nss_attrib_blink) FMT(";6");
    if (cel.attr & nss_attrib_inverse) FMT(";7");
    if (cel.attr & nss_attrib_invisible) FMT(";8");
    if (cel.attr & nss_attrib_strikethrough) FMT(";9");

    // Encode foreground color
    if (cel.fg < 8) FMT(";%d", 30 + cel.fg);
    else if (cel.fg < 16) FMT(";%d", 90 + cel.fg - 8);
    else if (cel.fg < NSS_PALETTE_SIZE - NSS_SPECIAL_COLORS) FMT(";38:5:%d", cel.fg);
    else if (cel.fg == NSS_SPECIAL_FG) /* FMT(";39") -- default, skip */;
    else FMT(";38:2:%d:%d:%d", fg >> 16 & 0xFF, fg >>  8 & 0xFF, fg >>  0 & 0xFF);

    // Encode background color
    if (cel.bg < 8) FMT(";%d", 40 + cel.bg);
    else if (cel.bg < 16) FMT(";%d", 100 + cel.bg - 8);
    else if (cel.bg < NSS_PALETTE_SIZE - NSS_SPECIAL_COLORS) FMT(";48:5:%d", cel.bg);
    else if (cel.bg == NSS_SPECIAL_BG) /* FMT(";49") -- default, skip */;
    else FMT(";48:2:%d:%d:%d", bg >> 16 & 0xFF, bg >>  8 & 0xFF, bg >>  0 & 0xFF);
#undef FMT
}

static void term_report_sgr(nss_term_t *term, nss_coord_t xs, nss_coord_t ys, nss_coord_t xe, nss_coord_t ye) {
    term_rect_pre(term, &xs, &ys, &xe, &ye);

    if (ys >= ye || xs >= xe) {
        // Invalid rectangle
        term_answerback(term, CSI"0m");
        return;
    }

    nss_cell_t common = term->screen[ys]->cell[xs];
    nss_color_t common_fg = 0, common_bg = 0;
    _Bool has_common_fg = 1, has_common_bg = 1;
    _Bool true_fg = 0, true_bg = 0;

    if (common.fg >= NSS_PALETTE_SIZE && term->screen[ys]->pal)
        common_fg = term->screen[ys]->pal->data[common.fg - NSS_PALETTE_SIZE], true_fg = 1;
    if (common.bg >= NSS_PALETTE_SIZE && term->screen[ys]->pal)
        common_bg = term->screen[ys]->pal->data[common.bg - NSS_PALETTE_SIZE], true_bg = 1;

    for (; ys < ye; ys++) {
        nss_line_t *line = term->screen[ys];
        for (nss_coord_t i = xs; i < xe; i++) {
            has_common_fg &= (common.fg == line->cell[i].fg || (true_fg && line->cell[i].fg >= NSS_PALETTE_SIZE &&
                    common_fg == line->pal->data[line->cell[i].fg - NSS_PALETTE_SIZE]));
            has_common_bg &= (common.bg == line->cell[i].bg || (true_bg && line->cell[i].bg >= NSS_PALETTE_SIZE &&
                    common_bg == line->pal->data[line->cell[i].bg - NSS_PALETTE_SIZE]));
            common.attr &= line->cell[i].attr;
        }
    }

    if (!has_common_bg) common.bg = NSS_SPECIAL_BG;
    if (!has_common_fg) common.fg = NSS_SPECIAL_FG;

    char sgr[SGR_BUFSIZ];
    term_encode_sgr(sgr, sgr + sizeof sgr, common, common_fg, common_bg);
    term_answerback(term, CSI"%sm", sgr);
}

static void term_apply_sgr(nss_term_t *term, nss_coord_t xs, nss_coord_t ys, nss_coord_t xe, nss_coord_t ye) {
    term_erase_pre(term, &xs, &ys, &xe, &ye, 1);
    nss_color_t fg = 0, bg = 0;
    nss_cell_t mask = {0}, val = {0};
    term_decode_sgr(term, 4, &mask, &val, &fg, &bg);

    mask.attr |= nss_attrib_drawn;
    val.attr &= ~nss_attrib_drawn;
    _Bool rect = term->mode & nss_tm_attr_ext_rectangle;
    for (; ys < ye; ys++) {
        nss_line_t *line = term->screen[ys];
        if (val.fg >= NSS_PALETTE_SIZE) val.fg = alloc_color(line, fg);
        if (val.bg >= NSS_PALETTE_SIZE) val.bg = alloc_color(line, bg);
        for (nss_coord_t i = xs; i < (rect || ys == ye - 1 ? xe : term_max_ox(term)); i++) {
            nss_cell_t *cel = &line->cell[i];
            cel->attr = (cel->attr & ~mask.attr) | (val.attr & mask.attr);
            if (mask.fg) cel->fg = val.fg;
            if (mask.bg) cel->bg = val.bg;
        }
        if (!rect) xs = term_min_ox(term);
    }
}

static void term_copy(nss_term_t *term, nss_coord_t xs, nss_coord_t ys, nss_coord_t xe, nss_coord_t ye, nss_coord_t xd, nss_coord_t yd, _Bool origin) {
    if (ye < ys) SWAP(nss_coord_t, ye, ys);
    if (xe < xs) SWAP(nss_coord_t, xe, xs);

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

    if (yd < ys || (yd == ys && xd < xs)) {
        for (; ys < ye; ys++, yd++) {
            nss_line_t *sl = term->screen[ys], *dl = term->screen[yd];
            for (nss_coord_t x1 = xs, x2 = xd; x1 < xe; x1++, x2++) {
                nss_cell_t cel = sl->cell[x1];
                cel.attr &= ~nss_attrib_drawn;
                if (cel.fg >= NSS_PALETTE_SIZE)
                    cel.fg = alloc_color(dl, sl->pal->data[cel.fg - NSS_PALETTE_SIZE]);
                if (cel.bg >= NSS_PALETTE_SIZE)
                    cel.bg = alloc_color(dl, sl->pal->data[cel.bg - NSS_PALETTE_SIZE]);
                dl->cell[x2] = cel;
            }
        }
    } else {
        for (yd += ye - ys, xd += xe - xs; ys < ye; ye--, yd--) {
            nss_line_t *sl = term->screen[ye - 1], *dl = term->screen[yd - 1];
            for (nss_coord_t x1 = xe, x2 = xd; x1 > xs; x1--, x2--) {
                nss_cell_t cel = sl->cell[x1 - 1];
                cel.attr &= ~nss_attrib_drawn;
                if (cel.fg >= NSS_PALETTE_SIZE)
                    cel.fg = alloc_color(dl, sl->pal->data[cel.fg - NSS_PALETTE_SIZE]);
                if (cel.bg >= NSS_PALETTE_SIZE)
                    cel.bg = alloc_color(dl, sl->pal->data[cel.bg - NSS_PALETTE_SIZE]);
                dl->cell[x2 - 1] = cel;
            }
        }
    }
}

static void term_fill(nss_term_t *term, nss_coord_t xs, nss_coord_t ys, nss_coord_t xe, nss_coord_t ye, _Bool origin, nss_char_t ch) {
    term_erase_pre(term, &xs, &ys, &xe, &ye, origin);

    for (; ys < ye; ys++) {
        nss_line_t *line = term->screen[ys];
        nss_cell_t cell = fixup_color(line, &term->c);
        cell.ch = ch;
        cell.attr = 0;
        for (nss_coord_t i = xs; i < xe; i++)
            line->cell[i] = cell;
    }
}

static void term_erase(nss_term_t *term, nss_coord_t xs, nss_coord_t ys, nss_coord_t xe, nss_coord_t ye, _Bool origin) {
    term_fill(term, xs, ys, xe, ye, origin, 0);
}

static void term_protective_erase(nss_term_t *term, nss_coord_t xs, nss_coord_t ys, nss_coord_t xe, nss_coord_t ye, _Bool origin) {
    term_erase_pre(term, &xs, &ys, &xe, &ye, origin);

    for (; ys < ye; ys++) {
        nss_line_t *line = term->screen[ys];
        nss_cell_t cell = fixup_color(line, &term->c);
        cell.attr = 0;
        for (nss_coord_t i = xs; i < xe; i++)
            if (!(line->cell[i].attr & nss_attrib_protected))
                line->cell[i] = cell;
    }
}

static void term_selective_erase(nss_term_t *term, nss_coord_t xs, nss_coord_t ys, nss_coord_t xe, nss_coord_t ye, _Bool origin) {
    term_erase_pre(term, &xs, &ys, &xe, &ye, origin);

    for (; ys < ye; ys++) {
        nss_line_t *line = term->screen[ys];
        for (nss_coord_t i = xs; i < xe; i++)
            if (!(line->cell[i].attr & nss_attrib_protected))
                line->cell[i] = MKCELLWITH(line->cell[i], 0);
    }
}


inline static void term_adjust_wide_left(nss_term_t *term, nss_coord_t x, nss_coord_t y) {
    if (x < 1) return;
    nss_cell_t *cell = &term->screen[y]->cell[x - 1];
    if (cell->attr & nss_attrib_wide) {
        cell->attr &= ~(nss_attrib_wide|nss_attrib_drawn);
        cell->ch = 0;
    }
}

inline static void term_adjust_wide_right(nss_term_t *term, nss_coord_t x, nss_coord_t y) {
    if (x >= term->screen[y]->width - 1) return;
    nss_cell_t *cell = &term->screen[y]->cell[x + 1];
    if (cell[-1].attr & nss_attrib_wide) {
        cell->attr &= ~nss_attrib_drawn;
        cell->ch = 0;
    }
}

inline static void term_reset_pending(nss_term_t *term) {
    term->c.pending = 0;
}

static void term_move_to(nss_term_t *term, nss_coord_t x, nss_coord_t y) {
    term->c.x = MIN(MAX(x, 0), term->width - 1);
    term->c.y = MIN(MAX(y, 0), term->height - 1);
    term_reset_pending(term);
}


static void term_bounded_move_to(nss_term_t *term, nss_coord_t x, nss_coord_t y) {
    term->c.x = MIN(MAX(x, term_min_x(term)), term_max_x(term) - 1);
    term->c.y = MIN(MAX(y, term_min_y(term)), term_max_y(term) - 1);
    term_reset_pending(term);
}

static void term_move_left(nss_term_t *term, nss_coord_t amount) {
    nss_coord_t x = term->c.x, y = term->c.y,
        first_left = x < term_min_x(term) ? 0 : term_min_x(term);


    // This is a hack that allows using proper line editing with reverse wrap
    // mode while staying compatible with VT100 wrapping mode
    if (term->mode & nss_tm_reverse_wrap) x += term->c.pending;

    if (amount > x - first_left && (term->mode & nss_tm_wrap) && (term->mode & nss_tm_reverse_wrap)) {
        _Bool in_tbm = term_min_y(term) <= term->c.y && term->c.y < term_max_y(term);
        nss_coord_t height = in_tbm ? term_max_y(term) - term_min_y(term) : term->height;
        nss_coord_t top = in_tbm ? term_min_y(term) : 0;

        amount -= x - first_left;
        x = term_max_x(term);
        y -= 1 + amount/(term_max_x(term) - term_min_x(term));
        amount %= term_max_x(term) - term_min_x(term);

        y = (y - top) % height + top;
        if (y < top) y += height;
    }

    (term->c.x >= term_min_x(term) ? term_bounded_move_to : term_move_to)(term, x - amount, y);
}

static void term_cursor_mode(nss_term_t *term, _Bool mode) {
    if (mode) /* save */ {
        term->cs = term->c;
    } else /* restore */ {
        term->c = term->cs;
        term->c.x = MIN(term->c.x, term->width - 1);
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

static void term_scroll_horizontal(nss_term_t *term, nss_coord_t left, nss_coord_t amount) {
    nss_coord_t top = term_min_y(term), right = term_max_x(term), bottom = term_max_y(term);

    /* Don't need to touch cursor unless copying window content
    if (term->prev_c_y >= 0 && top <= term->prev_c_y && term->prev_c_y < bottom &&
        left <= term->prev_c_y && term->prev_c_x < right) {
        term->screen[term->prev_c_y]->cell[term->prev_c_x].attr &= ~nss_attrib_drawn;
        if (amount >= 0) term->prev_c_x = MAX(0, term->prev_c_x - amount);
        else term->prev_c_x = MIN(term->width - 1, term->prev_c_x - amount);
    }
    */

    for (nss_coord_t i = top; i < bottom; i++) {
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

    //TODO Optimize: shift window contents
}

static void term_scroll(nss_term_t *term, nss_coord_t top, nss_coord_t amount, _Bool save) {
    nss_coord_t left = term_min_x(term), right = term_max_x(term), bottom = term_max_y(term);

    if (left == 0 && right == term->width) { // Fast scrolling without margins
        if (top <= term->prev_c_y && term->prev_c_y < bottom) {
            term->screen[term->prev_c_y]->cell[term->prev_c_x].attr &= ~nss_attrib_drawn;
            if (amount >= 0) term->prev_c_y = MAX(top, term->prev_c_y - amount);
            else term->prev_c_y = MIN(bottom, term->prev_c_y - amount);
        }

        if (amount > 0) { /* up */
            amount = MIN(amount, (bottom - top));
            nss_coord_t rest = (bottom - top) - amount;

            if (save && !(term->mode & nss_tm_altscreen) && term->top == top) {
                for (nss_coord_t i = 0; i < amount; i++) {
                    term_append_history(term, term->screen[top + i]);
                    term->screen[top + i] = term_create_line(term, term->width);
                }
            } else term_erase(term, 0, top, term->width, top + amount, 0);

            for (nss_coord_t i = 0; i < rest; i++)
                SWAP(nss_line_t *, term->screen[top + i], term->screen[top + amount + i]);

            if (term->view || !nss_window_shift(term->win, top + amount, top, bottom - top - amount, 1))
                nss_term_damage_lines(term, term->view + top, term->view + bottom - amount);
        } else { /* down */
            amount = MAX(amount, -(bottom - top));
            nss_coord_t rest = (bottom - top) + amount;

            term_erase(term, 0, bottom + amount, term->width, bottom, 0);

            for (nss_coord_t i = 1; i <= rest; i++)
                SWAP(nss_line_t *, term->screen[bottom - i], term->screen[bottom + amount - i]);

            if (term->view || !nss_window_shift(term->win, top, top - amount, bottom - top + amount, 1))
                nss_term_damage_lines(term, term->view + top - amount, term->view + bottom);
        }
    } else { // Slow scrolling with margins
        for (nss_coord_t i = top; i < bottom; i++) {
            term_adjust_wide_left(term, left, i);
            term_adjust_wide_right(term, right - 1, i);
        }

        if (amount > 0) { /* up */
            amount = MIN(amount, bottom - top);

            if (save && !(term->mode & nss_tm_altscreen) && term->top == top) {
                for (nss_coord_t i = 0; i < amount; i++) {
                    nss_line_t *ln = term_create_line(term, term_max_x(term));
                    for (ssize_t k = term_min_x(term); k < term_max_x(term); k++) {
                        nss_cell_t cel = term->screen[top + i]->cell[k];
                        if (cel.fg >= NSS_PALETTE_SIZE) cel.fg = alloc_color(ln,
                                term->screen[top + i]->pal->data[cel.fg - NSS_PALETTE_SIZE]);
                        if (cel.bg >= NSS_PALETTE_SIZE) cel.bg = alloc_color(ln,
                                term->screen[top + i]->pal->data[cel.bg - NSS_PALETTE_SIZE]);
                        ln->cell[k] = cel;
                    }
                    term_append_history(term, ln);
                }
            }

            term_copy(term, left, top + amount, right, bottom, left, top, 0);
            term_erase(term, left, bottom - amount, right, bottom, 0);
        } else { /* down */
            amount = MIN(-amount, bottom - top);
            term_copy(term, left, top, right, bottom - amount, left, top + amount, 0);
            term_erase(term, left, top, right, top + amount, 0);
        }

        //TODO Optimize: shift window contents
    }
    term_scroll_selection(term, amount, save);
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

static void term_set_lr_margins(nss_term_t *term, nss_coord_t left, nss_coord_t right) {
    if (left < right) {
        term->left = MAX(0, MIN(term->width - 1, left));
        term->right = MAX(0, MIN(term->width - 1, right));
    } else {
        term->left = 0;
        term->right = term->width - 1;
    }
}

static void term_reset_margins(nss_term_t *term) {
    term->top = 0;
    term->left = 0;
    term->bottom = term->height - 1;
    term->right = term->width - 1;
}

inline static _Bool term_cursor_in_region(nss_term_t *term) {
    return term->c.x >= term_min_x(term) && term->c.x < term_max_x(term) &&
            term->c.y >= term_min_y(term) && term->c.y < term_max_y(term);
}

static void term_insert_cells(nss_term_t *term, nss_coord_t n) {
    if (term_cursor_in_region(term)) {
        n = MAX(term_min_x(term), MIN(n, term_max_x(term) - term->c.x));

        nss_line_t *line = term->screen[term->c.y];

        term_adjust_wide_left(term, term->c.x, term->c.y);
        term_adjust_wide_right(term, term->c.x, term->c.y);

        memmove(line->cell + term->c.x + n, line->cell + term->c.x,
                (term_max_x(term) - term->c.x - n) * sizeof(nss_cell_t));
        for (nss_coord_t i = term->c.x + n; i < term_max_x(term); i++)
            line->cell[i].attr &= ~nss_attrib_drawn;

        term_erase(term, term->c.x, term->c.y, term->c.x + n, term->c.y + 1, 0);
    }

    term_reset_pending(term);
}

static void term_delete_cells(nss_term_t *term, nss_coord_t n) {
    // Do not check top/bottom margins, DCH sould work outside them
    if (term->c.x >= term_min_x(term) && term->c.x < term_max_x(term)) {
        n = MAX(0, MIN(n, term_max_x(term) - term->c.x));

        nss_line_t *line = term->screen[term->c.y];

        term_adjust_wide_left(term, term->c.x, term->c.y);
        term_adjust_wide_right(term, term->c.x + n - 1, term->c.y);

        memmove(line->cell + term->c.x, line->cell + term->c.x + n,
                (term_max_x(term) - term->c.x - n) * sizeof(nss_cell_t));
        for (nss_coord_t i = term->c.x; i < term_max_x(term) - n; i++)
            line->cell[i].attr &= ~nss_attrib_drawn;

        term_erase(term, term_max_x(term) - n, term->c.y, term_max_x(term), term->c.y + 1, 0);
    }

    term_reset_pending(term);
}

static void term_insert_lines(nss_term_t *term, nss_coord_t n) {
    if (term_cursor_in_region(term))
        term_scroll(term, term->c.y, -n, 0);
    term_move_to(term, term_min_x(term), term->c.y);
}

static void term_delete_lines(nss_term_t *term, nss_coord_t n) {
    if (term_cursor_in_region(term))
        term_scroll(term, term->c.y, n, 0);
    term_move_to(term, term_min_x(term), term->c.y);
}

static void term_insert_columns(nss_term_t *term, nss_coord_t n) {
    if (term_cursor_in_region(term))
        term_scroll_horizontal(term, term->c.x, -n);
}

static void term_delete_columns(nss_term_t *term, nss_coord_t n) {
    if (term_cursor_in_region(term))
        term_scroll_horizontal(term, term->c.x, n);
}

static void term_index_horizonal(nss_term_t *term) {
    if (term->c.x == term_max_x(term) - 1 && term_cursor_in_region(term)) {
        term_scroll_horizontal(term, term_min_x(term), 1);
        term_reset_pending(term);
    } else if (term->c.x != term_max_x(term) - 1)
        term_move_to(term, term->c.x + 1, term->c.y);
}

static void term_rindex_horizonal(nss_term_t *term) {
    if (term->c.x == term_min_x(term) && term_cursor_in_region(term)) {
        term_scroll_horizontal(term, term_min_x(term), -1);
        term_reset_pending(term);
    } else if (term->c.x != term_min_x(term))
        term_move_to(term, term->c.x - 1, term->c.y);
}

static void term_index(nss_term_t *term) {
    if (term->c.y == term_max_y(term) - 1 && term_cursor_in_region(term)) {
        term_scroll(term, term_min_y(term), 1, 1);
        term_reset_pending(term);
    } else if (term->c.y != term_max_y(term) - 1)
        term_move_to(term, term->c.x, term->c.y + 1);
}

static void term_rindex(nss_term_t *term) {
    if (term->c.y == term_min_y(term) && term_cursor_in_region(term)) {
        term_scroll(term,  term_min_y(term), -1, 1);
        term_reset_pending(term);
    } else if (term->c.y != term_min_y(term))
        term_move_to(term, term->c.x, term->c.y - 1);
}

static void term_cr(nss_term_t *term) {
    term_move_to(term, term->c.x < term_min_x(term) ?
            term_min_ox(term) : term_min_x(term), term->c.y);
}

static void term_print_char(nss_term_t *term, nss_char_t ch) {
    uint8_t buf[5] = {ch};
    size_t sz = 1;
    if (term->mode & nss_tm_utf8) sz = utf8_encode(ch, buf, buf + 5);
    if (write(term->printerfd, buf, sz) < 0) {
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

    while (top < bottom) term_print_line(term, term->screen[top++]);
    if (term->mode & nss_tm_print_form_feed)
        term_print_char(term, '\f');
}

inline static void term_do_wrap(nss_term_t *term) {
    term->screen[term->c.y]->wrap_at = term->c.x + 1;
    if ((term->mode & nss_tm_print_mask) == nss_tm_print_auto)
        term_print_line(term, term->screen[term->c.y]);
    term_index(term);
    term_cr(term);
}

static void term_tabs(nss_term_t *term, nss_coord_t n) {
    //TODO CHT is not affected by DECCOM but CBT is?

    if (n >= 0) {
        if (term->mode & nss_tm_xterm_more_hack && term->c.pending)
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

static void term_load_config(nss_term_t *term) {
    term->c = term->back_cs = term->cs = (nss_cursor_t) {
        .cel = MKCELL(NSS_SPECIAL_FG, NSS_SPECIAL_BG, 0, 0),
        .fg = nss_config_color(NSS_CCONFIG_FG),
        .bg = nss_config_color(NSS_CCONFIG_BG),
        .gl = 0, .gl_ss = 0, .gr = 2,
        .gn = {nss_94cs_ascii, nss_94cs_ascii, nss_94cs_ascii, nss_94cs_ascii}
    };

    for (size_t i = 0; i < NSS_PALETTE_SIZE; i++)
        term->palette[i] = nss_config_color(NSS_CCONFIG_COLOR_0 + i);
    if (nss_config_integer(NSS_ICONFIG_REVERSE_VIDEO)) {
        SWAP(nss_color_t, term->palette[NSS_SPECIAL_BG], term->palette[NSS_SPECIAL_FG]);
        SWAP(nss_color_t, term->palette[NSS_SPECIAL_CURSOR_BG], term->palette[NSS_SPECIAL_CURSOR_FG]);
        SWAP(nss_color_t, term->palette[NSS_SPECIAL_SELECTED_BG], term->palette[NSS_SPECIAL_SELECTED_FG]);
        term->mode |= nss_tm_reverse_video;
    }

    //term->mode |= nss_tm_cksm_mask;

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

static void term_free_scrollback(nss_term_t *term) {
    if (!term->scrollback) return;

    term_reset_view(term, 0);
    for (ssize_t i = 0; i < term->sb_limit; i++)
        term_free_line(term->scrollback[i]);
    free(term->scrollback);

    term->scrollback = NULL;
    term->sb_caps = 0;
    term->sb_limit = 0;
    term->sb_top = 0;
}

static void term_reset_tabs(nss_term_t *term) {
    memset(term->tabs, 0, term->width * sizeof(term->tabs[0]));
    nss_coord_t tabw = nss_config_integer(NSS_ICONFIG_TAB_WIDTH);
    for (nss_coord_t i = tabw; i < term->width; i += tabw)
        term->tabs[i] = 1;
}

static void term_request_resize(nss_term_t *term, int16_t w, int16_t h, _Bool in_cells) {
    int16_t cur_w, cur_h, scr_w, scr_h;
    nss_window_get_dim(term->win, &cur_w, &cur_h);
    nss_window_get_dim_ext(term->win, nss_dt_screen_size, &scr_w, &scr_h);

    if (in_cells) {
        int16_t ce_w, ce_h, bo_w, bo_h;
        nss_window_get_dim_ext(term->win, nss_dt_cell_size, &ce_w, &ce_h);
        nss_window_get_dim_ext(term->win, nss_dt_border, &bo_w, &bo_h);
        if (w > 0) w = w * ce_w + bo_w * 2;
        if (h > 0) h = h * ce_h + bo_h * 2;
    }

    w = !w ? scr_w : w < 0 ? cur_w : w;
    h = !h ? scr_h : h < 0 ? cur_h : h;

    nss_window_resize(term->win, w, h);
}

static void term_set_132(nss_term_t *term, _Bool set) {
    term_reset_margins(term);
    term_move_to(term, term_min_ox(term), term_min_oy(term));
    if (!(term->mode & nss_tm_132_preserve_display))
        term_erase(term, 0, 0, term->width, term->height, 0);
    if (nss_config_integer(NSS_ICONFIG_ALLOW_WINDOW_OPS))
        term_request_resize(term, set ? 132 : 80, -1, 1);
    ENABLE_IF(set, term->mode, nss_tm_132cols);
}

static void term_reset(nss_term_t *term, _Bool hard) {
    if (term->mode & nss_tm_132cols) term_set_132(term, 0);
    if (term->mode & nss_tm_altscreen) term_swap_screen(term, 1);

    term->mode &= nss_tm_focused;
    term->inmode = nss_config_input_mode();

    nss_coord_t cx = term->c.x, cy = term->c.y;
    _Bool cpending = term->c.pending;

    term_load_config(term);
    term_reset_margins(term);
    term_reset_tabs(term);

    nss_window_set_mouse(term->win, 0);
    nss_window_set_cursor(term->win, nss_config_integer(NSS_ICONFIG_CURSOR_SHAPE));
    nss_window_set_colors(term->win, term->palette[NSS_SPECIAL_BG], term->palette[NSS_SPECIAL_CURSOR_FG]);

    if (hard) {
        term_cursor_mode(term, 1);
        term->back_cs = term->cs;
        term_erase(term, 0, 0, term->width, term->height, 0);

        term_free_scrollback(term);

        term->vt_level = term->vt_version / 100;

        nss_window_set_title(term->win, nss_tt_icon_label | nss_tt_title, NULL, term->mode & nss_tm_title_set_utf8);
    } else {
        term->c.x = cx;
        term->c.y = cy;
        term->c.pending = cpending;
    }

    term->esc.state = esc_ground;
}

void nss_term_reset(nss_term_t *term) {
    term_reset(term, 1);
}

nss_term_t *nss_create_term(nss_window_t *win, nss_coord_t width, nss_coord_t height) {
    nss_term_t *term = calloc(1, sizeof(nss_term_t));

    term->palette = malloc(NSS_PALETTE_SIZE * sizeof(nss_color_t));
    term->win = win;

    term->inmode = nss_config_input_mode();
    term->printerfd = -1;
    term->sb_max_caps = nss_config_integer(NSS_ICONFIG_HISTORY_LINES);
    term->vt_version = nss_config_integer(NSS_ICONFIG_VT_VERION);
    term->vt_level = term->vt_version / 100;
    term->fd_start = term->fd_end = term->fd_buf;

    term_load_config(term);

    for (size_t i = 0; i < 2; i++) {
        term_cursor_mode(term, 1);
        term_erase(term, 0, 0, term->width, term->height, 0);
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
    switch (mode) {
    case P('='): /* Tertinary DA */
        CHK_VT(4);
        /* DECREPTUI */
        term_answerback(term, DCS"!|00000000"ST);
        break;
    case P('>'): /* Secondary DA */ {
        param_t ver = 0;
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
        term_answerback(term, CSI">%"PRIu32";666;0c", ver);
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
                    term->inmode.keyboard_mapping == nss_km_vt220 ? ";8" : "",
                    term->vt_level >= 4 ? ";21;22;28" : ";22");
        }
    }
}

static void term_dispatch_dsr(nss_term_t *term) {
    if (term->esc.selector & P_MASK) {
        switch (term->esc.param[0]) {
        case 6: /* DECXCPR -- CSI ? Py ; Px ; R ; 1  */
            term_answerback(term, CSI"%"PRIu16";%"PRIu16"%sR",
                    term->c.y - term_min_oy(term) + 1,
                    term->c.x - term_min_ox(term) + 1,
                    term->vt_level >= 4 ? ";1" : "");
            break;
        case 15: /* Printer status -- Has printer*/
            CHK_VT(2);
            term_answerback(term, term->printerfd >= 0 ? CSI"?10n" : CSI"?13n");
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
        case 53: /* Report locator status */
        case 55:
            CHK_VT(4);
            term_answerback(term, CSI"?50n"); // no locator
            break;
        case 56: /* Report locator type */
            CHK_VT(4);
            term_answerback(term, CSI"?57;0n"); // can't identify
            break;
        case 62: /* DECMSR, Macro space -- No data, no space for macros */
            CHK_VT(4);
            term_answerback(term, CSI"0*{");
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
    return -1U;
#undef NRC
}

inline static void term_parse_cursor_report(nss_term_t *term) {
    char *dstr = (char *)(term->esc.str_ptr ? term->esc.str_ptr : term->esc.str_data);

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
    enum nss_char_set gn[4];
    for (size_t i = 0; i < 4; i++) {
        param_t sel = 0;
        char c;
        if ((c = *dstr++) < 0x30) {
            sel |= I1(c);
            if ((c = *dstr++) < 0x30) goto err;
        }
        sel |= E(c);
        if ((gn[i] = parse_nrcs(sel, (c96 >> i) & 1, term->vt_level,
                term->mode & nss_tm_enable_nrcs)) == -1U) goto err;
    }

    // Everything is OK, load

    term->c = (nss_cursor_t) {
        .x = MIN(x, term->width - 1),
        .y = MIN(y, term->height - 1),
        .cel = (nss_cell_t) {
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
        .gl_ss = flags & 4 ? 3 : flags & 2 ? 2 : term->c.gl
    };

    if (sgr0 & 1) term->c.cel.attr |= nss_attrib_bold;
    if (sgr0 & 2) term->c.cel.attr |= nss_attrib_underlined;
    if (sgr0 & 4) term->c.cel.attr |= nss_attrib_blink;
    if (sgr0 & 8) term->c.cel.attr |= nss_attrib_inverse;
    if (sgr1 & 1) term->c.cel.attr |= nss_attrib_italic;
    if (sgr1 & 2) term->c.cel.attr |= nss_attrib_faint;
    if (sgr1 & 4) term->c.cel.attr |= nss_attrib_strikethrough;
    if (sgr1 & 8) term->c.cel.attr |= nss_attrib_invisible;
    if (prot & 1) term->c.cel.attr |= nss_attrib_protected;

    return;
err:
    term_esc_dump(term, 0);
}

inline static void term_parse_tabs_report(nss_term_t *term) {
    memset(term->tabs, 0, term->width*sizeof(*term->tabs));
    uint8_t *dstr = term->esc.str_ptr ? term->esc.str_ptr : term->esc.str_data;
    uint8_t *dend = dstr + term->esc.si;
    for (ssize_t tab = 0; dstr <= dend; dstr++) {
        if (*dstr == '/' || dstr == dend) {
            if (tab - 1 < term->width) term->tabs[tab - 1] = 1;
            tab = 0;
        } else if (isdigit(*dstr)) {
            tab = 10 * tab + *dstr - '0';
        } else  term_esc_dump(term, 0);
    }
}

static void term_dispatch_dcs(nss_term_t *term) {
    // Fixup parameter count
    term->esc.i += term->esc.param[term->esc.i] >= 0;

    if (term->esc.state != esc_dcs_string) {
        term->esc.selector = term->esc.old_selector;
        term->esc.state = term->esc.old_state;
    }

    term_esc_dump(term, 1);

    // Only SGR is allowed to have subparams
    if (term->esc.subpar_mask) return;

    uint8_t *dstr = term->esc.str_ptr ? term->esc.str_ptr : term->esc.str_data;

    switch (term->esc.selector) {
    case C('s') | P('='): /* iTerm2 syncronous updates */
        switch (PARAM(0,0)) {
        case 1: /* Begin syncronous update */
            nss_window_set_sync(term->win, 1);
            break;
        case 2: /* End syncronous update */
            nss_window_set_sync(term->win, 0);
            break;
        default:
            term_esc_dump(term, 0);
        }
        break;
    case C('q') | I0('$'): /* DECRQSS -> DECRPSS */ {
        if (term->esc.si && term->esc.si < 3) {
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
                term_answerback(term, DCS"1$r%d$|"ST, term->mode & nss_tm_132cols ? 132 : 80);
                break;
            case 'q' << 8 | '"': /* -> DECSCA */
                term_answerback(term, DCS"1$r%d\"q"ST, term->mode & nss_tm_protected ? 2 :
                        term->c.cel.attr & nss_attrib_protected ? 1 : 2);
                break;
            case 'q' << 8 | ' ': /* -> DECSCUSR */
                term_answerback(term, DCS"1$r%d q"ST, nss_window_get_cursor(term->win));
                break;
            case '|' << 8 | '*': /* -> DECSLNS */
                term_answerback(term, DCS"1$r%d*|"ST, term->height);
                break;
            case 'x' << 8 | '*': /* -> DECSACE */
                term_answerback(term, term->vt_level < 4 ? DCS"0$r"ST :
                        DCS"1$r%d*x"ST, term->mode & nss_tm_attr_ext_rectangle ? 2 : 1);
                break;
            case 'p' << 8 | '"': /* -> DECSCL */
                term_answerback(term, DCS"1$r%d%s\"p"ST, 60 + MAX(term->vt_level, 1),
                        term->vt_level >= 2 ? (term->mode & nss_tm_8bit ? ";2" : ";1") : "");
                break;
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
    default:
        term_esc_dump(term, 0);
    }

    term_esc_finish_string(term);
    term->esc.old_state = 0;
    term->esc.state = esc_ground;
}

static nss_clipboard_target_t decode_target(uint8_t targ, _Bool mode) {
    switch (targ) {
    case 'p': return nss_ct_primary;
    case 'q': return nss_ct_secondary;
    case 'c': return nss_ct_clipboard;
    case 's': return mode ? nss_ct_clipboard : nss_ct_primary;
    default: return -1;
    }
}

static void term_dispatch_osc(nss_term_t *term) {
    if (term->esc.state != esc_osc_string) {
        term->esc.state = term->esc.old_state;
        term->esc.selector = term->esc.old_selector;
    }
    term_esc_dump(term, 1);

    uint8_t *dstr = term->esc.str_ptr ? term->esc.str_ptr : term->esc.str_data;
    uint8_t *dend = dstr + term->esc.si;

    switch (term->esc.selector) {
        nss_color_t col;
    case 0: /* Change window icon name and title */
    case 1: /* Change window icon name */
    case 2: /* Change window title */
        if (term->mode & nss_tm_title_set_hex) {
            if (*hex_decode(dstr, dstr, dend)) {
                term_esc_dump(term, 0);
                break;
            }
            dend = memchr(dstr, 0, ESC_MAX_STR);
        }
        uint8_t *res = NULL;
        if (!(term->mode & nss_tm_title_set_utf8) && term->mode & nss_tm_utf8) {
            uint8_t *dst = dstr;
            const uint8_t *ptr = dst;
            nss_char_t val = 0;
            while (*ptr && utf8_decode(&val, &ptr, dend))
                *dst++ = val;
            *dst = '\0';
        } else if (term->mode & nss_tm_title_set_utf8 && !(term->mode & nss_tm_utf8)) {
            res = malloc(term->esc.si * 2 + 1);
            uint8_t *ptr = res, *src = dstr;
            if (res) {
                while (*src) ptr += utf8_encode(*src++, ptr, res + term->esc.si * 2);
                *ptr = '\0';
                dstr = res;
            }
        }
        nss_window_set_title(term->win, 3 - term->esc.selector, (char *)dstr, term->mode & nss_tm_utf8);
        free(res);
        break;
    case 4: /* Set color */ {
        uint8_t *pstr = dstr, *pnext = NULL, *s_end;
        while (pstr < dend && (pnext = memchr(pstr, ';', dend - pstr))) {
            *pnext = '\0';
            errno = 0;
            unsigned long idx = strtoul((char *)pstr, (char **)&s_end, 10);

            uint8_t *parg  = pnext + 1;
            if ((pnext = memchr(parg, ';', dend - parg))) *pnext = '\0';
            else pnext = dend;

            if (!errno && !*s_end && s_end != pstr && idx < NSS_PALETTE_SIZE - NSS_SPECIAL_COLORS) {
                nss_color_t col = parse_color(parg, pnext);
                if (col) term->palette[idx] = col;
                else if (parg[0] == '?' && parg[1] == '\0')
                    term_answerback(term, OSC"4;#%06X"ST, term->palette[idx] & 0x00FFFFFF);
                else term_esc_dump(term, 0);
            }
            pstr = pnext + 1;
        }
        if (pstr < dend && !pnext) {
            while (dstr++ < dend) if (!*dstr) *dstr = ';';
            term_esc_dump(term, 0);
        }
        break;
    }
    case 5: /* Set special color */
    case 6: /* Enable/disable special color */
        term_esc_dump(term, 0);
        break;
    case 10: /* Set VT100 foreground color */
        if ((col = parse_color(dstr, dend))) {
            if (term->mode & nss_tm_reverse_video) {
                term->palette[NSS_SPECIAL_BG] = col;
                nss_window_set_colors(term->win, col, 0);
            } else term->palette[NSS_SPECIAL_FG] = col;
        } else term_esc_dump(term, 0);
        break;
    case 11: /* Set VT100 background color */
        if ((col = parse_color(dstr, dend))) {
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
    case 12: /* Set Cursor color */
        if ((col = parse_color(dstr, dend))) {
            if (!(term->mode & nss_tm_reverse_video)) {
                nss_window_set_colors(term->win, 0, term->palette[NSS_SPECIAL_CURSOR_FG] = col);
            } else term->palette[NSS_SPECIAL_CURSOR_BG] = col;
        } else term_esc_dump(term, 0);
        break;
    case 13: /* Set Mouse foreground color */
    case 14: /* Set Mouse background color */
        break;
    case 17: /* Set Highlight background color */
        if ((col = parse_color(dstr, dend))) {
            if (!(term->mode & nss_tm_reverse_video))
                term->palette[NSS_SPECIAL_SELECTED_BG] = col;
            else term->palette[NSS_SPECIAL_SELECTED_FG] = col;
            term_update_selection(term, nss_sstate_none, (nss_selected_t){0});
        } else term_esc_dump(term, 0);
        break;
    case 19: /* Set Highlight foreground color */
        if ((col = parse_color(dstr, dend))) {
            if (!(term->mode & nss_tm_reverse_video))
                term->palette[NSS_SPECIAL_SELECTED_FG] = col;
            else term->palette[NSS_SPECIAL_SELECTED_BG] = col;
            term_update_selection(term, nss_sstate_none, (nss_selected_t){0});
        } else term_esc_dump(term, 0);
        break;
    case 50: /* Set Font */
        term_esc_dump(term, 0);
        break;
    case 52: /* Manipulate selecion data */ {
        if (!nss_config_integer(NSS_ICONFIG_ALLOW_WINDOW_OPS)) break;

        nss_clipboard_target_t ts[nss_ct_MAX] = {0};
        _Bool toclip = term->mode & nss_tm_select_to_clipboard;
        uint8_t *parg = dstr, letter = 0;
        for (; parg < dend && *parg !=  ';'; parg++) {
            if (strchr("pqsc", *parg)) {
                ts[decode_target(*parg, toclip)] = 1;
                if (!letter) letter = *parg;
            }
        }
        if (parg++ < dend) {
            if (!letter) ts[decode_target((letter = 'c'), toclip)] = 1;
            if (!strcmp("?", (char*)parg)) {
                term->paste_from = letter;
                nss_window_paste_clip(term->win, decode_target(letter, toclip));
            } else {
                if (base64_decode(parg, parg, dend) != dend) parg = NULL;
                for (size_t i = 0; i < nss_ct_MAX; i++) {
                    if (ts[i]) {
                        if (i == term->vsel.targ) term->vsel.targ = -1;
                        nss_window_set_clip(term->win, parg ? (uint8_t *)strdup((char *)parg) : parg, NSS_TIME_NOW, i);
                    }
                }
            }
        } else term_esc_dump(term, 0);
        break;
    }
    case 104: /* Reset color */
        if (term->esc.si) {
            uint8_t *pnext, *s_end;
            while ((pnext = memchr(dstr, ';', dend - dstr))) {
                *pnext = '\0';
                errno = 0;
                unsigned long idx = strtoul((char *)dstr, (char **)&s_end, 10);
                if (!errno && !*s_end && s_end != dstr && idx < NSS_PALETTE_SIZE - NSS_SPECIAL_COLORS) {
                    term->palette[idx] = nss_config_color(NSS_CCONFIG_COLOR_0 + idx);
                } else term_esc_dump(term, 0);
                dstr = pnext + 1;
            }
        } else {
            for (size_t i = 0; i < NSS_PALETTE_SIZE - NSS_SPECIAL_COLORS; i++)
                term->palette[i] = nss_config_color(NSS_CCONFIG_COLOR_0 + i);
        }
        break;
    case 105: /* Reset special color */
    case 106: /* Enable/disable special color */
        term_esc_dump(term, 0);
        break;
    case 110: /*Reset  VT100 foreground color */
        if (!(term->mode & nss_tm_reverse_video))
            term->palette[NSS_SPECIAL_FG] = nss_config_color(NSS_CCONFIG_FG);
        else
            term->palette[NSS_SPECIAL_BG] = nss_config_color(NSS_CCONFIG_FG);
        break;
    case 111: /*Reset  VT100 background color */
        if (!(term->mode & nss_tm_reverse_video)) {
            term->palette[NSS_SPECIAL_CURSOR_BG] = nss_config_color(NSS_CCONFIG_CURSOR_BG);
            nss_window_set_colors(term->win, term->palette[NSS_SPECIAL_BG] = nss_config_color(NSS_CCONFIG_BG), 0);
        } else {
            term->palette[NSS_SPECIAL_FG] = nss_config_color(NSS_CCONFIG_BG);
            nss_window_set_colors(term->win, 0, term->palette[NSS_SPECIAL_CURSOR_FG] = nss_config_color(NSS_CCONFIG_CURSOR_BG));
        }
        break;
    case 112: /*Reset  Cursor color */
        if (!(term->mode & nss_tm_reverse_video)) {
            nss_window_set_colors(term->win, 0, term->palette[NSS_SPECIAL_CURSOR_FG] = nss_config_color(NSS_CCONFIG_CURSOR_FG));
        } else term->palette[NSS_SPECIAL_CURSOR_BG] = nss_config_color(NSS_CCONFIG_CURSOR_FG);
        break;
    case 113: /*Reset  Mouse foreground color */
    case 114: /*Reset  Mouse background color */
        break;
    case 117: /*Reset  Highlight background color */
        if (!(term->mode & nss_tm_reverse_video))
            term->palette[NSS_SPECIAL_SELECTED_BG] = nss_config_color(NSS_CCONFIG_SELECTED_BG);
        else term->palette[NSS_SPECIAL_SELECTED_FG] = nss_config_color(NSS_CCONFIG_SELECTED_BG);
        term_update_selection(term, nss_ssnap_none, (nss_selected_t){0});
        break;
    case 119: /*Reset  Highlight foreground color */
        if (!(term->mode & nss_tm_reverse_video)) {
            term->palette[NSS_SPECIAL_SELECTED_FG] = nss_config_color(NSS_CCONFIG_SELECTED_FG);
        } else term->palette[NSS_SPECIAL_SELECTED_BG] = nss_config_color(NSS_CCONFIG_SELECTED_FG);
        term_update_selection(term, nss_ssnap_none, (nss_selected_t){0});
        break;
    case 13001: {
        errno = 0;
        unsigned long res = strtoul((char *)dstr, (char **)&dstr, 10);
        if (res > 255 || errno || *dstr) {
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

    term_esc_finish_string(term);
    term->esc.old_state = 0;
    term->esc.state = esc_ground;
}

static void term_dispatch_srm(nss_term_t *term, _Bool set) {
    if (term->esc.selector & P_MASK) {
        for (size_t i = 0; i < term->esc.i; i++) {
            switch (PARAM(i, 0)) {
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
                    term->mode &= nss_tm_focused | nss_tm_reverse_video;
                    term_esc_start_seq(term);
                }
                break;
            case 3: /* DECCOLM */
                if (!(term->mode & nss_tm_disable_132cols))
                    term_set_132(term, set);
                break;
            case 4: /* DECSCLM */
                ENABLE_IF(set, term->mode, nss_tm_smooth_scroll);
                break;
            case 5: /* DECSCNM */
                if (set ^ !!(term->mode & nss_tm_reverse_video)) {
                    SWAP(nss_color_t, term->palette[NSS_SPECIAL_BG], term->palette[NSS_SPECIAL_FG]);
                    SWAP(nss_color_t, term->palette[NSS_SPECIAL_CURSOR_BG], term->palette[NSS_SPECIAL_CURSOR_FG]);
                    SWAP(nss_color_t, term->palette[NSS_SPECIAL_SELECTED_BG], term->palette[NSS_SPECIAL_SELECTED_FG]);
                    term_update_selection(term, nss_sstate_none, (nss_selected_t){0});
                    nss_window_set_colors(term->win, term->palette[NSS_SPECIAL_BG], term->palette[NSS_SPECIAL_CURSOR_FG]);
                }
                ENABLE_IF(set, term->mode, nss_tm_reverse_video);
                break;
            case 6: /* DECCOM */
                term->c.origin = set;
                term_move_to(term, term_min_ox(term), term_min_oy(term));
                break;
            case 7: /* DECAWM */
                ENABLE_IF(set, term->mode, nss_tm_wrap);
                if (!set) term_reset_pending(term);
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
                nss_window_set_cursor(term->win, ((nss_window_get_cursor(term->win) + 1) & ~1) - set);
                break;
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
                    term->screen[term->c.y]->cell[term->c.x].attr &= ~nss_attrib_drawn;
                ENABLE_IF(!set, term->mode, nss_tm_hide_cursor);
                break;
            case 30: /* Show scrollbar */
                // IGNORE - There is no scrollbar
                break;
            case 35: /* URXVT Allow change font */
                ENABLE_IF(set, term->mode, nss_tm_allow_change_font);
                break;
            case 40: /* 132COLS */
                ENABLE_IF(!set, term->mode, nss_tm_disable_132cols);
                break;
            case 41: /* XTerm more(1) hack */
                ENABLE_IF(set, term->mode, nss_tm_xterm_more_hack);
                break;
            case 42: /* DECNRCM */
                CHK_VT(3);
                ENABLE_IF(set, term->mode, nss_tm_enable_nrcs);
                break;
            case 45: /* Reverse wrap */
                ENABLE_IF(set, term->mode, nss_tm_reverse_wrap);
                break;
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
            case 69: /* DECLRMM */
                CHK_VT(4);
                ENABLE_IF(set, term->mode, nss_tm_lr_margins);
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
            case 1006: /* SGR mouse tracking */
                ENABLE_IF(set, term->mode, nss_tm_mouse_format_sgr);
                break;
            case 1007: /* Alternate scroll */
                ENABLE_IF(set, term->mode, nss_tm_alternate_scroll);
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
                ENABLE_IF(set, term->mode, nss_tm_keep_selection);
                break;
            case 1041: /* Use CLIPBOARD instead of PRIMARY */
                ENABLE_IF(set, term->mode, nss_tm_select_to_clipboard);
                break;
            case 1044: /* Don't clear X11 CLIPBOARD selection */
                ENABLE_IF(set, term->mode, nss_tm_keep_clipboard);
                break;
            case 1046: /* Allow altscreen */
                ENABLE_IF(!set, term->mode, nss_tm_disable_altscreen);
                break;
            case 1047: /* Enable altscreen and clear screen */
                if (term->mode & nss_tm_disable_altscreen) break;
                if (set == !(term->mode & nss_tm_altscreen))
                    term_swap_screen(term, !set);
                if (set) term_erase(term, 0, 0, term->width, term->height, 0);
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
                if (set) term_erase(term, 0, 0, term->width, term->height, 0);
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
            case 1060: /* Legacy xterm function keys */
                term->inmode.keyboard_mapping = set ? nss_km_legacy : nss_km_default;
                break;
            case 1061: /* VT220 function keys */
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
        for (size_t i = 0; i < term->esc.i; i++) {
            switch (PARAM(i, 0)) {
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

static void term_dispatch_mc(nss_term_t *term) {
    if (term->esc.selector & P_MASK) {
        switch (PARAM(0, 0)) {
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
            for (ssize_t i = 1; i <= term->sb_limit; i++)
                term_print_line(term, term->scrollback[(term->sb_top + i) % term->sb_limit]);
        case 10: /* Print screen */
            term_print_screen(term, 1);
            break;
        default:
            term_esc_dump(term, 0);
        }
    } else {
        switch (PARAM(0, 0)) {
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
    for (size_t i = 0; i < term->esc.i; i++) {
        switch (term->esc.param[i]) {
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

static void term_putchar(nss_term_t *term, nss_char_t ch) {
    // 'print' state

    term->prev_ch = ch; // For REP CSI

    nss_coord_t width = wcwidth(ch);
    if (width < 0) width = 1;
    else if (width > 1) width = 2;
    else if(!width) {
        nss_cell_t *cel = &term->screen[term->c.y]->cell[term->c.x];
        if (term->c.x) cel--;
        if (!cel->ch && term->c.x > 1 && cel[-1].attr & nss_attrib_wide) cel--;
        ch = try_precompose(cel->ch, ch);
        if (cel->ch != ch) *cel = MKCELLWITH(*cel, ch);
        return;
    }

    // This call has mesurable perforemance impact
    // so it should be commented out unless used for debugging
    //debug("%lc (%u)", ch, ch);

    // Wrap line if needed
    if (term->mode & nss_tm_wrap) {
        if (term->c.pending || (width == 2 && term->c.x == term_max_x(term) - 1))
            term_do_wrap(term);
    } else term->c.x = MIN(term->c.x, term_max_x(term) - width);

    nss_cell_t *cell = &term->screen[term->c.y]->cell[term->c.x];

    // Shift characters to the left if insert mode is enabled
    if (term->mode & nss_tm_insert && term->c.x + width < term_max_x(term)) {
        for (nss_cell_t *c = cell + width; c - term->screen[term->c.y]->cell < term_max_x(term); c++)
            c->attr &= ~nss_attrib_drawn;
        memmove(cell + width, cell, (term_max_x(term) - term->c.x - width)*sizeof(*cell));
    }

    // Erase overwritten parts of wide characters
    term_adjust_wide_left(term, term->c.x, term->c.y);
    term_adjust_wide_right(term, term->c.x + width - 1, term->c.y);

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

    term->c.pending = term->c.x + width == term_max_x(term);
    term->c.x += width - term->c.pending;

    term->c.gl_ss = term->c.gl; // Reset single shift
}

static void term_dispatch_window_op(nss_term_t *term) {
    param_t pa = PARAM(0, 24);
    // Only title operations allowed by default
    if (!nss_config_integer(NSS_ICONFIG_ALLOW_WINDOW_OPS) &&
            (pa < 20 || pa > 23)) return;

    switch (pa) {
    case 1: /* Undo minimize */
        nss_window_action(term->win, nss_wa_restore_minimized);
        break;
    case 2: /* Minimize */
        nss_window_action(term->win, nss_wa_minimize);
        break;
    case 3: /* Move */
        nss_window_move(term->win, PARAM(1,0), PARAM(2,0));
        break;
    case 4: /* Resize */
    case 8: /* Resize (in cell units) */
        term_request_resize(term, term->esc.param[2], term->esc.param[1], pa == 8);
        break;
    case 5: /* Raise */
        nss_window_action(term->win, nss_wa_raise);
        break;
    case 6: /* Lower */
        nss_window_action(term->win, nss_wa_lower);
        break;
    case 7: /* Refresh */
        nss_term_damage_lines(term, 0, term->height);
        break;
    case 9: /* Maximize operations */ {
        nss_window_action_t act = -1;

        switch(PARAM(1, 0)) {
        case 0: /* Undo maximize */
            act = nss_wa_restore;
            break;
        case 1: /* Maximize */
            act = nss_wa_maximize;
            break;
        case 2: /* Maximize vertically */
            act = nss_wa_maximize_height;
            break;
        case 3: /* Maximize horizontally */
            act = nss_wa_maximize_width;
            break;
        default:
            term_esc_dump(term, 0);
        }
        if (act >= 0) nss_window_action(term->win, act);
        break;
    }
    case 10: /* Fullscreen operations */ {
        nss_window_action_t act = -1;

        switch(PARAM(1, 0)) {
        case 0: /* Undo fullscreen */
            act = nss_wa_restore;
            break;
        case 1: /* Fullscreen */
            act = nss_wa_fullscreen;
            break;
        case 2: /* Toggle fullscreen */
            act = nss_wa_toggle_fullscreen;
            break;
        default:
            term_esc_dump(term, 0);
        }
        if (act >= 0) nss_window_action(term->win, act);
        break;
    }
    case 11: /* Report state */
        term_answerback(term, CSI"%dt", 1 + !nss_window_is_mapped(term->win));
        break;
    case 13: /* Report position opetations */
        switch(PARAM(1,0)) {
            int16_t x, y;
        case 0: /* Report window position */
            nss_window_get_dim_ext(term->win, nss_dt_window_position, &x, &y);
            term_answerback(term, CSI"3;%d;%dt", x, y);
            break;
        case 2: /* Report grid position */
            nss_window_get_dim_ext(term->win, nss_dt_grid_position, &x, &y);
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
            nss_window_get_dim_ext(term->win, nss_dt_grid_size, &x, &y);
            term_answerback(term, CSI"4;%d;%dt", y, x);
            break;
        case 2: /* Report window size */
            nss_window_get_dim(term->win, &x, &y);
            term_answerback(term, CSI"4;%d;%dt", y, x);
            break;
        default:
            term_esc_dump(term, 0);
        }
        break;
    case 15: /* Report screen size */ {
        int16_t x, y;
        nss_window_get_dim_ext(term->win, nss_dt_screen_size, &x, &y);
        term_answerback(term, CSI"5;%d;%dt", y, x);
        break;
    }
    case 16: /* Report cell size */ {
        int16_t x, y;
        nss_window_get_dim_ext(term->win, nss_dt_cell_size, &x, &y);
        term_answerback(term, CSI"6;%d;%dt", y, x);
        break;
    }
    case 18: /* Report grid size (in cell units) */
        term_answerback(term, CSI"8;%d;%dt", term->height, term->width);
        break;
    case 19: /* Report screen size (in cell units) */ {
        int16_t s_w, s_h, c_w, c_h, b_w, b_h;
        nss_window_get_dim_ext(term->win, nss_dt_screen_size, &s_w, &s_h);
        nss_window_get_dim_ext(term->win, nss_dt_cell_size, &c_w, &c_h);
        nss_window_get_dim_ext(term->win, nss_dt_border, &b_w, &b_h);
        term_answerback(term, CSI"9;%d;%dt", (s_h - 2*b_h)/c_h, (s_w - 2*b_w)/c_w);
        break;
    }
    case 20: /* Report icon label */
    case 21: /* Report title */ {
        uint8_t *res = NULL, *res2 = NULL, *tmp;
        const uint8_t *title = (const uint8_t *)nss_window_get_title(term->win,
                pa == 20 ? nss_tt_icon_label : nss_tt_title);
        _Bool tutf8 = nss_window_is_title_utf8(term->win,
                pa == 20 ? nss_tt_icon_label : nss_tt_title);
        size_t tlen = strlen((const char *)title);

        if (!(term->mode & nss_tm_title_query_utf8) && tutf8) {
            if ((tmp = res2 = malloc(tlen + 1))) {
                const uint8_t *end = title + tlen;
                uint32_t u;
                while (utf8_decode(&u, &title, end)) *tmp++ = u;
                *tmp = '\0';
                tlen = tmp - res2;
                title = res2;
            }
        } else if (term->mode & nss_tm_title_query_utf8 && !tutf8) {
            if ((tmp = res2 = malloc(2 * tlen + 1))) {
                while (*title) tmp += utf8_encode(*title++, tmp, res2 + 2 * tlen);
                *tmp = '\0';
                tlen = tmp - res2;
                title = res2;
            }
        }
        if (term->mode & nss_tm_title_query_hex) {
            if ((res = malloc(tlen*2 + 1))) {
                hex_encode(res, title, title + tlen);
                title = res;
            }
        }
        term_answerback(term, OSC"%c%s"ST, pa == 20 ? 'L' : 'l', title);
        free(res);
        free(res2);
        break;
    }
    case 22: /* Save */
        switch (PARAM(1, 0)) {
        case 0: /* Title and icon label */
        case 1: /* Icon label */
        case 2: /* Title */
            nss_window_push_title(term->win, 3 - PARAM(1, 0));
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
            nss_window_pop_title(term->win, 3 - PARAM(1, 0));
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

inline static const char *unparse_nrcs(enum nss_char_set cs) {
    return (const char *[nss_nrcs_MAX + 1]){
        [nss_94cs_ascii]              = "B",
        [nss_94cs_british]            = "A",
        [nss_94cs_dec_altchars]       = "1",
        [nss_94cs_dec_altgraph]       = "2",
        [nss_94cs_dec_graph]          = "0",
        [nss_94cs_dec_greek]          = "\"?",
        [nss_94cs_dec_hebrew]         = "\"4",
        [nss_94cs_dec_sup]            = "<",
        [nss_94cs_dec_sup_graph]      = "%5",
        [nss_94cs_dec_tech]           = ">",
        [nss_94cs_dec_turkish]        = "%0",
        [nss_96cs_greek]              = "F",
        [nss_96cs_hebrew]             = "H",
        [nss_96cs_latin_1]            = "A",
        [nss_96cs_latin_5]            = "M",
        [nss_96cs_latin_cyrillic]     = "L",
        [nss_nrcs_cyrillic]           = "&4",
        [nss_nrcs_dutch]              = "4",
        [nss_nrcs_finnish2]           = "5",
        [nss_nrcs_finnish]            = "C",
        [nss_nrcs_french2]            = "f",
        [nss_nrcs_french]             = "R",
        [nss_nrcs_french_canadian2]   = "9",
        [nss_nrcs_french_canadian]    = "Q",
        [nss_nrcs_german]             = "K",
        [nss_nrcs_greek]              = "\">",
        [nss_nrcs_hebrew]             = "%=",
        [nss_nrcs_itallian]           = "Y",
        [nss_nrcs_norwegian_dannish2] = "6",
        [nss_nrcs_norwegian_dannish3] = "`",
        [nss_nrcs_norwegian_dannish]  = "E",
        [nss_nrcs_portuguese]         = "%6",
        [nss_nrcs_spannish]           = "Z",
        [nss_nrcs_swedish2]           = "7",
        [nss_nrcs_swedish]            = "H",
        [nss_nrcs_swiss]              = "=",
        [nss_nrcs_turkish]            = "%2",
    }[cs];
}

inline static _Bool nrcs_is_96(enum nss_char_set cs) {
    return cs >= nss_96cs_latin_1;
}

static void term_report_cursor(nss_term_t *term) {
    char csgr[3] = { 0x40, 0, 0 };
    if (term->c.cel.attr & nss_attrib_bold) csgr[0] |= 1;
    if (term->c.cel.attr & nss_attrib_underlined) csgr[0] |= 2;
    if (term->c.cel.attr & nss_attrib_blink) csgr[0] |= 4;
    if (term->c.cel.attr & nss_attrib_inverse) csgr[0] |= 8;

    if (nss_config_integer(NSS_ICONFIG_EXTENDED_CIR)) {
        csgr[0] |= 0x20;
        csgr[1] |= 0x40;
        // Extended byte
        if (term->c.cel.attr & nss_attrib_italic) csgr[1] |= 1;
        if (term->c.cel.attr & nss_attrib_faint) csgr[1] |= 2;
        if (term->c.cel.attr & nss_attrib_strikethrough) csgr[1] |= 4;
        if (term->c.cel.attr & nss_attrib_invisible) csgr[1] |= 8;
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
        /* cell protection */ 0x40 + !!(term->c.cel.attr & nss_attrib_protected),
        /* flags */ cflags,
        /* gl */ term->c.gl,
        /* gr */ term->c.gr,
        /* cs size */ cg96,
        /* g0 */ unparse_nrcs(term->c.gn[0]),
        /* g1 */ unparse_nrcs(term->c.gn[1]),
        /* g2 */ unparse_nrcs(term->c.gn[2]),
        /* g3 */ unparse_nrcs(term->c.gn[3]));
}

static void term_report_tabs(nss_term_t *term) {
    size_t caps = TABSR_INIT_CAP, len = 0;
    char *tabs = malloc(caps), *tmp;

    for (nss_coord_t i = 0; tabs && i < term->width; i++) {
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

static void term_dispatch_csi(nss_term_t *term) {
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
        term_scroll_horizontal(term, term_min_x(term), PARAM(0, 1));
        break;
    case C('A'): /* CUU */
        (term->c.y >= term_min_y(term) ? term_bounded_move_to : term_move_to)
                (term, term->c.x, term->c.y - PARAM(0, 1));
        break;
    case C('A') | I0(' '): /* SR */
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
        void (*erase)(nss_term_t *, nss_coord_t, nss_coord_t, nss_coord_t, nss_coord_t, _Bool) =
                term->esc.selector & P_MASK ? (term->mode & nss_tm_protected ? term_erase : term_selective_erase) :
                term->mode & nss_tm_protected ? term_protective_erase : term_erase;
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
        case 3:
            /* UNIMPLEMENTED - Erase scrollback, xterm */
        default:
            term_esc_dump(term, 0);
        }
        term_reset_pending(term);
        break;
    }
    case C('K') | P('?'): /* DECSEL */
    case C('K'): /* EL */ {
        void (*erase)(nss_term_t *, nss_coord_t, nss_coord_t, nss_coord_t, nss_coord_t, _Bool) =
                term->esc.selector & P_MASK ? (term->mode & nss_tm_protected ? term_erase : term_selective_erase) :
                term->mode & nss_tm_protected ? term_protective_erase : term_erase;
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
        (term->mode & nss_tm_protected ? term_protective_erase : term_erase)
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
        for (param_t i = PARAM(0, 1); i > 0; i--)
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
    case C('m') | P('>'): /* XTMODKEYS */ {
        nss_input_mode_t mode = nss_config_input_mode();
        param_t p = PARAM(0, 0), inone = !term->esc.i && term->esc.param[0] < 0;
        if (term->esc.i > 0 && term->esc.param[1] >= 0) {
            switch (p) {
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
        term_decode_sgr(term, 0, &(nss_cell_t){0}, &term->c.cel, &term->c.fg, &term->c.bg);
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
    case C('q'): /* DECLL */
        for (param_t i = 0; i < term->esc.i; i++) {
            switch (PARAM(i, 0)) {
            case 1: term->mode |= nss_tm_led_num_lock; break;
            case 2: term->mode |= nss_tm_led_caps_lock; break;
            case 3: term->mode |= nss_tm_led_scroll_lock; break;
            case 0: term->mode &= ~(nss_tm_led_caps_lock | nss_tm_led_scroll_lock); //fallthrough
            case 21: term->mode &= ~nss_tm_led_num_lock; break;
            case 22: term->mode &= ~nss_tm_led_caps_lock; break;
            case 23: term->mode &= ~nss_tm_led_scroll_lock; break;
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
        if (term->mode & nss_tm_lr_margins) {
            term_set_lr_margins(term, PARAM(0, 1) - 1, PARAM(1, term->width) - 1);
            term_move_to(term, term_min_ox(term), term_min_oy(term));
        } else
            term_cursor_mode(term, 1);
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
            param_t p = PARAM(0, 0);
            if (p < 2) term_answerback(term, CSI"%d;1;1;128;128;1;0x", p + 2);
        }
        break;
    case C('q') | I0(' '): /* DECSCUSR */ {
        nss_cursor_type_t csr = PARAM(0, 1);
        if (csr < 7) nss_window_set_cursor(term->win, csr);
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
        if (p > 1) switch (PARAM(1, 2)) {
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
        switch (PARAM(0, 2)) {
        case 1:
            term->c.cel.attr |= nss_attrib_protected;
            break;
        case 0: case 2:
            term->c.cel.attr &= ~nss_attrib_protected;
            break;
        }
        term->mode &= ~nss_tm_protected;
        break;
    case C('p') | I0('$'): /* RQM */ {
        CHK_VT(3);
        param_t val = 0; /* unknown */
        switch(PARAM(0, 0)) {
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
            val = 4; /* always reset */
            break;
        case 2: /* KAM */
            val = 1 + !term->inmode.keylock; /* reset/set */
            break;
        case 3: /* CRM */
            val = 2; /* reset */
            break;
        case 4: /* IRM */
            val = 1 + !(term->mode & nss_tm_insert); /* reset/set */
            break;
        case 12: /* SRM */
            val = 1 + !(term->mode & nss_tm_echo); /* reset/set */
            break;
        case 20: /* LNM */
            val = 1 + !(term->mode & nss_tm_crlf); /* reset/set */
            break;
        default:
            term_esc_dump(term, 0);
        }
        term_answerback(term, CSI"%d;%d$y", PARAM(0, 0), val);
        break;
    }
    case C('p') | P('?') | I0('$'): /* DECRQM */ {
        CHK_VT(3);
        param_t val = 0; /* unknown */
        switch(PARAM(0, 0)) {
        case 1: /* DECCKM */
            val = 1 + !term->inmode.appcursor;
            break;
        case 2: /* DECANM */
            val = 2;
            break;
        case 3: /* DECCOLM */
            val = 1 + !(term->mode & nss_tm_132cols);
            break;
        case 4: /* DECSCLM */
            val = 1 + !(term->mode & nss_tm_smooth_scroll);
            break;
        case 5: /* DECCNM */
            val = 1 + !(term->mode & nss_tm_reverse_video);
            break;
        case 6: /* DECCOM */
            val = 1 + !term->c.origin;
            break;
        case 7: /* DECAWM */
            val = 1 + !(term->mode & nss_tm_wrap);
            break;
        case 8: /* DECARM */
            val = 4;
            break;
        case 9: /* X10 Mouse */
            val = 1 + !(term->mode & nss_tm_mouse_x10);
            break;
        case 10: /* Show toolbar */
            val = 4;
            break;
        case 12: /* Start blinking cursor */
        case 13:
            val = 1 + !(nss_window_get_cursor(term->win) & 1);
            break;
        case 14: /* Enable XORG of control sequence and menu for blinking */
            val = 4;
            break;
        case 18: /* DECPFF */
            val = 1 + !(term->mode & nss_tm_print_form_feed);
            break;
        case 19: /* DECREX */
            val = 1 + !(term->mode & nss_tm_print_extend);
            break;
        case 25: /* DECTCEM */
            val = 1 + !!(term->mode & nss_tm_hide_cursor);
            break;
        case 30: /* Show scrollbar */
            val = 4;
            break;
        case 35: /* URXVT Allow change font */
            val = 1 + !(term->mode & nss_tm_allow_change_font);
            break;
        case 40: /* 132COLS */
            val = 1 + !!(term->mode & nss_tm_disable_132cols);
            break;
        case 41: /* XTerm more(1) hack */
            val = 1 + !(term->mode & nss_tm_xterm_more_hack);
            break;
        case 42: /* DECNRCM */
            val = 1 + !(term->mode & nss_tm_enable_nrcs);
            break;
        case 45: /* Reverse wrap */
            val = 1 + !(term->mode & nss_tm_reverse_wrap);
            break;
        case 47: /* Enable altscreen */
            val = 1 + !(term->mode & nss_tm_altscreen);
            break;
        case 66: /* DECNKM */
            val = 1 + !term->inmode.appkey;
            break;
        case 67: /* DECBKM */
            val = 1 + term->inmode.backspace_is_del;
            break;
        case 69: /* DECLRMM */
            val = 1 + !(term->mode & nss_tm_lr_margins);
            break;
        //case 80: /* DECSDM */ //TODO SIXEL
        //   break;
        case 95: /* DECNCSM */
            val = 1 + !(term->mode & nss_tm_132_preserve_display);
            break;
        case 1000: /* X11 Mouse tracking */
            val = 1 + !(term->mode & nss_tm_mouse_button);
            break;
        case 1001: /* Highlight mouse tracking */
            val = 4;
            break;
        case 1002: /* Cell motion tracking on keydown */
            val = 1 + !(term->mode & nss_tm_mouse_motion);
            break;
        case 1003: /* All motion mouse tracking */
            val = 1 + !(term->mode & nss_tm_mouse_many);
            break;
        case 1004: /* Focus in/out events */
            val = 1 + !(term->mode & nss_tm_track_focus);
            break;
        case 1005: /* UTF-8 mouse tracking */
            val = 4;
            break;
        case 1006: /* SGR Mouse tracking */
            val = 1 + !(term->mode & nss_tm_mouse_format_sgr);
            break;
        case 1007: /* Alternate scroll */
            val = 1 + !(term->mode & nss_tm_alternate_scroll);
            break;
        case 1010: /* Scroll to bottom on output */
            val = 1 + !(term->mode & nss_tm_scroll_on_output);
            break;
        case 1011: /* Scroll to bottom on keypress */
            val = 1 + !!(term->mode & nss_tm_dont_scroll_on_input);
            break;
        case 1015: /* Urxvt mouse tracking */
            val = 4;
            break;
        case 1034: /* Interpret meta */
            val = 1 + !term->inmode.has_meta;
            break;
        case 1035: /* Numlock */
            val = 1 + !term->inmode.allow_numlock;
            break;
        case 1036: /* Meta sends escape */
            val = 1 + !term->inmode.meta_escape;
            break;
        case 1037: /* Backspace is delete */
            val = 1 + !term->inmode.backspace_is_del;
            break;
        case 1040: /* Don't clear X11 PRIMARY selecion */
            val = 1 + !(term->mode & nss_tm_keep_selection);
            break;
        case 1041: /* Use CLIPBOARD instead of PRIMARY */
            val = 1 + !(term->mode & nss_tm_select_to_clipboard);
            break;
        case 1044: /* Don't clear X11 CLIPBOARD */
            val = 1 + !(term->mode & nss_tm_keep_clipboard);
            break;
        case 1046: /* Allow altscreen */
            val = 1 + !!(term->mode & nss_tm_disable_altscreen);
            break;
        case 1047: /* Enable altscreen and clear screen */
            val = 1 + !(term->mode & nss_tm_altscreen);
            break;
        case 1048: /* Save cursor */
            val = 3; // TODO: What should be reported there?
            break;
        case 1049: /* Save cursor and switch to altscreen */
            val = 1 + !(term->mode & nss_tm_altscreen);
            break;
        case 1051: /* SUN function keys */
            val = 1 + !(term->inmode.keyboard_mapping == nss_km_sun);
            break;
        case 1052: /* HP function keys */
            val = 1 + !(term->inmode.keyboard_mapping == nss_km_hp);
            break;
        case 1053: /* SCO function keys */
            val = 1 + !(term->inmode.keyboard_mapping == nss_km_sco);
            break;
        case 1060: /* Legacy xterm function keys */
            val = 1 + !(term->inmode.keyboard_mapping == nss_km_legacy);
            break;
        case 1061: /* VT220 function keys */
            val = 1 + !(term->inmode.keyboard_mapping == nss_km_vt220);
            break;
        case 2004: /* Bracketed paste */
            val = 1 + !(term->mode & nss_tm_bracketed_paste);
            break;
        default:
            term_esc_dump(term, 0);
        }
        // DECRPM
        term_answerback(term, CSI"?%d;%d$y", PARAM(0, 0), val);
        break;
    }
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
        (term->mode & nss_tm_protected ? term_protective_erase : term_erase)
                (term, term_min_ox(term) + PARAM(1, 1) - 1, term_min_oy(term) + PARAM(0, 1) - 1,
                term_min_ox(term) + PARAM(3, term_max_ox(term) - term_min_ox(term)),
                term_min_oy(term) + PARAM(2, term_max_oy(term) - term_min_oy(term)), 1);
        break;
    case C('{') | I0('$'): /* DECSERA */
        CHK_VT(4);
        (term->mode & nss_tm_protected ? term_erase : term_selective_erase)
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
    case C('y') | I0('#'): /* XTCHECKSUM */
        term->mode &= ~nss_tm_cksm_mask;
        term->mode |= (PARAM(0, 0) & 0x3FULL) << 48;
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
        case 1:
            term->mode &= ~nss_tm_attr_ext_rectangle;
            break;
        case 2:
            term->mode |= nss_tm_attr_ext_rectangle;
            break;
        default:
            term_esc_dump(term, 0);
        }
        break;
    case C('|') | I0('$'): /* DECSCPP */
        if (nss_config_integer(NSS_ICONFIG_ALLOW_WINDOW_OPS))
            term_request_resize(term, PARAM(0, 80), -1, 1);
        break;
    case C('|') | I0('*'): /* DECSNLS */
        if (nss_config_integer(NSS_ICONFIG_ALLOW_WINDOW_OPS))
            term_request_resize(term, -1, PARAM(0, 24), 1);
    case C('W') | P('?'): /* DECST8C */
        if (PARAM(0, 5) == 5) term_reset_tabs(term);
        else term_esc_dump(term, 0);
        break;
    //case C('t') | I0(' '): /* DECSWBV */
    //    break;
    //case C('u') | I0(' '): /* DECSMBV */
    //    break;
    //case C('w') | I0('\''): /* DECEFR */
    //    break;
    //case C('z') | I0('\''): /* DECELR */
    //    break;
    //case C('{') | I0('\''): /* DECSLE */
    //    break;
    //case C('|') | I0('\''): /* DECRQLP */
    //    break;
    //case C('p') | P('>'): /* XTSMPOINTER */
    //    break;
    //case C('S') | P('?'): /* XTSMSGRAPHICS */
    //    break;
    //case C('S') | P('>'): /* Set graphics attributes, xterm */ //TODO SIXEL
    //    break;
    default:
        term_esc_dump(term, 0);
    }

    term->esc.state = esc_ground;
}

static void term_dispatch_esc(nss_term_t *term) {
    if (nss_config_integer(NSS_ICONFIG_LOG_LEVEL) > 2) {
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
        term->esc.old_state = 0;
        return;
    case E('\\'): /* ST */
        if (term->esc.old_state == esc_dcs_string)
            term_dispatch_dcs(term);
        else if (term->esc.old_state == esc_osc_string)
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
        for (nss_coord_t i = 0; i < term->height; i++)
            for (nss_coord_t j = 0; j < term->width; j++)
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
        switch (term->esc.selector & I0_MASK) {
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

    term_esc_finish_string(term);
    term->esc.old_state = 0;
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
        term_move_left(term, 1);
        break;
    case 0x09: /* HT */
        term_tabs(term, 1);
        break;
    case 0x0a: /* LF */
    case 0x0b: /* VT */
    case 0x0c: /* FF */
        if ((term->mode & nss_tm_print_mask) == nss_tm_print_auto)
            term_print_line(term, term->screen[term->c.y]);
        term_index(term);
        if (term->mode & nss_tm_crlf) term_cr(term);
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
        term_move_to(term, term->c.x, term->c.y);
        // Clear selection when selected cell is overwritten
        if (nss_term_is_selected(term, term->c.x, term->c.y))
            nss_term_clear_selection(term);
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
    (term->c.origin ? term_bounded_move_to : term_move_to)
            (term, term_min_ox(term) + term->esc.param[1], term_min_oy(term) + term->esc.param[0]);
    term->esc.state = esc_ground;
}

static void term_dispatch(nss_term_t *term, nss_char_t ch) {
    // TODO More sophisticated filtering
    if (term->mode & nss_tm_print_enabled)
        term_print_char(term, ch);

    if (IS_C1(ch) && term->vt_level > 1) {
        term->esc.old_selector = term->esc.selector;
        term->esc.old_state = term->esc.state;
        term->esc.state = esc_esc_entry;
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
            uint8_t buf[UTF8_MAX_LEN + 1];
            size_t char_len = utf8_encode(ch, buf, buf + UTF8_MAX_LEN);
            buf[char_len] = '\0';

            if (term->esc.si + char_len + 1 > term->esc.str_cap) {
                size_t new_cap = STR_CAP_STEP(term->esc.str_cap);
                if (new_cap > ESC_MAX_LONG_STR) break;
                uint8_t *new = realloc(term->esc.str_ptr, new_cap + 1);
                if (!new) break;
                if (!term->esc.str_ptr) memcpy(new, term->esc.str_data, term->esc.si);
                term->esc.str_ptr = new;
                term->esc.str_cap = new_cap;
            }

            memcpy((term->esc.str_ptr ? term->esc.str_ptr : term->esc.str_data) + term->esc.si, buf, char_len + 1);
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
        else  {
            // Decode nrcs

            // In theory this should be disabled while in UTF-8 mode, but
            // in practive applications use these symbols, so keep translating (but restrict charsets to only DEC Graph in GL)
            if ((term->mode & nss_tm_utf8) && !nss_config_integer(NSS_ICONFIG_FORCE_UTF8_NRCS))
                ch = nrcs_decode_fast(glv, ch);
            else
                ch = nrcs_decode(glv, term->c.gn[term->c.gr], ch, term->mode & nss_tm_enable_nrcs);

            term_putchar(term, ch);
        }
    }
}

static void term_write(nss_term_t *term, const uint8_t **start, const uint8_t **end, _Bool show_ctl) {
    while (*start < *end) {
        nss_char_t ch;
        // Try to handle unencoded C1 bytes even if UTF-8 is enabled
        if (!(term->mode & nss_tm_utf8) || IS_C1(**start)) ch = *(*start)++;
        else if (!utf8_decode(&ch, (const uint8_t **)start, *end)) break;

        if (show_ctl) {
            if (IS_C1(ch)) {
                term_dispatch(term, '^');
                term_dispatch(term, '[');
                ch ^= 0xc0;
            } else if ((IS_C0(ch) || IS_DEL(ch)) && ch != '\n' && ch != '\t' && ch != '\r') {
                term_dispatch(term, '^');
                ch ^= 0x40;
            }
        }
        term_dispatch(term, ch);
    }
}


static ssize_t term_refill(nss_term_t *term) {
    if (term->fd == -1) return -1;

    ssize_t inc, sz = term->fd_end - term->fd_start;

    if (term->fd_start != term->fd_buf) {
        memmove(term->fd_buf, term->fd_start, sz);
        term->fd_end = term->fd_buf + sz;
        term->fd_start = term->fd_buf;
    }

    if ((inc = read(term->fd, term->fd_end, sizeof(term->fd_buf) - sz)) < 0) {
        warn("Can't read from tty");
        nss_term_hang(term);
        return -1;
    }

    term->fd_end += inc;
    return inc;
}

void nss_term_read(nss_term_t *term) {
    if (term_refill(term) <= 0) return;

    if (term->mode & nss_tm_scroll_on_output && term->view)
        term_reset_view(term, 1);

    term_write(term, (const uint8_t **)&term->fd_start,
            (const uint8_t **)&term->fd_end, 0);
}

inline static void tty_write_raw(nss_term_t *term, const uint8_t *buf, ssize_t len) {
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
                    lim = term_refill(term);
                len -= res;
                buf += res;
            } else break;
        }
        if (pfd.revents & POLLIN)
            lim = term_refill(term);
    }
}

inline static void term_tty_write(nss_term_t *term, const uint8_t *buf, size_t len) {
    if (term->fd == -1) return;

    const uint8_t *next;

    if (!(term->mode & nss_tm_crlf))
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

static size_t term_encode_c1(nss_term_t *term, const uint8_t *in, uint8_t *out) {
    uint8_t *fmtp = out;
    for (uint8_t *it = (uint8_t *)in; *it && fmtp - out < MAX_REPORT - 1; it++) {
        if (IS_C1(*it) && (term->mode & nss_tm_utf8 ||
                !(term->mode & nss_tm_8bit) || term->vt_level < 2)) {
            *fmtp++ = 0x1B;
            *fmtp++ = *it ^ 0xC0;
            // Theoretically we can use C1 encoded as UTF-8 in nss_tm_utf8
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

static void term_answerback(nss_term_t *term, const char *str, ...) {
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

    const uint8_t *end = len + str;
    if (term->mode & nss_tm_echo)
        term_write(term, (const uint8_t **)&str, &end, 1);

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

    _Bool cur_moved = term->c.x == term->width - 1 && term->c.pending;

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

    if (width > term->width) {
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

    ssize_t view = term->view;
    for (size_t j = 0; j < 2; j++) {
        // Reallocate line if it is not wide enough
        for (nss_coord_t i = 0; i < minh; i++)
            if (term->screen[i]->width < width)
                term->screen[i] = term_realloc_line(term, term->screen[i], width);
        term_swap_screen(term, 0);
    }
    term->view = view;

    // Reset scroll region

    term_reset_margins(term);
    term_reset_pending(term);
    if (cur_moved) {
        term->screen[term->c.y]->cell[term->c.x].attr &= ~nss_attrib_drawn;
        term->screen[term->c.y]->cell[MAX(term->c.x - 1, 0)].attr &= ~nss_attrib_drawn;
    }

    // Damage screen

    if (!(term->mode & nss_tm_altscreen)) {
        if (dy > 0) term_damage(term, (nss_rect_t) { 0, minh, minw, dy });
        if (dx > 0) term_damage(term, (nss_rect_t) { minw, 0, dx, height });
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
     * Probably creating the queue of paste requests
     * would be a valid solution
     *
     * But this race isn't that destructive and
     * rather rare to deal with
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
    term->screen[term->c.y]->cell[term->c.x].attr &= ~nss_attrib_drawn;
}

inline static size_t descomose_selection(nss_rect_t dst[static 3], nss_selected_t seld, nss_rect_t bound, ssize_t pos) {
    size_t count = 0;
    nss_coord_t x0 = seld.x0, x1 = seld.x1 + 1;
    ssize_t y0 = seld.y0 + pos, y1 = seld.y1 + 1 + pos;
    if (seld.rect || y1 - y0 == 1) {
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

static void term_update_selection(nss_term_t *term, uint8_t oldstate, nss_selected_t old) {
    // There could be at most 6 difference rectangles

    nss_rect_t d_old[4] = {{0}}, d_new[4] = {{0}}, d_diff[16] = {{0}};
    size_t sz_old = 0, sz_new = 0, count = 0;
    nss_rect_t *res = d_diff, bound = {0, 0, term->width, term->height};

    if (oldstate != nss_sstate_none && oldstate != nss_sstate_pressed)
        sz_old = descomose_selection(d_old, old, bound, term->view);
    if (term->vsel.state != nss_sstate_none && term->vsel.state != nss_sstate_pressed)
        sz_new = descomose_selection(d_new, term->vsel.n, bound, term->view);

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
        term_damage(term, res[i]);
}

void nss_term_clear_selection(nss_term_t *term) {
    nss_selected_t old = term->vsel.n;
    uint8_t oldstate = term->vsel.state;

    term->vsel.state = nss_sstate_none;

    term_update_selection(term, oldstate, old);

    if (term->vsel.targ > 0) {
        if (term->mode & nss_tm_keep_selection) return;

        nss_window_set_clip(term->win, NULL, NSS_TIME_NOW, term->vsel.targ);
        term->vsel.targ = -1;
    }
}

static void term_scroll_selection(nss_term_t *term, nss_coord_t amount, _Bool save) {
    // TODO Selction now just ignores margins

    if (term->vsel.state == nss_sstate_none) return;

    _Bool cond0 = (term->top <= term->vsel.n.y0 && term->vsel.n.y0 <= term->bottom);
    _Bool cond1 = (term->top <= term->vsel.n.y1 && term->vsel.n.y1 <= term->bottom);

    // Clear sellection if it is going to be split by scroll
    if ((cond0 ^ cond1) && !(save && term->vsel.n.y1 <= term->bottom)) nss_term_clear_selection(term);
    else if (term->vsel.n.y1 <= term->bottom) {
        // Scroll and cut off scroll off lines

        term->vsel.r.y0 -= amount;
        term->vsel.n.y0 -= amount;
        term->vsel.r.y1 -= amount;
        term->vsel.n.y1 -= amount;

        _Bool swapped = term->vsel.r.y0 > term->vsel.r.y1;

        if (swapped) {
            SWAP(ssize_t, term->vsel.r.y0, term->vsel.r.y1);
            SWAP(ssize_t, term->vsel.r.x0, term->vsel.r.x1);
        }

        ssize_t top = save ? -term->sb_limit : term->top;
        if (term->vsel.r.y0 < top) {
            term->vsel.r.y0 = top;
            term->vsel.n.y0 = top;
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
 }

inline static _Bool is_separator(nss_char_t ch) {
        if (!ch) return 1;
        uint8_t cbuf[UTF8_MAX_LEN + 1];
        cbuf[utf8_encode(ch, cbuf, cbuf + UTF8_MAX_LEN)] = '\0';
        return strstr(nss_config_string(NSS_SCONFIG_WORD_SEPARATORS), (char *)cbuf);
}

static void term_snap_selection(nss_term_t *term) {
    term->vsel.n.x0 = term->vsel.r.x0, term->vsel.n.y0 = term->vsel.r.y0;
    term->vsel.n.x1 = term->vsel.r.x1, term->vsel.n.y1 = term->vsel.r.y1;
    term->vsel.r.rect = term->vsel.n.rect;
    if (term->vsel.n.y1 <= term->vsel.n.y0) {
        if (term->vsel.n.y1 < term->vsel.n.y0) {
            SWAP(nss_coord_t, term->vsel.n.y0, term->vsel.n.y1);
            SWAP(nss_coord_t, term->vsel.n.x0, term->vsel.n.x1);
        } else if (term->vsel.n.x1 < term->vsel.n.x0) {
            SWAP(nss_coord_t, term->vsel.n.x0, term->vsel.n.x1);
        }
    }
    if (term->vsel.n.rect && term->vsel.n.x1 < term->vsel.n.x0)
            SWAP(nss_coord_t, term->vsel.n.x0, term->vsel.n.x1);

    if (term->vsel.snap != nss_ssnap_none && term->vsel.state == nss_sstate_pressed)
        term->vsel.state = nss_sstate_progress;

    if (term->vsel.snap == nss_ssnap_line) {
        nss_line_iter_t it = make_screen_iter(term, -term->sb_limit, term->vsel.n.y0 + 1);
        line_iter_inc(&it, term->vsel.n.y0 + term->sb_limit);
        term->vsel.n.x0 = 0;
        term->vsel.n.x1 = term->width - 1;

        nss_line_t *line;
        term->vsel.n.y0++;
        do line = line_iter_prev(&it), term->vsel.n.y0--;
        while (line && line->wrap_at);

        it = make_screen_iter(term, term->vsel.n.y1, term->height);
        line = line_iter_next(&it);
        if (!line) return;

        while (line && line->wrap_at)
            line = line_iter_next(&it), term->vsel.n.y1++;

    } else if (term->vsel.snap == nss_ssnap_word) {
        nss_line_iter_t it = make_screen_iter(term, -term->sb_limit, term->vsel.n.y0 + 1);
        line_iter_inc(&it, term->vsel.n.y0 + term->sb_limit);
        nss_line_t *line = line_iter_ref(&it);

        if (!line) return;

        term->vsel.n.x0 = MIN(term->vsel.n.x0, line->width - 1);
        _Bool first = 1, cat = is_separator(line->cell[term->vsel.n.x0].ch);
        if (term->vsel.n.x0 >= 0) do {
            if (!first) {
                term->vsel.n.x0 = line->wrap_at;
                term->vsel.n.y0--;
            } else first = 0;
            while (term->vsel.n.x0 > 0 &&
                    cat == is_separator(line->cell[term->vsel.n.x0 - 1].ch)) term->vsel.n.x0--;
            if (cat != is_separator(line->cell[0].ch)) break;
        } while ((line = line_iter_prev(&it)) && line->wrap_at);

        it = make_screen_iter(term, term->vsel.n.y1, term->height);
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
                line_len = line->wrap_at ? line->wrap_at : line->width;
            } else first = 0;
            while (term->vsel.n.x1 < line_len - 1 &&
                    cat == is_separator(line->cell[term->vsel.n.x1 + 1].ch)) term->vsel.n.x1++;
            if (cat != is_separator(line->cell[line_len - 1].ch)) break;
        } while (line->wrap_at && (line = line_iter_next(&it)));
    }

    // Snap selection on wide characters
    term->vsel.n.x1 += !!(term_line_at(term, term->vsel.n.y1)->cell[term->vsel.n.x1].attr & nss_attrib_wide);
    if (term->vsel.n.x0 > 0)
        term->vsel.n.x0 -= !!(term_line_at(term, term->vsel.n.y0)->cell[term->vsel.n.x0 - 1].attr & nss_attrib_wide);
}

_Bool nss_term_is_selected(nss_term_t *term, nss_coord_t x, nss_coord_t y) {
    if (term->vsel.state == nss_sstate_none || term->vsel.state == nss_sstate_pressed) return 0;

    y -= term->view;

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
        nss_line_iter_t it = make_screen_iter(term, term->vsel.n.y0, term->vsel.n.y1 + 1);
        if (term->vsel.n.rect || term->vsel.n.y0 == term->vsel.n.y1) {
            while ((line = line_iter_next(&it)))
                append_line(&pos, &cap, &res, line, term->vsel.n.x0, term->vsel.n.x1 + 1);
        } else {
            while ((line = line_iter_next(&it))) {
                if (!line_iter_y(&it))
                    append_line(&pos, &cap, &res, line, term->vsel.n.x0, line->width);
                else if (line_iter_y(&it) == term->vsel.n.y1 - term->vsel.n.y0)
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
    nss_selected_t old = term->vsel.n;
    uint8_t oldstate = term->vsel.state;

    if (state == nss_sstate_pressed) {
        term->vsel.r.x0 = x;
        term->vsel.r.y0 = y - term->view;

        struct timespec now;
        clock_gettime(NSS_CLOCK, &now);

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
    term->vsel.r.y1 = y - term->view;

    term_snap_selection(term);
    term_update_selection(term, oldstate, old);
}

void nss_term_mouse(nss_term_t *term, nss_coord_t x, nss_coord_t y, nss_mouse_state_t mask, nss_mouse_event_t event, uint8_t button) {
    x = MIN(MAX(0, x), term->width - 1);
    y = MIN(MAX(0, y), term->height - 1);

    /* Report mouse */
    if (term->mode & nss_tm_mouse_mask && (mask & 0xFF) != nss_input_force_mouse_mask()) {
        if (term->mode & nss_tm_mouse_x10 && button > 2) return;
        if (!(term->mode & nss_tm_mouse_mask)) return;

        if (event == nss_me_motion) {
            if (!(term->mode & (nss_tm_mouse_many | nss_tm_mouse_motion))) return;
            if (term->mode & nss_tm_mouse_button && term->prev_mouse_button == 3) return;
            if (x == term->prev_mouse_x && y == term->prev_mouse_y) return;
            button = term->prev_mouse_button + 32;
        } else {
            if (button > 6) button += 128 - 7;
            else if (button > 2) button += 64 - 3;
            if (event == nss_me_release) {
                if (term->mode & nss_tm_mouse_x10) return;
                /* Don't report wheel relese events */
                if (button == 64 || button == 65) return;
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
        } else if (x < 223 || y < 223) {
            term_answerback(term, CSI"%s%c%c%c",
                    term->inmode.keyboard_mapping == nss_km_sco ? ">M" : "M",
                    button + ' ', x + 1 + ' ', y + 1 + ' ');
        } else return;

        term->prev_mouse_x = x;
        term->prev_mouse_y = y;
    /* Scroll view */
    } else if (event == nss_me_press && (button == 3 || button == 4)) {
        nss_term_scroll_view(term, (2 *(button == 3) - 1) * nss_config_integer(NSS_ICONFIG_SCROLL_AMOUNT));
    /* Select */
    } else if ((event == nss_me_press && button == 0) ||
               (event == nss_me_motion && mask & nss_ms_button_1 &&
                    (term->vsel.state == nss_sstate_progress || term->vsel.state == nss_sstate_pressed)) ||
               (event == nss_me_release && button == 0 &&
                    (term->vsel.state == nss_sstate_progress))) {

        term_change_selection(term, event + 1, x, y, mask & nss_mm_mod1);

        if (event == nss_me_release) {
            term->vsel.targ = term->mode & nss_tm_select_to_clipboard ? nss_ct_clipboard : nss_ct_primary;
            nss_window_set_clip(term->win, term_selection_data(term), NSS_TIME_NOW, term->vsel.targ);
        }
    /* Paste */
    } else if (button == 1 && event == nss_me_release) {
        nss_window_paste_clip(term->win, nss_ct_primary);
    }
}

void nss_term_hang(nss_term_t *term) {
    if (term->fd >= 0) {
        close(term->fd);
        if (term->printerfd != STDOUT_FILENO)
            close(term->printerfd);
        term->fd = -1;
    }
    kill(term->child, SIGHUP);
}

void nss_free_term(nss_term_t *term) {
    nss_term_hang(term);

    term_free_scrollback(term);

    for (nss_coord_t i = 0; i < term->height; i++) {
        term_free_line(term->screen[i]);
        term_free_line(term->back_screen[i]);
    }
    free(term->screen);
    free(term->back_screen);


    free(term->tabs);
    free(term->palette);
    free(term);
}
