#include "relay_core.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>

namespace rs64::relay {

namespace {

constexpr const char* kSaveNote = "Use 'save' to keep the current configuration";

std::string trim(const std::string& s)
{

    const size_t a = s.find_first_not_of(" \t\r\n");

    if (a == std::string::npos)
    {
        return "";
    }

    const size_t b = s.find_last_not_of(" \t\r\n");

    return s.substr(a, b - a + 1);
}

bool parse_int(const std::string& s, int* out)
{

    const std::string t = trim(s);
    int v = 0;
    const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), v);

    if (t.empty() || ec != std::errc() || end != t.data() + t.size())
    {
        return false;
    }

    *out = v;
    return true;
}

std::string flag_name(std::string key)
{

    std::replace(key.begin(), key.end(), '_', '-');
    return "--" + key;
}

std::vector<std::string> words(const std::string& line)
{

    std::istringstream in(line);
    std::vector<std::string> out;
    std::string w;

    while (in >> w) out.push_back(w);

    return out;
}

}

const std::vector<LimitDef>& limits()
{

    static const std::vector<LimitDef> s_limits = {
        {"max_clients", &Settings::max_clients, 1, kPeerCeiling, "Connections at once (hosts, joiners and browsers)"},
        {"max_games", &Settings::max_games, 1, 2000, "Registered games"},
        {"max_per_ip", &Settings::max_per_ip, 1, kPeerCeiling, "Connections from one address"},
        {"rate", &Settings::rate, 10, 10000, "Packets per second per client before it is dropped"},
        {"idle_timeout", &Settings::idle_timeout, 5, 3600, "Seconds before a connection outside a game is closed"},
    };

    return s_limits;
}

bool set_value(Settings* s, const std::string& key, const std::string& value, std::string* error)
{

    int v = 0;

    if (key == "port") {
        if (!parse_int(value, &v) || v < 1 || v > 65535) {
            *error = "port must be 1-65535";
            return false;
        }

        s->port = (uint16_t)v;
        return true;
    }

    if (key == "name") {
        const std::string name = sanitize_name(value, "");

        if (name.empty()) {
            *error = "name needs letters or digits";
            return false;
        }

        s->name = name;
        return true;
    }

    for (const LimitDef& d : limits()) {
        if (key != d.key) continue;

        if (!parse_int(value, &v) || v < d.lo || v > d.hi) {
            *error = std::string(d.key) + " must be " + std::to_string(d.lo) + "-" + std::to_string(d.hi);
            return false;
        }

        s->*d.field = v;
        return true;
    }

    *error = "unknown setting '" + key + "'";
    return false;
}

std::vector<std::string> parse_config(const std::string& text, Settings* s)
{

    std::vector<std::string> warnings;
    std::istringstream in(text);
    std::string line;
    int number = 0;

    while (std::getline(in, line)) {
        ++number;

        const std::string body = trim(line.substr(0, line.find('#')));

        if (body.empty()) continue;

        const size_t eq = body.find('=');
        std::string error = "expected 'key = value'";

        if (eq == std::string::npos || !set_value(s, trim(body.substr(0, eq)), trim(body.substr(eq + 1)), &error)) {
            warnings.push_back("line " + std::to_string(number) + ": " + error + ", skipped");
        }
    }

    return warnings;
}

std::string config_text(const Settings& s)
{

    std::string out = "# RS64 relay server settings ('save' in the server console rewrites this file)\n";

    out += "# UDP port, 1-65535\nport = " + std::to_string(s.port) + "\n";
    out += "# Server name shown in the game's list (A-Z, 0-9, space)\nname = " + s.name + "\n";

    for (const LimitDef& d : limits()) {
        out += "# " + std::string(d.what) + ", " + std::to_string(d.lo) + "-" + std::to_string(d.hi) + "\n";
        out += std::string(d.key) + " = " + std::to_string(s.*d.field) + "\n";
    }

    return out;
}

bool load_config(const std::string& path, Settings* s, std::vector<std::string>* warnings)
{

    std::ifstream in(path, std::ios::binary);

    if (!in)
    {
        return false;
    }

    std::stringstream text;

    text << in.rdbuf();

    for (const std::string& w : parse_config(text.str(), s)) warnings->push_back(path + " " + w);

    return true;
}

bool save_config(const std::string& path, const Settings& s)
{

    std::ofstream out(path, std::ios::binary | std::ios::trunc);

    out << config_text(s);
    return out.good();
}

Options parse_args(int argc, const char* const* argv)
{

    Options o;
    std::vector<std::pair<std::string, std::string>> flags;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];

        if (a == "--help" || a == "-h") {
            o.help = true;
            continue;
        }

        if (a.rfind("--", 0) != 0) {
            o.errors.push_back("unexpected argument '" + a + "'");
            continue;
        }

        std::string key = a.substr(2);
        std::string value;
        const size_t eq = key.find('=');

        if (eq != std::string::npos) {
            value = key.substr(eq + 1);
            key = key.substr(0, eq);
        } else if (i + 1 < argc) {
            value = argv[++i];
        } else {
            o.errors.push_back("missing value for " + a);
            continue;
        }

        std::replace(key.begin(), key.end(), '-', '_');

        if (key == "config") {
            o.config = value;
        } else {
            flags.push_back({key, value});
        }
    }

    o.config_found = load_config(o.config, &o.settings, &o.warnings);

    for (const auto& [key, value] : flags) {
        std::string error;

        if (!set_value(&o.settings, key, value, &error)) o.errors.push_back(flag_name(key) + ": " + error);
    }

    return o;
}

std::string usage()
{

    const Settings d;
    std::string out = "Usage: rs64_relay [options]\n\n";

    out += "  --config <path>       Config file (default " + std::string(kDefaultConfig) + "); flags override it\n";
    out += "  --port <n>            UDP port (default " + std::to_string(d.port) + ")\n";
    out += "  --name <text>         Server name shown in the game's list (default " + d.name + ")\n";

    for (const LimitDef& l : limits()) {
        std::string flag = flag_name(l.key) + " <n>";

        flag.resize(22, ' ');
        out += "  " + flag + l.what + " (default " + std::to_string(d.*l.field) + ", " + std::to_string(l.lo) + "-" + std::to_string(l.hi) + ")\n";
    }

    out += "  --help                This text\n\n";
    out += "Console commands: help, games, kick <game id>, limits, set <limit> <value>, save, reload, quit\n";
    return out;
}

Relay::Relay(Settings s, std::string config_path)
    : s_(std::move(s)), config_path_(std::move(config_path))
{
}

void Relay::on_connect(uint32_t peer, const std::string& ip, int64_t now_ms)
{

    const int from_ip = (int)std::count_if(clients_.begin(), clients_.end(), [&](const auto& kv) { return kv.second.ip == ip; });
    Reason refuse = Reason::None;

    if ((int)clients_.size() >= s_.max_clients) {
        refuse = Reason::ServerFull;
    } else if (from_ip >= s_.max_per_ip) {
        refuse = Reason::TooManyFromAddress;
    }

    if (refuse != Reason::None) {
        log("refused " + ip + ": " + reason_text(refuse));
        actions_.push_back(Action{Action::Kind::Disconnect, peer, true, {}, refuse});
        return;
    }

    clients_[peer] = Client{ip, 0, now_ms, now_ms, 0};
}

void Relay::on_disconnect(uint32_t peer)
{

    if (!clients_.count(peer))
    {
        return;
    }

    leave_game(peer);
    clients_.erase(peer);
}

void Relay::on_receive(uint32_t peer, bool reliable, const uint8_t* data, size_t n, int64_t now_ms)
{

    auto it = clients_.find(peer);

    if (it == clients_.end())
    {
        return;
    }

    Client& c = it->second;

    if (now_ms - c.window_ms >= 1000) {
        c.window_ms = now_ms;
        c.packets = 0;
    }

    if (++c.packets > s_.rate) {
        drop(peer, Reason::RateLimited);
        return;
    }

    c.last_rx_ms = now_ms;
    handle(peer, c, reliable, data, n);
}

void Relay::handle(uint32_t peer, Client& c, bool reliable, const uint8_t* data, size_t n)
{

    bool version_ok = true;
    const uint8_t type = message_type(data, n, &version_ok);

    if (!version_ok)
    {
        return drop(peer, Reason::Version);
    }

    switch ((Type)type) {
    case Type::Data: {
        auto g = games_.find(c.game);

        if (g == games_.end() || !g->second.guest)
        {
            return;
        }

        send(g->second.host == peer ? g->second.guest : g->second.host, std::vector<uint8_t>(data, data + n), reliable);
        return;
    }

    case Type::Register: {
        Game g;
        std::string name;

        if (!decode_register(data, n, &g.build, &g.locked, &name))
        {
            return drop(peer, Reason::BadRequest);
        }

        if (c.game)
        {
            return send(peer, encode_refused(Type::Register, Reason::AlreadyInGame));
        }

        if ((int)games_.size() >= s_.max_games)
        {
            return send(peer, encode_refused(Type::Register, Reason::GamesFull));
        }

        // A random 6-digit id: it is the game's join code in the game's menus.
        uint32_t id = 0;

        while (!id || games_.count(id)) id = 100000u + (uint32_t)(rng_() % 900000u);

        g.name = sanitize_name(name, "GAME " + std::to_string(id));
        g.host = peer;
        games_[id] = g;
        c.game = id;
        send(peer, encode_registered(id));
        log("game " + std::to_string(id) + " '" + g.name + "' opened by " + c.ip + (g.locked ? " (password)" : ""));
        return;
    }

    case Type::Update: {
        bool locked = false;
        std::string name;

        if (!decode_update(data, n, &locked, &name))
        {
            return drop(peer, Reason::BadRequest);
        }

        auto g = games_.find(c.game);

        if (g == games_.end() || g->second.host != peer)
        {
            return send(peer, encode_refused(Type::Update, Reason::BadRequest));
        }

        g->second.locked = locked;
        g->second.name = sanitize_name(name, g->second.name);
        return;
    }

    case Type::List: {
        uint32_t build = 0;

        if (!decode_list(data, n, &build))
        {
            return drop(peer, Reason::BadRequest);
        }

        ListReply r;

        r.server = s_.name;

        for (const auto& [id, g] : games_) {
            if (g.build != build || g.guest) continue;

            ++r.total;

            if (r.games.size() < kListMax) r.games.push_back(GameEntry{id, g.locked, g.name});
        }

        send(peer, encode_list_reply(r));
        return;
    }

    case Type::Join: {
        uint32_t id = 0;
        uint32_t build = 0;

        if (!decode_join(data, n, &id, &build))
        {
            return drop(peer, Reason::BadRequest);
        }

        auto g = games_.find(id);
        Reason refuse = Reason::None;

        if (c.game) {
            refuse = Reason::AlreadyInGame;
        } else if (g == games_.end()) {
            refuse = Reason::GameGone;
        } else if (g->second.build != build) {
            refuse = Reason::BuildMismatch;
        } else if (g->second.guest) {
            refuse = Reason::GameFull;
        }

        if (refuse != Reason::None)
        {
            return send(peer, encode_refused(Type::Join, refuse));
        }

        g->second.guest = peer;
        c.game = id;
        send(peer, encode_joined(id));
        send(g->second.host, encode_peer_joined());
        log("game " + std::to_string(id) + " '" + g->second.name + "': " + c.ip + " joined");
        return;
    }

    default:
        return drop(peer, Reason::BadRequest);
    }
}

void Relay::tick(int64_t now_ms)
{

    std::vector<uint32_t> idle;

    for (const auto& [peer, c] : clients_) {
        if (!c.game && now_ms - c.last_rx_ms >= (int64_t)s_.idle_timeout * 1000) idle.push_back(peer);
    }

    for (uint32_t peer : idle) drop(peer, Reason::Idle);
}

std::vector<std::string> Relay::command(const std::string& line)
{

    const std::vector<std::string> w = words(line);

    if (w.empty())
    {
        return {};
    }

    const std::string& cmd = w[0];

    if (cmd == "help") {
        return {"help                  This list",
                "games                 Games, players and the limits in use",
                "kick <game id>        End one game",
                "limits                Every limit and its value",
                "set <limit> <value>   Change a limit now",
                "save                  Write the current settings to " + config_path_,
                "reload                Re-read " + config_path_,
                "quit                  Close every game and stop"};
    }

    if (cmd == "games")
    {
        return games_text();
    }

    if (cmd == "kick") {
        int id = 0;

        if (w.size() != 2 || !parse_int(w[1], &id))
        {
            return {"usage: kick <game id>"};
        }

        auto g = games_.find((uint32_t)id);

        if (g == games_.end())
        {
            return {"no game " + w[1]};
        }

        const Game game = g->second;

        log("game " + w[1] + " '" + game.name + "' kicked");

        if (game.guest) drop(game.guest, Reason::Kicked);

        drop(game.host, Reason::Kicked);
        return {};
    }

    if (cmd == "limits") {
        std::vector<std::string> out;

        for (const LimitDef& d : limits()) {
            std::string key = d.key;
            std::string value = std::to_string(s_.*d.field);

            key.resize(14, ' ');
            value.resize(8, ' ');
            out.push_back(key + value + d.what);
        }

        return out;
    }

    if (cmd == "set") {
        if (w.size() != 3)
        {
            return {"usage: set <limit> <value>"};
        }

        const bool is_limit = std::any_of(limits().begin(), limits().end(), [&](const LimitDef& d) { return w[1] == d.key; });
        std::string error = "unknown limit '" + w[1] + "' (see limits)";

        if (!is_limit || !set_value(&s_, w[1], w[2], &error))
        {
            return {error};
        }

        std::vector<std::string> out = {w[1] + " = " + w[2]};

        if (!told_save_) {
            out.push_back(kSaveNote);
            told_save_ = true;
        }

        return out;
    }

    if (cmd == "save")
    {
        return {save_config(config_path_, s_) ? "saved " + config_path_ : "cannot write " + config_path_};
    }

    if (cmd == "reload") {
        Settings next = s_;
        std::vector<std::string> out;

        if (!load_config(config_path_, &next, &out))
        {
            return {"no config file at " + config_path_};
        }

        if (next.port != s_.port) out.push_back("port " + std::to_string(next.port) + " applies after a restart");

        s_ = next;
        out.push_back("reloaded " + config_path_);
        return out;
    }

    if (cmd == "quit") {
        std::vector<uint32_t> peers;

        for (const auto& kv : clients_) peers.push_back(kv.first);

        for (uint32_t peer : peers) drop(peer, Reason::ServerQuit);

        quitting_ = true;
        return {"stopping"};
    }

    return {"unknown command '" + cmd + "' (type help)"};
}

std::vector<std::string> Relay::games_text() const
{

    std::string head = std::to_string(clients_.size()) + " clients, " + std::to_string(games_.size()) + " games; limits:";

    for (const LimitDef& d : limits()) head += " " + std::string(d.key) + " " + std::to_string(s_.*d.field);

    std::vector<std::string> out = {head};
    const auto ip = [&](uint32_t peer) { return clients_.count(peer) ? clients_.at(peer).ip : std::string("?"); };

    for (const auto& [id, g] : games_) {
        std::string line = "  " + std::to_string(id) + "  '" + g.name + "'" + (g.locked ? " password" : "") + "  host " + ip(g.host);

        line += g.guest ? "  playing with " + ip(g.guest) : "  waiting";
        out.push_back(line);
    }

    if (games_.empty()) out.push_back("  no games");

    return out;
}

std::vector<Action> Relay::take_actions()
{

    std::vector<Action> out;

    out.swap(actions_);
    return out;
}

std::vector<std::string> Relay::take_log()
{

    std::vector<std::string> out;

    out.swap(log_);
    return out;
}

void Relay::send(uint32_t peer, std::vector<uint8_t> bytes, bool reliable)
{
    actions_.push_back(Action{Action::Kind::Send, peer, reliable, std::move(bytes), Reason::None});
}

void Relay::drop(uint32_t peer, Reason reason)
{

    auto it = clients_.find(peer);

    if (it == clients_.end())
    {
        return;
    }

    if (reason != Reason::ServerQuit && reason != Reason::Kicked) log("closed " + it->second.ip + ": " + reason_text(reason));

    leave_game(peer);
    clients_.erase(peer);
    actions_.push_back(Action{Action::Kind::Disconnect, peer, true, {}, reason});
}

void Relay::leave_game(uint32_t peer)
{

    auto c = clients_.find(peer);

    if (c == clients_.end() || !c->second.game)
    {
        return;
    }

    const uint32_t id = c->second.game;
    auto g = games_.find(id);

    c->second.game = 0;

    if (g == games_.end())
    {
        return;
    }

    const std::string label = "game " + std::to_string(id) + " '" + g->second.name + "'";

    if (g->second.host == peer) {
        if (auto guest = clients_.find(g->second.guest); guest != clients_.end()) {
            guest->second.game = 0;
            send(g->second.guest, encode_peer_left());
        }

        games_.erase(g);
        log(label + " closed");
        return;
    }

    g->second.guest = 0;
    send(g->second.host, encode_peer_left());
    log(label + ": " + c->second.ip + " left, waiting again");
}

void Relay::log(std::string line)
{
    log_.push_back(std::move(line));
}

}
