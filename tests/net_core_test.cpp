#include "net_core.h"
#include "check.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>

using namespace rs64::net;

static void test_launch_and_bye_roundtrip() {
    const std::string h = "rs64-ls v2\nseed=1\nlevel=0\n---\n";
    const std::vector<uint8_t> b = encode_launch(h);
    std::string out;
    CHECK(message_type(b.data(), b.size()) == (uint8_t)Msg::Launch);
    CHECK(decode_launch(b.data(), b.size(), &out) && out == h);
    CHECK(!decode_launch(b.data(), b.size() - 1, &out));
    const std::vector<uint8_t> y = encode_bye(ByeReason::Desync, 1234);
    ByeReason r = ByeReason::Finished;
    uint32_t f = 0;
    CHECK(decode_bye(y.data(), y.size(), &r, &f) && r == ByeReason::Desync && f == 1234);
    std::vector<uint8_t> bad = y;
    bad[2] = 9;
    CHECK(!decode_bye(bad.data(), bad.size(), &r, &f));
}

static void test_state_roundtrip_and_bounds() {
    StatePacket s;
    s.seq = 77;
    for (int i = 0; i < 3; ++i) {
        s.pos[i] = 1.5f + i;
        s.fwd[i] = -0.25f * i;
        s.down[i] = 0.5f;
        s.vel[i] = 3.0f - i;
    }
    for (int i = 0; i < 6; ++i) s.pad[i] = (uint8_t)(0x10 + i);
    s.foil = 200;
    s.sent_ms = 123456789u;
    const std::vector<uint8_t> b = encode_state(s);
    CHECK(message_type(b.data(), b.size()) == (uint8_t)Msg::State);
    StatePacket q;
    CHECK(decode_state(b.data(), b.size(), &q));
    CHECK(q.seq == 77 && q.pos[2] == 3.5f && q.fwd[1] == -0.25f && q.vel[0] == 3.0f && q.pad[5] == 0x15 && q.foil == 200 && q.sent_ms == 123456789u);
    for (size_t n = 0; n < b.size(); ++n) CHECK(!decode_state(b.data(), n, &q));
    std::vector<uint8_t> longer = b;
    longer.push_back(0);
    CHECK(!decode_state(longer.data(), longer.size(), &q));
}

static void test_result_presence_and_state_epoch() {
    uint8_t e = 0, v = 0;
    const std::vector<uint8_t> r = encode_result(7, 2);
    CHECK(message_type(r.data(), r.size()) == (uint8_t)Msg::Result);
    CHECK(decode_result(r.data(), r.size(), &e, &v) && e == 7 && v == 2);
    std::vector<uint8_t> bad = r;
    bad[3] = 4;
    CHECK(!decode_result(bad.data(), bad.size(), &e, &v));
    CHECK(!decode_result(r.data(), r.size() - 1, &e, &v));
    const std::vector<uint8_t> p = encode_presence(3, 1);
    CHECK(message_type(p.data(), p.size()) == (uint8_t)Msg::Presence);
    CHECK(decode_presence(p.data(), p.size(), &e, &v) && e == 3 && v == 1);
    std::vector<uint8_t> badp = p;
    badp[3] = 2;
    CHECK(!decode_presence(badp.data(), badp.size(), &e, &v));
    StatePacket s;
    s.seq = 5;
    s.flags = rs64::net::kStateDown;
    s.epoch = 9;
    StatePacket q;
    const std::vector<uint8_t> b = encode_state(s);
    CHECK(decode_state(b.data(), b.size(), &q) && q.epoch == 9 && q.seq == 5 && q.flags == kStateDown);
}

static void test_obj_snapshot_and_event() {
    ObjSnapshot s;
    s.epoch = 4;
    s.seq = 99;
    for (int i = 0; i < 128; ++i) {
        s.bools[i] = (uint8_t)(i & 1);
        s.counts[i] = i * 3 - 50;
    }
    for (int i = 0; i < 8; ++i) s.timers[i] = 1.5f * i;
    const std::vector<uint8_t> b = encode_obj(s);
    s.lives = 2;
    const std::vector<uint8_t> b2 = encode_obj(s);
    CHECK(b2.size() == 680 && message_type(b2.data(), b2.size()) == (uint8_t)Msg::Obj);
    ObjSnapshot q2;
    CHECK(decode_obj(b2.data(), b2.size(), &q2) && q2.lives == 2);
    ObjSnapshot q;
    CHECK(decode_obj(b.data(), b.size(), &q));
    CHECK(q.epoch == 4 && q.seq == 99 && q.bools[127] == 1 && q.counts[0] == -50 && q.counts[127] == 331 && q.timers[7] == 10.5f);
    CHECK(!decode_obj(b.data(), b.size() - 1, &q));
}

static void test_mission_id() {
    const std::vector<uint8_t> b = encode_mission(42);
    CHECK(message_type(b.data(), b.size()) == (uint8_t)Msg::Mission);
    uint8_t id = 0;
    CHECK(decode_mission(b.data(), b.size(), &id) && id == 42);
    CHECK(!decode_mission(b.data(), b.size() - 1, &id));
}

static void test_life_lost() {
    const std::vector<uint8_t> b = encode_life_lost(6);
    CHECK(message_type(b.data(), b.size()) == (uint8_t)Msg::Life);
    uint8_t e = 0;
    CHECK(decode_life_lost(b.data(), b.size(), &e) && e == 6);
    CHECK(!decode_life_lost(b.data(), b.size() - 1, &e));
}

static void test_hello_roundtrip() {
    Hello h{kProtocol, 0xC0FFEE01u, 1};
    memcpy(h.name, "WED", 3);
    const std::vector<uint8_t> b = encode_hello(h);
    CHECK(message_type(b.data(), b.size()) == (uint8_t)Msg::Hello);
    Hello out{};
    CHECK(decode_hello(b.data(), b.size(), &out) && out.protocol == kProtocol && out.build == 0xC0FFEE01u && out.role == 1);
    CHECK(memcmp(out.name, "WED", 3) == 0);
    CHECK(!decode_hello(b.data(), b.size() - 1, &out));
    // A peer on another protocol still reads as a HELLO, so the lobby can say VERSION MISMATCH.
    std::vector<uint8_t> other = encode_hello(Hello{(uint16_t)(kProtocol + 1), 7u, 0});
    other[0] = (uint8_t)(kProtocol + 1);
    CHECK(message_type(other.data(), other.size()) == (uint8_t)Msg::Hello);
    CHECK(decode_hello(other.data(), other.size(), &out) && out.protocol == kProtocol + 1 && out.build == 7u);
}

static void test_address_editor() {
    AddressEditor a;
    CHECK(AddressEditor::parse("192.168.1.20", &a));
    CHECK(a.text() == "192.168.1.20");
    // The menu font gains a period (its unused Ü glyph, see ensure_menu_period).
    CHECK(a.label() == "192.168.1.20");
    a.editing = true;
    a.cursor = 1;
    CHECK(a.label() == "192.<168>.1.20");
    a.step(+100);
    CHECK(a.octet[1] == 255);
    a.step(-300);
    CHECK(a.octet[1] == 0);
    a.move(+1);
    a.move(+1);
    a.move(+1);
    CHECK(a.cursor == 3);
    a.move(-4);
    CHECK(a.cursor == 0);
    CHECK(!AddressEditor::parse("300.1.1.1", &a));
    CHECK(!AddressEditor::parse("1.2.3", &a));
    CHECK(!AddressEditor::parse("::1", &a));
    CHECK(!AddressEditor::parse("1.2.3.4.5", &a));
    CHECK(!AddressEditor::parse("1..3.4", &a));
}

static void test_code_editor() {
    CodeEditor c;
    CHECK(c.code() == "000000" && c.label() == "JOIN CODE 000000");
    c.editing = true;
    CHECK(c.label() == "JOIN CODE <0>00000");
    CHECK(c.input(kPadDown, 0) && c.code() == "900000");
    c.input(kPadRight, 0);
    c.input(kPadUp, 0);
    c.input(kPadUp, 0);
    CHECK(c.code() == "920000" && c.label() == "JOIN CODE 9<2>0000");
    for (int i = 0; i < 9; ++i) c.input(kPadRight, 0);
    CHECK(c.cursor == CodeEditor::kDigits - 1);
    c.input(kPadA, 0);
    CHECK(!c.editing && !c.input(kPadUp, 0) && c.code() == "920000");
    c.begin_typing();
    CHECK(c.typing && c.label() == std::string("920000") + kCaret);
    c.type_backspace();
    c.type_backspace();
    c.type_text("4x8.");
    c.type_text("123");
    CHECK(c.label() == std::string("920048") + kCaret);
    CHECK(c.end_typing() && !c.typing && c.code() == "920048");
    c.begin_typing();
    c.type_backspace();
    CHECK(!c.end_typing() && c.code() == "920048");
    c.begin_typing();
    c.type_text("7");
    c.cancel_typing();
    CHECK(!c.typing && !c.editing && c.code() == "920048");
    CHECK(CodeEditor::valid("123456") && !CodeEditor::valid("12345") && !CodeEditor::valid("12345a") && !CodeEditor::valid("1234567"));
    c.load("654321");
    CHECK(c.code() == "654321");
    c.load("bad");
    CHECK(c.code() == "654321");
}

static void test_lobby_join_connects_and_checks_version() {
    Lobby l;
    l.join();
    CHECK(l.state() == LobbyState::Joining && l.status() == "CONNECTING");
    l.on_connected(1000);
    CHECK(!l.ready());
    l.on_hello(Hello{kProtocol, kBuildId, 0});
    CHECK(l.ready() && l.state() == LobbyState::Connected && l.status() == "CONNECTED");
}

static void test_lobby_join_times_out() {
    Lobby l;
    l.join();
    l.tick(0);
    l.tick(15001);
    CHECK(l.state() == LobbyState::Failed && l.status() == "NO ANSWER");
}

static void test_lobby_no_hello_after_connect() {
    Lobby l;
    l.join();
    l.tick(0);
    l.on_connected(100);
    l.tick(5200);
    CHECK(l.state() == LobbyState::Failed && l.status() == "NO HELLO");
}

static void test_lobby_version_mismatch_fails_both() {
    Lobby h;
    h.host();
    CHECK(h.status() == "WAITING FOR PLAYER");
    h.on_connected(0);
    h.on_hello(Hello{kProtocol, kBuildId + 1, 1});
    CHECK(h.state() == LobbyState::Failed && h.status() == "VERSION MISMATCH" && !h.ready());
    Lobby c;
    c.join();
    c.on_connected(0);
    c.on_hello(Hello{(uint16_t)(kProtocol + 1), kBuildId, 0});
    CHECK(c.state() == LobbyState::Failed && c.status() == "VERSION MISMATCH");
}

static void test_lobby_local_build_override() {
    Lobby l;
    l.set_local_build(999);
    CHECK(l.local_build() == 999);
    l.join();
    l.on_connected(0);
    l.on_hello(Hello{kProtocol, kBuildId, 0});
    CHECK(l.status() == "VERSION MISMATCH");
    l.join();
    CHECK(l.local_build() == 999);
}

static void test_lobby_lost_after_connect() {
    Lobby c;
    c.join();
    c.on_connected(0);
    c.on_hello(Hello{kProtocol, kBuildId, 0});
    c.on_lost();
    CHECK(c.state() == LobbyState::Failed && c.status() == "CONNECTION LOST");
    Lobby h;
    h.host();
    h.on_connected(0);
    h.on_hello(Hello{kProtocol, kBuildId, 1});
    h.on_lost();
    CHECK(h.state() == LobbyState::Hosting && h.status() == "PLAYER LEFT");
    h.tick(1000);
    h.tick(7000);
    CHECK(h.status() == "WAITING FOR PLAYER");
}

static void test_lobby_cancel_from_anywhere() {
    Lobby l;
    l.join();
    l.cancel();
    CHECK(l.state() == LobbyState::Idle && !l.ready());
    l.host();
    l.on_connected(0);
    l.on_hello(Hello{kProtocol, kBuildId, 1});
    l.cancel();
    CHECK(l.state() == LobbyState::Idle && !l.ready());
}

static void test_lobby_join_lost_before_hello() {
    Lobby l;
    l.join();
    l.on_connected(0);
    l.on_lost();
    CHECK(l.state() == LobbyState::Failed && l.status() == "CONNECTION LOST");
}

static void test_lobby_join_timeout_setting() {
    Lobby l;
    l.set_join_timeout(300000);
    l.join();
    l.tick(0);
    l.tick(20000);
    CHECK(l.state() == LobbyState::Joining);
    l.tick(300001);
    CHECK(l.status() == "NO ANSWER");
}

static void test_address_editor_saved_hostname() {
    AddressEditor a;
    a.load("friend.ddns.net");
    CHECK(a.target() == "friend.ddns.net" && a.label() == "SAVED ADDRESS");
    a.editing = true;
    a.input(kPadUp, kPadUp);
    CHECK(a.target() == a.text() && a.label() != "SAVED ADDRESS");
    AddressEditor b;
    b.load("10.0.0.5");
    CHECK(b.target() == "10.0.0.5" && b.label() == "10.0.0.5");
    AddressEditor c;
    c.load("");
    CHECK(c.target() == "192.168.1.2");
}

static void test_browse() {
    const std::vector<uint8_t> b = encode_browse(4, 3);
    CHECK(message_type(b.data(), b.size()) == (uint8_t)Msg::Browse);
    uint8_t level = 0, state = 0;
    CHECK(decode_browse(b.data(), b.size(), &level, &state) && level == 4 && state == 3);
    CHECK(!decode_browse(b.data(), b.size() - 1, &level, &state));
    const std::vector<uint8_t> bad_state = encode_browse(1, 9);
    CHECK(!decode_browse(bad_state.data(), bad_state.size(), &level, &state));
    const std::vector<uint8_t> bad_level = encode_browse(0x13, 2);
    CHECK(!decode_browse(bad_level.data(), bad_level.size(), &level, &state));
}

static void test_pick_and_ready() {
    const std::vector<uint8_t> p = encode_pick(2);
    CHECK(message_type(p.data(), p.size()) == (uint8_t)Msg::Pick);
    uint8_t v = 0xAA;
    CHECK(decode_pick(p.data(), p.size(), &v) && v == 2);
    CHECK(!decode_pick(p.data(), p.size() - 1, &v));
    const std::vector<uint8_t> bad = encode_pick(0x13);
    CHECK(!decode_pick(bad.data(), bad.size(), &v));
    const std::vector<uint8_t> r = encode_ready(8);
    CHECK(message_type(r.data(), r.size()) == (uint8_t)Msg::Ready);
    CHECK(decode_ready(r.data(), r.size(), &v) && v == 8);
    const std::vector<uint8_t> back = encode_ready(0xFF);
    CHECK(decode_ready(back.data(), back.size(), &v) && v == 0xFF);
    const std::vector<uint8_t> nine = encode_ready(9);
    CHECK(!decode_ready(nine.data(), nine.size(), &v));
    CHECK(!decode_ready(p.data(), p.size(), &v));
}

static void test_address_editor_input() {
    AddressEditor a;
    CHECK(!a.input(kPadUp, kPadUp));
    a.editing = true;
    CHECK(a.input(kPadUp, kPadUp) && a.octet[0] == 193);
    CHECK(a.input(kPadRight, kPadRight) && a.cursor == 1);
    CHECK(a.input(kPadDown, kPadDown) && a.octet[1] == 167);
    // Held: nothing for 11 more frames, then a step every 3 frames, by 10 from frame 45.
    for (int f = 0; f < 10; ++f) {
        a.input(0, kPadDown);
    }
    CHECK(a.octet[1] == 167);
    a.input(0, kPadDown);
    CHECK(a.octet[1] == 166);
    for (int f = 0; f < 40; ++f) {
        a.input(0, kPadDown);
    }
    const int before = a.octet[1];
    a.input(0, kPadDown);
    a.input(0, kPadDown);
    a.input(0, kPadDown);
    CHECK(before - a.octet[1] == 10);
    // Idle frames are still taken while editing, so the menu never sees a stray press.
    CHECK(a.input(0, 0));
    // B and A both end editing and are taken.
    CHECK(a.input(kPadB, kPadB) && !a.editing);
    CHECK(!a.input(kPadB, kPadB));
    a.editing = true;
    CHECK(a.input(kPadA, kPadA) && !a.editing);
}

static void test_address_editor_light_flick_steps() {
    AddressEditor a;
    a.editing = true;
    const int before = a.octet[0];
    CHECK(a.input(kPadUp, 0) && a.octet[0] == before + 1);
    CHECK(a.input(kPadDown, 0) && a.octet[0] == before);
}

static void test_discovery_messages() {
    const std::vector<uint8_t> q = encode_discovery_query();
    CHECK(is_discovery_query(q.data(), q.size()));
    DiscoveryReply r;
    r.port = 27064;
    r.name[0] = 'A';
    r.name[1] = 'B';
    r.name[2] = 'C';
    const std::vector<uint8_t> b = encode_discovery_reply(r);
    CHECK(!is_discovery_query(b.data(), b.size()));
    DiscoveryReply out;
    CHECK(decode_discovery_reply(b.data(), b.size(), &out));
    CHECK(out.port == 27064 && out.build == kBuildId && out.name[0] == 'A' && out.name[2] == 'C');
    CHECK(!decode_discovery_reply(q.data(), q.size(), &out));
    CHECK(!decode_discovery_reply(b.data(), b.size() - 1, &out));
    // Game traffic is never mistaken for discovery.
    const std::vector<uint8_t> h = encode_hello(Hello{});
    CHECK(!is_discovery_query(h.data(), h.size()) && !decode_discovery_reply(h.data(), h.size(), &out));
}

static void test_skip_message() {
    const std::vector<uint8_t> b = encode_skip();
    CHECK(message_type(b.data(), b.size()) == (uint8_t)Msg::Skip);
    CHECK(decode_skip(b.data(), b.size()));
    const std::vector<uint8_t> h = encode_hello(Hello{});
    CHECK(!decode_skip(h.data(), h.size()));
}

static void test_upgrades_message() {
    const std::vector<uint8_t> b = encode_upgrades(7, 0x800u | 0x10000u);
    CHECK(message_type(b.data(), b.size()) == (uint8_t)Msg::Upgrades);
    uint8_t e = 0;
    uint32_t bits = 0;
    CHECK(decode_upgrades(b.data(), b.size(), &e, &bits) && e == 7 && bits == 0x10800u);
    CHECK(!decode_upgrades(b.data(), b.size() - 1, &e, &bits));
    // Only power-up bits: no craft unlocks, no settings, never empty.
    const std::vector<uint8_t> unlock = encode_upgrades(7, 0x80000u);
    CHECK(!decode_upgrades(unlock.data(), unlock.size(), &e, &bits));
    const std::vector<uint8_t> none = encode_upgrades(7, 0);
    CHECK(!decode_upgrades(none.data(), none.size(), &e, &bits));
    const std::vector<uint8_t> k = encode_skip();
    CHECK(!decode_upgrades(k.data(), k.size(), &e, &bits));
    const std::vector<uint8_t> p = encode_pickup(9, 0x1234);
    CHECK(message_type(p.data(), p.size()) == (uint8_t)Msg::Pickup);
    uint16_t item = 0;
    CHECK(decode_pickup(p.data(), p.size(), &e, &item) && e == 9 && item == 0x1234);
    CHECK(!decode_pickup(p.data(), p.size() - 1, &e, &item));
    CHECK(!decode_pickup(b.data(), b.size(), &e, &item) && !decode_upgrades(p.data(), p.size(), &e, &bits));
}

static void test_briefing_done_message() {
    const std::vector<uint8_t> b = encode_briefing_done();
    CHECK(message_type(b.data(), b.size()) == (uint8_t)Msg::BriefingDone);
    CHECK(decode_briefing_done(b.data(), b.size()));
    const std::vector<uint8_t> k = encode_skip();
    CHECK(!decode_briefing_done(k.data(), k.size()) && !decode_skip(b.data(), b.size()));
}

static void test_address_editor_typing() {
    const std::string caret(1, kCaret);
    AddressEditor a;
    a.editing = true;
    // Typing starts from the current address, with the caret after it.
    a.begin_typing();
    CHECK(a.typing && a.label() == "192.168.1.2" + caret);
    a.type_backspace();
    a.type_backspace();
    CHECK(a.label() == "192.168.1" + caret);
    a.type_text("0.1x89");
    CHECK(a.label() == "192.168.10.189" + caret);
    // Enter: a valid address replaces the octets; editing ends.
    CHECK(a.end_typing() && !a.typing && !a.editing);
    CHECK(a.text() == "192.168.10.189" && a.target() == "192.168.10.189");
    // An invalid one keeps the old octets.
    a.editing = true;
    a.begin_typing();
    a.type_text("9");
    CHECK(!a.end_typing() && a.text() == "192.168.10.189");
    // At most 21 characters (255.255.255.255:65535).
    a.editing = true;
    a.begin_typing();
    for (int i = 0; i < 20; ++i) a.type_backspace();
    CHECK(a.label() == caret);
    a.type_text("1234567890123456789012345");
    CHECK(a.label() == "123456789012345678901" + caret);
    // While typing the pad's up/down and left/right do nothing, but A or B still end editing.
    a.input(kPadUp, kPadUp);
    CHECK(a.typing && a.label() == "123456789012345678901" + caret);
    a.input(kPadB, kPadB);
    CHECK(!a.typing && !a.editing);
    // Leaving the page mid-typing drops the typed text and keeps the address.
    const std::string before = a.text();
    a.editing = true;
    a.begin_typing();
    a.type_text("10.0.0.5");
    a.cancel_typing();
    CHECK(!a.typing && !a.editing && a.typed.empty() && a.text() == before);
    // Cancelling when not typing changes nothing.
    a.cancel_typing();
    CHECK(!a.typing && a.text() == before);
}

static void test_lobby_idle_status() {
    Lobby l;
    CHECK(l.status() == "HOST OR JOIN A GAME");
}

static void test_status_dots() {
    // Waiting states grow ". .. ..." one dot per period, padded to a fixed width so the centred line does not jump.
    // Padding is kDotPad, a blank glyph exactly as wide as a period, so the line keeps one width.
    const std::string pad(1, kDotPad);
    CHECK(status_dots("CONNECTING", 0) == "CONNECTING" + pad + pad + pad);
    CHECK(status_dots("CONNECTING", kDotPeriodMs) == "CONNECTING." + pad + pad);
    CHECK(status_dots("CONNECTING", 2 * kDotPeriodMs) == "CONNECTING.." + pad);
    CHECK(status_dots("CONNECTING", 3 * kDotPeriodMs) == "CONNECTING...");
    CHECK(status_dots("CONNECTING", 4 * kDotPeriodMs) == "CONNECTING" + pad + pad + pad);
    CHECK(waiting_status("SEARCHING") && waiting_status("CONNECTING") && waiting_status("PLAYER JOINING") && waiting_status("WAITING FOR PLAYER") && waiting_status("OPENING INTERNET PORT"));
    CHECK(!waiting_status("CONNECTED") && !waiting_status("HOST OR JOIN A GAME") && !waiting_status("VERSION MISMATCH"));
}

static void test_public_ipv4() {
    CHECK(public_ipv4(0x490C2238u));
    CHECK(!public_ipv4(0x0A0000BDu));
    CHECK(!public_ipv4(0xAC100001u) && !public_ipv4(0xAC1FFFFFu) && public_ipv4(0xAC200000u));
    CHECK(!public_ipv4(0xC0A80101u));
    // Carrier-grade NAT (100.64/10) and link-local are not reachable from the internet either.
    CHECK(!public_ipv4(0x64400001u) && !public_ipv4(0x647FFFFFu) && public_ipv4(0x64800000u));
    CHECK(!public_ipv4(0xA9FE0101u) && !public_ipv4(0x7F000001u) && !public_ipv4(0u));
}

static void test_online_label() {
    CHECK(online_label(PortMapState::Idle).empty());
    CHECK(online_label(PortMapState::Working) == "OPENING INTERNET PORT");
    CHECK(online_label(PortMapState::Open).empty());
    CHECK(online_label(PortMapState::NoRouter) == "UPNP IS OFF ON YOUR ROUTER");
    CHECK(online_label(PortMapState::Refused) == "YOUR ROUTER REFUSED THE PORT");
    CHECK(online_label(PortMapState::SharedAddress) == "YOUR ISP SHARES YOUR ADDRESS");
}

static void test_address_editor_port() {
    AddressEditor a;
    CHECK(AddressEditor::parse("10.0.0.5:28000", &a));
    CHECK(a.port == 28000 && a.text() == "10.0.0.5:28000" && a.target() == "10.0.0.5" && a.entry() == "10.0.0.5:28000" && a.label() == "10.0.0.5:28000");
    CHECK(AddressEditor::parse("10.0.0.5", &a) && a.port == 0 && a.entry() == "10.0.0.5");
    CHECK(!AddressEditor::parse("10.0.0.5:", &a));
    CHECK(!AddressEditor::parse("10.0.0.5:0", &a));
    CHECK(!AddressEditor::parse("10.0.0.5:65536", &a));
    CHECK(!AddressEditor::parse("10.0.0.5:1:2", &a));
    CHECK(!AddressEditor::parse("10.0.0:5", &a));
    // Typing a port keeps it; typing an address without one clears it.
    AddressEditor t;
    t.editing = true;
    t.begin_typing();
    t.type_text(":27100");
    CHECK(t.end_typing() && t.port == 27100 && t.target() == "192.168.1.2");
    t.editing = true;
    t.begin_typing();
    CHECK(t.typed == "192.168.1.2:27100");
    for (int i = 0; i < 6; ++i) t.type_backspace();
    CHECK(t.end_typing() && t.port == 0);
    // A saved hostname keeps its port apart; an IPv6 literal stays whole.
    AddressEditor h;
    h.load("friend.ddns.net:28000");
    CHECK(h.target() == "friend.ddns.net" && h.port == 28000 && h.entry() == "friend.ddns.net:28000" && !h.ipv4());
    h.load("fe80::1");
    CHECK(h.target() == "fe80::1" && h.port == 0);
    h.load("10.0.0.5:28000");
    CHECK(h.ipv4() == 0x0A000005u && h.port == 28000);
}

static void test_choose_host_address() {
    const std::vector<uint32_t> local = {0xC0A80105u, 0x64400007u};
    // The row is one of this machine's addresses (here a Tailscale one): listen there only.
    HostAddress h = choose_host_address(0x64400007u, local, 0xC0A80105u);
    CHECK(h.bind == 0x64400007u && h.shown == 0x64400007u);
    // Loopback always counts as local.
    h = choose_host_address(0x7F000001u, {}, std::nullopt);
    CHECK(h.bind == 0x7F000001u && h.shown == 0x7F000001u);
    // Another machine's address (the last JOIN), a saved hostname, or 0.0.0.0: every address, the fallback shown.
    h = choose_host_address(0x0A000009u, local, 0xC0A80105u);
    CHECK(h.bind == 0 && h.shown == 0xC0A80105u);
    h = choose_host_address(std::nullopt, local, 0xC0A80105u);
    CHECK(h.bind == 0 && h.shown == 0xC0A80105u);
    h = choose_host_address(0u, local, 0xC0A80105u);
    CHECK(h.bind == 0 && h.shown == 0xC0A80105u);
    h = choose_host_address(0x0A000009u, {}, std::nullopt);
    CHECK(h.bind == 0 && h.shown == 0);
    CHECK(ipv4_text(0x64400007u) == "100.64.0.7");
}

static void test_host_address_label() {
    CHECK(host_address_label(PortMapState::Open, "10.0.0.189", "73.12.34.56", 28000) == "YOUR NET ADDRESS 73.12.34.56:28000");
    CHECK(host_address_label(PortMapState::NoRouter, "10.0.0.189", "", 28000) == "YOUR LAN ADDRESS 10.0.0.189:28000");
    CHECK(host_address_label(PortMapState::Idle, "", "", 28000) == "NO NETWORK ADDRESS");
    CHECK(host_address_label(PortMapState::Open, "10.0.0.189", "73.12.34.56") == "YOUR NET ADDRESS 73.12.34.56");
    CHECK(host_address_label(PortMapState::Working, "10.0.0.189", "") == "YOUR LAN ADDRESS 10.0.0.189");
    CHECK(host_address_label(PortMapState::NoRouter, "10.0.0.189", "") == "YOUR LAN ADDRESS 10.0.0.189");
    CHECK(host_address_label(PortMapState::SharedAddress, "10.0.0.189", "100.64.0.9") == "YOUR LAN ADDRESS 10.0.0.189");
    CHECK(host_address_label(PortMapState::Idle, "", "") == "NO NETWORK ADDRESS");
}

static void test_lobby_start_failed() {
    Lobby h;
    h.host();
    h.on_start_failed();
    CHECK(h.state() == LobbyState::Failed && h.status() == "CANNOT HOST");
    Lobby j;
    j.join();
    j.on_start_failed();
    CHECK(j.state() == LobbyState::Failed && j.status() == "NETWORK ERROR");
}

static void test_player_out() {
    const std::vector<uint8_t> b = encode_out(9);
    CHECK(message_type(b.data(), b.size()) == (uint8_t)Msg::Out);
    uint8_t e = 0;
    CHECK(decode_out(b.data(), b.size(), &e) && e == 9);
    CHECK(!decode_out(b.data(), b.size() - 1, &e));
    const std::vector<uint8_t> life = encode_life_lost(9);
    CHECK(!decode_out(life.data(), life.size(), &e));
}

static void test_damage_batch() {
    std::vector<std::pair<uint16_t, int32_t>> in = {{0, 65}, {17, -8}, {301, 0}};
    const std::vector<uint8_t> b = encode_damage(3, in);
    CHECK(message_type(b.data(), b.size()) == (uint8_t)Msg::Damage);
    uint8_t e = 0;
    std::vector<std::pair<uint16_t, int32_t>> out;
    CHECK(decode_damage(b.data(), b.size(), &e, &out) && e == 3 && out == in);
    CHECK(!decode_damage(b.data(), b.size() - 1, &e, &out));
    std::vector<uint8_t> zero = b;
    zero[3] = 0;
    CHECK(!decode_damage(zero.data(), 4, &e, &out));
    std::vector<std::pair<uint16_t, int32_t>> many(250, {1, 1});
    CHECK(encode_damage(1, many).size() <= kMaxPacket);
}

static void test_link_poll_edges() {
    // first connect
    LinkEdges e = link_poll_edges(true, false, 1, 0);
    CHECK(e.connected && !e.lost);
    // steady
    e = link_poll_edges(true, true, 1, 1);
    CHECK(!e.connected && !e.lost);
    // plain disconnect
    e = link_poll_edges(false, true, 1, 1);
    CHECK(!e.connected && e.lost);
    // drop and reconnect between two polls
    e = link_poll_edges(true, true, 2, 1);
    CHECK(e.connected && e.lost);
    // connect and drop between two polls: no phantom peer
    e = link_poll_edges(false, false, 1, 0);
    CHECK(!e.connected && !e.lost);
    // connected, then connect+drop before the next poll
    e = link_poll_edges(false, true, 2, 1);
    CHECK(!e.connected && e.lost);
    // idle
    e = link_poll_edges(false, false, 0, 0);
    CHECK(!e.connected && !e.lost);
}

int main() {
#ifdef _WIN32
    // Report failed checks on stderr instead of the Debug CRT's modal dialog.
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    test_link_poll_edges();
    test_launch_and_bye_roundtrip();
    test_state_roundtrip_and_bounds();
    test_result_presence_and_state_epoch();
    test_obj_snapshot_and_event();
    test_damage_batch();
    test_life_lost();
    test_mission_id();
    test_player_out();
    test_hello_roundtrip();
    test_address_editor();
    test_code_editor();
    test_lobby_join_connects_and_checks_version();
    test_lobby_join_times_out();
    test_lobby_no_hello_after_connect();
    test_lobby_version_mismatch_fails_both();
    test_lobby_lost_after_connect();
    test_lobby_cancel_from_anywhere();
    test_lobby_local_build_override();
    test_address_editor_input();
    test_address_editor_light_flick_steps();
    test_lobby_start_failed();
    test_discovery_messages();
    test_skip_message();
    test_briefing_done_message();
    test_upgrades_message();
    test_address_editor_typing();
    test_lobby_idle_status();
    test_status_dots();
    test_public_ipv4();
    test_online_label();
    test_host_address_label();
    test_address_editor_port();
    test_choose_host_address();
    test_lobby_join_lost_before_hello();
    test_lobby_join_timeout_setting();
    test_address_editor_saved_hostname();
    test_pick_and_ready();
    test_browse();
    printf("net_core_test OK\n");
    return 0;
}
