// Ghost co-op world sync: the host's objective state, mirrored DAT damage, shared power-ups and the client's trigger volumes.
#include "session/ghost_internal.h"
#include "core/mp_core.h"
#include "util/debug_logs.h"
#include "util/rdram_words.h"
#include "mp_host.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace rs64::ghost {

using namespace rs64::ls;
using rs64::mips::rw;
using rs64::mips::ww;
using rs64::mips::rf;

// Objective state (research: Level objectives): booleans u8[128] at 0x801388A0, counts s32[128] at the relocated 0x80138060, timers f32[8] at 0x8010C9F8.
constexpr uint32_t kObjBools = 0x801388A0u;
constexpr uint32_t kObjTimers = 0x8010C9F8u;

uint32_t obj_counts_addr() {
    return rs64::ls::relocated(0x80138060u);
}

void read_objectives(const uint8_t* rdram, rs64::net::ObjSnapshot* s) {

    for (uint32_t i = 0; i < 128; ++i) {
        s->bools[i] = rdram[(kObjBools + i - 0x80000000u) ^ 3];
        s->counts[i] = (int32_t)rw(rdram, obj_counts_addr() + i * 4);
    }

    for (uint32_t i = 0; i < 8; ++i) {
        s->timers[i] = rf(rdram, kObjTimers + i * 4);
    }

    s->lives = rdram[(kLives - 0x80000000u) ^ 3];
}

void write_objectives(uint8_t* rdram, const rs64::net::ObjSnapshot& s)
{

    for (uint32_t i = 0; i < 128; ++i) {
        rdram[(kObjBools + i - 0x80000000u) ^ 3] = s.bools[i];
        ww(rdram, obj_counts_addr() + i * 4, (uint32_t)s.counts[i]);
    }

    for (uint32_t i = 0; i < 8; ++i) {
        uint32_t b;
        memcpy(&b, &s.timers[i], 4);
        ww(rdram, kObjTimers + i * 4, b);
    }

    rdram[(kLives - 0x80000000u) ^ 3] = s.lives;
}

// Level objects are DAT items: the type-0 list under D_801375D8 (count at +0, item pointer array at +4). Health word = item + 0x6C + 4 * difficulty (0x80137CE4), live and persistent; item+2 != 0 = invulnerable.
uint32_t dat_count(const uint8_t* rdram) {

    const uint32_t base = rw(rdram, 0x801375D8u);

    if (base < 0x80000000u || base >= 0x80800000u)
    {
        return 0;
    }

    const uint32_t n = rw(rdram, base);
    return n <= 4096 ? n : 0;
}

uint32_t dat_item(const uint8_t* rdram, uint32_t i)
{

    const uint32_t list = rw(rdram, rw(rdram, 0x801375D8u) + 4);

    if (list < 0x80000000u || list + i * 4 + 4 > 0x80800000u)
    {
        return 0u;
    }

    const uint32_t item = rw(rdram, list + i * 4);
    return (item >= 0x80000000u && item < 0x80800000u - 0x100u) ? item : 0u;
}

bool dat_tracked(const uint8_t* rdram, uint32_t item) {
    return item && rs64::mips::rh(rdram, item + 2) == 0;
}

// dealDamagetoDatItem follows the item's live slot (+6) into the NPC context and its game object; a slot mid-teardown can point at a freed context, so only a consistent one (ctx+0x16 == slot, game object in RDRAM) takes a hit. Unspawned (0xFFFF) is always safe.
bool dat_damage_safe(const uint8_t* rdram, uint32_t item)
{

    const uint32_t slot = rs64::mips::rh(rdram, item + 6);

    if (slot == 0xFFFFu)
    {
        return true;
    }

    const uint32_t table = rw(rdram, 0x80130BB0u);

    if (slot >= 0x800u || table < 0x80000000u || table >= 0x80800000u)
    {
        return false;
    }

    const uint32_t ctx = rw(rdram, table + slot * 8);

    if (ctx < 0x80000000u || ctx >= 0x80800000u - 0x40u || rs64::mips::rh(rdram, ctx + 0x16) != slot)
    {
        return false;
    }

    const uint32_t obj = rw(rdram, ctx + 0x10);
    return obj == 0 || (obj >= 0x80000000u && obj < 0x80800000u - 0x200u);
}

int32_t dat_health(const uint8_t* rdram, uint32_t item) {

    const uint32_t diff = rw(rdram, 0x80137CE4u) & 3u;
    return (int32_t)rw(rdram, item + 0x6C + diff * 4);
}

// Player 1's trigger list (getPlayerRecordTargetBuffer(0) = 0x80137DBC + 0x34): u16[63], low 12 bits = index into the DAT event table at *(D_801375D8)+0x3C.
constexpr uint32_t kPlyList = 0x80137DF0u;
constexpr uint32_t kPlyListEnd = kPlyList + 63u * 2u;

// The trigger event with this index, or 0 if it is not a type 0x18/0x19 event (the only types applyDatObjectiveTriggerEffect handles).
uint32_t trigger_event(const uint8_t* rdram, uint16_t idx) {

    const uint32_t dat = rw(rdram, 0x801375D8u);

    if (dat < 0x80000000u || dat >= 0x80800000u - 0x40u)
    {
        return 0u;
    }

    const uint32_t table = rw(rdram, dat + 0x3C);

    if (table < 0x80000000u || table + idx * 4u + 4u > 0x80800000u)
    {
        return 0u;
    }

    const uint32_t ev = rw(rdram, table + idx * 4u);

    if (ev < 0x80000000u || ev >= 0x80800000u - 0x68u)
    {
        return 0u;
    }

    const uint16_t type = rs64::mips::rh(rdram, ev);
    return (type == 0x18u || type == 0x19u) ? ev : 0u;
}

void send_hit(uint8_t* rdram, recomp_context* ctx, uint32_t slot, uint32_t damage)
{

    const uint32_t msg = 0x80B40100u;
    ww(rdram, msg + 0x0, 0xFFFFFFFFu);
    ww(rdram, msg + 0x4, (7u << 16) | (damage > 0xFFFFu ? 0xFFFFu : damage));
    ww(rdram, msg + 0x8, 0u);
    rs64::mp::call(rdram, ctx, 0x8003E8DCu, {slot, 5u, msg});
}

void kill_player(uint8_t* rdram, recomp_context* ctx)
{

    const float damage = 1.0e6f;
    uint32_t bits;

    memcpy(&bits, &damage, 4);
    wf(rdram, 0x80137DBCu + 0xC8u, 0.0f);
    rs64::mp::call(rdram, ctx, 0x800B13E0u, {0x80137DBCu, bits});
}

// Friendly fire: the other player's hits on this craft, through the game's own damage path (its spawn shield and death included). A spectator takes none.
void apply_friendly_fire(uint8_t* rdram, recomp_context* ctx)
{

    Ghost& s = g();

    if (s.ff_damage == 0)
    {
        return;
    }

    const float damage = (float)s.ff_damage;
    uint32_t bits;

    s.ff_damage = 0;

    if (s.out)
    {
        return;
    }

    memcpy(&bits, &damage, 4);

    const float before = rf(rdram, kPlayerHealth);
    const float shield = rf(rdram, 0x80137DBCu + 0xC8u);

    rs64::mp::call(rdram, ctx, 0x800B13E0u, {0x80137DBCu, bits});

    fprintf(stderr, "[ghost] friendly fire: took %.0f from the other player (health %.1f -> %.1f, shield %.2f, god %u)\n", damage, before, rf(rdram, kPlayerHealth), shield, (rw(rdram, 0x80130B4Cu) >> 23) & 1u);
}

// Shared deaths: the peer's health drops go in through the game's own damage path (dealDamagetoDatItem 0x800C7390: a HIT for a spawned item, a stored-health cut for an unspawned one); then our own drops go out.
void sync_dat(uint8_t* rdram, recomp_context* ctx)
{

    Ghost& s = g();
    const uint32_t n = dat_count(rdram);

    if (s.dat_baseline && s.dat_last.size() != n)
    {
        fprintf(stderr, "[ghost] DAT item count changed (%zu -> %u); re-baselining\n", s.dat_last.size(), n);
        s.dat_baseline = false;
        s.dat_apply.clear();
    }

    // The baseline is every item's real health (an item invulnerable now may become vulnerable later).
    if (!s.dat_baseline) {
        s.dat_last.assign(n, 0);

        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t item = dat_item(rdram, i);
            s.dat_last[i] = item ? dat_health(rdram, item) : 0;
        }

        s.dat_baseline = true;
    }

    // A kill applied before the mission settles (opening cutscene, first frames) runs the objective trigger (triggerObjectiveCompleteWithVoiceLines) on state that is not set up yet and faults; a late joiner's backlog waits.
    const bool settled = s.frame >= kDamageSettleFrames && !rs64::mp::in_cutscene();
    std::vector<std::pair<uint16_t, int32_t>> retry;

    for (const auto& d : settled ? s.dat_apply : retry)
    {
        if (d.first >= n) {
            continue;
        }

        const uint32_t item = dat_item(rdram, d.first);

        if (!dat_tracked(rdram, item)) {
            continue;
        }

        // A tripped walker dies from its own fall (which counts the objective); a hit on it would skip that count.
        if ((rw(rdram, item + 0x4C) & 0x80000000u) || std::find(s.tow_in.begin(), s.tow_in.end(), d.first) != s.tow_in.end())
        {
            continue;
        }

        const int32_t local = dat_health(rdram, item);

        if (local > 0 && local > d.second && !dat_damage_safe(rdram, item))
        {
            // An inconsistent slot is usually mid-teardown; retry for up to ~2 s.
            if (s.dat_retry_frames[d.first]++ < 60) {
                retry.push_back(d);
            } else {
                s.dat_retry_frames.erase(d.first);
            }

            continue;
        }

        s.dat_retry_frames.erase(d.first);

        if (local > 0 && local > d.second)
        {
            const uint32_t slot = rs64::mips::rh(rdram, item + 6);
            const uint32_t dmg = (uint32_t)(local - d.second);

            if (slot == 0xFFFFu && d.second > 0)
            {
                // Unspawned, not lethal: the health word directly, since dealDamagetoDatItem counts a type-0x14 item's objective on every unspawned hit (0x800C74B0).
                ww(rdram, item + 0x6C + (rw(rdram, 0x80137CE4u) & 3u) * 4u, (uint32_t)d.second);
            } else if (slot == 0xFFFFu) {
                // Unspawned, lethal: dealDamagetoDatItem cuts the stored health, so the item streams in as rubble.
                rs64::mp::call(rdram, ctx, 0x800C7390u, {rw(rdram, item + 0xC), dmg});
            } else {
                send_hit(rdram, ctx, slot, dmg);
            }

            if (s.dat_applied++ < 20)
            {
                char name[25] = {};

                for (int k = 0; k < 24; ++k) {
                    name[k] = (char)rb(rdram, rw(rdram, item + 0xC) + k);

                    if (!name[k]) {
                        break;
                    }
                }

                fprintf(stderr, "[ghost] mirrored damage: %s %d -> %d (now %d)\n", name, local, d.second, dat_health(rdram, item));
            }

            // The item's actual health after the hit, so a damage scale other than 1 can't ratchet the two worlds down.
            s.dat_last[d.first] = dat_health(rdram, item);
        }
    }

    if (settled) {
        s.dat_apply.swap(retry);
    }

    if (s.peer_left)
    {
        return;
    }

    // Each frame, every tracked item whose health dropped since last frame; the last value follows refills too. A resync sends every tracked item's health.
    const bool full = s.resync;
    s.resync = false;
    std::vector<std::pair<uint16_t, int32_t>> batch;
    auto flush = [&s, &batch]() {
        s.link.send(rs64::net::encode_damage(mission_tag(), batch), true);
        s.dat_sent += (uint32_t)batch.size();
        batch.clear();
    };

    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t item = dat_item(rdram, i);

        if (!item) {
            continue;
        }

        const int32_t hp = dat_health(rdram, item);

        if (dat_tracked(rdram, item) && (full || hp < s.dat_last[i])) {
            batch.emplace_back((uint16_t)i, hp);

            if (batch.size() == rs64::net::kDamageBatch) {
                flush();
            }
        }

        s.dat_last[i] = hp;
    }

    if (!batch.empty()) {
        flush();
    }

    if (full) {
        fprintf(stderr, "[ghost] DAT health resync sent to the peer\n");
    }
}

// Shared power-ups: a pickup ORs one bit into the settings word (npcPowerUpUpdate 0x800EBDB0-0x800EBE4C). Bits gained this mission go to the peer, theirs are ORed in here; the game's own commit saves them to the pilot on success and reverts them otherwise.
void sync_upgrades(uint8_t* rdram)
{

    constexpr uint32_t kSettings = 0x80130B4Cu;
    Ghost& s = g();
    const uint32_t word = rw(rdram, kSettings);

    if (s.upgrades_base < 0) {
        s.upgrades_base = word & rs64::net::kUpgradeMask;
    }

    const uint32_t add = s.upgrades_in & ~word;

    if (add)
    {
        ww(rdram, kSettings, word | add);

        fprintf(stderr, "[ghost] upgrades %05X from the other player\n", add);
    }

    s.upgrades_shared |= s.upgrades_in;
    s.upgrades_in = 0;
    const uint32_t gained = word & rs64::net::kUpgradeMask & ~(uint32_t)s.upgrades_base & ~s.upgrades_shared;

    if (gained && !s.peer_left)
    {
        s.link.send(rs64::net::encode_upgrades(mission_tag(), gained), true);
        s.upgrades_shared |= gained;

        fprintf(stderr, "[ghost] upgrades %05X picked up; sent to the other player\n", gained);
    }
}

// RS64_HOOK_NPC_HIT (a0 = the target slot, a2 = the HIT: +2 the shooter's slot, +6 damage): with friendly fire on, this player's hits on the other player's ship go to them.
extern "C" void rs64_ghost_npc_hit(uint8_t* rdram, recomp_context* ctx)
{

    using rs64::mips::rh;
    Ghost& s = g();

    if (!s.session_ff || !s.in_session || s.peer_left || rs64_imposter_npc() == 0)
    {
        return;
    }

    const uint32_t msg = (uint32_t)ctx->r6;
    const bool on_partner = ((uint32_t)ctx->r4 & 0xFFFFu) == rh(rdram, kImposterRec + 0x6);

    if (on_partner && rh(rdram, msg + 2) == rh(rdram, 0x80137DBAu)) {
        s.link.send(rs64::net::encode_hit(mission_tag(), rh(rdram, msg + 6)), true);

        fprintf(stderr, "[ghost] friendly fire: hit the other player for %u\n", rh(rdram, msg + 6));
    }
}

// npcPowerUpUpdate's touch test (0x800EBD20, s1 = the power-up, its DAT record at +4): a power-up the other player collected passes it (squared distance f4 = 0), so this world runs the game's own pickup.
extern "C" void rs64_ghost_powerup_touch(uint8_t* rdram, recomp_context* ctx)
{

    Ghost& s = g();

    // Test pickup: the first power-up that ticks from that mission frame on is collected, as if flown through.
    static const TestStep* const pickup = test_step("pickup", -1);

    if (pickup && s.in_session && (long)s.frame >= pickup->frame && !s.test_pickup_done)
    {
        s.test_pickup_done = true;
        ctx->f4.fl = 0.0f;

        fprintf(stderr, "[ghost] test pickup at frame %u\n", s.frame);
        return;
    }

    if (s.pickups_forced.empty())
    {
        return;
    }

    const uint32_t item = rw(rdram, (uint32_t)ctx->r17 + 4u);

    if (std::find(s.pickups_forced.begin(), s.pickups_forced.end(), item) != s.pickups_forced.end())
    {
        ctx->f4.fl = 0.0f;
    }
}

// The pickup itself (0x800EBD34, a1 = the sound's position): one the other player collected plays at this player's ship (player block 0x80137DC0) so it is heard; this player's own goes to the other player by DAT index.
extern "C" void rs64_ghost_powerup_collect(uint8_t* rdram, recomp_context* ctx)
{

    Ghost& s = g();
    const uint32_t item = rw(rdram, (uint32_t)ctx->r17 + 4u);
    const auto forced = std::find(s.pickups_forced.begin(), s.pickups_forced.end(), item);

    if (forced != s.pickups_forced.end()) {
        s.pickups_forced.erase(forced);
        ctx->r5 = (gpr)(int32_t)0x80137DC0u;

        fprintf(stderr, "[ghost] power-up collected for the other player\n");
        return;
    }

    if (!s.in_session || s.peer_left)
    {
        return;
    }

    const uint32_t n = dat_count(rdram);

    for (uint32_t i = 0; i < n; ++i) {
        if (dat_item(rdram, i) == item)
        {
            s.pickups_mine.push_back((uint16_t)i);
            s.link.send(rs64::net::encode_pickup(mission_tag(), (uint16_t)i), true);

            fprintf(stderr, "[ghost] power-up %u collected; sent to the other player\n", i);
            return;
        }
    }
}

// The client's trigger edges, applied through the game's own effect (host): the next snapshot carries the result back to the client.
void apply_remote_triggers(uint8_t* rdram, recomp_context* ctx)
{

    Ghost& s = g();

    // Like mirrored damage, not before the host's mission settles or during its cutscene: the edges wait, in order.
    if (s.trigger_in.empty() || s.frame < kDamageSettleFrames || rs64::mp::in_cutscene())
    {
        return;
    }

    for (const auto& t : s.trigger_in)
    {
        const uint32_t ev = trigger_event(rdram, t.first);

        if (!ev)
        {
            fprintf(stderr, "[ghost] trigger %u from the other player is not a trigger event here; ignored\n", t.first);
            continue;
        }

        if (!s.team_triggers.on_edge(1, t.first, t.second)) {
            continue;
        }

        s.trigger_applying = true;
        rs64::mp::set_flag("mp_trigger_for_peer", 1);
        rs64::mp::call(rdram, ctx, 0x80065980u, {ev, t.second ? 1u : 0u});
        rs64::mp::set_flag("mp_trigger_for_peer", 0);
        s.trigger_applying = false;

        fprintf(stderr, "[ghost] trigger %u %s from the other player applied\n", t.first, t.second ? "enter" : "exit");
    }

    s.trigger_in.clear();
}
}

using namespace rs64::ghost;
using rs64::mips::rw;

// datItemSetObjectiveBooleanCount(bool index, value, count1 index, add, count2 index, add; indices 1-based, 0 = none): a client in session drops the write. Its callers are death handlers, and every death is mirrored into the host's world (sync_dat), where the host's handler counts it once; the host's snapshot brings the counts back.
// The ledger keeps each dropped write until a snapshot shows it, for a host that drops out before it does.
extern "C" uint32_t rs64_ghost_objective_event(uint8_t* rdram, recomp_context* ctx)
{

    Ghost& s = g();

    if (!s.in_session || s.player != 1 || s.peer_left)
    {
        return 0u;
    }

    const uint32_t sp = (uint32_t)ctx->r29;
    const int32_t bool_index = (int32_t)ctx->r4;
    const int32_t count1 = (int32_t)ctx->r6;
    const int32_t count2 = (int32_t)rw(rdram, sp + 0x10);
    const double now = now_s();

    if (bool_index != 0) s.obj_ledger.dropped_bool(bool_index - 1, (uint8_t)ctx->r5, now);
    if (count1 != 0) s.obj_ledger.dropped_count(count1 - 1, (int32_t)ctx->r7, now);
    if (count2 != 0) s.obj_ledger.dropped_count(count2 - 1, (int32_t)rw(rdram, sp + 0x14), now);

    return 1u;
}

// applyDatObjectiveTriggerEffect, for player 1's own trigger volumes (s0 = the list entry): a client sends the edge to the host and drops it, since the host's snapshot brings the result back; the host runs it once per team.
extern "C" uint32_t rs64_ghost_trigger_effect(uint8_t* rdram, recomp_context* ctx)
{

    Ghost& s = g();

    if (!s.in_session || s.peer_left || s.trigger_applying)
    {
        return 0u;
    }

    const uint32_t entry = (uint32_t)ctx->r16;

    if (entry < kPlyList || entry >= kPlyListEnd)
    {
        return 0u;
    }

    const uint16_t idx = rs64::mips::rh(rdram, entry) & 0xFFFu;
    const bool enter = (ctx->r5 & 0xFF) == 1;

    if (s.player == 1)
    {
        s.link.send(rs64::net::encode_trigger(mission_tag(), idx, enter), true);

        fprintf(stderr, "[ghost] trigger %u %s sent to the host\n", idx, enter ? "enter" : "exit");
        return 1u;
    }

    // ROGUESQ_MP_TEST_TRIGGER_SOLO=1: the host drops its own trigger edges, so only the client's can advance the mission.
    static const bool solo = recomp::dbg::env_on("ROGUESQ_MP_TEST_TRIGGER_SOLO");

    if (solo)
    {
        fprintf(stderr, "[ghost] test: own trigger %u %s dropped\n", idx, enter ? "enter" : "exit");
        return 1u;
    }

    return s.team_triggers.on_edge(0, idx, enter) ? 0u : 1u;
}
