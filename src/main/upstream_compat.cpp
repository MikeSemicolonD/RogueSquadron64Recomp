// Upstream-librecomp gap-filler: libultra _recomp bindings that the
// recompiled game calls but upstream librecomp doesn't provide. Stubbed
// to safe defaults — none of these primitives have meaningful semantics
// on a non-N64 host (cache management, controller pak access, etc.).
//
// All taken verbatim from MikeSemicolonD/N64ModernRuntime fork additions.
// Lifted into our repo so we don't have to maintain a librecomp fork.

#include <cstdio>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
// For RtlCaptureStackBackTrace + GetCurrentThreadId in the task-submit
// stack-trace gate. Keep these out of broader code so we don't pull in
// windows.h elsewhere.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")
#endif

#include <atomic>

#include "recomp.h"
#include "librecomp/helpers.hpp"
#include "librecomp/overlays.hpp"
#include "ultramodern/ultramodern.hpp"

// Runtime overlay registration. RS64 ships three overlays that all load at
// VA 0x800A5130 and swap at runtime: .ovl.mission, .ovl.menu, .ovl.cinematic.
// librecomp's boot-time load_overlays(0x1000, entrypoint, 1MB) only covers
// ROM offsets below 0x101000 — that registers .ovl.mission (rom 0xA5D30) but
// NOT .ovl.menu (rom 0x10C2D0) or .ovl.cinematic (rom 0x137580). Without this
// hook, calls into the menu/cinematic overlay regions keep dispatching to the
// mission overlay's recompiled functions, so the menu overlay's distinct code
// (attribution draw, menu screens) never runs.
//
// Called from a [[patches.hook]] on the game's loadOverlay (0x80000B20) with
// the overlay id in $a0. We unload whatever currently occupies the shared VA,
// then register the requested overlay's functions into func_map so subsequent
// direct calls resolve correctly. VA is always 0x800A5130; per-overlay ROM
// offset + size are the section_table entries from recomp_overlays.inl.
extern "C" void rs64_load_overlay(unsigned int overlay_id) {
    // Largest overlay size (mission, 0x665A0) — unload_overlays cleanly drops
    // any smaller overlay fully contained in this range.
    unload_overlays(0x800A5130, 0x000665A0);
    switch (overlay_id) {
        case 0: load_overlays(0x000A5D30, 0x800A5130, 0x000665A0); break; // .ovl.mission
        case 1: load_overlays(0x0010C2D0, 0x800A5130, 0x000283F0); break; // .ovl.menu
        case 2: load_overlays(0x00137580, 0x800A5130, 0x0000B810); break; // .ovl.cinematic
        default: break;
    }
    static int s_log = -1;
    if (s_log < 0) {
        const char* e = std::getenv("ROGUESQ_LOG_OVERLAY");
        s_log = (e && *e && *e != '0') ? 1 : 0;
    }
    if (s_log) {
        fprintf(stderr, "[overlay] loadOverlay(%u) -> registered functions in func_map\n", overlay_id);
        fflush(stderr);
    }
}

extern "C" void __osContRamRead_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    _return<s32>(ctx, -1);  // no rumble pak
}

extern "C" void __osContRamWrite_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    _return<s32>(ctx, -1);  // no rumble pak
}

extern "C" void __osPfsSelectBank_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    _return<s32>(ctx, 1);  // PFS_ERR_NOPACK — no memory pak
}

extern "C" void osDpGetCounters_recomp(uint8_t* rdram, recomp_context* ctx) {
    // Zero out the 8 DP counters. buf_ptr stays gpr (64-bit) so the
    // sign-extended MIPS address survives MEM_W's KSEG0 unmasking.
    gpr buf_ptr = ctx->r4;
    for (int i = 0; i < 8; i++) {
        MEM_W(i * 4, buf_ptr) = 0;
    }
}

extern "C" void osViGetCurrentField_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    ctx->r2 = 0;  // always field 0 (progressive / non-interlaced)
}

// Override upstream's `assert(false)` in ultra_translation.cpp:32. The real
// libultra osYieldThread voluntarily yields to the scheduler. On the host
// the closest equivalent is std::this_thread::yield(); blocking briefly is
// fine — the game only calls this from idle/wait paths.
//
// This symbol is defined in upstream librecomp too (with the assert), but
// the linker picks our definition first because /FORCE:MULTIPLE is enabled
// (already used for the patches/heap_guards.c override). Same trick.
extern "C" void osYieldThread_recomp(uint8_t* /*rdram*/, recomp_context* /*ctx*/) {
    std::this_thread::yield();
}

// RS64 fix: game calls osViBlack(1) twice during boot at funcs_0.c:2083 and
// :2142 and never osViBlack(0). That keeps ultramodern's VI_STATE_BLACK set
// forever → hStart forced to 0 → VI::visible() returns false in RT64 →
// PresentEarly matcher fails → no presents fire. Override the recomp wrapper
// so the game's calls are no-ops (visibility stays on by default).
extern "C" void osViBlack_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    static int s_count = 0;
    int n = ++s_count;
    if (n <= 8) {
        fprintf(stderr, "[osViBlack #%d] active=%u (ignored — RS64 fix)\n",
                n, (uint32_t)ctx->r4);
        fflush(stderr);
    }
}

// Diagnostic: log osViSwapBuffer calls. The game tells VI which fb to display
// via this call. If the address doesn't match any prior SET_COLOR_IMAGE
// address, RT64 has no rendered content for it → black screen.
extern "C" void osViSwapBuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    static int s_count = 0;
    int n = ++s_count;
    uint32_t fb = (uint32_t)ctx->r4;

    // EXPERIMENT 2026-05-13: ROGUESQ_FB_REDIRECT_HIRES — if the game asks VI
    // to display a lo-res fb (0x806BA000 / 0x806DD000 are 320×224 buffers,
    // ~0x23000 apart), redirect to the hi-res scratch fb at 0x8062B800
    // instead. Tests the hypothesis that attribution text lives in the
    // hi-res scratch but VI is swapping to an empty lo-res target.
    //
    // Empirically: SET_CIMG fires for both fb classes per frame, but ALL
    // texrects target 0x62B800 — the lo-res buffers only receive fillRect
    // clears. So this redirect should pull the actual drawn content into
    // VI's sampling stream.
    static int s_redirect = -1;
    if (s_redirect < 0) {
        const char* v = std::getenv("ROGUESQ_FB_REDIRECT_HIRES");
        s_redirect = (v && *v && v[0] != '0') ? 1 : 0;
        if (s_redirect) {
            fprintf(stderr, "[fb-redirect] lo-res VI swaps will be redirected to 0x8062B800 (hi-res scratch)\n");
            fflush(stderr);
        }
    }
    if (s_redirect) {
        const uint32_t phys = fb & 0x00FFFFFF;
        // Lo-res-fb signature: addresses 0x6BA000 / 0x6DD000 / 0x7DD000
        // (under 0x800000, in the small-fb cluster). Hi-res scratch is at
        // 0x62B800 — leave that untouched; redirect anything else in the
        // "expected VI fb" range to the hi-res scratch.
        if ((phys == 0x6BA000) || (phys == 0x6DD000) || (phys == 0x7DD000)) {
            uint32_t orig = fb;
            fb = 0x8062B800;
            if (n <= 8 || (n & 63) == 0) {
                fprintf(stderr, "[fb-redirect #%d] 0x%08X -> 0x%08X\n", n, orig, fb);
                fflush(stderr);
            }
        }
    }

    if (n <= 16 || (n & 63) == 0) {
        fprintf(stderr, "[osViSwapBuffer #%d] fb=0x%08X\n", n, fb);
        fflush(stderr);
    }
    // ROGUESQ_LOG_VI_FB_CONTENT=1: sample a few non-zero bytes from the fb
    // RDRAM to verify whether the game is writing attribution pixels into it
    // (vs the fb being empty and attribution rendering happening elsewhere).
    static int s_dump_fb = -1;
    if (s_dump_fb < 0) {
        const char* v = std::getenv("ROGUESQ_LOG_VI_FB_CONTENT");
        s_dump_fb = (v && *v && v[0] != '0') ? 1 : 0;
    }
    if (s_dump_fb && (n <= 64 || (n & 31) == 0)) {
        auto scan_fb = [&](uint32_t target_addr, const char* label) {
            const uint32_t phys = target_addr & 0x00FFFFFF;
            size_t nonzero = 0;
            uint32_t first_nz_off = 0xFFFFFFFFu;
            uint32_t first_nz_val = 0;
            const uint32_t scan = 320u * 224u * 2u;
            for (uint32_t i = 0; i < scan; i += 2) {
                uint32_t off = phys + i;
                uint8_t b0 = rdram[off ^ 3];
                uint8_t b1 = rdram[(off + 1) ^ 3];
                if (b0 || b1) {
                    if (first_nz_off == 0xFFFFFFFFu) {
                        first_nz_off = i;
                        first_nz_val = (uint32_t)((b0 << 8) | b1);
                    }
                    ++nonzero;
                }
            }
            fprintf(stderr, "[vi-fb-content #%d %s] addr=0x%08X nz_px=%zu/%u first@%u=0x%04X\n",
                    n, label, target_addr, nonzero, scan / 2u, first_nz_off, first_nz_val);
        };
        scan_fb(fb, "VI");
        // Also scan the hi-res scratch fb (0x8062B800) so we can see if
        // experimental op_02 output landed there.
        scan_fb(0x8062B800u, "hiRes");
        fflush(stderr);
    }
    extern void osViSwapBuffer(uint8_t* rdram, int32_t frameBufPtr);
    osViSwapBuffer(rdram, (int32_t)fb);
}

// Diagnostic: log osViSetMode calls + decode the OS_VI_MODE struct contents.
// OS_VI_MODE layout (libultra):
//   type      u32 at +0x00
//   comRegs.ctrl/width/burst/vSync/hSync/leap/hStart/xScale/vCurrent (9 u32) +0x04
//     → width at +0x08, xScale at +0x20
//   fldRegs[0].origin/yScale/vStart/vBurst/vIntr (5 u32) at +0x28
//     → yScale at +0x2C
//   fldRegs[1] at +0x3C (same layout)
// Total 80 bytes. We dump the first 80 bytes raw + key fields decoded so we
// can tell which mode is lo-res (width=320, xScale~0x200) vs hi-res
// (width=640, xScale~0x400).
extern "C" void osViSetMode_recomp(uint8_t* rdram, recomp_context* ctx) {
    static int s_count = 0;
    int n = ++s_count;
    uint32_t mode_ptr = (uint32_t)ctx->r4;
    uint32_t rdram_off = mode_ptr & 0x00FFFFFF;
    auto read_u32 = [&](uint32_t off) -> uint32_t {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v = (v << 8) | rdram[(rdram_off + off + i) ^ 3];
        }
        return v;
    };
    uint32_t type    = read_u32(0x00);
    uint32_t ctrl    = read_u32(0x04);
    uint32_t width   = read_u32(0x08);
    uint32_t xScale  = read_u32(0x20);
    uint32_t yScale  = read_u32(0x2C);
    fprintf(stderr,
        "[osViSetMode #%d] mode_ptr=0x%08X  type=0x%08X ctrl=0x%08X "
        "width=%u xScale=0x%08X yScale=0x%08X\n",
        n, mode_ptr, type, ctrl, width, xScale, yScale);
    fflush(stderr);
    extern void osViSetMode(uint8_t* rdram, int32_t modePtr);
    osViSetMode(rdram, (int32_t)ctx->r4);
}

extern "C" void osViSetXScale_recomp(uint8_t* rdram, recomp_context* ctx) {
    static int s_count = 0;
    int n = ++s_count;
    if (n <= 4) {
        fprintf(stderr, "[osViSetXScale #%d] scale=%f\n", n, (double)ctx->f12.fl);
        fflush(stderr);
    }
    extern void osViSetXScale(float scale);
    osViSetXScale(ctx->f12.fl);
}

extern "C" void osViSetYScale_recomp(uint8_t* rdram, recomp_context* ctx) {
    static int s_count = 0;
    int n = ++s_count;
    if (n <= 4) {
        fprintf(stderr, "[osViSetYScale #%d] scale=%f\n", n, (double)ctx->f12.fl);
        fflush(stderr);
    }
    extern void osViSetYScale(float scale);
    osViSetYScale(ctx->f12.fl);
}

// osGetMemSize override — defaults to 8MB (Expansion Pak present, hi-res
// mode available). ROGUESQ_MEM_SIZE_MB=4 forces 4MB to test base-N64 lo-res
// path. 2026-05-13 test confirmed 4MB alone crashes boot in mainBootstrapWorker
// (game still goes hi-res because the mode flag is gated on more than memsize),
// so the env var is a knob, not a recommended setting.
extern "C" void osGetMemSize_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    static int s_mb = -1;
    if (s_mb < 0) {
        const char* v = std::getenv("ROGUESQ_MEM_SIZE_MB");
        s_mb = (v && *v) ? std::atoi(v) : 8;
        if (s_mb != 8) {
            fprintf(stderr, "[osGetMemSize] reporting %d MB (env override)\n", s_mb);
            fflush(stderr);
        }
    }
    ctx->r2 = (gpr)(s_mb * 1024 * 1024);
}

// zmemcpy is stubbed in rogue_squadron.toml because the original MIPS code
// at 0x80018F4C contains a `cache 0x0D` instruction unsupported by N64Recomp.
// The toml comment claims "the runtime provides memcpy" but no such
// implementation existed — leaving zmemcpy as an empty body. That made
// zlib's inflate_flush a no-op (it calls zmemcpy to copy from the sliding
// window to the user's next_out buffer), so every decompressed asset stayed
// all-zero. This is the actual implementation: byte-by-byte copy honoring
// the recompile's XOR-3 byte swizzle on each access.
//
// Signature: void zmemcpy(Bytef *dest, const Bytef *source, uInt len);
//   a0/r4 = dest (MIPS virtual address)
//   a1/r5 = source (MIPS virtual address)
//   a2/r6 = len (byte count)
extern "C" void zmemcpy(uint8_t* rdram, recomp_context* ctx) {
    // Use the full 64-bit register values for the bounds check. The MEM_BU
    // macro does (reg + offset) ^ 3 - 0xFFFFFFFF80000000 in 64-bit, so
    // truncating to uint32_t can mask sign-extension issues.
    uint64_t dest_full = (uint64_t)ctx->r4;
    uint64_t src_full  = (uint64_t)ctx->r5;
    uint64_t len_full  = (uint64_t)ctx->r6;
    constexpr uint64_t KSEG0_BASE = 0xFFFFFFFF80000000ull;
    constexpr uint64_t KSEG0_END  = 0xFFFFFFFF80800000ull;
    constexpr uint32_t KSEG0_BASE_LO = 0x80000000u;
    constexpr uint32_t KSEG0_END_LO  = 0x80800000u;
    auto in_range = [](uint64_t addr_full, uint64_t n) {
        // Accept either sign-extended (0xFFFFFFFF80...) or zero-extended (0x80...)
        // as long as it lands within the 8MB rdram region.
        if (addr_full >= KSEG0_BASE && addr_full + n <= KSEG0_END) return true;
        uint32_t lo = (uint32_t)addr_full;
        if (lo >= KSEG0_BASE_LO && (uint64_t)lo + n <= KSEG0_END_LO &&
            (addr_full >> 32) == 0) return true;
        return false;
    };
    if (len_full > 0 && (!in_range(dest_full, len_full) || !in_range(src_full, len_full))) {
        static int s_warned = 0;
        if (s_warned++ < 8) {
            fprintf(stderr, "[zmemcpy] OOB skip dst=0x%016llX src=0x%016llX len=%llu\n",
                    (unsigned long long)dest_full,
                    (unsigned long long)src_full,
                    (unsigned long long)len_full);
            fflush(stderr);
        }
        ctx->r2 = ctx->r4;  // zlib's zmemcpy returns nothing meaningful
        return;
    }
    // Use full gpr (uint64_t) values for MEM_BU/MEM_B. Truncating to uint32_t
    // breaks the macro's `- 0xFFFFFFFF80000000` arithmetic under C integer
    // promotion (uint32 + int64 = the uint32 gets zero-extended, so subtracting
    // a sign-extended int64 produces a 4GB-offset host pointer → AV).
    gpr dest_reg = ctx->r4;
    gpr src_reg  = ctx->r5;
    uint32_t len = (uint32_t)len_full;
    for (uint32_t i = 0; i < len; ++i) {
        ctx->r2 = MEM_BU(i, src_reg);
        MEM_B(i, dest_reg) = ctx->r2;
    }
    ctx->r2 = ctx->r4;  // zlib's zmemcpy returns nothing meaningful
}

// heapWalker cycle-detection state. Updated by hooks in funcs_3.c (entry +
// loop-top). When the walk revisits its starting pointer, the hook breaks
// out of the loop instead of spinning forever.
extern "C" unsigned g_heapwalker_initial = 0;
extern "C" unsigned g_heapwalker_iter = 0;

// tickTextureMaterialExpiry cycle-detection state. Inner walk over the
// material list can loop forever when a node's next ptr points back into
// the visited set. KSEG0 guards prevent AVs but not cycles. Reset at
// function entry; capped iter count + first-node-revisit triggers bailout.
extern "C" unsigned g_tick_walker_first = 0;
extern "C" unsigned g_tick_walker_iter  = 0;

// func_800225F8 (texture-material list walker) cycle-detection state. Its
// linked-list walk (next ptr at +0x0) spins forever when the list is cyclic;
// the KSEG0 guard catches bad pointers but not a cycle of valid ones. Reset
// at function entry; first-node-revisit or iter cap triggers bailout.
extern "C" unsigned g_matwalk_first = 0;
extern "C" unsigned g_matwalk_iter  = 0;

// func_800079A4 (free-list insert/coalesce) cycle-detection state. Its
// L_800079B8 loop walks a next-pointer chain (field +0x0) with no KSEG0
// check and no cycle guard — the unguarded walk that AVs in the
// tickTextureMaterialExpiry -> func_800079A4 path. Reset at function entry.
extern "C" unsigned g_f79a4_first = 0;
extern "C" unsigned g_f79a4_iter  = 0;

// Watchdog for the cinematic inner loop. cinematicLoopBody iterates many
// times per cutscene playback; if it stops iterating for more than 2 seconds
// while the rest of the runtime keeps ticking, the game thread is hung in
// some callee. We capture the game thread's stack to find which one.
#ifdef _WIN32
static std::atomic<DWORD> g_cine_tid{0};
static std::atomic<uint64_t> g_cine_iter{0};
static std::atomic<uint64_t> g_cine_last_ms{0};

extern "C" void rs64_cine_dump_if_stuck(void);
extern "C" void rs64_cine_progress_log(void);
extern "C" void rs64_cine_start_watchdog_thread(void);

extern "C" void rs64_cine_iter_tick(unsigned iter) {
    g_cine_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_cine_iter.store(iter, std::memory_order_relaxed);
    g_cine_last_ms.store(GetTickCount64(), std::memory_order_relaxed);
    // First tick spawns the dedicated watchdog thread.
    rs64_cine_start_watchdog_thread();
}

// Spawn a dedicated watchdog thread the first time iter_tick fires. The
// gfx_thread's update_screen path could itself be deadlocked behind the
// same mutex chain that's hanging the game thread (events_context.message_mutex
// in ultramodern, in particular), so polling from gfx_thread isn't reliable.
// A standalone thread that only uses Sleep + atomics is immune.
extern "C" void rs64_cine_start_watchdog_thread(void) {
    static std::atomic<bool> s_started{false};
    bool was = s_started.exchange(true, std::memory_order_relaxed);
    if (was) return;
    std::thread([]() {
        for (;;) {
            ::Sleep(500);
            rs64_cine_progress_log();
            rs64_cine_dump_if_stuck();
        }
    }).detach();
}

// Periodic progress log so we can see whether the inner loop is iterating
// at all and at what rate. Called from update_screen on a coarse schedule.
extern "C" void rs64_cine_progress_log(void) {
    static uint64_t s_last_log_ms = 0;
    static uint64_t s_last_log_iter = 0;
    uint64_t now = GetTickCount64();
    if (now - s_last_log_ms < 2000) return;  // 2s cadence
    uint64_t cur = g_cine_iter.load(std::memory_order_relaxed);
    uint64_t delta = cur - s_last_log_iter;
    uint64_t age_ms = (g_cine_last_ms.load() ? now - g_cine_last_ms.load() : 0);
    fprintf(stderr, "[cine-progress] iter=%llu delta=%llu in %llums (idle=%llums)\n",
            (unsigned long long)cur, (unsigned long long)delta,
            (unsigned long long)(now - s_last_log_ms),
            (unsigned long long)age_ms);
    fflush(stderr);
    s_last_log_ms = now;
    s_last_log_iter = cur;
}

extern void print_stack_with_symbols(void** frames, USHORT count);

extern "C" void rs64_cine_dump_if_stuck(void) {
    static std::atomic<bool> s_dumped{false};
    if (s_dumped.load(std::memory_order_relaxed)) return;

    uint64_t last = g_cine_last_ms.load(std::memory_order_relaxed);
    if (last == 0) return;  // never started iterating

    uint64_t now = GetTickCount64();
    if (now - last < 2000) return;  // not stuck yet

    DWORD tid = g_cine_tid.load(std::memory_order_relaxed);
    if (tid == 0) return;

    bool expected = false;
    if (!s_dumped.compare_exchange_strong(expected, true)) return;  // single shot

    fprintf(stderr, "[cine-watchdog] freeze detected: tid=%lu iter=%llu idle=%llums — opening thread\n",
            tid, (unsigned long long)g_cine_iter.load(), (unsigned long long)(now - last));
    fflush(stderr);

    // Try several access masks — some Win10 builds reject the wide masks.
    DWORD masks[] = {
        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
        0x1F03FF,  // pre-Vista THREAD_ALL_ACCESS
        0x1FFFFF,  // Vista+ THREAD_ALL_ACCESS
    };
    HANDLE hThread = NULL;
    DWORD last_err = 0;
    for (DWORD m : masks) {
        hThread = OpenThread(m, FALSE, tid);
        if (hThread) {
            fprintf(stderr, "[cine-watchdog] OpenThread succeeded with mask=0x%lX\n", m);
            break;
        }
        last_err = GetLastError();
    }
    if (hThread == NULL) {
        fprintf(stderr, "[cine-watchdog] OpenThread failed for tid=%lu last_err=%lu (cur tid=%lu)\n",
                tid, last_err, GetCurrentThreadId());
        fflush(stderr);
        s_dumped.store(false, std::memory_order_relaxed);
        return;
    }
    fprintf(stderr, "[cine-watchdog] opened, suspending\n"); fflush(stderr);
    DWORD susp = SuspendThread(hThread);
    if (susp == (DWORD)-1) {
        fprintf(stderr, "[cine-watchdog] SuspendThread failed err=%lu\n", GetLastError());
        fflush(stderr);
        CloseHandle(hThread);
        return;
    }
    fprintf(stderr, "[cine-watchdog] suspended (prev count=%lu), getting context\n", susp); fflush(stderr);
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_ALL;
    if (!GetThreadContext(hThread, &ctx)) {
        fprintf(stderr, "[cine-watchdog] GetThreadContext failed err=%lu\n", GetLastError());
        fflush(stderr);
        ResumeThread(hThread);
        CloseHandle(hThread);
        return;
    }
    fprintf(stderr,
        "[cine-watchdog] game thread tid=%lu hung at RIP=0x%llX RSP=0x%llX RBP=0x%llX\n",
        tid, (unsigned long long)ctx.Rip, (unsigned long long)ctx.Rsp, (unsigned long long)ctx.Rbp);
    fflush(stderr);

    // Stack walk.
    STACKFRAME64 frame{};
    frame.AddrPC.Offset    = ctx.Rip; frame.AddrPC.Mode    = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp; frame.AddrStack.Mode = AddrModeFlat;
    void* frames[48];
    USHORT count = 0;
    HANDLE hProcess = GetCurrentProcess();
    SymInitialize(hProcess, NULL, TRUE);
    fprintf(stderr, "[cine-watchdog] walking stack\n"); fflush(stderr);
    while (count < 48) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, hProcess, hThread, &frame, &ctx,
                         NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL))
            break;
        if (frame.AddrPC.Offset == 0) break;
        frames[count++] = (void*)(uintptr_t)frame.AddrPC.Offset;
    }
    fprintf(stderr, "[cine-watchdog] walked %u frames, resuming + symbolizing\n", count); fflush(stderr);
    ResumeThread(hThread);
    CloseHandle(hThread);
    print_stack_with_symbols(frames, count);
    fflush(stderr);
}
#else
extern "C" void rs64_cine_iter_tick(unsigned) {}
extern "C" void rs64_cine_dump_if_stuck(void) {}
#endif

// Yield the cinematic CPU thread every N iterations to break the iter-810
// OS-level thread starvation stall. Empirically a kernel I/O call (stderr
// flush) unsticks it; Win32 Sleep alone does not. SwitchToThread yields to
// any ready thread on the same CPU.
extern "C" void rs64_cine_yield(void) {
    static int s_count = 0;
    ++s_count;
    // ~Every 16 iters: yield + flush stderr. Cheap enough to do hot.
    if ((s_count & 0xF) == 0) {
#ifdef _WIN32
        ::SwitchToThread();
#else
        std::this_thread::yield();
#endif
        std::fflush(stderr);
    }
}

// Pace the 32-iteration attribution-display loop inside
// runIdleFramesAndLoadSaveData (each iter renders one attribution frame).
// On real N64, each iter waits for the previous frame to complete via
// waitForPrevFrameDone → waitForPostSwapAck → which we patched to NOBLOCK
// (boot deadlock workaround). Without the wait, all 32 frames flash by in
// <1ms and the attribution screen is never visible.
// Sleep here so attribution displays for ~5 seconds (configurable).
//
// ROGUESQ_ATTRIBUTION_SLEEP_MS=N (default 150ms × 32 iters ≈ 4.8 sec).
// Set to 0 to disable.
extern "C" void rs64_idle_pace(void) {
#ifdef _WIN32
    static int s_sleep_ms = -1;
    if (s_sleep_ms < 0) {
        const char* e = std::getenv("ROGUESQ_ATTRIBUTION_SLEEP_MS");
        s_sleep_ms = (e && *e) ? std::atoi(e) : 150;
        if (s_sleep_ms < 0) s_sleep_ms = 0;
        if (s_sleep_ms > 5000) s_sleep_ms = 5000;
        fprintf(stderr, "[idle-pace] attribution sleep_ms=%d (32 iters ≈ %ds total)\n",
                s_sleep_ms, (s_sleep_ms * 32) / 1000);
        fflush(stderr);
    }
    if (s_sleep_ms == 0) return;
    ::Sleep((DWORD)s_sleep_ms);
#endif
}

// Pace cinematicLoopBody to a target frame rate. Without this, the loop
// runs at >1000 Hz on the host (no pacing from NOBLOCK-patched osRecvMesg
// waits), racing through the cinematic in <2 seconds and skipping past
// the attribution screen, fade, and N64-logo segments before they're
// visible. Default 30 Hz to match N64 native cinematic rate.
// Configure via ROGUESQ_CINE_TARGET_FPS env var: 0 = disabled, positive
// int = target FPS.
extern "C" void rs64_cine_pace(void) {
#ifdef _WIN32
    static int s_target_fps = -1;
    if (s_target_fps < 0) {
        const char* e = std::getenv("ROGUESQ_CINE_TARGET_FPS");
        s_target_fps = (e && *e) ? std::atoi(e) : 30;
        if (s_target_fps < 0) s_target_fps = 0;
        if (s_target_fps > 240) s_target_fps = 240;
        fprintf(stderr, "[cine-pace] init target_fps=%d (env='%s')\n",
                s_target_fps, e ? e : "(null)");
        fflush(stderr);
        // Bump Windows timer resolution to 1ms so Sleep(33) actually sleeps
        // ~33ms instead of the default ~15.6ms quantum.
        timeBeginPeriod(1);
    }
    if (s_target_fps == 0) return;  // disabled
    uint32_t target_interval_ms = 1000u / (uint32_t)s_target_fps;
    static uint64_t s_last_ms = 0;
    static uint32_t s_calls = 0;
    static uint32_t s_sleeps = 0;
    uint64_t now = GetTickCount64();
    if (s_last_ms == 0) {
        s_last_ms = now;
        return;
    }
    uint64_t elapsed = now - s_last_ms;
    if (elapsed < (uint64_t)target_interval_ms) {
        ::Sleep((DWORD)(target_interval_ms - elapsed));
        ++s_sleeps;
    }
    s_last_ms = GetTickCount64();
    ++s_calls;
    if (s_calls <= 5 || (s_calls & 0x3F) == 0) {
        fprintf(stderr, "[cine-pace] call=%u sleeps=%u elapsed=%llums interval=%ums\n",
                s_calls, s_sleeps, (unsigned long long)elapsed, target_interval_ms);
        fflush(stderr);
    }
#endif
}

// Diagnostic helper for [[patches.hook]] entries that need to log to
// stderr — the hook code lives in funcs_*.c which doesn't include stdio.
// Format args: 4 unsigned 32-bit values + a tag string.
extern "C" void rs64_dbg_log4(const char* tag, unsigned a, unsigned b, unsigned c, unsigned d) {
    static int s_log = -1;
    if (s_log < 0) {
        const char* e = std::getenv("ROGUESQ_LOG_HOOKS");
        s_log = (e && *e && *e != '0') ? 1 : 0;
    }
    if (!s_log) return;
    fprintf(stderr, "[hook] %s a=0x%08X b=0x%08X c=0x%08X d=0x%08X\n",
            tag ? tag : "?", a, b, c, d);
    fflush(stderr);
}

// Optional task-submission diagnostic. Gated by ROGUESQ_LOG_TASKSUBMIT.
// When enabled, logs task type + data_ptr/size + first 32 bytes of the DL
// buffer for the first 4 tasks + every 64th. Originally added 2026-05-09
// to investigate why GFX tasks were submitted with data_size=0; finding:
// Factor 5's task protocol uses data_ptr but ignores data_size (the DL
// chains via G_DL jumps to static setup code in .text). data_size=0 is
// normal for Factor 5 cinematic tasks.
extern void print_stack_with_symbols(void** frames, USHORT count);

extern "C" void osSpTaskStartGo_recomp(uint8_t* rdram, recomp_context* ctx) {
    static int s_log = -1;
    if (s_log < 0) {
        const char* e = std::getenv("ROGUESQ_LOG_TASKSUBMIT");
        s_log = (e && *e && *e != '0') ? 1 : 0;
    }
    // Separate gate: print host-side stack on first GFX submit so we can
    // identify which game thread / function chain is driving submission.
    // One-shot per task type — once we know who submits GFX vs audio, the
    // log is noise.
    {
        static int s_log_stk = -1;
        if (s_log_stk < 0) {
            const char* e = std::getenv("ROGUESQ_LOG_TASKSUBMIT_STACK");
            s_log_stk = (e && *e && *e != '0') ? 1 : 0;
        }
        if (s_log_stk) {
            OSTask* task = TO_PTR(OSTask, ctx->r4);
            static bool s_logged_gfx = false;
            static bool s_logged_audio = false;
            const bool is_gfx = (task->t.type == 1u);
            const bool is_audio = (task->t.type == 2u);
            if ((is_gfx && !s_logged_gfx) || (is_audio && !s_logged_audio)) {
                if (is_gfx) s_logged_gfx = true;
                if (is_audio) s_logged_audio = true;
                fprintf(stderr,
                    "[task-submit-stack] tid=%lu type=0x%X data_ptr=0x%08X ucode=0x%08X\n",
                    GetCurrentThreadId(),
                    (unsigned)task->t.type,
                    (unsigned)task->t.data_ptr,
                    (unsigned)task->t.ucode);
                void* frames[24];
                USHORT count = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
                print_stack_with_symbols(frames, count);
                fflush(stderr);
            }
        }
    }
    if (s_log) {
        OSTask* task = TO_PTR(OSTask, ctx->r4);
        static int n = 0;
        ++n;
        // Log first 4, every 64th, AND every GFX task regardless of count
        // (audio tasks fire at high rate and would mask GFX in normal sampling).
        const bool is_gfx = (task->t.type == 1u);
        if (n <= 4 || (n & 63) == 0 || is_gfx) {
            fprintf(stderr,
                "[task-submit #%d] type=0x%X flags=0x%X data_ptr=0x%08X data_size=%u ucode=0x%08X ucode_size=%u\n",
                n,
                (unsigned)task->t.type,
                (unsigned)task->t.flags,
                (unsigned)task->t.data_ptr,
                (unsigned)task->t.data_size,
                (unsigned)task->t.ucode,
                (unsigned)task->t.ucode_size);
            uint32_t dp = (uint32_t)task->t.data_ptr;
            if (task->t.type == M_GFXTASK && dp >= 0x80000000u && dp < 0x80800000u) {
                uint32_t off = dp - 0x80000000u;
                fprintf(stderr, "  data_ptr[0..32]:");
                for (int i = 0; i < 32; ++i) {
                    fprintf(stderr, " %02X", (unsigned)rdram[(off + i) ^ 3]);
                }
                fprintf(stderr, "\n");
            }
            fflush(stderr);
        }
    }
    ultramodern::submit_rsp_task(rdram, ctx->r4);
}

// Optional sender-trace for osSendMesg. Captures host stack on the first
// send to each unique queue address — quickly maps which game functions
// drive which queues. Gated by ROGUESQ_LOG_SENDMESG_STACK=1.
//
// We override the librecomp default (link warning re: duplicate symbol is
// expected; lld-link picks our version). The behavior is identical otherwise.
extern "C" int32_t osSendMesg(uint8_t* rdram, int32_t mq_, OSMesg mesg, s32 flag);

extern "C" void osSendMesg_recomp(uint8_t* rdram, recomp_context* ctx) {
    static int s_log_stk = -1;
    if (s_log_stk < 0) {
        const char* e = std::getenv("ROGUESQ_LOG_SENDMESG_STACK");
        s_log_stk = (e && *e && *e != '0') ? 1 : 0;
    }
    if (s_log_stk) {
        // Only trace sends to the graph thread's queue at 0x8011A420 — the
        // queue func_80019BF4 receives on. Other queues are noise for this
        // investigation. Easy to extend later by editing this list.
        const uint32_t target_mq = 0x8011A420u;
        const uint32_t mq_addr = (uint32_t)ctx->r4;
        if (mq_addr == target_mq) {
            static int s_count = 0;
            ++s_count;
            if (s_count <= 3) {
                fprintf(stderr,
                    "[sendmesg #%d] tid=%lu mq=0x%08X mesg_ptr=0x%08X flag=%d\n",
                    s_count,
                    GetCurrentThreadId(),
                    mq_addr,
                    (unsigned)ctx->r5,
                    (int)ctx->r6);
                void* frames[24];
                USHORT count = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
                print_stack_with_symbols(frames, count);
                // Dump the message bytes (first 0x10) so we can see the tag/payload.
                uint32_t mesg_ptr = (uint32_t)ctx->r5;
                if (mesg_ptr >= 0x80000000u && mesg_ptr < 0x80800000u) {
                    uint32_t off = mesg_ptr - 0x80000000u;
                    fprintf(stderr, "  mesg[0..16]:");
                    for (int i = 0; i < 16; ++i) {
                        fprintf(stderr, " %02X", (unsigned)rdram[(off + i) ^ 3]);
                    }
                    fprintf(stderr, "\n");
                }
                fflush(stderr);
            }
        }
    }
    ctx->r2 = osSendMesg(rdram, (int32_t)ctx->r4, (OSMesg)ctx->r5, (s32)ctx->r6);
}

// Override upstream's osStopThread_recomp because upstream's osStopThread
// asserts(false) when called with a non-self thread handle (threads.cpp:278).
// RS64's func_80000C68 stops other threads. We replicate libultra's actual
// semantics: remove the target thread from any queue it's blocked on and
// mark it STOPPED so the scheduler doesn't try to schedule it. The host
// std::thread is parked in wait_for_resumed until a future osStartThread.
extern "C" void osStopThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSThread) t_ = (PTR(OSThread))(int32_t)ctx->r4;
    if (t_ == NULLPTR) {
        // Self-stop: same path upstream takes — yield to scheduler.
        ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
        return;
    }
    OSThread* t = TO_PTR(OSThread, t_);
    if (t->state != OSThreadState::STOPPED) {
        if (t->queue != NULLPTR) {
            ultramodern::thread_queue_remove(PASS_RDRAM t->queue, t_);
        }
        t->state = OSThreadState::STOPPED;
    }
}

