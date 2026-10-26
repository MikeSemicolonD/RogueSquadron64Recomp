#include "game/lang.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <thread>

#include "librecomp/mods.hpp"
#include "ultramodern/error_handling.hpp"
#include "lockstep/lockstep_core.h"
#include "recomp.h"
#include "util/rdram_words.h"
#include "video/video_config.h"


namespace rs64::lang {

namespace {

using rs64::mips::rh;
using rs64::mips::rw;
using rs64::mips::ww;

std::vector<Language> s_languages;
std::once_flag s_loaded;

// Font objects by font id (docs/guides/adding-menus-and-buttons.md "Fonts").
constexpr uint32_t kFontTable = 0x8009FC10;
constexpr unsigned kFontIds = 12;

struct ExtendedFont {
    uint32_t obj;
    uint16_t count;
    uint32_t charset;
    std::string name;
};

std::vector<ExtendedFont> s_fonts;
std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> s_tables;
CodePage s_page;

// Encoded strings by (bank << 24 | text id), the slots waiting for their glyphs, and which text file getGameOrFrontText reads (0 front, 1 game).
struct Placed {
    uint32_t addr = 0;
    std::string bytes;
    std::vector<uint32_t> cps;
};

std::recursive_mutex s_text_mutex;
std::unordered_map<uint32_t, Placed> s_placed;
std::vector<uint8_t> s_pending;
std::unordered_map<std::string, std::string> s_originals;
int s_bank = 0;
uint32_t s_voice_id = 0;

void wb(uint8_t* rdram, uint32_t a, uint8_t v) {
    rdram[(a - 0x80000000u) ^ 3] = v;
}

void wh(uint8_t* rdram, uint32_t a, uint16_t v)
{

    wb(rdram, a, (uint8_t)(v >> 8));
    wb(rdram, a + 1, (uint8_t)v);
}

uint8_t rb(const uint8_t* rdram, uint32_t a) {
    return rdram[(a - 0x80000000u) ^ 3];
}

// Extended font objects and their tables live in host RAM below 0x81000000 (the game checks some pointers for top byte 0x80), in this reserved 64 KB.
constexpr uint32_t kTablesStart = 0x80C44000u, kTablesEnd = 0x80C54000u;

// The texture record table (0x24 bytes per record) and its two word arrays for kRecords records follow, ending where the first texture pool region starts.
constexpr uint32_t kRecords = 4096;
constexpr uint32_t kRecordTable = kTablesEnd, kRecordWords2 = kRecordTable + kRecords * 0x24, kRecordWords3 = kRecordWords2 + kRecords * 4;
constexpr uint32_t kRecordsEnd = kRecordWords3 + kRecords * 4;

// The HMT material slot table ({u16 flags, u16 record} each) and its two 16-byte-per-slot arrays for kSlots slots follow.
constexpr uint32_t kSlots = 4096;
constexpr uint32_t kHostSlotTable = kRecordsEnd, kHostSlotArrayA = kHostSlotTable + kSlots * 4, kHostSlotArrayB = kHostSlotArrayA + kSlots * 16;
constexpr uint32_t kSlotsEnd = kHostSlotArrayB + kSlots * 16;

// Encoded mod-language strings (one byte per character, so a re-encode always fits in place); the first texture pool region starts after it.
constexpr uint32_t kTextStart = kSlotsEnd, kTextEnd = kTextStart + 0x40000u;

// Strings are placed per language: a language change (seen at the next text load) starts the area over.
uint32_t s_text_next = kTextStart;
std::string s_placed_code;

uint32_t alloc_bytes(uint8_t* rdram, const std::vector<uint8_t>& bytes)
{

    static uint32_t s_next = kTablesStart;
    const uint32_t a = s_next;

    if (a + bytes.size() > kTablesEnd)
    {
        return 0;
    }

    s_next = (uint32_t)((a + bytes.size() + 0xF) & ~size_t(0xF));

    for (size_t i = 0; i < bytes.size(); ++i) wb(rdram, a + (uint32_t)i, bytes[i]);

    return a;
}

void apply_map(uint8_t* rdram, const ExtendedFont& f, bool mod_language)
{

    const uint32_t map = rw(rdram, f.obj + 0x20);

    if (!map)
    {
        return;
    }

    std::vector<uint8_t> charset(f.count);

    for (uint16_t i = 0; i < f.count; ++i) charset[i] = rb(rdram, f.charset + i);

    const auto entries = slot_map(charset.data(), f.count, mod_language);

    for (int s = 0; s < CodePage::kSlots; ++s) wb(rdram, map + CodePage::kFirst + s, entries[s]);
}

std::string mod_text(const std::string& mod_id, const char* path)
{

    bool exists = false;
    const std::vector<char> data = recomp::mods::read_mod_file(mod_id, path, exists);

    return exists ? std::string(data.begin(), data.end()) : std::string{};
}

void load_glyph_packs(const std::string& mod_id, Language& lang)
{

    for (const FontCell& cell : kFontCells)
    {
        const std::string data = mod_text(mod_id, (std::string("lang/glyphs/") + cell.name + ".bin").c_str());

        if (data.empty()) continue;

        std::string error;
        auto pack = parse_glyph_pack(std::vector<uint8_t>(data.begin(), data.end()), &error);

        if (pack && !pack_fits_font(*pack, cell.name)) error = "cell " + std::to_string(pack->cell_w) + "x" + std::to_string(pack->cell_h) + " does not match the font";

        if (!error.empty())
        {
            fprintf(stderr, "[lang] %s: glyphs/%s.bin skipped: %s\n", mod_id.c_str(), cell.name, error.c_str());
            continue;
        }

        pack->drawn = cell.drawn;
        lang.glyphs[cell.name] = std::move(*pack);
    }

    fill_missing_glyphs(lang);
}

// One code page slot's glyph in one loaded font: its advance in the width table and its pixels in each of its glyph textures (A and S kinds). The font keeps
// HMT material slot ids per glyph (registerHmtTextureInTable's result); a slot's entry (table 0x8011A444, {u16 flags, u16 record}) names the texture record.
void write_slot(uint8_t* rdram, const ExtendedFont& f, const GlyphPack* pack, int s)
{

    constexpr uint32_t kRecordTablePtr = 0x80128F08u, kRecordCount = 0x80128EF0u, kMaterialTablePtr = 0x8011A444u, kMaterialCount = 0x8011A8B4u;
    const uint32_t cp = s_page.slot_cp[s];
    const int glyph = slot_glyph(f.count, s);
    const uint8_t width = pack ? slot_width(*pack, cp) : (uint8_t)(rh(rdram, f.obj + 0x02) / 2);
    const uint32_t table = rw(rdram, kRecordTablePtr);
    const uint16_t records = rh(rdram, kRecordCount);

    wb(rdram, rw(rdram, f.obj + 0x18) + glyph, width);

    for (const auto [flag, ids] : { std::pair{ 0x1Eu, 0x24u }, std::pair{ 0x1Fu, 0x28u } })
    {
        const uint32_t id_array = rw(rdram, f.obj + ids);

        if (!rb(rdram, f.obj + flag) || !id_array) continue;

        const uint16_t material = rh(rdram, id_array + glyph * 2);

        if (material >= rh(rdram, kMaterialCount)) continue;

        const uint16_t id = rh(rdram, rw(rdram, kMaterialTablePtr) + material * 4u + 2);

        if (id >= records) continue;

        const uint32_t rec = table + id * 0x24u;
        const uint32_t data = rw(rdram, rec + 0x10);
        const uint16_t rows = rh(rdram, rec + 0x0A);

        if (!data || !rows) continue;

        // A records are 4 bits per pixel, S records 8.
        const uint32_t stride = rh(rdram, rec + 0x0C) / rows;
        const bool s_kind = flag == 0x1Fu;
        const auto pixels = !pack ? std::vector<uint8_t>(rh(rdram, rec + 0x0C), 0)
                          : s_kind ? glyph_rows_8bpp(*pack, cp, stride, rows) : glyph_rows(*pack, cp, stride, rows);

        for (size_t i = 0; i < pixels.size(); ++i) wb(rdram, data + (uint32_t)i, pixels[i]);
    }
}

const GlyphPack* pack_for(const Language& lang, const ExtendedFont& f)
{

    const auto it = lang.glyphs.find(f.name);

    return it == lang.glyphs.end() ? nullptr : &it->second;
}

} // namespace

void load_all()
{

    std::call_once(s_loaded, [] {

        for (const auto& d : recomp::mods::get_all_mod_details("rs64"))
        {
            if (!recomp::mods::is_mod_enabled(d.mod_id)) continue;

            const std::string manifest = mod_text(d.mod_id, "lang/language.json");

            if (manifest.empty()) continue;

            std::vector<std::pair<std::string, std::string>> files;

            for (const char* bank : { "front", "game", "voice", "menu" })
            {
                std::string text = mod_text(d.mod_id, (std::string("lang/") + bank + ".json").c_str());

                if (!text.empty()) files.emplace_back(bank, std::move(text));
            }

            std::vector<std::string> errors;
            auto lang = parse_language(manifest, files, errors);

            for (const auto& e : errors)
                fprintf(stderr, "[lang] %s: %s\n", d.mod_id.c_str(), e.c_str());

            if (!lang || find(lang->code)) continue;

            load_glyph_packs(d.mod_id, *lang);

            fprintf(stderr, "[lang] %s: %s (%zu front, %zu game, %zu voice, %zu glyph packs)\n", d.mod_id.c_str(), lang->code.c_str(), lang->front.size(),
                    lang->game.size(), lang->voice.size(), lang->glyphs.size());

            s_languages.push_back(std::move(*lang));
        }
    });
}

const Language* find(const std::string& code)
{

    for (const auto& l : s_languages)
        if (l.code == code) return &l;

    return nullptr;
}

std::vector<std::string> codes()
{

    std::vector<std::string> out(std::begin(kBuiltinCodes), std::end(kBuiltinCodes));

    for (const auto& l : s_languages) out.push_back(l.code);

    return out;
}

const Language* active()
{

    load_all();

    return find(rs64::video::language());
}

bool installed()
{

    load_all();

    return !s_languages.empty();
}

void apply_maps(uint8_t* rdram)
{

    const bool mod_language = active() != nullptr;

    for (const auto& f : s_fonts) apply_map(rdram, f, mod_language);
}

CodePage& code_page()
{
    return s_page;
}

std::string encode_text(const std::string& utf8)
{

    std::lock_guard lock(s_text_mutex);
    const Language* lang = active();

    return encode(s_page, utf8, lang && lang->rtl, &s_pending);
}

void note_original(const std::string& shown, const std::string& original)
{

    std::lock_guard lock(s_text_mutex);

    s_originals[shown] = original;
}

std::string original(const std::string& shown)
{

    std::lock_guard lock(s_text_mutex);
    const auto it = s_originals.find(shown);

    return it == s_originals.end() ? shown : it->second;
}

void flush(uint8_t* rdram)
{

    std::lock_guard lock(s_text_mutex);

    if (s_pending.empty() || !active())
    {
        return;
    }

    write_slots(rdram, s_pending);
    s_pending.clear();
}

void write_slots(uint8_t* rdram, const std::vector<uint8_t>& slots)
{

    const Language* lang = active();

    if (!lang)
    {
        return;
    }

    for (const auto& f : s_fonts)
    {
        for (const uint8_t b : slots)
            if (b >= CodePage::kFirst && b <= CodePage::kLast) write_slot(rdram, f, pack_for(*lang, f), b - CodePage::kFirst);
    }
}

} // namespace rs64::lang

using namespace rs64::lang;

extern "C" void rs64_f5_set_texture_pool_move(uint32_t orig, uint32_t moved, uint32_t size);
extern "C" void rs64_snapshot_add_range(uint32_t lo, uint32_t hi);

// A scene's texture pool (bounds at 0x8011A850/54) moves to host RAM below 0x81000000 (the game takes texture pointers as RAM only for top byte 0x80); RT64 maps the move back. The previous
// pool can still be live while the next loads, so pools alternate between two regions; stock pools are at most 1.28 MB (D_80037560+0x38, D_800CD2B0, D_800B072C), so either region fits.
extern "C" void rs64_lang_texture_pool(uint8_t* rdram)
{

    constexpr uint32_t kPoolStart = 0x8011A850u, kPoolEnd = 0x8011A854u, kMinExtra = 0x40000u;

    // The second region starts past librecomp's cart PI handle (0x80800000) and the patches ELF's data (0x80801000).
    static const std::pair<uint32_t, uint32_t> kRegions[] = { { kTextEnd, 0x81000000u }, { 0x80810000u, 0x80A00000u } };
    static uint32_t s_start[2] = {};
    static int s_last = 1;

    if (!installed()) return;

    const uint32_t start = rw(rdram, kPoolStart), end = rw(rdram, kPoolEnd);
    const uint32_t need = end - start + kMinExtra;

    if (end <= start) return;

    auto fits = [need](int region) { return kRegions[region].first + need <= kRegions[region].second; };
    int r = s_start[0] == start ? 0 : s_start[1] == start ? 1 : 1 - s_last;

    if (!fits(r)) r = 1 - r;

    if (!fits(r) || (r == s_last && s_start[r] != start))
    {
        static bool s_warned = false;

        fprintf(stderr, "[lang] no host room for texture pool %08X-%08X\n", start, end);

        // Only a code mod can enlarge a pool past the stock 1.28 MB. Tell the player once, from another thread so the game thread never waits on the dialog.
        if (!s_warned)
        {
            char msg[512];

            snprintf(msg, sizeof msg, "A mod made a scene's texture pool %.2f MB, more than the %.1f MB / %.1f MB language mods set aside for it.\n\n"
                     "The game keeps running, but this scene may show missing text or textures. If it does, disable the language mod or the mod that enlarges the scene.",
                     (end - start) / 1048576.0, (kRegions[1].second - kRegions[1].first) / 1048576.0, (kRegions[0].second - kRegions[0].first) / 1048576.0);
            std::thread([text = std::string(msg)] { ultramodern::error_handling::message_box(text.c_str()); }).detach();
            s_warned = true;
        }

        return;
    }

    const uint32_t moved = kRegions[r].first;
    const uint32_t size = kRegions[r].second - moved;

    s_start[r] = start;
    s_last = r;
    ww(rdram, kPoolStart, moved);
    ww(rdram, kPoolEnd, moved + size);
    rs64_snapshot_add_range(moved - 0x80000000u, moved - 0x80000000u + size);
    rs64_f5_set_texture_pool_move(start, moved, size);
    fprintf(stderr, "[lang] texture pool %08X-%08X -> %08X-%08X\n", start, end, moved, moved + size);
}

// The texture record table and its arrays (the game just allocated and cleared them) move to host buffers for kRecords records: room for the code page glyphs'
// records beside the scene's own. The game's blocks are kept for destroyTextureLUTs to free.
namespace {

constexpr uint32_t kCount = 0x80128EF0u, kCount2 = 0x80128F04u, kTable = 0x80128F08u, kWords2 = 0x80128EFCu, kWords3 = 0x80128F00u;

struct GameRecordTables {
    bool held = false;
    uint16_t count = 0, count2 = 0;
    uint32_t table = 0, words2 = 0, words3 = 0;
};

GameRecordTables s_game_tables;

} // namespace

extern "C" void rs64_lang_record_tables(uint8_t* rdram)
{

    if (!installed()) return;

    s_game_tables = { true, rh(rdram, kCount), rh(rdram, kCount2), rw(rdram, kTable), rw(rdram, kWords2), rw(rdram, kWords3) };

    if (s_game_tables.count > kRecords || s_game_tables.count2 > kRecords)
    {
        s_game_tables.held = false;
        return;
    }

    for (uint32_t a = kRecordTable; a < kRecordsEnd; ++a) wb(rdram, a, 0);

    ww(rdram, kTable, kRecordTable);
    ww(rdram, kWords2, kRecordWords2);
    ww(rdram, kWords3, kRecordWords3);
    wh(rdram, kCount, (uint16_t)kRecords);
    wh(rdram, kCount2, (uint16_t)kRecords);
}

extern "C" void rs64_lang_record_tables_release(uint8_t* rdram)
{

    if (!s_game_tables.held || rw(rdram, kTable) != kRecordTable) return;

    ww(rdram, kTable, s_game_tables.table);
    ww(rdram, kWords2, s_game_tables.words2);
    ww(rdram, kWords3, s_game_tables.words3);
    wh(rdram, kCount, s_game_tables.count);
    wh(rdram, kCount2, s_game_tables.count2);
    s_game_tables.held = false;
}

// The scene's HMT material slots (registerHmtTextureInTable hands one to each texture; table at 0x8011A444, capacity u16 0x8011A8B4, optional arrays at 0x801163B4 /
// 0x801143AC) move to host buffers: the code page glyphs take slots the scene's own textures (SELECT LEVEL and craft previews) need. freeRenderSubsystemResources gets the game's blocks back.
namespace {

constexpr uint32_t kSlotCount = 0x8011A8B4u, kSlotTable = 0x8011A444u, kSlotArrayA = 0x801163B4u, kSlotArrayB = 0x801143ACu;

struct GameSlotTables {
    bool held = false;
    uint16_t count = 0;
    uint32_t table = 0, array_a = 0, array_b = 0;
};

GameSlotTables s_game_slots;

} // namespace

extern "C" void rs64_lang_slot_tables(uint8_t* rdram)
{

    if (!installed()) return;

    s_game_slots = { true, rh(rdram, kSlotCount), rw(rdram, kSlotTable), rw(rdram, kSlotArrayA), rw(rdram, kSlotArrayB) };

    if (s_game_slots.count > kSlots)
    {
        s_game_slots.held = false;
        return;
    }

    for (uint32_t a = kHostSlotTable; a < kSlotsEnd; ++a) wb(rdram, a, 0);

    ww(rdram, kSlotTable, kHostSlotTable);

    if (s_game_slots.array_a) ww(rdram, kSlotArrayA, kHostSlotArrayA);
    if (s_game_slots.array_b) ww(rdram, kSlotArrayB, kHostSlotArrayB);

    wh(rdram, kSlotCount, (uint16_t)kSlots);
}

extern "C" void rs64_lang_slot_tables_release(uint8_t* rdram)
{

    if (!s_game_slots.held || rw(rdram, kSlotTable) != kHostSlotTable) return;

    ww(rdram, kSlotTable, s_game_slots.table);
    ww(rdram, kSlotArrayA, s_game_slots.array_a);
    ww(rdram, kSlotArrayB, s_game_slots.array_b);
    wh(rdram, kSlotCount, s_game_slots.count);
    s_game_slots.held = false;
}

// Before a font loads: its object moves to a host block with room for a byte per glyph past +0x104, its charset and widths become host copies with the code page bytes
// appended (full cell width, so every glyph buffer fits any glyph), and its count grows to match.
extern "C" void rs64_lang_font_load(uint8_t* rdram, unsigned font_id)
{

    if (!installed()) return;

    for (const unsigned id : { font_id & 0xFFFFu, font_id & 0xFFu })
    {
        if (id >= kFontIds) continue;

        const uint32_t old_obj = rw(rdram, kFontTable + id * 4);
        const uint16_t count = rh(rdram, old_obj);

        const auto seen = std::find_if(s_fonts.begin(), s_fonts.end(), [old_obj](const ExtendedFont& f) { return f.obj == old_obj; });

        // A reload sizes each glyph buffer from its width: slot widths go back to the cell width (write_slot set pack advances).
        if (seen != s_fonts.end())
        {
            const uint32_t widths = rw(rdram, seen->obj + 0x18);

            for (int s = 0; s < CodePage::kSlots; ++s) wb(rdram, widths + slot_glyph(seen->count, s), (uint8_t)rh(rdram, seen->obj + 0x02));

            continue;
        }
        if (count >= kPlaceholderGlyph) continue;

        std::string name;

        for (uint32_t a = rw(rdram, old_obj + 0x10); name.size() < 16 && rb(rdram, a); ++a) name += (char)rb(rdram, a);

        if (!extends_font(name)) continue;

        std::vector<uint8_t> copy(0x200, 0);

        for (uint32_t i = 0; i < 0x104; ++i) copy[i] = rb(rdram, old_obj + i);

        const uint32_t obj = alloc_bytes(rdram, copy);

        if (!obj) continue;

        // Several font ids can share one object (italic35 is ids 0 and 5): every id that pointed at it gets the extended copy.
        for (unsigned k = 0; k < kFontIds; ++k)
            if (rw(rdram, kFontTable + k * 4) == old_obj) ww(rdram, kFontTable + k * 4, obj);

        // Per font object, not per charset: fonts that share a charset (subtitle8 / subtitle12) need their own slot widths.
        const uint32_t charset = rw(rdram, obj + 0x0C);
        auto& tables = s_tables[old_obj];

        if (!tables.first)
        {
            std::vector<uint8_t> chars(count), widths(count);
            const uint32_t widths_at = rw(rdram, obj + 0x18);

            for (uint16_t i = 0; i < count; ++i)
            {
                chars[i] = rb(rdram, charset + i);
                widths[i] = rb(rdram, widths_at + i);
            }

            const FontTables t = extend_font_tables(chars.data(), widths.data(), count, (uint8_t)rh(rdram, obj + 0x02));

            tables = { alloc_bytes(rdram, t.charset), alloc_bytes(rdram, t.widths) };
        }

        ww(rdram, obj + 0x0C, tables.first);
        ww(rdram, obj + 0x18, tables.second);
        wh(rdram, obj, (uint16_t)(count + kExtraGlyphs));
        s_fonts.push_back({ obj, count, tables.first, name });
    }
}

// A font has loaded (fresh glyph records): its map points at the slots, and the slots already in use get their glyphs.
extern "C" void rs64_lang_font_loaded(uint8_t* rdram, unsigned map_font, unsigned glyph_font)
{

    const Language* lang = installed() ? active() : nullptr;

    for (const auto& f : s_fonts)
    {
        if (f.obj != map_font && f.obj != glyph_font) continue;

        apply_map(rdram, f, lang != nullptr);

        for (int s = 0; lang && s < CodePage::kSlots; ++s)
            if (s_page.slot_cp[s]) write_slot(rdram, f, pack_for(*lang, f), s);
    }
}

// loadTxtFile entry: kind 0 is the front or game file (by name) that getGameOrFrontText reads, kind 1 the voice file.
extern "C" void rs64_lang_txt_file(uint8_t* rdram, uint32_t name, int kind)
{

    // A language change (the LANGUAGE page, fonts loaded) gives the loaded fonts their maps and slot glyphs. Only then: a scene change also loads text while the old
    // scene's font maps may already be freed, and writing to them corrupts the heap.
    std::unique_lock lock(s_text_mutex);
    const std::string code = active() ? active()->code : std::string{};

    if (code != s_placed_code)
    {
        s_placed.clear();
        s_text_next = kTextStart;
        s_placed_code = code;
        apply_maps(rdram);

        for (const auto& f : s_fonts)
            for (int s = 0; active() && s < CodePage::kSlots; ++s)
                if (s_page.slot_cp[s]) write_slot(rdram, f, pack_for(*active(), f), s);

        s_pending.clear();
    }

    lock.unlock();

    if (kind != 0 || !name)
    {
        return;
    }

    std::string s;

    for (uint32_t a = name; s.size() < 5 && rb(rdram, a); ++a) s += (char)std::tolower(rb(rdram, a));

    s_bank = s == "front" ? 0 : 1;
}

// A translated string for getGameOrFrontText's text id in the loaded file (the game's own at `game_str`), as a host address; 0 keeps the game's.
extern "C" uint32_t rs64_lang_text(uint8_t* rdram, uint32_t id, uint32_t game_str)
{

    std::lock_guard lock(s_text_mutex);
    static bool s_full_logged = false;
    const Language* lang = active();

    if (!lang)
    {
        return 0;
    }

    const auto& table = s_bank == 0 ? lang->front : lang->game;
    const auto it = table.find(id);

    if (it == table.end())
    {
        return 0;
    }

    Placed& p = s_placed[(uint32_t)s_bank << 24 | id];

    if (p.addr && still_encoded(s_page, p.bytes, p.cps))
    {
        touch(s_page, p.bytes);
        return p.addr;
    }

    if (!p.addr)
    {
        p.cps = display_codepoints(it->second, lang->rtl);

        if (s_text_next + p.cps.size() + 1 > kTextEnd)
        {
            if (!s_full_logged) fprintf(stderr, "[lang] text area full; untranslated strings from here on\n");

            s_full_logged = true;
            s_placed.erase((uint32_t)s_bank << 24 | id);

            return 0;
        }

        p.addr = s_text_next;
        s_text_next += (uint32_t)p.cps.size() + 1;
    }

    p.bytes = encode(s_page, it->second, lang->rtl, &s_pending);

    for (size_t i = 0; i <= p.bytes.size(); ++i) wb(rdram, p.addr + (uint32_t)i, i < p.bytes.size() ? (uint8_t)p.bytes[i] : 0);

    std::string game;

    for (uint32_t a = game_str; game_str && game.size() < 256 && rb(rdram, a); ++a) game += (char)rb(rdram, a);

    s_originals[p.bytes] = game;
    flush(rdram);

    return p.addr;
}

extern "C" void rs64_lang_original(const char* shown, char* out, size_t cap)
{

    const std::string s = original(shown);

    snprintf(out, cap, "%s", s.c_str());
}

// getVoiceText: the id at entry, then (after it decrypted the line into voiceTxtString) the translation in its place.
extern "C" void rs64_lang_voice_id(uint32_t id)
{
    s_voice_id = id;
}

extern "C" void rs64_lang_voice_text(uint8_t* rdram)
{

    constexpr size_t kVoiceTextSize = 0x190;
    const Language* lang = active();

    if (!lang)
    {
        return;
    }

    const auto it = lang->voice.find(s_voice_id);

    if (it == lang->voice.end())
    {
        return;
    }

    std::string bytes = encode_text(it->second);
    const uint32_t at = rs64::ls::relocated(0x80138E90u);

    bytes.resize(std::min(bytes.size(), kVoiceTextSize - 1));

    for (size_t i = 0; i <= bytes.size(); ++i) wb(rdram, at + (uint32_t)i, i < bytes.size() ? (uint8_t)bytes[i] : 0);

    flush(rdram);
}
