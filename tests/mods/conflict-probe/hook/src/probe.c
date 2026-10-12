// Must load: hangarInitialize (menu overlay) carries rogue_squadron.toml hooks, which LiveRecomp replays when the hook regenerates it. Counts calls at 0x80C00040.
#include "rs64_api.h"

RECOMP_HOOK("hangarInitialize") void probe_hangar_init(void) {
    (*(volatile unsigned int*)0x80C00040)++;
}
