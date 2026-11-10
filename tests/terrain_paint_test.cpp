#include "game/mod_content/terrain_paint.h"
#include "check.h"
#include <cstdio>
#include <vector>

using namespace rs64::terrain_paint;

// n entries of kEntryBytes, entry i filled with the byte `first + i`.
static std::vector<uint8_t> entries(uint32_t n, uint8_t first)
{

    std::vector<uint8_t> out(size_t(n) * kEntryBytes);

    for (uint32_t i = 0; i < n; ++i)
    {
        std::fill(out.begin() + size_t(i) * kEntryBytes, out.begin() + size_t(i + 1) * kEntryBytes, uint8_t(first + i));
    }

    return out;
}

// _TEX words: each field as u16 bank + u16 0, then the end marker.
static std::vector<uint8_t> tex(std::initializer_list<uint16_t> fields)
{

    std::vector<uint8_t> out;

    for (uint16_t f : fields)
    {
        out.insert(out.end(), { uint8_t(f >> 8), uint8_t(f), 0, 0 });
    }

    out.insert(out.end(), { 0xFF, 0xFF, 0, 0 });

    return out;
}

static uint16_t field(const std::vector<uint8_t>& t, size_t id)
{
    return uint16_t((t[id * 4] << 8) | t[id * 4 + 1]);
}

int main()
{

    // marked_fields_point_past_the_stock_bank
    {
        const std::vector<uint8_t> bank = entries(3, 10);
        const std::vector<uint8_t> paint = entries(2, 50);
        const std::vector<uint8_t> t = tex({ 1, 0x2000, 2, 0x2001, 0x8001 });
        std::vector<uint8_t> outBank, outTex;

        CHECK(merge_bank(bank.data(), uint32_t(bank.size()), t.data(), uint32_t(t.size()), paint.data(), uint32_t(paint.size()), outBank, outTex));
        CHECK(outBank.size() == 5 * kEntryBytes);
        CHECK(outBank[3 * kEntryBytes] == 50);
        CHECK(outBank[4 * kEntryBytes] == 51);
        CHECK(field(outTex, 0) == 1);
        CHECK(field(outTex, 1) == 3);
        CHECK(field(outTex, 2) == 2);
        CHECK(field(outTex, 3) == 4);
        CHECK(field(outTex, 4) == 0x8001);
        CHECK(field(outTex, 5) == 0xFFFF);
    }

    // a_field_past_the_paint_file_fails_and_leaves_the_outputs
    {
        const std::vector<uint8_t> bank = entries(3, 10);
        const std::vector<uint8_t> paint = entries(2, 50);
        const std::vector<uint8_t> t = tex({ 0x2002 });
        std::vector<uint8_t> outBank = { 7 }, outTex = { 8 };

        CHECK(!merge_bank(bank.data(), uint32_t(bank.size()), t.data(), uint32_t(t.size()), paint.data(), uint32_t(paint.size()), outBank, outTex));
        CHECK(outBank.size() == 1 && outBank[0] == 7);
        CHECK(outTex.size() == 1 && outTex[0] == 8);
    }

    // no_paint_file_keeps_tex
    {
        const std::vector<uint8_t> bank = entries(3, 10);
        const std::vector<uint8_t> t = tex({ 0, 2 });
        std::vector<uint8_t> outBank, outTex;

        CHECK(merge_bank(bank.data(), uint32_t(bank.size()), t.data(), uint32_t(t.size()), nullptr, 0, outBank, outTex));
        CHECK(outBank == bank);
        CHECK(outTex == t);
    }

    std::printf("terrain_paint_test: ok\n");

    return 0;
}
