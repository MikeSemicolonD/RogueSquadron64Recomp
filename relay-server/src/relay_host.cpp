#include "relay_host.h"
#include <enet6/enet.h>
#include <chrono>

namespace rs64::relay {

namespace {

std::string peer_ip(const ENetPeer* peer)
{

    char ip[64] = "?";

    enet_address_get_host_ip(&peer->address, ip, sizeof ip);
    return ip;
}

}

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool Host::open(uint16_t port)
{

    if (host_ || enet_initialize() != 0)
    {
        return false;
    }

    // IPv6 any on a dual-stack socket takes IPv4 players too.
    ENetAddress addr = {};

    enet_address_build_any(&addr, ENET_ADDRESS_TYPE_IPV6);
    addr.port = port;
    host_ = enet_host_create(ENET_ADDRESS_TYPE_ANY, &addr, kPeerCeiling, kChannels, 0, 0);

    if (!host_) {
        enet_deinitialize();
        return false;
    }

    host_->maximumPacketSize = kMaxPacket;
    return true;
}

void Host::service(Relay& relay, int timeout_ms)
{

    ENetEvent ev;

    while (host_ && enet_host_service(host_, &ev, (enet_uint32)timeout_ms) > 0) {
        const uint32_t id = (uint32_t)(uintptr_t)ev.peer->data;

        switch (ev.type) {
        case ENET_EVENT_TYPE_CONNECT:
            ev.peer->data = (void*)(uintptr_t)next_peer_;
            peers_[next_peer_] = ev.peer;
            relay.on_connect(next_peer_++, peer_ip(ev.peer), now_ms());
            break;

        case ENET_EVENT_TYPE_RECEIVE: {
            const bool reliable = (ev.packet->flags & ENET_PACKET_FLAG_RELIABLE) != 0;

            if (id) relay.on_receive(id, reliable, ev.packet->data, ev.packet->dataLength, now_ms());

            enet_packet_destroy(ev.packet);
            break;
        }

        case ENET_EVENT_TYPE_DISCONNECT:
        case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT:
            if (id) {
                relay.on_disconnect(id);
                peers_.erase(id);
            }

            break;

        default:
            break;
        }

        // An action may close a peer that a later event in this batch names; carry them out before the next event.
        apply(relay);
        timeout_ms = 0;
    }
}

// A closed peer's data is cleared so its later DISCONNECT event is not reported to the relay again.
void Host::apply(Relay& relay)
{

    for (Action& a : relay.take_actions()) {
        auto p = peers_.find(a.peer);

        if (p == peers_.end()) continue;

        if (a.kind == Action::Kind::Send) {
            const uint8_t channel = a.reliable ? kChannelReliable : kChannelUnreliable;

            enet_peer_send(p->second, channel, enet_packet_create(a.bytes.data(), a.bytes.size(), a.reliable ? ENET_PACKET_FLAG_RELIABLE : 0));
            continue;
        }

        p->second->data = nullptr;
        enet_peer_disconnect_later(p->second, (enet_uint32)a.reason);
        peers_.erase(p);
    }

    if (host_) enet_host_flush(host_);
}

void Host::close(int wait_ms)
{

    if (!host_)
    {
        return;
    }

    const int64_t until = now_ms() + wait_ms;
    ENetEvent ev;

    while (host_->connectedPeers > 0 && now_ms() < until) {
        if (enet_host_service(host_, &ev, 10) > 0 && ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
    }

    enet_host_destroy(host_);
    host_ = nullptr;
    peers_.clear();
    enet_deinitialize();
}

}
