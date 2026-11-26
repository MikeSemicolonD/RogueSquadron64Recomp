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
#define RS64_SHIPS_VERSION 3

typedef struct rs64_ships {
    uint32_t version;
    uint32_t size;
    // The ship this player has in view in the hangar, or flies once one is picked; "" for a stock craft.
    const char* (*shown)(void);
    // Nonzero when a mod here installs the ship `id` with its own model (pl_crafts/<id>).
    int (*has)(const char* id);
    // While set, an NPC wingman that spawns draws the ship `id`'s model (loaded beforehand) instead of its wingman model; NULL clears.
    void (*set_puppet)(const char* id);
    // Version 2: the craft a hangar bay shows (bays 1..8 stock, 9 and up the ships, in their template's craft id); -1 out of range.
    int (*bay_craft)(int bay);
    // Version 3: the flying ship's special position this frame, 0 (rest) .. 1 (engaged: S-foils shut, boost on); -1 when no ship flies.
    float (*special)(void);
    // Version 3: poses an NPC drawing ship `id` (model = its model instance) at special position t: the ship's clip, else its S-foil
    // wings. Nonzero when `id` is a ship here, 0 when the caller should pose the model itself.
    int (*pose_puppet)(uint8_t* rdram, void* ctx, const char* id, uint32_t model, float t);
} rs64_ships;

#ifdef __cplusplus
}
#endif
#endif
