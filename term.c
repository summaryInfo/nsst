/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

#include "feature.h"

#define _DEFAULT_SOURCE
#include <assert.h>

#include "config.h"
#include "input.h"
#include "line.h"
#include "mouse.h"
#include "nrcs.h"
#include "screen.h"
#include "term.h"
#include "tty.h"
#if USE_URI
#    include "uri.h"
#endif
#include "window.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#define ESC_MAX_PARAM 32
#define ESC_MAX_STR 256
#define ESC_MAX_LONG_STR 0x10000000
#define ESC_DUMP_MAX 768
#define SGR_BUFSIZ 64
#define MAX_REPORT 1024

#define CSI "\x9B"
#define OSC "\x9D"
#define DCS "\x90"
#define ESC "\x1B"
#define ST "\x9C"

#define TABSR_INIT_CAP 48
#define TABSR_CAP_STEP(x) (4*(x)/3)
#define TABSR_MAX_ENTRY 6

#define STR_CAP_STEP(x) (4*(x)/3)
#define PARAM(i, d) (term->esc.param[i] > 0 ? (uparam_t)term->esc.param[i] : (uparam_t)(d))
#define CHK_VT(v) { if (term->vt_level < (v)) break; }

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
    bool focused : 1;
    bool reverse_video : 1;
    bool sixel : 1;
    bool eight_bit : 1;
    bool protected : 1;
    bool track_focus : 1;
    bool scroll_on_output : 1;
    bool no_scroll_on_input : 1;
    bool columns_132 : 1;
    bool preserve_display_132 : 1;
    bool enable_columns_132 : 1;
    bool enable_nrcs : 1;
    bool title_set_utf8 : 1;
    bool title_query_utf8 : 1;
    bool title_set_hex : 1;
    bool title_query_hex : 1;
    bool bracketed_paste : 1;
    bool keep_clipboard : 1;
    bool led_num_lock : 1;
    bool led_caps_lock : 1;
    bool led_scroll_lock : 1;
    bool allow_change_font : 1;
    bool paste_quote : 1;
    bool paste_literal_nl : 1;
    bool bell_raise : 1;
    bool bell_urgent : 1;
    bool altscreen_scroll : 1;
    bool utf8 : 1;
};

/* Parsing state
 * IMPORTANT: Elements should not be
 * rearranged, state machine will break
 * */
enum escape_state {
    esc_ground,
    esc_esc_entry, esc_esc_1, esc_esc_2, esc_esc_ignore,
    esc_csi_entry, esc_csi_0, esc_csi_1, esc_csi_2, esc_csi_ignore,
    esc_dcs_entry, esc_dcs_0, esc_dcs_1, esc_dcs_2,
    esc_osc_entry, esc_osc_1, esc_osc_2, esc_osc_string,
    esc_dcs_string,
    esc_ign_entry, esc_ign_string,
    esc_vt52_entry, esc_vt52_cup_0, esc_vt52_cup_1,
};

struct term {
    struct screen scr;

    /* VT52 mode saves current cursor G0-G3,GL,GR and origin
     * state, and current terminal mode, and restores on exit */
    struct cursor vt52_saved_c;
    struct term_mode vt52_saved_mode;
    struct screen_mode vt52_saved_screen_mode;

    /* If this flag is set application
     * already knows that terminal window
     * would be resized, so it would be
     * incorrect to read from terminal before
     * resize */
    bool requested_resize;

    /* OSC 52 character description
     * of selection being pasted from */
    uint8_t paste_from;

    /* Mouse state */
    struct mouse_state mstate;
    /* Keyboard state */
    struct keyboard_state kstate;
    /* TTY State, input buffer */
    struct tty tty;

    /* Terminal modes, most of DECSET/DECRST and RM/SM modes
     * are stored here */
    struct term_mode mode;

    /* XTerm checksum extension flags */
    struct checksum_mode checksum_mode;

#if USE_URI
    /* URI match state for URI autodetection */
    struct uri_match_state uri_match;
    ssize_t shift_bookmark;
#endif

    /* Bell volume */
    uint8_t bvol;

    /* This is compressed bit array,
     * that stores encoded modes for
     * XTSAVE/XTRESTORE
     * [0-96] : 12 bytes;
     * [1000-1063] : 8 bytes;
     * [2000-2007] : 1 byte */
    uint8_t saved_modbits[21];
    /* Other parts of saved state for XTSAVE/XTRESTORE */
    enum mouse_mode saved_mouse_mode;
    enum mouse_format saved_mouse_format;
    uint8_t saved_keyboard_type;

    struct escape {
        /* Parser state */
        enum escape_state state;
        /* Encoded escape sequence description
         * that allows to dispatch in one 'switch'*/
        uparam_t selector;

        /* Saved state/selector, used for ST interpretation */
        enum escape_state old_state;
        uparam_t old_selector;

        /* CSI/DCS argument count and arguments */
        size_t i;
        iparam_t param[ESC_MAX_PARAM];
        /* IMPORTANT: subpar_mask must contain
         * at least ESC_MAX_PARAM bits */
        uint32_t subpar_mask;

        /* Length of OSC/DCS string part */
        size_t str_len;
        /* Storage capacity */
        size_t str_cap;
        /* Short strings are not allocated,
         * but long strings are */
        uint8_t str_data[ESC_MAX_STR + 1];
        uint8_t *str_ptr;
    } esc;

    /* Emulated VT version, like 420 */
    uint16_t vt_version;
    /* Emulation level, usually vt_version/100 */
    uint16_t vt_level;

    /* Global color palette */
    color_t palette[PALETTE_SIZE];
};

inline static void term_esc_start(struct term *term) {
    term->esc.selector = 0;
}

inline static void term_esc_start_seq(struct term *term) {
    memset(term->esc.param, 0xFF, (term->esc.i + 1)*sizeof term->esc.param[0]);

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

static void term_request_resize(struct term *term, int16_t w, int16_t h, bool in_cells) {
    struct window *win = screen_window(&term->scr);
    struct extent cur = window_get_size(win);
    struct extent scr = window_get_screen_size(win);

    if (in_cells) {
        struct extent ce = window_get_cell_size(win);
        struct extent bo = window_get_border(win);
        if (w > 0) w = w * ce.width + bo.width * 2;
        if (h > 0) h = h * ce.height + bo.height * 2;
    }

    w = !w ? scr.width : w < 0 ? cur.width : w;
    h = !h ? scr.height : h < 0 ? cur.height : h;

    term->requested_resize |= window_resize(win, w, h);
}

static void term_set_132(struct term *term, bool set) {
    struct screen *scr = &term->scr;
    term->mode.columns_132 = set;
    screen_reset_margins(scr);
    screen_move_to(scr, screen_min_ox(scr), screen_min_oy(scr));
    if (!term->mode.preserve_display_132)
        screen_erase(scr, 0, 0, screen_width(scr), screen_height(scr), false);
    if (window_cfg(screen_window(scr))->allow_window_ops)
        term_request_resize(term, set ? 132 : 80, 24, true);
}

static void term_set_vt52(struct term *term, bool set) {
    /* This thing is mess... */
    struct screen *scr = &term->scr;
    struct screen_mode *smode = &scr->mode;
    if (set) {
        term->kstate.keyboad_vt52 = 1;
        term->vt_level = 0;
        term->vt52_saved_c = *screen_cursor(scr);
        term->vt52_saved_mode = term->mode;
        term->vt52_saved_screen_mode = *smode;

        *screen_cursor(scr) = (struct cursor) {
            .x = screen_cursor(scr)->x,
            .y = screen_cursor(scr)->y,
            .pending = screen_cursor(scr)->pending,
            .gl = 0,
            .gl_ss = 0,
            .gr = 2,
            .gn = { cs94_ascii, cs94_ascii, cs94_ascii, cs94_dec_graph }
        };
        term->mode = (struct term_mode) {
            .focused = term->mode.focused,
            .reverse_video = term->mode.reverse_video,
        };
        *smode  = (struct screen_mode) {
            .wrap = 1
        };
        screen_set_altscreen(&term->scr, 0, 0, 0);
        term_esc_start_seq(term);
    } else {
        term->kstate.keyboad_vt52 = 0;
        term->vt_level = 1;
        struct cursor *c = screen_cursor(scr);
        term->vt52_saved_c.x = c->x;
        term->vt52_saved_c.y = c->y;
        term->vt52_saved_c.pending = c->pending;
        *screen_cursor(scr) = term->vt52_saved_c;

        *smode = term->vt52_saved_screen_mode;
        bool alt = screen_altscreen(scr);
        smode->altscreen = 0;
        term->mode = term->vt52_saved_mode;
        screen_set_altscreen(&term->scr, alt, 0, 0);
    }
}

void term_set_reverse(struct term *term, bool set) {
    if (set ^ term->mode.reverse_video) {
        SWAP(term->palette[SPECIAL_BG], term->palette[SPECIAL_FG]);
        SWAP(term->palette[0], term->palette[7]);
        SWAP(term->palette[8], term->palette[15]);
        SWAP(term->palette[SPECIAL_CURSOR_BG], term->palette[SPECIAL_CURSOR_FG]);
        SWAP(term->palette[SPECIAL_SELECTED_BG], term->palette[SPECIAL_SELECTED_FG]);
        window_set_colors(screen_window(&term->scr), term->palette[SPECIAL_BG], term->palette[SPECIAL_CURSOR_FG]);
    }
    term->mode.reverse_video = set;
}

static void term_load_config(struct term *term, bool reset) {
    struct instance_config *cfg = window_cfg(screen_window(&term->scr));

    if (reset) {
        /* Initialize escape parameters to default values */
        memset(term->esc.param, 0xFF, sizeof term->esc.param);

        term->mstate = (struct mouse_state) {0};

        term->mode = (struct term_mode) {
            .focused = term->mode.focused,
            .title_query_utf8 = cfg->utf8,
            .title_set_utf8 = cfg->utf8,
            .utf8 = cfg->utf8,
            .enable_columns_132 = true,
        };

        term->kstate = (struct keyboard_state) {
            .appcursor = cfg->appcursor,
            .appkey = cfg->appkey,
            .backspace_is_del = cfg->backspace_is_delete,
            .delete_is_del = cfg->delete_is_delete,
            .fkey_inc_step = cfg->fkey_increment,
            .has_meta = cfg->has_meta,
            .keyboard_mapping = cfg->mapping,
            .keylock = cfg->lock,
            .meta_escape = cfg->meta_is_esc,
            .modkey_cursor = cfg->modify_cursor,
            .modkey_fn = cfg->modify_function,
            .modkey_keypad = cfg->modify_keypad,
            .modkey_other = cfg->modify_other,
            .modkey_other_fmt = cfg->modify_other_fmt,
            .modkey_legacy_allow_edit_keypad = cfg->allow_legacy_edit,
            .modkey_legacy_allow_function = cfg->allow_legacy_function,
            .modkey_legacy_allow_keypad = cfg->allow_legacy_keypad,
            .modkey_legacy_allow_misc = cfg->allow_legacy_misc,
            .allow_numlock = cfg->numlock,
        };
    }

    for (size_t i = 0; i < PALETTE_SIZE; i++)
        term->palette[i] = cfg->palette[i];
    term_set_reverse(term, cfg->reverse_video);

    switch(cfg->bell_volume) {
    case 0: term->bvol = 0; break;
    case 1: term->bvol = cfg->bell_low_volume; break;
    case 2: term->bvol = cfg->bell_high_volume;
    }

    term->mode.no_scroll_on_input = !cfg->scroll_on_input;
    term->mode.scroll_on_output = cfg->scroll_on_output;
    term->mode.keep_clipboard = cfg->keep_clipboard;
    term->mode.bell_raise = cfg->raise_on_bell;
    term->mode.bell_urgent = cfg->urgency_on_bell;
}

void term_reload_config(struct term *term) {
    screen_load_config(&term->scr, 0);
    term_load_config(term, 0);
}

static void term_do_reset(struct term *term, bool hard) {
    struct screen *scr = &term->scr;
    struct window *win = screen_window(scr);

    if (term->mode.columns_132)
        term_set_132(term, false);
    screen_set_altscreen(scr, 0, 0, 0);

    ssize_t cx, cy;
    bool cpending;
    screen_store_cursor_position(scr, &cx, &cy, &cpending);

    screen_load_config(scr, true);
    screen_reset_margins(scr);
    screen_reset_tabs(scr);

    term_load_config(term, true);
    keyboard_reset_udk(term);
#if USE_URI
    uri_match_reset(&term->uri_match, false);
#endif

    // TODO Reset cursor shape to default
    window_set_mouse(win, USE_URI);
    window_set_colors(win, term->palette[SPECIAL_BG], term->palette[SPECIAL_CURSOR_FG]);
    window_set_autorepeat(win, window_cfg(win)->autorepeat);

    if (hard) {
        screen_erase(scr, 0, 0, screen_width(scr), screen_height(scr), false);
        screen_free_scrollback(scr, window_cfg(win)->scrollback_size);

        term->vt_version = window_cfg(win)->vt_version;
        term->vt_level = term->vt_version / 100;
        if (!term->vt_level) term_set_vt52(term, 1);

        window_set_title(win, target_icon_label | target_title, NULL, term->mode.title_set_utf8);
    } else {
        screen_load_cursor_position(scr, cx, cy, cpending);
    }

    term->esc.state = esc_ground;
}

static void term_esc_dump(struct term *term, bool use_info) {
    if (use_info && !gconfig.trace_controls) return;

    const char *pref = use_info ? "Seq: " : "Unrecognized ";

    term_esc_str(term)[term->esc.str_len] = 0;

    char buf[ESC_DUMP_MAX] = "^[";
    size_t pos = 2;
    switch (term->esc.state) {
        case esc_esc_entry:
        case esc_esc_1:
            buf[pos++] = 0x20 + ((term->esc.selector & I0_MASK) >> 9) - 1;
            /* fallthrough */
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
                pos += snprintf(buf + pos, ESC_DUMP_MAX - pos, "%u", term->esc.param[i]);
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
        term_answerback(term, CSI">%u;%u;0c", ver, NSST_VERSION);
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

            term_answerback(term, CSI"?%u;1;2;6%s;9;15%sc",
                    60 + term->vt_version/100,
                    term->kstate.keyboard_mapping == keymap_vt220 ? ";8" : "",
                    term->vt_level >= 4 ? ";16;17;18;21;22;28;29" : ";22;29");
        }
    }
}

static void term_dispatch_dsr(struct term *term) {
    struct screen *scr = &term->scr;
    if (term->esc.selector & P_MASK) {
        switch (term->esc.param[0]) {
        case 6: /* DECXCPR -- CSI ? Py ; Px ; R ; 1  */
            term_answerback(term, CSI"?%zu;%zu%sR",
                    screen_cursor_y(scr) - screen_min_oy(scr) + 1,
                    screen_cursor_x(scr) - screen_min_ox(scr) + 1,
                    term->vt_level >= 4 ? ";1" : "");
            break;
        case 15: /* Printer status -- Has printer*/
            CHK_VT(2);
            term_answerback(term, printer_is_available(screen_printer(scr)) ? CSI"?10n" : CSI"?13n");
            break;
        case 25: /* User defined keys lock */
            CHK_VT(2);
            term_answerback(term, CSI"?%un", 20 + term->kstate.udk_locked);
            break;
        case 26: /* Keyboard language -- North American */
            CHK_VT(2);
            term_answerback(term, CSI"?27;1%sn",
                    term->vt_level >= 4 ? ";0;0" : /* ready, LK201 */
                    term->vt_level >= 3 ? ";0" : ""); /* ready */
            break;
        case 53: /* Report locator status */
        case 55:
            CHK_VT(4);
            term_answerback(term, CSI"?53n"); /* Locator available */
            break;
        case 56: /* Report locator type */
            CHK_VT(4);
            term_answerback(term, CSI"?57;1n"); /* Mouse */
            break;
        case 62: /* DECMSR, Macro space -- No data, no space for macros */
            CHK_VT(4);
            term_answerback(term, CSI"0*{");
            break;
        case 63: /* DECCKSR, Memory checksum -- 0000 (hex) */
            CHK_VT(4);
            term_answerback(term, DCS"%u!~0000"ST, PARAM(1, 0));
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
            term_answerback(term, CSI"%zu;%zuR",
                    screen_cursor_y(scr) - screen_min_oy(scr) + 1,
                    screen_cursor_x(scr) - screen_min_ox(scr) + 1);
            break;
        }
    }
}

inline static bool term_parse_cursor_report(struct term *term, char *dstr) {
    struct screen *scr = &term->scr;
    /* Cursor Y */
    ssize_t y = strtoul(dstr, &dstr, 10);
    if (!dstr || *dstr++ != ';') return 0;

    /* Cursor X */
    ssize_t x = strtoul(dstr, &dstr, 10);
    if (!dstr || *dstr++ != ';') return 0;

    /* Page, always '1' */
    if (*dstr++ != '1' || *dstr++ != ';') return 0;

    /* SGR */
    char sgr0 = *dstr++, sgr1 = 0x40;
    if ((sgr0 & 0xD0) != 0x40) return 0;

    /* Optional extended byte */
    if (sgr0 & 0x20) sgr1 = *dstr++;
    if ((sgr1 & 0xF0) != 0x40 || *dstr++ != ';') return 0;

    /* Protection */
    char prot = *dstr++;
    if ((prot & 0xFE) != 0x40 || *dstr++ != ';') return 0;

    /* Flags */
    char flags = *dstr++;
    if ((flags & 0xF0) != 0x40 || *dstr++ != ';') return 0;

    /* GL */
    unsigned long gl = strtoul(dstr, &dstr, 10);
    if (!dstr || *dstr++ != ';'|| gl > 3) return 0;

    /* GR */
    unsigned long gr = strtoul(dstr, &dstr, 10);
    if (!dstr || *dstr++ != ';' || gr > 3) return 0;

    /* G0 - G3 sizes */
    char c96 = *dstr++;
    if ((flags & 0xF0) != 0x40 || *dstr++ != ';') return 0;

    /* G0 - G3 */
    enum charset gn[4];
    for (size_t i = 0; i < 4; i++) {
        uparam_t sel = 0;
        char c;
        if ((c = *dstr++) < 0x30) {
            sel |= I1(c);
            if ((c = *dstr++) < 0x30) return 0;
        }
        sel |= E(c);
        if ((gn[i] = nrcs_parse(sel, (c96 >> i) & 1,
                term->vt_level, term->mode.enable_nrcs)) == nrcs_invalid) return 0;
    }

    /* Everything is OK, load */
    *screen_cursor(scr) = (struct cursor) {
        .x = MIN(x, screen_width(scr) - 1),
        .y = MIN(y, screen_height(scr) - 1),
        .origin = flags & 1,
        .pending = !!(flags & 8),
        .gn = { gn[0], gn[1], gn[2], gn[3] },
        .gl = gl,
        .gr = gr,
        .gl_ss = flags & 4 ? 3 : flags & 2 ? 2 : screen_cursor(scr)->gl
    };

#if USE_URI
    uri_unref(screen_sgr(scr)->uri);
#endif
    *screen_sgr(scr) = (struct attr) {
        .fg = screen_sgr(scr)->fg,
        .bg = screen_sgr(scr)->bg,
        .ul = screen_sgr(scr)->ul,
        .uri = screen_sgr(scr)->uri,
        .protected = prot & 1,
        .bold = sgr0 & 1,
        .underlined = sgr0 & 2,
        .blink = sgr0 & 4,
        .reverse = sgr0 & 8,
        .italic = sgr1 & 1,
        .faint = sgr1 & 2,
        .strikethrough = sgr1 & 4,
        .invisible = sgr1 & 8,
    };

    return 1;
}

inline static bool term_parse_tabs_report(struct term *term, const uint8_t *dstr, const uint8_t *dend) {
    screen_clear_tabs(&term->scr);
    for (ssize_t tab = 0; dstr <= dend; dstr++) {
        if (*dstr == '/' || dstr == dend) {
            if (tab - 1 < screen_width(&term->scr))
                screen_set_tab(&term->scr, tab - 1, 1);
            tab = 0;
        } else if (isdigit(*dstr)) {
            tab = 10 * tab + *dstr - '0';
        } else return 0;
    }
    return 1;
}

static void term_dispatch_dcs(struct term *term) {
    struct screen *scr = &term->scr;

    /* Fixup parameter count */
    term->esc.i += term->esc.param[term->esc.i] >= 0;

    if (term->esc.state != esc_dcs_string) {
        term->esc.selector = term->esc.old_selector;
        term->esc.state = term->esc.old_state;
    }

    term_esc_dump(term, 1);

    /* Only SGR is allowed to have subparams */
    if (term->esc.subpar_mask) {
        term_esc_dump(term, 0);
        goto finish;
    }

    uint8_t *dstr = term_esc_str(term);
    uint8_t *dend = dstr + term->esc.str_len;

    switch (term->esc.selector) {
    case C('s') | P('='): /* iTerm2 synchronized updates */
        switch (PARAM(0,0)) {
        case 1: /* Begin synchronous update */
            window_set_sync(screen_window(scr), 1);
            break;
        case 2: /* End synchronous update */
            window_set_sync(screen_window(scr), 0);
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
                encode_sgr(sgr, sgr + sizeof sgr, screen_sgr(scr));
                term_answerback(term, DCS"1$r%sm"ST, sgr);
                break;
            }
            case 'r': /* -> DECSTBM */
                term_answerback(term, DCS"1$r%zu;%zur"ST,
                                screen_min_y(scr) + 1, screen_max_y(scr));
                break;
            case 's': /* -> DECSLRM */
                term_answerback(term, term->vt_level >= 4 ? DCS"1$r%zu;%zus"ST : DCS"0$r"ST,
                                screen_min_x(scr) + 1, screen_max_x(scr));
                break;
            case 't': /* -> DECSLPP */
                /* Can't report less than 24 lines */
                term_answerback(term, DCS"1$r%zut"ST, MAX(screen_height(scr), 24));
                break;
            case '|' << 8 | '$': /* -> DECSCPP */
                /* It should be either 80 or 132 despite actual column count
                 * New apps use ioctl(TIOGWINSZ, ...) instead */
                term_answerback(term, DCS"1$r%u$|"ST, term->mode.columns_132 ? 132 : 80);
                break;
            case 'q' << 8 | '"': /* -> DECSCA */
                term_answerback(term, DCS"1$r%u\"q"ST, 2 - (screen_sgr(scr)->protected && !term->mode.protected));
                break;
            case 'q' << 8 | ' ': /* -> DECSCUSR */
                term_answerback(term, DCS"1$r%u q"ST, window_cfg(screen_window(scr))->cursor_shape);
                break;
            case '|' << 8 | '*': /* -> DECSLNS */
                term_answerback(term, DCS"1$r%zu*|"ST, screen_height(scr));
                break;
            case 'x' << 8 | '*': /* -> DECSACE */;
                struct screen_mode *smode = &scr->mode;
                term_answerback(term, term->vt_level < 4 ? DCS"0$r"ST :
                        DCS"1$r%u*x"ST, smode->attr_ext_rectangle + 1);
                break;
            case 'p' << 8 | '"': /* -> DECSCL */
                term_answerback(term, DCS"1$r%u%s\"p"ST, 60 + MAX(term->vt_level, 1),
                        term->vt_level >= 2 ? (term->mode.eight_bit ? ";2" : ";1") : "");
                break;
            case 't' << 8 | ' ': /* -> DECSWBV */ {
                uparam_t val = 8;
                if (term->bvol == window_cfg(screen_window(scr))->bell_low_volume) val = 4;
                else if (!term->bvol) val = 0;
                term_answerback(term, DCS"1$r%u t"ST, val);
                break;
            }
            case 'u' << 8 | ' ': /* -> DECSMBV */ {
                uparam_t val = screen_get_margin_bell_volume(scr) * 4;
                term_answerback(term, DCS"1$r%u u"ST, val);
                break;
            }
            default:
                /* Invalid request */
                term_answerback(term, DCS"0$r"ST);
                term_esc_dump(term, 0);
            }
        } else term_esc_dump(term, 0);
        break;
    }
    case C('t') | I0('$'): /* DECRSPS */
        switch(PARAM(0, 0)) {
        case 1: /* <- DECCIR */
            if (!term_parse_cursor_report(term, (char *)term_esc_str(term)))
                term_esc_dump(term, 0);
            break;
        case 2: /* <- DECTABSR */
            if (!term_parse_tabs_report(term, term_esc_str(term),
                    term_esc_str(term) + term->esc.str_len))
                 term_esc_dump(term, 0);
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
        enum charset cs = nrcs_parse(sel, PARAM(0, 0), term->vt_level, term->mode.enable_nrcs);
        if (cs != nrcs_invalid) screen_set_upcs(scr, cs);
        break;
    }
    case C('q') | I0('+'): /* XTGETTCAP */ {
        // TODO Termcap: Support proper tcap db
        //      for now, just implement Co/colors
        bool valid = 0;
        if (!strcmp((char *)dstr, "436F") || /* "Co" */
                !strcmp((char *)dstr, "636F6C6F7266")) { /* "colors" */
            uint8_t tmp[16];
            int len = snprintf((char *)tmp, sizeof tmp, "%u", PALETTE_SIZE - SPECIAL_PALETTE_SIZE);
            *dend = '=';
            hex_encode(dend + 1, tmp, tmp + len);
            valid = 1;
        }
        term_answerback(term, DCS"%u+r%s"ST, valid, dstr);
        break;
    }
    //case C('p') | I0('+'): /* XTSETTCAP */ // TODO Termcap
    //    break;
    default:
        term_esc_dump(term, 0);
    }

finish:
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
        return clip_invalid;
    }
}

static uint32_t selector_to_cid(uint32_t sel, bool rev) {
    switch(sel) {
    case 10: return rev ? SPECIAL_BG : SPECIAL_FG;
    case 11: return rev ? SPECIAL_FG : SPECIAL_BG;
    case 12: return rev ? SPECIAL_CURSOR_BG : SPECIAL_CURSOR_FG;
    case 17: return rev ? SPECIAL_SELECTED_FG : SPECIAL_SELECTED_BG;
    case 19: return rev ? SPECIAL_SELECTED_BG : SPECIAL_SELECTED_FG;
    }
    warn("Unreachable");
    return 0;
}

static void term_colors_changed(struct term *term, uint32_t sel, color_t col) {
    struct window *win = screen_window(&term->scr);
    switch(sel) {
    case 10:
        if(term->mode.reverse_video)
            window_set_colors(win, col, 0);
        break;
    case 11:
        if(!(term->mode.reverse_video))
            window_set_colors(win, term->palette[SPECIAL_CURSOR_BG] = col, 0);
        else
            window_set_colors(win, 0, term->palette[SPECIAL_CURSOR_FG] = col);
        break;
    case 12:
        if(!(term->mode.reverse_video))
            window_set_colors(win, 0, col);
        break;
    case 17: case 19:
        screen_damage_selection(&term->scr);
    }
}

static void term_do_set_color(struct term *term, uint32_t sel, uint8_t *dstr, uint8_t *dend) {
    color_t col;
    uint32_t cid = selector_to_cid(sel, term->mode.reverse_video);

    if (!strcmp((char *)dstr, "?")) {
        col = term->palette[cid];

        term_answerback(term, OSC"%u;rgb:%04x/%04x/%04x"ST, sel,
               color_r(col) * 0x101, color_g(col) * 0x101, color_b(col) * 0x101);
    } else if ((col = parse_color(dstr, dend))) {
        term->palette[cid] = col;

        term_colors_changed(term, sel, col);
    } else term_esc_dump(term, 0);
}

static void term_do_reset_color(struct term *term) {
    uint32_t cid = selector_to_cid(term->esc.selector - 100, term->mode.reverse_video);

    color_t *palette = window_cfg(screen_window(&term->scr))->palette;
    term->palette[cid] = palette[selector_to_cid(term->esc.selector - 100, 0)];
    term_colors_changed(term, term->esc.selector - 100, term->esc.selector == 111 ?
            palette[SPECIAL_CURSOR_BG] : term->palette[cid]);
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
    struct screen *scr = &term->scr;
    struct instance_config *cfg = window_cfg(screen_window(scr));

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
        if (term->mode.title_set_utf8 && !term->mode.utf8) {
            uint8_t *dst = dstr;
            const uint8_t *ptr = dst;
            uint32_t val = 0;
            while (*ptr && utf8_decode(&val, &ptr, dend))
                *dst++ = val;
            *dst = '\0';
        } else if (!term->mode.title_set_utf8 && term->mode.utf8) {
            res = xalloc(term->esc.str_len * 2 + 1);
            uint8_t *ptr = res, *src = dstr;
            if (res) {
                while (*src) ptr += utf8_encode(*src++, ptr, res + term->esc.str_len * 2);
                *ptr = '\0';
                dstr = res;
            }
        }
        window_set_title(screen_window(scr), 3 - term->esc.selector, (char *)dstr, term->mode.utf8);
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
                if (term->mode.reverse_video) switch(idx) {
                    case 0: idx = 7; break;
                    case 7: idx = 0; break;
                    case 8: idx =15; break;
                    case 15:idx = 8; break;
                }
                if (parg[0] == '?' && parg[1] == '\0') {
                    term_answerback(term, OSC"%u;%lu;rgb:%04x/%04x/%04x"ST, term->esc.selector,
                            idx - (term->esc.selector == 5) * SPECIAL_BOLD, color_r(term->palette[idx]) * 0x101,
                            color_g(term->palette[idx]) * 0x101, color_b(term->palette[idx]) * 0x101);
                } else if ((col = parse_color(parg, pnext))) {
                    term->palette[idx] = col;
                } else {
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
                if (term->mode.reverse_video) switch(idx) {
                    case 0: idx = 7; break;
                    case 7: idx = 0; break;
                    case 8: idx =15; break;
                    case 15:idx = 8; break;
                }
                if (!errno && !*s_end && s_end != dstr && idx < PALETTE_SIZE - SPECIAL_PALETTE_SIZE + 5)
                    term->palette[idx] = cfg->palette[idx];
                else term_esc_dump(term, 0);
                if (pnext != dend) *pnext = ';';
                dstr = pnext + 1;
            } while (pnext != dend);
        } else {
            for (size_t i = 0; i < PALETTE_SIZE - SPECIAL_PALETTE_SIZE + 5; i++)
                term->palette[i] = cfg->palette[i];
        }
        break;
    }
    case 6:
    case 106: /* Enable/disable special color */ {
        ssize_t n;
        uparam_t idx, val;
        if (sscanf((char *)dstr, "%"SCNparam";%"SCNparam"%zn", &idx, &val, &n) == 2 && n == dend - dstr && idx < 5) {
            switch (idx) {
            case 0: cfg->special_bold = val; break;
            case 1: cfg->special_blink = val; break;
            case 2: cfg->special_underline = val; break;
            case 3: cfg->special_italic = val; break;
            case 4: cfg->special_reverse = val; break;
            }
        }
        else term_esc_dump(term, 0);
        break;
    }
    case 7: /* Specify current directory */ {
        const char *path = (char *)dstr;
        const size_t filelen = sizeof "file://" - 1;
        bool valid = 1;
        if (!strncmp(path, "file://", filelen)) {
            const char *pathstart = path + filelen;
            path = strchr(pathstart, '/');
            /* Only support local host paths */
            valid = path && (path == pathstart || !strncmp(gconfig.hostname, pathstart, path - pathstart));
        } else {
            /* No relative paths allowed */
            valid = (*dstr == '/');
        }
        if (valid) {
            struct stat stt;
            /* No symlinks... */
            valid = !stat(path, &stt) &&
                    (stt.st_mode & S_IFMT) == S_IFDIR;
        }
        if (valid) set_option_entry(cfg, find_option_entry("cwd", true), path, 0);
        else term_esc_dump(term, 0);
        break;
    }
#if USE_URI
    case 8: /* Set current URI */ {
        char *uri = strchr((char *)dstr, ';');
        if (!uri || !(cfg->uri_mode != uri_mode_off)) {
            if (!uri) term_esc_dump(term, 0);
            break;
        }
        *uri++ = '\0';

        /* Parse attributes */
        char *val, *attr = (char *)dstr;
        char *id = NULL, *id_end = NULL;
        while (attr && *attr) {
            if (!(val = strchr(attr, '='))) break;
            /* At the moment, only id is handled */
            if (!strncmp(attr, "id=", sizeof("id=") - 1)) {
                id = val + 1;
                id_end = attr = strchr(val, ':');
                if (id_end)
                    *id_end = '\0';
            } else attr = strchr(val, ':');
        }

        uri_unref(screen_sgr(scr)->uri);
        screen_sgr(scr)->uri = uri_add(uri, id);
        if (id_end) id_end[-1] = ':';
        uri[-1] = ';';
        break;
    }
#endif
    case 10: /* Set VT100 foreground color */ {
        /* OSC 10 can also have second argument that works as OSC 11 */
        uint8_t *str2;
        if ((str2 = memchr(dstr, ';', dend - dstr))) {
            *str2 = '\0';
            term_do_set_color(term, 10, dstr, str2);
            *str2 = ';';
            dstr = str2 + 1;
            term->esc.selector++;
        }
        /* fallthrough */
    case 11: /* Set VT100 background color */
    case 12: /* Set Cursor color */
    case 17: /* Set Highlight background color */
    case 19: /* Set Highlight foreground color */
        term_do_set_color(term, term->esc.selector, dstr, dend);
        break;
    }
    case 110: /* Reset VT100 foreground color */
    case 111: /* Reset VT100 background color */
    case 112: /* Reset Cursor color */
    case 117: /* Reset Highlight background color */
    case 119: /* Reset Highlight foreground color */
        term_do_reset_color(term);
        break;
    case 52: /* Manipulate selecion data */ {
        if (!cfg->allow_window_ops) break;

        enum clip_target ts[clip_MAX] = {0};
        bool toclip = screen_selection(scr)->select_to_clipboard;
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
                window_paste_clip(screen_window(scr), decode_target(letter, toclip));
            } else {
                if (base64_decode(parg, parg, dend) != dend) parg = NULL;
                for (ssize_t i = 0; i < clip_MAX; i++) {
                    if (ts[i]) {
                        if (i == screen_selection(scr)->targ) screen_selection(scr)->targ = clip_invalid;
                        window_set_clip(screen_window(scr), parg ? (uint8_t *)strdup((char *)parg) : parg, CLIP_TIME_NOW, i);
                    }
                }
            }
        } else term_esc_dump(term, 0);
        break;
    }
    case 133: /* Shell integration */ {
        if (dstr[1] && dstr[1] != ';') {
            term_esc_dump(term, 0);
            break;
        }
        switch (dstr[0]) {
        case 'A': /* Prompt start */
        case 'D': /* Command finished */
            /* Make sure shell plays well with re-wraping */
            screen_ensure_new_paragaph(scr);
            if (dstr[0] == 'A')
                screen_cursor_line_set_prompt(scr);
            break;
        case 'B': /* Command start */
        case 'C': /* Command executed */
            /* nothing */
            break;
        default:
            term_esc_dump(term, 0);
        }
        break;
    }
    case 13001: /* Select background alpha */ {
        errno = 0;
        double res = strtod((char *)dstr, (char **)&dstr);
        if (errno || *dstr) {
            term_esc_dump(term, 0);
            break;
        }
        /* For compatibility with older versions,
         * if res is not normalized, assume it to be 8-bit value. */
        if (res > 1) res /= 255;
        window_set_alpha(screen_window(scr), res);
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
    struct screen *scr = &term->scr;
    struct screen_mode *smode = &scr->mode;
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
            if (term->mode.enable_columns_132)
                term_set_132(term, set);
            break;
        case 4: /* DECSCLM */
            smode->smooth_scroll = set;
            break;
        case 5: /* DECSCNM */
            term_set_reverse(term, set);
            break;
        case 6: /* DECCOM */
            screen_set_origin(scr, set);
            break;
        case 7: /* DECAWM */
            smode->wrap = set;
            screen_reset_pending(scr);
            break;
        case 8: /* DECARM */
            window_set_autorepeat(screen_window(scr), set);
            break;
        case 9: /* X10 Mouse tracking */
            window_set_mouse(screen_window(scr), USE_URI);
            term->mstate.mouse_mode = set ? mouse_mode_x10 : mouse_mode_none;
            break;
        case 10: /* Show toolbar */
            /* IGNORE - There is no toolbar */
            break;
        case 12: /* Start blinking cursor */;
            enum cursor_type *shp = &window_cfg(screen_window(scr))->cursor_shape;
            *shp = ((*shp + 1) & ~1) - set;
            break;
        case 13: /* Start blinking cursor (menu item) */
        case 14: /* Enable XOR of controll sequence and menu for blinking */
            /* IGNORE */
            break;
        case 18: /* DECPFF */
            smode->print_form_feed = set;
            break;
        case 19: /* DECREX */
            smode->print_extend = set;
            break;
        case 25: /* DECTCEM */
            screen_damage_cursor(scr);
            smode->hide_cursor = !set;
            break;
        case 30: /* Show scrollbar */
            /* IGNORE - There is no scrollbar */
            break;
        case 35: /* URXVT Allow change font */
            term->mode.allow_change_font = set;
            break;
        case 40: /* 132COLS */
            term->mode.enable_columns_132 = set;
            break;
        case 41: /* XTerm more(1) hack */
            smode->xterm_more_hack = set;
            break;
        case 42: /* DECNRCM */
            CHK_VT(3);
            term->mode.enable_nrcs = set;
            break;
        case 44: /* Margin bell */
            smode->margin_bell = set;
            break;
        case 45: /* Reverse wrap */
            smode->reverse_wrap = set;
            break;
        case 47: /* Enable altscreen */
            screen_set_altscreen(scr, set, 0, 0);
            break;
        case 66: /* DECNKM */
            term->kstate.appkey = set;
            break;
        case 67: /* DECBKM */
            term->kstate.backspace_is_del = !set;
            break;
        case 69: /* DECLRMM */
            CHK_VT(4);
            smode->lr_margins = set;
            break;
        //case 80: /* DECSDM */ //TODO SIXEL
        //    break;
        case 95: /* DECNCSM */
            CHK_VT(5);
            term->mode.preserve_display_132 = set;
            break;
        case 1000: /* X11 Mouse tracking */
            window_set_mouse(screen_window(scr), USE_URI);
            term->mstate.mouse_mode = set ? mouse_mode_button : mouse_mode_none;
            break;
        case 1001: /* Highlight mouse tracking */
            /* IGNORE */
            break;
        case 1002: /* Cell motion mouse tracking on keydown */
            window_set_mouse(screen_window(scr), USE_URI);
            term->mstate.mouse_mode = set ? mouse_mode_drag : mouse_mode_none;
            break;
        case 1003: /* All motion mouse tracking */
            window_set_mouse(screen_window(scr), set || USE_URI);
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
            screen_selection(scr)->keep_selection = set;
            break;
        case 1041: /* Use CLIPBOARD instead of PRIMARY */
            screen_selection(scr)->select_to_clipboard = set;
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
            if (!set) screen_set_altscreen(scr, 0, 0, 0);
            smode->disable_altscreen = !set;
            break;
        case 1047: /* Enable altscreen and clear screen */
            screen_set_altscreen(scr, set, 1, 0);
            break;
        case 1048: /* Save cursor  */
            screen_save_cursor(scr, set);
            break;
        case 1049: /* Save cursor and switch to altscreen */
            screen_set_altscreen(scr, set, 1, 1);
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
        case 2026: /* Synchronized updates */
            window_set_sync(screen_window(scr), set);
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
            smode->insert = set;
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
    struct screen *scr = &term->scr;
    struct screen_mode *smode = &scr->mode;
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
            val = MODSTATE(smode->smooth_scroll);
            break;
        case 5: /* DECCNM */
            val = MODSTATE(term->mode.reverse_video);
            break;
        case 6: /* DECCOM */
            val = MODSTATE(screen_cursor(scr)->origin);
            break;
        case 7: /* DECAWM */
            val = MODSTATE(smode->wrap);
            break;
        case 8: /* DECARM */
            val = MODSTATE(window_get_autorepeat(screen_window(scr)));
            break;
        case 9: /* X10 Mouse */
            val = MODSTATE(term->mstate.mouse_mode == mouse_mode_x10);
            break;
        case 10: /* Show toolbar */
            val = modstate_aways_disabled;
            break;
        case 12: /* Start blinking cursor */
            val = MODSTATE(window_cfg(screen_window(scr))->cursor_shape & 1);
            break;
        case 13: /* Start blinking cursor (menu item) */
        case 14: /* Enable XORG of control sequence and menu for blinking */
            val = modstate_aways_disabled;
            break;
        case 18: /* DECPFF */
            val = MODSTATE(smode->print_form_feed);
            break;
        case 19: /* DECREX */
            val = MODSTATE(smode->print_extend);
            break;
        case 25: /* DECTCEM */
            val = MODSTATE(!smode->hide_cursor);
            break;
        case 30: /* Show scrollbar */
            val = modstate_aways_disabled;
            break;
        case 35: /* URXVT Allow change font */
            val = MODSTATE(term->mode.allow_change_font);
            break;
        case 40: /* 132COLS */
            val = MODSTATE(term->mode.enable_columns_132);
            break;
        case 41: /* XTerm more(1) hack */
            val = MODSTATE(smode->xterm_more_hack);
            break;
        case 42: /* DECNRCM */
            val = MODSTATE(term->mode.enable_nrcs);
            break;
        case 44: /* Margin bell */
            val = MODSTATE(smode->margin_bell);
            break;
        case 45: /* Reverse wrap */
            val = MODSTATE(smode->reverse_wrap);
            break;
        case 47: /* Enable altscreen */
            val = MODSTATE(smode->altscreen);
            break;
        case 66: /* DECNKM */
            val = MODSTATE(term->kstate.appkey);
            break;
        case 67: /* DECBKM */
            val = MODSTATE(!term->kstate.backspace_is_del);
            break;
        case 69: /* DECLRMM */
            val = MODSTATE(smode->lr_margins);
            break;
        case 80: /* DECSDM */ // TODO SIXEL
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
            val = MODSTATE(screen_selection(scr)->keep_selection);
            break;
        case 1041: /* Use CLIPBOARD instead of PRIMARY */
            val = MODSTATE(screen_selection(scr)->select_to_clipboard);
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
            val = MODSTATE(!smode->disable_altscreen);
            break;
        case 1047: /* Enable altscreen and clear screen */
            val = MODSTATE(smode->altscreen);
            break;
        case 1048: /* Save cursor */
            val = modstate_aways_enabled;
            break;
        case 1049: /* Save cursor and switch to altscreen */
            val = MODSTATE(smode->altscreen);
            break;
        case 1050: /* termcap function keys */
            val = modstate_aways_disabled; // TODO Termcap
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
        case 2026: /* Synchronized update */
            val = MODSTATE(window_get_sync(screen_window(scr)));
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
            val = MODSTATE(smode->insert); /* reset/set */
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

static void term_dispatch_mc(struct term *term, bool private, uparam_t func) {
    struct screen *scr = &term->scr;
    struct screen_mode *smode = &scr->mode;
    struct printer *pr = screen_printer(scr);
    if (private) {
        switch (func) {
        case 1: /* Print current line */
            screen_print_cursor_line(scr);
            break;
        case 4: /* Disable autoprint */
            smode->print_auto = 0;
            break;
        case 5: /* Enable autoprint */
            smode->print_auto = 1;
            break;
        case 11: /* Print scrollback and screen */
            screen_print_all(scr);
            /* fallthrough */
        case 10: /* Print screen */
            screen_print_screen(scr, 1);
            break;
        default:
            term_esc_dump(term, 0);
        }
    } else {
        switch (func) {
        case 0: /* Print screen */
            screen_print_screen(scr, 0);
            break;
        case 4: /* Disable printer */
            /* Well, this is never reached...
             * but let it be here anyways. */
            pr->print_controller = 0;
            break;
        case 5: /* Enable printer */
            if (printer_is_available(screen_printer(scr))) {
                pr->print_controller = 1;
                pr->state = pr_ground;
                printer_intercept(pr, (const uint8_t **)&term->tty.start, term->tty.end);
            }
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

static void term_dispatch_window_op(struct term *term) {
    struct screen *scr = &term->scr;
    struct window *win = screen_window(scr);
    uparam_t pa = PARAM(0, 24);
    /* Only title operations allowed by default */
    if (!window_cfg(win)->allow_window_ops && (pa < 20 || pa > 23)) return;

    switch (pa) {
    case 1: /* Undo minimize */
        term->requested_resize |= window_action(win, action_restore_minimized);
        break;
    case 2: /* Minimize */
        term->requested_resize |= window_action(win, action_minimize);
        break;
    case 3: /* Move */
        window_move(win, PARAM(1,0), PARAM(2,0));
        break;
    case 4: /* Resize */
    case 8: /* Resize (in cell units) */
        term_request_resize(term, term->esc.param[2], term->esc.param[1], pa == 8);
        break;
    case 5: /* Raise */
        window_action(win, action_raise);
        break;
    case 6: /* Lower */
        window_action(win, action_lower);
        break;
    case 7: /* Refresh */
        screen_damage_lines(scr, 0, screen_height(scr));
        break;
    case 9: /* Maximize operations */ {
        enum window_action act = action_none;

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
        term->requested_resize |= window_action(win, act);
        break;
    }
    case 10: /* Fullscreen operations */ {
        enum window_action act = action_none;

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
        term->requested_resize |= window_action(win, act);
        break;
    }
    case 11: /* Report state */
        term_answerback(term, CSI"%ut", 1 + !window_is_mapped(win));
        break;
    case 13: /* Report position opetations */
        switch(PARAM(1,0)) {
        case 0: /* Report window position */ {
            struct extent ext = window_get_position(win);
            term_answerback(term, CSI"3;%u;%ut", ext.width, ext.height);
            break;
        }
        case 2: /* Report grid position */ {
            struct extent ext = window_get_grid_position(win);
            term_answerback(term, CSI"3;%u;%ut", ext.width, ext.height);
            break;
        }
        default:
            term_esc_dump(term, 0);
        }
        break;
    case 14: /* Report size operations */
        switch(PARAM(1,0)) {
        case 0: /* Report grid size */ {
            struct extent ext = window_get_grid_size(win);
            term_answerback(term, CSI"4;%u;%ut", ext.height, ext.width);
            break;
        };
        case 2: /* Report window size */ {
            struct extent ext = window_get_size(win);
            term_answerback(term, CSI"4;%u;%ut", ext.height, ext.width);
            break;
        }
        default:
            term_esc_dump(term, 0);
        }
        break;
    case 15: /* Report screen size */ {
        struct extent ext = window_get_screen_size(win);
        term_answerback(term, CSI"5;%u;%ut", ext.height, ext.width);
        break;
    }
    case 16: /* Report cell size */ {
        struct extent ext = window_get_cell_size(win);
        term_answerback(term, CSI"6;%u;%ut", ext.height, ext.width);
        break;
    }
    case 18: /* Report grid size (in cell units) */
        term_answerback(term, CSI"8;%zu;%zut", screen_height(scr), screen_width(scr));
        break;
    case 19: /* Report screen size (in cell units) */ {
        struct extent s = window_get_screen_size(win);
        struct extent c = window_get_cell_size(win);
        struct extent b = window_get_border(win);
        term_answerback(term, CSI"9;%u;%ut", (s.height - 2*b.height)/c.height, (s.width - 2*b.width)/c.width);
        break;
    }
    case 20: /* Report icon label */
    case 21: /* Report title */ {
        bool tutf8;
        uint8_t *res = NULL, *res2 = NULL, *tit = NULL;
        window_get_title(win, pa == 20 ? target_icon_label : target_title, (char **)&tit, &tutf8);
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
            title = tit;
        } else if (term->mode.title_query_utf8 && !tutf8) {
            tmp = res2 = xalloc(2 * tlen + 1);
            while (*title) tmp += utf8_encode(*title++, tmp, res2 + 2 * tlen);
            *tmp = '\0';
            tlen = tmp - res2;
            title = res2;
        }
        if (term->mode.title_query_hex) {
            res = xalloc(2 * tlen + 1);
            hex_encode(res, title, title + tlen);
            title = res;
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
            window_push_title(win, 3 - PARAM(1, 0));
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
            window_pop_title(win, 3 - PARAM(1, 0));
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
    struct attr *sgr = screen_sgr(&term->scr);
    struct cursor *c = screen_cursor(&term->scr);

    char csgr[3] = { 0x40, 0, 0 };
    if (sgr->bold) csgr[0] |= 1;
    if (sgr->underlined) csgr[0] |= 2;
    if (sgr->blink) csgr[0] |= 4;
    if (sgr->reverse) csgr[0] |= 8;

    if (window_cfg(screen_window(&term->scr))->extended_cir) {
        csgr[0] |= 0x20;
        csgr[1] |= 0x40;
        /* Extended byte */
        if (sgr->italic) csgr[1] |= 1;
        if (sgr->faint) csgr[1] |= 2;
        if (sgr->strikethrough) csgr[1] |= 4;
        if (sgr->invisible) csgr[1] |= 8;
    }

    char cflags = 0x40;
    if (c->origin) cflags |= 1; /* origin */
    if (c->gl_ss == 2 && c->gl != 2) cflags |= 2; /* ss2 */
    if (c->gl_ss == 3 && c->gl != 3) cflags |= 4; /* ss3 */
    if (c->pending) cflags |= 8; /* pending wrap */

    char cg96 = 0x40;
    if (nrcs_is_96(c->gn[0])) cg96 |= 1;
    if (nrcs_is_96(c->gn[1])) cg96 |= 2;
    if (nrcs_is_96(c->gn[2])) cg96 |= 4;
    if (nrcs_is_96(c->gn[3])) cg96 |= 8;

    term_answerback(term, DCS"1$u%zu;%zu;1;%s;%c;%c;%u;%u;%c;%s%s%s%s"ST,
        /* line */ c->y + 1,
        /* column */ c->x + 1,
        /* attributes */ csgr,
        /* cell protection */ 0x40 + sgr->protected,
        /* flags */ cflags,
        /* gl */ c->gl,
        /* gr */ c->gr,
        /* cs size */ cg96,
        /* g0 */ nrcs_unparse(c->gn[0]),
        /* g1 */ nrcs_unparse(c->gn[1]),
        /* g2 */ nrcs_unparse(c->gn[2]),
        /* g3 */ nrcs_unparse(c->gn[3]));
}

static void term_report_tabs(struct term *term) {
    size_t caps = TABSR_INIT_CAP, len = 0;
    char *tabs = xalloc(caps);

    for (int16_t i = 0; tabs && i < screen_width(&term->scr); i++) {
        if (screen_has_tab(&term->scr, i)) {
            if (len + TABSR_MAX_ENTRY > caps) {
                tabs = xrealloc(tabs, caps, TABSR_CAP_STEP(caps));
                caps = TABSR_CAP_STEP(caps);
            }
            len += snprintf(tabs + len, caps, len ? "/%u" : "%u", i + 1);
        }
    }

    term_answerback(term, DCS"2$u%s"ST, tabs ? tabs : "");
}

static size_t term_decode_color(struct term *term, size_t arg, color_t *rc, color_t *valid) {
    ssize_t argc = 0;
    if (arg + 1 >= ESC_MAX_PARAM) goto dump;

    argc = __builtin_ctzl(~(term->esc.subpar_mask >> (arg + 1))) + 1;
    bool has_subpars = argc > 1;
    if (!has_subpars)
        argc = ESC_MAX_PARAM - arg;

    iparam_t type = term->esc.param[arg];
    if (type == 2) {
        if (argc < 4) goto dump;
        if (!has_subpars) argc = 4;

        ssize_t i = arg + 1 + (argc > 4);
        *rc = mk_color(MIN(MAX(0, term->esc.param[i]), 0xFF),
                       MIN(MAX(0, term->esc.param[i + 1]), 0xFF),
                       MIN(MAX(0, term->esc.param[i + 2]), 0xFF), 0xFF);
    } else if (type == 5) {
        if (argc < 2 || term->esc.param[arg + 1] < 0 ||
            term->esc.param[arg + 1] >= PALETTE_SIZE - SPECIAL_PALETTE_SIZE) goto dump;
        if (!has_subpars) argc = 2;

        *rc = indirect_color(term->esc.param[arg + 1]);
    } else {
        argc = 1;
        goto dump;
    }

    *valid = true;
    return argc;

dump:
    term_esc_dump(term, false);
    *valid = false;
    return argc;
}

static void term_decode_sgr(struct term *term, size_t i, struct attr *mask, struct attr *sgr) {
#define SET(f) (mask->f = sgr->f = 1)
#define RESET(f) (mask->f = 1, sgr->f = 0)
#define SETFG(f) (mask->fg = 1, sgr->fg = indirect_color(f))
#define SETBG(f) (mask->bg = 1, sgr->bg = indirect_color(f))
#define SETUL(f) (mask->ul = 1, sgr->ul = indirect_color(f))
    do {
        uparam_t par = PARAM(i, 0);
        if ((term->esc.subpar_mask >> i) & 1) return;
        switch (par) {
        case 0:
            SETFG(SPECIAL_FG);
            SETBG(SPECIAL_BG);
            SETUL(SPECIAL_BG);
            attr_mask_set(sgr, 0);
            attr_mask_set(mask, ATTR_MASK);
            break;
        case 1:  SET(bold); break;
        case 2:  SET(faint); break;
        case 3:  SET(italic); break;
        case 21: /* <- should be double underlind */
            mask->underlined = 1;
            sgr->underlined = 2;
            break;
        case 4:
            if (i < term->esc.i && (term->esc.subpar_mask >> (i + 1)) & 1)
                sgr->underlined = term->esc.param[++i];
            else sgr->underlined = 1;
            mask->underlined = 1;
            break;
        case 5:  /* <- should be slow blink */
        case 6:  SET(blink); break;
        case 7:  SET(reverse); break;
        case 8:  SET(invisible); break;
        case 9:  SET(strikethrough); break;
        case 22: RESET(faint); RESET(bold); break;
        case 23: RESET(italic); break;
        case 24: RESET(underlined); break;
        case 25: /* <- should be slow blink reset */
        case 26: RESET(blink); break;
        case 27: RESET(reverse); break;
        case 28: RESET(invisible); break;
        case 29: RESET(strikethrough); break;
        case 30: case 31: case 32: case 33:
        case 34: case 35: case 36: case 37:
            SETFG(par - 30); break;
        case 38:
            i += term_decode_color(term, i + 1, &sgr->fg, &mask->fg);
            break;
        case 39: SETFG(SPECIAL_FG); break;
        case 40: case 41: case 42: case 43:
        case 44: case 45: case 46: case 47:
            SETBG(par - 40); break;
        case 48:
            i += term_decode_color(term, i + 1, &sgr->bg, &mask->bg);
            break;
        case 49: SETBG(SPECIAL_BG); break;
        case 58:
            i += term_decode_color(term, i + 1, &sgr->ul, &mask->ul);
            break;
        case 59: SETUL(SPECIAL_BG); break;
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
#undef SETUL
}

/* Utility functions for XTSAVE/XTRESTORE */

inline static void store_mode(uint8_t modbits[], uparam_t mode, bool val) {
    if (mode < 96) modbits += mode / 8;
    else if (1000 <= mode && mode < 1064) modbits += mode / 8 - 113;
    else if (2000 <= mode && mode < 2007) modbits += 20;
    else {
        /* Don't save synchronized update state */
        if (mode != 2026)
            warn("Can't save mode %u", mode);
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
        /* Don't restore synchronized update state */
        if (mode != 2026)
            warn("Can't restore mode %u", mode);
        return 0;
    }
    return (*modbits >> (mode % 8)) & 1;
}

static void term_dispatch_csi(struct term *term) {
    /* Fix parameter count up */
    term->esc.i += term->esc.param[term->esc.i] >= 0;
    struct screen *scr = &term->scr;

    term_esc_dump(term, 1);

    /* Only SGR is allowed to have subparams */
    if (term->esc.subpar_mask && term->esc.selector != C('m')) goto finish;

    switch (term->esc.selector) {
    case C('@'): /* ICH */
        screen_insert_cells(scr, PARAM(0, 1));
        break;
    case C('@') | I0(' '): /* SL */
        if (screen_cursor_in_region(scr))
            screen_scroll_horizontal(scr, screen_min_x(scr), PARAM(0, 1));
        break;
    case C('A'): /* CUU */
        (screen_cursor_y(scr) >= screen_min_y(scr) ? screen_bounded_move_to : screen_move_to)
                (scr, screen_cursor_x(scr), screen_cursor_y(scr) - PARAM(0, 1));
        break;
    case C('A') | I0(' '): /* SR */
        if (screen_cursor_in_region(scr))
            screen_scroll_horizontal(scr, screen_min_x(scr), -PARAM(0, 1));
        break;
    case C('B'): /* CUD */
        (screen_cursor_y(scr) < screen_max_y(scr) ? screen_bounded_move_to : screen_move_to)
                (scr, screen_cursor_x(scr), screen_cursor_y(scr) + PARAM(0, 1));
        break;
    case C('e'): /* VPR */
        screen_move_width_origin(scr, screen_cursor_x(scr), screen_cursor_y(scr) + PARAM(0, 1));
        break;
    case C('C'): /* CUF */
        (screen_cursor_x(scr) < screen_max_x(scr) ? screen_bounded_move_to : screen_move_to)
                (scr, screen_cursor_x(scr) + PARAM(0, 1),  screen_cursor_y(scr));
        break;
    case C('D'): /* CUB */
        screen_move_left(scr, PARAM(0, 1));
        break;
    case C('E'): /* CNL */
        (screen_cursor_y(scr) < screen_max_y(scr) ? screen_bounded_move_to : screen_move_to)
                (scr, screen_cursor_x(scr), screen_cursor_y(scr) + PARAM(0, 1));
        screen_cr(scr);
        break;
    case C('F'): /* CPL */
        (screen_cursor_y(scr) >= screen_min_y(scr) ? screen_bounded_move_to : screen_move_to)
                (scr, screen_cursor_x(scr), screen_cursor_y(scr) - PARAM(0, 1));
        screen_cr(scr);
        break;
    case C('`'): /* HPA */
    case C('G'): /* CHA */
        screen_move_width_origin(scr, screen_min_ox(scr) + PARAM(0, 1) - 1, screen_cursor_y(scr));
        break;
    case C('H'): /* CUP */
    case C('f'): /* HVP */
        screen_move_width_origin(scr, screen_min_ox(scr) + PARAM(1, 1) - 1,
                                 screen_min_oy(scr) + PARAM(0, 1) - 1);
        break;
    case C('I'): /* CHT */
        screen_tabs(scr, PARAM(0, 1));
        break;
    case C('J') | P('?'): /* DECSED */
    case C('J'): /* ED */ {
        void (*erase)(struct screen *, int16_t, int16_t, int16_t, int16_t, bool) =
                term->esc.selector & P_MASK ? (term->mode.protected ? screen_erase : screen_selective_erase) :
                term->mode.protected ? screen_protective_erase : screen_erase;
        switch(PARAM(0, 0)) {
        case 0: /* Below */
            screen_cursor_adjust_wide_left(scr);
            erase(scr, screen_cursor_x(scr), screen_cursor_y(scr), screen_width(scr), screen_cursor_y(scr) + 1, false);
            erase(scr, 0, screen_cursor_y(scr) + 1, screen_width(scr), screen_height(scr), false);
            break;
        case 1: /* Above */
            screen_cursor_adjust_wide_right(scr);
            erase(scr, 0, screen_cursor_y(scr), screen_cursor_x(scr) + 1, screen_cursor_y(scr) + 1, false);
            erase(scr, 0, 0, screen_width(scr), screen_cursor_y(scr), false);
            break;
        case 2: /* All */
            erase(scr, 0, 0, screen_width(scr), screen_height(scr), false);
            break;
        case 3: /* Scrollback */
            if (window_cfg(screen_window(scr))->allow_erase_scrollback && !screen_altscreen(scr)) {
                screen_free_scrollback(scr, window_cfg(screen_window(scr))->scrollback_size);
                break;
            }
            /* fallthrough */
        default:
            term_esc_dump(term, 0);
        }
        screen_reset_pending(scr);
        break;
    }
    case C('K') | P('?'): /* DECSEL */
    case C('K'): /* EL */ {
        void (*erase)(struct screen *, int16_t, int16_t, int16_t, int16_t, bool) =
                term->esc.selector & P_MASK ? (term->mode.protected ? screen_erase : screen_selective_erase) :
                term->mode.protected ? screen_protective_erase : screen_erase;
        switch (PARAM(0, 0)) {
        case 0: /* To the right */
            screen_cursor_adjust_wide_left(scr);
            erase(scr, screen_cursor_x(scr), screen_cursor_y(scr), screen_width(scr), screen_cursor_y(scr) + 1, false);
            break;
        case 1: /* To the left */
            screen_cursor_adjust_wide_right(scr);
            erase(scr, 0, screen_cursor_y(scr), screen_cursor_x(scr) + 1, screen_cursor_y(scr) + 1, false);
            break;
        case 2: /* Whole */
            erase(scr, 0, screen_cursor_y(scr), screen_width(scr), screen_cursor_y(scr) + 1, false);
            break;
        default:
            term_esc_dump(term, 0);
        }
        screen_reset_pending(scr);
        break;
    }
    case C('L'): /* IL */
        screen_insert_lines(scr, PARAM(0, 1));
        break;
    case C('M'): /* DL */
        screen_delete_lines(scr, PARAM(0, 1));
        break;
    case C('P'): /* DCH */
        screen_delete_cells(scr, PARAM(0, 1));
        break;
    case C('S'): /* SU */
        screen_scroll(scr, screen_min_y(scr), PARAM(0, 1), 0);
        break;
    case C('T') | P('>'): /* XTRMTITLE */
        term_dispatch_tmode(term, 0);
        break;
    case C('T'): /* SD */
    case C('^'): /* SD */
        screen_scroll(scr, screen_min_y(scr), -PARAM(0, 1), 0);
        break;
    case C('X'): /* ECH */
        (term->mode.protected ? screen_protective_erase : screen_erase)
                (scr, screen_cursor_x(scr), screen_cursor_y(scr), screen_cursor_x(scr) + PARAM(0, 1), screen_cursor_y(scr) + 1, false);
        screen_reset_pending(scr);
        break;
    case C('Z'): /* CBT */
        screen_tabs(scr, -PARAM(0, 1));
        break;
    case C('a'): /* HPR */
        screen_move_width_origin(scr, screen_cursor_x(scr) + PARAM(0, 1), screen_cursor_y(scr) + PARAM(1, 0));
        break;
    case C('b'): /* REP */
        screen_rep(scr, PARAM(0, 1));
        break;
    case C('c'): /* DA1 */
    case C('c') | P('>'): /* DA2 */
    case C('c') | P('='): /* DA3 */
        if (PARAM(0, 0)) break;
        term_dispatch_da(term, term->esc.selector & P_MASK);
        break;
    case C('d'): /* VPA */
        screen_move_width_origin(scr, screen_cursor_x(scr), screen_min_oy(scr) + PARAM(0, 1) - 1);
        break;
    case C('g'): /* TBC */
        switch (PARAM(0, 0)) {
        case 0:
            screen_set_tab(scr, screen_cursor_x(scr), 0);
            break;
        case 3:
            screen_clear_tabs(scr);
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
        term_dispatch_mc(term, term->esc.selector & P_MASK, PARAM(0, 0));
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
            struct instance_config *cfg = window_cfg(screen_window(scr));
            if (inone || p == 0) {
                term->kstate.modkey_legacy_allow_keypad = cfg->allow_legacy_keypad;
                term->kstate.modkey_legacy_allow_edit_keypad = cfg->allow_legacy_edit;
                term->kstate.modkey_legacy_allow_function = cfg->allow_legacy_function;
                term->kstate.modkey_legacy_allow_misc = cfg->allow_legacy_misc;
            }
            if (inone || p == 1) term->kstate.modkey_cursor = cfg->modify_cursor;
            if (inone || p == 2) term->kstate.modkey_fn = cfg->modify_function;
            if (inone || p == 3) term->kstate.modkey_keypad = cfg->modify_keypad;
            if (inone || p == 4) term->kstate.modkey_other = cfg->modify_other;
        }
        break;
    }
    case C('m'): /* SGR */
        term_decode_sgr(term, 0, &(struct attr){0}, screen_sgr(scr));
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
        screen_set_tb_margins(scr, PARAM(0, 1) - 1, PARAM(1, screen_height(scr)) - 1);
        screen_move_to(scr, screen_min_ox(scr), screen_min_oy(scr));
        break;
    case C('s'): /* DECSLRM/(SCOSC) */
        if (screen_set_lr_margins(scr, PARAM(0, 1) - 1, PARAM(1, screen_width(scr)) - 1))
            screen_move_to(scr, screen_min_ox(scr), screen_min_oy(scr));
        else
            screen_save_cursor(scr, 1);
        break;
    case C('t'): /* XTWINOPS, xterm */
        term_dispatch_window_op(term);
        break;
    case C('t') | P('>'):/* XTSMTITLE */
        term_dispatch_tmode(term, 1);
        break;
    case C('u'): /* (SCORC) */
        screen_save_cursor(scr, 0);
        break;
    case C('x'): /* DECREQTPARAM */
        if (term->vt_version < 200) {
            uparam_t p = PARAM(0, 0);
            if (p < 2) term_answerback(term, CSI"%u;1;1;128;128;1;0x", p + 2);
        }
        break;
    case C('q') | I0(' '): /* DECSCUSR */ {
        enum cursor_type csr = PARAM(0, 1);
        if (csr < 7) window_cfg(screen_window(scr))->cursor_shape = csr;
        break;
    }
    case C('p') | I0('!'): /* DECSTR */
        term_do_reset(term, 0);
        break;
    case C('p') | I0('"'): /* DECSCL */
        if (term->vt_version < 200) break;

        term_do_reset(term, true);
        uparam_t p = PARAM(0, 65) - 60;
        if (p && p <= term->vt_version/100)
            term->vt_level = p;
        if (p > 1) switch (PARAM(1, 2)) {
        case 2: term->mode.eight_bit = 1; break;
        case 1: term->mode.eight_bit = 0; break;
        default:
            term->esc.state = esc_csi_2;
            term_esc_dump(term, 0);
        }
        break;
    case C('q') | I0('"'): /* DECSCA */
        switch (PARAM(0, 2)) {
        case 1:
            screen_sgr(scr)->protected = 1;
            break;
        case 0: case 2:
            screen_sgr(scr)->protected = 0;
            break;
        }
        term->mode.protected = 0;
        break;
    case C('p') | I0('$'): /* RQM -> RPM */
        CHK_VT(3);
        term_answerback(term, CSI"%u;%u$y", PARAM(0, 0), term_get_mode(term, 0, PARAM(0, 0)));
        break;
    case C('p') | P('?') | I0('$'): /* DECRQM -> DECRPM */
        CHK_VT(3);
        term_answerback(term, CSI"?%u;%u$y", PARAM(0, 0), term_get_mode(term, 1, PARAM(0, 0)));
        break;
    case C('r') | I0('$'): /* DECCARA */ {
        CHK_VT(4);
        struct attr mask = {0}, sgr = {0};
        term_decode_sgr(term, 4, &mask, &sgr);
        screen_apply_sgr(scr, screen_min_ox(scr) + PARAM(1, 1) - 1, screen_min_oy(scr) + PARAM(0, 1) - 1,
                screen_min_ox(scr) + PARAM(3, screen_max_ox(scr) - screen_min_ox(scr)),
                screen_min_oy(scr) + PARAM(2, screen_max_oy(scr) - screen_min_oy(scr)), &mask, &sgr);
        break;
    }
    case C('t') | I0('$'): /* DECRARA */ {
        CHK_VT(4);
        struct attr mask = {0}, sgr = {0};
        term_decode_sgr(term, 4, &mask, &sgr);
        screen_reverse_sgr(scr, screen_min_ox(scr) + PARAM(1, 1) - 1, screen_min_oy(scr) + PARAM(0, 1) - 1,
                screen_min_ox(scr) + PARAM(3, screen_max_ox(scr) - screen_min_ox(scr)),
                screen_min_oy(scr) + PARAM(2, screen_max_oy(scr) - screen_min_oy(scr)), &mask);
        break;
    }
    case C('v') | I0('$'): /* DECCRA */
        CHK_VT(4);
        screen_copy(scr, screen_min_ox(scr) + PARAM(1, 1) - 1, screen_min_oy(scr) + PARAM(0, 1) - 1,
                screen_min_ox(scr) + PARAM(3, screen_max_ox(scr) - screen_min_ox(scr)),
                screen_min_oy(scr) + PARAM(2, screen_max_oy(scr) - screen_min_oy(scr)),
                screen_min_ox(scr) + PARAM(6, 1) - 1, screen_min_oy(scr) + PARAM(5, 1) - 1, 1);
        break;
    case C('|') | I0('#'): /* XTREPORTSGR */ {
        CHK_VT(4);
        struct attr sgr = screen_common_sgr(scr, screen_min_ox(scr) + PARAM(1, 1) - 1, screen_min_oy(scr) + PARAM(0, 1) - 1,
                screen_min_ox(scr) + PARAM(3, screen_max_ox(scr) - screen_min_ox(scr)),
                screen_min_oy(scr) + PARAM(2, screen_max_oy(scr) - screen_min_oy(scr)));
        char str[SGR_BUFSIZ];
        encode_sgr(str, str + sizeof str, &sgr);
        term_answerback(term, CSI"%sm", str);
        break;
    }
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
        screen_fill(scr, screen_min_ox(scr) + PARAM(2, 1) - 1, screen_min_oy(scr) + PARAM(1, 1) - 1,
                screen_min_ox(scr) + PARAM(4, screen_max_ox(scr) - screen_min_ox(scr)),
                screen_min_oy(scr) + PARAM(3, screen_max_oy(scr) - screen_min_oy(scr)), 1, PARAM(0, 0));
        break;
    case C('z') | I0('$'): /* DECERA */
        CHK_VT(4);
        (term->mode.protected ? screen_protective_erase : screen_erase)
                (scr, screen_min_ox(scr) + PARAM(1, 1) - 1, screen_min_oy(scr) + PARAM(0, 1) - 1,
                screen_min_ox(scr) + PARAM(3, screen_max_ox(scr) - screen_min_ox(scr)),
                screen_min_oy(scr) + PARAM(2, screen_max_oy(scr) - screen_min_oy(scr)), true);
        break;
    case C('{') | I0('$'): /* DECSERA */
        CHK_VT(4);
        (term->mode.protected ? screen_erase : screen_selective_erase)
                (scr, screen_min_ox(scr) + PARAM(1, 1) - 1, screen_min_oy(scr) + PARAM(0, 1) - 1,
                screen_min_ox(scr) + PARAM(3, screen_max_ox(scr) - screen_min_ox(scr)),
                screen_min_oy(scr) + PARAM(2, screen_max_oy(scr) - screen_min_oy(scr)), true);
        break;
    case C('y') | I0('*'): /* DECRQCRA */
        CHK_VT(4);
        uint16_t sum = screen_checksum(scr, screen_min_ox(scr) + PARAM(3, 1) - 1, screen_min_oy(scr) + PARAM(2, 1) - 1,
                screen_min_ox(scr) + PARAM(5, screen_max_ox(scr) - screen_min_ox(scr)),
                screen_min_oy(scr) + PARAM(4, screen_max_oy(scr) - screen_min_oy(scr)), term->checksum_mode, term->mode.enable_nrcs);
        /* DECRPCRA */
        term_answerback(term, DCS"%u!~%04X"ST, PARAM(0, 0), sum);
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
        screen_insert_columns(scr, PARAM(0, 1));
        break;
    case C('~') | I0('\''): /* DECDC */
        CHK_VT(4);
        screen_delete_columns(scr, PARAM(0, 1));
        break;
    case C('x') | I0('*'): /* DECSACE */
        CHK_VT(4);
        struct screen_mode *smode = &scr->mode;
        switch (PARAM(0, 1)) {
        case 1: smode->attr_ext_rectangle = 0; break;
        case 2: smode->attr_ext_rectangle = 1; break;
        default: term_esc_dump(term, 0);
        }
        break;
    case C('|') | I0('$'): /* DECSCPP */
        if (window_cfg(screen_window(scr))->allow_window_ops)
            term_request_resize(term, PARAM(0, 80), -1, 1);
        break;
    case C('|') | I0('*'): /* DECSNLS */
        if (window_cfg(screen_window(scr))->allow_window_ops)
            term_request_resize(term, -1, PARAM(0, 24), 1);
        break;
    case C('W') | P('?'): /* DECST8C */
        if (PARAM(0, 5) == 5) screen_reset_tabs(scr);
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
                    screen_save_cursor(scr, 1);
                    break;
                case 1047: case 1049:
                    mode = 47;
                    /* fallthrough */
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
                window_set_mouse(screen_window(scr), term->mstate.mouse_mode == mouse_mode_motion || USE_URI);
                break;
            case 1050: case 1051: case 1052:
            case 1053: case 1060: case 1061:
                term->kstate.keyboard_mapping = term->saved_keyboard_type;
                break;
            case 1048:
                screen_save_cursor(scr, 0);
                break;
            case 1047: case 1049:
                mode = 47;
                /* fallthrough */
            default:
                term_srm(term, 1, mode, load_mode(term->saved_modbits, mode));
            }
        }
        break;
    case C('u') | I0('&'): /* DECRQUPSS */;
        enum charset upcs = screen_get_upcs(scr);
        term_answerback(term, DCS"%u!u%s"ST, nrcs_is_96(upcs), nrcs_unparse(upcs));
        break;
    case C('t') | I0(' '): /* DECSWBV */
        switch (PARAM(0, 1)) {
        case 1:
            term->bvol = 0;
            break;
        case 2: case 3: case 4:
            term->bvol = window_cfg(screen_window(scr))->bell_low_volume;
            break;
        default:
            term->bvol = window_cfg(screen_window(scr))->bell_high_volume;
        }
        break;
    case C('u') | I0(' '): /* DECSMBV */;
        int16_t vol = PARAM(0, 8);
        screen_set_margin_bell_volume(scr, (vol > 1) + (vol > 4));
        break;
    case C('w') | I0('\''): /* DECEFR */ {
        int16_t x, y;
        window_get_pointer(screen_window(scr), &x, &y, NULL);
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
            /* fallthrough */
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
                /* fallthrough */
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
        window_get_pointer(screen_window(scr), &x, &y, &mask);
        mouse_report_locator(term, 1, x, y, mask);
        break;
    }
    case C('q') | P('>'): /* XTVERSION */
        term_answerback(term, DCS">|%s"ST, version_string());
        break;
    //case C('p') | P('>'): /* XTSMPOINTER */ // TODO Pointer
    //    break;
    //case C('S') | P('?'): /* XTSMSGRAPHICS */ // TODO SIXEL
    //    break;
    //case C('S') | P('>'): /* Set graphics attributes, xterm */ //TODO SIXEL
    //    break;
    default:
        term_esc_dump(term, 0);
    }

finish:
    term->esc.state = esc_ground;
}

static void term_dispatch_esc(struct term *term) {
    if (gconfig.trace_controls) {
        if (term->esc.selector != E('[') && term->esc.selector != E('P') &&
                term->esc.selector != E(']'))
            term_esc_dump(term, 1);
    }

    struct screen *scr = &term->scr;
    switch (term->esc.selector) {
    case E('D'): /* IND */
        screen_index(scr);
        break;
    case E('E'): /* NEL */
        screen_index(scr);
        screen_cr(scr);
        break;
    case E('F'): /* HP Home Down */
        screen_move_to(scr, screen_min_ox(scr), screen_max_oy(scr));
        break;
    case E('H'): /* HTS */
        screen_set_tab(scr, screen_cursor_x(scr), 1);
        break;
    case E('M'): /* RI */
        screen_rindex(scr);
        break;
    case E('N'): /* SS2 */
        screen_set_gl(scr, 2, 1);
        break;
    case E('O'): /* SS3 */
        screen_set_gl(scr, 3, 1);
        break;
    case E('P'): /* DCS */
        term->esc.state = esc_dcs_entry;
        term->esc.old_state = 0;
        return;
    case E('V'): /* SPA */
        screen_sgr(scr)->protected = 1;
        term->mode.protected = 1;
        break;
    case E('W'): /* EPA */
        screen_sgr(scr)->protected = 0;
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
        screen_rindex_horizonal(scr);
        break;
    case E('7'): /* DECSC */
        screen_save_cursor(scr, 1);
        break;
    case E('8'): /* DECRC */
        screen_save_cursor(scr, 0);
        break;
    case E('9'): /* DECFI */
        CHK_VT(4);
        screen_index_horizonal(scr);
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
        screen_set_tb_margins(scr, screen_cursor_y(scr), screen_max_y(scr) - 1);
        break;
    case E('m'): /* HP Memory unlock */
        screen_set_tb_margins(scr, 0, screen_max_y(scr) - 1);
        break;
    case E('n'): /* LS2 */
        screen_set_gl(scr, 2, 0);
        break;
    case E('o'): /* LS3 */
        screen_set_gl(scr, 3, 0);
        break;
    case E('|'): /* LS3R */
        screen_set_gr(scr, 3);
        break;
    case E('}'): /* LS2R */
        screen_set_gr(scr, 2);
        break;
    case E('~'): /* LS1R */
        screen_set_gr(scr, 1);
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
        screen_set_charset(scr, 1, cs94_ascii);
        screen_set_gr(scr, 1);
        /* fallthrough */
    case E('N') | I0(' '): /* ANSI_LEVEL_3 */
        screen_set_charset(scr, 0, cs94_ascii);
        screen_set_gl(scr, 0, 0);
        break;
    //case E('3') | I0('#'): /* DECDHL */
    //case E('4') | I0('#'): /* DECDHL */
    //case E('5') | I0('#'): /* DECSWL */
    //case E('6') | I0('#'): /* DECDWL */
    //    break;
    case E('8') | I0('#'): /* DECALN*/
        screen_reset_margins(scr);
        screen_move_to(scr, 0, 0);
        screen_fill(scr, 0, 0, screen_width(scr), screen_height(scr), 0, 'E');
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
                screen_set_charset(scr, ((term->esc.selector & I0_MASK) - I0('(')) >> 9, set);
            break;
        case I0('-'): /* G1D6 */
        case I0('.'): /* G2D6 */
        case I0('/'): /* G3D6 */
            if ((set = nrcs_parse(term->esc.selector, 1, term->vt_level, term->mode.enable_nrcs)) > 0)
                screen_set_charset(scr, 1 + (((term->esc.selector & I0_MASK) - I0('-')) >> 9), set);
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

static void term_dispatch_c0(struct term *term, uint32_t ch) {
    if (gconfig.trace_controls && ch != 0x1B)
        info("Seq: ^%c", ch ^ 0x40);

    struct screen *scr = &term->scr;
    switch (ch) {
    case 0x00: /* NUL (IGNORE) */
    case 0x01: /* SOH (IGNORE) */
    case 0x02: /* STX (IGNORE) */
    case 0x03: /* ETX (IGNORE) */
    case 0x04: /* EOT (IGNORE) */
        break;
    case 0x05: /* ENQ */
        term_answerback(term, "%s", window_cfg(screen_window(scr))->answerback_string);
        break;
    case 0x06: /* ACK (IGNORE) */
        break;
    case 0x07: /* BEL */
        if (term->esc.state == esc_dcs_string)
            term_dispatch_dcs(term);
        else if (is_osc_state(term->esc.state))
            term_dispatch_osc(term);
        else window_bell(screen_window(scr), term->bvol);
        break;
    case 0x08: /* BS */
        screen_move_left(scr, 1);
        break;
    case 0x09: /* HT */
        screen_tabs(scr, 1);
        break;
    case 0x0a: /* LF */
    case 0x0b: /* VT */
    case 0x0c: /* FF */
        screen_autoprint(scr);
        screen_index(scr);
        if (term->mode.crlf) screen_cr(scr);
        break;
    case 0x0d: /* CR */
        screen_cr(scr);
        break;
    case 0x0e: /* SO/LS1 */
        screen_set_gl(scr, 1, 0);
        if (!term->vt_level) term->esc.state = esc_ground;
        break;
    case 0x0f: /* SI/LS0 */
        screen_set_gl(scr, 0, 0);
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
        screen_putchar(scr, '?');
        /* fallthrough */
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

static void term_dispatch_vt52(struct term *term, uint32_t ch) {
    struct screen *scr = &term->scr;
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
        screen_move_width_origin(scr, screen_cursor_x(scr),
                                 screen_cursor_y(scr) - 1);
        break;
    case 'B':
        screen_move_width_origin(scr, screen_cursor_x(scr),
                                 screen_cursor_y(scr) + 1);
        break;
    case 'C':
        screen_move_width_origin(scr, screen_cursor_x(scr) + 1,
                                 screen_cursor_y(scr));
        break;
    case 'D':
        screen_move_width_origin(scr, screen_cursor_x(scr) - 1,
                                 screen_cursor_y(scr));
        break;
    case 'F':
        screen_set_gl(scr, 1, 0);
        break;
    case 'G':
        screen_set_gl(scr, 0, 0);
        break;
    case 'H':
        screen_move_to(scr, screen_min_ox(scr), screen_min_oy(scr));
        break;
    case 'I':
        screen_rindex(scr);
        break;
    case 'J':
        screen_cursor_adjust_wide_left(scr);
        screen_erase(scr, screen_cursor_x(scr), screen_cursor_y(scr),
                     screen_width(scr), screen_cursor_y(scr) + 1, false);
        screen_erase(scr, 0, screen_cursor_y(scr) + 1,
                     screen_width(scr), screen_height(scr), false);
        break;
    case 'K':
        screen_cursor_adjust_wide_left(scr);
        screen_erase(scr, screen_cursor_x(scr), screen_cursor_y(scr),
                     screen_width(scr), screen_cursor_y(scr) + 1, false);
        break;
    case 'V': /* Print cursor line */
        screen_print_cursor_line(scr);
        break;
    case 'W': /* Enable printer controller mode */
        term_dispatch_mc(term, 0, 5);
        break;
    case 'X': /* Disable printer printer controller mdoe */
        /* This is never reached... */
        term_dispatch_mc(term, 0, 4);
        break;
    case 'Y':
        term->esc.state = esc_vt52_cup_0;
        return;
    case 'Z':
        term_answerback(term, ESC"/Z");
        break;
    case ']': /* Print screen */
        term_dispatch_mc(term, 0, 0);
        break;
    case '^': /* Autoprint on */
        term_dispatch_mc(term, 1, 5);
        break;
    case '_': /* Autoprint off */
        term_dispatch_mc(term, 1, 4);
        break;
    default:
        warn("Unrecognized ^[%c", ch);
    }

    term->esc.state = esc_ground;
}

static void term_dispatch_vt52_cup(struct term *term) {
    struct screen *scr = &term->scr;
    screen_move_width_origin(scr, screen_min_ox(scr) + term->esc.param[1],
                             screen_min_oy(scr) + term->esc.param[0]);
    term->esc.state = esc_ground;
}


inline static bool term_dispatch_dcs_string(struct term *term, uint8_t ch, const uint8_t **start, const uint8_t *end) {
    ch = *--*start;

    bool utf8 = term->mode.utf8 || (term->mode.title_set_utf8 && !term->mode.title_set_hex
            && term->esc.state == esc_osc_string && term->esc.selector < 3);
    do {
        ssize_t len = 1;

        if (ch >= 0xC0 && ch < 0xF8 && utf8) {
            len += (uint8_t[7]){ 1, 1, 1, 1, 2, 2, 3 }[(ch >> 3U) - 24];
        } else if ((term->esc.state == esc_dcs_string && IS_DEL(ch)) || IS_C0(ch)) {
            ++*start, len = 0;
        }

        if (len + *start >= end) return false;

        if (UNLIKELY(term->esc.str_len + len + 1 >= term->esc.str_cap)) {
            size_t new_cap = STR_CAP_STEP(term->esc.str_cap);
            if (new_cap > ESC_MAX_LONG_STR) break;

            uint8_t *new = xrealloc(term->esc.str_ptr, term->esc.str_cap, new_cap + 1);

            if (!term->esc.str_ptr)
                memcpy(new, term->esc.str_data, term->esc.str_len);

            term->esc.str_ptr = new;
            term->esc.str_cap = new_cap;
        }

        uint8_t *str = term_esc_str(term);
        while (len--) {
            str[term->esc.str_len++] = ch;
            ch = *++*start;
            if ((ch & 0xA0) != 0x80) break;
        }

    } while (*start < end && !IS_STREND(ch) && !IS_C1(ch));

    term_esc_str(term)[term->esc.str_len] = '\0';

    return true;
}

inline static bool term_dispatch(struct term *term, const uint8_t **start, const uint8_t *end) {
    uint8_t ch = **start;

    /* Fast path for graphical characters, it can print one line at a time. */
    if (term->esc.state == esc_ground && !IS_CBYTE(ch))
        return screen_dispatch_print(&term->scr, start, end, term->mode.utf8, term->mode.enable_nrcs);

    ++*start;

    /* C1 controls are interpreted in all states, try them before others */
    if (UNLIKELY(IS_C1(ch)) && term->vt_level > 1) {
        term->esc.old_selector = term->esc.selector;
        term->esc.old_state = term->esc.state;
        term->esc.state = esc_esc_entry;
        term->esc.selector = E(ch ^ 0xC0);
        term_dispatch_esc(term);
        return !term->requested_resize;
    }

    /* Treat bytes with 8th bits set as their lower counterparts
     * (Unless they are a printable character, part of a string or C1 control) */
    ch &= 0x7F;

    switch (term->esc.state) {
    case esc_ground:
        /* Only C0 arrives, so ch < 0x20 */
        term_dispatch_c0(term, ch);
        break;
    case esc_esc_entry:
        term_esc_start(term);
        /* fallthrough */
    case esc_esc_1:
        if (0x20 <= ch && ch <= 0x2F) {
            term->esc.selector |= term->esc.state ==
                    esc_esc_entry ? I0(ch) : I1(ch);
            term->esc.state++;
        } else
        /* fallthrough */
    case esc_esc_2:
        if (0x30 <= ch && ch <= 0x7E) {
            term->esc.selector |= E(ch);
            term_dispatch_esc(term);
        } else
        /* fallthrough */
    case esc_esc_ignore:
        if (IS_C0(ch))
            term_dispatch_c0(term, ch);
        else if (IS_DEL(ch))
            /* ignore */;
        else if (0x30 <= ch && ch <= 0x7E)
            term->esc.state = esc_ground;
        else
            term->esc.state = esc_esc_ignore;
        break;
    case esc_dcs_entry:
        term_esc_start_string(term);
        /* fallthrough */
    case esc_csi_entry:
        term_esc_start_seq(term);
        term->esc.state++;
        if (0x3C <= ch && ch <= 0x3F)
            term->esc.selector |= P(ch);
        else
        /* fallthrough */
    case esc_csi_0:
    case esc_dcs_0:
        if (0x30 <= ch && ch <= 0x39)
            term->esc.param[term->esc.i] = (ch - 0x30) +
                MAX(term->esc.param[term->esc.i] * 10, 0);
        else if (ch == 0x3B) {
            if (term->esc.i < ESC_MAX_PARAM - 1)
                ++term->esc.i;
        } else if (ch == 0x3A) {
            if (term->esc.i < ESC_MAX_PARAM - 1) {
                ++term->esc.i;
                term->esc.subpar_mask |= 1 << term->esc.i;
            }
        } else
        /* fallthrough */
    case esc_csi_1:
    case esc_dcs_1:
        if (0x20 <= ch && ch <= 0x2F) {
            term->esc.selector |= (term->esc.state == esc_csi_0 ||
                    term->esc.state == esc_dcs_0) ? I0(ch) : I1(ch);
            term->esc.state++;
        } else
        /* fallthrough */
    case esc_csi_2:
    case esc_dcs_2:
        if (0x40 <= ch && ch <= 0x7E) {
            term->esc.selector |= C(ch);
            if (esc_dcs_entry <= term->esc.state && term->esc.state <= esc_dcs_2)
                term->esc.state = esc_dcs_string;
            else
                term_dispatch_csi(term);
        } else
        /* fallthrough */
    case esc_csi_ignore:
        if (IS_C0(ch)) {
            if (esc_dcs_entry > term->esc.state || term->esc.state > esc_dcs_2)
                term_dispatch_c0(term, ch);
        } else if (IS_DEL(ch))
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
        /* fallthrough */
    case esc_osc_1:
        if (0x30 <= ch && ch <= 0x39)
            term->esc.selector = (ch - 0x30) + term->esc.selector * 10;
        else
        /* fallthrough */
    case esc_osc_2:
        if (ch == 0x3B)
            term->esc.state = esc_osc_string;
        else
        /* fallthrough */
    case esc_ign_string:
        if (IS_STREND(ch))
            term_dispatch_c0(term, ch);
        else {
            term->esc.state = esc_ign_string;
            ch = *--*start;
            do {
                ssize_t len = 1;
                if (ch >= 0xC0 && ch < 0xF8 && term->mode.utf8)
                    len += (uint8_t[7]){ 1, 1, 1, 1, 2, 2, 3 }[(ch >> 3U) - 24];
                if (len + *start >= end) return false;
                while (len--) {
                    ch = *++*start;
                    if ((ch & 0xA0) != 0x80) break;
                }
            } while (*start < end && !IS_STREND(ch) && !IS_C1(ch));
        }
        break;
    case esc_ign_entry:
        term_esc_start_string(term);
        term->esc.state = esc_ign_string;
        if (IS_STREND(ch))
            term_dispatch_c0(term, ch);
        break;
    case esc_osc_string:
        if (IS_C0(ch) && !IS_STREND(ch))
            /* ignore */;
        else
        /* fallthrough */
    case esc_dcs_string:
        if (IS_STREND(ch))
            term_dispatch_c0(term, ch);
        return term_dispatch_dcs_string(term, ch, start, end);
    case esc_vt52_entry:
        if (IS_C0(ch))
            term_dispatch_c0(term, ch);
        else
            term_dispatch_vt52(term, ch);
        break;
    case esc_vt52_cup_0:
        term_esc_start_seq(term);
        /* fallthrough */
    case esc_vt52_cup_1:
        if (IS_C0(ch))
            term_dispatch_c0(term, ch);
        else {
            term->esc.param[term->esc.i++] = ch - ' ';
            if (term->esc.state == esc_vt52_cup_1)
                term_dispatch_vt52_cup(term);
            else term->esc.state++;
        }
        break;
    }

    return !term->requested_resize;
}

#if USE_URI
inline static void apply_matched_uri(struct term *term) {
    struct line_span uri_end = screen_span(&term->scr, screen_cursor_y(&term->scr));
    struct line_span *uri_start = screen_get_bookmark(&term->scr);
    uri_end.offset += screen_cursor_x(&term->scr);
    uri_start->offset -= term->shift_bookmark;

    if (!uri_start->line) return;

    /* URI is located on single line, contiguous and has
     * common SGR, since control characters reset URI match. */
    struct line *line = uri_end.line;

    assert(uri_end.offset <= line->size);
    assert(uri_start->offset <= line->size);
    assert(uri_start->offset >= 0);
    assert(line == uri_start->line);

    struct attr attr = *view_attr_at(&uri_end, 0);
    attr.uri = uri_add(uri_match_get(&term->uri_match), NULL);
    uint32_t attrid = alloc_attr(line, &attr);

    for (ssize_t i = uri_start->offset; i < uri_end.offset; i++)
        line->cell[i].attrid = attrid;

    uri_unref(attr.uri);
}
#endif

HOT
bool term_read(struct term *term) {
    if (tty_refill(&term->tty) < 0 ||
        !tty_has_data(&term->tty)) return false;

    term->requested_resize = false;
    printer_intercept(screen_printer(&term->scr), (const uint8_t **)&term->tty.start, term->tty.end);

    if (term->mode.scroll_on_output)
        screen_reset_view(&term->scr, 1);

#if USE_URI
    if (window_cfg(screen_window(term_screen(term)))->uri_mode == uri_mode_auto) {
        for (const uint8_t *cur = term->tty.start; cur < term->tty.end; cur++) {
            if (term->uri_match.state < uris1_slash1) {
                /* Skip until potential URI start,
                 * if we are not in the middle of URL matching. */
                cur = memchr(cur, ':', term->tty.end - cur);
                if (LIKELY(!cur)) break;

                const uint8_t *proto_start;
                proto_start = match_reverse_proto_tree(&term->uri_match, cur, MAX_PROTOCOL_LEN - 1);
                if (!proto_start) continue;

                if (proto_start < term->tty.start) {
                    term->shift_bookmark = term->tty.start - proto_start;
                    screen_set_bookmark(&term->scr, term->tty.start);
                } else {
                    term->shift_bookmark = 0;
                    screen_set_bookmark(&term->scr, proto_start);
                }

            }

            for (; cur < term->tty.end; cur++) {
                enum uri_match_result res = uri_match_next_from_colon(&term->uri_match, *cur);
                if (res == urim_ground) break;
                if (res == urim_finished) {
                    while (term->tty.start < cur)
                        if (!term_dispatch(term, (const uint8_t **)&term->tty.start, cur)) break;
                    if (term->requested_resize) goto finish;

                    if (term->esc.state == esc_ground  &&
                        !screen_altscreen(&term->scr) &&
                        !screen_sgr(&term->scr)->uri) apply_matched_uri(term);
                    uri_match_reset(&term->uri_match, true);
                    break;
                }
            }
        }
    }
#endif

    while (term->tty.start < term->tty.end)
        if (!term_dispatch(term, (const uint8_t **)&term->tty.start, term->tty.end)) break;
    if (term->requested_resize) goto finish;

finish:
    screen_set_bookmark(&term->scr, NULL);
    screen_drain_scrolled(&term->scr);

    return true;
}

inline static bool is_osc52_reply(struct term *term) {
    return term->paste_from;
}

void term_paste(struct term *term, uint8_t *data, ssize_t size, bool utf8, bool is_first, bool is_last) {
    static uint8_t leftover[3], leftover_len;
    static uint8_t buf1[2*PASTE_BLOCK_SIZE];
    static uint8_t buf2[4*PASTE_BLOCK_SIZE];

    assert(size <= PASTE_BLOCK_SIZE);

    /* There's a race condition
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

    uint8_t *pos = data, *end = data + size;

    if (!size) return;

    if (is_first) {
        leftover_len = 0;
        if (is_osc52_reply(term)) {
            term_answerback(term, OSC"52;%c;", term->paste_from);
        } else if (term->mode.bracketed_paste) {
            term_answerback(term, CSI"200~");
        }
    }

    if (!term->mode.paste_literal_nl)
        while ((pos = memchr(pos, '\n', end - pos))) *pos++ = '\r';

    if (utf8 ^ term->mode.utf8) {
        pos = data;
        size = 0;
        if (utf8) {
            uint32_t ch;
            while (pos < end)
                if (utf8_decode(&ch, (const uint8_t **)&pos, end))
                    buf1[size++] = ch;
        } else {
            while (pos < end)
                size += utf8_encode(*pos++, buf1 + size, buf1 + LEN(buf1));
        }

        data = buf1;
    }

    if (is_osc52_reply(term)) {
        while (leftover_len < 3 && size) leftover[leftover_len++] = *data++, size--;
        size_t pre = base64_encode(buf2, leftover, leftover + leftover_len) - buf2;

        if (size) {
            if (!is_last) {
                leftover_len = size % 3;
                if (leftover_len > 0) leftover[0] = data[size - leftover_len], size--;
                if (leftover_len > 1) leftover[1] = data[size - 1], size--;
            }

            size = base64_encode(buf2 + pre, data, data + size) - buf2;
        }
        data = buf2;
    } else if (term->mode.paste_quote) {
        bool quote_c1 = !term_is_utf8_enabled(term);
        ssize_t i = 0, j = 0;
        while (i < size) {
            /* Prefix control symbols with Ctrl-V */
            if (buf1[i] < 0x20 || buf1[i] == 0x7F ||
                    (quote_c1 && buf1[i] > 0x7F && buf1[i] < 0xA0))
                buf2[j++] = 0x16;
            buf2[j++] = buf1[i++];
        }
        size = j;
        data = buf2;
    }

    term_sendkey(term, data, size);

    if (is_last) {
        if (is_osc52_reply(term)) {
            term_answerback(term, ST);
            term->paste_from = 0;
        } else if (term->mode.bracketed_paste) {
            term_answerback(term, CSI"201~");
        }
    }
}

void term_toggle_numlock(struct term *term) {
    term->kstate.allow_numlock = !term->kstate.allow_numlock;
}

bool term_is_keep_clipboard_enabled(struct term *term) {
    return term->mode.keep_clipboard;
}

bool term_is_utf8_enabled(struct term *term) {
    return term->mode.utf8;
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

struct screen *term_screen(struct term *term) {
    return &term->scr;
}

struct keyboard_state *term_get_kstate(struct term *term) {
    return &term->kstate;
}

struct mouse_state *term_get_mstate(struct term *term) {
    return &term->mstate;
}

struct selection_state *term_get_sstate(struct term *term) {
    return screen_selection(&term->scr);
}

struct window *term_window(struct term *term) {
    return screen_window(&term->scr);
}

color_t *term_palette(struct term *term) {
    return term->palette;
}

bool term_is_reverse(struct term *term) {
    return term->mode.reverse_video;
}

int term_fd(struct term *term) {
    return term->tty.w.fd;
}

void term_break(struct term *term) {
    tty_break(&term->tty);
}

void term_reset(struct term *term) {
    term_do_reset(term, true);
}

void term_handle_focus(struct term *term, bool set) {
    term->mode.focused = set;
    if (term->mode.track_focus)
        term_answerback(term, set ? CSI"I" : CSI"O");
    screen_damage_cursor(&term->scr);
}

void term_scroll_view_to_cmd(struct term *term, int16_t amount) {
    screen_scroll_view_to_cmd(&term->scr, amount);
}

void term_scroll_view(struct term *term, int16_t amount) {
    if (screen_altscreen(&term->scr)) {
        if (term->mode.altscreen_scroll)
            term_answerback(term, CSI"%u%c", abs(amount), amount > 0 ? 'A' : 'D');
        return;
    }

    screen_scroll_view(&term->scr, amount);
}

static size_t encode_c1(uint8_t *out, const uint8_t *in, bool eightbit) {
    uint8_t *fmtp = out;
    for (const uint8_t *it = (const uint8_t *)in; *it && fmtp - out < MAX_REPORT - 1; it++) {
        if (IS_C1(*it) && !eightbit) {
            *fmtp++ = 0x1B;
            *fmtp++ = *it ^ 0xC0;
            /* Theoretically we can use C1 encoded as UTF-8 if term->mode.utf8
             * but noone understands that format */
            //*fmtp++ = 0xC0 | (*it >> 6);
            //*fmtp++ = 0x80 | (*it & 0x3F);
        } else {
            *fmtp++ = *it;
        }
    }
    *fmtp = 0x00;
    return fmtp - out;
}

inline static bool has_8bit(struct term *term) {
    return term->mode.eight_bit && term->vt_level > 1;
}

void term_answerback(struct term *term, const char *str, ...) {
    uint8_t fmt[MAX_REPORT], csi[MAX_REPORT];

    encode_c1(fmt, (const uint8_t *)str, has_8bit(term));

    va_list vl;
    va_start(vl, str);
    ssize_t res = vsnprintf((char *)csi, sizeof(csi), (char *)fmt, vl);
    va_end(vl);

    tty_write(&term->tty, csi, res, term->mode.crlf);

    if (gconfig.trace_input) {
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
    bool encode = !len, utf8 = term->mode.utf8, nrcs = term->mode.enable_nrcs;
    if (!len) len = strlen((const char *)str);

    if (!term->mode.no_scroll_on_input)
        screen_reset_view(&term->scr, 1);

    uint8_t rep[MAX_REPORT];
    if (encode) len = encode_c1(rep, str, has_8bit(term));

    /* Local echo */
    if (term->mode.echo) {
        const uint8_t *start = encode ? rep : str, *ptmp;
        const uint8_t *end = start + len;
        uint8_t pre2[4] = "^[", pre1[3] = "^";
        while (*start < *end) {
            uint32_t ch = *start;
            if (IS_C1(ch)) {
                pre2[2] = *start++ ^ 0xC0;
                ptmp = pre2;
                while (ptmp < pre2 + 3 && screen_dispatch_print(&term->scr, &ptmp, pre2 + 3, utf8, nrcs));
            } else if ((IS_C0(ch) && ch != '\n' && ch != '\t' && ch != '\r') || IS_DEL(ch)) {
                pre1[1] = *start++ ^ 0x40;
                ptmp = pre1;
                while (ptmp < pre1 + 2 && screen_dispatch_print(&term->scr, &ptmp, pre1 + 2, utf8, nrcs));
            } else {
                if (!screen_dispatch_print(&term->scr, &start, end, utf8, nrcs)) break;
            }
        }
    }

    tty_write(&term->tty, encode ? rep : str, len, term->mode.crlf);
}

void term_resize(struct term *term, int16_t width, int16_t height) {
    struct screen *scr = &term->scr;

    /* First try to read from tty to empty out input queue
     * since this is input from program not yet aware about resize.
     * Don't empty if it's requested resize, since application is already aware. */
    if (!term->requested_resize)
        term_read(term);
    term->requested_resize = false;

    /* Notify application */
    int16_t wwidth = window_cfg(screen_window(scr))->width;
    int16_t wheight = window_cfg(screen_window(scr))->height;
    tty_set_winsz(&term->tty, width, height, wwidth, wheight);

    screen_resize(scr, width, height);
}

struct term *create_term(struct window *win, int16_t width, int16_t height) {
    struct term *term = xzalloc(sizeof(struct term));

    if (tty_open(&term->tty, window_cfg(win)) < 0) {
        warn("Can't create tty");
        free_term(term);
        return NULL;
    }

    if (!init_screen(&term->scr, win)) {
        warn("Can't allocate selection state");
        free_term(term);
        return NULL;
    }

    term_load_config(term, true);

    term->vt_version = window_cfg(win)->vt_version;
    term->vt_level = term->vt_version / 100;
    if (!term->vt_level) term_set_vt52(term, 1);

    screen_free_scrollback(&term->scr, window_cfg(win)->scrollback_size);
    term_resize(term, width, height);

    return term;
}

void term_hang(struct term *term) {
    tty_hang(&term->tty);
}

void free_term(struct term *term) {
    tty_hang(&term->tty);

#if USE_URI
    uri_match_reset(&term->uri_match, false);
#endif

    free_screen(&term->scr);
    keyboard_reset_udk(term);

    free(term);
}
