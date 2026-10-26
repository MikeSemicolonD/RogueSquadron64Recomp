#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>
#include "net_link.h"
#include "relay_protocol.h"
#include "rs64/mp_transport.h"

namespace rs64::net {

// The "server" transport: co-op through a self-hosted rs64_relay (relay-server/). A Link to the server carries the relay protocol, whose Data packets hold the game's messages; a second Link fetches the game list.
// A game's id on the server is its 6-digit join code. Game thread only (each Link runs its own network thread).
class RelayClient {
public:
    // "host[:port]" (port default relay::kDefaultPort); empty: not configured, available() is false.
    void set_server(const std::string& server);
    bool configured() const { return !host_.empty(); }
    // Configured and not known to be down. The first call checks the server in the background; a down server is checked again every kRetryMs.
    bool available();
    bool start(int role, const std::string& peer, uint32_t build);
    void stop();
    void shutdown();
    bool connected();
    bool lost() const { return lost_; }
    uint32_t connect_count() const { return connects_; }
    void send(const uint8_t* bytes, uint32_t n, bool reliable);
    bool flush(int timeout_ms) { return link_.flush(timeout_ms); }
    void receive(rs64_mp_recv_fn fn, void* user);
    const char* status() const { return status_.c_str(); }
    const char* host_label() const { return host_label_.c_str(); }
    void refresh(uint32_t build);
    int game_count();
    const relay::GameEntry* game(int i) const { return i >= 0 && i < (int)games_.size() ? &games_[i] : nullptr; }
    // The game's join code: what start() takes as its peer.
    const char* game_peer(int i) const { return game(i) ? codes_[i].c_str() : ""; }
    void set_info(const std::string& name, bool locked);
    // The mp.transport table over this client (name "server").
    const rs64_mp_transport* table();

private:
    enum class Health { Unknown, Up, Down };
    void pump();
    void pump_list();
    void start_list(bool probe);
    void mark_down(const char* why);
    LinkConfig link_config() const;

    std::string host_;
    uint16_t port_ = relay::kDefaultPort;
    Health health_ = Health::Unknown;
    int64_t retry_at_ms_ = 0;
    int64_t started_ms_ = 0;
    Link link_;
    int role_ = 0;
    uint32_t build_ = 0;
    uint32_t game_ = 0;
    bool said_ = false;
    bool gone_ = false;
    bool connected_ = false;
    bool lost_ = false;
    bool failed_ = false;
    uint32_t connects_ = 0;
    std::deque<std::vector<uint8_t>> inbox_;
    std::string status_;
    std::string host_label_;
    std::string name_;
    bool locked_ = false;

    Link list_link_;
    bool listing_ = false;
    // The list link only checks that the server answers (no LIST).
    bool probe_ = false;
    bool list_said_ = false;
    uint32_t list_build_ = 0;
    int64_t list_until_ms_ = 0;
    std::vector<relay::GameEntry> games_;
    std::vector<std::string> codes_;
    rs64_mp_transport table_{};
};

// Menu text for a relay server's refusal or close reason.
std::string relay_reason_text(relay::Reason r);

}
