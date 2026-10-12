// Must load: getNpcCurrentHealth is patched by the patches ELF, so the hook is regenerated from the patch and keeps its guard. Counts calls at 0x80C00030.
#include "rs64_api.h"

RECOMP_HOOK("getNpcCurrentHealth") void probe_npc_health(void) {
    (*(volatile unsigned int*)0x80C00030)++;
}
