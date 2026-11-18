#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace rs64::relay {

// The per-address limits' key: one home has a whole /64 of IPv6 addresses, so an IPv6 address counts as its /64; IPv4 (and IPv4-mapped) as itself.
std::string address_group(const std::string& ip);

// Carries browser sessions to the relay's own ENet port: each session gets a UDP socket on ::1, so the ENet host sees an ordinary peer.
// open, from_browser and close may run on WebTransport worker threads; pump runs on one bridge thread.
class Bridge {
public:
    using Send = std::function<void(const uint8_t*, size_t)>;

    explicit Bridge(uint16_t enet_port) : enet_port_(enet_port) {}
    ~Bridge();
    // Opens a session's socket; 0 when no socket could be opened.
    uint32_t open(const std::string& remote, Send to_browser);
    void from_browser(uint32_t id, const uint8_t* data, size_t len);
    void close(uint32_t id);
    // The browser address of the session whose loopback socket has this port, or "" for any other peer.
    std::string remote_of(uint16_t local_port) const;
    // Waits up to timeout_ms for datagrams on the session sockets and hands each one to its session.
    void pump(int timeout_ms);
    size_t sessions() const;

private:
    struct Session {
        intptr_t socket = 0;
        uint16_t port = 0;
        std::string remote;
        Send send;
    };

    uint16_t enet_port_;
    mutable std::mutex mu_;
    // Held while a session's send runs; close takes it, so once close returns that session is never sent to again.
    std::mutex send_mu_;
    std::map<uint32_t, Session> sessions_;
    std::vector<intptr_t> closing_;
    uint32_t next_ = 1;
};

}
