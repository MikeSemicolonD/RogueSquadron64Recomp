// The browser build's network: ENet's sockets (web_socket.c) carried as WebTransport datagrams by the page's main thread (rs64net.js).
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Chrome takes datagrams up to 1210 bytes; a web ENet host asks for this MTU so its connect handshake negotiates it down.
#define RS64_WEB_ENET_MTU 1200

// The certificate hash a relay printed for browser players (its address's part after '#'), for the ip:port ENet sends to. 0 on success.
int rs64_web_net_set_hash(const char* ip, uint16_t port, const char* sha256_hex);

#ifdef __cplusplus
}
#endif
