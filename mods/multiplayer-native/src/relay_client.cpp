#include "relay_client.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace rs64::net {

namespace {

constexpr int64_t kListTimeoutMs = 5000;
// A session's connection to the server gives up after this; a server found down is checked again after kRetryMs.
constexpr int64_t kConnectTimeoutMs = 5000;
constexpr int64_t kRetryMs = 5000;
constexpr const char* kUnreachable = "COULD NOT CONNECT TO SERVER";

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string code_of(uint32_t id) {
    char b[16];
    snprintf(b, sizeof b, "%06u", id % 1000000u);
    return b;
}

}

std::string relay_reason_text(relay::Reason r) {
    using relay::Reason;
    switch (r) {
    case Reason::ServerFull:
    case Reason::GamesFull:
        return "RELAY SERVER FULL";
    case Reason::TooManyFromAddress:
        return "TOO MANY CONNECTIONS";
    case Reason::GameFull:
        return "GAME FULL";
    case Reason::GameGone:
        return "NO GAME WITH THAT CODE";
    case Reason::BuildMismatch:
        return "VERSION MISMATCH";
    case Reason::Kicked:
        return "REMOVED BY THE SERVER";
    case Reason::ServerQuit:
        return "RELAY SERVER CLOSED";
    case Reason::Version:
        return "RELAY SERVER NEEDS AN UPDATE";
    case Reason::RateLimited:
        return "TOO MUCH TRAFFIC";
    default:
        return "RELAY SERVER ERROR";
    }
}

void RelayClient::set_server(const std::string& server) {
    std::string host = server;
    uint16_t port = relay::kDefaultPort;
    // "host:port"; an IPv6 address has more than one ':' and is taken whole.
    const size_t colon = server.rfind(':');
    if (colon != std::string::npos && server.find(':') == colon) {
        const int p = atoi(server.c_str() + colon + 1);
        host = server.substr(0, colon);
        port = (p > 0 && p < 65536) ? (uint16_t)p : relay::kDefaultPort;
    }
    if (host == host_ && port == port_) {
        return;
    }
    shutdown();
    host_ = host;
    port_ = port;
}

bool RelayClient::available() {
    if (!configured()) {
        return false;
    }
    pump_list();
    if (!listing_ && (health_ == Health::Unknown || (health_ == Health::Down && now_ms() >= retry_at_ms_))) {
        start_list(true);
    }
    return health_ != Health::Down;
}

void RelayClient::mark_down(const char* why) {
    health_ = Health::Down;
    retry_at_ms_ = now_ms() + kRetryMs;
    fprintf(stderr, "[relay] %s:%u: %s\n", host_.c_str(), (unsigned)port_, why);
}

LinkConfig RelayClient::link_config() const {
    LinkConfig cfg;
    cfg.role = Role::Client;
    cfg.addr = host_;
    cfg.port = port_;
    return cfg;
}

bool RelayClient::start(int role, const std::string& peer, uint32_t build) {
    stop();
    failed_ = false;
    if (!configured() || (role == RS64_MP_ROLE_CLIENT && !CodeEditor::valid(peer))) {
        return false;
    }
    role_ = role;
    build_ = build;
    started_ms_ = now_ms();
    game_ = role == RS64_MP_ROLE_CLIENT ? (uint32_t)atoi(peer.c_str()) : 0;
    status_ = "CONNECTING TO RELAY SERVER";
    if (!link_.start(link_config())) {
        role_ = 0;
        status_ = kUnreachable;
        return false;
    }
    return true;
}

void RelayClient::stop() {
    link_.stop();
    role_ = 0;
    game_ = 0;
    said_ = false;
    gone_ = false;
    connected_ = false;
    lost_ = false;
    connects_ = 0;
    inbox_.clear();
    host_label_.clear();
    // A failure's reason stays on the status line until the next start().
    if (!failed_) {
        status_.clear();
    }
}

void RelayClient::shutdown() {
    stop();
    list_link_.stop();
    listing_ = false;
    health_ = Health::Unknown;
    games_.clear();
    codes_.clear();
    status_.clear();
}

// The session link: say REGISTER or JOIN once the server is reached, then turn its replies into connect and lost edges and its Data into game messages.
void RelayClient::pump() {
    if (!role_) {
        return;
    }
    // The server never answered (or its address did not resolve): the lobby fails a join with this, and a host hosts directly instead.
    if (!said_ && !gone_ && !link_.connected() && (link_.lost() || now_ms() - started_ms_ > kConnectTimeoutMs)) {
        gone_ = true;
        mark_down("no answer");
        status_ = kUnreachable;
        lost_ = true;
        failed_ = true;
        return;
    }
    if (link_.connected() && !said_) {
        said_ = true;
        health_ = Health::Up;
        const bool host = role_ == RS64_MP_ROLE_HOST;
        link_.send(host ? relay::encode_register(build_, locked_, name_) : relay::encode_join(game_, build_), true);
        status_ = host ? "OPENING GAME ON RELAY SERVER" : "JOINING GAME " + code_of(game_);
    }
    for (const std::vector<uint8_t>& m : link_.receive()) {
        uint32_t id = 0;
        relay::Type request = relay::Type::Data;
        relay::Reason reason = relay::Reason::None;
        const uint8_t* bytes = nullptr;
        size_t len = 0;
        switch ((relay::Type)relay::message_type(m.data(), m.size())) {
        case relay::Type::Data:
            if (relay::decode_data(m.data(), m.size(), &bytes, &len)) {
                inbox_.emplace_back(bytes, bytes + len);
            }
            break;
        case relay::Type::Registered:
            if (relay::decode_registered(m.data(), m.size(), &id)) {
                game_ = id;
                host_label_ = "JOIN CODE " + code_of(id);
                status_ = "SHARE THE JOIN CODE";
            }
            break;
        case relay::Type::Joined:
        case relay::Type::PeerJoined:
            connected_ = true;
            lost_ = false;
            ++connects_;
            status_ = role_ == RS64_MP_ROLE_HOST ? "PLAYER JOINED" : "CONNECTED";
            break;
        case relay::Type::PeerLeft:
            lost_ = connected_ || role_ == RS64_MP_ROLE_CLIENT;
            connected_ = false;
            status_ = role_ == RS64_MP_ROLE_HOST ? "SHARE THE JOIN CODE" : "THE HOST LEFT";
            failed_ = role_ == RS64_MP_ROLE_CLIENT;
            break;
        case relay::Type::Refused:
            if (relay::decode_refused(m.data(), m.size(), &request, &reason)) {
                status_ = relay_reason_text(reason);
                lost_ = true;
                failed_ = true;
            }
            break;
        default:
            break;
        }
    }
    // The server went away (or closed this connection with a reason).
    if (link_.lost() && !gone_) {
        gone_ = true;
        const uint32_t why = link_.close_data();
        mark_down(why ? "closed the connection" : "connection lost");
        status_ = why ? relay_reason_text((relay::Reason)why) : "RELAY SERVER LOST";
        connected_ = false;
        lost_ = true;
        failed_ = true;
    }
}

bool RelayClient::connected() {
    pump();
    return connected_;
}

void RelayClient::send(const uint8_t* bytes, uint32_t n, bool reliable) {
    if (connected_ && n <= relay::kMaxGamePacket) {
        link_.send(relay::encode_data(bytes, n), reliable);
    }
}

void RelayClient::receive(rs64_mp_recv_fn fn, void* user) {
    pump();
    while (!inbox_.empty()) {
        const std::vector<uint8_t>& m = inbox_.front();
        fn(m.data(), (uint32_t)m.size(), user);
        inbox_.pop_front();
    }
}

void RelayClient::set_info(const std::string& name, bool locked) {
    if (name == name_ && locked == locked_) {
        return;
    }
    name_ = name;
    locked_ = locked;
    if (role_ == RS64_MP_ROLE_HOST && game_) {
        link_.send(relay::encode_update(locked_, name_), true);
    }
}

void RelayClient::refresh(uint32_t build) {
    list_build_ = build;
    start_list(false);
}

// probe: only check that the server answers; else fetch the list (the old one is dropped).
void RelayClient::start_list(bool probe) {
    list_link_.stop();
    if (!probe) {
        games_.clear();
        codes_.clear();
    }
    probe_ = probe;
    listing_ = configured() && list_link_.start(link_config());
    list_said_ = false;
    list_until_ms_ = now_ms() + kListTimeoutMs;
}

// The list link: LIST once connected (a probe just hangs up), keep the reply, hang up. A server that never answers in kListTimeoutMs is down; one that answers but sends no list lists nothing.
void RelayClient::pump_list() {
    if (!listing_) {
        return;
    }
    if (list_link_.connected()) {
        health_ = Health::Up;
        if (probe_) {
            listing_ = false;
        } else if (!list_said_) {
            list_said_ = true;
            list_link_.send(relay::encode_list(list_build_), true);
        }
    }
    for (const std::vector<uint8_t>& m : list_link_.receive()) {
        relay::ListReply r;
        if (relay::decode_list_reply(m.data(), m.size(), &r)) {
            games_ = r.games;
            for (const relay::GameEntry& e : games_) {
                codes_.push_back(code_of(e.id));
            }
            listing_ = false;
        }
    }
    if (listing_ && (now_ms() > list_until_ms_ || list_link_.lost())) {
        listing_ = false;
        if (!list_said_) {
            mark_down("no answer");
        } else {
            fprintf(stderr, "[relay] no game list from %s:%u\n", host_.c_str(), (unsigned)port_);
        }
    }
    if (!listing_) {
        list_link_.stop();
    }
}

int RelayClient::game_count() {
    pump_list();
    return listing_ && !probe_ ? -1 : (int)games_.size();
}

const rs64_mp_transport* RelayClient::table() {
    static const auto C = [](void* u) -> RelayClient& { return *(RelayClient*)u; };
    table_ = {
        RS64_MP_TRANSPORT_VERSION,
        (uint32_t)sizeof(rs64_mp_transport),
        "server",
        nullptr,
        [](void* u) -> int { return C(u).available() ? 1 : 0; },
        [](void* u, int role, const char* peer, uint32_t build) -> int { return C(u).start(role, peer ? peer : "", build) ? 1 : 0; },
        [](void* u) { C(u).stop(); },
        [](void* u) { C(u).shutdown(); },
        [](void* u) -> int { return C(u).connected() ? 1 : 0; },
        [](void* u) -> int { return C(u).lost() ? 1 : 0; },
        [](void* u) -> uint32_t { return C(u).connect_count(); },
        [](void* u, const uint8_t* b, uint32_t n, int reliable) { C(u).send(b, n, reliable != 0); },
        [](void* u, int ms) -> int { return C(u).flush(ms) ? 1 : 0; },
        [](void* u, rs64_mp_recv_fn fn, void* fu) { C(u).receive(fn, fu); },
        [](void* u) -> const char* { return C(u).status(); },
        [](void* u) -> const char* { return C(u).host_label(); },
        [](void*) -> const char* { return nullptr; },
        [](void* u, uint32_t build) { C(u).refresh(build); },
        [](void* u) -> int { return C(u).game_count(); },
        [](void* u, int i) -> const char* { return C(u).game(i) ? C(u).game(i)->name.c_str() : ""; },
        [](void* u, int i) -> const char* { return C(u).game_peer(i); },
        [](void* u, int i) -> int { return C(u).game(i) && C(u).game(i)->locked ? 1 : 0; },
        [](void* u, const char* name, int locked) { C(u).set_info(name ? name : "", locked != 0); },
    };
    table_.user = this;
    return &table_;
}

}
