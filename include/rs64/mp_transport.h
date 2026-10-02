#ifndef RS64_MP_TRANSPORT_H
#define RS64_MP_TRANSPORT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// The "mp.transport" host-API service: a mod (mods/steam-relay) publishes one with add_service, and multiplayer looks it up after the registry seals and uses it in place of ENet while available() is true.
// Every call comes from the game thread; the transport runs its own network thread. Messages are whole byte buffers, in order per reliability class.
#define RS64_MP_TRANSPORT_SERVICE "mp.transport"
// Bump only when a field is appended; existing fields never change.
#define RS64_MP_TRANSPORT_VERSION 1

#define RS64_MP_ROLE_HOST 1
#define RS64_MP_ROLE_CLIENT 2

typedef void (*rs64_mp_recv_fn)(const uint8_t* bytes, uint32_t len, void* fn_user);

typedef struct rs64_mp_transport {
    uint32_t version;
    uint32_t size;
    // Lowercase id for logs and ROGUESQ_MP_TRANSPORT ("steam").
    const char* name;
    // Passed back as the first argument of every call.
    void* user;
    // Non-zero when start() can work now; may initialize lazily, so call it only when the player is about to host or join.
    int (*available)(void* user);
    // peer: the client's target as the player entered it (a join code) or as pending_join returned it, NULL for the host. build: multiplayer's protocol build id; a host advertises it and a client only joins a matching host. Non-zero = started; the outcome arrives through connected() / lost().
    int (*start)(void* user, int role, const char* peer, uint32_t build);
    // Ends the session and discards queued messages; start() may run again.
    void (*stop)(void* user);
    // stop() plus releasing the backend (Steam shuts down); the next available() initializes again.
    void (*shutdown)(void* user);
    int (*connected)(void* user);
    // A connected peer went away (or a client's join failed for good); cleared by the next connect or stop().
    int (*lost)(void* user);
    // Connections made since start(), so a drop and reconnect between two polls is still seen.
    uint32_t (*connect_count)(void* user);
    void (*send)(void* user, const uint8_t* bytes, uint32_t len, int reliable);
    // Waits up to timeout_ms for everything sent so far to leave this machine; non-zero on success.
    int (*flush)(void* user, int timeout_ms);
    // Calls fn once per received message, oldest first, and forgets them.
    void (*receive)(void* user, rs64_mp_recv_fn fn, void* fn_user);
    // Menu-font-safe text (uppercase, digits, spaces); "" for none. status: why the transport is or is not in use; host_label: what the host shows the other player (a join code).
    const char* (*status)(void* user);
    const char* (*host_label)(void* user);
    // A peer the player asked to join from outside the game's menus (a Steam friends-list join), or NULL; returning it clears it.
    const char* (*pending_join)(void* user);
} rs64_mp_transport;

#ifdef __cplusplus
}
#endif
#endif
