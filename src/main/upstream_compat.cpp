// libultra _recomp bindings upstream librecomp does not provide, plus the ones we override
// (/FORCE:MULTIPLE picks these over librecomp's) to match hardware semantics this game needs.
// Hook entry points for rogue_squadron.toml live in hook_helpers.cpp.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <intrin.h>
#pragma comment(lib, "dbghelp.lib")
#else
#define _ReturnAddress() __builtin_return_address(0)
#endif

#include "recomp.h"
#include "librecomp/helpers.hpp"
#include "librecomp/overlays.hpp"
#include "ultramodern/ultramodern.hpp"
#include "debug_logs.h"
#include "upstream_compat.h"   // declares the exports defined below (rs64_vi_driven, g_active_overlay, …)

using recomp::dbg::env_str;
using recomp::dbg::env_on;
using recomp::dbg::env_int;

extern "C" void rs64_gfx_task_submitted(void);          // ultramodern events.cpp
extern "C" void rs64_wait_gfx_parse(void);
extern "C" int rs64_gfx_parse_inflight_wait(int ms);
void dequeue_external_messages(uint8_t* rdram);         // ultramodern mesgqueue.cpp, C++ linkage
extern void print_stack_with_symbols(void** frames, unsigned short count);  // main.cpp

// ---- Switches shared with other host files ----

// Default on: the game frame loop runs the hardware way (blocking frame-barrier receives, real
// post-swap ack wait, no host pacing). ROGUESQ_VI_DRIVEN_LOOP=0 restores host-token pacing for A/B.
extern "C" int rs64_vi_driven(void) {
    static const int s = env_on("ROGUESQ_VI_DRIVEN_LOOP", true) ? 1 : 0;
    return s;
}

// ROGUESQ_FB_GUARDS bitmask: 1 = CIMG neutralizer, 2 = (retired), 4 = RT64 fb-registry sanitizer.
// Default 6; the neutralizer rewrote DL payload words, so it is opt-in.
extern "C" int rs64_fb_guards_mask(void) {
    static const int s = env_int("ROGUESQ_FB_GUARDS", 6);
    return s;
}
extern "C" int rs64_fb_guards(void) { return rs64_fb_guards_mask() != 0; }

// ---- Overlays ----

// Which overlay is mapped at 0x800A5130: 0 = mission, 1 = menu, 2 = cinematic, -1 = none yet.
// The F5 op_01 handler loads matrices only for the menu overlay.
extern "C" volatile int g_active_overlay = -1;

// Called from the loadOverlay (0x80000B20) hook with the overlay id. librecomp's boot-time
// load_overlays only covers ROM offsets below 0x101000, so the menu and cinematic overlays
// have to be swapped in here.
extern "C" void rs64_load_overlay(unsigned int overlay_id) {
    unload_overlays(0x800A5130, 0x000665A0);   // largest overlay (mission) covers the others
    switch (overlay_id) {
        case 0: load_overlays(0x000A5D30, 0x800A5130, 0x000665A0); break; // .ovl.mission
        case 1: load_overlays(0x0010C2D0, 0x800A5130, 0x000283F0); break; // .ovl.menu
        case 2: load_overlays(0x00137580, 0x800A5130, 0x0000B810); break; // .ovl.cinematic
        default: break;
    }
    if (overlay_id <= 2) g_active_overlay = (int)overlay_id;
}

// ---- Stubs ----

extern "C" void __osContRamRead_recomp(uint8_t* /*rdram*/, recomp_context* ctx) { _return<s32>(ctx, -1); }   // no rumble pak
extern "C" void __osContRamWrite_recomp(uint8_t* /*rdram*/, recomp_context* ctx) { _return<s32>(ctx, -1); }
extern "C" void __osPfsSelectBank_recomp(uint8_t* /*rdram*/, recomp_context* ctx) { _return<s32>(ctx, 1); }  // PFS_ERR_NOPACK
extern "C" void osViGetCurrentField_recomp(uint8_t* /*rdram*/, recomp_context* ctx) { ctx->r2 = 0; }

// Per-frame RT64 workload counts (lib/rt64 rt64_state.cpp), read-and-cleared here.
extern "C" std::atomic<uint32_t> g_rs64_frame_tris;
extern "C" std::atomic<uint32_t> g_rs64_frame_draws;
extern "C" std::atomic<uint32_t> g_rs64_frame_texloads;

extern "C" void osDpGetCounters_recomp(uint8_t* rdram, recomp_context* ctx) {
    gpr buf_ptr = ctx->r4;   // full gpr so the sign-extended address survives MEM_W
    for (int i = 0; i < 8; i++) MEM_W(i * 4, buf_ptr) = 0;

    // No real RDP on the PC path, so feed the F5 profiler HUD's three RDP slots
    // genuine RT64 workload proxies (counts since last frame). buf[1/2/3] are the
    // struct's +0x14/+0x18/+0x1C fields submitGfxFrame scales into the HUD.
    static int ts = -1, ds = -1, xs = -1;
    if (ts < 0) { const char* s = recomp::os::getenv("ROGUESQ_TRIS_SCALE"); ts = (s && *s) ? atoi(s) : 64; }
    if (ds < 0) { const char* s = recomp::os::getenv("ROGUESQ_DRAW_SCALE"); ds = (s && *s) ? atoi(s) : 256; }
    if (xs < 0) { const char* s = recomp::os::getenv("ROGUESQ_TEX_SCALE");  xs = (s && *s) ? atoi(s) : 256; }
    uint32_t tris     = g_rs64_frame_tris.exchange(0, std::memory_order_relaxed);
    uint32_t draws    = g_rs64_frame_draws.exchange(0, std::memory_order_relaxed);
    uint32_t texloads = g_rs64_frame_texloads.exchange(0, std::memory_order_relaxed);
    uint32_t magv = draws * (uint32_t)ds;
    // submitGfxFrame reads these as cumulative counters: white = buf[2] - magenta.
    // Add magenta back into buf[2] so the delta resolves to exactly the tri proxy
    // (magenta and green are read directly, no subtraction).
    MEM_W(1 * 4, buf_ptr) = (int32_t)(magv);                            // magenta: cmd/draw load
    MEM_W(2 * 4, buf_ptr) = (int32_t)(tris * (uint32_t)ts + magv);      // white:   pipe/raster load
    MEM_W(3 * 4, buf_ptr) = (int32_t)(texloads * (uint32_t)xs);         // green:   tmem/texture load
}

// The game calls osViBlack(1) twice at boot and never osViBlack(0); honouring it would leave
// ultramodern's VI black forever. No-op.
extern "C" void osViBlack_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    static int s_count = 0;
    if (++s_count <= 8 && recomp::dbg::log_vi()) {
        fprintf(stderr, "[osViBlack #%d] active=%u (ignored)\n", s_count, (uint32_t)ctx->r4);
        fflush(stderr);
    }
}

// ROGUESQ_MEM_SIZE_MB=4 reports a base N64 (crashes boot; knob only).
extern "C" void osGetMemSize_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    static const int s_mb = env_int("ROGUESQ_MEM_SIZE_MB", 8);
    ctx->r2 = (gpr)(s_mb * 1024 * 1024);
}

// __osInitialize_common is HLE'd and never touches the game's libultra globals. Real libultra
// leaves osClockRate = OS_CPU_COUNTER (46875000); the ROM's static initializer is OS_CLOCK_RATE
// (62500000), so every osGetTime-derived delta ran at 3/4 speed.
extern "C" void osInitialize(void);
static void rs64_os_initialize(uint8_t* rdram) {
    osInitialize();
    *reinterpret_cast<uint32_t*>(rdram + 0x39100) = 0u;          // osClockRate (u64) high word
    *reinterpret_cast<uint32_t*>(rdram + 0x39104) = 46875000u;   // low word = OS_CPU_COUNTER
}
extern "C" void __osInitialize_common_recomp(uint8_t* rdram, recomp_context*) { rs64_os_initialize(rdram); }
extern "C" void osInitialize_recomp(uint8_t* rdram, recomp_context*) { rs64_os_initialize(rdram); }

// zmemcpy is stubbed in rogue_squadron.toml (its MIPS body has a `cache` instruction N64Recomp
// cannot recompile). Without a body zlib's inflate_flush copied nothing. Byte copy through the
// XOR-3 swizzle; full gpr values so MEM_* sign-extension arithmetic stays correct.
extern "C" void zmemcpy(uint8_t* rdram, recomp_context* ctx) {
    uint64_t dest_full = (uint64_t)ctx->r4;
    uint64_t src_full  = (uint64_t)ctx->r5;
    uint64_t len_full  = (uint64_t)ctx->r6;
    auto in_range = [](uint64_t addr, uint64_t n) {
        if (addr >= 0xFFFFFFFF80000000ull && addr + n <= 0xFFFFFFFF80800000ull) return true;
        uint32_t lo = (uint32_t)addr;
        return (addr >> 32) == 0 && lo >= 0x80000000u && (uint64_t)lo + n <= 0x80800000u;
    };
    if (len_full > 0 && (!in_range(dest_full, len_full) || !in_range(src_full, len_full))) {
        static int s_warned = 0;
        if (s_warned++ < 8) {
            fprintf(stderr, "[zmemcpy] OOB skip dst=0x%016llX src=0x%016llX len=%llu\n",
                    (unsigned long long)dest_full, (unsigned long long)src_full, (unsigned long long)len_full);
            fflush(stderr);
        }
        ctx->r2 = ctx->r4;
        return;
    }
    gpr dest_reg = ctx->r4, src_reg = ctx->r5;
    uint32_t len = (uint32_t)len_full;
    for (uint32_t i = 0; i < len; ++i) {
        ctx->r2 = MEM_BU(i, src_reg);
        MEM_B(i, dest_reg) = ctx->r2;
    }
    ctx->r2 = ctx->r4;
}

// ---- VI ----

// Last osViSwapBuffer target; the present side reads it.
extern "C" volatile unsigned g_last_swap_fb = 0;

extern "C" void osViSwapBuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t fb = (uint32_t)ctx->r4;
    g_last_swap_fb = fb;
    static int s_count = 0;
    const int n = ++s_count;
    if (recomp::dbg::log_vi() && (n <= 200 || (n & 63) == 0)) {
        fprintf(stderr, "[osViSwapBuffer #%d] fb=0x%08X\n", n, fb);
        fflush(stderr);
    }
    extern void osViSwapBuffer(uint8_t* rdram, int32_t frameBufPtr);
    osViSwapBuffer(rdram, (int32_t)fb);
}

extern "C" void osViSetMode_recomp(uint8_t* rdram, recomp_context* ctx) {
    if (recomp::dbg::log_vi()) {
        static int s_count = 0;
        const uint32_t mode_ptr = (uint32_t)ctx->r4;
        const uint32_t off = mode_ptr & 0x00FFFFFF;
        auto read_u32 = [&](uint32_t o) { uint32_t v = 0; for (int i = 0; i < 4; ++i) v = (v << 8) | rdram[(off + o + i) ^ 3]; return v; };
        // OS_VI_MODE: type +0, ctrl +4, width +8, xScale +0x20, yScale +0x2C.
        fprintf(stderr, "[osViSetMode #%d] mode_ptr=0x%08X type=0x%08X ctrl=0x%08X width=%u xScale=0x%08X yScale=0x%08X\n",
                ++s_count, mode_ptr, read_u32(0x00), read_u32(0x04), read_u32(0x08), read_u32(0x20), read_u32(0x2C));
        fflush(stderr);
    }
    extern void osViSetMode(uint8_t* rdram, int32_t modePtr);
    osViSetMode(rdram, (int32_t)ctx->r4);
}

extern "C" void osViSetXScale_recomp(uint8_t*, recomp_context* ctx) {
    static int s_count = 0;
    if (++s_count <= 4 && recomp::dbg::log_vi()) { fprintf(stderr, "[osViSetXScale #%d] scale=%f\n", s_count, (double)ctx->f12.fl); fflush(stderr); }
    extern void osViSetXScale(float scale);
    osViSetXScale(ctx->f12.fl);
}

extern "C" void osViSetYScale_recomp(uint8_t*, recomp_context* ctx) {
    static int s_count = 0;
    if (++s_count <= 4 && recomp::dbg::log_vi()) { fprintf(stderr, "[osViSetYScale #%d] scale=%f\n", s_count, (double)ctx->f12.fl); fflush(stderr); }
    extern void osViSetYScale(float scale);
    osViSetYScale(ctx->f12.fl);
}

// ---- Message trace ----

#ifdef _WIN32
static const char* rs64_host_caller_name(void* addr) {
    static std::map<void*, std::string> cache;
    auto it = cache.find(addr);
    if (it != cache.end()) return it->second.c_str();
    static bool init = [](){ SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS); return SymInitialize(GetCurrentProcess(), nullptr, TRUE) != 0; }();
    char buf[sizeof(SYMBOL_INFO) + 256]; SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 255; DWORD64 disp = 0;
    char out[300];
    if (init && SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)addr, &disp, sym)) snprintf(out, sizeof out, "%s+0x%llx", sym->Name, (unsigned long long)disp);
    else snprintf(out, sizeof out, "rva%llx", (unsigned long long)((uintptr_t)addr - (uintptr_t)GetModuleHandleW(nullptr)));
    return cache.emplace(addr, out).first->second.c_str();
}

// Hardware write-watch (DR0, 4 bytes) on one host RDRAM word. The VEH only records the
// faulting RIP and new value so the writing thread is not slowed; the drain symbolizes.
struct Rs64BpHit { uint32_t value; uint32_t tid; uint64_t rip; };
static Rs64BpHit s_bp_hits[256];
static std::atomic<unsigned> s_bp_nhits{0};
static volatile uint32_t* s_bp_addr = nullptr;

static LONG CALLBACK rs64_bp_veh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(ep->ContextRecord->Dr6 & 1))
        return EXCEPTION_CONTINUE_SEARCH;
    ep->ContextRecord->Dr6 = 0;
    const unsigned i = s_bp_nhits.fetch_add(1);
    if (i < 256) s_bp_hits[i] = { *s_bp_addr, GetCurrentThreadId(), ep->ContextRecord->Rip };
    return EXCEPTION_CONTINUE_EXECUTION;
}

extern "C" void rs64_data_bp_arm(void* host_addr) {
    SymSetOptions(SymGetOptions() | SYMOPT_LOAD_LINES);
    s_bp_addr = (volatile uint32_t*)host_addr;
    AddVectoredExceptionHandler(1, rs64_bp_veh);
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    int armed = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != (DWORD)-1) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c)) {
                c.Dr0 = (DWORD64)host_addr;
                c.Dr7 = (c.Dr7 & ~0xF0003ull) | 1ull | (1ull << 16) | (3ull << 18);
                if (SetThreadContext(t, &c)) ++armed;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    fprintf(stderr, "[data-bp] armed %d threads at host %p\n", armed, host_addr);
    fflush(stderr);
}

extern "C" void rs64_data_bp_drain(int vi) {
    static unsigned s_done = 0;
    const unsigned n = std::min(s_bp_nhits.load(), 256u);
    for (; s_done < n; ++s_done) {
        const Rs64BpHit& h = s_bp_hits[s_done];
        const char* fn = rs64_host_caller_name((void*)h.rip);
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof line;
        DWORD ldisp = 0;
        const bool has_line = SymGetLineFromAddr64(GetCurrentProcess(), h.rip, &ldisp, &line) != 0;
        fprintf(stderr, "[data-bp] hit#%u vi=#%d tid=%u value=%08X at %s (%s:%lu)\n", s_done + 1, vi, h.tid, h.value, fn,
                has_line ? line.FileName : "?", has_line ? line.LineNumber : 0ul);
    }
    if (n) fflush(stderr);
}
#endif

// ROGUESQ_LOG_MESG_TRACE=1: thread/message order for cinematic frames ROGUESQ_MESG_TRACE_FRAMES=lo-hi
// (default 60-70) -> logs/mesg_trace_recomp.csv, same first six columns as the PJ64 hook in
// tools/validate/pj64_rs64_dump.js, plus effective flags, host thread id, host caller.
static void rs64_mesg_trace(uint8_t* rdram, const char* ev, uint32_t q, int flags, uint32_t ra, int eff_flags = -1) {
#ifdef _WIN32
    static const bool on = env_on("ROGUESQ_LOG_MESG_TRACE");
    if (!on) return;
    static uint32_t lo = 60, hi = 70;
    static const bool win = [](){ if (const char* e = env_str("ROGUESQ_MESG_TRACE_FRAMES")) { char* end = nullptr; unsigned long a = std::strtoul(e, &end, 10); if (end && *end == '-') { unsigned long b = std::strtoul(end + 1, nullptr, 10); lo = (uint32_t)a; hi = (uint32_t)b; } } return true; }();
    (void)win;
    uint32_t frame = *reinterpret_cast<const uint32_t*>(rdram + 0x13889C);
    if (frame < lo || frame > hi) return;
    static std::mutex m; std::lock_guard<std::mutex> lk(m);
    static FILE* f = recomp::os::fopen("../../logs/mesg_trace_recomp.csv", "w");   // cwd is build/Debug
    if (!f) return;
    void* frames[4]; USHORT n = RtlCaptureStackBackTrace(2, 4, frames, nullptr);
    const char* caller = n ? rs64_host_caller_name(frames[0]) : "?";
    if (ev[0] == 'y' && strncmp(caller, "waitForMusyXAudioTaskDone", 25) == 0) { static unsigned spin = 0; if (++spin % 1000 != 1) return; }
    PTR(OSThread) self = ultramodern::this_thread();
    int tid = (self >= 0x80000000u && self < 0x80800000u) ? (int)TO_PTR(OSThread, self)->id : -1;   // bootstrap thread has none
    fprintf(f, "%u,%s,%d,%x,%d,%x,%d,%lu,%s\n", frame, ev, tid, q, flags, ra,
            eff_flags < 0 ? flags : eff_flags, GetCurrentThreadId(), caller);
    fflush(f);
#else
    (void)rdram; (void)ev; (void)q; (void)flags; (void)ra; (void)eff_flags;
#endif
}

// ---- Threads and scheduling ----

// Real libultra semantics: re-queue at priority and switch to the highest runnable thread. A host
// yield keeps the N64 scheduler slot, so any game spin-loop starves every other N64 thread.
// ROGUESQ_HOST_YIELD_ONLY=1 restores that.
extern "C" void osYieldThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    rs64_mesg_trace(rdram, "yield", 0, 0, (uint32_t)ctx->r31);
    static const bool s_old = env_on("ROGUESQ_HOST_YIELD_ONLY");
    if (s_old) { std::this_thread::yield(); return; }
    // Host VI/DP/AI events arrive as external messages that only drain inside osRecvMesg/osSendMesg
    // or when the run queue is empty; a spinning thread would never let the VI thread wake.
    dequeue_external_messages(rdram);
    if (ultramodern::thread_queue_empty(rdram, ultramodern::running_queue)) {
        ultramodern::wait_for_external_message_timed(rdram, 1);
        dequeue_external_messages(rdram);
    }
    ultramodern::schedule_running_thread(rdram, ultramodern::this_thread());
    ultramodern::run_next_thread_and_wait(rdram);
}

// Attribution-loop pacing: wait for the next host VI tick by yielding through the N64 scheduler
// (~1 ms per step, cap ~40), so pending VI/DP events are delivered and the higher-priority VI
// handler preempts the loop as on hardware. A host Sleep here holds the run slot and let the
// frame-buffer arbiter deadlock (producer waiting SP-done, VI handler blocked on its ack).
// ROGUESQ_ATTRIB_HOST_SLEEP=1 restores the host-sleep wait.
extern "C" volatile unsigned g_vi_tick;
extern "C" void rs64_attrib_wait_vi(void);
extern "C" void rs64_attrib_wait_vi_yield(uint8_t* rdram, recomp_context* ctx) {
    static const bool s_host = env_on("ROGUESQ_ATTRIB_HOST_SLEEP");
    if (s_host) {
        rs64_attrib_wait_vi();
        return;
    }
    const unsigned start = g_vi_tick;
    for (int i = 0; i < 40 && g_vi_tick == start; ++i) osYieldThread_recomp(rdram, ctx);
}

// ROGUESQ_LOG_THREADS=1: one line per thread start/stop/destroy with the target's state and the
// recompiled caller (lost-thread / scheduler diagnosis).
static void rs64_thread_log(const char* op, uint8_t* rdram, PTR(OSThread) t_, void* ret) {
    static const bool on = env_on("ROGUESQ_LOG_THREADS");
    if (!on) return;
    const OSThread* t = t_ ? TO_PTR(OSThread, t_) : nullptr;
#ifdef _WIN32
    const char* caller = rs64_host_caller_name(ret);
#else
    const char* caller = "?"; (void)ret;
#endif
    fprintf(stderr, "[thr] vi=%u %s t=%08X id=%d pri=%d state=%d queue=%08X self=%08X caller=%s\n",
            g_vi_tick, op, (uint32_t)t_, t ? (int)t->id : -1, t ? (int)t->priority : -1, t ? (int)t->state : -1,
            t ? (uint32_t)t->queue : 0u, (uint32_t)ultramodern::this_thread(), caller);
    fflush(stderr);
}

extern "C" void osStartThread(uint8_t* rdram, PTR(OSThread) t);
extern "C" void osStartThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    rs64_mesg_trace(rdram, "start", (uint32_t)ctx->r4, 0, (uint32_t)ctx->r31);
    rs64_thread_log("start", rdram, (PTR(OSThread))(int32_t)ctx->r4, _ReturnAddress());
    osStartThread(rdram, (int32_t)ctx->r4);
}

// Upstream's osStopThread asserts on a non-self handle; the game stops other threads. libultra
// semantics: pull the target off whatever queue it waits on and mark it STOPPED.
extern "C" void osStopThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSThread) t_ = (PTR(OSThread))(int32_t)ctx->r4;
    rs64_thread_log("stop", rdram, t_, _ReturnAddress());
    if (t_ == NULLPTR) {
        ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
        return;
    }
    OSThread* t = TO_PTR(OSThread, t_);
    if (t->state != OSThreadState::STOPPED) {
        if (t->queue != NULLPTR) ultramodern::thread_queue_remove(PASS_RDRAM t->queue, t_);
        t->state = OSThreadState::STOPPED;
    }
}

// The boot thread destroys a queued thread whose queue chain can be outside RDRAM; ultramodern's
// walk then faults. Mark such a thread STOPPED so the walk is skipped.
extern "C" void osDestroyThread(uint8_t* rdram, PTR(OSThread) t_);
extern "C" void osDestroyThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSThread) t_ = (PTR(OSThread))(int32_t)ctx->r4;
    rs64_thread_log("destroy", rdram, t_, _ReturnAddress());
    if (t_ != NULLPTR) {
        OSThread* t = TO_PTR(OSThread, t_);
        auto in_ram = [](uint32_t p) { return p >= 0x80000000u && p < 0x80800000u; };
        // A non-STOPPED thread with a NULL queue is between queues; treat as in no queue.
        bool qok = (t->queue == ultramodern::running_queue) || in_ram((uint32_t)t->queue) ||
                   (t->queue == NULLPTR && t->state == (uint16_t)OSThreadState::STOPPED);
        bool chain_ok = true; int hops = 0;
        if (qok && t->queue != NULLPTR && t->queue != ultramodern::running_queue && t->state != (uint16_t)OSThreadState::STOPPED) {
            PTR(OSThread) cur = *TO_PTR(PTR(OSThread), t->queue);
            while (cur != NULLPTR && hops < 64) {
                if (!in_ram((uint32_t)cur)) { chain_ok = false; break; }
                cur = TO_PTR(OSThread, cur)->next; ++hops;
            }
        }
        if (recomp::dbg::log_threads() || !qok || !chain_ok) {
            fprintf(stderr, "[osDestroyThread] t=0x%08X id=%d pri=%d state=%u queue=0x%08X qok=%d chain_ok=%d hops=%d\n",
                    (uint32_t)t_, (int)t->id, (int)t->priority, (unsigned)t->state, (uint32_t)t->queue, (int)qok, (int)chain_ok, hops);
            fflush(stderr);
        }
        if (!qok || !chain_ok) t->state = (uint16_t)OSThreadState::STOPPED;
    }
    osDestroyThread(rdram, t_);
}

// ---- Message queues ----

// Wait for the in-flight RT64 parse from an N64 thread, yielding between slices so higher-priority
// N64 threads (retrace: buffer swap + audio) run meanwhile, as hardware preemption would let them.
// Also called from hooks in rogue_squadron.toml.
extern "C" volatile unsigned g_rs64_pw_calls = 0, g_rs64_pw_waited = 0, g_rs64_pw_ms = 0, g_rs64_pw_timeouts = 0;
extern "C" void rs64_wait_gfx_parse_yield(uint8_t* rdram, recomp_context* ctx) {
    g_rs64_pw_calls = g_rs64_pw_calls + 1;
    const auto t0 = std::chrono::steady_clock::now();
    auto ms_since = [&]() { return (unsigned)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(); };
    for (int i = 0; i < 500; ++i) {
        if (!rs64_gfx_parse_inflight_wait(1)) { if (i) { g_rs64_pw_waited = g_rs64_pw_waited + 1; g_rs64_pw_ms = g_rs64_pw_ms + ms_since(); } return; }
        osYieldThread_recomp(rdram, ctx);
    }
    g_rs64_pw_timeouts = g_rs64_pw_timeouts + 1; g_rs64_pw_ms = g_rs64_pw_ms + ms_since();
}

extern "C" void osRecvMesg_recomp(uint8_t* rdram, recomp_context* ctx) {
    s32 flags = (s32)ctx->r6;
    const uint32_t q = (uint32_t)ctx->r4;
    // Host-paced loop only: the per-frame gfx-barrier receives (0x8011A7E8 / 0x8011A818) would
    // otherwise crawl at the VI-prime token rate. ROGUESQ_GFX_BARRIER_NOBLOCK=0 disables.
    static const bool s_nb = env_on("ROGUESQ_GFX_BARRIER_NOBLOCK", true);
    if (s_nb && !rs64_vi_driven() && (q == 0x8011A7E8u || q == 0x8011A818u)) flags = 0;   // OS_MESG_NOBLOCK
    rs64_mesg_trace(rdram, "recv", q, (int)(s32)ctx->r6, (uint32_t)ctx->r31, (int)flags);
    ctx->r2 = osRecvMesg(rdram, (int32_t)ctx->r4, (int32_t)ctx->r5, flags);
    // Frame start (video queue 0x80128CF0): the game is about to rebuild the chunks of the buffer
    // slot it just got back. On hardware the RSP finished reading them long ago; RT64's parse may
    // still be running. Hold here until no graphics parse is in flight.
    if (q == 0x80128CF0u && ctx->r2 == 0 && rs64_vi_driven()) {
        static unsigned s_n = 0; ++s_n;
        const int before = rs64_gfx_parse_inflight_wait(0);
        rs64_wait_gfx_parse_yield(rdram, ctx);
        static const bool s_lg = env_on("ROGUESQ_LOG_GFX_TASK");
        if (s_lg && (s_n <= 4 || (s_n & (s_n - 1)) == 0)) {
            fprintf(stderr, "[parse-wait] frame-recv #%u inflight-now=%d | calls=%u waited=%u timeouts=%u total=%u ms\n",
                    s_n, before, g_rs64_pw_calls, g_rs64_pw_waited, g_rs64_pw_timeouts, g_rs64_pw_ms);
            fflush(stderr);
        }
    }
    // Message type the SP scheduler (0x8011A420) / DP handler (0x8011A408) dequeued: 1/2 = gfx/audio
    // request, 0x81 = SP-done. Those sends come from ultramodern, so they are invisible otherwise.
    if ((q == 0x8011A420u || q == 0x8011A408u) && ctx->r2 == 0) {
        uint32_t mp = (uint32_t)ctx->r5, mv = 0;
        if (mp >= 0x80000000u && mp < 0x80800000u) mv = *reinterpret_cast<uint32_t*>(rdram + (mp & 0x7FFFFFu));
        int type = (mv >= 0x80000000u && mv < 0x80800000u) ? rdram[(mv & 0x7FFFFFu) ^ 3] : -1;
        rs64_mesg_trace(rdram, "recvret", q, type, mv);
    }
}

extern "C" int32_t osSendMesg(uint8_t* rdram, int32_t mq_, OSMesg mesg, s32 flag);
extern "C" void osSendMesg_recomp(uint8_t* rdram, recomp_context* ctx) {
    rs64_mesg_trace(rdram, "send", (uint32_t)ctx->r4, (int)(s32)ctx->r6, (uint32_t)ctx->r31);
    // A graphics task is in flight from the game's REQUEST (type-1 message to the RSP scheduler
    // queue), not from osSpTaskStartGo: with an audio task on the RSP the request sits pending while
    // the game already rebuilds chunks the pending list references.
    if ((uint32_t)ctx->r4 == 0x8011A420u) {
        const uint32_t mp = (uint32_t)ctx->r5;
        if (mp >= 0x80000000u && mp < 0x80800000u && rdram[(mp & 0x7FFFFFu) ^ 3] == 1u) {
            rs64_gfx_task_submitted();
            static const bool s_lg = env_on("ROGUESQ_LOG_GFX_TASK");
            static unsigned s_n = 0;
            if (s_lg && ++s_n <= 6) { fprintf(stderr, "[gfx-request #%u] ra=0x%08X\n", s_n, (uint32_t)ctx->r31); fflush(stderr); }
        }
    }
    ctx->r2 = osSendMesg(rdram, (int32_t)ctx->r4, (OSMesg)ctx->r5, (s32)ctx->r6);
}

// ---- RSP tasks ----

// ROGUESQ_CHECK_CHUNKLIST=1: walk the DL chunk free list (head 0x801163B0; word 0 next, word 1 prev)
// at every SP task start, on the N64 thread so nothing mutates it concurrently. The first bad node
// dumps RDRAM to ROGUESQ_DUMP_RDRAM_PATH (default chunklist_corrupt.bin).
static void rs64_check_chunk_freelist(uint8_t* rdram) {
    static const bool s_chk = env_on("ROGUESQ_CHECK_CHUNKLIST");
    static bool s_dumped = false;
    if (!s_chk || s_dumped) return;
    auto rw = [&](uint32_t off) { return *reinterpret_cast<const uint32_t*>(rdram + (off & 0x7FFFFCu)); };
    uint32_t node = rw(0x1163B0), prev = 0; int steps = 0; const char* why = nullptr;
    while (node != 0 && steps < 8192) {
        const uint32_t off = node & 0x00FFFFFFu;
        if ((node >> 24) != 0x80u || off < 0x400000u || off + 0x108u > 0x800000u) { why = "node outside RDRAM chunk range"; break; }
        if (steps > 0 && rw(off + 4) != prev) { why = "prev link mismatch"; break; }
        prev = node; node = rw(off); ++steps;
    }
    if (steps >= 8192) why = "list longer than 8192 (cycle)";
    { static int s_n = 0;
      if ((++s_n & 15) == 1) { fprintf(stderr, "[chunklist] task %d free=%d\n", s_n, steps); fflush(stderr); } }
    if (!why) return;
    s_dumped = true;
    fprintf(stderr, "[chunklist] CORRUPT at task start: %s: prev=0x%08X bad=0x%08X steps=%d\n", why, prev, node, steps);
    const char* path = env_str("ROGUESQ_DUMP_RDRAM_PATH");
    FILE* f = recomp::os::fopen(path ? path : "chunklist_corrupt.bin", "wb");
    if (f) {
        static uint8_t buf[0x10000];
        for (uint32_t base = 0; base < 0x800000u; base += sizeof buf) {
            for (uint32_t i = 0; i < sizeof buf; ++i) buf[i] = rdram[(base + i) ^ 3];
            fwrite(buf, 1, sizeof buf, f);
        }
        fclose(f);
        fprintf(stderr, "[chunklist] dumped RDRAM\n");
    }
    fflush(stderr);
}

// ROGUESQ_LOG_TASKSUBMIT=1: every GFX task plus a sample of audio tasks, with the first DL bytes.
// Factor 5 ignores data_size (the DL chains through G_DL), so data_size=0 is normal.
extern "C" void osSpTaskStartGo_recomp(uint8_t* rdram, recomp_context* ctx) {
    rs64_check_chunk_freelist(rdram);
    static const bool s_log = env_on("ROGUESQ_LOG_TASKSUBMIT");
    if (s_log) {
        OSTask* task = TO_PTR(OSTask, ctx->r4);
        static int n = 0;
        ++n;
        if (n <= 4 || (n & 63) == 0 || task->t.type == 1u) {
            fprintf(stderr, "[task-submit #%d] type=0x%X flags=0x%X data_ptr=0x%08X data_size=%u ucode=0x%08X ucode_size=%u\n",
                    n, (unsigned)task->t.type, (unsigned)task->t.flags, (unsigned)task->t.data_ptr,
                    (unsigned)task->t.data_size, (unsigned)task->t.ucode, (unsigned)task->t.ucode_size);
            uint32_t dp = (uint32_t)task->t.data_ptr;
            if (task->t.type == M_GFXTASK && dp >= 0x80000000u && dp < 0x80800000u) {
                uint32_t off = dp - 0x80000000u;
                fprintf(stderr, "  data_ptr[0..32]:");
                for (int i = 0; i < 32; ++i) fprintf(stderr, " %02X", (unsigned)rdram[(off + i) ^ 3]);
                fprintf(stderr, "\n");
            }
            fflush(stderr);
        }
    }
    ultramodern::submit_rsp_task(rdram, ctx->r4);
}
