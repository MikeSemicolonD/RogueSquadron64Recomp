#ifndef RS64_SHIPS_H
#define RS64_SHIPS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// The "ships" host-API service: the new hangar ships mods add (ships.json). The game publishes it before mods init; look it up with
// get_service. Every call comes from the game thread.
#define RS64_SHIPS_SERVICE "ships"
// Bump only when a field is appended; existing fields never change.
#define RS64_SHIPS_VERSION 1

typedef struct rs64_ships {
    uint32_t version;
    uint32_t size;
    // The ship this player has in view in the hangar, or flies once one is picked; "" for a stock craft.
    const char* (*shown)(void);
    // Nonzero when a mod here installs the ship `id` with its own model (pl_crafts/<id>).
    int (*has)(const char* id);
    // While set, an NPC wingman that spawns draws the ship `id`'s model (loaded beforehand) instead of its wingman model; NULL clears.
    void (*set_puppet)(const char* id);
} rs64_ships;

#ifdef __cplusplus
}
#endif
#endif
