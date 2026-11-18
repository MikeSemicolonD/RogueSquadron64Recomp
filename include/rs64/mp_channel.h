#ifndef RS64_MP_CHANNEL_H
#define RS64_MP_CHANNEL_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// The "mp.channel" host-API service: online co-op (mods/multiplayer-native) publishes it so other mods can send their own data to the other player.
// Look it up with get_service after the registry seals and check version / size. Both players need the sending mod; a message on a channel nobody listens to is dropped.
#define RS64_MP_CHANNEL_SERVICE "mp.channel"
// Bump only when a field is appended; existing fields never change.
#define RS64_MP_CHANNEL_VERSION 1
// Largest payload send() takes, in bytes.
#define RS64_MP_CHANNEL_MAX 1024
// Longest channel name, in characters.
#define RS64_MP_CHANNEL_NAME_MAX 31

// One message the other player sent on a channel; runs on the game thread, so it may touch game state. data is valid only during the call.
typedef void (*rs64_mp_message_fn)(const uint8_t* data, uint32_t size, void* user);

typedef struct rs64_mp_channel {
    uint32_t version;
    uint32_t size;
    // Non-zero while connected to the other player (past the lobby's handshake), in the lobby or a mission.
    int (*connected)(void);
    // 0 for the host, 1 for the joining player, -1 when not connected.
    int (*player)(void);
    // Sends size bytes (up to RS64_MP_CHANNEL_MAX) to the other player on channel (1-31 characters; prefix it with your mod's id). reliable: delivered once, in order; otherwise it may be dropped (for state sent every frame).
    // 0 = sent, -1 not connected, -2 a bad channel name or size. From the game thread.
    int (*send)(const char* channel, const void* data, uint32_t size, int reliable);
    // fn receives every message on channel from now on; a second listen on the same channel replaces the first, NULL stops listening. 0, or -2 for a bad name. Any time.
    int (*listen)(const char* channel, rs64_mp_message_fn fn, void* user);
} rs64_mp_channel;

#ifdef __cplusplus
}
#endif
#endif
