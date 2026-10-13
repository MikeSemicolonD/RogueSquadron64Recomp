// Ghost co-op front-end lobby: host/join, LAN discovery, address editing, and the mission/craft select hooks.
#include "ghost_internal.h"
#include "debug_logs.h"
#include "mp_host.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using namespace rs64::ghost;
using namespace rs64::ls;
using rs64::mips::rw;
using rs64::mips::ww;
using rs64::mips::rf;

// The menus connected two players: ghost mode is on (rs64_ghost_mode) without ROGUESQ_MP.
extern "C" uint32_t rs64_lobby_menu_session(void) {
    return g().from_menu ? 1u : 0u;
}

// Base code reads the co-op modes as flags: the env switches, or a session the menus started.
static void publish_modes() {
    rs64::mp::set_flag("ghost_mode", rs64_ghost_mode() ? 1 : 0);
    rs64::mp::set_flag("coop_imposter", rs64_coop_imposter() ? 1 : 0);
}

static void set_from_menu(bool on) {
    g().from_menu = on;
    publish_modes();
}

namespace rs64::ghost {

const rs64_mp_transport* relay() {
    static const rs64_mp_transport* s_relay = [] {
        const char* force = recomp::dbg::env_str("ROGUESQ_MP_TRANSPORT");
        if (force && strcmp(force, "enet") == 0) {
            return (const rs64_mp_transport*)nullptr;
        }
        const auto* t = (const rs64_mp_transport*)rs64::mp::service(RS64_MP_TRANSPORT_SERVICE);
        if (t && (t->version < 1 || t->size < sizeof(rs64_mp_transport))) {
            fprintf(stderr, "[lobby] ignoring the %s transport: built for another interface version\n", t->name ? t->name : "?");
            t = nullptr;
        }
        if (t) {
            fprintf(stderr, "[lobby] %s transport present\n", t->name);
        }
        return t;
    }();
    return s_relay;
}

const rs64_mp_transport* relay_ready() {
    const rs64_mp_transport* t = relay();
    return t && t->available(t->user) ? t : nullptr;
}

void relay_shutdown() {
    if (const rs64_mp_transport* t = relay()) {
        t->shutdown(t->user);
    }
    g().relay_session = false;
}

}

// Host or join over the relay transport: no LAN discovery, no router mapping, the code row instead of the address.
static void relay_start(const rs64_mp_transport* t, rs64::net::Role role, const std::string& peer) {
    Ghost& s = g();
    rs64::net::LinkConfig cfg = link_config();
    cfg.role = role;
    cfg.external = t;
    cfg.peer = peer;
    cfg.build = s.lobby.local_build();
    s.code.cancel_typing();
    s.relay_session = true;
    set_from_menu(true);
    lobby_start(cfg);
    fprintf(stderr, "[lobby] %s over %s%s%s%s\n", role == rs64::net::Role::Host ? "hosting" : "joining", t->name, peer.empty() ? "" : " ", peer.c_str(), s.started ? "" : " (start failed)");
}

// HOST GAME on the address row: one of this machine's addresses there is the only one listened on (a typed port too); else last_host if it is still local, else every address with the auto pick shown in the row. 0.0.0.0 asks for every address and keeps its port.
static void host_on_row(rs64::net::LinkConfig* cfg) {
    Ghost& s = g();
    if (s.editor.typing) {
        s.editor.end_typing();
    }
    s.editor.editing = false;
    if (!s.row_hosting) {
        s.join_entry = s.editor.entry();
    }
    s.row_hosting = true;
    const std::vector<uint32_t> local = rs64::net::local_ipv4s();
    const std::optional<uint32_t> fallback = rs64::net::lan_ipv4();
    const std::optional<uint32_t> row = s.editor.ipv4();
    rs64::net::HostAddress h = rs64::net::choose_host_address(row, local, fallback);
    uint16_t port = (h.bind || row == 0u) ? s.editor.port : 0;
    if (!h.bind && row != 0u) {
        rs64::net::AddressEditor last;
        last.load(s.last_host);
        const rs64::net::HostAddress again = rs64::net::choose_host_address(last.ipv4(), local, fallback);
        if (again.bind) {
            h = again;
            port = last.port;
            s.editor.load(s.last_host);
        } else if (h.shown) {
            s.editor.load(rs64::net::ipv4_text(h.shown));
        }
    }
    cfg->bind_v4 = h.bind;
    if (port) {
        cfg->port = port;
    }
    if (h.bind) {
        s.last_host = rs64::net::ipv4_text(h.bind) + (port ? ":" + std::to_string(port) : "");
    }
    s.lan_address = h.shown ? rs64::net::ipv4_text(h.shown) : "";
    s.host_port = cfg->port != 27064 ? cfg->port : 0;
    save_net_config();
    fprintf(stderr, "[lobby] host address %s port %u (row %s)\n", h.bind ? rs64::net::ipv4_text(h.bind).c_str() : "every address", (unsigned)cfg->port, s.editor.entry().c_str());
}

// The row goes back to the join address once this side stops hosting.
static void restore_join_row() {
    Ghost& s = g();
    if (!s.row_hosting) {
        return;
    }
    s.row_hosting = false;
    s.editor.load(s.join_entry);
}

extern "C" void rs64_lobby_host(void) {
    Ghost& s = g();
    load_net_config();
    if (const rs64_mp_transport* t = relay_ready()) {
        relay_start(t, rs64::net::Role::Host, "");
        return;
    }
    rs64::net::LinkConfig cfg = link_config();
    cfg.role = rs64::net::Role::Host;
    set_from_menu(true);
    host_on_row(&cfg);
    lobby_start(cfg);
    rs64::net::DiscoveryReply info;
    if (s.rdram) {
        for (int i = 0; i < 3; ++i) {
            info.name[i] = (char)rb(s.rdram, 0x80130B47u + (uint32_t)i);
        }
    }
    info.build = s.lobby.local_build();
    const bool answering = s.started && s.responder.start(cfg.port, info);
    fprintf(stderr, "[lobby] LAN discovery %s on port %u\n", answering ? "answering" : "unavailable", (unsigned)(cfg.port + rs64::net::kDiscoveryPortOffset));
    // ROGUESQ_MP_UPNP=0 leaves the router alone (test runs).
    static const bool s_upnp = recomp::dbg::env_int("ROGUESQ_MP_UPNP", 1) != 0;
    if (s.started && s_upnp && (cfg.bind_v4 >> 24) != 127u) {
        s.mapper.start(cfg.port, cfg.bind_v4 ? rs64::net::ipv4_text(cfg.bind_v4) : "");
    }
}

constexpr uint32_t kSearchMs = 2000;
constexpr uint32_t kQueryEveryMs = 250;

// Leaving the lobby (cancel, or the page without a ready session): LAN discovery stops and the router mapping goes.
static void stop_discovery() {
    Ghost& s = g();
    s.responder.stop();
    s.finder.stop();
    s.searching = false;
    s.mapper.stop();
}

// JOIN with nothing found on the LAN (or a typed address in ROGUESQ_MP_ADDR): connect to the address the editor holds.
static void join_typed(rs64::net::LinkConfig cfg) {
    Ghost& s = g();
    // ROGUESQ_MP_ADDR (a hostname or IPv6 the octets cannot show) wins until the player edits the address.
    if (!(s.editor.saved.empty() && !s.editor.touched && recomp::dbg::env_str("ROGUESQ_MP_ADDR"))) {
        cfg.addr = s.editor.target();
    }
    lobby_start(cfg);
}

extern "C" void rs64_lobby_join(void) {
    Ghost& s = g();
    load_net_config();
    save_net_config();
    s.lobby.set_join_timeout(rs64::net::Lobby::kJoinTimeoutMs);
    if (const rs64_mp_transport* t = relay_ready()) {
        if (s.code.typing) {
            s.code.end_typing();
        }
        s.code.editing = false;
        // ROGUESQ_MP_CODE: the join code for scripted runs.
        if (const char* c = recomp::dbg::env_str("ROGUESQ_MP_CODE")) {
            s.code.load(c);
        }
        relay_start(t, rs64::net::Role::Client, s.code.code());
        return;
    }
    rs64::net::LinkConfig cfg = link_config();
    cfg.role = rs64::net::Role::Client;
    // JOIN tapped mid-typing joins the typed address.
    if (s.editor.typing) {
        s.editor.end_typing();
    }
    s.editor.editing = false;
    if (s.editor.port) {
        cfg.port = s.editor.port;
    }
    set_from_menu(true);
    // A scripted address skips the search; the pair runner shares one machine and port.
    if (recomp::dbg::env_str("ROGUESQ_MP_ADDR") || !s.finder.start(cfg.port)) {
        join_typed(cfg);
        return;
    }
    s.lobby.join();
    s.searching = true;
    s.search_cfg = cfg;
    s.search_until_ms = now_ms32() + kSearchMs;
    s.next_query_ms = 0;
    fprintf(stderr, "[lobby] searching the LAN\n");
}

// One front-end frame of discovery: the host answers; a search connects to the first host that answers, or to the typed address when time runs out.
static void discovery_tick() {
    Ghost& s = g();
    s.responder.poll();
    if (s.lobby.ready()) {
        s.responder.stop();
    }
    if (!s.searching) {
        return;
    }
    const uint32_t now = now_ms32();
    if ((int32_t)(now - s.next_query_ms) >= 0) {
        s.finder.query();
        s.next_query_ms = now + kQueryEveryMs;
    }
    const std::vector<rs64::net::FoundHost> found = s.finder.poll();
    rs64::net::LinkConfig cfg = s.search_cfg;
    if (!found.empty()) {
        cfg.addr = found[0].addr;
        cfg.port = found[0].info.port;
        s.editor.load(cfg.addr);
        s.editor.port = cfg.port == link_config().port ? 0 : cfg.port;
        save_net_config();
        fprintf(stderr, "[lobby] found %.3s at %s:%u\n", found[0].info.name, cfg.addr.c_str(), cfg.port);
        stop_discovery();
        lobby_start(cfg);
        return;
    }
    if ((int32_t)(now - s.search_until_ms) >= 0) {
        fprintf(stderr, "[lobby] no host answered on the LAN; trying the typed address\n");
        stop_discovery();
        join_typed(cfg);
    }
}

static void skip_filter(uint16_t* buttons) {
    constexpr uint16_t kStart = 0x1000;
    Ghost& s = g();
    const uint32_t skips = rs64::mp::cutscene_skips();
    const bool skipped = skips != s.skips_seen;
    s.skips_seen = skips;
    if (!s.started || !s.lobby.ready()) {
        rs64::mp::set_flag("hangar_launch", 0);
        return;
    }
    // Skippable scenes: a cutscene timeline (the crawl, in-mission cutscenes) or the hangar launch after both picked (not a timeline; START skips it).
    const bool timeline = rs64::mp::in_cutscene() != 0;
    // The launch starts once the hangar took the pick (result 0x800CD6D4 = the craft; 0 before, 0xFFFFFE = left); the START that confirms is not a skip.
    const uint32_t hangar_result = s.rdram ? rw(s.rdram, 0x800CD6D4u) : 0u;
    const bool launching = !timeline && s.in_hangar && s.craft.released() && hangar_result != 0u && hangar_result != 0xFFFFFEu;
    rs64::mp::set_flag("hangar_launch", launching ? 1 : 0);
    const int scene = timeline ? 2 : launching ? 1 : 0;
    if (scene != s.skip_scene) {
        s.skip.end_scene();
        s.skip_scene = scene;
    }
    const bool cine = scene != 0;
    const bool launch_skipped = launching && (*buttons & kStart) && !(s.skip_prev_buttons & kStart);
    s.skip_prev_buttons = *buttons;
    // Mission hooks do not run during cutscenes, so the link is drained here.
    if (cine) {
        drain();
    }
    if (s.skip.on_local(skipped || launch_skipped, now_s())) {
        s.link.send(rs64::net::encode_skip(), true);
        fprintf(stderr, "[ghost] cutscene skipped here; telling the other player\n");
    } else if (skipped) {
        fprintf(stderr, "[ghost] cutscene skipped by the other player's skip\n");
    }
    // Test: ROGUESQ_MP_TEST_NO_SKIP_SHARE=1 ignores the other player's skips (a late joiner).
    static const bool s_no_share = recomp::dbg::env_on("ROGUESQ_MP_TEST_NO_SKIP_SHARE");
    if (s.skip.press(now_s(), cine) && !s_no_share) {
        *buttons |= kStart;
    }
}

extern "C" uint32_t rs64_lobby_ready(void) {
    return g().lobby.ready() ? 1u : 0u;
}

// Leaving a lobby page: drop the connection unless it is ready for a mission.
extern "C" void rs64_lobby_leave(void) {
    Ghost& s = g();
    s.editor.cancel_typing();
    s.code.cancel_typing();
    if (s.lobby.ready()) {
        return;
    }
    stop_discovery();
    s.link.stop();
    relay_shutdown();
    restore_join_row();
    s.started = false;
    s.stop_at_ms = -1;
    s.lobby.cancel();
    s.lobby_session = false;
    set_from_menu(false);
    rs64::mp::set_flag("mp_lobby_state", (int)s.lobby.state());
    fprintf(stderr, "[lobby] cancelled\n");
}

// release_relay: also shut the relay down (the session ends); STOP HOSTING / CANCEL JOIN on the page keep it up for the next try.
static void lobby_stop(bool release_relay) {
    Ghost& s = g();
    stop_discovery();
    s.link.stop();
    if (release_relay) {
        relay_shutdown();
    }
    s.relay_session = false;
    s.started = false;
    s.stop_at_ms = -1;
    s.lobby.cancel();
    rs64::mp::set_flag("mp_lobby_state", (int)s.lobby.state());
    s.lobby_session = false;
    set_from_menu(false);
    s.editor.cancel_typing();
    s.code.cancel_typing();
    restore_join_row();
    fprintf(stderr, "[lobby] stopped\n");
}

extern "C" void rs64_lobby_cancel(void) {
    lobby_stop(true);
}

// Each front-end frame: link events and messages in (the receive queue never piles up in the menus).
extern "C" void rs64_lobby_tick(void) {
    rs64_lobby_status();
    // A Steam friends-list join (or accepted invite) while no lobby runs here joins that game.
    if (const rs64_mp_transport* t = relay(); t && !g().started) {
        if (const char* peer = t->pending_join(t->user)) {
            relay_start(t, rs64::net::Role::Client, peer);
        }
    }
    discovery_tick();
    if (g().started) {
        drain();
    }
}

extern "C" const char* rs64_lobby_status(void) {
    Ghost& s = g();
    rs64::mp::set_flag("mp_lobby_state", (int)s.lobby.state());
    s.status_text = s.searching ? "SEARCHING" : s.lobby.status();
    if (rs64::net::waiting_status(s.status_text)) {
        s.status_text = rs64::net::status_dots(s.status_text, now_ms32());
    }
    return s.status_text.c_str();
}

extern "C" const char* rs64_lobby_online_label(void) {
    Ghost& s = g();
    s.online_text = rs64::net::online_label(s.mapper.state());
    if (rs64::net::waiting_status(s.online_text)) {
        s.online_text = rs64::net::status_dots(s.online_text, now_ms32());
    }
    return s.online_text.c_str();
}

extern "C" const char* rs64_lobby_host_address(void) {
    Ghost& s = g();
    if (s.lan_address.empty()) {
        s.lan_address = rs64::net::lan_address_text();
    }
    s.host_address = rs64::net::host_address_label(s.mapper.state(), s.lan_address, s.mapper.external(), s.host_port);
    return s.host_address.c_str();
}

// LobbyState as an int: 0 idle, 1 hosting, 2 joining, 3 connected, 4 failed.
extern "C" int rs64_lobby_state(void) {
    return (int)g().lobby.state();
}

extern "C" uint32_t rs64_lobby_hosting(void) {
    const Ghost& s = g();
    return (s.started && s.lobby.hosting()) ? 1u : 0u;
}

// Off the page the editor is closed, typing included: rs64_lobby_input then hides the keyboard and clears text_entry (the Android picture shift).
extern "C" void rs64_lobby_edit_off(void) {
    g().editor.cancel_typing();
    g().code.cancel_typing();
}

// Keyboard entry from the SDL event loop (same game thread as rs64_lobby_input).
extern "C" int rs64_lobby_typing(void) {
    return (g().editor.typing || g().code.typing) ? 1 : 0;
}

extern "C" void rs64_lobby_type_text(const char* utf8) {
    if (g().code.typing) {
        g().code.type_text(utf8);
    } else {
        g().editor.type_text(utf8);
    }
}

// key: '\b' deletes the last character, '\r' finishes (applies a valid address and hides the keyboard, never confirms).
extern "C" void rs64_lobby_type_key(int key) {
    Ghost& s = g();
    if (s.code.typing) {
        if (key == '\b') {
            s.code.type_backspace();
        } else if (key == '\r') {
            const bool ok = s.code.end_typing();
            fprintf(stderr, "[lobby] code typed: %s%s\n", s.code.code().c_str(), ok ? "" : " (kept; typed text was not a full code)");
        }
        return;
    }
    if (key == '\b') {
        s.editor.type_backspace();
    } else if (key == '\r') {
        const bool ok = s.editor.end_typing();
        fprintf(stderr, "[lobby] address typed: %s%s\n", s.editor.text().c_str(), ok ? "" : " (kept; typed text was not an address)");
    }
}

extern "C" void rs64_lobby_edit_toggle(void) {
    Ghost& s = g();
    if (relay_ready()) {
        s.code.editing = !s.code.editing;
        if (s.code.editing) {
            s.code.begin_typing();
        } else if (s.code.typing) {
            s.code.end_typing();
        }
        fprintf(stderr, "[lobby] code edit %s: %s\n", s.code.editing ? "on" : "off", s.code.code().c_str());
        return;
    }
    // The row is the address being hosted on: STOP HOSTING first.
    if (rs64_lobby_hosting()) {
        return;
    }
    s.editor.editing = !s.editor.editing;
    s.editor.held_frames = 0;
    // Editing starts in keyboard mode; the pad can still finish it (A or B).
    if (s.editor.editing) {
        s.editor.begin_typing();
    } else if (s.editor.typing) {
        s.editor.end_typing();
    }
    fprintf(stderr, "[lobby] address edit %s: %s\n", s.editor.editing ? "on" : "off", s.editor.text().c_str());
}

namespace rs64::ghost {

// A menu lobby session is on: the mission and craft screens follow the lobby rules.
bool mp_menus() {
    const Ghost& s = g();
    return s.from_menu && s.lobby.ready();
}

// What each screen's status slot shows (cleared when the screen re-creates its fonts).
static std::string s_ms_shown, s_cs_shown;

// A status line on a screen font's extra slot (the screen's fontAlloc hook adds it in a session): setFontTextSlot(font, str, slot, x = -0x100 centred, y), alpha 0x80061C74. Callers use y = -0x7C on mission select (just under the title, above the hologram) and -0x70 on craft select (under its hint line).
void show_status(uint8_t* rdram, recomp_context* ctx, uint32_t font, uint32_t slot, int y, const std::string& text, std::string* shown) {
    Ghost& s = g();
    if (*shown == text) {
        return;
    }
    if (!s.status_buf) {
        const uint32_t a = rs64::mp::alloc(rdram, 40);
        if (!a) {
            return;
        }
        s.status_buf = a;
    }
    const size_t n = std::min<size_t>(text.size(), 39);
    for (size_t i = 0; i < n; ++i) {
        rdram[((s.status_buf - 0x80000000u) + (uint32_t)i) ^ 3] = (uint8_t)text[i];
    }
    rdram[((s.status_buf - 0x80000000u) + (uint32_t)n) ^ 3] = 0;
    rs64::mp::call(rdram, ctx, 0x80063CFCu, {font, s.status_buf, slot, (uint32_t)-0x100}, {(uint32_t)y});
    rs64::mp::call(rdram, ctx, 0x80061C74u, {font, slot, text.empty() ? 0u : 0xFFu});
    *shown = text;
}

}

// Mission select (menu overlay, 0x800C5D9C) before its init call (0x800C5E84: a2 = highest level shown, a3 = start level): a client that already has the host's pick starts on it.
extern "C" void rs64_lobby_mission_select_init(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    Ghost& s = g();
    static const long test_unlock = recomp::dbg::env_int("ROGUESQ_MP_TEST_UNLOCK", -1);
    if (!mp_menus()) {
        return;
    }
    // ROGUESQ_MP_TEST_ALLCRAFT=1: the all-crafts unlock (gGameSettings+0xC bit 0x80000), so a test pair can fly different crafts.
    static const bool test_allcraft = recomp::dbg::env_on("ROGUESQ_MP_TEST_ALLCRAFT");
    if (test_allcraft) {
        ww(rdram, 0x80130B4Cu, rw(rdram, 0x80130B4Cu) | 0x80000u);
    }
    s.in_hangar = false;
    s.follow_out = false;
    if (s.player == 0) {
        s.sent_browse = -1;
        // A new round: the client's craft comes with its next READY.
        s.remote_craft = -1;
        if (test_unlock > (long)(uint32_t)ctx->r6) {
            ctx->r6 = (gpr)test_unlock;
        }
    }
    const int start = s.host_pick >= 0 ? s.host_pick : s.host_cursor;
    if (s.player == 1 && start >= 0) {
        ctx->r7 = (gpr)start;
        if ((int)(uint32_t)ctx->r6 < start) {
            ctx->r6 = (gpr)start;
        }
    }
}

// The level select's font 2 gets a third slot for the status line (fontAlloc at 0x800AF0D0, a2 = slot count).
extern "C" void rs64_lobby_mission_select_fonts(recomp_context* ctx) {
    if (mp_menus()) {
        ctx->r6 = 3;
        // The new slot starts empty: whatever was shown last time must be written again.
        s_ms_shown.clear();
    }
}

// Mission select tick, after its pad poll (0x800AF3EC): the client waits for the host's pick (its A/Start taken), then walks the cursor there and confirms with injected presses (pressed word 0x8013A960 + 4 * port; state: cursor 0x800CDAC1, screen state 0x800CDAC4, unlocked mask 0x800CDAD0).
extern "C" void rs64_lobby_mission_select_tick(uint8_t* rdram, recomp_context* ctx) {
    Ghost& s = g();
    std::string& shown = s_ms_shown;
    // The menu tree's tick does not run on this screen: drain the link here so the host's pick arrives.
    if (s.started) {
        drain();
    }
    if (!mp_menus()) {
        shown.clear();
        return;
    }
    const int cursor = rb(rdram, 0x800CDAC1u);
    const int state = rb(rdram, 0x800CDAC4u);
    const uint32_t pressed = 0x8013A960u + 4u * rb(rdram, 0x800CDAC8u);
    const uint32_t own = rw(rdram, pressed);
    // Host: its cursor and screen state go out on change, so the client's screen moves with it (a B back-out also sets state 4: reported as browsing, result 0x800CDACC).
    if (s.player == 0) {
        // The client ended the briefing (state 3): A here confirms it for both (the client follows this screen out).
        if (s.brief_done_ms >= 0) {
            if (state == 3) {
                ww(rdram, pressed, own | 0x8000u);
                fprintf(stderr, "[lobby] the other player ended the briefing\n");
                s.brief_done_ms = -1;
            } else if (now_ms32() - (uint32_t)s.brief_done_ms > 2000u) {
                s.brief_done_ms = -1;
            }
        }
        const int reported = rs64::ls::browse_report(state, rw(rdram, 0x800CDACCu));
        const int now = (cursor << 8) | reported;
        if (now != s.sent_browse) {
            s.sent_browse = now;
            s.link.send(rs64::net::encode_browse((uint8_t)cursor, (uint8_t)reported), true);
        }
        return;
    }
    // Client: A or START during the briefing asks the host to end it (once per briefing).
    if (state != 3) {
        s.brief_sent = false;
    } else if (!s.brief_sent && (own & 0x9000u)) {
        s.brief_sent = true;
        s.link.send(rs64::net::encode_briefing_done(), true);
        fprintf(stderr, "[lobby] briefing ended here; telling the host\n");
    }
    if (s.host_pick < 0 && s.host_cursor < 0) {
        ww(rdram, pressed, own & ~0x9000u);
        show_status(rdram, ctx, 2, 2, -0x7C, "WAITING FOR HOST", &shown);
        return;
    }
    show_status(rdram, ctx, 2, 2, -0x7C, "HOST IS CHOOSING", &shown);
    // Mirror the host: its cursor and briefing while it browses, its pick once made. The client's own presses are replaced.
    // The host confirmed when its screen starts exiting (BROWSE state 4); its PICK only follows after its exit fade.
    const bool picked = s.host_pick >= 0 || s.host_state == 4;
    const int target = s.host_pick >= 0 ? s.host_pick : s.host_cursor;
    ww(rdram, 0x800CDAD0u, rw(rdram, 0x800CDAD0u) | ((1u << (target + 1)) - 1u));
    uint32_t edge = 0;
    if (--s.follow_cool <= 0) {
        const int step = rs64::ls::follow_step(cursor, state, target, s.host_state, picked);
        edge = step == rs64::ls::kFollowLeft ? 0x0200u : step == rs64::ls::kFollowRight ? 0x0100u : step == rs64::ls::kFollowConfirm ? 0x8000u : step == rs64::ls::kFollowBack ? 0x4000u : 0u;
        if (edge) {
            s.follow_cool = 8;
        }
    }
    // The client can still leave (B, back to the main menu, which ends the session) while the host browses.
    if (state == 2 && !picked) {
        edge |= own & 0x4000u;
    }
    ww(rdram, pressed, edge);
}

// initMission right after choosePlayerCraftAssets (0x800FA5BC, still the load phase): the other player's craft as the puppet. Its wingman mesh is loaded the way the level's
// wingman loader does it (isHobObjectLoaded 0x80056DA4; else sprintf "pl_crafts/%s" @0x8003BB5C, load_hmt_and_hob 0x8005645C(path, 0, 1, 0), loadCraftShadowTextures 0x8006B710).
static int s_puppet_model = -1;

extern "C" void rs64_ghost_craft_assets(uint8_t* rdram, recomp_context* ctx) {
    Ghost& s = g();
    s_puppet_model = -1;
    if (!s.in_session) {
        return;
    }
    const int m = rs64::ls::puppet_model(s.remote_craft);
    if (m < 0) {
        fprintf(stderr, "[coop] other player's craft %d has no wingman model; puppet uses the level's\n", s.remote_craft);
        return;
    }
    static uint32_t buf = 0;
    if (!buf) {
        const uint32_t a = rs64::mp::alloc(rdram, 64);
        if (!a) {
            return;
        }
        buf = a;
    }
    const char* name = rs64::ls::wingman_model_name(m);
    for (size_t i = 0; i <= strlen(name); ++i) {
        rdram[((buf - 0x80000000u) + (uint32_t)i) ^ 3] = (uint8_t)name[i];
    }
    if (rs64::mp::call(rdram, ctx, 0x80056DA4u, {buf}).v0 == 0) {
        static const uint32_t kShadow[8] = {2, 3, 0, 1, 4, 5, 7, 6};
        rs64::mp::call(rdram, ctx, 0x80033CC4u, {buf + 16u, 0x8003BB5Cu, buf});
        rs64::mp::call(rdram, ctx, 0x8005645Cu, {buf + 16u, 0u, 1u, 0u});
        rs64::mp::call(rdram, ctx, 0x8006B710u, {kShadow[m]});
        fprintf(stderr, "[coop] loaded %s for the other player's craft %d\n", name, s.remote_craft);
    }
    s_puppet_model = m;
}

// The puppet's wingman model for this mission (-1: use the level's).
extern "C" int rs64_ghost_puppet_model(void) {
    return g().in_session ? s_puppet_model : -1;
}

// Craft select init: font 2 gets a fourth slot for the status line (fontAlloc at 0x800AAC44, a2 = slot count), and this player has not picked yet.
extern "C" void rs64_lobby_craft_select_init(recomp_context* ctx) {
    Ghost& s = g();
    if (!mp_menus()) {
        return;
    }
    ctx->r6 = 4;
    s.craft.back();
    s.release_cool = 0;
    s.in_hangar = true;
    s.left_sent = false;
    s.follow_out = false;
    s_cs_shown.clear();
}

// Craft select frame, after its pad poll (0x800AADD8): A/Start records the craft (bay 0x800CD6E4 -> table 0x800CC3F8) and sends READY instead of confirming; while waiting only B passes. Leaving (hangar result 0x800CD6D4 = 0xFFFFFE) sends READY 0xFF and the other player follows back.
// Once both picked, one A is injected (every half second until the hangar takes it); the hangar idle timer (0x800CD714, confirms on its own) is held at 0 until then.
extern "C" void rs64_lobby_craft_select_tick(uint8_t* rdram, recomp_context* ctx) {
    Ghost& s = g();
    std::string& shown = s_cs_shown;
    if (s.started) {
        drain();
    }
    if (!mp_menus()) {
        shown.clear();
        return;
    }
    const uint32_t pressed = 0x8013A960u + 4u * rb(rdram, 0x800CD6D0u);
    uint32_t p = rw(rdram, pressed);
    if (!s.left_sent && rw(rdram, 0x800CD6D4u) == 0xFFFFFEu) {
        s.left_sent = true;
        s.craft.back();
        if (!s.follow_out) {
            s.link.send(rs64::net::encode_ready(0xFF), true);
            fprintf(stderr, "[lobby] left craft select\n");
        }
    }
    if (s.follow_out) {
        p = 0;
        if (--s.release_cool <= 0) {
            p = 0x4000u;
            s.release_cool = 30;
        }
        show_status(rdram, ctx, 2, 3, -0x70, "", &shown);
        ww(rdram, pressed, p);
        return;
    }
    if (!s.craft.released()) {
        ww(rdram, 0x800CD714u, 0);
    }
    if (s.craft.local < 0 && (p & 0x9000u)) {
        const int c = rb(rdram, 0x800CC3F8u + rb(rdram, 0x800CD6E4u));
        s.craft.pick(c);
        s.link.send(rs64::net::encode_ready((uint8_t)c), true);
        fprintf(stderr, "[lobby] picked craft %d, waiting for partner\n", c);
        p &= ~0x9000u;
    } else if (s.craft.local >= 0 && !s.craft.released()) {
        p &= 0x4000u;
    }
    // Released: the player's own presses pass (START skips the launch that follows); the A pulse confirms the pick the barrier held back.
    if (s.craft.released()) {
        if (--s.release_cool <= 0) {
            p |= 0x8000u;
            s.release_cool = 30;
        }
        show_status(rdram, ctx, 2, 3, -0x70, "", &shown);
    } else if (s.craft.local >= 0) {
        p &= ~0x9000u;
        show_status(rdram, ctx, 2, 3, -0x70, "WAITING ON PARTNER", &shown);
    } else {
        show_status(rdram, ctx, 2, 3, -0x70, "", &shown);
    }
    ww(rdram, pressed, p);
}

// Mission select's level write (0x800C5ED8, s0 = the confirmed level): the host announces it, the client takes the host's.
extern "C" void rs64_lobby_mission_confirmed(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    Ghost& s = g();
    if (!mp_menus()) {
        return;
    }
    const uint32_t level = (uint32_t)ctx->r16 & 0xFFu;
    if (s.player == 0) {
        s.link.send(rs64::net::encode_pick((uint8_t)level), true);
        fprintf(stderr, "[lobby] host picked level %u (sent)\n", level);
        // Level 0x10 forces the T-16 and skips craft select, so no READY comes.
        if (level == 0x10) {
            s.remote_craft = 7;
        }
        return;
    }
    const int level_here = s.host_pick >= 0 ? s.host_pick : (s.host_state == 4 ? s.host_cursor : -1);
    if (level_here >= 0) {
        fprintf(stderr, "[lobby] following the host to level %d\n", level_here);
        ctx->r16 = (gpr)level_here;
        s.host_pick = -1;
        s.host_cursor = -1;
        s.host_state = 0;
    }
}

// Front-end loop, after the pad poll: while the address is being edited it takes the pad (pressed word 0x8013A960; held from the raw pad 0x80130B88: u16 buttons, s8 stick x, s8 stick y) and clears the pressed word so the menu does not move or confirm.
template <class Editor>
static void edit_input(uint8_t* rdram, Editor& editor);

extern "C" void rs64_lobby_input(uint8_t* rdram) {
    g().rdram = rdram;
    Ghost& s = g();
    if (s.code.editing || s.code.typing) {
        edit_input(rdram, s.code);
    } else {
        edit_input(rdram, s.editor);
    }
}

// One front-end frame for whichever row is being edited (the address, or a relay's join code).
template <class Editor>
static void edit_input(uint8_t* rdram, Editor& editor) {
    Ghost& s = g();
    // The keyboard (on-screen on a phone) is up exactly while the address is being typed.
    if (editor.typing && !s.text_on) {
        rs64::mp::text_input(true);
        s.text_on = true;
        s.kb_shown = false;
    } else if (!editor.typing && s.text_on) {
        rs64::mp::text_input(false);
        s.text_on = false;
        // Return is also bound to A: its press must not reach the menu (it would reopen the editor).
        s.swallow_frames = 15;
    }
    if (s.swallow_frames > 0) {
        --s.swallow_frames;
        ww(rdram, 0x8013A960u, rw(rdram, 0x8013A960u) & ~0x9000u);
    }
    // A phone keyboard hidden by the system (back, its own hide key) ends typing.
    if (editor.typing) {
        const int kb = rs64::mp::screen_keyboard_shown();
        if (kb == 1) {
            s.kb_shown = true;
        } else if (kb == 0 && s.kb_shown) {
            editor.end_typing();
            fprintf(stderr, "[lobby] keyboard hidden: %s\n", editor.label().c_str());
        }
    }
    rs64::mp::set_flag("text_entry", editor.typing ? 1 : 0);
    if (!editor.editing) {
        return;
    }
    // While typing, presses that come from held keyboard keys (Backspace and Space are bound to B) are the typing, not the pad.
    if (editor.typing && rs64::mp::any_key_held()) {
        ww(rdram, 0x8013A960u, 0);
        return;
    }
    const uint32_t p = rw(rdram, 0x8013A960u);
    const uint16_t btn = rs64::mips::rh(rdram, 0x80130B88u);
    const int8_t sy = (int8_t)rb(rdram, 0x80130B8Bu);
    uint32_t pressed = 0;
    pressed |= (p & 0x00200800u) ? rs64::net::kPadUp : 0u;
    pressed |= (p & 0x00100400u) ? rs64::net::kPadDown : 0u;
    pressed |= (p & 0x00800200u) ? rs64::net::kPadLeft : 0u;
    pressed |= (p & 0x00400100u) ? rs64::net::kPadRight : 0u;
    pressed |= (p & 0x8000u) ? rs64::net::kPadA : 0u;
    pressed |= (p & 0x4000u) ? rs64::net::kPadB : 0u;
    uint32_t held = 0;
    held |= ((btn & 0x0800u) || sy > 40) ? rs64::net::kPadUp : 0u;
    held |= ((btn & 0x0400u) || sy < -40) ? rs64::net::kPadDown : 0u;
    if (editor.input(pressed, held)) {
        ww(rdram, 0x8013A960u, 0);
    }
    if (!editor.editing) {
        fprintf(stderr, "[lobby] address edit off: %s\n", editor.label().c_str());
    }
}

extern "C" const char* rs64_lobby_address_label(void) {
    Ghost& s = g();
    load_net_config();
    s.address_text = s.editor.label();
    return s.address_text.c_str();
}

// Each source keeps its own buffer: the returned pointer stays valid until that source is read again.
static const char* mp_text(std::string& buf, const std::string& v) {
    buf = v;
    return buf.c_str();
}

extern "C" void rs64_ghost_register_host(void) {
    using rs64::mp::add_text_source;
    add_text_source("mp_host_label", [](void*) -> const char* {
        static std::string b;
        return mp_text(b, rs64_lobby_hosting() ? "STOP HOSTING" : "HOST GAME");
    }, nullptr);
    add_text_source("mp_join_label", [](void*) -> const char* {
        static std::string b;
        return mp_text(b, !rs64_lobby_hosting() && (rs64_lobby_state() == 2 || rs64_lobby_state() == 3) ? "CANCEL JOIN" : "JOIN GAME");
    }, nullptr);
    // Polling this row is what initializes the relay transport: it is read only while the MULTIPLAYER page is up.
    add_text_source("mp_address", [](void*) -> const char* {
        static std::string b;
        Ghost& s = g();
        const bool code_row = relay_ready() != nullptr;
        // The row's mode can change mid-edit (Steam starts or closes): close the editor it no longer shows, so input never goes to a hidden one.
        if (code_row && (s.editor.editing || s.editor.typing)) {
            s.editor.cancel_typing();
        } else if (!code_row && (s.code.editing || s.code.typing)) {
            s.code.cancel_typing();
        }
        return mp_text(b, code_row ? s.code.label() : std::string(rs64_lobby_address_label()));
    }, nullptr);
    add_text_source("mp_status", [](void*) -> const char* {
        static std::string b;
        return mp_text(b, rs64_lobby_status());
    }, nullptr);
    // Host-only lines stay blank (a space: an empty source falls back to the JSON label) until HOST GAME.
    add_text_source("mp_host_address", [](void*) -> const char* {
        static std::string b;
        const rs64_mp_transport* t = relay();
        if (rs64_lobby_hosting() && g().relay_session && t) {
            const char* h = t->host_label(t->user);
            return mp_text(b, h && *h ? std::string(h) : std::string(" "));
        }
        return mp_text(b, rs64_lobby_hosting() ? std::string(rs64_lobby_host_address()) : std::string(" "));
    }, nullptr);
    // The relay's status shows while idle too (STEAM API DLL MISSING); a lobby over ENet shows the router status instead.
    add_text_source("mp_online", [](void*) -> const char* {
        static std::string b;
        const rs64_mp_transport* r = relay();
        std::string t;
        if (r && (g().relay_session || !g().started)) {
            const char* st = r->status(r->user);
            t = st ? st : "";
        } else if (rs64_lobby_hosting()) {
            t = rs64_lobby_online_label();
        }
        return mp_text(b, t.empty() ? std::string(" ") : t);
    }, nullptr);
    rs64::mp::add_input_filter([](int port, uint16_t* buttons, float*, float*, void*) {
        if (port == 0) skip_filter(buttons);
    }, nullptr);
    // MULTIPLAYER page: HOST GAME / JOIN GAME start or stop (JOIN does nothing while this side hosts), the address row toggles its editor; mp_ready ends the pilot-first page.
    rs64::mp::add_action("mp_host", [](void*) {
        if (rs64_lobby_hosting()) {
            lobby_stop(false);
        } else {
            rs64_lobby_host();
        }
    });
    rs64::mp::add_action("mp_join", [](void*) {
        if (rs64_lobby_hosting()) {
            return;
        }
        if (rs64_lobby_state() == 2 || rs64_lobby_state() == 3) {
            lobby_stop(false);
        } else {
            rs64_lobby_join();
        }
    });
    rs64::mp::add_action("mp_edit", [](void*) { rs64_lobby_edit_toggle(); });
    rs64::mp::add_action("mp_leave", [](void*) { rs64_lobby_leave(); });
    rs64::mp::add_condition("mp_ready", [](void*) -> int { return rs64_lobby_ready() != 0; });
    // While the address is typed, typed text and Backspace/Enter go to its editor, not the game.
    rs64::mp::add_key_handler([](const char* utf8, int key, void*) -> int {
        if (!rs64_lobby_typing()) {
            return 0;
        }
        if (utf8) {
            rs64_lobby_type_text(utf8);
        } else {
            rs64_lobby_type_key(key);
        }
        return 1;
    });
    publish_modes();
}
