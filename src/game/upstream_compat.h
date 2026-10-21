// upstream_compat.h — public C-linkage surface of upstream_compat.cpp
// (libultra shim overrides + host frame-pacing state), declared once so the
// other host TUs stop re-`extern`-ing these by hand (which drifted).
#ifndef RS64_UPSTREAM_COMPAT_H
#define RS64_UPSTREAM_COMPAT_H

extern "C" {
    // ROGUESQ_VI_DRIVEN_LOOP: 1 = hardware VI protocol (default), 0 = host tokens.
    int rs64_vi_driven(void);
    // ROGUESQ_FB_GUARDS bitmask (0 = off).
    int rs64_fb_guards_mask(void);

    // Active overlay id (0..2), or -1; set by rs64_load_overlay.
    extern volatile int g_active_overlay;
    // Last framebuffer address handed to osViSwapBuffer.
    extern volatile unsigned g_last_swap_fb;
}

#endif // RS64_UPSTREAM_COMPAT_H
