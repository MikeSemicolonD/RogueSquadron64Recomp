// Menu font helpers: glyph lookups, and the glyphs the italic menu font lacks, made from its own.
#include "game/menus/menu_config_internal.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace rs64::menucfg {

// A text group's charset (its record in the table at 0x8009FC10: charset at +0x0C, a width per charset slot at +0x18): the slot of c,
// or -1 when the font lacks it.
int font_glyph(uint8_t* rdram, uint32_t font, char c)
{

    const uint32_t obj = (uint32_t)rd_w(rdram, 0x9FC10u + font * 4) - 0x80000000u;
    const uint32_t charset = (uint32_t)rd_w(rdram, obj + 0x0C) - 0x80000000u;

    for (int slot = 0; slot < 255 && rd_b(rdram, charset + slot); ++slot)
    {
        if (rd_b(rdram, charset + slot) == (uint8_t)c) return slot;
    }

    return -1;
}

// Width of s in a text group's font, in font pixels; a character the font lacks (the space) counts gap.
int font_width(uint8_t* rdram, uint32_t font, const std::string& s, int gap)
{

    const uint32_t widths = (uint32_t)rd_w(rdram, (uint32_t)rd_w(rdram, 0x9FC10u + font * 4) - 0x80000000u + 0x18) - 0x80000000u;
    int w = 0;

    for (const char c : s)
    {
        const int slot = font_glyph(rdram, font, c);

        w += slot >= 0 ? rd_b(rdram, widths + slot) : gap;
    }

    return w;
}

// The padlock takes the brace '}' slot, which is also the level select's right arrow (the string "}" at 0x800A5BCC): the slot shows the padlock only on
// a page whose live labels hold a '$' (co-op's locked games), the brace everywhere else.
static struct { uint32_t dst = 0; std::vector<uint8_t> brace, lock; bool shown = true; } s_lock_glyph;

// Image and charset entry of the shared slot (50) for this frame; runs after ensure_menu_period, which renames the slot to '$' every frame.
void show_menu_lock(uint8_t* rdram, bool lock)
{

    if (!s_lock_glyph.dst) return;

    for (const uint32_t id : { 0u, 5u })
    {
        const uint32_t obj = (uint32_t)rd_w(rdram, 0x9FC10u + id * 4) - 0x80000000u;
        const uint32_t charset = (uint32_t)rd_w(rdram, obj + 0x0C) - 0x80000000u;

        wr_b(rdram, charset + 50, lock ? '$' : '}');
    }

    if (lock == s_lock_glyph.shown) return;

    const std::vector<uint8_t>& img = lock ? s_lock_glyph.lock : s_lock_glyph.brace;

    for (size_t i = 0; i < img.size(); ++i) wr_b(rdram, s_lock_glyph.dst + (uint32_t)i, img[i]);

    s_lock_glyph.shown = lock;
}

// The italic menu font ("italic35") lacks '.', a period-wide blank ('`', status_dots padding) and a caret ('|'): the '&', '"' and '/' slots (no language's text uses them; German needs the umlauts) become them, cut from the font's own
// ':' (lower dot; blank) and '!' (its stem). Glyphs are D_80128F08 entries (0x24: W @8, size @0xC, data @0x10, name @0x14 = "F" char "A"); the font's charset and widths share the index.
void ensure_menu_period(uint8_t* rdram)
{

    enum Rows { kLastRun, kNone, kAllButLastRun, kUpArrow, kDownArrow, kLock };
    struct Glyph { int slot; uint8_t from; uint8_t to; uint8_t src; int src_slot; Rows rows; };

    // ';' and '_' (not in the letter wheels' alphabet) become the Controls list's scroll arrows '^' and '#', blank-wide so a row keeps its width; '}' becomes a padlock '$' in its own box (multiplayer's locked games).
    static const Glyph kGlyphs[] = {{40, '&', '.', ':', 36, kLastRun}, {41, '"', '`', ':', 36, kNone}, {39, '/', '|', '!', 44, kAllButLastRun},
                                    {35, ';', '^', ':', 36, kUpArrow}, {37, '_', '#', ':', 36, kDownArrow}, {50, '}', '$', '}', 50, kLock}};
    constexpr int kCount = (int)(sizeof(kGlyphs) / sizeof(kGlyphs[0]));

    // italic35 is font ids 0 and 5 (table 0x8009FC10); its object holds the charset (+0x0C), widths (+0x18) and the char -> glyph map built at load (+0x20), which language mods may move to host copies.
    for (const uint32_t id : { 0u, 5u })
    {
        const uint32_t obj = (uint32_t)rd_w(rdram, 0x9FC10u + id * 4) - 0x80000000u;
        const uint32_t charset = (uint32_t)rd_w(rdram, obj + 0x0C) - 0x80000000u;
        const uint32_t widths = (uint32_t)rd_w(rdram, obj + 0x18) - 0x80000000u;

        for (const Glyph& g : kGlyphs) {
            if (rd_b(rdram, charset + g.slot) == g.from) {
                wr_b(rdram, charset + g.slot, g.to);
                wr_b(rdram, widths + g.slot, rd_b(rdram, widths + g.src_slot));
            }
        }

        const uint32_t map = (uint32_t)rd_w(rdram, obj + 0x20);

        if (map < 0x80000000u || map >= 0x80800000u || rd_b(rdram, (map & 0x7FFFFFu) + ':') != 36) continue;

        for (const Glyph& g : kGlyphs)
            wr_b(rdram, (map & 0x7FFFFFu) + g.to, (uint8_t)g.slot);
    }

    const uint32_t table = (uint32_t)rd_w(rdram, 0x128F08u);

    if (table < 0x80000000u || table >= 0x81000000u) return;

    // A reloaded font gets fresh records while the old converted ones stay in the table, so scan every frame; converted records are renamed and never match again.
    uint32_t rec[kCount] = {}, src_rec[kCount] = {};

    for (uint32_t e = table - 0x80000000u, n = 0; n < 4096 && e + 0x24 <= 0x1000000u; e += 0x24, ++n)
    {
        if (rd_b(rdram, e + 0x14) != 'F' || rd_b(rdram, e + 0x16) != 'A' || rd_b(rdram, e + 0x17) != 0) continue;

        const uint8_t c = rd_b(rdram, e + 0x15);

        for (int k = 0; k < kCount; ++k) {
            if (c == kGlyphs[k].from) rec[k] = e;
            if (c == kGlyphs[k].src) src_rec[k] = e;
        }
    }

    for (int k = 0; k < kCount; ++k)
        if (!rec[k] || !src_rec[k] || (uint16_t)rd_h(rdram, rec[k] + 0x0C) < (uint16_t)rd_h(rdram, src_rec[k] + 0x0C)) return;

    // I4 glyphs 33 rows high, loaded at the record's width rounded up; each new glyph takes its source's layout, size and width (a target buffer smaller than its source skips the conversion).
    // Rows keep their bytes (the odd-row word swap stays inside a row), so a row copies as-is.
    constexpr int kRows = 33;

    for (int k = 0; k < kCount; ++k)
    {
        const uint32_t src = (uint32_t)rd_w(rdram, src_rec[k] + 0x10) - 0x80000000u;
        const uint16_t size = (uint16_t)rd_h(rdram, src_rec[k] + 0x0C);
        const uint32_t stride = size / kRows;

        auto row_lit = [&](int y) {

            for (uint32_t i = 0; i < stride; ++i)
                if (rd_b(rdram, src + y * stride + i)) return true;

            return false;
        };

        // The last run of lit rows: the colon's lower dot, the exclamation mark's dot.
        int last = kRows - 1;

        while (last >= 0 && !row_lit(last)) --last;

        int first = last;

        while (first > 0 && row_lit(first - 1)) --first;
        if (stride == 0 || last < 0) return;

        const uint32_t dst = (uint32_t)rd_w(rdram, rec[k] + 0x10) - 0x80000000u;
        const Rows mode = kGlyphs[k].rows;

        // A filled triangle in the source's box, apex on the colon's dot rows' span: I4 nibbles, high = left pixel; odd rows have their two words swapped.
        if (mode == kUpArrow || mode == kDownArrow)
        {
            const int w = rd_h(rdram, src_rec[k] + 0x08);
            const int h = last - first + 8, top = last - h + 1, cx2 = w - 1;
            auto lit = [&](int x, int y) {

                const int r = mode == kUpArrow ? y - top : top + h - 1 - y;

                return r >= 0 && r < h && std::abs(2 * x - cx2) <= r * cx2 / (h - 1);
            };

            for (uint32_t i = 0; i < size; ++i)
            {
                const int y = (int)(i / stride), j = (int)(i % stride);
                const uint8_t v = (uint8_t)((lit(2 * j, y) ? 0xF0 : 0) | (lit(2 * j + 1, y) ? 0x0F : 0));
                wr_b(rdram, dst + y * stride + ((y & 1) && stride >= 8 ? (j ^ 4) : j), v);
            }
        }

        // A padlock over the brace's lit rows: a shackle (an inverted U of stroke t) on the top 45%, a solid body with a keyhole below.
        if (mode == kLock)
        {
            const int w = rd_h(rdram, src_rec[k] + 0x08);
            int top = 0;

            while (top < last && !row_lit(top)) ++top;

            const int h = last - top + 1, body = top + h * 9 / 20, t = std::max(2, w / 6), inset = w / 5;
            const int hole_top = body + (last - body) / 3, hole_bottom = last - (last - body) / 4;

            s_lock_glyph.brace.resize(size);

            for (uint32_t i = 0; i < size; ++i) s_lock_glyph.brace[i] = rd_b(rdram, dst + i);

            auto lit = [&](int x, int y) {

                if (x < 1 || x >= w - 1 || y < top || y > last) return false;
                if (y >= body) return !(std::abs(2 * x - (w - 1)) <= 2 && y >= hole_top && y <= hole_bottom);

                const bool side = (x >= inset && x < inset + t) || (x > w - 1 - inset - t && x <= w - 1 - inset);
                return side || (y < top + t && x >= inset && x <= w - 1 - inset);
            };

            for (uint32_t i = 0; i < size; ++i)
            {
                const int y = (int)(i / stride), j = (int)(i % stride);
                const uint8_t v = (uint8_t)((lit(2 * j, y) ? 0xF0 : 0) | (lit(2 * j + 1, y) ? 0x0F : 0));
                wr_b(rdram, dst + y * stride + ((y & 1) && stride >= 8 ? (j ^ 4) : j), v);
            }

            s_lock_glyph.lock.resize(size);

            for (uint32_t i = 0; i < size; ++i) s_lock_glyph.lock[i] = rd_b(rdram, dst + i);

            s_lock_glyph.dst = dst;
            s_lock_glyph.shown = true;
        }

        for (uint32_t i = 0; i < size && mode != kUpArrow && mode != kDownArrow && mode != kLock; ++i)
        {
            const int y = (int)(i / stride);
            const bool keep = kGlyphs[k].rows == kLastRun ? (y >= first && y <= last) : kGlyphs[k].rows == kAllButLastRun ? (y < first) : false;
            wr_b(rdram, dst + i, keep ? rd_b(rdram, src + i) : 0);
        }

        wr_h(rdram, rec[k] + 0x08, rd_h(rdram, src_rec[k] + 0x08));
        wr_h(rdram, rec[k] + 0x0C, (int16_t)size);
        wr_b(rdram, rec[k] + 0x15, kGlyphs[k].to);
    }

    fprintf(stderr, "[menu] italic font: period, period-wide blank, caret, arrows and lock made\n");
}
}
