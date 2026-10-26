#pragma once
#include <cstdint>
#include <map>
#include "relay_core.h"

struct _ENetHost;
struct _ENetPeer;

namespace rs64::relay {

int64_t now_ms();

// The relay's network side: one dual-stack UDP socket (IPv4 and IPv6) whose events go to a Relay and whose actions it carries out. One thread.
class Host {
public:
    ~Host() { close(0); }
    bool open(uint16_t port);
    // Services the socket for up to timeout_ms, feeding relay and carrying out its actions as they come.
    void service(Relay& relay, int timeout_ms);
    // Carries out actions the relay queued outside service() (console commands, tick).
    void apply(Relay& relay);
    // Lets queued disconnects reach the players (up to wait_ms), then closes.
    void close(int wait_ms = 500);

private:
    _ENetHost* host_ = nullptr;
    std::map<uint32_t, _ENetPeer*> peers_;
    uint32_t next_peer_ = 1;
};

}
