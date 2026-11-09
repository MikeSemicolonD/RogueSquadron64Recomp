// Draw-only stand-ins for level objects and ground units the game has not spawned yet, so they show at long draw distance: a model instance and prop state
// block in host RAM, built as the prop handler (0x8006CBB0) spawns one, go through the game's visibility bucket. Nothing the game reads is written.
#include "util/mips_call.h"
#include "util/rdram_words.h"
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
constexpr uint32_t kRootMatrix = 0x80059C24u;
constexpr uint32_t kUniformRootMatrix = 0x80059B50u;
constexpr uint32_t kConditionMet = 0x80065828u;
constexpr uint32_t kGridEntries = 0x80136FCCu;
constexpr uint32_t kCutsceneFlag = 0x80130B39u;
constexpr uint32_t kRegion = 0x80A5C000u;
constexpr uint32_t kRegionEnd = 0x80B00000u;
constexpr uint32_t kStateSize = 0x50u;

struct Standin {
    uint32_t item;
    uint32_t state;
    uint32_t inst;
    uint32_t scale;
};

uint32_t s_dat = 0;
std::vector<Standin> s_standins;

bool names_in_item(uint16_t sub)
{

    switch (sub) {
    case 0x14: case 0x33: case 0x36: case 0x44: case 0x46: case 0x4A: case 0x4B: case 0x4F:
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

// The prop handler's spawn (message 1): position in row 0, Euler rotation, the instance and the item, and the cull radius = model radius
// (+0x44) times the largest scale component (instance flag 0x400 when y is largest, 0x800 when z is).
void build_state(uint8_t* rdram, recomp_context* ctx, uint32_t item, uint32_t state, uint32_t inst, uint32_t model, uint32_t* scale)
{

    for (uint32_t i = 0; i < kStateSize; i += 4) ww(rdram, state + i, 0);

    for (uint32_t i = 0; i < 12; i += 4) ww(rdram, state + i, rw(rdram, item + 0x10u + i));

    mips_call(rdram, ctx, kComposeEuler, { state, rw(rdram, item + 0x1Cu), rw(rdram, item + 0x20u), rw(rdram, item + 0x24u) });

    for (uint32_t i = 0x24u; i < 0x30u; i += 4) ww(rdram, state + i, 0);

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

    if (sx < sy) {
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

        Standin s{ item, state, inst, 0 };

        build_state(rdram, ctx, item, state, inst, model, &s.scale);
        s_standins.push_back(s);
        at = (inst + size + 15u) & ~15u;
    }

    fprintf(stderr, "[standins] %zu objects, %u KB\n", s_standins.size(), (at - kRegion) / 1024u);
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

    for (const Standin& s : s_standins) {
        if (rh(rdram, s.item + 6u) != 0xFFFFu) continue;

        if (!std::binary_search(s_linked.begin(), s_linked.end(), s.item)) continue;

        if ((int32_t)rw(rdram, s.item + 0x6Cu + diff * 4u) <= 0) continue;

        // Trigger and wave gates: isDatItemConditionMet reads only objective flags and counters.
        if (!cutscene && mips_call(rdram, ctx, kConditionMet, { s.item }).v0 == 0) continue;

        // A probe droid grows with distance, so it goes through npcProbeDriod's own render call. The mark gives it the draw distance in replays too.
        if (rh(rdram, s.item) == kSubProbeDroid) {
            rs64_npc_draw_only_mark(1);
            mips_call(rdram, ctx, kSelectLodAndSubmit, { view, s.inst, s.state, rw(rdram, s.item + 0x28u) }, { rw(rdram, kProbeSizeBonus) });
            rs64_npc_draw_only_mark(0);
            continue;
        }

        mips_call(rdram, ctx, kAddToBucket, { view, s.inst, s.state, s.scale });
    }
}
