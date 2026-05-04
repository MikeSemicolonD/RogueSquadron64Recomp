#include <memory>
#include <atomic>
#include <cstdio>

#define HLSL_CPU
#include "hle/rt64_application.h"

#include "ultramodern/ultramodern.hpp"
#include "ultramodern/renderer_context.hpp"

// Shared Application accessor — set on construction, queried by the LLE DPC
// bridge in src/rsp/dpc_bridge.cpp to forward Factor5 RDP commands.
static std::atomic<RT64::Application*> g_rt64_app{nullptr};
extern "C" RT64::Application* rs64_get_rt64_app() { return g_rt64_app.load(); }

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
        app->userConfig.developerMode = debug;
        app->userConfig.displayBuffering = RT64::UserConfiguration::DisplayBuffering::Triple;

        uint32_t thread_id = 0;
#ifdef _WIN32
        thread_id = window_handle.thread_id;
#endif
        setup_result = map_result(app->setup(thread_id));
        if (setup_result != ultramodern::renderer::SetupResult::Success) {
            fprintf(stderr, "[RT64] setup failed: %d\n", (int)setup_result);
            app = nullptr;
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
            app->enhancementConfig.presentation.mode =
                RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
            app->updateEnhancementConfig();
        }
    }

    void send_dl(const OSTask* task) override {
        if (app) {
            app->state->rsp->reset();
            app->interpreter->loadUCodeGBI(
                task->t.ucode & 0x3FFFFFF,
                task->t.ucode_data & 0x3FFFFFF,
                true);
            app->processDisplayLists(app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF, 0, true);
        }
    }

    void send_rdp_range(uint32_t lo_phys, uint32_t hi_phys) override {
        if (app && hi_phys > lo_phys) {
            app->processDisplayLists(app->core.RDRAM, lo_phys, hi_phys, /*isHLE*/ false);
        }
    }

    void update_screen() override {
        if (app) app->updateScreen();
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
