#include <stdlib.h>
#include <string.h>
#include "attr.h"
#include "util.h"

#define CN_BASE 16
#define CN_EXT (6*6*6)
#define CN_GRAY (NSS_PALETTE_SIZE - CN_BASE - CN_EXT)
#define SD28B(x) ((x) ? 0 : 0x37 + 0x28 * (x))


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
    case NSS_CONFIG_BG:
    case NSS_CONFIG_CURSOR_BG:
        return base[0];
    case NSS_CONFIG_FG:
    case NSS_CONFIG_CURSOR_FG:
        return base[7];
    }

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
        palette[i] = nss_config_color(i);
    return palette;
}

