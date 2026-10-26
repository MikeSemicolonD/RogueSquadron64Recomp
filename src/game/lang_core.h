#pragma once

// Language mods: pure data and logic, no runtime dependencies (tests/lang_test.cpp).
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "json/json.hpp"

namespace rs64::lang {

// The game's own text languages, by gGameSettings+6; mods may not reuse these codes.
inline constexpr const char* kBuiltinCodes[] = { "en", "de", "fr" };

struct Glyph {
    uint8_t width = 0;
    std::vector<uint8_t> pixels;
};

// A mod's glyphs for one game font (lang/glyphs/<font>.bin): "RSG1", u16 cell width, u16 cell height, u32 count, then per glyph u32 code point, u8 advance and
// cell_w * cell_h / 2 bytes of I4 rows (high nibble first, the game strips' order). All big-endian.
struct GlyphPack {
    uint16_t cell_w = 0, cell_h = 0;
    std::unordered_map<uint32_t, Glyph> glyphs;
    uint16_t drawn = 0;
};

struct Language {
    std::string code, name;
    bool rtl = false;
    std::unordered_map<uint32_t, std::string> front, game, voice;
    std::unordered_map<std::string, std::string> menu;
    std::unordered_map<std::string, GlyphPack> glyphs;
};

// Menu label text keys (a label map's "key"): per language code, key -> text, from mods' menu_text.json and language mods' lang/menu.json.
struct MenuText {
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> by_code;
};

// Later tables win over earlier ones for the same key.
inline void add_menu_text(MenuText& t, const std::string& code, const nlohmann::json& table)
{

    if (!table.is_object()) return;

    for (const auto& [key, value] : table.items())
        if (value.is_string()) t.by_code[code][key] = value.get<std::string>();
}

inline const std::string* menu_text_for(const MenuText& t, const std::string& code, const std::string& key)
{

    const auto c = t.by_code.find(code);

    if (c == t.by_code.end()) return nullptr;

    const auto k = c->second.find(key);

    return k == c->second.end() ? nullptr : &k->second;
}

inline int builtin_slot(const std::string& code)
{

    for (int i = 0; i < (int)std::size(kBuiltinCodes); ++i)
        if (code == kBuiltinCodes[i]) return i;

    return -1;
}

// `manifest` is language.json; `files` pairs a bank ("front", "game", "voice") with its JSON of text id ("0xA2" or "162") -> UTF-8 text.
inline std::optional<Language> parse_language(const std::string& manifest, const std::vector<std::pair<std::string, std::string>>& files,
                                              std::vector<std::string>& errors)
{

    using nlohmann::json;

    const json m = json::parse(manifest, nullptr, false);

    if (!m.is_object() || !m.contains("code") || !m["code"].is_string() || !m.contains("name") || !m["name"].is_string())
    {
        errors.push_back("language.json needs \"code\" and \"name\" strings");
        return std::nullopt;
    }

    Language lang;
    lang.code = m["code"].get<std::string>();
    lang.name = m["name"].get<std::string>();
    lang.rtl = m.value("rtl", false);

    if (lang.code.empty() || builtin_slot(lang.code) >= 0)
    {
        errors.push_back("language code \"" + lang.code + "\" is empty or built in");
        return std::nullopt;
    }

    for (const auto& [bank, text] : files)
    {
        if (bank == "menu")
        {
            const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);

            if (!j.is_object())
            {
                errors.push_back(lang.code + ": menu.json is not a JSON object");
                continue;
            }

            for (const auto& [key, value] : j.items())
                if (value.is_string()) lang.menu[key] = value.get<std::string>();

            continue;
        }

        auto* table = bank == "front" ? &lang.front : bank == "game" ? &lang.game : bank == "voice" ? &lang.voice : nullptr;
        const json j = json::parse(text, nullptr, false);

        if (!table || !j.is_object())
        {
            errors.push_back(lang.code + ": " + bank + ".json is not a JSON object");
            continue;
        }

        for (const auto& [key, value] : j.items())
        {
            char* end = nullptr;
            const unsigned long id = std::strtoul(key.c_str(), &end, 0);

            if (key.empty() || *end || !value.is_string())
            {
                errors.push_back(lang.code + ": " + bank + ".json entry \"" + key + "\" skipped");
                continue;
            }

            (*table)[(uint32_t)id] = value.get<std::string>();
        }
    }

    return lang;
}

// Runtime code page: ASCII draws as itself; any other character borrows a byte 0x80-0xFE whose glyph the host writes into every loaded font.
struct CodePage {
    static constexpr uint8_t kFirst = 0x80, kLast = 0xFE;
    static constexpr int kSlots = kLast - kFirst + 1;

    uint32_t slot_cp[kSlots] = {};
    uint32_t last_use[kSlots] = {};
    uint32_t clock = 0;
};

inline uint32_t slot_codepoint(const CodePage& cp, uint8_t byte)
{
    return (byte >= CodePage::kFirst && byte <= CodePage::kLast) ? cp.slot_cp[byte - CodePage::kFirst] : byte;
}

// UTF-8 to code points; a malformed sequence becomes '?'.
inline std::vector<uint32_t> decode_utf8(std::string_view s)
{

    std::vector<uint32_t> out;

    for (size_t i = 0; i < s.size();)
    {
        const uint8_t c = (uint8_t)s[i];
        const int len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        uint32_t cp = len == 1 ? c : len == 2 ? (c & 0x1Fu) : len == 3 ? (c & 0x0Fu) : (c & 0x07u);
        bool ok = len > 0 && i + len <= s.size();

        for (int k = 1; ok && k < len; ++k)
        {
            const uint8_t cc = (uint8_t)s[i + k];

            ok = (cc & 0xC0) == 0x80;
            cp = (cp << 6) | (cc & 0x3Fu);
        }

        out.push_back(ok ? cp : (uint32_t)'?');
        i += ok ? len : 1;
    }

    return out;
}

// Right-to-left lines are stored in display order: each line reversed, with runs of ASCII letters and digits kept left to right.
inline void reverse_rtl_lines(std::vector<uint32_t>& cps)
{

    auto ltr = [](uint32_t c) { return c < 0x80 && std::isalnum((int)c); };

    for (size_t start = 0; start < cps.size();)
    {
        size_t end = start;

        while (end < cps.size() && cps[end] != '\n') ++end;

        std::reverse(cps.begin() + start, cps.begin() + end);

        for (size_t i = start; i < end;)
        {
            size_t j = i;

            while (j < end && ltr(cps[j])) ++j;

            std::reverse(cps.begin() + i, cps.begin() + j);
            i = j == i ? i + 1 : j;
        }

        start = end + 1;
    }
}

// Encodes UTF-8 for the game's one-byte text. Each slot whose character changed is appended to `new_slots` (its glyph must be written).
// The characters an encoded string draws, in order (one per output byte).
inline std::vector<uint32_t> display_codepoints(std::string_view utf8, bool rtl)
{

    std::vector<uint32_t> cps = decode_utf8(utf8);

    if (rtl) reverse_rtl_lines(cps);

    return cps;
}

inline std::string encode(CodePage& cp, std::string_view utf8, bool rtl, std::vector<uint8_t>* new_slots)
{

    const std::vector<uint32_t> cps = display_codepoints(utf8, rtl);
    std::string out;

    for (uint32_t c : cps)
    {
        if (c < 0x80)
        {
            out += (char)c;
            continue;
        }

        int slot = -1;

        for (int s = 0; s < CodePage::kSlots && slot < 0; ++s)
            if (cp.slot_cp[s] == c) slot = s;

        if (slot < 0)
        {
            slot = 0;

            for (int s = 1; s < CodePage::kSlots; ++s)
                if (cp.last_use[s] < cp.last_use[slot]) slot = s;

            cp.slot_cp[slot] = c;

            if (new_slots) new_slots->push_back((uint8_t)(CodePage::kFirst + slot));
        }

        cp.last_use[slot] = ++cp.clock;
        out += (char)(CodePage::kFirst + slot);
    }

    return out;
}

// An encoded string still draws `cps` while none of its slots has been given to another character.
inline bool still_encoded(const CodePage& cp, const std::string& bytes, const std::vector<uint32_t>& cps)
{

    if (bytes.size() != cps.size()) return false;

    for (size_t i = 0; i < bytes.size(); ++i)
        if ((uint8_t)bytes[i] >= CodePage::kFirst && slot_codepoint(cp, (uint8_t)bytes[i]) != cps[i]) return false;

    return true;
}

// Marks a string's slots as just used, so a string the game keeps drawing is not the one evicted.
inline void touch(CodePage& cp, const std::string& bytes)
{

    for (const char c : bytes)
        if ((uint8_t)c >= CodePage::kFirst && (uint8_t)c <= CodePage::kLast) cp.last_use[(uint8_t)c - CodePage::kFirst] = ++cp.clock;
}

// Fonts that gain code page glyphs. credits (30x38 cells) and starwars50 (64x64) stay as they are: the front end has no memory for their extended strips.
inline bool extends_font(const std::string& name) {
    return name != "credits" && name != "starwars50";
}

// A game font file (u16 4, u16 0, u16 cell width, u16 strip height, then a 4-bit strip and padding) with `extra_cells` blank glyphs appended. The loader writes into
// the strip per glyph, so every glyph of an extended font needs its cell.
inline std::vector<uint8_t> extend_font_strip(const std::vector<uint8_t>& file, uint16_t cell_h, int extra_cells)
{

    if (file.size() < 8)
    {
        return {};
    }

    const uint32_t w = (file[4] << 8) | file[5];
    const uint32_t h = (file[6] << 8) | file[7];
    const size_t pixels = 8 + w * h / 2;
    const uint32_t new_h = h + (uint32_t)cell_h * extra_cells;

    if (pixels > file.size() || new_h > 0xFFFF) return {};

    std::vector<uint8_t> out(file.begin(), file.begin() + pixels);

    out[6] = (uint8_t)(new_h >> 8);
    out[7] = (uint8_t)new_h;
    out.resize(8 + w * new_h / 2, 0);
    out.insert(out.end(), file.begin() + pixels, file.end());

    return out;
}

// Glyphs 126 and 127 are left as placeholders that no byte maps to (the font object itself moves to a bigger host block).
inline constexpr int kPlaceholderGlyph = 126;
inline constexpr int kExtraGlyphs = CodePage::kSlots + 2;

// Glyph index of code page slot `s` in a font whose own glyphs number `count` (< kPlaceholderGlyph).
inline int slot_glyph(int count, int s)
{
    return count + s >= kPlaceholderGlyph ? count + s + 2 : count + s;
}

struct FontTables {
    std::vector<uint8_t> charset, widths;
};

// A font's charset and width table with the code page bytes and the two placeholders appended, all at `load_w` (their glyph records load that wide).
inline FontTables extend_font_tables(const uint8_t* charset, const uint8_t* widths, uint16_t count, uint8_t load_w)
{

    FontTables t;

    t.charset.assign(charset, charset + count);
    t.widths.assign(widths, widths + count);
    t.charset.resize(count + kExtraGlyphs, 0);
    t.widths.resize(count + kExtraGlyphs, load_w);

    for (int s = 0; s < CodePage::kSlots; ++s) t.charset[slot_glyph(count, s)] = (uint8_t)(CodePage::kFirst + s);

    return t;
}

inline std::optional<GlyphPack> parse_glyph_pack(const std::vector<uint8_t>& b, std::string* error)
{

    auto be = [&b](size_t at, int n) {

        uint32_t v = 0;

        for (int i = 0; i < n; ++i) v = (v << 8) | b[at + i];

        return v;
    };

    if (b.size() < 12 || b[0] != 'R' || b[1] != 'S' || b[2] != 'G' || b[3] != '1')
    {
        *error = "not an RSG1 glyph pack";
        return std::nullopt;
    }

    GlyphPack pack;
    pack.cell_w = (uint16_t)be(4, 2);
    pack.cell_h = (uint16_t)be(6, 2);

    if (!pack.cell_w || !pack.cell_h || (pack.cell_w & 1))
    {
        *error = "glyph pack cell size must be nonzero with an even width";
        return std::nullopt;
    }

    const uint32_t count = be(8, 4);
    const size_t glyph_bytes = (size_t)pack.cell_w * pack.cell_h / 2;

    if (b.size() != 12 + (size_t)count * (5 + glyph_bytes))
    {
        *error = "glyph pack size does not match its glyph count";
        return std::nullopt;
    }

    for (size_t at = 12, i = 0; i < count; ++i, at += 5 + glyph_bytes)
    {
        Glyph& g = pack.glyphs[be(at, 4)];
        g.width = b[at + 4];
        g.pixels.assign(b.begin() + at + 5, b.begin() + at + 5 + glyph_bytes);
    }

    return pack;
}

inline const Glyph* find_glyph(const GlyphPack& pack, uint32_t cp)
{

    const auto it = pack.glyphs.find(cp);

    return it == pack.glyphs.end() ? nullptr : &it->second;
}

// Cell sizes and drawn heights of the fonts that gain code page glyphs (docs/adding-menus-and-buttons.md "Fonts").
struct FontCell {
    const char* name;
    uint16_t w, h, drawn;
};

inline constexpr FontCell kFontCells[] = { { "italic35", 22, 35, 33 }, { "normal10", 10, 14, 14 }, { "normal11", 8, 15, 15 }, { "subtitle8", 6, 12, 10 },
                                           { "hatten13", 10, 20, 15 }, { "subtitle12", 12, 12, 10 }, { "subtitle20", 10, 20, 15 } };

inline uint8_t glyph_pixel(const Glyph& g, uint16_t cell_w, int x, int y)
{

    return (g.pixels[(size_t)y * (cell_w / 2) + x / 2] >> ((x & 1) ? 0 : 4)) & 0xF;
}

// What a character no pack has draws as: an outlined box inside the drawn rows.
inline Glyph placeholder_glyph(const GlyphPack& pack)
{

    const int drawn = pack.drawn ? pack.drawn : pack.cell_h;
    const int w = std::max(2, pack.cell_w * 3 / 5), top = drawn / 5, bottom = std::max(top, drawn - 1 - drawn / 10);
    Glyph g;

    g.width = (uint8_t)std::min<int>(pack.cell_w, w + 1);
    g.pixels.assign((size_t)pack.cell_w * pack.cell_h / 2, 0);

    for (int y = top; y <= bottom; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            if (y == top || y == bottom || x == 0 || x == w - 1) g.pixels[(size_t)y * (pack.cell_w / 2) + x / 2] |= (x & 1) ? 0x0C : 0xC0;
        }
    }

    return g;
}

// Advance for a slot: the glyph's own, else the placeholder's.
inline uint8_t slot_width(const GlyphPack& pack, uint32_t cp)
{

    if (const Glyph* g = find_glyph(pack, cp)) return g->width;

    return placeholder_glyph(pack).width;
}

// One glyph redrawn at another font's size, scaled by the ratio of the two fonts' drawn heights (each pixel the average of 4 x 4 samples).
inline Glyph scale_glyph(const Glyph& g, const GlyphPack& from, const GlyphPack& to)
{

    const float s = (float)(to.drawn ? to.drawn : to.cell_h) / (float)(from.drawn ? from.drawn : from.cell_h);
    Glyph out;

    out.width = (uint8_t)std::clamp<int>((int)(g.width * s + 0.5f), 1, to.cell_w);
    out.pixels.assign((size_t)to.cell_w * to.cell_h / 2, 0);

    for (int y = 0; y < to.cell_h; ++y)
    {
        for (int x = 0; x < to.cell_w; ++x)
        {
            int sum = 0;

            for (int sy = 0; sy < 4; ++sy)
            {
                for (int sx = 0; sx < 4; ++sx)
                {
                    const int fx = (int)((x + (sx + 0.5f) / 4) / s), fy = (int)((y + (sy + 0.5f) / 4) / s);

                    if (fx < from.cell_w && fy < from.cell_h) sum += glyph_pixel(g, from.cell_w, fx, fy);
                }
            }

            out.pixels[(size_t)y * (to.cell_w / 2) + x / 2] |= (uint8_t)(((sum + 8) / 16) << ((x & 1) ? 0 : 4));
        }
    }

    return out;
}

// Gives every extended font a pack holding every character any of the language's packs has: a missing one is scaled from the pack whose drawn height is
// nearest (the taller on a tie). A language without packs gains none.
inline void fill_missing_glyphs(Language& lang)
{

    if (lang.glyphs.empty()) return;

    for (const FontCell& c : kFontCells)
    {
        GlyphPack& p = lang.glyphs[c.name];

        p.cell_w = c.w;
        p.cell_h = c.h;
        p.drawn = c.drawn;
    }

    std::vector<uint32_t> cps;

    for (const auto& [name, p] : lang.glyphs)
        for (const auto& [cp, g] : p.glyphs) cps.push_back(cp);

    std::sort(cps.begin(), cps.end());
    cps.erase(std::unique(cps.begin(), cps.end()), cps.end());

    std::unordered_map<std::string, GlyphPack> filled = lang.glyphs;

    for (auto& [name, p] : filled)
    {
        for (const uint32_t cp : cps)
        {
            if (find_glyph(p, cp)) continue;

            const GlyphPack* best = nullptr;

            for (const auto& [other, q] : lang.glyphs)
            {
                if (!find_glyph(q, cp)) continue;

                const int d = std::abs(q.drawn - p.drawn), bd = best ? std::abs(best->drawn - p.drawn) : 0;

                if (!best || d < bd || (d == bd && q.drawn > best->drawn)) best = &q;
            }

            p.glyphs[cp] = scale_glyph(*find_glyph(*best, cp), *best, p);
        }
    }

    lang.glyphs = std::move(filled);
}

// A glyph record's pixels: `rows` rows of `stride` bytes from the top of the cell (the loader keeps the font's drawn height); a missing glyph is the placeholder.
// The buffer is in TMEM order: odd rows swap the two words of each 8-byte group.
inline std::vector<uint8_t> glyph_rows(const GlyphPack& pack, uint32_t cp, uint32_t stride, uint16_t rows)
{

    std::vector<uint8_t> out((size_t)stride * rows, 0);
    const Glyph box = find_glyph(pack, cp) ? Glyph{} : placeholder_glyph(pack);
    const Glyph* g = box.pixels.empty() ? find_glyph(pack, cp) : &box;
    const uint32_t row_bytes = pack.cell_w / 2u;
    const bool interleave = stride % 8 == 0;

    for (uint32_t y = 0; g && y < rows && y < pack.cell_h; ++y)
    {
        const uint32_t swap = interleave && (y & 1) ? 4 : 0;

        for (uint32_t x = 0; x < stride && x < row_bytes; ++x) out[y * stride + (x ^ swap)] = g->pixels[y * row_bytes + x];
    }

    return out;
}

// The same rows for an S-kind record, which is 8 bits per pixel: 4-bit value v becomes v * 0x11 (one byte that reads the same as I8 and IA8).
inline std::vector<uint8_t> glyph_rows_8bpp(const GlyphPack& pack, uint32_t cp, uint32_t stride, uint16_t rows)
{

    std::vector<uint8_t> out((size_t)stride * rows, 0);
    const Glyph box = find_glyph(pack, cp) ? Glyph{} : placeholder_glyph(pack);
    const Glyph* g = box.pixels.empty() ? find_glyph(pack, cp) : &box;
    const uint32_t row_bytes = pack.cell_w / 2u;
    const bool interleave = stride % 8 == 0;

    for (uint32_t y = 0; g && y < rows && y < pack.cell_h; ++y)
    {
        const uint32_t swap = interleave && (y & 1) ? 4 : 0;

        for (uint32_t x = 0; x < stride && x < pack.cell_w; ++x)
        {
            const uint8_t v = (g->pixels[y * row_bytes + x / 2] >> ((x & 1) ? 0 : 4)) & 0xF;

            out[y * stride + (x ^ swap)] = (uint8_t)(v * 0x11);
        }
    }

    return out;
}

inline bool font_cell(const std::string& name, uint16_t* w, uint16_t* h)
{

    for (const FontCell& c : kFontCells)
    {
        if (name == c.name)
        {
            *w = c.w;
            *h = c.h;
            return true;
        }
    }

    return false;
}

inline bool pack_fits_font(const GlyphPack& pack, const std::string& font)
{

    uint16_t w = 0, h = 0;

    return font_cell(font, &w, &h) && pack.cell_w == w && pack.cell_h == h;
}

// Map entries for bytes 0x80-0xFE: the slot glyphs (after the font's `count` own glyphs) while a mod language is active, otherwise the game's own charset search.
inline std::vector<uint8_t> slot_map(const uint8_t* charset, uint16_t count, bool mod_language)
{

    std::vector<uint8_t> out(CodePage::kSlots, 0xFF);

    for (int s = 0; s < CodePage::kSlots; ++s)
    {
        if (mod_language)
        {
            out[s] = (uint8_t)slot_glyph(count, s);
            continue;
        }

        for (uint16_t i = 0; i < count; ++i)
        {
            if (charset[i] == CodePage::kFirst + s)
            {
                out[s] = (uint8_t)i;
                break;
            }
        }
    }

    return out;
}

} // namespace rs64::lang
