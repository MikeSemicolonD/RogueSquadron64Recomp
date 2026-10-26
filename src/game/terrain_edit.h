#pragma once
// Runtime terrain edits on the game's HMP map, written into the game's own data so its height queries (collision) and the renderer agree.
// Descriptor 0x80136DC0: +0 per-cell u16 tile index (low 13 bits), +4 tile table (0x1E bytes a tile: +0 u16 texture id, +2 flags, +5 25 s8 heights), +0x38/+0x3A width/height.
#include <cstdint>

#include "util/rdram_words.h"

namespace rs64::terrain {

constexpr uint32_t kDescriptor = 0x80136DC0u;
constexpr uint32_t kTileBytes = 0x1Eu;
constexpr uint32_t kMaxTiles = 0x2000u;

// The tile table moves here on a map's first edit: cells share tiles, so an edited cell gets its own copy, and the level's table has no room after it.
constexpr uint32_t kTable = 0x80A10000u;
constexpr uint32_t kTableEnd = kTable + kMaxTiles * kTileBytes;

enum Result { Ok = 0, NoMap = -1, OutOfRange = -2, Full = -3 };

struct Map {
    uint32_t idx = 0, tab = 0, w = 0, h = 0;
};

inline uint8_t rb(const uint8_t* rdram, uint32_t a) {
    return rdram[(a - 0x80000000u) ^ 3];
}

inline void wb(uint8_t* rdram, uint32_t a, uint8_t v) {
    rdram[(a - 0x80000000u) ^ 3] = v;
}

inline void wh(uint8_t* rdram, uint32_t a, uint16_t v) {
    wb(rdram, a, (uint8_t)(v >> 8));
    wb(rdram, a + 1, (uint8_t)v);
}

// A KSEG0 range inside the game's 8 MB or the moved table.
inline bool in_ram(uint32_t a, uint32_t size) {
    const bool game = (a >= 0x80000000u) && (a + size <= 0x80800000u);
    const bool moved = (a >= kTable) && (a + size <= kTableEnd);

    return game || moved;
}

inline bool read_map(const uint8_t* rdram, Map& m) {

    m.idx = mips::rw(rdram, kDescriptor);
    m.tab = mips::rw(rdram, kDescriptor + 4);
    m.w = mips::rh(rdram, kDescriptor + 0x38);
    m.h = mips::rh(rdram, kDescriptor + 0x3A);

    return (m.w > 0) && (m.h > 0) && (m.w <= 256) && (m.h <= 256) && in_ram(m.idx, m.w * m.h * 2) && in_ram(m.tab, kTileBytes);
}

inline uint32_t tile_of(const uint8_t* rdram, const Map& m, uint32_t cell) {
    return mips::rh(rdram, m.idx + cell * 2) & (kMaxTiles - 1);
}

// One past the highest tile any cell uses.
inline uint32_t tile_count(const uint8_t* rdram, const Map& m) {

    uint32_t n = 0;

    for (uint32_t c = 0; c < m.w * m.h; ++c) {
        const uint32_t t = tile_of(rdram, m, c) + 1;
        n = (t > n) ? t : n;
    }

    return n;
}

inline int get_cell(const uint8_t* rdram, uint32_t col, uint32_t row, int8_t heights[25], uint16_t* tex) {

    Map m;

    if (!read_map(rdram, m))
    {
        return NoMap;
    }

    if ((col >= m.w) || (row >= m.h))
    {
        return OutOfRange;
    }

    const uint32_t a = m.tab + tile_of(rdram, m, row * m.w + col) * kTileBytes;

    if (!in_ram(a, kTileBytes))
    {
        return NoMap;
    }

    for (uint32_t k = 0; k < 25; ++k) {
        heights[k] = (int8_t)rb(rdram, a + 5 + k);
    }

    if (tex) {
        *tex = mips::rh(rdram, a);
    }

    return Ok;
}

// The legacy renderer's cell cache (nodes of 0xB8 bytes at [kCellPool], count at [kCellCount], 0-4 list heads) keeps a pointer to its tile's heights at +0x44.
constexpr uint32_t kCellPool = 0x80137A5Cu;
constexpr uint32_t kCellCount = 0x80137610u;
constexpr uint32_t kCellBytes = 0xB8u;

inline void repoint_cached_cell(uint8_t* rdram, uint32_t col, uint32_t row, uint32_t tile)
{

    const uint32_t pool = mips::rw(rdram, kCellPool);
    const uint32_t count = mips::rw(rdram, kCellCount);

    if ((count > 0x1000) || !in_ram(pool, count * kCellBytes))
    {
        return;
    }

    for (uint32_t i = 5; i < count; ++i) {
        const uint32_t n = pool + i * kCellBytes;

        if ((mips::rh(rdram, n + 0x18) == col) && (mips::rh(rdram, n + 0x1A) == row) && (mips::rw(rdram, n + 0x44) != 0)) {
            mips::ww(rdram, n + 0x44, tile + 5);
        }
    }
}

// tex < 0 keeps the cell's texture. The map's first edit moves the table to kTable; a tile other cells share is copied for this one first.
inline int set_cell(uint8_t* rdram, uint32_t col, uint32_t row, const int8_t heights[25], int tex) {

    Map m;

    if (!read_map(rdram, m))
    {
        return NoMap;
    }

    if ((col >= m.w) || (row >= m.h))
    {
        return OutOfRange;
    }

    uint32_t count = tile_count(rdram, m);

    if (m.tab != kTable) {
        if (!in_ram(m.tab, count * kTileBytes) || (count > kMaxTiles))
        {
            return NoMap;
        }

        for (uint32_t i = 0; i < count * kTileBytes; ++i) {
            wb(rdram, kTable + i, rb(rdram, m.tab + i));
        }

        mips::ww(rdram, kDescriptor + 4, kTable);
        m.tab = kTable;
    }

    const uint32_t cell = row * m.w + col;
    uint32_t tile = tile_of(rdram, m, cell);
    bool shared = false;

    for (uint32_t c = 0; (c < m.w * m.h) && !shared; ++c) {
        shared = (c != cell) && (tile_of(rdram, m, c) == tile);
    }

    if (shared) {
        if (count >= kMaxTiles)
        {
            return Full;
        }

        for (uint32_t i = 0; i < kTileBytes; ++i) {
            wb(rdram, kTable + count * kTileBytes + i, rb(rdram, kTable + tile * kTileBytes + i));
        }

        const uint16_t word = mips::rh(rdram, m.idx + cell * 2);

        wh(rdram, m.idx + cell * 2, (uint16_t)((word & ~(kMaxTiles - 1)) | count));
        tile = count;
    }

    const uint32_t a = kTable + tile * kTileBytes;

    for (uint32_t k = 0; k < 25; ++k) {
        wb(rdram, a + 5 + k, (uint8_t)heights[k]);
    }

    if (tex >= 0) {
        wh(rdram, a, (uint16_t)tex);
    }

    repoint_cached_cell(rdram, col, row, a);

    return Ok;
}

}
