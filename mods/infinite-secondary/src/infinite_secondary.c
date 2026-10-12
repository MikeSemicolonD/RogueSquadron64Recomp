// Infinite Secondary: every craft/weapon starts the mission at 0xFF, which the game treats as unlimited (the decrement at 0x800FD2E8 skips it).

#include "rs64_api.h"

RECOMP_PATCH unsigned int getSecondaryWeaponCount(unsigned int craft, unsigned int weapon) {
    return 0xFF;
}
