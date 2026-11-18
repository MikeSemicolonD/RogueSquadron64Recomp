#include "relay_bridge.h"
#include <enet6/enet.h>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace rs64::relay {

namespace {

ENetSocket sock(intptr_t s)
{
    return (ENetSocket)s;
}

// enet6's enet_socket_get_address leaves getsockname's length unset on Windows.
uint16_t bound_port(ENetSocket s)
{

    sockaddr_in6 a = {};
    socklen_t len = sizeof a;

    if (getsockname(s, (sockaddr*)&a, &len) != 0)
    {
        return 0;
    }

    return ntohs(a.sin6_port);
}

}

std::string address_group(const std::string& ip)
{

    ENetAddress a;

    if (enet_address_set_host_ip(&a, ip.c_str()) != 0)
    {
        return ip;
    }

    const auto& v6 = a.host.v6;

    if (a.type != ENET_ADDRESS_TYPE_IPV6 || (!v6[0] && !v6[1] && !v6[2] && !v6[3]))
    {
        return ip;
    }

    char group[32];

    snprintf(group, sizeof group, "%04x:%04x:%04x:%04x::/64", v6[0], v6[1], v6[2], v6[3]);
    return group;
}

Bridge::~Bridge()
{

    std::lock_guard<std::mutex> lock(mu_);

    for (auto& [id, s] : sessions_) enet_socket_destroy(sock(s.socket));

    for (intptr_t s : closing_) enet_socket_destroy(sock(s));
}

uint32_t Bridge::open(const std::string& remote, Send to_browser)
{

    const ENetSocket s = enet_socket_create(ENET_ADDRESS_TYPE_IPV6, ENET_SOCKET_TYPE_DATAGRAM);

    if (s == ENET_SOCKET_NULL)
    {
        return 0;
    }

    ENetAddress local;

    enet_address_build_loopback(&local, ENET_ADDRESS_TYPE_IPV6);
    local.port = 0;

    const uint16_t port = (enet_socket_bind(s, &local) == 0) ? bound_port(s) : 0;

    if (!port) {
        enet_socket_destroy(s);
        return 0;
    }

    enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1);

    std::lock_guard<std::mutex> lock(mu_);
    const uint32_t id = next_++;

    sessions_[id] = Session{ (intptr_t)s, port, remote, std::move(to_browser) };
    return id;
}

void Bridge::from_browser(uint32_t id, const uint8_t* data, size_t len)
{

    std::lock_guard<std::mutex> lock(mu_);
    const auto it = sessions_.find(id);

    if (it == sessions_.end())
    {
        return;
    }

    ENetAddress to;
    ENetBuffer b;

    b.data = (void*)data;
    b.dataLength = len;

    enet_address_build_loopback(&to, ENET_ADDRESS_TYPE_IPV6);
    to.port = enet_port_;
    enet_socket_send(sock(it->second.socket), &to, &b, 1);
}

// The socket closes on the next pump, so a pump already waiting on it never sees it vanish; a send in progress finishes first.
void Bridge::close(uint32_t id)
{

    std::lock_guard<std::mutex> sending(send_mu_);
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = sessions_.find(id);

    if (it == sessions_.end())
    {
        return;
    }

    closing_.push_back(it->second.socket);
    sessions_.erase(it);
}

std::string Bridge::remote_of(uint16_t local_port) const
{

    std::lock_guard<std::mutex> lock(mu_);

    for (const auto& [id, s] : sessions_) {
        if (s.port == local_port) return s.remote;
    }

    return {};
}

size_t Bridge::sessions() const
{

    std::lock_guard<std::mutex> lock(mu_);

    return sessions_.size();
}

void Bridge::pump(int timeout_ms)
{

    std::vector<std::pair<uint32_t, intptr_t>> open;
    std::unique_lock<std::mutex> lock(mu_);

    for (intptr_t s : closing_) enet_socket_destroy(sock(s));

    closing_.clear();

    for (const auto& [id, s] : sessions_) open.emplace_back(id, s.socket);

    lock.unlock();

    if (open.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        return;
    }

    ENetSocketSet set;
    ENetSocket top = sock(open[0].second);

    ENET_SOCKETSET_EMPTY(set);

    for (const auto& [id, s] : open) {
        ENET_SOCKETSET_ADD(set, sock(s));

        if (sock(s) > top) top = sock(s);
    }

    if (enet_socketset_select(top, &set, nullptr, (enet_uint32)timeout_ms) <= 0)
    {
        return;
    }

    uint8_t buf[2048];

    for (const auto& [id, s] : open) {
        if (!ENET_SOCKETSET_CHECK(set, sock(s))) continue;

        for (;;) {
            ENetAddress from;
            ENetBuffer b;

            b.data = buf;
            b.dataLength = sizeof buf;
            const int n = enet_socket_receive(sock(s), &from, &b, 1);

            if (n <= 0) break;

            std::lock_guard<std::mutex> sending(send_mu_);

            lock.lock();

            const auto it = sessions_.find(id);
            Send send = (it != sessions_.end()) ? it->second.send : Send{};

            lock.unlock();

            if (send) send(buf, (size_t)n);
        }
    }
}

}
