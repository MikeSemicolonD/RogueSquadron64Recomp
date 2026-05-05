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
    }

    (void)rdram;
    ultramodern::submit_rdp_range(submit_lo, submit_hi);
}
