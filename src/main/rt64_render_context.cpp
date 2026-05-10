#include <memory>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#define HLSL_CPU
#include "hle/rt64_application.h"

#include "ultramodern/ultramodern.hpp"
#include "ultramodern/renderer_context.hpp"

// Shared Application accessor for the LLE DPC bridge (src/rsp/dpc_bridge.cpp).
// The bridge needs to forward Factor 5 raw RDP byte ranges via
// processDisplayLists(isHLE=false). Set on construction, cleared on shutdown.
static std::atomic<RT64::Application*> g_rt64_app{nullptr};

extern "C" void rs64_dpc_get_cumulative_histogram(uint32_t out[64]);
extern "C" uint32_t rs64_dpc_get_cumulative_fullsyncs();

// RDP/RSP register state owned by this file
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

// ---------------------------------------------------------------------------
// RT64Context — wraps RT64::Application as a RendererContext
// ---------------------------------------------------------------------------
namespace recomp {

class RT64Context : public ultramodern::renderer::RendererContext {
public:
    std::unique_ptr<RT64::Application> app;
    static inline std::atomic<bool> s_hle_disabled{false};

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
        // Force developer mode ON so the RT64 ImGui debugger is visible —
        // gives a direct view of workloads / draw calls / framebuffers /
        // shaders RT64 is producing. Even if game pixels stay black, the
        // debugger overlays render so we can confirm RT64 itself is alive.
        // Disable by setting ROGUESQ_HLE_DEV_MODE=0.
        bool dev_mode_on = true;
        if (const char* v = std::getenv("ROGUESQ_HLE_DEV_MODE")) {
            if (v[0] == '0') dev_mode_on = false;
        }
        app->userConfig.developerMode = debug || dev_mode_on;
        app->userConfig.displayBuffering = RT64::UserConfiguration::DisplayBuffering::Triple;
        // Diagnostic: ROGUESQ_GFX_API=vulkan forces Vulkan instead of the
        // default D3D12 (Automatic on Windows). Lets us isolate whether crashes
        // are DX12-specific or apply to both backends.
        if (const char* api = std::getenv("ROGUESQ_GFX_API")) {
            if (std::string_view(api) == "vulkan") {
                app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Vulkan;
                fprintf(stderr, "[RT64] graphics API forced to Vulkan via ROGUESQ_GFX_API\n");
            } else if (std::string_view(api) == "d3d12") {
                app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::D3D12;
                fprintf(stderr, "[RT64] graphics API forced to D3D12 via ROGUESQ_GFX_API\n");
            }
        }

        uint32_t thread_id = 0;
#ifdef _WIN32
        thread_id = window_handle.thread_id;
#endif
        setup_result = map_result(app->setup(thread_id));
        if (setup_result != ultramodern::renderer::SetupResult::Success) {
            fprintf(stderr, "[RT64] setup failed: %d\n", (int)setup_result);
            app = nullptr;
        }
        else if (app && app->appWindow) {
            // Diagnostic: dump the post-setup state of RT64's window/filter
            // so we know whether F1-F4 keypresses can reach the inspector.
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
        if (app) {
            // Disabled for Factor 5: PresentEarly only commits a workload if
            // its colorImage.address matches an fbAddress already in
            // viHistory. Factor 5's render-then-swap flow renders to a back
            // buffer BEFORE VI samples it, so PresentEarly's check fails for
            // every workload → nothing is presented → black screen.
            // Default SkipBuffering presents via the normal VI updateScreen
            // path, which follows the actual swap chain. ROGUESQ_HLE_PRESENT_EARLY=1
            // forces PresentEarly back on for testing.
            const char* v = std::getenv("ROGUESQ_HLE_PRESENT_EARLY");
            if (v && v[0] && v[0] != '0') {
                app->enhancementConfig.presentation.mode =
                    RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
                app->updateEnhancementConfig();
            }
            // else: leave as default SkipBuffering.
        }
    }

    void send_dl(const OSTask* task) override {
        // Standard HLE pipeline (matches Zelda64Recompiled / Starfox64Recomp).
        // RT64's GBI database now includes the Factor 5 ucode signatures (see
        // lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp), so loadUCodeGBI matches
        // and dispatches commands via the F3DFACTOR5 handlers.
        //
        // Mask 0x3FFFFFF strips the KSEG0 bits to get the physical RDRAM
        // offset that processDisplayLists expects.
        if (app) {
            static int s_count = 0;
            static uint32_t s_last_ucode = 0;
            static uint32_t s_last_data = 0;
            int n = ++s_count;
            const bool ucode_changed = (task->t.ucode != s_last_ucode) || (task->t.ucode_data != s_last_data);
            if (n <= 8 || (n & 63) == 0 || ucode_changed) {
                fprintf(stderr,
                    "[hle send_dl #%d]%s type=%u ucode=0x%08X data=0x%08X dl=0x%08X size=%u\n",
                    n, ucode_changed ? " [UCODE-CHANGE]" : "",
                    (unsigned)task->t.type, (unsigned)task->t.ucode,
                    (unsigned)task->t.ucode_data, (unsigned)task->t.data_ptr,
                    (unsigned)task->t.data_size);
                fflush(stderr);
            }
            s_last_ucode = task->t.ucode;
            s_last_data = task->t.ucode_data;
            if (s_hle_disabled.load(std::memory_order_relaxed)) {
                return;
            }
            app->state->rsp->reset();
            app->interpreter->loadUCodeGBI(
                task->t.ucode & 0x3FFFFFF,
                task->t.ucode_data & 0x3FFFFFF,
                true);
#ifdef _WIN32
            __try {
                app->processDisplayLists(app->core.RDRAM,
                                         task->t.data_ptr & 0x3FFFFFF,
                                         0,
                                         /*isHLE*/ true);

                // 2026-05-10 update: Factor 5's ucode DOES emit G_RDPFULLSYNC
                // (op 0xE9) mid-DL — confirmed via opcode tracer. RT64's own
                // fullSync handler runs there, advancing the workload cursor
                // and presenting the populated workload normally. An external
                // fullSync from here runs on the *next* (empty) workload,
                // overwriting the real frame with nothing → black screen.
                // Default OFF; only enable for debugging via
                // ROGUESQ_HLE_AUTO_FULLSYNC=1.
                static bool s_auto_fullsync = []() {
                    const char* v = std::getenv("ROGUESQ_HLE_AUTO_FULLSYNC");
                    return v && v[0] && v[0] != '0';
                }();
                if (app->state) {
                    // Conditional fullSync. Factor 5 emits its own
                    // G_RDPFULLSYNC mid-DL, so most tasks DON'T need an
                    // external one — calling fullSync on the new (empty)
                    // workload that Factor 5's fullSync advanced to would
                    // overwrite the real frame with nothing.
                    //
                    // BUT some Factor 5 DLs end without emitting fullSync,
                    // leaving fbPairCount > fbPairSubmitted (uncommitted
                    // work). If we don't flush, that work piles up across
                    // tasks and eventually trips checkRDRAM assertions.
                    //
                    // Strategy: fullSync only if the current workload has
                    // unsubmitted fbPairs OR pending triangles. Otherwise
                    // assume Factor 5's natural fullSync already ran.
                    int wc = app->state->ext.workloadQueue->writeCursor;
                    auto& wl = app->state->ext.workloadQueue->workloads[wc];
                    const bool needs_flush = (wl.fbPairCount > wl.fbPairSubmitted) ||
                                             (app->state->drawCall.triangleCount > 0);
                    if (needs_flush) {
                        // Bracket fullSync so the dlCpuProfiler ends in the
                        // stopped state processDisplayLists left it in.
                        app->state->dlCpuProfiler.start();
                        app->state->fullSync();
                        app->state->dlCpuProfiler.end();
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                // Set the disable flag and write the log FIRST — any further
                // RT64 state poking can re-AV and we'd lose visibility.
                s_hle_disabled.store(true, std::memory_order_relaxed);
                fprintf(stderr, "[hle send_dl] SEH in processDisplayLists "
                                "ucode=0x%08X data=0x%08X dl=0x%08X — HLE submission "
                                "DISABLED for the rest of this session.\n",
                                (unsigned)task->t.ucode, (unsigned)task->t.ucode_data,
                                (unsigned)task->t.data_ptr);
                fflush(stderr);
                if (app && app->state) {
                    app->state->dlCpuProfiler.startedTimestamp = RT64::Timestamp{};
                }
            }
#else
            app->processDisplayLists(app->core.RDRAM,
                                     task->t.data_ptr & 0x3FFFFFF,
                                     0,
                                     /*isHLE*/ true);
#endif
        }
    }

    void update_screen() override {
        if (app) {
            // Always log first 4 + every 64th update_screen so we can tell
            // whether VI events are firing at all. The visual-output question
            // depends entirely on this being called regularly.
            static int s_n = 0;
            static bool s_filter_logged = false;
            ++s_n;
            // Log once when the SDL filter actually installs.
            if (!s_filter_logged && app->appWindow && app->appWindow->sdlEventFilterInstalled) {
                fprintf(stderr,
                    "[RT64] sdlEventFilter NOW installed at update_screen #%d (sdlWindow=%p)\n",
                    s_n, (void*)app->appWindow->sdlWindow);
                fflush(stderr);
                s_filter_logged = true;
            }
            if (s_n <= 4 || (s_n & 63) == 0) {
                ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
                // Sample 16 bytes at VI_ORIGIN to see if there's actual pixel
                // data there. If all zeros, VI is sampling an empty buffer.
                uint32_t origin = vi->VI_ORIGIN_REG & 0x00FFFFFF;
                uint32_t any_nonzero = 0;
                if (app && app->core.RDRAM && origin > 0 && origin + 16 < 0x800000) {
                    for (int i = 0; i < 16; i++) {
                        any_nonzero |= app->core.RDRAM[origin + i];
                    }
                }
                fprintf(stderr,
                    "[vi] update_screen #%d origin=0x%08X width=%u status=0x%X v_current=%u nonzero=%d fs=%u\n",
                    s_n, vi->VI_ORIGIN_REG, vi->VI_WIDTH_REG,
                    vi->VI_STATUS_REG, vi->VI_V_CURRENT_LINE_REG, any_nonzero != 0,
                    rs64_dpc_get_cumulative_fullsyncs());
                // Every 256 updates, also dump the cumulative opcode histogram.
                if ((s_n & 255) == 0) {
                    uint32_t hist[64];
                    rs64_dpc_get_cumulative_histogram(hist);
                    uint32_t total = 0;
                    for (int i = 0; i < 64; ++i) total += hist[i];
                    fprintf(stderr, "  [opcode-hist total=%u top:", total);
                    uint32_t copy[64];
                    for (int i = 0; i < 64; ++i) copy[i] = hist[i];
                    for (int slot = 0; slot < 6; ++slot) {
                        int max_idx = 0;
                        for (int i = 0; i < 64; ++i) {
                            if (copy[i] > copy[max_idx]) max_idx = i;
                        }
                        if (copy[max_idx] == 0) break;
                        fprintf(stderr, " op%02X=%u", max_idx, copy[max_idx]);
                        copy[max_idx] = 0;
                    }
                    fprintf(stderr, "]\n");
                }
                fflush(stderr);
            }
            app->updateScreen();
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
};

std::unique_ptr<ultramodern::renderer::RendererContext>
create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window, bool developer_mode) {
    return std::make_unique<RT64Context>(rdram, window, developer_mode);
}

} // namespace recomp

// Free-function bridge for the LLE DPC pipeline (see src/rsp/dpc_bridge.cpp).
// Forwards raw RDP byte ranges from the recompiled Factor 5 ucode into the
// live RT64::Application for rasterization (isHLE=false).
namespace ultramodern {
    // Catch AVs from RT64's processDisplayLists. Currently
    // RT64::RDP::updateCallTexcoords (rt64_rdp.cpp:1343) reads
    // workload.drawData.callTiles[drawCall.tileIndex + t] before that
    // vector has been populated, when Factor 5's ucode emits a TEXRECT
    // without prior tile setup. The OOB returns garbage in release
    // builds (and our _ITERATOR_DEBUG_LEVEL=0 path) which then AVs on
    // the load. Catch the SEH exception here and skip the submission;
    // we lose that frame's content but the game continues running.
    // SEH-protected submission. RT64's processDisplayLists can AV in
    // RDP::updateCallTexcoords (rt64_rdp.cpp:1343) when Factor 5's ucode
    // emits a TEXRECT-like sequence without prior tile setup —
    // drawCall.tileCount > 0 but workload.drawData.callTiles is empty,
    // so the indexed read goes past end.
    //
    // After the AV, even if we reset dlCpuProfiler, downstream RT64 state
    // is corrupted in multiple places (next assert is at
    // rt64_framebuffer_manager.cpp:248). Rather than chase each, we
    // disable submission entirely after the first SEH. Game continues to
    // run, just without further graphics output. Better than crashing.
    // TODO: pre-validate at dpc_bridge to drop bad commands BEFORE submit.
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
            // Reset the profiler timer too in case anything still tries to
            // touch it (the disable flag should prevent that, but be safe).
            if (app->state) {
                app->state->dlCpuProfiler.startedTimestamp = RT64::Timestamp{};
            }
            s_rdp_disabled.store(true, std::memory_order_relaxed);
            fprintf(stderr, "[rdp-submit] SEH in processDisplayLists "
                            "lo=0x%08X hi=0x%08X — RDP submission DISABLED for the rest "
                            "of this session (RT64 state corrupted by AV).\n",
                            lo_phys, hi_phys);
            // Dump cumulative opcode histogram so we can see what kinds of
            // commands the game submitted before the crash. Tells us
            // whether actual rendering work (TEXRECT, FILLRECT, triangles)
            // was happening.
            uint32_t hist[64];
            rs64_dpc_get_cumulative_histogram(hist);
            uint32_t total = 0;
            for (int i = 0; i < 64; ++i) total += hist[i];
            fprintf(stderr, "[rdp-submit] cumulative opcode histogram (total=%u):\n", total);
            // Print top 12 by count
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
            // Diagnostic: log the LAST submission's last bytes so we can see
            // what RDP commands trigger RT64's vector-OOB assert. Gate behind
            // ROGUESQ_LOG_RDP_SUBMIT=1.
            static int s_log = -1;
            if (s_log < 0) {
                const char* e = std::getenv("ROGUESQ_LOG_RDP_SUBMIT");
                s_log = (e && *e && *e != '0') ? 1 : 0;
            }
            if (s_log) {
                static int s_count = 0;
                ++s_count;
                uint32_t len = hi_phys - lo_phys;
                fprintf(stderr, "[rdp-submit #%d] lo=0x%08X hi=0x%08X len=%u\n",
                        s_count, lo_phys, hi_phys, len);
                // Dump first 32 bytes of submission (4 commands worth)
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
