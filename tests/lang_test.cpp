#include "game/lang_core.h"
#include "check.h"
#include <cstdio>

using namespace rs64::lang;

int main() {

    // parses_language_and_strings
    {
        std::vector<std::string> errors;
        const auto lang = parse_language(R"({ "code": "ja", "name": "日本語", "rtl": false })",
                                         { { "front", R"({ "0xA2": "スタート", "163": "オプション" })" },
                                           { "voice", R"({ "0x1": "了解" })" } },
                                         errors);

        CHECK(lang.has_value());
        CHECK(errors.empty());
        CHECK(lang->code == "ja" && lang->name == "日本語" && !lang->rtl);
        CHECK(lang->front.at(0xA2) == "スタート");
        CHECK(lang->front.at(0xA3) == "オプション");
        CHECK(lang->voice.at(1) == "了解");
        CHECK(lang->game.empty());
    }

    // bad_json_is_reported_not_thrown
    {
        std::vector<std::string> errors;
        const auto bad_manifest = parse_language("{ nope", {}, errors);

        CHECK(!bad_manifest.has_value());
        CHECK(errors.size() == 1);

        errors.clear();
        const auto bad_strings = parse_language(R"({ "code": "xx", "name": "X" })", { { "game", "[1,2" } }, errors);

        CHECK(bad_strings.has_value());
        CHECK(bad_strings->game.empty());
        CHECK(errors.size() == 1);
    }

    // manifest_needs_code_and_name
    {
        std::vector<std::string> errors;

        CHECK(!parse_language(R"({ "name": "X" })", {}, errors).has_value());
        CHECK(!parse_language(R"({ "code": "en", "name": "X" })", {}, errors).has_value());
    }

    // builtin_slots
    {
        CHECK(builtin_slot("en") == 0);
        CHECK(builtin_slot("de") == 1);
        CHECK(builtin_slot("fr") == 2);
        CHECK(builtin_slot("ja") == -1);
        CHECK(builtin_slot("") == -1);
    }

    // encode_ascii_passes_through
    {
        CodePage cp;
        std::vector<uint8_t> fresh;

        CHECK(encode(cp, "BACK", false, &fresh) == "BACK");
        CHECK(fresh.empty());
        CHECK(encode(cp, "%s %d #s:0\x01", false, &fresh) == "%s %d #s:0\x01");
        CHECK(fresh.empty());
    }

    // encode_assigns_and_reuses_slots
    {
        CodePage cp;
        std::vector<uint8_t> fresh;
        const std::string s = encode(cp, "スタート", false, &fresh);

        CHECK(s.size() == 4);
        CHECK(fresh.size() == 4);

        for (unsigned char c : s) CHECK(c >= CodePage::kFirst && c <= CodePage::kLast);

        CHECK(slot_codepoint(cp, (uint8_t)s[0]) == 0x30B9);
        CHECK(s[2] != s[3]);

        fresh.clear();
        const std::string t = encode(cp, "ートー", false, &fresh);

        CHECK(fresh.empty());
        CHECK(t[0] == s[2] && t[1] == s[3] && t[2] == s[2]);
    }

    // encode_evicts_least_recently_used
    {
        CodePage cp;
        std::vector<uint8_t> fresh;
        const std::string first = encode(cp, "\xE4\xB8\x80", false, &fresh);
        const std::string second = encode(cp, "\xE4\xB8\x81", false, &fresh);

        for (uint32_t c = 0x4E02; c < 0x4E00 + 127; ++c)
        {
            const char u[4] = { (char)(0xE0 | (c >> 12)), (char)(0x80 | ((c >> 6) & 0x3F)), (char)(0x80 | (c & 0x3F)), 0 };
            encode(cp, u, false, &fresh);
        }

        encode(cp, "\xE4\xB8\x80", false, &fresh);
        fresh.clear();
        const std::string evictor = encode(cp, "\xE5\x80\x80", false, &fresh);

        CHECK(fresh.size() == 1 && fresh[0] == (uint8_t)evictor[0]);
        CHECK(evictor == second);
        CHECK(slot_codepoint(cp, (uint8_t)first[0]) == 0x4E00);
        CHECK(slot_codepoint(cp, (uint8_t)evictor[0]) == 0x5000);
    }

    // encode_rtl_reverses_lines_but_not_digit_runs
    {
        CodePage cp;
        std::vector<uint8_t> fresh;
        const std::string s = encode(cp, "\xD7\x90\xD7\x91\xD7\x92 12\n\xD7\x93\xD7\x94", true, &fresh);

        CHECK(s.size() == 9);
        CHECK(s[0] == '1' && s[1] == '2' && s[2] == ' ');
        CHECK(slot_codepoint(cp, (uint8_t)s[3]) == 0x05D2);
        CHECK(slot_codepoint(cp, (uint8_t)s[5]) == 0x05D0);
        CHECK(s[6] == '\n');
        CHECK(slot_codepoint(cp, (uint8_t)s[7]) == 0x05D4);
    }

    // encode_invalid_utf8_becomes_question_mark
    {
        CodePage cp;

        CHECK(encode(cp, "A\xFF" "B\xC3", false, nullptr) == "A?B?");
    }

    // extend_font_strip_appends_blank_cells
    {
        std::vector<uint8_t> file = { 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x06 };

        for (int i = 0; i < 4 * 6 / 2; ++i) file.push_back(0x11);
        for (int i = 0; i < 4; ++i) file.push_back(0xEE);

        const auto out = extend_font_strip(file, 3, 2);

        CHECK(out.size() == file.size() + 4 * 6 / 2);
        CHECK(out[6] == 0x00 && out[7] == 12);
        CHECK(out[8 + 11] == 0x11 && out[8 + 12] == 0x00 && out[8 + 23] == 0x00);
        CHECK(out[8 + 24] == 0xEE && out.back() == 0xEE);
        CHECK(extend_font_strip({ 1, 2, 3 }, 3, 2).empty());
    }

    // extended_fonts_skip_credits_and_starwars50
    {
        CHECK(extends_font("italic35") && extends_font("subtitle20") && extends_font("hatten13"));
        CHECK(!extends_font("credits") && !extends_font("starwars50"));
    }

    // extend_font_tables_appends_slot_bytes_at_cell_width
    {
        const uint8_t charset[] = { 'A', 'B', 0xC4 };
        const uint8_t widths[] = { 10, 11, 12 };
        const auto t = extend_font_tables(charset, widths, 3, 22);

        CHECK(t.charset.size() == 3 + kExtraGlyphs && t.widths.size() == t.charset.size());
        CHECK(t.charset[2] == 0xC4 && t.charset[3] == 0x80 && t.charset.back() == 0xFE);
        CHECK(t.widths[1] == 11 && t.widths[3] == 22 && t.widths.back() == 22);

        // Glyphs 126 and 127 sit on the font object's +0x102/+0x103 fields: placeholders no byte maps to.
        CHECK(t.charset[kPlaceholderGlyph] == 0 && t.charset[kPlaceholderGlyph + 1] == 0);
        CHECK(t.charset[kPlaceholderGlyph - 1] == 0x80 + (kPlaceholderGlyph - 1 - 3));
        CHECK(t.charset[kPlaceholderGlyph + 2] == 0x80 + (kPlaceholderGlyph - 3));
    }

    // slot_glyph_skips_the_placeholders
    {
        CHECK(slot_glyph(3, 0) == 3);
        CHECK(slot_glyph(3, kPlaceholderGlyph - 4) == kPlaceholderGlyph - 1);
        CHECK(slot_glyph(3, kPlaceholderGlyph - 3) == kPlaceholderGlyph + 2);
        CHECK(slot_glyph(101, CodePage::kSlots - 1) == 101 + kExtraGlyphs - 1);
    }

    // slot_map_points_slots_at_their_glyphs_or_the_charset
    {
        const uint8_t charset[] = { 'A', 0xC4, 'B' };
        const auto on = slot_map(charset, 3, true);
        const auto off = slot_map(charset, 3, false);

        CHECK(on[0] == 3 && on[0xC4 - 0x80] == slot_glyph(3, 0x44) && on[CodePage::kSlots - 1] == slot_glyph(3, 126));
        CHECK(off[0xC4 - 0x80] == 1 && off[0] == 0xFF);
    }

    // glyph_pack_parses_and_looks_up_code_points
    {
        // RSG1, 4 x 2 cells, two glyphs: U+30A2 (width 3) and space (width 2); I4 rows are 2 bytes.
        const std::vector<uint8_t> bytes = { 'R', 'S', 'G', '1', 0, 4, 0, 2, 0, 0, 0, 2,
                                             0, 0, 0x30, 0xA2, 3, 0x12, 0x34, 0x56, 0x78,
                                             0, 0, 0, 0x20, 2, 0, 0, 0, 0 };
        std::string error;
        const auto pack = parse_glyph_pack(bytes, &error);

        CHECK(pack.has_value() && error.empty());
        CHECK(pack->cell_w == 4 && pack->cell_h == 2);

        const Glyph* a = find_glyph(*pack, 0x30A2);

        CHECK(a && a->width == 3 && a->pixels == std::vector<uint8_t>({ 0x12, 0x34, 0x56, 0x78 }));
        CHECK(find_glyph(*pack, 0x30A4) == nullptr);
        CHECK(slot_width(*pack, 0x30A2) == 3);

        // Rows into a record buffer: stride 3 (wider than the cell row), 3 rows (taller than the cell); extra bytes and rows stay blank.
        CHECK(glyph_rows(*pack, 0x30A2, 3, 3) == std::vector<uint8_t>({ 0x12, 0x34, 0, 0x56, 0x78, 0, 0, 0, 0 }));
        CHECK(glyph_rows(*pack, 0x30A2, 1, 1) == std::vector<uint8_t>({ 0x12 }));

        // A code point no pack has draws the placeholder box.
        const Glyph box = placeholder_glyph(*pack);

        CHECK(slot_width(*pack, 0x30A4) == box.width);
        CHECK(glyph_rows(*pack, 0x30A4, 2, 2) == box.pixels);
        CHECK(glyph_rows(*pack, 0x30A4, 2, 2) != std::vector<uint8_t>(4, 0));

        // Record buffers keep TMEM's interleave: on odd rows the two words of each 8-byte group are swapped (only for strides that are whole 8-byte groups).
        const std::vector<uint8_t> two_rows = glyph_rows(*pack, 0x30A2, 8, 2);

        CHECK(two_rows[0] == 0x12 && two_rows[1] == 0x34 && two_rows[8] == 0 && two_rows[12] == 0x56 && two_rows[13] == 0x78);

        // S-kind records are 8 bits per pixel: each 4-bit value v becomes v * 0x11 (the same byte in I8 and IA8), with the odd-row word swap.
        const std::vector<uint8_t> wide = glyph_rows_8bpp(*pack, 0x30A2, 8, 2);

        CHECK(wide.size() == 16 && wide[0] == 0x11 && wide[1] == 0x22 && wide[2] == 0x33 && wide[3] == 0x44 && wide[4] == 0);
        CHECK(wide[12] == 0x55 && wide[13] == 0x66 && wide[14] == 0x77 && wide[15] == 0x88);
    }

    // menu_text_later_mod_wins
    {
        MenuText t;
        add_menu_text(t, "de", nlohmann::json::parse(R"({ "ds": "ANZEIGE", "rt": "RAYTRACING" })"));
        add_menu_text(t, "de", nlohmann::json::parse(R"({ "ds": "BILD" })"));

        CHECK(menu_text_for(t, "de", "ds") && *menu_text_for(t, "de", "ds") == "BILD");
        CHECK(menu_text_for(t, "de", "rt") && *menu_text_for(t, "de", "rt") == "RAYTRACING");
    }

    // menu_text_misses_fall_through
    {
        MenuText t;
        add_menu_text(t, "de", nlohmann::json::parse(R"({ "ds": "ANZEIGE", "n": 3 })"));

        CHECK(menu_text_for(t, "fr", "ds") == nullptr);
        CHECK(menu_text_for(t, "de", "nope") == nullptr);
        CHECK(menu_text_for(t, "de", "n") == nullptr);
    }

    // language_parses_menu_table
    {
        std::vector<std::string> errors;
        const auto lang = parse_language(R"({ "code": "ja", "name": "NIHONGO" })", { { "menu", R"({ "ds": "表示" })" } }, errors);

        CHECK(lang.has_value() && errors.empty());
        CHECK(lang->menu.at("ds") == "表示");
    }

    // placeholder_box_stays_in_the_drawn_rows
    {
        GlyphPack pack{ 10, 20, {} };
        pack.drawn = 15;

        const Glyph box = placeholder_glyph(pack);
        auto at = [&](int x, int y) { return (box.pixels[y * 5 + x / 2] >> ((x & 1) ? 0 : 4)) & 0xF; };
        int ink = 0, below = 0;

        for (int y = 0; y < 20; ++y)
            for (int x = 0; x < 10; ++x) (y < 15 ? ink : below) += at(x, y) != 0;

        CHECK(box.pixels.size() == 100 && ink > 0 && below == 0);
        CHECK(box.width > 0 && box.width <= 10);
        CHECK(at(box.width / 2, 7) == 0);
    }

    // missing_glyphs_come_from_the_nearest_sized_pack
    {
        auto solid = [](uint16_t w, uint16_t h, uint16_t ink_w, uint16_t ink_h, uint8_t width) {

            Glyph g;
            g.width = width;
            g.pixels.assign((size_t)w * h / 2, 0);

            for (int y = 0; y < ink_h; ++y)
                for (int x = 0; x < ink_w; ++x) g.pixels[y * (w / 2) + x / 2] |= (x & 1) ? 0x0F : 0xF0;

            return g;
        };

        Language lang;
        lang.glyphs["italic35"] = GlyphPack{ 22, 35, {}, 33 };
        lang.glyphs["subtitle8"] = GlyphPack{ 6, 12, {}, 10 };
        lang.glyphs["italic35"].glyphs[0x3042] = solid(22, 35, 20, 33, 21);
        lang.glyphs["italic35"].glyphs[0x3044] = solid(22, 35, 20, 33, 21);
        lang.glyphs["subtitle8"].glyphs[0x3044] = solid(6, 12, 2, 10, 2);
        lang.glyphs["subtitle8"].glyphs[0x3046] = solid(6, 12, 5, 10, 6);

        const Glyph kept = lang.glyphs["subtitle8"].glyphs[0x3044];

        fill_missing_glyphs(lang);

        // Every extended font gets a pack at its own cell size holding every character any pack has.
        CHECK(lang.glyphs.size() == std::size(kFontCells));

        for (const FontCell& c : kFontCells)
        {
            const GlyphPack& p = lang.glyphs[c.name];

            CHECK(pack_fits_font(p, c.name) && p.drawn == c.drawn);
            CHECK(find_glyph(p, 0x3042) && find_glyph(p, 0x3044) && find_glyph(p, 0x3046));
        }

        // A pack's own glyphs stay as they are.
        CHECK(lang.glyphs["subtitle8"].glyphs[0x3044].pixels == kept.pixels && lang.glyphs["subtitle8"].glyphs[0x3044].width == 2);

        // Scaled by the drawn heights: italic35's あ (21 wide, 33 rows) in subtitle8 is 21 * 10 / 33 = 6 wide, with no ink under the drawn rows.
        const Glyph& a8 = lang.glyphs["subtitle8"].glyphs[0x3042];
        int ink = 0, below = 0;

        for (int y = 0; y < 12; ++y)
            for (int x = 0; x < 6; ++x) (y < 10 ? ink : below) += ((a8.pixels[y * 3 + x / 2] >> ((x & 1) ? 0 : 4)) & 0xF) != 0;

        CHECK(a8.width == 6 && ink > 30 && below == 0);

        // normal10 (14 rows) takes い from subtitle8 (10 rows, nearer than italic35's 33): 2 * 14 / 10 rounds to 3.
        CHECK(lang.glyphs["normal10"].glyphs[0x3044].width == 3);

        // A language without packs gains none.
        Language bare;
        fill_missing_glyphs(bare);
        CHECK(bare.glyphs.empty());
    }

    // glyph_pack_rejects_bad_files
    {
        std::string error;

        CHECK(!parse_glyph_pack({ 'X', 'S', 'G', '1', 0, 4, 0, 2, 0, 0, 0, 0 }, &error) && !error.empty());
        error.clear();
        CHECK(!parse_glyph_pack({ 'R', 'S', 'G', '1', 0, 4, 0, 2, 0, 0, 0, 1, 0, 0, 0, 0x41, 3, 0x12 }, &error) && !error.empty());
        error.clear();
        CHECK(!parse_glyph_pack({ 'R', 'S', 'G', '1', 0, 3, 0, 2, 0, 0, 0, 0 }, &error) && !error.empty());
    }

    // glyph_pack_cell_must_match_the_font
    {
        uint16_t w = 0, h = 0;

        CHECK(font_cell("italic35", &w, &h) && w == 22 && h == 35);
        CHECK(font_cell("subtitle8", &w, &h) && w == 6 && h == 12);
        CHECK(!font_cell("credits", &w, &h) && !font_cell("nope", &w, &h));
        CHECK(pack_fits_font(GlyphPack{ 22, 35, {} }, "italic35"));
        CHECK(!pack_fits_font(GlyphPack{ 22, 33, {} }, "italic35"));
    }

    // cached_text_notices_evicted_slots
    {
        CodePage cp;
        const std::string first = encode(cp, "アイ", false, nullptr);
        const auto cps = display_codepoints("アイ", false);

        CHECK(cps.size() == 2 && cps[0] == 0x30A2);
        CHECK(still_encoded(cp, first, cps));

        // Fill every other slot, then one more character evicts the least recently used (ア, since イ is touched).
        auto utf8 = [](uint32_t c) { return std::string{ (char)(0xE0 | (c >> 12)), (char)(0x80 | ((c >> 6) & 0x3F)), (char)(0x80 | (c & 0x3F)) }; };

        for (uint32_t c = 0x4E00; c < 0x4E00 + CodePage::kSlots - 2; ++c) encode(cp, utf8(c), false, nullptr);
        touch(cp, std::string(1, first[1]));
        encode(cp, "犬", false, nullptr);
        CHECK(!still_encoded(cp, first, cps));

        // Re-encoding gives the same length (one byte per character).
        CHECK(encode(cp, "アイ", false, nullptr).size() == first.size());
    }

    std::printf("lang_test: ok\n");

    return 0;
}
