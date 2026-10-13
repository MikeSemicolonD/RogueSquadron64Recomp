#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "net_core.h"
#include "rs64/mp_transport.h"

namespace rs64::net {

enum class Role { None, Host, Client };

// This machine's IPv4 addresses (host byte order) on every adapter that is up.
std::vector<uint32_t> local_ipv4s();
// Fallback pick from a list: a private LAN address, preferring 192.168/16, then 10/8, then 172.16/12; else the first address that is not loopback or link-local.
std::optional<uint32_t> pick_lan_ipv4(const std::vector<uint32_t>& addrs);
// The address a joining player should type, dotted ("10.0.0.189"): the source address of the default route (VM host-only adapters also use 192.168), else pick_lan_ipv4; empty with no network.
std::string lan_address_text();
std::optional<uint32_t> lan_ipv4();

// Where a joiner sends discovery queries: the limited broadcast, then each LAN address's /24 broadcast (a broadcast to 255.255.255.255 leaves through one adapter only).
std::vector<uint32_t> discovery_targets(const std::vector<uint32_t>& local);

// Host side of LAN discovery: answers queries on the game port + kDiscoveryPortOffset. Non-blocking; poll() from any one thread.
class DiscoveryResponder {
public:
    ~DiscoveryResponder() { stop(); }
    bool start(uint16_t game_port, const DiscoveryReply& info);
    void poll();
    void stop();
private:
    intptr_t sock_ = -1;
    DiscoveryReply info_;
};

struct FoundHost {
    std::string addr;
    DiscoveryReply info;
};

// Joiner side: query() sends one round of queries, poll() returns every host that answered so far (one entry per address).
class DiscoveryFinder {
public:
    ~DiscoveryFinder() { stop(); }
    // targets empty = discovery_targets(local_ipv4s()).
    bool start(uint16_t game_port, std::vector<uint32_t> targets = {});
    void query();
    std::vector<FoundHost> poll();
    void stop();
private:
    intptr_t sock_ = -1;
    uint16_t port_ = 0;
    std::vector<uint32_t> targets_;
    std::vector<FoundHost> found_;
};

// Internet hosting: asks the router (UPnP IGD) to forward the game's UDP port to this machine on a worker thread, renews the lease while hosting, and removes the mapping on stop().
// Calls come from one thread (the game thread); each run's worker owns its own Job, so stop() never waits on a router search or request.
class PortMapper {
public:
    ~PortMapper() { stop(); }
    // iface: the local IPv4 to search for the router from (empty: lan_address_text()).
    void start(uint16_t port, const std::string& iface = "");
    // Returns at once; the worker finishes and removes its mapping in the background, before the next start()'s worker begins.
    void stop();
    PortMapState state() const;
    std::string external() const;
    // True once the last worker has finished (its mapping removed after a stop()).
    bool settled() const;
private:
    struct Job {
        std::mutex mu;
        std::condition_variable cv;
        bool quit = false;
        bool done = false;
        PortMapState state = PortMapState::Working;
        std::string external;
    };
    static void run(std::shared_ptr<Job> job, std::shared_ptr<Job> prev, uint16_t port, std::string iface);
    std::shared_ptr<Job> job_;
    std::shared_ptr<Job> last_;
};

struct LinkConfig {
    Role role = Role::None;
    std::string addr = "127.0.0.1";
    uint16_t port = 27064;
    // Simulated one-way latency added to this side's sends, and the percentage of unreliable sends dropped.
    int latency_ms = 0;
    int loss_pct = 0;
    // Simulated jitter: each send waits a further random 0..jitter_ms (sends stay in order).
    int jitter_ms = 0;
    // Non-null: run over this mp.transport service instead of ENet (addr, port and the simulated network are unused); peer is the client's target in its terms.
    const rs64_mp_transport* external = nullptr;
    std::string peer;
    uint32_t build = kBuildId;
    // Host: listen on this IPv4 only (host byte order); 0 = every address, IPv4 and IPv6.
    uint32_t bind_v4 = 0;
};

// One ENet peer owned by a network thread, or a forward to an external transport; the game thread only queues and drains byte buffers.
class Link {
public:
    ~Link();
    bool start(const LinkConfig& cfg);
    void stop();
    bool connected() const;
    bool lost() const;
    uint32_t connect_count() const;
    void send(std::vector<uint8_t> bytes, bool reliable);
    // Waits up to timeout_ms for everything queued so far to reach ENet's socket; false on timeout.
    bool flush(int timeout_ms);
    std::vector<std::vector<uint8_t>> receive();
private:
    struct Out {
        std::chrono::steady_clock::time_point at;
        std::vector<uint8_t> bytes;
        bool reliable;
    };
    void run();
    LinkConfig cfg_;
    const rs64_mp_transport* ext_ = nullptr;
    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> lost_{false};
    std::atomic<uint32_t> connects_{0};
    std::atomic<uint64_t> queued_{0};
    std::atomic<uint64_t> handled_{0};
    std::atomic<int64_t> last_rx_ms_{-1};
    std::mutex mu_;
    std::deque<Out> out_;
    std::vector<std::vector<uint8_t>> in_;
    std::mt19937 rng_{1234};
};

}
