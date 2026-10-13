#include "net_link.h"
#include <enet6/enet.h>
#include <miniupnpc.h>
#include <upnpcommands.h>
#include <upnperrors.h>
#include <algorithm>
#include <cstdio>
#include <utility>
#ifdef _WIN32
#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#pragma comment(lib, "iphlpapi.lib")
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

namespace rs64::net {

std::vector<uint32_t> local_ipv4s() {
    std::vector<uint32_t> out;
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
    }
    if (rc != NO_ERROR) {
        return out;
    }
    for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) {
            continue;
        }
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr && u->Address.lpSockaddr->sa_family == AF_INET) {
                out.push_back(ntohl(((sockaddr_in*)u->Address.lpSockaddr)->sin_addr.s_addr));
            }
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) {
        return out;
    }
    for (ifaddrs* i = list; i; i = i->ifa_next) {
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_UP)) {
            out.push_back(ntohl(((sockaddr_in*)i->ifa_addr)->sin_addr.s_addr));
        }
    }
    freeifaddrs(list);
#endif
    return out;
}

std::optional<uint32_t> pick_lan_ipv4(const std::vector<uint32_t>& addrs) {
    // 172.16/12 last: WSL, Hyper-V and Docker virtual adapters live there.
    const auto ranges = {std::make_pair(0xC0A80000u, 16), std::make_pair(0x0A000000u, 8), std::make_pair(0xAC100000u, 12)};
    for (const auto& [net, bits] : ranges) {
        for (uint32_t a : addrs) {
            if ((a >> (32 - bits)) == (net >> (32 - bits))) {
                return a;
            }
        }
    }
    for (uint32_t a : addrs) {
        if ((a >> 24) != 127u && (a >> 16) != 0xA9FEu && a != 0u) {
            return a;
        }
    }
    return std::nullopt;
}

namespace {

bool enet_ready();

// Connecting a UDP socket sends nothing; it only makes the OS pick the source address of its default route.
std::optional<uint32_t> default_route_ipv4() {
    if (!enet_ready()) {
        return std::nullopt;
    }
#ifdef _WIN32
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        return std::nullopt;
    }
#else
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        return std::nullopt;
    }
#endif
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = htonl(0x08080808u);
    std::optional<uint32_t> out;
    sockaddr_in me{};
    socklen_t len = sizeof(me);
    if (connect(s, (sockaddr*)&to, sizeof(to)) == 0 && getsockname(s, (sockaddr*)&me, &len) == 0 && me.sin_addr.s_addr != 0) {
        out = ntohl(me.sin_addr.s_addr);
    }
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
    return out;
}

}

std::optional<uint32_t> lan_ipv4() {
    std::optional<uint32_t> a = default_route_ipv4();
    return a ? a : pick_lan_ipv4(local_ipv4s());
}

std::string lan_address_text() {
    const std::optional<uint32_t> a = lan_ipv4();
    return a ? ipv4_text(*a) : "";
}

std::vector<uint32_t> discovery_targets(const std::vector<uint32_t>& local) {
    std::vector<uint32_t> out = {0xFFFFFFFFu};
    for (uint32_t a : local) {
        if (a == 0u || (a >> 24) == 127u || (a >> 16) == 0xA9FEu) {
            continue;
        }
        const uint32_t b = (a & 0xFFFFFF00u) | 0xFFu;
        if (std::find(out.begin(), out.end(), b) == out.end()) {
            out.push_back(b);
        }
    }
    return out;
}

namespace {

ENetAddress v4_address(uint32_t ip, uint16_t port) {
    ENetAddress a = {};
    a.type = ENET_ADDRESS_TYPE_IPV4;
    a.port = port;
    a.host.v4[0] = (enet_uint8)(ip >> 24);
    a.host.v4[1] = (enet_uint8)(ip >> 16);
    a.host.v4[2] = (enet_uint8)(ip >> 8);
    a.host.v4[3] = (enet_uint8)ip;
    return a;
}

// A non-blocking IPv4 datagram socket bound to port (0 = any), or ENET_SOCKET_NULL.
ENetSocket open_udp4(uint16_t port, bool broadcast) {
    ENetSocket s = enet_socket_create(ENET_ADDRESS_TYPE_IPV4, ENET_SOCKET_TYPE_DATAGRAM);
    if (s == ENET_SOCKET_NULL) {
        return s;
    }
    enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1);
    enet_socket_set_option(s, ENET_SOCKOPT_REUSEADDR, 1);
    if (broadcast) {
        enet_socket_set_option(s, ENET_SOCKOPT_BROADCAST, 1);
    }
    ENetAddress a = {};
    enet_address_build_any(&a, ENET_ADDRESS_TYPE_IPV4);
    a.port = port;
    if (enet_socket_bind(s, &a) < 0) {
        enet_socket_destroy(s);
        return ENET_SOCKET_NULL;
    }
    return s;
}

void send_bytes(ENetSocket s, const ENetAddress& to, const std::vector<uint8_t>& bytes) {
    ENetBuffer b;
    b.data = (void*)bytes.data();
    b.dataLength = bytes.size();
    enet_socket_send(s, &to, &b, 1);
}

// One datagram, or 0 bytes when none is waiting.
int receive_bytes(ENetSocket s, ENetAddress* from, uint8_t* buf, size_t cap) {
    ENetBuffer b;
    b.data = buf;
    b.dataLength = cap;
    return enet_socket_receive(s, from, &b, 1);
}

}

bool DiscoveryResponder::start(uint16_t game_port, const DiscoveryReply& info) {
    stop();
    if (!enet_ready()) {
        return false;
    }
    const ENetSocket s = open_udp4((uint16_t)(game_port + kDiscoveryPortOffset), false);
    if (s == ENET_SOCKET_NULL) {
        return false;
    }
    sock_ = (intptr_t)s;
    info_ = info;
    info_.port = game_port;
    return true;
}

void DiscoveryResponder::poll() {
    if (sock_ == (intptr_t)ENET_SOCKET_NULL) {
        return;
    }
    const ENetSocket s = (ENetSocket)sock_;
    uint8_t buf[64];
    ENetAddress from = {};
    for (int i = 0; i < 32; ++i) {
        const int n = receive_bytes(s, &from, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        if (is_discovery_query(buf, (size_t)n)) {
            send_bytes(s, from, encode_discovery_reply(info_));
        }
    }
}

void DiscoveryResponder::stop() {
    if (sock_ != (intptr_t)ENET_SOCKET_NULL) {
        enet_socket_destroy((ENetSocket)sock_);
        sock_ = (intptr_t)ENET_SOCKET_NULL;
    }
}

bool DiscoveryFinder::start(uint16_t game_port, std::vector<uint32_t> targets) {
    stop();
    if (!enet_ready()) {
        return false;
    }
    const ENetSocket s = open_udp4(0, true);
    if (s == ENET_SOCKET_NULL) {
        return false;
    }
    sock_ = (intptr_t)s;
    port_ = (uint16_t)(game_port + kDiscoveryPortOffset);
    targets_ = targets.empty() ? discovery_targets(local_ipv4s()) : std::move(targets);
    found_.clear();
    return true;
}

void DiscoveryFinder::query() {
    if (sock_ == (intptr_t)ENET_SOCKET_NULL) {
        return;
    }
    const std::vector<uint8_t> q = encode_discovery_query();
    for (uint32_t t : targets_) {
        send_bytes((ENetSocket)sock_, v4_address(t, port_), q);
    }
}

std::vector<FoundHost> DiscoveryFinder::poll() {
    if (sock_ == (intptr_t)ENET_SOCKET_NULL) {
        return found_;
    }
    uint8_t buf[64];
    ENetAddress from = {};
    for (int i = 0; i < 32; ++i) {
        const int n = receive_bytes((ENetSocket)sock_, &from, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        DiscoveryReply r;
        if (!decode_discovery_reply(buf, (size_t)n, &r) || from.type != ENET_ADDRESS_TYPE_IPV4) {
            continue;
        }
        const std::string addr = std::to_string(from.host.v4[0]) + "." + std::to_string(from.host.v4[1]) + "." + std::to_string(from.host.v4[2]) + "." + std::to_string(from.host.v4[3]);
        if (std::none_of(found_.begin(), found_.end(), [&](const FoundHost& f) { return f.addr == addr; })) {
            found_.push_back({addr, r});
        }
    }
    return found_;
}

void DiscoveryFinder::stop() {
    if (sock_ != (intptr_t)ENET_SOCKET_NULL) {
        enet_socket_destroy((ENetSocket)sock_);
        sock_ = (intptr_t)ENET_SOCKET_NULL;
    }
}

void PortMapper::start(uint16_t port, const std::string& iface) {
    stop();
    std::shared_ptr<Job> prev = last_;
    job_ = std::make_shared<Job>();
    last_ = job_;
    std::thread(&PortMapper::run, job_, prev, port, iface).detach();
}

void PortMapper::stop() {
    if (!job_) {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(job_->mu);
        job_->quit = true;
    }
    job_->cv.notify_all();
    job_.reset();
}

PortMapState PortMapper::state() const {
    if (!job_) {
        return PortMapState::Idle;
    }
    std::lock_guard<std::mutex> lk(job_->mu);
    return job_->state;
}

std::string PortMapper::external() const {
    if (!job_) {
        return "";
    }
    std::lock_guard<std::mutex> lk(job_->mu);
    return job_->external;
}

bool PortMapper::settled() const {
    if (!last_) {
        return true;
    }
    std::lock_guard<std::mutex> lk(last_->mu);
    return last_->done;
}

void PortMapper::run(std::shared_ptr<Job> job, std::shared_ptr<Job> prev, uint16_t port, std::string iface) {
    auto set = [&](PortMapState state, const std::string& external) {
        std::lock_guard<std::mutex> lk(job->mu);
        job->state = state;
        job->external = external;
    };
    auto quitting = [&]() {
        std::lock_guard<std::mutex> lk(job->mu);
        return job->quit;
    };
    struct Done {
        Job& j;
        ~Done() {
            std::lock_guard<std::mutex> lk(j.mu);
            j.done = true;
            j.cv.notify_all();
        }
    } done{*job};
    // The previous run removes its mapping of the same port first, or its delete would undo this run's add.
    if (prev) {
        std::unique_lock<std::mutex> lk(prev->mu);
        prev->cv.wait(lk, [&]() { return prev->done; });
    }
    if (quitting()) {
        return;
    }
    // SSDP goes out the default route's adapter: on a machine with virtual adapters the default multicast interface is often a virtual one.
    const std::string lan = iface.empty() ? lan_address_text() : iface;
    int err = 0;
    UPNPDev* devs = upnpDiscover(2000, lan.empty() ? nullptr : lan.c_str(), nullptr, UPNP_LOCAL_PORT_ANY, 0, 2, &err);
    if (devs && quitting()) {
        freeUPNPDevlist(devs);
        return;
    }
    if (!devs) {
        set(PortMapState::NoRouter, "");
        fprintf(stderr, "[upnp] no UPnP device answered on %s\n", lan.empty() ? "the default interface" : lan.c_str());
        return;
    }
    UPNPUrls urls{};
    IGDdatas data{};
    char lanaddr[64] = {};
    char wanaddr[64] = {};
    const int igd = UPNP_GetValidIGD(devs, &urls, &data, lanaddr, sizeof(lanaddr), wanaddr, sizeof(wanaddr));
    freeUPNPDevlist(devs);
    if (igd != UPNP_CONNECTED_IGD && igd != UPNP_PRIVATEIP_IGD) {
        if (igd != UPNP_NO_IGD) {
            FreeUPNPUrls(&urls);
        }
        set(PortMapState::NoRouter, "");
        fprintf(stderr, "[upnp] no connected internet gateway (UPNP_GetValidIGD %d)\n", igd);
        return;
    }
    if (quitting()) {
        FreeUPNPUrls(&urls);
        return;
    }
    char ext[40] = {};
    UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, ext);
    const std::string port_s = std::to_string(port);
    auto add = [&]() {
        int r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype, port_s.c_str(), port_s.c_str(), lanaddr, "Rogue Squadron 64", "UDP", nullptr, "3600");
        // 725 OnlyPermanentLeasesSupported: such routers take only lease 0.
        if (r == 725) {
            r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype, port_s.c_str(), port_s.c_str(), lanaddr, "Rogue Squadron 64", "UDP", nullptr, "0");
        }
        return r;
    };
    const int r = add();
    if (r != UPNPCOMMAND_SUCCESS) {
        FreeUPNPUrls(&urls);
        set(PortMapState::Refused, "");
        fprintf(stderr, "[upnp] the router refused UDP %u -> %s: %d %s\n", port, lanaddr, r, strupnperror(r));
        return;
    }
    unsigned o[4] = {};
    const bool parsed = sscanf(ext, "%u.%u.%u.%u", &o[0], &o[1], &o[2], &o[3]) == 4 && o[0] < 256 && o[1] < 256 && o[2] < 256 && o[3] < 256;
    const uint32_t ip = parsed ? (o[0] << 24) | (o[1] << 16) | (o[2] << 8) | o[3] : 0u;
    // A private or carrier-NAT address on the router's internet side means another NAT is in front of it: the mapping cannot make this machine reachable.
    const bool reachable = igd == UPNP_CONNECTED_IGD && public_ipv4(ip);
    set(reachable ? PortMapState::Open : PortMapState::SharedAddress, ext);
    fprintf(stderr, "[upnp] UDP %u -> %s:%u mapped; router's internet address %s%s\n", port, lanaddr, port, ext, reachable ? "" : " (not public: another NAT in front)");
    std::unique_lock<std::mutex> lk(job->mu);
    while (!job->quit) {
        if (job->cv.wait_for(lk, std::chrono::minutes(30), [&]() { return job->quit; })) {
            break;
        }
        lk.unlock();
        add();
        lk.lock();
    }
    lk.unlock();
    UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, port_s.c_str(), "UDP", nullptr);
    FreeUPNPUrls(&urls);
    fprintf(stderr, "[upnp] UDP %u mapping removed\n", port);
}

namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool enet_ready() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, []() { ok = enet_initialize() == 0; });
    return ok;
}

}

Link::~Link() {
    stop();
}

bool Link::start(const LinkConfig& cfg) {
    if (cfg.role == Role::None || thread_.joinable() || ext_) {
        return false;
    }
    if (cfg.external) {
        cfg_ = cfg;
        const int role = cfg.role == Role::Host ? RS64_MP_ROLE_HOST : RS64_MP_ROLE_CLIENT;
        const char* peer = cfg.role == Role::Client ? cfg.peer.c_str() : nullptr;
        if (!cfg.external->start(cfg.external->user, role, peer, cfg.build)) {
            return false;
        }
        ext_ = cfg.external;
        return true;
    }
    if (!enet_ready()) {
        return false;
    }
    cfg_ = cfg;
    rng_.seed(cfg.role == Role::Host ? 1234u : 5678u);
    quit_ = false;
    lost_ = false;
    thread_ = std::thread([this]() { run(); });
    return true;
}

// Leaves the link as new, so start() can run again (a lobby cancel, then another JOIN).
void Link::stop() {
    if (ext_) {
        ext_->stop(ext_->user);
        ext_ = nullptr;
    }
    quit_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
    std::lock_guard<std::mutex> lock(mu_);
    in_.clear();
    out_.clear();
    handled_ = queued_.load();
    connected_ = false;
    lost_ = false;
    last_rx_ms_ = -1;
}

bool Link::connected() const { return ext_ ? ext_->connected(ext_->user) != 0 : connected_.load(); }
bool Link::lost() const { return ext_ ? ext_->lost(ext_->user) != 0 : lost_.load(); }
uint32_t Link::connect_count() const { return ext_ ? ext_->connect_count(ext_->user) : connects_.load(); }

void Link::send(std::vector<uint8_t> bytes, bool reliable) {
    if (ext_) {
        ext_->send(ext_->user, bytes.data(), (uint32_t)bytes.size(), reliable ? 1 : 0);
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (!reliable && cfg_.loss_pct > 0 && (int)(rng_() % 100u) < cfg_.loss_pct) {
        return;
    }
    const int jitter = cfg_.jitter_ms > 0 ? (int)(rng_() % (uint32_t)(cfg_.jitter_ms + 1)) : 0;
    auto at = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg_.latency_ms + jitter);
    // FIFO: a send never goes out before an earlier one.
    if (!out_.empty() && at < out_.back().at) {
        at = out_.back().at;
    }
    out_.push_back(Out{at, std::move(bytes), reliable});
    ++queued_;
}

bool Link::flush(int timeout_ms) {
    if (ext_) {
        return ext_->flush(ext_->user, timeout_ms) != 0;
    }
    const uint64_t target = queued_;
    const auto t0 = std::chrono::steady_clock::now();
    while (handled_ < target) {
        if (!thread_.joinable() || std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(timeout_ms)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

std::vector<std::vector<uint8_t>> Link::receive() {
    std::vector<std::vector<uint8_t>> out;
    if (ext_) {
        ext_->receive(ext_->user, [](const uint8_t* b, uint32_t n, void* u) { ((std::vector<std::vector<uint8_t>>*)u)->emplace_back(b, b + n); }, &out);
        return out;
    }
    std::lock_guard<std::mutex> lock(mu_);
    out.swap(in_);
    return out;
}

void Link::run() {
    ENetAddress addr = {};
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;
    if (cfg_.role == Role::Host && cfg_.bind_v4) {
        addr = v4_address(cfg_.bind_v4, cfg_.port);
        host = enet_host_create(ENET_ADDRESS_TYPE_IPV4, &addr, 1, 2, 0, 0);
    } else if (cfg_.role == Role::Host) {
        // IPv6 any on a dual-stack socket (build_any leaves an ANY-typed address unset).
        enet_address_build_any(&addr, ENET_ADDRESS_TYPE_IPV6);
        addr.port = cfg_.port;
        host = enet_host_create(ENET_ADDRESS_TYPE_ANY, &addr, 1, 2, 0, 0);
    } else {
        // The client socket takes the resolved address's family.
        if (enet_address_set_host(&addr, ENET_ADDRESS_TYPE_ANY, cfg_.addr.c_str()) == 0) {
            addr.port = cfg_.port;
            host = enet_host_create(addr.type, nullptr, 1, 2, 0, 0);
            if (host) {
                peer = enet_host_connect(host, &addr, 2, 0);
                if (peer) {
                    enet_peer_timeout(peer, 0, 1000, 2000);
                }
            }
        }
    }
    if (!host) {
        fprintf(stderr, "[mp] cannot open an ENet host on %s port %u\n", cfg_.bind_v4 ? ipv4_text(cfg_.bind_v4).c_str() : "any address", cfg_.port);
        lost_ = true;
        return;
    }
    while (!quit_) {
        ENetEvent ev;
        while (enet_host_service(host, &ev, 1) > 0) {
            if (ev.type == ENET_EVENT_TYPE_CONNECT) {
                peer = ev.peer;
                enet_peer_timeout(peer, 0, 5000, 10000);
                last_rx_ms_ = now_ms();
                // A host takes a new player after losing one.
                lost_ = false;
                connected_ = true;
                ++connects_;
            } else if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                last_rx_ms_ = now_ms();
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    in_.emplace_back(ev.packet->data, ev.packet->data + ev.packet->dataLength);
                }
                enet_packet_destroy(ev.packet);
            } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT || ev.type == ENET_EVENT_TYPE_DISCONNECT_TIMEOUT) {
                if (connected_) {
                    lost_ = true;
                }
                connected_ = false;
                peer = nullptr;
            }
        }
        // A client that hasn't connected yet (the host not listening, or its attempt timed out) keeps trying.
        if (cfg_.role == Role::Client && !peer && !connected_ && !lost_) {
            peer = enet_host_connect(host, &addr, 2, 0);
            if (peer) {
                enet_peer_timeout(peer, 0, 1000, 2000);
            }
        }
        std::vector<Out> due;
        {
            std::lock_guard<std::mutex> lock(mu_);
            const auto now = std::chrono::steady_clock::now();
            while (!out_.empty() && out_.front().at <= now) {
                due.push_back(std::move(out_.front()));
                out_.pop_front();
            }
        }
        if (peer && connected_ && !due.empty()) {
            for (Out& o : due) {
                ENetPacket* p = enet_packet_create(o.bytes.data(), o.bytes.size(), o.reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
                enet_peer_send(peer, o.reliable ? 1 : 0, p);
            }
            enet_host_flush(host);
        }
        handled_ += due.size();
    }
    if (peer && connected_) {
        enet_peer_disconnect_now(peer, 0);
    }
    enet_host_destroy(host);
    connected_ = false;
}

}
