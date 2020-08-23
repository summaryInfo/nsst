/* Copyright (c) 2019-2020, Evgeny Baskov. All rights reserved */

#include "feature.h"

#include "config.h"
#include "boxdraw.h"
#include "font.h"
#include "term.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

static void draw_rect(struct glyph * glyph, _Bool lcd, int16_t xs, int16_t ys, int16_t xe, int16_t ye, uint8_t val) {
    if (xs < xe && ys < ye) {
        xs = MAX(0, xs);
        xe = MIN(xe, glyph->width);
        ys = MAX(0, ys);
        ye = MIN(ye, glyph->height);

        if (lcd) xs *= 4, xe *= 4;
        for (int16_t j = ys; j < ye; j++)
            for (int16_t i = xs; i < xe; i++)
                glyph->data[j * glyph->stride + i] = val;
    }
}

static void put(struct glyph *glyph, _Bool lcd, int16_t x, int16_t y, uint8_t val) {
    if (!lcd) glyph->data[glyph->stride * y + x] = val;
    else for (size_t i = 0; i < 4; i++)
        glyph->data[glyph->stride*y + 4*x + i] = val;
}

struct glyph *make_boxdraw(uint32_t c, int16_t width, int16_t height, int16_t depth) {
    if (!is_boxdraw(c)) return NULL;

    enum pixel_mode pixmode = iconf(ICONF_PIXEL_MODE);
    _Bool lcd = pixmode != pixmode_mono;
    size_t stride = lcd ? 4*width : (width + 3) & ~3;
    struct glyph *glyph = calloc(1, sizeof(struct glyph) + stride * (height + depth) * sizeof(uint8_t));
    if (!glyph) return NULL;

    glyph->y_off = 0;
    glyph->x = iconf(ICONF_FONT_SPACING)/2;
    glyph->y = height + iconf(ICONF_LINE_SPACING)/2;
    glyph->height = height + depth;
    glyph->x_off = glyph->width = width;
    glyph->stride = stride;
    glyph->pixmode = pixmode;

    enum {
        NOC = 1 << 1, // No center
        CUR = 1 << 2, // Curved
        TL = 1 << 4, BL = 1 << 5, LL = 1 << 6, RL = 1 << 7, // Light lines
        TD = 1 << 8, BD = 1 << 9, LD = 1 << 10, RD = 1 << 11, // Double lines
        // These are considered blocks
        BLK = 1 << 3, // Block
        TLQ = BLK | 1 << 4, TRQ = BLK | 1 << 5, BLQ = BLK | 1 << 6, BRQ = BLK | 1 << 7, // Quoters
        V = BLK | 1 << 8, VR = BLK | 1 << 9, // Vertical blocks
        H = BLK | 1 << 10, HR = BLK | 1 << 11, // Horizontal blocks
        LX = BLK | 1 << 12, RX = BLK | 1 << 13, // Diagonal lines
        DT1 = 1 << 14, DT2 = 1 << 15, // Dotted (both for blocks and lines)
    };
    uint16_t desc = (uint16_t[]){
        LL|RL,               LL|LD|RL|RD,         TL|BL,               TL|TD|BL|BD,
        LL|RL|DT1,           LL|LD|RL|RD|DT1,     TL|BL|DT1,           TL|TD|BL|BD|DT1,
        LL|RL|DT2,           LL|LD|RL|RD|DT2,     TL|BL|DT2,           TL|TD|BL|BD|DT2,
        BL|RL,               BL|RL|RD,            BL|BD|RL,            BL|BD|RL|RD,
        BL|LL,               BL|LL|LD,            BL|BD|LL,            BL|BD|LL|LD,
        TL|RL,               TL|RL|RD,            TL|TD|RL,            TL|TD|RL|RD,
        TL|LL,               TL|LL|LD,            TL|TD|LL,            TL|TD|LL|LD,
        TL|BL|RL,            TL|BL|RL|RD,         TL|TD|BL|RL,         TL|BL|BD|RL,
        TL|TD|BL|BD|RL,      TL|TD|BL|RL|RD,      TL|BL|BD|RL|RD,      TL|TD|BL|BD|RL|RD,
        TL|BL|LL,            TL|BL|LL|LD,         TL|TD|BL|LL,         TL|BL|BD|LL,
        TL|TD|BL|BD|LL,      TL|TD|BL|LL|LD,      TL|BL|BD|LL|LD,      TL|TD|BL|BD|LL|LD,
        LL|BL|RL,            LL|LD|BL|RL,         LL|BL|RL|RD,         LL|LD|BL|RL|RD,
        LL|BL|BD|RL,         LL|LD|BL|BD|RL,      LL|BL|BD|RL|RD,      LL|LD|BL|BD|RL|RD,
        LL|TL|RL,            LL|LD|TL|RL,         LL|TL|RL|RD,         LL|LD|TL|RL|RD,
        LL|TL|TD|RL,         LL|LD|TL|TD|RL,      LL|TL|TD|RL|RD,      LL|LD|TL|TD|RL|RD,
        LL|RL|TL|BL,         LL|LD|RL|TL|BL,      LL|RL|RD|TL|BL,      LL|LD|RL|RD|TL|BL,
        LL|RL|TL|TD|BL,      LL|RL|TL|BL|BD,      LL|RL|TL|TD|BL|BD,   LL|LD|RL|TL|TD|BL,
        LL|RL|RD|TL|TD|BL,   LL|LD|RL|TL|BL|BD,   LL|RL|RD|TL|BL|BD,   LL|LD|RL|RD|TL|TD|BL,
        LL|LD|RL|RD|TL|BL|BD,LL|LD|RL|TL|TD|BL|BD,LL|RL|RD|TL|TD|BL|BD,LL|LD|RL|RD|TL|TD|BL|BD,
        LL|RL|NOC,           LL|LD|RL|RD|NOC,     TL|BL|NOC,           TL|TD|BL|BD|NOC,
        LD|RD,               TD|BD,               BL|RD,               BD|RL,
        BD|RD,               BL|LD,               BD|LL,               BD|LD,
        TL|RD,               TD|RL,               TD|RD,               TL|LD,
        TD|LL,               TD|LD,               TL|BL|RD,            TD|BD|RL,
        TD|BD|RD,            TL|BL|LD,            TD|BD|LL,            TD|BD|LD,
        LD|RD|BL,            LL|RL|BD,            LD|RD|BD,            LD|RD|TL,
        LL|RL|TD,            LD|RD|TD,            TL|BL|RD|LD,         TD|BD|RL|LL,
        TD|BD|RD|LD,         RL|BL|CUR|NOC,       LL|BL|CUR|NOC,       TL|LL|CUR|NOC,
        TL|RL|CUR|NOC,       RX,                  LX,                  RX|LX,
        LL,                  TL,                  RL,                  BL,
        LD,                  RL,                  TD,                  BD,
        LL|RD,               TL|BD,               LD|RL,               TD|BL,
        TLQ|TRQ,             H,                   H|1,                 H|2,
        H|3,                 H|4,                 H|5,                 H|6,
        H|7,                 V|6,                 V|5,                 V|4,
        V|3,                 V|2,                 V|1,                 V,
        TRQ|BRQ,             BLK|DT1,             BLK|DT2,             DT1|H|7,
        HR,                  VR,                  BLQ,                 BRQ,
        TLQ,                 TLQ|BRQ|BLQ,         TLQ|BRQ,             TLQ|TRQ|BLQ,
        TLQ|TRQ|BRQ,         TRQ,                 TRQ|BLQ,             TRQ|BLQ|BRQ,
    }[c - 0x2500];

    int16_t h = height + depth, w = width;
    int16_t ch = h/2, cw = w/2;
    int16_t lw = MAX(w/8, 1), lw2 = MAX(lw/2, 1);
    int16_t mod, x0 = cw-lw+lw2, y0 = ch-lw+lw2;
    _Bool dt1 = desc & DT1, dt2 = desc & DT2, noc = desc & NOC, cur = desc & CUR;
    _Bool td = desc & TD, bd = desc & BD, ld = desc & LD, rd = desc & RD;
    _Bool tl = desc & TL, bl = desc & BL, ll = desc & LL, rl = desc & RL;

    if (desc & BLK) {
        desc &= ~BLK;
        if (desc & TLQ) draw_rect(glyph, lcd, 0, 0, cw, ch, 0xFF);
        if (desc & TRQ) draw_rect(glyph, lcd, cw, 0, w, ch, 0xFF);
        if (desc & BLQ) draw_rect(glyph, lcd, 0, ch, cw, h, 0xFF);
        if (desc & BRQ) draw_rect(glyph, lcd, cw, ch, w, h, 0xFF);
        if (desc & H) draw_rect(glyph, lcd, 0, h*(7 - (desc & 7))/8, w, h, 0xFF);
        if (desc & V) draw_rect(glyph, lcd, 0, 0, w*(desc & 7)/8, h, 0xFF);
        if (desc & HR) draw_rect(glyph, lcd, 0, 0, w, h*(desc & 7)/8, 0xFF);
        if (desc & VR) draw_rect(glyph, lcd, w*(7 - (desc & 7))/8, 0, w, h, 0xFF);
        if (desc & LX)
            for (int16_t i = 0; i < h; i++)
                put(glyph, lcd, (w*i/h), i, 0xFF);
        if (desc & RX)
            for (int16_t i = 0; i < h; i++)
                put(glyph, lcd, w - 1 - (w*i/h), i, 0xFF);
        if (dt1 | dt2)
            for (int16_t i = 0; i < h; i += (1 + dt1))
                for (int16_t j = i & (1 + dt1); j < w;  j+= 2*(1 + dt1))
                    put(glyph, lcd, j, i, 0xFF * !(desc & H));
    } else {

        if (cur) draw_rect(glyph, lcd, x0+lw*(2*rl-1), y0+lw*(2*bl-1), x0+lw*2*rl, y0+lw*2*bl, 0xFF);

        mod = !noc * !(td && bd && !tl) - noc;
        if (ll) draw_rect(glyph, lcd, 0, y0, x0+lw*mod, y0+lw, 0xFF);
        if (rl) draw_rect(glyph, lcd, x0+lw*(1-mod), y0, w, y0+lw, 0xFF);

        mod = !noc * !(ld && rd && !ll) - noc;
        if (tl) draw_rect(glyph, lcd, x0, 0, x0+lw, y0+lw*mod, 0xFF);
        if (bl) draw_rect(glyph, lcd, x0, y0+lw*(1-mod), x0+lw, h, 0xFF);

        mod = !noc * (!(td|tl) * MAX(bl,2*bd) + !(tl|td|bl|bd)) - noc;
        if (ld) draw_rect(glyph, lcd, 0, y0-lw, x0+lw*mod, y0, 0xFF);
        if (rd) draw_rect(glyph, lcd, x0+lw*(1-mod), y0-lw, w, y0, 0xFF);

        mod = !noc * (!(bd|bl) * MAX(tl,2*td) + !(tl|td|bl|bd)) - noc;
        if (ld) draw_rect(glyph, lcd, 0, y0+lw, x0+lw*mod, y0+2*lw, 0xFF);
        if (rd) draw_rect(glyph, lcd, x0+lw*(1-mod), y0+lw, w, y0+2*lw, 0xFF);

        mod = !noc * (!(ld|ll) * MAX(rl,2*rd) + !(ll|ld|rl|rd)) - noc;
        if (td) draw_rect(glyph, lcd, x0-lw, 0, x0, y0+lw*mod, 0xFF);
        if (bd) draw_rect(glyph, lcd, x0-lw, y0+lw*(1-mod), x0, h, 0xFF);

        mod = !noc * (!(rd|rl) * MAX(ll,2*ld) + !(ll|ld|rl|rd)) - noc;
        if (td) draw_rect(glyph, lcd, x0+lw, 0, x0+2*lw, y0+lw*mod, 0xFF);
        if (bd) draw_rect(glyph, lcd, x0+lw, y0+lw*(1-mod), x0+2*lw, h, 0xFF);

        if (dt1 | dt2) {
            for (int16_t step = MAX(1, ((tl?h:w)+1+dt2)/(3 + dt2)), i = 1; i < 3 + dt2; i++)
                if (tl)
                    draw_rect(glyph, lcd, x0 - lw, i*step - (lw+1)/2, x0 + 2*lw, i*step + (lw+1)/2, 0);
                else
                    draw_rect(glyph, lcd, i*step - (lw+1)/2, y0 - lw, i*step + (lw+1)/2, y0 + 2*lw, 0);
        }
    }
    return glyph;
}
