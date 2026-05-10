#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#include "common/rt64_common.h"

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

// Wrapper: captures rdram into the watchdog global on first call so the
// hwbp/poll diagnostic threads can attach. Forwards to the real entrypoint.
extern "C" volatile uint8_t* volatile g_recomp_rdram_for_wp_raw;
extern "C" void rs64_entrypoint_with_rdram_capture(uint8_t* rdram, recomp_context* ctx) {
    g_recomp_rdram_for_wp_raw = rdram;
    recomp_entrypoint(rdram, ctx);
}
// The game's N64 "main" function — renamed to avoid clash with C main()
extern "C" void rs_main(uint8_t* rdram, recomp_context* ctx);
gpr get_entrypoint_address();

// ---------------------------------------------------------------------------
// RSP microcode dispatch
// ---------------------------------------------------------------------------
extern RspExitReason factor5_ucode(uint8_t* rdram, uint32_t ucode_addr);
extern RspExitReason factor5_boot (uint8_t* rdram, uint32_t ucode_addr);
extern uint8_t dmem[];
extern "C" void rs64_dpc_drain_histogram(uint32_t out[64]);

// Cached for the next ucode invocation. get_rsp_microcode is called with the
// OSTask immediately before the ucode runs on the same thread.
static thread_local uint32_t s_pending_task_data_ptr  = 0;
static thread_local uint32_t s_pending_task_ucode_data = 0;
static thread_local uint32_t s_pending_task_ucode_data_size = 0;

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

// Factor 5 GFX ucode runner (LLE side of the hybrid pipeline).
//
// Runs the boot ucode to set up DMEM + DMA the data section, emulates
// L_112C's first DL fetch by hand (the original ucode normally calls L_112C
// from inside the dispatch loop, but on first invocation that hasn't happened
// yet so DMEM has no real DL bytes), then runs the main ucode. The main ucode
// processes DL commands and, for vertex-pipeline ops, emits raw RDP triangle
// bytes via mtc0 DPC_END writes that flow through src/rsp/dpc_bridge.cpp into
// RT64 (isHLE=false).
static RspExitReason factor5_gfx_runner(uint8_t* rdram, uint32_t ucode_addr) {
    uint32_t dl_ptr           = s_pending_task_data_ptr;
    uint32_t ucode_data_addr  = s_pending_task_ucode_data;
    uint32_t ucode_data_size  = s_pending_task_ucode_data_size;

    // Mimic SP_BOOT (silicon-level RSP boot ucode): zero DMEM[0..0xFC0] then
    // DMA OSTask.ucode_data into DMEM[0..ucode_data_size]. Leave DMEM[0xFC0
    // ..0x1000] alone — that's the OSTask region the boot ucode reads.
    // Zeroing it would wipe OSTask.ucode at DMEM[0xFD0] and the next boot
    // call would DMA from RDRAM[0] (garbage).
    std::memset(dmem, 0, 0xFC0);
    if (ucode_data_addr != 0 && ucode_data_size != 0 && ucode_data_size <= 0xFC0) {
        dma_rdram_to_dmem(rdram, /*dmem*/0, /*dram*/ucode_data_addr & 0x00FFFFFF,
                          /*rd_len*/ucode_data_size - 1);
    }
    // Diagnostic: log re-DMA params + DMEM[0..0x10] after the copy.
    {
        static int s_log = -1;
        if (s_log < 0) {
            const char* e = std::getenv("ROGUESQ_LOG_SPBOOT");
            s_log = (e && *e && *e != '0') ? 1 : 0;
        }
        if (s_log) {
            static int s_n = 0;
            ++s_n;
            if (s_n <= 4) {
                fprintf(stderr,
                    "[spboot] task #%d ucode_data=0x%08X size=0x%X DMEM[0..0x10]:",
                    s_n, ucode_data_addr, ucode_data_size);
                for (int i = 0; i < 0x10; ++i) {
                    fprintf(stderr, " %02X", dmem[i ^ 3]);
                }
                fprintf(stderr, "\n");
                fflush(stderr);
            }
        }
    }

    // Boot exits via UnhandledJumpTarget on its `jr $7=0x1080` (jumping into
    // the main ucode it just DMA'd to IMEM 0x80) — that's expected.
    RspExitReason boot_r = factor5_boot(rdram, ucode_addr);
    if (boot_r != RspExitReason::UnhandledJumpTarget && boot_r != RspExitReason::Broke) {
        fprintf(stderr, "[RSP] factor5_boot returned unexpected %d, abandoning task\n", (int)boot_r);
        return RspExitReason::Broke;
    }

    auto poke_be32 = [](uint32_t off, uint32_t val) {
        for (int i = 0; i < 4; ++i) {
            dmem[(off + i) ^ 3] = (uint8_t)(val >> (24 - 8*i));
        }
    };
    if (dl_ptr) {
        // Stage the first 0x110 bytes of DL into DMEM at 0x170 (where the
        // main ucode's L_112C helper would normally DMA). Set DMEM[0x654] to
        // 0x178 so the dispatcher's first `lw $17, 0x654` lands past the
        // 8-byte header at the start of real commands.
        dma_rdram_to_dmem(rdram, /*dmem*/0x170, /*dram*/dl_ptr & 0x00FFFFFF, /*rd_len*/0x10F);
        // DMEM[$18+0x30] = the "current chunk RDRAM addr". With $18=0x100
        // (the value our fixup injects, matching L_1DB0's bootstrap), this
        // is DMEM[0x130]. L_11B0's chunk-fetch reads this slot for the next
        // re-DMA. L_1DB0 normally bootstraps it from DMEM[$1+0x30] = 0xFF0;
        // we mirror that bootstrap here in case L_1DB0 itself doesn't fire
        // every task. (Without this, tasks 2+ exit before emitting any RDP
        // because the static-data segment leaves 0x130 as bogus.)
        poke_be32(0x130, dl_ptr);
        poke_be32(0xFF0, dl_ptr);
        poke_be32(0x101C, dl_ptr);
        poke_be32(0x654,  0x178);
        // Reset the DL-stack-pointer byte. The ucode runs with $18 = 0x100
        // (set by L_1DB0's bootstrap), so $18+0x52 = DMEM[0x152]. Op_0F's
        // L_12C4 handler decrements this by 8 each call and exits the
        // dispatch loop when it goes negative. After task #1 underflows the
        // byte is left negative; subsequent tasks would exit immediately with
        // 0 RDP work emitted. Reset to 0x18 (3 stack entries) at task start.
        dmem[0x152 ^ 3] = 0x18;
        dmem[0x153 ^ 3] = 0;     // L_1DB0 also clears 0x53($18)

        // Diagnostic: dump first 16 DMEM bytes at the dispatch start so we
        // can see what opcode the first iter is dispatching on per task.
        static int s_dump = -1;
        if (s_dump < 0) {
            const char* e = std::getenv("ROGUESQ_LOG_DMEM_DL");
            s_dump = (e && *e && *e != '0') ? 1 : 0;
        }
        if (s_dump) {
            static int s_n = 0;
            ++s_n;
            if (s_n <= 8) {
                fprintf(stderr, "[dl] task #%d dl_ptr=0x%08X DMEM[0x178..0x190]:",
                        s_n, dl_ptr);
                for (int i = 0x178; i < 0x190; ++i) {
                    fprintf(stderr, " %02X", dmem[i ^ 3]);
                }
                fprintf(stderr, "\n");
                fflush(stderr);
            }
        }
    } else {
        poke_be32(0x654, 0x270);
    }
    RspExitReason r = factor5_ucode(rdram, ucode_addr);
    {
        static int s_log = -1;
        if (s_log < 0) {
            const char* e = std::getenv("ROGUESQ_LOG_RUNNER");
            s_log = (e && *e && *e != '0') ? 1 : 0;
        }
        if (s_log) {
            static int s_n = 0;
            ++s_n;
            if (s_n <= 8 || (s_n & 31) == 0) {
                // Pull the dpc_bridge per-task RDP-opcode histogram (raw 6-bit
                // RDP opcodes, NOT the F3D DL byte). 0x08-0x0F = triangle
                // variants; 0x24/0x25 = TEXRECT/TEXRECT_FLIP; 0x29 = SYNC_FULL;
                // 0x36 = FILL_RECTANGLE; 0x3E/0x3F = SET_DEPTH/COLOR_IMAGE.
                uint32_t hist[64];
                rs64_dpc_drain_histogram(hist);
                uint32_t tris = 0;
                for (int i = 0x08; i <= 0x0F; ++i) tris += hist[i];
                fprintf(stderr,
                        "[runner] task #%d exit=%d dl_ptr=0x%08X tris=%u texrects=%u fillrects=%u syncs=%u cimg=%u depth=%u\n",
                        s_n, (int)r, dl_ptr,
                        tris, hist[0x24] + hist[0x25], hist[0x36],
                        hist[0x29], hist[0x3F], hist[0x3E]);
                fflush(stderr);
            }
        }
    }
    return r;
}

RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    switch (task->t.type) {
    case M_GFXTASK:
        // Cache OSTask fields for factor5_gfx_runner. The runner needs to:
        //   - DMA the ucode_data segment to DMEM (mimicking SP_BOOT) so each
        //     task starts with fresh per-task data, not state carried over
        //     from the previous task's exit.
        //   - DMA the first chunk of the DL into DMEM[0x170] so the first
        //     dispatch loop iter has real bytes.
        s_pending_task_data_ptr        = (uint32_t)task->t.data_ptr;
        s_pending_task_ucode_data      = (uint32_t)task->t.ucode_data;
        s_pending_task_ucode_data_size = (uint32_t)task->t.ucode_data_size;
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

// (Periodic mqdiag watchdog removed — mqdiag_dump lived in the librecomp
//  fork and isn't in upstream. Hangs are rare enough now that on-demand
//  dump-game.ps1 is sufficient.)

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
    static volatile uint32_t s_last_val = 0;
    DWORD tid = GetCurrentThreadId();
    uint32_t cur_val = 0;
    if (g_wp_addr) cur_val = *(uint32_t*)g_wp_addr;
    // Only log on value TRANSITIONS — the watched word may be written many times
    // with the same value (a DMA hot path); we want the moments it CHANGES.
    bool changed = (cur_val != s_last_val);
    if (changed) s_last_val = cur_val;
    LONG n = changed ? InterlockedIncrement(&g_wp_hit_count) : g_wp_hit_count;
    if (changed && n <= 200) {
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

// Helper: read a 4-byte BE word from RDRAM at the given RDRAM-relative offset.
// (RDRAM is host-LE-stored; XOR-3 byte order for BE).
static inline uint32_t rdram_be32(const uint8_t* rdram, uint32_t off) {
    return (uint32_t(rdram[(off + 0) ^ 3]) << 24) |
           (uint32_t(rdram[(off + 1) ^ 3]) << 16) |
           (uint32_t(rdram[(off + 2) ^ 3]) <<  8) |
            uint32_t(rdram[(off + 3) ^ 3]);
}

// Periodic poll of the scene-state struct at D_80130B10 + the per-frame
// callback array at D_8011A8A4. Both regions are documented in
// `project_thread_topology_2026_05_09.md`. Gated by ROGUESQ_LOG_STATE=1.
//
// Reports the first dump immediately, then logs changes only. The state byte
// at +0x14 of D_80130B10 indexes the level/scene jump-table at jtbl_8003A3E8.
// The 4 callback slots at D_8011A8A4 are the per-frame draw callbacks the
// main game thread iterates each frame.
static void start_state_poller() {
    if (!std::getenv("ROGUESQ_LOG_STATE")) return;
    static std::thread t{[]{
        uint8_t* rdram = nullptr;
        while (!(rdram = (uint8_t*)g_recomp_rdram_for_wp_raw)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // KSEG0 → RDRAM-relative offset (mask 0x00FFFFFF).
        const uint32_t state_off  = 0x130B10;  // D_80130B10..B40 (scene state)
        const uint32_t cbarr_off  = 0x11A8A4;  // D_8011A8A4..B4  (4 callbacks)
        // 12 words covers B10..B3F. asm/main/3EBA0.s reads bytes at +0x20,
        // +0x21, +0x22 (D_80130B30..B32) to gate state transitions, so the
        // first poll missed exactly the bytes needed for forward-progress
        // analysis. Now we cover the whole B10..B3F region.
        uint32_t prev[16] = {0xFFFFFFFFu};
        bool first = true;
        for (int i = 0; i < 1200; ++i) {  // ~60s @ 50ms
            uint32_t state[12];
            for (int j = 0; j < 12; ++j) state[j] = rdram_be32(rdram, state_off + j * 4);
            uint32_t cbs[4];    // 4 callback fn ptrs
            for (int j = 0; j < 4; ++j) cbs[j]   = rdram_be32(rdram, cbarr_off + j * 4);

            // Drain-loop diagnostic — boot thread is stuck waiting for these.
            // D_800A0F50 byte: drain-enable gate (0 = bypass drain, return ready)
            // D_80141AD0 byte: drain-entry count
            // D_801496F8 word: array base ptr (each entry 0x88 bytes; byte[0] = active flag)
            uint8_t  a0f50 = rdram[0xA0F50 ^ 3];
            uint8_t  c1ad0 = rdram[0x141AD0 ^ 3];
            uint32_t array_base = rdram_be32(rdram, 0x1496F8);
            // Sound-asset data dumps — boot deadlocks in func_80097518 walking
            // pool_SND, possibly because the asset loader leaves buffers zeroed.
            // Dump all 3 sound assets once each when they become non-null.
            //   D_80139B4C = sound/proj_SND  (first asset)
            //   D_80139B54 = sound/pool_SND  (second asset, fed to func_80097518)
            //   D_80139B50 = sound/sdir_SND  (third asset)
            static bool snd_dumped[3] = {false, false, false};
            const uint32_t snd_addrs[3] = {0x139B4C, 0x139B54, 0x139B50};
            const char* snd_names[3] = {"proj_SND", "pool_SND", "sdir_SND"};
            for (int s = 0; s < 3; ++s) {
                if (snd_dumped[s]) continue;
                uint32_t ptr = rdram_be32(rdram, snd_addrs[s]);
                uint32_t off = ptr & 0x00FFFFFFu;
                if ((ptr & 0xF0000000u) == 0x80000000u && off + 0x40 < 0x800000u) {
                    fprintf(stderr, "[%s] t=%4d ms ptr=0x%08X first 0x40 bytes (BE):\n",
                            snd_names[s], i * 50, ptr);
                    for (int row = 0; row < 4; ++row) {
                        fprintf(stderr, "  %04X:", row * 16);
                        for (int b = 0; b < 16; ++b) {
                            fprintf(stderr, " %02X", rdram[(off + row * 16 + b) ^ 3]);
                        }
                        fprintf(stderr, "\n");
                    }
                    fflush(stderr);
                    snd_dumped[s] = true;
                    // EXPERIMENTAL: if the buffer is all zeros, write -1 sentinel
                    // into the first word. The user's hypothesis is that the
                    // attribution screen has no audio, so empty pool is correct
                    // and the game's pool walker should exit early on -1 sentinel.
                    // Our zero-init heap puts 0 instead. Test the hypothesis:
                    // ROGUESQ_FORCE_EMPTY_POOL_SENTINEL=1.
                    if (std::getenv("ROGUESQ_FORCE_EMPTY_POOL_SENTINEL")) {
                        bool all_zero = true;
                        for (int j = 0; j < 16; ++j) {
                            if (rdram[(off + j) ^ 3] != 0) { all_zero = false; break; }
                        }
                        if (all_zero) {
                            // Write 0xFFFFFFFF (BE) to first word.
                            for (int j = 0; j < 4; ++j) {
                                rdram[(off + j) ^ 3] = 0xFF;
                            }
                            fprintf(stderr,
                                "[%s-FIX] wrote sentinel -1 to first word at 0x%08X\n",
                                snd_names[s], ptr);
                            fflush(stderr);
                        }
                    }
                }
            }
            // Compute first-N active flags if array_base is a sane KSEG0 ptr
            uint8_t flags[8] = {0};
            uint32_t array_off = array_base & 0x00FFFFFFu;
            int n = c1ad0 < 8 ? c1ad0 : 8;
            if ((array_base & 0xF0000000u) == 0x80000000u && array_off + n * 0x88 < 0x800000u) {
                for (int j = 0; j < n; ++j) {
                    flags[j] = rdram[(array_off + j * 0x88) ^ 3];
                }
            }

            uint32_t cur[16];
            for (int j = 0; j < 12; ++j) cur[j] = state[j];
            for (int j = 0; j < 4; ++j) cur[12 + j] = cbs[j];
            bool changed = first;
            for (int j = 0; j < 16; ++j) if (cur[j] != prev[j]) { changed = true; break; }
            // Track drain state too — re-log when it changes.
            static uint8_t prev_a0f50 = 0xFF, prev_c1ad0 = 0xFF;
            static uint32_t prev_array_base = 0xFFFFFFFFu;
            static uint8_t prev_flags[8] = {0xFF};
            if (a0f50 != prev_a0f50 || c1ad0 != prev_c1ad0 || array_base != prev_array_base) changed = true;
            for (int j = 0; j < 8; ++j) if (flags[j] != prev_flags[j]) { changed = true; break; }
            if (changed) {
                first = false;
                fprintf(stderr,
                    "[state] t=%4d ms B10=%08X B14=%08X B18=%08X B1C=%08X B20=%08X B24=%08X B28=%08X B2C=%08X B30=%08X B34=%08X B38=%08X B3C=%08X | scene=%u | cb=[%08X,%08X,%08X,%08X] | drain.A0F50=%02X drain.cnt=%u arr=0x%08X flags=[%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X]\n",
                    i * 50,
                    state[0], state[1], state[2], state[3], state[4], state[5],
                    state[6], state[7], state[8], state[9], state[10], state[11],
                    (state[1] >> 24) & 0xFFu,
                    cbs[0], cbs[1], cbs[2], cbs[3],
                    a0f50, c1ad0, array_base,
                    flags[0], flags[1], flags[2], flags[3],
                    flags[4], flags[5], flags[6], flags[7]);
                fflush(stderr);
                for (int j = 0; j < 16; ++j) prev[j] = cur[j];
                prev_a0f50 = a0f50;
                prev_c1ad0 = c1ad0;
                prev_array_base = array_base;
                for (int j = 0; j < 8; ++j) prev_flags[j] = flags[j];
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }};
    t.detach();
}

// Periodic poll of MEM[0x80130B50] (the inner-loop transition gate). Reports
// changes so we can see WHO writes it and what bits flip. Gated by
// ROGUESQ_LOG_B50=1.
static void start_b50_poller() {
    if (!std::getenv("ROGUESQ_LOG_B50")) return;
    static std::thread t{[]{
        uint8_t* rdram = nullptr;
        while (!(rdram = (uint8_t*)g_recomp_rdram_for_wp_raw)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // RDRAM-relative offsets for KSEG0 0x80130B50 / 0x80130B58.
        const uint32_t b50_off = 0x130B50;
        const uint32_t b58_off = 0x130B58;
        uint32_t prev_b50 = 0xFFFFFFFFu;
        uint32_t prev_b58 = 0xFFFFFFFFu;
        for (int i = 0; i < 1200; ++i) {  // ~60s @ 50ms
            // Reads via byte XOR-3 to get BE word (RDRAM is host-LE-stored).
            uint32_t b50 = (uint32_t(rdram[(b50_off + 0) ^ 3]) << 24) |
                           (uint32_t(rdram[(b50_off + 1) ^ 3]) << 16) |
                           (uint32_t(rdram[(b50_off + 2) ^ 3]) <<  8) |
                            uint32_t(rdram[(b50_off + 3) ^ 3]);
            uint32_t b58 = (uint32_t(rdram[(b58_off + 0) ^ 3]) << 24) |
                           (uint32_t(rdram[(b58_off + 1) ^ 3]) << 16) |
                           (uint32_t(rdram[(b58_off + 2) ^ 3]) <<  8) |
                            uint32_t(rdram[(b58_off + 3) ^ 3]);
            if (b50 != prev_b50 || b58 != prev_b58) {
                fprintf(stderr, "[b50] t=%4d ms B50=0x%08X B58=0x%08X (b50.bit5=%d b50.b3=0x%02X b58.bit25=%d)\n",
                        i * 50, b50, b58,
                        (b50 >> 5) & 1, b50 & 0xFF,
                        (b58 >> 25) & 1);
                fflush(stderr);
                prev_b50 = b50;
                prev_b58 = b58;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }};
    t.detach();
}

static void start_memwp_watchdog() {
    static std::thread wp_thread{[]{
        uint8_t* rdram = nullptr;
        while (!(rdram = (uint8_t*)g_recomp_rdram_for_wp_raw)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // Default watch is rdram+0x3CBC4 (N64-logo low-mem fb writeback hunt).
        // Override via ROGUESQ_HWBP_ADDR=<hex offset> for new investigations.
        uint32_t watch_off = 0x3CBC4u;
        if (const char* e = std::getenv("ROGUESQ_HWBP_ADDR")) {
            unsigned long v = strtoul(e, nullptr, 0);
            if (v > 0 && v < 0x800000u) watch_off = (uint32_t)v;
        }
        uintptr_t addr = (uintptr_t)(rdram + watch_off);
        g_wp_addr = addr;
        AddVectoredExceptionHandler(1, memwp_veh);
        fprintf(stderr, "[hwbp] watching VA 0x%llX (rdram+0x%X), initial=0x%08X\n",
            (unsigned long long)addr, watch_off, *(uint32_t*)(rdram + watch_off));
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
            fprintf(stderr, "[main] SDL_QUIT received, exiting\n");
            fflush(stderr);
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
        // Diagnostic: log F1-F4 to confirm the keys are reaching the SDL
        // queue at all. If we see these logs, SDL is delivering keypresses
        // to our process — RT64's filter (which runs before SDL_PollEvent)
        // may have already consumed them, or it may not be installed.
        if (e.type == SDL_KEYDOWN) {
            const auto& sym = e.key.keysym.sym;
            if (sym == SDLK_F1 || sym == SDLK_F2 || sym == SDLK_F3 || sym == SDLK_F4) {
                fprintf(stderr, "[input] SDL_KEYDOWN sym=%d (F%d) reached PollEvent\n",
                        (int)sym, (int)(sym - SDLK_F1 + 1));
                fflush(stderr);
            }
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

void update_gfx(ultramodern::gfx_callbacks_t::gfx_data_t) {
    // Pump SDL events on the main thread (the one that owns the SDL window).
    // Win32 routes window messages to the window-owning thread's queue, so
    // SDL_PumpEvents on any other thread won't dispatch them. Without this,
    // the window appears "Not Responding" and F1/F2/F3/F4 keypresses never
    // reach RT64's developer-mode filter — even though poll_input() on the
    // game thread also calls SDL_PollEvent, that thread doesn't own the
    // window so messages stay queued.
    //
    // RT64's SDL_SetEventFilter installed via Application::sdlEventFilter
    // intercepts F1-F4 here (filters run before SDL_PollEvent dequeues), so
    // the controller-input poll on the game thread never sees those keys.
    SDL_PumpEvents();
}

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
        .entrypoint           = rs64_entrypoint_with_rdram_capture,
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
    CreateDirectoryA("dumps", NULL);
    CreateDirectoryA("dumps/crash-dumps", NULL);
    snprintf(path, sizeof(path),
        "dumps/crash-dumps/crash_%04u%02u%02u_%02u%02u%02u.dmp",
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
    // Default to a lighter dump type. The earlier MiniDumpWithFullMemory
    // captured the full process address space (≈5 GB on this app, dominated
    // by D3D12/Vulkan render targets and texture caches). The symbolicated
    // stack trace in the .log already covers the common debug case; the
    // dump only needs threads + stacks + indirectly-referenced memory.
    // Set ROGUESQ_FULL_DUMP=1 to restore the full 5 GB dump for deep-dives.
    static const bool s_full_dump = []{
        const char *e = std::getenv("ROGUESQ_FULL_DUMP");
        return e && *e && *e != '0';
    }();
    MINIDUMP_TYPE dumpType = s_full_dump
        ? MiniDumpWithFullMemory
        : (MINIDUMP_TYPE)(MiniDumpNormal
                          | MiniDumpWithIndirectlyReferencedMemory
                          | MiniDumpWithDataSegs
                          | MiniDumpWithThreadInfo
                          | MiniDumpWithUnloadedModules);
    BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
        hFile, dumpType, pmei, NULL, NULL);
    CloseHandle(hFile);
    if (ok) {
        fprintf(stderr, "[CRASH] Minidump written: %s%s\n",
            path, s_full_dump ? " (full memory)" : " (lite)");
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
    // Capture stack BEFORE minidump — if the process is killed mid-minidump
    // (e.g. by an external watchdog), we still want the trace in stderr.
    void* frames[32];
    USHORT count = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
    fprintf(stderr, "[CRASH] Stack trace (%u frames):\n", (unsigned)count);
    print_stack_with_symbols(frames, count);
    fflush(stderr);
    write_minidump_safe(ep);
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    // RT64's RT64_LOG_PRINTF macro (debug builds) writes to GlobalLogFile via
    // unchecked fprintf + fflush. Pristine RT64 expects RT64::Application::start
    // to open it (we bypass Application), so without redirection the pointer
    // stays NULL and any RT64 debug-log call asserts in the CRT.
    //
    // Routing to stderr works but RT64 calls these macros at high rate (every
    // fullSync), and the per-call fflush starves the SDL message pump → game
    // window goes "Not Responding". Send writes to NUL instead — same effect
    // as the fork's `if(false) fprintf` cruft, but stays in our repo.
    if (FILE *nul = fopen("NUL", "w")) {
        RT64::GlobalLogFile = nul;
    } else {
        RT64::GlobalLogFile = stderr;  // last-resort fallback
    }
    // Hardware-breakpoint watchdog on rdram+0x3CBC4 (where a corruption was
    // first observed). Useful for tracing the writer when investigating the
    // bug; emits a stack-traced [hwbp-hit] line for every write to the
    // watched address. Default off — set ROGUESQ_HWBP=1 to enable, or just
    // ROGUESQ_HWBP_ADDR=<offset> to enable + watch a different address.
    {
        const char *e = std::getenv("ROGUESQ_HWBP");
        const char *e_addr = std::getenv("ROGUESQ_HWBP_ADDR");
        bool flag_enabled = (e && *e && *e != '0');
        // ADDR enables the watchdog if it parses to a non-zero number — accepts
        // "0x470", "1136", etc. Don't test *e_addr against '0' (broken for "0x...").
        bool addr_enabled = false;
        if (e_addr && *e_addr) {
            unsigned long v = std::strtoul(e_addr, nullptr, 0);
            if (v != 0) addr_enabled = true;
        }
        if (flag_enabled || addr_enabled) {
            fprintf(stderr, "[hwbp] watchdog enabled (ROGUESQ_HWBP=%s ROGUESQ_HWBP_ADDR=%s)\n",
                    e ? e : "(unset)", e_addr ? e_addr : "(unset, defaults to 0x3CBC4)");
            fflush(stderr);
            start_memwp_watchdog();
        }
    }

    start_b50_poller();
    start_state_poller();

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
