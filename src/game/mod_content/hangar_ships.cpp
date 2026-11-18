// Custom ships in the craft-select hangar: ships.json entries get a bay of their own after the stock ships and fly as their template
// craft. Every hook is inert when no enabled mod lists a ship, so the stock hangar is unchanged.
#include "game/mod_content/asset_overrides.h"
#include "game/mod_content/mod_ships.h"
#include "host_api/host_api.h"
#include "rs64/ships.h"
#include "librecomp/addresses.hpp"
#include "librecomp/mods.hpp"
#include "recomp.h"
#include "rs64_toml_hooks.h"
#include "util/mips_call.h"
#include "util/rdram_words.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using rs64::mips::rb;
using rs64::mips::rf;
using rs64::mips::wb;
using rs64::mips::wf;
using rs64::mips::wh;
using rs64::mips::ww;
using rs64::mips::wstr;

namespace {

// Hangar tables (menu overlay) and state bytes.
constexpr uint32_t kBayCraft = 0x800CC3F8u;
constexpr uint32_t kStopBay = 0x800CC404u;
constexpr uint32_t kStopCraft = 0x800CC40Cu;
constexpr uint32_t kStopU = 0x800CC414u;
constexpr uint32_t kStopV = 0x800CC430u;
constexpr uint32_t kBay = 0x800CD6E4u;
constexpr uint32_t kStop = 0x800CD6E8u;
constexpr uint32_t kFalconUnlocked = 0x800CD6CDu;
constexpr uint32_t kNabooUnlocked = 0x800CD6CFu;
constexpr uint32_t kObjects = 0x800CDA5Cu;
constexpr uint32_t kObjectCount = 0x800CDA40u;
constexpr uint32_t kShipsEnd = 0x800CDA46u;
// How many objects the per-object setup loop (hangarInitialize 0x800AA398) runs over: 48, set before the objects are listed.
constexpr uint32_t kSetupCount = 0x800CDA42u;
constexpr uint32_t kObjectSize = 0x278u;
constexpr uint16_t kShipFlags = 0x621u;
constexpr int kFirstBay = 9;
constexpr int kFirstStop = 8;
constexpr int kStockStops = 8;
// The stops turning steps through (7 is the TIE's own reveal).
constexpr int kTurnStops = 7;

std::vector<rs64ships::Ship> s_ships;

}

const std::set<std::string>& rs64_mod_files();

namespace {

// Host copies of the per-stop tables, one block: stop -> bay, stop -> craft (16 bytes each), then the two route positions (16 floats each).
uint32_t s_tables = 0;
uint32_t s_names = 0;
uint32_t s_paths = 0;
bool s_in_hangar = false;
bool s_in_cutscene = false;
int s_cut_player = -1;
int s_flying = -1;

constexpr uint32_t kTblBay = 0x00u;
constexpr uint32_t kTblCraft = 0x10u;
constexpr uint32_t kTblU = 0x20u;
constexpr uint32_t kTblV = 0x60u;
// stop -> bay for the crew voice lines, whose per-bay tables hold only the stock eight: a ship's stop speaks its template's line.
constexpr uint32_t kTblVoiceBay = 0xA0u;
// The ships' words of the 8-word per-bay array at 0x800CDA20, which the object counts follow.
constexpr uint32_t kTblBayWord = 0xB0u;
// Ship slot -> object for the per-frame ship pose loop (composeNpcOrientationMatrix), whose 8-byte table is 0x800CC518.
constexpr uint32_t kTblSlotObject = 0xC0u;
constexpr uint32_t kTblSize = 0xD0u;
constexpr uint32_t kBayWords = 0x800CDA20u;
constexpr uint32_t kSlotObject = 0x800CC518u;
// Per-slot shadow extents (12 bytes each); the ships' slots use their template's.
constexpr uint32_t kSlotExtents = 0x800CC52Cu;
constexpr uint32_t kStockSlots = 8;

uint32_t alloc_rdram(uint8_t* rdram, size_t size)
{

    auto* p = (uint8_t*)recomp::alloc(rdram, size);

    return p ? (uint32_t)(p - rdram) + 0x80000000u : 0u;
}

int ship_at_stop(int stop)
{

    const int k = stop - kFirstStop;

    return k >= 0 && k < (int)s_ships.size() ? k : -1;
}

// The stock bay showing a ship's template craft, 0 for none.
uint32_t template_bay(uint8_t* rdram, int k)
{

    for (uint32_t bay = 1; bay < (uint32_t)kFirstBay; bay++)
    {
        if (rb(rdram, kBayCraft + bay) == (uint8_t)s_ships[k].tmpl) return bay;
    }

    return 0;
}

// The craft line's per-bay "played" bytes (8, by bay - 1). A ship with its own line keeps its own byte, swapped into its template's
// while its stop shows.
constexpr uint32_t kLinePlayed = 0x800CDA08u;
bool s_line_played[rs64ships::kMaxShips];
int s_line_swapped = -1;
uint8_t s_line_saved = 0;
}

extern "C" void rs64_ships_init()
{

    for (const auto& d : recomp::mods::get_all_mod_details("rs64"))
    {
        if (!recomp::mods::is_mod_enabled(d.mod_id)) continue;

        std::ifstream in(recomp::mods::get_mod_filename(d.mod_id) / "ships.json", std::ios::binary);

        if (!in) continue;

        std::vector<std::string> errors;

        for (auto& s : rs64ships::parseShips(d.mod_id, std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), errors))
        {
            if (s_ships.size() == rs64ships::kMaxShips)
            {
                fprintf(stderr, "[ships] %s: %s left out; the hangar holds %zu custom ships\n", d.mod_id.c_str(), s.id.c_str(), rs64ships::kMaxShips);
                continue;
            }

            fprintf(stderr, "[ships] %s: %s (\"%s\", flies as craft %d)\n", d.mod_id.c_str(), s.id.c_str(), s.name.c_str(), s.tmpl);
            s_ships.push_back(std::move(s));
        }

        for (const auto& e : errors) fprintf(stderr, "[ships] %s\n", e.c_str());
    }
}

// hangarInitialize entry: the stop tables are copied with the custom stops after the stock eight, and the bay table's spare bytes take
// the ships' template crafts.
extern "C" void rs64_ships_hangar_begin(uint8_t* rdram)
{

    s_in_hangar = !s_ships.empty();
    s_line_swapped = -1;

    for (bool& p : s_line_played) p = false;
    s_flying = -1;

    if (s_ships.empty()) return;

    if (!s_tables) s_tables = alloc_rdram(rdram, kTblSize);

    for (int i = 0; i < kStockStops; i++)
    {
        wb(rdram, s_tables + kTblVoiceBay + i, rb(rdram, kStopBay + i));
        wb(rdram, s_tables + kTblBay + i, rb(rdram, kStopBay + i));
        wb(rdram, s_tables + kTblCraft + i, rb(rdram, kStopCraft + i));
        wf(rdram, s_tables + kTblU + i * 4, rf(rdram, kStopU + i * 4));
        wf(rdram, s_tables + kTblV + i * 4, rf(rdram, kStopV + i * 4));
    }

    for (size_t k = 0; k < s_ships.size(); k++)
    {
        const auto& s = s_ships[k];
        const int stop = kFirstStop + (int)k;

        wb(rdram, s_tables + kTblBay + stop, (uint8_t)(kFirstBay + k));

        for (uint32_t bay = 1; bay < (uint32_t)kFirstBay; bay++)
        {
            if (rb(rdram, kBayCraft + bay) == (uint8_t)s.tmpl) wb(rdram, s_tables + kTblVoiceBay + stop, (uint8_t)bay);
        }

        wb(rdram, s_tables + kTblSlotObject + (uint32_t)k, (uint8_t)(kFirstBay + k));
        wb(rdram, s_tables + kTblCraft + stop, (uint8_t)s.tmpl);
        wf(rdram, s_tables + kTblU + stop * 4, s.stop[0]);
        wf(rdram, s_tables + kTblV + stop * 4, s.stop[1]);
        wb(rdram, kBayCraft + kFirstBay + (uint32_t)k, (uint8_t)s.tmpl);
    }
}

// Bay b's camera start / look-at: element b (12 bytes) of 0x800CD714 / 0x800CD774, from camstart_<b-1> / camwatch_<b-1>.
// The arrays are back to back, so the ships' bays use host copies, filled from the scene records <id>_camstart / <id>_camwatch.
constexpr uint32_t kCamStart = 0x800CD714u;
constexpr uint32_t kCamWatch = 0x800CD774u;
constexpr uint32_t kCamElems = 16;
constexpr uint32_t kHangarDat = 0x800CC58Cu;
static uint32_t s_cam = 0;

extern "C" uint32_t rs64_ships_cam_base(uint32_t original)
{

    if (!s_cam) return original;

    if (original == kCamStart) return s_cam;

    if (original == kCamWatch) return s_cam + kCamElems * 12;

    // The take-off target, the camwatch point indexed by bay - 1.
    if (original == kCamWatch + 12) return s_cam + kCamElems * 12 + 12;

    return original;
}

// After the stock camera points are read: copy them, then each ship's from its scene records (getDatItemByName 0x80047B70).
extern "C" void rs64_ships_cam_fill(uint8_t* rdram, recomp_context* ctx)
{

    if (s_ships.empty()) return;

    if (!s_cam) s_cam = alloc_rdram(rdram, 2 * kCamElems * 12);

    static uint32_t s_name = 0;

    if (!s_name) s_name = alloc_rdram(rdram, 32);

    for (uint32_t i = 0; i < kFirstBay + 1 && i < kCamElems; i++)
    {
        for (uint32_t a = 0; a < 12; a += 4)
        {
            ww(rdram, s_cam + i * 12 + a, rs64::mips::rw(rdram, kCamStart + i * 12 + a));
            ww(rdram, s_cam + (kCamElems + i) * 12 + a, rs64::mips::rw(rdram, kCamWatch + i * 12 + a));
        }
    }

    for (size_t k = 0; k < s_ships.size(); k++)
    {
        const uint32_t bay = kFirstBay + (uint32_t)k;

        for (int which = 0; which < 2; which++)
        {
            wstr(rdram, s_name, s_ships[k].id + (which ? "_camwatch" : "_camstart"));

            const uint32_t rec = rs64::mips::mips_call(rdram, ctx, 0x80047B70u, { rs64::mips::rw(rdram, kHangarDat), s_name }).v0;

            if (!rec)
            {
                fprintf(stderr, "[ships] %s: no %s record in the hangar scene\n", s_ships[k].id.c_str(), which ? "_camwatch" : "_camstart");
                continue;
            }

            for (uint32_t a = 0; a < 12; a += 4) ww(rdram, s_cam + ((which ? kCamElems : 0) + bay) * 12 + a, rs64::mips::rw(rdram, rec + 0x10 + a));
        }
    }
}

// A per-stop table base the hangar code built with lui/addiu: the host copy while ships are installed.
extern "C" uint32_t rs64_ships_table(uint32_t original)
{

    if (!s_tables) return original;

    switch (original)
    {
    case kStopBay:   return s_tables + kTblBay;
    case kStopCraft: return s_tables + kTblCraft;
    case kStopU:     return s_tables + kTblU;
    case kStopV:     return s_tables + kTblV;
    default:         return original;
    }
}

// An element address in the per-bay word array (0x800CDA20 + 4 * (bay - 1)): a ship's bay uses its host word.
extern "C" uint32_t rs64_ships_bay_word(uint32_t addr)
{

    if (!s_tables || addr < kBayWords + 0x20u || addr >= kBayWords + 0x20u + 4u * (uint32_t)s_ships.size()) return addr;

    return s_tables + kTblBayWord + (addr - kBayWords - 0x20u);
}

// The pose loop runs on through one slot per ship.
extern "C" uint32_t rs64_ships_slot_loop(uint32_t slot) {
    return slot < kStockSlots + (uint32_t)s_ships.size();
}

// A ship slot's object byte (the loop reads 0x800CC518 + slot).
extern "C" uint32_t rs64_ships_slot_object(uint32_t addr)
{

    const uint32_t k = addr - kSlotObject - kStockSlots;

    return s_tables && k < s_ships.size() ? s_tables + kTblSlotObject + k : addr;
}

// A ship slot's extents (0x800CC52C + 12 * slot): its template's slot.
extern "C" uint32_t rs64_ships_slot_extents(uint8_t* rdram, uint32_t addr)
{

    const uint32_t k = (addr - kSlotExtents) / 12 - kStockSlots;

    if (!s_tables || addr < kSlotExtents || k >= s_ships.size()) return addr;

    for (uint32_t slot = 0; slot < kStockSlots; slot++)
    {
        if (rb(rdram, kBayCraft + rb(rdram, kSlotObject + slot)) == (uint8_t)s_ships[k].tmpl) return kSlotExtents + 12 * slot;
    }

    return kSlotExtents;
}

extern "C" uint32_t rs64_ships_voice_table(uint32_t original) {
    return s_tables && original == kStopBay ? s_tables + kTblVoiceBay : original;
}

// After the koelsch object (the last stock ship) is filled: one object per ship, named "+<id>" like the stock ships, so the generic
// per-object setup finds its hangar record and model by that name. Returns the new running count.
extern "C" uint32_t rs64_ships_add_objects(uint8_t* rdram, uint32_t count)
{

    if (s_ships.empty()) return count;

    if (!s_names)
    {
        s_names = alloc_rdram(rdram, 16 * s_ships.size());

        for (size_t k = 0; k < s_ships.size(); k++) wstr(rdram, s_names + 16 * (uint32_t)k, "+" + s_ships[k].id);
    }

    const uint32_t objects = rs64::mips::rw(rdram, kObjects);

    for (size_t k = 0; k < s_ships.size(); k++, count++)
    {
        const uint32_t o = objects + count * kObjectSize;

        ww(rdram, o, s_names + 16 * (uint32_t)k);
        wh(rdram, o + 0x40, kShipFlags);
    }

    wh(rdram, kObjectCount, (uint16_t)count);
    wh(rdram, kShipsEnd, (uint16_t)count);
    wh(rdram, kSetupCount, (uint16_t)(rs64::mips::rh(rdram, kSetupCount) + s_ships.size()));

    return count;
}

// The model file of ship k (nullptr past the last), loaded with the stock ship models.
extern "C" uint32_t rs64_ships_model_path(uint8_t* rdram, uint32_t k)
{

    if (k >= s_ships.size()) return 0;

    if (!s_paths)
    {
        s_paths = alloc_rdram(rdram, 32 * s_ships.size());

        for (size_t i = 0; i < s_ships.size(); i++) wstr(rdram, s_paths + 32 * (uint32_t)i, "pl_crafts/" + s_ships[i].id);
    }

    return s_paths + 32 * k;
}

extern "C" int rs64_ships_count() {
    return (int)s_ships.size();
}

// Turning, after the stock step wrote the stop byte: the stock seven then the ships, both ways round.
extern "C" void rs64_ships_turn(uint8_t* rdram, int dir)
{

    if (s_ships.empty()) return;

    // The turn starts at the stop showing the current bay.
    const int bay = rb(rdram, kBay);
    int from = bay >= kFirstBay ? kFirstStop + bay - kFirstBay : -1;
    std::vector<rs64ships::StopAt> stops;

    for (int s = 0; s < kTurnStops; s++)
    {
        stops.push_back({ s, rf(rdram, kStopU + 4u * (uint32_t)s) });

        if (rb(rdram, kStopBay + (uint32_t)s) == bay) from = s;
    }

    for (size_t k = 0; k < s_ships.size(); k++) stops.push_back({ kFirstStop + (int)k, s_ships[k].stop[0] });

    // The stock turn skips the Falcon and the Naboo starfighter until they are unlocked.
    const auto locked = [&](int s) {

        if (s >= kTurnStops) return false;

        const uint8_t craft = rb(rdram, kStopCraft + (uint32_t)s);

        return (craft == 5 && !rb(rdram, kFalconUnlocked)) || (craft == 8 && !rb(rdram, kNabooUnlocked));
    };

    const int to = rs64ships::nextStop(from, dir, stops, locked);

    if (to < 0) return;

    wb(rdram, kStop, (uint8_t)to);

    if (s_line_swapped >= 0)
    {
        const uint32_t flag = kLinePlayed + template_bay(rdram, s_line_swapped) - 1;

        s_line_played[s_line_swapped] = rb(rdram, flag) != 0;
        wb(rdram, flag, s_line_saved);
        s_line_swapped = -1;
    }

    const int k = ship_at_stop(to);
    const auto* line = k >= 0 ? rs64::assets::find("pl_crafts/v_" + s_ships[k].id) : nullptr;
    const uint32_t tb = k >= 0 ? template_bay(rdram, k) : 0;

    if (!line || !line->size || !tb) return;

    s_line_saved = rb(rdram, kLinePlayed + tb - 1);
    wb(rdram, kLinePlayed + tb - 1, s_line_played[k] ? 1 : 0);
    s_line_swapped = k;
}

// Each ship's own shadow texture id, 0xFFFF for none (its template's then).
static uint16_t s_shadow_tex[rs64ships::kMaxShips];

// pl_crafts/s_<id> loaded as loadCraftShadowTextures (0x8006B710) loads one: read (0x8006488C), fix up (0x80055488), register (0x80022B90).
// Returns the texture id, 0xFFFF when the mod has none.
static uint16_t load_ship_shadow(uint8_t* rdram, recomp_context* ctx, const std::string& id)
{

    const std::string path = "pl_crafts/s_" + id;

    if (!rs64_mod_files().count(path)) return 0xFFFF;

    static uint32_t s_path = 0;
    static uint32_t s_desc = 0;

    if (!s_path) s_path = alloc_rdram(rdram, 32);

    if (!s_desc) s_desc = alloc_rdram(rdram, 0x20);

    wstr(rdram, s_path, path);

    const uint32_t file = rs64::mips::mips_call(rdram, ctx, 0x8006488Cu, { s_path }).v0;

    if (!file) return 0xFFFF;

    const uint32_t tex = file + 4;

    rs64::mips::mips_call(rdram, ctx, 0x80055488u, { tex, tex });
    wh(rdram, file + 0x32, (uint16_t)(rs64::mips::rh(rdram, file + 0x32) | 0x2000));

    for (uint32_t i = 0; i < 0x20; i++) wb(rdram, s_desc + i, 0);

    wh(rdram, s_desc, 1);

    return (uint16_t)rs64::mips::mips_call(rdram, ctx, 0x80022B90u, { s_desc, tex, 1u }).v0;
}

// The ships' models, loaded with the stock ship models (loadModel 0x8005645C), and their shadows.
extern "C" void rs64_ships_load_models(uint8_t* rdram, recomp_context* ctx)
{

    for (uint32_t k = 0; k < s_ships.size(); k++)
    {
        rs64::mips::mips_call(rdram, ctx, 0x8005645Cu, { rs64_ships_model_path(rdram, k), 0u, 0u, 0u });
        s_shadow_tex[k] = load_ship_shadow(rdram, ctx, s_ships[k].id);
    }
}

static float s_boost_t = 0.0f;
static bool s_aim_logged = false;
static float s_foil_t = 0.0f;
static int s_foil_state = 0;
static uint32_t s_wings[2] = { 0, 0 };
static bool s_wings_looked = false;
static bool s_speed_on = false;
static float s_speed = 0.0f;

static float f_of(gpr r)
{

    const uint32_t bits = (uint32_t)r;
    float f;

    memcpy(&f, &bits, sizeof f);

    return f;
}

static uint32_t bits_of(float f)
{

    uint32_t b;

    memcpy(&b, &f, sizeof b);

    return b;
}

// The player's craft context (allocateAndInitCraftNpcContext 0x8006BD18) shares its shadow cache slot with its craft's wingmen; the
// ship's own is loaded once per mission and once per cutscene, each its own scene (s_shadow_scene 0 / 1, -1 none yet).
static bool s_player_context = false;
static int s_shadow_scene = -1;
static uint16_t s_mission_shadow = 0xFFFF;

extern "C" void rs64_ships_player_context() {
    s_player_context = true;
}

extern "C" void rs64_ships_mission_reset() {
    s_shadow_scene = -1;
    s_in_cutscene = false;
    s_boost_t = 0.0f;
    s_aim_logged = false;
    s_foil_t = 0.0f;
    s_foil_state = 0;
    s_wings_looked = false;
    s_wings[0] = s_wings[1] = 0;
    s_speed_on = false;
}

// 0x8006BD7C, s0 = the context's shadow texture id.
extern "C" void rs64_ships_context_shadow(uint8_t* rdram, recomp_context* ctx)
{

    const bool player = s_player_context;

    s_player_context = false;

    if (!player || s_flying < 0) return;

    if (s_shadow_scene != (int)s_in_cutscene)
    {
        s_shadow_scene = (int)s_in_cutscene;
        s_mission_shadow = load_ship_shadow(rdram, ctx, s_ships[s_flying].id);

        fprintf(stderr, "[ships] %s %s shadow tex 0x%X\n", s_ships[s_flying].id.c_str(), s_in_cutscene ? "cutscene" : "mission", s_mission_shadow);
    }

    if (s_mission_shadow != 0xFFFF) ctx->r16 = s_mission_shadow;
}

// A player craft's lock-on search call (a3 = range, 0x10($sp) = cone cosine): "aim" takes the A-wing's wider cone and range, an A-wing
// template without it the X-wing's.
extern "C" void rs64_ships_aim(uint8_t* rdram, recomp_context* ctx)
{

    if (s_flying < 0) return;

    const auto& s = s_ships[s_flying];
    const bool aim = rs64ships::hasBehaviour(s, "aim");

    if (aim == (s.tmpl == 2)) return;

    const uint32_t cone_at = (uint32_t)ctx->r29 + 0x10;
    const float range = aim ? 10.0f : 9.0f;
    const float cone = rf(rdram, aim ? 0x800A5ABCu : 0x800A593Cu);
    const float r = aim ? std::max(f_of(ctx->r7), range) : range;

    wf(rdram, cone_at, aim ? std::min(rf(rdram, cone_at), cone) : cone);
    ctx->r7 = (gpr)(int32_t)bits_of(r);

    if (!s_aim_logged)
    {
        s_aim_logged = true;
        fprintf(stderr, "[ships] %s aim: range %.1f, cone cos %.4f\n", s.id.c_str(), r, rf(rdram, cone_at));
    }
}

// A ship's V-wing boost (0x800B9074..0x800B9280) and X-wing S-foils (0x800B4940..0x800B4C40, fire lock 0x800B521C) on another craft, with
// the game's own sounds, lines and rumble, at the shared physics step (a0 = the player's flight record, a1 = dt).
constexpr uint32_t kPlayerFlight = 0x80137DBCu;
constexpr uint32_t kHeldButtons = 0x80130B88u;
constexpr uint32_t kBindings = 0x8010BE18u;
constexpr uint32_t kBindBrake = 0x12u;
constexpr uint32_t kBindFire = 0x1Au;
constexpr uint32_t kBindSpecial = 0x22u;

static uint16_t held(uint8_t* rdram, uint32_t bind)
{
    return (uint16_t)(rs64::mips::rh(rdram, kHeldButtons) & rs64::mips::rh(rdram, kBindings + bind));
}

static void speech_once(uint8_t* rdram, recomp_context* ctx, uint32_t stock, uint32_t level15)
{
    rs64::mips::mips_call(rdram, ctx, 0x800B3FB4u, { rb(rdram, 0x80130B40u) == 0xF ? level15 : stock, 9u });
}

static void craft_sound(uint8_t* rdram, recomp_context* ctx, uint32_t dir, uint32_t pos, float f, uint32_t a, uint32_t b)
{
    rs64::mips::mips_call(rdram, ctx, 0x800989D8u, { 0u, dir, pos, bits_of(20.0f) }, { bits_of(f), 0u, a, b, 0u });
}

// The cockpit view plays a flat sound in place of the craft's positioned one.
static bool cockpit_view(uint8_t* rdram)
{

    const uint32_t view = rs64::mips::rw(rdram, 0x80138D1Cu);

    return view && rs64::mips::rh(rdram, view + 4) == 3;
}

static void foil_sound(uint8_t* rdram, recomp_context* ctx, uint32_t rec, uint32_t flat, uint32_t a)
{

    if (cockpit_view(rdram))
    {
        rs64::mips::mips_call(rdram, ctx, 0x80087EE8u, { flat, 0x78u, 0x40u });
        return;
    }

    craft_sound(rdram, ctx, rec + 4, rec + 0x28, 0.0f, a, 0x5Au);
}

static void boost(uint8_t* rdram, recomp_context* ctx, uint32_t rec, float* target)
{

    if (held(rdram, kBindSpecial))
    {
        if (s_boost_t > 0.0f)
        {
            speech_once(rdram, ctx, 0x130u, 0x214u);
        }
        else
        {
            static uint32_t s_vec = 0;

            if (!s_vec) s_vec = alloc_rdram(rdram, 32);

            const uint32_t m = rec + 0x250u;

            for (int i = 0; i < 2 && s_vec; i++)
            {
                wf(rdram, s_vec, 0.0f);
                wf(rdram, s_vec + 4, 0.0f);
                wf(rdram, s_vec + 8, -1.0f);
                rs64::mips::mips_call(rdram, ctx, 0x800193E8u, { m, s_vec, s_vec + 16 });

                for (uint32_t a = 0; a < 3; a++) ww(rdram, s_vec + a * 4, rs64::mips::rw(rdram, m + 8 + a * 12));

                craft_sound(rdram, ctx, s_vec + 16, s_vec, 0.0f, 0x25u, 0x78u);
            }

            s_boost_t = 12.0f;

            speech_once(rdram, ctx, 0x12Bu, 0x20Fu);
            rs64::mips::mips_call(rdram, ctx, 0x800B3E4Cu, { rec, 8u });
            rs64::mips::mips_call(rdram, ctx, 0x80101018u, { 0u, 2u, bits_of(10.8f) });
            fprintf(stderr, "[ships] %s boost\n", s_ships[s_flying].id.c_str());
        }
    }

    if (s_boost_t <= 0.0f) return;

    // The V-wing's boost 6 and cooldown cap 3 against its top speed 4.5, scaled to the template's top speed (the throttle hooks' caps).
    static constexpr uint32_t kTopSpeed[] = { 0x800A58DCu, 0x800A5B90u, 0x800A5A54u, 0x800A5CC0u, 0x800A532Cu, 0x800A5DE8u, 0, 0, 0x800A6154u };
    const uint32_t top_at = kTopSpeed[s_ships[s_flying].tmpl];
    const float scale = top_at ? rf(rdram, top_at) / 4.5f : 1.0f;

    *target = s_boost_t > 6.0f ? 6.0f * scale : std::min(*target, 3.0f * scale);
    s_boost_t = std::max(0.0f, s_boost_t - f_of(ctx->r5));
}

static void sfoils(uint8_t* rdram, recomp_context* ctx, uint32_t rec, float* target)
{

    const float dt = f_of(ctx->r5);

    if (s_foil_state == 0 && s_foil_t == 0.0f)
    {
        if (held(rdram, kBindSpecial))
        {
            foil_sound(rdram, ctx, rec, 0x4Du, 0x20u);
            craft_sound(rdram, ctx, rec + 4, rec + 0x28, 0.4f, 0x25u, 0x7Au);
            rs64::mips::mips_call(rdram, ctx, 0x800B3E4Cu, { rec, 8u });
            rs64::mips::mips_call(rdram, ctx, 0x80101018u, { 0u, 2u, bits_of(1.5f) });
            s_foil_t = dt;
        }
    }
    else if (s_foil_state == 0)
    {
        s_foil_t += dt;

        if (s_foil_t > 1.0f)
        {
            s_foil_t = 1.0f;
            s_foil_state = 1;
        }
    }
    else if (s_foil_t == 1.0f)
    {
        if (held(rdram, kBindSpecial) || held(rdram, kBindBrake))
        {
            foil_sound(rdram, ctx, rec, 0x37u, 0x1Fu);
            s_foil_t -= dt;
        }
    }
    else
    {
        s_foil_t -= dt;

        if (s_foil_t < 0.0f)
        {
            s_foil_t = 0.0f;
            s_foil_state = 0;
        }
    }

    // Closed foils: guns locked (Luke's line once the wings are nearly shut) and 1.5x the speed, as the X-wing's +0x14 speed scale.
    if (s_foil_state == 1 && held(rdram, kBindFire))
    {
        if (s_foil_t > 0.95f) speech_once(rdram, ctx, 0x134u, 0x28Cu);

        wh(rdram, kHeldButtons, (uint16_t)(rs64::mips::rh(rdram, kHeldButtons) & ~rs64::mips::rh(rdram, kBindings + kBindFire)));
    }

    if (s_foil_t > 0.0f) *target *= 1.0f + 0.5f * s_foil_t;

    // The X-wing's per-player foil state and position, which the HUD reads (isWeaponSlotReady 0x800B585C) to show the guns locked.
    ww(rdram, 0x8010BC80u, (uint32_t)s_foil_state);
    wf(rdram, 0x8010BC90u, s_foil_t);

    if (!s_wings_looked)
    {
        static uint32_t s_names = 0;

        if (!s_names) s_names = alloc_rdram(rdram, 32);

        s_wings_looked = true;

        const uint32_t model = rs64::mips::rw(rdram, rec);

        for (int i = 0; i < 2 && s_names && model; i++)
        {
            wstr(rdram, s_names + i * 16, i ? "wngrot_b" : "wngrot_a");
            s_wings[i] = rs64::mips::mips_call(rdram, ctx, 0x80059434u, { model, s_names + i * 16, 0u }).v0;
        }

        fprintf(stderr, "[ships] %s S-foil wings %s\n", s_ships[s_flying].id.c_str(), s_wings[0] && s_wings[1] ? "found" : "missing (name them wngrot_a / wngrot_b)");
    }

    const float angle = (1.0f - s_foil_t) * 10.0f;

    for (int i = 0; i < 2; i++)
    {
        if (s_wings[i]) rs64::mips::mips_call(rdram, ctx, 0x800AA424u, { s_wings[i] + 0x1C, 0u, 0u, bits_of(i ? angle : -angle) });
    }
}

extern "C" void rs64_ships_behaviours(uint8_t* rdram, recomp_context* ctx)
{

    const uint32_t rec = (uint32_t)ctx->r4;

    if (s_flying < 0 || s_in_cutscene || rec != kPlayerFlight) return;

    const auto& s = s_ships[s_flying];
    const float game_target = rf(rdram, rec + 0xEC);
    float target = game_target;

    if (s.tmpl != 3 && rs64ships::hasBehaviour(s, "boost")) boost(rdram, ctx, rec, &target);

    if (s.tmpl != 0 && rs64ships::hasBehaviour(s, "sfoils")) sfoils(rdram, ctx, rec, &target);

    if (target == game_target)
    {
        s_speed_on = false;
        return;
    }

    if (!s_speed_on) s_speed = rf(rdram, rec + 0xE8);

    s_speed_on = true;
    const float rate = rs64_rate_scale();

    s_speed += std::clamp(target - s_speed, -rf(rdram, rec + 0xF4) * rate, rf(rdram, rec + 0xF0) * rate);

    // Only the speed: the crafts' throttles start each frame from their own target (+0xEC), which physics doesn't read.
    wf(rdram, rec + 0xE8, s_speed);
}

// The template's own special-button tests (v0 = the button's mask): the V-wing's boost, the X-wing's S-foils, off for a ship without them.
extern "C" void rs64_ships_special_gate(recomp_context* ctx, int craft)
{

    if (s_flying < 0 || s_ships[s_flying].tmpl != craft) return;

    if (!rs64ships::hasBehaviour(s_ships[s_flying], craft == 3 ? "boost" : "sfoils")) ctx->r2 = 0;
}

// The bay setup loop's shadow call (hangarInitialize 0x800AABFC, a1 = the bay's shadow texture, s2 the bay + 1): a ship's own shadow.
extern "C" void rs64_ships_bay_shadow_tex(recomp_context* ctx)
{

    const uint32_t k = (uint32_t)ctx->r18 - 1u - (uint32_t)kFirstBay;

    if (k < s_ships.size() && s_shadow_tex[k] != 0xFFFF) ctx->r5 = s_shadow_tex[k];
}

// A loop over the bays (objects 0..8) runs on through the ships' bays.
extern "C" uint32_t rs64_ships_bay_loop(uint32_t i)
{
    return i < (uint32_t)(kFirstBay + (int)s_ships.size());
}

// The stop showing bay (the turn start's bay -> stop search, which covers only the stock eight).
extern "C" int rs64_ships_stop_of_bay(uint8_t* rdram, int found)
{

    const int bay = rb(rdram, kBay);
    const int k = bay - kFirstBay;

    return k >= 0 && k < (int)s_ships.size() ? kFirstStop + k : found;
}

// Confirm (the hangar wrapper storing the chosen craft): a ship's bay flies its template and is remembered.
extern "C" void rs64_ships_confirm(uint8_t* rdram)
{

    const int k = rb(rdram, kBay) - kFirstBay;

    s_flying = k >= 0 && k < (int)s_ships.size() ? k : -1;
    s_in_hangar = false;

    if (s_flying >= 0) fprintf(stderr, "[ships] flying %s as craft %d\n", s_ships[s_flying].id.c_str(), s_ships[s_flying].tmpl);
}

// The craft name the hangar shows (text 0x18 + craft) while a ship's stop is the current one.
extern "C" unsigned rs64_ships_text(uint8_t* rdram, unsigned text_id)
{

    if (!s_in_hangar) return 0;

    const int k = ship_at_stop(rb(rdram, kStop));

    if (k < 0 || text_id != 0x18u + (unsigned)s_ships[k].tmpl) return 0;

    static uint32_t s_text = 0;

    if (!s_text) s_text = alloc_rdram(rdram, 64);

    wstr(rdram, s_text, s_ships[k].name.substr(0, 63));

    return s_text;
}

extern "C" int rs64_ships_flying() {
    return s_flying;
}

extern "C" const char* rs64_ships_flying_id() {
    return s_flying >= 0 ? s_ships[s_flying].id.c_str() : "";
}

// ---- The ships service (include/rs64/ships.h): co-op shows the other player's ship ----
static std::string s_puppet;
static uint8_t* s_rdram = nullptr;

static const char* svc_shown()
{

    const int k = rb(s_rdram, kBay) - kFirstBay;

    if (s_in_hangar && k >= 0 && k < (int)s_ships.size()) return s_ships[k].id.c_str();

    return rs64_ships_flying_id();
}

static int svc_has(const char* id)
{

    for (const auto& s : s_ships)
    {
        if (s.id == id && rs64_mod_files().count("pl_crafts/" + s.id + "_HOB")) return 1;
    }

    return 0;
}

extern "C" void rs64_ships_register_service(uint8_t* rdram)
{

    s_rdram = rdram;

    static const rs64_ships svc = {
        RS64_SHIPS_VERSION,
        sizeof(rs64_ships),
        svc_shown,
        svc_has,
        [](const char* id) { s_puppet = id ? id : ""; },
    };

    rs64::host::add_service(RS64_SHIPS_SERVICE, &svc);
}

// npcWingmanUpdate's model lookup (0x800D94A4, a0 = the wingman model's name): a co-op puppet showing a ship draws the ship's model.
extern "C" void rs64_ships_puppet_mesh(uint8_t* rdram, recomp_context* ctx)
{

    if (s_puppet.empty()) return;

    static uint32_t s_name = 0;

    if (!s_name) s_name = alloc_rdram(rdram, 16);

    wstr(rdram, s_name, s_puppet);
    ctx->r4 = (gpr)(int32_t)s_name;

    fprintf(stderr, "[ships] co-op puppet draws %s\n", s_puppet.c_str());
}

// A replay of a ship's run, which boots straight into the mission: that ship flies.
extern "C" int rs64_ships_fly(const char* id)
{

    for (size_t k = 0; k < s_ships.size(); k++)
    {
        if (s_ships[k].id != id) continue;

        s_flying = (int)k;

        fprintf(stderr, "[ships] flying %s as craft %d (replay)\n", id, s_ships[k].tmpl);

        return 1;
    }

    return 0;
}

// Back in the menus (menuOverlayInit state; 3 = a mission's debrief, after which a death replays the level with the same craft).
extern "C" void rs64_ships_menu(int state)
{

    if (state == 3 || s_flying < 0) return;

    fprintf(stderr, "[ships] menu state %d: %s no longer flies\n", state, s_ships[s_flying].id.c_str());

    s_flying = -1;
}

// dispatchOverlaySetupByParentAssetName 0x80053D30 (v1 = the craft setup type, -1 when the model's name s0 is no stock craft's): a ship's
// model gets its template's setup (wing and engine parts).
extern "C" int rs64_ships_setup_type(uint8_t* rdram, uint32_t name, int type)
{

    if (type >= 0 || s_ships.empty() || (name & 0xFF800000u) != 0x80000000u) return type;

    const std::string n = rs64::mips::rstr(rdram, name, 16);

    for (const auto& s : s_ships)
    {
        if (s.id == n) return rs64ships::kSetupType[s.tmpl];
    }

    return type;
}

extern "C" void rs64_mod_levels_stream_for(uint8_t* rdram, uint32_t carrier, uint32_t rom);

// The craft line (craftSelectionVoiceLineHelper 0x800AE3A4, a0 = the template's 0xFFFF-ended list of sound ids): a ship's own
// pl_crafts/v_<id> recording plays alone, carried by the list's first id.
extern "C" void rs64_ships_craft_line(uint8_t* rdram, recomp_context* ctx)
{

    const int k = ship_at_stop(rb(rdram, kStop));

    if (k < 0) return;

    const auto* rec = rs64::assets::find("pl_crafts/v_" + s_ships[k].id);

    if (!rec || !rec->size) return;

    static uint32_t s_list = 0;

    if (!s_list) s_list = alloc_rdram(rdram, 4);

    const uint32_t carrier = rs64::mips::rh(rdram, (uint32_t)ctx->r4);

    wh(rdram, s_list, (uint16_t)carrier);
    wh(rdram, s_list + 2, 0xFFFF);
    ctx->r4 = (gpr)(int32_t)s_list;
    rs64_mod_levels_stream_for(rdram, carrier, rec->rom_off);

    fprintf(stderr, "[ships] %s: own craft line in place of sound 0x%X\n", s_ships[k].id.c_str(), carrier);
}

// The take-off climb rate (updateHangarBayScene 0x800ACEC0, 8 floats at 0x800CC44C indexed by bay - 1): a ship's bay takes its template's.
extern "C" uint32_t rs64_ships_climb_rate(uint8_t* rdram, uint32_t addr)
{

    constexpr uint32_t kClimbRate = 0x800CC44Cu;
    const uint32_t k = (addr - kClimbRate) / 4 + 1 - (uint32_t)kFirstBay;

    if (addr < kClimbRate || k >= s_ships.size()) return addr;

    const uint32_t bay = template_bay(rdram, (int)k);

    return bay ? kClimbRate + 4 * (bay - 1) : addr;
}

// The bay setup loop's shadow read (hangarInitialize 0x800AABE4, v0 = 0x800CC520 + bay, the bay's shadow image index): the eight-entry
// table ends at bay 8, so a ship's bay reads its template's bay.
extern "C" uint32_t rs64_ships_bay_shadow(uint8_t* rdram, uint32_t addr)
{

    constexpr uint32_t kBayShadow = 0x800CC520u;
    const uint32_t k = addr - kBayShadow - (uint32_t)kFirstBay;

    if (k >= s_ships.size()) return addr;

    const uint32_t bay = template_bay(rdram, (int)k);

    return bay ? kBayShadow + bay : addr;
}

// ---- AVAILABLE CRAFT (the level's craft as hologram blueprints): ships of an allowed template join, after the nine stock craft ----
constexpr uint32_t kBlueprintScaleX = 0x800CC704u;
constexpr uint32_t kBlueprintScaleY = 0x800CC728u;
constexpr uint32_t kStockCraft = 9;
// The layouts (0x800CC74C) go up to eight craft.
constexpr int kMaxShown = 8;
static uint32_t s_bp = 0;

// Host copies for the ships: sprite records (0x30 each, the stock ones at 0x800CDDB0), texture ids, screen positions, blueprint names.
static uint32_t bp_rec(uint32_t k) {
    return s_bp + 0x30u * k;
}

static uint32_t bp_tex(uint32_t k) {
    return s_bp + 0x30u * (uint32_t)rs64ships::kMaxShips + 2u * k;
}

static uint32_t bp_pos(uint32_t k) {
    return s_bp + 0x30u * (uint32_t)rs64ships::kMaxShips + 8u + 4u * k;
}

static uint32_t bp_name(uint32_t k) {
    return s_bp + 0x30u * (uint32_t)rs64ships::kMaxShips + 0x20u + 16u * k;
}

// The level's craft mask (0x800C63C0, bit = craft id) with bit 9 + k for each ship whose template it allows, while at most eight show.
extern "C" uint32_t rs64_ships_craft_mask(uint32_t mask)
{

    int shown = 0;

    for (uint32_t c = 0; c < kStockCraft; c++) shown += (mask >> c) & 1;

    for (size_t k = 0; k < s_ships.size() && shown < kMaxShown; k++)
    {
        if (!(mask & (1u << s_ships[k].tmpl))) continue;

        mask |= 1u << (kStockCraft + k);
        shown++;
    }

    return mask;
}

extern "C" int rs64_ships_craft_loop(uint32_t i) {
    return i < kStockCraft + (uint32_t)s_ships.size();
}

// bridgeLoadShipBlueprints 0x800B1714 (a2 = the blueprint name for "blueprints/%s", s4 the craft): a ship's own blueprint, else its template's.
extern "C" void rs64_ships_bp_name(uint8_t* rdram, recomp_context* ctx)
{

    const uint32_t k = (uint32_t)ctx->r20 - kStockCraft;

    if (k >= s_ships.size()) return;

    const auto& s = s_ships[k];

    if (rs64_mod_files().count("blueprints/" + s.id))
    {
        wstr(rdram, bp_name(k), s.id);
        ctx->r6 = (gpr)(int32_t)bp_name(k);

        return;
    }

    ctx->r6 = (gpr)(int32_t)rs64::mips::rw(rdram, (uint32_t)ctx->r29 + 0x30u + 4u * (uint32_t)s.tmpl);
}

// bridgeLoadShipBlueprints 0x800B17D4 (the loop's branch, s4 the next craft): the ships' records, texture slots and their templates' scales.
extern "C" void rs64_ships_bp_load_next(uint8_t* rdram, recomp_context* ctx)
{

    if (s_ships.empty()) return;

    if (!s_bp) s_bp = alloc_rdram(rdram, 0x30u * rs64ships::kMaxShips + 0x20u + 16u * rs64ships::kMaxShips);

    const uint32_t k = (uint32_t)ctx->r20 - kStockCraft;

    ctx->r2 = rs64_ships_craft_loop((uint32_t)ctx->r20);

    if (k >= s_ships.size()) return;

    const uint32_t t = 4u * (uint32_t)s_ships[k].tmpl;

    ctx->r18 = (gpr)(int32_t)(bp_rec(k) - 0x30u);
    ctx->r19 = (gpr)(int32_t)bp_tex(k);
    ctx->r21 = (gpr)(int32_t)(kBlueprintScaleX + t);
    ctx->r22 = (gpr)(int32_t)(kBlueprintScaleY + t);
}

// The layout loop's branch (0x800B19B4, s0 the next craft, s1 its record, s2 its position before the delay slot's +4).
extern "C" void rs64_ships_bp_layout_next(recomp_context* ctx)
{

    if (s_ships.empty() || !s_bp) return;

    const uint32_t k = (uint32_t)ctx->r16 - kStockCraft;

    ctx->r2 = rs64_ships_craft_loop((uint32_t)ctx->r16);

    if (k >= s_ships.size()) return;

    ctx->r17 = (gpr)(int32_t)bp_rec(k);
    ctx->r18 = (gpr)(int32_t)(bp_pos(k) - 4u);
}

// The per-frame draw loop's branch (0x800B12E0, s2 the next craft, s0 its record before the delay slot's +0x30, s1 its position, s3 / s4
// its scales).
extern "C" void rs64_ships_bp_draw_next(recomp_context* ctx)
{

    if (s_ships.empty() || !s_bp) return;

    const uint32_t k = (uint32_t)ctx->r18 - kStockCraft;

    ctx->r2 = rs64_ships_craft_loop((uint32_t)ctx->r18);

    if (k >= s_ships.size()) return;

    const uint32_t t = 4u * (uint32_t)s_ships[k].tmpl;

    ctx->r16 = (gpr)(int32_t)(bp_rec(k) - 0x30u);
    ctx->r17 = (gpr)(int32_t)bp_pos(k);
    ctx->r19 = (gpr)(int32_t)(kBlueprintScaleX + t);
    ctx->r20 = (gpr)(int32_t)(kBlueprintScaleY + t);
}

// The cinematic's scene setup (initSceneForMission entry), before its level and the player's craft load: the template redirects stay
// off (s_in_cutscene) until the mission starts, so the cutscene's other craft keep their stock models.
extern "C" void rs64_ships_cutscene_begin()
{

    s_in_cutscene = s_flying >= 0;
    s_cut_player = -1;
    s_shadow_scene = -1;
}

// processCutsceneActions return: the player's actor (event 18 named it pl_crafts/<craft>) is renamed to the ship, which the game loads
// by path through the asset table's catch-all entry and draws by its object name.
extern "C" void rs64_ships_cutscene_actors(uint8_t* rdram, uint32_t cut)
{

    if (!s_in_cutscene || !rs64_mod_files().count("pl_crafts/" + s_ships[s_flying].id + "_HOB")) return;

    const uint32_t events = rs64::mips::rh(rdram, cut + 0x4E);

    for (uint32_t i = 0; i < events; i++)
    {
        const uint32_t e = cut + 0x58 + i * 0x18;

        if (rs64::mips::rw(rdram, e) != 0x12) continue;

        s_cut_player = (int)rs64::mips::rw(rdram, e + 8);
        wstr(rdram, cut + 0x13D8 + (uint32_t)s_cut_player * 0x4C, "pl_crafts/" + s_ships[s_flying].id);
    }
}

// spawnCutsceneObjectsFromList 0x800A8254 (s7 = actor): the player's actor's craft context takes the ship's shadow.
extern "C" void rs64_ships_cutscene_shadow(int actor)
{

    if (s_in_cutscene && actor == s_cut_player) rs64_ships_player_context();
}

// The player's actor loads by path through the table's catch-all entry, which casts no shadow: 0x800A8228 (v0 = the entry's flags)
// gains the shadow bit and 0x800A8248 (v0 = its 20-byte shadow record at 0x800B04AC) takes the template's.
extern "C" uint32_t rs64_ships_cutscene_shadow_flags(int actor, uint32_t flags)
{

    return s_in_cutscene && actor == s_cut_player ? flags | 4u : flags;
}

extern "C" uint32_t rs64_ships_cutscene_shadow_record(uint8_t* rdram, int actor, uint32_t record)
{

    constexpr uint32_t kCutAssets = 0x800AF92Cu;
    constexpr uint32_t kCutShadows = 0x800B04ACu;

    if (!s_in_cutscene || actor != s_cut_player) return record;

    for (uint32_t i = 0; i < 32; i++)
    {
        if (rs64::mips::rstr(rdram, kCutAssets + i * 0x5C + 0x20, 16) == rs64ships::kExterior[s_ships[s_flying].tmpl]) return kCutShadows + i * 20;
    }

    return record;
}

// walkMeshdef0List entry (a model looked up by its object name): while a ship with its own exterior flies, the template's name finds
// the ship's model, whose object is named after the ship.
extern "C" uint32_t rs64_ships_model_name(uint8_t* rdram, uint32_t name)
{

    if (s_flying < 0 || s_in_cutscene) return name;

    const auto& s = s_ships[s_flying];

    if (rs64::mips::rstr(rdram, name, 16) != rs64ships::kExterior[s.tmpl] || !rs64_mod_files().count("pl_crafts/" + s.id + "_HOB")) return name;

    static uint32_t s_name = 0;

    if (!s_name) s_name = alloc_rdram(rdram, 16);

    wstr(rdram, s_name, s.id);

    return s_name;
}

// find_manifest_entry: while a ship flies, a lookup of its template's exterior, cockpit or HUD icon returns the ship's own file.
extern "C" uint32_t rs64_ships_lookup(uint8_t* rdram, uint32_t name)
{

    if (s_flying < 0 || s_in_cutscene) return name;

    const std::string cur = rs64::mips::rstr(rdram, name, 255);
    const std::string own = rs64ships::shipAsset(cur, s_ships[s_flying], rs64_mod_files());

    if (own.empty()) return name;

    static uint32_t s_lookup = 0;

    if (!s_lookup) s_lookup = alloc_rdram(rdram, 32);

    wstr(rdram, s_lookup, own.substr(0, 31));

    fprintf(stderr, "[ships] %s -> %s\n", cur.c_str(), own.c_str());

    return s_lookup;
}
