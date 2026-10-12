// Must be refused: getNpcCurrentHealth carries a rogue_squadron.toml entry hook.
#include "rs64_api.h"

RECOMP_PATCH unsigned int getNpcCurrentHealth(void* npc) {
    return 1;
}
