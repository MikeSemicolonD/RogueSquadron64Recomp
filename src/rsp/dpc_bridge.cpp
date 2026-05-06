// DPC bridge for RSP graphics ucodes that emit RDP commands directly via
// mtc0 to DPC_START/DPC_END (Factor5 ucode does this). On DPC_END writes
// we forward [start..end) RDRAM bytes to RT64's RDP interpreter (LLE path,
// processDisplayLists with isHLE=false), matching what RT64 would consume
// from raw RDP command bytes.
//
// Reads of DPC_CURRENT/DPC_END return g_rsp_dpc_end so busy-wait loops in
// the ucode see "RDP completed instantly" and exit.

#include <cstdint>
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
