// Ghost co-op tow cables: the other player's AT-AT trips applied here, and their cable drawn by the game's own tow-cable code on a mirror pool.
#include "session/ghost_internal.h"
#include "core/mp_core.h"
#include "util/rdram_words.h"
#include "mp_host.h"
#include <algorithm>
#include <cstdio>
#include <vector>

namespace rs64::ghost {

using namespace rs64::ls;
using rs64::mips::rw;
using rs64::mips::ww;

// The other player's tow-cable trips: this world's copy gets the harpoon's own trip message (action 9, kind 0xE at +0x18), with sender slot 0xFFFF so the walker's release reply goes nowhere. An unspawned walker waits until it spawns.
void apply_remote_tows(uint8_t* rdram, recomp_context* ctx)
{

    Ghost& s = g();

    if (s.tow_in.empty() || s.frame < kDamageSettleFrames || rs64::mp::in_cutscene())
    {
        return;
    }

    std::vector<uint16_t> wait;

    for (uint16_t i : s.tow_in)
    {
        const uint32_t item = dat_item(rdram, i);

        if (!item || (rw(rdram, item + 0x4C) & 0x80000000u) || dat_health(rdram, item) <= 0)
        {
            continue;
        }

        const uint32_t slot = rs64::mips::rh(rdram, item + 6);

        if (slot == 0xFFFFu || !dat_damage_safe(rdram, item)) {
            wait.push_back(i);
            continue;
        }

        const uint32_t msg = 0x80B40140u;

        for (uint32_t k = 0; k < 0x20u; k += 4) {
            ww(rdram, msg + k, 0u);
        }

        ww(rdram, msg + 0x0, 0xFFFF0000u);
        ww(rdram, msg + 0x4, 0xFFFFFFFFu);
        wb(rdram, msg + 0x18, 0x0Eu);
        s.tow_injecting = true;
        rs64::mp::call(rdram, ctx, 0x8003E8DCu, {slot, 9u, msg});
        s.tow_injecting = false;

        if (rw(rdram, item + 0x4C) & 0x80000000u)
        {
            s.tow_retry.erase(i);

            fprintf(stderr, "[ghost] walker %u tripped for the other player\n", i);
            continue;
        }

        // Not taken (the walker ignored the message in its current state): retry for ~2 s, then let the other player's mirrored damage reach it again.
        if (s.tow_retry[i]++ < 60)
        {
            wait.push_back(i);
        } else {
            s.tow_retry.erase(i);

            fprintf(stderr, "[ghost] walker %u did not take the other player's trip; mirrored damage applies\n", i);
        }
    }

    s.tow_in.swap(wait);
}

// The snowspeeder's tow cable: one segment pool (pointer 0x8010B6E0, kCableStride bytes each: start +0, 4x3 transform +0x10, mesh instance +0x40, active +0xF4) and one state (0x8010B6E8: phase, head index, count).
// The other player's cable runs the game's own loader, renderer and free on a mirror pool, with the pool, state and shot id (0x8010B738, which the loader clears) swapped in for the call.
constexpr uint32_t kCablePool = 0x8010B6E0u;
constexpr uint32_t kCableState = 0x8010B6E8u;
constexpr uint32_t kCableShot = 0x8010B738u;
constexpr uint32_t kCableStride = 0xF8u;
constexpr uint8_t kSnowspeeder = 4;

struct CableSwap {
    uint8_t* rdram;
    uint32_t pool;
    uint32_t shot;
    uint8_t state[3];

    CableSwap(uint8_t* r, uint32_t mirror, const rs64::net::TowSeg* t) : rdram(r), pool(rw(r, kCablePool)), shot(rw(r, kCableShot)) {
        for (uint32_t i = 0; i < 3; ++i) {
            state[i] = rb(r, kCableState + i);
        }

        ww(r, kCablePool, mirror);

        if (t) {
            wb(r, kCableState + 0, t->phase);
            wb(r, kCableState + 1, t->head);
            wb(r, kCableState + 2, t->count);
        }
    }

    ~CableSwap() {
        ww(rdram, kCablePool, pool);
        ww(rdram, kCableShot, shot);

        for (uint32_t i = 0; i < 3; ++i) {
            wb(rdram, kCableState + i, state[i]);
        }
    }
};

// The ring's size: the renderer's wrap (0x800ADDD0, addiu a0, a0, n): 60, or more with a long-cable mod.
uint32_t cable_ring(const uint8_t* rdram) {

    const uint32_t n = rw(rdram, 0x800ADDD0u) & 0xFFFFu;
    return (n > 0 && n < rs64::net::TowSeg::kNoSegment) ? n : 60u;
}

bool cable_active(uint8_t phase, uint8_t count) {
    return phase != 0 && phase != 4 && count > 0;
}

// Sender: this player's cable (snowspeeder only), its state on change, segments finished since the last frame reliably, the stretching head every frame.
void sync_tow_cable(uint8_t* rdram)
{

    Ghost& s = g();
    const uint32_t pool = rw(rdram, kCablePool);

    if (s.peer_left || rb(rdram, 0x80130B41u) != kSnowspeeder || pool < 0x80000000u || pool >= 0x80800000u)
    {
        return;
    }

    rs64::net::TowSeg t;
    t.epoch = mission_tag();
    t.phase = rb(rdram, kCableState + 0);
    t.head = rb(rdram, kCableState + 1);
    t.count = rb(rdram, kCableState + 2);
    const bool active = cable_active(t.phase, t.count);

    if (t.phase != s.tow_sent_phase) {
        s.link.send(rs64::net::encode_tow_seg(t), true);
        s.tow_sent_phase = t.phase;
    }

    if (!active) {
        s.tow_sent_head = -1;
        return;
    }

    auto send_segment = [&](uint32_t i, bool reliable) {
        t.index = (uint8_t)i;

        for (uint32_t k = 0; k < 16; ++k) {
            t.words[k] = rw(rdram, pool + i * kCableStride + k * 4u);
        }

        s.link.send(rs64::net::encode_tow_seg(t), reliable);
    };
    const uint32_t ring = cable_ring(rdram);

    for (uint32_t i = (uint32_t)s.tow_sent_head, n = 0; s.tow_sent_head >= 0 && i != t.head && n < ring; i = (i + 1) % ring, ++n)
    {
        send_segment(i, true);
    }

    send_segment(t.head, false);
    s.tow_sent_head = t.head;
}

// Receiver: the other player's cable into the mirror pool (a new cable starts with every segment inactive).
void apply_tow_seg(uint8_t* rdram, const rs64::net::TowSeg& t)
{

    Ghost& s = g();

    if (!s.tow_pool)
    {
        return;
    }

    const uint32_t ring = cable_ring(rdram);

    if (!rs64::net::tow_seg_ok(t, ring))
    {
        return;
    }

    if (cable_active(t.phase, t.count) && !cable_active(s.tow_remote.phase, s.tow_remote.count))
    {
        for (uint32_t i = 0; i < ring; ++i) {
            wb(rdram, s.tow_pool + i * kCableStride + 0xF4u, 0);
        }

        fprintf(stderr, "[ghost] the other player's tow cable is out: drawing it\n");
    }

    s.tow_remote.phase = t.phase;
    s.tow_remote.head = t.head;
    s.tow_remote.count = (uint8_t)std::min<uint32_t>(t.count, ring);

    if (t.index == rs64::net::TowSeg::kNoSegment || t.index >= ring)
    {
        return;
    }

    const uint32_t seg = s.tow_pool + t.index * kCableStride;

    for (uint32_t k = 0; k < 16; ++k) {
        ww(rdram, seg + k * 4u, t.words[k]);
    }

    wb(rdram, seg + 0xF4u, 1);
}

// npcAtAtUpdate accepting a trip (0x800CED7C, s1 = the AT-AT ext, +0x34 its DAT item): the other player trips its own copy, whose fall then kills it and counts the objective in its world.
extern "C" void rs64_ghost_walker_tripped(uint8_t* rdram, recomp_context* ctx)
{

    Ghost& s = g();

    if (!s.in_session || s.peer_left || s.tow_injecting)
    {
        return;
    }

    const uint32_t item = rw(rdram, (uint32_t)ctx->r17 + 0x34u);
    const uint32_t n = dat_count(rdram);

    for (uint32_t i = 0; i < n; ++i) {
        if (dat_item(rdram, i) == item)
        {
            s.tows_mine.push_back((uint16_t)i);
            s.link.send(rs64::net::encode_tow_trip(mission_tag(), (uint16_t)i), true);

            fprintf(stderr, "[ghost] walker %u tripped; sent to the other player\n", i);
            return;
        }
    }

    fprintf(stderr, "[ghost] tripped walker %08X is not a level DAT item; not shared\n", item);
}
}

using namespace rs64::ghost;
using rs64::mips::rw;
using rs64::mips::ww;

// The other player flies a snowspeeder: its cable's mirror pool, through the game's loader (loadEffectModelInstancePool 0x800AE138: the tow_cable HOB, the instances). Load phase only (initMission).
extern "C" void rs64_ghost_tow_load(uint8_t* rdram, recomp_context* ctx)
{

    Ghost& s = g();

    // Without the lobby the host never hears the client's craft: a snowspeeder here means a snowspeeder level.
    const bool remote_speeder = s.remote_craft == kSnowspeeder || (s.remote_craft < 0 && rb(rdram, 0x80130B41u) == kSnowspeeder);

    if (s.tow_pool || !remote_speeder)
    {
        return;
    }

    const uint32_t pool = [&] {

        CableSwap swap(rdram, 0, nullptr);

        rs64::mp::call(rdram, ctx, 0x800AE138u, {});
        return rw(rdram, kCablePool);
    }();

    if (pool < 0x80000000u || pool >= 0x80800000u)
    {
        return;
    }

    s.tow_pool = pool;

    for (uint32_t i = 0, ring = cable_ring(rdram); i < ring; ++i) {
        wb(rdram, pool + i * kCableStride + 0xF4u, 0);
    }

    fprintf(stderr, "[ghost] the other player's tow cable pool loaded\n");
}

// Mission end: the mirror pool goes back through the game's free (freeEffectModelInstancePool 0x800AE218).
extern "C" void rs64_ghost_tow_release(uint8_t* rdram, recomp_context* ctx)
{

    Ghost& s = g();

    if (!s.tow_pool)
    {
        return;
    }

    [&] {

        CableSwap swap(rdram, s.tow_pool, nullptr);

        rs64::mp::call(rdram, ctx, 0x800AE218u, {});
    }();

    s.tow_pool = 0;
    s.tow_remote = rs64::net::TowSeg{};
}

// The imposter wingman was drawn (RS64_HOOK_WINGMAN_DRAW, s3 = the NPC, s1 = the render info): the other player's cable goes through the game's renderer (0x800ADD5C) with the mirror pool and state swapped in.
extern "C" void rs64_ghost_tow_draw(uint8_t* rdram, recomp_context* ctx)
{

    const Ghost& s = g();

    if (!s.in_session || !s.tow_pool || !cable_active(s.tow_remote.phase, s.tow_remote.count) || (uint32_t)ctx->r19 != rs64_imposter_npc() || rs64_imposter_npc() == 0)
    {
        return;
    }

    CableSwap swap(rdram, s.tow_pool, &s.tow_remote);
    rs64::mp::call(rdram, ctx, 0x800ADD5Cu, {(uint32_t)ctx->r17});
}
