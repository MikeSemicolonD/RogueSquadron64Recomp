// DPC bridge for RSP graphics ucodes that emit RDP commands directly via
// mtc0 to DPC_START/DPC_END (Factor5 ucode does this). On DPC_END writes
// we forward [start..end) RDRAM bytes to RT64's RDP interpreter (LLE path,
// processDisplayLists with isHLE=false), matching what RT64 would consume
// from raw RDP command bytes.
//
// Reads of DPC_CURRENT/DPC_END return g_rsp_dpc_end so busy-wait loops in
// the ucode see "RDP completed instantly" and exit.

#include <cstdint>
#include <cstdio>
#include <atomic>
#include "librecomp/rsp.hpp"
#include "ultramodern/events.hpp"

uint32_t g_rsp_dpc_start = 0;
uint32_t g_rsp_dpc_end   = 0;

void rsp_dpc_submit(uint8_t* rdram, uint32_t start, uint32_t end) {
    static std::atomic<uint64_t> n{0};
    uint64_t cur = ++n;

    if (end <= start) {
        if (cur <= 5) {
            fprintf(stderr, "[dpc] submit #%llu skipped (empty range start=0x%08X end=0x%08X)\n",
                    (unsigned long long)cur, start, end);
            fflush(stderr);
        }
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
    static uint32_t s_dl_base = 0;       // last DPC_START we observed (phys)
    static uint32_t s_last_end = 0;      // last end we forwarded to RT64 (phys)

    if (start_phys != s_dl_base) {
        // New DPC_START — start of a new RDP command stream.
        s_dl_base = start_phys;
        s_last_end = start_phys;
    }

    if (end_phys <= s_last_end) {
        return;  // nothing new
    }

    uint32_t submit_lo = s_last_end;
    uint32_t submit_hi = end_phys;
    s_last_end = end_phys;

    if (cur <= 20 || (cur % 200) == 0) {
        fprintf(stderr, "[dpc] submit #%llu lo=0x%08X hi=0x%08X delta=%u (→ gfx queue)\n",
                (unsigned long long)cur, submit_lo, submit_hi, submit_hi - submit_lo);
        fflush(stderr);
    }

    // PAK-SCREEN CAPTURE: dump first 500 + chunks 50000-50500 (after Pak
    // model has loaded) to inspect the byte stream factor5_ucode emits.
    bool capture_now = (cur <= 500) || (cur >= 50000 && cur <= 50500);
    if (capture_now) {
        FILE* fp = fopen("pak_rdp_emit.bin", (cur == 1) ? "wb" : "ab");
        if (fp) {
            uint32_t len = submit_hi - submit_lo;
            // Tag each chunk with a 16-byte header so we can demux later:
            //   [0..3]   submit ordinal (LE)
            //   [4..7]   submit_lo phys (LE)
            //   [8..11]  submit_hi phys (LE)
            //   [12..15] length (LE)
            uint32_t hdr[4] = { (uint32_t)cur, submit_lo, submit_hi, len };
            fwrite(hdr, 4, 4, fp);
            for (uint32_t i = 0; i < len; ++i) {
                int64_t mips = (int64_t)(int32_t)(submit_lo + i + 0x80000000);
                uint8_t b = MEM_B(0, mips);
                fwrite(&b, 1, 1, fp);
            }
            fclose(fp);
        }
    }

    // Enqueue onto the same action_queue HLE uses, so RT64 sees this submission
    // on the gfx thread (no shared-state races with send_dl).
    (void)rdram;
    ultramodern::submit_rdp_range(submit_lo, submit_hi);
}
