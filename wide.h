/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#ifndef _WIDE_H
#define _WIDE_H 1

#include <stdint.h>

const uint8_t wide_table1_[1024] = {
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /*  0xxx */
     0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /*  1xxx */
     0, 0, 0, 3, 0, 4, 5, 6, 0, 0, 0, 7, 0, 0, 8, 9, /*  2xxx */
    10,11,12, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /*  3xxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,13, 1, 1, /*  4xxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /*  5xxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /*  6xxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /*  7xxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /*  8xxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /*  9xxx */
     1, 1, 1, 1,14, 0, 0, 0, 0,15, 0, 0, 1, 1, 1, 1, /*  Axxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /*  Bxxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /*  Cxxx */
     1, 1, 1, 1, 1, 1, 1,16, 0, 0, 0, 0, 0, 0, 0, 0, /*  Dxxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /*  Exxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0,17,18, /*  Fxxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 10xxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 11xxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 12xxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 13xxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 14xxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 15xxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,19, /* 16xxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 17xxx */
     1, 1, 1, 1, 1, 1, 1,20, 1, 1, 1, 1,21,22, 0, 0, /* 18xxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 19xxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,23, /* 1Axxx */
     1,24,25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 1Bxxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 1Cxxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 1Dxxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 1Exxx */
    26,27,28,29,30,31,32,33, 0,34,35, 0, 0, 0, 0, 0, /* 1Fxxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 20xxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 21xxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 22xxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 23xxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 24xxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 25xxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 26xxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 27xxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 28xxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 29xxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 2Axxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 2Bxxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 2Cxxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 2Dxxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 2Exxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,36, /* 2Fxxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 30xxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 31xxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 32xxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 33xxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 34xxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 35xxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 36xxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 37xxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 38xxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 39xxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 3Axxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 3Bxxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 3Cxxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 3Dxxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 3Exxx */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,36, /* 3Fxxx */
};

const uint8_t combining_table1_[496] = {
     0, 0, 0,30,38,39,40,41,42,43,44,45,46,40,48,49, /*  0xxx */
    50,51, 0,52, 0, 0, 0,53,54,55,56,50,58,59, 0, 0, /*  1xxx */
    60, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,61, 0, 0, 0, /*  2xxx */
    62, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /*  3xxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /*  4xxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /*  5xxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /*  6xxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /*  7xxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /*  8xxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /*  9xxx */
     0, 0, 0, 0, 0, 0,63, 0,64,65,66,60, 0, 0, 0, 0, /*  Axxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /*  Bxxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /*  Cxxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /*  Dxxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /*  Exxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,68, 0, 0,69,70, /*  Fxxx */
     0,71,72,73, 0, 0, 0, 0, 0, 0,74, 0, 0,75,76,70, /* 10xxx */
    78,79,80,81,82,83,84,85,86,80,88, 0,89,90,91, 0, /* 11xxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 12xxx */
     0, 0, 0, 0,92, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 13xxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 14xxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 15xxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0,93,94, 0, 0, 0,95, /* 16xxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 17xxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 18xxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 19xxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 1Axxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,96, 0, 0, 0, /* 1Bxxx */
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,90, /* 1Cxxx */
    98,99, 0, 0, 0, 0, 0, 0, 0, 0,100, 0, 0, 0, 0, 0, /* 1Dxxx */
    101,102,103, 0, 0, 0, 0, 0,104,105, 0, 0, 0, 0, 0, 0, /* 1Exxx */

};

const uint32_t width_data_[106][8] = {
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, },
    { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, },
    { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, },
    { 0x0C000000, 0x00000600, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00091E00, },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x60000000, },
    { 0x00300000, 0x00000000, 0x000FFF00, 0x80000000, 0x00080000, 0x60000C02, 0x00104030, 0x242C0400, },
    { 0x00000C20, 0x00000100, 0x00B85000, 0x00000000, 0x00E00000, 0x80010000, 0x00000000, 0x00000000, },
    { 0x18000000, 0x00000000, 0x00210000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xFBFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x000FFFFF, },
    { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x003FFFFF, 0x0FFF0000, },
    { 0xFFFFFFFF, 0x7FFFFFFF, 0xFFFFFFFE, 0xFFFFFFFF, 0xFE7FFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, },
    { 0xFFFFFFE0, 0xFFFEFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFF7FFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFF000F, },
    { 0x7FFFFFFF, 0xFFFFFFFF, 0xFFFF00FF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, },
    { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000, 0x00000000, },
    { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFF1FFF, 0xFFFFFFFF, 0x0000007F, 0x00000000, },
    { 0x00000000, 0x00000000, 0x00000000, 0x1FFFFFFF, 0x00000000, 0x00000000, 0x00000000, 0x00000000, },
    { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x0000000F, 0x00000000, 0x00000000, },
    { 0x03FF0000, 0xFFFF0000, 0xFFF7FFFF, 0x00000F7F, 0x00000000, 0x00000000, 0x00000000, 0x00000000, },
    { 0xFFFFFFFE, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x0000007F, },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0003001F, },
    { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x00FFFFFF, },
    { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x003FFFFF, 0x00000000, },
    { 0x000001FF, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x6FEF0000, },
    { 0xFFFFFFFF, 0x00000007, 0x00070000, 0xFFFF00F0, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, },
    { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x0FFFFFFF, },
    { 0x00000010, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00008000, 0x00000000, },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x07FE4000, 0x00000000, 0x00000000, 0x00000000, },
    { 0xFFFF0007, 0x0FFFFFFF, 0x000301FF, 0x0000003F, 0x00000000, 0x00000000, 0x00000000, 0x00000000, },
    { 0xFFFFFFFF, 0xFFBFE001, 0xFFFFFFFF, 0xDFFFFFFF, 0x000FFFFF, 0xFFFFFFFF, 0x000F87FF, 0xFF11FFFF, },
    { 0xFFFFFFFF, 0x7FFFFFFF, 0xFFFFFFFD, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x9FFFFFFF, },
    { 0xFFFFFFFF, 0x3FFFFFFF, 0xFFFF7800, 0x040000FF, 0x00600000, 0x00000010, 0x00000000, 0xF8000000, },
    { 0xFFFFFFFF, 0xFFFFFFFF, 0x0000FFFF, 0x00000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xE0E7103F, 0x1FF01800, },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00010FFF, },
    { 0xFFFFF000, 0xF7FFFFFF, 0xFFFFFFBF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, },
    { 0x00000000, 0x00000000, 0x00000000, 0x1F1F0000, 0xFFFF007F, 0x07FF1FFF, 0x03FF003F, 0x007F00FF, },
    { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x3FFFFFFF, },
    { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x0000FFFF, 0x00000000, 0x00000000, 0x00000000, 0x00000000, },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x000003F8, 0x00000000, 0x00000000, 0x00000000, },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xFFFE0000, 0xBFFFFFFF, 0x000000B6, 0x00000000, },
    { 0x17FF003F, 0x00000000, 0xFFFFF800, 0x00010000, 0x00000000, 0x00000000, 0xBFC00000, 0x00003D9F, },
    { 0x00028000, 0xFFFF0000, 0x000007FF, 0x00000000, 0x00000000, 0x0001FFC0, 0x00000000, 0x200FF800, },
    { 0xFBC00000, 0x00003EEF, 0x0E000000, 0x00000000, 0xFF030000, 0x00000000, 0xFFFFFC00, 0xFFFFFFFF, },
    { 0x00000007, 0x14000000, 0x00FE21FE, 0x0000000C, 0x00000002, 0x10000000, 0x0000201E, 0x4000000C, },
    { 0x00000006, 0x10000000, 0x00023986, 0x00230000, 0x00000006, 0x10000000, 0x000021BE, 0xFC00000C, },
    { 0x00000002, 0x90000000, 0x0060201E, 0x0000000C, 0x00000004, 0x00000000, 0x00002001, 0x00000000, },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, },
    { 0x00000003, 0x18000000, 0x0000201E, 0x0000000C, 0x00000002, 0x00000000, 0x005C0400, 0x00000000, },
    { 0x00000000, 0x07F20000, 0x00007F80, 0x00000000, 0x00000000, 0x1FF20000, 0x00003F00, 0x00000000, },
    { 0x03000000, 0x02A00000, 0x00000000, 0x7FFE0000, 0xFEFFE0DF, 0x1FFFFFFF, 0x00000040, 0x00000000, },
    { 0x00000000, 0x66FDE000, 0xC3000000, 0x001E0001, 0x20002064, 0x00000000, 0x00000000, 0x00000000, },
    { 0x00000000, 0x00000000, 0x00000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, },
    { 0x00000000, 0x00000000, 0xE0000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, },
    { 0x001C0000, 0x000C0000, 0x000C0000, 0x000C0000, 0x00000000, 0x3FB00000, 0x200FFE40, 0x00000000, },
    { 0x0000F800, 0x00000000, 0x00000000, 0x00000000, 0x00000060, 0x00000200, 0x00000000, 0x00000000, },
    { 0x00000000, 0x0E040187, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, },
    { 0x09800000, 0x00000000, 0x7F400000, 0x9FF81FE5, 0x00000000, 0xFFFF0000, 0x00007FFF, 0x00000000, },
    { 0x0000000F, 0x17D00000, 0x00000004, 0x000FF800, 0x00000003, 0x00003B3C, 0x00000000, 0x0003A340, },
    { 0x00000000, 0x00CFF000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xFFF70000, 0x031021FD, },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xFFFFFFFF, 0xFFFFFFFF, },
    { 0x0000F800, 0x00007C00, 0x00000000, 0x0000FFDF, 0x00000000, 0x00000000, 0xFFFF0000, 0x0001FFFF, },
    { 0x00000000, 0x00000000, 0x00000000, 0x80000000, 0x00000000, 0x00000000, 0x00000000, 0xFFFFFFFF, },
    { 0x00000000, 0x00003C00, 0x00000000, 0x00000000, 0x06000000, 0x00000000, 0x00000000, 0x00000000, },
    { 0x00000000, 0x00000000, 0x00000000, 0x3FF78000, 0xC0000000, 0x00000000, 0x00000000, 0x00030000, },
    { 0x00000844, 0x00001060, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000030, 0x8003FFFF, },
    { 0x00000000, 0x00003FC0, 0x0003FF80, 0x00000000, 0x00000007, 0x33C80000, 0x00000000, 0x00000020, },
    { 0x00000000, 0x00667E00, 0x00001008, 0x10000000, 0x00000000, 0xC19D0000, 0x00000002, 0x00403000, },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00002120, },
    { 0x40000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, },
    { 0x0000FFFF, 0x0000FFFF, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x80000000, },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0E000000, },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x20000000, },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000001, },
    { 0x00000000, 0x00000000, 0x00000000, 0x07C00000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, },
    { 0x0000F06E, 0x87000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000060, },
    { 0x00000000, 0x000000F0, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00001800, 0x00000000, 0x00000000, },
    { 0x00000000, 0x00000000, 0x0001FFC0, 0x00000000, 0x0000003C, 0x00000000, 0x00000000, 0x00000000, },
    { 0x00000002, 0xFF000000, 0x0000007F, 0x80190000, 0x00000003, 0x26780000, 0x00002004, 0x00000000, },
    { 0x00000007, 0x001FEF80, 0x00000000, 0x00080000, 0x00000003, 0x7FC00000, 0x00009E00, 0x00000000, },
    { 0x00000000, 0x40D38000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x80000000, 0x000007F8, },
    { 0x00000003, 0x18000000, 0x00000001, 0x001F1FC0, 0x00000000, 0x00000000, 0x00000000, 0x00000000, },
    { 0x00000000, 0xFF000000, 0x4000005C, 0x00000000, 0x00000000, 0x85F80000, 0x0000000D, 0x00000000, },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xB03C0000, 0x30000001, 0x00000000, },
    { 0x00000000, 0xA7F80000, 0x00000001, 0x00000000, 0x00000000, 0x00BF2800, 0x00000000, 0x00000000, },
    { 0xE0000000, 0x00000FBC, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, },
    { 0x00000000, 0x06FF8000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, },
    { 0x00000000, 0x58000000, 0x00000008, 0x00000000, 0x00000000, 0x00000000, 0x0CF00000, 0x00000001, },
    { 0x000007FE, 0x79F80000, 0x0E7E0080, 0x00000000, 0x037FFC00, 0x00000000, 0x00000000, 0x00000000, },
    { 0x00000000, 0xBF7F0000, 0x00000000, 0x00000000, 0xFFFC0000, 0x006DFCFF, 0x00000000, 0x00000000, },
    { 0x00000000, 0xB47E0000, 0x000000BF, 0x00000000, 0x00A30000, 0x00000000, 0x00000000, 0x00000000, },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00180000, },
    { 0x00000000, 0x01FF0000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x001F0000, },
    { 0x00000000, 0x007F0000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, },
    { 0x00000000, 0x00000000, 0x00008000, 0x00000000, 0x00078000, 0x00000000, 0x00000000, 0x00000010, },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x60000000, 0x0000000F, 0x00000000, 0x00000000, },
    { 0xFFFFFFFF, 0xFFFF3FFF, 0x0000007F, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, },
    { 0x00000000, 0x00000000, 0x00000000, 0xFFF80380, 0x00000FE7, 0x00003C00, 0x00000000, 0x00000000, },
    { 0x00000000, 0x00000000, 0x0000001C, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, },
    { 0xFFFFFFFF, 0xF87FFFFF, 0xFFFFFFFF, 0x00201FFF, 0xF8000010, 0x0000FFFE, 0x00000000, 0x00000000, },
    { 0xF9FFFF7F, 0x000007DB, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, },
    { 0x00000000, 0x007F0000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00004000, 0x00000000, 0x0000F000, },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x007F0000, 0x00000000, },
    { 0x00000000, 0x00000000, 0x000007F0, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, },
};

#endif