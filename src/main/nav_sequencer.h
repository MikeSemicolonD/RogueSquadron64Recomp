// State-gated BOOT_TARGET navigation sequencer: drives the game's own menus to a
// target (level / cutscene / demo) instead of poking state-machine fields.
// See plans/2026-09-22-boot-target-nav-engine-design.md.
#pragma once
#include <cstdint>

enum RsNavKind { NAV_NONE = 0, NAV_LEVEL, NAV_CUTSCENE, NAV_DEMO, NAV_ABORT };
struct RsNavTarget { RsNavKind kind; int a; int b; };  // level: a=level b=craft; cutscene/demo: a=index

extern "C" {
    RsNavTarget rs64_nav_parse(const char* boot_target);
    void rs64_nav_set_target(const char* boot_target);  // called once from the render context
    void rs64_nav_inject(uint16_t buttons, float x, float y);  // stage a one-shot controller state
    bool rs64_nav_consume(uint16_t* buttons, float* x, float* y);  // get_n64_input drains it
    // overlay: g_active_overlay (0 mission, 1 menu, 2 cinematic, -1 none).
    void rs64_nav_tick(uint8_t* rdram, int overlay);  // called each present; advances the step machine
}
