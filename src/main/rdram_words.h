#pragma once
// RDRAM word access by KSEG0 address (N64 byte order).
#include <cstdint>
#include <cstring>

namespace rs64::mips {

inline uint32_t rw(const uint8_t* rdram, uint32_t a) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        v = (v << 8) | rdram[(a + i - 0x80000000u) ^ 3];
    }
    return v;
}

inline void ww(uint8_t* rdram, uint32_t a, uint32_t v) {
    for (uint32_t i = 0; i < 4; ++i) {
        rdram[(a + i - 0x80000000u) ^ 3] = (uint8_t)(v >> (24 - 8 * i));
    }
}

inline float rf(const uint8_t* rdram, uint32_t a) {
    const uint32_t b = rw(rdram, a);
    float f;
    memcpy(&f, &b, 4);
    return f;
}

inline uint16_t rh(const uint8_t* rdram, uint32_t a) {
    return (uint16_t)((rdram[(a - 0x80000000u) ^ 3] << 8) | rdram[(a + 1 - 0x80000000u) ^ 3]);
}

}
