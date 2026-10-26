#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>
#include "relay_protocol.h"

namespace rs64::relay {

// The ENet host's peer slots: the ceiling for max_clients.
constexpr int kPeerCeiling = 4000;
constexpr const char* kDefaultConfig = "rs64_relay.cfg";

struct Settings {
    uint16_t port = kDefaultPort;
    std::string name = "RS64 RELAY";
    int max_clients = 64;
    int max_games = 32;
    int max_per_ip = 4;
    int rate = 120;
    int idle_timeout = 60;
};

struct LimitDef {
    const char* key;
    int Settings::*field;
    int lo;
    int hi;
    const char* what;
};

const std::vector<LimitDef>& limits();

// Sets one config key (port, name or a limit) from text; false with *error set when the key or value is bad.
bool set_value(Settings* s, const std::string& key, const std::string& value, std::string* error);
// Applies a config file's text over *s; returns one warning per skipped line.
std::vector<std::string> parse_config(const std::string& text, Settings* s);
std::string config_text(const Settings& s);
// False when the file does not exist (s untouched).
bool load_config(const std::string& path, Settings* s, std::vector<std::string>* warnings);
bool save_config(const std::string& path, const Settings& s);

struct Options {
    Settings settings;
    std::string config = kDefaultConfig;
    bool config_found = false;
    bool help = false;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

// Defaults, then the config file (--config or rs64_relay.cfg), then the flags.
Options parse_args(int argc, const char* const* argv);
std::string usage();

struct Action {
    enum class Kind { Send, Disconnect };
    Kind kind = Kind::Send;
    uint32_t peer = 0;
    bool reliable = true;
    std::vector<uint8_t> bytes;
    Reason reason = Reason::None;
};

// The relay without sockets: the caller feeds connection events and packets (peer ids from 1) and carries out the actions.
class Relay {
public:
    Relay(Settings s, std::string config_path);
    void on_connect(uint32_t peer, const std::string& ip, int64_t now_ms);
    void on_disconnect(uint32_t peer);
    void on_receive(uint32_t peer, bool reliable, const uint8_t* data, size_t n, int64_t now_ms);
    // Closes idle connections.
    void tick(int64_t now_ms);
    // One console line; returns the lines to print.
    std::vector<std::string> command(const std::string& line);
    std::vector<Action> take_actions();
    std::vector<std::string> take_log();
    bool quitting() const { return quitting_; }
    const Settings& settings() const { return s_; }
    size_t client_count() const { return clients_.size(); }
    size_t game_count() const { return games_.size(); }

private:
    struct Client {
        std::string ip;
        uint32_t game = 0;
        int64_t last_rx_ms = 0;
        int64_t window_ms = 0;
        int packets = 0;
    };

    struct Game {
        uint32_t build = 0;
        bool locked = false;
        std::string name;
        uint32_t host = 0;
        uint32_t guest = 0;
    };

    void send(uint32_t peer, std::vector<uint8_t> bytes, bool reliable = true);
    void drop(uint32_t peer, Reason reason);
    void leave_game(uint32_t peer);
    void handle(uint32_t peer, Client& c, bool reliable, const uint8_t* data, size_t n);
    void log(std::string line);
    std::vector<std::string> games_text() const;

    Settings s_;
    std::string config_path_;
    std::map<uint32_t, Client> clients_;
    std::map<uint32_t, Game> games_;
    std::mt19937 rng_{std::random_device{}()};
    bool told_save_ = false;
    bool quitting_ = false;
    std::vector<Action> actions_;
    std::vector<std::string> log_;
};

}
