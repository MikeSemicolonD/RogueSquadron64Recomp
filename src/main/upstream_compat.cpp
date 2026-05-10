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
#endif

#include "recomp.h"
#include "librecomp/helpers.hpp"
#include "ultramodern/ultramodern.hpp"

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

// Diagnostic: log osViSwapBuffer calls. The game tells VI which fb to display
// via this call. If the address doesn't match any prior SET_COLOR_IMAGE
// address, RT64 has no rendered content for it → black screen.
extern "C" void osViSwapBuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    static int s_count = 0;
    int n = ++s_count;
    if (n <= 16 || (n & 63) == 0) {
        fprintf(stderr, "[osViSwapBuffer #%d] fb=0x%08X\n",
                n, (uint32_t)ctx->r4);
        fflush(stderr);
    }
    extern void osViSwapBuffer(uint8_t* rdram, int32_t frameBufPtr);
    osViSwapBuffer(rdram, (int32_t)ctx->r4);
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

