// ENet's sockets in the browser build: each remote address is a WebTransport session the page's main thread owns (rs64net.js).
// enet6's unix.c is compiled with its socket functions renamed (mods/multiplayer-native/CMakeLists.txt), so these take their names.
#define ENET_BUILDING_LIB 1
#include "enet6/enet.h"
#include "web_net.h"

#include <emscripten.h>
#include <emscripten/threading.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { kSockets = 8, kSessions = 16, kHashes = 8, kSlots = 256, kSlotBytes = 1536, kSocketBase = 100 };

// One received datagram and who sent it.
typedef struct {
    ENetAddress from;
    uint16_t len;
    uint8_t data[kSlotBytes];
} Slot;

// Single producer (the main thread's datagram reader), single consumer (the ENet host's thread).
typedef struct {
    _Atomic uint32_t head;
    _Atomic uint32_t tail;
    Slot slots[kSlots];
} Ring;

typedef struct {
    int used;
    ENetAddressType type;
    Ring* ring;
} WebSocket;

typedef struct {
    int used;
    int socket;
    ENetAddress remote;
} Session;

typedef struct {
    int used;
    ENetAddress addr;
    char hex[65];
} Hash;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static WebSocket g_sockets[kSockets];
static Session g_sessions[kSessions];
static Hash g_hashes[kHashes];

static WebSocket* socket_of(ENetSocket s)
{
    const int i = (int)s - kSocketBase;

    return (i >= 0 && i < kSockets && g_sockets[i].used) ? &g_sockets[i] : NULL;
}

static int same_address(const ENetAddress* a, const ENetAddress* b)
{
    if (a->type != b->type || a->port != b->port)
        return 0;

    return a->type == ENET_ADDRESS_TYPE_IPV4 ? memcmp(a->host.v4, b->host.v4, sizeof a->host.v4) == 0 : memcmp(a->host.v6, b->host.v6, sizeof a->host.v6) == 0;
}

int rs64_web_net_set_hash(const char* ip, uint16_t port, const char* sha256_hex)
{
    ENetAddress a;
    int slot = -1;

    if (strlen(sha256_hex) != 64 || enet_address_set_host_ip(&a, ip) != 0)
        return -1;

    a.port = port;
    pthread_mutex_lock(&g_mu);

    for (int i = 0; i < kHashes && slot < 0; ++i) {
        if (g_hashes[i].used && same_address(&g_hashes[i].addr, &a))
            slot = i;
    }

    for (int i = 0; i < kHashes && slot < 0; ++i) {
        if (!g_hashes[i].used)
            slot = i;
    }

    if (slot >= 0) {
        g_hashes[slot].used = 1;
        g_hashes[slot].addr = a;
        memcpy(g_hashes[slot].hex, sha256_hex, 65);
    }

    pthread_mutex_unlock(&g_mu);
    return slot >= 0 ? 0 : -1;
}

// The session to this address from this socket, opening one when the relay's hash is known; 0 when it can't be reached. Caller holds g_mu.
static int session_for(ENetSocket socket, const ENetAddress* to)
{
    const Hash* hash = NULL;
    int free_slot = -1;

    for (int i = 0; i < kSessions; ++i) {
        if (g_sessions[i].used && g_sessions[i].socket == (int)socket && same_address(&g_sessions[i].remote, to))
            return i + 1;

        if (!g_sessions[i].used && free_slot < 0)
            free_slot = i;
    }

    for (int i = 0; i < kHashes && !hash; ++i) {
        if (g_hashes[i].used && same_address(&g_hashes[i].addr, to))
            hash = &g_hashes[i];
    }

    if (!hash || free_slot < 0)
        return 0;

    char ip[64];
    char* url = malloc(96);
    char* hex = strdup(hash->hex);

    // enet6 prints the port too when it is set.
    ENetAddress bare = *to;

    bare.port = 0;
    enet_address_get_host_ip(&bare, ip, sizeof ip);
    snprintf(url, 96, to->type == ENET_ADDRESS_TYPE_IPV6 ? "https://[%s]:%u/" : "https://%s:%u/", ip, to->port);
    g_sessions[free_slot] = (Session){ 1, (int)socket, *to };

    // The strings are freed by the main thread once it has read them.
    MAIN_THREAD_ASYNC_EM_ASM({ Module.rs64Net.open($0, $1, $2); }, free_slot + 1, url, hex);
    return free_slot + 1;
}

EMSCRIPTEN_KEEPALIVE void rs64_net_deliver(int session, const uint8_t* data, int len)
{
    pthread_mutex_lock(&g_mu);

    const Session* s = (session >= 1 && session <= kSessions && g_sessions[session - 1].used) ? &g_sessions[session - 1] : NULL;
    WebSocket* ws = s ? socket_of(s->socket) : NULL;

    if (ws && len > 0 && len <= kSlotBytes) {
        Ring* r = ws->ring;
        const uint32_t head = atomic_load_explicit(&r->head, memory_order_relaxed);

        if (head - atomic_load_explicit(&r->tail, memory_order_acquire) < kSlots) {
            Slot* slot = &r->slots[head % kSlots];

            slot->from = s->remote;
            slot->len = (uint16_t)len;
            memcpy(slot->data, data, (size_t)len);
            atomic_store_explicit(&r->head, head + 1, memory_order_release);
            emscripten_futex_wake((void*)&r->head, 1);
        }
    }

    pthread_mutex_unlock(&g_mu);
}

// The session ended (closed, refused or failed): the next send to that address opens a new one.
EMSCRIPTEN_KEEPALIVE void rs64_net_closed(int session)
{
    pthread_mutex_lock(&g_mu);

    if (session >= 1 && session <= kSessions)
        g_sessions[session - 1].used = 0;

    pthread_mutex_unlock(&g_mu);
}

ENetSocket enet_socket_create(ENetAddressType type, ENetSocketType socketType)
{
    ENetSocket out = ENET_SOCKET_NULL;

    if (socketType != ENET_SOCKET_TYPE_DATAGRAM)
        return ENET_SOCKET_NULL;

    pthread_mutex_lock(&g_mu);

    for (int i = 0; i < kSockets && out == ENET_SOCKET_NULL; ++i) {
        if (!g_sockets[i].used) {
            g_sockets[i].ring = calloc(1, sizeof(Ring));

            if (g_sockets[i].ring) {
                g_sockets[i].used = 1;
                g_sockets[i].type = type;
                out = kSocketBase + i;
            }
        }
    }

    pthread_mutex_unlock(&g_mu);
    return out;
}

void enet_socket_destroy(ENetSocket socket)
{
    pthread_mutex_lock(&g_mu);

    WebSocket* ws = socket_of(socket);

    if (ws) {
        for (int i = 0; i < kSessions; ++i) {
            if (g_sessions[i].used && g_sessions[i].socket == (int)socket) {
                g_sessions[i].used = 0;
                MAIN_THREAD_ASYNC_EM_ASM({ Module.rs64Net.close($0); }, i + 1);
            }
        }

        free(ws->ring);
        ws->ring = NULL;
        ws->used = 0;
    }

    pthread_mutex_unlock(&g_mu);
}

int enet_socket_send(ENetSocket socket, const ENetAddress* address, const ENetBuffer* buffers, size_t bufferCount)
{
    size_t total = 0;

    if (!address)
        return -1;

    for (size_t i = 0; i < bufferCount; ++i) total += buffers[i].dataLength;

    pthread_mutex_lock(&g_mu);

    const int session = socket_of(socket) ? session_for(socket, address) : 0;

    pthread_mutex_unlock(&g_mu);

    if (!session)
        return -1;

    uint8_t* copy = malloc(total ? total : 1);
    size_t at = 0;

    for (size_t i = 0; i < bufferCount; ++i) {
        memcpy(copy + at, buffers[i].data, buffers[i].dataLength);
        at += buffers[i].dataLength;
    }

    // The main thread writes it to the session and frees it.
    MAIN_THREAD_ASYNC_EM_ASM({ Module.rs64Net.send($0, $1, $2); }, session, copy, (int)total);
    return (int)total;
}

int enet_socket_receive(ENetSocket socket, ENetAddress* address, ENetBuffer* buffers, size_t bufferCount)
{
    WebSocket* ws = socket_of(socket);

    if (!ws || bufferCount == 0)
        return -1;

    Ring* r = ws->ring;
    const uint32_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);

    if (tail == atomic_load_explicit(&r->head, memory_order_acquire))
        return 0;

    const Slot* slot = &r->slots[tail % kSlots];

    // ENet receives into one buffer; a datagram too big for it is dropped, as a truncated UDP read is.
    const int len = slot->len <= buffers[0].dataLength ? slot->len : -2;

    if (len > 0)
        memcpy(buffers[0].data, slot->data, (size_t)len);

    if (address)
        *address = slot->from;

    atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
    return len;
}

int enet_socket_wait(ENetSocket socket, enet_uint32* condition, enet_uint32 timeout)
{
    WebSocket* ws = socket_of(socket);

    if (*condition & ENET_SOCKET_WAIT_SEND) {
        *condition = ENET_SOCKET_WAIT_SEND;
        return 0;
    }

    if (!ws)
        return -1;

    Ring* r = ws->ring;
    const double until = emscripten_get_now() + timeout;

    for (;;) {
        const uint32_t head = atomic_load_explicit(&r->head, memory_order_acquire);

        if (head != atomic_load_explicit(&r->tail, memory_order_relaxed)) {
            *condition = ENET_SOCKET_WAIT_RECEIVE;
            return 0;
        }

        const double left = until - emscripten_get_now();

        if (left <= 0) {
            *condition = ENET_SOCKET_WAIT_NONE;
            return 0;
        }

        emscripten_futex_wait((void*)&r->head, head, left);
    }
}

int enet_socketset_select(ENetSocket maxSocket, ENetSocketSet* readSet, ENetSocketSet* writeSet, enet_uint32 timeout)
{
    (void)maxSocket;
    (void)readSet;
    (void)writeSet;
    emscripten_thread_sleep(timeout);
    return 0;
}

int enet_socket_bind(ENetSocket socket, const ENetAddress* address)
{
    (void)address;
    return socket_of(socket) ? 0 : -1;
}

int enet_socket_get_address(ENetSocket socket, ENetAddress* address)
{
    WebSocket* ws = socket_of(socket);

    if (!ws)
        return -1;

    enet_address_build_any(address, ws->type == ENET_ADDRESS_TYPE_IPV4 ? ENET_ADDRESS_TYPE_IPV4 : ENET_ADDRESS_TYPE_IPV6);
    return 0;
}

int enet_socket_set_option(ENetSocket socket, ENetSocketOption option, int value)
{
    (void)option;
    (void)value;
    return socket_of(socket) ? 0 : -1;
}

int enet_socket_get_option(ENetSocket socket, ENetSocketOption option, int* value)
{
    (void)option;
    *value = 0;
    return socket_of(socket) ? 0 : -1;
}

int enet_socket_listen(ENetSocket socket, int backlog)
{
    (void)socket;
    (void)backlog;
    return -1;
}

int enet_socket_connect(ENetSocket socket, const ENetAddress* address)
{
    (void)socket;
    (void)address;
    return -1;
}

ENetSocket enet_socket_accept(ENetSocket socket, ENetAddress* address)
{
    (void)socket;
    (void)address;
    return ENET_SOCKET_NULL;
}

int enet_socket_shutdown(ENetSocket socket, ENetSocketShutdown how)
{
    (void)socket;
    (void)how;
    return -1;
}
