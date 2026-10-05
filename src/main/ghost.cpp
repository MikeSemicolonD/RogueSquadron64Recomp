// Ghost co-op (ROGUESQ_MP): each instance flies its own player 1 in its own world; the other player is the imposter, posed from the state that instance streams.
// The host's mission result ends the client's mission; each side announces entering and leaving a mission so the other can remove its puppet.
#include "ghost_internal.h"
#include "lockstep_core.h"
#include "net_link.h"
#include "debug_logs.h"
#include "rdram_words.h"
#include "mp_host.h"
#include "json/json.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace rs64::ghost {

using namespace rs64::ls;
using rs64::mips::rw;
using rs64::mips::ww;
using rs64::mips::rf;

Ghost& g() {
    static Ghost s;
    return s;
}

uint8_t mission_tag() {
    const Ghost& s = g();
    return s.player == 0 ? s.epoch : s.mission_id;
}

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

void write_objectives(uint8_t* rdram, const rs64::net::ObjSnapshot& s) {
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
    if (base < 0x80000000u || base >= 0x80800000u) {
        return 0;
    }
    const uint32_t n = rw(rdram, base);
    return n <= 4096 ? n : 0;
}

uint32_t dat_item(const uint8_t* rdram, uint32_t i) {
    const uint32_t list = rw(rdram, rw(rdram, 0x801375D8u) + 4);
    if (list < 0x80000000u || list + i * 4 + 4 > 0x80800000u) {
        return 0u;
    }
    const uint32_t item = rw(rdram, list + i * 4);
    return (item >= 0x80000000u && item < 0x80800000u - 0x100u) ? item : 0u;
}

bool dat_tracked(const uint8_t* rdram, uint32_t item) {
    return item && rs64::mips::rh(rdram, item + 2) == 0;
}

// dealDamagetoDatItem follows the item's live slot (+6) into the NPC context and its game object; a slot mid-teardown can point at a freed context, so only a consistent one (ctx+0x16 == slot, game object in RDRAM) takes a hit. Unspawned (0xFFFF) is always safe.
bool dat_damage_safe(const uint8_t* rdram, uint32_t item) {
    const uint32_t slot = rs64::mips::rh(rdram, item + 6);
    if (slot == 0xFFFFu) {
        return true;
    }
    const uint32_t table = rw(rdram, 0x80130BB0u);
    if (slot >= 0x800u || table < 0x80000000u || table >= 0x80800000u) {
        return false;
    }
    const uint32_t ctx = rw(rdram, table + slot * 8);
    if (ctx < 0x80000000u || ctx >= 0x80800000u - 0x40u || rs64::mips::rh(rdram, ctx + 0x16) != slot) {
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
    if (dat < 0x80000000u || dat >= 0x80800000u - 0x40u) {
        return 0u;
    }
    const uint32_t table = rw(rdram, dat + 0x3C);
    if (table < 0x80000000u || table + idx * 4u + 4u > 0x80800000u) {
        return 0u;
    }
    const uint32_t ev = rw(rdram, table + idx * 4u);
    if (ev < 0x80000000u || ev >= 0x80800000u - 0x68u) {
        return 0u;
    }
    const uint16_t type = rs64::mips::rh(rdram, ev);
    return (type == 0x18u || type == 0x19u) ? ev : 0u;
}

uint8_t rb(const uint8_t* rdram, uint32_t a) {
    return rdram[(a - 0x80000000u) ^ 3];
}

void wb(uint8_t* rdram, uint32_t a, uint8_t v) {
    rdram[(a - 0x80000000u) ^ 3] = v;
}

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool demo_active(const uint8_t* rdram) {
    return (rw(rdram, 0x80130B50u) & 0x60u) != 0;
}

// ROGUESQ_MP must be "host" or "join"; anything else leaves co-op off.
rs64::net::Role role_from_env() {
    const char* mp = recomp::dbg::env_str("ROGUESQ_MP");
    if (!mp) {
        return rs64::net::Role::None;
    }
    if (strcmp(mp, "host") == 0) {
        return rs64::net::Role::Host;
    }
    return strcmp(mp, "join") == 0 ? rs64::net::Role::Client : rs64::net::Role::None;
}

int& config_port();

rs64::net::LinkConfig link_config() {
    rs64::net::LinkConfig c;
    c.role = role_from_env();
    if (const char* a = recomp::dbg::env_str("ROGUESQ_MP_ADDR")) {
        c.addr = a;
    }
    const int port = recomp::dbg::env_int("ROGUESQ_MP_PORT", config_port());
    c.port = (uint16_t)(port > 0 && port < 65536 ? port : 27064);
    c.latency_ms = recomp::dbg::env_int("ROGUESQ_MP_SIM_LATENCY_MS", 0);
    c.loss_pct = recomp::dbg::env_int("ROGUESQ_MP_SIM_LOSS_PCT", 0);
    c.jitter_ms = recomp::dbg::env_int("ROGUESQ_MP_SIM_JITTER_MS", 0);
    return c;
}

// A remote state must be finite, inside a sane world range, with roughly orthonormal axes, before any of it reaches game code.
bool valid_state(const rs64::net::StatePacket& p) {
    auto len = [](const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
    for (int i = 0; i < 3; ++i) {
        const float vals[4] = {p.pos[i], p.fwd[i], p.down[i], p.vel[i]};
        for (float v : vals) {
            if (!std::isfinite(v) || std::fabs(v) > 1e5f) {
                return false;
            }
        }
    }
    const float dot = p.fwd[0] * p.down[0] + p.fwd[1] * p.down[1] + p.fwd[2] * p.down[2];
    return std::fabs(len(p.fwd) - 1.0f) < 0.1f && std::fabs(len(p.down) - 1.0f) < 0.1f && std::fabs(dot) < 0.1f;
}

// Mod-256 "newer than": within half the range ahead.
bool epoch_newer(uint8_t e, int current) {
    return current < 0 || (uint8_t)(e - (uint8_t)current) - 1u < 127u;
}

uint32_t now_ms32() {
    return (uint32_t)(now_s() * 1000.0);
}

// roguesq_net.json next to the exe: { "last_address": "10.0.0.189", "last_host": "10.0.0.5", "port": 27064 }. The last JOIN address seeds the editor; either may end in ":port".
std::string net_config_path() {
    // Two instances of one exe (the pair runner) each point ROGUESQ_NET_CONFIG at their own file.
    if (const char* p = recomp::dbg::env_str("ROGUESQ_NET_CONFIG")) {
        return p;
    }
    const std::string dir = rs64::mp::base_path();
    return dir + "roguesq_net.json";
}

int& config_port() {
    static int port = 27064;
    return port;
}

void load_net_config() {
    static bool loaded = false;
    if (loaded) {
        return;
    }
    loaded = true;
    std::ifstream f(net_config_path());
    if (!f.is_open()) {
        return;
    }
    try {
        nlohmann::json j;
        f >> j;
        g().editor.load(j.value("last_address", std::string{}));
        g().last_host = j.value("last_host", std::string{});
        const int port = j.value("port", 27064);
        config_port() = (port > 0 && port < 65536) ? port : 27064;
    } catch (...) {
    }
}

void save_net_config() {
    nlohmann::json j;
    const Ghost& s = g();
    j["last_address"] = s.row_hosting ? s.join_entry : s.editor.entry();
    if (!s.last_host.empty()) {
        j["last_host"] = s.last_host;
    }
    j["port"] = config_port();
    std::ofstream f(net_config_path());
    if (f.is_open()) {
        f << j.dump(2) << "\n";
    }
}

// Starts (or restarts) the link as host or client and the lobby handshake on top of it.
void lobby_start(const rs64::net::LinkConfig& cfg) {
    Ghost& s = g();
    s.link.stop();
    s.remote_craft = -1;
    s.player = cfg.role == rs64::net::Role::Host ? 0 : 1;
    s.was_connected = false;
    s.seen_connects = s.link.connect_count();
    s.stop_at_ms = -1;
    s.was_ready = false;
    s.peer_left = false;
    static const long test_build = recomp::dbg::env_int("ROGUESQ_MP_TEST_VERSION", -1);
    if (test_build >= 0) {
        s.lobby.set_local_build((uint32_t)test_build);
    }
    if (cfg.role == rs64::net::Role::Host) {
        s.lobby.host();
    } else {
        s.lobby.join();
    }
    s.started = s.link.start(cfg);
    if (!s.started) {
        s.lobby.on_start_failed();
    }
    if (cfg.external) {
        return;
    }
    if (s.player == 0) {
        fprintf(stderr, "[lobby] hosting on port %u latency %d ms loss %d%%%s\n", cfg.port, cfg.latency_ms, cfg.loss_pct, s.started ? "" : " (network start failed)");
    } else {
        fprintf(stderr, "[lobby] joining %s:%u latency %d ms loss %d%%%s\n", cfg.addr.c_str(), cfg.port, cfg.latency_ms, cfg.loss_pct, s.started ? "" : " (network start failed)");
    }
}

// Link events into the lobby: HELLO on connect, the version verdict, a peer lost. A failed JOIN stops retrying.
void lobby_poll() {
    Ghost& s = g();
    if (!s.started) {
        return;
    }
    const uint32_t now = now_ms32();
    const bool c = s.link.connected();
    const uint32_t connects = s.link.connect_count();
    const rs64::net::LinkEdges edges = rs64::net::link_poll_edges(c, s.was_connected, connects, s.seen_connects);
    s.seen_connects = connects;
    if (edges.lost) {
        s.lobby.on_lost();
        fprintf(stderr, "[lobby] %s\n", s.lobby.status().c_str());
    }
    if (edges.connected) {
        s.lobby.on_connected(now);
        rs64::net::Hello h;
        h.build = s.lobby.local_build();
        h.role = s.player;
        // The pilot name (gGameSettings+7), for the label over this player's ship on the other screen.
        if (s.rdram) {
            for (int i = 0; i < 3; ++i) {
                h.name[i] = (char)rb(s.rdram, 0x80130B47u + (uint32_t)i);
            }
        }
        s.link.send(rs64::net::encode_hello(h), true);
    }
    s.was_connected = c;
    s.lobby.tick(now);
    const bool ready = s.lobby.ready();
    if (ready && !s.was_ready) {
        s.lobby_session = true;
        s.peer_left = false;
        fprintf(stderr, "[lobby] %s (version ok)\n", s.player == 0 ? "player connected" : "connected to the host");
    }
    const bool had_ready = s.was_ready;
    s.was_ready = ready;
    if (s.lobby.state() == rs64::net::LobbyState::Failed && s.stop_at_ms < 0) {
        fprintf(stderr, "[lobby] failed: %s\n", s.lobby.status().c_str());
        // A session in progress loses its peer here, not through link.lost(): the stop below clears that flag.
        if ((had_ready || s.in_session) && !s.peer_left) {
            s.peer_left = true;
            fprintf(stderr, "[ghost] peer left (connection lost)\n");
        }
        // Stop a little later, so what is still queued (our HELLO, when the peer's arrived in the same batch) goes out and the peer sees the mismatch too.
        s.stop_at_ms = (int64_t)now + 500;
    }
    if (s.stop_at_ms >= 0 && (int64_t)now >= s.stop_at_ms) {
        s.link.stop();
        s.started = false;
        s.stop_at_ms = -1;
    }
}

void wf(uint8_t* rdram, uint32_t a, float v) {
    uint32_t b;
    memcpy(&b, &v, 4);
    ww(rdram, a, b);
}

// Ends this machine's mission out of lives, the way the game does (result 3 + request bit 1); on the host its RESULT carries it to the client.
void game_over(uint8_t* rdram, const char* why) {
    wb(rdram, 0x80130B14u, 3);
    ww(rdram, 0x8010C9E0u, rw(rdram, 0x8010C9E0u) | 1u);
    fprintf(stderr, "[ghost] game over: %s\n", why);
}

// Out of shared lives (host): the client is told at once, not after this side's game-over sequence, or a spectating client sits on the survivor's last pose until then.
void send_game_over() {
    Ghost& s = g();
    if (!s.result_sent && !s.peer_left) {
        s.link.send(rs64::net::encode_result(s.epoch, 3), true);
        s.result_sent = true;
        fprintf(stderr, "[ghost] host result 3 sent early\n");
    }
}

void drain() {
    Ghost& s = g();
    // The connect edge first: the peer's HELLO can arrive in the same batch as the connection, and the lobby ignores a HELLO before it knows it is connected.
    lobby_poll();
    for (const auto& m : s.link.receive()) {
        const uint8_t type = rs64::net::message_type(m.data(), m.size());
        if (type == (uint8_t)rs64::net::Msg::State) {
            rs64::net::StatePacket p;
            if (!rs64::net::decode_state(m.data(), m.size(), &p) || !valid_state(p)) {
                continue;
            }
            if (p.epoch != s.remote_epoch) {
                if (!epoch_newer(p.epoch, s.remote_epoch)) {
                    continue;
                }
                s.remote_epoch = p.epoch;
                s.last_seq = 0;
                s.have_remote = false;
                s.remote_in = true;
            }
            if (s.have_remote && p.seq <= s.last_seq) {
                continue;
            }
            s.last_seq = p.seq;
            s.have_remote = true;
            s.remote_rx_s = now_s();
            memcpy(s.remote_pad, p.pad, 6);
            s.remote_foil = p.foil;
            s.remote_down = (p.flags & rs64::net::kStateDown) != 0;
            s.remote_pos = {p.pos[0], p.pos[1], p.pos[2]};
            PuppetState ps;
            ps.pos = {p.pos[0], p.pos[1], p.pos[2]};
            ps.fwd = {p.fwd[0], p.fwd[1], p.fwd[2]};
            ps.down = {p.down[0], p.down[1], p.down[2]};
            ps.vel = {p.vel[0], p.vel[1], p.vel[2]};
            s.puppet.on_state(ps, s.remote_rx_s, p.sent_ms / 1000.0);
        } else if (type == (uint8_t)rs64::net::Msg::Presence) {
            uint8_t e = 0, in = 0;
            if (!rs64::net::decode_presence(m.data(), m.size(), &e, &in)) {
                continue;
            }
            if (in) {
                fprintf(stderr, "[ghost] peer in mission\n");
                // Whatever this side destroyed or picked up before the peer arrived goes over again.
                s.resync = true;
                s.upgrades_shared = 0;
                for (uint16_t i : s.pickups_mine) {
                    s.link.send(rs64::net::encode_pickup(mission_tag(), i), true);
                }
                for (uint16_t i : s.tows_mine) {
                    s.link.send(rs64::net::encode_tow_trip(mission_tag(), i), true);
                }
            } else if (s.remote_in && (s.remote_epoch < 0 || e == (uint8_t)s.remote_epoch)) {
                s.remote_in = false;
                s.team_triggers.forget(1);
                fprintf(stderr, "[ghost] peer left the mission\n");
            }
        } else if (type == (uint8_t)rs64::net::Msg::Result) {
            uint8_t e = 0, r = 0;
            // One result per mission: the host also repeats it at its mission end, and applying it again mid-transition re-raises the end request.
            if (rs64::net::decode_result(m.data(), m.size(), &e, &r) && s.player == 1 && s.in_session && s.host_result < 0 && (s.remote_epoch < 0 || e == (uint8_t)s.remote_epoch)) {
                s.pending_result = r;
                s.host_result = r;
                fprintf(stderr, "[ghost] host result %u received\n", r);
            }
        } else if (type == (uint8_t)rs64::net::Msg::Obj) {
            rs64::net::ObjSnapshot o;
            if (s.player == 1 && s.in_session && rs64::net::decode_obj(m.data(), m.size(), &o) && o.epoch == s.mission_id && o.seq > s.obj_last_seq) {
                s.obj_last_seq = o.seq;
                s.obj_pending = o;
                s.obj_have = true;
            }
        } else if (type == (uint8_t)rs64::net::Msg::Life) {
            uint8_t e = 0;
            if (s.player == 0 && s.in_session && s.rdram && rs64::net::decode_life_lost(m.data(), m.size(), &e) && e == s.epoch) {
                uint8_t& lives = s.rdram[(kLives - 0x80000000u) ^ 3];
                switch (rs64::ls::shared_life(lives, s.remote_out)) {
                case rs64::ls::LifeOutcome::Spend:
                    lives -= 1;
                    fprintf(stderr, "[ghost] client lost a life; shared lives now %u\n", lives);
                    break;
                case rs64::ls::LifeOutcome::Out:
                    lives = 0;
                    s.remote_out = true;
                    s.link.send(rs64::net::encode_out(s.epoch), true);
                    fprintf(stderr, "[ghost] client lost the last shared life: out, spectating\n");
                    break;
                case rs64::ls::LifeOutcome::GameOver:
                    game_over(s.rdram, "client down with no lives left");
                    send_game_over();
                    break;
                case rs64::ls::LifeOutcome::Ignore:
                    break;
                }
            }
        } else if (type == (uint8_t)rs64::net::Msg::Out) {
            uint8_t e = 0;
            if (s.player == 1 && s.in_session && rs64::net::decode_out(m.data(), m.size(), &e) && e == s.mission_id && !s.out) {
                s.out = true;
                fprintf(stderr, "[ghost] out of shared lives: spectating\n");
            }
        } else if (type == (uint8_t)rs64::net::Msg::Damage) {
            uint8_t e = 0;
            std::vector<std::pair<uint16_t, int32_t>> items;
            if (s.in_session && rs64::net::decode_damage(m.data(), m.size(), &e, &items) && e == mission_tag()) {
                s.dat_apply.insert(s.dat_apply.end(), items.begin(), items.end());
            }
        } else if (type == (uint8_t)rs64::net::Msg::Upgrades) {
            uint8_t e = 0;
            uint32_t bits = 0;
            if (s.in_session && rs64::net::decode_upgrades(m.data(), m.size(), &e, &bits) && e == mission_tag()) {
                s.upgrades_in |= bits;
            }
        } else if (type == (uint8_t)rs64::net::Msg::Pickup) {
            uint8_t e = 0;
            uint16_t i = 0;
            if (s.in_session && s.rdram && rs64::net::decode_pickup(m.data(), m.size(), &e, &i) && e == mission_tag() && i < dat_count(s.rdram)) {
                const uint32_t item = dat_item(s.rdram, i);
                if (item && std::find(s.pickups_forced.begin(), s.pickups_forced.end(), item) == s.pickups_forced.end()) {
                    s.pickups_forced.push_back(item);
                    fprintf(stderr, "[ghost] the other player collected power-up %u\n", i);
                }
            }
        } else if (type == (uint8_t)rs64::net::Msg::Trigger) {
            uint8_t e = 0;
            uint16_t idx = 0;
            bool enter = false;
            if (s.player == 0 && s.in_session && rs64::net::decode_trigger(m.data(), m.size(), &e, &idx, &enter) && e == mission_tag()) {
                s.trigger_in.emplace_back(idx, enter);
            }
        } else if (type == (uint8_t)rs64::net::Msg::TowTrip) {
            uint8_t e = 0;
            uint16_t i = 0;
            if (s.in_session && s.rdram && rs64::net::decode_tow_trip(m.data(), m.size(), &e, &i) && e == mission_tag() && i < dat_count(s.rdram) && std::find(s.tow_in.begin(), s.tow_in.end(), i) == s.tow_in.end()) {
                s.tow_in.push_back(i);
            }
        } else if (type == (uint8_t)rs64::net::Msg::Mission) {
            uint8_t id = 0;
            if (s.player == 1 && rs64::net::decode_mission(m.data(), m.size(), &id)) {
                s.latest_mission = id;
            }
        } else if (type == (uint8_t)rs64::net::Msg::Hello) {
            rs64::net::Hello h;
            if (rs64::net::decode_hello(m.data(), m.size(), &h)) {
                s.remote_name.assign(h.name, strnlen(h.name, 3));
                s.lobby.on_hello(h);
                if (!s.lobby.ready() && s.lobby.state() == rs64::net::LobbyState::Failed) {
                    fprintf(stderr, "[lobby] peer build %08X protocol %u, this build %08X protocol %u\n", h.build, h.protocol, s.lobby.local_build(), rs64::net::kProtocol);
                }
            }
        } else if (type == (uint8_t)rs64::net::Msg::Skip) {
            if (rs64::net::decode_skip(m.data(), m.size())) {
                s.skip.on_remote(now_s());
                fprintf(stderr, "[ghost] the other player skipped the cutscene\n");
            }
        } else if (type == (uint8_t)rs64::net::Msg::BriefingDone) {
            if (s.player == 0 && rs64::net::decode_briefing_done(m.data(), m.size())) {
                s.brief_done_ms = now_ms32();
            }
        } else if (type == (uint8_t)rs64::net::Msg::Browse) {
            uint8_t level = 0, state = 0;
            if (s.player == 1 && rs64::net::decode_browse(m.data(), m.size(), &level, &state)) {
                s.host_cursor = level;
                s.host_state = state;
            }
        } else if (type == (uint8_t)rs64::net::Msg::Ready) {
            uint8_t craft = 0;
            if (rs64::net::decode_ready(m.data(), m.size(), &craft)) {
                s.craft.on_remote(craft == 0xFF ? -1 : craft);
                if (craft != 0xFF) {
                    s.remote_craft = craft;
                } else if (s.in_hangar && !s.left_sent) {
                    // They went back to mission select: follow them there.
                    s.follow_out = true;
                    s.release_cool = 0;
                }
                fprintf(stderr, "[lobby] partner %s\n", craft == 0xFF ? "left craft select" : "picked a craft");
            }
        } else if (type == (uint8_t)rs64::net::Msg::Pick) {
            uint8_t level = 0;
            if (s.player == 1 && rs64::net::decode_pick(m.data(), m.size(), &level)) {
                s.host_pick = level;
                fprintf(stderr, "[lobby] host picked level %u\n", level);
            }
        } else if (type == (uint8_t)rs64::net::Msg::Launch) {
            rs64::net::decode_launch(m.data(), m.size(), &s.launch);
        } else if (type == (uint8_t)rs64::net::Msg::Bye && !s.peer_left) {
            s.peer_left = true;
            fprintf(stderr, "[ghost] peer left\n");
        }
    }
    if (s.link.lost() && !s.peer_left) {
        s.peer_left = true;
        fprintf(stderr, "[ghost] peer left (connection lost)\n");
    }
    lobby_poll();
}

template <typename Pred>
bool wait_for(uint8_t* rdram, recomp_context* ctx, uint32_t timeout_ms, Pred pred) {
    const auto t0 = std::chrono::steady_clock::now();
    while (true) {
        drain();
        if (pred()) {
            return true;
        }
        if (g().link.lost() || g().peer_left || std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(timeout_ms)) {
            return false;
        }
        rs64::mp::yield(rdram, ctx);
    }
}

void load_pad_recording(const char* path, std::vector<FrameInput>* out) {
    out->clear();
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[ghost] cannot read %s\n", path);
        return;
    }
    char line[256];
    bool in_frames = false;
    while (fgets(line, sizeof(line), f)) {
        if (!in_frames) {
            in_frames = strncmp(line, "---", 3) == 0;
            continue;
        }
        FrameInput fi;
        if (parse_frame(line, &fi)) {
            out->push_back(fi);
        }
    }
    fclose(f);
}

// Frames into a mission before the other player's damage is applied.
constexpr uint32_t kDamageSettleFrames = 30;

// Shared deaths: the peer's health drops go in through the game's own damage path (dealDamagetoDatItem 0x800C7390: a HIT for a spawned item, a stored-health cut for an unspawned one); then our own drops go out.
void sync_dat(uint8_t* rdram, recomp_context* ctx) {
    Ghost& s = g();
    const uint32_t n = dat_count(rdram);
    if (s.dat_baseline && s.dat_last.size() != n) {
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
    for (const auto& d : settled ? s.dat_apply : retry) {
        if (d.first >= n) {
            continue;
        }
        const uint32_t item = dat_item(rdram, d.first);
        if (!dat_tracked(rdram, item)) {
            continue;
        }
        // A tripped walker dies from its own fall (which counts the objective); a hit on it would skip that count.
        if ((rw(rdram, item + 0x4C) & 0x80000000u) || std::find(s.tow_in.begin(), s.tow_in.end(), d.first) != s.tow_in.end()) {
            continue;
        }
        const int32_t local = dat_health(rdram, item);
        if (local > 0 && local > d.second && !dat_damage_safe(rdram, item)) {
            // An inconsistent slot is usually mid-teardown; retry for up to ~2 s.
            if (s.dat_retry_frames[d.first]++ < 60) {
                retry.push_back(d);
            } else {
                s.dat_retry_frames.erase(d.first);
            }
            continue;
        }
        s.dat_retry_frames.erase(d.first);
        if (local > 0 && local > d.second) {
            const uint32_t slot = rs64::mips::rh(rdram, item + 6);
            const uint32_t dmg = (uint32_t)(local - d.second);
            if (slot == 0xFFFFu) {
                // Unspawned: dealDamagetoDatItem cuts the stored health, so the item streams in damaged or as rubble.
                rs64::mp::call(rdram, ctx, 0x800C7390u, {rw(rdram, item + 0xC), dmg});
            } else {
                // Spawned: the HIT dealDamagetoDatItem sends (action 5, weapon 7), without its follow-up lookup, which reads the freed slot after a lethal hit.
                const uint32_t msg = 0x80B40100u;
                ww(rdram, msg + 0x0, 0xFFFFFFFFu);
                ww(rdram, msg + 0x4, (7u << 16) | (dmg > 0xFFFFu ? 0xFFFFu : dmg));
                ww(rdram, msg + 0x8, 0u);
                rs64::mp::call(rdram, ctx, 0x8003E8DCu, {slot, 5u, msg});
            }
            if (s.dat_applied++ < 20) {
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
    if (s.peer_left) {
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

// The other player's tow-cable trips: this world's copy gets the harpoon's own trip message (action 9, kind 0xE at +0x18), with sender slot 0xFFFF so the walker's release reply goes nowhere. An unspawned walker waits until it spawns.
void apply_remote_tows(uint8_t* rdram, recomp_context* ctx) {
    Ghost& s = g();
    if (s.tow_in.empty() || s.frame < kDamageSettleFrames || rs64::mp::in_cutscene()) {
        return;
    }
    std::vector<uint16_t> wait;
    for (uint16_t i : s.tow_in) {
        const uint32_t item = dat_item(rdram, i);
        if (!item || (rw(rdram, item + 0x4C) & 0x80000000u) || dat_health(rdram, item) <= 0) {
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
        if (rw(rdram, item + 0x4C) & 0x80000000u) {
            s.tow_retry.erase(i);
            fprintf(stderr, "[ghost] walker %u tripped for the other player\n", i);
            continue;
        }
        // Not taken (the walker ignored the message in its current state): retry for ~2 s, then let the other player's mirrored damage reach it again.
        if (s.tow_retry[i]++ < 60) {
            wait.push_back(i);
        } else {
            s.tow_retry.erase(i);
            fprintf(stderr, "[ghost] walker %u did not take the other player's trip; mirrored damage applies\n", i);
        }
    }
    s.tow_in.swap(wait);
}

// Shared power-ups: a pickup ORs one bit into the settings word (npcPowerUpUpdate 0x800EBDB0-0x800EBE4C). Bits gained this mission go to the peer, theirs are ORed in here; the game's own commit saves them to the pilot on success and reverts them otherwise.
void sync_upgrades(uint8_t* rdram) {
    constexpr uint32_t kSettings = 0x80130B4Cu;
    Ghost& s = g();
    const uint32_t word = rw(rdram, kSettings);
    if (s.upgrades_base < 0) {
        s.upgrades_base = word & rs64::net::kUpgradeMask;
    }
    const uint32_t add = s.upgrades_in & ~word;
    if (add) {
        ww(rdram, kSettings, word | add);
        fprintf(stderr, "[ghost] upgrades %05X from the other player\n", add);
    }
    s.upgrades_shared |= s.upgrades_in;
    s.upgrades_in = 0;
    const uint32_t gained = word & rs64::net::kUpgradeMask & ~(uint32_t)s.upgrades_base & ~s.upgrades_shared;
    if (gained && !s.peer_left) {
        s.link.send(rs64::net::encode_upgrades(mission_tag(), gained), true);
        s.upgrades_shared |= gained;
        fprintf(stderr, "[ghost] upgrades %05X picked up; sent to the other player\n", gained);
    }
}

// npcPowerUpUpdate's touch test (0x800EBD20, s1 = the power-up, its DAT record at +4): a power-up the other player collected passes it (squared distance f4 = 0), so this world runs the game's own pickup.
extern "C" void rs64_ghost_powerup_touch(uint8_t* rdram, recomp_context* ctx) {
    Ghost& s = g();
    // ROGUESQ_MP_TEST_PICKUP=<frame>: the host collects the first power-up that ticks from that mission frame on, as if it flew through it.
    static const long test_pickup = recomp::dbg::env_int("ROGUESQ_MP_TEST_PICKUP", -1);
    if (test_pickup >= 0 && s.player == 0 && s.in_session && (long)s.frame >= test_pickup && !s.test_pickup_done) {
        s.test_pickup_done = true;
        ctx->f4.fl = 0.0f;
        fprintf(stderr, "[ghost] test pickup at frame %u\n", s.frame);
        return;
    }
    if (s.pickups_forced.empty()) {
        return;
    }
    const uint32_t item = rw(rdram, (uint32_t)ctx->r17 + 4u);
    if (std::find(s.pickups_forced.begin(), s.pickups_forced.end(), item) != s.pickups_forced.end()) {
        ctx->f4.fl = 0.0f;
    }
}

// The pickup itself (0x800EBD34, a1 = the sound's position): one the other player collected plays at this player's ship (player block 0x80137DC0) so it is heard; this player's own goes to the other player by DAT index.
extern "C" void rs64_ghost_powerup_collect(uint8_t* rdram, recomp_context* ctx) {
    Ghost& s = g();
    const uint32_t item = rw(rdram, (uint32_t)ctx->r17 + 4u);
    const auto forced = std::find(s.pickups_forced.begin(), s.pickups_forced.end(), item);
    if (forced != s.pickups_forced.end()) {
        s.pickups_forced.erase(forced);
        ctx->r5 = (gpr)(int32_t)0x80137DC0u;
        fprintf(stderr, "[ghost] power-up collected for the other player\n");
        return;
    }
    if (!s.in_session || s.peer_left) {
        return;
    }
    const uint32_t n = dat_count(rdram);
    for (uint32_t i = 0; i < n; ++i) {
        if (dat_item(rdram, i) == item) {
            s.pickups_mine.push_back((uint16_t)i);
            s.link.send(rs64::net::encode_pickup(mission_tag(), (uint16_t)i), true);
            fprintf(stderr, "[ghost] power-up %u collected; sent to the other player\n", i);
            return;
        }
    }
}

// npcAtAtUpdate accepting a trip (0x800CED7C, s1 = the AT-AT ext, +0x34 its DAT item): the other player trips its own copy, whose fall then kills it and counts the objective in its world.
extern "C" void rs64_ghost_walker_tripped(uint8_t* rdram, recomp_context* ctx) {
    Ghost& s = g();
    if (!s.in_session || s.peer_left || s.tow_injecting) {
        return;
    }
    const uint32_t item = rw(rdram, (uint32_t)ctx->r17 + 0x34u);
    const uint32_t n = dat_count(rdram);
    for (uint32_t i = 0; i < n; ++i) {
        if (dat_item(rdram, i) == item) {
            s.tows_mine.push_back((uint16_t)i);
            s.link.send(rs64::net::encode_tow_trip(mission_tag(), (uint16_t)i), true);
            fprintf(stderr, "[ghost] walker %u tripped; sent to the other player\n", i);
            return;
        }
    }
    fprintf(stderr, "[ghost] tripped walker %08X is not a level DAT item; not shared\n", item);
}

// The client's trigger edges, applied through the game's own effect (host): the next snapshot carries the result back to the client.
void apply_remote_triggers(uint8_t* rdram, recomp_context* ctx) {
    Ghost& s = g();
    // Like mirrored damage, not before the host's mission settles or during its cutscene: the edges wait, in order.
    if (s.trigger_in.empty() || s.frame < kDamageSettleFrames || rs64::mp::in_cutscene()) {
        return;
    }
    for (const auto& t : s.trigger_in) {
        const uint32_t ev = trigger_event(rdram, t.first);
        if (!ev) {
            fprintf(stderr, "[ghost] trigger %u from the other player is not a trigger event here; ignored\n", t.first);
            continue;
        }
        if (!s.team_triggers.on_edge(1, t.first, t.second)) {
            continue;
        }
        s.trigger_applying = true;
        rs64::mp::call(rdram, ctx, 0x80065980u, {ev, t.second ? 1u : 0u});
        s.trigger_applying = false;
        fprintf(stderr, "[ghost] trigger %u %s from the other player applied\n", t.first, t.second ? "enter" : "exit");
    }
    s.trigger_in.clear();
}

// Applies the host's result on the client: success/fail by raising the game's own end request (refused during a respawn or pause, so retried each frame), abort and out-of-lives by the direct write the game's abort uses.
void apply_pending_result(uint8_t* rdram) {
    Ghost& s = g();
    const int r = s.pending_result;
    const uint32_t req = rw(rdram, 0x8010C9E0u);
    bool done = false;
    if (r == 1 || r == 2) {
        // Success: request bit 8 (setHudEnableBit8) -> requestMissionTransitionMode(3) -> mode 3 -> requestSpeechResponseMode1 writes 1.
        // Failure: bit 4 (setHudEnableBit4) -> (2) -> mode 4 -> requestSpeechResponseMode2 writes 2. The client's own pending request for the other one is dropped.
        const uint32_t bit = r == 1 ? 8u : 4u;
        const uint32_t other = r == 1 ? 4u : 8u;
        const uint32_t end_mode = r == 1 ? 3u : 4u;
        s.allow_transition = r == 1 ? 3 : 2;
        // The bit is raised directly: the request functions also reset speech playback (initSpeechPlaybackState), which would cut off the end cue the client's own level script has just started.
        ww(rdram, 0x8010C9E0u, (req & ~other) | bit);
        done = rw(rdram, 0x8010CA20u) == end_mode;
    } else if (rw(rdram, 0x8010CA20u) != 5) {
        // Abort / game over: not while the pause menu is open (its own close-down would be skipped).
        wb(rdram, 0x80130B14u, (uint8_t)r);
        ww(rdram, 0x8010C9E0u, req | 1u);
        done = true;
    }
    if (done) {
        fprintf(stderr, "[ghost] applied host result %d\n", r);
        s.pending_result = -1;
    }
}

}

using namespace rs64::ghost;
using namespace rs64::ls;
using rs64::mips::rw;
using rs64::mips::ww;
using rs64::mips::rf;

// ROGUESQ_MP, or two players connected from the MULTIPLAYER menu: ghost co-op (independent simulations, remote player as a puppet).
extern "C" uint32_t rs64_ghost_mode(void) {
    static const uint32_t on = recomp::dbg::env_str("ROGUESQ_MP") ? 1u : 0u;
    return (on || rs64_lobby_menu_session()) ? 1u : 0u;
}

extern "C" uint32_t rs64_ghost_active(void) {
    return g().in_session ? 1u : 0u;
}

// Quit: tells the other player at once instead of leaving them to the connection timeout. Runs on a game thread (poll_input) but _Exit follows, so the 200 ms wait is acceptable only on this exit path.
extern "C" void rs64_ghost_quit(void) {
    Ghost& s = g();
    if (s.started && s.link.connected()) {
        s.link.send(rs64::net::encode_bye(rs64::net::ByeReason::Quit, s.frame), true);
        s.link.flush(200);
    }
    // After the BYE: the relay carries it.
    s.link.stop();
    relay_shutdown();
}

// Recording-driven test runs keep the harness's fixed 1/30 s step so a recording replays at its own pace; live play keeps the game's dt.
extern "C" uint32_t rs64_ghost_fixed_dt(void) {
    return (g().in_session && !g().frames.empty()) ? 1u : 0u;
}

extern "C" uint32_t rs64_ghost_is_client(void) {
    static const uint32_t env_client = role_from_env() == rs64::net::Role::Client ? 1u : 0u;
    return g().from_menu ? (g().player == 1 ? 1u : 0u) : env_client;
}

// The client's own start offset, applied once per mission by rs64_imposter_frame.
extern "C" uint32_t rs64_ghost_take_start_offset(void) {
    Ghost& s = g();
    if (!s.in_session || s.start_offset_done) {
        return 0u;
    }
    s.start_offset_done = true;
    return 1u;
}

// Whether the other player's puppet should exist now (in this mission, not down, state received), and where to spawn it.
extern "C" uint32_t rs64_ghost_want_puppet(float out_pos[3]) {
    const Ghost& s = g();
    if (!s.in_session || s.peer_left || !s.remote_in || s.remote_down || !s.have_remote || s.out) {
        return 0u;
    }
    out_pos[0] = s.remote_pos.x;
    out_pos[1] = s.remote_pos.y;
    out_pos[2] = s.remote_pos.z;
    return 1u;
}

extern "C" void rs64_ghost_puppet_spawned(void) {
    Ghost& s = g();
    if (s.despawned && s.puppet_seen) {
        fprintf(stderr, "[ghost] puppet back\n");
    }
    s.despawned = false;
    s.puppet_seen = true;
    if (s.puppet.has_target()) {
        s.puppet.snap();
    }
}

// runInMissionFrame's freeze branch (0x800FA8E4, taken when 0x8010CA1C & 1): while paused (mode 5) in a session, the world keeps ticking; the pause menu, tint and audio duck still run.
extern "C" uint32_t rs64_ghost_unfreeze(uint8_t* rdram) {
    return (g().in_session && !g().peer_left && rw(rdram, 0x8010CA20u) == 5) ? 1u : 0u;
}

extern "C" uint32_t rs64_ghost_block_result(void) {
    const Ghost& s = g();
    return (s.in_session && s.player == 1 && !s.peer_left) ? 1u : 0u;
}

// requestMissionTransitionMode(a0): arg 3 = success end mode, 2 = failure. A client in session enters them only for the host's result; the host announces its result as soon as it enters one (mode 0 = the transition will be accepted).
extern "C" uint32_t rs64_ghost_block_transition(uint8_t* rdram, uint32_t arg) {
    Ghost& s = g();
    if (!s.in_session || s.peer_left) {
        return 0u;
    }
    // arg 1 = the local player died (mode 0 = accepted now; it decrements numLives and ends the mission with result 3 at 0).
    // A client never runs out on its own: it respawns and reports the death, and the host rules on it. Whether a life was really spent is checked next frame.
    if (arg == 1 && rw(rdram, 0x8010CA20u) == 0) {
        uint8_t& lives = rdram[(kLives - 0x80000000u) ^ 3];
        if (s.out) {
            s.restore_lives = lives;
            lives = 2;
        } else if (s.player == 1) {
            if (lives <= 1) {
                lives = 2;
            }
            s.lives_before = lives;
            s.life_check = true;
        } else {
            switch (rs64::ls::shared_life(lives, false)) {
            case rs64::ls::LifeOutcome::Out:
                s.out = true;
                s.restore_lives = 0;
                lives = 2;
                fprintf(stderr, "[ghost] host lost the last shared life: out, spectating\n");
                break;
            case rs64::ls::LifeOutcome::GameOver:
                // The client is already out: the game's own decrement to 0 ends the mission with result 3.
                lives = 1;
                fprintf(stderr, "[ghost] game over: host down with no lives left\n");
                send_game_over();
                break;
            default:
                break;
            }
        }
        return 0u;
    }
    if (arg == 1) {
        return 0u;
    }
    if (arg != 2 && arg != 3) {
        return 0u;
    }
    if (s.player == 0) {
        if (!s.result_sent && rw(rdram, 0x8010CA20u) == 0) {
            const uint8_t result = arg == 3 ? 1 : 2;
            s.link.send(rs64::net::encode_result(s.epoch, result), true);
            s.result_sent = true;
            fprintf(stderr, "[ghost] host result %u sent early\n", result);
        }
        return 0u;
    }
    return (int)arg == s.allow_transition ? 0u : 1u;
}

// datItemSetObjectiveBooleanCount: a client in session drops the write. Its callers are death handlers, and every death is mirrored into the host's world (sync_dat), where the host's handler counts it once; the host's snapshot brings the counts back.
extern "C" uint32_t rs64_ghost_objective_event(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
    const Ghost& s = g();
    return (s.in_session && s.player == 1 && !s.peer_left) ? 1u : 0u;
}

// applyDatObjectiveTriggerEffect, for player 1's own trigger volumes (s0 = the list entry): a client sends the edge to the host and drops it, since the host's snapshot brings the result back; the host runs it once per team.
extern "C" uint32_t rs64_ghost_trigger_effect(uint8_t* rdram, recomp_context* ctx) {
    Ghost& s = g();
    if (!s.in_session || s.peer_left || s.trigger_applying) {
        return 0u;
    }
    const uint32_t entry = (uint32_t)ctx->r16;
    if (entry < kPlyList || entry >= kPlyListEnd) {
        return 0u;
    }
    const uint16_t idx = rs64::mips::rh(rdram, entry) & 0xFFFu;
    const bool enter = (ctx->r5 & 0xFF) == 1;
    if (s.player == 1) {
        s.link.send(rs64::net::encode_trigger(mission_tag(), idx, enter), true);
        fprintf(stderr, "[ghost] trigger %u %s sent to the host\n", idx, enter ? "enter" : "exit");
        return 1u;
    }
    // ROGUESQ_MP_TEST_TRIGGER_SOLO=1: the host drops its own trigger edges, so only the client's can advance the mission.
    static const bool solo = recomp::dbg::env_on("ROGUESQ_MP_TEST_TRIGGER_SOLO");
    if (solo) {
        fprintf(stderr, "[ghost] test: own trigger %u %s dropped\n", idx, enter ? "enter" : "exit");
        return 1u;
    }
    return s.team_triggers.on_edge(0, idx, enter) ? 0u : 1u;
}

// requestSpeechResponseMode1/2 land result 1/2 when `ready`: on a client in session only the host's result may land, whatever raised the request in its own world.
extern "C" uint32_t rs64_ghost_gate_response(uint32_t result, uint32_t ready) {
    const Ghost& s = g();
    if (!s.in_session || s.player != 1 || s.peer_left) {
        return ready;
    }
    return s.host_result == (int)result ? ready : 0u;
}

// initMission: connect (first mission only), exchange LAUNCH so both are on the same level and craft, then announce this mission.
extern "C" void rs64_ghost_mission_init(uint8_t* rdram, recomp_context* ctx) {
    Ghost& s = g();
    s.rdram = rdram;
    s.in_session = false;
    s.pending_result = -1;
    s.host_result = -1;
    s.allow_transition = -1;
    // The menus' round state ends here on every path (a mission played alone included): a late PICK or READY belongs to the round that just ended.
    s.craft = rs64::ls::CraftBarrier{};
    s.host_pick = -1;
    s.host_cursor = -1;
    s.host_state = 0;
    s.in_hangar = false;
    s.follow_out = false;
    s.lives_seen = -1;
    if (!rs64_ghost_mode() || demo_active(rdram)) {
        return;
    }
    drain();
    if (!s.lobby.ready() && !s.lobby_session) {
        // ROGUESQ_MP without the menus: the same handshake, started here. The first mission waits for the other player to arrive; later missions only proceed together with a live peer.
        const rs64::net::LinkConfig cfg = link_config();
        if (cfg.role == rs64::net::Role::None) {
            fprintf(stderr, "[ghost] ROGUESQ_MP must be host or join; playing alone\n");
            return;
        }
        if (!s.started && s.missions == 0) {
            s.lobby.set_join_timeout(300000);
            lobby_start(cfg);
        }
        const uint32_t connect_ms = s.missions == 0 ? 300000u : 0u;
        wait_for(rdram, ctx, connect_ms, [&]() { return s.lobby.ready() || s.lobby.state() == rs64::net::LobbyState::Failed; });
    }
    if (s.peer_left || !s.lobby.ready()) {
        fprintf(stderr, "[ghost] %s; playing alone\n", s.lobby.status() == "VERSION MISMATCH" ? "version mismatch" : "no peer");
        return;
    }
    if (s.from_menu && s.missions == 0) {
        fprintf(stderr, "[ghost] using the lobby link\n");
    }
    const SessionHeader running = read_session(rdram, 1);
    if (s.player == 0) {
        // The mission id goes first on the same reliable channel, so the client has it when LAUNCH arrives.
        s.mission_id = (uint8_t)(s.epoch + 1);
        s.link.send(rs64::net::encode_mission(s.mission_id), true);
        s.link.send(rs64::net::encode_launch(format_header(running)), true);
    } else {
        SessionHeader h;
        if (!wait_for(rdram, ctx, 60000, [&]() { return !s.launch.empty(); }) || !parse_header(s.launch, &h)) {
            fprintf(stderr, "[ghost] no LAUNCH from the host; playing alone\n");
            return;
        }
        s.launch.clear();
        // A different mission means this mission alone, not the end of the session; each player flies their own craft.
        if (h.level != running.level) {
            fprintf(stderr, "[ghost] level mismatch: host level %u, here level %u; playing this mission alone\n", h.level, running.level);
            return;
        }
        s.remote_craft = h.craft;
        fprintf(stderr, "[ghost] level %u, craft here %u, host's craft %u\n", running.level, running.craft, h.craft);
        s.mission_id = s.latest_mission >= 0 ? (uint8_t)s.latest_mission : 0;
    }
    s.missions++;
    s.epoch++;
    s.in_session = true;
    s.seq = 0;
    s.have_prev = false;
    s.puppet = Puppet{};
    static const long interp_ms = recomp::dbg::env_int("ROGUESQ_GHOST_INTERP_MS", 50);
    s.puppet.set_interp(interp_ms / 1000.0);
    s.puppet_shown = false;
    // Nothing spawned yet: rs64_ghost_puppet_spawned clears this when the puppet appears.
    s.despawned = true;
    s.start_offset_done = false;
    s.puppet_seen = false;
    s.frame = 0;
    s.rdram = rdram;
    s.result_sent = false;
    s.resync = false;
    s.life_check = false;
    s.out = false;
    s.remote_out = false;
    s.restore_lives = -1;
    s.obj_sent_any = false;
    s.obj_resend = 0;
    s.obj_have = false;
    s.obj_last_seq = 0;
    s.obj_applied = 0;
    s.dat_last.clear();
    s.dat_baseline = false;
    s.dat_apply.clear();
    s.dat_retry_frames.clear();
    s.dat_sent = 0;
    s.dat_applied = 0;
    s.upgrades_base = -1;
    s.upgrades_shared = 0;
    s.upgrades_in = 0;
    s.pickups_forced.clear();
    s.pickups_mine.clear();
    s.team_triggers.reset();
    s.trigger_in.clear();
    s.trigger_applying = false;
    s.tows_mine.clear();
    s.tow_in.clear();
    s.tow_retry.clear();
    s.tow_injecting = false;
    if (const char* pad = recomp::dbg::env_str("ROGUESQ_MP_PAD")) {
        load_pad_recording(pad, &s.frames);
    }
    if (const char* path = recomp::dbg::env_str("ROGUESQ_GHOST_TRACE")) {
        if (!s.trace) {
            s.trace = fopen(path, "wb");
            if (s.trace) {
                fprintf(s.trace, "ms,local_x,local_y,local_z,puppet_x,puppet_y,puppet_z\n");
            }
        }
    }
    s.link.send(rs64::net::encode_presence(s.epoch, 1), true);
    fprintf(stderr, "[ghost] %s in session: level %u craft %u epoch %u, %zu recorded input frames\n", s.player == 0 ? "host" : "client", running.level, running.craft, s.epoch, s.frames.size());
}

// endMissionCleanup: announce leaving the mission; the host also sends its result.
extern "C" void rs64_ghost_mission_end(uint8_t* rdram) {
    Ghost& s = g();
    if (!s.in_session) {
        return;
    }
    const uint8_t result = rb(rdram, 0x80130B14u);
    fprintf(stderr, "[ghost] mission ended: result %u (objective snapshots applied %u, damage sent %u applied %u)\n", result, s.obj_applied, s.dat_sent, s.dat_applied);
    if (s.player == 0) {
        s.link.send(rs64::net::encode_result(s.epoch, result), true);
    }
    s.link.send(rs64::net::encode_presence(s.epoch, 0), true);
    s.in_session = false;
}

// Frame start (after the pad poll): remote messages in, the host's result applied, local input from a recording if one is set, local state out, remote pad onto port 1.
extern "C" void rs64_ghost_frame(uint8_t* rdram, recomp_context* ctx) {
    Ghost& s = g();
    if (!s.started) {
        return;
    }
    drain();
    if (!s.in_session || demo_active(rdram)) {
        return;
    }
    s.rdram = rdram;
    // A client death last frame: a lower lives byte means a life was spent (levels and cheats where a death costs none leave it alone). Checked before the host's snapshot overwrites it.
    if (s.player == 1 && s.life_check) {
        s.life_check = false;
        if (rb(rdram, kLives) < s.lives_before && !s.peer_left) {
            s.link.send(rs64::net::encode_life_lost(s.mission_id), true);
            fprintf(stderr, "[ghost] life lost (sent to host)\n");
        }
    }
    if (s.restore_lives >= 0) {
        wb(rdram, kLives, (uint8_t)s.restore_lives);
        s.restore_lives = -1;
    }
    // Spectating with nobody left to watch: the survivor quit, so this side's mission is over too.
    if (s.out && s.peer_left) {
        s.out = false;
        game_over(rdram, "the other player left while this one was out");
    }
    // Objectives: the host streams its state (on change for 5 frames, and every 15); the client takes the newest before its level script runs this frame.
    if (s.player == 1 && s.obj_have) {
        write_objectives(rdram, s.obj_pending);
        s.obj_have = false;
        if (s.obj_applied++ == 0) {
            fprintf(stderr, "[ghost] objectives: first snapshot applied\n");
        }
    } else if (s.player == 0 && !s.peer_left) {
        rs64::net::ObjSnapshot now;
        read_objectives(rdram, &now);
        const bool changed = !s.obj_sent_any || now.lives != s.obj_last_sent.lives || memcmp(now.bools, s.obj_last_sent.bools, sizeof(now.bools)) != 0 || memcmp(now.counts, s.obj_last_sent.counts, sizeof(now.counts)) != 0;
        if (changed) {
            s.obj_resend = 5;
        }
        if (s.obj_resend > 0 || s.frame % 15 == 0) {
            if (s.obj_resend > 0) {
                s.obj_resend--;
            }
            now.epoch = s.epoch;
            now.seq = ++s.obj_seq;
            s.link.send(rs64::net::encode_obj(now), false);
            s.obj_last_sent = now;
            s.obj_sent_any = true;
        }
        // After the snapshot: the level script reacts to the effect later this frame, so the next snapshot carries both (a client sent the effect alone runs its own reaction, e.g. level 3's random Nonnah location).
        apply_remote_triggers(rdram, ctx);
    }
    sync_dat(rdram, ctx);
    sync_upgrades(rdram);
    apply_remote_tows(rdram, ctx);
    // The shared pool dropped for the other player's death: show the HUD lives counter the way a local respawn does (HUD NPC slot 0x8010BFD0, action 0xC restarts its show timer HUD+0x228 and refreshes the digit).
    const int lives_now = rb(rdram, kLives);
    if (s.lives_seen >= 0 && lives_now < s.lives_seen && rw(rdram, 0x8010CA20u) != 1) {
        const uint32_t hud = rs64::mips::rh(rdram, 0x8010BFD0u);
        if (hud != 0xFFFFu) {
            rs64::mp::call(rdram, ctx, 0x8003E8DCu, {hud, 0xCu, 0u});
        }
        fprintf(stderr, "[ghost] shared lives now %d (shown)\n", lives_now);
    }
    s.lives_seen = lives_now;
    // ROGUESQ_GHOST_OBJ_TRACE=1: both sides log hashes of the objective state and of DAT health (clamped at 0, overkill differs per world) every 150 frames, to compare the two worlds.
    static const bool obj_trace = recomp::dbg::env_on("ROGUESQ_GHOST_OBJ_TRACE");
    if (obj_trace && s.frame % 150 == 0) {
        rs64::net::ObjSnapshot now;
        read_objectives(rdram, &now);
        uint64_t hh = 0xcbf29ce484222325ull;
        uint32_t alive = 0;
        const uint32_t n = dat_count(rdram);
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t item = dat_item(rdram, i);
            if (!dat_tracked(rdram, item)) {
                continue;
            }
            const int32_t hp = dat_health(rdram, item) > 0 ? dat_health(rdram, item) : 0;
            alive += hp > 0 ? 1u : 0u;
            hh = rs64::ls::fnv1a((const uint8_t*)&hp, 4, hh);
        }
        fprintf(stderr, "[ghost] obj frame %u bools=%016llX counts=%016llX dat=%016llX alive=%u\n", s.frame, (unsigned long long)rs64::ls::fnv1a(now.bools, sizeof(now.bools)), (unsigned long long)rs64::ls::fnv1a((const uint8_t*)now.counts, sizeof(now.counts)), (unsigned long long)hh, alive);
    }
    if (s.player == 1 && s.pending_result >= 0) {
        apply_pending_result(rdram);
    }
    // ROGUESQ_MP_TEST_END=<frame>:<result>: the host ends its mission on that frame with that result, the way the game's abort does, to test the client following it.
    static const std::pair<long, int> test_end = []() {
        long f = -1;
        int r = 0;
        const char* e = recomp::dbg::env_str("ROGUESQ_MP_TEST_END");
        if (!e || sscanf(e, "%ld:%d", &f, &r) != 2 || r < 0 || r > 3) {
            f = -1;
        }
        return std::make_pair(f, r);
    }();
    // ROGUESQ_MP_TEST_QUIT=<frame>: the host process dies on that mission frame (no goodbye), to test the client losing its host mid-mission.
    static const long test_quit = recomp::dbg::env_int("ROGUESQ_MP_TEST_QUIT", -1);
    if (s.player == 0 && test_quit >= 0 && (long)s.frame == test_quit) {
        fprintf(stderr, "[ghost] test quit at frame %u\n", s.frame);
        fflush(stderr);
        // ROGUESQ_MP_TEST_QUIT_CLEAN=1 takes the menu QUIT path (sends BYE) instead of dying silently.
        static const bool clean = recomp::dbg::env_on("ROGUESQ_MP_TEST_QUIT_CLEAN");
        if (clean) {
            rs64::mp::request_quit();
        } else {
            std::_Exit(3);
        }
    }
    // ROGUESQ_MP_TEST_LIVES=<n>: the host starts the shared pool at n, to reach the last-life rules quickly.
    static const long test_lives = recomp::dbg::env_int("ROGUESQ_MP_TEST_LIVES", -1);
    if (s.player == 0 && test_lives >= 0 && s.frame == 0) {
        wb(rdram, kLives, (uint8_t)test_lives);
        fprintf(stderr, "[ghost] test lives: shared pool set to %ld\n", test_lives);
    }
    // ROGUESQ_MP_TEST_UPGRADE=<frame>:<hex bits>: the host ORs power-up bits into its settings word on that frame, the way a pickup does.
    static const std::pair<long, uint32_t> test_upgrade = []() {
        long f = -1;
        unsigned b = 0;
        const char* e = recomp::dbg::env_str("ROGUESQ_MP_TEST_UPGRADE");
        return (e && sscanf(e, "%ld:%x", &f, &b) == 2) ? std::make_pair(f, (uint32_t)b & rs64::net::kUpgradeMask) : std::make_pair(-1L, 0u);
    }();
    if (s.player == 0 && test_upgrade.first >= 0 && (long)s.frame == test_upgrade.first) {
        ww(rdram, 0x80130B4Cu, rw(rdram, 0x80130B4Cu) | test_upgrade.second);
        fprintf(stderr, "[ghost] test upgrade: %05X at frame %u\n", test_upgrade.second, s.frame);
    }
    if (s.player == 0 && test_end.first >= 0 && (long)s.frame == test_end.first) {
        wb(rdram, 0x80130B14u, (uint8_t)test_end.second);
        ww(rdram, 0x8010C9E0u, rw(rdram, 0x8010C9E0u) | 1u);
        fprintf(stderr, "[ghost] test end: result %d at frame %u\n", test_end.second, s.frame);
    }
    if (!s.despawned && (!s.remote_in || s.peer_left || s.remote_down || s.out)) {
        rs64_imposter_despawn(rdram, ctx);
        s.despawned = true;
        fprintf(stderr, "[ghost] puppet removed\n");
    }
    if (!s.frames.empty()) {
        // A recording doesn't drive the pause menu (it would pick menu entries, even Quit); paused frames get an idle pad.
        const bool paused = rw(rdram, 0x8010CA20u) == 5;
        const FrameInput* f = (!paused && s.frame < s.frames.size()) ? &s.frames[s.frame] : nullptr;
        for (int i = 0; i < 6; ++i) {
            wb(rdram, 0x80130B88u + i, f ? f->pad[i] : (uint8_t)(i == 4 ? 1 : 0));
        }
    }
    const float dt = 1.0f / 30.0f;
    // Spectating: the craft takes the survivor's pose before the tick pass (player block 0x80137DC0: pos, fwd +0xC, up +0x18, vel +0x24), so the camera follows them; health stays full and the pad keeps START plus the survivor's fire.
    if (s.out && rw(rdram, 0x8010CA20u) != 5) {
        uint8_t pad[6];
        for (int i = 0; i < 6; ++i) {
            pad[i] = rb(rdram, 0x80130B88u + i);
        }
        // The survivor's fire button through player 1's control map (slot 0 at 0x8010BE18, fire entry +0x1A): their lasers leave the craft riding their pose.
        const bool live = !s.remote_down && now_s() - s.remote_rx_s < kPuppetCoastS;
        const uint16_t held = live ? (uint16_t)((s.remote_pad[0] << 8) | s.remote_pad[1]) : 0;
        rs64::ls::spectate_pad(pad, held, rs64::mips::rh(rdram, 0x8010BE18u + 0x1A));
        for (int i = 0; i < 6; ++i) {
            wb(rdram, 0x80130B88u + i, pad[i]);
        }
    }
    // Also through the respawn (mode 1, after the crash): the craft never shows at the checkpoint, and the respawn's camera reset lands on the survivor.
    if (s.out && s.puppet.has_target() && !s.remote_down) {
        const PuppetState p = s.puppet.step(now_s(), dt);
        const Vec3* parts[4] = {&p.pos, &p.fwd, &p.down, &p.vel};
        for (int k = 0; k < 4; ++k) {
            wf(rdram, 0x80137DC0u + k * 0xC + 0x0, parts[k]->x);
            wf(rdram, 0x80137DC0u + k * 0xC + 0x4, parts[k]->y);
            wf(rdram, 0x80137DC0u + k * 0xC + 0x8, parts[k]->z);
        }
        ww(rdram, 0x80137E7Cu, rw(rdram, 0x80137E80u));
    }
    // Pause-begin holds the camera: 0x80137CF4 (read only by the camera update and the pause functions) and 0x80138925. In a session the camera keeps following while the menu is open.
    if (rw(rdram, 0x8010CA20u) == 5) {
        wb(rdram, 0x80137CF4u, 0);
        wb(rdram, 0x80138925u, 0);
    }
    // ROGUESQ_MP_TEST_PAUSE=<frame>: press START on that frame (two frames held), to test that pausing keeps the world running.
    static const long test_pause = recomp::dbg::env_int("ROGUESQ_MP_TEST_PAUSE", -1);
    if (test_pause >= 0 && ((long)s.frame == test_pause || (long)s.frame == test_pause + 1)) {
        wb(rdram, 0x80130B88u, (uint8_t)(rb(rdram, 0x80130B88u) | 0x10u));
    }
    const uint32_t m = 0x8013800Cu;
    const Vec3 pos = {rf(rdram, 0x80137DC0u), rf(rdram, 0x80137DC4u), rf(rdram, 0x80137DC8u)};
    if ((rw(rdram, 0x8010CA20u) == 5 || (test_pause >= 0 && (long)s.frame + 150 >= test_pause)) && s.frame % 30 == 0) {
        // View 0's camera matrix translation (view array 0x80138D18, matrix at +0x34, translation at +0x24).
        fprintf(stderr, "[ghost] %s: frame %u pos %.1f,%.1f,%.1f camera %.1f,%.1f,%.1f\n", rw(rdram, 0x8010CA20u) == 5 ? "paused, world running" : "running", s.frame, pos.x, pos.y, pos.z, rf(rdram, 0x80138D70u), rf(rdram, 0x80138D74u), rf(rdram, 0x80138D78u));
    }
    // Until the client has moved itself to its start offset it sits exactly on the host's start, and a puppet spawned there overlaps the host's craft.
    const bool placed = s.player == 0 || s.start_offset_done || s.frame >= 90;
    if (!s.peer_left && placed) {
        rs64::net::StatePacket out;
        out.seq = ++s.seq;
        out.epoch = s.epoch;
        out.sent_ms = now_ms32();
        // Mission mode 1 = this player died and is respawning; an out player is down for good.
        out.flags = (rw(rdram, 0x8010CA20u) == 1 || s.out) ? rs64::net::kStateDown : 0;
        // The X-wing's S-foils: its handler extension (slot table 0x80130BB0 [player slot 0x80137DBA] -> ctx +4) holds foil t at +4, 0 open .. 1 closed.
        if (rb(rdram, 0x80130B41u) == 0) {
            const uint32_t slot = rs64::mips::rh(rdram, 0x80137DBAu);
            const uint32_t table = rw(rdram, 0x80130BB0u);
            const uint32_t npc = (slot < rs64::ls::kNpcSlots && table) ? rw(rdram, table + slot * 8u) : 0u;
            const uint32_t ext = (npc >= 0x80000000u && npc < 0x80800000u) ? rw(rdram, npc + 4u) : 0u;
            if (ext >= 0x80000000u && ext < 0x80800000u) {
                const float t = std::clamp(rf(rdram, ext + 4u), 0.0f, 1.0f);
                out.foil = (uint8_t)std::lround((1.0f - t) * 250.0f);
            }
        }
        out.pos[0] = pos.x;
        out.pos[1] = pos.y;
        out.pos[2] = pos.z;
        const uint32_t fwd_off[3] = {0x8, 0x14, 0x20};
        const uint32_t down_off[3] = {0x4, 0x10, 0x1C};
        for (int i = 0; i < 3; ++i) {
            out.fwd[i] = rf(rdram, m + fwd_off[i]);
            out.down[i] = rf(rdram, m + down_off[i]);
        }
        const Vec3 d = {pos.x - s.prev_pos.x, pos.y - s.prev_pos.y, pos.z - s.prev_pos.z};
        // A respawn, teleport or the client's start offset is a jump, not motion.
        if (s.have_prev && d.x * d.x + d.y * d.y + d.z * d.z < kPuppetSnap * kPuppetSnap) {
            out.vel[0] = d.x / dt;
            out.vel[1] = d.y / dt;
            out.vel[2] = d.z / dt;
        }
        // Paused, the pad drives the pause menu: the other side's puppet gets an idle pad so menu presses don't fire it.
        const bool paused_now = rw(rdram, 0x8010CA20u) == 5;
        for (int i = 0; i < 6; ++i) {
            out.pad[i] = paused_now ? (uint8_t)(i == 4 ? 1 : 0) : rb(rdram, 0x80130B88u + i);
        }
        s.link.send(rs64::net::encode_state(out), false);
    }
    s.prev_pos = pos;
    s.have_prev = true;
    // The pad goes idle when the peer stops sending (paused, on a results screen, stalled), so the puppet never keeps firing.
    FrameInput remote;
    const bool live = s.have_remote && !s.peer_left && s.remote_in && now_s() - s.remote_rx_s < kPuppetCoastS;
    memcpy(remote.pad, s.remote_pad, 6);
    uint8_t port1[6];
    pad2_bytes(live ? &remote : nullptr, port1);
    for (int i = 0; i < 6; ++i) {
        wb(rdram, 0x80130B8Eu + i, port1[i]);
    }
    if (s.trace) {
        const double ms = now_s() * 1000.0;
        if (s.puppet_shown && !s.despawned) {
            fprintf(s.trace, "%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n", ms, pos.x, pos.y, pos.z, s.puppet_pos.x, s.puppet_pos.y, s.puppet_pos.z);
        } else {
            fprintf(s.trace, "%.3f,%.4f,%.4f,%.4f,nan,nan,nan\n", ms, pos.x, pos.y, pos.z);
        }
        fflush(s.trace);
    }
    s.frame++;
}
