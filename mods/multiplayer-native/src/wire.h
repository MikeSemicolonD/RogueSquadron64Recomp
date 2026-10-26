#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rs64::net {

// Little-endian wire writer.
struct Writer {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { u8((uint8_t)v); u8((uint8_t)(v >> 8)); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) u8((uint8_t)(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) u8((uint8_t)(v >> (8 * i))); }
};

// Every read is bounds-checked; a short read clears ok and returns 0.
struct Reader {
    const uint8_t* p;
    size_t n;
    size_t at = 0;
    bool ok = true;
    bool need(size_t k) {
        if (!ok || n - at < k) {
            ok = false;
        }
        return ok;
    }
    uint8_t u8() { return need(1) ? p[at++] : 0; }
    uint16_t u16() {
        if (!need(2)) return 0;
        const uint16_t v = (uint16_t)(p[at] | (p[at + 1] << 8));
        at += 2;
        return v;
    }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = 0;
        for (int i = 3; i >= 0; --i) v = (v << 8) | p[at + i];
        at += 4;
        return v;
    }
    uint64_t u64() {
        if (!need(8)) return 0;
        uint64_t v = 0;
        for (int i = 7; i >= 0; --i) v = (v << 8) | p[at + i];
        at += 8;
        return v;
    }
    bool done() const { return ok && at == n; }
};

}
