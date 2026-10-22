#ifndef RS64_CAMERA_SHOTS_H
#define RS64_CAMERA_SHOTS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// The "camera.shots" host-API service: mid-mission camera shots that mods link to level triggers (camera_shots.txt). The game
// publishes it before mods init; look it up with get_service. Every call comes from the game thread.
#define RS64_CAMERA_SHOTS_SERVICE "camera.shots"
// Bump only when a field is appended; existing fields never change.
#define RS64_CAMERA_SHOTS_VERSION 1

typedef void (*rs64_shot_fn)(const char* camera, const char* trigger, void* user);

typedef struct rs64_camera_shots {
    uint32_t version;
    uint32_t size;
    // Queues the shot of this level's camera item named `camera`, set off by the level trigger `trigger`, for the running mission; it
    // starts once no shot, mission transition or pause is in the way. Ignored when that trigger already fired here, dropped (logged)
    // when the level has no such camera item.
    void (*play)(const char* camera, const char* trigger);
    // fn is called for a shot one of this game's level triggers set off that the other player should see (not for play()): a shared
    // shot as it starts here, or a personal one this game applied for the other player; one listener, NULL clears.
    void (*set_listener)(rs64_shot_fn fn, void* user);
} rs64_camera_shots;

#ifdef __cplusplus
}
#endif
#endif
