/* Copyright (c) 2019-2021, Evgeny Baskov. All rights reserved */

#include "feature.h"

#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "input.h"
#include "nrcs.h"
#include "util.h"
#include "window.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>


static double default_dpi = 96;
static double default_utf8 = 1;

struct global_config gconfig = {
    .log_level = 3,
};

struct optmap_item optmap[] = {
    [o_autorepeat] = {"autorepeat", "\t\t(Enable key autorepeat)"},
    [o_allow_alternate] = {"allow-alternate", "\t(Enable alternate screen)"},
    [o_allow_blinking] = {"allow-blinking", "\t(Allow blinking text and cursor)"},
    [o_allow_modify_edit_keypad] = {"allow-modify-edit-keypad", " (Allow modifing edit keypad keys)"},
    [o_allow_modify_function] = {"allow-modify-function", "\t(Allow modifing function keys)"},
    [o_allow_modify_keypad] = {"allow-modify-keypad", "\t(Allow modifing keypad keys)"},
    [o_allow_modify_misc] = {"allow-modify-misc", "\t(Allow modifing miscelleneous keys)"},
    [o_alpha] = {"alpha", "\t\t\t(Backround opacity, requires compositor to be running)"},
    [o_alternate_scroll] = {"alternate-scroll", "\t(Scrolling sends arrow keys escapes in alternate screen)"},
    [o_answerback_string] = {"answerback-string", "\t(ENQ report)"},
    [o_appcursor] = {"appcursor", "\t\t(Initial application cursor mode value)"},
    [o_appkey] = {"appkey", "\t\t(Initial application keypad mode value)"},
    [o_autowrap] = {"autowrap", "\t\t(Initial autowrap setting)"},
    [o_background] = {"background", "\t\t(Default background color)"},
    [o_backspace_is_del] = {"backspace-is-del", "\t(Backspace sends DEL instead of BS)"},
    [o_bell] = {"bell", "\t\t\t(Bell setting)"},
    [o_bell_high_volume] = {"bell-high-volume", "\t(High volume value for DECSWBV)"},
    [o_bell_low_volume] = {"bell-low-volume", "\t(Low volume value for DECSWBV)"},
    [o_blend_all_background] = {"blend-all-background", "\t(Apply opacity to all background colors, not just default one)"},
    [o_blend_foreground] = {"blend-foreground", "\t(Apply opacity to foreground colors)"},
    [o_blink_color] = {"blink-color", "\t\t(Special color of blinking text)"},
    [o_blink_time] = {"blink-time", "\t\t(Text blink interval in microseconds)"},
    [o_bold_color] = {"bold-color", "\t\t(Special color of bold text)"},
    [o_config] = {"config", "\t\t(Configuration file path)"},
    [o_cursor_background] = {"cursor-background", "\t(Default cursor background color)"},
    [o_cursor_foreground] = {"cursor-foreground", "\t(Default cursor foreground color)"},
    [o_cursor_shape] = {"cursor-shape", "\t\t(Shape of cursor)"},
    [o_cursor_width] = {"cursor-width", "\t\t(Width of lines that forms cursor)"},
    [o_cut_lines] = {"cut-lines", "\t\t(Cut long lines on resize with rewrapping disabled)"},
    [o_cwd] = {"cwd", "\t\t\t(Current working directory for an application)"},
    [o_daemon] = {"daemon", "\t\t(Start terminal as daemon)"},
    [o_delete_is_del] = {"delete-is-del", "\t\t(Delete sends DEL symbol instead of escape sequence)"},
    [o_double_click_time] = {"double-click-time", "\t(Time gap in microseconds in witch two mouse presses will be considered double)"},
    [o_dpi] = {"dpi", "\t\t\t(DPI value for fonts)"},
    [o_erase_scrollback] = {"erase-scrollback", "\t(Allow ED 3 to clear scrollback buffer)"},
    [o_extended_cir] = {"extended-cir", "\t\t(Report all SGR attributes in DECCIR)"},
    [o_fixed] = {"fixed", "\t\t\t(Don't allow to change window size, if supported)"},
    [o_fkey_increment] = {"fkey-increment", "\t(Step in numbering function keys)"},
    [o_font] = {"font", ", -f<value>\t(Comma-separated list of fontconfig font patterns)"},
    [o_font_gamma] = {"font-gamma", "\t\t(Factor of font sharpenning)"},
    [o_font_size] = {"font-size", "\t\t(Font size in points)"},
    [o_font_size_step] = {"font-size-step", "\t(Font size step in points)"},
    [o_font_spacing] = {"font-spacing", "\t\t(Additional spacing for individual symbols)"},
    [o_force_mouse_mod] = {"force-mouse-mod", "\t(Modifer to force mouse action)"},
    [o_force_nrcs] = {"force-nrcs", "\t\t(Enable NRCS translation when UTF-8 mode is enabled)"},
    [o_force_scalable] = {"force-scalable", "\t(Do not search for pixmap fonts)"},
    [o_foreground] = {"foreground", "\t\t(Default foreground color)"},
    [o_fps] = {"fps", "\t\t\t(Window refresh rate)"},
    [o_frame_wait_delay] = {"frame-wait-delay", "\t(Maximal time since last application output before redraw)"},
    [o_has_meta] = {"has-meta", "\t\t(Handle meta/alt)"},
    [o_horizontal_border] = {"horizontal-border", "\t(Top and bottom botders)"},
    [o_italic_color] = {"italic-color", "\t\t(Special color of italic text)"},
    [o_keep_clipboard] = {"keep-clipboard", "\t(Reuse copied clipboard content instead of current selection data)"},
    [o_keep_selection] = {"keep-selection", "\t(Don't clear X11 selection when unhighlighted)"},
    [o_key_break] = {"key-break", "\t\t(Send break hotkey"},
    [o_key_copy] = {"key-copy", "\t\t(Copy to clipboard hotkey)"},
    [o_key_dec_font] = {"key-dec-font", "\t\t(Decrement font size hotkey)"},
    [o_key_inc_font] = {"key-inc-font", "\t\t(Increment font size hotkey)"},
    [o_key_new_window] = {"key-new-window", "\t(Create new window hotkey)"},
    [o_key_numlock] = {"key-numlock", "\t\t('appkey' mode allow toggle hotkey)"},
    [o_key_paste] = {"key-paste", "\t\t(Paste from clipboard hotkey)"},
    [o_key_reload_config] = {"key-reload-config", "\t(Reload config hotkey)"},
    [o_key_reset] = {"key-reset", "\t\t(Terminal reset hotkey)"},
    [o_key_reset_font] = {"key-reset-font", "\t(Reset font size hotkey)"},
    [o_key_reverse_video] = {"key-reverse-video", "\t(Toggle reverse video mode hotkey)"},
    [o_key_scroll_down] = {"key-scroll-down", "\t(Scroll down hotkey)"},
    [o_key_scroll_up] = {"key-scroll-up", "\t\t(Scroll up hotkey)"},
    [o_keyboard_dialect] = {"keyboard-dialect", "\t(National replacement character set to be used in non-UTF-8 mode)"},
    [o_keyboard_mapping] = {"keyboard-mapping", "\t(Initial keyboad mapping)"},
    [o_line_spacing] = {"line-spacing", "\t\t(Additional lines vertical spacing)"},
    [o_lock_keyboard] = {"lock-keyboard", "\t\t(Disable keyboad input)"},
    [o_log_level] = {"log-level","\t\t(Filering level of logged information)"},
    [o_luit] = {"luit", "\t\t\t(Run luit if terminal doesn't support encoding by itself)"},
    [o_luit_path] = {"luit-path", "\t\t(Path to luit executable)"},
    [o_margin_bell] = {"margin-bell", "\t\t(Margin bell setting)"},
    [o_margin_bell_column] = {"margin-bell-column", "\t(Columnt at which margin bell rings when armed)"},
    [o_margin_bell_high_volume] = {"margin-bell-high-volume", " (High volume value for DECSMBV)"},
    [o_margin_bell_low_volume] = {"margin-bell-low-volume", "(Low volume value for DECSMBV)"},
    [o_max_frame_time] = {"max-frame-time", "\t(Maximal time between frames in microseconds)"},
    [o_meta_sends_escape] = {"meta-sends-escape", "\t(Alt/Meta sends escape prefix instead of setting 8-th bit)"},
    [o_minimize_scrollback] = {"minimize-scrollback", "\t(Realloc lines to save memory; makes scrolling a little slower)"},
    [o_modify_cursor] = {"modify-cursor", "\t\t(Enable encoding modifiers for cursor keys)"},
    [o_modify_function] = {"modify-function", "\t(Enable encoding modifiers for function keys)"},
    [o_modify_keypad] = {"modify-keypad", "\t\t(Enable encoding modifiers keypad keys)"},
    [o_modify_other] = {"modify-other", "\t\t(Enable encoding modifiers for other keys)"},
    [o_modify_other_fmt] = {"modify-other-fmt", "\t(Format of encoding modifers)"},
    [o_nrcs] = {"nrcs", "\t\t\t(Enable NRCSs support)"},
    [o_numlock] = {"numlock", "\t\t(Initial numlock state)"},
#if USE_URI
    [o_allow_uris] = {"allow-uris", "\t(Allow URI parsing/clicking)"},
    [o_open_command] = {"open-cmd", "\t\t(A command used to open URIs when clicked)"},
    [o_uri_click_mod] = {"uri-click-mod", "\t\t(keyboard modifer used to click-open URIs)"},
    [o_unique_uris] = {"unique-uris", "\t(Make distinction between URIs with the same location)"},
    [o_key_copy_uri] = {"key-copy-uri", "\t(Copy underlying URL hotkey)"},
#endif
#if USE_BOXDRAWING
    [o_override_boxdrawing] = {"override-boxdrawing", "\t(Use built-in box drawing characters)"},
#endif
    [o_pixel_mode] = {"pixel-mode", "\t\t(Subpixel rendering config; mono, bgr, rgb, bgrv, or rgbv)"},
    [o_print_attributes] = {"print-attributes", "\t(Print cell attributes when printing is enabled)"},
    [o_print_command] = {"print-command", "\t\t(Program to pipe CSI MC output into)"},
    [o_printer_file] = {"printer-file", ", -o<value> (File where CSI MC output to)"},
    [o_raise_on_bell] = {"raise-on-bell", "\t\t(Raise terminal window on bell)"},
    [o_reverse_video] = {"reverse-video", "\t\t(Initial reverse video setting)"},
    [o_reversed_color] = {"reversed-color", "\t(Special color of reversed text)"},
    [o_rewrap] = {"rewrap", "\t\t(Rewrap text on resize)"},
    [o_scroll_amount] = {"scroll-amount", "\t\t(Number of lines scrolled in a time)"},
    [o_scroll_on_input] = {"scroll-on-input", "\t(Scroll view to bottom on key press)"},
    [o_scroll_on_output] = {"scroll-on-output", "\t(Scroll view to bottom when character in printed)"},
    [o_scrollback_size] = {"scrollback-size", ", -H<value> (Number of saved lines)"},
    [o_select_scroll_time] = {"select-scroll-time", "\t(Delay between scrolls of window while selecting with mouse in microseconds)"},
    [o_select_to_clipboard] = {"select-to-clipboard", "\t(Use CLIPBOARD selection to store hightlighted data)"},
    [o_selected_background] = {"selected-background", "\t(Color of selected background)"},
    [o_selected_foreground] = {"selected-foreground", "\t(Color of selected text)"},
    [o_shell] = {"shell", "\t\t\t(Shell to start in new instance)"},
    [o_smooth_scroll] = {"smooth-scroll", "\t\t(Inital value of DECSCLM mode)"},
    [o_smooth_scroll_delay] = {"smooth-scroll-delay", "\t(Delay between scrolls when DECSCLM is enabled)"},
    [o_smooth_scroll_step] = {"smooth-scroll-step", "\t(Amount of lines per scroll when DECSCLM is enabled)"},
    [o_socket] = {"socket", ", -s<value> \t(Daemon socket path)"},
    [o_special_blink] = {"special-blink", "\t\t(If special color should be used for blinking text)"},
    [o_special_bold] = {"special-bold", "\t\t(If special color should be used for bold text)"},
    [o_special_italic] = {"special-italic", "\t(If special color should be used for italic text)"},
    [o_special_reverse] = {"special-reverse", "\t(If special color should be used for reverse text)"},
    [o_special_underlined] = {"special-underlined", "\t(If special color should be used for underlined text)"},
    [o_substitute_fonts] = {"substitute-fonts", "\t(Enable substitute font support)"},
    [o_sync_timeout] = {"sync-timeout", "\t\t(Syncronous update timeout)"},
    [o_tab_width] = {"tab-width", "\t\t(Initial width of tab character)"},
    [o_term_mod] = {"term-mod", "\t\t(Meaning of 'T' modifer)"},
    [o_term_name] = {"term-name", ", -D<value>\t(TERM value)"},
    [o_title] = {"title", ", -T<value>, -t<value> (Initial window title)"},
    [o_trace_characters] = {"trace-characters", "\t(Trace interpreted characters)"},
    [o_trace_controls] = {"trace-controls", "\t(Trace interpreted control characters and sequences)"},
    [o_trace_events] = {"trace-events", "\t\t(Trace recieved events)"},
    [o_trace_fonts] = {"trace-fonts", "\t\t(Log font related information)"},
    [o_trace_input] = {"trace-input", "\t\t(Trace user input)"},
    [o_trace_misc] = {"trace-misc", "\t\t(Trace miscelleneous information)"},
    [o_triple_click_time] = {"triple-click-time", "\t(Time gap in microseconds in witch tree mouse presses will be considered triple)"},
    [o_underline_width] = {"underline-width", "\t(Text underline width)"},
    [o_underlined_color] = {"underlined-color", "\t(Special color of underlined text)"},
    [o_urgent_on_bell] = {"urgent-on-bell", "\t(Set window urgency on bell)"},
    [o_use_utf8] = {"use-utf8", "\t\t(Enable UTF-8 I/O)"},
    [o_vertical_border] = {"vertical-border", "\t(Left and right borders)"},
    [o_visual_bell] = {"visual-bell", "\t\t(Whether bell should be visual or normal)"},
    [o_visual_bell_time] = {"visual-bell-time", "\t(Length of visual bell)"},
    [o_vt_version] = {"vt-version", ", -V<value>\t(Emulated VT version)"},
    [o_window_class] = {"window-class", ", -c<value> (X11 Window class)"},
    [o_window_ops] = {"window-ops", "\t\t(Allow window manipulation with escape sequences)"},
    [o_word_break] = {"word-break", "\t\t(Symbols treated as word separators when snapping mouse selection)"},
};


#define CN_BASE 16
#define CN_EXT (6*6*6)
#define CN_GRAY (PALETTE_SIZE - CN_BASE - CN_EXT)
#define SD28B(x) ((x) ? 0x37 + 0x28 * (x) : 0)

static color_t color(uint32_t n) {
    static const color_t base[CN_BASE] = {
    // That's gruvbox colors
            0xFF222222, 0xFFFF4433, 0xFFBBBB22, 0xFFFFBB22,
            0xFF88AA99, 0xFFDD8899, 0xFF88CC77, 0xFFDDCCAA,
            0xFF665555, 0xFFFF4433, 0xFFBBBB22, 0xFFFFBB22,
            0xFF88AA99, 0xFFDD8899, 0xFF88CC77, 0xFFFFFFCC,
    // Replace it with
    //        0xff000000, 0xff0000cd, 0xff00cd00, 0xff00cdcd,
    //        0xffee0000, 0xffcd00cd, 0xffcdcd00, 0xffe5e5e5,
    //        0xff7f7f7f, 0xff0000ff, 0xff00ff00, 0xff00ffff,
    //        0xffff5c5c, 0xffff00ff, 0xffffff00, 0xffffffff,
    // to get default xterm colors
    };

    switch (n) {
    case SPECIAL_BG:
    case SPECIAL_CURSOR_BG:
        return base[0];
    case SPECIAL_FG:
    case SPECIAL_CURSOR_FG:
    case SPECIAL_BOLD:
    case SPECIAL_UNDERLINE:
    case SPECIAL_BLINK:
    case SPECIAL_REVERSE:
    case SPECIAL_ITALIC:
        return base[15];
        /* Invert text by default */
    case SPECIAL_SELECTED_BG:
    case SPECIAL_SELECTED_FG:
        return 0;
    }

    if (n < CN_BASE) return base[n];
    else if (n < CN_EXT + CN_BASE) {
        return 0xFF000000 | SD28B(((n - CN_BASE) / 1) % 6) |
            (SD28B(((n - CN_BASE) / 6) % 6) << 8) | (SD28B(((n - CN_BASE) / 36) % 6) << 16);
    } else if (n < CN_GRAY + CN_EXT + CN_BASE) {
        uint8_t val = MIN(0x08 + 0x0A * (n - CN_BASE - CN_EXT), 0xFF);
        return 0xFF000000 + val * 0x10101;
    }

    return base[0];
}

static bool parse_bool(const char *str, bool *val, bool dflt) {
    if (!strcasecmp(str, "default")) {
        *val = dflt;
        return 1;
    } else if (!strcasecmp(str, "true") || !strcasecmp(str, "yes") || !strcasecmp(str, "y") || !strcmp(str, "1")) {
        *val = 1;
        return 1;
    } else if (!strcasecmp(str, "false") || !strcasecmp(str, "no") || !strcasecmp(str, "n") || !strcmp(str, "0")) {
        *val = 0;
        return 1;
    }
    return 0;
}

static bool parse_geometry(struct instance_config *cfg, const char *value) {
    int16_t x = 0, y = 0, w = 0, h = 0;
    char xsgn = '+', ysgn = '+';
    if (value[0] == '=') value++;
    if (value[0] == '+' || value[0] == '-') {
        bool scanned = sscanf(value, "%c%"SCNd16"%c%"SCNd16, &xsgn, &x, &ysgn, &y) == 4;
        if (!scanned || (xsgn != '+' && xsgn != '-') || (ysgn != '+' && ysgn != '-')) return 0;
        if (xsgn == '-') x = -x;
        if (ysgn == '-') y = -y;
    } else {
        int res = sscanf(value, "%"SCNd16"%*[xX]%"SCNd16"%c%"SCNd16"%c%"SCNd16,
                &w, &h, &xsgn, &x, &ysgn, &y);
        if (res == 6) {
            if ((xsgn != '+' && xsgn != '-') || (ysgn != '+' && ysgn != '-')) return 0;
            if (xsgn == '-') x = -x;
            if (ysgn == '-') y = -y;
        } else if (res != 2) return 0;
        cfg->width = w;
        cfg->height = h;
    }

    cfg->user_geometry = 1;
    cfg->x = x;
    cfg->y = y;
    cfg->stick_to_right = xsgn == '-';
    cfg->stick_to_bottom = ysgn == '-';
    return 1;
}

static bool parse_int(const char *str, int64_t *val, int64_t min, int64_t max, int64_t dflt) {
    if (!strcasecmp(str, "default")) *val = dflt;
    else {
        errno = 0;
        char *end;
        *val = strtoll(str, &end, 0);
        if (errno || !end || *end) return 0;
        if (*val < min) *val = min;
        if (*val > max) *val = max;
    }
    return 1;
}

static bool parse_enum(const char *str, int *val, int dflt, int start, ...) {
    if (!strcasecmp(str, "default")) *val = dflt;
    else {
        va_list va;
        va_start(va, start);
        const char *s;
        bool valset = 0;
        while ((s = va_arg(va, const char *))) {
            if (!strcasecmp(str, s)) {
                *val = start;
                valset = 1;
                break;
            }
            start++;
        }
        va_end(va);
        return valset;
    }
    return 1;
}

static void parse_str(char **dst, const char *str, const char *dflt) {
    char *res;
    if (!strcasecmp(str, "default")) {
        res = dflt ? strdup(dflt) : NULL;
    } else res = strdup(str);

    if (*dst) free(*dst);
    *dst = res;
}

static bool parse_col(const char *str, color_t *val, color_t dflt) {
    if (!strcasecmp(str, "default")) *val = dflt;
    else {
        const uint8_t *end = (const uint8_t *)str + strlen(str);
        *val = parse_color((const uint8_t *)str, end);
        if (!*val) return 0;
    }
    return 1;
}

static bool parse_double(const char *str, double *val, double min, double max, double dflt) {
    if (!strcasecmp(str, "default")) *val = dflt;
    else if (sscanf(str, "%lf", val) == 1) {
        if (*val > max) *val = max;
        if (*val < min) *val = min;
    } else return 0;
    return 1;
}

bool set_option(struct instance_config *c, const char *name, const char *value, bool allow_global) {
    struct global_config *g = &gconfig;
    color_t *p = c->palette;

    switch(*name) {
        union {
            int64_t i;
            int e;
            bool b;
            char *s;
            double f;
            color_t c;
        } val;
        unsigned cnum;
    case 'a':
#if USE_URI
        if (!strcmp(name, optmap[o_allow_uris].opt)) {
            if (parse_bool(value, &val.b, 1)) c->allow_uris = val.b;
        } else
#endif
        if (!strcmp(name, optmap[o_autorepeat].opt)) {
            if (parse_bool(value, &val.b, 1)) c->autorepeat = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_allow_alternate].opt)) {
            if (parse_bool(value, &val.b, 1)) c->allow_altscreen = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_allow_blinking].opt)) {
            if (parse_bool(value, &val.b, 1)) c->allow_blinking = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_allow_modify_edit_keypad].opt)) {
            if (parse_bool(value, &val.b, 0)) c->allow_legacy_edit = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_allow_modify_function].opt)) {
            if (parse_bool(value, &val.b, 0)) c->allow_legacy_function = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_allow_modify_keypad].opt)) {
            if (parse_bool(value, &val.b, 0)) c->allow_legacy_keypad = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_allow_modify_misc].opt)) {
            if (parse_bool(value, &val.b, 0)) c->allow_legacy_misc = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_alpha].opt)) {
            if (parse_double(value, &val.f, 0, 1, 1)) c->alpha = val.f;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_alternate_scroll].opt)) {
            if (parse_bool(value, &val.b, 0)) c->alternate_scroll = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_answerback_string].opt)) {
            parse_str(&c->answerback_string, value, "\006");
        } else if (!strcmp(name, optmap[o_appcursor].opt)) {
            if (parse_bool(value, &val.b, 0)) c->appcursor = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_appkey].opt)) {
            if (parse_bool(value, &val.b, 0)) c->appkey = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_autowrap].opt)) {
            if (parse_bool(value, &val.b, 1)) c->wrap = val.b;
            else goto e_value;
        } else goto e_unknown;
        break;
    case 'b':
        if (!strcmp(name, optmap[o_background].opt)) {
            if (parse_col(value, &val.c, color(SPECIAL_BG))) p[SPECIAL_BG] = val.c;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_backspace_is_del].opt)) {
            if (parse_bool(value, &val.b, 1)) c->wrap = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_bell].opt)) {
            if (parse_enum(value, &val.e, 2, 0, "off", "low", "high", NULL)) c->bell_volume = val.e;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_bell_high_volume].opt)) {
            if (parse_int(value, &val.i, 0, 100, 100)) c->bell_high_volume = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_bell_low_volume].opt)) {
            if (parse_int(value, &val.i, 0, 100, 50)) c->bell_low_volume = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_blend_all_background].opt)) {
            if (parse_bool(value, &val.b, 0)) c->blend_all_bg = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_blend_foreground].opt)) {
            if (parse_bool(value, &val.b, 0)) c->blend_fg = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_blink_color].opt)) {
            if (parse_col(value, &val.c, color(SPECIAL_BLINK))) p[SPECIAL_BLINK] = val.c;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_blink_time].opt)) {
            if (parse_int(value, &val.i, 0, 10*SEC/1000, 800000)) c->blink_time = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_bold_color].opt)) {
            if (parse_col(value, &val.c, color(SPECIAL_BOLD))) p[SPECIAL_BOLD] = val.c;
            else goto e_value;
        } else goto e_unknown;
        break;
    case 'c':
        if (!strcmp(name, optmap[o_config].opt)) {
            parse_str(&c->config_path, value, NULL);
        } else if (!strcmp(name, optmap[o_cursor_background].opt)) {
            if (parse_col(value, &val.c, color(SPECIAL_CURSOR_BG))) p[SPECIAL_CURSOR_BG] = val.c;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_cursor_foreground].opt)) {
            if (parse_col(value, &val.c, color(SPECIAL_CURSOR_FG))) p[SPECIAL_CURSOR_FG] = val.c;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_cursor_shape].opt)) {
            if (parse_enum(value, &val.e, 6, 1, "blinking-block", "block",
                    "blinking-underline", "underline", "blinking-bar", "bar", NULL)) c->cursor_shape = val.e;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_cursor_width].opt)) {
            if (parse_int(value, &val.i, 0, 16, 2)) c->cursor_width = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_cut_lines].opt)) {
            if (parse_bool(value, &val.b, 0)) c->cut_lines = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_cwd].opt)) {
            parse_str(&c->cwd, value, NULL);
        } else if (sscanf(name, "color%u", &cnum) == 1 && cnum < PALETTE_SIZE - SPECIAL_PALETTE_SIZE) {
            if (parse_col(value, &val.c, color(cnum))) p[cnum] = val.c;
            else goto e_value;
        } else goto e_unknown;
        break;
    case 'd':
        if (!strcmp(name, optmap[o_daemon].opt)) {
            if (parse_bool(value, &val.b, 0) && allow_global) g->daemon_mode = val.b;
        } else if (!strcmp(name, optmap[o_delete_is_del].opt)) {
            if (parse_bool(value, &val.b, 0)) c->delete_is_delete = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_double_click_time].opt)) {
            if (parse_int(value, &val.i, 0, 10*SEC/1000, 300000)) c->double_click_time = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_dpi].opt)) {
            if (parse_double(value, &val.f, 0, 1000, default_dpi)) c->dpi = val.f;
            else goto e_value;
        } else goto e_unknown;
        break;
    case 'e':
        if (!strcmp(name, optmap[o_erase_scrollback].opt)) {
            if (parse_bool(value, &val.b, 1)) c->allow_erase_scrollback = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_extended_cir].opt)) {
            if (parse_bool(value, &val.b, 1)) c->extended_cir = val.b;
            else goto e_value;
        } else goto e_unknown;
        break;
    case 'f':
        if (!strcmp(name, optmap[o_fixed].opt)) {
            if (parse_bool(value, &val.b, 0)) c->fixed = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_fkey_increment].opt)) {
            if (parse_int(value, &val.i, 0, 48, 10)) c->fkey_increment = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_font].opt)) {
            parse_str(&c->font_name, value, "mono");
        } else if (!strcmp(name, optmap[o_font_gamma].opt)) {
            if (parse_double(value, &val.f, 0.2, 2, 1)) c->gamma = val.f;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_font_size].opt)) {
            if (parse_int(value, &val.i, 1, 1000, 0)) c->font_size = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_font_size_step].opt)) {
            if (parse_int(value, &val.i, 0, 250, 1)) c->font_size_step = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_font_spacing].opt)) {
            if (parse_int(value, &val.i, -100, 100, 0)) c->font_spacing = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_force_mouse_mod].opt)) {
            parse_str(&c->force_mouse_mod, value, "T");
        } else if (!strcmp(name, optmap[o_force_nrcs].opt)) {
            if (parse_bool(value, &val.b, 0)) c->force_utf8_nrcs = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_force_scalable].opt)) {
            if (parse_bool(value, &val.b, 0)) c->force_scalable = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_foreground].opt)) {
            if (parse_col(value, &val.c, color(SPECIAL_FG))) p[SPECIAL_FG] = val.c;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_fps].opt)) {
            if (parse_int(value, &val.i, 2, 1000, 60)) c->fps = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_frame_wait_delay].opt)) {
            if (parse_int(value, &val.i, 0, 10*SEC/1000, SEC/240000)) c->frame_finished_delay = val.i;
            else goto e_value;
        } else goto e_unknown;
        break;
    case 'g':
        if (!strcmp(name, "geometry")) {
            if (!parse_geometry(c, value)) goto e_value;
        } else goto e_unknown;
        break;
    case 'h':
        if (!strcmp(name, optmap[o_has_meta].opt)) {
            if (parse_bool(value, &val.b, 1)) c->has_meta = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_horizontal_border].opt)) {
            if (parse_int(value, &val.i, 0, 200, 8)) c->left_border = val.i;
            else goto e_value;
        } else goto e_unknown;
        break;
    case 'i':
        if (!strcmp(name, optmap[o_italic_color].opt)) {
            if (parse_col(value, &val.c, color(SPECIAL_ITALIC))) p[SPECIAL_ITALIC] = val.c;
            else goto e_value;
        } else goto e_unknown;
        break;
    case 'k':
        if (!strcmp(name, optmap[o_keep_clipboard].opt)) {
            if (parse_bool(value, &val.b, 0)) c->keep_clipboard = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_keep_selection].opt)) {
            if (parse_bool(value, &val.b, 0)) c->keep_selection = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_key_break].opt)) {
             parse_str(&c->key[shortcut_break], value, "Break");
        } else if (!strcmp(name, optmap[o_key_copy].opt)) {
             parse_str(&c->key[shortcut_copy], value, "T-C");
#if USE_URI
        } else if (!strcmp(name, optmap[o_key_copy_uri].opt)) {
             parse_str(&c->key[shortcut_copy_uri], value, "T-U");
#endif
        } else if (!strcmp(name, optmap[o_key_dec_font].opt)) {
             parse_str(&c->key[shortcut_font_down], value, "T-Page_Down");
        } else if (!strcmp(name, optmap[o_key_inc_font].opt)) {
             parse_str(&c->key[shortcut_font_up], value, "T-Page_Up");
        } else if (!strcmp(name, optmap[o_key_new_window].opt)) {
             parse_str(&c->key[shortcut_new_window], value, "T-N");
        } else if (!strcmp(name, optmap[o_key_numlock].opt)) {
             parse_str(&c->key[shortcut_numlock], value, "T-Num_Lock");
        } else if (!strcmp(name, optmap[o_key_paste].opt)) {
             parse_str(&c->key[shortcut_paste], value, "T-V");
        } else if (!strcmp(name, optmap[o_key_reload_config].opt)) {
             parse_str(&c->key[shortcut_reload_config], value, "T-X");
        } else if (!strcmp(name, optmap[o_key_reset].opt)) {
             parse_str(&c->key[shortcut_reset], value, "T-R");
        } else if (!strcmp(name, optmap[o_key_reset_font].opt)) {
             parse_str(&c->key[shortcut_font_default], value, "T-Home");
        } else if (!strcmp(name, optmap[o_key_reverse_video].opt)) {
             parse_str(&c->key[shortcut_reverse_video], value, "T-I");
        } else if (!strcmp(name, optmap[o_key_scroll_down].opt)) {
             parse_str(&c->key[shortcut_scroll_down], value, "T-Down");
        } else if (!strcmp(name, optmap[o_key_scroll_up].opt)) {
             parse_str(&c->key[shortcut_scroll_up], value, "T-Up");
        } else if (!strcmp(name, optmap[o_keyboard_dialect].opt)) {
            if (!strcasecmp(value, "default")) c->keyboard_nrcs = cs94_ascii;
            else {
#define E(c) ((c) & 0x7F)
#define I0(i) ((i) ? (((i) & 0xF) + 1) << 9 : 0)
#define I1(i) (I0(i) << 5)
                if (!value[1] && value[0] > 0x2F && value[0] < 0x7F) {
                    uint32_t sel = E(value[0]);
                    val.e = nrcs_parse(sel, 0, 5, 1);
                    if (val.e < 0) val.e = nrcs_parse(sel, 1, 5, 1);
                } else if (value[0] >= 0x20 && value[0] < 0x30 && value[1] > 0x2F && value[1] < 0x7F && !value[3]) {
                    uint32_t sel = E(value[1]) | I0(value[0]);
                    val.e = nrcs_parse(sel, 0, 5, 1);
                    if (val.e == nrcs_invalid) val.e = nrcs_parse(sel, 1, 5, 1);
                    if (val.e == nrcs_invalid) goto e_value;
                    c->keyboard_nrcs = val.e;
                } else goto e_value;
            }
        } else if (!strcmp(name, optmap[o_keyboard_mapping].opt)) {
            if (parse_enum(value, &val.e, keymap_default, keymap_legacy,
                    "legacy", "vt220", "hp", "sun", "sco", NULL)) c->mapping = val.e;
            else goto e_value;
        } else goto e_unknown;
        break;
    case 'l':
        if (!strcmp(name, optmap[o_line_spacing].opt)) {
            if (parse_int(value, &val.i, -100, 100, 0)) c->line_spacing = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_lock_keyboard].opt)) {
            if (parse_bool(value, &val.b, 0)) c->lock = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_log_level].opt)) {
            if (parse_enum(name, &val.e, 3, 0,
                    "quiet", "fatal", "warn", "info", NULL) && allow_global) g->log_level = val.e;
        } else if (!strcmp(name, optmap[o_luit].opt)) {
            if (parse_bool(value, &val.b, 1)) c->allow_luit = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_luit_path].opt)) {
            parse_str(&c->luit, value, "/usr/bin/luit");
        } else goto e_unknown;
        break;
    case 'm':
        if (!strcmp(name, optmap[o_margin_bell].opt)) {
            if (parse_enum(value, &val.e, 2, 0, "off", "low", "high", NULL)) c->margin_bell_volume = val.e;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_margin_bell_column].opt)) {
            if (parse_int(value, &val.i, 0, 200, 10)) c->margin_bell_column = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_margin_bell_high_volume].opt)) {
            if (parse_int(value, &val.i, 0, 100, 100)) c->margin_bell_high_volume = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_margin_bell_low_volume].opt)) {
            if (parse_int(value, &val.i, 0, 100, 50)) c->margin_bell_low_volume = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_max_frame_time].opt)) {
            if (parse_int(value, &val.i, 0, 10*SEC/1000, SEC/20000)) c->max_frame_time = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_meta_sends_escape].opt)) {
            if (parse_bool(value, &val.b, 1)) c->meta_is_esc = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_minimize_scrollback].opt)) {
            if (parse_bool(value, &val.b, 1)) c->minimize_scrollback = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_modify_cursor].opt)) {
            if (parse_int(value, &val.i, 0, 3, 3)) c->modify_cursor = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_modify_function].opt)) {
            if (parse_int(value, &val.i, 0, 3, 3)) c->modify_function = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_modify_keypad].opt)) {
            if (parse_int(value, &val.i, 0, 3, 3)) c->modify_keypad = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_modify_other].opt)) {
            if (parse_int(value, &val.i, 0, 4, 0)) c->modify_other = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_modify_other_fmt].opt)) {
            if (parse_enum(value, &val.e, 0, 0, "xterm", "csi-u", NULL)) c->modify_other_fmt = val.e;
            else goto e_value;
        } else goto e_unknown;
        break;
    case 'n':
        if (!strcmp(name, optmap[o_nrcs].opt)) {
            if (parse_bool(value, &val.b, 1)) c->allow_nrcs = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_numlock].opt)) {
            if (parse_bool(value, &val.b, 1)) c->numlock = val.b;
            else goto e_value;
        } else goto e_unknown;
        break;
    case 'o':
#if USE_URI
        if (!strcmp(name, optmap[o_open_command].opt)) {
            parse_str(&g->open_command, value, "nsst-open");
        } else
#endif
#if USE_BOXDRAWING
        if (!strcmp(name, optmap[o_override_boxdrawing].opt)) {
            if (parse_bool(value, &val.b, 0)) c->override_boxdraw = val.b;
            else goto e_value;
        } else
#endif
        goto e_unknown;
        break;
    case 'p':
        if (!strcmp(name, optmap[o_pixel_mode].opt)) {
            if (parse_enum(value, &val.e, pixmode_mono, pixmode_mono,
                    "mono", "bgr", "rgb", "bgrv", "rgbv", NULL)) c->modify_other_fmt = val.e;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_print_attributes].opt)) {
            if (parse_bool(value, &val.b, 1)) c->print_attr = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_print_command].opt)) {
            parse_str(&c->printer_cmd, value, NULL);
        } else if (!strcmp(name, optmap[o_printer_file].opt)) {
            parse_str(&c->printer_file, value, NULL);
        } else goto e_unknown;
        break;
    case 'r':
        if (!strcmp(name, optmap[o_raise_on_bell].opt)) {
            if (parse_bool(value, &val.b, 0)) c->raise_on_bell = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_reverse_video].opt)) {
            if (parse_bool(value, &val.b, 0)) c->reverse_video = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_reversed_color].opt)) {
            if (parse_col(value, &val.c, color(SPECIAL_REVERSE))) p[SPECIAL_REVERSE] = val.c;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_rewrap].opt)) {
            if (parse_bool(value, &val.b, 1)) c->rewrap = val.b;
            else goto e_value;
        } else goto e_unknown;
        break;
    case 's':
        if (!strcmp(name, optmap[o_scroll_amount].opt)) {
            if (parse_int(value, &val.i, 0, 1000, 2)) c->scroll_amount = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_scroll_on_input].opt)) {
            if (parse_bool(value, &val.b, 1)) c->scroll_on_input = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_scroll_on_output].opt)) {
            if (parse_bool(value, &val.b, 0)) c->scroll_on_output = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_scrollback_size].opt)) {
            if (parse_int(value, &val.i, 0, 1000000000, 10000)) c->scrollback_size = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_select_scroll_time].opt)) {
            if (parse_int(value, &val.i, 0, 10*SEC/1000, 10000)) c->select_scroll_time = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_select_to_clipboard].opt)) {
            if (parse_bool(value, &val.b, 0)) c->select_to_clipboard = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_selected_background].opt)) {
            if (parse_col(value, &val.c, color(SPECIAL_SELECTED_BG))) p[SPECIAL_SELECTED_BG] = val.c;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_selected_foreground].opt)) {
            if (parse_col(value, &val.c, color(SPECIAL_SELECTED_FG))) p[SPECIAL_SELECTED_FG] = val.c;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_shell].opt)) {
            parse_str(&c->shell, value, "/bin/sh");
        } else if (!strcmp(name, optmap[o_smooth_scroll].opt)) {
            if (parse_bool(value, &val.b, 0)) c->smooth_scroll = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_smooth_scroll_delay].opt)) {
            if (parse_int(value, &val.i, 0, 10*SEC/1000, 500)) c->smooth_scroll_delay = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_smooth_scroll_step].opt)) {
            if (parse_int(value, &val.i, 1, 100000, 1)) c->smooth_scroll_step = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_special_blink].opt)) {
            if (parse_bool(value, &val.b, 0)) c->special_blink = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_special_bold].opt)) {
            if (parse_bool(value, &val.b, 0)) c->special_bold = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_special_italic].opt)) {
            if (parse_bool(value, &val.b, 0)) c->special_italic = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_special_reverse].opt)) {
            if (parse_bool(value, &val.b, 0)) c->special_reverse = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_special_underlined].opt)) {
            if (parse_bool(value, &val.b, 0)) c->special_underline = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_substitute_fonts].opt)) {
            if (parse_bool(value, &val.b, 1)) c->allow_subst_font = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_sync_timeout].opt)) {
            if (parse_int(value, &val.i, 0, 10*SEC/1000, SEC/2000)) c->sync_time = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_socket].opt)) {
            if (allow_global) {
                parse_str(&g->sockpath, value, "/tmp/nsst-sock0");
            }
        } else goto e_unknown;
        break;
    case 't':
        if (!strcmp(name, optmap[o_tab_width].opt)) {
            if (parse_int(value, &val.i, 1, 1000, 8)) c->tab_width = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_term_mod].opt)) {
            parse_str(&c->term_mod, value, "SC");
        } else if (!strcmp(name, optmap[o_term_name].opt)) {
            parse_str(&c->terminfo, value, "xterm");
        } else if (!strcmp(name, optmap[o_title].opt)) {
            parse_str(&c->title, value, "Not So Simple Terminal");
        } else if (!strcmp(name, optmap[o_trace_characters].opt)) {
            if (parse_bool(value, &val.b, 0) && allow_global) g->trace_characters = val.b;
        } else if (!strcmp(name, optmap[o_trace_controls].opt)) {
            if (parse_bool(value, &val.b, 0) && allow_global) g->trace_controls = val.b;
        } else if (!strcmp(name, optmap[o_trace_events].opt)) {
            if (parse_bool(value, &val.b, 0) && allow_global) g->trace_events = val.b;
        } else if (!strcmp(name, optmap[o_trace_fonts].opt)) {
            if (parse_bool(value, &val.b, 0) && allow_global) g->trace_fonts = val.b;
        } else if (!strcmp(name, optmap[o_trace_input].opt)) {
            if (parse_bool(value, &val.b, 0) && allow_global) g->trace_input = val.b;
        } else if (!strcmp(name, optmap[o_trace_misc].opt)) {
            if (parse_bool(value, &val.b, 0) && allow_global) g->trace_misc = val.b;
        } else if (!strcmp(name, optmap[o_triple_click_time].opt)) {
            if (parse_int(value, &val.i, 0, 10*SEC/1000, 600000)) c->triple_click_time = val.i;
            else goto e_value;
        } else goto e_unknown;
        break;
    case 'u':
#if USE_URI
        if (!strcmp(name, optmap[o_uri_click_mod].opt)) {
            parse_str(&c->uri_click_mod, value, "");
        } else if (!strcmp(name, optmap[o_unique_uris].opt)) {
            if (parse_bool(value, &val.b, 0)) g->unique_uris = val.b;
            else goto e_value;
        } else
#endif
        if (!strcmp(name, optmap[o_underline_width].opt)) {
            if (parse_int(value, &val.i, 0, 16, 1)) c->underline_width = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_underlined_color].opt)) {
            if (parse_col(value, &val.c, color(SPECIAL_UNDERLINE))) p[SPECIAL_UNDERLINE] = val.c;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_urgent_on_bell].opt)) {
            if (parse_bool(value, &val.b, 0)) c->urgency_on_bell = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_use_utf8].opt)) {
            if (parse_bool(value, &val.b, default_utf8)) c->utf8 = val.b;
            else goto e_value;
        } else goto e_unknown;
        break;
    case 'v':
        if (!strcmp(name, optmap[o_vertical_border].opt)) {
            if (parse_int(value, &val.i, 0, 200, 8)) c->top_border = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_visual_bell].opt)) {
            if (parse_bool(value, &val.b, 1)) c->visual_bell = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_visual_bell_time].opt)) {
            if (parse_int(value, &val.i, 0, 10*SEC/1000, 200000)) c->visual_bell_time = val.i;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_vt_version].opt)) {
            if (parse_int(value, &val.i, 0, 999, 420)) c->vt_version = val.i;
            else goto e_value;
        } else goto e_unknown;
        break;
    case 'w':
        if (!strcmp(name, optmap[o_window_class].opt)) {
            parse_str(&c->window_class, value, NULL);
        } else if (!strcmp(name, optmap[o_window_ops].opt)) {
            if (parse_bool(value, &val.b, 1)) c->allow_window_ops = val.b;
            else goto e_value;
        } else if (!strcmp(name, optmap[o_word_break].opt)) {
            parse_str(&c->word_separators, value, " \t!$^*()+={}[]\\\"'|,;<>~`");
        } else goto e_unknown;
        break;
    default:
    e_unknown:
        warn("Unknown option: %s=\"%s\"", name, value);
        return 0;
    e_value:
        warn("Invalid value: %s=\"%s\"", name, value);
        return 0;
    }
    if (gconfig.trace_misc) info("Option set: %s=\"%s\"", name, value);
    return 1;
}

void set_default_dpi(double dpi) {
    default_dpi = dpi;
}

void set_default_utf8(bool set) {
    default_utf8 = set;
}

void copy_config(struct instance_config *dst, struct instance_config *src) {
    *dst = *src;
    for (ssize_t i = 0; i < shortcut_MAX; i++)
        dst->key[i] = src->key[i] ? strdup(src->key[i]) : NULL;
    dst->word_separators = src->word_separators ? strdup(src->word_separators) : NULL;
    dst->cwd = src->cwd ? strdup(src->cwd) : NULL;
    dst->printer_cmd = src->printer_cmd ? strdup(src->printer_cmd) : NULL;
    dst->printer_file = src->printer_file ? strdup(src->printer_file) : NULL;
    dst->luit = src->luit ? strdup(src->luit) : NULL;
    dst->terminfo = src->terminfo ? strdup(src->terminfo) : NULL;
    dst->answerback_string = src->answerback_string ? strdup(src->answerback_string) : NULL;
    dst->title = src->title ? strdup(src->title) : NULL;
    dst->window_class = src->window_class ? strdup(src->window_class) : NULL;
    dst->font_name = src->font_name ? strdup(src->font_name) : NULL;
    dst->config_path = src->config_path ? strdup(src->config_path) : NULL;
    dst->term_mod = src->term_mod ? strdup(src->term_mod) : NULL;
    dst->force_mouse_mod = src->force_mouse_mod ? strdup(src->force_mouse_mod) : NULL;
    dst->shell = src->shell ? strdup(src->shell) : NULL;
#if USE_URI
    dst->uri_click_mod = src->uri_click_mod ? strdup(src->uri_click_mod) : NULL;
#endif
    src->argv = NULL;
}

void free_config(struct instance_config *src) {
    for (ssize_t i = 0; i < shortcut_MAX; i++)
        free(src->key[i]);
    free(src->word_separators);
    free(src->cwd);
    free(src->printer_cmd);
    free(src->printer_file);
    free(src->luit);
    free(src->terminfo);
    free(src->answerback_string);
    free(src->title);
    free(src->window_class);
    free(src->font_name);
    free(src->config_path);
    free(src->term_mod);
    free(src->force_mouse_mod);
    free(src->shell);
#if USE_URI
    free(src->uri_click_mod);
#endif
}

void parse_config(struct instance_config *cfg, bool allow_global) {
    char pathbuf[PATH_MAX];
    const char *path = cfg->config_path;
    int fd = -1;

    /* Config file is search in following places:
     * 1. sconf(SCONF_CONFIG_PATH) set with --config=
     * 2. $XDG_CONFIG_HOME/nsst.conf
     * 3. $HOME/.config/nsst.conf
     * If file is not found in those places, just give up */

    if (path) fd = open(path, O_RDONLY);
    if (fd < 0) {
        const char *xdg_cfg = getenv("XDG_CONFIG_HOME");
        if (xdg_cfg) {
            snprintf(pathbuf, sizeof pathbuf, "%s/nsst.conf", xdg_cfg);
            fd = open(pathbuf, O_RDONLY);
        }
    }
    if (fd < 0) {
        const char *home = getenv("HOME");
        if (home) {
            snprintf(pathbuf, sizeof pathbuf, "%s/.config/nsst.conf", home);
            fd = open(pathbuf, O_RDONLY);
        }
    }

    if (fd < 0) {
        if (path) goto e_open;
        return;
    }

    struct stat stt;
    if (fstat(fd, &stt) < 0) goto e_open;

    char *addr = mmap(NULL, stt.st_size + 1, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (addr == MAP_FAILED) goto e_open;

    close(fd);

    char *ptr = addr, *end = addr + stt.st_size;
    char saved1 = '\0', saved2 = '\0';
    ssize_t line_n = 0;
    while (ptr < end) {
        line_n++;

        while (ptr < end && isspace((unsigned)*ptr)) ptr++;
        if (ptr >= end) break;

        char *start = ptr;
        if (isalpha((unsigned)*ptr)) {
            char *name_start, *name_end, *value_start, *value_end;

            name_start = ptr;

            while (ptr < end && !isspace((unsigned)*ptr) && *ptr != '#' && *ptr != '=') ptr++;
            name_end = ptr;

            while (ptr < end && isblank((unsigned)*ptr)) ptr++;
            if (ptr >= end || *ptr++ != '=') goto e_wrong_line;
            while (ptr < end && isblank((unsigned)*ptr)) ptr++;
            value_start = ptr;

            while (ptr < end && *ptr != '\n') ptr++;
            while (ptr > value_start && isblank((unsigned)ptr[-1])) ptr--;
            value_end = ptr;

            SWAP(*value_end, saved1);
            SWAP(*name_end, saved2);
            set_option(cfg, name_start, value_start, allow_global);
            SWAP(*name_end, saved2);
            SWAP(*value_end, saved1);
        } else if (*ptr == '#') {
            while (ptr < end && *ptr != '\n') ptr++;
        } else {
e_wrong_line:
            ptr = start;
            while(ptr < end && *ptr != '\n') ptr++;
            SWAP(*ptr, saved1);
            warn("Can't parse config line #%zd: %s", line_n, start);
            SWAP(*ptr, saved1);
            ptr++;
        }
    }

    munmap(addr, stt.st_size + 1);

e_open:
    // Parse all shortcuts
    keyboard_parse_config(cfg);

    if (fd < 0) warn("Can't read config file: %s", path ? path : pathbuf);
}

void init_instance_config(struct instance_config *cfg, const char *config_path, bool allow_global) {
    for (size_t i = 0; i < sizeof(optmap)/sizeof(*optmap); i++)
        if (i != o_config) set_option(cfg, optmap[i].opt, "default", allow_global);
    for (size_t i = 0; i < PALETTE_SIZE; i++)
        cfg->palette[i] = color(i);

    cfg->x = 200;
    cfg->y = 200;
    cfg->width = 800;
    cfg->height = 600;

    if (config_path)
        set_option(cfg, optmap[o_config].opt, config_path, 0);

    parse_config(cfg, allow_global);
}

