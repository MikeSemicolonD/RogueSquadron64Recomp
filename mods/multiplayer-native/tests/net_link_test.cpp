#include "net/net_core.h"
#include "net/net_link.h"
#include "check.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace rs64::net;

// Two in-process endpoints wired back to back through the mp.transport table.
struct FakeEnd {
    FakeEnd* other = nullptr;
    bool started = false;
    bool was_connected = false;
    bool lost = false;
    uint32_t connects = 0;
    int role = 0;
    uint32_t build = 0;
    std::string peer;
    std::vector<std::vector<uint8_t>> inbox;
    bool up() const { return started && other && other->started; }
};
static FakeEnd* fe(void* u) { return (FakeEnd*)u; }
static void fake_poll(FakeEnd* e) {
    if (e->up() && !e->was_connected) {
        e->was_connected = true;
        e->lost = false;
        ++e->connects;
    } else if (!e->up() && e->was_connected) {
        e->was_connected = false;
        e->lost = true;
    }
}
static int f_avail(void*) { return 1; }
static int f_start(void* u, int role, const char* peer, uint32_t build) {
    fe(u)->started = true;
    fe(u)->role = role;
    fe(u)->peer = peer ? peer : "";
    fe(u)->build = build;
    return 1;
}
static void f_stop(void* u) {
    FakeEnd* e = fe(u);
    *e = FakeEnd{e->other};
}
static int f_conn(void* u) { fake_poll(fe(u)); return fe(u)->was_connected; }
static int f_lost(void* u) { fake_poll(fe(u)); return fe(u)->lost; }
static uint32_t f_count(void* u) { fake_poll(fe(u)); return fe(u)->connects; }
// Every byte the fake transport carried, as a relay would see it.
static std::vector<uint8_t> g_wire;
static void f_send(void* u, const uint8_t* b, uint32_t n, int) {
    g_wire.insert(g_wire.end(), b, b + n);
    if (fe(u)->up()) fe(u)->other->inbox.emplace_back(b, b + n);
}
static int f_flush(void*, int) { return 1; }
static void f_recv(void* u, rs64_mp_recv_fn fn, void* fu) {
    for (const auto& m : fe(u)->inbox) fn(m.data(), (uint32_t)m.size(), fu);
    fe(u)->inbox.clear();
}
static const char* f_text(void*) { return ""; }
static const char* f_none(void*) { return nullptr; }
static rs64_mp_transport fake_table(FakeEnd* e) {
    return rs64_mp_transport{RS64_MP_TRANSPORT_VERSION, (uint32_t)sizeof(rs64_mp_transport), "fake", e, f_avail, f_start, f_stop, f_stop, f_conn, f_lost, f_count, f_send, f_flush, f_recv, f_text, f_text, f_none};
}

// A Link over an external transport behaves like the ENet one: connect edges, ordered messages both ways, lost on the peer's stop, and a clean restart.
static void test_external_transport() {
    FakeEnd a, b;
    a.other = &b;
    b.other = &a;
    const rs64_mp_transport ta = fake_table(&a), tb = fake_table(&b);
    Link host, client;
    LinkConfig hc{Role::Host};
    hc.external = &ta;
    LinkConfig cc{Role::Client};
    cc.external = &tb;
    cc.peer = "123456";
    CHECK(host.start(hc));
    CHECK(!host.connected());
    CHECK(client.start(cc));
    CHECK(b.role == RS64_MP_ROLE_CLIENT && b.peer == "123456" && a.role == RS64_MP_ROLE_HOST && a.peer.empty());
    CHECK(a.build == kBuildId && b.build == kBuildId);
    CHECK(host.connected() && client.connected() && host.connect_count() == 1);
    for (int i = 0; i < 5; ++i) client.send({(uint8_t)i}, i % 2 == 0);
    host.send(encode_launch("over fake"), true);
    CHECK(client.flush(10));
    const auto got = host.receive();
    CHECK(got.size() == 5 && got[0][0] == 0 && got[4][0] == 4);
    std::string launch;
    const auto cgot = client.receive();
    CHECK(cgot.size() == 1 && decode_launch(cgot[0].data(), cgot[0].size(), &launch) && launch == "over fake");
    client.stop();
    CHECK(!host.connected() && host.lost());
    CHECK(client.start(cc));
    CHECK(host.connected() && !host.lost() && host.connect_count() == 2);
    host.stop();
    client.stop();
    CHECK(!a.started && !b.started);
}

static bool wire_has(const std::string& s) {
    return std::search(g_wire.begin(), g_wire.end(), s.begin(), s.end()) != g_wire.end();
}

// One side of ghost.cpp's lobby loop over a Link: connect edge, open and dispatch, queued replies before keys, ACCEPT once ready.
struct Side {
    Link link;
    Lobby lobby;
    bool was_up = false, was_ready = false;
    std::vector<std::vector<uint8_t>> app;
    void flush() {
        for (auto& b : lobby.take_sends()) link.send(std::move(b), true);
        SessionKeys k;
        if (lobby.take_keys(&k)) link.set_cipher(k);
    }
    void poll() {
        if (link.connected() && !was_up) {
            link.clear_cipher();
            lobby.on_connected(0);
        }
        was_up = link.connected();
        for (auto& raw : link.receive()) {
            flush();
            std::vector<uint8_t> m;
            if (link.open(raw, &m) != Cipher::Open::Ok) continue;
            const uint8_t t = message_type(m.data(), m.size());
            uint8_t point[kPakePoint], tag[kPakeTag];
            Hello h;
            if (t == (uint8_t)Msg::Hello && decode_hello(m.data(), m.size(), &h)) lobby.on_hello(h);
            else if (t == (uint8_t)Msg::Accept && decode_accept(m.data(), m.size())) lobby.on_accept();
            else if (t == (uint8_t)Msg::Pake1 && decode_pake(m.data(), m.size(), Msg::Pake1, point, tag)) lobby.on_pake1(point);
            else if (t == (uint8_t)Msg::Pake2 && decode_pake(m.data(), m.size(), Msg::Pake2, point, tag)) lobby.on_pake2(point, tag);
            else if (t == (uint8_t)Msg::Pake3 && decode_pake(m.data(), m.size(), Msg::Pake3, point, tag)) lobby.on_pake3(tag);
            else if (lobby.ready() && !handshake_message(t)) app.push_back(m);
        }
        flush();
        if (lobby.ready() && !was_ready && lobby.hosting()) link.send(encode_accept(), true);
        was_ready = lobby.ready();
    }
};

// A password game over a relay-like transport: the handshake finishes, then game messages cross sealed and the transport never sees the password or their text.
static void test_sealed_session() {
    FakeEnd a, b;
    a.other = &b;
    b.other = &a;
    const rs64_mp_transport ta = fake_table(&a), tb = fake_table(&b);
    Side host, client;
    LinkConfig hc{Role::Host};
    hc.external = &ta;
    LinkConfig cc{Role::Client};
    cc.external = &tb;
    cc.peer = "123456";
    host.lobby.set_password("424242");
    host.lobby.host();
    client.lobby.set_password("424242");
    client.lobby.join();
    g_wire.clear();
    CHECK(host.link.start(hc) && client.link.start(cc));
    for (int i = 0; i < 10; ++i) {
        host.poll();
        client.poll();
    }
    CHECK(host.lobby.ready() && client.lobby.ready());
    client.link.send(encode_launch("SECRET BRIEFING"), true);
    host.link.send(encode_launch("HOST SECRET"), false);
    for (int i = 0; i < 3; ++i) {
        host.poll();
        client.poll();
    }
    std::string text;
    CHECK(host.app.size() == 1 && decode_launch(host.app[0].data(), host.app[0].size(), &text) && text == "SECRET BRIEFING");
    CHECK(client.app.size() == 1 && decode_launch(client.app[0].data(), client.app[0].size(), &text) && text == "HOST SECRET");
    CHECK(!wire_has("424242") && !wire_has("SECRET"));

    // A plaintext message injected after sealing is refused.
    a.inbox.push_back(encode_launch("INJECTED"));
    host.poll();
    CHECK(host.app.size() == 1);
    host.link.stop();
    client.link.stop();
}

struct Seen {
    bool last = false;
    int distinct = 0;
    int prev = -1;
    bool ordered = true;
};

static StatePacket state_at(uint32_t f) {
    StatePacket s;
    s.seq = f;
    s.pos[0] = (float)f;
    return s;
}

static void drain(Link& link, Seen& seen, std::string* launch) {
    for (const auto& m : link.receive()) {
        const uint8_t type = message_type(m.data(), m.size());
        if (type == (uint8_t)Msg::State) {
            StatePacket p;
            if (decode_state(m.data(), m.size(), &p)) {
                const int s = (int)p.seq;
                if (s != seen.prev) {
                    ++seen.distinct;
                }
                if (s < seen.prev) {
                    seen.ordered = false;
                }
                seen.prev = s;
                if (s == 299 && p.pos[0] == 299.0f) {
                    seen.last = true;
                }
            }
        } else if (launch && type == (uint8_t)Msg::Launch) {
            decode_launch(m.data(), m.size(), launch);
        }
    }
}

// A kick lands after what was sent before it, and frees the host's one slot for the next player.
static void test_kick() {
    const auto until = [](auto done) {
        const auto t0 = std::chrono::steady_clock::now();
        while (!done() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(8)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return done();
    };
    Link host, first, second;
    CHECK(host.start(LinkConfig{Role::Host, "127.0.0.1", 27165}));
    CHECK(first.start(LinkConfig{Role::Client, "127.0.0.1", 27165}));
    CHECK(until([&] { return host.connected() && first.connected(); }));
    host.send(encode_accept(), true);
    host.kick();
    CHECK(until([&] { return host.lost(); }));
    const auto got = first.receive();
    CHECK(got.size() == 1 && decode_accept(got[0].data(), got[0].size()));
    first.stop();
    CHECK(second.start(LinkConfig{Role::Client, "127.0.0.1", 27165}));
    CHECK(until([&] { return host.connected() && second.connected(); }));
    host.stop();
    second.stop();
}

int main() {
#ifdef _WIN32
    // Report failed checks on stderr instead of the Debug CRT's modal dialog.
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    test_external_transport();
    test_sealed_session();
    test_kick();
    using clock = std::chrono::steady_clock;
    constexpr int kLossPct = 20;
    Link host;
    Link client;
    // The client starts first and must keep retrying until the host listens.
    const bool client_ok = client.start(LinkConfig{Role::Client, "127.0.0.1", 27164, 30, kLossPct});
    CHECK(client_ok);
    std::this_thread::sleep_for(std::chrono::seconds(4));
    const bool host_ok = host.start(LinkConfig{Role::Host, "127.0.0.1", 27164, 30, kLossPct});
    CHECK(host_ok);
    const auto t0 = clock::now();
    while (!(host.connected() && client.connected()) && clock::now() - t0 < std::chrono::seconds(10)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(host.connected() && client.connected());
    host.send(encode_launch("hello launch"), true);
    std::string launch;
    Seen host_seen;
    Seen client_seen;
    // 300 unreliable STATE sends per side over a lossy link; the last one is resent until it lands.
    for (uint32_t f = 0; f < 300 || !(host_seen.last && client_seen.last); ++f) {
        const uint32_t s = f < 299 ? f : 299;
        host.send(encode_state(state_at(s)), false);
        client.send(encode_state(state_at(s)), false);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        drain(host, host_seen, nullptr);
        drain(client, client_seen, &launch);
        CHECK(f < 1000);
    }
    CHECK(host_seen.last && client_seen.last);
    // Sequenced unreliable: no reordering (only the resent last packet may repeat), and well over half of the non-dropped sends arrive.
    const int min_arrived = 300 * (100 - kLossPct) / 100 / 2;
    CHECK(host_seen.distinct >= min_arrived && client_seen.distinct >= min_arrived);
    CHECK(host_seen.ordered && client_seen.ordered);
    CHECK(launch == "hello launch");
    client.stop();
    const auto t = clock::now();
    while (!host.lost() && clock::now() - t < std::chrono::seconds(15)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(host.lost());
    const uint32_t connects_before = host.connect_count();
    // A stopped client restarts cleanly: first against a port nobody listens on, then back to the host, which is still listening and takes the new player.
    const bool dead_ok = client.start(LinkConfig{Role::Client, "127.0.0.1", 27165, 0, 0});
    CHECK(dead_ok);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    CHECK(!client.connected());
    client.stop();
    const bool again_ok = client.start(LinkConfig{Role::Client, "127.0.0.1", 27164, 0, 0});
    CHECK(again_ok);
    CHECK(client.receive().empty());
    const auto t2 = clock::now();
    while (!(host.connected() && client.connected()) && clock::now() - t2 < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(host.connected() && client.connected() && !host.lost() && !client.lost());
    CHECK(host.connect_count() == connects_before + 1);
    // flush() returns once the queued BYE is on the wire, well inside its cap, and the peer gets it.
    {
        host.send(encode_bye(ByeReason::Quit, 7), true);
        const auto tf = clock::now();
        CHECK(host.flush(200));
        CHECK(clock::now() - tf < std::chrono::milliseconds(200));
        bool bye = false;
        const auto tb = clock::now();
        while (!bye && clock::now() - tb < std::chrono::seconds(1)) {
            for (const auto& m : client.receive()) {
                bye = bye || message_type(m.data(), m.size()) == (uint8_t)Msg::Bye;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(bye);
    }
    client.stop();
    host.stop();
    // A host bound to one IPv4 takes clients on that address only.
    {
        Link bh;
        Link bc;
        LinkConfig hc{Role::Host, "", 27167, 0, 0};
        hc.bind_v4 = 0x7F000001u;
        CHECK(bh.start(hc));
        CHECK(bc.start(LinkConfig{Role::Client, "127.0.0.1", 27167, 0, 0}));
        const auto tb = clock::now();
        while (!(bh.connected() && bc.connected()) && clock::now() - tb < std::chrono::seconds(5)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(bh.connected() && bc.connected() && !bh.lost());
        bc.stop();
        bh.stop();
        const std::optional<uint32_t> lan = lan_ipv4();
        if (lan && (*lan >> 24) != 127u) {
            hc.bind_v4 = *lan;
            CHECK(bh.start(hc));
            CHECK(bc.start(LinkConfig{Role::Client, "127.0.0.1", 27167, 0, 0}));
            std::this_thread::sleep_for(std::chrono::seconds(1));
            CHECK(!bh.connected() && !bc.connected() && !bh.lost());
            bc.stop();
            bh.stop();
        }
    }
    CHECK(!pick_lan_ipv4({}).has_value());
    CHECK(!pick_lan_ipv4({0x7F000001u}).has_value());
    CHECK(pick_lan_ipv4({0x7F000001u, 0x08080808u, 0xC0A80114u}).value() == 0xC0A80114u);
    CHECK(pick_lan_ipv4({0x7F000001u, 0x08080808u}).value() == 0x08080808u);
    CHECK(pick_lan_ipv4({0xAC1F0002u, 0x0A000005u}).value() == 0x0A000005u);
    CHECK(!pick_lan_ipv4({0xA9FE8283u}).has_value());
    CHECK(pick_lan_ipv4({0xA9FE8283u, 0x0A0000BDu}).value() == 0x0A0000BDu);
    // Simulated jitter delays each send by a random 0..jitter ms but keeps the order.
    {
        Link jh;
        Link jc;
        LinkConfig hc{Role::Host, "127.0.0.1", 27166, 0, 0};
        hc.jitter_ms = 60;
        CHECK(jh.start(hc));
        CHECK(jc.start(LinkConfig{Role::Client, "127.0.0.1", 27166, 0, 0}));
        const auto tj = clock::now();
        while (!(jh.connected() && jc.connected()) && clock::now() - tj < std::chrono::seconds(5)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(jh.connected() && jc.connected());
        for (int i = 0; i < 30; ++i) {
            jh.send(encode_launch("m" + std::to_string(i)), true);
        }
        std::vector<std::string> got;
        const auto tr = clock::now();
        while (got.size() < 30 && clock::now() - tr < std::chrono::seconds(3)) {
            for (const auto& m : jc.receive()) {
                std::string text;
                if (decode_launch(m.data(), m.size(), &text)) {
                    got.push_back(text);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(got.size() == 30);
        for (int i = 0; i < 30; ++i) {
            CHECK(got[i] == "m" + std::to_string(i));
        }
        jc.stop();
        jh.stop();
    }
    // Discovery targets: the limited broadcast plus each LAN address's /24 broadcast, never loopback or link-local, no repeats.
    {
        const std::vector<uint32_t> t = discovery_targets({0x7F000001u, 0x0A0000BDu, 0xA9FE8283u, 0x0A0000C8u, 0xC0A83801u});
        CHECK(t.size() == 3);
        CHECK(t[0] == 0xFFFFFFFFu && t[1] == 0x0A0000FFu && t[2] == 0xC0A838FFu);
    }
    // A finder aimed at loopback finds a responder and reads its game port and pilot name.
    {
        DiscoveryReply info;
        info.port = 27170;
        info.name[0] = 'R';
        info.name[1] = 'S';
        info.name[2] = '6';
        DiscoveryResponder responder;
        CHECK(responder.start(27170, info));
        DiscoveryFinder finder;
        CHECK(finder.start(27170, {0x7F000001u}));
        std::vector<FoundHost> found;
        const auto td = clock::now();
        while (found.empty() && clock::now() - td < std::chrono::seconds(3)) {
            finder.query();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            responder.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            found = finder.poll();
        }
        CHECK(!found.empty());
        printf("found %s port %u name %.3s\n", found[0].addr.c_str(), found[0].info.port, found[0].info.name);
        CHECK(found[0].addr == "127.0.0.1" && found[0].info.port == 27170 && found[0].info.name[1] == 'S');
        finder.stop();
        responder.stop();
    }
    // UPnP: whatever the network has, the mapper settles within the discovery timeout, and stop() works mid-search and after.
    {
        PortMapper m;
        CHECK(m.state() == PortMapState::Idle);
        // stop() runs on the game thread: it must not wait for a router search or request in progress.
        m.start(27180);
        const auto ts = clock::now();
        m.stop();
        const auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - ts).count();
        printf("upnp stop mid-search: %lld ms\n", (long long)stop_ms);
        CHECK(stop_ms < 100);
        CHECK(m.state() == PortMapState::Idle);
        m.start(27180);
        m.stop();
        m.start(27180);
        const auto tu = clock::now();
        while (m.state() == PortMapState::Working && clock::now() - tu < std::chrono::seconds(15)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        printf("upnp: state %d %s\n", (int)m.state(), m.external().c_str());
        CHECK(m.state() != PortMapState::Working);
        m.stop();
        CHECK(m.state() == PortMapState::Idle);
        const auto tw = clock::now();
        while (!m.settled() && clock::now() - tw < std::chrono::seconds(10)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(m.settled());
    }
    const std::string shown = lan_address_text();
    printf("local IPv4 addresses: %zu, shown: %s\n", local_ipv4s().size(), shown.c_str());
    if (!local_ipv4s().empty()) CHECK(!shown.empty());
    printf("net_link_test OK\n");
    return 0;
}
