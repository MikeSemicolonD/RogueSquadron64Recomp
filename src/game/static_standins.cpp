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
constexpr uint16_t kSubMovingSpawn = 0x46u;
constexpr uint32_t kRootMatrix = 0x80059C24u;
constexpr uint32_t kUniformRootMatrix = 0x80059B50u;
constexpr uint32_t kConditionMet = 0x80065828u;
constexpr uint32_t kGridEntries = 0x80136FCCu;
constexpr uint32_t kGridScale = 0x8003A7B8u;
constexpr uint32_t kGridHalfCells = 0x8003A7BCu;
constexpr uint32_t kSpawnRef = 0x801375F8u;
constexpr uint32_t kSpawnRefMode = 0x8010CA1Cu;
constexpr uint32_t kCraftRecord = 0x8006C8F0u;
constexpr uint32_t kSpawnRangeScale = 0x8003A7D4u;
constexpr uint32_t kSpawnRangeSq = 0x8003A7D8u;
constexpr uint32_t kCutsceneFlag = 0x80130B39u;
constexpr uint32_t kDatItemByName = 0x80047B48u;
constexpr uint32_t kTerrainHeight = 0x80068180u;
constexpr uint32_t kSplineEval = 0x80018F98u;
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
constexpr uint32_t kIdentity = 0x8001CA50u;
constexpr uint32_t kApplyScale = 0x8001CB04u;
constexpr uint32_t kComposeQuat = 0x8001DAECu;

constexpr uint16_t kSubShield = 0x32u;
constexpr uint16_t kSubRadarDish = 0x2Au;
constexpr uint16_t kSubPowerGenerator = 0x4Au;
constexpr uint16_t kSubPowerUp = 0x4Cu;
constexpr uint32_t kObjectiveCounts = 0x80138060u;
constexpr uint32_t kObjectiveBooleans = 0x801388A0u;
constexpr uint32_t kMissionSeconds = 0x80130BA4u;
constexpr uint32_t kDishSpinOn = 0x801399FBu;

// wreck/wreck_scale: a destroyed structure's model, 0 when it has none. spin_*: the nodes its handler turns about Y, spin_rate degrees per second.
struct Standin {
    uint32_t item;
    uint32_t state;
    uint32_t inst;
    uint32_t scale;
    uint32_t wreck;
    uint32_t wreck_scale;
    uint32_t spin_first = 0;
    uint32_t spin_count = 0;
    float spin_rate = 0.0f;
};

// turns: this node's angle as a multiple of the stand-in's spin angle.
struct SpinNode {
    uint32_t node;
    uint32_t rec;
    float turns;
};

uint32_t s_dat = 0;
std::vector<Standin> s_standins;
std::vector<SpinNode> s_spins;

bool names_in_item(uint16_t sub)
{

    switch (sub) {
    case 0x14: case 0x21: case 0x33: case 0x36: case 0x44: case 0x46: case 0x4A: case 0x4B: case 0x4F:
        return true;
    default:
        return false;
    }
}

// Subtypes whose handler picks the model itself (mission overlay strings, the undamaged model): turrets, walkers, droids, radar dish, shield dome,
// shuttles (npcShuttleUpdate), ground transports (npcGroundTransportUpdate), TIEs (npcTieVariantUpdate), train cars, power-ups, water craft (npcWaterCraftUpdate).
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
    case 0x3E: return 0x800A83ACu;
    case 0x40: return 0x800A83BCu;
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

// A unit with a path (name at item + item[+0x8C], +0x90 for moving spawns, "-" for none) spawns on it (func_800F947C): the spline starts at the path's second point (+0x20 offsets from +0x10,
// +0x1C the count) facing along (p2 - p0). Walkers, moving spawns and water craft sit on the game's height grid (the water surface for boats), ground transports keep the DAT's height above it.
void place_on_path(uint8_t* rdram, recomp_context* ctx, uint32_t item, uint32_t state)
{

    const uint32_t offset = rw(rdram, item + (rh(rdram, item) == kSubMovingSpawn ? 0x90u : 0x8Cu));
    const uint32_t name = item + offset;

    if (offset < 0x90u || offset >= 0x400u || rb(rdram, name) == '-' || rb(rdram, name) == 0) return;

    const uint32_t path = mips_call(rdram, ctx, kDatItemByName, { name }).v0;

    if (path < 0x80000000u || path >= 0x80800000u || rh(rdram, path) != 3 || rw(rdram, path + 0x1Cu) < 3) return;

    const uint32_t pts = rw(rdram, path + 0x20u);

    if (pts < 0x80000000u || pts >= 0x80800000u) return;

    float x = rf(rdram, path + 0x10u) + rf(rdram, pts + 12u);
    float y = rf(rdram, path + 0x14u) + rf(rdram, pts + 16u);
    float z = rf(rdram, path + 0x18u) + rf(rdram, pts + 20u);
    float dx = rf(rdram, pts + 24u) - rf(rdram, pts);
    float dz = rf(rdram, pts + 32u) - rf(rdram, pts + 8u);

    const uint16_t sub = rh(rdram, item);

    // Water craft start item +0x9C seconds along at item +0x90 units per second (npcWaterCraftUpdate -> initActorAsType1, path +0x24 its length, past the end back to 0),
    // placed as buildPositionAndCallTransformHelper does: evalCubicSplineAtTime at 0.999t + 0.001, heading from 0.0009 earlier. Spline {count, points, knots} and output go in the state's unused tail.
    const float t = (sub == 0x3E || sub == 0x40) && rf(rdram, path + 0x24u) > 0.0f ? rf(rdram, item + 0x9Cu) * rf(rdram, item + 0x90u) / rf(rdram, path + 0x24u) : 0.0f;

    if (t > 0.0f && t <= 1.0f)
    {
        const uint32_t spline = state + 0x38u;
        const uint32_t out = state + 0x44u;
        const float u = t * 0.999f + 0.001f;

        ww(rdram, spline, rw(rdram, path + 0x1Cu));
        ww(rdram, spline + 4u, pts);
        ww(rdram, spline + 8u, pts + rw(rdram, path + 0x1Cu) * 12u);
        mips_call(rdram, ctx, kSplineEval, { spline, float_bits(u - 0.0009f), out });

        const float bx = rf(rdram, out), bz = rf(rdram, out + 8u);

        mips_call(rdram, ctx, kSplineEval, { spline, float_bits(u), out });
        x = rf(rdram, path + 0x10u) + rf(rdram, out);
        y = rf(rdram, path + 0x14u) + rf(rdram, out + 4u);
        z = rf(rdram, path + 0x18u) + rf(rdram, out + 8u);
        dx = rf(rdram, out) - bx;
        dz = rf(rdram, out + 8u) - bz;

        for (uint32_t i = 0x38u; i < kStateSize; i += 4) ww(rdram, state + i, 0);
    }

    const float len = std::sqrt(dx * dx + dz * dz);

    const bool walker = sub == 0x0F || sub == 0x1F || sub == 0x47 || sub == kSubMovingSpawn || sub == 0x3E || sub == 0x40;
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

    // npcRadarDishUpdate turns its dish 180 degrees from the DAT yaw.
    const uint16_t sub = rh(rdram, item);
    const uint32_t yaw = sub == kSubRadarDish ? float_bits(rf(rdram, item + 0x20u) + 180.0f) : rw(rdram, item + 0x20u);

    mips_call(rdram, ctx, kComposeEuler, { state, rw(rdram, item + 0x1Cu), yaw, rw(rdram, item + 0x24u) });

    for (uint32_t i = 0x24u; i < 0x30u; i += 4) ww(rdram, state + i, 0);

    place_on_path(rdram, ctx, item, state);
    ww(rdram, state + 0x30u, inst);
    ww(rdram, state + 0x34u, item);

    if (sub == kSubProbeDroid) ww(rdram, inst + 4u, kProbeLodTable);

    // The dish scales each axis by the item's scale over the model's +0x34 and culls at its +0x44 times the x scale.
    if (sub == kSubRadarDish)
    {
        const float k = 1.0f / rf(rdram, model + 0x34u);
        const float r = rf(rdram, model + 0x44u) * rf(rdram, item + 0x28u) * k;
        const uint32_t m = inst + 0x28u;

        std::memcpy(scale, &r, 4);
        ww(rdram, state + 0x38u, *scale);
        mips_call(rdram, ctx, kScaleMatrix, { m, float_bits(rf(rdram, item + 0x28u) * k), float_bits(rf(rdram, item + 0x2Cu) * k), float_bits(rf(rdram, item + 0x30u) * k) });
        mips_call(rdram, ctx, kRotateX, { m, rw(rdram, item + 0x1Cu) });
        mips_call(rdram, ctx, kRotateY, { m, yaw });
        mips_call(rdram, ctx, kRotateZ, { m, rw(rdram, item + 0x24u) });
        mips_call(rdram, ctx, kTranslate, { m, rw(rdram, state), rw(rdram, state + 4u), rw(rdram, state + 8u) });

        return;
    }

    // Walkers (AT-AT, AT-ST, AT-PT handlers), shuttles and water craft draw at a uniform scale of item +0x28 over the model's +0x34, about 10x the item scale for an AT-ST.

    if (sub == 0x0F || sub == 0x1F || sub == 0x47 || sub == 0x24 || sub == 0x2C || sub == 0x48 || sub == 0x4D || sub == 0x3E || sub == 0x40) {
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

// lookupHmtMaterialByName 0x80059434: the model's +0x28 table of 10-byte entries (9-byte name, node index) gives the instance node and the model's node record.
void add_spin_node(const uint8_t* rdram, uint32_t inst, const char* name, float turns)
{

    const uint32_t model = rw(rdram, inst);
    const uint32_t names = rw(rdram, model + 0x28u);

    if (names == 0) return;

    for (uint32_t e = names; rb(rdram, e) != 0 && e < names + 2560u; e += 10u) {
        if (rstr(rdram, e, 9) != name) continue;

        const uint32_t idx = rb(rdram, e + 9u);
        const uint32_t rec = rw(rdram, rw(rdram, model + 0x14u) + 8u + idx * 8u);

        s_spins.push_back({ inst + 0xCu + idx * 0x4Cu, rec, turns });
        return;
    }
}

// The parts a handler turns about Y while the object is alive: the radar dish's top (npcRadarDishUpdate, rate item +0x94 when +0x98 is set), the power
// generator's base, main ring and eight cable rings (npcPowerGeneratorUpdate, 1, -1 and 2 times item +0x94) and the power-up (npcPowerUpUpdate, 90 degrees a second).
void add_spins(const uint8_t* rdram, Standin& s)
{

    const uint16_t sub = rh(rdram, s.item);

    s.spin_first = (uint32_t)s_spins.size();

    if (sub == kSubRadarDish && rw(rdram, s.item + 0x98u) != 0) {
        s.spin_rate = rf(rdram, s.item + 0x94u) * 30.0f;
        add_spin_node(rdram, s.inst, "top_tran", 1.0f);
    } else if (sub == kSubPowerGenerator) {
        s.spin_rate = rf(rdram, s.item + 0x94u) * 30.0f;
        add_spin_node(rdram, s.inst, "base", 1.0f);
        add_spin_node(rdram, s.inst, "maintrns", -1.0f);

        char ring[] = "cb_trns1";

        for (int i = 0; i < 8; ++i) {
            ring[7] = (char)('1' + i);
            add_spin_node(rdram, s.inst, ring, 2.0f);
        }
    } else if (sub == kSubPowerUp) {
        s.spin_rate = 90.0f;
        add_spin_node(rdram, s.inst, "rot_a", 1.0f);
    }

    s.spin_count = (uint32_t)s_spins.size() - s.spin_first;
}

// The handlers' node pose: about the record's pivot (+0x50, flag 1) with its scale (+0x5C, flag 2) and quaternion (+0x68, flag 4), a turn about Y,
// a lift (the power-up's bob), then the pivot and offset (+0x78, flag 8) back.
void pose_node(uint8_t* rdram, recomp_context* ctx, const SpinNode& n, float yaw, float lift)
{

    const uint32_t m = n.node + 0x1Cu;
    const uint32_t flags = rw(rdram, n.rec + 0x4Cu);
    const float px = rf(rdram, n.rec + 0x50u);
    const float py = rf(rdram, n.rec + 0x54u);
    const float pz = rf(rdram, n.rec + 0x58u);

    mips_call(rdram, ctx, kIdentity, { m });

    if (flags & 1u) mips_call(rdram, ctx, kTranslate, { m, float_bits(-px), float_bits(-py), float_bits(-pz) });

    if (flags & 2u) mips_call(rdram, ctx, kApplyScale, { m, rw(rdram, n.rec + 0x5Cu), rw(rdram, n.rec + 0x60u), rw(rdram, n.rec + 0x64u) });

    if (flags & 4u) mips_call(rdram, ctx, kComposeQuat, { m, n.rec + 0x68u });

    mips_call(rdram, ctx, kRotateY, { m, float_bits(yaw) });

    if (lift != 0.0f) mips_call(rdram, ctx, kTranslate, { m, 0, float_bits(lift), 0 });

    const float ox = rf(rdram, n.rec + 0x78u);
    const float oy = rf(rdram, n.rec + 0x7Cu);
    const float oz = rf(rdram, n.rec + 0x80u);

    if ((flags & 9u) == 9u) {
        mips_call(rdram, ctx, kTranslate, { m, float_bits(ox + px), float_bits(oy + py), float_bits(oz + pz) });
    } else if ((flags & 9u) == 8u) {
        mips_call(rdram, ctx, kTranslate, { m, float_bits(ox), float_bits(oy), float_bits(oz) });
    } else if (flags & 1u) {
        mips_call(rdram, ctx, kTranslate, { m, float_bits(px), float_bits(py), float_bits(pz) });
    }
}

// npcShieldUpdate collapses once flag +0x7C equals +0x80 or counter +0x74 reaches +0x78 (1-based, 0 = none), and is then never relinked to its grid cell.
bool shield_down(const uint8_t* rdram, uint32_t item)
{

    const uint32_t flag = rw(rdram, item + 0x7Cu);
    const uint32_t counter = rw(rdram, item + 0x74u);

    if (flag != 0 && rb(rdram, kObjectiveBooleans + flag - 1u) == rw(rdram, item + 0x80u)) return true;

    return counter != 0 && (int32_t)rw(rdram, kObjectiveCounts + (counter - 1u) * 4u) >= (int32_t)rw(rdram, item + 0x78u);
}

void rebuild(uint8_t* rdram, recomp_context* ctx, uint32_t dat)
{

    static bool s_registered = false;

    if (!s_registered) {
        rs64_snapshot_add_range(kRegion - 0x80000000u, kRegionEnd - 0x80000000u);
        s_registered = true;
    }

    s_standins.clear();
    s_spins.clear();
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

        add_spins(rdram, s);
        s_standins.push_back(s);
    }

    fprintf(stderr, "[standins] %zu objects, %u wrecks, %zu spinning parts, %u KB\n", s_standins.size(), wrecks, s_spins.size(), (at - kRegion) / 1024u);
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
    static std::vector<uint32_t> s_linked, s_was_linked;
    const uint32_t cells = rw(rdram, kDatFile + 0x14u) * rw(rdram, kDatFile + 0x18u);
    const uint32_t heads = rw(rdram, kDatFile + 0x1Cu);
    const uint32_t entries = rw(rdram, kGridEntries);

    s_was_linked.swap(s_linked);
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

    // The game's spawn window (updateActiveGridCellState): cells within kGridHalfCells of player 1's craft, or of kSpawnRef (the last cutscene's point) while the cutscene bit is set.
    const uint32_t ref = (rw(rdram, kSpawnRefMode) & 8u) ? kSpawnRef : mips_call(rdram, ctx, kCraftRecord, { 0 }).v0;
    const float scale = rf(rdram, kGridScale);
    const float half = rf(rdram, kGridHalfCells);
    const float origin_x = rf(rdram, kDatFile + 4u);
    const float origin_z = rf(rdram, kDatFile + 8u);
    const float ref_x = (rf(rdram, ref) - origin_x) * scale;
    const float ref_z = (rf(rdram, ref + 8u) - origin_z) * scale;
    const float range_scale = rf(rdram, kSpawnRangeScale);
    const float range_sq = rf(rdram, kSpawnRangeSq);

    // Inside the square the game spawns an item only within isPointXzWithinRangeOfRef's circle, so the square's corners stay stand-ins.
    const auto in_spawn_window = [&](uint32_t state) {

        const int cx = (int)((rf(rdram, state) - origin_x) * scale);
        const int cz = (int)((rf(rdram, state + 8u) - origin_z) * scale);
        const float dx = (rf(rdram, state) - rf(rdram, ref)) * range_scale;
        const float dz = (rf(rdram, state + 8u) - rf(rdram, ref + 8u)) * range_scale;

        if (range_sq < dx * dx + dz * dz) return false;

        // The game's cell loops stop before the upper bound, which is clamped to the last row or column.
        const int max_x = std::min((int)(ref_x + half), (int)rw(rdram, kDatFile + 0x14u) - 1);
        const int max_z = std::min((int)(ref_z + half), (int)rw(rdram, kDatFile + 0x18u) - 1);

        return cx >= (int)(ref_x - half) && cx < max_x && cz >= (int)(ref_z - half) && cz < max_z;
    };

    for (const Standin& s : s_standins) {
        // The game first draws a unit the frame after it spawns, so the stand-in covers the spawn frame.
        const bool spawned = rh(rdram, s.item + 6u) != 0xFFFFu;
        const bool linked = std::binary_search(s_linked.begin(), s_linked.end(), s.item);
        const bool spawn_frame = spawned && !linked && std::binary_search(s_was_linked.begin(), s_was_linked.end(), s.item);

        if (!spawn_frame && (spawned || !linked)) continue;

        // A spent spawn count means destroyed: the game respawns a structure as its wreck, anything else not at all. A shield has no spawn
        // count (its +0x74 is a counter index), so it goes only when shield_down.
        const bool shield = rh(rdram, s.item) == kSubShield;
        const bool destroyed = !shield && (int32_t)rw(rdram, s.item + 0x6Cu + diff * 4u) <= 0;

        if (destroyed && s.wreck == 0) continue;

        if (shield && shield_down(rdram, s.item)) continue;

        // Trigger and wave gates: isDatItemConditionMet reads only objective flags and counters.
        if (!cutscene && mips_call(rdram, ctx, kConditionMet, { s.item }).v0 == 0) continue;

        // The game spawns by the DAT position, so the window can only hold a path unit drawn at its path start with its DAT spot still out of range.
        // Only walkers (the Gerrard V AT-PT) hide there, as a stand-in you can't shoot; boats, ships and other path units stay drawn until they spawn in place.
        const uint16_t sub = rh(rdram, s.item);
        const bool walker = sub == 0x0F || sub == 0x1F || sub == 0x47;

        if (!destroyed && walker && in_spawn_window(s.state)) continue;

        // Spinning parts follow mission time; a dish holds its pose while byte 0x801399FB (1 from load_dat_file_assets) is cleared.
        const bool dish_held = rh(rdram, s.item) == kSubRadarDish && rb(rdram, kDishSpinOn) != 1;

        if (!destroyed && s.spin_count != 0 && !dish_held)
        {
            const float t = rf(rdram, kMissionSeconds);
            const float lift = rh(rdram, s.item) == kSubPowerUp ? std::sin((t - std::floor(t)) * 6.2831853f) * 0.5f : 0.0f;

            for (uint32_t i = 0; i < s.spin_count; ++i) {
                const SpinNode& n = s_spins[s.spin_first + i];

                pose_node(rdram, ctx, n, std::fmod(s.spin_rate * n.turns * t, 360.0f), lift);
            }
        }

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
