// Ghost co-op (ROGUESQ_MP): each instance flies its own player 1 in its own world; the other player is the imposter, posed from the state that instance streams.
// The host's mission result ends the client's mission; each side announces entering and leaving a mission so the other can remove its puppet.
#include "session/ghost_internal.h"
#include "core/mp_core.h"
#include "net/net_link.h"
#include "util/debug_logs.h"
#include "util/rdram_words.h"
#include "mp_host.h"
#include "rs64/camera_shots.h"
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

uint8_t rb(const uint8_t* rdram, uint32_t a) {
    return rdram[(a - 0x80000000u) ^ 3];
}

void wb(uint8_t* rdram, uint32_t a, uint8_t v) {
    rdram[(a - 0x80000000u) ^ 3] = v;
}

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ROGUESQ_MP must be "host" or "join"; anything else leaves co-op off.
rs64::net::Role role_from_env()
{

    const char* mp = recomp::dbg::env_str("ROGUESQ_MP");

    if (!mp)
    {
        return rs64::net::Role::None;
    }

    if (strcmp(mp, "host") == 0)
    {
        return rs64::net::Role::Host;
    }

    return strcmp(mp, "join") == 0 ? rs64::net::Role::Client : rs64::net::Role::None;
}

int& config_port();

rs64::net::LinkConfig link_config()
{

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
bool valid_state(const rs64::net::StatePacket& p)
{

    auto len = [](const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };

    for (int i = 0; i < 3; ++i) {
        const float vals[4] = {p.pos[i], p.fwd[i], p.down[i], p.vel[i]};

        for (float v : vals) {
            if (!std::isfinite(v) || std::fabs(v) > 1e5f)
            {
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
    // Wraps; a browser steady clock is past 2^32 ms, where a direct cast saturates.
    return (uint32_t)(uint64_t)(now_s() * 1000.0);
}

// roguesq_net.json next to the exe: { "last_address": "10.0.0.189", "last_host": "10.0.0.5", "port": 27064 }. The last JOIN address seeds the editor; either may end in ":port".
std::string net_config_path()
{
    // Two instances of one exe (the pair runner) each point ROGUESQ_NET_CONFIG at their own file.
    if (const char* p = recomp::dbg::env_str("ROGUESQ_NET_CONFIG"))
    {
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

    if (loaded)
    {
        return;
    }

    loaded = true;
    std::ifstream f(net_config_path());

    if (!f.is_open())
    {
        return;
    }

    try {
        nlohmann::json j;
        f >> j;
        g().editor.load(j.value("last_address", std::string{}));
        g().last_host = j.value("last_host", std::string{});
        g().relay_server = j.value("relay_server", std::string{});
        g().host_name.name = rs64::net::NameEditor::clean(j.value("host_name", std::string{}));
        const std::string pin = j.value("host_password", std::string{});
        g().host_pin_set = rs64::net::CodeEditor::valid(pin);
        g().host_pin.load(pin);
        const int port = j.value("port", 27064);
        config_port() = (port > 0 && port < 65536) ? port : 27064;
    } catch (...) {
    }
}

void save_net_config()
{

    nlohmann::json j;
    const Ghost& s = g();
    j["last_address"] = s.row_hosting ? s.join_entry : s.editor.entry();

    if (!s.last_host.empty()) {
        j["last_host"] = s.last_host;
    }

    if (!s.relay_server.empty()) {
        j["relay_server"] = s.relay_server;
    }

    j["host_name"] = s.host_name.name;
    j["host_password"] = s.host_pin_set ? s.host_pin.code() : std::string{};
    j["port"] = config_port();
    std::ofstream f(net_config_path());

    if (f.is_open()) {
        f << j.dump(2) << "\n";
    }
}

// Starts (or restarts) the link as host or client and the lobby handshake on top of it.
void lobby_start(const rs64::net::LinkConfig& cfg)
{

    Ghost& s = g();
    s.link.stop();
    s.remote_craft = -1;
    s.player = cfg.role == rs64::net::Role::Host ? 0 : 1;
    s.was_connected = false;
    s.seen_connects = s.link.connect_count();
    s.stop_at_ms = -1;
    s.was_ready = false;
    s.peer_left = false;
    s.partner_left_ms = -1;
    static const long test_build = recomp::dbg::env_int("ROGUESQ_MP_TEST_VERSION", -1);

    if (test_build >= 0) {
        s.lobby.set_local_build((uint32_t)test_build);
    }

    if (cfg.role == rs64::net::Role::Host)
    {
        s.lobby.set_password(s.host_pin_set ? s.host_pin.code() : std::string{});
        s.lobby.host();
    } else {
        static const char* test_pin = recomp::dbg::env_str("ROGUESQ_MP_TEST_JOIN_PIN");

        if (test_pin && rs64::net::CodeEditor::valid(test_pin)) {
            s.join_pin.load(test_pin);
            s.join_pin_set = true;
        }

        s.lobby.set_password(s.join_pin_set ? s.join_pin.code() : std::string{});
        s.lobby.join();
    }

    s.started = s.link.start(cfg);

    if (!s.started) {
        s.lobby.on_start_failed();
    }

    if (cfg.external)
    {
        return;
    }

    if (s.player == 0)
    {
        fprintf(stderr, "[lobby] hosting on port %u latency %d ms loss %d%%%s\n", cfg.port, cfg.latency_ms, cfg.loss_pct, s.started ? "" : " (network start failed)");
    } else {
        fprintf(stderr, "[lobby] joining %s:%u latency %d ms loss %d%%%s\n", cfg.addr.c_str(), cfg.port, cfg.latency_ms, cfg.loss_pct, s.started ? "" : " (network start failed)");
    }
}

// The lobby's handshake messages go out before its keys are installed: the client's PAKE3 is the last plaintext message.
void lobby_flush()
{

    Ghost& s = g();
    rs64::net::SessionKeys k;

    for (auto& b : s.lobby.take_sends()) {
        s.link.send(std::move(b), true);
    }

    if (s.lobby.take_keys(&k)) {
        s.link.set_cipher(k);
        rs64::net::wipe_keys(&k);

        fprintf(stderr, "[lobby] password checked, session sealed\n");
    }
}

// Link events into the lobby: HELLO on connect, the version verdict, a peer lost. A failed JOIN stops retrying.
void lobby_poll()
{

    Ghost& s = g();

    if (!s.started)
    {
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

    if (edges.connected)
    {
        s.link.clear_cipher();
        s.lobby.on_connected(now);
    }

    s.was_connected = c;

    // A join whose host or relay server cannot be reached fails now, with the transport's reason (COULD NOT CONNECT TO SERVER, NO GAME WITH THAT CODE).
    if (s.player == 1 && s.lobby.state() == rs64::net::LobbyState::Joining && s.link.lost())
    {
        const char* why = s.relay_session && s.relay_t ? s.relay_t->status(s.relay_t->user) : nullptr;
        s.lobby.on_join_failed(why ? why : "");
    }

    s.lobby.tick(now);
    lobby_flush();

    if (s.lobby.password_failed()) {
        s.pin_mode = true;
    }

    // A password kept from an earlier game, refused by an open one: the next JOIN goes without it.
    if (s.player == 1 && s.lobby.status() == "GAME HAS NO PASSWORD") {
        s.join_pin_set = false;
    }

    if (s.lobby.take_kick())
    {
        s.link.kick();

        fprintf(stderr, "[lobby] closed a connection that failed the handshake\n");
    }

    const bool ready = s.lobby.ready();

    if (ready && !s.was_ready)
    {
        if (s.player == 0) {
            s.link.send(rs64::net::encode_accept(), true);
        }

        // The pilot name (gGameSettings+7), for the label over this player's ship on the other screen.
        char name[3] = {0, 0, 0};

        if (s.rdram) {
            for (int i = 0; i < 3; ++i) {
                name[i] = (char)rb(s.rdram, 0x80130B47u + (uint32_t)i);
            }
        }

        s.link.send(rs64::net::encode_name(name), true);

        s.lobby_session = true;
        s.peer_left = false;

        fprintf(stderr, "[lobby] %s (version ok)\n", s.player == 0 ? "player connected" : "connected to the host");
    }

    const bool had_ready = s.was_ready;
    s.was_ready = ready;

    // The partner left (this side did not stop) while both were in the menus: mission and craft select say so and back out to the main menu.
    if (had_ready && !ready && s.started && s.from_menu && !s.in_session) {
        s.partner_left_ms = now;
        s.partner_left_text = s.player == 1 ? "HOST LEFT" : "PLAYER LEFT";

        fprintf(stderr, "[lobby] %s in the menus\n", s.partner_left_text.c_str());
    }

    if (s.lobby.state() == rs64::net::LobbyState::Failed && s.stop_at_ms < 0)
    {
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

        fprintf(stderr, "[ghost] host result 3 sent at %.2f s\n", now_s());
    }
}

const TestStep* test_step(const char* op, long frame)
{

    static const std::vector<TestStep> steps = [] {

        std::vector<TestStep> v;
        const char* p = recomp::dbg::env_str("ROGUESQ_MP_TEST");

        while (p && *p) {
            const char* at = strchr(p, '@');

            if (!at) {
                break;
            }

            TestStep t;
            char* end = nullptr;
            t.op.assign(p, at);
            t.frame = std::strtol(at + 1, &end, 10);

            if (*end == ':') {
                t.arg = std::strtol(end + 1, &end, 0);
            }

            v.push_back(t);
            p = *end == ',' ? end + 1 : nullptr;
        }

        return v;
    }();

    for (const TestStep& t : steps) {
        if (t.op == op && (frame < 0 || t.frame == frame)) return &t;
    }

    return nullptr;
}

// Mod camera shots (camera_shots.txt) start from trigger effects, which in session only the host's world applies: the host sends each one it plays.
void on_local_shot(const char* camera, const char* trigger, void*) {

    Ghost& s = g();

    if (s.in_session && !s.peer_left && s.player == 0)
    {
        s.link.send(rs64::net::encode_shot(mission_tag(), camera, trigger), true);
    }
}

const rs64_camera_shots* camera_shots()
{

    static const auto* svc = (const rs64_camera_shots*)rs64::mp::service(RS64_CAMERA_SHOTS_SERVICE);
    static bool listening = false;

    if (svc && !listening) {
        svc->set_listener(on_local_shot, nullptr);
        listening = true;
    }

    return svc;
}

// Peak backlogs a peer can leave waiting for the mission to settle: four full resyncs of the largest DAT list, and far more trigger edges than a level has.
constexpr size_t kMaxDatApply = 4 * 4096;
constexpr size_t kMaxTriggerIn = 1024;

void drain()
{

    Ghost& s = g();

    // The connect edge first: the peer's HELLO can arrive in the same batch as the connection, and the lobby ignores a HELLO before it knows it is connected.
    lobby_poll();

    for (auto& raw : s.link.receive())
    {
        // The previous message's handshake replies and keys first: the next message may already be sealed.
        lobby_flush();

        std::vector<uint8_t> m;
        const rs64::net::Cipher::Open opened = s.link.open(raw, &m);

        if (opened == rs64::net::Cipher::Open::Fail)
        {
            fprintf(stderr, "[lobby] a sealed message did not open; closing the session\n");

            s.link.clear_cipher();
            s.lobby.on_lost();

            if (s.player == 0) {
                s.link.kick();
            }

            continue;
        }

        if (opened != rs64::net::Cipher::Open::Ok)
        {
            continue;
        }

        const uint8_t type = rs64::net::message_type(m.data(), m.size());

        // A connection that has not passed the handshake (and the password) only gets to attempt it.
        if (!s.lobby.ready() && !rs64::net::handshake_message(type)) {
            continue;
        }

        switch ((rs64::net::Msg)type) {
        case rs64::net::Msg::State: {
            rs64::net::StatePacket p;

            if (!rs64::net::decode_state(m.data(), m.size(), &p) || !valid_state(p))
            {
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
            break;
        }
        case rs64::net::Msg::Presence: {
            uint8_t e = 0, in = 0;

            if (!rs64::net::decode_presence(m.data(), m.size(), &e, &in)) {
                continue;
            }

            if (in)
            {
                fprintf(stderr, "[ghost] peer in mission\n");
                s.peer_arrived = true;

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

            break;
        }
        case rs64::net::Msg::Result: {
            uint8_t e = 0, r = 0;

            // One result per mission: the host repeats it at its mission end, and applying it again mid-transition re-raises the end request.
            if (rs64::net::decode_result(m.data(), m.size(), &e, &r) && s.player == 1 && s.in_session && (s.remote_epoch < 0 || e == (uint8_t)s.remote_epoch))
            {
                if (s.host_result < 0) {
                    s.pending_result = r;
                    s.host_result = r;

                    fprintf(stderr, "[ghost] host result %u received at %.2f s\n", r, now_s());
                }
            }

            break;
        }
        case rs64::net::Msg::Obj: {
            rs64::net::ObjSnapshot o;

            if (s.player == 1 && s.in_session && rs64::net::decode_obj(m.data(), m.size(), &o) && o.epoch == s.mission_id && o.seq > s.obj_last_seq)
            {
                s.obj_last_seq = o.seq;
                s.obj_pending = o;
                s.obj_have = true;
            }

            break;
        }
        case rs64::net::Msg::Life: {
            uint8_t e = 0;

            if (s.player == 0 && s.in_session && s.rdram && rs64::net::decode_life_lost(m.data(), m.size(), &e) && e == s.epoch)
            {
                uint8_t& lives = s.rdram[(kLives - 0x80000000u) ^ 3];

                switch (rs64::ls::shared_life(lives, s.remote_out))
                {
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
                    s.pending_result = 3;

                    fprintf(stderr, "[ghost] game over: client down with no lives left\n");
                    send_game_over();
                    break;
                case rs64::ls::LifeOutcome::Ignore:
                    break;
                }
            }

            break;
        }
        case rs64::net::Msg::Out: {
            uint8_t e = 0;

            if (s.player == 1 && s.in_session && rs64::net::decode_out(m.data(), m.size(), &e) && e == s.mission_id && !s.out)
            {
                s.out = true;

                fprintf(stderr, "[ghost] out of shared lives: spectating\n");
            }

            break;
        }
        case rs64::net::Msg::Damage: {
            uint8_t e = 0;
            std::vector<std::pair<uint16_t, int32_t>> items;

            if (s.in_session && s.dat_apply.size() < kMaxDatApply && rs64::net::decode_damage(m.data(), m.size(), &e, &items) && e == mission_tag())
            {
                s.dat_apply.insert(s.dat_apply.end(), items.begin(), items.end());
            }

            break;
        }
        case rs64::net::Msg::Upgrades: {
            uint8_t e = 0;
            uint32_t bits = 0;

            if (s.in_session && rs64::net::decode_upgrades(m.data(), m.size(), &e, &bits) && e == mission_tag())
            {
                s.upgrades_in |= bits;
            }

            break;
        }
        case rs64::net::Msg::Pickup: {
            uint8_t e = 0;
            uint16_t i = 0;

            if (s.in_session && s.rdram && rs64::net::decode_pickup(m.data(), m.size(), &e, &i) && e == mission_tag() && i < dat_count(s.rdram))
            {
                const uint32_t item = dat_item(s.rdram, i);

                if (item && std::find(s.pickups_forced.begin(), s.pickups_forced.end(), item) == s.pickups_forced.end())
                {
                    s.pickups_forced.push_back(item);

                    fprintf(stderr, "[ghost] the other player collected power-up %u\n", i);
                }
            }

            break;
        }
        case rs64::net::Msg::Trigger: {
            uint8_t e = 0;
            uint16_t idx = 0;
            bool enter = false;

            if (s.player == 0 && s.in_session && s.trigger_in.size() < kMaxTriggerIn && rs64::net::decode_trigger(m.data(), m.size(), &e, &idx, &enter) && e == mission_tag())
            {
                s.trigger_in.emplace_back(idx, enter);
            }

            break;
        }
        case rs64::net::Msg::Shot: {
            uint8_t e = 0;
            std::string cam, trig;

            if (s.player == 1 && s.in_session && rs64::net::decode_shot(m.data(), m.size(), &e, &cam, &trig) && e == mission_tag() && camera_shots())
            {
                camera_shots()->play(cam.c_str(), trig.c_str());
            }

            break;
        }
        case rs64::net::Msg::TowTrip: {
            uint8_t e = 0;
            uint16_t i = 0;

            if (s.in_session && s.rdram && rs64::net::decode_tow_trip(m.data(), m.size(), &e, &i) && e == mission_tag() && i < dat_count(s.rdram) && std::find(s.tow_in.begin(), s.tow_in.end(), i) == s.tow_in.end())
            {
                s.tow_in.push_back(i);
            }

            break;
        }
        case rs64::net::Msg::Mission: {
            uint8_t id = 0;

            if (s.player == 1 && rs64::net::decode_mission(m.data(), m.size(), &id))
            {
                s.latest_mission = id;
            }

            break;
        }
        case rs64::net::Msg::Hello: {
            rs64::net::Hello h;

            if (rs64::net::decode_hello(m.data(), m.size(), &h))
            {
                s.lobby.on_hello(h);

                if (!s.lobby.ready() && s.lobby.state() == rs64::net::LobbyState::Failed)
                {
                    fprintf(stderr, "[lobby] peer build %08X protocol %u, this build %08X protocol %u\n", h.build, h.protocol, s.lobby.local_build(), rs64::net::kProtocol);
                }
            }

            break;
        }
        case rs64::net::Msg::Skip: {
            uint16_t scene = 0;

            // The host dictates skips: only a client takes one.
            if (s.player == 1 && rs64::net::decode_skip(m.data(), m.size(), &scene))
            {
                s.skip.on_remote(scene);

                fprintf(stderr, "[ghost] the host skipped cutscene %04X (here %04X) at %.2f s\n", scene, s.skip_tag, now_s());
            }

            break;
        }
        case rs64::net::Msg::TowSeg: {
            rs64::net::TowSeg t;

            if (s.in_session && s.rdram && rs64::net::decode_tow_seg(m.data(), m.size(), &t) && t.epoch == mission_tag())
            {
                apply_tow_seg(s.rdram, t);
            }

            break;
        }
        case rs64::net::Msg::Continue: {
            if (rs64::net::decode_continue(m.data(), m.size())) {
                s.debrief.on_remote(0);

                fprintf(stderr, "[lobby] partner pressed continue\n");
            }

            break;
        }
        case rs64::net::Msg::BriefingDone: {
            if (s.player == 0 && rs64::net::decode_briefing_done(m.data(), m.size()))
            {
                s.brief_done_ms = now_ms32();
            }

            break;
        }
        case rs64::net::Msg::Browse: {
            uint8_t level = 0, state = 0;

            if (s.player == 1 && rs64::net::decode_browse(m.data(), m.size(), &level, &state))
            {
                s.host_cursor = level;
                s.host_state = state;
            }

            break;
        }
        case rs64::net::Msg::Ready: {
            uint8_t craft = 0;

            if (rs64::net::decode_ready(m.data(), m.size(), &craft))
            {
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

            break;
        }
        case rs64::net::Msg::Pick: {
            uint8_t level = 0;

            if (s.player == 1 && rs64::net::decode_pick(m.data(), m.size(), &level))
            {
                s.host_pick = level;

                fprintf(stderr, "[lobby] host picked level %u\n", level);
            }

            break;
        }
        case rs64::net::Msg::Launch:
            rs64::net::decode_launch(m.data(), m.size(), &s.launch);
            break;
        case rs64::net::Msg::Auth:
            if (s.player == 0 && rs64::net::decode_auth(m.data(), m.size()) && !s.lobby.on_auth())
            {
                fprintf(stderr, "[lobby] refused a player: no password\n");
            }

            break;
        case rs64::net::Msg::Pake1:
        case rs64::net::Msg::Pake2:
        case rs64::net::Msg::Pake3: {
            uint8_t point[rs64::net::kPakePoint];
            uint8_t tag[rs64::net::kPakeTag];
            const rs64::net::Msg t = (rs64::net::Msg)type;

            if (!rs64::net::decode_pake(m.data(), m.size(), t, point, tag))
            {
                break;
            }

            if (t == rs64::net::Msg::Pake1) {
                s.lobby.on_pake1(point);
            } else if (t == rs64::net::Msg::Pake2) {
                s.lobby.on_pake2(point, tag);
            } else {
                s.lobby.on_pake3(tag);
            }

            if (s.lobby.password_failed())
            {
                fprintf(stderr, "[lobby] the host has another password\n");
            }

            break;
        }
        case rs64::net::Msg::Name: {
            char name[3];

            if (rs64::net::decode_name(m.data(), m.size(), name))
            {
                s.remote_name = rs64::net::pilot_name(name);
            }

            break;
        }
        case rs64::net::Msg::Accept:
            if (s.player == 1 && rs64::net::decode_accept(m.data(), m.size())) {
                s.lobby.on_accept();
            }

            break;
        case rs64::net::Msg::Bye: {
            rs64::net::ByeReason why = rs64::net::ByeReason::Finished;
            uint32_t frame = 0;

            if (s.player == 1 && rs64::net::decode_bye(m.data(), m.size(), &why, &frame) && why == rs64::net::ByeReason::WrongPassword)
            {
                s.lobby.on_refused(s.join_pin_set);
                s.pin_mode = true;

                fprintf(stderr, "[lobby] the host refused this player (%s)\n", s.join_pin_set ? "password" : "no password");
                break;
            }

            if (!s.peer_left) {
                s.peer_left = true;

                fprintf(stderr, "[ghost] peer left\n");
            }

            break;
        }
        default:
            break;
        }
    }

    if (s.link.lost() && !s.peer_left) {
        s.peer_left = true;

        fprintf(stderr, "[ghost] peer left (connection lost)\n");
    }

    lobby_poll();
}

template <typename Pred>
bool wait_for(uint8_t* rdram, recomp_context* ctx, uint32_t timeout_ms, Pred pred)
{

    const auto t0 = std::chrono::steady_clock::now();

    while (true)
    {
        drain();

        if (pred())
        {
            return true;
        }

        if (g().link.lost() || g().peer_left || std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(timeout_ms))
        {
            return false;
        }

        rs64::mp::yield(rdram, ctx);
    }
}

void load_pad_recording(const char* path, std::vector<FrameInput>* out)
{

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

// Applies the host's result on the client (and the host's own last-life game over): success/fail by raising the game's own end request (refused during a respawn or pause, so retried each frame), abort and out-of-lives by the direct write the game's abort uses.
void apply_pending_result(uint8_t* rdram) {

    Ghost& s = g();
    const int r = s.pending_result;
    const uint32_t req = rw(rdram, 0x8010C9E0u);
    bool done = false;

    if (r == 1 || r == 2)
    {
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
        fprintf(stderr, "[ghost] applied host result %d at %.2f s\n", r, now_s());
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
extern "C" uint32_t rs64_ghost_mode(void)
{

    static const uint32_t on = recomp::dbg::env_str("ROGUESQ_MP") ? 1u : 0u;
    return (on || rs64_lobby_menu_session()) ? 1u : 0u;
}

extern "C" uint32_t rs64_ghost_active(void) {
    return g().in_session ? 1u : 0u;
}

// Quit: tells the other player at once instead of leaving them to the connection timeout. Runs on a game thread (poll_input) but _Exit follows, so the 200 ms wait is acceptable only on this exit path.
extern "C" void rs64_ghost_quit(void) {

    Ghost& s = g();

    if (s.started && s.link.connected())
    {
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

extern "C" uint32_t rs64_ghost_is_client(void)
{

    static const uint32_t env_client = role_from_env() == rs64::net::Role::Client ? 1u : 0u;
    return g().from_menu ? (g().player == 1 ? 1u : 0u) : env_client;
}

// The client's own start offset, applied once per mission by rs64_imposter_frame.
extern "C" uint32_t rs64_ghost_take_start_offset(void) {

    Ghost& s = g();

    if (!s.in_session || s.start_offset_done)
    {
        return 0u;
    }

    s.start_offset_done = true;
    return 1u;
}

// Whether the other player's puppet should exist now (in this mission, not down, state received), and where to spawn it.
extern "C" uint32_t rs64_ghost_want_puppet(float out_pos[3]) {
    const Ghost& s = g();

    if (!s.in_session || s.peer_left || !s.remote_in || s.remote_down || !s.have_remote || s.out)
    {
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
extern "C" uint32_t rs64_ghost_unfreeze(uint8_t* rdram)
{
    return (g().in_session && !g().peer_left && rw(rdram, 0x8010CA20u) == 5) ? 1u : 0u;
}

extern "C" uint32_t rs64_ghost_block_result(void) {

    const Ghost& s = g();
    return (s.in_session && s.player == 1 && !s.peer_left) ? 1u : 0u;
}

// requestMissionTransitionMode(a0): arg 3 = success end mode, 2 = failure. A client in session enters them only for the host's result; the host announces its result as soon as it enters one (mode 0 = the transition will be accepted).
extern "C" uint32_t rs64_ghost_block_transition(uint8_t* rdram, uint32_t arg)
{

    Ghost& s = g();

    if (!s.in_session || s.peer_left)
    {
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
            switch (rs64::ls::shared_life(lives, false))
            {
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

    if (arg == 1)
    {
        return 0u;
    }

    if (arg != 2 && arg != 3)
    {
        return 0u;
    }

    if (s.player == 0) {
        if (!s.result_sent && rw(rdram, 0x8010CA20u) == 0) {
            const uint8_t result = arg == 3 ? 1 : 2;
            s.link.send(rs64::net::encode_result(s.epoch, result), true);
            s.result_sent = true;

            fprintf(stderr, "[ghost] host result %u sent at %.2f s\n", result, now_s());
        }

        return 0u;
    }

    return (int)arg == s.allow_transition ? 0u : 1u;
}

// requestSpeechResponseMode1/2 land result 1/2 when `ready`: on a client in session only the host's result may land, whatever raised the request in its own world.
extern "C" uint32_t rs64_ghost_gate_response(uint32_t result, uint32_t ready)
{

    Ghost& s = g();

    if (!s.in_session || s.peer_left)
    {
        return ready;
    }

    // Each side lands after its own end cue (a voiceline), without waiting for the other.
    if (s.player == 0)
    {
        return ready;
    }

    if (s.host_result != (int)result)
    {
        return 0u;
    }

    if (ready) {
        fprintf(stderr, "[ghost] result %u landed at %.2f s\n", result, now_s());
    }

    return ready;
}

// initMission: connect (first mission only), exchange LAUNCH so both are on the same level and craft, then announce this mission.
extern "C" void rs64_ghost_mission_init(uint8_t* rdram, recomp_context* ctx)
{

    Ghost& s = g();
    s.rdram = rdram;
    s.in_session = false;
    s.pending_result = -1;
    s.host_result = -1;
    s.allow_transition = -1;

    // The menus' round state ends here on every path (a mission played alone included): a late PICK or READY belongs to the round that just ended.
    s.craft = rs64::ls::CraftBarrier{};
    s.debrief = rs64::ls::CraftBarrier{};
    s.host_pick = -1;
    s.host_cursor = -1;
    s.host_state = 0;
    s.in_hangar = false;
    s.follow_out = false;
    s.lives_seen = -1;
    s.skip_tag = (uint16_t)((s.skip_tag & 0xFF00u) + 0x100u);
    s.crawl_over = false;

    if (!rs64_ghost_mode() || demo_active(rdram))
    {
        return;
    }

    drain();

    if (!s.lobby.ready() && !s.lobby_session)
    {
        // ROGUESQ_MP without the menus: the same handshake, started here. The first mission waits for the other player to arrive; later missions only proceed together with a live peer.
        const rs64::net::LinkConfig cfg = link_config();

        if (cfg.role == rs64::net::Role::None)
        {
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

    if (s.peer_left || !s.lobby.ready())
    {
        fprintf(stderr, "[ghost] %s; playing alone\n", s.lobby.status() == "VERSION MISMATCH" ? "version mismatch" : "no peer");
        return;
    }

    if (s.from_menu && s.missions == 0) {
        fprintf(stderr, "[ghost] using the lobby link\n");
    }

    const SessionHeader running = read_session(rdram, 1);

    if (s.player == 0)
    {
        // The mission id goes first on the same reliable channel, so the client has it when LAUNCH arrives.
        s.mission_id = (uint8_t)(s.epoch + 1);
        s.link.send(rs64::net::encode_mission(s.mission_id), true);
        s.peer_arrived = false;
        s.link.send(rs64::net::encode_launch(format_header(running)), true);

        // Start together: a client that arrives late gets the host's objective state copied over steps its level script never ran, and faults on them (trigger 21 on level 0). The wait is capped; a slower client still catches up as before.
        constexpr uint32_t kStartWaitMs = 10000;
        const double wait_from = now_s();

        if (!wait_for(rdram, ctx, kStartWaitMs, [&]() { return s.peer_arrived || s.peer_left; }))
        {
            fprintf(stderr, "[ghost] the other player is still loading; starting without waiting\n");
        }

        fprintf(stderr, "[ghost] waited %.2f s at mission start for the other player\n", now_s() - wait_from);
    } else {
        SessionHeader h;

        if (!wait_for(rdram, ctx, 60000, [&]() { return !s.launch.empty(); }) || !parse_header(s.launch, &h))
        {
            fprintf(stderr, "[ghost] no LAUNCH from the host; playing alone\n");
            return;
        }

        s.launch.clear();

        // A different mission means this mission alone, not the end of the session; each player flies their own craft.
        if (h.level != running.level)
        {
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
    s.play_clock.reset();
    static const long interp_ms = recomp::dbg::env_int("ROGUESQ_GHOST_INTERP_MS", 50);
    s.puppet.set_interp(interp_ms / 1000.0);
    s.puppet_shown = false;

    // Nothing spawned yet: rs64_ghost_puppet_spawned clears this when the puppet appears.
    s.despawned = true;
    s.puppet_dying = -1;
    s.spectate_hit = false;
    s.tow_sent_phase = 0;
    s.tow_sent_head = -1;
    s.tow_remote = rs64::net::TowSeg{};
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
    s.obj_ledger.reset();
    s.obj_settled = false;
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

            if (s.trace)
            {
                fprintf(s.trace, "ms,local_x,local_y,local_z,puppet_x,puppet_y,puppet_z\n");
            }
        }
    }

    s.link.send(rs64::net::encode_presence(s.epoch, 1), true);

    fprintf(stderr, "[ghost] %s in session: level %u craft %u epoch %u, %zu recorded input frames\n", s.player == 0 ? "host" : "client", running.level, running.craft, s.epoch, s.frames.size());
}

// endMissionCleanup: announce leaving the mission; the host also sends its result.
extern "C" void rs64_ghost_mission_end(uint8_t* rdram)
{

    Ghost& s = g();

    if (!s.in_session)
    {
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
extern "C" void rs64_ghost_frame(uint8_t* rdram, recomp_context* ctx)
{

    Ghost& s = g();

    if (!s.started)
    {
        return;
    }

    camera_shots();
    drain();

    if (!s.in_session || demo_active(rdram))
    {
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

    // The host is gone: the client's writes it never showed count now, once (a kill just before the dropout would otherwise be lost and could stall an objective).
    if (s.player == 1 && s.peer_left && !s.obj_settled)
    {
        s.obj_settled = true;

        const rs64::ls::ObjectiveLedger::Owed owed = s.obj_ledger.settle();
        rs64::net::ObjSnapshot now;

        read_objectives(rdram, &now);

        for (const auto& [i, add] : owed.counts) now.counts[i] += add;
        for (const auto& [i, v] : owed.bools) now.bools[i] = v;

        write_objectives(rdram, now);

        fprintf(stderr, "[ghost] objectives: host gone, %zu owed counts and %zu owed booleans applied\n", owed.counts.size(), owed.bools.size());
    }

    // Objectives: the host streams its state (on change for 5 frames, and every 15); the client takes the newest before its level script runs this frame.
    if (s.player == 1 && s.obj_have)
    {
        write_objectives(rdram, s.obj_pending);
        s.obj_ledger.snapshot(s.obj_pending.counts, s.obj_pending.bools, now_s());
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
    sync_tow_cable(rdram);

    // The shared pool dropped for the other player's death: show the HUD lives counter the way a local respawn does (HUD NPC slot 0x8010BFD0, action 0xC restarts its show timer HUD+0x228 and refreshes the digit).
    const int lives_now = rb(rdram, kLives);

    if (s.lives_seen >= 0 && lives_now < s.lives_seen && rw(rdram, 0x8010CA20u) != 1)
    {
        const uint32_t hud = rs64::mips::rh(rdram, 0x8010BFD0u);

        if (hud != 0xFFFFu) {
            rs64::mp::call(rdram, ctx, 0x8003E8DCu, {hud, 0xCu, 0u});
        }

        fprintf(stderr, "[ghost] shared lives now %d (shown)\n", lives_now);
    }

    s.lives_seen = lives_now;

    // ROGUESQ_GHOST_OBJ_TRACE=1: both sides log hashes of the objective state and of DAT health (clamped at 0, overkill differs per world) every 150 frames, to compare the two worlds.
    static const bool obj_trace = recomp::dbg::env_on("ROGUESQ_GHOST_OBJ_TRACE");

    if (obj_trace && s.frame % 150 == 0)
    {
        rs64::net::ObjSnapshot now;
        read_objectives(rdram, &now);
        uint64_t hh = 0xcbf29ce484222325ull;
        uint32_t alive = 0;
        const uint32_t n = dat_count(rdram);

        for (uint32_t i = 0; i < n; ++i)
        {
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

    if (s.pending_result >= 0) {
        apply_pending_result(rdram);
    }

    const long frame = (long)s.frame;

    // Test quit: arg 1 takes the menu QUIT path (sends BYE), otherwise the process dies with no goodbye.
    if (const TestStep* t = test_step("quit", frame)) {
        fprintf(stderr, "[ghost] test quit at frame %u\n", s.frame);
        fflush(stderr);

        if (t->arg == 1) {
            rs64::mp::request_quit();
        } else {
            std::_Exit(3);
        }
    }

    // ROGUESQ_MP_TEST_LIVES=<n>: the host starts the shared pool at n, to reach the last-life rules quickly.
    static const long test_lives = recomp::dbg::env_int("ROGUESQ_MP_TEST_LIVES", -1);

    if (s.player == 0 && test_lives >= 0 && s.frame == 0)
    {
        wb(rdram, kLives, (uint8_t)test_lives);

        fprintf(stderr, "[ghost] test lives: shared pool set to %ld\n", test_lives);
    }

    if (test_step("die", frame)) {
        kill_player(rdram, ctx);

        fprintf(stderr, "[ghost] test die at frame %u\n", s.frame);
    }

    // Test upgrade: OR power-up bits into the settings word, the way a pickup does.
    if (const TestStep* t = test_step("upgrade", frame)) {
        const uint32_t bits = (uint32_t)t->arg & rs64::net::kUpgradeMask;
        ww(rdram, 0x80130B4Cu, rw(rdram, 0x80130B4Cu) | bits);

        fprintf(stderr, "[ghost] test upgrade: %05X at frame %u\n", bits, s.frame);
    }

    // Test end: this result, the way the game's abort ends a mission.
    if (const TestStep* t = test_step("end", frame)) {
        wb(rdram, 0x80130B14u, (uint8_t)t->arg);
        ww(rdram, 0x8010C9E0u, rw(rdram, 0x8010C9E0u) | 1u);

        fprintf(stderr, "[ghost] test end: result %ld at frame %u\n", t->arg, s.frame);
    }

    // An end the game writes directly (request bit 1: an abort, a scripted end) skips requestMissionTransitionMode, so the host tells the client the frame it is raised.
    if (s.player == 0 && !s.result_sent && !s.peer_left && (rw(rdram, 0x8010C9E0u) & 1u))
    {
        const uint8_t result = rb(rdram, 0x80130B14u);

        s.link.send(rs64::net::encode_result(s.epoch, result), true);
        s.result_sent = true;

        fprintf(stderr, "[ghost] host result %u sent at %.2f s\n", result, now_s());
    }

    // The other player went down: their puppet takes a lethal hit so the game plays its death, and is removed kPuppetDeathFrames later (or at once when the hit could not land, or they are back up).
    constexpr int kPuppetDeathFrames = 60;
    const bool gone = !s.remote_in || s.peer_left || s.out;

    if (!s.despawned && !gone && s.remote_down && s.puppet_dying < 0)
    {
        s.puppet_dying = rs64_imposter_kill(rdram, ctx) ? kPuppetDeathFrames : 0;

        if (s.puppet_dying > 0) {
            fprintf(stderr, "[ghost] puppet shot down\n");
        }
    } else if (s.puppet_dying > 0) {
        --s.puppet_dying;
    }

    const bool death_over = s.puppet_dying == 0 || (s.puppet_dying > 0 && !s.remote_down);

    if (!s.despawned && (gone || death_over)) {
        rs64_imposter_despawn(rdram, ctx);
        s.despawned = true;
        s.puppet_dying = -1;

        fprintf(stderr, "[ghost] puppet removed\n");
    }

    // Spectating: the craft rides the survivor's pose, so their death is shown as this craft's (an out player's death costs no life, rs64_ghost_block_transition).
    if (s.out && s.remote_down && !s.spectate_hit && rw(rdram, 0x8010CA20u) == 0)
    {
        kill_player(rdram, ctx);
        s.spectate_hit = true;

        fprintf(stderr, "[ghost] spectated player down: shown here\n");
    } else if (!s.remote_down) {
        s.spectate_hit = false;
    }

    if (!s.frames.empty())
    {
        // A recording doesn't drive the pause menu (it would pick menu entries, even Quit); paused frames get an idle pad.
        const bool paused = rw(rdram, 0x8010CA20u) == 5;
        const FrameInput* f = (!paused && s.frame < s.frames.size()) ? &s.frames[s.frame] : nullptr;

        for (int i = 0; i < 6; ++i)
        {
            wb(rdram, 0x80130B88u + i, f ? f->pad[i] : (uint8_t)(i == 4 ? 1 : 0));
        }
    }

    // A mission frame's game time: 1/60 when this side runs 60 logic frames a second, so the play clock and sent velocity match the other side's at any rate.
    const float dt = rs64::mp::frame_seconds();

    // Spectating: the craft takes the survivor's pose before the tick pass (player block 0x80137DC0: pos, fwd +0xC, up +0x18, vel +0x24), so the camera follows them; health stays full and the pad keeps START plus the survivor's fire.
    if (s.out && rw(rdram, 0x8010CA20u) != 5)
    {
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
    if (s.out && s.puppet.has_target() && !s.remote_down)
    {
        const PuppetState p = s.puppet.step(s.play_clock.advance(now_s(), dt), dt);
        const Vec3* parts[4] = {&p.pos, &p.fwd, &p.down, &p.vel};

        for (int k = 0; k < 4; ++k) {
            wf(rdram, 0x80137DC0u + k * 0xC + 0x0, parts[k]->x);
            wf(rdram, 0x80137DC0u + k * 0xC + 0x4, parts[k]->y);
            wf(rdram, 0x80137DC0u + k * 0xC + 0x8, parts[k]->z);
        }

        // The craft's rotation lives in its orientation matrix (0x8013800C, 3x3 row-major, columns right, down, fwd), which the flight model integrates and the sender reads: the block alone leaves the dead craft's rotation.
        const Vec3 right = {p.down.y * p.fwd.z - p.down.z * p.fwd.y, p.down.z * p.fwd.x - p.down.x * p.fwd.z, p.down.x * p.fwd.y - p.down.y * p.fwd.x};
        const Vec3* cols[3] = {&right, &p.down, &p.fwd};

        for (int c = 0; c < 3; ++c) {
            wf(rdram, 0x8013800Cu + c * 4 + 0x00, cols[c]->x);
            wf(rdram, 0x8013800Cu + c * 4 + 0x0C, cols[c]->y);
            wf(rdram, 0x8013800Cu + c * 4 + 0x18, cols[c]->z);
        }

        ww(rdram, kPlayerHealth, rw(rdram, kPlayerHealth + 4u));
    }

    // Pause-begin holds the camera: 0x80137CF4 (read only by the camera update and the pause functions) and 0x80138925. In a session the camera keeps following while the menu is open.
    if (rw(rdram, 0x8010CA20u) == 5) {
        wb(rdram, 0x80137CF4u, 0);
        wb(rdram, 0x80138925u, 0);
    }

    // Test pause: START held for two frames, to test that pausing keeps the world running.
    const TestStep* pause = test_step("pause", -1);

    if (pause && ((long)s.frame == pause->frame || (long)s.frame == pause->frame + 1))
    {
        wb(rdram, 0x80130B88u, (uint8_t)(rb(rdram, 0x80130B88u) | 0x10u));
    }

    const uint32_t m = 0x8013800Cu;
    const Vec3 pos = {rf(rdram, 0x80137DC0u), rf(rdram, 0x80137DC4u), rf(rdram, 0x80137DC8u)};

    if ((rw(rdram, 0x8010CA20u) == 5 || (pause && (long)s.frame + 150 >= pause->frame)) && s.frame % 30 == 0)
    {
        // View 0's camera matrix translation (view array 0x80138D18, matrix at +0x34, translation at +0x24).
        fprintf(stderr, "[ghost] %s: frame %u pos %.1f,%.1f,%.1f camera %.1f,%.1f,%.1f\n", rw(rdram, 0x8010CA20u) == 5 ? "paused, world running" : "running", s.frame, pos.x, pos.y, pos.z, rf(rdram, 0x80138D70u), rf(rdram, 0x80138D74u), rf(rdram, 0x80138D78u));
    }

    // Until the client has moved itself to its start offset it sits exactly on the host's start, and a puppet spawned there overlaps the host's craft.
    const bool placed = s.player == 0 || s.start_offset_done || s.frame >= 90;

    if (!s.peer_left && placed)
    {
        rs64::net::StatePacket out;
        out.seq = ++s.seq;
        out.epoch = s.epoch;
        out.sent_ms = now_ms32();

        // Mission mode 1 = this player died and is respawning; an out player is down for good.
        out.flags = (rw(rdram, 0x8010CA20u) == 1 || s.out) ? rs64::net::kStateDown : 0;

        // The X-wing's S-foils: its handler extension (slot table 0x80130BB0 [player slot 0x80137DBA] -> ctx +4) holds foil t at +4, 0 open .. 1 closed.
        if (rb(rdram, 0x80130B41u) == 0)
        {
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
        if (s.have_prev && d.x * d.x + d.y * d.y + d.z * d.z < kPuppetSnap * kPuppetSnap)
        {
            out.vel[0] = d.x / dt;
            out.vel[1] = d.y / dt;
            out.vel[2] = d.z / dt;
        }

        // Paused, the pad drives the pause menu: the other side's puppet gets an idle pad so menu presses don't fire it.
        const bool paused_now = rw(rdram, 0x8010CA20u) == 5;

        for (int i = 0; i < 6; ++i)
        {
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

        if (s.puppet_shown && !s.despawned)
        {
            fprintf(s.trace, "%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n", ms, pos.x, pos.y, pos.z, s.puppet_pos.x, s.puppet_pos.y, s.puppet_pos.z);
        } else {
            fprintf(s.trace, "%.3f,%.4f,%.4f,%.4f,nan,nan,nan\n", ms, pos.x, pos.y, pos.z);
        }

        fflush(s.trace);
    }

    s.frame++;
}
