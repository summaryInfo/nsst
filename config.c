/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

#include "feature.h"

#define _GNU_SOURCE

#include "config.h"
#include "input.h"
#include "nrcs.h"
#include "util.h"
#include "window.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <langinfo.h>
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

/* FIXME: These probably need to use alternate description when ignored:
 * "uri-mode", "open-cmd", "uri-click-mod", "unique-uris", "key-copy-uri", "override-boxdrawing", */

struct global_config gconfig;

static hashtable_t options_hashtable;
static struct option *short_opts[26*2];
static ssize_t max_help_line_len;
static struct option *dpi_option_entry;

#define COLOR_SPECIAL_SELECTED_BG 0
#define COLOR_SPECIAL_SELECTED_FG 0
#define COLOR_SPECIAL_URI_TEXT 0
#define COLOR_SPECIAL_URI_UNDERLINE 0

#if 1 /* Gruvbox colors */

#define COLOR_SPECIAL_BG 0xFF222222
#define COLOR_SPECIAL_CURSOR_BG 0xFF222222

#define COLOR_SPECIAL_FG 0xFFFFFFCC
#define COLOR_SPECIAL_CURSOR_FG 0xFFFFFFCC
#define COLOR_SPECIAL_BOLD 0xFFFFFFCC
#define COLOR_SPECIAL_UNDERLINE 0xFFFFFFCC
#define COLOR_SPECIAL_BLINK 0xFFFFFFCC
#define COLOR_SPECIAL_REVERSE 0xFFFFFFCC
#define COLOR_SPECIAL_ITALIC 0xFFFFFFCC

#define BASE_COLOR(n) ((const color_t[CN_BASE]) { \
        0xFF222222, 0xFFFF4433, 0xFFBBBB22, 0xFFFFBB22, \
        0xFF88AA99, 0xFFDD8899, 0xFF88CC77, 0xFFDDCCAA, \
        0xFF665555, 0xFFFF4433, 0xFFBBBB22, 0xFFFFBB22, \
        0xFF88AA99, 0xFFDD8899, 0xFF88CC77, 0xFFFFFFCC, \
    }[(n)])

#else /* XTerm colors */

#define COLOR_SPECIAL_BG 0xFF000000
#define COLOR_SPECIAL_CURSOR_BG 0xFF000000

#define COLOR_SPECIAL_FG 0xFFFFFFFF
#define COLOR_SPECIAL_CURSOR_FG 0xFFFFFFFF
#define COLOR_SPECIAL_BOLD 0xFFFFFFFF
#define COLOR_SPECIAL_UNDERLINE 0xFFFFFFFF
#define COLOR_SPECIAL_BLINK 0xFFFFFFFF
#define COLOR_SPECIAL_REVERSE 0xFFFFFFFF
#define COLOR_SPECIAL_ITALIC 0xFFFFFFFF

#define BASE_COLOR(n) ((const color_t[CN_BASE]) { \
        0xFF000000, 0xFF0000CD, 0xFF00CD00, 0xFF00CDCD, \
        0xFFEE0000, 0xFFCD00CD, 0xFFCDCD00, 0xFFE5E5E5, \
        0xFF7F7F7F, 0xFF0000FF, 0xFF00FF00, 0xFF00FFFF, \
        0xFFFF5C5C, 0xFFFF00FF, 0xFFFFFF00, 0xFFFFFFFF, \
    }[(n)])

#endif

#define CN_BASE 16
#define CN_EXT (6*6*6)
#define CN_GRAY (PALETTE_SIZE - SPECIAL_PALETTE_SIZE - CN_BASE - CN_EXT)

#define SD28B(x) ((x) ? 0x37 + 0x28 * (x) : 0)
#define COLOR_8B(n) (0xFF000000 | \
        (SD28B(((n) / 1) % 6) << 0) | \
        (SD28B(((n) / 6) % 6) << 8) | \
        (SD28B(((n) / 36) % 6) << 16))

#define COLOR_GRAY(n) (0xFF000000 | \
        (MIN(0x08 + 0x0A * (n), 0xFF) * 0x10101))

#define MAX_SHORT_OPT 2

#define OPTION_TYPES \
    T(boolean,           "bool",           bool,            bool dflt;                               ) \
    T(color,             "color",          color_t,         color_t dflt;                            ) \
    T(double,            "real",           double,          double dflt; double min; double max;     ) \
    T(enum,              "enum",           int,             int dflt; int start; const char **values;) \
    T(geometry,          "geometry",       struct geometry, bool in_char;                            ) \
    T(int16,             "int",            int16_t,         int64_t dflt; int64_t min; int64_t max;  ) \
    T(int64,             "int",            int64_t,         int64_t dflt; int64_t min; int64_t max;  ) \
    T(nrcs,              "charset string", enum charset,    enum charset dflt;                       ) \
    T(string,            "str",            char *,          const char *dflt;                        ) \
    T(uint8,             "int",            uint8_t,         int64_t dflt; int64_t min; int64_t max;  ) \
    T(border,            "int",            struct border,   int64_t dflt; int64_t min; int64_t max;  ) \
    T(vertical_border,   "int",            struct border,   int64_t dflt; int64_t min; int64_t max;  ) \
    T(horizontal_border, "int",            struct border,   int64_t dflt; int64_t min; int64_t max;  ) \
    T(time,              "time",           int64_t,         int64_t dflt; int64_t min; int64_t max;  ) \

#define T(name, y, z, ...) struct name##_arg { __VA_ARGS__ } arg_##name;
union opt_limits {
    OPTION_TYPES
};
#undef T

enum option_type {
#define T(x, ...) option_type_##x,
    OPTION_TYPES
#undef T
};

typedef bool (*parse_fn)(const char *, void *, union opt_limits *);

#define T(x, ...) static bool do_parse_##x(const char *, void *, union opt_limits *);
OPTION_TYPES
#undef T

struct option {
    ht_head_t head;
    const char *name;
    const char *description;
    enum option_type type;
    char short_opt[MAX_SHORT_OPT];
    bool global;
    uint8_t field_size;
    ptrdiff_t offset;
    union opt_limits limits;
};

struct option_type_desc {
    const char *name;
    parse_fn parse;
    /* type_size field is stored modulo 256 */
    uint8_t type_size;
    uint8_t name_len;
} option_types[] = {
#define T(type_, name_, size_, ...) [option_type_##type_] = { \
        .name = name_, \
        .parse = do_parse_##type_, \
        .type_size = (uint8_t)sizeof(size_), \
        .name_len = sizeof name_ - 1\
    },
    OPTION_TYPES
#undef T
};

/* Instance option */
#define X2(type_, field_, l1_, l2_, name_, desc_, ...) { \
        .head = { 0 }, \
        .name = name_, \
        .description =  desc_, \
        .type = option_type_##type_, \
        .global = false, \
        .short_opt = { l1_, l2_ }, \
        .field_size = (uint8_t)sizeof(((struct instance_config *)NULL)->field_), \
        .offset = offsetof(struct instance_config, field_), \
        .limits.arg_##type_ = { __VA_ARGS__ } \
    }

#define X1(type_, field_, l1_, name_, desc_, ...) \
    X2(type_, field_, l1_, 0, name_, desc_, __VA_ARGS__)

#define X(type_, field_, name_, desc_, ...) \
    X2(type_, field_, 0, 0, name_, desc_, __VA_ARGS__)

/* Global option */
#define G1(type_, field_, l1_, name_, desc_, ...) { \
        .head = { 0 }, \
        .name = name_, \
        .description =  desc_, \
        .type = option_type_##type_, \
        .global = true, \
        .short_opt = { l1_, 0 }, \
        .field_size = (uint8_t)sizeof(((struct global_config *)NULL)->field_), \
        .offset = offsetof(struct global_config, field_), \
        .limits.arg_##type_ = { __VA_ARGS__ } \
    }

#define G(type_, field_, name_, desc_, ...) \
    G1(type_, field_, 0, name_, desc_, __VA_ARGS__)

// FIXME Select better option names in 2.0

static struct option options[] = {
    X1(geometry, geometry, 'g', "geometry", "Window geometry, format is [=][<width>{xX}<height>][{+-}<xoffset>{+-}<yoffset>]", false),
    X1(geometry, geometry, 'G', "char-geometry", "Window geometry in characters, format is [=][<width>{xX}<height>][{+-}<xoffset>{+-}<yoffset>]", true),
    X(boolean, autorepeat, "autorepeat", "Enable key autorepeat", true),
    X(boolean, allow_altscreen, "allow-alternate", "Enable alternate screen", true),
    X(boolean, allow_blinking, "allow-blinking", "Allow blinking text and cursor", true),
    X(boolean, allow_legacy_edit, "allow-modify-edit-keypad", "Allow modifying edit keypad keys", false),
    X(boolean, allow_legacy_function, "allow-modify-function", "Allow modifying function keys", false),
    X(boolean, allow_legacy_keypad, "allow-modify-keypad", "Allow modifying keypad keys", false),
    X(boolean, allow_legacy_misc, "allow-modify-misc", "Allow modifying miscellaneous keys", false),
    X(double, alpha, "alpha", "Background opacity, requires compositor to be running", 1, 0, 1),
    X(boolean, alternate_scroll, "alternate-scroll", "Scrolling sends arrow keys escapes in alternate screen", false),
    X(string, answerback_string, "answerback-string", "ENQ report", "\006"),
    X(boolean, appcursor, "appcursor", "Initial application cursor mode value", false),
    X(boolean, appkey, "appkey", "Initial application keypad mode value", false),
    X(boolean, wrap, "autowrap", "Initial autowrap setting", true),
    X(color, palette[SPECIAL_BG], "background", "Default background color", COLOR_SPECIAL_BG),
    X(boolean, backspace_is_delete, "backspace-is-del", "Backspace sends DEL instead of BS", true),
    X(enum, bell_volume, "bell", "Bell setting", 2, 0, (const char *[]){"off", "low", "high", NULL}),
    X(uint8, bell_high_volume, "bell-high-volume", "High volume value for DECSWBV", 100, 0, 100),
    X(uint8, bell_low_volume, "bell-low-volume", "Low volume value for DECSWBV", 50, 0, 100),
    X(boolean, blend_all_bg, "blend-all-background", "Apply opacity to all background colors, not just default one", false),
    X(boolean, blend_fg, "blend-foreground", "Apply opacity to foreground colors", false),
    X(color, palette[SPECIAL_BLINK], "blink-color", "Special color of blinking text", COLOR_SPECIAL_BLINK),
    X(time, blink_time, "blink-time", "Text blink interval", 800000, 0, 10*SEC/1000),
    X(color, palette[SPECIAL_BOLD], "bold-color", "Special color of bold text", COLOR_SPECIAL_BOLD),
    X1(string, config_path, 'C', "config", "Configuration file path", NULL),
    X(color, palette[SPECIAL_CURSOR_BG], "cursor-background", "Default cursor background color", COLOR_SPECIAL_CURSOR_BG),
    X(color, palette[SPECIAL_CURSOR_FG], "cursor-foreground", "Default cursor foreground color", COLOR_SPECIAL_CURSOR_FG),
    X(int16, cursor_width, "cursor-width", "Width of lines that forms cursor", 2, 1, 16),
    X(enum, cursor_shape, "cursor-shape", "Shape of cursor", 2, 1, (const char *[]){"blinking-block", "block", "blinking-underline", "underline", "blinking-bar", "bar", NULL}),
    X(string, cwd, "cwd", "Current working directory for an application", NULL),
    G1(boolean, daemon_mode, 'd', "daemon", "Start terminal as daemon", false),
    X(boolean, delete_is_delete, "delete-is-del", "Delete sends DEL symbol instead of escape sequence", false),
    X(time, double_click_time, "double-click-time", "Maximum time between button presses of the double click", 300000, 0, 10*SEC/1000),
    X(double, dpi, "dpi", "DPI value for fonts", 96, 0, 1000),
    X(boolean, allow_erase_scrollback, "erase-scrollback", "Allow ED 3 to clear scrollback buffer", true),
    X(boolean, extended_cir, "extended-cir", "Report all SGR attributes in DECCIR", true),
    X(boolean, fixed, "fixed", "Don't allow to change window size, if supported", false),
    X(uint8, fkey_increment, "fkey-increment", "Step in numbering function keys", 10, 0, 48),
    X1(string, font_name, 'f', "font", "Comma-separated list of fontconfig font patterns", "mono"),
    X(double, gamma, "font-gamma", "Factor of font sharpening", 1, 0.2, 2),
    X(int16, font_size, "font-size", "Font size in points", 0, 1, 1000),
    X(int16, font_size_step, "font-size-step", "Font size step in points", 1, 0, 250),
    X(int16, font_spacing, "font-spacing", "Additional spacing for individual symbols", 0, -100, 100),
    X(boolean, force_wayland_csd, "force-wayland-csd", "Don't request SSD", false),
    X(string, force_mouse_mod, "force-mouse-mod", "Modifier to force mouse action", "T"),
    X(boolean, force_utf8_nrcs, "force-nrcs", "Enable NRCS translation when UTF-8 mode is enabled", false),
    X(boolean, force_scalable, "force-scalable", "Do not search for pixmap fonts", false),
    X(color, palette[SPECIAL_FG], "foreground", "Default foreground color", COLOR_SPECIAL_FG),
    G(boolean, fork, "fork", "Fork in daemon mode", 1),
    X(int64, fps, "fps", "Window refresh rate", 60, 2, 1000),
    X(time, frame_finished_delay, "frame-wait-delay", "Maximum time since last application output before redraw", SEC/240000, 1, 10*SEC/1000),
    X(boolean, has_meta, "has-meta", "Handle meta/alt", true),
    X(int16, border.left, "left-border", "Left border size", 8, 0, 200),
    X(int16, border.right, "right-border", "Right border size", 8, 0, 200),
    X(color, palette[SPECIAL_ITALIC], "italic-color", "Special color of italic text", COLOR_SPECIAL_ITALIC),
    X(boolean, keep_clipboard, "keep-clipboard", "Reuse copied clipboard content instead of current selection data", false),
    X(boolean, keep_selection, "keep-selection", "Don't clear X11 selection when unhighlighted", false),
    X(string, key[shortcut_break], "key-break", "Send break hotkey", "Break"),
    X(string, key[shortcut_copy], "key-copy", "Copy to clipboard hotkey", "T-C"),
    X(string, key[shortcut_copy_uri], "key-copy-uri", "Copy selected URI to clipboard hotkey", "T-U"),
    X(string, key[shortcut_font_down], "key-dec-font", "Decrement font size hotkey", "T-Page_Down"),
    X(string, key[shortcut_font_up], "key-inc-font", "Increment font size hotkey", "T-Page_Up"),
    X(string, key[shortcut_new_window], "key-new-window", "Create new window hotkey", "T-N"),
    X(string, key[shortcut_numlock], "key-numlock", "'appkey' mode allow toggle hotkey", "T-Num_Lock"),
    X(string, key[shortcut_paste], "key-paste", "Paste from clipboard hotkey", "T-V"),
    X(string, key[shortcut_reload_config], "key-reload-config", "Reload config hotkey", "T-X"),
    X(string, key[shortcut_font_default], "key-reset-font", "Reset font size hotkey", "T-Home"),
    X(string, key[shortcut_reset], "key-reset", "Terminal reset hotkey", "T-R"),
    X(string, key[shortcut_reverse_video], "key-reverse-video", "Toggle reverse video mode hotkey", "T-I"),
    X(string, key[shortcut_scroll_down], "key-scroll-down", "Scroll down hotkey", "T-Down"),
    X(string, key[shortcut_scroll_up], "key-scroll-up", "Scroll up hotkey", "T-Up"),
    X(string, key[shortcut_view_next_cmd], "key-jump-next-cmd", "Jump to next command beginning hotkey", "T-F"),
    X(string, key[shortcut_view_prev_cmd], "key-jump-prev-cmd", "Jump to previous command beginning hotkey", "T-B"),
    X(nrcs, keyboard_nrcs, "keyboard-dialect", "National replacement character set to be used in non-UTF-8 mode", cs94_ascii),
    X(enum, mapping, "keyboard-mapping", "Initial keyboard mapping", keymap_default, keymap_legacy, (const char *[]){"legacy", "vt220", "hp", "sun", "sco", NULL}),
    X(int16, line_spacing, "line-spacing", "Additional lines vertical spacing", 0, -100, 100),
    X(boolean, lock, "lock-keyboard", "Disable keyboard input", false),
    G1(enum, log_level, 'L', "log-level", "Filtering level of logged information", 3, 0, (const char *[]){"quiet", "fatal", "warn", "info", NULL}),
    X(boolean, allow_luit, "luit", "Run luit if terminal doesn't support encoding by itself", true),
    X(string, luit, "luit-path", "Path to luit executable", "/usr/bin/luit"),
    X(enum, margin_bell_volume, "margin-bell", "Margin bell setting", 2, 0, (const char *[]){"off", "low", "high", NULL}),
    X(int16, margin_bell_column, "margin-bell-column", "Column at which margin bell rings when armed", 10, 0, 200),
    X(uint8, margin_bell_high_volume, "margin-bell-high-volume", "High volume value for DECSWBV", 100, 0, 100),
    X(uint8, margin_bell_low_volume, "margin-bell-low-volume", "Low volume value for DECSWBV", 50, 0, 100),
    X(time, max_frame_time, "max-frame-time", "Maximum time between frames in microseconds", -1, 0, 10*SEC/1000),
    X(boolean, meta_is_esc, "meta-sends-escape", "Alt/Meta sends escape prefix instead of setting 8-th bit", true),
    X(uint8, modify_cursor, "modify-cursor", "Enable encoding modifiers for cursor keys", 3, 0, 3),
    X(uint8, modify_function, "modify-function", "Enable encoding modifiers for function keys", 3, 0, 3),
    X(uint8, modify_keypad, "modify-keypad", "Enable encoding modifiers keypad keys", 3, 0, 3),
    X(uint8, modify_other, "modify-other", "Enable encoding modifiers for other keys", 0, 0, 4),
    X(enum, modify_other_fmt, "modify-other-fmt", "Format of encoding modifiers", 0, 0, (const char *[]){"xterm", "csi-u", NULL}),
    X(boolean, allow_nrcs, "nrcs", "Enable NRCSs support", 1),
    X(boolean, numlock, "numlock", "Initial numlock state", 1),
    G(string, open_command, "open-cmd", "A command used for opening URIs when clicked", "nsst-open"),
    G(string, notify_command, "notify-cmd", "A command used for sending desktop notifications", "notify-send"),
    X(boolean, override_boxdraw, "override-boxdrawing", "Use built-in box drawing characters", false),
    X(string, uri_click_mod, "uri-click-mod", "keyboard modifier used to click-open URIs", ""),
    G(boolean, unique_uris, "unique-uris", "Make distinction between URIs with the same location", false),
    X(enum, pixel_mode, "pixel-mode", "Subpixel rendering config; mono, bgr, rgb, bgrv, or rgbv", pixmode_mono, pixmode_mono, (const char *[]){"mono", "bgr", "rgb", "bgrv", "rgbv", NULL}),
    X(boolean, print_attr, "print-attributes", "Print cell attributes when printing is enabled", true),
    X(string, printer_cmd, "print-command", "Program to pipe CSI MC output into", NULL),
    X1(string, printer_file, 'o', "printer-file", "File where CSI MC output to", NULL),
    X(boolean, raise_on_bell, "raise-on-bell", "Raise terminal window on bell", false),
    X(boolean, reverse_video, "reverse-video", "Initial reverse video setting", false),
    X(color, palette[SPECIAL_REVERSE], "reversed-color", "Special color of reversed text", COLOR_SPECIAL_REVERSE),
    X(int16, scroll_amount, "scroll-amount", "Number of lines scrolled in a time", 2, 0, 1000),
    X(boolean, scroll_on_input, "scroll-on-input", "Scroll view to bottom on key press", true),
    X(boolean, scroll_on_output, "scroll-on-output", "Scroll view to bottom when character in printed", false),
    X1(int64, scrollback_size, 'H', "scrollback-size", "Number of saved lines", 10000, 0, 1000000000),
    X(time, select_scroll_time, "select-scroll-time", "Delay between scrolls of window while selecting with mouse", 10000, 0, 10*SEC/1000),
    X(boolean, select_to_clipboard, "select-to-clipboard", "Use CLIPBOARD selection to store highlighted data", false),
    X(color, palette[SPECIAL_SELECTED_BG],"selected-background", "Color of selected background", COLOR_SPECIAL_SELECTED_BG),
    X(color, palette[SPECIAL_SELECTED_FG],"selected-foreground", "Color of selected text", COLOR_SPECIAL_SELECTED_FG),
    X(string, shell, "shell", "Shell to start in new instance", "/bin/sh"),
    X(boolean, smooth_scroll, "smooth-scroll", "Initial value of DECSCLM mode", false),
    X(time, smooth_scroll_delay, "smooth-scroll-delay", "Delay between scrolls when DECSCLM is enabled", 500, 0, 10*SEC/1000),
    X(int16, smooth_scroll_step, "smooth-scroll-step", "Amount of lines per scroll when DECSCLM is enabled", 1, 1, 100000),
    G1(string, sockpath, 's', "socket", "Daemon socket path", "/tmp/nsst-sock0"),
    X(boolean, special_blink, "special-blink", "Replace blinking attribute with the corresponding special", false),
    X(boolean, special_bold, "special-bold", "Replace bold attribute with the corresponding special", false),
    X(boolean, special_italic, "special-italic", "Replace italic attribute with the corresponding special", false),
    X(boolean, special_reverse, "special-reverse", "Replace reverse attribute with the corresponding special", false),
    X(boolean, special_underline, "special-underlined", "Replace underlined attribute with the corresponding special", false),
    X(boolean, allow_subst_font, "substitute-fonts", "Enable substitute font support", true),
    X(time, sync_time, "sync-timeout", "Synchronous update timeout", SEC/2000, 0, 10*SEC/1000),
    X(int16, tab_width, "tab-width", "Initial width of tab character", 8, 1, 1000),
    X(string, term_mod, "term-mod", "Meaning of 'T' modifier", "SC"),
    X1(string, terminfo, 'D', "term-name", "TERM value", "xterm"),
    X2(string, title, 'T', 't', "title", "Initial window title", "Not So Simple Terminal"),
    G(boolean, trace_characters, "trace-characters", "Trace interpreted characters", false),
    G(boolean, trace_controls, "trace-controls", "Trace interpreted control characters and sequences", false),
    G(boolean, trace_events, "trace-events", "Trace received events", false),
    G(boolean, trace_fonts, "trace-fonts", "Log font related information", false),
    G(boolean, trace_input, "trace-input", "Trace user input", false),
    G(boolean, trace_misc, "trace-misc", "Trace miscellaneous information", false),
    X(time, triple_click_time, "triple-click-time", "Maximum time between second and third button presses of the triple click", 600000, 0, 10*SEC/1000),
    X(color, palette[SPECIAL_UNDERLINE], "underlined-color", "Special color of underlined text", COLOR_SPECIAL_UNDERLINE),
    X(int16, underline_width, "underline-width", "Text underline width", 1, 0, 16),
    X(boolean, urgency_on_bell, "urgent-on-bell", "Set window urgency on bell", false),
    X(color, palette[SPECIAL_URI_TEXT], "uri-color", "Special color of URI text", COLOR_SPECIAL_URI_TEXT),
    X(enum, uri_mode, "uri-mode", "Allow URI parsing/clicking", uri_mode_auto, uri_mode_off, (const char *[]){"off", "manual", "auto", NULL}),
    G(enum, backend, "backend", "Select rendering backend", renderer_auto, renderer_auto, (const char *[]){"auto", "x11", "wayland", "x11xrender", "x11shm", "waylandshm", NULL}),
    X(color, palette[SPECIAL_URI_UNDERLINE], "uri-underline-color", "Special color of URI underline", COLOR_SPECIAL_URI_UNDERLINE),
    X(boolean, utf8, "use-utf8", "Enable UTF-8 I/O", true),
    X(int16, border.top, "top-border", "Top border size", 8, 0, 200),
    X(int16, border.bottom, "bottom-border", "Bottom border size", 8, 0, 200),
    X(border, border, "border", "Border size", -1, 0, 200),
    X(vertical_border, border, "vertical-border", "Vertical border size (deprecated)", -1, 0, 200),
    X(horizontal_border, border, "horizontal-border", "Horizontal border size (deprecated)", -1, 0, 200),
    X(boolean, visual_bell, "visual-bell", "Whether bell should be visual or normal", false),
    X(time, visual_bell_time, "visual-bell-time", "Duration of the visual bell", 200000, 0, 10*SEC/1000),
    X1(int16, vt_version, 'V', "vt-version", "Emulated VT version", 420, 0, 999),
    X1(string, window_class, 'c', "window-class", "X11 Window class", NULL),
    X(boolean, allow_window_ops, "window-ops", "Allow window manipulation with escape sequences", true),
    X(time, wait_for_configure_delay, "wait-for-configure-delay", "Duration terminal waits for application output before redraw after window resize", 2000, 0, 10*SEC/1000),
    X(string, word_separators, "word-break", "Symbols treated as word separators when snapping mouse selection", " \t!$^*()+={}[]\\\"'|,;<>~`"),
    X(boolean, smooth_resize, "smooth-resize", "Don't force window size to be aligned on character size", false),
    X(string, pointer_shape, "pointer-shape", "Default mouse pointer shape for the window", "xterm"),
};

static inline color_t default_color(uint32_t n) {
    static_assert(CN_GRAY + CN_EXT + CN_BASE == PALETTE_SIZE - SPECIAL_PALETTE_SIZE, "Palette size mismatch");
    static_assert(PALETTE_SIZE - SPECIAL_PALETTE_SIZE == 256, "Palette size mismatch");
    assert(n < PALETTE_SIZE - SPECIAL_PALETTE_SIZE);

    if (n < CN_BASE) return BASE_COLOR(n);
    if (n < CN_BASE + CN_EXT) return COLOR_8B(n - CN_BASE);
    return COLOR_GRAY(n - CN_BASE - CN_EXT);
}

#define NO_INDEX SIZE_MAX

static inline size_t short_opt_i(char ch) {
    if ('a' <= ch && ch <= 'z')
        return ch - 'a';
    if ('A' <= ch && ch <= 'Z')
        return ch - 'A' + ('z' - 'a');
    return NO_INDEX;
}

static bool option_eq(const ht_head_t *a, const ht_head_t *b) {
    struct option *opt_a = (struct option *)a;
    struct option *opt_b = (struct option *)b;
    return !strcmp(opt_a->name, opt_b->name);
}

void free_options(void) {
    ht_iter_t it = ht_begin(&options_hashtable);
    while (ht_current(&it))
        ht_erase_current(&it);
    ht_free(&options_hashtable);
}

void init_options(void) {

    /* This is needed to get error messages from option parsing code */
    gconfig.log_level = 3;
    gconfig.log_color = isatty(STDERR_FILENO) && !getenv("NO_COLOR");

    /* Preprocess options metadata */
    ht_init(&options_hashtable, 2 * LEN(options), option_eq);

    const size_t short_opt_len = strlen(", -X<>");
    const size_t long_opt_len = strlen("\t --=<>");

    for (size_t i = 0; i < LEN(options); i++) {
        /* Build fast lookup hash table */
        struct option *opt = &options[i];
        opt->head.hash = hash64(opt->name, strlen(opt->name));
        ht_insert(&options_hashtable, &opt->head);

        /* Build fast lookup table for short options */
        size_t n_short = 0;
        for (size_t j = 0; j < MAX_SHORT_OPT; j++) {
            size_t idx = short_opt_i(opt->short_opt[j]);
            if (idx == NO_INDEX) continue;
            short_opts[idx] = opt;
            n_short++;
        }

        /* Calculate maximal help line length */
        ssize_t help_line_len =
            long_opt_len + option_types[opt->type].name_len + strlen(opt->name) +
            n_short * (short_opt_len + option_types[opt->type].name_len);
        if (help_line_len > max_help_line_len)
            max_help_line_len = help_line_len;

        /* We cannot do type checking in compile time, so check it here */
        if (opt->field_size != option_types[opt->type].type_size) {
            die("Wrong field size for option '%s' (%d) of type '%s' (%d)",
                    opt->name, opt->field_size,
                    option_types[opt->type].name,
                    option_types[opt->type].type_size);
        }
    }

    /* Initialize cached hostname
     * (let's assume that it won't change when terminal is running) */
    gethostname(gconfig.hostname, MAX_DOMAIN_NAME - 1);

    char *charset = nl_langinfo(CODESET);
    if (charset) {
        /* Builtin support for locales only include UTF-8, Latin-1 and ASCII */
        // TODO: Check for supported NRCSs and prefer them to luit
        bool utf8 = !strncasecmp(charset, "UTF", 3) && (charset[3] == '8' || charset[4] == '8');
        bool supported = 0;
        const char *lc_supported[] = {
            "C",
            "POSIX",
            "ASCII",
            "US-ASCII",
            "ANSI_X3.4-1968",
            "ISO-8869-1",
            "ISO8869-1",
        };

        for (size_t i = 0; !supported && i < LEN(lc_supported); i++)
            supported |= !strcasecmp(charset, lc_supported[i]);

        struct option *opt = find_option_entry("use-utf8", true);
        opt->limits.arg_boolean.dflt = utf8;

        gconfig.want_luit = !supported && !utf8;
    }

    dpi_option_entry = find_option_entry("dpi", true);
}

bool is_boolean_option(struct option *opt) {
    return opt && opt->type == option_type_boolean;
}

struct option *find_short_option_entry(char name) {
    size_t idx = short_opt_i(name);
    if (idx == NO_INDEX) return NULL;

    struct option *result = short_opts[idx];
    if (!result)
        warn("Unknown option: '-%c'", name);
    return result;
}

struct option *find_option_entry(const char *name, bool need_warn) {
    static struct option opt;
    unsigned color_idx;

    /* Color options are special --- they are dynamically generated. */
    if (sscanf(name, "color%u", &color_idx) == 1 &&
            color_idx < PALETTE_SIZE - SPECIAL_PALETTE_SIZE) {
        opt = (struct option) X(color, palette[color_idx], name,
                                "Color of terminal palette", default_color(color_idx));
        return &opt;
    }

    opt.name = name;
    opt.head.hash = hash64(name, strlen(name));

    struct option *result = (struct option *)ht_find(&options_hashtable, &opt.head);
    if (need_warn && !result)
        warn("Unknown option: '--%s'", name);
    return result;
}

bool set_option_entry(struct instance_config *c, struct option *opt, const char *value, bool allow_global) {
    if (opt->global && !allow_global)
        return false;

    void *dest = opt->offset + (opt->global ? (char *)&gconfig : (char *)c);
    if (!option_types[opt->type].parse(value, dest, &opt->limits)) {
        warn("Invalid value: %s=\"%s\"", opt->name, value);
        return false;
    }

    if (gconfig.trace_misc)
        info("Option set: %s=\"%s\"", opt->name, value);
    return true;
}

void set_default_dpi(double dpi, struct instance_config *cfg) {
    if (cfg->dpi == dpi_option_entry->limits.arg_double.dflt) cfg->dpi = dpi;
    dpi_option_entry->limits.arg_double.dflt = dpi;
}

static bool do_parse_double(const char *str, void *dst, union opt_limits *limits) {
    double result = 0, *pdst = dst;

    if (!strcasecmp(str, "default")) {
        result = limits->arg_double.dflt;
    } else if (sscanf(str, "%lf", &result) == 1) {
        if (result > limits->arg_double.max)
            result = limits->arg_double.max;
        if (result < limits->arg_double.min)
            result = limits->arg_double.min;
    } else return false;

    *pdst = result;
    return true;
}

static bool do_parse_nrcs(const char *str, void *dst, union opt_limits *limits) {
    enum charset result, *pdst = dst;
    if (!strcasecmp(str, "default")) result = limits->arg_nrcs.dflt;
    else if (!str[1] && str[0] > 0x2F && str[0] < 0x7F) {
        uint32_t sel = E(str[0]);
        result = nrcs_parse(sel, 0, 5, 1);
        if (result < 0) result = nrcs_parse(sel, 1, 5, 1);
    } else if (str[0] >= 0x20 && str[0] < 0x30 &&
               str[1] > 0x2F && str[1] < 0x7F && !str[3]) {
        uint32_t sel = E(str[1]) | I0(str[0]);
        result = nrcs_parse(sel, 0, 5, 1);
        if (result == nrcs_invalid) result = nrcs_parse(sel, 1, 5, 1);
        if (result == nrcs_invalid) return false;
    } else return false;

    *pdst = result;
    return true;
}

static bool do_parse_boolean(const char *str, void *dst, union opt_limits *limits) {
    bool result, *pdst = dst;
    if (!strcasecmp(str, "default")) {
        result = limits->arg_boolean.dflt;
    } else if (!strcasecmp(str, "true") || !strcasecmp(str, "yes") ||
               !strcasecmp(str, "y") || !strcmp(str, "1")) {
        result = true;
    } else if (!strcasecmp(str, "false") || !strcasecmp(str, "no") ||
               !strcasecmp(str, "n") || !strcmp(str, "0")) {
        result = false;
    } else return false;
    *pdst = result;
    return true;
}

static bool do_parse_int64(const char *str, void *dst, union opt_limits *limits) {
    int64_t result = 0, *pdst = dst;

    if (!strcasecmp(str, "default")) {
        result = limits->arg_int64.dflt;
    } else {
        errno = 0;
        char *end;
        result = strtoll(str, &end, 0);
        if (!end || *end || errno)
            return false;
        result = MIN(result, limits->arg_int64.max);
        result = MAX(result, limits->arg_int64.min);
    }

    *pdst = result;
    return true;
}

static bool do_parse_time(const char *str, void *dst, union opt_limits *limits) {
    int64_t result = 0, *pdst = dst;

    if (!strcasecmp(str, "default")) {
        result = limits->arg_time.dflt;
    } else {
        errno = 0;
        char *end;
        long frac = 0, fscale = SEC;
        result = strtoll(str, &end, 0);
        if (!end || errno)
            return false;

        if (*end == '.') {
            for (end++; isdigit((unsigned)*end); end++) {
                if (fscale == 1) continue;
                fscale /= 10;
                frac = frac*10 + (*end - '0');
            }
            frac *= fscale;
        }

        // FIXME Store time in ns

        if (*end) {
            int64_t unit = 1;
            if (!strcmp(end, "s"))
                unit = SEC/1000L;
            else if (!strcmp(end, "ms"))
                unit = SEC/1000000L;
            //else if (!strcmp(end, "ns"))
            //    unit = SEC/1000L;
            else if (!strcmp(end, "us"))
                unit = SEC/1000000000L;
            else
                return false;

            if (__builtin_mul_overflow(result, unit, &result))
                result = limits->arg_time.max;
        }

        result += frac/1000L;
        result = MIN(result, limits->arg_int64.max);
        result = MAX(result, limits->arg_int64.min);
    }

    *pdst = result;
    return true;
}

static bool do_parse_uint8(const char *str, void *dst, union opt_limits *limits) {
    int64_t val64 = 0;
    uint8_t *pdst = dst;
    if (!do_parse_int64(str, &val64, limits))
        return false;
    *pdst = val64;
    return true;
}

static bool do_parse_border(const char *str, void *dst, union opt_limits *limits) {
    struct border *border = dst;
    int64_t val64;
    if (!do_parse_int64(str, &val64, limits))
        return false;

    if (val64 != -1) {
        border->left = border->right = val64;
        border->top = border->bottom = val64;
    }

    return true;
}

static bool do_parse_vertical_border(const char *str, void *dst, union opt_limits *limits) {
    struct border *border = dst;
    int64_t val64;
    if (!do_parse_int64(str, &val64, limits))
        return false;

    if (val64 != -1) {
        warn("--vertical-border is obsolete, use --top-border, --bottom-border or --border instead");
        border->top = border->bottom = val64;
    }

    return true;
}

static bool do_parse_horizontal_border(const char *str, void *dst, union opt_limits *limits) {
    struct border *border = dst;
    int64_t val64;
    if (!do_parse_int64(str, &val64, limits))
        return false;

    if (val64 != -1) {
        warn("--horizontal-border is obsolete, use --left-border, --right-border or --border instead");
        border->left = border->right = val64;
    }

    return true;
}

static bool do_parse_int16(const char *str, void *dst, union opt_limits *limits) {
    int64_t val64 = 0;
    uint8_t *pdst = dst;
    if (!do_parse_int64(str, &val64, limits))
        return false;
    *pdst = val64;
    return true;
}

static bool do_parse_enum(const char *str, void *dst, union opt_limits *limits) {
    int result, *pdst = dst;
    if (!strcasecmp(str, "default")) {
        result = limits->arg_enum.dflt;
    } else {
        const char **it = limits->arg_enum.values;
        int index = limits->arg_enum.start;
        bool has_value = false;
        do if (!strcasecmp(str, *it)) {
                result = index;
                has_value = true;
                break;
        } while (++index, *++it);
        if (!has_value) return false;
    }

    *pdst = result;
    return true;
}

static bool do_parse_string(const char *str, void *dst, union opt_limits *limits) {
    char *result = NULL, **pdst = dst;
    if (!strcasecmp(str, "default")) {
        const char *r = limits->arg_string.dflt;
        if (r) result = strdup(r);
    } else result = strdup(str);

    if (*pdst) free(*pdst);
    *pdst = result;
    return true;
}

static bool do_parse_color(const char *str, void *dst, union opt_limits *limits) {
    color_t result, *pdst = dst;
    if (!strcasecmp(str, "default")) {
        result = limits->arg_color.dflt;
    } else {
        const uint8_t *end = (const uint8_t *)str + strlen(str);
        result = parse_color((const uint8_t *)str, end);
        if (!result) return false;
    }

    *pdst = result;
    return true;
}

static bool do_parse_geometry(const char *value, void *dst, union opt_limits *limits) {
    struct geometry *g = dst;

    if (!strcasecmp(value, "default")) {
        *g = (struct geometry) {
            .char_geometry = true,
            .has_extent = true,
            .r = { .width = 80, .height = 24, }
        };
        return true;
    }

    int16_t x = 0, y = 0, w = 0, h = 0;
    char xsgn[2] = "+", ysgn[2] = "+";

    if (value[0] == '=') value++;

    if (value[0] == '+' || value[0] == '-') {
        if (sscanf(value, "%1[-+]%"SCNd16"%1[-+]%"SCNd16, xsgn, &x, ysgn, &y) != 4)
            return false;
        g->has_position = true;
        g->has_extent = false;
        g->char_geometry = true;
        g->r.width = 80;
        g->r.height = 24;
    } else {
        int res = sscanf(value, "%"SCNd16"%*[xX]%"SCNd16"%1[-+]%"SCNd16"%1[-+]%"SCNd16,
                &w, &h, xsgn, &x, ysgn, &y);
        if (res == 6) {
            g->has_position = true;
        } else if (res == 2) {
            g->has_position = false;
            x = 0, y = 0;
        } else return false;
        g->has_extent = true;
        g->char_geometry = limits->arg_geometry.in_char;
        g->r.width = w;
        g->r.height = h;
    }

    if (xsgn[0] == '-') {
        g->stick_to_right = true;
        x = -x;
    }

    if (ysgn[0] == '-') {
        g->stick_to_bottom = true;
        y = -y;
    }

    g->r.x = x;
    g->r.y = y;

    return true;
}

void copy_config(struct instance_config *dst, struct instance_config *src) {
    *dst = *src;
    src->argv = NULL;
    for (size_t i = 0; i < LEN(options); i++) {
        if (options[i].global || options[i].type != option_type_string) continue;
        char *dst1 = (char *)dst + options[i].offset, *value;
        memcpy(&value, dst1, sizeof value);
        if (value) value = strdup(value);
        memcpy(dst1, &value, sizeof value);
    }
}

void free_config(struct instance_config *src) {
    for (size_t i = 0; i < LEN(options); i++) {
        if (options[i].global || options[i].type != option_type_string) continue;
        char *src1 = (char *)src + options[i].offset, *value;
        memcpy(&value, src1, sizeof value);
        free(value);
    }
}

static void parse_config(struct instance_config *cfg, bool allow_global) {
    char pathbuf[PATH_MAX];
    const char *path = cfg->config_path;
    int fd = -1;

    /* Config file is search in following places:
     * 1. Path set with --config=
     * 2. $XDG_CONFIG_HOME/nsst.conf
     * 3. $HOME/.config/nsst.conf
     * If file is not found in those places, just give up */

    if (path) fd = open(path, O_RDONLY);

    const char *xdg_cfg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");

    if (fd < 0 && xdg_cfg) {
        snprintf(pathbuf, sizeof pathbuf, "%s/nsst.conf", xdg_cfg);
        fd = open(pathbuf, O_RDONLY);
    }

    if (fd < 0 && xdg_cfg) {
        snprintf(pathbuf, sizeof pathbuf, "%s/nsst/nsst.conf", xdg_cfg);
        fd = open(pathbuf, O_RDONLY);
    }

    if (fd < 0 && home) {
        snprintf(pathbuf, sizeof pathbuf, "%s/.config/nsst.conf", home);
        fd = open(pathbuf, O_RDONLY);
    }

    if (fd < 0 && home) {
        snprintf(pathbuf, sizeof pathbuf, "%s/.config/nsst/nsst.conf", home);
        fd = open(pathbuf, O_RDONLY);
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
            struct option *opt = find_option_entry(name_start, true);
            if (opt) set_option_entry(cfg, opt, value_start, allow_global);
            SWAP(*name_end, saved2);
            SWAP(*value_end, saved1);
        } else if (*ptr == '#') {
            while (ptr < end && *ptr != '\n') ptr++;
        } else {
e_wrong_line:
            ptr = start;
            while (ptr < end && *ptr != '\n') ptr++;
            SWAP(*ptr, saved1);
            warn("Can't parse config line #%zd: %s", line_n, start);
            SWAP(*ptr, saved1);
            ptr++;
        }
    }

    munmap(addr, stt.st_size + 1);

e_open:
    if (fd < 0) warn("Can't read config file: %s", path ? path : pathbuf);

}

void init_instance_config(struct instance_config *cfg, const char *config_path, bool allow_global) {
    struct option *cpath = find_short_option_entry('C');
    assert(cpath);

    for (size_t i = 0; i < LEN(options); i++)
        if (&options[i] != cpath)
            set_option_entry(cfg, &options[i], "default", allow_global);
    for (size_t i = 0; i < PALETTE_SIZE - SPECIAL_PALETTE_SIZE; i++)
        cfg->palette[i] = default_color(i);

    if (config_path)
        set_option_entry(cfg, cpath, config_path, 0);

    parse_config(cfg, allow_global);

    /* Initialize max frame time depending on FPS */
    if (cfg->max_frame_time == -1)
        cfg->max_frame_time = 2*SEC/(1000*cfg->fps);

    /* Parse all shortcuts */
    keyboard_parse_config(cfg);
}


const char *usage_string(ssize_t idx) {
#define MAX_OPTION_DESC 512
#define APPEND(...) pbuf += snprintf(pbuf, MAX_OPTION_DESC - (pbuf - buffer), __VA_ARGS__)
    static char buffer[MAX_OPTION_DESC + 1];
    char *pbuf = buffer;

    if (!idx) {
        APPEND( /* argv0 here*/ " [-options] [-e] [command [args]]\n" "Where options are:\n");
        APPEND("%-*s(Print this message and exit)\n", (int)max_help_line_len, "\t--help, -h");
        APPEND("%-*s(Print version and exit)\n", (int)max_help_line_len, "\t--version, -v");
        APPEND("%-*s(Set palette color <N>, <N> is from 0 to 255)\n", (int)max_help_line_len, "\t--color<N>=<color>");
        return buffer;
    } else if (idx - 1 < (ssize_t)LEN(options)) {
        struct option *opt = &options[idx - 1];
        struct option_type_desc *type = &option_types[opt->type];

        APPEND("\t--%s=<%s>", opt->name, type->name);
        for (size_t i = 0; i < MAX_SHORT_OPT; i++)
            if (opt->short_opt[i])
                APPEND(", -%c<%s>", opt->short_opt[i], type->name);

        /* Pad current buffer with spaces to make everything nicely aligned */
        while (pbuf - buffer < max_help_line_len) *pbuf++ = ' ';

        APPEND("(%s)\n", opt->description);
        return buffer;
    } else if (idx == LEN(options) + 1) {
        return  "For every boolean option --<X>=<Y>\n"
                "\t--<X>, --<X>=yes, --<X>=y,  --<X>=true\n"
            "are equivalent to --<X>=1, and\n"
                "\t--no-<X>, --<X>=no, --<X>=n, --<X>=false\n"
            "are equivalent to --<X>=0,\n"
            "where 'yes', 'y', 'true', 'no', 'n' and 'false' are case independent.\n"
            "Time options accept units 's', 'us', 'ms'. If unit is not specified 'us' is implied\n"
            "All options are also accept special value 'default' to reset to built-in default.\n";
    } else return NULL;
#undef APPEND
}
