#include "main/debug_pollers.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include "os_compat.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else

static inline uint64_t GetTickCount64()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

#endif

extern "C" volatile uint8_t* volatile g_recomp_rdram_for_wp_raw;

// Big-endian RDRAM accessors (RDRAM is host-LE-stored; XOR-3 swizzles byte order).
static inline uint32_t rdram_be32(const uint8_t* rdram, uint32_t off)
{

    return (uint32_t(rdram[(off + 0) ^ 3]) << 24) | (uint32_t(rdram[(off + 1) ^ 3]) << 16) |
           (uint32_t(rdram[(off + 2) ^ 3]) <<  8) |  uint32_t(rdram[(off + 3) ^ 3]);
}
static inline uint16_t rdram_be16(const uint8_t* rdram, uint32_t off)
{
    return (uint16_t)((uint16_t(rdram[(off + 0) ^ 3]) << 8) | uint16_t(rdram[(off + 1) ^ 3]));
}

namespace rs64::diag {
// Periodic poll of the scene-state struct at D_80130B10 + the per-frame
// callback array at D_8011A8A4. Both regions are documented in
// `project_thread_topology_2026_05_09.md`. Gated by ROGUESQ_LOG_STATE=1.
//
// Reports the first dump immediately, then logs changes only. The state byte
// at +0x14 of D_80130B10 indexes the level/scene jump-table at jtbl_8003A3E8.
// The 4 callback slots at D_8011A8A4 are the per-frame draw callbacks the
// main game thread iterates each frame.
void start_state_poller() {

    if (!recomp::os::getenv("ROGUESQ_LOG_STATE")) return;

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
                }
            }

            // Compute first-N active flags if array_base is a sane KSEG0 ptr
            uint8_t flags[8] = {0};
            uint32_t array_off = array_base & 0x00FFFFFFu;
            int n = c1ad0 < 8 ? c1ad0 : 8;

            if ((array_base & 0xF0000000u) == 0x80000000u && array_off + n * 0x88 < 0x800000u)
            {
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

            if (changed)
            {
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

// Periodic poll of cinematic phase state so we can see stage transitions
// and timing. Gated by ROGUESQ_LOG_PHASE=1. Reports on change only.
//
// State variables (per memory notes project_cinematic_data_structures.md
// + project_b50_b58_polled_state.md):
//   cineState  : MEM_W[0x800B0934] — master cinematic state word
//   gateCtr    : MEM_W[0x800B0B28] — cinematic frame counter (compared
//                vs cutscene[0x44])
//   cutscene   : MEM_W[0x800B1904] — pointer to currently-loaded cutscene
//                struct
//   cuts_44    : MEM_W[cutscene+0x44] — total-count / duration field
//   B50        : MEM_W[0x80130B50] — state word, bit 5 is inner-loop
//                advance gate
//   B58        : MEM_W[0x80130B58] — state word, bit 25 is outer gate
//   slot_dispatcher_ptr : MEM_W[0x80130BB0] — pointer to 6-slot table
//                (each slot is 8B, jalr handler at slot+0x00)
//   active_slot_indices : 6 halfwords at 0x80139560
void start_phase_poller() {

    if (!recomp::os::getenv("ROGUESQ_LOG_PHASE")) return;

    static std::thread t{[]{
        uint8_t* rdram = nullptr;

        while (!(rdram = (uint8_t*)g_recomp_rdram_for_wp_raw)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        uint32_t prev_cineState = 0xFFFFFFFFu;
        uint32_t prev_gateCtr   = 0xFFFFFFFFu;
        uint32_t prev_cutscene  = 0xFFFFFFFFu;
        uint32_t prev_cuts44    = 0xFFFFFFFFu;
        uint32_t prev_b50       = 0xFFFFFFFFu;
        uint32_t prev_b58       = 0xFFFFFFFFu;
        uint32_t prev_slot_tbl  = 0xFFFFFFFFu;
        uint32_t prev_cur_ovl   = 0xFFFFFFFFu;
        uint32_t prev_star_word = 0xFFFFFFFFu;
        uint32_t prev_cb0 = 0xDEADBEEFu, prev_cb1 = 0xDEADBEEFu;
        uint32_t prev_cb2 = 0xDEADBEEFu, prev_cb3 = 0xDEADBEEFu;
        uint16_t prev_active[6] = {0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu};
        uint64_t start_ms = GetTickCount64();
        uint64_t last_change_ms = start_ms;
        bool first = true;

        auto rdram_be32_at = [&](uint32_t off) -> uint32_t { return rdram_be32(rdram, off); };
        auto rdram_be16_at = [&](uint32_t off) -> uint16_t { return rdram_be16(rdram, off); };

        for (int i = 0; i < 1200; ++i) {  // ~60s @ 50ms
            uint32_t cur_ovl   = rdram_be32_at(0x375B0);  // gCurrentLoadedOverlay

            // Probe first 4 bytes of STAR WARS string at menu_overlay 0xA5E60
            // — if menu overlay is loaded, these should spell 'STAR' (0x53 54 41 52).
            uint32_t star_word = rdram_be32_at(0xA5E60);

            // Per-frame draw callbacks at 0x8011A8A4 (4 slots, fn ptrs)
            uint32_t cb0 = rdram_be32_at(0x11A8A4);
            uint32_t cb1 = rdram_be32_at(0x11A8A8);
            uint32_t cb2 = rdram_be32_at(0x11A8AC);
            uint32_t cb3 = rdram_be32_at(0x11A8B0);
            uint32_t cineState = rdram_be32_at(0xB0934);
            uint32_t gateCtr   = rdram_be32_at(0xB0B28);
            uint32_t cutscene  = rdram_be32_at(0xB1904);
            uint32_t cuts44 = 0;

            if ((cutscene & 0xF0000000u) == 0x80000000u &&
                (cutscene & 0x00FFFFFFu) + 0x44 < 0x800000u)
            {
                cuts44 = rdram_be32_at((cutscene & 0x00FFFFFFu) + 0x44);
            }

            uint32_t b50      = rdram_be32_at(0x130B50);
            uint32_t b58      = rdram_be32_at(0x130B58);
            uint32_t slot_tbl = rdram_be32_at(0x130BB0);
            uint16_t active[6];

            for (int j = 0; j < 6; ++j) active[j] = rdram_be16_at(0x139560 + j * 2);

            bool changed = first;

            if (cineState != prev_cineState) changed = true;
            if (gateCtr   != prev_gateCtr)   changed = true;
            if (cutscene  != prev_cutscene)  changed = true;
            if (cuts44    != prev_cuts44)    changed = true;
            if (b50       != prev_b50)       changed = true;
            if (b58       != prev_b58)       changed = true;
            if (slot_tbl  != prev_slot_tbl)  changed = true;
            if (cur_ovl   != prev_cur_ovl)   changed = true;
            if (star_word != prev_star_word) changed = true;
            if (cb0 != prev_cb0 || cb1 != prev_cb1 || cb2 != prev_cb2 || cb3 != prev_cb3) changed = true;

            for (int j = 0; j < 6; ++j) {
                if (active[j] != prev_active[j]) { changed = true; break; }
            }

            if (changed)
            {
                uint64_t now = GetTickCount64();
                uint64_t since_start = now - start_ms;
                uint64_t stable = now - last_change_ms;

                // Decode star_word as 4-char ASCII for at-a-glance
                // "is the menu overlay's STAR WARS string here" check.
                char sw[5];
                sw[0]=(char)(star_word>>24);
                sw[1]=(char)(star_word>>16);
                sw[2]=(char)(star_word>>8);
                sw[3]=(char)star_word;
                sw[4]=0;

                for (int k=0;k<4;k++) if (sw[k]<0x20||sw[k]>0x7E) sw[k]='.';

                fprintf(stderr,
                    "[phase] t=%5llums (stable %4llums)  cineState=%08X gateCtr=%5u "
                    "cuts=%08X cuts44=%5u B50=%08X B58=%08X slotTbl=%08X "
                    "active=[%04X %04X %04X %04X %04X %04X] curOvl=%u starWord=0x%08X(%s) "
                    "cb=[%08X %08X %08X %08X]\n",
                    (unsigned long long)since_start, (unsigned long long)stable,
                    cineState, gateCtr,
                    cutscene, cuts44,
                    b50, b58,
                    slot_tbl,
                    active[0], active[1], active[2], active[3], active[4], active[5],
                    cur_ovl, star_word, sw,
                    cb0, cb1, cb2, cb3);
                fflush(stderr);

                // When slotTbl becomes valid, dump non-empty slot entries
                // so we can see which NPCs are populated. Compare to real
                // N64 RAM dumps (e.g. lucasArtsScreen has handlers in
                // 0x80206000-0x80209000 range; N64-logo state is different).
                if ((slot_tbl & 0xF0000000u) == 0x80000000u && slot_tbl != prev_slot_tbl)
                {
                    uint32_t pool_off = slot_tbl & 0x00FFFFFFu;
                    int populated = 0;

                    fprintf(stderr, "[phase] slot pool @ 0x%08X populated entries:\n", slot_tbl);

                    for (int s = 0; s < 2048 && populated < 40; ++s) {
                        uint32_t entry_off = pool_off + (uint32_t)s * 8;

                        if (entry_off + 8 > 0x800000u) break;

                        uint32_t h = rdram_be32_at(entry_off);
                        uint32_t w = rdram_be32_at(entry_off + 4);

                        if (h != 0 && h != 0xFFFFFFFFu)
                        {
                            fprintf(stderr, "  slot[%4d]: handler=0x%08X  word2=0x%08X\n", s, h, w);
                            ++populated;
                        }
                    }

                    fprintf(stderr, "[phase] (showing first %d populated of pool 2048)\n", populated);
                    fflush(stderr);
                }

                first = false;
                prev_cineState = cineState;
                prev_gateCtr   = gateCtr;
                prev_cutscene  = cutscene;
                prev_cuts44    = cuts44;
                prev_b50       = b50;
                prev_b58       = b58;
                prev_slot_tbl  = slot_tbl;
                prev_cur_ovl   = cur_ovl;
                prev_star_word = star_word;
                prev_cb0 = cb0;
                prev_cb1 = cb1;
                prev_cb2 = cb2;
                prev_cb3 = cb3;

                for (int j = 0; j < 6; ++j) prev_active[j] = active[j];

                last_change_ms = now;
            }

            // Periodic slot-pool snapshot every ~1 second so we can see if
            // NPCs spawn over time (real N64 attribution has 30+ populated;
            // if we stay at 4, something's stopping further spawns).
            if ((i % 20) == 0 && (slot_tbl & 0xF0000000u) == 0x80000000u) {
                uint32_t pool_off = slot_tbl & 0x00FFFFFFu;
                int total_pop = 0;

                for (int s = 0; s < 2048; ++s) {
                    uint32_t entry_off = pool_off + (uint32_t)s * 8;

                    if (entry_off + 8 > 0x800000u) break;

                    uint32_t h = rdram_be32_at(entry_off);

                    if (h != 0 && h != 0xFFFFFFFFu) ++total_pop;
                }

                fprintf(stderr, "[phase] t=%5llums slot-pool count = %d\n",
                        (unsigned long long)(GetTickCount64() - start_ms), total_pop);
                fflush(stderr);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }};
    t.detach();
}
}
