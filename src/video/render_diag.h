#pragma once
// Env-gated diagnostics of the RT64 render context (rt64_render_context.cpp): traces, RDRAM watches and dumps, built on a view of the context's state.
#include <chrono>
#include <cstdint>
#include "ultramodern/ultra64.h"

namespace RT64 { struct Application; }

namespace rs64::video {

struct RenderDiag {
    RT64::Application* app;
    int vi_count_;

    // VI origin/width as the game set them, before apply_vi_overrides.
    uint32_t game_vi_origin_;
    uint32_t game_vi_width_;

    // RDRAM accessors by physical address, byte-swapped (index ^ 3) and bounds-checked.
    uint8_t rd8(uint32_t addr) const;
    uint32_t rd32(uint32_t addr) const;
    void wr8(uint32_t addr, uint8_t v);
    void wr32(uint32_t addr, uint32_t v);

    int workload_cursor() const;
    int present_cursor() const;

    void frame_profile(std::chrono::high_resolution_clock::time_point pres0);
    void watch_rdram();
    void data_bp_tick();
    void dump_ucode_once(const OSTask* task);
    void log_task(const OSTask* task);
    void log_transition_lum();
    void log_gamestate();
    void dump_rdram_if_armed();
    void log_vi_state();
};

}
