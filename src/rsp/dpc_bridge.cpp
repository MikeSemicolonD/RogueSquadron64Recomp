// DPC bridge for RSP graphics ucodes that emit RDP commands directly via
// mtc0 to DPC_START/DPC_END (Factor5 ucode does this). On DPC_END writes
// we forward [start..end) RDRAM bytes to RT64's RDP interpreter (LLE path,
// processDisplayLists with isHLE=false), matching what RT64 would consume
// from raw RDP command bytes.
//
// Reads of DPC_CURRENT/DPC_END return g_rsp_dpc_end so busy-wait loops in
// the ucode see "RDP completed instantly" and exit.

#include <cstdint>
#include <cstdlib>
#include <atomic>
#include "librecomp/rsp.hpp"
#include "ultramodern/events.hpp"

uint32_t g_rsp_dpc_start = 0;
uint32_t g_rsp_dpc_end   = 0;

// Set when an RDP FULL_SYNC (op 0x29) is observed in a submission. The
// factor5_ucode dispatch loop polls this and force-returns Broke so
// task_thread_func can fire sp_complete() and the CPU's GFX_SCHED can
// advance to the next task. Without this, the LLE ucode runs forever
// (the original game halts the RSP externally between frames; we don't
// model that).
std::atomic<bool> g_rsp_full_sync_seen{false};

// RDRAM-relative address of the most recent real FULL_SYNC byte (op 0x29)
// the bridge forwarded. rsp_force_fullsync() re-submits those 8 bytes via
// ultramodern::submit_rdp_range so RT64 sees a fullSync at task-end. Paired
// with the LoadOperation validity check in rt64_state.cpp:fullSyncFramebufferPairTiles
// — without that check, mid-frame fullSync crashes in loadTileOperation.
static std::atomic<uint32_t> g_last_fullsync_addr{0xFFFFFFFFu};

// Per-task RDP byte / command counters. The ucode resets these via
// rsp_task_log_and_reset() at the synthetic-halt site so we get a per-task
// chunk-size distribution.
static std::atomic<uint32_t> g_task_rdp_bytes{0};
static std::atomic<uint32_t> g_task_rdp_cmds{0};
static std::atomic<uint32_t> g_task_rdp_fullsyncs{0};
// Per-task RDP opcode histogram (low 6 bits of first byte = op). Bridge and
// task_log_and_reset both run on the RSP task thread, so plain array is fine.
static uint32_t g_task_op_count[64] = {0};

void rsp_dpc_submit(uint8_t* rdram, uint32_t start, uint32_t end) {
    if (end <= start) {
        return;
    }

    // RT64 expects RDRAM-relative addresses. Factor5 writes KSEG0 (0x80xxxxxx)
    // to DPC_START/END — mask to 24 bits.
    uint32_t start_phys = start & 0x3FFFFFF;
    uint32_t end_phys   = end   & 0x3FFFFFF;

    // The ucode emits one RDP command at a time and bumps DPC_END after each.
    // On real hardware, RDP processes [DPC_CURRENT..DPC_END) once and advances
    // CURRENT. We mirror that: only forward bytes the RT64 RDP hasn't seen yet.
    // Reset on a new DPC_START (KSEG0 base address change).
    static uint32_t s_dl_base = 0;
    static uint32_t s_last_end = 0;

    if (start_phys != s_dl_base) {
        s_dl_base = start_phys;
        s_last_end = start_phys;
    }

    if (end_phys <= s_last_end) {
        return;
    }

    uint32_t submit_lo = s_last_end;
    uint32_t submit_hi = end_phys;
    s_last_end = end_phys;

    g_task_rdp_bytes.fetch_add(submit_hi - submit_lo, std::memory_order_relaxed);
    g_task_rdp_cmds.fetch_add(1, std::memory_order_relaxed);
    {
        int64_t mips_first = (int64_t)(int32_t)(submit_lo + 0x80000000);
        uint8_t op = (uint8_t)MEM_B(0, mips_first) & 0x3F;
        g_task_op_count[op]++;
    }

    // PIPESYNC FILTER: Factor5 emits PIPESYNC (op_int 0x27) very heavily.
    // RT64 implicitly maintains pipeline coherence; per-cmd PIPESYNC floods
    // the action_queue. Keep LOADSYNC (0x26), TILESYNC (0x28), FULLSYNC (0x29).
    if ((submit_hi - submit_lo) == 8) {
        int64_t mips = (int64_t)(int32_t)(submit_lo + 0x80000000);
        uint8_t b0 = MEM_B(0, mips);
        if ((b0 & 0x3F) == 0x27) {
            (void)rdram;
            return;
        }
        if ((b0 & 0x3F) == 0x29) {
            // FULL_SYNC marks the end of a frame's RDP work — on real hw
            // it raises DP interrupt and the CPU halts the RSP. We don't
            // halt externally, so signal the ucode dispatcher to break out.
            g_rsp_full_sync_seen.store(true, std::memory_order_release);
            g_task_rdp_fullsyncs.fetch_add(1, std::memory_order_relaxed);
            g_last_fullsync_addr.store(submit_lo, std::memory_order_release);
            // Count FULL_SYNC bytes submitted. Compare to State::fullSync
            // count to see if RT64 is observing every one we send.
            static std::atomic<uint64_t> s_fs{0};
            uint64_t n = ++s_fs;
            if (n <= 8 || (n & 31) == 0) {
                fprintf(stderr, "[dpc] FULL_SYNC byte sent #%llu\n",
                    (unsigned long long)n);
                fflush(stderr);
            }
        }
        // SET_COLOR/DEPTH/TEXTURE_IMAGE tracer. Confirmed Factor5 LLE cycles a
        // small (A,B,C) tuple set forever; addFramebufferPair now dedupes
        // search-all by tuple, so per-call printing is no longer useful. Keep
        // the first few prints as a smoke test that the byte stream still has
        // the expected shape.
        uint8_t op6 = b0 & 0x3F;
        if (op6 == 0x3D || op6 == 0x3E || op6 == 0x3F) {
            static std::atomic<uint64_t> s_seen{0};
            uint64_t n = ++s_seen;
            if (n <= 8) {
                uint32_t w0 = ((uint32_t)MEM_B(0, mips) << 24) |
                              ((uint32_t)(uint8_t)MEM_B(1, mips) << 16) |
                              ((uint32_t)(uint8_t)MEM_B(2, mips) <<  8) |
                              ((uint32_t)(uint8_t)MEM_B(3, mips));
                uint32_t w1 = ((uint32_t)(uint8_t)MEM_B(4, mips) << 24) |
                              ((uint32_t)(uint8_t)MEM_B(5, mips) << 16) |
                              ((uint32_t)(uint8_t)MEM_B(6, mips) <<  8) |
                              ((uint32_t)(uint8_t)MEM_B(7, mips));
                const char *name = (op6 == 0x3F) ? "SET_COLOR_IMAGE" :
                                   (op6 == 0x3E) ? "SET_DEPTH_IMAGE" : "SET_TEXTURE_IMAGE";
                fprintf(stderr, "[dpc] %s #%llu w0=0x%08X w1=0x%08X (addr=0x%08X)\n",
                    name, (unsigned long long)n, w0, w1, w1 & 0x00FFFFFF);
                fflush(stderr);
            }
        }
    }

    (void)rdram;
    ultramodern::submit_rdp_range(submit_lo, submit_hi);
}

// Called from the factor5_ucode synthetic-halt site. Logs per-task RDP byte
// volume / command count / FULL_SYNC count, then resets the counters for the
// next task. Throttled to first 16 tasks + every 256 thereafter so we get the
// distribution without flooding the SDL message pump.
extern "C" void rsp_task_log_and_reset(uint32_t iters, uint32_t data_size, uint32_t r17,
                                        uint32_t cmd_w0, uint32_t cmd_w1) {
    uint32_t bytes = g_task_rdp_bytes.exchange(0, std::memory_order_acq_rel);
    uint32_t cmds  = g_task_rdp_cmds.exchange(0, std::memory_order_acq_rel);
    uint32_t fs    = g_task_rdp_fullsyncs.exchange(0, std::memory_order_acq_rel);
    static std::atomic<uint64_t> s_n{0};
    uint64_t n = ++s_n;
    bool capped_no_fs = (iters > 16000 && fs == 0);
    // Throttle CAPPED-NO-FS spam — at 13/s with histogram dump it's ~117
    // lines/sec, enough to starve the SDL pump and stall GFX scheduling.
    static std::atomic<uint64_t> s_capped_count{0};
    bool log_capped = false;
    if (capped_no_fs) {
        uint64_t cn = ++s_capped_count;
        log_capped = (cn <= 8 || (cn & 4095) == 0);
    }
    if (n <= 16 || (n & 255) == 0 || log_capped) {
        fprintf(stderr,
            "[task] #%llu iters=%u data_size=%u r17=0x%X cmd=[%08X %08X] rdp_bytes=%u cmds=%u fullsyncs=%u%s\n",
            (unsigned long long)n, iters, data_size, r17, cmd_w0, cmd_w1,
            bytes, cmds, fs,
            capped_no_fs ? " CAPPED-NO-FS" : "");
        fflush(stderr);
        // For cap-without-fullSync tasks, dump the opcode histogram (top 8).
        // Helps identify the loop: if one opcode dominates, that's the stuck
        // command. Reset is unconditional below.
        if (log_capped) {
            uint32_t copy[64];
            for (int i = 0; i < 64; i++) copy[i] = g_task_op_count[i];
            // Find top 8 by count.
            for (int slot = 0; slot < 8; slot++) {
                int max_idx = 0;
                for (int i = 0; i < 64; i++) {
                    if (copy[i] > copy[max_idx]) max_idx = i;
                }
                if (copy[max_idx] == 0) break;
                fprintf(stderr, "  [task#%llu hist] op 0x%02X = %u\n",
                    (unsigned long long)n, (unsigned)max_idx, copy[max_idx]);
                copy[max_idx] = 0;
            }
            fflush(stderr);
        }
    }
    for (int i = 0; i < 64; i++) g_task_op_count[i] = 0;
}

// Submit a synthetic FULL_SYNC by re-using the RDRAM bytes of the most recent
// real FULL_SYNC. Called from factor5_ucode at synthetic-halt for tasks that
// don't naturally emit op 0x29 (cinematic phase). The action_queue is FIFO,
// so all prior submit_rdp_range calls process first; this fullSync runs after
// state has all the task's geometry. Paired with the LoadOperation validity
// check in rt64_state.cpp so partial tile state is skipped, not crashed.
//
// Env var ROGUESQ_NO_SYNTH_FULLSYNC=1 disables this and reverts to the slow
// "wait for real FULL_SYNC bytes" path (~5 fps cinematic) — useful for A/B
// comparison against the higher-rate-but-stalls-at-60s default behavior.
extern "C" void rsp_force_fullsync() {
    static const bool disabled = []() {
        const char *e = std::getenv("ROGUESQ_NO_SYNTH_FULLSYNC");
        bool d = (e != nullptr && *e != '\0' && *e != '0');
        if (d) {
            fprintf(stderr, "[dpc] synthetic FULL_SYNC injection DISABLED via env\n");
            fflush(stderr);
        }
        return d;
    }();
    if (disabled) return;
    uint32_t addr = g_last_fullsync_addr.load(std::memory_order_acquire);
    if (addr == 0xFFFFFFFFu) {
        return;
    }
    ultramodern::submit_rdp_range(addr, addr + 8);
}
