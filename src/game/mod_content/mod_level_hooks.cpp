// Mod level content: mid-mission camera shots, mod levels and their crawls, medals, objectives, music and level rules.

#include "game/mod_content/asset_overrides.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <vector>


#include "librecomp/game.hpp"
#include "librecomp/mods.hpp"
#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "rs64_toml_hooks.h"
#include "util/debug_logs.h"
#include "main/cli.h"
#include "game/game_state.h"
#include "game/mod_content/camera_shots.h"
#include "game/mod_content/mod_levels.h"
#include "game/mod_content/level_rules.h"
#include "lockstep/lockstep_core.h"
#include "rs64/camera_shots.h"
#include "host_api/host_api.h"
#include <deque>
#include "util/mips_call.h"
#include "util/rdram_words.h"

using recomp::dbg::env_str;
using recomp::dbg::env_on;
using recomp::dbg::env_int;
using namespace rs64::mips;

// ---- Mid-mission camera shots: mods link a level trigger to an in-level camera item in camera_shots.txt ----
namespace {
std::vector<rs64shots::Link> s_shot_links;
rs64shots::Queue s_shot_queue;
uint32_t s_shot_frame = 0;

// Shots queued through the camera.shots service (another player's, in co-op); they never call the listener back.
std::deque<rs64shots::Shot> s_remote_shots;
rs64_shot_fn s_shot_listener = nullptr;
void* s_shot_listener_user = nullptr;

// Host RAM for the camera name the game's shot starter reads (above the manifest copies).
constexpr uint32_t kShotName = 0x80C40000u;
}

extern "C" const char* rs64_mod_levels_current_folder();

extern "C" void rs64_camera_shots_init() {

    for (const auto& d : recomp::mods::get_all_mod_details("rs64"))
    {
        if (!recomp::mods::is_mod_enabled(d.mod_id)) continue;

        std::ifstream in(recomp::mods::get_mod_filename(d.mod_id) / "camera_shots.txt", std::ios::binary);

        if (!in) continue;

        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const size_t before = s_shot_links.size();

        for (const auto& old : rs64shots::addLinks(s_shot_links, text))
        {
            fprintf(stderr, "[shots] %s: overrides the earlier link %s %s -> %s\n", d.mod_id.c_str(), old.level.c_str(), old.trigger.c_str(), old.camera.c_str());
        }

        fprintf(stderr, "[shots] %s: camera_shots.txt read (%zu links in all, %zu new)\n", d.mod_id.c_str(), s_shot_links.size(), s_shot_links.size() - before);
    }
}

extern "C" void rs64_camera_shots_mission_start(uint8_t* rdram)
{

    s_remote_shots.clear();

    if (s_shot_links.empty()) return;

    static const char* digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    const uint32_t level = rw(rdram, 0x80130B70u);
    std::string id;

    for (uint32_t v = level; ; v /= 36) {
        id.insert(id.begin(), digits[v % 36]);

        if (v < 36) break;
    }

    // A mod level (playing as its template) has its own links, under its folder name.
    const char* mod_folder = rs64_mod_levels_current_folder();
    s_shot_queue.reset(mod_folder ? std::string(mod_folder) : "lv_" + id);
    s_shot_frame = 0;
}

extern "C" void rs64_camera_shots_trigger(uint8_t* rdram, uint32_t item, uint32_t phase)
{

    if (s_shot_links.empty() || phase != 1 || (item & 0xFF800000u) != 0x80000000u) return;

    // Like the function itself, only subtypes 0x18 / 0x19 are trigger items. Loading turns the file's name offset (+0x0C) into a pointer.
    const uint32_t sub = rw(rdram, item) >> 16, name = rw(rdram, item + 0x0C);

    if ((sub != 0x18 && sub != 0x19) || (name & 0xFF800000u) != 0x80000000u) return;

    const std::string trigger = rstr(rdram, name, 255);

    // Co-op: the multiplayer mod raises mp_trigger_for_peer while the host applies the other player's trigger.
    const bool for_peer = rs64::host::flag("mp_trigger_for_peer") != 0;

    if (s_shot_queue.fired(s_shot_links, trigger, for_peer)) fprintf(stderr, "[shots] %s: %s fired%s (mission frame %u)\n", s_shot_queue.level().c_str(), trigger.c_str(), for_peer ? " by the other player" : "", s_shot_frame);
}

// The DAT item named name (getLevelDatItemByName 0x80047B48, the name written to kShotName), or 0.
static uint32_t level_object(uint8_t* rdram, recomp_context* ctx, const std::string& name)
{

    const std::string n = name.substr(0, 255);

    wstr(rdram, kShotName, n);

    const uint32_t item = rs64::mips::mips_call(rdram, ctx, 0x80047B48u, { kShotName }).v0;

    return (item & 0xFF800000u) == 0x80000000u ? item : 0;
}

// Destroys the level object named name as a lethal hit would; tag starts each log line.
static void destroy_level_object(uint8_t* rdram, recomp_context* ctx, const std::string& name, const std::string& tag)
{

    const uint32_t item = level_object(rdram, ctx, name);

    if (!item)
    {
        fprintf(stderr, "%s: no object %s; skipped\n", tag.c_str(), name.c_str());
        return;
    }

    const uint32_t slot = rw(rdram, item + 4) & 0xFFFFu, table = rw(rdram, 0x80130BB0u);
    const int32_t health = (int32_t)rw(rdram, item + 0x6C + (rw(rdram, 0x80137CE4u) & 3u) * 4);

    // The slot's NPC context (table + slot * 8) must name the same slot back (+0x16), as the co-op damage sync checks.
    const uint32_t npc = slot < 0x800u && table >= 0x80000000u && table < 0x80800000u ? rw(rdram, table + slot * 8) : 0;
    const bool consistent = npc >= 0x80000000u && npc < 0x807FFFC0u && (rw(rdram, npc + 0x14) & 0xFFFFu) == slot;
    const auto kind = rs64shots::destroyKind(health, slot, consistent, (rw(rdram, item + 0x4C) & 0x80000000u) != 0);

    if (kind == rs64shots::Destroy::Skip)
    {
        fprintf(stderr, "%s: %s is already destroyed or mid-teardown; skipped\n", tag.c_str(), name.c_str());
        return;
    }

    if (kind == rs64shots::Destroy::CutHealth)
    {
        // Not loaded (far from the player): dealDamagetoDatItem only cuts its stored health, so it streams in as rubble.
        rs64::mips::mips_call(rdram, ctx, 0x800C7390u, { kShotName, (uint32_t)health });

        fprintf(stderr, "%s: %s is not loaded; health cut, no explosion\n", tag.c_str(), name.c_str());
        return;
    }

    // dealDamagetoDatItem's lethal HIT (action 5, weapon 7, damage -1) through the unit's own handler, without its follow-up
    // lookup, which reads the freed slot after the kill. The name scratch holds the message.
    const uint32_t msg[3] = { 0xFFFFFFFFu, (7u << 16) | 0xFFFFu, 0u };

    for (int i = 0; i < 3; i++) *(uint32_t*)(rdram + kShotName + i * 4 - 0x80000000u) = msg[i];

    rs64::mips::mips_call(rdram, ctx, 0x8003E8DCu, { slot, 5u, kShotName });

    fprintf(stderr, "%s: destroyed %s\n", tag.c_str(), name.c_str());
}

extern "C" void rs64_camera_shots_frame(uint8_t* rdram, recomp_context* ctx)
{

    if (s_shot_links.empty() && s_remote_shots.empty()) return;

    s_shot_frame++;
    const bool busy = rs64shots::busy(rw(rdram, 0x8010C9E0u), rw(rdram, 0x8010CA1Cu));
    auto shot = s_shot_queue.take(busy);

    const auto put_name = [&](const std::string& name) {

        wstr(rdram, kShotName, name);

        return rs64::mips::mips_call(rdram, ctx, 0x80047B48u, { kShotName }).v0;
    };

    if (shot && !shot->destroy.empty()) {
        for (const auto& obj : shot->destroy) destroy_level_object(rdram, ctx, obj, "[shots] " + s_shot_queue.level());

        return;
    }

    const bool local = shot.has_value();

    if (local && !rs64shots::playsHere(*shot))
    {
        // Personal, set off by the other player: theirs only.
        fprintf(stderr, "[shots] %s: %s handed to the other player\n", s_shot_queue.level().c_str(), shot->camera.c_str());

        if (s_shot_listener) s_shot_listener(shot->camera.c_str(), shot->trigger.c_str(), s_shot_listener_user);

        return;
    }

    if (!shot && !busy && !s_remote_shots.empty()) {
        shot = std::move(s_remote_shots.front());
        s_remote_shots.pop_front();
    }

    if (!shot) return;

    const std::string name = shot->camera.substr(0, 255);
    const uint32_t item = put_name(name);

    if ((item & 0xFF800000u) != 0x80000000u || !rs64shots::isCameraItem(rw(rdram, item)))
    {
        fprintf(stderr, "[shots] %s: no camera %s; dropped\n", s_shot_queue.level().c_str(), name.c_str());

        return;
    }

    rs64::mips::mips_call(rdram, ctx, 0x800C7884u, { kShotName });

    fprintf(stderr, "[shots] %s: playing %s (mission frame %u)%s\n", s_shot_queue.level().c_str(), name.c_str(), s_shot_frame, local ? "" : " for the other player");

    if (local && rs64shots::tellsPeer(*shot) && s_shot_listener) s_shot_listener(name.c_str(), shot->trigger.c_str(), s_shot_listener_user);
}

// ---- Mod levels: levels.json entries shown in SELECT LEVEL after the stock ones, played as their template level ----
namespace {
std::vector<rs64levels::ModLevel> s_mod_levels;
rs64levels::Entries s_level_entries;
bool s_level_select_open = false;

// The mod level being played (index into s_mod_levels), -1 for a stock level.
int s_current_mod_level = -1;

// Manifest paths the enabled mods ship under assets/ (files, .from and .bps), so a lookup is redirected only to a file that exists.
std::set<std::string> s_mod_level_files;

// Host RAM for a redirected asset name (after the mid-mission shot name).
constexpr uint32_t kLookupName = 0x80C40100u;

// The SELECT LEVEL cursor (menu overlay, u8 at 0x800CDAC1).
const rs64levels::ModLevel* entry_at_cursor(uint8_t* rdram)
{

    if (!s_level_select_open) return nullptr;

    const int i = rb(rdram, 0x800CDAC1u) - s_level_entries.base;

    return i >= 0 && i < s_level_entries.count ? &s_mod_levels[i] : nullptr;
}

// ROGUESQ_MOD_LEVEL=<id>: that mod level plays whenever its template does (replays and BOOT_TARGET runs); -1 without it.
int forced_mod_level()
{

    static const char* forced = recomp::dbg::env_str("ROGUESQ_MOD_LEVEL");

    for (size_t k = 0; forced && k < s_mod_levels.size(); k++) {
        if (s_mod_levels[k].id == forced) return (int)k;
    }

    return -1;
}
uint32_t level_str(uint8_t* rdram, const std::string& s)
{

    static std::unordered_map<std::string, uint32_t> cache;

    if (auto it = cache.find(s); it != cache.end()) return it->second;

    auto* p = (uint8_t*)recomp::alloc(rdram, s.size() + 1);

    if (!p) return 0;

    const uint32_t off = (uint32_t)(p - rdram);

    wstr(rdram, off + 0x80000000u, s);

    return cache[s] = off + 0x80000000u;
}
}

extern "C" void rs64_rules_init();
extern "C" void rs64_crawls_init();
extern "C" void rs64_pause_point(const char* clock, uint32_t frame);

extern "C" void rs64_mod_levels_init() {

    for (const auto& d : recomp::mods::get_all_mod_details("rs64"))
    {
        if (!recomp::mods::is_mod_enabled(d.mod_id)) continue;

        // Every enabled mod's files count, so a crawl-only mod's own files are found too.
        std::ifstream in(recomp::mods::get_mod_filename(d.mod_id) / "levels.json", std::ios::binary);
        std::vector<std::string> errors;

        if (in)
        {
            for (auto& l : rs64levels::parseModLevels(d.mod_id, std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), errors))
            {
                fprintf(stderr, "[levels] %s: %s (\"%s\", plays as level %d, folder %s)\n", d.mod_id.c_str(), l.id.c_str(), l.title.c_str(), l.tmpl, l.folder.c_str());
                s_mod_levels.push_back(std::move(l));
            }
        }

        for (const auto& e : errors) fprintf(stderr, "[levels] %s\n", e.c_str());

        std::error_code ec;
        const auto assets = recomp::mods::get_mod_filename(d.mod_id) / "assets";

        for (auto it = std::filesystem::recursive_directory_iterator(assets, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            if (!it->is_regular_file()) continue;

            std::string p = std::filesystem::relative(it->path(), assets, ec).generic_string();

            if (p == rs64::assets::kFromMap) continue;

            for (const char* ext : { ".from", ".bps" })
            {
                if (p.size() > strlen(ext) && p.compare(p.size() - strlen(ext), strlen(ext), ext) == 0) p.resize(p.size() - strlen(ext));
            }

            s_mod_level_files.insert(p);
        }

        for (const auto& [key, src] : rs64::assets::read_from_map(assets)) s_mod_level_files.insert(key);
    }

    rs64_rules_init();
    rs64_crawls_init();

    int named = -1;
    const char* env_boot = recomp::dbg::env_str("ROGUESQ_BOOT_TARGET");
    const std::string boot = env_boot ? env_boot : "";
    const std::string tmpl_target = rs64levels::bootTargetFor(boot, s_mod_levels, named);

    // --boot-target=level:<mod level id>: the nav drives to its template, and the mod level plays in its place.
    if (!tmpl_target.empty())
    {
        rs64::cli::set_env("ROGUESQ_BOOT_TARGET", tmpl_target.c_str());
        rs64::cli::set_env("ROGUESQ_MOD_LEVEL", s_mod_levels[named].id.c_str());

        fprintf(stderr, "[levels] boot target %s -> %s\n", boot.c_str(), tmpl_target.c_str());
    }

    s_current_mod_level = forced_mod_level();
}

// SELECT LEVEL opens (a2 = highest stock entry shown): mod levels take the next entries.
extern "C" void rs64_mod_levels_select_open(uint8_t*, recomp_context* ctx)
{

    if (s_mod_levels.empty()) return;

    s_level_entries = rs64levels::layout((int)ctx->r6, s_mod_levels.size());

    if (s_level_entries.count < (int)s_mod_levels.size()) fprintf(stderr, "[levels] %zu mod levels do not fit SELECT LEVEL; %d shown\n", s_mod_levels.size(), s_level_entries.count);

    ctx->r6 = s_level_entries.base + s_level_entries.count - 1;
    s_level_select_open = true;
}

// Confirmed (s0 = entry): a mod entry plays as its template; a stock one clears the mod level.
extern "C" void rs64_mod_levels_select_confirm(uint8_t*, recomp_context* ctx)
{

    s_current_mod_level = forced_mod_level();

    if (!s_level_select_open) return;

    const int i = (int)ctx->r16 - s_level_entries.base;

    if (i < 0 || i >= s_level_entries.count) return;

    s_current_mod_level = i;
    ctx->r16 = s_mod_levels[i].tmpl;

    fprintf(stderr, "[levels] playing %s (as level %d)\n", s_mod_levels[i].id.c_str(), s_mod_levels[i].tmpl);
}

extern "C" void rs64_mod_levels_select_close() {
    s_level_select_open = false;
}

// SELECT LEVEL text for the entry under the cursor: v0 = its title (desc 0) or description (desc 1).
extern "C" void rs64_mod_levels_text(uint8_t* rdram, recomp_context* ctx, int desc)
{

    const auto* l = entry_at_cursor(rdram);

    if (!l) return;

    const std::string& s = desc ? l->description : l->title;

    if (const uint32_t p = level_str(rdram, s.empty() ? " " : rs64levels::menuText(s))) ctx->r2 = (gpr)(int32_t)p;
}

// A per-level lookup made for a mod entry while SELECT LEVEL is open (preview image, craft icons) uses its template.
extern "C" void rs64_mod_levels_entry_level(uint32_t* level)
{

    if (!s_level_select_open) return;

    const int i = (int)*level - s_level_entries.base;

    if (i >= 0 && i < s_level_entries.count) *level = (uint32_t)s_mod_levels[i].tmpl;
}

// Available craft for level: a mod level's approved craft mask (a SELECT LEVEL mod entry, or the template id while one plays), else -1
// with a mod entry's index turned into its template's.
extern "C" int rs64_mod_levels_craft(uint32_t* level) {

    const rs64levels::ModLevel* l = nullptr;
    const int i = (int)*level - s_level_entries.base;

    if (s_level_select_open && i >= 0 && i < s_level_entries.count)
    {
        l = &s_mod_levels[i];
        *level = (uint32_t)l->tmpl;
    } else if (s_current_mod_level >= 0 && (int)*level == s_mod_levels[s_current_mod_level].tmpl)
    {
        l = &s_mod_levels[s_current_mod_level];
    }

    return l ? l->craftMask : -1;
}

// A mod level's own briefing recording (a MORT stream at assets/level/<folder>/brief) plays through its template's briefing voice id:
// s_brief_index is that voice's speech index while the recording waits to start, s_brief_rom the stream's cart offset.
static int32_t s_brief_index = -1;
static uint32_t s_brief_rom = 0;
static bool s_brief_now = false;

// SELECT LEVEL briefing voice (v0): a mod entry's recording or chosen stock line, or 0xFFFF (an empty line list) for none.
extern "C" void rs64_mod_levels_voice(uint8_t* rdram, recomp_context* ctx)
{

    s_brief_index = -1;
    const auto* l = entry_at_cursor(rdram);

    if (!l) return;

    ctx->r2 = l->voice >= 0 ? (uint32_t)l->voice : 0xFFFFu;
    const auto* brief = rs64::assets::find("level/" + l->folder + "/brief");

    if (!brief) return;

    const uint32_t carrier = 0x279u + (uint32_t)l->tmpl;
    ctx->r2 = carrier;
    s_brief_index = (int16_t)MEM_H(0, (gpr)(int32_t)(0x8009FFE0u + carrier * 2u));
    s_brief_rom = brief->rom_off;
}

// allocAndConfigureNewVoice 0x8009B114 (s3 = speech index, s2 = rate or 0 for the table's): the recording is 8 kHz.
extern "C" void rs64_mod_levels_brief_rate(recomp_context* ctx)
{

    s_brief_now = s_brief_index >= 0 && (int32_t)ctx->r19 == s_brief_index;

    if (s_brief_now) ctx->r18 = 8000;
}

// allocAndConfigureNewVoice 0x8009B1F4: a1 = the stream's table offset; the call's delay slot adds the speech base, so the recording's
// cart offset goes in relative to that base.
extern "C" void rs64_mod_levels_brief_stream(uint8_t* rdram, recomp_context* ctx) {

    if (!s_brief_now) return;

    s_brief_now = false;
    s_brief_index = -1;
    const uint32_t base = (uint32_t)MEM_W(0, (gpr)(int32_t)0x80154684u);
    ctx->r5 = (gpr)(int32_t)(s_brief_rom - (base & 0x0FFFFFFFu));
}

// The SELECT LEVEL entry of the mod level a boot target names (ROGUESQ_MOD_LEVEL), while SELECT LEVEL is open; -1 otherwise. The nav
// picks it there so the run goes through the same confirm as a player's pick (briefing, medals).
extern "C" int rs64_mod_levels_boot_entry()
{

    const int i = forced_mod_level();

    return s_level_select_open && i >= 0 && i < s_level_entries.count ? s_level_entries.base + i : -1;
}

// Back in the menus (menuOverlayInit state; 3 = a mission's debrief, after which a death replays the level): no mod level is being
// played until SELECT LEVEL confirms one.
extern "C" void rs64_mod_levels_menu(int state) {
    if (state != 3) s_current_mod_level = forced_mod_level();
}

extern "C" int rs64_mod_levels_playing() {
    return s_current_mod_level >= 0;
}

// Crawl files from enabled mods (assets/crawl/<name>/crawl.json), in load order, and the one picked for the crawl now playing.
static std::vector<rs64levels::Crawl> s_crawls;
static const rs64levels::Crawl* s_playing_crawl = nullptr;

// The "crawl" host flag, readable lock-free on the per-object draw distance path.
static std::atomic<int> s_crawl_playing{ 0 };

static void set_crawl_playing(int on)
{

    s_crawl_playing.store(on, std::memory_order_relaxed);
    rs64::host::set_flag("crawl", on);
}

// 1 from the opening crawl's start through its planet pan-down, until the mission's first frame.
extern "C" int rs64_crawl_playing(void)
{
    return s_crawl_playing.load(std::memory_order_relaxed);
}

extern "C" void rs64_crawls_init()
{

    for (const auto& d : recomp::mods::get_all_mod_details("rs64"))
    {
        if (!recomp::mods::is_mod_enabled(d.mod_id)) continue;

        std::error_code ec;
        const auto dir = recomp::mods::get_mod_filename(d.mod_id) / "assets" / "crawl";

        for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
        {
            std::ifstream in(it->path() / "crawl.json", std::ios::binary);

            if (!in) continue;

            std::vector<std::string> errors;
            const std::string name = it->path().filename().string();
            auto c = rs64levels::parseCrawl(name, std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), errors);

            for (const auto& e : errors) fprintf(stderr, "[crawl] %s: %s\n", d.mod_id.c_str(), e.c_str());

            if (!c) continue;

            c->folder = "crawl/" + name;
            fprintf(stderr, "[crawl] %s: %s\n", d.mod_id.c_str(), name.c_str());
            s_crawls.push_back(std::move(*c));
        }
    }
}

// The opening crawl for level (getLevelIntroCrawlId's a0): -2 the game's own, -1 none, 0-3 the crawl to play (it picks the backdrop
// and fly-bys). A crawl file wins over the game's crawl; a mod level's inline crawl is its own.
extern "C" int rs64_mod_levels_crawl_id(int level)
{

    const auto* mod = s_current_mod_level >= 0 ? &s_mod_levels[s_current_mod_level] : nullptr;

    s_playing_crawl = rs64levels::pickCrawl(level, mod, s_crawls);

    int crawl = mod ? mod->crawl : rs64levels::kCrawlTemplate;

    if (s_playing_crawl)
    {
        fprintf(stderr, "[crawl] level %d plays %s\n", level, s_playing_crawl->name.c_str());
        crawl = s_playing_crawl->planet;
    } else if (mod && !mod->crawlName.empty())
    {
        static std::string s_warned;

        if (s_warned != mod->crawlName) fprintf(stderr, "[crawl] %s: no crawl \"%s\"; the template's plays\n", mod->id.c_str(), mod->crawlName.c_str());

        s_warned = mod->crawlName;
    }

    // A crawl plays now (the game's own for levels 0, 5, 10 and 15); the nav sequencer watches this flag.
    const bool stock = level == 0 || level == 5 || level == 10 || level == 15;
    set_crawl_playing(crawl >= 0 || (crawl == rs64levels::kCrawlTemplate && stock));

    return crawl;
}

// The crawl's text, written to host RAM (0x80C41000-0x80C41FFF); 0 when the game's own plays.
extern "C" uint32_t rs64_mod_levels_crawl_text(uint8_t* rdram)
{

    constexpr uint32_t kCrawlText = 0x80C41000u;

    if (s_playing_crawl)
    {
        wstr(rdram, kCrawlText, rs64levels::crawlString(*s_playing_crawl).substr(0, 0xFFF));
        return kCrawlText;
    }

    if (s_current_mod_level < 0 || s_mod_levels[s_current_mod_level].crawl < 0) return 0;

    wstr(rdram, kCrawlText, rs64levels::crawlString(s_mod_levels[s_current_mod_level]).substr(0, 0xFFF));

    return kCrawlText;
}

namespace {
// The playing crawl's keyed camera at runModelViewerLoop's clock (seconds at its sp + 0xD4); false for the game's own camera.
float crawl_clock(uint8_t* rdram, uint32_t loop_sp) {
    return rf(rdram, loop_sp + 0xD4);
}

bool crawl_camera(uint8_t* rdram, uint32_t loop_sp, float pos[3], float rot[3])
{
    return s_playing_crawl && rs64levels::crawlCameraAt(s_playing_crawl->camera, crawl_clock(rdram, loop_sp), pos, rot);
}
}

// setModelViewerObjectTransform entry (the crawl camera from its drop f12): a crawl file's camera keys build it instead, with the game's
// own calls (scale 0x800A7BC4 x it x 1, rotations x/y/z, translation) into the matrix at 0x800CFFB0 and its inverse at 0x800D039C.
extern "C" int rs64_crawl_camera(uint8_t* rdram, recomp_context* ctx)
{

    float pos[3], rot[3];

    rs64_pause_point("crawl", (uint32_t)(crawl_clock(rdram, (uint32_t)ctx->r29) * 30.0f));

    if (!crawl_camera(rdram, (uint32_t)ctx->r29, pos, rot)) return 0;

    static const rs64levels::Crawl* s_logged = nullptr;

    if (s_logged != s_playing_crawl) fprintf(stderr, "[crawl] %s: camera from its %zu keys\n", s_playing_crawl->name.c_str(), s_playing_crawl->camera.size());

    s_logged = s_playing_crawl;

    constexpr uint32_t kCam = 0x800CFFB0u, kView = 0x800D039Cu;
    const uint32_t scale = rw(rdram, 0x800A7BC4u);

    rs64::mips::mips_call(rdram, ctx, 0x8001CB64u, { kCam, scale, scale, float_bits(1.0f) });
    rs64::mips::mips_call(rdram, ctx, 0x8001CB9Cu, { kCam, float_bits(rot[0]) });
    rs64::mips::mips_call(rdram, ctx, 0x8001CCCCu, { kCam, float_bits(rot[1]) });
    rs64::mips::mips_call(rdram, ctx, 0x8001CDFCu, { kCam, float_bits(rot[2]) });
    rs64::mips::mips_call(rdram, ctx, 0x8001CA90u, { kCam, float_bits(pos[0]), float_bits(pos[1]), float_bits(pos[2]) });
    rs64::mips::mips_call(rdram, ctx, 0x8001C774u, { kView, kCam });
    ctx->r2 = (gpr)(int32_t)kCam;

    return 1;
}

// runModelViewerLoop 0x800CA080, after the pan sets the camera drop f26: the backdrop's rise follows the keyed camera's height.
extern "C" void rs64_crawl_drop(uint8_t* rdram, recomp_context* ctx)
{

    float pos[3], rot[3];

    if (crawl_camera(rdram, (uint32_t)ctx->r29, pos, rot)) ctx->f26.fl = pos[1];
}

extern "C" const char* rs64_mod_levels_current_folder()
{
    return s_current_mod_level >= 0 ? s_mod_levels[s_current_mod_level].folder.c_str() : nullptr;
}

// ---- Mod level medals: their own thresholds and their own record (roguesq_mod_medals.json), never the game's medals ----
namespace {
rs64levels::MedalStore s_mod_medals;
bool s_mod_medals_loaded = false;
bool s_debrief = false;
int s_earned_medal = -1;
uint32_t s_debrief_level = 0, s_saved_medal_info = 0;
uint8_t s_saved_medal = 0, s_saved_rank = 0;

// Host RAM for a mod level's three medal_info records (after the redirected asset name).
constexpr uint32_t kModMedals = 0x80C40200u;
constexpr const char* kModMedalsFile = "roguesq_mod_medals.json";

// Per profile: the account (0-2) the game plays with is the byte at 0x80130B44, the same number as its SELECT GAME slot.
std::string medal_key(int profile, const rs64levels::ModLevel& l) {
    return "p" + std::to_string(profile) + "/" + l.mod + "/" + l.id;
}
int current_profile(uint8_t* rdram) {
    return rb(rdram, 0x80130B44u);
}
rs64levels::MedalStore& mod_medals() {

    if (!s_mod_medals_loaded)
    {
        std::ifstream in(kModMedalsFile, std::ios::binary);
        s_mod_medals.load(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
        s_mod_medals_loaded = true;
    }

    return s_mod_medals;
}
}

// postLevelStatScreen entry (level): for a mod level, its thresholds (medal_info pointer at 0x800D03DC, indexed level * 3 + medal - 1)
// and its best medal (0x80130B1C[level], what "new medal" compares against) stand in for the template's until the debrief returns.
extern "C" void rs64_mod_levels_debrief_begin(uint8_t* rdram, uint32_t level)
{

    if (s_current_mod_level < 0) return;

    const auto& l = s_mod_levels[s_current_mod_level];
    s_debrief = true;
    s_earned_medal = -1;
    s_debrief_level = level & 0xFF;
    s_saved_medal_info = rw(rdram, 0x800D03DCu);
    s_saved_medal = rb(rdram, 0x80130B1Cu + s_debrief_level);
    s_saved_rank = rb(rdram, 0x80130B11u);

    if (l.hasMedals)
    {
        const auto rec = rs64levels::medalRecords(l);

        for (size_t i = 0; i < rec.size(); i++) wb(rdram, kModMedals + (uint32_t)i, rec[i]);

        // Bonuses are the level's secret pickups, which the template's mission script awards: keep its thresholds (u16 at +0xA).
        for (uint32_t t = 0; t < 3; t++)
        {
            for (uint32_t k = 0; k < 2; k++) wb(rdram, kModMedals + t * 0xCu + 0xAu + k, rb(rdram, s_saved_medal_info + s_debrief_level * 0x24u + t * 0xCu + 0xAu + k));
        }

        ww(rdram, 0x800D03DCu, kModMedals - s_debrief_level * 0x24u);
    }

    wb(rdram, 0x80130B1Cu + s_debrief_level, (uint8_t)mod_medals().best(medal_key(current_profile(rdram), l)));
}

extern "C" int rs64_mod_levels_debriefing() {
    return s_debrief;
}

extern "C" void rs64_mod_levels_medal_earned(int medal) {
    if (s_debrief) s_earned_medal = medal;
}

// The debrief header: the mod level's title.
extern "C" void rs64_mod_levels_title(uint8_t* rdram, recomp_context* ctx)
{

    if (!s_debrief) return;
    if (const uint32_t p = level_str(rdram, rs64levels::menuText(s_mod_levels[s_current_mod_level].title))) ctx->r2 = (gpr)(int32_t)p;
}

// The debrief returned: the template's medal_info, medal and rank come back; a better medal goes to the mod medal record.
extern "C" void rs64_mod_levels_debrief_end(uint8_t* rdram)
{

    if (!s_debrief) return;

    s_debrief = false;
    ww(rdram, 0x800D03DCu, s_saved_medal_info);
    wb(rdram, 0x80130B1Cu + s_debrief_level, s_saved_medal);
    wb(rdram, 0x80130B11u, s_saved_rank);
    const auto& l = s_mod_levels[s_current_mod_level];

    fprintf(stderr, "[levels] %s debrief: medal %d (best %d)\n", l.id.c_str(), s_earned_medal, mod_medals().best(medal_key(current_profile(rdram), l)));

    if (s_earned_medal > 0 && mod_medals().record(medal_key(current_profile(rdram), l), s_earned_medal))
    {
        std::ofstream(kModMedalsFile, std::ios::binary) << mod_medals().save();
    }
}

// SELECT GAME (setupCraftAnimationFlagSet, after it drew the profile's medal totals as text group 8 entries 1-3; s1 = the slot record at
// 0x800CF180 + slot * 0x48, +0xC = account data or 0): the profile's mod level medals as a second set, entries 4-6, beside the game's.
extern "C" void rs64_mod_levels_select_game(uint8_t* rdram, recomp_context* ctx)
{

    if (s_mod_levels.empty()) return;

    const uint32_t rec = (uint32_t)ctx->r17;

    if (rec < 0x800CF180u || (rec - 0x800CF180u) % 0x48u || (rec - 0x800CF180u) / 0x48u >= 3 || !rw(rdram, rec + 0xC)) return;

    const int profile = (int)((rec - 0x800CF180u) / 0x48u);
    int count[4] = {};

    for (const auto& l : s_mod_levels) count[mod_medals().best(medal_key(profile, l)) & 3]++;

    constexpr uint32_t kIcons = 0x80C40300u, kCounts = 0x80C40320u, kColour = 0x80C40360u;
    wstr(rdram, kIcons, rstr(rdram, 0x800A6964u, 31));
    char counts[64];
    snprintf(counts, sizeof(counts), "~%d\n\n~~~~~~~~~%d\n\n~~~~~~~~%d", count[3], count[2], count[1]);
    wstr(rdram, kCounts, counts);

    // Counts in light blue, so the mod set reads apart from the game's medals; icons untinted.
    ww(rdram, kColour, 0x90C8FFFFu);
    ww(rdram, kColour + 4, 0xFFFFFFFFu);
    constexpr int kDx = 64;
    const uint32_t scale = rw(rdram, 0x800A6970u);

    const struct { uint32_t entry, str; int x, y; uint32_t key, colour, scale; } e[3] = {
        { 4, kIcons, -0x74 + kDx, -0x7F, 0xF, kColour + 4, 0x3F800000u },
        { 5, kCounts, -0x7C + kDx, -0x7F, 0xA, kColour, scale },
        { 6, kCounts, -0x7B + kDx, -0x7D, 0xC, 0x800CC870u, scale },
    };

    for (const auto& x : e)
    {
        rs64::mips::mips_call(rdram, ctx, 0x80063CFCu, { 8, x.str, x.entry, (uint32_t)x.x }, { (uint32_t)x.y });
        const uint32_t elem = rs64::mips::mips_call(rdram, ctx, 0x80063C3Cu, { 8, x.entry }).v0;
        rs64::mips::mips_call(rdram, ctx, 0x800646F8u, { rw(rdram, 0x800CF078u), elem, x.key });
        rs64::mips::mips_call(rdram, ctx, 0x80063E54u, { 8, x.entry, x.colour });
        rs64::mips::mips_call(rdram, ctx, 0x80061F34u, { 8, x.entry, x.scale });
    }
}

// ---- Mod level objectives: a level with its own objectives runs none of its template's mission script ----
namespace {
float s_obj_elapsed = 0;
unsigned s_obj_done = 0, s_obj_failed = 0, s_obj_forced_done = 0, s_obj_forced_failed = 0;
const std::vector<rs64levels::Objective>* own_objectives() {

    if (s_current_mod_level < 0) return nullptr;

    const auto& o = s_mod_levels[s_current_mod_level].objectives;

    return o.empty() ? nullptr : &o;
}
}

// initializeObjectiveTracking entry: the pause list shows the level's objectives (missionObjectiveTextIds row non-zero, count, none
// hidden or complete). Returns 1 to skip the template's handlers.
extern "C" int rs64_mod_levels_objectives_init(uint8_t* rdram)
{

    const auto* objs = own_objectives();

    if (!objs) return 0;

    s_obj_elapsed = 0;
    s_obj_done = 0;
    s_obj_failed = 0;
    s_obj_forced_done = 0;
    s_obj_forced_failed = 0;
    const uint32_t row = 0x80109D2Cu + rw(rdram, 0x80130B70u) * 4u;

    for (uint32_t i = 0; i < rs64levels::kMaxObjectives; i++) wb(rdram, row + i, i < objs->size() ? 1 : 0);

    // The pause screen reads a line aloud unless its byte in this table is 0xFF; the voice itself comes from rs64_mod_levels_pause_voice.
    const uint32_t voices = 0x80109D80u + rw(rdram, 0x80130B70u) * 4u;

    for (uint32_t i = 0; i < rs64levels::kMaxObjectives; i++)
    {
        const auto* o = i < objs->size() ? &(*objs)[i] : nullptr;
        const bool rec = o && !o->pauseRecording.empty() && rs64::assets::find("level/" + s_mod_levels[s_current_mod_level].folder + "/" + o->pauseRecording) != nullptr;

        wb(rdram, voices + i, o && (o->pauseVoice >= 0 || rec) ? 0 : 0xFF);
    }

    wb(rdram, 0x80130B17u, (uint8_t)objs->size());
    wb(rdram, 0x80130B16u, 0);
    wb(rdram, 0x80130B15u, 0);

    return 1;
}

// A voice id whose speech and text are entry 0 (silence, a blank subtitle): what recordings and messages play as.
static int mod_level_voice_carrier(uint8_t* rdram)
{

    for (uint32_t v = 1; v < 0x2F4; v++)
    {
        if (MEM_H(0, (gpr)(int32_t)(0x8009FFE0u + v * 2u)) == 0) return (int)v;
    }

    return rs64levels::kVoiceNone;
}

// The voice id to play for the mod level: its recording (level/<folder>/<recording>) when it ships one, else the stock voice; a
// recording streams through the carrier line, redirected like the briefing's.
static int mod_level_voice(uint8_t* rdram, int voice, const std::string& recording)
{

    const auto& l = s_mod_levels[s_current_mod_level];
    const auto* rec = recording.empty() ? nullptr : rs64::assets::find("level/" + l.folder + "/" + recording);

    if (!rec) return voice;

    s_brief_index = 0;
    s_brief_rom = rec->rom_off;

    return mod_level_voice_carrier(rdram);
}

// A cut to the level's camera named camera (setHudFlagBit40AndStoreArg 0x800C7884), with the name kept at scratch: only when the
// level has it, since the cut reads the item without a check.
static void cut_to_camera(uint8_t* rdram, recomp_context* ctx, const std::string& camera, uint32_t scratch)
{

    wstr(rdram, scratch, camera.substr(0, 0x1F));

    if (rs64::mips::mips_call(rdram, ctx, 0x80047B48u, { scratch }).v0) rs64::mips::mips_call(rdram, ctx, 0x800C7884u, { scratch });
}

// An objective's callout, as the stock scripts make one: its voice line through playObjectiveVoiceLine2 0x800C7D1C (half a second
// late) and a cut to its DAT camera, which needs a name the level has.
static void objective_callout(uint8_t* rdram, recomp_context* ctx, uint32_t i)
{

    const auto& o = s_mod_levels[s_current_mod_level].objectives[i];
    const int voice = mod_level_voice(rdram, o.voice, o.recording);
    constexpr uint32_t kHalfSecond = 0x3F000000u;

    if (voice >= 0) rs64::mips::mips_call(rdram, ctx, 0x800C7D1Cu, { (uint32_t)voice, kHalfSecond });

    if (!o.camera.empty()) cut_to_camera(rdram, ctx, o.camera, 0x80C42100u + i * 0x20u);
}

// The per-level song table (0x8010B2EC, the song id per level startLevelBackgroundMusic plays); the original byte comes back at
// the next mission start.
namespace {
constexpr uint32_t kLevelSongs = 0x8010B2ECu;
int s_song_saved_level = -1;
uint8_t s_song_saved = 0;

void set_level_song(uint8_t* rdram, int song)
{

    const uint32_t level = rw(rdram, 0x80130B70u);

    if (s_song_saved_level < 0)
    {
        s_song_saved_level = (int)level;
        s_song_saved = rb(rdram, kLevelSongs + level);
    }

    wb(rdram, kLevelSongs + level, (uint8_t)song);
}
}

// initMission entry, before the level's songs are preloaded: a mod level's starting song.
extern "C" void rs64_mod_levels_music(uint8_t* rdram)
{

    if (s_song_saved_level >= 0) wb(rdram, kLevelSongs + (uint32_t)s_song_saved_level, s_song_saved);

    s_song_saved_level = -1;

    if (s_current_mod_level >= 0 && s_mod_levels[s_current_mod_level].music >= 0) set_level_song(rdram, s_mod_levels[s_current_mod_level].music);
}

// Pause screen 0x800C227C (initOrUpdatePauseScreenDim; s1 + 0x7C4 = the line it is reading aloud): the line's own voice. The call's
// delay slot adds 0x2C0 to a0 after this, so the id goes in less 0x2C0.
extern "C" void rs64_mod_levels_pause_voice(uint8_t* rdram, recomp_context* ctx)
{

    const auto* objs = own_objectives();

    if (!objs) return;

    const uint32_t i = rb(rdram, (uint32_t)ctx->r17 + 0x7C4u);

    if (i >= objs->size()) return;

    const auto& o = (*objs)[i];
    const int voice = mod_level_voice(rdram, o.pauseVoice, o.pauseRecording);

    if (voice >= 0) ctx->r4 = (gpr)(int32_t)(voice - 0x2C0);
}

// ---- Level rules: assets/level/<folder>/rules.json from enabled mods, run against the level being played ----
namespace {
std::map<std::string, std::vector<rs64rules::Rule>> s_rules_by_folder;
rs64rules::Runner s_rules;
std::string s_rules_folder;
std::string s_message_text;
float s_message_left = 0;
uint32_t s_message_text_id = 0xFFFFFFFFu;
bool s_rules_pending = false;
float s_rules_elapsed = 0;
constexpr uint32_t kRuleName = 0x80C42200u;

int rule_int(const rs64rules::Action& a, const char* key, int fallback = 0)
{
    return rs64rules::paramNumber(a, key, fallback);
}

std::string rule_str(const rs64rules::Action& a, const char* key)
{
    return rs64rules::paramText(a, key);
}

// An action a native mod registered with add_rule_action, given the rule's parameters as strings.
bool run_mod_rule_action(uint8_t* rdram, recomp_context* ctx, const rs64rules::Action& a)
{

    std::vector<std::string> values;
    std::vector<const char*> keys, vals;

    for (const auto& p : a.params) values.push_back(rule_str(a, p.key.c_str()));

    for (size_t i = 0; i < a.params.size(); i++)
    {
        keys.push_back(a.params[i].key.c_str());
        vals.push_back(values[i].c_str());
    }

    return rs64::host::run_rule_action(a.kind.c_str(), rdram, ctx, keys.data(), vals.data(), (int)keys.size());
}

void rules_run(uint8_t* rdram, recomp_context* ctx)
{

    const std::string tag = "[rules] " + s_rules_folder;

    for (const auto& a : s_rules.take())
    {
        const std::string& k = a.kind;

        fprintf(stderr, "%s: %s\n", tag.c_str(), k.c_str());

        if (k == "voice" || k == "recording")
        {
            const int voice = s_current_mod_level >= 0 ? mod_level_voice(rdram, rule_int(a, "id", -1), rule_str(a, "file")) : rule_int(a, "id", -1);

            if (voice >= 0) rs64::mips::mips_call(rdram, ctx, 0x800C7D1Cu, { (uint32_t)voice, 0u });
        }
        else if (k == "music")
        {
            // A new song in the level's table, and startLevelBackgroundMusic's once flag cleared so it stages that song again.
            set_level_song(rdram, rule_int(a, "song"));
            wb(rdram, 0x8010C424u, 0);
        }
        else if (k == "win") rs64::mips::mips_call(rdram, ctx, 0x800C776Cu, {});
        else if (k == "fail") rs64::mips::mips_call(rdram, ctx, 0x800C7738u, {});
        else if (k == "objectiveDone" || k == "objectiveFail")
        {
            const int i = rule_int(a, "objective") - 1;

            if (own_objectives() && i >= 0 && i < (int)own_objectives()->size()) (k == "objectiveDone" ? s_obj_forced_done : s_obj_forced_failed) |= 1u << i;
        }
        else if (k == "flag" || k == "wave" || k == "spawn")
        {
            // A wave or spawn starts when its gate flag (written by the explorer) is raised.
            const int f = rule_int(a, "flag");

            if (f >= 1 && f <= 128) wb(rdram, rs64::ls::relocated(0x801388A0u) + (uint32_t)(f - 1), (uint8_t)rule_int(a, "value", 1));
        }
        else if (k == "counter")
        {
            const int c = rule_int(a, "counter");
            const uint32_t at = rs64::ls::relocated(0x80138060u) + (uint32_t)(c - 1) * 4u;

            if (c >= 1 && c <= 128) ww(rdram, at, rw(rdram, at) + (uint32_t)rule_int(a, "add", 1));
        }
        else if (k == "message")
        {
            // A subtitle: a silent line (speech 0, blank text) streamed for seconds, its text swapped in by getVoiceText's hooks.
            const int seconds = std::clamp(rule_int(a, "seconds", 4), 1, 10);
            const int carrier = mod_level_voice_carrier(rdram);

            if (carrier < 0) continue;

            s_message_text = rs64levels::menuText(rule_str(a, "text")).substr(0, 0x17F);
            s_message_left = (float)seconds + 0.5f;
            s_brief_index = 0;
            s_brief_rom = rs64::assets::silence_rom(seconds);
            rs64::mips::mips_call(rdram, ctx, 0x800C7D1Cu, { (uint32_t)carrier, 0u });
        }
        else if (k == "destroy") destroy_level_object(rdram, ctx, rule_str(a, "unit"), tag);
        else if (k == "camera") cut_to_camera(rdram, ctx, rule_str(a, "camera"), kRuleName);
        else if (!run_mod_rule_action(rdram, ctx, a)) fprintf(stderr, "%s: no action %s; skipped\n", tag.c_str(), k.c_str());
    }
}
}

// Boot: every enabled mod's assets/level/<folder>/rules.json; later mods add to a level's list.
extern "C" void rs64_rules_init()
{

    for (const auto& d : recomp::mods::get_all_mod_details("rs64"))
    {
        if (!recomp::mods::is_mod_enabled(d.mod_id)) continue;

        std::error_code ec;
        const auto levels = recomp::mods::get_mod_filename(d.mod_id) / "assets" / "level";

        for (auto it = std::filesystem::directory_iterator(levels, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
        {
            std::ifstream in(it->path() / "rules.json", std::ios::binary);

            if (!in) continue;

            std::vector<std::string> errors;
            auto rules = rs64rules::parseRules(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), errors);
            const std::string folder = it->path().filename().string();

            for (const auto& e : errors) fprintf(stderr, "[rules] %s %s: %s\n", d.mod_id.c_str(), folder.c_str(), e.c_str());

            fprintf(stderr, "[rules] %s: %zu rules for %s\n", d.mod_id.c_str(), rules.size(), folder.c_str());

            auto& list = s_rules_by_folder[folder];
            list.insert(list.end(), rules.begin(), rules.end());
        }
    }
}

// Mission start (initMission): the level's rules start on the first mission frame, once its DAT is loaded to check names against.
extern "C" void rs64_rules_mission_start(uint8_t* rdram)
{

    const char* mod = rs64_mod_levels_current_folder();

    set_crawl_playing(0);
    s_playing_crawl = nullptr;
    s_rules_folder = rs64rules::rulesFolder(mod ? mod : "", (int)rw(rdram, 0x80130B70u));
    s_rules_pending = s_rules_by_folder.count(s_rules_folder) != 0;
    s_rules_elapsed = 0;
    s_rules.reset({}, [](const std::string&) { return true; });

    // A message or mod event from the last mission does not carry into this one.
    s_message_text.clear();
    s_message_left = 0;
    rs64::host::take_rule_events();
}

// Each mission frame (runInMissionFrame).
extern "C" void rs64_rules_frame(uint8_t* rdram, recomp_context* ctx)
{

    if (s_rules_pending)
    {
        s_rules_pending = false;

        for (const auto& e : s_rules.reset(s_rules_by_folder[s_rules_folder], [&](const std::string& n) { return level_object(rdram, ctx, n) != 0; })) {
            fprintf(stderr, "[rules] %s: %s; skipped\n", s_rules_folder.c_str(), e.c_str());
        }
    }

    // shortcut: a fixed 1/30 s per mission frame (the game's cap); read the frame dt if 60 fps logic ever lands.
    constexpr float kFrame = 1.0f / 30.0f;

    s_rules_elapsed += kFrame;

    if (s_message_left > 0 && (s_message_left -= kFrame) <= 0) s_message_text.clear();

    const auto condition = [&](const rs64rules::Rule& r) {

        const uint32_t f = rs64::ls::relocated(0x801388A0u), c = rs64::ls::relocated(0x80138060u);

        if (r.when == rs64rules::When::FlagIs) return r.value >= 1 && r.value <= 128 && rb(rdram, f + (uint32_t)(r.value - 1)) != 0;

        if (r.when == rs64rules::When::CounterAtLeast) return r.value >= 1 && r.value <= 128 && (int32_t)rw(rdram, c + (uint32_t)(r.value - 1) * 4u) >= r.count;

        // getDatItemHealthPercentage 0x800C7C28 (100 when the name is missing).
        wstr(rdram, kRuleName, r.object.substr(0, 0x3F));

        return (int32_t)rs64::mips::mips_call(rdram, ctx, 0x800C7C28u, { kRuleName }).v0 < r.value;
    };

    for (const auto& [name, object] : rs64::host::take_rule_events()) s_rules.event({ rs64rules::When::Custom, object, "", name });

    s_rules.tick(kFrame, s_rules_elapsed, condition);
    rules_run(rdram, ctx);
}

// applyDatObjectiveTriggerEffect past its gates (a0 = the trigger item, a1 = 1 entering / 0 leaving).
extern "C" void rs64_rules_area(uint8_t* rdram, uint32_t item, uint32_t enter)
{

    const uint32_t name = rw(rdram, item + 0x0C);

    if ((item & 0xFF800000u) != 0x80000000u || (name & 0xFF800000u) != 0x80000000u) return;

    s_rules.event({ enter ? rs64rules::When::Enter : rs64rules::When::Leave, rstr(rdram, name, 255) });
}

// A path event as an NPC reaches it (updateNpcEffectIntensity entry, a1 = the event's DAT record), by the event's name.
extern "C" void rs64_rules_path(uint8_t* rdram, uint32_t item)
{

    const uint32_t name = (item & 0xFF800000u) == 0x80000000u ? rw(rdram, item + 0x0C) : 0;

    if ((name & 0xFF800000u) == 0x80000000u) s_rules.event({ rs64rules::When::PathPoint, rstr(rdram, name, 255) });
}

// A unit's death effect (before datItemSetObjectiveBooleanCount at 0x8006D6F0 / 0x80078CC8, v1 = its DAT record).
extern "C" void rs64_rules_destroyed(uint8_t* rdram, recomp_context* ctx)
{

    const uint32_t item = (uint32_t)ctx->r3, name = (item & 0xFF800000u) == 0x80000000u ? rw(rdram, item + 0x0C) : 0;

    if ((name & 0xFF800000u) == 0x80000000u) s_rules.event({ rs64rules::When::Destroyed, rstr(rdram, name, 255) });
}

// getVoiceText entry (a0 = the text id) and before its return (0x80055A10, the text decrypted into voiceTxtString 0x80138E90):
// while a message shows, the blank line's text is the message.
extern "C" void rs64_rules_voice_text_id(recomp_context* ctx)
{
    s_message_text_id = (uint32_t)ctx->r4;
}

extern "C" void rs64_rules_voice_text(uint8_t* rdram)
{

    if (s_message_text.empty() || s_message_text_id != 0) return;

    wstr(rdram, rs64::ls::relocated(0x80138E90u), s_message_text);
}

extern "C" void rs64_rules_objective(int done, int index)
{
    s_rules.event({ done ? rs64rules::When::ObjectiveDone : rs64rules::When::ObjectiveFailed, "", "", "", index });
}

// checkComplexObjectives entry ($f12 = frame time): green each objective as it completes, then end the mission through the game's
// own setters (fail 0x800C7738 wins over success 0x800C776C). Returns 1 to skip the template's checks.
extern "C" int rs64_mod_levels_objectives_tick(uint8_t* rdram, recomp_context* ctx)
{

    const auto* objs = own_objectives();

    if (!objs) return 0;

    s_obj_elapsed += ctx->f12.fl;
    int32_t counts[128];
    uint8_t flags[128];
    const uint32_t c = rs64::ls::relocated(0x80138060u), f = rs64::ls::relocated(0x801388A0u);

    for (uint32_t i = 0; i < 128; i++) {
        counts[i] = (int32_t)rw(rdram, c + i * 4u);
        flags[i] = rb(rdram, f + i);
    }

    auto st = rs64levels::evaluateObjectives(*objs, counts, flags, s_obj_elapsed);
    unsigned completing = 0;

    for (uint32_t i = 0; i < objs->size(); i++)
    {
        const auto g = (*objs)[i].goal;

        if (g != rs64levels::Goal::Protect && g != rs64levels::Goal::TimeLimit) completing |= 1u << i;
    }

    // Level rules can complete or fail an objective themselves.
    st.done |= s_obj_forced_done;
    st.failed |= s_obj_forced_failed;
    st.fail = st.failed != 0;
    st.success = !st.fail && completing && (st.done & completing) == completing;

    if (st.success) st.done = (1u << objs->size()) - 1;

    for (uint32_t i = 0; i < objs->size(); i++)
    {
        if ((st.done & ~s_obj_done) & (1u << i)) rs64_rules_objective(1, (int)i + 1);

        if ((st.failed & ~s_obj_failed) & (1u << i)) rs64_rules_objective(0, (int)i + 1);
    }

    // The outcome first: its setter resets speech playback, so a callout queued before it in the same frame would be dropped.
    if (st.fail) rs64::mips::mips_call(rdram, ctx, 0x800C7738u, {});
    else if (st.success) rs64::mips::mips_call(rdram, ctx, 0x800C776Cu, {});

    for (uint32_t i = 0; i < objs->size(); i++)
    {
        if (!((st.done & ~s_obj_done) & (1u << i))) continue;

        rs64::mips::mips_call(rdram, ctx, 0x800C76B0u, { i + 1 });

        const auto g = (*objs)[i].goal;

        if (g != rs64levels::Goal::Protect && g != rs64levels::Goal::TimeLimit) objective_callout(rdram, ctx, i);
    }

    s_obj_done |= st.done;

    for (uint32_t i = 0; i < objs->size(); i++)
    {
        if ((st.failed & ~s_obj_failed) & (1u << i)) objective_callout(rdram, ctx, i);
    }

    s_obj_failed |= st.failed;

    return 1;
}

// calculateFriendliesSaved entry: a level with its own objectives counts none. Returns 1 to skip the template's count.
extern "C" int rs64_mod_levels_objectives_friendlies(uint8_t* rdram, recomp_context* ctx) {

    if (!own_objectives()) return 0;

    rs64::mips::mips_call(rdram, ctx, 0x800C77B0u, { 0 });

    return 1;
}

// Pause list 0x800C2F1C (v0 = the line's text, s4 -> its 0-based index): the level's own objective text.
extern "C" void rs64_mod_levels_objective_text(uint8_t* rdram, recomp_context* ctx)
{

    const auto* objs = own_objectives();

    if (!objs) return;

    const uint32_t i = (uint32_t)(int16_t)MEM_H(0, ctx->r20);

    if (i >= objs->size()) return;

    constexpr uint32_t kObjectiveText = 0x80C42000u;
    wstr(rdram, kObjectiveText + i * 0x40u, rs64levels::menuText((*objs)[i].text).substr(0, 0x3F));
    ctx->r2 = (gpr)(int32_t)(kObjectiveText + i * 0x40u);
}

// SELECT LEVEL medal for the entry at index: a mod entry's own best medal, -1 for a stock entry.
extern "C" int rs64_mod_levels_entry_medal(uint8_t* rdram, uint32_t index)
{

    if (!s_level_select_open) return -1;

    const int i = (int)index - s_level_entries.base;

    return i >= 0 && i < s_level_entries.count ? mod_medals().best(medal_key(current_profile(rdram), s_mod_levels[i])) : -1;
}

// find_manifest_entry: while a mod level plays, a lookup of a template file it ships returns the mod's copy (the name is rewritten
// into kLookupName); returns the name address to use.
extern "C" uint32_t rs64_mod_levels_lookup(uint8_t* rdram, uint32_t name)
{

    if ((s_current_mod_level < 0 && !s_playing_crawl) || (name & 0xFF800000u) != 0x80000000u) return name;

    const std::string cur = rstr(rdram, name, 255);

    if (s_playing_crawl)
    {
        if (const auto own = rs64levels::crawlAsset(*s_playing_crawl, cur, s_mod_level_files))
        {
            wstr(rdram, kLookupName, rs64levels::manifestPath(*own));

            fprintf(stderr, "[crawl] %s -> %s\n", cur.c_str(), own->c_str());

            return kLookupName;
        }
    }

    if (s_current_mod_level < 0) return name;

    const std::string mod = rs64levels::remapPath(cur, s_mod_levels[s_current_mod_level], s_mod_level_files);

    if (mod.empty()) return name;

    wstr(rdram, kLookupName, rs64levels::manifestPath(mod));

    fprintf(stderr, "[levels] %s -> %s\n", cur.c_str(), mod.c_str());

    return kLookupName;
}

// The camera.shots service (include/rs64/camera_shots.h), published before mods init.
extern "C" void rs64_camera_shots_register_service() {

    static const rs64_camera_shots svc = {
        RS64_CAMERA_SHOTS_VERSION, (uint32_t)sizeof(rs64_camera_shots),
        [](const char* camera, const char* trigger) {
            if (!camera || !*camera || !trigger) return;

            if (!s_shot_queue.remote(s_shot_links, trigger))
            {
                fprintf(stderr, "[shots] %s: %s from the other player already played here\n", s_shot_queue.level().c_str(), trigger);

                return;
            }

            s_remote_shots.push_back(rs64shots::Shot{ camera, false, false, trigger });
        },
        [](rs64_shot_fn fn, void* user) {
            s_shot_listener = fn;
            s_shot_listener_user = user;
        },
    };

    rs64::host::add_service(RS64_CAMERA_SHOTS_SERVICE, &svc);
}
