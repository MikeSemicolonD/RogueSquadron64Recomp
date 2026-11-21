// Draw-only stand-ins for level objects, ships and ground units the game has not spawned yet, or destroyed structures, turrets and AT-STs as their wrecks, so they show at long draw distance: a model instance and prop state
// block in host RAM, built as the prop handler (0x8006CBB0) spawns one, go through the game's visibility bucket. Nothing the game reads is written.
#include "util/mips_call.h"
#include "util/rdram_words.h"
#include "util/host_ram_map.h"
#include "recomp.h"
#include "rs64_toml_hooks.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using namespace rs64::mips;

constexpr uint32_t kDatFile = 0x801375D8u;
constexpr uint32_t kModelHash = 0x801394B0u;
constexpr uint32_t kModelEntries = 0x80139020u;
constexpr uint32_t kDifficulty = 0x80137CE4u;
constexpr uint32_t kComposeEuler = 0x80059D78u;
constexpr uint32_t kAddToBucket = 0x80057C8Cu;
constexpr uint32_t kSelectLodAndSubmit = 0x80058068u;
constexpr uint32_t kProbeLodTable = 0x8010A600u;
constexpr uint32_t kProbeSizeBonus = 0x800A6CA8u;
constexpr uint16_t kSubProbeDroid = 0x0Eu;
constexpr uint16_t kSubShip = 0x21u;
constexpr uint32_t kRootMatrix = 0x80059C24u;
constexpr uint32_t kUniformRootMatrix = 0x80059B50u;
constexpr uint32_t kConditionMet = 0x80065828u;
constexpr uint32_t kGridEntries = 0x80136FCCu;
constexpr uint32_t kGridScale = 0x8003A7B8u;
constexpr uint32_t kGridHalfCells = 0x8003A7BCu;
constexpr uint32_t kSpawnRef = 0x801375F8u;
constexpr uint32_t kCutsceneFlag = 0x80130B39u;
constexpr uint32_t kDatItemByName = 0x80047B48u;
constexpr uint32_t kTerrainHeight = 0x80068180u;
constexpr uint32_t kRegion = RS64_HR_STANDINS;
constexpr uint32_t kRegionEnd = RS64_HR_STANDINS_END;
constexpr uint32_t kStateSize = 0x50u;

constexpr uint16_t kSubStructure = 0x14u;
constexpr uint32_t kDebrisNames = 0x8003C9D0u;
constexpr uint32_t kDebrisScale = 0x8003CA14u;
constexpr uint32_t kScaleMatrix = 0x8001CB64u;
constexpr uint32_t kRotateX = 0x8001CB9Cu;
constexpr uint32_t kRotateY = 0x8001CCCCu;
constexpr uint32_t kRotateZ = 0x8001CDFCu;
constexpr uint32_t kTranslate = 0x8001CA90u;

// wreck/wreck_scale: a destroyed structure's model, 0 when it has none.
struct Standin {
    uint32_t item;
    uint32_t state;
    uint32_t inst;
    uint32_t scale;
    uint32_t wreck;
    uint32_t wreck_scale;
};

uint32_t s_dat = 0;
std::vector<Standin> s_standins;

bool names_in_item(uint16_t sub)
{

    switch (sub) {
    case 0x14: case 0x21: case 0x33: case 0x36: case 0x44: case 0x46: case 0x4A: case 0x4B: case 0x4F:
        return true;
    default:
        return false;
    }
}

// Subtypes whose handler picks the model itself (mission overlay strings, the undamaged model): turrets, walkers, droids, radar dish,
// shield dome, shuttles (npcShuttleUpdate), ground transports (npcGroundTransportUpdate), TIEs (npcTieVariantUpdate), train cars, power-ups.
uint32_t handler_model_name(uint16_t sub)
{

    switch (sub) {
    case 0x0D: return 0x800A6A40u;
    case 0x1E: return 0x800A6A4Cu;
    case 0x28: return 0x800A6A58u;
    case 0x41: return 0x800A6A64u;
    case 0x42: return 0x800A6A70u;
    case 0x0E: return 0x800A6C00u;
    case 0x0F: return 0x800A6ED0u;
    case 0x1F: return 0x800A70E0u;
    case 0x25: return 0x800A76F4u;
    case 0x2A: return 0x800A7D38u;
    case 0x32: return 0x8003D518u;
    case 0x47: return 0x800A874Cu;
    case 0x24: return 0x800A7480u;
    case 0x2C: return 0x800A748Cu;
    case 0x48: return 0x800A7498u;
    case 0x4D: return 0x800A74A4u;
    case 0x2B: return 0x800A7E60u;
    case 0x2D: return 0x800A7E70u;
    case 0x2E: return 0x800A7E7Cu;
    case 0x0C: return 0x800A7C00u;
    case 0x12: return 0x800A7BE0u;
    case 0x13: return 0x800A7BD4u;
    case 0x29: return 0x800A7BECu;
    case 0x43: return 0x800A7BF4u;
    case 0x2F: return 0x800A80C0u;
    case 0x30: return 0x800A80CCu;
    case 0x31: return 0x800A80D8u;
    case 0x34: return 0x800A80E8u;
    case 0x35: return 0x800A80F4u;
    case 0x4C: return 0x800A8890u;
    default: return 0;
    }
}

// The loaded model named `name` (walkMeshdef0List 0x80056EB0: byte sum mod 25 indexes u16 chains of 12-byte entries, +4 the model, names compared up to 16 bytes).
uint32_t find_model(const uint8_t* rdram, uint32_t name)
{

    uint32_t sum = 0;

    for (uint32_t i = 0; i < 64 && rb(rdram, name + i) != 0; ++i) sum += rb(rdram, name + i);

    uint32_t idx = rh(rdram, kModelHash + (sum % 25u) * 2u);

    for (int hops = 0; idx != 0xFFFFu && hops < 1024; ++hops) {
        const uint32_t entry = kModelEntries + idx * 12u;
        const uint32_t model = rw(rdram, entry + 4u);
        bool same = true;

        for (uint32_t i = 0; i < 16; ++i) {
            const uint8_t a = rb(rdram, name + i);

            if (a != rb(rdram, model + i)) {
                same = false;
                break;
            }

            if (a == 0) break;
        }

        if (same) return model;

        idx = rh(rdram, entry);
    }

    return 0;
}

// walkMeshdef0List's instance: 0xC header (+0 model), nodes of 0x4C (four child links, +0x10 leaf list, +0x14/+0x18 and the 0x30-byte matrix
// at +0x1C copied from the model's node), then leaves of 0x5C chained per node. The model's use count (+0x3A) is left alone.
uint32_t build_instance(uint8_t* rdram, recomp_context* ctx, uint32_t model, uint32_t at)
{

    const uint32_t table = rw(rdram, model + 0x14u);
    const uint32_t nodes = rh(rdram, table);
    const uint32_t leaves = rh(rdram, table + 2u);
    const uint32_t size = 0xCu + nodes * 0x4Cu + leaves * 0x5Cu;

    if (at + size > kRegionEnd) return 0;

    for (uint32_t i = 0; i < size; i += 4) ww(rdram, at + i, 0);

    ww(rdram, at, model);

    const uint32_t node0 = at + 0xCu;
    uint32_t leaf = node0 + nodes * 0x4Cu;

    for (uint32_t i = 0; i < leaves; ++i) {
        ww(rdram, leaf + i * 0x5Cu + 0xCu, 4);
    }

    uint32_t lists = rw(rdram, model + 0x18u) + 4u;

    for (uint32_t n = 0; n < nodes; ++n) {
        const uint32_t node = node0 + n * 0x4Cu;
        const uint32_t links = rw(rdram, table + 4u + n * 8u);
        const uint32_t src = rw(rdram, table + 8u + n * 8u);
        static constexpr int kShift[4] = { 24, 18, 12, 6 };

        for (int k = 0; k < 4; ++k) {
            const uint32_t child = (links >> kShift[k]) & 0x3Fu;

            ww(rdram, node + 4u * k, child ? node0 + (child - 1u) * 0x4Cu : 0u);
        }

        ww(rdram, node + 0x14u, rw(rdram, src + 0x14u));
        ww(rdram, node + 0x18u, rw(rdram, src + 0x18u));

        for (uint32_t i = 0; i < 0x30u; i += 4) ww(rdram, node + 0x1Cu + i, rw(rdram, src + 0x1Cu + i));

        if (rw(rdram, lists) == 0) {
            ww(rdram, node + 0x10u, 0);
            lists += 4u;
            continue;
        }

        ww(rdram, node + 0x10u, leaf);

        uint32_t prev = 0;

        while (rw(rdram, lists) != 0) {
            const bool more = rw(rdram, lists + 4u) != 0;

            ww(rdram, leaf, more ? leaf + 0x5Cu : 0u);
            ww(rdram, leaf + 4u, prev);
            ww(rdram, leaf + 8u, rw(rdram, lists));
            ww(rdram, leaf + 0xCu, 4);
            prev = leaf;
            leaf += 0x5Cu;
            lists += 4u;
        }

        lists += 4u;
    }

    return size;
}

// A unit with a path (name at item + item[+0x8C], "-" for none) spawns on it (func_800F947C): the spline starts at the path's second point
// (+0x20 offsets from +0x10, +0x1C the count) facing along (p2 - p0). Walkers stand on the terrain, ground transports keep the DAT's height above it.
void place_on_path(uint8_t* rdram, recomp_context* ctx, uint32_t item, uint32_t state)
{

    const uint32_t offset = rw(rdram, item + 0x8Cu);
    const uint32_t name = item + offset;

    if (offset < 0x90u || offset >= 0x400u || rb(rdram, name) == '-' || rb(rdram, name) == 0) return;

    const uint32_t path = mips_call(rdram, ctx, kDatItemByName, { name }).v0;

    if (path < 0x80000000u || path >= 0x80800000u || rh(rdram, path) != 3 || rw(rdram, path + 0x1Cu) < 3) return;

    const uint32_t pts = rw(rdram, path + 0x20u);

    if (pts < 0x80000000u || pts >= 0x80800000u) return;

    const float x = rf(rdram, path + 0x10u) + rf(rdram, pts + 12u);
    const float y = rf(rdram, path + 0x14u) + rf(rdram, pts + 16u);
    const float z = rf(rdram, path + 0x18u) + rf(rdram, pts + 20u);
    const float dx = rf(rdram, pts + 24u) - rf(rdram, pts);
    const float dz = rf(rdram, pts + 32u) - rf(rdram, pts + 8u);
    const float len = std::sqrt(dx * dx + dz * dz);

    const uint16_t sub = rh(rdram, item);
    const bool walker = sub == 0x0F || sub == 0x1F || sub == 0x47;
    const bool transport = sub == 0x2B || sub == 0x2D || sub == 0x2E;
    const float ground = walker || transport ? mips_call(rdram, ctx, kTerrainHeight, { 0, 0, 0, 0 }, {}, x, z).f0 : 0.0f;
    const float dat_x = rf(rdram, item + 0x10u);
    const float dat_z = rf(rdram, item + 0x18u);
    const float dat_ground = transport ? mips_call(rdram, ctx, kTerrainHeight, { 0, 0, 0, 0 }, {}, dat_x, dat_z).f0 : 0.0f;

    wf(rdram, state, x);
    wf(rdram, state + 4u, walker ? ground : (transport ? rf(rdram, item + 0x14u) + ground - dat_ground : y));
    wf(rdram, state + 8u, z);

    if (len > 1e-4f) {
        wf(rdram, state + 0xCu, dx / len);
        wf(rdram, state + 0x10u, 0.0f);
        wf(rdram, state + 0x14u, dz / len);
        wf(rdram, state + 0x18u, 0.0f);
        wf(rdram, state + 0x1Cu, 1.0f);
        wf(rdram, state + 0x20u, 0.0f);
    }
}

// The prop handler's spawn (message 1): position in row 0, Euler rotation, the instance and the item, and the cull radius = model radius
// (+0x44) times the largest scale component (instance flag 0x400 when y is largest, 0x800 when z is).
void build_state(uint8_t* rdram, recomp_context* ctx, uint32_t item, uint32_t state, uint32_t inst, uint32_t model, uint32_t* scale)
{

    for (uint32_t i = 0; i < kStateSize; i += 4) ww(rdram, state + i, 0);

    for (uint32_t i = 0; i < 12; i += 4) ww(rdram, state + i, rw(rdram, item + 0x10u + i));

    mips_call(rdram, ctx, kComposeEuler, { state, rw(rdram, item + 0x1Cu), rw(rdram, item + 0x20u), rw(rdram, item + 0x24u) });

    for (uint32_t i = 0x24u; i < 0x30u; i += 4) ww(rdram, state + i, 0);

    place_on_path(rdram, ctx, item, state);
    ww(rdram, state + 0x30u, inst);
    ww(rdram, state + 0x34u, item);

    // Walkers (AT-AT, AT-ST, AT-PT handlers) and shuttles draw at a uniform scale of item +0x28 over the model's +0x34, about 10x the item scale for an AT-ST.
    const uint16_t sub = rh(rdram, item);

    if (sub == kSubProbeDroid) ww(rdram, inst + 4u, kProbeLodTable);

    if (sub == 0x0F || sub == 0x1F || sub == 0x47 || sub == 0x24 || sub == 0x2C || sub == 0x48 || sub == 0x4D) {
        const float ws = rf(rdram, item + 0x28u) / rf(rdram, model + 0x34u);
        const float r = rf(rdram, model + 0x44u) * ws;
        uint32_t bits;

        std::memcpy(&bits, &ws, 4);
        std::memcpy(scale, &r, 4);
        mips_call(rdram, ctx, kUniformRootMatrix, { inst + 0x28u, state, bits });

        return;
    }

    const float sx = rf(rdram, item + 0x28u), sy = rf(rdram, item + 0x2Cu), sz = rf(rdram, item + 0x30u);
    float s = sx;
    uint32_t flags = 0;

    // Ships and vehicles (npcDebrisFieldUpdate) cull at 1.25 times the x scale, with no axis flag.
    if (sub == kSubShip) {
        s = sx * 1.25f;
    } else if (sx < sy) {
        s = sy;
        flags = 0x400u;

        if (!(sz < sy)) {
            s = sz;
            flags = 0x800u;
        }
    } else if (sx < sz) {
        s = sz;
        flags = 0x800u;
    }

    wh(rdram, inst + 8u, (uint16_t)(rh(rdram, inst + 8u) | flags));

    const float r = rf(rdram, model + 0x44u) * s;

    std::memcpy(scale, &r, 4);
    ww(rdram, state + 0x38u, *scale);

    // The root node's world matrix: the rotation scaled per axis by the item's scale (+0x28), at its position.
    mips_call(rdram, ctx, kRootMatrix, { inst + 0x28u, state, item + 0x28u });
}

uint32_t bits(float f)
{

    uint32_t u;

    std::memcpy(&u, &f, 4);

    return u;
}

// A structure respawns destroyed as attachDebrisModelVariant (0x8006C9C0) builds it: item +0x90 picks bld_a/b/c_dst or fueltnk_dst (1-4), none (5)
// or its own model; each axis scales by item +0x94.. times 0.25 when set, else by its own scale times the intact model's +0x34; then its rotation and position.
uint32_t build_wreck(uint8_t* rdram, recomp_context* ctx, uint32_t item, uint32_t intact, uint32_t at, uint32_t* radius)
{

    const uint32_t kind = rw(rdram, item + 0x90u);

    if (kind == 5) return 0;

    const uint32_t name = (kind >= 1 && kind <= 4) ? kDebrisNames + (kind - 1) * 12u : rw(rdram, item + 0x5Cu);
    const uint32_t model = (name >= 0x80000000u && name < 0x80800000u) ? find_model(rdram, name) : 0;

    if (model == 0) return 0;

    const uint32_t size = build_instance(rdram, ctx, model, at);

    if (size == 0) return 0;

    float s[3];

    for (uint32_t k = 0; k < 3; ++k) {
        const float set = rf(rdram, item + 0x94u + k * 4u);

        s[k] = set != 0.0f ? set * rf(rdram, kDebrisScale + k * 4u) : rf(rdram, item + 0x28u + k * 4u) * rf(rdram, intact + 0x34u);
    }

    const uint32_t m = at + 0x28u;

    mips_call(rdram, ctx, kScaleMatrix, { m, bits(s[0]), bits(s[1]), bits(s[2]) });
    mips_call(rdram, ctx, kRotateX, { m, rw(rdram, item + 0x1Cu) });
    mips_call(rdram, ctx, kRotateY, { m, rw(rdram, item + 0x20u) });
    mips_call(rdram, ctx, kRotateZ, { m, rw(rdram, item + 0x24u) });
    mips_call(rdram, ctx, kTranslate, { m, rw(rdram, item + 0x10u), rw(rdram, item + 0x14u), rw(rdram, item + 0x18u) });

    *radius = bits(rf(rdram, model + 0x44u) * std::max({ s[0], s[1], s[2] }));

    return size;
}

// The model a turret (npcTurretUpdate) or AT-ST (npcAtStUpdate) loads when it respawns with its spawn count spent, 0 for units without one.
uint32_t unit_wreck_name(uint16_t sub)
{

    switch (sub) {
    case 0x0D: return 0x800A6A30u;
    case 0x1E: return 0x800A6A30u;
    case 0x28: return 0x800A6A0Cu;
    case 0x41: return 0x800A6A18u;
    case 0x42: return 0x800A6A24u;
    case 0x1F: return 0x800A7060u;
    default: return 0;
    }
}

// A destroyed turret or AT-ST: the same pose with its wreck model. The AT-ST's uniform scale is the item's over the loaded model's +0x34, as build_state does.
uint32_t build_unit_wreck(uint8_t* rdram, recomp_context* ctx, const Standin& s, uint32_t at)
{

    const uint16_t sub = rh(rdram, s.item);
    const uint32_t name = unit_wreck_name(sub);
    const uint32_t model = name ? find_model(rdram, name) : 0;

    if (model == 0) return 0;

    const uint32_t size = build_instance(rdram, ctx, model, at);

    if (size == 0) return 0;

    if (sub == 0x1F)
    {
        mips_call(rdram, ctx, kUniformRootMatrix, { at + 0x28u, s.state, bits(rf(rdram, s.item + 0x28u) / rf(rdram, model + 0x34u)) });

        return size;
    }

    wh(rdram, at + 8u, rh(rdram, s.inst + 8u));

    for (uint32_t i = 0; i < 0x30u; i += 4) ww(rdram, at + 0x28u + i, rw(rdram, s.inst + 0x28u + i));

    return size;
}

void rebuild(uint8_t* rdram, recomp_context* ctx, uint32_t dat)
{

    static bool s_registered = false;

    if (!s_registered) {
        rs64_snapshot_add_range(kRegion - 0x80000000u, kRegionEnd - 0x80000000u);
        s_registered = true;
    }

    s_standins.clear();
    s_dat = dat;

    const uint32_t count = rw(rdram, dat);
    const uint32_t list = rw(rdram, dat + 4u);
    uint32_t at = kRegion;
    uint32_t wrecks = 0;

    for (uint32_t i = 0; i < count && i < 4096; ++i) {
        const uint32_t item = rw(rdram, list + i * 4u);
        const uint16_t sub = rh(rdram, item);
        const uint32_t name = names_in_item(sub) ? rw(rdram, item + 0x5Cu) : handler_model_name(sub);

        if (name < 0x80000000u || name >= 0x80800000u) continue;

        const uint32_t model = find_model(rdram, name);

        if (model == 0) continue;

        const uint32_t state = at;
        const uint32_t inst = at + kStateSize;
        const uint32_t size = build_instance(rdram, ctx, model, inst);

        if (size == 0) break;

        Standin s{ item, state, inst, 0, 0, 0 };

        build_state(rdram, ctx, item, state, inst, model, &s.scale);
        at = (inst + size + 15u) & ~15u;

        uint32_t wreck_size = 0;

        if (sub == kSubStructure) {
            wreck_size = build_wreck(rdram, ctx, item, model, at, &s.wreck_scale);
        } else if ((wreck_size = build_unit_wreck(rdram, ctx, s, at)) != 0) {
            s.wreck_scale = s.scale;
        }

        if (wreck_size != 0)
        {
            s.wreck = at;
            at = (at + wreck_size + 15u) & ~15u;
            ++wrecks;
        }

        s_standins.push_back(s);
    }

    fprintf(stderr, "[standins] %zu objects, %u wrecks, %u KB\n", s_standins.size(), wrecks, (at - kRegion) / 1024u);
}

}

// runInMissionFrame 0x800FAC68, after every NPC got the render message ($s2 = the message, +0 the view): items the game would still spawn here
// go into the visibility bucket at their DAT pose, as a spawned prop's render does. Only above 1x draw distance.
extern "C" void rs64_standins_submit(uint8_t* rdram, recomp_context* ctx)
{

    const uint32_t dat = rw(rdram, kDatFile);

    if (!(rs64_draw_dist() > 1.0f) || dat < 0x80000000u || dat >= 0x80800000u) return;

    // A new level or a restart (the mission frame counter goes back) reloads the models, so the instances are rebuilt.
    static uint32_t s_level = ~0u, s_frame = 0;
    const uint32_t level = rw(rdram, 0x80130B70u);
    const uint32_t frame = rw(rdram, 0x8013889Cu);

    if (dat != s_dat || level != s_level || frame < s_frame) {
        s_level = level;
        rebuild(rdram, ctx, dat);
    }

    s_frame = frame;

    const uint32_t view = rw(rdram, (uint32_t)ctx->r18);
    const uint32_t diff = rw(rdram, kDifficulty) & 3u;
    // Items the game still spawns on approach are linked in a DAT grid cell (updateActiveGridCellState 0x8004736C moves a spawned
    // item's entry to the free list): 12-byte entries, +2 next, +4 type (3 = item), +8 the item.
    static std::vector<uint32_t> s_linked;
    const uint32_t cells = rw(rdram, kDatFile + 0x14u) * rw(rdram, kDatFile + 0x18u);
    const uint32_t heads = rw(rdram, kDatFile + 0x1Cu);
    const uint32_t entries = rw(rdram, kGridEntries);

    s_linked.clear();

    for (uint32_t c = 0; c < cells && c < 0x10000u; ++c) {
        uint32_t idx = rh(rdram, heads + c * 2u);

        for (int hops = 0; idx != 0xFFFFu && hops < 4096; ++hops) {
            const uint32_t e = entries + idx * 12u;

            if (rh(rdram, e + 4u) == 3) s_linked.push_back(rw(rdram, e + 8u));

            idx = rh(rdram, e + 2u);
        }
    }

    std::sort(s_linked.begin(), s_linked.end());

    const bool cutscene = rb(rdram, kCutsceneFlag) != 0;

    // The game's spawn window (updateActiveGridCellState): the cells within kGridHalfCells of the reference point. A stand-in drawn inside it is a unit
    // the game has not spawned there (a path unit away from its DAT cell), so it shows nothing yet and neither does the stand-in.
    const float scale = rf(rdram, kGridScale);
    const float half = rf(rdram, kGridHalfCells);
    const float origin_x = rf(rdram, kDatFile + 4u);
    const float origin_z = rf(rdram, kDatFile + 8u);
    const float ref_x = (rf(rdram, kSpawnRef) - origin_x) * scale;
    const float ref_z = (rf(rdram, kSpawnRef + 8u) - origin_z) * scale;

    const auto in_spawn_window = [&](uint32_t state) {

        const int cx = (int)((rf(rdram, state) - origin_x) * scale);
        const int cz = (int)((rf(rdram, state + 8u) - origin_z) * scale);

        return cx >= (int)(ref_x - half) && cx <= (int)(ref_x + half) && cz >= (int)(ref_z - half) && cz <= (int)(ref_z + half);
    };

    for (const Standin& s : s_standins) {
        if (rh(rdram, s.item + 6u) != 0xFFFFu) continue;

        if (!std::binary_search(s_linked.begin(), s_linked.end(), s.item)) continue;

        // A spent spawn count means destroyed: the game respawns a structure as its wreck, anything else not at all.
        const bool destroyed = (int32_t)rw(rdram, s.item + 0x6Cu + diff * 4u) <= 0;

        if (destroyed && s.wreck == 0) continue;

        // Trigger and wave gates: isDatItemConditionMet reads only objective flags and counters.
        if (!cutscene && mips_call(rdram, ctx, kConditionMet, { s.item }).v0 == 0) continue;

        if (!destroyed && in_spawn_window(s.state)) continue;

        // A probe droid grows with distance, so it goes through npcProbeDriod's own render call. The mark gives it the draw distance in replays too.
        if (rh(rdram, s.item) == kSubProbeDroid) {
            rs64_npc_draw_only_mark(1);
            mips_call(rdram, ctx, kSelectLodAndSubmit, { view, s.inst, s.state, rw(rdram, s.item + 0x28u) }, { rw(rdram, kProbeSizeBonus) });
            rs64_npc_draw_only_mark(0);
            continue;
        }

        mips_call(rdram, ctx, kAddToBucket, { view, destroyed ? s.wreck : s.inst, s.state, destroyed ? s.wreck_scale : s.scale });
    }
}
