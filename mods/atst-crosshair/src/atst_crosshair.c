// AT-ST Crosshair: playerAtStUpdate keeps CROSSHAIRS in the walker (+0xAF) and clears the setting every frame, which hides the rings. They also sit on a line from the body, close to it, while the bolts leave the guns on the head.

#include "rs64_api.h"

#define CROSSHAIR_ON    (*(volatile unsigned char*)0x8010CA35)
#define CURRENT_LEVEL   (*(volatile unsigned int*)0x80130B70)
#define LEVEL_CHICKEN   20u
#define KEPT_SETTING(w) (*(volatile unsigned char*)((w) + 0xAF))
#define MODEL(w)        (*(void* volatile*)((w) + 0x30))

// World distances of the rings along the line of fire. The game puts them 2 and 6 forward vectors from the frame position, so RING_STEP stretches the vector and RING_BACK moves the start.
#define RING_NEAR 4.0f
#define RING_FAR  16.0f
#define RING_STEP ((RING_FAR - RING_NEAR) / 4.0f)
#define RING_BACK (2.0f * RING_STEP - RING_NEAR)

// Fills `out` with a named bone's position, forward and up in world space; 0 if the model has no such bone.
int lookupHmtTextureByName(void* model, const char* name, int index, float* out, void* matrix, int flags);

static char* walker;
static float* aimedFrame;
static float bodyFrame[6];

RECOMP_HOOK("updatePlayerAtStBehavior") void atstCrosshairTick(char* w) {
    walker = w;
    CROSSHAIR_ON = KEPT_SETTING(w);
}

// The rings read only the frame's position and forward (+0xC): for this call they are the middle of the two guns and the direction spawnPlayerAtStCannonFire fires along.
RECOMP_HOOK("updatePlayerHudFrame") void atstCrosshairAim(unsigned int player, void* view, float* frame) {
    float gunA[9];
    float gunB[9];

    if (CURRENT_LEVEL != LEVEL_CHICKEN || !walker) {
        return;
    }

    if (!lookupHmtTextureByName(MODEL(walker), "a", 0, gunA, 0, 0) || !lookupHmtTextureByName(MODEL(walker), "b", 0, gunB, 0, 0)) {
        return;
    }

    for (int i = 0; i < 3; i++) {
        bodyFrame[i] = frame[i];
        bodyFrame[3 + i] = frame[3 + i];
        frame[i] = (gunA[i] + gunB[i]) * 0.5f - gunA[3 + i] * RING_BACK;
        frame[3 + i] = gunA[3 + i] * RING_STEP;
    }

    aimedFrame = frame;
}

RECOMP_HOOK_RETURN("updatePlayerHudFrame") void atstCrosshairRestore(void) {
    if (!aimedFrame) {
        return;
    }

    for (int i = 0; i < 6; i++) {
        aimedFrame[i] = bodyFrame[i];
    }

    aimedFrame = 0;
}
