#include "video/render_diag.h"
#include <algorithm>
#include <memory>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <thread>
#include <mutex>
#include <vector>
#include <filesystem>
#include <fstream>
#include "main/renderdoc_capture.h"
#include "SDL.h"
#ifdef __ANDROID__
#include "SDL_syswm.h"
extern SDL_Window* g_sdl_window;
#endif
#include "common/rt64_user_configuration.h"
#include "video/video_config.h"
#include "video/rt64_render_context.h"

#ifndef HLSL_CPU
#define HLSL_CPU
#endif
#include "hle/rt64_application.h"
#include "hle/rt64_rs64_lights.h"
#include "hle/rt64_rs64_transition.h"
#include "librecomp/mods.hpp"

#include "ultramodern/ultramodern.hpp"
#include "ultramodern/renderer_context.hpp"
#include "util/debug_logs.h"
#include "rs64_toml_hooks.h"
#include "game/upstream_compat.h"      // rs64_fb_guards_mask, g_active_overlay, g_last_swap_fb
#include "game/hook_helpers.h"         // g_boot_pulse_start, g_current_scene, rs64_cine_iter_get, rs64_neutralize_matpool_cimg
#include "game/game_state.h"           // rs64_state_poll, rs64_state_current_id
#include "game/nav_sequencer.h"        // rs64_nav_set_target, rs64_nav_tick
#include "main/main.h"                // rs64_reloc_guard_tick

using recomp::dbg::env_str;
using recomp::dbg::env_on;
using recomp::dbg::env_int;
using recomp::dbg::env_u32;

extern "C" volatile unsigned g_f5_heur[4];

#ifdef RS64_MCP
extern "C" void rs64_mcp_note_dl();
#endif

#ifdef _WIN32

extern "C" void rs64_data_bp_arm(void* host_addr);
extern "C" void rs64_data_bp_set2(void* host_addr);
extern "C" void rs64_data_bp_drain(int vi);

#endif

// src/main/main.cpp
extern void print_stack_with_symbols(void** frames, unsigned short count);

// ultramodern events.cpp
extern "C" uint8_t* g_rs64_parse_rdram;             // RDRAM snapshot for the current parse

// lib/rt64 F5 GBI
extern "C" volatile unsigned g_most_drawn_fb;        // most-drawn color image and its width
extern "C" volatile unsigned g_most_drawn_fb_width;
extern "C" volatile unsigned long long g_most_drawn_fb_ms;
extern "C" volatile uint32_t g_rs64_clear_seq;          // mode-change clear count and size (hook_helpers.cpp)
extern "C" volatile uint32_t g_rs64_clear_bytes;

extern "C" volatile long long g_rs64_pdl_us_total;
extern "C" volatile long long g_rs64_pdl_task_total;
extern "C" volatile long long g_rs64_pdl_us;
extern "C" volatile long long g_rs64_snap_us;

namespace rs64::video {

uint8_t RenderDiag::rd8(uint32_t addr) const {

    if (!app->core.RDRAM || addr >= 0x01000000) return 0;

    return app->core.RDRAM[addr ^ 3];
}
uint32_t RenderDiag::rd32(uint32_t addr) const
{

    return (uint32_t(rd8(addr)) << 24) | (uint32_t(rd8(addr + 1)) << 16) |
           (uint32_t(rd8(addr + 2)) << 8) | uint32_t(rd8(addr + 3));
}
void RenderDiag::wr8(uint32_t addr, uint8_t v)
{
    if (app->core.RDRAM && addr < 0x01000000) app->core.RDRAM[addr ^ 3] = v;
}
void RenderDiag::wr32(uint32_t addr, uint32_t v) {

    wr8(addr, uint8_t(v >> 24)); wr8(addr + 1, uint8_t(v >> 16));
    wr8(addr + 2, uint8_t(v >> 8)); wr8(addr + 3, uint8_t(v));
}

int RenderDiag::workload_cursor() const { return app->workloadQueue ? (int)app->workloadQueue->writeCursor : -1; }
int RenderDiag::present_cursor() const { return app->presentQueue ? (int)app->presentQueue->writeCursor : -1; }

// ROGUESQ_LOG_FRAME_PROFILE=1: once/sec, attribute frame time to its phases so a busy-scene
// drop can be classified CPU-bound (DL walk/submit), GPU/present-bound, or sim/pacing-bound.
//   present : actual presents/sec (the real framerate)
//   period  : wall time between presents, ms (min/avg/max) -- spikes = the drops
//   dl      : processDisplayLists us/present (CPU DL translate+submit) + tasks/present
//   present : app->updateScreen() us (submit + any GPU/vsync wait)
//   other   : period - dl - present = game-thread sim + scheduler + idle
void RenderDiag::frame_profile(std::chrono::high_resolution_clock::time_point pres0)
{

    static const bool s_on = env_on("ROGUESQ_LOG_FRAME_PROFILE", false);

    if (!s_on) return;

    const auto now = std::chrono::high_resolution_clock::now();
    const long long pres_us =
        std::chrono::duration_cast<std::chrono::microseconds>(now - pres0).count();

    static bool s_have_prev = false;
    static std::chrono::high_resolution_clock::time_point s_prev, s_win0;
    static long long s_pdl_base = 0, s_task_base = 0;
    static long long s_period_min = 0, s_period_max = 0, s_period_sum = 0;
    static long long s_pres_sum = 0, s_pres_max = 0;
    static int s_frames = 0;

    if (!s_have_prev) {
        s_have_prev = true;
        s_prev = now;
        s_win0 = now;
        s_pdl_base = g_rs64_pdl_us_total;
        s_task_base = g_rs64_pdl_task_total;
        s_period_min = s_period_max = s_period_sum = 0;
        s_frames = 0;
        s_pres_sum = s_pres_max = 0;

        return;
    }

    const long long period_us =
        std::chrono::duration_cast<std::chrono::microseconds>(now - s_prev).count();
    s_prev = now;

    // Per-hitch attribution: on a long present interval, dump the most-recent walk + snapshot
    // costs so a 30-76ms spike can be routed to walk-bound / memcpy-bound / pacing-bound.
    //   walk_last : last F5 processDisplayLists (Gfx Thread) us -- includes any GPU-barrier wait
    //   snap_us   : last RDRAM snapshot memcpy (game thread) us
    //   present_us: this update_screen() us (submit + present wait)
    constexpr long long kHitchUs = 25000;

    if (period_us > kHitchUs)
    {
        fprintf(stderr,
            "[frameprof HITCH] period=%.1fms walk_last=%.1fms snap=%.1fms present=%.2fms\n",
            period_us / 1000.0, g_rs64_pdl_us / 1000.0, g_rs64_snap_us / 1000.0,
            pres_us / 1000.0);
        fflush(stderr);
    }

    if (s_frames == 0 || period_us < s_period_min) s_period_min = period_us;
    if (period_us > s_period_max) s_period_max = period_us;

    s_period_sum += period_us;
    s_pres_sum += pres_us;

    if (pres_us > s_pres_max) s_pres_max = pres_us;

    ++s_frames;

    const long long win_us =
        std::chrono::duration_cast<std::chrono::microseconds>(now - s_win0).count();

    if (win_us >= 1000000 && s_frames > 0)
    {
        const long long dl_us = g_rs64_pdl_us_total - s_pdl_base;
        const long long dl_tasks = g_rs64_pdl_task_total - s_task_base;
        const double fps = s_frames * 1e6 / (double)win_us;
        const double display_ms = rs64_rt64_present_ms();

        fprintf(stderr,
            "[frameprof] present=%.1f fps display=%.1f fps | period ms min/avg/max=%.1f/%.1f/%.1f | "
            "dl us/frame=%.0f tasks/frame=%.1f | present us avg/max=%.0f/%lld | other ms/frame=%.1f\n",
            fps, (display_ms > 0.0) ? 1000.0 / display_ms : 0.0,
            s_period_min / 1000.0, (s_period_sum / (double)s_frames) / 1000.0, s_period_max / 1000.0,
            dl_us / (double)s_frames, dl_tasks / (double)s_frames,
            s_pres_sum / (double)s_frames, s_pres_max,
            ((s_period_sum - dl_us - s_pres_sum) / (double)s_frames) / 1000.0);
        fflush(stderr);
        s_win0 = now;
        s_pdl_base = g_rs64_pdl_us_total;
        s_task_base = g_rs64_pdl_task_total;
        s_period_min = s_period_max = s_period_sum = 0;
        s_frames = 0;
        s_pres_sum = s_pres_max = 0;
    }
}

// ROGUESQ_WATCH_ADDRS=<addr>[,<addr>...]: log each listed RDRAM word when it changes, per VI.
void RenderDiag::watch_rdram() {

    static const char* s_spec = env_str("ROGUESQ_WATCH_ADDRS");

    if (!s_spec || !app->core.RDRAM) return;

    static uint32_t s_waddr[16], s_wlast[16];
    static int s_n = -1;

    if (s_n < 0) {
        s_n = 0;

        for (const char* p = s_spec; *p && s_n < 16; )
        {
            s_waddr[s_n] = (uint32_t)std::strtoul(p, (char**)&p, 16) & 0x00FFFFFCu;
            s_wlast[s_n] = rd32(s_waddr[s_n]);

            fprintf(stderr, "[watch] vi=#%d %08X = %08X (initial)\n", vi_count_, 0x80000000u | s_waddr[s_n], s_wlast[s_n]);
            ++s_n;

            if (*p == ',') ++p;
            else break;
        }
    }

    for (int i = 0; i < s_n; ++i)
    {
        const uint32_t v = rd32(s_waddr[i]);

        if (v == s_wlast[i]) continue;

        fprintf(stderr, "[watch] vi=#%d %08X %08X -> %08X\n", vi_count_, 0x80000000u | s_waddr[i], s_wlast[i], v);
        s_wlast[i] = v;
    }
}

// ROGUESQ_DATA_BP=<addr>: hardware write-watch on that word, armed at ROGUESQ_DATA_BP_ARM_VI.
void RenderDiag::data_bp_tick()
{

#ifdef _WIN32

    static const char* s_bp = env_str("ROGUESQ_DATA_BP");

    if (!s_bp || !app->core.RDRAM) return;

    static const int s_arm_vi = env_int("ROGUESQ_DATA_BP_ARM_VI", 1);
    static bool s_armed = false;

    // Re-arm every 30 VIs so game threads created after the first arm (per-screen workers) are watched too.
    if (vi_count_ >= s_arm_vi && (!s_armed || (vi_count_ % 30) == 0))
    {
        s_armed = true;
        const uint32_t a = (uint32_t)std::strtoul(s_bp, nullptr, 16) & 0x00FFFFFCu;

        // ROGUESQ_DATA_BP2=<addr>: a second watched word (DR1).
        if (const char* bp2 = env_str("ROGUESQ_DATA_BP2"))
        {
            rs64_data_bp_set2(app->core.RDRAM + ((uint32_t)std::strtoul(bp2, nullptr, 16) & 0x00FFFFFCu));
        }

        rs64_data_bp_arm(app->core.RDRAM + a);
    }

    if (s_armed) rs64_data_bp_drain(vi_count_);

#endif
}

// ROGUESQ_DUMP_UCODE=path: one-shot IMEM + DMEM dump for offline disassembly.
void RenderDiag::dump_ucode_once(const OSTask* task)
{

    static const char* s_path = env_str("ROGUESQ_DUMP_UCODE");
    static bool s_done = false;

    if (!s_path || s_done) return;

    s_done = true;
    const uint32_t ucode_phys = (uint32_t)task->t.ucode & 0x3FFFFFF;
    const uint32_t udata_phys = (uint32_t)task->t.ucode_data & 0x3FFFFFF;
    const uint32_t udata_size = task->t.ucode_data_size ? task->t.ucode_data_size : 0x800;

    auto write_region = [&](const char* suffix, uint32_t phys, uint32_t size) {

        char path[512];
        snprintf(path, sizeof path, "%s.%s.bin", s_path, suffix);
        FILE* f = recomp::os::fopen(path, "wb");

        if (!f) return;

        std::vector<uint8_t> buf(size);

        for (uint32_t i = 0; i < size; ++i) buf[i] = rd8(phys + i);

        fwrite(buf.data(), 1, size, f);
        fclose(f);

        fprintf(stderr, "[dump-ucode] wrote %u bytes to %s (src=0x%08X)\n", size, path, phys);
    };

    write_region("imem", ucode_phys, 0x2000);
    write_region("dmem", udata_phys, udata_size);
    fflush(stderr);
}

// First 8 tasks, every 64th, and every ucode change.
void RenderDiag::log_task(const OSTask* task)
{

    if (!recomp::dbg::log_heartbeat()) return;

    static int s_count = 0;
    static uint32_t s_last_ucode = 0, s_last_data = 0;
    int n = ++s_count;
    const bool ucode_changed = (task->t.ucode != s_last_ucode) || (task->t.ucode_data != s_last_data);
    s_last_ucode = task->t.ucode;
    s_last_data = task->t.ucode_data;

    if (n <= 8 || (n & 63) == 0 || ucode_changed)
    {
        fprintf(stderr,
            "[hle send_dl #%d]%s type=%u ucode=0x%08X data=0x%08X dl=0x%08X size=%u wq=%d pq=%d\n",
            n, ucode_changed ? " [UCODE-CHANGE]" : "",
            (unsigned)task->t.type, (unsigned)task->t.ucode,
            (unsigned)task->t.ucode_data, (unsigned)task->t.data_ptr,
            (unsigned)task->t.data_size, workload_cursor(), present_cursor());
        fflush(stderr);
    }
}

// ROGUESQ_LOG_TRANSITION=1: for 180 VIs after each mode-change clear, sample the scanned-out buffer in RDRAM (RT64 writes rendered frames back) and log how white it is.
// Keeps the last 240 VI lines and dumps them when a clear fires, so the frames leading into a mode change are visible too.
void RenderDiag::log_transition_lum()
{

    static const bool s_on = env_on("ROGUESQ_LOG_TRANSITION");

    if (!s_on || !app->core.RDRAM) return;

    constexpr int kRing = 240;
    static std::string s_ring[kRing];
    static int s_head = 0;
    static uint32_t s_seq = 0;
    static int s_left = 0;

    if (g_rs64_clear_seq != s_seq)
    {
        s_seq = g_rs64_clear_seq;
        s_left = 180;

        fprintf(stderr, "[transition] ---- last %d VIs before clear #%u ----\n", kRing, s_seq);

        for (int i = 0; i < kRing; ++i) {
            const std::string& e = s_ring[(s_head + i) % kRing];

            if (!e.empty()) fprintf(stderr, "%s\n", e.c_str());
        }

        fprintf(stderr, "[transition] ---- end ring ----\n");
    }

    ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();

    if (!vi) return;

    const uint32_t origin = vi->VI_ORIGIN_REG & 0x00FFFFFFu;
    const uint32_t w = vi->VI_WIDTH_REG;
    const uint32_t bpp = ((vi->VI_STATUS_REG & 3u) == 3u) ? 4u : 2u;

    if (w < 16 || w > 1024 || origin == 0) return;

    const uint32_t bytes = g_rs64_clear_bytes;
    uint32_t h = bytes ? bytes / (w * bpp) : 240u;

    if (h < 8 || h > 480) h = 240;

    int white = 0, n = 0;
    uint32_t sum = 0;

    for (uint32_t gy = 0; gy < 24; ++gy) {
        for (uint32_t gx = 0; gx < 32; ++gx)
        {
            const uint32_t x = (gx * w) / 32 + w / 64, y = (gy * h) / 24 + h / 48;
            const uint32_t a = origin + (y * w + x) * bpp;
            uint32_t r, g, b;

            if (bpp == 2) {
                const uint32_t hi = rd8(a), lo = rd8(a + 1);
                r = (hi >> 3) << 3;
                g = (((hi & 7) << 2) | (lo >> 6)) << 3;
                b = ((lo >> 1) & 31) << 3;
            }
            else {
                r = rd8(a);
                g = rd8(a + 1);
                b = rd8(a + 2);
            }

            const uint32_t l = (r * 3 + g * 6 + b) / 10;
            sum += l;

            if (l >= 220) ++white;

            ++n;
        }
    }

    const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() % 100000;
    char line[256];
    snprintf(line, sizeof line, "[transition] vi-lum t=%05lld vi=#%d origin=0x%06X w=%u game=0x%06X/%u hstart=0x%X vstart=0x%X white=%d/%d mean=%u",
             ms, vi_count_, origin, w, game_vi_origin_, game_vi_width_, vi->VI_H_START_REG, vi->VI_V_START_REG, white, n, sum / (uint32_t)n);

    if (s_left > 0) {
        --s_left;

        fprintf(stderr, "%s\n", line);
    }
    else {
        s_ring[s_head] = line;
        s_head = (s_head + 1) % kRing;
    }
}

// ROGUESQ_LOG_GAMESTATE=1: log engine globals (docs/reference/game-architecture.md) on change and
// every 512 presents. gateCtr vs cuts44 shows whether the cinematic timeline is advancing.
void RenderDiag::log_gamestate()
{

    if ((vi_count_ & 63) != 0) return;

    static const bool s_on = env_on("ROGUESQ_LOG_GAMESTATE");

    if (!s_on) return;

    struct Snap {
        uint8_t level, craft, cineStage, u20, u21, u22, screen, menuId, sub;
        uint32_t cineState, cutscene, menuPtr;
        int sceneId;
        bool operator==(const Snap& o) const
        {

            return level == o.level && craft == o.craft && cineStage == o.cineStage &&
                   u20 == o.u20 && u21 == o.u21 && u22 == o.u22 && screen == o.screen &&
                   menuId == o.menuId && menuPtr == o.menuPtr && sceneId == o.sceneId &&
                   sub == o.sub &&
                   cineState == o.cineState && cutscene == o.cutscene;
        }
    };

    Snap s{};
    s.sceneId = g_current_scene;
    s.level = rd8(0x130B70);
    s.craft = rd8(0x130B41);
    s.cineState = rd32(0x0B0934);
    s.cineStage = rd8(0x0B0938);
    s.cutscene = rd32(0x0B1904);
    s.u20 = rd8(0x130B60);
    s.u21 = rd8(0x130B61);
    s.u22 = rd8(0x130B62);
    s.screen = rd8(0x130B14);
    s.menuId = rd8(0x0CE734);
    s.menuPtr = rd32(0x0CE730);
    s.sub = rd8(0x0CDAC4);
    const uint8_t b04 = rd8(0x130B44), b1f = rd8(0x130B5F), b25 = rd8(0x130B65);
    const uint32_t gateCtr = rd32(0x0B0B28);
    const bool cut_ok = s.cutscene >= 0x80000000u && s.cutscene < 0x80800000u;
    const uint32_t cuts44 = cut_ok ? rd32((s.cutscene - 0x80000000u) + 0x44) : 0;

    static Snap s_last{};
    static bool s_have_last = false;
    const bool changed = !s_have_last || !(s == s_last);

    if (!changed && (vi_count_ & 511) != 0) return;

    fprintf(stderr,
        "[gamestate vi=#%d] state=%s scene=%d menuId=%u sub=%u menuPtr=0x%08X screen=%u level=%u craft=%u b04=%u b1f=%u b25=%u u20=%u u21=%u u22=%u cineState=0x%08X cineStage=%u cutscene=0x%08X gateCtr=%u cuts44=%u%s\n",
        vi_count_, rs64_state_current_id(), g_current_scene, s.menuId, s.sub, s.menuPtr, s.screen, s.level, s.craft, b04, b1f, b25, s.u20, s.u21, s.u22,
        s.cineState, s.cineStage, s.cutscene, gateCtr, cuts44, changed ? " [CHANGED]" : "");

    if (cut_ok) {
        const uint32_t off = s.cutscene - 0x80000000u;

        fprintf(stderr, "  cutscene-filename: \"");

        for (int i = 0; i < 32; i++) {
            const uint8_t b = rd8(off + i);

            if (b == 0) break;

            fputc((b >= 0x20 && b < 0x7F) ? b : '?', stderr);
        }

        fprintf(stderr, "\"\n");
    }

    fflush(stderr);
    s_last = s;
    s_have_last = true;
}

// One-shot RDRAM dump (un-swizzled to MIPS byte order, same layout as a PJ64 dump) for
// tools/debug/validate/rdram_golden_diff.py. Triggers, first match wins:
//   ROGUESQ_DUMP_RDRAM_AT_VI=<n>              present number
//   ROGUESQ_DUMP_RDRAM_ON_CINE_ITER=<n>       cinematic loop iteration (rs64_cine_iter_get)
//   ROGUESQ_DUMP_RDRAM_ON_CINE_STALL=<ms>     cinematic frame counter (0x8013889C) unchanged that long
//   ROGUESQ_DUMP_RDRAM_ON_SCENE=<id>[,settle] g_current_scene held for settle presents (default 60)
//   ROGUESQ_DUMP_RDRAM_ON_SCREEN=<n|menu[,settle]>  screenState == n, or menu overlay after the cinematic
// Path: ROGUESQ_DUMP_RDRAM_PATH, default dumps/rdram_<tag>.bin.
void RenderDiag::dump_rdram_if_armed()
{
    // ROGUESQ_DUMP_RDRAM_ON_STATE=<id>[,<id>...]: multi-milestone dump keyed to the game-state
    // classifier -- dumps dumps/rdram_state_<id>.bin the first present each listed state becomes
    // current, ALL in one run (unlike the one-shot triggers below). Feeds tools/debug/validate/state_diff.ps1.
    static const char* s_on_state = env_str("ROGUESQ_DUMP_RDRAM_ON_STATE");

    if (s_on_state && app->core.RDRAM)
    {
        static char s_buf[256];
        static const char* s_ids[16];
        static bool s_iddone[16];
        static int s_held[16];
        static const int s_settle = env_int("ROGUESQ_DUMP_RDRAM_STATE_SETTLE", 0);
        static int s_nids = -1;

        if (s_nids < 0) {
            std::snprintf(s_buf, sizeof s_buf, "%s", s_on_state);
            s_nids = 0;

            for (char* p = s_buf; *p && s_nids < 16; ) {
                s_ids[s_nids] = p;
                s_iddone[s_nids] = false;
                s_held[s_nids] = 0;
                ++s_nids;
                char* c = std::strchr(p, ',');

                if (!c) break;

                *c = 0;
                p = c + 1;
            }
        }

        const char* cur = rs64_state_current_id();

        for (int i = 0; i < s_nids; ++i)
        {
            if (s_iddone[i]) continue;
            if (std::strcmp(cur, s_ids[i]) != 0) { s_held[i] = 0; continue; }
            if (++s_held[i] <= s_settle) continue;

            s_iddone[i] = true;
            char path[512];
            std::snprintf(path, sizeof path, "dumps/rdram_state_%s.bin", s_ids[i]);
            FILE* f = recomp::os::fopen(path, "wb");

            if (f) {
                static uint8_t sbuf[0x10000];

                for (uint32_t base = 0; base < 0x800000u; base += sizeof sbuf)
                {
                    for (uint32_t j = 0; j < sizeof sbuf; ++j) sbuf[j] = app->core.RDRAM[(base + j) ^ 3];

                    fwrite(sbuf, 1, sizeof sbuf, f);
                }

                fclose(f);
            }

            fprintf(stderr, "[rdram-dump] state=%s vi=#%d -> %s (%s)\n", s_ids[i], vi_count_, path, f ? "ok" : "OPEN FAILED");
            fflush(stderr);
        }
    }

    static const char* s_at_vi = env_str("ROGUESQ_DUMP_RDRAM_AT_VI");
    static const char* s_on_cine = env_str("ROGUESQ_DUMP_RDRAM_ON_CINE_ITER");
    static const char* s_on_stall = env_str("ROGUESQ_DUMP_RDRAM_ON_CINE_STALL");
    static const char* s_on_scene = env_str("ROGUESQ_DUMP_RDRAM_ON_SCENE");
    static const char* s_on_screen = env_str("ROGUESQ_DUMP_RDRAM_ON_SCREEN");
    static bool s_done = false, s_seen_cine = false;

    if (s_done || !app->core.RDRAM || !(s_at_vi || s_on_cine || s_on_stall || s_on_scene || s_on_screen)) return;

    const uint8_t screenState = rd8(0x130B14);

    if (screenState >= 6 || g_active_overlay == 2) s_seen_cine = true;

    char tag[48] = {0};

    if (s_at_vi)
    {
        if (vi_count_ < std::atoi(s_at_vi)) return;

        std::snprintf(tag, sizeof tag, "vi_%d", vi_count_);
    } else if (s_on_cine) {
        // Comma-separated list: one dump per listed iteration (ROGUESQ_DUMP_RDRAM_PATH ignored for lists).
        static std::vector<unsigned long long> s_iters;
        static size_t s_next = 0;

        if (s_iters.empty()) { for (const char* c = s_on_cine; *c; ) { s_iters.push_back((unsigned long long)std::strtoull(c, (char**)&c, 10)); if (*c == ',') ++c; else break; } if (s_iters.empty()) s_iters.push_back(0); }

        const unsigned long long it = rs64_cine_iter_get();

        if (s_next >= s_iters.size() || it < s_iters[s_next]) return;

        ++s_next;
        std::snprintf(tag, sizeof tag, "cine_iter%llu", it);

        if (s_next < s_iters.size()) {
            char path2[512];
            std::snprintf(path2, sizeof path2, "dumps/rdram_%s.bin", tag);

            if (FILE* f2 = recomp::os::fopen(path2, "wb"))
            {
                static uint8_t buf2[0x10000];

                for (uint32_t base = 0; base < 0x800000u; base += sizeof buf2)
                {
                    for (uint32_t i = 0; i < sizeof buf2; ++i) buf2[i] = app->core.RDRAM[(base + i) ^ 3];

                    fwrite(buf2, 1, sizeof buf2, f2);
                }

                fclose(f2);

                fprintf(stderr, "[rdram-dump] vi=#%u cine_iter=%llu -> %s (ok)\n", vi_count_, it, path2);
                fflush(stderr);
            }

            return;
        }
    } else if (s_on_stall) {
        const uint32_t it = rd32(0x13889C);
        static uint32_t s_last_it = 0;
        static uint64_t s_last_ms = 0;
        const uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        if (it != s_last_it) { s_last_it = it; s_last_ms = now; return; }
        if (it == 0 || !s_last_ms || now - s_last_ms < (uint64_t)std::atoll(s_on_stall)) return;

        std::snprintf(tag, sizeof tag, "cine_stall_iter%u", it);
    } else if (s_on_scene) {
        const int want = std::atoi(s_on_scene);
        const char* comma = std::strchr(s_on_scene, ',');
        const int settle = comma ? std::atoi(comma + 1) : 60;
        static int s_held = 0;
        s_held = (g_current_scene == want) ? s_held + 1 : 0;

        if (s_held < settle) return;

        std::snprintf(tag, sizeof tag, "scene_%d", want);
    } else {
        const bool want_menu = (s_on_screen[0] == 'm');
        const char* comma = std::strchr(s_on_screen, ',');
        const int settle = comma ? std::atoi(comma + 1) : 0;
        const bool cond = want_menu ? (s_seen_cine && g_active_overlay == 1)
                                    : (screenState == (uint8_t)std::atoi(s_on_screen));
        static int s_held = 0;
        s_held = cond ? s_held + 1 : 0;

        if (!cond || s_held <= settle) return;

        std::snprintf(tag, sizeof tag, "screen_%u", screenState);
    }

    s_done = true;
    char path[512];

    if (const char* p = env_str("ROGUESQ_DUMP_RDRAM_PATH")) std::snprintf(path, sizeof path, "%s", p);
    else std::snprintf(path, sizeof path, "dumps/rdram_%s.bin", tag);

    FILE* f = recomp::os::fopen(path, "wb");

    if (f) {
        static uint8_t buf[0x10000];

        for (uint32_t base = 0; base < 0x800000u; base += sizeof buf)
        {
            for (uint32_t i = 0; i < sizeof buf; ++i) buf[i] = app->core.RDRAM[(base + i) ^ 3];

            fwrite(buf, 1, sizeof buf, f);
        }

        fclose(f);
    }

    fprintf(stderr, "[rdram-dump] vi=#%d screen=%u -> %s (%s)\n", vi_count_, screenState, path, f ? "ok" : "OPEN FAILED");
    fflush(stderr);
}

// First 4 presents and every 64th: VI regs, queue cursors, and whether the scanout buffer
// has any pixel data.
void RenderDiag::log_vi_state()
{
    // ROGUESQ_LOG_VI_CHANGES=1: the VI registers as presented (after the menu fix) whenever any of them changes.
    static const bool s_vi_changes = env_on("ROGUESQ_LOG_VI_CHANGES");

    if (s_vi_changes)
    {
        const ultramodern::renderer::ViRegs* v = ultramodern::renderer::get_vi_regs();
        static uint32_t s_prev[7] = {};
        const uint32_t cur[7] = { v->VI_ORIGIN_REG, v->VI_WIDTH_REG, v->VI_X_SCALE_REG, v->VI_Y_SCALE_REG, v->VI_H_START_REG, v->VI_V_START_REG, v->VI_STATUS_REG };

        if (std::memcmp(cur, s_prev, sizeof(cur)) != 0)
        {
            std::memcpy(s_prev, cur, sizeof(cur));

            fprintf(stderr, "[vi-change] #%d origin=%08X width=%u xscale=%X yscale=%X hstart=%X vstart=%X status=%X drawn=%06X dw=%u\n",
                    vi_count_, cur[0], cur[1], cur[2], cur[3], cur[4], cur[5], cur[6], (unsigned)g_most_drawn_fb, (unsigned)g_most_drawn_fb_width);
        }
    }

    if (!recomp::dbg::log_heartbeat()) return;

    static bool s_filter_logged = false;

    if (!s_filter_logged && app->appWindow && app->appWindow->sdlEventFilterInstalled)
    {
        fprintf(stderr, "[RT64] sdlEventFilter NOW installed at update_screen #%d (sdlWindow=%p)\n",
                vi_count_, (void*)app->appWindow->sdlWindow);
        fflush(stderr);
        s_filter_logged = true;
    }

    if (vi_count_ > 4 && (vi_count_ & 63) != 0) return;

    ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
    const uint32_t origin = vi->VI_ORIGIN_REG & 0x00FFFFFF;
    uint32_t any_nonzero = 0;

    if (app->core.RDRAM && origin > 0 && origin + 16 < 0x800000)
    {
        for (int i = 0; i < 16; i++) any_nonzero |= app->core.RDRAM[origin + i];
    }

    fprintf(stderr,
        "[vi] update_screen #%d origin=0x%08X width=%u status=0x%X xscale=0x%X yscale=0x%X v_current=%u nonzero=%d pq.wc=%d wq.wc=%d drawn=0x%08X dw=%u\n",
        vi_count_, vi->VI_ORIGIN_REG, vi->VI_WIDTH_REG, vi->VI_STATUS_REG, vi->VI_X_SCALE_REG, vi->VI_Y_SCALE_REG, vi->VI_V_CURRENT_LINE_REG,
        any_nonzero != 0, present_cursor(), workload_cursor(),
        (unsigned)g_most_drawn_fb, (unsigned)g_most_drawn_fb_width);
    fflush(stderr);
}

}
