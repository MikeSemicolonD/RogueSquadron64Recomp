// Must load with a warning: a force patch deliberately replaces the base game's patched getNpcCurrentHealth.
#include "rs64_api.h"

RECOMP_FORCE_PATCH unsigned int getNpcCurrentHealth(void* npc) {
    return 1;
}
