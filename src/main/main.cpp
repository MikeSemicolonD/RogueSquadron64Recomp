#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <vector>
#include <cinttypes>
#include <filesystem>
#include <thread>
#include <chrono>
#include <atomic>

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/events.hpp"
#include "librecomp/game.hpp"
#include "librecomp/rsp.hpp"

#define SDL_MAIN_HANDLED
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>
#include <TlHelp32.h>
#pragma comment(lib, "Dbghelp.lib")
#include <crtdbg.h>
#include "SDL.h"
#include "SDL_syswm.h"
#else
#include "SDL2/SDL.h"
#include "SDL2/SDL_syswm.h"
#endif

// N64 button bitmasks (from libultra PR/controller.h)
#define N64_A_BUTTON     0x8000
#define N64_B_BUTTON     0x4000
#define N64_Z_TRIG       0x2000
#define N64_START_BUTTON 0x1000
#define N64_U_JPAD       0x0800
#define N64_D_JPAD       0x0400
#define N64_L_JPAD       0x0200
#define N64_R_JPAD       0x0100
#define N64_L_TRIG       0x0020
#define N64_R_TRIG       0x0010
#define N64_U_CBUTTONS   0x0008
#define N64_D_CBUTTONS   0x0004
#define N64_L_CBUTTONS   0x0002
#define N64_R_CBUTTONS   0x0001

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
void rs64_register_overlays();
extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
// The game's N64 "main" function — renamed to avoid clash with C main()
extern "C" void rs_main(uint8_t* rdram, recomp_context* ctx);
gpr get_entrypoint_address();

// ---------------------------------------------------------------------------
// RSP microcode dispatch
// ---------------------------------------------------------------------------
extern RspUcodeFunc aspMain;
extern RspExitReason factor5_ucode(uint8_t* rdram, uint32_t ucode_addr);
extern RspExitReason factor5_boot (uint8_t* rdram, uint32_t ucode_addr);
extern uint8_t dmem[];

// Cached for the next ucode invocation. get_rsp_microcode is called with the
// OSTask immediately before the ucode runs on the same thread.
static thread_local uint32_t s_pending_task_data_ptr = 0;

// Rogue Squadron uses Factor5's MusyX audio ucode, NOT stock aspMain. Running
// aspMain on MusyX-formatted task data produces garbage or hangs the audio
// thread (no shared format). Until MusyX has a real recomp pass (see
// project_audio_musyx.md), stub all audio tasks: return Broke immediately so
// the game thinks the task completed, sp_complete() fires, and play continues.
// Cost: no audio. Trade-off: keeps the rest of the game responsive.
static RspExitReason musyx_stub(uint8_t* /*rdram*/, uint32_t /*ucode_addr*/) {
    return RspExitReason::Broke;
}

// Catch-all stub for unrecognised task types. Returning Broke (instead of
// QUICK_EXIT'ing) avoids tearing down the process when a stale/uninitialised
// task struct gets dispatched — observed early in boot with type fields like
// 0x21E50ADF that don't match M_GFXTASK/M_AUDTASK.
static RspExitReason unknown_task_stub(uint8_t* /*rdram*/, uint32_t /*ucode_addr*/) {
    return RspExitReason::Broke;
}

// Factor5 GFX ucode — recompiled to C by RSPRecomp. Replaces RT64's HLE GBI
// interpreter for graphics tasks. RDP commands the ucode emits via DPC_START/
// DPC_END writes go through src/rsp/dpc_bridge.cpp into RT64.
//
// The original Factor5 boot ucode (a separate ~0xD0-byte blob at RDRAM
// 0x800825D0) DMAs the data section to DMEM and stages task->t.data_ptr in
// DMEM[0x654] for the main ucode to pick up via `lw $17, 0x654($8)`. We don't
// recompile the boot, so we write that location manually here.
static RspExitReason factor5_gfx_runner(uint8_t* rdram, uint32_t ucode_addr) {
    uint32_t dl_ptr = s_pending_task_data_ptr;

    // Run the boot ucode first to set up registers + DMA the data section.
    // Boot exits via UnhandledJumpTarget on its `jr $7=0x1080` (jumping into
    // the main ucode it just DMA'd to IMEM 0x80) — that's expected, since the
    // main ucode is our separately-recompiled C function we call next.
    RspExitReason boot_r = factor5_boot(rdram, ucode_addr);
    if (boot_r != RspExitReason::UnhandledJumpTarget && boot_r != RspExitReason::Broke) {
        fprintf(stderr, "[RSP] factor5_boot returned unexpected %d, abandoning task\n", (int)boot_r);
        return RspExitReason::Broke;
    }

    // Emulate L_112C's DL fetch by hand: in the original ucode, L_112C is
    // called from inside the L_1010 dispatch loop to DMA the next 0x110 bytes
    // of DL from OSTask.data_ptr → DMEM[0x170]. On first task it has not yet
    // been called, so DMEM has no real DL and the dispatcher would loop on
    // garbage. We do that DMA up-front here, then poke DMEM[0x654] = 0x178 so
    // the recompile's first `lw $17, 0x654` lands at the start of real
    // commands (DMA target was 0x170; first 8 bytes are header so r17=0x178).
    auto poke_be32 = [](uint32_t off, uint32_t val) {
        for (int i = 0; i < 4; ++i) {
            dmem[(off + i) ^ 3] = (uint8_t)(val >> (24 - 8*i));
        }
    };
    if (dl_ptr) {
        dma_rdram_to_dmem(rdram, /*dmem*/0x170, /*dram*/dl_ptr & 0x00FFFFFF, /*rd_len*/0x10F);
        // Save data_ptr at DMEM[0x101C] (= 0xFC0+0x5C, where r18=0xFC0 means
        // L_112C's `sw $r2, 0x5C($18)` writes after future re-DMAs).
        poke_be32(0x101C, dl_ptr);
        // Set DL pointer for the dispatcher: r17 = 0x178 (skip 8-byte header).
        poke_be32(0x654, 0x178);
    } else {
        poke_be32(0x654, 0x270);  // fallback if no data_ptr cached yet
    }
    return factor5_ucode(rdram, ucode_addr);
}

RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    switch (task->t.type) {
    case M_GFXTASK:
        // Cache data_ptr for factor5_gfx_runner — the recompile reads $17 from
        // DMEM[0x654] which the boot ucode normally pre-populates.
        s_pending_task_data_ptr = (uint32_t)task->t.data_ptr;
        return &factor5_gfx_runner;
    case M_AUDTASK:
        return &musyx_stub;
    default:
        // Don't crash — log once per distinct type and stub it out.
        static thread_local uint32_t last_unknown = 0;
        if (task->t.type != last_unknown) {
            last_unknown = task->t.type;
            fprintf(stderr, "[RSP] Stubbing unknown task type: %" PRIu32 " (0x%08X)\n",
                task->t.type, task->t.type);
            fflush(stderr);
        }
        return &unknown_task_stub;
    }
}

// ---------------------------------------------------------------------------
// Audio (SDL2)
// ---------------------------------------------------------------------------
static SDL_AudioDeviceID audio_device = 0;
static uint32_t audio_sample_rate = 48000;

static void set_frequency(uint32_t freq) {
    if (audio_device) {
        SDL_CloseAudioDevice(audio_device);
        audio_device = 0;
    }
    audio_sample_rate = freq;

    SDL_AudioSpec desired{};
    desired.freq     = (int)freq;
    desired.format   = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples  = 1024;

    audio_device = SDL_OpenAudioDevice(nullptr, 0, &desired, nullptr, 0);
    if (!audio_device) {
        fprintf(stderr, "[Audio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(audio_device, 0);
}

static void queue_samples(int16_t* samples, size_t num_bytes) {
    if (audio_device) {
        SDL_QueueAudio(audio_device, samples, (Uint32)num_bytes);
    }
}

static size_t get_frames_remaining() {
    if (!audio_device) return 0;
    Uint32 queued = SDL_GetQueuedAudioSize(audio_device);
    // queued is in bytes; 4 bytes per stereo frame (2 ch × 2 bytes)
    return queued / 4;
}

// Forward declaration so the F12 hotkey in poll_input() can write a dump.
static void write_minidump_safe(EXCEPTION_POINTERS* ep);
// Forward declaration so the hwbp VEH (defined before print_stack_with_symbols)
// can render the offending thread's stack with symbols. Non-static so RT64's
// d3d12 allocator-failure tracer can extern-declare and call it.
void print_stack_with_symbols(void** frames, USHORT count);

// Periodic mqdiag watchdog: dumps message-queue stats every 3s to
// mqdiag_watchdog.txt. Lets us see queue progress mid-hang even when
// the SDL message pump is starved (rules out trying to use F12).
extern "C" void mqdiag_dump(const char *path);
static void start_mqdiag_watchdog() {
    static std::thread watchdog{[]{
        for (int n = 0; ; ++n) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            char path[64];
            std::snprintf(path, sizeof(path), "logs/mqdiag_%03d.txt", n);
            mqdiag_dump(path);
        }
    }};
    watchdog.detach();
}

// Memory watchpoint: caught the corruption window via polling but missed the
// instigating store. Switch to a Win32 hardware data breakpoint (DR0, write,
// 4 bytes) on rdram + 0x3CBC4. When a write hits, the OS raises
// EXCEPTION_SINGLE_STEP on the offending thread; our vectored handler logs
// tid/RIP/stack so we can name the racing writer.
//
// DR0..DR3 are per-thread; we walk the process thread list and arm DR0 on each
// thread (skipping ourselves). New threads spawned later need re-arming, so we
// re-walk the list on a slow cadence — overhead is negligible vs. catching the
// race.
extern "C" volatile uint8_t* volatile g_recomp_rdram_for_wp_raw = nullptr;

static volatile uintptr_t g_wp_addr = 0;     // address being watched (in our VA)
static volatile LONG      g_wp_hit_count = 0; // limit logging — race may fire fast

static LONG WINAPI memwp_veh(EXCEPTION_POINTERS* ep) {
    // Hardware data breakpoints raise EXCEPTION_SINGLE_STEP with B0..B3 set in DR6.
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* ctx = ep->ContextRecord;
    DWORD64 dr6_hits = ctx->Dr6 & 0xFu;
    if (!dr6_hits) {
        return EXCEPTION_CONTINUE_SEARCH;  // BS or other source — not us
    }

    // Throttle: catastrophic loop would flood stderr and slow everything to a halt.
    LONG n = InterlockedIncrement(&g_wp_hit_count);
    if (n <= 16) {
        DWORD tid = GetCurrentThreadId();
        uint32_t cur_val = 0;
        uint8_t* rdram = (uint8_t*)g_recomp_rdram_for_wp_raw;
        if (rdram) cur_val = *(uint32_t*)(rdram + 0x3CBC4);
        fprintf(stderr,
            "[hwbp-hit #%ld] tid=%lu RIP=0x%llX  watch=0x%llX  "
            "current=0x%08X  DR6=0x%llX\n",
            n, tid,
            (unsigned long long)ctx->Rip,
            (unsigned long long)g_wp_addr,
            cur_val,
            (unsigned long long)ctx->Dr6);
        void* frames[24];
        USHORT count = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
        print_stack_with_symbols(frames, count);
        fflush(stderr);
    }

    // Clear DR6 status bits so further hits can be observed.
    ctx->Dr6 &= ~0xFull;
    // Set RF in EFlags to avoid refire on the same instruction (data BP is a
    // trap that fires AFTER the write, but RF is harmless here and matches
    // canonical post-handler behavior).
    ctx->EFlags |= 0x10000;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Configure DR0 on the given (suspended) thread to trap on 4-byte WRITE at addr.
// Returns true if we actually wrote new debug regs.
static bool arm_hwbp_on_thread(HANDLE hThread, uintptr_t addr) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(hThread, &ctx)) return false;
    if (ctx.Dr0 == (DWORD64)addr && (ctx.Dr7 & 0x000D0001ull) == 0x000D0001ull) {
        // Already armed — no need to rewrite (avoids per-tick churn cost).
        return false;
    }
    ctx.Dr0 = (DWORD64)addr;
    // DR7: clear DR0 fields, then set L0=1, R/W0=01 (write), LEN0=11 (4 bytes).
    //   L0=bit0, R/W0=bits16-17, LEN0=bits18-19. Combined mask 0xF0001 in low20.
    DWORD64 dr7 = ctx.Dr7;
    dr7 &= ~((0x3ull << 16) | (0x3ull << 18) | 0x1ull);
    dr7 |= (0x1ull << 16);  // R/W0 = 01 (write)
    dr7 |= (0x3ull << 18);  // LEN0 = 11 (4 bytes)
    dr7 |= 0x1ull;          // L0 = 1
    ctx.Dr7 = dr7;
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    return SetThreadContext(hThread, &ctx) != 0;
}

static void arm_hwbp_all_threads(uintptr_t addr) {
    DWORD me  = GetCurrentThreadId();
    DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            if (te.th32ThreadID == me) continue;
            HANDLE h = OpenThread(
                THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                FALSE, te.th32ThreadID);
            if (!h) continue;
            if (SuspendThread(h) != (DWORD)-1) {
                arm_hwbp_on_thread(h, addr);  // skips if already armed; silent
                ResumeThread(h);
            }
            CloseHandle(h);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    // Intentionally no per-tick print — recurring stderr writes starve the
    // SDL message pump and produce a "Not Responding" window. Hits are still
    // reported by memwp_veh; that's the only signal we care about.
}

static void start_memwp_watchdog() {
    static std::thread wp_thread{[]{
        uint8_t* rdram = nullptr;
        while (!(rdram = (uint8_t*)g_recomp_rdram_for_wp_raw)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        uintptr_t addr = (uintptr_t)(rdram + 0x3CBC4);
        g_wp_addr = addr;
        AddVectoredExceptionHandler(1, memwp_veh);
        fprintf(stderr, "[hwbp] watching VA 0x%llX (rdram+0x3CBC4), initial=0x%08X\n",
            (unsigned long long)addr, *(uint32_t*)(rdram + 0x3CBC4));
        fflush(stderr);
        // Re-arm cadence: most game threads are created in the first ~2 s of boot.
        // After that, fresh threads are rare (RT64 lazy workers, the occasional
        // file IO). 1 s is plenty and keeps SuspendThread/SetThreadContext churn
        // off the SDL pump's back. Per-thread arm itself is silent.
        for (;;) {
            arm_hwbp_all_threads(addr);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }};
    wp_thread.detach();
}

// ---------------------------------------------------------------------------
// Input (SDL2 gamepad — one controller)
// ---------------------------------------------------------------------------
static SDL_GameController* controller = nullptr;

static void poll_input() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            exit(EXIT_SUCCESS);
        }
        if (e.type == SDL_CONTROLLERDEVICEADDED) {
            if (!controller) {
                controller = SDL_GameControllerOpen(e.cdevice.which);
            }
        }
        if (e.type == SDL_CONTROLLERDEVICEREMOVED && controller) {
            if (SDL_GameControllerGetJoystick(controller) ==
                SDL_JoystickFromInstanceID(e.cdevice.which)) {
                SDL_GameControllerClose(controller);
                controller = nullptr;
            }
        }
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F12) {
            fprintf(stderr, "[F12] manual minidump requested\n");
            fflush(stderr);
            write_minidump_safe(nullptr);
        }
    }
}

static bool get_n64_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (controller_num != 0 || !controller) {
        *buttons = 0; *x = 0.0f; *y = 0.0f;
        return false;
    }

    uint16_t btn = 0;
    auto b = [&](uint16_t mask, SDL_GameControllerButton sdl) {
        if (SDL_GameControllerGetButton(controller, sdl)) btn |= mask;
    };

    // Rogue Squadron N64 → modern gamepad mapping:
    //   A (fire)        → face A
    //   B (bombs)       → face X
    //   Z (brake)       → left trigger (digital, via axis threshold below)
    //   R (boost)       → right shoulder
    //   L (targeting)   → left shoulder
    //   C-Up (view)     → right stick up (handled via axis) — face Y as fallback
    //   C-Down          → face B
    //   C-Left          → right stick left (axis) — d-left as fallback
    //   C-Right         → right stick right (axis) — d-right as fallback
    //   D-Pad           → d-pad
    //   Start           → start
    b(N64_A_BUTTON,     SDL_CONTROLLER_BUTTON_A);
    b(N64_B_BUTTON,     SDL_CONTROLLER_BUTTON_X);
    b(N64_START_BUTTON, SDL_CONTROLLER_BUTTON_START);
    b(N64_U_JPAD,       SDL_CONTROLLER_BUTTON_DPAD_UP);
    b(N64_D_JPAD,       SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    b(N64_L_JPAD,       SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    b(N64_R_JPAD,       SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    b(N64_L_TRIG,       SDL_CONTROLLER_BUTTON_LEFTSHOULDER);   // targeting computer
    b(N64_R_TRIG,       SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);  // boost/accelerate
    b(N64_U_CBUTTONS,   SDL_CONTROLLER_BUTTON_Y);
    b(N64_D_CBUTTONS,   SDL_CONTROLLER_BUTTON_B);
    b(N64_L_CBUTTONS,   SDL_CONTROLLER_BUTTON_BACK);
    b(N64_R_CBUTTONS,   SDL_CONTROLLER_BUTTON_GUIDE);
    // Z trigger (brake) from left analog trigger axis
    if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) btn |= N64_Z_TRIG;

    int16_t ax = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX);
    int16_t ay = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY);
    *x = ax / 32767.0f;
    *y = -(ay / 32767.0f); // N64 Y is inverted vs SDL
    *buttons = btn;
    return true;
}

static void set_rumble(int, bool) {}

static ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    if (controller_num == 0 && controller) {
        return { ultramodern::input::Device::Controller, ultramodern::input::Pak::None };
    }
    return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
}

// ---------------------------------------------------------------------------
// Graphics (SDL2 window creation — rt64 takes over from here)
// ---------------------------------------------------------------------------
ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        exit(EXIT_FAILURE);
    }
    return nullptr;
}

// Defined in rt64_render_context.cpp
namespace recomp {
    std::unique_ptr<ultramodern::renderer::RendererContext>
    create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);
}

ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    SDL_Window* sdl_window = SDL_CreateWindow(
        "Star Wars: Rogue Squadron 64 Recompiled",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        640, 480,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN
    );
    if (!sdl_window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        exit(EXIT_FAILURE);
    }
#if defined(_WIN32)
    SDL_SysWMinfo wm{};
    SDL_VERSION(&wm.version);
    SDL_GetWindowWMInfo(sdl_window, &wm);
    return { wm.info.win.window };
#else
    return sdl_window;
#endif
}

void update_gfx(ultramodern::gfx_callbacks_t::gfx_data_t) {}

// ---------------------------------------------------------------------------
// Thread naming
// ---------------------------------------------------------------------------
static std::string get_game_thread_name(const OSThread* t) {
    switch (t->id) {
    case 1:  return "[Game] IDLE";
    case 3:  return "[Game] MAIN";
    case 4:  return "[Game] EEPROM";  // entry func_8006F2CC: save writer (osEepromLongWrite), not GRAPH
    case 5:  return "[Game] SCHED";
    case 10: return "[Game] AUDIOMGR";
    case 18: return "[Game] DMAMGR";
    default: return "[Game] " + std::to_string(t->id);
    }
}

// ---------------------------------------------------------------------------
// Game registration
// ---------------------------------------------------------------------------
// ROM hash: xxHash3-64 of rogue_squadron.z64 (USA v1.0, 16MB)
static constexpr uint64_t RS64_ROM_HASH = 0x6B66A44153594DEAULL;

std::vector<recomp::GameEntry> supported_games = {
    {
        .rom_hash             = RS64_ROM_HASH,
        .internal_name        = "ROGUE SQUADRON",
        .game_id              = u8"rs64.n64.us.1.0",
        .mod_game_id          = "rs64",
        .save_type            = recomp::SaveType::Eep4k,
        .is_enabled           = true,
        .entrypoint_address   = get_entrypoint_address(),
        .entrypoint           = recomp_entrypoint,
    },
};

// ---------------------------------------------------------------------------
// main()
// ---------------------------------------------------------------------------
// One-shot init of DbgHelp symbol resolution — done lazily on first crash.
static void ensure_dbghelp_init() {
    static bool init = false;
    if (init) return;
    init = true;
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
}

// Print stack frames with symbol resolution. Each frame becomes:
//   [ N] 0xADDR  module!function+0xOFF  (file:line)
void print_stack_with_symbols(void** frames, USHORT count) {
    ensure_dbghelp_init();
    HANDLE proc = GetCurrentProcess();
    HMODULE exe_base = GetModuleHandleW(nullptr);
    constexpr DWORD kNameMax = 512;
    char buf[sizeof(SYMBOL_INFO) + kNameMax];
    SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    for (USHORT i = 0; i < count; i++) {
        DWORD64 addr = (DWORD64)(uintptr_t)frames[i];
        uintptr_t rva = (uintptr_t)frames[i] - (uintptr_t)exe_base;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = kNameMax - 1;
        DWORD64 disp = 0;
        const char* name = "?";
        if (SymFromAddr(proc, addr, &disp, sym)) {
            name = sym->Name;
        }
        IMAGEHLP_LINE64 line{}; line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;
        if (SymGetLineFromAddr64(proc, addr, &lineDisp, &line)) {
            fprintf(stderr, "  [%2u] 0x%llX rva 0x%llX  %s+0x%llX  (%s:%lu)\n",
                (unsigned)i, (unsigned long long)addr, (unsigned long long)rva,
                name, (unsigned long long)disp, line.FileName, (unsigned long)line.LineNumber);
        } else {
            fprintf(stderr, "  [%2u] 0x%llX rva 0x%llX  %s+0x%llX\n",
                (unsigned)i, (unsigned long long)addr, (unsigned long long)rva,
                name, (unsigned long long)disp);
        }
    }
}

// Full-memory minidump so we can inspect rdram contents post-mortem.
// ep may be null (SIGABRT path) — we still capture process+thread state.
static void write_minidump_safe(EXCEPTION_POINTERS* ep) {
    char path[MAX_PATH];
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(path, sizeof(path),
        "dumps/crash_%04u%02u%02u_%02u%02u%02u.dmp",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    HANDLE hFile = CreateFileA(path, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[CRASH] CreateFile(%s) failed err=%lu\n",
            path, GetLastError());
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    PMINIDUMP_EXCEPTION_INFORMATION pmei = nullptr;
    if (ep) {
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        pmei = &mei;
    }
    BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
        hFile, MiniDumpWithFullMemory, pmei, NULL, NULL);
    CloseHandle(hFile);
    if (ok) {
        fprintf(stderr, "[CRASH] Minidump written: %s\n", path);
    } else {
        fprintf(stderr, "[CRASH] MiniDumpWriteDump failed err=%lu\n", GetLastError());
    }
    fflush(stderr);
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    uintptr_t addr = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    fprintf(stderr, "[CRASH] Exception 0x%08lX at 0x%llX\n", code, (unsigned long long)addr);
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        fprintf(stderr, "[CRASH] Access violation %s address 0x%llX\n",
            ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
            (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    }
    write_minidump_safe(ep);
    void* frames[32];
    USHORT count = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
    fprintf(stderr, "[CRASH] Stack trace (%u frames):\n", (unsigned)count);
    print_stack_with_symbols(frames, count);
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    start_mqdiag_watchdog();
    start_memwp_watchdog();

#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_handler);
    signal(SIGABRT, [](int){
        fprintf(stderr, "[ABORT] caught SIGABRT, dumping stack:\n");
        void* frames[32];
        USHORT count = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
        print_stack_with_symbols(frames, count);
        // No EXCEPTION_POINTERS on the abort path — passing nullptr makes
        // VS open the dump without the "unhandled exception" dialog.
        write_minidump_safe(nullptr);
        _Exit(3);
    });
    // Route CRT debug asserts to stderr instead of the blocking dialog.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    // Hook fires BEFORE abort() runs — gives us a chance to dump the stack
    // for "vector subscript out of range" and similar STL bounds checks.
    _CrtSetReportHook([](int reportType, char* message, int*) -> int {
        fprintf(stderr, "[CRT_REPORT type=%d] %s\n", reportType,
            message ? message : "(null)");
        void* frames[48];
        USHORT count = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
        HMODULE base = GetModuleHandleW(nullptr);
        for (USHORT i = 0; i < count; i++) {
            uintptr_t rva = (uintptr_t)frames[i] - (uintptr_t)base;
            fprintf(stderr, "  [%2u] 0x%llX  rva 0x%llX\n", (unsigned)i,
                (unsigned long long)(uintptr_t)frames[i],
                (unsigned long long)rva);
        }
        fflush(stderr);
        // Return 1 to SUPPRESS the abort. STL bounds-check assertions ("vector
        // subscript out of range") fire when a Factor5 ucode handler indexes
        // past a vector limit due to state we can't fully replicate yet. The
        // resulting abort kills the game even though continuing with whatever
        // garbage the out-of-bounds read returned often lets play continue.
        // Trade-off: occasional visual glitches over a hard crash. Print first.
        return 1;
    });
    // MSVC debug iterators call _invalid_parameter on bounds-check failure
    // (e.g. "vector subscript out of range"). Default handler aborts silently;
    // ours prints a stack trace first.
    _set_invalid_parameter_handler([](const wchar_t* expr, const wchar_t* func,
                                       const wchar_t* file, unsigned int line,
                                       uintptr_t) {
        fprintf(stderr, "[INVALID_PARAM] expr=%ls func=%ls file=%ls:%u\n",
            expr ? expr : L"(null)", func ? func : L"(null)",
            file ? file : L"(null)", line);
        void* frames[48];
        USHORT count = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
        HMODULE base = GetModuleHandleW(nullptr);
        for (USHORT i = 0; i < count; i++) {
            uintptr_t rva = (uintptr_t)frames[i] - (uintptr_t)base;
            fprintf(stderr, "  [%2u] 0x%llX  rva 0x%llX\n", (unsigned)i,
                (unsigned long long)(uintptr_t)frames[i],
                (unsigned long long)rva);
        }
        fflush(stderr);
        _Exit(4);
    });
#endif

    rs64_register_overlays();

    // Use the working directory as the config/data path (portable mode).
    recomp::register_config_path(std::filesystem::current_path());

    for (const auto& game : supported_games) {
        recomp::register_game(game);
    }

    // Check if the ROM is already stored; if not, try to import it from common filenames.
    recomp::check_all_stored_roms();
    std::u8string rs_game_id = supported_games[0].game_id;
    if (!recomp::is_rom_valid(rs_game_id)) {
        static const char* rom_candidates[] = {
            "rogue_squadron.z64",
            "rogue squadron.z64",
            "RogueSquadron.z64",
            "rs64.z64",
        };
        for (const char* name : rom_candidates) {
            std::filesystem::path p = std::filesystem::current_path() / name;
            auto result = recomp::select_rom(p, rs_game_id);
            if (result == recomp::RomValidationError::Good) {
                fprintf(stderr, "[ROM] Imported %s\n", name);
                break;
            }
        }
    }
    // Re-check after any import attempt so is_rom_valid reflects the new file.
    recomp::check_all_stored_roms();
    if (!recomp::is_rom_valid(rs_game_id)) {
        fprintf(stderr,
            "[ROM] Place your Rogue Squadron (USA v1.0) ROM named\n"
            "      'rogue_squadron.z64' next to the executable and restart.\n");
    }

    recomp::start(recomp::Configuration{
        .project_version = { 0, 1, 0 },
        .window_handle = {},
        .rsp_callbacks = {
            .get_rsp_microcode = get_rsp_microcode,
        },
        .renderer_callbacks = {
            .create_render_context = recomp::create_render_context,
        },
        .audio_callbacks = {
            .queue_samples        = queue_samples,
            .get_frames_remaining = get_frames_remaining,
            .set_frequency        = set_frequency,
        },
        .input_callbacks = {
            .poll_input                = poll_input,
            .get_input                 = get_n64_input,
            .set_rumble                = set_rumble,
            .get_connected_device_info = get_connected_device_info,
        },
        .gfx_callbacks = {
            .create_gfx    = create_gfx,
            .create_window = create_window,
            .update_gfx    = update_gfx,
        },
        .events_callbacks = {
            .vi_callback = nullptr,
            .gfx_init_callback = []() {
                std::u8string game_id = u8"rs64.n64.us.1.0";
                if (recomp::is_rom_valid(game_id)) {
                    recomp::start_game(game_id);
                }
            },
        },
        .error_handling_callbacks = {
            .message_box = [](const char* msg) { fprintf(stderr, "[Error] %s\n", msg); },
        },
        .threads_callbacks = {
            .get_game_thread_name = get_game_thread_name,
        },
        .message_queue_control = {},
    });

    return EXIT_SUCCESS;
}
