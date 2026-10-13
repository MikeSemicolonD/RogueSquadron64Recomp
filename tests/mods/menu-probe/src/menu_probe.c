// Exports the menu file's @ behaviors: each writes to spare host RDRAM (0x80C00100..) after calling into the runtime, so a run shows they ran on a usable stack.
#include "rs64_api.h"

#include "recompconfig.h"
#include "recomputils.h"

#define ACTION_RUNS (*(volatile unsigned int*)0x80C00100)
#define ACTION_MODE (*(volatile unsigned int*)0x80C00104)
#define ACTION_LEVEL (*(volatile unsigned int*)0x80C00108)
#define TOGGLE_STATE (*(volatile unsigned int*)0x80C0010C)
#define ACTION_SPEED (*(volatile unsigned int*)0x80C00110)

RECOMP_EXPORT void probe_action(void) {
    ACTION_RUNS++;
    ACTION_MODE = recomp_get_config_u32("mode");
    ACTION_LEVEL = (unsigned int)recomp_get_config_double("level");
    ACTION_SPEED = recomp_get_config_u32("speed");
    recomp_printf("[menu_probe] action %u: mode %u level %u speed %u\n", ACTION_RUNS, ACTION_MODE, ACTION_LEVEL, ACTION_SPEED);
}

RECOMP_EXPORT void probe_toggle(void) {
    TOGGLE_STATE = !TOGGLE_STATE;
    recomp_printf("[menu_probe] toggle now %u\n", TOGGLE_STATE);
}

RECOMP_EXPORT unsigned int probe_toggle_get(void) {
    return TOGGLE_STATE;
}
