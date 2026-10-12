// mainGameLoop carries only co-op relocation lui patches; runInMissionFrame carries native hooks, some of which return mid-function. Counts entries at 0x80C00044 / 0x80C00048.
#include "rs64_api.h"

RECOMP_HOOK("mainGameLoop") void probe_main_loop(void) {
    (*(volatile unsigned int*)0x80C00044)++;
}

RECOMP_HOOK("runInMissionFrame") void probe_mission_frame(void) {
    (*(volatile unsigned int*)0x80C00048)++;
}
