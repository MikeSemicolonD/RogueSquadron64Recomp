// Mod asset overrides: files under <mod>/assets/ appended past the ROM end and patched into the game's RAM manifests.

#include "game/mod_content/asset_overrides.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <atomic>
#include <span>
#include <variant>
#include <vector>


#include "librecomp/game.hpp"
#include "librecomp/mods.hpp"
#include "librecomp/patcher.hpp"
#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "rs64_toml_hooks.h"
#include "util/debug_logs.h"
#include "game/game_state.h"
#include "game/mod_content/mod_levels.h"
#include "game/mod_content/terrain_paint.h"
#include "game/lang/lang.h"
#include "game/mod_content/level_rules.h"
#include "host_api/host_api.h"
#include "util/mips_call.h"
#include "util/rdram_words.h"
#include "util/host_ram_map.h"

using recomp::dbg::env_str;
using recomp::dbg::env_on;
using recomp::dbg::env_int;
using namespace rs64::mips;
using rs64::assets::kFromMap;
using rs64::assets::read_from_map;

// Files under <mod>/assets/<manifest path> (e.g. assets/pl_crafts/xwing_HOB) are appended past the ROM end; the game's RAM manifest points at them as raw entries.
// shortcut: folder mods only (not zipped), later mods win on the same path; read zips through the mod file handle if packaged asset mods appear.
namespace {
using AssetOverride = rs64::assets::Override;
std::unordered_map<std::string, AssetOverride> s_asset_overrides;
// Cart offsets of the silent 1-10 s voice streams level-rule messages play.
uint32_t s_silence_rom[10] = {};

// Craft HOBs with recoloured exhaust, one per exhaust-color option (index 0 = default, unused).
std::unordered_map<std::string, std::vector<AssetOverride>> s_exhaust_variants;
constexpr const char* kExhaustMod = "exhaust-color";
constexpr uint32_t kExhaustTints[] = { 0, 0xFF2000, 0xFF8000, 0xFFE000, 0x20FF20, 0x20FFFF, 0x2060FF, 0xA040FF, 0xFF40C0, 0xFFFFFF };

uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

// Data-blob manifest walk, as tools/web/rs64-explorer openRom: path (without the segment name) -> decompressed bytes.
std::unordered_map<std::string, std::vector<uint8_t>> rom_assets(std::span<const uint8_t> rom, const std::function<bool(const std::string&)>& want)
{

    std::unordered_map<std::string, std::vector<uint8_t>> out;
    constexpr uint32_t kBlobStart = 0x144340, kBlobData = kBlobStart + 0x40;

    if (rom.size() < kBlobData) return out;

    const uint32_t base = kBlobData + be32(&rom[kBlobStart + 0x1C]);
    const uint32_t manifest = base + be32(&rom[base]);
    const uint32_t count = (((uint32_t)rom[base + 6] << 8) | rom[base + 7]) / 0x20;
    std::vector<std::pair<std::string, uint32_t>> dirs;

    for (uint32_t i = 0; i < count && manifest + (i + 1) * 0x20 <= rom.size(); i++)
    {
        while (!dirs.empty() && i >= dirs.back().second) dirs.pop_back();

        const uint8_t* e = &rom[manifest + i * 0x20];
        const std::string name(reinterpret_cast<const char*>(e + 0x10), strnlen(reinterpret_cast<const char*>(e + 0x10), 16));
        std::string path;

        for (const auto& d : dirs) path += d.first + "/";

        path += name;

        if (e[0x0C] & 0x80)
        {
            dirs.emplace_back(name, i + ((((uint32_t)e[0x0E] << 8) | e[0x0F]) >> 5));
            continue;
        }

        if (!want(path)) continue;

        const uint32_t off = base + be32(e), size = be32(e + 4), csize = be32(e + 8);
        const uint32_t stored = csize == 0xFFFFFFFFu ? size : csize;

        if (off + stored > rom.size()) continue;

        std::vector<uint8_t> data(size);

        if (csize == 0xFFFFFFFFu)
        {
            std::memcpy(data.data(), &rom[off], size);
        } else if (tinfl_decompress_mem_to_mem(data.data(), size, &rom[off], stored, TINFL_FLAG_PARSE_ZLIB_HEADER) != size)
        {
            continue;
        }

        out.emplace(path, std::move(data));
    }

    return out;
}

// Tint the vertex colours of a HOB's eng_* parts (layout as tools/web/rs64-explorer parseHOB/parseFaces). The white core stays white: only the coloured part (max - min) takes the tint.
bool tint_exhaust(std::vector<uint8_t>& hob, uint32_t tint) {

    const size_t n = hob.size();
    auto ok = [&](size_t o, size_t len) { return o + len <= n; };

    if (!ok(0, 2)) return false;

    bool changed = false;
    const uint32_t objs = ((uint32_t)hob[0] << 8) | hob[1];

    for (uint32_t oi = 0; oi < objs && ok(8 + oi * 0x74, 0x74); oi++)
    {
        for (uint32_t p = be32(&hob[8 + oi * 0x74 + 0x2C]); p && ok(p, 12) && be32(&hob[p]); p += 12)
        {
            if (std::memcmp(&hob[p + 4], "eng_", 4) != 0) continue;

            const uint32_t part = be32(&hob[p]);

            if (!ok(part, 0x24)) continue;

            const uint32_t fg = be32(&hob[part + 0x20]);

            if (!ok(fg, 16)) continue;

            uint32_t o = be32(&hob[fg + 8]);

            for (uint32_t f = 0, nf = be32(&hob[fg + 12]); f < nf && ok(o, 20); f++)
            {
                const uint32_t flags = be32(&hob[o]), size = (hob[o + 4] & 0x3Fu) * 4u;
                const uint32_t nv = (flags & 8) ? 4 : 3;
                uint32_t c = o + 20 + ((be32(&hob[o + 4]) & 0x2000) ? 8 : 0);
                const uint32_t nc = (flags & 0x20) ? ((flags & 0x10) ? nv : 1) : 0;

                for (uint32_t k = 0; k < nc && ok(c, 4); k++, c += 4)
                {
                    uint8_t* rgb = &hob[c];
                    const int lo = std::min({ rgb[0], rgb[1], rgb[2] }), hi = std::max({ rgb[0], rgb[1], rgb[2] });

                    for (int ch = 0; ch < 3; ch++) rgb[ch] = (uint8_t)(lo + (hi - lo) * ((tint >> (16 - ch * 8)) & 0xFF) / 255);

                    changed = true;
                }

                if (!size) break;

                o += size;
            }
        }
    }

    return changed;
}

uint32_t exhaust_choice()
{

    if (s_exhaust_variants.empty() || !recomp::mods::is_mod_enabled(kExhaustMod)) return 0;

    const recomp::mods::ConfigValueVariant v = recomp::mods::get_mod_config_value(kExhaustMod, "color");
    const uint32_t* idx = std::get_if<uint32_t>(&v);

    return idx ? *idx : 0;
}

// Manifests grown with mod-added files live in host RAM above the game's 8 MB, one 64 KB block per gManifestTable slot: {copy, game's buffer}.
constexpr uint32_t kManifestCopies = RS64_HR_MANIFESTS, kManifestCopySize = 0x10000u;
std::pair<uint32_t, uint32_t> s_manifest_copies[4];
static_assert(kManifestCopies + std::size(s_manifest_copies) * kManifestCopySize <= RS64_HR_MANIFESTS_END, "manifest copies outgrew their host RAM range");

// A manifest entry (0x20 bytes, 8 words): +0 offset in block, +4 size, +8 stored size (-1 raw), +0xC flags (0x80 directory) and, for a
// directory, its own entry plus everything inside in bytes at +0xE; +0x10 name[16].
struct ManifestNode {
    uint32_t w[8] = {};
    std::string name;
    std::vector<ManifestNode> kids;
    bool dir() const { return (w[3] & 0x80000000u) != 0; }
};

// Rewrites slot idx's manifest as a copy with the mod-added files inside their directories (missing directories created, every enclosing
// directory's size grown), then points the slot at the copy.
void add_manifest_files(uint8_t* rdram, uint32_t idx, uint32_t block_rom, const std::vector<std::pair<std::string, AssetOverride>>& files)
{

    const uint32_t slot = 0x80110A80u + idx * 0x50u, manifest = rw(rdram, slot + 0x40), count = rw(rdram, slot + 0x48);
    std::vector<ManifestNode> top;
    std::function<ManifestNode(uint32_t&)> read = [&](uint32_t& i)
    {

        ManifestNode n;

        for (int k = 0; k < 8; k++) n.w[k] = rw(rdram, manifest + i * 0x20u + k * 4u);

        n.name = rstr(rdram, manifest + i * 0x20u + 0x10, 16);

        const uint32_t end = i + ((n.w[3] & 0xFFFFu) >> 5);
        i++;

        if (n.dir()) {
            while (i < end && i < count) n.kids.push_back(read(i));
        }

        return n;
    };

    for (uint32_t i = 0; i < count;) top.push_back(read(i));

    auto set_name = [](ManifestNode& n) {

        char buf[16] = {};
        std::memcpy(buf, n.name.data(), std::min<size_t>(n.name.size(), 15));

        for (int k = 0; k < 4; k++) n.w[4 + k] = be32(reinterpret_cast<const uint8_t*>(buf) + k * 4);
    };

    for (const auto& [path, ov] : files)
    {
        std::vector<ManifestNode>* level = &top;
        size_t start = 0;

        for (size_t slash; (slash = path.find('/', start)) != std::string::npos; start = slash + 1)
        {
            const std::string part = path.substr(start, slash - start);
            auto it = std::find_if(level->begin(), level->end(), [&](const ManifestNode& n) { return n.dir() && n.name == part; });

            if (it == level->end()) {
                ManifestNode d;
                d.name = part;
                d.w[3] = 0x80000000u;
                set_name(d);
                level->push_back(std::move(d));
                it = level->end() - 1;
            }

            level = &it->kids;
        }

        ManifestNode f;
        f.name = path.substr(start);
        f.w[0] = ov.rom_off - block_rom;
        f.w[1] = ov.size;
        f.w[2] = 0xFFFFFFFFu;
        set_name(f);
        level->push_back(std::move(f));

        fprintf(stderr, "[assets] slot %u: adds %s -> rom 0x%X\n", idx, path.c_str(), ov.rom_off);
    }

    std::vector<std::array<uint32_t, 8>> out;
    size_t largest_dir = 0;

    std::function<void(ManifestNode&)> emit = [&](ManifestNode& n) {

        const size_t at = out.size();
        out.emplace_back();

        for (auto& k : n.kids) emit(k);

        if (n.dir())
        {
            largest_dir = std::max(largest_dir, (out.size() - at) * 0x20u);
            n.w[3] = (n.w[3] & 0xFFFF0000u) | (uint32_t)((out.size() - at) * 0x20u);
        }

        std::copy(std::begin(n.w), std::end(n.w), out[at].begin());
    };

    for (auto& n : top) emit(n);

    // A directory's byte size is 16 bits.
    if (out.size() * 0x20u > kManifestCopySize || largest_dir > 0xFFFFu)
    {
        fprintf(stderr, "[assets] slot %u: %zu manifest entries do not fit the 64 KB copy; mod-added files skipped\n", idx, out.size());

        return;
    }

    const uint32_t copy = kManifestCopies + idx * kManifestCopySize;

    for (size_t i = 0; i < out.size(); i++)
    {
        for (int k = 0; k < 8; k++) ww(rdram, copy + (uint32_t)i * 0x20u + k * 4u, out[i][k]);
    }

    if (manifest != copy) s_manifest_copies[idx] = { copy, manifest };

    ww(rdram, slot + 0x40, copy);
    ww(rdram, slot + 0x48, (uint32_t)out.size());
}

}

std::vector<std::pair<std::string, std::string>> rs64::assets::read_from_map(const std::filesystem::path& assets)
{

    std::vector<std::pair<std::string, std::string>> out;
    std::ifstream in(assets / kFromMap);

    if (!in) return out;

    const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);

    if (!j.is_object()) {
        fprintf(stderr, "[assets] %s: not a JSON object\n", (assets / kFromMap).generic_string().c_str());

        return out;
    }

    for (const auto& [k, v] : j.items()) {
        if (v.is_string()) out.emplace_back(k, v.get<std::string>());
    }

    return out;
}

// A file named <path>.bps is a BPS patch applied to the player's own copy of <path>, so mods built from game assets ship no game data.
extern "C" void rs64_asset_overrides_init()
{

    std::vector<uint8_t> rom;

    auto append = [&rom](const std::vector<uint8_t>& data) {

        if (rom.empty()) rom.assign(recomp::get_rom().begin(), recomp::get_rom().end());

        rom.resize((rom.size() + 15) & ~size_t(15));
        const AssetOverride ov{ (uint32_t)rom.size(), (uint32_t)data.size() };
        rom.insert(rom.end(), data.begin(), data.end());

        return ov;
    };

    std::unordered_map<std::string, std::pair<std::string, std::vector<uint8_t>>> patches;
    std::unordered_map<std::string, std::pair<std::string, std::string>> derived;

    for (const auto& d : recomp::mods::get_all_mod_details("rs64"))
    {
        if (!recomp::mods::is_mod_enabled(d.mod_id)) continue;

        const std::filesystem::path dir = recomp::mods::get_mod_filename(d.mod_id) / "assets";
        std::error_code ec;

        if (!std::filesystem::is_directory(dir, ec)) continue;

        for (const auto& f : std::filesystem::recursive_directory_iterator(dir, ec))
        {
            if (!f.is_regular_file()) continue;

            std::ifstream in(f.path(), std::ios::binary);
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

            if (data.empty()) continue;

            std::string key = std::filesystem::relative(f.path(), dir).generic_string();

            if (key == kFromMap) continue;

            // <path>.from names an existing asset that <path> is a copy of (patched by <path>.bps when present): a new asset made from game data without shipping it.
            if (f.path().extension() == ".from")
            {
                key.resize(key.size() - 5);
                std::string src(data.begin(), data.end());
                src.erase(std::remove_if(src.begin(), src.end(), [](char c) { return c == '\r' || c == '\n' || c == ' ' || c == '\t'; }), src.end());
                s_asset_overrides.erase(key);
                derived[key] = { d.mod_id, src };
                continue;
            }

            if (f.path().extension() == ".bps") {
                key.resize(key.size() - 4);
                s_asset_overrides.erase(key);
                patches[key] = { d.mod_id, std::move(data) };
                continue;
            }

            patches.erase(key);
            s_asset_overrides[key] = append(data);

            fprintf(stderr, "[assets] %s overrides %s (%zu bytes)\n", d.mod_id.c_str(), key.c_str(), data.size());
        }

        for (const auto& [key, src] : read_from_map(dir))
        {
            s_asset_overrides.erase(key);
            derived[key] = { d.mod_id, src };
        }
    }

    if (!derived.empty())
    {
        const auto sources = rom_assets(recomp::get_rom(), [&](const std::string& p)
        {
            return std::any_of(derived.begin(), derived.end(), [&](const auto& kv) { return kv.second.second == p; });
        });

        for (const auto& [path, from] : derived)
        {
            const auto& [mod_id, src_path] = from;
            const auto src = sources.find(src_path);

            if (src == sources.end())
            {
                fprintf(stderr, "[assets] %s: %s is made from %s, which the game does not have\n", mod_id.c_str(), path.c_str(), src_path.c_str());
                continue;
            }

            std::vector<uint8_t> out = src->second;

            if (const auto p = patches.find(path); p != patches.end())
            {
                std::vector<uint8_t> patched;

                if (recomp::patcher::patch_rom(src->second, p->second.second, patched) != recomp::patcher::PatcherResult::Success || patched.empty())
                {
                    fprintf(stderr, "[assets] %s: patch for %s does not apply to %s\n", mod_id.c_str(), path.c_str(), src_path.c_str());
                    patches.erase(p);
                    continue;
                }

                out = std::move(patched);
                patches.erase(p);
            }

            s_asset_overrides[path] = append(out);

            fprintf(stderr, "[assets] %s adds %s from %s (%zu bytes)\n", mod_id.c_str(), path.c_str(), src_path.c_str(), out.size());
        }
    }

    if (!patches.empty())
    {
        for (const auto& [path, src] : rom_assets(recomp::get_rom(), [&](const std::string& p) { return patches.count(p) != 0; }))
        {
            const auto& [mod_id, bps] = patches[path];
            std::vector<uint8_t> out;
            const auto res = recomp::patcher::patch_rom(src, bps, out);

            if (res != recomp::patcher::PatcherResult::Success || out.empty())
            {
                fprintf(stderr, "[assets] %s: patch for %s does not apply (%s)\n", mod_id.c_str(), path.c_str(), res == recomp::patcher::PatcherResult::WrongRom ? "made from a different asset" : "invalid patch");
                continue;
            }

            s_asset_overrides[path] = append(out);

            fprintf(stderr, "[assets] %s patches %s (%zu bytes)\n", mod_id.c_str(), path.c_str(), out.size());
        }
    }

    // A crawl's own files and a level's picture are also served inside the directories where their lookups find them.
    std::vector<std::pair<std::string, AssetOverride>> served;

    for (const auto& [path, ov] : s_asset_overrides)
    {
        if (const std::string s = rs64levels::crawlServedPath(path); !s.empty()) served.emplace_back(s, ov);

        if (const std::string s = rs64levels::previewServedPath(path); !s.empty()) served.emplace_back(s, ov);
    }

    for (const auto& [path, ov] : served) s_asset_overrides[path] = ov;

    // Every colour variant is built up front so a change in the mod's config applies at the next level load.
    if (recomp::mods::is_mod_enabled(kExhaustMod))
    {
        const auto is_craft = [](const std::string& p) { return p.rfind("pl_crafts/", 0) == 0 && p.size() > 4 && p.compare(p.size() - 4, 4, "_HOB") == 0; };

        for (auto& [path, hob] : rom_assets(recomp::get_rom(), is_craft)) {
            auto& variants = s_exhaust_variants[path];
            variants.resize(std::size(kExhaustTints));

            for (size_t t = 1; t < std::size(kExhaustTints); t++) {
                std::vector<uint8_t> data = hob;

                if (!tint_exhaust(data, kExhaustTints[t])) break;

                variants[t] = append(data);
            }

            if (!variants[1].size) s_exhaust_variants.erase(path);
        }

        fprintf(stderr, "[assets] exhaust colours for %zu craft models\n", s_exhaust_variants.size());
    }

    // Language mods: extended fonts get blank cells for their code page glyphs (rs64_lang_font_load raises the glyph count to match).
    if (rs64::lang::installed())
    {
        const auto& r = recomp::get_rom();
        const auto rom_word = [&r](uint32_t vram) { return be32(&r[vram - 0x80000400u + 0x1000u]); };
        std::unordered_map<std::string, uint16_t> cell_h;

        for (uint32_t id = 0; id < 12; id++)
        {
            const uint32_t obj = rom_word(0x8009FC10u + id * 4);
            const uint32_t name = rom_word(obj + 0x10) - 0x80000400u + 0x1000u;
            const std::string font(reinterpret_cast<const char*>(&r[name]), strnlen(reinterpret_cast<const char*>(&r[name]), 16));

            if (rs64::lang::extends_font(font)) cell_h["fonts/" + font] = (uint16_t)(rom_word(obj + 4) >> 16);
        }

        for (const auto& [path, data] : rom_assets(r, [&cell_h](const std::string& p) { return cell_h.count(p) != 0; }))
        {
            const std::vector<uint8_t> out = rs64::lang::extend_font_strip(data, cell_h[path], rs64::lang::kExtraGlyphs);

            if (out.empty()) continue;
            if (s_asset_overrides.count(path)) fprintf(stderr, "[lang] language glyphs replace a mod's %s\n", path.c_str());

            s_asset_overrides[path] = append(out);
        }

        fprintf(stderr, "[lang] %zu fonts extended for language mods\n", cell_h.size());
    }

    // Silent voice streams of 1-10 s for level-rule messages (a subtitle stays up while its line plays).
    for (int s = 1; s <= 10; s++) s_silence_rom[s - 1] = append(rs64rules::silentMort(s)).rom_off;

    if (!rom.empty()) {
        rom.shrink_to_fit();
        recomp::set_rom_contents(std::move(rom));
    }
}

// findManifestEntryByName exit: slot idx of gManifestTable (0x80110A80, 0x50 each) has just had its manifest loaded.
extern "C" void rs64_asset_overrides_patch(uint8_t* rdram, uint32_t idx)
{

    if ((s_asset_overrides.empty() && s_exhaust_variants.empty()) || idx >= 4) return;

    const uint32_t exhaust = exhaust_choice();
    const uint32_t slot = 0x80110A80u + idx * 0x50u;

    // Loaded slots have +0x4E = 1; findManifestEntryByName also reaches this exit (from 0x800048D0) with a free slot.
    if (rb(rdram, slot + 0x4E) != 1) return;

    const uint32_t manifest = rw(rdram, slot + 0x40);
    const uint32_t block_rom = rw(rdram, slot + 0x44) & 0x0FFFFFFFu;
    const uint32_t count = rw(rdram, slot + 0x48);

    if ((manifest & 0xFF800000u) != 0x80000000u) return;

    std::vector<std::pair<std::string, uint32_t>> dirs;
    std::unordered_set<std::string> seen, top_dirs;

    for (uint32_t i = 0; i < count; i++)
    {
        while (!dirs.empty() && i >= dirs.back().second) dirs.pop_back();

        const uint32_t e = manifest + i * 0x20u;
        char name[17] = {};

        for (int c = 0; c < 16; c++) name[c] = (char)rb(rdram, e + 0x10 + c);

        std::string path;

        for (const auto& d : dirs) path += d.first + "/";

        path += name;
        const uint32_t w3 = rw(rdram, e + 0x0C);

        if (w3 & 0x80000000u) {
            if (dirs.empty()) top_dirs.insert(name);

            dirs.emplace_back(name, i + ((w3 & 0xFFFFu) >> 5));
            continue;
        }

        seen.insert(path);
        AssetOverride ov{};

        if (const auto it = s_asset_overrides.find(path); it != s_asset_overrides.end())
        {
            ov = it->second;
        } else if (const auto ex = s_exhaust_variants.find(path); exhaust && ex != s_exhaust_variants.end() && exhaust < ex->second.size())
        {
            ov = ex->second[exhaust];
        }

        if (!ov.size) continue;

        ww(rdram, e + 0x00, ov.rom_off - block_rom);
        ww(rdram, e + 0x04, ov.size);
        ww(rdram, e + 0x08, 0xFFFFFFFFu);
        ww(rdram, e + 0x0C, w3 & 0xFFFFu);

        fprintf(stderr, "[assets] slot %u: %s -> rom 0x%X\n", idx, path.c_str(), ov.rom_off);
    }

    std::vector<std::pair<std::string, AssetOverride>> added;

    for (const auto& [path, ov] : s_asset_overrides)
    {
        const std::string stored = rs64levels::manifestPath(path);

        if (!seen.count(stored) && top_dirs.count(stored.substr(0, stored.find('/')))) added.emplace_back(stored, ov);
    }

    if (!added.empty()) add_manifest_files(rdram, idx, block_rom, added);
}

// rs_free of a manifest copied to host RAM frees the game's own buffer instead (freeManifestSegmentAssets 0x80004A0C / 0x80004A80).
extern "C" uint32_t rs64_asset_manifest_to_free(uint32_t ptr) {

    for (auto& [copy, orig] : s_manifest_copies) {
        if (copy && ptr == copy) {
            copy = 0;

            return orig;
        }
    }

    return ptr;
}

// Terrain textures for RT64's whole-map pass, built once per level: level/tex and lv_<level, base 36>_TEX (a mod's copy when it replaces one) with the level's Soft paint textures (lv_<level>_PAINT) merged in as extra bank entries.
// shortcut: mod-level aliases are not followed (Soft paint is stock levels only); follow them if mod levels get painted terrain.
extern "C" int rs64_terrain_textures(uint32_t level, const uint8_t** bank, uint32_t* bank_size, const uint8_t** ids, uint32_t* ids_size)
{

    static std::once_flag s_once;
    static std::unordered_map<std::string, std::vector<uint8_t>> s_files;
    static std::unordered_map<uint32_t, std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> s_levels;
    static std::mutex s_mutex;

    std::call_once(s_once, [] {
        s_files = rom_assets(recomp::get_rom(), [](const std::string& p) { return p == "level/tex" || (p.rfind("level/lv_", 0) == 0 && p.size() > 4 && p.compare(p.size() - 4, 4, "_TEX") == 0); });
    });

    static constexpr char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyz";

    if (level >= 36)
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_levels.find(level);

    if (it == s_levels.end())
    {
        const std::string lv = std::string("level/lv_") + kDigits[level];

        const auto file = [](const std::string& path) {

            std::vector<uint8_t> out;

            if (const AssetOverride* o = rs64::assets::find(path))
            {
                const auto& rom = recomp::get_rom();

                if ((size_t)o->rom_off + o->size <= rom.size()) out.assign(rom.begin() + o->rom_off, rom.begin() + o->rom_off + o->size);
            } else if (const auto s = s_files.find(path); s != s_files.end())
            {
                out = s->second;
            }

            return out;
        };

        const std::vector<uint8_t> b = file("level/tex");
        const std::vector<uint8_t> t = file(lv + "_TEX");
        const std::vector<uint8_t> p = file(lv + "_PAINT");
        std::pair<std::vector<uint8_t>, std::vector<uint8_t>> merged;

        if (b.empty() || t.empty() || !rs64::terrain_paint::merge_bank(b.data(), (uint32_t)b.size(), t.data(), (uint32_t)t.size(), p.data(), (uint32_t)p.size(), merged.first, merged.second))
        {
            fprintf(stderr, "[assets] %s: no terrain textures for the whole-map pass\n", lv.c_str());
        }

        it = s_levels.emplace(level, std::move(merged)).first;
    }

    if (it->second.first.empty())
    {
        return 0;
    }

    *bank = it->second.first.data();
    *bank_size = (uint32_t)it->second.first.size();
    *ids = it->second.second.data();
    *ids_size = (uint32_t)it->second.second.size();

    return 1;
}

// Soft paint on the game's own terrain: the level's lv_<id>_PAINT is opened next to level/tex and the texture worker reads marked _TEX entries from it.
namespace {
std::string s_paint_dir;
std::atomic<uint32_t> s_paint_handle{ 0 };
std::atomic<uint32_t> s_paint_count{ 0 };
constexpr uint32_t kTexService = 0x80138920u;
constexpr uint32_t kTexSegment = 0x80137CFCu;
constexpr uint32_t kFindAsset = 0x80004AACu;
constexpr uint32_t kCloseAsset = 0x80004C70u;
}

// load_level_hmp entry: dir is the level's folder ("level/lv_1"), the prefix of its _TEX and _PAINT.
extern "C" void rs64_paint_level_dir(uint8_t* rdram, uint32_t dir)
{

    s_paint_dir.clear();

    for (uint32_t i = 0; i < 48; i++)
    {
        const char c = (char)rb(rdram, dir + i);

        if (!c)
        {
            break;
        }

        s_paint_dir += c;
    }
}

// loadLevelTextureCache, right after it opens level/tex: opens the level's painted textures the same way when a mod has them.
extern "C" void rs64_paint_open(uint8_t* rdram, recomp_context* ctx)
{

    s_paint_handle = 0;

    const std::string name = s_paint_dir + "_PAINT";
    const AssetOverride* o = rs64::assets::find(name);

    if (!o || name.size() >= RS64_HR_PAINT_NAME_END - RS64_HR_PAINT_NAME)
    {
        return;
    }

    for (size_t i = 0; i <= name.size(); i++)
    {
        wb(rdram, RS64_HR_PAINT_NAME + (uint32_t)i, i < name.size() ? (uint8_t)name[i] : 0);
    }

    const uint32_t handle = rs64::mips::mips_call(rdram, ctx, kFindAsset, { rw(rdram, kTexService), 0u, rw(rdram, kTexSegment), RS64_HR_PAINT_NAME }, { 0u }).v0;

    if (handle == 0 || handle == 0xFFFFFFFFu)
    {
        fprintf(stderr, "[assets] %s: the game could not open it\n", name.c_str());

        return;
    }

    s_paint_count = o->size / rs64::terrain_paint::kEntryBytes;
    s_paint_handle = handle;

    fprintf(stderr, "[assets] %s: %u painted textures\n", name.c_str(), s_paint_count.load());
}

// teardownGridLayerWorker entry: closes the painted textures with level/tex.
extern "C" void rs64_paint_close(uint8_t* rdram, recomp_context* ctx)
{

    const uint32_t handle = s_paint_handle.exchange(0);

    if (handle)
    {
        rs64::mips::mips_call(rdram, ctx, kCloseAsset, { rw(rdram, kTexService), handle });
    }
}

// The texture worker's read of _TEX field `field`: 0 = a level/tex entry, 1 = painted entry `index` of file `handle`, -1 = painted but not loadable.
extern "C" int rs64_paint_dma(uint32_t field, uint32_t* handle, uint32_t* index)
{

    if (!rs64::terrain_paint::is_painted(field))
    {
        return 0;
    }

    *handle = s_paint_handle;
    *index = field & rs64::terrain_paint::kIndexMask;

    return (*handle && *index < s_paint_count) ? 1 : -1;
}

const rs64::assets::Override* rs64::assets::find(const std::string& path)
{

    const auto it = s_asset_overrides.find(path);

    return it == s_asset_overrides.end() ? nullptr : &it->second;
}

uint32_t rs64::assets::silence_rom(int seconds) {
    return (seconds >= 1 && seconds <= 10) ? s_silence_rom[seconds - 1] : 0;
}
