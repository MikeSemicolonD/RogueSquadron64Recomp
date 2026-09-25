#include <memory>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <thread>
#include <vector>
#include <filesystem>
#include <fstream>
#include "SDL.h"
#include "common/rt64_user_configuration.h"
#include "video_config.h"

#ifndef HLSL_CPU
#define HLSL_CPU
#endif
#include "hle/rt64_application.h"

#include "ultramodern/ultramodern.hpp"
#include "ultramodern/renderer_context.hpp"
#include "debug_logs.h"
#include "upstream_compat.h"      // rs64_vi_driven, rs64_fb_guards_mask, g_active_overlay, g_last_swap_fb
#include "hook_helpers.h"         // g_boot_pulse_start, g_current_scene, rs64_cine_iter_get, rs64_neutralize_matpool_cimg
#include "game_state.h"           // rs64_state_poll, rs64_state_current_id
#include "nav_sequencer.h"        // rs64_nav_set_target, rs64_nav_tick
#include "rt64_render_context.h"  // create_render_context + submit_rdp_range (defined below)
#include "../rsp/dpc_bridge.h"    // rs64_dpc_get_cumulative_histogram / _fullsyncs

using recomp::dbg::env_str;
using recomp::dbg::env_on;
using recomp::dbg::env_int;
using recomp::dbg::env_u32;

extern "C" volatile unsigned g_f5_heur[4];

#ifdef _WIN32
extern "C" void rs64_data_bp_arm(void* host_addr);
extern "C" void rs64_data_bp_drain(int vi);
#endif

// src/main/main.cpp
extern void print_stack_with_symbols(void** frames, unsigned short count);
// ultramodern events.cpp
extern "C" uint8_t* g_rs64_parse_rdram;             // RDRAM snapshot for the current parse
// lib/rt64 F5 GBI
extern "C" volatile unsigned g_most_drawn_fb;        // most-drawn color image and its width
extern "C" volatile unsigned g_most_drawn_fb_width;
extern "C" volatile unsigned long long g_most_drawn_fb_ms;
// Defined below
extern "C" void rs64_sanitize_fb_registry(void);

// Last task's processDisplayLists time, reported by the ROGUESQ_LOG_GFX_TASK line.
extern "C" volatile long long g_rs64_pdl_us;
volatile long long g_rs64_pdl_us = 0;
// Running total of processDisplayLists CPU time (us) across all tasks; ROGUESQ_LOG_FRAME_PROFILE
// samples it per present to attribute frame time to DL-walk/submit vs present vs sim.
extern "C" volatile long long g_rs64_pdl_us_total;
volatile long long g_rs64_pdl_us_total = 0;
extern "C" volatile long long g_rs64_pdl_task_total;
volatile long long g_rs64_pdl_task_total = 0;
// Snapshot memcpy time (us), set on the game thread in events.cpp take_rdram_snapshot.
extern "C" volatile long long g_rs64_snap_us;

// Set on construction, cleared on shutdown; the LLE DPC bridge submits through it.
static std::atomic<RT64::Application*> g_rt64_app{nullptr};

// True while RT64's F1 developer inspector (ImGui) is up. The inspector is
// created/destroyed on the window thread inside RT64's SDL event filter, which
// runs during the same SDL_PumpEvents as update_gfx's caller -- so this read is
// same-thread with the writer and needs no lock. Used to release mouse-steering
// capture so the cursor is free for the inspector.
extern "C" int rs64_rt64_inspector_open(void) {
    RT64::Application* app = g_rt64_app.load(std::memory_order_relaxed);
    if (!app || !app->presentQueue) return 0;
    // Only the developer-mode F1 inspector counts. Outside developer mode the
    // inspector object still exists to host our own ImGui render hook (the
    // Controls window), but the F1 developer UI can't be opened, so that object
    // being non-null must not suppress capture.
    if (!app->userConfig.developerMode) return 0;
    return app->presentQueue->inspector != nullptr ? 1 : 0;
}

// Horizontal widening RT64 applies to the frame; the game's frustum culls scale by it via rogue_squadron.toml hooks.
// ROGUESQ_CULL_WIDEN=0 disables.
extern "C" float rs64_cull_widen(void) {
    static const bool s_on = env_on("ROGUESQ_CULL_WIDEN", true);
    RT64::Application* app = g_rt64_app.load(std::memory_order_relaxed);
    if (!s_on || !app || !app->presentQueue) return 1.0f;
    const hlslpp::float2 rs = app->presentQueue->ext.sharedResources->resolutionScale;
    const float y = float(rs.y);
    const float w = (y > 0.0f) ? float(rs.x) / y : 1.0f;
    return (w > 1.0f && w < 4.0f) ? w : 1.0f;
}

#ifdef _WIN32
// Symbolizes on the faulting thread inside the filter; capped to bound loader-lock exposure.
static int hle_seh_filter(EXCEPTION_POINTERS* ep) {
    static int s_logged = 0;
    if (ep && ep->ExceptionRecord && s_logged++ < 5) {
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        unsigned long long acc = (er->NumberParameters >= 2) ? (unsigned long long)er->ExceptionInformation[1] : 0;
        int rw = (er->NumberParameters >= 1) ? (int)er->ExceptionInformation[0] : -1;
        fprintf(stderr, "[hle-seh] code=0x%08X pc=%p access=0x%llX rw=%d (0=read,1=write,8=exec)\n",
                (unsigned)er->ExceptionCode, er->ExceptionAddress, acc, rw);
        void* frames[24];
        unsigned short n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
        print_stack_with_symbols(frames, n);
        fflush(stderr);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

static uint8_t DMEM[0x1000];
static uint8_t IMEM[0x1000];

static unsigned int MI_INTR_REG      = 0;
static unsigned int DPC_START_REG    = 0;
static unsigned int DPC_END_REG      = 0;
static unsigned int DPC_CURRENT_REG  = 0;
static unsigned int DPC_STATUS_REG   = 0;
static unsigned int DPC_CLOCK_REG    = 0;
static unsigned int DPC_BUFBUSY_REG  = 0;
static unsigned int DPC_PIPEBUSY_REG = 0;
static unsigned int DPC_TMEM_REG     = 0;

static void dummy_check_interrupts() {}

static ultramodern::renderer::SetupResult map_result(RT64::Application::SetupResult r) {
    switch (r) {
    case RT64::Application::SetupResult::Success:                  return ultramodern::renderer::SetupResult::Success;
    case RT64::Application::SetupResult::DynamicLibrariesNotFound: return ultramodern::renderer::SetupResult::DynamicLibrariesNotFound;
    case RT64::Application::SetupResult::InvalidGraphicsAPI:       return ultramodern::renderer::SetupResult::InvalidGraphicsAPI;
    case RT64::Application::SetupResult::GraphicsAPINotFound:      return ultramodern::renderer::SetupResult::GraphicsAPINotFound;
    case RT64::Application::SetupResult::GraphicsDeviceNotFound:   return ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
    }
    return ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
}

namespace recomp {

class RT64Context : public ultramodern::renderer::RendererContext {
public:
    std::unique_ptr<RT64::Application> app;
    static inline std::atomic<bool> s_hle_disabled{false};  // tripped by the SEH streak, retried periodically

    RT64Context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool debug) {
        static unsigned char dummy_rom_header[0x40] = {};

        RT64::Application::Core appCore{};
#if defined(_WIN32)
        appCore.window = window_handle.window;
#elif defined(__linux__) || defined(__ANDROID__)
        appCore.window = window_handle;
#elif defined(__APPLE__)
        appCore.window.window = window_handle.window;
        appCore.window.view   = window_handle.view;
#endif

        appCore.checkInterrupts = dummy_check_interrupts;
        appCore.HEADER  = dummy_rom_header;
        appCore.RDRAM   = rdram;
        appCore.DMEM    = DMEM;
        appCore.IMEM    = IMEM;

        appCore.MI_INTR_REG     = &MI_INTR_REG;
        appCore.DPC_START_REG   = &DPC_START_REG;
        appCore.DPC_END_REG     = &DPC_END_REG;
        appCore.DPC_CURRENT_REG = &DPC_CURRENT_REG;
        appCore.DPC_STATUS_REG  = &DPC_STATUS_REG;
        appCore.DPC_CLOCK_REG   = &DPC_CLOCK_REG;
        appCore.DPC_BUFBUSY_REG = &DPC_BUFBUSY_REG;
        appCore.DPC_PIPEBUSY_REG= &DPC_PIPEBUSY_REG;
        appCore.DPC_TMEM_REG    = &DPC_TMEM_REG;

        ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
        appCore.VI_STATUS_REG         = &vi->VI_STATUS_REG;
        appCore.VI_ORIGIN_REG         = &vi->VI_ORIGIN_REG;
        appCore.VI_WIDTH_REG          = &vi->VI_WIDTH_REG;
        appCore.VI_INTR_REG           = &vi->VI_INTR_REG;
        appCore.VI_V_CURRENT_LINE_REG = &vi->VI_V_CURRENT_LINE_REG;
        appCore.VI_TIMING_REG         = &vi->VI_TIMING_REG;
        appCore.VI_V_SYNC_REG         = &vi->VI_V_SYNC_REG;
        appCore.VI_H_SYNC_REG         = &vi->VI_H_SYNC_REG;
        appCore.VI_LEAP_REG           = &vi->VI_LEAP_REG;
        appCore.VI_H_START_REG        = &vi->VI_H_START_REG;
        appCore.VI_V_START_REG        = &vi->VI_V_START_REG;
        appCore.VI_V_BURST_REG        = &vi->VI_V_BURST_REG;
        appCore.VI_X_SCALE_REG        = &vi->VI_X_SCALE_REG;
        appCore.VI_Y_SCALE_REG        = &vi->VI_Y_SCALE_REG;

        RT64::ApplicationConfiguration appConfig;
        appConfig.useConfigurationFile = false;
        appConfig.detectDataPath = false;

        app = std::make_unique<RT64::Application>(appCore, appConfig);
        // F1 inspector: default on in Debug, off in Release. ROGUESQ_HLE_DEV_MODE=0/1 overrides.
#ifdef _DEBUG
        const bool dev_mode_default = true;
#else
        const bool dev_mode_default = false;
#endif
        using UC = RT64::UserConfiguration;
        // --- Display config: baseline defaults -> roguesq_video.json -> ROGUESQ_* env overrides ---
        app->userConfig.developerMode = debug || env_on("ROGUESQ_HLE_DEV_MODE", dev_mode_default);
        app->userConfig.displayBuffering = UC::DisplayBuffering::Triple;
        app->userConfig.threePointFiltering = false;             // GPU bilinear on all textures
        // Frame interpolation OFF by default: it smooths rigid objects but F5's pooled effect quads
        // flicker and vertex-morphed terrain stutters under it (no clean per-object classifier found).
        // ROGUESQ_RT_INTERP=<hz> re-enables it; the majority-vote pacing + node-id paths stay behind it.
        app->userConfig.refreshRate = UC::RefreshRate::Original;
        app->userConfig.refreshRateTarget = 60;

        // roguesq_video.json next to the exe (same convention as roguesq_input.json): present keys
        // override the baseline above; a default file is written on first run so every RT64 display
        // setting is discoverable/editable. RT64's to_json/from_json handle the enums as strings.
        // The ROGUESQ_* env vars below still override the file (dev/testing escape hatch).
        {
            std::string cfgPath;
            if (char* base = SDL_GetBasePath()) { cfgPath = base; SDL_free(base); }
            cfgPath += "roguesq_video.json";
            video_cfg_path_ = cfgPath;                           // enables F1-menu save-back
            rs64::video::LoadResult lr = rs64::video::load(app->userConfig, cfgPath);
            if (lr.loaded) {
                fprintf(stderr, "[RT64] loaded %s%s\n", cfgPath.c_str(),
                        lr.migrated ? " (migrated to friendly schema)" : "");
            }
            if (!lr.loaded || lr.migrated) {
                std::ofstream out{cfgPath, std::ios::trunc};
                if (out) {
                    out << rs64::video::to_friendly(app->userConfig).dump(2) << "\n";
                    fprintf(stderr, "[RT64] wrote %s %s\n", cfgPath.c_str(),
                            lr.migrated ? "(migrated)" : "(default)");
                }
            }
        }

        // ROGUESQ_* overrides (win over the file); each acts only when its var is explicitly set.
        {
            // ROGUESQ_MSAA=2|4|8: multisample anti-aliasing (default off).
            if (env_str("ROGUESQ_MSAA")) {
                int s = env_int("ROGUESQ_MSAA");
                app->userConfig.antialiasing = (s >= 8) ? UC::Antialiasing::MSAA8X
                                             : (s >= 4) ? UC::Antialiasing::MSAA4X
                                             : (s >= 2) ? UC::Antialiasing::MSAA2X
                                                        : UC::Antialiasing::None;
                fprintf(stderr, "[RT64] MSAA=%ux\n", UC::msaaSampleCount(app->userConfig.antialiasing));
            }
            // ROGUESQ_RES_SCALE=<mult>: internal render-resolution multiplier (Manual).
            if (const char* v = env_str("ROGUESQ_RES_SCALE")) {
                double m = std::atof(v);
                if (m > 0.0) {
                    app->userConfig.resolution = UC::Resolution::Manual;
                    app->userConfig.resolutionMultiplier = m;
                    fprintf(stderr, "[RT64] resolutionMultiplier=%.2f\n", m);
                }
            }
            // ROGUESQ_SSAA=<n>: supersample/downsample factor (>=2 sharper, costlier).
            {
                int d = env_int("ROGUESQ_SSAA");
                if (d >= 1) { app->userConfig.downsampleMultiplier = d; fprintf(stderr, "[RT64] downsample=%d\n", d); }
            }
            // ROGUESQ_ASPECT=<w/h> forces a ratio; ROGUESQ_WIDESCREEN=1 expands to the window.
            if (const char* v = env_str("ROGUESQ_ASPECT")) {
                double a = std::atof(v);
                if (a > 0.0) {
                    app->userConfig.aspectRatio = UC::AspectRatio::Manual;
                    app->userConfig.aspectTarget = a;
                    fprintf(stderr, "[RT64] aspect=Manual %.4f\n", a);
                }
            } else if (env_on("ROGUESQ_WIDESCREEN")) {
                app->userConfig.aspectRatio = UC::AspectRatio::Expand;
                fprintf(stderr, "[RT64] aspect=Expand\n");
            }
            // ROGUESQ_HDR=1: high internal color format.
            if (env_on("ROGUESQ_HDR")) {
                app->userConfig.internalColorFormat = UC::InternalColorFormat::High;
                fprintf(stderr, "[RT64] HDR internal color format\n");
            }
            // ROGUESQ_TEX_FILTER=nearest|linear|aa. This drives BOTH the VI present-time upscale
            // (userConfig.filtering) AND the per-texture sampler (userConfig.threePointFiltering).
            // The per-texture bit is the one that actually filters game textures: threePointFiltering
            // off => flags.linearFiltering forced on for every draw => GPU bilinear on ALL tiles,
            // including G_TF_POINT sprites (the point-sampled explosion/HUD quads). The upscale filter
            // alone (what we set before) does not touch the game textures, so "linear" looked inert.
            if (const char* v = env_str("ROGUESQ_TEX_FILTER")) {
                std::string_view s(v);
                if (s == "nearest") { app->userConfig.filtering = UC::Filtering::Nearest; app->userConfig.threePointFiltering = true; }
                else if (s == "linear") { app->userConfig.filtering = UC::Filtering::Linear; app->userConfig.threePointFiltering = false; }
                else if (s == "aa") app->userConfig.filtering = UC::Filtering::AntiAliasedPixelScaling;
                fprintf(stderr, "[RT64] filtering=%s threePoint=%d\n", v, app->userConfig.threePointFiltering ? 1 : 0);
            }
            // ROGUESQ_THREE_POINT=0|1: per-texture sampler independent of the upscale filter.
            // 0 = GPU bilinear on all textures; 1 = N64 3-point (faithful; point on G_TF_POINT tiles).
            if (const char* v = env_str("ROGUESQ_THREE_POINT")) {
                app->userConfig.threePointFiltering = (v[0] != '0');
                fprintf(stderr, "[RT64] threePointFiltering=%d\n", app->userConfig.threePointFiltering ? 1 : 0);
            }
            // ROGUESQ_RT_INTERP=0 disables frame interpolation; =<hz> sets a target (raster-only,
            // does not touch the logic tick). Baseline is ON at 60 Hz; only overrides when set.
            if (const char* v = env_str("ROGUESQ_RT_INTERP")) {
                if (v[0] == '0') {
                    app->userConfig.refreshRate = UC::RefreshRate::Original;
                    fprintf(stderr, "[RT64] frame interpolation OFF\n");
                } else {
                    int hz = env_int("ROGUESQ_RT_INTERP");
                    if (hz < 20) hz = 60;
                    app->userConfig.refreshRate = UC::RefreshRate::Manual;
                    app->userConfig.refreshRateTarget = hz;
                    fprintf(stderr, "[RT64] frame interpolation ON (Manual %d Hz)\n", hz);
                }
            }
        }

        // PresentEarly (default on): the cinematic stays on one VI fb address, so RT64's
        // updateScreen never sees a VI change and would never present. ROGUESQ_HLE_PRESENT_EARLY=0 opts out.
        if (env_on("ROGUESQ_HLE_PRESENT_EARLY", true)) {
            app->enhancementConfig.presentation.mode =
                RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
        }

        // Zelda64Recomp's enhancement settings; without them glyph texrects sample stale TMEM.
        app->enhancementConfig.f3dex.forceBranch = true;
        app->enhancementConfig.textureLOD.scale = true;
        // ROGUESQ_GFX_API=vulkan|d3d12 forces the backend (default Automatic).
        if (const char* api = env_str("ROGUESQ_GFX_API")) {
            if (std::string_view(api) == "vulkan") {
                app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Vulkan;
                fprintf(stderr, "[RT64] graphics API forced to Vulkan via ROGUESQ_GFX_API\n");
            } else if (std::string_view(api) == "d3d12") {
                app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::D3D12;
                fprintf(stderr, "[RT64] graphics API forced to D3D12 via ROGUESQ_GFX_API\n");
            }
        }

        // Snapshot the resolved config; update_screen rewrites roguesq_video.json when the F1 menu edits it.
        try { video_cfg_snapshot_ = rs64::video::to_friendly(app->userConfig).dump(); } catch (...) {}

        uint32_t thread_id = 0;
#ifdef _WIN32
        thread_id = window_handle.thread_id;
#endif
        setup_result = map_result(app->setup(thread_id));
        if (setup_result != ultramodern::renderer::SetupResult::Success) {
            fprintf(stderr, "[RT64] setup failed: %d\n", (int)setup_result);
            app = nullptr;
        }
        else {
            // setup() snapshots enhancementConfig before the edits above; push them through.
            app->updateEnhancementConfig();
            fprintf(stderr, "[RT64] enhancementConfig: presentMode=%d forceBranch=%d textureLOD.scale=%d\n",
                    (int)app->enhancementConfig.presentation.mode,
                    (int)app->enhancementConfig.f3dex.forceBranch,
                    (int)app->enhancementConfig.textureLOD.scale);
            fflush(stderr);

            // ROGUESQ_TEXTURE_PACK=<dir-or-zip>: RT64 replacement directory (rt64.json + DDS/PNG).
            if (const char* tp = env_str("ROGUESQ_TEXTURE_PACK")) {
                if (app->textureCache) {
                    std::vector<RT64::ReplacementDirectory> dirs;
                    dirs.emplace_back(std::filesystem::path(tp));
                    bool ok = app->textureCache->loadReplacementDirectories(dirs);
                    app->textureCache->textureMap.replacementMapEnabled = ok;
                    fprintf(stderr, "[RT64] texture pack '%s' load %s\n", tp, ok ? "OK" : "FAILED");
                    fflush(stderr);
                }
            }
        }
        if (app && app->appWindow) {
            fprintf(stderr,
                "[RT64] post-setup: sdlWindow=%p sdlEventFilterInstalled=%d windowHook=%p developerMode=%d usesWindowMessageFilter=%d\n",
                (void*)app->appWindow->sdlWindow,
                (int)app->appWindow->sdlEventFilterInstalled,
#ifdef _WIN32
                (void*)app->appWindow->windowHook,
#else
                (void*)nullptr,
#endif
                (int)app->userConfig.developerMode,
                (int)app->usesWindowMessageFilter());
            fflush(stderr);
        }
        g_rt64_app.store(app.get());
    }

    bool valid() override {
        return app != nullptr;
    }

    bool update_config(const ultramodern::renderer::GraphicsConfig&,
                       const ultramodern::renderer::GraphicsConfig&) override {
        return true;
    }

    void enable_instant_present() override {
        // Re-applies the constructor's PresentEarly choice; same opt-out.
        if (app && env_on("ROGUESQ_HLE_PRESENT_EARLY", true)) {
            app->enhancementConfig.presentation.mode =
                RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
            app->updateEnhancementConfig();
        }
    }

    void send_dl(const OSTask* task) override {
        if (!app) return;
        static const int s_task_delay_ms = env_int("ROGUESQ_GFX_TASK_DELAY_MS", 0);
        if (s_task_delay_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(s_task_delay_ms));
        dump_ucode_once(task);
        log_task(task);
        run_hle_task(task);
    }

    void update_screen() override {
        if (!app) return;
        ++vi_count_;
        apply_vi_overrides();
        drive_buffer_arbiter();
        drive_boot_target();
        poll_gamestate();
        {
            static bool s_nav_init = false;
            if (!s_nav_init) { s_nav_init = true; rs64_nav_set_target(env_str("ROGUESQ_BOOT_TARGET")); }
        }
        rs64_nav_tick((uint8_t*)app->core.RDRAM);
        dump_rdram_if_armed();
        watch_rdram();
        data_bp_tick();
        {
            // ROGUESQ_LOG_HEURISTICS=1: firing counts of F5 render heuristics under retirement review.
            static const bool s_heur = env_on("ROGUESQ_LOG_HEURISTICS");
            if (s_heur && (vi_count_ & 255) == 0)
                fprintf(stderr, "[heur] vi=#%d op0A=%u proj_mismatch=%u b4_filler=%u attrib_deflicker=%u\n",
                        vi_count_, g_f5_heur[0], g_f5_heur[1], g_f5_heur[2], g_f5_heur[3]);
        }
        log_vi_state();
        maybe_persist_video_cfg();
        const auto pres0 = std::chrono::high_resolution_clock::now();
        app->updateScreen();
        frame_profile(pres0);
    }

    // ROGUESQ_LOG_FRAME_PROFILE=1: once/sec, attribute frame time to its phases so a busy-scene
    // drop can be classified CPU-bound (DL walk/submit), GPU/present-bound, or sim/pacing-bound.
    //   present : actual presents/sec (the real framerate)
    //   period  : wall time between presents, ms (min/avg/max) -- spikes = the drops
    //   dl      : processDisplayLists us/present (CPU DL translate+submit) + tasks/present
    //   present : app->updateScreen() us (submit + any GPU/vsync wait)
    //   other   : period - dl - present = game-thread sim + scheduler + idle
    void frame_profile(std::chrono::high_resolution_clock::time_point pres0) {
        static const bool s_on = env_on("ROGUESQ_LOG_FRAME_PROFILE", false);
        if (!s_on) return;
        const auto now = std::chrono::high_resolution_clock::now();
        const long long pres_us =
            std::chrono::duration_cast<std::chrono::microseconds>(now - pres0).count();

        static bool s_have_prev = false;
        static std::chrono::high_resolution_clock::time_point s_prev, s_win0;
        static long long s_pdl_base = 0, s_task_base = 0;
        static long long s_period_min = 0, s_period_max = 0, s_period_sum = 0;
        static long long s_pres_sum = 0, s_pres_max = 0;
        static int s_frames = 0;

        if (!s_have_prev) {
            s_have_prev = true; s_prev = now; s_win0 = now;
            s_pdl_base = g_rs64_pdl_us_total; s_task_base = g_rs64_pdl_task_total;
            s_period_min = s_period_max = s_period_sum = 0; s_frames = 0;
            s_pres_sum = s_pres_max = 0;
            return;
        }
        const long long period_us =
            std::chrono::duration_cast<std::chrono::microseconds>(now - s_prev).count();
        s_prev = now;
        // Per-hitch attribution: on a long present interval, dump the most-recent walk + snapshot
        // costs so a 30-76ms spike can be routed to walk-bound / memcpy-bound / pacing-bound.
        //   walk_last : last F5 processDisplayLists (Gfx Thread) us -- includes any GPU-barrier wait
        //   snap_us   : last RDRAM snapshot memcpy (game thread) us
        //   present_us: this update_screen() us (submit + present wait)
        constexpr long long kHitchUs = 25000;
        if (period_us > kHitchUs) {
            fprintf(stderr,
                "[frameprof HITCH] period=%.1fms walk_last=%.1fms snap=%.1fms present=%.2fms\n",
                period_us / 1000.0, g_rs64_pdl_us / 1000.0, g_rs64_snap_us / 1000.0,
                pres_us / 1000.0);
            fflush(stderr);
        }
        if (s_frames == 0 || period_us < s_period_min) s_period_min = period_us;
        if (period_us > s_period_max) s_period_max = period_us;
        s_period_sum += period_us;
        s_pres_sum += pres_us;
        if (pres_us > s_pres_max) s_pres_max = pres_us;
        ++s_frames;

        const long long win_us =
            std::chrono::duration_cast<std::chrono::microseconds>(now - s_win0).count();
        if (win_us >= 1000000 && s_frames > 0) {
            const long long dl_us = g_rs64_pdl_us_total - s_pdl_base;
            const long long dl_tasks = g_rs64_pdl_task_total - s_task_base;
            const double fps = s_frames * 1e6 / (double)win_us;
            fprintf(stderr,
                "[frameprof] present=%.1f fps | period ms min/avg/max=%.1f/%.1f/%.1f | "
                "dl us/frame=%.0f tasks/frame=%.1f | present us avg/max=%.0f/%lld | other ms/frame=%.1f\n",
                fps,
                s_period_min / 1000.0, (s_period_sum / (double)s_frames) / 1000.0, s_period_max / 1000.0,
                dl_us / (double)s_frames, dl_tasks / (double)s_frames,
                s_pres_sum / (double)s_frames, s_pres_max,
                ((s_period_sum - dl_us - s_pres_sum) / (double)s_frames) / 1000.0);
            fflush(stderr);
            s_win0 = now; s_pdl_base = g_rs64_pdl_us_total; s_task_base = g_rs64_pdl_task_total;
            s_period_min = s_period_max = s_period_sum = 0; s_frames = 0;
            s_pres_sum = s_pres_max = 0;
        }
    }

    void shutdown() override {
        if (app) {
            g_rt64_app.store(nullptr);
            app->end();
            app.reset();
        }
    }

    uint32_t get_display_framerate() const override {
        if (app && app->presentQueue) {
            return app->presentQueue->ext.sharedResources->swapChainRate;
        }
        return 60;
    }

    float get_resolution_scale() const override {
        return app ? float(app->userConfig.resolutionMultiplier) : 1.0f;
    }

private:
    int vi_count_ = 0;
    std::string video_cfg_path_;      // roguesq_video.json next to the exe ("" = disabled)
    std::string video_cfg_snapshot_;  // last-persisted userConfig json; F1-menu edits rewrite the file

    // Persist F1-menu display changes back to roguesq_video.json. The menu edits app->userConfig
    // in place (State::ext.userConfig points at it), so a throttled dirty-check catches changes.
    void maybe_persist_video_cfg() {
        if (video_cfg_path_.empty() || !app) return;
        if ((vi_count_ % 15) != 0) return;                 // ~4x/sec; config changes are rare
        std::string cur;
        try { cur = rs64::video::to_friendly(app->userConfig).dump(); } catch (...) { return; }
        if (cur == video_cfg_snapshot_) return;
        video_cfg_snapshot_ = cur;
        std::ofstream out(video_cfg_path_, std::ios::trunc);
        if (out.is_open()) {
            try { out << json::parse(cur).dump(2) << "\n"; } catch (...) {}
            fprintf(stderr, "[RT64] saved %s (menu change)\n", video_cfg_path_.c_str());
        }
    }

    // ROGUESQ_WATCH_ADDRS=<addr>[,<addr>...]: log each listed RDRAM word when it changes, per VI.
    void watch_rdram() {
        static const char* s_spec = env_str("ROGUESQ_WATCH_ADDRS");
        if (!s_spec || !app->core.RDRAM) return;
        static uint32_t s_waddr[16], s_wlast[16];
        static int s_n = -1;
        if (s_n < 0) {
            s_n = 0;
            for (const char* p = s_spec; *p && s_n < 16; ) {
                s_waddr[s_n] = (uint32_t)std::strtoul(p, (char**)&p, 16) & 0x7FFFFCu;
                s_wlast[s_n] = rd32(s_waddr[s_n]);
                fprintf(stderr, "[watch] vi=#%d %08X = %08X (initial)\n", vi_count_, 0x80000000u | s_waddr[s_n], s_wlast[s_n]);
                ++s_n;
                if (*p == ',') ++p;
                else break;
            }
        }
        for (int i = 0; i < s_n; ++i) {
            const uint32_t v = rd32(s_waddr[i]);
            if (v == s_wlast[i]) continue;
            fprintf(stderr, "[watch] vi=#%d %08X %08X -> %08X\n", vi_count_, 0x80000000u | s_waddr[i], s_wlast[i], v);
            s_wlast[i] = v;
        }
    }

    // ROGUESQ_DATA_BP=<addr>: hardware write-watch on that word, armed at ROGUESQ_DATA_BP_ARM_VI.
    void data_bp_tick() {
#ifdef _WIN32
        static const char* s_bp = env_str("ROGUESQ_DATA_BP");
        if (!s_bp || !app->core.RDRAM) return;
        static const int s_arm_vi = env_int("ROGUESQ_DATA_BP_ARM_VI", 1);
        static bool s_armed = false;
        if (!s_armed && vi_count_ >= s_arm_vi) {
            s_armed = true;
            const uint32_t a = (uint32_t)std::strtoul(s_bp, nullptr, 16) & 0x7FFFFCu;
            rs64_data_bp_arm(app->core.RDRAM + a);
        }
        if (s_armed) rs64_data_bp_drain(vi_count_);
#endif
    }

    // RDRAM accessors, byte-swapped (index ^ 3) and bounds-checked.
    uint8_t rd8(uint32_t addr) const {
        if (!app->core.RDRAM || addr >= 0x800000) return 0;
        return app->core.RDRAM[addr ^ 3];
    }
    uint32_t rd32(uint32_t addr) const {
        return (uint32_t(rd8(addr)) << 24) | (uint32_t(rd8(addr + 1)) << 16) |
               (uint32_t(rd8(addr + 2)) << 8) | uint32_t(rd8(addr + 3));
    }
    void wr8(uint32_t addr, uint8_t v) {
        if (app->core.RDRAM && addr < 0x800000) app->core.RDRAM[addr ^ 3] = v;
    }
    void wr32(uint32_t addr, uint32_t v) {
        wr8(addr, uint8_t(v >> 24)); wr8(addr + 1, uint8_t(v >> 16));
        wr8(addr + 2, uint8_t(v >> 8)); wr8(addr + 3, uint8_t(v));
    }

    // ---- send_dl ----

    // ROGUESQ_DUMP_UCODE=path: one-shot IMEM + DMEM dump for offline disassembly.
    void dump_ucode_once(const OSTask* task) {
        static const char* s_path = env_str("ROGUESQ_DUMP_UCODE");
        static bool s_done = false;
        if (!s_path || s_done) return;
        s_done = true;
        const uint32_t ucode_phys = (uint32_t)task->t.ucode & 0x3FFFFFF;
        const uint32_t udata_phys = (uint32_t)task->t.ucode_data & 0x3FFFFFF;
        const uint32_t udata_size = task->t.ucode_data_size ? task->t.ucode_data_size : 0x800;
        auto write_region = [&](const char* suffix, uint32_t phys, uint32_t size) {
            char path[512];
            snprintf(path, sizeof path, "%s.%s.bin", s_path, suffix);
            FILE* f = recomp::os::fopen(path, "wb");
            if (!f) return;
            std::vector<uint8_t> buf(size);
            for (uint32_t i = 0; i < size; ++i) buf[i] = rd8(phys + i);
            fwrite(buf.data(), 1, size, f);
            fclose(f);
            fprintf(stderr, "[dump-ucode] wrote %u bytes to %s (src=0x%08X)\n", size, path, phys);
        };
        write_region("imem", ucode_phys, 0x2000);
        write_region("dmem", udata_phys, udata_size);
        fflush(stderr);
    }

    // First 8 tasks, every 64th, and every ucode change.
    void log_task(const OSTask* task) {
        static int s_count = 0;
        static uint32_t s_last_ucode = 0, s_last_data = 0;
        int n = ++s_count;
        const bool ucode_changed = (task->t.ucode != s_last_ucode) || (task->t.ucode_data != s_last_data);
        s_last_ucode = task->t.ucode;
        s_last_data = task->t.ucode_data;
        if (n <= 8 || (n & 63) == 0 || ucode_changed) {
            fprintf(stderr,
                "[hle send_dl #%d]%s type=%u ucode=0x%08X data=0x%08X dl=0x%08X size=%u wq=%d pq=%d\n",
                n, ucode_changed ? " [UCODE-CHANGE]" : "",
                (unsigned)task->t.type, (unsigned)task->t.ucode,
                (unsigned)task->t.ucode_data, (unsigned)task->t.data_ptr,
                (unsigned)task->t.data_size, workload_cursor(), present_cursor());
            fflush(stderr);
        }
    }

    int workload_cursor() const { return app->workloadQueue ? (int)app->workloadQueue->writeCursor : -1; }
    int present_cursor() const { return app->presentQueue ? (int)app->presentQueue->writeCursor : -1; }

    void run_hle_task(const OSTask* task) {
        if (s_hle_disabled.load(std::memory_order_relaxed)) {
            // Retry every 120 tasks: a transient AV burst (scene transitions) recovers on its
            // own, while a genuinely stuck stream just re-trips the streak.
            static int s_retry = 0;
            if ((++s_retry % 120) != 0) return;
            s_hle_disabled.store(false, std::memory_order_relaxed);
            fprintf(stderr, "[hle send_dl] auto-recovery: re-enabling HLE submission\n");
            fflush(stderr);
        }
        app->state->rsp->reset();
        // ROGUESQ_INTERP_RATE=<hz> pins the game's logical rate, bypassing RT64's VI-factor detection
        // (setRefreshRate -> extended.refreshRate wins over logicalRateFromFactors). "auto"/"strict"
        // are handled in logicalRateFromFactors; only a numeric value forces a fixed rate here.
        {
            static const int s_rate = [](){ const char* e = env_str("ROGUESQ_INTERP_RATE");
                if (!e || !e[0]) return 0; int v = atoi(e); return (v >= 10 && v <= 60) ? v : 0; }();
            if (s_rate) app->state->setRefreshRate((uint16_t)s_rate);
        }
        app->interpreter->loadUCodeGBI(task->t.ucode & 0x3FFFFFF, task->t.ucode_data & 0x3FFFFFF, true);
#ifdef _WIN32
        static std::atomic<int> s_seh_streak{0};
        __try {
            process_hle_task(task);
            s_seh_streak.store(0, std::memory_order_relaxed);
        }
        __except (hle_seh_filter(GetExceptionInformation())) {
            // Threshold sits well above the normal cinematic->menu AV burst; the state
            // cleanup below makes consecutive AVs no worse than isolated ones.
            constexpr int kDisableStreak = 90;
            int streak = s_seh_streak.fetch_add(1, std::memory_order_relaxed) + 1;
            fprintf(stderr, "[hle send_dl] SEH streak=%d in processDisplayLists ucode=0x%08X data=0x%08X dl=0x%08X%s\n",
                    streak, (unsigned)task->t.ucode, (unsigned)task->t.ucode_data, (unsigned)task->t.data_ptr,
                    streak >= kDisableStreak ? " -- HLE submission DISABLED" : "");
            fflush(stderr);
            if (streak >= kDisableStreak) s_hle_disabled.store(true, std::memory_order_relaxed);
            if (app->state) {
                // The __try body may have faulted before restoring the snapshot pointer.
                app->state->RDRAM = app->core.RDRAM;
                app->state->writeBackRDRAM = nullptr;
                app->state->dlCpuProfiler.startedTimestamp = RT64::Timestamp{};
                // checkRDRAM on the next task asserts these are empty.
                app->state->drawFbOperations.clear();
                app->state->drawFbDiscards.clear();
                app->state->differentFbs.clear();
                app->state->rdramCheckPending = false;
            }
        }
#else
        process_hle_task(task);
#endif
    }

    void process_hle_task(const OSTask* task) {
        const int wq_pre = workload_cursor(), pq_pre = present_cursor();
        // Parse from the task-start RDRAM snapshot (events.cpp) when one exists; RT64 writes
        // back to live RDRAM.
        uint8_t* const parseMem = g_rs64_parse_rdram ? g_rs64_parse_rdram : app->core.RDRAM;
        rs64_neutralize_matpool_cimg(parseMem, (uint32_t)task->t.data_ptr & 0x3FFFFFF);
        const auto pdl0 = std::chrono::high_resolution_clock::now();
        app->state->RDRAM = parseMem;
        app->state->writeBackRDRAM = (parseMem != app->core.RDRAM) ? app->core.RDRAM : nullptr;
        app->processDisplayLists(parseMem, task->t.data_ptr & 0x3FFFFFF, 0, /*isHLE*/ true);
        app->state->RDRAM = app->core.RDRAM;
        app->state->writeBackRDRAM = nullptr;
        g_rs64_pdl_us = (long long)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - pdl0).count();
        g_rs64_pdl_us_total += g_rs64_pdl_us;
        g_rs64_pdl_task_total += 1;
        rs64_sanitize_fb_registry();

        static int s_n = 0;
        int n = ++s_n;
        if ((n <= 8 || (n & 63) == 0) && app->workloadQueue) {
            auto& q = *app->workloadQueue;
            const size_t wc = q.writeCursor;
            const size_t prev = (wc + q.workloads.size() - 1) % q.workloads.size();
            fprintf(stderr, "[hle send_dl #%d post] wq=%d->%d pq=%d->%d nextFbPairs=%u prevFbPairs=%u\n",
                    n, wq_pre, workload_cursor(), pq_pre, present_cursor(),
                    (unsigned)q.workloads[wc].fbPairCount, (unsigned)q.workloads[prev].fbPairCount);
            fflush(stderr);
        }

        // Flush uncommitted work after each task: F5's mid-DL fullSync leaves the trailing
        // workload accumulating fbPairs unbounded. ROGUESQ_HLE_AUTO_FULLSYNC=0 disables.
        static const bool s_auto_fullsync = env_on("ROGUESQ_HLE_AUTO_FULLSYNC", true);
        if (s_auto_fullsync && app->state) {
            auto& wl = app->state->ext.workloadQueue->workloads[app->state->ext.workloadQueue->writeCursor];
            if (wl.fbPairCount > wl.fbPairSubmitted || app->state->drawCall.triangleCount > 0) {
                app->state->dlCpuProfiler.start();
                app->state->fullSync();
                app->state->dlCpuProfiler.end();
            }
        }
    }

    // ---- update_screen ----

    // Present-side VI register overrides. The menu fix is the load-bearing one; the FORCE_*
    // switches are A/B levers for it.
    void apply_vi_overrides() {
        ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
        if (!vi) return;
        auto set_origin = [&](uint32_t base) { vi->VI_ORIGIN_REG = (base & ~0xFFFu) | (vi->VI_ORIGIN_REG & 0xFFFu); };

        // ROGUESQ_FORCE_SWAP_FB=1: scan out the game's last osViSwapBuffer target (physical).
        static const bool s_force_swap = env_on("ROGUESQ_FORCE_SWAP_FB");
        if (s_force_swap && g_last_swap_fb) set_origin(g_last_swap_fb & 0x03FFFFFFu);
        // ROGUESQ_FORCE_VI_ADDR=<phys> / ROGUESQ_FORCE_VI_WIDTH=<n>: pin scanout origin / width.
        static const uint32_t s_force_vi = env_u32("ROGUESQ_FORCE_VI_ADDR");
        static const uint32_t s_force_viw = env_u32("ROGUESQ_FORCE_VI_WIDTH");
        if (s_force_vi) set_origin(s_force_vi);
        if (s_force_viw) vi->VI_WIDTH_REG = s_force_viw;
        if (s_force_vi || s_force_viw) return;

        // Menu present fix: the menu composes into a 512-wide color image while VI_WIDTH_REG
        // stays on the cinematic mode (640/1024), so RT64 cannot match origin+width and shows
        // black. Present the GBI's most-drawn buffer at its published width whenever that width
        // differs from VI's. The GBI publishes both from the gfx thread, so this never walks
        // RT64's live fb map. ROGUESQ_NO_MENU_PRESENT_FIX=1 disables.
        static const bool s_menufix = !env_on("ROGUESQ_NO_MENU_PRESENT_FIX");
        if (!s_menufix || !g_most_drawn_fb) return;
        const uint32_t w = g_most_drawn_fb_width;
        if (w < 16 || w > 1024 || w == vi->VI_WIDTH_REG) return;
        // Only redirect to a buffer that is still being drawn. The mission text crawl draws
        // only triangles into a 640-wide buffer, so the texrect-based most-drawn value stays
        // on the 512-wide hangar buffer and would black out the crawl. ROGUESQ_MENU_FIX_STALE_MS.
        static const uint64_t s_stale_ms = [](){ uint32_t v = env_u32("ROGUESQ_MENU_FIX_STALE_MS"); return v ? v : 250u; }();
        const uint64_t now_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now_ms - (uint64_t)g_most_drawn_fb_ms > s_stale_ms) return;
        static const bool s_log = env_on("ROGUESQ_LOG_MENU_FIX");
        static int s_logged = 0;
        if (s_log && s_logged++ < 200) {
            fprintf(stderr, "[menu-fix FIRED] drawn=0x%08X w=%u (was VIw=%u VIorg=0x%08X) "
                    "xscale=0x%X yscale=0x%X hstart=0x%X vstart=0x%X vsync=0x%X status=0x%X\n",
                    (unsigned)g_most_drawn_fb, w, vi->VI_WIDTH_REG, vi->VI_ORIGIN_REG,
                    vi->VI_X_SCALE_REG, vi->VI_Y_SCALE_REG, vi->VI_H_START_REG,
                    vi->VI_V_START_REG, vi->VI_V_SYNC_REG, vi->VI_STATUS_REG);
            fflush(stderr);
        }
        set_origin(g_most_drawn_fb & 0x03FFFFFFu);
        vi->VI_WIDTH_REG = w;
        // The menu buffer is a 512x448 full frame but VI is still in interlaced mode, so RT64's
        // fbSize() yields one field (224). Halving Y scale doubles it back to the full height.
        // ROGUESQ_NO_MENU_VSCALE_FIX=1 disables.
        static const bool s_vfix = !env_on("ROGUESQ_NO_MENU_VSCALE_FIX");
        if (s_vfix && vi->VI_Y_SCALE_REG == 0x400) vi->VI_Y_SCALE_REG = 0x800;
    }

    // Host-side buffer-arbiter consumer for the legacy host-paced loop: advances the slot that
    // just went on screen 4 PRESENTED -> 5 DISPLAYED and frees the previous one (5 -> 0, posting
    // the free token on 0x80128CF0). Never touches producer-owned states 2/3. Off when VI-driven;
    // ROGUESQ_FORCE_BUFFER_PROGRESS=0 disables. ROGUESQ_DUMP_ARBITER=1 logs.
    void drive_buffer_arbiter() {
        static const bool s_on = env_on("ROGUESQ_FORCE_BUFFER_PROGRESS", true) && !rs64_vi_driven();
        if (!s_on || !app->core.RDRAM) return;
        static const bool s_dump = env_on("ROGUESQ_DUMP_ARBITER");
        constexpr uint32_t STATE_BASE = 0x128EAA, COUNT_ADDR = 0x128EAD, FBPTR_BASE = 0x128E98;
        const uint8_t total = rd8(COUNT_ADDR);
        if (total == 0 || total > 8 || !g_last_swap_fb) return;
        const uint32_t want = g_last_swap_fb & 0x03FFF000u;
        int matched = -1;
        for (uint8_t i = 0; i < total; i++) {
            if ((rd32(FBPTR_BASE + 4u * i) & 0x03FFF000u) == want) { matched = (int)i; break; }
        }
        static int s_prev = -1;
        static unsigned s_tick = 0;
        const bool log = s_dump && (++s_tick & 63) == 1;
        if (matched < 0) {
            if (log) { fprintf(stderr, "[arbiter-host] no slot matches swap fb 0x%08X (miss #%u)\n", (unsigned)g_last_swap_fb, s_tick); fflush(stderr); }
            return;
        }
        const uint8_t mst = rd8(STATE_BASE + (uint32_t)matched);
        uint8_t pst = 0xFF;
        bool did45 = false, did50 = false;
        if (mst == 4) { wr8(STATE_BASE + (uint32_t)matched, 5); did45 = true; }
        if (s_prev >= 0 && s_prev != matched) {
            pst = rd8(STATE_BASE + (uint32_t)s_prev);
            if (pst == 5) {
                wr8(STATE_BASE + (uint32_t)s_prev, 0);
                did50 = true;
                ultramodern::enqueue_external_message((PTR(OSMesgQueue))0x80128CF0u, (OSMesg)0, false, false);
            }
        }
        if (log) {
            fprintf(stderr, "[arbiter-host] match slot=%d st=%u prev=%d pst=%u total=%u 45=%d 50=%d\n",
                    matched, mst, s_prev, pst, total, did45, did50);
            fflush(stderr);
        }
        s_prev = matched;
    }

    // ROGUESQ_BOOT_TARGET=menu | demo:N | level:<id>[,craft]. menu/demo fast-forward the intro
    // and press START (the game's own title skip) once the cinematic overlay is up, stopping when
    // the menu overlay is resident. level:<id> launches from the game thread (rs64_boot_target_level
    // in upstream_compat.cpp), so it gets the fast-forward only. See plans/boot-into-mission-plan.md.
    // ROGUESQ_CINE_FASTFWD=N adds N VI ticks per present to the cinematic clock (0x8011A890) until
    // the cutscene's end frame, so the timeline completes through the game's own logic.
    // ROGUESQ_SKIP_DEMO=<n> (alias ROGUESQ_SKIP_TO_JADEMOON=1 for n=1) pins the attract demoId
    // (0x80130B54) and its cycle byte so that demo loops. 0=Tatooine 1=JadeMoon 2=KileII
    // 3=Taloraan 4=Fest 5=TrenchRun.
    void drive_boot_target() {
        enum { BOOT_OFF = -1, BOOT_MENU, BOOT_DEMO, BOOT_LEVEL };
        static const int s_mode = [](){
            const char* v = env_str("ROGUESQ_BOOT_TARGET");
            if (!v) return (int)BOOT_OFF;
            if (!std::strncmp(v, "menu", 4))  return (int)BOOT_MENU;
            if (!std::strncmp(v, "demo", 4))  return (int)BOOT_DEMO;
            if (!std::strncmp(v, "level", 5)) return (int)BOOT_LEVEL;
            return (int)BOOT_OFF;
        }();
        static const int s_demo_arg = [](){
            const char* v = env_str("ROGUESQ_BOOT_TARGET");
            const char* c = v ? std::strchr(v, ':') : nullptr;
            int n = (c && c[1]) ? std::atoi(c + 1) : 0;
            return (n >= 0 && n <= 5) ? n : 0;
        }();
        static const int s_ffwd = env_int("ROGUESQ_CINE_FASTFWD", (s_mode != BOOT_OFF) ? 400 : 0);
        static const int s_skip_demo = [](){
            if (env_str("ROGUESQ_SKIP_DEMO")) return env_int("ROGUESQ_SKIP_DEMO");
            if (env_on("ROGUESQ_SKIP_TO_JADEMOON")) return 1;
            return (s_mode == BOOT_DEMO) ? s_demo_arg : -1;
        }();
        if (!app->core.RDRAM) return;

        if (s_ffwd > 0) {
            const uint32_t cut = rd32(0x0B1904);
            if (cut >= 0x80000000u && cut < 0x80800000u) {
                const uint32_t gate = rd32(0x0B0B28);
                const uint32_t end_frame = rd32((cut - 0x80000000u) + 0x44);
                if (end_frame > 0x10 && end_frame < 0x100000u && gate < (end_frame - 0xA)) {
                    wr32(0x11A890, rd32(0x11A890) + (uint32_t)s_ffwd);
                    static int s_log = 0;
                    if ((s_log++ & 127) == 0) {
                        fprintf(stderr, "[cine-ffwd] +%d/present gateCtr=%u/%u cutscene=0x%08X\n", s_ffwd, gate, end_frame, cut);
                        fflush(stderr);
                    }
                }
            }
        }

        if (s_mode != BOOT_OFF) {
            static bool s_seen_cine = false, s_skip_done = false;
            if (g_active_overlay == 2) s_seen_cine = true;
            if (!s_skip_done && s_seen_cine && g_active_overlay == 1) {
                s_skip_done = true;
                g_boot_pulse_start = 0;
                fprintf(stderr, "[boot-skip] menu handoff reached; START pulse off\n");
                fflush(stderr);
            }
            // All nav targets reach the front-end menu; pulse START through the intro handoff.
            // (demo-immediate is blocked by the custom menu displacing the attract idle path;
            // see plans/2026-09-22-boot-target-nav-engine-design.md.)
            g_boot_pulse_start = (s_skip_done || g_active_overlay != 2) ? 0 : 1;
        }

        if (s_skip_demo >= 0 && s_skip_demo <= 5) {
            wr8(0x130B54, (uint8_t)s_skip_demo);
            wr8(0x130B55, 0);
        }
    }

    // ROGUESQ_LOG_GAMESTATE=1: log engine globals (docs/game-architecture.md) on change and
    // every 512 presents. gateCtr vs cuts44 shows whether the cinematic timeline is advancing.
    void poll_gamestate() {
        if (!app->core.RDRAM) return;
        rs64_state_poll((const uint8_t*)app->core.RDRAM);   // every present: the nav sequencer needs
                                                            // fresh state to catch fast menu screens
        if ((vi_count_ & 63) != 0) return;
        static const bool s_on = env_on("ROGUESQ_LOG_GAMESTATE");
        if (!s_on) return;
        struct Snap {
            uint8_t level, craft, cineStage, u20, u21, u22, screen, menuId, sub;
            uint32_t cineState, cutscene, menuPtr;
            int sceneId;
            bool operator==(const Snap& o) const {
                return level == o.level && craft == o.craft && cineStage == o.cineStage &&
                       u20 == o.u20 && u21 == o.u21 && u22 == o.u22 && screen == o.screen &&
                       menuId == o.menuId && menuPtr == o.menuPtr && sceneId == o.sceneId &&
                       sub == o.sub &&
                       cineState == o.cineState && cutscene == o.cutscene;
            }
        };
        Snap s{};
        s.sceneId = g_current_scene;
        s.level = rd8(0x130B70); s.craft = rd8(0x130B41);
        s.cineState = rd32(0x0B0934); s.cineStage = rd8(0x0B0938);
        s.cutscene = rd32(0x0B1904);
        s.u20 = rd8(0x130B60); s.u21 = rd8(0x130B61); s.u22 = rd8(0x130B62);
        s.screen = rd8(0x130B14);
        s.menuId = rd8(0x0CE734); s.menuPtr = rd32(0x0CE730); s.sub = rd8(0x0CE626);
        const uint8_t b04 = rd8(0x130B44), b1f = rd8(0x130B5F), b25 = rd8(0x130B65);
        const uint32_t gateCtr = rd32(0x0B0B28);
        const bool cut_ok = s.cutscene >= 0x80000000u && s.cutscene < 0x80800000u;
        const uint32_t cuts44 = cut_ok ? rd32((s.cutscene - 0x80000000u) + 0x44) : 0;

        static Snap s_last{};
        static bool s_have_last = false;
        const bool changed = !s_have_last || !(s == s_last);
        if (!changed && (vi_count_ & 511) != 0) return;
        fprintf(stderr,
            "[gamestate vi=#%d] state=%s scene=%d menuId=%u sub=%u menuPtr=0x%08X screen=%u level=%u craft=%u b04=%u b1f=%u b25=%u u20=%u u21=%u u22=%u cineState=0x%08X cineStage=%u cutscene=0x%08X gateCtr=%u cuts44=%u%s\n",
            vi_count_, rs64_state_current_id(), g_current_scene, s.menuId, s.sub, s.menuPtr, s.screen, s.level, s.craft, b04, b1f, b25, s.u20, s.u21, s.u22,
            s.cineState, s.cineStage, s.cutscene, gateCtr, cuts44, changed ? " [CHANGED]" : "");
        if (cut_ok) {
            const uint32_t off = s.cutscene - 0x80000000u;
            fprintf(stderr, "  cutscene-filename: \"");
            for (int i = 0; i < 32; i++) {
                const uint8_t b = rd8(off + i);
                if (b == 0) break;
                fputc((b >= 0x20 && b < 0x7F) ? b : '?', stderr);
            }
            fprintf(stderr, "\"\n");
        }
        fflush(stderr);
        s_last = s;
        s_have_last = true;
    }

    // One-shot RDRAM dump (un-swizzled to MIPS byte order, same layout as a PJ64 dump) for
    // tools/validate/rdram_golden_diff.py. Triggers, first match wins:
    //   ROGUESQ_DUMP_RDRAM_AT_VI=<n>              present number
    //   ROGUESQ_DUMP_RDRAM_ON_CINE_ITER=<n>       cinematic loop iteration (rs64_cine_iter_get)
    //   ROGUESQ_DUMP_RDRAM_ON_CINE_STALL=<ms>     cinematic frame counter (0x8013889C) unchanged that long
    //   ROGUESQ_DUMP_RDRAM_ON_SCENE=<id>[,settle] g_current_scene held for settle presents (default 60)
    //   ROGUESQ_DUMP_RDRAM_ON_SCREEN=<n|menu[,settle]>  screenState == n, or menu overlay after the cinematic
    // Path: ROGUESQ_DUMP_RDRAM_PATH, default dumps/rdram_<tag>.bin.
    void dump_rdram_if_armed() {
        // ROGUESQ_DUMP_RDRAM_ON_STATE=<id>[,<id>...]: multi-milestone dump keyed to the game-state
        // classifier -- dumps dumps/rdram_state_<id>.bin the first present each listed state becomes
        // current, ALL in one run (unlike the one-shot triggers below). Feeds tools/validate/state_diff.ps1.
        {
            static const char* s_on_state = env_str("ROGUESQ_DUMP_RDRAM_ON_STATE");
            if (s_on_state && app->core.RDRAM) {
                static char s_buf[256];
                static const char* s_ids[16];
                static bool s_iddone[16];
                static int s_held[16];
                static const int s_settle = env_int("ROGUESQ_DUMP_RDRAM_STATE_SETTLE", 0);
                static int s_nids = -1;
                if (s_nids < 0) {
                    std::snprintf(s_buf, sizeof s_buf, "%s", s_on_state);
                    s_nids = 0;
                    for (char* p = s_buf; *p && s_nids < 16; ) {
                        s_ids[s_nids] = p; s_iddone[s_nids] = false; s_held[s_nids] = 0; ++s_nids;
                        char* c = std::strchr(p, ',');
                        if (!c) break;
                        *c = 0; p = c + 1;
                    }
                }
                const char* cur = rs64_state_current_id();
                for (int i = 0; i < s_nids; ++i) {
                    if (s_iddone[i]) continue;
                    if (std::strcmp(cur, s_ids[i]) != 0) { s_held[i] = 0; continue; }
                    if (++s_held[i] <= s_settle) continue;
                    s_iddone[i] = true;
                    char path[512];
                    std::snprintf(path, sizeof path, "dumps/rdram_state_%s.bin", s_ids[i]);
                    FILE* f = recomp::os::fopen(path, "wb");
                    if (f) {
                        static uint8_t sbuf[0x10000];
                        for (uint32_t base = 0; base < 0x800000u; base += sizeof sbuf) {
                            for (uint32_t j = 0; j < sizeof sbuf; ++j) sbuf[j] = app->core.RDRAM[(base + j) ^ 3];
                            fwrite(sbuf, 1, sizeof sbuf, f);
                        }
                        fclose(f);
                    }
                    fprintf(stderr, "[rdram-dump] state=%s vi=#%d -> %s (%s)\n", s_ids[i], vi_count_, path, f ? "ok" : "OPEN FAILED");
                    fflush(stderr);
                }
            }
        }

        static const char* s_at_vi = env_str("ROGUESQ_DUMP_RDRAM_AT_VI");
        static const char* s_on_cine = env_str("ROGUESQ_DUMP_RDRAM_ON_CINE_ITER");
        static const char* s_on_stall = env_str("ROGUESQ_DUMP_RDRAM_ON_CINE_STALL");
        static const char* s_on_scene = env_str("ROGUESQ_DUMP_RDRAM_ON_SCENE");
        static const char* s_on_screen = env_str("ROGUESQ_DUMP_RDRAM_ON_SCREEN");
        static bool s_done = false, s_seen_cine = false;
        if (s_done || !app->core.RDRAM || !(s_at_vi || s_on_cine || s_on_stall || s_on_scene || s_on_screen)) return;

        const uint8_t screenState = rd8(0x130B14);
        if (screenState >= 6 || g_active_overlay == 2) s_seen_cine = true;
        char tag[48] = {0};
        if (s_at_vi) {
            if (vi_count_ < std::atoi(s_at_vi)) return;
            std::snprintf(tag, sizeof tag, "vi_%d", vi_count_);
        } else if (s_on_cine) {
            // Comma-separated list: one dump per listed iteration (ROGUESQ_DUMP_RDRAM_PATH ignored for lists).
            static std::vector<unsigned long long> s_iters; static size_t s_next = 0;
            if (s_iters.empty()) { for (const char* c = s_on_cine; *c; ) { s_iters.push_back((unsigned long long)std::strtoull(c, (char**)&c, 10)); if (*c == ',') ++c; else break; } if (s_iters.empty()) s_iters.push_back(0); }
            const unsigned long long it = rs64_cine_iter_get();
            if (s_next >= s_iters.size() || it < s_iters[s_next]) return;
            ++s_next;
            std::snprintf(tag, sizeof tag, "cine_iter%llu", it);
            if (s_next < s_iters.size()) {
                char path2[512]; std::snprintf(path2, sizeof path2, "dumps/rdram_%s.bin", tag);
                if (FILE* f2 = recomp::os::fopen(path2, "wb")) {
                    static uint8_t buf2[0x10000];
                    for (uint32_t base = 0; base < 0x800000u; base += sizeof buf2) {
                        for (uint32_t i = 0; i < sizeof buf2; ++i) buf2[i] = app->core.RDRAM[(base + i) ^ 3];
                        fwrite(buf2, 1, sizeof buf2, f2);
                    }
                    fclose(f2);
                    fprintf(stderr, "[rdram-dump] vi=#%u cine_iter=%llu -> %s (ok)\n", vi_count_, it, path2); fflush(stderr);
                }
                return;
            }
        } else if (s_on_stall) {
            const uint32_t it = rd32(0x13889C);
            static uint32_t s_last_it = 0;
            static uint64_t s_last_ms = 0;
            const uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (it != s_last_it) { s_last_it = it; s_last_ms = now; return; }
            if (it == 0 || !s_last_ms || now - s_last_ms < (uint64_t)std::atoll(s_on_stall)) return;
            std::snprintf(tag, sizeof tag, "cine_stall_iter%u", it);
        } else if (s_on_scene) {
            const int want = std::atoi(s_on_scene);
            const char* comma = std::strchr(s_on_scene, ',');
            const int settle = comma ? std::atoi(comma + 1) : 60;
            static int s_held = 0;
            s_held = (g_current_scene == want) ? s_held + 1 : 0;
            if (s_held < settle) return;
            std::snprintf(tag, sizeof tag, "scene_%d", want);
        } else {
            const bool want_menu = (s_on_screen[0] == 'm');
            const char* comma = std::strchr(s_on_screen, ',');
            const int settle = comma ? std::atoi(comma + 1) : 0;
            const bool cond = want_menu ? (s_seen_cine && g_active_overlay == 1)
                                        : (screenState == (uint8_t)std::atoi(s_on_screen));
            static int s_held = 0;
            s_held = cond ? s_held + 1 : 0;
            if (!cond || s_held <= settle) return;
            std::snprintf(tag, sizeof tag, "screen_%u", screenState);
        }

        s_done = true;
        char path[512];
        if (const char* p = env_str("ROGUESQ_DUMP_RDRAM_PATH")) std::snprintf(path, sizeof path, "%s", p);
        else std::snprintf(path, sizeof path, "dumps/rdram_%s.bin", tag);
        FILE* f = recomp::os::fopen(path, "wb");
        if (f) {
            static uint8_t buf[0x10000];
            for (uint32_t base = 0; base < 0x800000u; base += sizeof buf) {
                for (uint32_t i = 0; i < sizeof buf; ++i) buf[i] = app->core.RDRAM[(base + i) ^ 3];
                fwrite(buf, 1, sizeof buf, f);
            }
            fclose(f);
        }
        fprintf(stderr, "[rdram-dump] vi=#%d screen=%u -> %s (%s)\n", vi_count_, screenState, path, f ? "ok" : "OPEN FAILED");
        fflush(stderr);
    }

    // First 4 presents and every 64th: VI regs, queue cursors, and whether the scanout buffer
    // has any pixel data. Every 256th adds the cumulative DPC opcode histogram.
    void log_vi_state() {
        static bool s_filter_logged = false;
        if (!s_filter_logged && app->appWindow && app->appWindow->sdlEventFilterInstalled) {
            fprintf(stderr, "[RT64] sdlEventFilter NOW installed at update_screen #%d (sdlWindow=%p)\n",
                    vi_count_, (void*)app->appWindow->sdlWindow);
            fflush(stderr);
            s_filter_logged = true;
        }
        if (vi_count_ > 4 && (vi_count_ & 63) != 0) return;
        ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
        const uint32_t origin = vi->VI_ORIGIN_REG & 0x00FFFFFF;
        uint32_t any_nonzero = 0;
        if (app->core.RDRAM && origin > 0 && origin + 16 < 0x800000) {
            for (int i = 0; i < 16; i++) any_nonzero |= app->core.RDRAM[origin + i];
        }
        fprintf(stderr,
            "[vi] update_screen #%d origin=0x%08X width=%u status=0x%X v_current=%u nonzero=%d fs=%u pq.wc=%d wq.wc=%d drawn=0x%08X dw=%u\n",
            vi_count_, vi->VI_ORIGIN_REG, vi->VI_WIDTH_REG, vi->VI_STATUS_REG, vi->VI_V_CURRENT_LINE_REG,
            any_nonzero != 0, rs64_dpc_get_cumulative_fullsyncs(), present_cursor(), workload_cursor(),
            (unsigned)g_most_drawn_fb, (unsigned)g_most_drawn_fb_width);
        if ((vi_count_ & 255) == 0) {
            uint32_t hist[64];
            rs64_dpc_get_cumulative_histogram(hist);
            uint32_t total = 0;
            for (int i = 0; i < 64; ++i) total += hist[i];
            fprintf(stderr, "  [opcode-hist total=%u top:", total);
            for (int slot = 0; slot < 6; ++slot) {
                int max_idx = 0;
                for (int i = 0; i < 64; ++i) if (hist[i] > hist[max_idx]) max_idx = i;
                if (hist[max_idx] == 0) break;
                fprintf(stderr, " op%02X=%u", max_idx, hist[max_idx]);
                hist[max_idx] = 0;
            }
            fprintf(stderr, "]\n");
        }
        fflush(stderr);
    }
};

std::unique_ptr<ultramodern::renderer::RendererContext>
create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window, bool developer_mode) {
    return std::make_unique<RT64Context>(rdram, window, developer_mode);
}

} // namespace recomp

// LLE DPC bridge entry (src/rsp/dpc_bridge.cpp): raw RDP byte ranges into RT64.
namespace ultramodern {
    // RT64 state is unrecoverable after an AV here, so the first SEH disables LLE submission.
    static std::atomic<bool> s_rdp_disabled{false};

    static void run_rdp_submission(RT64::Application* app, uint32_t lo_phys, uint32_t hi_phys) {
        if (s_rdp_disabled.load(std::memory_order_relaxed)) {
            return;
        }
#ifdef _WIN32
        __try {
            app->processDisplayLists(app->core.RDRAM, lo_phys, hi_phys, /*isHLE*/ false);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            if (app->state) {
                app->state->dlCpuProfiler.startedTimestamp = RT64::Timestamp{};
            }
            s_rdp_disabled.store(true, std::memory_order_relaxed);
            fprintf(stderr, "[rdp-submit] SEH in processDisplayLists "
                            "lo=0x%08X hi=0x%08X — RDP submission DISABLED for the rest "
                            "of this session (RT64 state corrupted by AV).\n",
                            lo_phys, hi_phys);
            uint32_t hist[64];
            rs64_dpc_get_cumulative_histogram(hist);
            uint32_t total = 0;
            for (int i = 0; i < 64; ++i) total += hist[i];
            fprintf(stderr, "[rdp-submit] cumulative opcode histogram (total=%u):\n", total);
            uint32_t copy[64];
            for (int i = 0; i < 64; ++i) copy[i] = hist[i];
            for (int slot = 0; slot < 12; ++slot) {
                int max_idx = 0;
                for (int i = 0; i < 64; ++i) {
                    if (copy[i] > copy[max_idx]) max_idx = i;
                }
                if (copy[max_idx] == 0) break;
                fprintf(stderr, "  op 0x%02X = %u\n", max_idx, copy[max_idx]);
                copy[max_idx] = 0;
            }
            fflush(stderr);
        }
#else
        app->processDisplayLists(app->core.RDRAM, lo_phys, hi_phys, /*isHLE*/ false);
#endif
    }

    void submit_rdp_range(uint32_t lo_phys, uint32_t hi_phys) {
        RT64::Application *app = g_rt64_app.load();
        if (app && hi_phys > lo_phys) {
            // ROGUESQ_LOG_RDP_SUBMIT=1: log each submission's range and first bytes.
            static const bool s_log = env_on("ROGUESQ_LOG_RDP_SUBMIT");
            if (s_log) {
                static int s_count = 0;
                ++s_count;
                uint32_t len = hi_phys - lo_phys;
                fprintf(stderr, "[rdp-submit #%d] lo=0x%08X hi=0x%08X len=%u\n",
                        s_count, lo_phys, hi_phys, len);
                if (len >= 8 && app->core.RDRAM) {
                    uint8_t* bytes = app->core.RDRAM + lo_phys;
                    int dump_len = (int)std::min<uint32_t>(len, 64);
                    fprintf(stderr, "  bytes: ");
                    for (int i = 0; i < dump_len; ++i) {
                        fprintf(stderr, "%02X ", bytes[i]);
                        if ((i & 7) == 7 && i + 1 < dump_len) fprintf(stderr, "\n         ");
                    }
                    fprintf(stderr, "\n");
                }
                fflush(stderr);
            }
            run_rdp_submission(app, lo_phys, hi_phys);
        }
    }
}

// Erases framebuffers RT64 registered outside [0x400000, 0x800000): real ones all live in
// [0x4B7800, 0x800000), anything else is garbage CIMG the pre-parse walker missed and would
// let writeback scribble over the heap. Runs before any workload consumes the entry.
extern "C" void rs64_sanitize_fb_registry(void) {
    if (!(rs64_fb_guards_mask() & 4)) return;
    RT64::Application* app = g_rt64_app.load();
    if (!app || !app->state) return;

    static int s_logs = 0;
#ifdef _WIN32
    __try {
#endif
        auto& fbs = app->state->framebufferManager.framebuffers;
        for (auto it = fbs.begin(); it != fbs.end(); ) {
            uint32_t raw_start = it->second.addressStart;
            bool is_low  = raw_start < 0x400000u;
            bool is_high = raw_start >= 0x800000u;
            if (is_low || is_high) {
                uint32_t key       = it->first;
                uint32_t start_n64 = raw_start | 0x80000000u;
                uint32_t end_n64   = it->second.addressEnd | 0x80000000u;
                uint32_t w         = it->second.width;
                uint32_t h         = it->second.height;
                if (s_logs++ < 16) {
                    fprintf(stderr, "[fb-sanitize] erasing garbage fb key=0x%08X "
                            "start=0x%08X end=0x%08X w=%u h=%u (%s)\n",
                            key, start_n64, end_n64, w, h,
                            is_low ? "below 0x400000" : "above 0x800000");
                    fflush(stderr);
                }
                it = fbs.erase(it);
            } else {
                ++it;
            }
        }
#ifdef _WIN32
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        fprintf(stderr, "[fb-sanitize] EXCEPTION during sweep\n");
        fflush(stderr);
    }
#endif
}
