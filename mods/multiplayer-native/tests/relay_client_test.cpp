// Loopback test for the relay server transport: an in-process rs64_relay (relay-server's Host + Relay) on a thread, RelayClients on this one.
#include "relay_client.h"
#include "relay_host.h"
#include "check.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace rs64::net;

namespace {

constexpr uint16_t kPort = 27181;
constexpr uint32_t kBuild = 0x52530000u;
const std::string kServer = "127.0.0.1:" + std::to_string(kPort);

template <class F>
bool wait_for(F f, int ms = 4000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        if (f()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

std::vector<std::string> drain(RelayClient& c) {
    std::vector<std::string> out;
    c.receive([](const uint8_t* b, uint32_t n, void* u) { ((std::vector<std::string>*)u)->emplace_back((const char*)b, n); }, &out);
    return out;
}

bool got(RelayClient& c, const std::string& want) {
    return wait_for([&] {
        for (const std::string& m : drain(c)) {
            if (m == want) {
                return true;
            }
        }
        return false;
    });
}

void send(RelayClient& c, const std::string& s, bool reliable) {
    c.send((const uint8_t*)s.data(), (uint32_t)s.size(), reliable);
}

}

int main() {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif

    rs64::relay::Settings settings;
    settings.port = kPort;
    rs64::relay::Relay relay(settings, "relay_client_test.cfg");
    std::atomic<bool> quit{false};
    std::atomic<bool> ready{false};
    std::thread server([&] {
        rs64::relay::Host host;
        CHECK(host.open(kPort));
        ready = true;
        while (!quit) {
            host.service(relay, 5);
            relay.tick(rs64::relay::now_ms());
            host.apply(relay);
        }
        relay.command("quit");
        host.apply(relay);
        host.close();
    });
    CHECK(wait_for([&] { return ready.load(); }));

    // Host: opens a game and gets its 6-digit join code.
    RelayClient a;
    a.set_server(kServer);
    a.set_info("ROGUE ONE", false);
    CHECK(a.start(RS64_MP_ROLE_HOST, "", kBuild));
    CHECK(wait_for([&] { a.connected(); return std::strlen(a.host_label()) > 0; }));
    const std::string code = std::string(a.host_label()).substr(std::strlen("JOIN CODE "));
    CHECK(CodeEditor::valid(code));

    // The list shows it, by name, with that code; another build sees nothing.
    RelayClient b;
    b.set_server(kServer);
    b.refresh(kBuild);
    CHECK(b.game_count() == -1);
    CHECK(wait_for([&] { return b.game_count() >= 0; }));
    CHECK(b.game_count() == 1 && b.game(0)->name == "ROGUE ONE" && !b.game(0)->locked && b.game_peer(0) == code);
    b.refresh(kBuild + 1);
    CHECK(wait_for([&] { return b.game_count() >= 0; }) && b.game_count() == 0);

    // A locked flag change reaches the list.
    a.set_info("ROGUE ONE", true);
    CHECK(wait_for([&] {
        a.connected();
        b.refresh(kBuild);
        return wait_for([&] { return b.game_count() >= 0; }) && b.game_count() == 1 && b.game(0)->locked;
    }));

    // Join by code: both sides connect, messages go both ways.
    CHECK(b.start(RS64_MP_ROLE_CLIENT, code, kBuild));
    // Both pump on every poll, as the lobby's per-frame checks do.
    CHECK(wait_for([&] {
        const bool ha = a.connected();
        const bool hb = b.connected();
        return ha && hb;
    }));
    CHECK(a.connect_count() == 1 && b.connect_count() == 1);
    send(a, "hello from host", true);
    CHECK(got(b, "hello from host"));
    send(b, "hello from client", true);
    CHECK(got(a, "hello from client"));
    // Unreliable traffic arrives too (sent a few times: loopback rarely drops, but may).
    CHECK(wait_for([&] {
        send(b, "state", false);
        return got(a, "state");
    }));

    // A full game is off the list; a third player is refused.
    RelayClient c;
    c.set_server(kServer);
    c.refresh(kBuild);
    CHECK(wait_for([&] { return c.game_count() >= 0; }) && c.game_count() == 0);
    CHECK(c.start(RS64_MP_ROLE_CLIENT, code, kBuild));
    CHECK(wait_for([&] { c.connected(); return c.lost(); }));
    CHECK(std::string(c.status()) == "GAME FULL");

    // The client leaves: the host sees it go and its game is listed again.
    b.stop();
    CHECK(wait_for([&] { a.connected(); return a.lost(); }));
    CHECK(!a.connected());
    c.refresh(kBuild);
    CHECK(wait_for([&] { return c.game_count() >= 0; }) && c.game_count() == 1);

    // Wrong code, wrong build.
    CHECK(c.start(RS64_MP_ROLE_CLIENT, code == "999999" ? "999998" : "999999", kBuild));
    CHECK(wait_for([&] { c.connected(); return c.lost(); }));
    CHECK(std::string(c.status()) == "NO GAME WITH THAT CODE");
    CHECK(c.start(RS64_MP_ROLE_CLIENT, code, kBuild + 1));
    CHECK(wait_for([&] { c.connected(); return c.lost(); }));
    CHECK(std::string(c.status()) == "VERSION MISMATCH");
    CHECK(!c.start(RS64_MP_ROLE_CLIENT, "12", kBuild));

    // A server that does not answer: available() turns false after the background check, a join fails with the reason, and a host ends lost and unavailable (the lobby then hosts directly).
    const std::string nowhere = "127.0.0.1:" + std::to_string(kPort + 1);
    RelayClient d;
    d.set_server(nowhere);
    CHECK(d.available());
    CHECK(wait_for([&] { return !d.available(); }, 9000));
    CHECK(d.start(RS64_MP_ROLE_CLIENT, "123456", kBuild));
    CHECK(wait_for([&] { d.connected(); return d.lost(); }, 9000));
    CHECK(std::string(d.status()) == "COULD NOT CONNECT TO SERVER");
    RelayClient e;
    e.set_server(nowhere);
    CHECK(e.start(RS64_MP_ROLE_HOST, "", kBuild));
    CHECK(wait_for([&] { e.connected(); return e.lost(); }, 9000));
    CHECK(!e.available() && std::string(e.status()) == "COULD NOT CONNECT TO SERVER");
    d.stop();
    e.stop();
    // A reachable server checks as up.
    RelayClient f;
    f.set_server(kServer);
    CHECK(wait_for([&] { return f.available(); }) && f.available());

    // The transport table forwards to the client.
    const rs64_mp_transport* t = c.table();
    CHECK(RS64_MP_TRANSPORT_HAS_LIST(t) && std::strcmp(t->name, "server") == 0 && t->available(t->user));

    // The server shuts down: the host is told why.
    quit = true;
    CHECK(wait_for([&] { a.connected(); return std::string(a.status()) == "RELAY SERVER CLOSED"; }));
    CHECK(a.lost());
    server.join();
    a.stop();
    c.stop();

    std::printf("relay_client_test OK\n");
    return 0;
}
