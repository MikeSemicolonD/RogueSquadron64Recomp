#include "relay_core.h"
#include "relay_bridge.h"
#include <enet6/enet.h>
#include "check.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace rs64::relay;

namespace {

constexpr uint32_t kBuild = 0x52530008u;

// Packets the relay sent to one peer, decoded by type.
std::vector<Action> to(const std::vector<Action>& actions, uint32_t peer)
{

    std::vector<Action> out;

    for (const Action& a : actions) {
        if (a.peer == peer) out.push_back(a);
    }

    return out;
}

uint8_t type_of(const Action& a)
{
    return message_type(a.bytes.data(), a.bytes.size());
}

void recv(Relay& r, uint32_t peer, const std::vector<uint8_t>& b, int64_t now = 0, bool reliable = true)
{
    r.on_receive(peer, reliable, b.data(), b.size(), now);
}

uint32_t host_game(Relay& r, uint32_t peer, const std::string& name, bool locked = false)
{

    recv(r, peer, encode_register(kBuild, locked, name));

    const std::vector<Action> a = r.take_actions();
    uint32_t id = 0;
    std::string listed;

    CHECK(a.size() == 1 && a[0].peer == peer);
    CHECK(decode_registered(a[0].bytes.data(), a[0].bytes.size(), &id, &listed));
    return id;
}

// The name the server lists a new game under.
std::string host_name_listed(Relay& r, uint32_t peer, const std::string& name)
{

    recv(r, peer, encode_register(kBuild, false, name));

    const std::vector<Action> a = r.take_actions();
    uint32_t id = 0;
    std::string listed;

    CHECK(a.size() == 1 && decode_registered(a[0].bytes.data(), a[0].bytes.size(), &id, &listed));
    return listed;
}

ListReply list(Relay& r, uint32_t peer, uint32_t build = kBuild)
{

    recv(r, peer, encode_list(build));

    const std::vector<Action> a = r.take_actions();
    ListReply out;

    CHECK(a.size() == 1 && decode_list_reply(a[0].bytes.data(), a[0].bytes.size(), &out));
    return out;
}

void test_protocol_roundtrip()
{

    uint32_t build = 0;
    bool locked = false;
    std::string name;
    const std::vector<uint8_t> reg = encode_register(kBuild, true, "MIKES GAME");

    CHECK(decode_register(reg.data(), reg.size(), &build, &locked, &name));
    CHECK(build == kBuild && locked && name == "MIKES GAME");
    CHECK(!decode_register(reg.data(), reg.size() - 1, &build, &locked, &name));

    std::vector<uint8_t> bad = reg;

    bad[0] = kVersion + 1;

    bool version_ok = true;

    CHECK(message_type(bad.data(), bad.size(), &version_ok) == 0 && !version_ok);

    ListReply lr;

    lr.server = "TEST";
    lr.total = 70;

    for (uint32_t i = 0; i < 60; ++i) lr.games.push_back(GameEntry{i + 1, i % 2 == 0, "GAME " + std::to_string(i)});

    const std::vector<uint8_t> lb = encode_list_reply(lr);
    ListReply back;

    CHECK(lb.size() <= kMaxPacket);
    CHECK(decode_list_reply(lb.data(), lb.size(), &back));
    CHECK(back.server == "TEST" && back.total == 70 && back.games.size() == kListMax);
    CHECK(back.games[3].id == 4 && !back.games[3].locked && back.games[3].name == "GAME 3");

    // Names from a server are cleaned on the way in, whatever it sent.
    ListReply dirty;

    dirty.games.push_back(GameEntry{1, false, "a~\x01z"});
    dirty.games.push_back(GameEntry{2, false, "~~~"});

    const std::vector<uint8_t> db = encode_list_reply(dirty);

    CHECK(decode_list_reply(db.data(), db.size(), &back) && back.games[0].name == "AZ" && back.games[1].name == "GAME");

    const std::vector<uint8_t> rb = encode_registered(9, "x\x7F" "y");
    uint32_t reg_id = 0;
    std::string reg_name;

    CHECK(decode_registered(rb.data(), rb.size(), &reg_id, &reg_name) && reg_name == "XY");

    const uint8_t payload[3] = {1, 2, 3};
    const std::vector<uint8_t> d = encode_data(payload, 3);
    const uint8_t* bytes = nullptr;
    size_t len = 0;

    CHECK(decode_data(d.data(), d.size(), &bytes, &len) && len == 3 && bytes[2] == 3);

    const std::vector<uint8_t> big(kMaxGamePacket + 1, 0);
    const std::vector<uint8_t> too_big = encode_data(big.data(), big.size());

    CHECK(!decode_data(too_big.data(), too_big.size(), &bytes, &len));
}

void test_sanitize()
{
    CHECK(sanitize_name("  mike's  game!! ", "X") == "MIKES GAME");
    CHECK(sanitize_name("@@@", "GAME 1") == "GAME 1");
    CHECK(sanitize_name("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "") == "ABCDEFGHIJKLMNOP");
}

void test_duplicate_names()
{

    Relay r(Settings{}, "unused.cfg");

    for (uint32_t p = 1; p <= 4; ++p) r.on_connect(p, "10.0.0." + std::to_string(p), 0);

    CHECK(host_name_listed(r, 1, "aaa") == "AAA");
    CHECK(host_name_listed(r, 2, "AAA") == "AAA 2");
    CHECK(host_name_listed(r, 3, "aaa") == "AAA 3");
    CHECK(host_name_listed(r, 4, "ABCDEFGHIJKLMNOP") == "ABCDEFGHIJKLMNOP");

    // A rename onto a listed name is numbered too, and the host is told; keeping its own name is not a clash.
    recv(r, 4, encode_update(false, "AAA"));

    std::vector<Action> a = r.take_actions();
    uint32_t id = 0;
    std::string listed;

    CHECK(a.size() == 1 && decode_registered(a[0].bytes.data(), a[0].bytes.size(), &id, &listed) && listed == "AAA 4");

    recv(r, 4, encode_update(true, "AAA 4"));
    a = r.take_actions();
    CHECK(a.size() == 1 && decode_registered(a[0].bytes.data(), a[0].bytes.size(), &id, &listed) && listed == "AAA 4");
}

void test_register_list_join_forward_leave()
{

    Relay r(Settings{}, "unused.cfg");

    r.on_connect(1, "10.0.0.1", 0);
    r.on_connect(2, "10.0.0.2", 0);
    r.on_connect(3, "10.0.0.3", 0);

    const uint32_t id = host_game(r, 1, "first", true);
    ListReply lr = list(r, 2);

    CHECK(lr.total == 1 && lr.games.size() == 1 && lr.games[0].id == id && lr.games[0].locked && lr.games[0].name == "FIRST");
    CHECK(list(r, 2, kBuild + 1).total == 0);

    recv(r, 2, encode_join(id, kBuild + 1));

    std::vector<Action> a = r.take_actions();
    Type rq = Type::Data;
    Reason why = Reason::None;

    CHECK(a.size() == 1 && decode_refused(a[0].bytes.data(), a[0].bytes.size(), &rq, &why) && rq == Type::Join && why == Reason::BuildMismatch);

    recv(r, 2, encode_join(id, kBuild));
    a = r.take_actions();
    CHECK(to(a, 2).size() == 1 && type_of(to(a, 2)[0]) == (uint8_t)Type::Joined);
    CHECK(to(a, 1).size() == 1 && type_of(to(a, 1)[0]) == (uint8_t)Type::PeerJoined);
    CHECK(list(r, 3).total == 0);

    recv(r, 3, encode_join(id, kBuild));
    a = r.take_actions();
    CHECK(a.size() == 1 && decode_refused(a[0].bytes.data(), a[0].bytes.size(), &rq, &why) && why == Reason::GameFull);

    const uint8_t payload[2] = {9, 8};
    const std::vector<uint8_t> d = encode_data(payload, 2);

    recv(r, 1, d, 0, false);
    a = r.take_actions();
    CHECK(a.size() == 1 && a[0].peer == 2 && !a[0].reliable && a[0].bytes == d);

    recv(r, 2, d, 0, true);
    a = r.take_actions();
    CHECK(a.size() == 1 && a[0].peer == 1 && a[0].reliable);

    r.on_disconnect(2);
    a = r.take_actions();
    CHECK(a.size() == 1 && a[0].peer == 1 && type_of(a[0]) == (uint8_t)Type::PeerLeft);
    CHECK(list(r, 3).total == 1);

    recv(r, 3, encode_join(id, kBuild));
    r.take_actions();
    r.on_disconnect(1);
    a = r.take_actions();
    CHECK(a.size() == 1 && a[0].peer == 3 && type_of(a[0]) == (uint8_t)Type::PeerLeft);
    CHECK(r.game_count() == 0);
    CHECK(list(r, 3).total == 0);
}

void test_limits()
{

    Settings s;

    s.max_clients = 3;
    s.max_per_ip = 2;
    s.max_games = 1;
    s.rate = 10;
    s.idle_timeout = 5;

    Relay r(s, "unused.cfg");

    r.on_connect(1, "1.1.1.1", 0);
    r.on_connect(2, "1.1.1.1", 0);
    r.on_connect(3, "1.1.1.1", 0);

    std::vector<Action> a = r.take_actions();

    CHECK(a.size() == 1 && a[0].peer == 3 && a[0].kind == Action::Kind::Disconnect && a[0].reason == Reason::TooManyFromAddress);

    r.on_connect(4, "2.2.2.2", 0);
    r.on_connect(5, "3.3.3.3", 0);
    a = r.take_actions();
    CHECK(a.size() == 1 && a[0].peer == 5 && a[0].reason == Reason::ServerFull);
    CHECK(r.client_count() == 3);

    host_game(r, 1, "ONE");
    recv(r, 4, encode_register(kBuild, false, "TWO"));
    a = r.take_actions();

    Type rq = Type::Data;
    Reason why = Reason::None;

    CHECK(a.size() == 1 && decode_refused(a[0].bytes.data(), a[0].bytes.size(), &rq, &why) && why == Reason::GamesFull);

    for (int i = 0; i < 9; ++i) recv(r, 4, encode_list(kBuild), 100);

    r.take_actions();
    recv(r, 4, encode_list(kBuild), 100);
    a = r.take_actions();
    CHECK(a.size() == 1 && a[0].reason == Reason::RateLimited && r.client_count() == 2);

    r.tick(4999);
    CHECK(r.take_actions().empty());

    r.tick(5100);
    a = r.take_actions();
    CHECK(a.size() == 1 && a[0].peer == 2 && a[0].reason == Reason::Idle);
    CHECK(r.client_count() == 1);

    const std::vector<uint8_t> junk = {kVersion, 77};

    recv(r, 1, junk);
    a = r.take_actions();
    CHECK(a.size() == 1 && a[0].reason == Reason::BadRequest && r.game_count() == 0);
}

void test_host_kick()
{

    Relay r(Settings{}, "unused.cfg");

    r.on_connect(1, "10.0.0.1", 0);
    r.on_connect(2, "10.0.0.2", 0);

    const uint32_t id = host_game(r, 1, "OPEN");

    recv(r, 2, encode_join(id, kBuild));
    r.take_actions();

    // Only the host can remove the joiner.
    recv(r, 2, encode_kick());
    CHECK(r.take_actions().empty() && r.client_count() == 2);

    recv(r, 1, encode_kick());

    const std::vector<Action> a = r.take_actions();

    CHECK(to(a, 2).size() == 1 && to(a, 2)[0].kind == Action::Kind::Disconnect && to(a, 2)[0].reason == Reason::Kicked);
    CHECK(to(a, 1).size() == 1 && type_of(to(a, 1)[0]) == (uint8_t)Type::PeerLeft);
    CHECK(r.client_count() == 1 && r.game_count() == 1 && list(r, 1).total == 1);
}

void test_address_groups()
{

    Settings s;

    s.max_per_ip = 1;

    Relay r(s, "unused.cfg");

    r.on_connect(1, "2001:db8::1", 0, "2001:db8::/64");
    r.on_connect(2, "2001:db8::2", 0, "2001:db8::/64");

    std::vector<Action> a = r.take_actions();

    CHECK(a.size() == 1 && a[0].peer == 2 && a[0].reason == Reason::TooManyFromAddress);

    Relay g(Settings{}, "unused.cfg");

    for (uint32_t p = 1; p <= 3; ++p) g.on_connect(p, "10.0.0.1", 0);

    host_game(g, 1, "ONE");
    host_game(g, 2, "TWO");
    recv(g, 3, encode_register(kBuild, false, "THREE"));
    a = g.take_actions();

    Type rq = Type::Data;
    Reason why = Reason::None;

    CHECK(a.size() == 1 && decode_refused(a[0].bytes.data(), a[0].bytes.size(), &rq, &why) && why == Reason::TooManyFromAddress);
}

void test_console()
{

    const std::filesystem::path cfg = std::filesystem::temp_directory_path() / "rs64_relay_core_test.cfg";

    std::filesystem::remove(cfg);

    Relay r(Settings{}, cfg.string());

    CHECK(r.command("set max_clients 100") == (std::vector<std::string>{"max_clients = 100", "Use 'save' to keep the current configuration"}));
    CHECK(r.command("set rate 200") == (std::vector<std::string>{"rate = 200"}));
    CHECK(r.settings().max_clients == 100 && r.settings().rate == 200);
    CHECK(r.command("set rate 1").size() == 1 && r.settings().rate == 200);
    CHECK(r.command("set port 1").size() == 1 && r.settings().port == kDefaultPort);
    CHECK(r.command("reload") == (std::vector<std::string>{"no config file at " + cfg.string()}));
    CHECK(r.command("save") == (std::vector<std::string>{"saved " + cfg.string()}));

    Settings loaded;
    std::vector<std::string> warnings;

    CHECK(load_config(cfg.string(), &loaded, &warnings) && warnings.empty());
    CHECK(loaded.max_clients == 100 && loaded.rate == 200 && loaded.name == "RS64 RELAY");

    std::ofstream(cfg, std::ios::app) << "port = 27080\nbogus = 1\n";
    r.command("set rate 300");

    const std::vector<std::string> out = r.command("reload");

    CHECK(out.size() == 3 && r.settings().rate == 200 && r.settings().port == 27080);

    r.on_connect(1, "1.1.1.1", 0);
    r.on_connect(2, "2.2.2.2", 0);

    const uint32_t id = host_game(r, 1, "KICK ME");

    recv(r, 2, encode_join(id, kBuild));
    r.take_actions();
    CHECK(r.command("games").size() == 2);
    CHECK(r.command("kick 99") == (std::vector<std::string>{"no game 99"}));
    CHECK(r.command("kick " + std::to_string(id)).empty());

    std::vector<Action> a = r.take_actions();
    int kicked = 0;

    for (const Action& x : a) kicked += x.kind == Action::Kind::Disconnect && x.reason == Reason::Kicked;

    CHECK(kicked == 2 && r.game_count() == 0 && r.client_count() == 0);

    r.on_connect(3, "3.3.3.3", 0);
    CHECK(r.command("quit") == (std::vector<std::string>{"stopping"}) && r.quitting());

    a = r.take_actions();
    CHECK(a.size() == 1 && a[0].reason == Reason::ServerQuit);
    CHECK(r.command("nope").size() == 1);
    std::filesystem::remove(cfg);
}

void test_args_and_config_order()
{

    const std::filesystem::path cfg = std::filesystem::temp_directory_path() / "rs64_relay_args_test.cfg";

    std::ofstream(cfg) << "# comment\nmax_games = 7\nrate = 50   # trailing\nname = from file\nmax_clients = nope\n";

    const std::string path = cfg.string();
    const char* argv[] = {"rs64_relay", "--config", path.c_str(), "--rate", "90", "--max-per-ip=8"};
    const Options o = parse_args(6, argv);

    CHECK(o.config_found && o.errors.empty() && o.warnings.size() == 1);
    CHECK(o.settings.max_games == 7 && o.settings.rate == 90 && o.settings.max_per_ip == 8 && o.settings.name == "FROM FILE");

    const char* bad[] = {"rs64_relay", "--config", path.c_str(), "--rate", "5", "--wat", "1", "stray"};

    CHECK(parse_args(8, bad).errors.size() == 3);

    const char* help[] = {"rs64_relay", "--help"};

    CHECK(parse_args(2, help).help && !usage().empty());

    const char* browser[] = {"rs64_relay", "--config", path.c_str(), "--browser-port", "0"};
    const char* browser_bad[] = {"rs64_relay", "--config", path.c_str(), "--browser-port", "70000"};
    Settings defaults;

    CHECK(defaults.browser_port == kDefaultPort + 1);
    CHECK(parse_args(5, browser).settings.browser_port == 0);
    CHECK(parse_args(5, browser_bad).errors.size() == 1);
    CHECK(config_text(defaults).find("browser_port = 27071") != std::string::npos);
    std::filesystem::remove(cfg);
}

// A browser session's datagrams reach the ENet port from the session's own loopback socket, and replies to that socket come back to the session.
void test_bridge()
{

    CHECK(enet_initialize() == 0);

    ENetAddress any;

    enet_address_build_loopback(&any, ENET_ADDRESS_TYPE_IPV6);
    any.port = 0;

    const ENetSocket enet_port = enet_socket_create(ENET_ADDRESS_TYPE_IPV6, ENET_SOCKET_TYPE_DATAGRAM);

    CHECK(enet_socket_bind(enet_port, &any) == 0);

    ENetAddress bound;

    CHECK(enet_socket_get_address(enet_port, &bound) == 0 && bound.port != 0);

    std::vector<std::vector<uint8_t>> to_browser;
    Bridge bridge(bound.port);
    const uint32_t id = bridge.open("203.0.113.7", [&](const uint8_t* d, size_t n) { to_browser.emplace_back(d, d + n); });

    CHECK(id != 0 && bridge.sessions() == 1);

    const uint8_t hello[] = { 'h', 'i' };

    bridge.from_browser(id, hello, sizeof hello);

    uint8_t buf[64];
    ENetBuffer b;
    ENetAddress from;
    ENetSocketSet set;

    b.data = buf;
    b.dataLength = sizeof buf;
    ENET_SOCKETSET_EMPTY(set);
    ENET_SOCKETSET_ADD(set, enet_port);
    CHECK(enet_socketset_select(enet_port, &set, nullptr, 1000) > 0);
    CHECK(enet_socket_receive(enet_port, &from, &b, 1) == 2 && buf[0] == 'h');
    CHECK(bridge.remote_of(from.port) == "203.0.113.7");
    CHECK(bridge.remote_of((uint16_t)(from.port + 1)).empty());

    const uint8_t reply[] = { 'o', 'k', '!' };
    ENetBuffer rb;

    rb.data = (void*)reply;
    rb.dataLength = sizeof reply;

    CHECK(enet_socket_send(enet_port, &from, &rb, 1) == 3);

    for (int i = 0; i < 20 && to_browser.empty(); ++i) bridge.pump(50);

    CHECK(to_browser.size() == 1 && to_browser[0].size() == 3 && to_browser[0][2] == '!');

    CHECK(address_group("203.0.113.7") == "203.0.113.7");
    CHECK(address_group("2001:db8::1:2") == "2001:0db8:0000:0000::/64");
    CHECK(address_group("::ffff:198.51.100.4") == "::ffff:198.51.100.4");

    bridge.from_browser(id + 99, hello, sizeof hello);
    bridge.close(id);
    CHECK(bridge.sessions() == 0 && bridge.remote_of(from.port).empty());

    enet_socket_destroy(enet_port);
    enet_deinitialize();
}

}

int main()
{

#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif

    test_protocol_roundtrip();
    test_sanitize();
    test_duplicate_names();
    test_register_list_join_forward_leave();
    test_limits();
    test_host_kick();
    test_address_groups();
    test_console();
    test_args_and_config_order();
    test_bridge();
    std::printf("relay_core_test: all passed\n");
    return 0;
}
