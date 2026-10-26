#include "game/terrain_edit.h"
#include "check.h"
#include <cstdio>
#include <vector>

using namespace rs64::terrain;

static constexpr uint32_t kIdx = 0x80100000u;
static constexpr uint32_t kTab = 0x80200000u;

// A 3x2 map over 3 tiles: cells use tiles 0 1 1 / 2 0 1 (cell 4 also carries the flag bit 0x2000); tile t's heights are t*10 + k, texture 100 + t.
static std::vector<uint8_t> make_ram()
{

    std::vector<uint8_t> ram(kTableEnd - 0x80000000u, 0);
    uint8_t* r = ram.data();
    const uint16_t cells[6] = { 0, 1, 1, 2, 0x2000 | 0, 1 };

    rs64::mips::ww(r, kDescriptor, kIdx);
    rs64::mips::ww(r, kDescriptor + 4, kTab);
    wh(r, kDescriptor + 0x38, 3);
    wh(r, kDescriptor + 0x3A, 2);

    for (uint32_t c = 0; c < 6; ++c) wh(r, kIdx + c * 2, cells[c]);

    for (uint32_t t = 0; t < 3; ++t) {
        wh(r, kTab + t * kTileBytes, (uint16_t)(100 + t));

        for (uint32_t k = 0; k < 25; ++k) wb(r, kTab + t * kTileBytes + 5 + k, (uint8_t)(t * 10 + k));
    }

    return ram;
}

int main() {

    // get_cell_reads_the_cells_tile
    {
        std::vector<uint8_t> ram = make_ram();
        int8_t h[25];
        uint16_t tex = 0;

        CHECK(get_cell(ram.data(), 2, 0, h, &tex) == Ok);
        CHECK(h[0] == 10 && h[24] == 34 && tex == 101);
        CHECK(get_cell(ram.data(), 3, 0, h, &tex) == OutOfRange);
    }

    // shared_tile_is_copied_and_the_table_moved
    {
        std::vector<uint8_t> ram = make_ram();
        uint8_t* r = ram.data();
        int8_t edit[25];
        int8_t h[25];
        uint16_t tex = 0;

        for (int k = 0; k < 25; ++k) edit[k] = (int8_t)(-k);

        CHECK(set_cell(r, 2, 0, edit, -1) == Ok);
        CHECK(rs64::mips::rw(r, kDescriptor + 4) == kTable);
        CHECK((rs64::mips::rh(r, kIdx + 2 * 2) & 0x1FFF) == 3);
        CHECK(get_cell(r, 2, 0, h, &tex) == Ok);
        CHECK(h[0] == 0 && h[24] == -24 && tex == 101);
        CHECK(get_cell(r, 1, 0, h, &tex) == Ok);
        CHECK(h[0] == 10 && h[24] == 34);
        CHECK(get_cell(r, 2, 1, h, nullptr) == Ok && h[5] == 15);
    }

    // a_cells_own_tile_is_edited_in_place_and_flags_kept
    {
        std::vector<uint8_t> ram = make_ram();
        uint8_t* r = ram.data();
        int8_t edit[25] = {};
        uint16_t tex = 0;

        CHECK(set_cell(r, 0, 1, edit, 7) == Ok);
        CHECK((rs64::mips::rh(r, kIdx + 3 * 2) & 0x1FFF) == 2);
        CHECK(get_cell(r, 0, 1, edit, &tex) == Ok && tex == 7);

        CHECK(set_cell(r, 1, 1, edit, -1) == Ok);
        CHECK(rs64::mips::rh(r, kIdx + 4 * 2) == (0x2000 | 3));
    }

    // a_cached_cell_reads_its_new_tile
    {
        std::vector<uint8_t> ram = make_ram();
        uint8_t* r = ram.data();
        constexpr uint32_t kPool = 0x80300000u;
        const uint32_t built = kPool + 5 * kCellBytes, unbuilt = kPool + 6 * kCellBytes, other = kPool + 7 * kCellBytes;
        int8_t edit[25] = {};

        rs64::mips::ww(r, kCellPool, kPool);
        rs64::mips::ww(r, kCellCount, 8);
        wh(r, built + 0x18, 2);
        rs64::mips::ww(r, built + 0x44, kTab + 1 * kTileBytes + 5);
        wh(r, unbuilt + 0x18, 2);
        wh(r, other + 0x18, 1);
        rs64::mips::ww(r, other + 0x44, kTab + 1 * kTileBytes + 5);

        CHECK(set_cell(r, 2, 0, edit, -1) == Ok);
        CHECK(rs64::mips::rw(r, built + 0x44) == kTable + 3 * kTileBytes + 5);
        CHECK(rs64::mips::rw(r, unbuilt + 0x44) == 0);
        CHECK(rs64::mips::rw(r, other + 0x44) == kTab + 1 * kTileBytes + 5);
    }

    // no_map_without_a_descriptor
    {
        std::vector<uint8_t> ram(kTableEnd - 0x80000000u, 0);
        int8_t h[25] = {};

        CHECK(get_cell(ram.data(), 0, 0, h, nullptr) == NoMap);
        CHECK(set_cell(ram.data(), 0, 0, h, -1) == NoMap);
    }

    std::printf("terrain_edit_test: ok\n");

    return 0;
}
