// The relay: connections, games and their pairing, packet forwarding, limits and the console commands.
#include "relay_core.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>

namespace rs64::relay {

namespace {

constexpr const char* kSaveNote = "Use 'save' to keep the current configuration";
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

        g.name = unique_name(sanitize_name(name, "GAME " + std::to_string(id)), id);
        g.host = peer;
        games_[id] = g;
        c.game = id;
        send(peer, encode_registered(id, g.name));
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
        g->second.name = unique_name(sanitize_name(name, g->second.name), g->first);
        send(peer, encode_registered(g->first, g->second.name));
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

// The name with " 2", " 3", ... while another game is listed under it; the base is cut so the suffix fits kNameMax.
std::string Relay::unique_name(const std::string& name, uint32_t self) const
{

    const auto taken = [&](const std::string& n) {
        return std::any_of(games_.begin(), games_.end(), [&](const auto& kv) { return kv.first != self && kv.second.name == n; });
    };

    std::string out = name;

    for (int i = 2; taken(out); ++i) {
        const std::string suffix = " " + std::to_string(i);
        std::string base = name.substr(0, kNameMax - suffix.size());

        while (!base.empty() && base.back() == ' ') base.pop_back();

        out = base + suffix;
    }

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
