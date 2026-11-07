#include <algorithm>
#include <memory>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <thread>
#include <mutex>
#include <vector>
#include <filesystem>
#include <fstream>
#include "main/renderdoc_capture.h"
#include "SDL.h"
#ifdef __ANDROID__
#include "SDL_syswm.h"
extern SDL_Window* g_sdl_window;
#endif
#include "common/rt64_user_configuration.h"
#include "video/video_config.h"

#ifndef HLSL_CPU
#define HLSL_CPU
#endif
#include "hle/rt64_application.h"
#include "hle/rt64_present_queue.h"
#include "hle/rt64_rs64_lights.h"
#include "hle/rt64_rs64_transition.h"
#include "librecomp/mods.hpp"

#include "ultramodern/ultramodern.hpp"
#include "ultramodern/renderer_context.hpp"
#include "util/debug_logs.h"
#include "rs64_toml_hooks.h"
#include "game/upstream_compat.h"      // rs64_fb_guards_mask, g_active_overlay, g_last_swap_fb
#include "game/hook_helpers.h"         // g_boot_pulse_start, g_current_scene, rs64_cine_iter_get, rs64_neutralize_matpool_cimg
#include "game/game_state.h"           // rs64_state_poll, rs64_state_current_id
#include "game/nav_sequencer.h"        // rs64_nav_set_target, rs64_nav_tick
#include "main/main.h"                // rs64_reloc_guard_tick
#include "lockstep/lockstep_core.h"    // load_replay: the recording's display settings
#include "video/rt64_render_context.h"
#include "video/render_diag.h"
#include "default_lights.h"
#ifdef __ANDROID__
#include "main/android_host.h"
#endif

using recomp::dbg::env_str;
using recomp::dbg::env_on;
using recomp::dbg::env_int;
using recomp::dbg::env_u32;

extern "C" volatile unsigned g_f5_heur[4];

#ifdef RS64_MCP
extern "C" void rs64_mcp_note_dl();
#endif

#ifdef _WIN32

extern "C" void rs64_data_bp_arm(void* host_addr);
extern "C" void rs64_data_bp_set2(void* host_addr);
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
extern "C" volatile uint32_t g_rs64_clear_seq;          // mode-change clear count and size (hook_helpers.cpp)
extern "C" volatile uint32_t g_rs64_clear_bytes;

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

static constexpr const char* kLightsFile = "roguesq_lights.json";

// Modification times and sizes of every roguesq_lights.json the reader below can see (next to the exe and in mod folders), as one string.
static std::string light_files_stamp()
{

    std::error_code ec;
    std::string stamp;

    const auto add = [&](const std::filesystem::path& p) {

        const auto t = std::filesystem::last_write_time(p, ec);

        if (!ec) stamp += p.string() + '|' + std::to_string((long long)t.time_since_epoch().count()) + '|' + std::to_string(std::filesystem::file_size(p, ec)) + '\n';
    };

    char* base = SDL_GetBasePath();

    add(std::filesystem::path(base ? base : "") / kLightsFile);
    SDL_free(base);

    for (const auto& dir : std::filesystem::directory_iterator(recomp::mods::get_mods_directory(), ec))
    {
        if (dir.is_directory(ec)) add(dir.path() / kLightsFile);
    }

    return stamp;
}

void rs64_queue_light_presets()
{

    // The defaults used to ship as this mod; an older install's leftover copy would override the built-in presets with stale ones.
    static constexpr const char* kRetiredDefaultsMod = "model-lights";

    // The F1 Lights tab's Reload light files runs this again.
    rs64lights::presetReloader() = rs64_queue_light_presets;

    // The built-in presets (src/video/default_lights.json) go first, so any mod's same-named preset replaces them; mods follow the MODS
    // list, lower wins, and the copy next to the exe comes last.
    std::vector<std::string> texts{ std::string(rs64_default_lights, rs64_default_lights_size) };

    for (const auto& d : recomp::mods::get_all_mod_details("rs64"))
    {
        if (!recomp::mods::is_mod_enabled(d.mod_id) || (d.mod_id == kRetiredDefaultsMod)) continue;

        bool exists = false;
        const std::vector<char> text = recomp::mods::read_mod_file(d.mod_id, kLightsFile, exists);

        if (exists)
        {
            texts.emplace_back(text.begin(), text.end());

            fprintf(stderr, "[RT64] light presets queued from mod '%s'\n", d.mod_id.c_str());
        }
    }

    char* base = SDL_GetBasePath();
    std::ifstream f(std::string(base ? base : "") + kLightsFile, std::ios::binary);

    SDL_free(base);

    if (f.is_open())
    {
        texts.emplace_back(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());

        fprintf(stderr, "[RT64] light presets queued from %s next to the exe\n", kLightsFile);
    }

    rs64lights::queuePresetReload(std::move(texts));

    // Light files are re-read once a second after one changes, so an edit (an explorer export, F1's Save library) shows without a restart.
    static std::once_flag s_watch;

    std::call_once(s_watch, [] {
        std::thread([] {

            std::string last = light_files_stamp();

            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));

                std::string now = light_files_stamp();

                if (now == last) continue;

                last = std::move(now);

                fprintf(stderr, "[RT64] a light file changed: reloading light presets\n");
                rs64_queue_light_presets();
            }
        }).detach();
    });
}

// True while RT64's F1 developer inspector (ImGui) is up. The inspector is
// created/destroyed on the window thread inside RT64's SDL event filter, which
// runs during the same SDL_PumpEvents as update_gfx's caller -- so this read is
// same-thread with the writer and needs no lock. Used to release mouse-steering
// capture so the cursor is free for the inspector.
extern "C" int rs64_rt64_inspector_open(void)
{

    RT64::Application* app = g_rt64_app.load(std::memory_order_relaxed);

    if (!app) return 0;

    // The inspector object always exists to host the Controls window, so only the F1 visibility counts.
    return (app->userConfig.developerMode && app->inspectorVisible) ? 1 : 0;
}

extern "C" double rs64_rt64_present_ms(void)
{

    RT64::Application* app = g_rt64_app.load(std::memory_order_relaxed);

    return (app && app->presentQueue) ? app->presentQueue->presentProfiler.average() : 0.0;
}

extern "C" void rs64_render_suspend_surface(void) {

    RT64::Application* app = g_rt64_app.load();

    if (app && app->presentQueue) {
        app->presentQueue->suspendSurface();

        fprintf(stderr, "[RT64] surface suspended (app backgrounded)\n");
    }
}

extern "C" bool rs64_render_surface_suspended(void)
{

    RT64::Application* app = g_rt64_app.load();

    return app && app->presentQueue && app->presentQueue->surfaceSuspended;
}

extern "C" bool rs64_render_resume_surface(void)
{

    RT64::Application* app = g_rt64_app.load();

    if (!app || !app->presentQueue || !app->presentQueue->surfaceSuspended)
    {
        return false;
    }

#ifdef __ANDROID__

    SDL_SysWMinfo wm{};
    SDL_VERSION(&wm.version);

    if (!::g_sdl_window || !SDL_GetWindowWMInfo(::g_sdl_window, &wm) || !wm.info.android.window ||
        ANativeWindow_getWidth(wm.info.android.window) <= 0)
    {
        static uint32_t s_last_log = 0;

        if (SDL_GetTicks() - s_last_log > 1000)
        {
            s_last_log = SDL_GetTicks();

            fprintf(stderr, "[RT64] resume waiting: window=%p\n", (void*)wm.info.android.window);
        }

        return false;
    }

    app->presentQueue->resumeSurface(wm.info.android.window);
    rs64::android::set_window_frame_rate(wm.info.android.window);

    fprintf(stderr, "[RT64] surface resumed on new window %p\n", (void*)wm.info.android.window);

    return true;

#else
    return false;
#endif
}

// RT64's widescreen widening of the 4:3 view (resolution scale x over y): 1 in 4:3.
extern "C" float rs64_aspect_scale(void)
{

    RT64::Application* app = g_rt64_app.load(std::memory_order_relaxed);

    if (!app || !app->presentQueue) return 1.0f;

    const hlslpp::float2 rs = app->presentQueue->ext.sharedResources->resolutionScale;
    const float y = float(rs.y);
    const float w = (y > 0.0f) ? float(rs.x) / y : 1.0f;

    return (w > 1.0f && w < 4.0f) ? w : 1.0f;
}

// The game's frustum culls (scaled via rogue_squadron.toml hooks) assume 4:3, so in widescreen they widen to the window over 4:3, plus slack for
// wings past a model's bounding sphere (the Calamari intro's far-left V-wing vanished while its wingtip showed). ROGUESQ_CULL_WIDEN=0 disables.
constexpr float kCullEdgeSlack = 1.05f;

extern "C" float rs64_cull_widen(void)
{

    static const bool s_on = env_on("ROGUESQ_CULL_WIDEN", true);
    RT64::Application* app = g_rt64_app.load(std::memory_order_relaxed);
    const float scale = rs64_aspect_scale();

    if (!s_on || !app || !app->presentQueue) return s_on ? scale : 1.0f;

    // Cutscene frames report no resolution-scale widening while the picture still widens, so widescreen is read from the setting.
    const auto& shared = *app->presentQueue->ext.sharedResources;
    const bool expand = (shared.userConfig.aspectRatio == RT64::UserConfiguration::AspectRatio::Expand);
    const float window = (shared.swapChainHeight > 0) ? float(shared.swapChainWidth) / float(shared.swapChainHeight) : 0.0f;
    const float widened = std::max(scale, expand ? window * 0.75f : 1.0f);

    return (widened > 1.0f) ? widened * kCullEdgeSlack : 1.0f;
}

// Field-of-view widening for the game's render-side object view tests while RT sun shadows are on, so craft just off screen are drawn and cast
// this frame's shadow (moving craft never enter the shadow history). Gameplay view tests are untouched. ROGUESQ_RT_SHADOWS_CULL_MARGIN overrides.
extern "C" float rs64_shadow_cull_margin(void)
{

    static const float s_margin = std::clamp(rs64lights::envFloat("ROGUESQ_RT_SHADOWS_CULL_MARGIN", 1.6f), 1.0f, 4.0f);

    return rs64lights::config().shadows.load(std::memory_order_relaxed) ? s_margin : 1.0f;
}

// Objects this close to the camera (game units, past their radius) pass the render-side view tests in any direction while RT sun shadows are on,
// so things behind or beside the player keep casting. 0 while shadows are off. ROGUESQ_RT_SHADOWS_KEEP_DIST overrides (0 = off).
extern "C" float rs64_shadow_keep_dist(void)
{

    static const float s_dist = std::max(rs64lights::envFloat("ROGUESQ_RT_SHADOWS_KEEP_DIST", 4096.0f), 0.0f);

    return rs64lights::config().shadows.load(std::memory_order_relaxed) ? s_dist : 0.0f;
}

// Output pixels per native pixel (RT64 vertical resolution scale), 1 before RT64 is up.
extern "C" float rs64_res_scale(void)
{

    RT64::Application* app = g_rt64_app.load(std::memory_order_relaxed);

    if (!app || !app->presentQueue) return 1.0f;

    const float y = float(app->presentQueue->ext.sharedResources->resolutionScale.y);

    return (y >= 1.0f) ? y : 1.0f;
}

#ifdef _WIN32

// Symbolizes on the faulting thread inside the filter; capped to bound loader-lock exposure.
static int hle_seh_filter(EXCEPTION_POINTERS* ep) {

    static int s_logged = 0;

    if (ep && ep->ExceptionRecord && s_logged++ < 5)
    {
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

// /proc/cpuinfo rather than the device tree: Flatpak hides /sys.
static bool is_raspberry_pi() {

#ifdef __linux__

    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;

    while (std::getline(cpuinfo, line)) {
        if (line.rfind("Model", 0) == 0) {
            return line.find("Raspberry Pi") != std::string::npos;
        }
    }

#endif

    return false;
}

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

    switch (r)
    {
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

    RT64Context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool debug)
        : rdram_(rdram), window_handle_(window_handle), debug_(debug)
    {
        create_app();
    }

    // Builds and sets up the RT64 application; also rebuilds it after the GPU device is lost.
    void create_app()
    {

        uint8_t* rdram = rdram_;
        ultramodern::renderer::WindowHandle window_handle = window_handle_;
        const bool debug = debug_;
        static unsigned char dummy_rom_header[0x40] = {};

        RT64::Application::Core appCore{};

#if defined(_WIN32)
        appCore.window = window_handle.window;
#elif defined(__ANDROID__)

        SDL_SysWMinfo wm{};
        SDL_VERSION(&wm.version);
        SDL_GetWindowWMInfo(window_handle, &wm);
        appCore.window = wm.info.android.window;
        rs64::android::set_window_frame_rate(wm.info.android.window);

#elif defined(__linux__)
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

        appCore.VI_STATUS_REG        = &vi->VI_STATUS_REG;
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
        appConfig.shaderComboList =
#include "rs64_shader_combos.inl"
            ;

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

        // A Raspberry Pi's GPU can't keep up above the N64's own resolution, and RT64's framebuffer write-back costs it about half its frame time.
        if (is_raspberry_pi()) {
            app->userConfig.resolution = UC::Resolution::Original;
            rs64::video::set_framebuffer_writeback(false);
        }

        // roguesq_video.json in the working directory overrides the baseline above (a default is written on first run); the ROGUESQ_* env vars below override the file.
        std::string cfgPath = "roguesq_video.json";
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

        apply_replay_display();
        rs64::video::apply_env_overrides(app->userConfig);

        // PresentEarly (default on): the cinematic stays on one VI fb address, so RT64's
        // updateScreen never sees a VI change and would never present. ROGUESQ_HLE_PRESENT_EARLY=0 opts out.
        if (env_on("ROGUESQ_HLE_PRESENT_EARLY", true)) {
            app->enhancementConfig.presentation.mode =
                RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
        }

        // Zelda64Recomp's enhancement settings; without them glyph texrects sample stale TMEM.
        app->enhancementConfig.f3dex.forceBranch = true;
        app->enhancementConfig.textureLOD.scale = true;

        // Snapshot the resolved config; update_screen rewrites roguesq_video.json when the F1 menu edits it.
        try { video_cfg_snapshot_ = rs64::video::to_friendly(app->userConfig).dump(); } catch (...) {}

        uint32_t thread_id = 0;

#ifdef _WIN32
        thread_id = window_handle.thread_id;
#endif

        setup_result = map_result(app->setup(thread_id));

#ifdef __ANDROID__

        // RT64 only records the SDL window on Linux; the ImGui hook (touch overlay) needs it on Android too.
        if (app && app->appWindow) {
            app->appWindow->sdlWindow = ::g_sdl_window;
        }

#endif

        if (setup_result != ultramodern::renderer::SetupResult::Success) {
            fprintf(stderr, "[RT64] setup failed: %d\n", (int)setup_result);
            app = nullptr;
        }
        else {
            // setup() snapshots enhancementConfig before the edits above; push them through.
            app->updateEnhancementConfig();
            rs64::video::set_ray_query_supported(app->device && app->device->getCapabilities().rayQuery);

            fprintf(stderr, "[RT64] enhancementConfig: presentMode=%d forceBranch=%d textureLOD.scale=%d\n",
                    (int)app->enhancementConfig.presentation.mode,
                    (int)app->enhancementConfig.f3dex.forceBranch,
                    (int)app->enhancementConfig.textureLOD.scale);
            fflush(stderr);

            // ROGUESQ_TEXTURE_PACK=<dir-or-zip>: RT64 replacement directory (rt64.json + DDS/PNG).
            if (const char* tp = env_str("ROGUESQ_TEXTURE_PACK")) {
                if (app->textureCache)
                {
                    std::vector<RT64::ReplacementDirectory> dirs;
                    dirs.emplace_back(std::filesystem::path(tp));

                    bool ok = app->textureCache->loadReplacementDirectories(dirs);
                    app->textureCache->textureMap.replacementMapEnabled = ok;

                    fprintf(stderr, "[RT64] texture pack '%s' load %s\n", tp, ok ? "OK" : "FAILED");
                    fflush(stderr);
                }
            }

            // ROGUESQ_TEXTURE_DUMP_DIR=<dir>: start RT64's texture dumper at boot (the inspector's "Start dumping textures").
            if (const char* dd = env_str("ROGUESQ_TEXTURE_DUMP_DIR")) {
                if (app->state) {
                    std::filesystem::create_directories(dd);
                    app->state->dumpingTexturesDirectory = std::filesystem::path(dd);

                    fprintf(stderr, "[RT64] dumping textures to '%s'\n", dd);
                    fflush(stderr);
                }
            }
        }

        if (app && app->appWindow)
        {
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
                       const ultramodern::renderer::GraphicsConfig&) override
    {
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

    void send_dl(const OSTask* task) override
    {

#ifdef RS64_MCP
        rs64_mcp_note_dl();
#endif

        if (!app) return;

        rebuild_renderer_if_lost();

        if (!app) return;

        diag().dump_ucode_once(task);
        diag().log_task(task);

#ifdef __ANDROID__

        const auto t0 = std::chrono::steady_clock::now();
        run_hle_task(task);
        rs64::android::perf_report_work(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());

#else
        run_hle_task(task);
#endif
    }

    void update_screen() override
    {

        if (!app) return;
        if (rebuild_renderer_if_lost() || !app) return;

        ++vi_count_;

        rs64_renderdoc_poll();

        if (ultramodern::renderer::ViRegs* vi0 = ultramodern::renderer::get_vi_regs())
        {
            game_vi_origin_ = vi0->VI_ORIGIN_REG & 0x00FFFFFFu;
            game_vi_width_ = vi0->VI_WIDTH_REG;
        }

        apply_vi_overrides();
        diag().log_transition_lum();
        drive_boot_target();
        poll_gamestate();

        static bool s_nav_init = false;

        if (!s_nav_init) {
            s_nav_init = true;
            rs64_nav_set_target(env_str("ROGUESQ_BOOT_TARGET"));
        }

        rs64_nav_tick((uint8_t*)app->core.RDRAM, g_active_overlay);
        diag().dump_rdram_if_armed();
        diag().watch_rdram();
        rs64_reloc_guard_tick((uint8_t*)app->core.RDRAM);
        diag().data_bp_tick();

        // ROGUESQ_LOG_HEURISTICS=1: firing counts of F5 render heuristics under retirement review.
        static const bool s_heur = env_on("ROGUESQ_LOG_HEURISTICS");

        if (s_heur && (vi_count_ & 255) == 0)
            fprintf(stderr, "[heur] vi=#%d op0A=%u b4_filler=%u attrib_deflicker=%u\n",
                    vi_count_, g_f5_heur[0], g_f5_heur[2], g_f5_heur[3]);

        diag().log_vi_state();

        // Menu display settings: MSAA recreates the render targets first, as the F1 inspector does.
        const rs64::video::ApplyResult menu_cfg = rs64::video::apply_pending(app->userConfig);

        if (menu_cfg.msaa) app->updateMultisampling();
        if (menu_cfg.changed) app->updateUserConfig(menu_cfg.discard_fbs);

        maybe_persist_video_cfg();

        const auto pres0 = std::chrono::high_resolution_clock::now();
        app->updateScreen();
        diag().frame_profile(pres0);
    }

    void shutdown() override {

        if (app) {
            g_rt64_app.store(nullptr);
            app->end();
            app.reset();
        }
    }

    uint32_t get_display_framerate() const override {

        if (app && app->presentQueue)
        {
            return app->presentQueue->ext.sharedResources->swapChainRate;
        }

        return 60;
    }

    float get_resolution_scale() const override {
        return app ? float(app->userConfig.resolutionMultiplier) : 1.0f;
    }

private:
    uint8_t* rdram_ = nullptr;
    ultramodern::renderer::WindowHandle window_handle_{};
    bool debug_ = false;
    int vi_count_ = 0;
    uint32_t game_vi_origin_ = 0;     // VI origin/width as the game set them, before apply_vi_overrides
    uint32_t game_vi_width_ = 0;
    std::string video_cfg_path_;      // roguesq_video.json next to the exe ("" = disabled)
    std::string video_cfg_snapshot_;  // last-persisted userConfig json; F1-menu edits rewrite the file

    // A lost GPU device never recovers; rebuild RT64 on the same window. Rebuilds back off (1 s, doubling to 16 s) while losses keep recurring so a persistent fault can't spin.
    // ROGUESQ_NO_DEVICE_LOST_RECOVERY=1 disables. Runs on the gfx thread, which is the only caller of send_dl/update_screen.
    bool rebuild_renderer_if_lost()
    {

        static const bool s_off = env_on("ROGUESQ_NO_DEVICE_LOST_RECOVERY");

        if (s_off || !app || !app->device || !app->device->isLost())
        {
            return false;
        }

        using clock = std::chrono::steady_clock;

        static clock::time_point s_last_rebuild{};
        static std::chrono::milliseconds s_backoff{ 0 };
        static int s_rebuilds = 0;
        const auto now = clock::now();

        if ((s_rebuilds > 0) && (now - s_last_rebuild < s_backoff))
        {
            return true;
        }

        // A quiet minute since the last rebuild resets the backoff.
        if ((s_rebuilds == 0) || (now - s_last_rebuild > std::chrono::seconds(60)))
        {
            s_backoff = std::chrono::milliseconds(1000);
        }
        else {
            s_backoff = std::min(s_backoff * 2, std::chrono::milliseconds(16000));
        }

        s_last_rebuild = now;
        ++s_rebuilds;

        fprintf(stderr, "[RT64] GPU device lost, rebuilding renderer (#%d, next retry backoff %lld ms)\n", s_rebuilds, (long long)s_backoff.count());
        fflush(stderr);

        g_rt64_app.store(nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        app->end();
        app.reset();
        create_app();

        fprintf(stderr, "[RT64] renderer rebuild %s\n", app ? "succeeded" : "FAILED");
        fflush(stderr);

        return true;
    }

    rs64::video::RenderDiag diag() const {
        return { app.get(), vi_count_, game_vi_origin_, game_vi_width_ };
    }

    // A replay plays with the widescreen and DRAW DISTANCE it was recorded with (they change the object culls), set before the save snapshot so the file keeps the player's own.
    void apply_replay_display()
    {

        const char* path = env_str("ROGUESQ_LS_REPLAY");
        rs64::ls::SessionHeader h;
        std::vector<rs64::ls::FrameInput> frames;
        std::map<uint32_t, std::vector<uint8_t>> song;

        if (!path || !rs64::ls::load_replay(path, &h, &frames, &song) || h.draw_pct == 0)
        {
            return;
        }

        using UC = RT64::UserConfiguration;

        app->userConfig.aspectRatio = h.widescreen ? UC::AspectRatio::Expand : UC::AspectRatio::Original;
        rs64::video::set_draw_distance((float)h.draw_pct / 100.0f);

        fprintf(stderr, "[ls] replay display: widescreen=%u draw=%.2f (from the recording)\n", h.widescreen, h.draw_pct / 100.0);
    }

    // Persist F1-menu display changes back to roguesq_video.json. The menu edits app->userConfig
    // in place (State::ext.userConfig points at it), so a throttled dirty-check catches changes.
    void maybe_persist_video_cfg()
    {

        if (video_cfg_path_.empty() || !app) return;
        if ((vi_count_ % 15) != 0) return;                 // ~4x/sec; config changes are rare

        std::string cur;

        try { cur = rs64::video::to_friendly(app->userConfig).dump(); } catch (...) { return; }

        if (cur == video_cfg_snapshot_) return;

        video_cfg_snapshot_ = cur;

        std::ofstream out(video_cfg_path_, std::ios::trunc);

        if (out.is_open())
        {
            try { out << json::parse(cur).dump(2) << "\n"; } catch (...) {}

            fprintf(stderr, "[RT64] saved %s (menu change)\n", video_cfg_path_.c_str());
        }
    }

    // ---- send_dl ----

    void run_hle_task(const OSTask* task)
    {

        if (s_hle_disabled.load(std::memory_order_relaxed))
        {
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
        static const int s_rate = [] {

            const char* e = env_str("ROGUESQ_INTERP_RATE");

            if (!e || !e[0])
            {
                return 0;
            }

            int v = atoi(e);

            return (v >= 10 && v <= 60) ? v : 0;
        }();

        if (s_rate) app->state->setRefreshRate((uint16_t)s_rate);

        app->interpreter->loadUCodeGBI(task->t.ucode & 0x3FFFFFF, task->t.ucode_data & 0x3FFFFFF, true);

#ifdef _WIN32

        static std::atomic<int> s_seh_streak{0};

        __try {
            process_hle_task(task);
            s_seh_streak.store(0, std::memory_order_relaxed);
        }
        __except (hle_seh_filter(GetExceptionInformation()))
        {
            // Threshold sits well above the normal cinematic->menu AV burst; the state
            // cleanup below makes consecutive AVs no worse than isolated ones.
            constexpr int kDisableStreak = 90;
            int streak = s_seh_streak.fetch_add(1, std::memory_order_relaxed) + 1;

            fprintf(stderr, "[hle send_dl] SEH streak=%d in processDisplayLists ucode=0x%08X data=0x%08X dl=0x%08X%s\n",
                    streak, (unsigned)task->t.ucode, (unsigned)task->t.ucode_data, (unsigned)task->t.data_ptr,
                    streak >= kDisableStreak ? " -- HLE submission DISABLED" : "");
            fflush(stderr);

            if (streak >= kDisableStreak) s_hle_disabled.store(true, std::memory_order_relaxed);

            if (app->state)
            {
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

    void process_hle_task(const OSTask* task)
    {

        const int wq_pre = diag().workload_cursor(), pq_pre = diag().present_cursor();

        // Parse from the task-start RDRAM snapshot (events.cpp) when one exists; RT64 writes
        // back to live RDRAM.
        uint8_t* const parseMem = g_rs64_parse_rdram ? g_rs64_parse_rdram : app->core.RDRAM;

        rs64_neutralize_matpool_cimg(parseMem, (uint32_t)task->t.data_ptr & 0x3FFFFFF);

        const auto pdl0 = std::chrono::high_resolution_clock::now();

        app->state->RDRAM = parseMem;
        app->state->writeBackRDRAM = (parseMem != app->core.RDRAM) ? app->core.RDRAM : nullptr;
        app->emulatorConfig.framebuffer.writeBack = rs64::video::framebuffer_writeback();

        // ROGUESQ_FB_COPY_GPU=0: textures read from a known framebuffer come from RDRAM instead of RT64's GPU render-target copy (A/B for fb-as-texture alignment).
        static const bool s_fb_copy_gpu = env_on("ROGUESQ_FB_COPY_GPU", true);

        app->emulatorConfig.framebuffer.copyWithGPU = s_fb_copy_gpu;
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

        if (recomp::dbg::log_heartbeat() && (n <= 8 || (n & 63) == 0) && app->workloadQueue)
        {
            auto& q = *app->workloadQueue;
            const size_t wc = q.writeCursor;
            const size_t prev = (wc + q.workloads.size() - 1) % q.workloads.size();

            fprintf(stderr, "[hle send_dl #%d post] wq=%d->%d pq=%d->%d nextFbPairs=%u prevFbPairs=%u\n",
                    n, wq_pre, diag().workload_cursor(), pq_pre, diag().present_cursor(),
                    (unsigned)q.workloads[wc].fbPairCount, (unsigned)q.workloads[prev].fbPairCount);
            fflush(stderr);
        }

        // Flush uncommitted work after each task: F5's mid-DL fullSync leaves the trailing
        // workload accumulating fbPairs unbounded. ROGUESQ_HLE_AUTO_FULLSYNC=0 disables.
        static const bool s_auto_fullsync = env_on("ROGUESQ_HLE_AUTO_FULLSYNC", true);

        if (s_auto_fullsync && app->state)
        {
            auto& wl = app->state->ext.workloadQueue->workloads[app->state->ext.workloadQueue->writeCursor];

            if (wl.fbPairCount > wl.fbPairSubmitted || app->state->drawCall.triangleCount > 0)
            {
                app->state->dlCpuProfiler.start();
                app->state->fullSync();
                app->state->dlCpuProfiler.end();
            }
        }
    }

    // ---- update_screen ----

    // Present-side VI register overrides. The menu fix is the load-bearing one; the FORCE_*
    // switches are A/B levers for it.
    void apply_vi_overrides()
    {

        ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();

        if (!vi) return;

        auto set_origin = [&](uint32_t base) { vi->VI_ORIGIN_REG = (base & ~0xFFFu) | (vi->VI_ORIGIN_REG & 0xFFFu); };

        // swapBufferWithViMode blanks mode changes with vStart == vEnd; RT64's visible() ignores vRegion, so zero hStart instead.
        static const bool s_viblank = !env_on("ROGUESQ_NO_VI_BLANK_MODE");

        if (s_viblank && rs64transition::isBlankVStart(vi->VI_V_START_REG)) {
            vi->VI_H_START_REG = 0;

            return;
        }

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

        // Hold the redirect while a mode-change clear is pending or gated, at most 30 VIs: the redirect may be what lets this screen present at all.
        static int s_heldVis = 0;
        const bool held = g_rs64_cpu_cleared_fb.load(std::memory_order_acquire) != 0 || g_rs64_transition_gate_fb.load(std::memory_order_acquire) != 0;

        s_heldVis = held ? (s_heldVis + 1) : 0;

        if (held && s_heldVis <= 30) return;

        const uint32_t w = g_most_drawn_fb_width;

        if (w < 16 || w > 1024 || w == vi->VI_WIDTH_REG) return;

        // Only redirect to a buffer that is still being drawn. The mission text crawl draws
        // only triangles into a 640-wide buffer, so the texrect-based most-drawn value stays
        // on the 512-wide hangar buffer and would black out the crawl. ROGUESQ_MENU_FIX_STALE_MS.
        static const uint64_t s_stale_ms = [](){

            const uint32_t v = env_u32("ROGUESQ_MENU_FIX_STALE_MS");

            return v ? v : 250u;
        }();

        const uint64_t now_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        if (now_ms - (uint64_t)g_most_drawn_fb_ms > s_stale_ms) return;

        static const bool s_log = env_on("ROGUESQ_LOG_MENU_FIX");
        static int s_logged = 0;

        if (s_log && s_logged++ < 200)
        {
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

    // ROGUESQ_BOOT_TARGET=menu | demo:N | level:<id>[,craft]. menu/demo fast-forward the intro
    // and press START (the game's own title skip) once the cinematic overlay is up, stopping when
    // the menu overlay is resident. level:<id> launches from the game thread (rs64_boot_target_level
    // in upstream_compat.cpp), so it gets the fast-forward only.
    // ROGUESQ_CINE_FASTFWD=N adds N per present to the submitted-frame counter the cinematic times from (0x8011A890) until
    // the cutscene's end frame, so the timeline completes through the game's own logic.
    // ROGUESQ_SKIP_DEMO=<n> pins the attract demoId
    // (0x80130B54) and its cycle byte so that demo loops. 0=Tatooine 1=JadeMoon 2=KileII
    // 3=Taloraan 4=Fest 5=TrenchRun.
    void drive_boot_target()
    {

        enum { BOOT_OFF = -1, BOOT_MENU, BOOT_DEMO, BOOT_LEVEL };

        static const int s_mode = [](){

            const char* v = env_str("ROGUESQ_BOOT_TARGET");

            if (!v) return (int)BOOT_OFF;
            if (!std::strncmp(v, "menu", 4))  return (int)BOOT_MENU;
            if (!std::strncmp(v, "demo", 4))  return (int)BOOT_DEMO;
            if (!std::strncmp(v, "level", 5) || !std::strncmp(v, "abort", 5)) return (int)BOOT_LEVEL;

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

            return (s_mode == BOOT_DEMO) ? s_demo_arg : -1;
        }();

        if (!app->core.RDRAM) return;

        if (s_ffwd > 0) {
            const uint32_t cut = diag().rd32(0x0B1904);

            if (cut >= 0x80000000u && cut < 0x80800000u)
            {
                const uint32_t gate = diag().rd32(0x0B0B28);
                const uint32_t end_frame = diag().rd32((cut - 0x80000000u) + 0x44);

                if (end_frame > 0x10 && end_frame < 0x100000u && gate < (end_frame - 0xA))
                {
                    diag().wr32(0x11A890, diag().rd32(0x11A890) + (uint32_t)s_ffwd);

                    static int s_log = 0;

                    if ((s_log++ & 127) == 0)
                    {
                        fprintf(stderr, "[cine-ffwd] +%d/present gateCtr=%u/%u cutscene=0x%08X\n", s_ffwd, gate, end_frame, cut);
                        fflush(stderr);
                    }
                }
            }
        }

        if (s_mode != BOOT_OFF)
        {
            static bool s_seen_cine = false, s_skip_done = false;

            if (g_active_overlay == 2) s_seen_cine = true;

            if (!s_skip_done && s_seen_cine && g_active_overlay == 1)
            {
                s_skip_done = true;
                g_boot_pulse_start = 0;

                fprintf(stderr, "[boot-skip] menu handoff reached; START pulse off\n");
                fflush(stderr);
            }

            // All nav targets reach the front-end menu; pulse START through the intro handoff.
            // (demo-immediate is blocked by the custom menu displacing the attract idle path.)
            g_boot_pulse_start = (s_skip_done || g_active_overlay != 2) ? 0 : 1;
        }

        if (s_skip_demo >= 0 && s_skip_demo <= 5) {
            diag().wr8(0x130B54, (uint8_t)s_skip_demo);
            diag().wr8(0x130B55, 0);
        }
    }

    // The game-state classifier every present, then the ROGUESQ_LOG_GAMESTATE trace.
    void poll_gamestate()
    {

        if (!app->core.RDRAM) return;

        // Every present: the nav sequencer needs fresh state to catch fast menu screens.
        rs64_state_poll((const uint8_t*)app->core.RDRAM);

        diag().log_gamestate();
    }
};

std::unique_ptr<ultramodern::renderer::RendererContext>
create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window, bool developer_mode)
{
    return std::make_unique<RT64Context>(rdram, window, developer_mode);
}
} // namespace recomp

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
