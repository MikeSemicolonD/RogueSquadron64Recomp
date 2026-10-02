#include "lockstep_core.h"
#include "debug_logs.h"
#include "ghost_internal.h"
#include "rdram_words.h"
#include "mp_host.h"
#include <cstdio>
#include <cstring>

using namespace rs64::ls;
using namespace rs64::mips;

namespace {

struct Imposter {
    bool spawned = false;
    uint32_t ctx = 0;
    uint32_t blk = 0;
    // The wingman model the imposter spawned with (0 = X-wing, the only one with S-foil nodes).
    int model = -1;
    // A spawn that got no slot retries every kRetryFrames, up to kRetries times.
    int wait = 0;
    int tries = 0;
};
constexpr int kRetryFrames = 30;
constexpr int kRetries = 20;

Imposter& imp() {
    static Imposter s;
    return s;
}

}

// ROGUESQ_COOP_LOCAL=1 (or ROGUESQ_MP): player 2 flies an allied NPC craft (the imposter) from port 1; off = the original single player.
extern "C" uint32_t rs64_coop_imposter(void) {
    static const uint32_t s_on = (recomp::dbg::env_on("ROGUESQ_COOP_LOCAL") || recomp::dbg::env_str("ROGUESQ_MP")) ? 1u : 0u;
    return (s_on || rs64_lobby_menu_session()) ? 1u : 0u;
}

// The record is hashed from mission frame 0, before the spawn rebuilds it, so a previous mission's or demo's record must not carry over.
extern "C" void rs64_imposter_reset(uint8_t* rdram) {
    imp() = Imposter{};
    if (rs64_coop_imposter()) {
        clear_imposter_record(rdram);
    }
}

extern "C" void rs64_imposter_frame(uint8_t* rdram, recomp_context* ctx) {
    Imposter& s = imp();
    if (!rs64_coop_imposter() || s.spawned) {
        return;
    }
    if (s.wait > 0) {
        --s.wait;
        return;
    }
    // Attract demos replay recorded inputs against an unchanged world; ghost co-op without a session (no peer, mismatch) plays alone.
    if ((rw(rdram, 0x80130B50u) & 0x60u) != 0 || (rs64_ghost_mode() && !rs64_ghost_active())) {
        return;
    }
    if (rh(rdram, 0x80137DBAu) == 0xFFFFu) {
        return;
    }
    const uint32_t m = 0x8013800Cu;
    const Vec3 right = {rf(rdram, m + 0x0), rf(rdram, m + 0xC), rf(rdram, m + 0x18)};
    Vec3 p1 = {rf(rdram, 0x80137DC0u), rf(rdram, 0x80137DC4u), rf(rdram, 0x80137DC8u)};
    Vec3 pos = imposter_spawn_pos(p1, right);
    if (rs64_ghost_mode()) {
        // Ghost co-op: both machines' player 1 starts at the level start, so the client moves itself 1.5 units right once per mission (where the host shows it).
        if (rs64_ghost_is_client() && rs64_ghost_take_start_offset()) {
            auto wf = [rdram](uint32_t a, float v) { uint32_t b; memcpy(&b, &v, 4); ww(rdram, a, b); };
            pos = p1;
            p1 = imposter_spawn_pos(p1, right);
            wf(0x80137DC0u, p1.x);
            wf(0x80137DC4u, p1.y);
            wf(0x80137DC8u, p1.z);
            wf(m + 0x24, p1.x);
            wf(m + 0x28, p1.y);
            wf(m + 0x2C, p1.z);
            fprintf(stderr, "[ghost] client start offset to %.1f,%.1f,%.1f\n", p1.x, p1.y, p1.z);
        }
        // The puppet exists only while the other player is in the mission and not down; it spawns at their streamed position.
        float remote[3];
        if (!rs64_ghost_want_puppet(remote)) {
            return;
        }
        pos = {remote[0], remote[1], remote[2]};
        // Never spawn it inside player 1: wait until the two craft are apart.
        const Vec3 d = {pos.x - p1.x, pos.y - p1.y, pos.z - p1.z};
        if (d.x * d.x + d.y * d.y + d.z * d.z < 0.75f * 0.75f) {
            return;
        }
    }
    // The wingman dereferences its mesh instance during the spawn, so use a wingman model this level loaded (Mos Eisley: wmxwng; others differ) or skip the imposter.
    int model = wingman_model(rdram);
    // Ghost co-op: the other player's craft, when its wingman mesh is loaded.
    const int remote_model = rs64_ghost_mode() ? rs64_ghost_puppet_model() : -1;
    if (remote_model >= 0 && mesh_loaded(rdram, wingman_model_name(remote_model))) {
        model = remote_model;
    }
    if (model < 0) {
        s.spawned = true;
        s.ctx = 0;
        s.blk = 0;
        fprintf(stderr, "[coop] no wingman model loaded on this level; imposter skipped\n");
        return;
    }
    build_imposter_record(rdram, pos);
    ww(rdram, kImposterRec + 0x8C, (uint32_t)model);
    // spawnNpcOfType does not fail cleanly on an exhausted pool (it pops past its context stack or writes slot entry 0xFFFF), so it is only called with room in both.
    if (!npc_pool_room(rdram)) {
        if (++s.tries < kRetries) {
            s.wait = kRetryFrames;
            fprintf(stderr, "[coop] NPC pool full; imposter spawn retry %d\n", s.tries);
            return;
        }
        s.spawned = true;
        s.ctx = 0;
        s.blk = 0;
        fprintf(stderr, "[coop] NPC pool full; imposter skipped\n");
        return;
    }
    const uint32_t slot = rs64::mp::call(rdram, ctx, 0x8003FFECu, {0x800D93C0u, kImposterRec, 2u, 0x5Au}).v0 & 0xFFFFu;
    const uint32_t table = rw(rdram, 0x80130BB0u);
    const uint32_t npc = slot < kNpcSlots ? rw(rdram, table + slot * 8) : 0u;
    const uint32_t blk = imposter_spawn_ok(slot, npc, 0x80000000u, 0x80000000u) ? rw(rdram, npc + 4) : 0u;
    const uint32_t mesh = imposter_spawn_ok(slot, npc, blk, 0x80000000u) ? rw(rdram, blk + 0x30) : 0u;
    if (!imposter_spawn_ok(slot, npc, blk, mesh)) {
        // Leave any half-spawned slot to the mission-end teardown (its reference count is 0) and stop trying this mission; blk 0 marks it dead.
        s.spawned = true;
        s.ctx = 0;
        s.blk = 0;
        fprintf(stderr, "[coop] imposter spawn failed (slot %04X ctx %08X blk %08X mesh %08X)\n", slot, npc, blk, mesh);
        return;
    }
    rdram[(kImposterRec + 0x6 - 0x80000000u) ^ 3] = (uint8_t)(slot >> 8);
    rdram[(kImposterRec + 0x7 - 0x80000000u) ^ 3] = (uint8_t)slot;
    // Start on player 1's heading: the wingman built its frame from the record's zero Euler angles.
    const uint32_t fwd_off[3] = {0x8, 0x14, 0x20};
    const uint32_t up_off[3] = {0x4, 0x10, 0x1C};
    for (int i = 0; i < 3; ++i) {
        ww(rdram, blk + 0x0C + 4 * i, rw(rdram, m + fwd_off[i]));
        ww(rdram, blk + 0x18 + 4 * i, rw(rdram, m + up_off[i]));
    }
    rdram[(npc + 0x1A - 0x80000000u) ^ 3] += 1;
    ww(rdram, kImposterRec + 0xE0, blk);
    ww(rdram, kImposterRec + 0xE4, npc);
    // Like setupNpcUpdateFunctions: one tick right after the spawn (with its dt constant) enters the wingman into its tick chains.
    ww(rdram, kImposterRec + 0xF8, rw(rdram, 0x8003BED4u));
    rs64::mp::call(rdram, ctx, 0x8003E8DCu, {slot, 3u, kImposterRec + 0xF8});
    s.spawned = true;
    s.ctx = npc;
    s.blk = blk;
    s.model = model;
    fprintf(stderr, "[coop] imposter slot %u ctx %08X blk %08X model %d at %.1f,%.1f,%.1f\n", slot, npc, blk, model, pos.x, pos.y, pos.z);
    if (rs64_ghost_mode()) {
        rs64_ghost_puppet_spawned();
    }
}

// Drops the spawn's reference before the mission's slots are torn down; destroyNpcContextArrays spins until every slot is free, and nothing else releases a record outside the level's DAT list.
extern "C" void rs64_imposter_release(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    if (rs64_ghost_mode()) {
        rs64_ghost_mission_end(rdram);
    }
    Imposter& s = imp();
    if (!s.spawned || s.ctx == 0) {
        s = Imposter{};
        return;
    }
    const uint32_t slot = rh(rdram, kImposterRec + 0x6);
    const uint32_t table = rw(rdram, 0x80130BB0u);
    if (slot != 0xFFFFu && rw(rdram, table + slot * 8) == s.ctx) {
        uint8_t& refs = rdram[(s.ctx + 0x1A - 0x80000000u) ^ 3];
        if (refs > 0) {
            refs -= 1;
        }
    }
    s = Imposter{};
}

// The remote player left the mission or is down: release the spawn's reference and destroy its slot (destroyNpcSlotByIndex); rs64_imposter_frame spawns it again when they're back.
extern "C" void rs64_imposter_despawn(uint8_t* rdram, recomp_context* ctx) {
    Imposter& s = imp();
    if (!s.spawned || s.blk == 0) {
        return;
    }
    const uint32_t slot = rh(rdram, kImposterRec + 0x6);
    const uint32_t table = rw(rdram, 0x80130BB0u);
    if (slot < kNpcSlots && rw(rdram, table + slot * 8) == s.ctx) {
        uint8_t& refs = rdram[(s.ctx + 0x1A - 0x80000000u) ^ 3];
        if (refs > 0) {
            refs -= 1;
        }
        rs64::mp::call(rdram, ctx, 0x8003ED74u, {slot});
    }
    s = Imposter{};
}

extern "C" void rs64_imposter_tick(uint8_t* rdram, recomp_context* ctx) {
    Imposter& s = imp();
    if (!s.spawned || (uint32_t)ctx->r19 != s.ctx || (ctx->r18 & 0xFFFF) != 3) {
        return;
    }
    // Action 3 (tick) passes a pointer to dt in $s1; other actions can reach this join with a stale $f20.
    const float dt = rf(rdram, (uint32_t)ctx->r17);
    const uint32_t blk = s.blk;
    auto wf = [rdram](uint32_t a, float v) { uint32_t b; memcpy(&b, &v, 4); ww(rdram, a, b); };
    ImposterCraft c;
    c.pos = {rf(rdram, blk + 0x00), rf(rdram, blk + 0x04), rf(rdram, blk + 0x08)};
    c.fwd = {rf(rdram, blk + 0x0C), rf(rdram, blk + 0x10), rf(rdram, blk + 0x14)};
    c.down = {rf(rdram, blk + 0x18), rf(rdram, blk + 0x1C), rf(rdram, blk + 0x20)};
    c.speed = rf(rdram, kImposterRec + 0xEC);
    c.target = rf(rdram, kImposterRec + 0xF4);
    c.boost_hold = rf(rdram, kImposterRec + 0xF8);
    c.level_latch = rdram[(kImposterRec + 0xFC - 0x80000000u) ^ 3] != 0;
    c.foil_closed = rdram[(kImposterRec + 0xFD - 0x80000000u) ^ 3] != 0;
    c.foil_t = rf(rdram, kImposterRec + 0xE8);
    // Player 2's control-map slot 1 (0x8010BE18 + 0x24), the entries the X-wing reads: +0x1A fire, +0x18 boost, +0x12 brake, +0x06 hold rotation, +0x22 S-foils.
    ImposterPad in;
    if (rdram[(0x80130B92u - 0x80000000u) ^ 3] == 1) {
        const uint16_t held = rh(rdram, 0x80130B8Eu);
        const uint32_t map = 0x8010BE18u + 0x24u;
        in.stick_x = (int8_t)rdram[(0x80130B90u - 0x80000000u) ^ 3];
        in.stick_y = (int8_t)rdram[(0x80130B91u - 0x80000000u) ^ 3];
        in.fire = (held & rh(rdram, map + 0x1A)) != 0;
        in.boost = (held & rh(rdram, map + 0x18)) != 0;
        in.brake = (held & rh(rdram, map + 0x12)) != 0;
        in.freeze = (held & rh(rdram, map + 0x06)) != 0;
        in.foil = (held & rh(rdram, map + 0x22)) != 0;
    }
    Vec3 vel;
    if (rs64_ghost_mode()) {
        // Ghost co-op: the remote player's streamed pose; without one yet the puppet holds where it spawned.
        const float seed[9] = {c.pos.x, c.pos.y, c.pos.z, c.fwd.x, c.fwd.y, c.fwd.z, c.down.x, c.down.y, c.down.z};
        float pose[12];
        if (rs64_ghost_puppet(seed, dt, pose)) {
            c.pos = {pose[0], pose[1], pose[2]};
            c.fwd = {pose[3], pose[4], pose[5]};
            c.down = {pose[6], pose[7], pose[8]};
            vel = {pose[9], pose[10], pose[11]};
        }
    } else {
        // Player 2 shares player 1's settings word (slot 0) and the level's ceiling (flight bounds 0x8010B7A0, min y).
        ImposterWorld w;
        w.settings = rw(rdram, 0x80130B4Cu);
        w.level = rdram[(0x80130B40u - 0x80000000u) ^ 3];
        w.ceiling_y = rf(rdram, 0x8010B7A0u);
        w.ground_y = rs64::mp::call(rdram, ctx, 0x80067D90u, {0u, 0u, 0x80B38058u, 0u}, {}, c.pos.x, c.pos.z).f0;
        imposter_fly(c, in, w, dt);
        vel = {c.fwd.x * c.speed, c.fwd.y * c.speed, c.fwd.z * c.speed};
    }
    const Vec3* parts[4] = {&c.pos, &c.fwd, &c.down, &vel};
    for (int p = 0; p < 4; ++p) {
        wf(blk + p * 0xC + 0x0, parts[p]->x);
        wf(blk + p * 0xC + 0x4, parts[p]->y);
        wf(blk + p * 0xC + 0x8, parts[p]->z);
    }
    wf(kImposterRec + 0xEC, c.speed);
    wf(kImposterRec + 0xF4, c.target);
    wf(kImposterRec + 0xF8, c.boost_hold);
    rdram[(kImposterRec + 0xFC - 0x80000000u) ^ 3] = c.level_latch ? 1 : 0;
    rdram[(kImposterRec + 0xFD - 0x80000000u) ^ 3] = c.foil_closed ? 1 : 0;
    wf(kImposterRec + 0xE8, c.foil_t);
    // Ghost co-op: the other player's S-foils on the wingman's wing nodes (wngrot_a/b at blk+0x98/+0x9C, X-wing model only: other models keep other data there; blk+0xE4 = angle, 0..10 degrees, a +/-).
    if (rs64_ghost_mode() && s.model == 0) {
        const uint32_t wing_a = rw(rdram, blk + 0x98);
        const uint32_t wing_b = rw(rdram, blk + 0x9C);
        if (wing_a >= 0x80000000u && wing_a < 0x80800000u && wing_b >= 0x80000000u && wing_b < 0x80800000u) {
            // 0x8001CE9C(node + 0x1C, angle) takes the angle as float bits in a1 (the wingman's animator does mfc1 $a1).
            const float angle = (float)rs64_ghost_remote_foil() * (10.0f / 250.0f);
            const float neg = -angle;
            uint32_t a_bits, n_bits;
            memcpy(&a_bits, &angle, 4);
            memcpy(&n_bits, &neg, 4);
            wf(blk + 0xE4, angle);
            rs64::mp::call(rdram, ctx, 0x8001CE9Cu, {wing_a + 0x1C, a_bits});
            rs64::mp::call(rdram, ctx, 0x8001CE9Cu, {wing_b + 0x1C, n_bits});
        }
    }
    rs64::mp::call(rdram, ctx, 0x80059B50u, {rw(rdram, blk + 0x30) + 0x28, blk, rw(rdram, blk + 0x44)});
    uint16_t cd = rh(rdram, kImposterRec + 0xF2);
    if (imposter_fire(in.fire, &cd)) {
        const uint32_t slot = rh(rdram, kImposterRec + 0x6);
        rs64::mp::call(rdram, ctx, 0x8005E3B0u, {0u, slot, blk, 0x80109730u}, {1u, rw(rdram, 0x800A5918u), 0x3F800000u});
    }
    rdram[(kImposterRec + 0xF2 - 0x80000000u) ^ 3] = (uint8_t)(cd >> 8);
    rdram[(kImposterRec + 0xF3 - 0x80000000u) ^ 3] = (uint8_t)cd;
    const uint32_t obj = rw(rdram, s.ctx + 0x10);
    const uint32_t health = rw(rdram, obj + 0x190) + (rw(rdram, 0x80137CE4u) << 2);
    if (health >= 0x80000000u && health < 0x81000000u) {
        ww(rdram, health, 1000u);
    }
}

// Both reference-point users key on player 1 only on their normal path (no cutscene byte 0x80130B39, no fixed-point flag 0x8010CA1C & 8).
static bool imposter_extends_player_range(const uint8_t* rdram) {
    return imp().blk != 0 && rdram[(0x80130B39u - 0x80000000u) ^ 3] == 0 && (rw(rdram, 0x8010CA1Cu) & 8u) == 0;
}

// An NPC out of range of player 1 stays active if it is in range of the imposter, by the game's own XZ range test.
extern "C" void rs64_imposter_activation(uint8_t* rdram, recomp_context* ctx, uint32_t npc_point) {
    if (!imposter_extends_player_range(rdram) || (uint32_t)ctx->r2 != 0) {
        return;
    }
    ctx->r2 = (gpr)(int32_t)rs64::mp::call(rdram, ctx, 0x80047DACu, {imp().blk, npc_point}).v0;
}

// Grid streaming is stateless per call: run it a second time around the imposter.
extern "C" void rs64_imposter_stream(uint8_t* rdram, recomp_context* ctx) {
    if (!imposter_extends_player_range(rdram)) {
        return;
    }
    rs64::mp::call(rdram, ctx, 0x80047368u, {imp().blk});
}
