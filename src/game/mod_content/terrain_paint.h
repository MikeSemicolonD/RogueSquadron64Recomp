#pragma once
// Soft paint: a level's painted textures live in level/lv_<id>_PAINT (entries laid out like level/tex), and a _TEX bank field 0x2000 + i is its entry i.
// The game owns the top two bits (0x8000 + id borrows another id's texture, 0x4000 marks it loaded), so painted fields keep them clear.
#include <cstdint>
#include <vector>

namespace rs64::terrain_paint {

constexpr uint16_t kMark = 0x2000;
constexpr uint16_t kIndexMask = 0x1FFF;
constexpr uint32_t kEntryBytes = 2080;

inline bool is_painted(uint32_t field)
{
    return (field & 0xE000u) == kMark;
}

// out_bank = bank + the paint entries; out_tex = tex with every painted field turned into stock count + i, so a reader that only knows
// level/tex sees painted textures as ordinary bank entries. False, outputs untouched, when a painted field indexes past the paint file.
inline bool merge_bank(const uint8_t* bank, uint32_t bank_size, const uint8_t* tex, uint32_t tex_size, const uint8_t* paint, uint32_t paint_size,
                       std::vector<uint8_t>& out_bank, std::vector<uint8_t>& out_tex)
{

    const uint32_t stock = bank_size / kEntryBytes;
    const uint32_t painted = paint ? paint_size / kEntryBytes : 0;
    std::vector<uint8_t> t(tex, tex + tex_size);

    for (uint32_t o = 0; o + 2 <= tex_size; o += 4)
    {
        const uint16_t f = uint16_t((t[o] << 8) | t[o + 1]);

        if (!is_painted(f))
        {
            continue;
        }

        if ((f & kIndexMask) >= painted)
        {
            return false;
        }

        const uint32_t merged = stock + (f & kIndexMask);

        t[o] = uint8_t(merged >> 8);
        t[o + 1] = uint8_t(merged);
    }

    out_bank.assign(bank, bank + size_t(stock) * kEntryBytes);
    out_bank.insert(out_bank.end(), paint, paint + size_t(painted) * kEntryBytes);
    out_tex = std::move(t);

    return true;
}

}
