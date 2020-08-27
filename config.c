/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#include "feature.h"

#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "input.h"
#include "nrcs.h"
#include "util.h"
#include "window.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CN_BASE 16
#define CN_EXT (6*6*6)
#define CN_GRAY (PALETTE_SIZE - CN_BASE - CN_EXT)
#define SD28B(x) ((x) ? 0x37 + 0x28 * (x) : 0)

struct optmap_item optmap[OPT_MAP_SIZE] = {
    {"allow-alternate", "\t(Enable alternate screen)", "allowAlternate", ICONF_ALLOW_ALTSCREEN},
    {"allow-blinking", "\t(Allow blinking text and cursor)", "allowBlinking", ICONF_ALLOW_BLINKING},
    {"allow-modify-edit-keypad", " (Allow modifing edit keypad keys)", "modkeyAllowEditKeypad", ICONF_MALLOW_EDIT},
    {"allow-modify-function", "\t(Allow modifing function keys)", "modkeyAllowFunction", ICONF_MALLOW_FUNCTION},
    {"allow-modify-keypad", "\t(Allow modifing keypad keys)", "modkeyAllowKeypad", ICONF_MALLOW_KEYPAD},
    {"allow-modify-misc", "\t(Allow modifing miscelleneous keys)", "modkeyAllowMisc", ICONF_MALLOW_MISC},
    {"alpha", "\t\t\t(Backround opacity, requires compositor to be running)", "alpha", ICONF_ALPHA},
    {"alternate-scroll", "\t(Scrolling sends arrow keys escapes in alternate screen)", "alternateScroll", ICONF_ALTERNATE_SCROLL},
    {"answerback-string", "\t(ENQ report)", "answerbackString", SCONF_ANSWERBACK_STRING},
    {"appcursor", "\t\t(Initial application cursor mode value)", "appcursor", ICONF_APPCURSOR},
    {"appkey", "\t\t(Initial application keypad mode value)", "appkey", ICONF_APPKEY},
    {"autowrap", "\t\t(Initial autowrap setting)", "enableAutowrap", ICONF_INIT_WRAP},
    {"background", "\t\t(Default backround color)", "background", CCONF_BG},
    {"backspace-is-del", "\t(Backspace sends DEL instead of BS)", "backspaceIsDelete", ICONF_BACKSPACE_IS_DELETE},
    {"bell", "\t\t\t(Bell setting)", "bell", ICONF_BELL_VOLUME},
    {"bell-high-volume", "\t(High volume value for DECSWBV)", "bellHighVolume", ICONF_BELL_HIGH_VOLUME},
    {"bell-low-volume", "\t(Low volume value for DECSWBV)", "bellLowVolume", ICONF_BELL_LOW_VOLUME},
    {"blink-color", "\t\t(Special color of blinking text)", "blinkColor", CCONF_BLINK},
    {"blink-time", "\t\t(Text blink interval in microseconds)","blinkTime", ICONF_BLINK_TIME},
    {"bold-color", "\t\t(Special color of bold text)", "boldColor", CCONF_BOLD},
    {"cursor-background", "\t(Default cursor backround color)", "cursorBackground", CCONF_CURSOR_BG},
    {"cursor-foreground", "\t(Default cursor foreround color)", "cursorForeground", CCONF_CURSOR_FG},
    {"cursor-shape", "\t\t(Shape of cursor)", "cursorShape", ICONF_CURSOR_SHAPE},
    {"cursor-width", "\t\t(Width of lines that forms cursor)", "cursorWidth", ICONF_CURSOR_WIDTH},
    {"cut-lines", "\t\t(Cut long lines on resize with rewrapping disabled)", "cutLines", ICONF_CUT_LINES},
    {"delete-is-del", "\t\t(Delete sends DEL symbol instead of escape sequence)", "deleteIsDelete", ICONF_DELETE_IS_DELETE},
    {"double-click-time", "\t(Time gap in milliseconds in witch two mouse presses will be considered double)", "doubleClickTime", ICONF_DOUBLE_CLICK_TIME},
    {"erase-scrollback", "\t(Allow ED 3 to clear scrollback buffer)", "eraseScrollback", ICONF_ALLOW_ERASE_SCROLLBACK},
    {"extended-cir", "\t\t(Report all SGR attributes in DECCIR)", "extendedCir", ICONF_EXTENDED_CIR},
    {"fixed", "\t\t\t(Don't allow to change window size, if supported)", "fixed", ICONF_FIXED_SIZE},
    {"fkey-increment", "\t(Step in numbering function keys)", "fkeyIncrement", ICONF_FKEY_INCREMENT},
    {"font", ", -f<value>\t(Comma-separated list of fontconfig font patterns)", "font", SCONF_FONT_NAME},
    {"font-gamma", "\t\t(Factor of sharpenning\t(king of hack))", "fontGamma",ICONF_GAMMA},
    {"font-size", "\t\t(Font size in points)", "fontSize", ICONF_FONT_SIZE},
    {"font-size-step", "\t(Font size step in points)", "fontSizeStep", ICONF_FONT_SIZE_STEP},
    {"font-spacing", "\t\t(Additional spacing for individual symbols)", "fontSpacing", ICONF_FONT_SPACING},
    {"force-dpi", "\t\t(DPI value for fonts)", "dpi", ICONF_DPI},
    {"force-mouse-mod", "\t(Modifer to force mouse action)", "forceMouseMod", SCONF_FORCE_MOUSE_MOD},
    {"force-nrcs", "\t\t(Enable NRCS translation when UTF-8 mode is enabled)", "forceNrcs", ICONF_FORCE_UTF8_NRCS},
    {"force-scalable", "\t(Do not search for pixmap fonts)", "forceScalable", ICONF_FORCE_SCALABLE},
    {"foreground", "\t\t(Default foreground color)", "foreground", CCONF_FG},
    {"fps", "\t\t\t(Window refresh rate)", "fps", ICONF_FPS},
    {"has-meta", "\t\t(Handle meta/alt)", "hasMeta", ICONF_HAS_META},
    {"horizontal-border", "\t(Top and bottom botders)", "horizontalBorder", ICONF_TOP_BORDER},
    {"italic-color", "\t\t(Special color of italic text)", "italicColor", CCONF_ITALIC},
    {"keep-clipboard", "\t(Reuse copied clipboard content instead of current selection data)", "keepClipboard", ICONF_KEEP_CLIPBOARD},
    {"keep-selection", "\t(Don't clear X11 selection when unhighlighted)", "keepSelection", ICONF_KEEP_SELECTION},
    {"key-break", "\t\t(Send break hotkey", "key.break)", KCONF_BREAK},
    {"key-dec-font", "\t\t(Decrement font size hotkey)", "key.decFontSize", KCONF_FONT_DEC},
    {"key-inc-font", "\t\t(Increment font size hotkey)", "key.incFontSize", KCONF_FONT_INC},
    {"key-new-window", "\t(Create new window hotkey)", "key.newWindow", KCONF_NEW_WINDOW},
    {"key-numlock", "\t\t('appkey' mode allow toggle hotkey)", "key.numlock", KCONF_NUMLOCK},
    {"key-reload-config", "\t(Reload config hotkey)", "key.reloadConfig", KCONF_RELOAD_CONFIG},
    {"key-reset", "\t\t(Terminal reset hotkey)", "key.reset", KCONF_RESET},
    {"key-reset-font", "\t(Reset font size hotkey)", "key.resetFontSize", KCONF_FONT_RESET},
    {"key-reverse-video", "\t(Toggle reverse video mode hotkey)", "key.reverseVideo", KCONF_REVERSE_VIDEO},
    {"key-scroll-down", "\t(Scroll down hotkey)", "key.scrollDown", KCONF_SCROLL_DOWN},
    {"key-scroll-up", "\t\t(Scroll up hotkey)", "key.scrollUp", KCONF_SCROLL_UP},
    {"keyboard-dialect", "\t(National replacement character set to be used in non-UTF-8 mode)", "keyboardDialect", ICONF_KEYBOARD_NRCS},
    {"keyboard-mapping", "\t(Initial keyboad mapping)", "keyboardMapping", ICONF_MAPPING},
    {"line-spacing", "\t\t(Additional lines vertical spacing)", "lineSpacing", ICONF_LINE_SPACING},
    {"lock-keyboard", "\t\t(Disable keyboad input)", "lockKeyboard", ICONF_LOCK},
    {"log-level","\t\t(Filering level of logged information)", "logLevel", ICONF_LOG_LEVEL},
    {"margin-bell", "\t\t(Margin bell setting)", "marginBell", ICONF_MARGIN_BELL_VOLUME},
    {"margin-bell-column", "\t(Columnt at which margin bell rings when armed)", "marginBellColumn", ICONF_MARGIN_BELL_COLUMN},
    {"margin-bell-high-volume", " (High volume value for DECSMBV)", "marginBellHighVolume", ICONF_MARGIN_BELL_HIGH_VOLUME},
    {"margin-bell-low-volume", "(Low volume value for DECSMBV)", "marginBellLowVolume", ICONF_MARGIN_BELL_LOW_VOLUME},
    {"meta-sends-escape", "\t(Alt/Meta sends escape prefix instead of setting 8-th bit)", "metaSendsEscape", ICONF_META_IS_ESC},
    {"minimize-scrollback", "\t(Realloc lines to save memory; makes scrolling a little slower)", "minimizeScrollback", ICONF_MINIMIZE_SCROLLBACK},
    {"modify-cursor", "\t\t(Enable encoding modifiers for cursor keys)", "modifyCursor", ICONF_MODIFY_CURSOR},
    {"modify-function", "\t(Enable encoding modifiers for function keys)", "modifyFunction", ICONF_MODIFY_FUNCTION},
    {"modify-keypad", "\t\t(Enable encoding modifiers keypad keys)", "modifyKeypad", ICONF_MODIFY_KEYPAD},
    {"modify-other", "\t\t(Enable encoding modifiers for other keys)", "modifyOther", ICONF_MODIFY_OTHER},
    {"modify-other-fmt", "\t(Format of encoding modifers)", "modifyOtherFmt", ICONF_MODIFY_OTHER_FMT},
    {"nrcs", "\t\t\t(Enable NRCSs support)", "allowNrcs", ICONF_ALLOW_NRCS},
    {"numlock", "\t\t(Initial numlock state)", "numlock", ICONF_NUMLOCK},
#if USE_BOXDRAWING
    {"override-boxdrawing", "\t(Use built-in box drawing characters)", "overrideBoxdrawing", ICONF_OVERRIDE_BOXDRAW},
#endif
    {"pixel-mode", "\t\t(Subpixel rendering config; mono, bgr, rgb, bgrv, or rgbv)", "pixelMode", ICONF_PIXEL_MODE},
    {"print-command", "\t\t(Program to pipe CSI MC output into)", "printerCommand", SCONF_PRINT_CMD},
    {"printer-file", ", -o<value> (File where CSI MC output to)", "printerFile", SCONF_PRINTER},
    {"print-attributes", "\t(Print cell attributes when printing is enabled)", "printAttributes", ICONF_PRINT_ATTR },
    {"raise-on-bell", "\t\t(Raise terminal window on bell)", "raiseOnBell", ICONF_RAISE_ON_BELL},
    {"resize-delay", "\t\t(Additional delay after resize in microseconds)", "resizeDelay", ICONF_RESIZE_DELAY},
    {"reverse-video", "\t\t(Initial reverse video setting)", "enableReverseVideo", ICONF_REVERSE_VIDEO},
    {"reversed-color", "\t(Special color of reversed text)", "reversedColor", CCONF_REVERSE},
    {"rewrap", "\t\t(Rewrap text on resize)", "rewrap", ICONF_REWRAP},
    {"scroll-amount", "\t\t(Number of lines scrolled in a time)", "scrollAmout", ICONF_SCROLL_AMOUNT},
    {"scroll-delay", "\t\t(Additional delay after scroll in microseconds)", "scrollDelay", ICONF_SCROLL_DELAY},
    {"scroll-on-input", "\t(Scroll view to bottom on key press)", "scrollOnInput", ICONF_SCROLL_ON_INPUT},
    {"scroll-on-output", "\t(Scroll view to bottom when character in printed)", "scrollOnOutput", ICONF_SCROLL_ON_OUTPUT},
    {"scrollback-size", ", -H<value> (Number of saved lines)", "scrollbackSize", ICONF_HISTORY_LINES},
    {"select-to-clipboard", "\t(Use CLIPBOARD selection to store hightlighted data)", "selectToClipboard", ICONF_SELECT_TO_CLIPBOARD},
    {"selected-background", "\t(Color of selected background)", "selectedBackground", CCONF_SELECTED_BG},
    {"selected-foreground", "\t(Color of selected text)", "selectedForeground", CCONF_SELECTED_FG},
    {"shell", ", -s<value>\t(Shell to start in new instance)", "shell", SCONF_SHELL},
    {"special-blink", "\t\t(If special color should be used for blinking text)", "specialBlink", ICONF_SPEICAL_BLINK},
    {"special-bold", "\t\t(If special color should be used for bold text)", "specialBold", ICONF_SPEICAL_BOLD},
    {"special-italic", "\t(If special color should be used for italic text)", "specialItalic", ICONF_SPEICAL_ITALIC},
    {"special-reverse", "\t(If special color should be used for reverse text)", "specialReverse", ICONF_SPEICAL_REVERSE},
    {"special-underlined", "\t(If special color should be used for underlined text)", "specialUnderlined", ICONF_SPEICAL_UNDERLINE},
    {"substitute-fonts", "\t(Enable substitute font support)", "substitudeFonts", ICONF_ALLOW_SUBST_FONTS},
    {"sync-timeout", "\t\t(Syncronous update timeout)", "syncTimeout", ICONF_SYNC_TIME},
    {"tab-width", "\t\t(Initial width of tab character)", "tabWidth", ICONF_TAB_WIDTH},
    {"term-mod", "\t\t(Meaning of 'T' modifer)", "termMod", SCONF_TERM_MOD},
    {"term-name", ", -D<value>\t(TERM value)", "termName", SCONF_TERM_NAME},
    {"title", ", -T<value>, -t<value> (Initial window title)", "title", SCONF_TITLE},
    {"trace-characters", "\t(Trace interpreted characters)", "traceCharacters", ICONF_TRACE_CHARACTERS},
    {"trace-controls", "\t(Trace interpreted control characters and sequences)", "traceControls", ICONF_TRACE_CONTROLS},
    {"trace-events", "\t\t(Trace recieved events)", "traceEvents", ICONF_TRACE_EVENTS},
    {"trace-fonts", "\t\t(Log font related information)", "traceFonts", ICONF_TRACE_FONTS},
    {"trace-input", "\t\t(Trace user input)", "traceInput", ICONF_TRACE_INPUT},
    {"trace-misc", "\t\t(Trace miscelleneous information)", "traceMisc", ICONF_TRACE_MISC},
    {"triple-click-time", "\t(Time gap in milliseconds in witch tree mouse presses will be considered triple)", "trippleClickTime", ICONF_TRIPLE_CLICK_TIME},
    {"underline-width", "\t(Text underline width)", "underlineWidth", ICONF_UNDERLINE_WIDTH},
    {"underlined-color", "\t(Special color of underlined text)", "underlinedColor", CCONF_UNDERLINE},
    {"urgent-on-bell", "\t(Set window urgency on bell)", "urgentOnBell", ICONF_URGENT_ON_BELL},
    {"use-utf8", "\t\t(Enable UTF-8 I/O)", "useUtf8", ICONF_UTF8},
    {"vertical-border", "\t(Left and right borders)", "verticalBorder", ICONF_LEFT_BORDER},
    {"visual-bell", "\t\t(Whether bell should be visual or normal)", "visualBell", ICONF_VISUAL_BELL},
    {"visual-bell-time", "\t(Length of visual bell)", "visualBellTime", ICONF_VISUAL_BELL_TIME},
    {"vt-version", ", -V<value>\t(Emulated VT version)", "vtVersion", ICONF_VT_VERION},
    {"window-class", ", -c<value> (X11 Window class)", "windowClass", SCONF_TERM_CLASS},
    {"window-ops", "\t\t(Allow window manipulation with escape sequences)", "allowWindowOps", ICONF_ALLOW_WINDOW_OPS},
    {"word-break", "\t\t(Symbols treated as word separators when snapping mouse selection)", "wordBreak", SCONF_WORD_SEPARATORS},
};

static struct {
    int32_t val;
    int32_t dflt;
    int32_t min;
    int32_t max;
} ioptions[] = {
    [ICONF_LOG_LEVEL - ICONF_MIN] = {3, 3, 0, 3},
    [ICONF_WINDOW_X - ICONF_MIN] = {200, 200, -32768, 32767 },
    [ICONF_WINDOW_Y - ICONF_MIN] = {200, 200, -32768, 32767 },
    [ICONF_WINDOW_NEGATIVE_X - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_WINDOW_NEGATIVE_Y - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_WINDOW_WIDTH - ICONF_MIN] = {800, 800, 1, 32767},
    [ICONF_WINDOW_HEIGHT - ICONF_MIN] = {600, 600, 1, 32767},
    [ICONF_FIXED_SIZE - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_HAS_GEOMETRY - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_HISTORY_LINES - ICONF_MIN] = {1024, 1024, -1, 100000},
    [ICONF_UTF8 - ICONF_MIN] = {1, 1, 0, 1},
    [ICONF_VT_VERION - ICONF_MIN] = {420, 420, 0, 999},
    [ICONF_FORCE_UTF8_NRCS - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_TAB_WIDTH - ICONF_MIN] = {8, 8, 1, 100},
    [ICONF_INIT_WRAP - ICONF_MIN] = {1, 1, 0, 1},
    [ICONF_SCROLL_ON_INPUT - ICONF_MIN] = {1, 1, 0, 1},
    [ICONF_SCROLL_ON_OUTPUT - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_CURSOR_SHAPE - ICONF_MIN] = {cusor_type_bar, cusor_type_bar, 1, 6},
    [ICONF_UNDERLINE_WIDTH - ICONF_MIN] = {1, 1, 0, 16},
    [ICONF_CURSOR_WIDTH - ICONF_MIN] = {2, 2, 0, 16},
    [ICONF_PIXEL_MODE - ICONF_MIN] = {0, 0, 0, 4},
    [ICONF_REVERSE_VIDEO - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_ALLOW_ALTSCREEN - ICONF_MIN] = {1, 1, 0, 1},
    [ICONF_LEFT_BORDER - ICONF_MIN] = {8, 8, 0, 100},
    [ICONF_TOP_BORDER - ICONF_MIN] = {8, 8, 0 , 100},
    [ICONF_BLINK_TIME - ICONF_MIN] = {800000, 800000, 0, 10000000},
    [ICONF_VISUAL_BELL_TIME - ICONF_MIN] = {200000, 200000, 0, 10000000},
    [ICONF_FONT_SIZE - ICONF_MIN] = {0, 0, 1, 200},
    [ICONF_FONT_SPACING - ICONF_MIN] = {0, 0, -100, 100},
    [ICONF_LINE_SPACING - ICONF_MIN] = {0, 0, -100, 100},
    [ICONF_GAMMA - ICONF_MIN] = {10000, 10000, 2000, 200000},
    [ICONF_DPI - ICONF_MIN] = {96, 96, 10, 10000},
    [ICONF_KEYBOARD_NRCS - ICONF_MIN] = {cs94_ascii, cs94_ascii, 0, nrcs_MAX},
    [ICONF_SKIP_CONFIG_FILE - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_ALLOW_NRCS - ICONF_MIN] = {1, 1, 0, 1},
    [ICONF_ALLOW_WINDOW_OPS - ICONF_MIN] = {1, 1, 0, 1},
#if USE_BOXDRAWING
    [ICONF_OVERRIDE_BOXDRAW - ICONF_MIN] = {0, 0, 0, 1},
#endif
    [ICONF_FPS - ICONF_MIN] = {60, 60, 2, 1000},
    [ICONF_SCROLL_DELAY - ICONF_MIN] = {SEC/180000, SEC/180000, 0, 10*SEC/1000},
    [ICONF_RESIZE_DELAY - ICONF_MIN] = {SEC/60000, SEC/60000, 0, 10*SEC/1000},
    [ICONF_SYNC_TIME - ICONF_MIN] = {SEC/2000, SEC/2000, 0, 10*SEC/1000},
    [ICONF_SCROLL_AMOUNT - ICONF_MIN] = {2, 2, 1, 100},
    [ICONF_FONT_SIZE_STEP - ICONF_MIN] = {1, 1, 1, 250},
    [ICONF_ALTERNATE_SCROLL - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_DOUBLE_CLICK_TIME - ICONF_MIN] = {300, 300, 10, 1000000},
    [ICONF_TRIPLE_CLICK_TIME - ICONF_MIN] = {600, 600, 10, 1000000},
    [ICONF_KEEP_CLIPBOARD - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_KEEP_SELECTION - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_SELECT_TO_CLIPBOARD - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_ALLOW_BLINKING - ICONF_MIN] = {1, 1, 0, 1},
    [ICONF_EXTENDED_CIR - ICONF_MIN] = {1, 1, 0, 1},
    [ICONF_SPEICAL_BOLD - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_SPEICAL_BLINK - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_SPEICAL_UNDERLINE - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_SPEICAL_ITALIC - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_SPEICAL_REVERSE - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_MARGIN_BELL_COLUMN - ICONF_MIN] = {10, 10, 0, 100},
    [ICONF_MARGIN_BELL_VOLUME - ICONF_MIN] = {0, 0, 0, 2},
    [ICONF_BELL_VOLUME - ICONF_MIN] = {2, 2, 0, 2},
    [ICONF_BELL_LOW_VOLUME - ICONF_MIN] = {50, 50, 0, 100},
    [ICONF_MARGIN_BELL_LOW_VOLUME - ICONF_MIN] = {50, 50, 0, 100},
    [ICONF_MARGIN_BELL_HIGH_VOLUME - ICONF_MIN] = {100, 100, 0, 100},
    [ICONF_BELL_HIGH_VOLUME - ICONF_MIN] = {100, 100, 0, 100},
    [ICONF_VISUAL_BELL - ICONF_MIN] = {1, 1, 0, 1},
    [ICONF_RAISE_ON_BELL - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_URGENT_ON_BELL - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_ALLOW_ERASE_SCROLLBACK - ICONF_MIN] = {1, 1, 0, 1},
    [ICONF_TRACE_CHARACTERS - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_TRACE_CONTROLS - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_TRACE_EVENTS - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_TRACE_FONTS - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_TRACE_INPUT - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_TRACE_MISC - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_APPCURSOR - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_APPKEY - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_BACKSPACE_IS_DELETE - ICONF_MIN] = {1, 1, 0, 1},
    [ICONF_DELETE_IS_DELETE - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_FKEY_INCREMENT - ICONF_MIN] = {10, 10, 0, 48},
    [ICONF_HAS_META - ICONF_MIN] = {1, 1, 0, 1},
    [ICONF_MAPPING - ICONF_MIN] = {keymap_default, keymap_default, 0, keymap_MAX},
    [ICONF_LOCK - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_META_IS_ESC - ICONF_MIN] = {1, 1, 0, 1},
    [ICONF_MODIFY_CURSOR - ICONF_MIN] = {3, 3, 0, 3},
    [ICONF_MODIFY_FUNCTION - ICONF_MIN] = {3, 3, 0, 3},
    [ICONF_MODIFY_KEYPAD - ICONF_MIN] = {3, 3, 0, 3},
    [ICONF_MODIFY_OTHER - ICONF_MIN] = {0, 0, 0, 4},
    [ICONF_MODIFY_OTHER_FMT - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_MALLOW_EDIT - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_MALLOW_FUNCTION - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_MALLOW_KEYPAD - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_MALLOW_MISC - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_NUMLOCK - ICONF_MIN] = {1, 1, 0, 1},
    [ICONF_REWRAP - ICONF_MIN] = {1, 1, 0, 1},
    [ICONF_CUT_LINES - ICONF_MIN] = {0, 0, 0, 1},
    [ICONF_MINIMIZE_SCROLLBACK - ICONF_MIN] = {1, 1, 0, 1},
    [ICONF_PRINT_ATTR - ICONF_MIN] = {1, 1, 0, 1},
    [ICONF_ALLOW_SUBST_FONTS] = {1, 1, 0, 1},
    [ICONF_FORCE_SCALABLE] = {0, 0, 0, 1},
    [ICONF_ALPHA] = {255, 255, 0, 255},
};

static struct {
    const char *dflt;
    char *val;
} soptions[] = {
    [SCONF_FONT_NAME - SCONF_MIN] = { "mono", NULL },
    [SCONF_ANSWERBACK_STRING - SCONF_MIN] = { "\006", NULL },
    [SCONF_SHELL - SCONF_MIN] = { "/bin/sh", NULL },
    [SCONF_TERM_NAME - SCONF_MIN] = { "xterm", NULL },
    [SCONF_TITLE - SCONF_MIN] = { "Not So Simple Terminal", NULL },
    [SCONF_PRINTER - SCONF_MIN] = { NULL, NULL },
    [SCONF_PRINT_CMD - SCONF_MIN] = { NULL, NULL },
    [SCONF_TERM_CLASS - SCONF_MIN] = { NULL, NULL },
    [SCONF_FORCE_MOUSE_MOD - SCONF_MIN] = { "T", NULL },
    [SCONF_TERM_MOD - SCONF_MIN] = {"SC", NULL },
    [SCONF_WORD_SEPARATORS - SCONF_MIN] = { " \t!#$%^&*()_+-={}[]\\\"'|/?,.<>~`", NULL },
    [KCONF_SCROLL_DOWN - SCONF_MIN] = { "T-Up", NULL },
    [KCONF_SCROLL_UP - SCONF_MIN] = { "T-Down", NULL },
    [KCONF_FONT_INC - SCONF_MIN] = { "T-Page_Up", NULL },
    [KCONF_FONT_DEC - SCONF_MIN] = { "T-Page_Down", NULL },
    [KCONF_FONT_RESET - SCONF_MIN] = { "T-Home", NULL },
    [KCONF_NEW_WINDOW - SCONF_MIN] = { "T-N", NULL },
    [KCONF_NUMLOCK - SCONF_MIN] = { "T-Num_Lock", NULL },
    [KCONF_COPY - SCONF_MIN] = { "T-C", NULL },
    [KCONF_PASTE - SCONF_MIN] = { "T-V", NULL },
    [KCONF_BREAK - SCONF_MIN] = { "Break", NULL },
    [KCONF_RESET - SCONF_MIN] = { "T-R", NULL },
    [KCONF_RELOAD_CONFIG - SCONF_MIN] = { "T-X", NULL },
    [KCONF_REVERSE_VIDEO - SCONF_MIN] = { "T-I", NULL },
};

static color_t coptions[PALETTE_SIZE];
static bool color_init;
static const char **argv = NULL;


/* Internal function, that calculates default palette
 *
 * base[CN_BASE] is default first 16 colors
 * next 6x6x6 colors are RGB cube
 * last 24 are gray scale
 *
 * default background and cursor background is color 0
 * default foreground and cursor foreground is color 15
 */
static color_t color(uint32_t opt) {
    static color_t base[CN_BASE] = {
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

    switch (opt) {
    case CCONF_BG:
    case CCONF_CURSOR_BG:
        return base[0];
    case CCONF_FG:
    case CCONF_CURSOR_FG:
    case CCONF_BOLD:
    case CCONF_UNDERLINE:
    case CCONF_BLINK:
    case CCONF_REVERSE:
    case CCONF_ITALIC:
        return base[15];
        /* Invert text by default */
    case CCONF_SELECTED_BG:
    case CCONF_SELECTED_FG:
        /* No default special colors */
        return 0;
    }

    opt -= CCONF_COLOR_0;

    if (opt < CN_BASE) return base[opt];
    else if (opt < CN_EXT + CN_BASE) {
        return 0xFF000000 | SD28B(((opt - CN_BASE) / 1) % 6) |
            (SD28B(((opt - CN_BASE) / 6) % 6) << 8) | (SD28B(((opt - CN_BASE) / 36) % 6) << 16);
    } else if (opt < CN_GRAY + CN_EXT + CN_BASE) {
        uint8_t val = MIN(0x08 + 0x0A * (opt - CN_BASE - CN_EXT), 0xFF);
        return 0xFF000000 + val * 0x10101;
    }

    return base[0];
}

int32_t iconf(uint32_t opt) {
    if (opt >= ICONF_MAX) {
        warn("Unknown integer config option %d", opt);
        return 0;
    }
    return ioptions[opt].val;
}

void iconf_set(uint32_t opt, int32_t val) {
    if (opt < ICONF_MAX) {
        if (opt == ICONF_MAPPING && val >= keymap_MAX)
            val = keymap_default;
        if (val > ioptions[opt].max) val = ioptions[opt].max;
        else if (val < ioptions[opt].min) val = ioptions[opt].min;
        ioptions[opt].val = val;
    } else {
        warn("Unknown integer option %d", opt);
    }
}

const char *sconf(uint32_t opt) {
    if (SCONF_MIN > opt || opt >= KCONF_MAX) {
        warn("Unknown string option %d", opt);
        return NULL;
    }
    opt -= SCONF_MIN;
    return soptions[opt].val ? soptions[opt].val : soptions[opt].dflt;
}

void sconf_set(uint32_t opt, const char *val) {
    if (SCONF_MIN <= opt && opt < KCONF_MAX) {
        opt -= SCONF_MIN;
        if (!strcasecmp(val, "default")) val = NULL;
        if (soptions[opt].val) free(soptions[opt].val);
        soptions[opt].val = val ? strdup(val) : NULL;
    } else if (opt < ICONF_MAX) {
        int32_t ival = ioptions[opt - ICONF_MIN].dflt;
        double alpha;
        // Accept floating point opacity values
        if (val && opt == ICONF_ALPHA && sscanf(val, "%lf", &alpha) == 1) {
            if (alpha > 1) alpha /= 255;
            iconf_set(opt, 255*alpha);
        } else if (!val || sscanf(val, "%"SCNd32, &ival) == 1) {
            iconf_set(opt, ival);
        } else {
            ival = -1;
            // Boolean options
            if (opt == ICONF_LOG_LEVEL) {
                if (!strcasecmp(val, "quiet")) ival = 0;
                else if (!strcasecmp(val, "fatal")) ival = 1;
                else if (!strcasecmp(val, "warn")) ival = 2;
                else if (!strcasecmp(val, "info")) ival = 3;
            } else if (opt == ICONF_CURSOR_SHAPE) {
                if (!strcasecmp(val, "blinking-block")) ival = 1;
                else if (!strcasecmp(val, "block")) ival = 2;
                else if (!strcasecmp(val, "blinking-underline")) ival = 3;
                else if (!strcasecmp(val, "underline")) ival = 4;
                else if (!strcasecmp(val, "blinking-bar")) ival = 5;
                else if (!strcasecmp(val, "bar")) ival = 6;
            } else if (opt == ICONF_PIXEL_MODE) {
                if (!strcasecmp(val, "mono")) ival = 0;
                else if (!strcasecmp(val, "bgr")) ival = 1;
                else if (!strcasecmp(val, "rgb")) ival = 2;
                else if (!strcasecmp(val, "bgrv")) ival = 3;
                else if (!strcasecmp(val, "rgbv")) ival = 4;
            } else if (opt == ICONF_KEYBOARD_NRCS) {
#define E(c) ((c) & 0x7F)
#define I0(i) ((i) ? (((i) & 0xF) + 1) << 9 : 0)
#define I1(i) (I0(i) << 5)
                if (!val[1] && val[0] > 0x2F && val[0] < 0x7F) {
                    uint32_t sel = E(val[0]);
                    ival = nrcs_parse(sel, 0, 5, 1);
                    if (ival < 0) ival = nrcs_parse(sel, 1, 5, 1);
                } else if (val[0] >= 0x20 && val[0] < 0x30 && val[1] > 0x2F && val[1] < 0x7F && !val[3]) {
                    uint32_t sel = E(val[1]) | I0(val[0]);
                    ival = nrcs_parse(sel, 0, 5, 1);
                    if (ival < 0) ival = nrcs_parse(sel, 1, 5, 1);
                }
            } else if (opt == ICONF_MARGIN_BELL_VOLUME || opt == ICONF_BELL_VOLUME) {
                if (!strcasecmp(val, "off")) ival = 0;
                else if (!strcasecmp(val, "low")) ival = 1;
                else if (!strcasecmp(val, "high")) ival = 2;
            } else if (opt == ICONF_MAPPING) {
                // "default" is parsed separately
                if (!strcasecmp(val, "legacy")) ival = keymap_legacy;
                else if (!strcasecmp(val, "vt220")) ival = keymap_vt220;
                else if (!strcasecmp(val, "hp")) ival = keymap_hp;
                else if (!strcasecmp(val, "sun")) ival = keymap_sun;
                else if (!strcasecmp(val, "sco")) ival = keymap_sco;
            } else if (opt == ICONF_MODIFY_OTHER_FMT) {
                if (!strcasecmp(val, "xterm")) ival = 0;
                else if (!strcasecmp(val, "csi-u")) ival = 1;
            } else if (ioptions[opt].min == 0 && ioptions[opt].max == 1) {
                if (!strcasecmp(val, "yes") || !strcasecmp(val, "y") || !strcasecmp(val, "true")) ival = 1;
                else if (!strcasecmp(val, "no") || !strcasecmp(val, "n") || !strcasecmp(val, "false")) ival = 0;
            } else if (!strcasecmp(val, "default")) {
                ival = ioptions[opt].dflt;
            }

            if (ival >= 0) iconf_set(opt, ival);
            else warn("Unknown string option %d", opt);
        }
    } else if (CCONF_MIN <= opt && opt < CCONF_MAX) {
            color_t col = 0;
            bool dflt = !val || !strcasecmp(val, "default");
            if (dflt) col = color(opt);
            else if (val) col = parse_color((uint8_t*)val, (uint8_t*)val + strlen(val));
            if (col || !val) {
                color_t old = cconf(opt);
                if (!dflt) {
                    // Keep alpha, unless its a special
                    // color value or we are resetting to default
                    if ((opt >= CCONF_SELECTED_BG) && !old) old = 0xFF000000;
                    col = (col & 0xFFFFFF) | (old & 0xFF000000);
                }
                cconf_set(opt, col);
            } else warn("Wrong color format: '%s'", val);
    } else {
        warn("Unknown string option %d", opt);
        return;
    }
}

bool bconf_set(uint32_t opt, bool val) {
    if (opt < ICONF_MAX && ioptions[opt].min == 0 && ioptions[opt].max == 1) {
        ioptions[opt].val = val;
        return 1;
    }
    return 0;
}

void cconf_set(uint32_t opt, color_t val) {
    if (!color_init) {
        for (size_t i = 0; i < PALETTE_SIZE; i++)
            coptions[i] = color(i + CCONF_COLOR_0);
        color_init = 1;
    }
    if (opt < CCONF_MIN || opt >= CCONF_MAX) {
        warn("Unknown color option");
        return;
    }
    coptions[opt - CCONF_COLOR_0] = val ? val : color(opt);
}

color_t cconf(uint32_t opt) {
    if (!color_init) {
        for (size_t i = 0; i < PALETTE_SIZE; i++)
            coptions[i] = color(i + CCONF_COLOR_0);
        color_init = 1;
    }
    if (CCONF_MIN > opt || opt >= CCONF_MAX) {
        warn("Unknown option");
        return 0;
    }
    color_t val = coptions[opt - CCONF_COLOR_0];
    return val ? val : color(opt);
}

const char **sconf_argv(void) {
    const char **res = argv;
    argv = NULL;
    return res;
}

void sconf_set_argv(const char **val) {
    argv = val;
}
