// hook_helpers.h — public C-linkage surface of hook_helpers.cpp (host entry
// points for the rogue_squadron.toml hooks + shared pacing/scene state),
// declared once so the other host TUs stop re-`extern`-ing these by hand.
#ifndef RS64_HOOK_HELPERS_H
#define RS64_HOOK_HELPERS_H

#include <cstdint>

extern "C" {
    // Per-VI tick counter (bumped by the VI hook).
    extern volatile unsigned g_vi_tick;
    // Boot START-pulse gate for the attract-title handoff.
    extern volatile int g_boot_pulse_start;
    // menuOverlayInit action id; 9 = attribution.
    extern volatile int g_current_scene;

    // Current cinematic loop iteration (for RDRAM-dump arming).
    unsigned long long rs64_cine_iter_get(void);
    // Rewrite a matpool-region SET_COLOR_IMAGE to a safe target (FB guard).
    void rs64_neutralize_matpool_cimg(uint8_t* rdram, uint32_t dl_phys);
}

#endif // RS64_HOOK_HELPERS_H
