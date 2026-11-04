#pragma once
// RDRAM word access by KSEG0 address (N64 byte order).
#include <cstdint>
#include <cstring>
#include <string>

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

inline uint8_t rb(const uint8_t* rdram, uint32_t a) {
    return rdram[(a - 0x80000000u) ^ 3];
}

inline void wb(uint8_t* rdram, uint32_t a, uint8_t v) {
    rdram[(a - 0x80000000u) ^ 3] = v;
}

inline void wh(uint8_t* rdram, uint32_t a, uint16_t v)
{

    wb(rdram, a, (uint8_t)(v >> 8));
    wb(rdram, a + 1, (uint8_t)v);
}

inline uint32_t float_bits(float f)
{

    uint32_t u;

    memcpy(&u, &f, 4);

    return u;
}

inline void wf(uint8_t* rdram, uint32_t a, float f) {
    ww(rdram, a, float_bits(f));
}

// A NUL-terminated string at a, at most max bytes.
inline std::string rstr(const uint8_t* rdram, uint32_t a, size_t max)
{

    std::string s;

    for (size_t i = 0; i < max; i++) {
        const char c = (char)rb(rdram, a + (uint32_t)i);

        if (!c) break;

        s += c;
    }

    return s;
}

// s and its NUL terminator at a.
inline void wstr(uint8_t* rdram, uint32_t a, const std::string& s)
{
    for (size_t i = 0; i <= s.size(); i++) wb(rdram, a + (uint32_t)i, (uint8_t)(i < s.size() ? s[i] : 0));
}

}
