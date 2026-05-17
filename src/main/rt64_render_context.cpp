#include <memory>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

#define HLSL_CPU
#include "hle/rt64_application.h"

#include "ultramodern/ultramodern.hpp"
#include "ultramodern/renderer_context.hpp"

// Material free-list integrity probe (src/main/upstream_compat.cpp).
extern "C" void rs64_matfreelist_check(uint8_t*, const char*);

// Shared Application accessor for the LLE DPC bridge (src/rsp/dpc_bridge.cpp).
// The bridge needs to forward Factor 5 raw RDP byte ranges via
// processDisplayLists(isHLE=false). Set on construction, cleared on shutdown.
static std::atomic<RT64::Application*> g_rt64_app{nullptr};

extern "C" void rs64_dpc_get_cumulative_histogram(uint32_t out[64]);
extern "C" uint32_t rs64_dpc_get_cumulative_fullsyncs();
extern "C" void rs64_cine_dump_if_stuck(void);
extern "C" void rs64_cine_progress_log(void);
// LLE GFX entry — runs the RSP recompile (factor5_boot + factor5_ucode)
// against this task's data. Defined in main.cpp. OSTask comes from
// ultramodern/ultra64.h (already included via ultramodern.hpp).
extern "C" int rs64_run_lle_gfx(uint8_t* rdram, const OSTask* task);

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
        // Dev mode default: ON for Debug builds (so F1 inspector is available),
        // OFF for Release. Override either way with ROGUESQ_HLE_DEV_MODE=0/1.
        // The inspector can be unstable with our HLE flow under load — fall
        // back to ROGUESQ_HLE_DEV_MODE=0 if it locks up an investigation.
#ifdef _DEBUG
        bool dev_mode_on = true;
#else
        bool dev_mode_on = false;
#endif
        if (const char* v = std::getenv("ROGUESQ_HLE_DEV_MODE")) {
            dev_mode_on = (v[0] && v[0] != '0');
        }
        app->userConfig.developerMode = debug || dev_mode_on;
        app->userConfig.displayBuffering = RT64::UserConfiguration::DisplayBuffering::Triple;

        // PresentEarly ON by default. Our cinematic stays on a single VI fb
        // address; without PresentEarly, RT64's updateScreen only pushes a
        // present when VI changes or RDRAM at VI_ORIGIN changes — neither
        // happens for HLE single-buffer flow, so rendered fbPairs never
        // reach the swapchain. PresentEarly pushes presents directly from
        // fullSync. Opt out via ROGUESQ_HLE_PRESENT_EARLY=0.
        bool present_early = true;
        if (const char* v = std::getenv("ROGUESQ_HLE_PRESENT_EARLY")) {
            present_early = (v[0] && v[0] != '0');
        }
        if (present_early) {
            app->enhancementConfig.presentation.mode =
                RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
        }

        // 2026-05-10: Apply Zelda64Recomp's RT64 enhancement-config settings.
        // Without these, RT64's textureLOD selection and gbi branching take
        // different paths that cause our texture lookups to sample stale or
        // empty TMEM regions → glyph texrects render invisibly.
        //
        // forceBranch=true: forces gbi depth branches, preventing LODs from
        // kicking in for textures that don't have proper mipmap chains.
        //
        // textureLOD.scale=true: scales LODs based on output resolution so
        // higher-res rendering doesn't bias toward LOD 0 unnecessarily.
        app->enhancementConfig.f3dex.forceBranch = true;
        app->enhancementConfig.textureLOD.scale = true;
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
        else {
            // Propagate construction-time enhancementConfig changes (e.g.
            // PresentEarly) into RT64's internal state. setup() initializes
            // before these are read, so without this call the changes stay
            // dormant.
            app->updateEnhancementConfig();
            fprintf(stderr, "[RT64] enhancementConfig: presentMode=%d forceBranch=%d textureLOD.scale=%d\n",
                    (int)app->enhancementConfig.presentation.mode,
                    (int)app->enhancementConfig.f3dex.forceBranch,
                    (int)app->enhancementConfig.textureLOD.scale);
            fflush(stderr);
        }
        if (app && app->appWindow) {
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
        // PresentEarly is set at construction (see ctor); this host hook
        // would re-apply it, but constructor-time setup is sufficient.
        // Respects ROGUESQ_HLE_PRESENT_EARLY=0 opt-out.
        if (app) {
            bool present_early = true;
            if (const char* v = std::getenv("ROGUESQ_HLE_PRESENT_EARLY")) {
                present_early = (v[0] && v[0] != '0');
            }
            if (present_early) {
                app->enhancementConfig.presentation.mode =
                    RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
                app->updateEnhancementConfig();
            }
        }
    }

    void send_dl(const OSTask* task) override {
        // ROGUESQ_LLE_FORCE=1: route this GFX task through the LLE RSP
        // recompile path INSTEAD of HLE. factor5_gfx_runner reproduces what
        // real-hardware RSP would do — runs boot+main ucode against DMEM,
        // emits RDP byte stream via mtc0 DPC_END writes, src/rsp/dpc_bridge.cpp
        // forwards those bytes to RT64 via processDisplayLists(isHLE=false).
        // This is the path needed for Factor 5's op 0x02 (vertex transform)
        // to actually emit triangles — HLE drops it.
        static int s_lle_force = -1;
        if (s_lle_force < 0) {
            const char* v = std::getenv("ROGUESQ_LLE_FORCE");
            s_lle_force = (v && *v && v[0] != '0') ? 1 : 0;
            if (s_lle_force) {
                fprintf(stderr, "[lle-force] LLE path enabled for M_GFXTASK; "
                                "HLE send_dl will skip processDisplayLists\n");
                fflush(stderr);
            }
        }
        if (s_lle_force && app) {
            // Gate LLE to attribution-shape DLs only. Cinematic-phase tasks
            // hit Unhandled jump target in factor5_ucode_recompiled and
            // corrupt RT64's deferred state (which then asserts 4-bit
            // readback downstream). Attribution DL signature: data_ptr =
            // 0x80720108 for task #1, 0x80720318 for task #2 of the same
            // phase (observed). Both live in the 0x80720000 page. Restrict
            // LLE to data_ptr in that page so cinematic tasks bypass it.
            //
            // ROGUESQ_LLE_UNGATED=1 disables this gate (debug/regression).
            const uint32_t dlp = (uint32_t)task->t.data_ptr;
            static int s_lle_ungated = -1;
            if (s_lle_ungated < 0) {
                const char* v = std::getenv("ROGUESQ_LLE_UNGATED");
                s_lle_ungated = (v && *v && v[0] != '0') ? 1 : 0;
            }
            const bool in_attribution_page = ((dlp & 0xFFFF0000u) == 0x80720000u);
            if (s_lle_ungated || in_attribution_page) {
                static int s_n = 0;
                int n = ++s_n;
                int r = rs64_run_lle_gfx(app->core.RDRAM, task);
                if (n <= 8 || (n & 31) == 0) {
                    fprintf(stderr, "[lle send_dl #%d] gate=%s exit=%d task->ucode=0x%08X dl=0x%08X\n",
                            n, in_attribution_page ? "attr" : "ungated",
                            r, (unsigned)task->t.ucode, dlp);
                    fflush(stderr);
                }
                // ROGUESQ_LLE_SOLO=1 skips the HLE fallthrough below. By default
                // we run HLE in parallel so HLE's green fillRect-override marker
                // stays visible alongside whatever LLE produces.
                static int s_lle_solo = -1;
                if (s_lle_solo < 0) {
                    const char* v = std::getenv("ROGUESQ_LLE_SOLO");
                    s_lle_solo = (v && *v && v[0] != '0') ? 1 : 0;
                }
                if (s_lle_solo) {
                    return;
                }
                // Fall through to HLE below.
            } else {
                // Cinematic / N64-logo / other tasks: HLE only. Skip LLE.
                static int s_skip_count = 0;
                int sn = ++s_skip_count;
                if (sn <= 4 || (sn & 127) == 0) {
                    fprintf(stderr, "[lle send_dl skip #%d] non-attribution DL dl=0x%08X (LLE bypassed)\n",
                            sn, dlp);
                    fflush(stderr);
                }
            }
        }

        // Standard HLE pipeline (matches Zelda64Recompiled / Starfox64Recomp).
        // RT64's GBI database now includes the Factor 5 ucode signatures (see
        // lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp), so loadUCodeGBI matches
        // and dispatches commands via the F3DFACTOR5 handlers.
        //
        // Mask 0x3FFFFFF strips the KSEG0 bits to get the physical RDRAM
        // offset that processDisplayLists expects.
        // ROGUESQ_LOG_OPCODE_HIST=1: per-call opcode histogram of the first
        // 256 DL commands at task->t.data_ptr. Lets us compare which
        // F3DFACTOR5 opcodes fire during attribution vs N64-logo phase to
        // test the "some opcode carries attribution-glyph semantics but is
        // currently no-op'd in our HLE" hypothesis.
        if (app) {
            // ROGUESQ_DUMP_UCODE=path — dump task ucode (IMEM) + ucode_data
            // (DMEM-bound segment) to disk on the first call so the F3DFACTOR5
            // ucode can be disassembled offline to decode op 0x02 semantics.
            static int s_dumped_ucode = 0;
            if (!s_dumped_ucode) {
                const char* dump_path = std::getenv("ROGUESQ_DUMP_UCODE");
                if (dump_path && *dump_path) {
                    s_dumped_ucode = 1;
                    const uint32_t ucode_phys = (uint32_t)task->t.ucode & 0x3FFFFFF;
                    const uint32_t udata_phys = (uint32_t)task->t.ucode_data & 0x3FFFFFF;
                    const uint32_t udata_size = task->t.ucode_data_size ? task->t.ucode_data_size : 0x800;
                    uint8_t* rdram = app->core.RDRAM;
                    // Standard RSP ucode IMEM is 4KB. Dump 8KB to be safe.
                    char ipath[512], dpath[512];
                    snprintf(ipath, sizeof ipath, "%s.imem.bin", dump_path);
                    snprintf(dpath, sizeof dpath, "%s.dmem.bin", dump_path);
                    FILE* f = fopen(ipath, "wb");
                    if (f) {
                        uint8_t buf[0x2000];
                        for (uint32_t i = 0; i < sizeof(buf); ++i) {
                            uint32_t off = ucode_phys + i;
                            buf[i] = (off < 0x800000u) ? rdram[off ^ 3] : 0;
                        }
                        fwrite(buf, 1, sizeof(buf), f);
                        fclose(f);
                        fprintf(stderr, "[dump-ucode] wrote IMEM %u bytes to %s (src=0x%08X)\n",
                                (unsigned)sizeof(buf), ipath, ucode_phys);
                    }
                    f = fopen(dpath, "wb");
                    if (f) {
                        std::vector<uint8_t> buf(udata_size);
                        for (uint32_t i = 0; i < udata_size; ++i) {
                            uint32_t off = udata_phys + i;
                            buf[i] = (off < 0x800000u) ? rdram[off ^ 3] : 0;
                        }
                        fwrite(buf.data(), 1, udata_size, f);
                        fclose(f);
                        fprintf(stderr, "[dump-ucode] wrote DMEM %u bytes to %s (src=0x%08X)\n",
                                udata_size, dpath, udata_phys);
                    }
                    fflush(stderr);
                }
            }
            // ROGUESQ_DUMP_VTXDATA=path — dump 64 KB starting at RAM
            // 0x80700000 and 0x80710000 on the first send_dl call. These
            // are the addresses op 0x01 references in attribution DLs
            // (alternating between consecutive submissions). Offline
            // decode reveals what vertex / asset data the F3DFACTOR5
            // ucode reads to drive op 0x02's matrix-vertex pipeline.
            static int s_dumped_vtx = 0;
            if (!s_dumped_vtx) {
                const char* dump_path = std::getenv("ROGUESQ_DUMP_VTXDATA");
                if (dump_path && *dump_path) {
                    s_dumped_vtx = 1;
                    uint8_t* rdram = app->core.RDRAM;
                    // Extended dump set: add the .data segment region around
                    // the state-setup sub-DL (0x80037658) to search for
                    // static vertex arrays.
                    for (uint32_t base : { 0x00700000u, 0x00710000u, 0x00037000u }) {
                        char path[512];
                        snprintf(path, sizeof path, "%s.0x%08X.bin", dump_path, base);
                        FILE* f = fopen(path, "wb");
                        if (f) {
                            uint8_t buf[0x10000];
                            for (uint32_t i = 0; i < sizeof(buf); ++i) {
                                uint32_t off = base + i;
                                buf[i] = (off < 0x800000u) ? rdram[off ^ 3] : 0;
                            }
                            fwrite(buf, 1, sizeof(buf), f);
                            fclose(f);
                            fprintf(stderr, "[dump-vtxdata] wrote 64 KB to %s\n", path);
                        }
                    }
                    fflush(stderr);
                }
            }
            static int s_op_hist = -1;
            if (s_op_hist < 0) {
                const char* v = std::getenv("ROGUESQ_LOG_OPCODE_HIST");
                s_op_hist = (v && *v && v[0] != '0') ? 1 : 0;
            }
            if (s_op_hist) {
                static int s_oh_count = 0;
                int ohn = ++s_oh_count;
                const uint32_t dl_phys = (uint32_t)task->t.data_ptr & 0x3FFFFFF;
                uint8_t* rdram = app->core.RDRAM;
                uint32_t hist[256] = {0};
                uint32_t total = 0;
                // Walk up to 256 8-byte commands (or stop on G_ENDDL=0xDF or
                // a zeroed slot). Follow at most 2 G_DL jumps so we get a
                // representative sample even when Factor 5 chains via G_DL.
                uint32_t addr = dl_phys;
                uint32_t stack[16];
                int sp = 0;
                int jumps_left = 16;
                // Track first 8 instances of each "interesting" no-op'd
                // opcode so we can see if their payloads encode something
                // meaningful (like a DMA src/dst pair).
                uint64_t op02_payloads[8] = {0};
                int op02_count = 0;
                uint64_t op80_payloads[8] = {0};
                int op80_count = 0;
                for (int i = 0; i < 1024; ++i) {
                    if (addr + 8u > 0x800000u) break;
                    uint8_t op = rdram[addr ^ 3];
                    hist[op]++;
                    total++;
                    if (op == 0x02 && op02_count < 8) {
                        uint64_t w = 0;
                        for (int k = 0; k < 8; ++k) {
                            w = (w << 8) | rdram[(addr + k) ^ 3];
                        }
                        op02_payloads[op02_count++] = w;
                    }
                    if (op == 0x80 && op80_count < 8) {
                        uint64_t w = 0;
                        for (int k = 0; k < 8; ++k) {
                            w = (w << 8) | rdram[(addr + k) ^ 3];
                        }
                        op80_payloads[op80_count++] = w;
                    }
                    // F3D-style G_ENDDL = 0xB8 (Factor 5 uses F3D's 0xB8,
                    // not F3DEX2's 0xDF). Pop on B8.
                    if (op == 0xB8) {
                        if (sp > 0) { addr = stack[--sp]; continue; }
                        break;
                    }
                    if (op == 0xDF) break;
                    // F3D G_DL = 0x06, F3DEX2 G_DL = 0xDE.
                    // 0x06 in Factor 5: branch=byte1 (0=push, 1=branch).
                    if ((op == 0x06 || op == 0xDE) && jumps_left > 0) {
                        uint8_t branch = rdram[(addr + 1) ^ 3];
                        uint32_t w1 = 0;
                        for (int k = 0; k < 4; ++k) {
                            w1 = (w1 << 8) | rdram[(addr + 4 + k) ^ 3];
                        }
                        uint32_t target = w1 & 0x3FFFFFF;
                        if (branch == 0) {  // G_DL push: save return addr
                            if (sp < 16) stack[sp++] = addr + 8;
                        }
                        addr = target;
                        jumps_left--;
                        continue;
                    }
                    addr += 8;
                }
                // Dump full DL bytes for the first call so we can hand-decode
                // the attribution DL structure.
                if (ohn == 1) {
                    uint32_t a = dl_phys;
                    int dump_stack_sp = 0;
                    uint32_t dump_stack[16];
                    int dump_jumps = 16;
                    fprintf(stderr, "[opcode-hist #1 FULL DL DUMP] start=0x%08X\n", dl_phys);
                    for (int i = 0; i < 256; ++i) {
                        if (a + 8u > 0x800000u) break;
                        uint64_t w = 0;
                        for (int k = 0; k < 8; ++k) {
                            w = (w << 8) | rdram[(a + k) ^ 3];
                        }
                        uint8_t op = rdram[a ^ 3];
                        fprintf(stderr, "  %08X: %016llX  op=%02X\n",
                                a, (unsigned long long)w, op);
                        if (op == 0xB8) {
                            if (dump_stack_sp > 0) { a = dump_stack[--dump_stack_sp]; continue; }
                            break;
                        }
                        if (op == 0xDF) break;
                        if ((op == 0x06 || op == 0xDE) && dump_jumps > 0) {
                            uint8_t branch = rdram[(a + 1) ^ 3];
                            uint32_t w1 = 0;
                            for (int k = 0; k < 4; ++k) {
                                w1 = (w1 << 8) | rdram[(a + 4 + k) ^ 3];
                            }
                            uint32_t target = w1 & 0x3FFFFFF;
                            if (branch == 0) {
                                if (dump_stack_sp < 16) dump_stack[dump_stack_sp++] = a + 8;
                            }
                            a = target;
                            dump_jumps--;
                            continue;
                        }
                        a += 8;
                    }
                    fflush(stderr);
                }
                fprintf(stderr, "[opcode-hist #%d] dl=0x%08X total=%u", ohn, dl_phys, total);
                for (int op = 0; op < 256; ++op) {
                    if (hist[op]) {
                        fprintf(stderr, " %02X=%u", op, hist[op]);
                    }
                }
                fprintf(stderr, "\n");
                if (op02_count > 0) {
                    fprintf(stderr, "[opcode-hist #%d op02 payloads]", ohn);
                    for (int k = 0; k < op02_count; ++k) {
                        fprintf(stderr, " %016llX", (unsigned long long)op02_payloads[k]);
                    }
                    fprintf(stderr, "\n");
                }
                if (op80_count > 0) {
                    fprintf(stderr, "[opcode-hist #%d op80 payloads]", ohn);
                    for (int k = 0; k < op80_count; ++k) {
                        fprintf(stderr, " %016llX", (unsigned long long)op80_payloads[k]);
                    }
                    fprintf(stderr, "\n");
                }
                fflush(stderr);
            }
        }
        if (app) {
            static int s_count = 0;
            static uint32_t s_last_ucode = 0;
            static uint32_t s_last_data = 0;
            int n = ++s_count;
            const bool ucode_changed = (task->t.ucode != s_last_ucode) || (task->t.ucode_data != s_last_data);
            if (n <= 8 || (n & 63) == 0 || ucode_changed) {
                // Capture workload state pre- and post-DL so we can see
                // fbPair growth and presentEarly matcher activity.
                int wq_wc_pre = app->workloadQueue ? (int)app->workloadQueue->writeCursor : -1;
                int pq_wc_pre = app->presentQueue ? (int)app->presentQueue->writeCursor : -1;
                fprintf(stderr,
                    "[hle send_dl #%d]%s type=%u ucode=0x%08X data=0x%08X dl=0x%08X size=%u wq=%d pq=%d\n",
                    n, ucode_changed ? " [UCODE-CHANGE]" : "",
                    (unsigned)task->t.type, (unsigned)task->t.ucode,
                    (unsigned)task->t.ucode_data, (unsigned)task->t.data_ptr,
                    (unsigned)task->t.data_size,
                    wq_wc_pre, pq_wc_pre);
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
            static std::atomic<int> s_seh_streak{0};
            __try {
                int wq_wc_pre = app->workloadQueue ? (int)app->workloadQueue->writeCursor : -1;
                int pq_wc_pre = app->presentQueue ? (int)app->presentQueue->writeCursor : -1;
                // Material free-list integrity around RT64's DL processing —
                // tests whether the renderer (CIMG / framebuffer writeback)
                // corrupts the node pool. Gated by ROGUESQ_LOG_MATFREELIST.
                rs64_matfreelist_check(app->core.RDRAM, "RT64 send_dl PRE-processDisplayLists");
                app->processDisplayLists(app->core.RDRAM,
                                         task->t.data_ptr & 0x3FFFFFF,
                                         0,
                                         /*isHLE*/ true);
                rs64_matfreelist_check(app->core.RDRAM, "RT64 send_dl POST-processDisplayLists");
                s_seh_streak.store(0, std::memory_order_relaxed);
                // Post-DL: see if workload + present cursors moved. Track
                // distinct transition patterns so we can spot when pq
                // *should* be advancing but isn't.
                if (n <= 8 || (n & 63) == 0) {
                    int wq_wc_post = app->workloadQueue ? (int)app->workloadQueue->writeCursor : -1;
                    int pq_wc_post = app->presentQueue ? (int)app->presentQueue->writeCursor : -1;
                    int wc = app->workloadQueue ? (int)app->workloadQueue->writeCursor : 0;
                    auto& wl = app->workloadQueue->workloads[wc];
                    auto& wlPrev = app->workloadQueue->workloads[(wc + app->workloadQueue->workloads.size() - 1) % app->workloadQueue->workloads.size()];
                    fprintf(stderr,
                        "[hle send_dl #%d post] wq=%d->%d pq=%d->%d nextFbPairs=%u prevFbPairs=%u\n",
                        n, wq_wc_pre, wq_wc_post, pq_wc_pre, pq_wc_post,
                        (unsigned)wl.fbPairCount, (unsigned)wlPrev.fbPairCount);
                    fflush(stderr);
                }

                // Default ON: needed for stability in cinematic phase (workload's
                // fbPairCount grows unbounded without flushing). Disable via
                // ROGUESQ_HLE_AUTO_FULLSYNC=0 to investigate cases where it might
                // double-fire and overwrite a present.
                static bool s_auto_fullsync = []() {
                    const char* v = std::getenv("ROGUESQ_HLE_AUTO_FULLSYNC");
                    if (v && v[0]) return v[0] != '0';
                    return true;
                }();
                if (app->state) {
                    // Conditional fullSync. Factor 5's cinematic DLs emit fullSync
                    // mid-DL but the post-fullSync workload accumulates fbPairs
                    // unbounded (60→229+ in one DL) without committing, eventually
                    // tripping RT64's internal limits. We flush when the current
                    // workload has uncommitted work; the gate skips the case where
                    // Factor 5's natural fullSync already advanced the cursor
                    // (next workload starts empty so needs_flush=false).
                    // Override OFF via ROGUESQ_HLE_AUTO_FULLSYNC=0 (default ON).
                    if (s_auto_fullsync) {
                        int wc = app->state->ext.workloadQueue->writeCursor;
                        auto& wl = app->state->ext.workloadQueue->workloads[wc];
                        const bool needs_flush = (wl.fbPairCount > wl.fbPairSubmitted) ||
                                                 (app->state->drawCall.triangleCount > 0);
                        if (needs_flush) {
                            app->state->dlCpuProfiler.start();
                            app->state->fullSync();
                            app->state->dlCpuProfiler.end();
                        }
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                // Log and try to keep going. The previous behavior latched
                // s_hle_disabled on first SEH and killed all subsequent
                // rendering — too aggressive: transient boot AVs would
                // permanently kill the screen. Only latch-off if SEHs
                // hit 3 IN A ROW with no successful submission between.
                int streak = s_seh_streak.fetch_add(1, std::memory_order_relaxed) + 1;
                fprintf(stderr, "[hle send_dl] SEH streak=%d in processDisplayLists "
                                "ucode=0x%08X data=0x%08X dl=0x%08X%s\n",
                                streak,
                                (unsigned)task->t.ucode, (unsigned)task->t.ucode_data,
                                (unsigned)task->t.data_ptr,
                                streak >= 3 ? " — HLE submission DISABLED" : "");
                fflush(stderr);
                if (streak >= 3) {
                    s_hle_disabled.store(true, std::memory_order_relaxed);
                }
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

            // Watchdog moved to its own thread (rs64_cine_start_watchdog_thread)
            // because the gfx_thread can itself be deadlocked on the same mutex
            // chain that's hanging the game thread.

            // Host-side buffer-arbiter state-machine driver. Under HLE, the game's
            // bufferArbiterProducerMark blocks on a recv that's supposed to be
            // signaled by viRetraceHandlerThread; if that signal misses (timing),
            // slot states stagnate in 0/1/2 and func_8000BF60's scan loop spins
            // waiting for state 3 or 4. We auto-advance any slot in state 2 → 3
            // (CONSUMED), state 3 → 4 (PRESENTED), state 4 → 5 (DISPLAYED),
            // state 5 → 0 (FREE). This mimics what viRetraceHandlerThread would
            // naturally do over multiple ticks but condenses it into one
            // update_screen pass. Gated by ROGUESQ_FORCE_BUFFER_PROGRESS=1.
            static const bool s_force_buffer = [](){
                const char *v = std::getenv("ROGUESQ_FORCE_BUFFER_PROGRESS");
                return v && v[0] && v[0] != '0';
            }();
            if (s_force_buffer && app->core.RDRAM) {
                // Slot state array at 0x80128EAA[N]; count byte at 0x80128EAD.
                // Conservative driver: only advance the retrace-thread's
                // transitions (4 PRESENTED → 5 DISPLAYED → 0 FREE).
                // Producer-mark's transitions (2→3 and X→4) are left to fire
                // naturally via bufferArbiterProducerMark — bypassing those
                // would cause ultramodern sync invariants to break (saw an AV
                // in ultramodern's mesgqueue/threads area on the aggressive
                // version of this driver at iter 1255).
                const uint32_t STATE_BASE = 0x128EAA;
                const uint32_t COUNT_ADDR = 0x128EAD;
                if (COUNT_ADDR < 0x800000) {
                    uint8_t total = app->core.RDRAM[COUNT_ADDR ^ 3];
                    if (total > 0 && total <= 8) {
                        for (uint8_t i = 0; i < total; i++) {
                            uint32_t addr = STATE_BASE + i;
                            if (addr >= 0x800000) continue;
                            uint8_t state = app->core.RDRAM[addr ^ 3];
                            switch (state) {
                                case 4: app->core.RDRAM[addr ^ 3] = 5; break;  // PRESENTED → DISPLAYED
                                case 5: app->core.RDRAM[addr ^ 3] = 0; break;  // DISPLAYED → FREE
                                default: break;  // leave 0/1/2/3 to game code
                            }
                        }
                    }
                }
            }

            // Game-state poller: read engine globals via RDRAM and log changes.
            // Tells us whether the game is actually progressing past boot, even
            // when the screen is black. Enable via ROGUESQ_LOG_GAMESTATE=1.
            static const bool s_log_gs = [](){
                const char *v = std::getenv("ROGUESQ_LOG_GAMESTATE");
                return v && v[0] && v[0] != '0';
            }();
            if (s_log_gs && app->core.RDRAM && (s_n & 63) == 0) {
                // Globals from docs/game-architecture.md. RDRAM is little-endian
                // word-swap relative to MIPS BE — use byte index ^ 3.
                auto rb = [&](uint32_t addr) -> uint8_t {
                    if (addr >= 0x800000) return 0;
                    return app->core.RDRAM[addr ^ 3];
                };
                auto rw = [&](uint32_t addr) -> uint32_t {
                    return (uint32_t(rb(addr)) << 24) | (uint32_t(rb(addr+1)) << 16) |
                           (uint32_t(rb(addr+2)) <<  8) |  uint32_t(rb(addr+3));
                };
                uint8_t curLevel = rb(0x130B70);
                uint8_t curCraft = rb(0x130B41);
                uint32_t cineStatePtr = rw(0x0B0934);
                uint8_t cineStage = rb(0x0B0938);
                uint32_t curCutsceneFile = rw(0x0B1904);
                uint8_t unk20 = rb(0x130B60);  // gGameSettings+0x20 (menu sub-state)
                uint8_t unk21 = rb(0x130B61);  // gGameSettings+0x21 (cinematic sub-state)
                uint8_t unk22 = rb(0x130B62);  // gGameSettings+0x22 (mission sub-state)

                static uint8_t s_last_level = 0xFF, s_last_craft = 0xFF;
                static uint32_t s_last_cineState = 0xFFFFFFFF;
                static uint8_t s_last_cineStage = 0xFF;
                static uint32_t s_last_cutscene = 0xFFFFFFFF;
                static uint8_t s_last_u20 = 0xFF, s_last_u21 = 0xFF, s_last_u22 = 0xFF;
                bool changed = (curLevel != s_last_level) || (curCraft != s_last_craft) ||
                               (cineStatePtr != s_last_cineState) || (cineStage != s_last_cineStage) ||
                               (curCutsceneFile != s_last_cutscene) ||
                               (unk20 != s_last_u20) || (unk21 != s_last_u21) || (unk22 != s_last_u22);
                if (changed || (s_n & 511) == 0) {
                    fprintf(stderr,
                        "[gamestate vi=#%d] level=%u craft=%u u20=%u u21=%u u22=%u cineState=0x%08X cineStage=%u cutscene=0x%08X%s\n",
                        s_n, curLevel, curCraft, unk20, unk21, unk22,
                        cineStatePtr, cineStage, curCutsceneFile,
                        changed ? " [CHANGED]" : "");
                    // Dump first 32 bytes of cutscene struct (filename field). Helps confirm WHICH cutscene.
                    if (curCutsceneFile >= 0x80000000 && curCutsceneFile < 0x80800000) {
                        uint32_t off = curCutsceneFile - 0x80000000;
                        fprintf(stderr, "  cutscene-filename: \"");
                        for (int i = 0; i < 32 && (off + i) < 0x800000; i++) {
                            uint8_t b = app->core.RDRAM[(off + i) ^ 3];
                            if (b >= 0x20 && b < 0x7F) fputc(b, stderr);
                            else if (b == 0) break;
                            else fputc('?', stderr);
                        }
                        fprintf(stderr, "\"\n");
                    }
                    fflush(stderr);
                    s_last_level = curLevel;
                    s_last_craft = curCraft;
                    s_last_cineState = cineStatePtr;
                    s_last_cineStage = cineStage;
                    s_last_cutscene = curCutsceneFile;
                    s_last_u20 = unk20;
                    s_last_u21 = unk21;
                    s_last_u22 = unk22;
                }
            }
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
                // Also log presentQueue + workloadQueue cursors so we can
                // see whether PresentEarly is pushing presents and the
                // workload worker is consuming them. If presents stay flat
                // while workloads advance, the matcher is failing.
                int pq_wc = app->presentQueue ? (int)app->presentQueue->writeCursor : -1;
                int wq_wc = app->workloadQueue ? (int)app->workloadQueue->writeCursor : -1;
                fprintf(stderr,
                    "[vi] update_screen #%d origin=0x%08X width=%u status=0x%X v_current=%u nonzero=%d fs=%u pq.wc=%d wq.wc=%d\n",
                    s_n, vi->VI_ORIGIN_REG, vi->VI_WIDTH_REG,
                    vi->VI_STATUS_REG, vi->VI_V_CURRENT_LINE_REG, any_nonzero != 0,
                    rs64_dpc_get_cumulative_fullsyncs(),
                    pq_wc, wq_wc);
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
