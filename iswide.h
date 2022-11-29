#ifndef _ISWIDE_H
#define _ISWIDE_H 1

#include <stdint.h>

extern const uint8_t wide_table1_[804];
extern const uint8_t combining_table1_[1026];
extern const uint32_t width_data_[119][8];

inline static bool iswide_compact(uint32_t x) {
    return x - 0x1100U < 0x100*sizeof(wide_table1_)/sizeof(wide_table1_[0]) - 0x1100U &&
           width_data_[wide_table1_[x >> 8]][(x >> 5) & 7] & (1U << (x & 0x1F));
}

inline static bool iswide(uint32_t x) {
    return iswide_compact(compact(x));
}

inline static bool iscombining_compact(uint32_t x) {
    return x - 0xADU < 0x100*sizeof(combining_table1_)/sizeof(combining_table1_[0]) - 0xADU &&
           width_data_[combining_table1_[x >> 8]][(x >> 5) & 7] & (1U << (x & 0x1F));
}

inline static bool iscombining(uint32_t x) {
    return iscombining_compact(compact(x));
}
#endif
