// State-gated BOOT_TARGET navigation sequencer: drives the game's own menus to a
// target (level / cutscene / demo) instead of poking state-machine fields.
#pragma once
#include <cstdint>

enum RsNavKind { NAV_NONE = 0, NAV_LEVEL, NAV_CUTSCENE, NAV_DEMO, NAV_ABORT, NAV_LOBBY, NAV_MENU };
struct RsNavTarget { RsNavKind kind; int a; int b; int c = -1; };  // level: a=level b=craft; cutscene/demo: a=index; lobby: a=level b=0 host / 1 join c=craft

RsNavTarget rs64_nav_parse(const char* boot_target);

extern "C" {
    void rs64_nav_set_target(const char* boot_target);  // called once from the render context
    void rs64_nav_inject(uint16_t buttons, float x, float y);  // stage a one-shot controller state
    bool rs64_nav_consume(uint16_t* buttons, float* x, float* y);  // get_n64_input drains it
    // overlay: g_active_overlay (0 mission, 1 menu, 2 cinematic, -1 none).
    void rs64_nav_tick(uint8_t* rdram, int overlay);  // called each present; advances the step machine
    // Called per graphics task: the step watchdog counts game frames.
    void rs64_nav_frame();

    // ROGUESQ_NAV_CHAIN=1 (smoke tests): an abort target holds each mission until this names the next level, then aborts to SELECT LEVEL and starts it.
    void rs64_nav_chain_next(int level);

    // Back out to the main menu, then select this menu path (labels joined by /), as a menu: boot target does.
    void rs64_nav_menu_next(const char* path);
    // Craft-select hangar frames run since boot; it advances only while the hangar is up.
    uint32_t rs64_nav_hangar_frame_count(void);
    // True while a boot target is still driving the game.
    bool rs64_nav_active(void);
}
