// Browser test of ENet over WebTransport (web/web_socket.c): two ENet clients reach a relay built with RELAY_WEBTRANSPORT, one hosts,
// the other finds the game in the list and joins, then a full-size game packet (fragmented past one datagram) crosses each way intact.
// cmake --build build-web --target enet_web_test; serve build-web and open enet_web_test.html?port=<browser_port>&hash=<sha256>.
#include "relay_protocol.h"
#include "web_net.h"
#include <enet6/enet.h>
#include <emscripten.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace rs64::relay;

namespace {

constexpr uint32_t kBuild = 0x52570001u;
int g_failures = 0;

void check(bool ok, const char* what)
{

    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);

    if (!ok) ++g_failures;
}

struct Client {
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;
    bool connected = false;
    std::vector<std::vector<uint8_t>> inbox;
};

void pump(Client& c, int ms)
{

    ENetEvent ev;

    while (enet_host_service(c.host, &ev, (enet_uint32)ms) > 0) {
        if (ev.type == ENET_EVENT_TYPE_CONNECT) c.connected = true;

        if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
            c.inbox.emplace_back(ev.packet->data, ev.packet->data + ev.packet->dataLength);
            enet_packet_destroy(ev.packet);
        }

        ms = 0;
    }
}

void send(Client& c, const std::vector<uint8_t>& bytes)
{
    enet_peer_send(c.peer, kChannelReliable, enet_packet_create(bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE));
}

// Services both clients until one of them holds a message of this type (up to 5 s); returns it, or an empty vector.
std::vector<uint8_t> await(Client& a, Client& b, Client& want, Type type)
{

    for (int i = 0; i < 500; ++i) {
        pump(a, 5);
        pump(b, 5);

        for (size_t k = 0; k < want.inbox.size(); ++k) {
            if (message_type(want.inbox[k].data(), want.inbox[k].size()) == (uint8_t)type) {
                std::vector<uint8_t> m = want.inbox[k];

                want.inbox.erase(want.inbox.begin() + (long)k);
                return m;
            }
        }
    }

    return {};
}

}

int main(int argc, char** argv)
{

    if (argc < 3) {
        std::printf("FAIL usage: enet_web_test.html?port=<browser_port>&hash=<sha256>\n");
        return 1;
    }

    const uint16_t port = (uint16_t)std::atoi(argv[1]);
    ENetAddress relay;

    check(enet_initialize() == 0, "enet_initialize");
    check(rs64_web_net_set_hash("127.0.0.1", port, argv[2]) == 0, "relay certificate hash accepted");
    enet_address_set_host_ip(&relay, "127.0.0.1");
    relay.port = port;

    Client host_side;
    Client guest;

    for (Client* c : { &host_side, &guest }) {
        c->host = enet_host_create(ENET_ADDRESS_TYPE_IPV4, nullptr, 1, kChannels, 0, 0);
        c->host->mtu = RS64_WEB_ENET_MTU;
        c->peer = enet_host_connect(c->host, &relay, kChannels, 0);
    }

    for (int i = 0; i < 500 && !(host_side.connected && guest.connected); ++i) {
        pump(host_side, 5);
        pump(guest, 5);
    }

    check(host_side.connected && guest.connected, "both clients connect to the relay");

    send(host_side, encode_register(kBuild, false, "WEB TEST"));

    const std::vector<uint8_t> reg = await(host_side, guest, host_side, Type::Registered);
    uint32_t id = 0;
    std::string name;

    check(decode_registered(reg.data(), reg.size(), &id, &name) && id != 0, "host registers a game");

    send(guest, encode_list(kBuild));

    const std::vector<uint8_t> list = await(host_side, guest, guest, Type::ListReply);
    ListReply listed;
    bool found = false;

    check(decode_list_reply(list.data(), list.size(), &listed), "guest gets the list");

    for (const GameEntry& g : listed.games) found = found || g.id == id;

    check(found, "the list holds the host's game");

    send(guest, encode_join(id, kBuild));
    check(!await(host_side, guest, guest, Type::Joined).empty(), "guest joins");
    check(!await(host_side, guest, host_side, Type::PeerJoined).empty(), "host hears the guest join");

    std::vector<uint8_t> payload(kMaxGamePacket);

    for (size_t i = 0; i < payload.size(); ++i) payload[i] = (uint8_t)(i * 7 + 3);

    for (int round = 0; round < 20; ++round) {
        Client& from = (round & 1) ? guest : host_side;
        Client& to = (round & 1) ? host_side : guest;

        payload[0] = (uint8_t)round;
        send(from, encode_data(payload.data(), payload.size()));

        const std::vector<uint8_t> got = await(host_side, guest, to, Type::Data);
        const uint8_t* bytes = nullptr;
        size_t len = 0;

        if (!decode_data(got.data(), got.size(), &bytes, &len) || len != payload.size() || std::vector<uint8_t>(bytes, bytes + len) != payload) {
            check(false, "a full-size packet crosses intact");
            break;
        }

        if (round == 19) check(true, "20 full-size packets cross intact, both ways");
    }

    std::printf("enet_web_test: %s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
