#include <stdlib.h>
#include <string.h>
#include "attr.h"
#include "util.h"
#include "window.h"

#define CN_BASE 16
#define CN_EXT (6*6*6)
#define CN_GRAY (NSS_PALETTE_SIZE - CN_BASE - CN_EXT)
#define SD28B(x) ((x) ? 0 : 0x37 + 0x28 * (x))

int32_t nss_config_integer(uint32_t opt, int32_t min, int32_t max) {
    int32_t val;
    switch (opt) {
    case nss_config_window_x: val = 200; break;
    case nss_config_window_y: val = 200; break;
    case nss_config_window_width: val = 800; break;
    case nss_config_window_height: val = 600; break;
    case nss_config_history_lines: val = 1024; break;
    case nss_config_utf8: val = 1; break;
    case nss_config_allow_nrcs: val = 1; break;
    case nss_config_tab_width: val = 8; break;
    case nss_config_init_wrap: val = 1; break;
    case nss_config_scroll_on_input: val = 1; break;
    case nss_config_scroll_on_output: val = 0; break;
    case nss_config_appkey: val = 0; break;
    case nss_config_appcursor: val = 0; break;
    case nss_config_numlock: val = 1; break;
    case nss_config_has_meta: val = 1; break;
    case nss_config_meta_escape: val = 1; break;
    case nss_config_backspace_is_delete: val = 1; break;
    case nss_config_delete_is_delete: val = 0; break;
    case nss_config_cursor_shape: val = nss_cursor_bar; break;
    case nss_config_underline_width: val = 1; break;
    case nss_config_cursor_width: val = 2; break;
    case nss_config_subpixel_fonts: val = 0; break;
    case nss_config_reverse_video: val = 0; break;
    case nss_config_allow_altscreen: val = 1; break;
    case nss_config_left_border: val = 8; break;
    case nss_config_top_border: val = 8; break;
    case nss_config_blink_time: val = 800000; break;
    case nss_config_font_size: val = 13; break;
    default:
        warn("Unknown config option");
        val = min;
        break;
    }
    return MIN(max, MAX(val, min));
}

const char *nss_config_string(uint32_t opt, const char *alt) {
    switch(opt) {
        case nss_config_font_name:
            return "Iosevka-13,MaterialDesignIcons-13";
        case nss_config_answerback_string:
            return "";
        case nss_config_shell:
            return "/bin/sh";
        case nss_config_term_name:
            return "xterm-new";
    }
    return alt;

}

nss_color_t nss_config_color(uint32_t opt) {
    static nss_color_t base[CN_BASE] = {
            0xFF222222, 0xFFFF4433, 0xFFBBBB22, 0xFFFFBB22,
            0xFF88AA99, 0xFFDD8899, 0xFF88CC77, 0xFFDDCCAA,
            0xFF665555, 0xFFFF4433, 0xFFBBBB22, 0xFFFFBB22,
            0xFF88AA99, 0xFFDD8899, 0xFF88CC77, 0xFFFFFFCC,
    //        0xff000000, 0xff0000cd, 0xff00cd00, 0xff00cdcd,
    //        0xffee0000, 0xffcd00cd, 0xffcdcd00, 0xffe5e5e5,
    //        0xff7f7f7f, 0xff0000ff, 0xff00ff00, 0xff00ffff,
    //        0xffff5c5c, 0xffff00ff, 0xffffff00, 0xffffffff,
    };

    switch(opt) {
    case nss_config_bg:
    case nss_config_cursor_bg:
        return base[0];
    case nss_config_fg:
    case nss_config_cursor_fg:
        return base[15];
    }

    opt -= nss_config_color_0;

    if (opt < CN_BASE) return base[opt];
    else if (opt < CN_EXT + CN_BASE) {
        return 0xFF000000 + SD28B(((opt - CN_BASE) / 36) % 6) +
                (SD28B(((opt - CN_BASE) / 36) % 6) << 8) + (SD28B(((opt - CN_BASE) / 36) % 6) << 16);
    } else if (opt < CN_GRAY + CN_EXT + CN_BASE) {
        uint8_t val = MIN(0x08 + 0x0A * (opt - CN_BASE - CN_EXT), 0xFF);
        return 0xFF000000 + val * 0x10101;
    }

    return base[0];
}

nss_color_t *nss_create_palette(void) {
    nss_color_t *palette = malloc(NSS_PALETTE_SIZE * sizeof(nss_color_t));
    for (size_t i = 0; i < NSS_PALETTE_SIZE; i++)
        palette[i] = nss_config_color(i + nss_config_color_0);
    return palette;
}

