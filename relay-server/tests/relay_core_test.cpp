#include "relay_core.h"
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

    CHECK(a.size() == 1 && a[0].peer == peer);
    CHECK(decode_registered(a[0].bytes.data(), a[0].bytes.size(), &id));
    return id;
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
    std::filesystem::remove(cfg);
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
    test_register_list_join_forward_leave();
    test_limits();
    test_console();
    test_args_and_config_order();
    std::printf("relay_core_test: all passed\n");
    return 0;
}
