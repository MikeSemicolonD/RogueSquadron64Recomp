// Host entry points for the [[patches.hook]] entries in rogue_squadron.toml. Hook code is
// generated MIPS C with no host headers, so anything it needs from the host is extern "C" here.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <unordered_map>
#include <span>
#include <variant>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

#include "librecomp/game.hpp"
#include "librecomp/mods.hpp"
#include "librecomp/patcher.hpp"
#include "recomp.h"
#include "video_config.h"
#include "debug_logs.h"
#include "hook_helpers.h"      // declares the exports defined below (g_vi_tick, rs64_cine_iter_get, …)
#include "upstream_compat.h"   // rs64_vi_driven, rs64_fb_guards_mask
#include "game_state.h"
#include "hle/rt64_rs64_transition.h"

using recomp::dbg::env_str;
using recomp::dbg::env_on;
using recomp::dbg::env_int;

// Diagnostic-only helper bodies (DL dumps, medal-probe, hook log sink) compile
// to no-op stubs in Release so the symbols still link for the recomp's calls.
#ifdef NDEBUG
#define RS64_DIAG 0
#else
#define RS64_DIAG 1
#endif


// ---- Draw distance ----

static float env_mult(const char* name, float def) {
    const char* v = env_str(name);
    const float m = v ? float(std::atof(v)) : def;
    return (m >= 0.25f && m <= 8.0f) ? m : def;
}

// Draw distance multiplier for the camera far plane and object culls: the in-game DRAW DISTANCE setting (roguesq_video.json), live; ROGUESQ_DRAW_DIST overrides.
static float draw_dist_setting() {
    static const float s_env = env_mult("ROGUESQ_DRAW_DIST", 0.0f);
    return (s_env > 0.0f) ? s_env : rs64::video::draw_distance();
}

static float terrain_dist_setting() {
    static const float s_env = env_mult("ROGUESQ_TERRAIN_DIST", 0.0f);
    return std::min((s_env > 0.0f) ? s_env : draw_dist_setting(), 2.5f);
}

// Cutscenes keep the game's own view distance and fog per shot unless the player opts out (keepCutsceneDrawDistance; ROGUESQ_CINE_DRAW_DIST=0/1 overrides).
static bool game_owns_view_setting() {
    static const int s_env = env_int("ROGUESQ_CINE_DRAW_DIST", -1);
    return (s_env >= 0) ? (s_env != 0) : rs64::video::keep_cutscene_draw_distance();
}

static bool game_owns_view() {
    return game_owns_view_setting() && rs64_state_in_cinematic();
}

extern "C" float rs64_draw_dist(void) {
    return rs64::video::effective_draw_distance(draw_dist_setting(), game_owns_view(), rs64_state_level());
}

// Terrain reach and fog multiplier, following the draw distance unless ROGUESQ_TERRAIN_DIST overrides. Capped at 2.5 (128-cell grid).
extern "C" float rs64_terrain_dist(void) {
    return rs64::video::effective_draw_distance(terrain_dist_setting(), game_owns_view(), rs64_state_level());
}

// ROGUESQ_LOG_FOG=1: level, far-range base D and its bounds, fog start/end/color the game set, cutscene flag and our multipliers, on change.
extern "C" void rs64_fog_probe(uint8_t* rdram) {
    static const bool s_on = env_on("ROGUESQ_LOG_FOG");
    if (!s_on) return;
    auto rw = [&](uint32_t a) { return *reinterpret_cast<uint32_t*>(rdram + (a - 0x80000000u)); };
    auto rf = [&](uint32_t a) { uint32_t u = rw(a); float f; std::memcpy(&f, &u, 4); return f; };
    auto rb = [&](uint32_t a) { return (uint32_t)rdram[(a - 0x80000000u) ^ 3u]; };
    const uint32_t level = rw(0x80130B70u);
    const uint32_t rgb = (rb(0x8011A873u) << 16) | (rb(0x8011A874u) << 8) | rb(0x8011A875u);
    const int cine = rs64_state_in_cinematic();
    const float cur[6] = { rf(0x8009DEACu), rf(0x80130C54u), rf(0x80130C58u), rf(0x8011A878u), rf(0x8011A87Cu), rs64_draw_dist() * 1000.0f + rs64_terrain_dist() };
    static float s_last[6] = {};
    static uint32_t s_level = ~0u, s_rgb = ~0u;
    static int s_cine = -1;
    if (level == s_level && rgb == s_rgb && cine == s_cine && std::memcmp(cur, s_last, sizeof(cur)) == 0) return;
    std::memcpy(s_last, cur, sizeof(cur));
    s_level = level;
    s_rgb = rgb;
    s_cine = cine;
    fprintf(stderr, "[fog] level=%u D=%.3f range=%.2f..%.2f fog=%.1f..%.1f rgb=%06X cine=%d dd=%.2f td=%.2f\n",
            level, cur[0], cur[1], cur[2], cur[3], cur[4], rgb, cine, rs64_draw_dist(), rs64_terrain_dist());
    fflush(stderr);
}

// Level terrain cell budget scale, evaluated at each level load: ROGUESQ_TGRID_BUDGET_MULT (1-2), default 2 when terrain distance is 2x or more.
// 3x exhausts the game heap and the level load stalls; N is also capped at 6553 (u16 vertex-block indices up to 10N-4).
// Set around setupCutsceneLevel's loadLevelAssets call (the classifier may not report "cinematic" yet at load); cutscene levels keep the stock budget while cutscenes keep their own view.
static bool s_cutscene_level_load = false;
extern "C" void rs64_cutscene_level_load(int on) {
    s_cutscene_level_load = on != 0;
}

extern "C" uint32_t rs64_tgrid_budget(uint32_t original) {
    static const int s_env = env_int("ROGUESQ_TGRID_BUDGET_MULT", 0);
    const bool cutscene_level = s_cutscene_level_load && game_owns_view_setting();
    const uint32_t s_mult = cutscene_level ? 1u : (uint32_t)std::clamp(s_env > 0 ? s_env : (terrain_dist_setting() >= 2.0f ? 2 : 1), 1, 2);
    const uint32_t n = original * s_mult;
    static const bool s_log = env_on("ROGUESQ_LOG_TERRAIN_GRID");
    if (s_log) { fprintf(stderr, "[tgrid] level-load budget %u x%u%s\n", original, s_mult, cutscene_level ? " (cutscene level)" : ""); fflush(stderr); }
    return n > 0x1999u ? 0x1999u : n;
}

// Peak terrain cells streamed in one grid build and how often the level budget stopped the stream.
static uint32_t s_tgrid_cells_peak = 0, s_tgrid_budget_hits = 0;
extern "C" void rs64_tgrid_budget_probe(uint32_t cells, uint32_t budget) {
    s_tgrid_cells_peak = std::max(s_tgrid_cells_peak, cells);
    if (cells >= budget) s_tgrid_budget_hits++;
}

// ROGUESQ_LOG_TERRAIN_GRID=1: terrain view bounding box and cell budget, every 60 grid builds.
extern "C" void rs64_terrain_grid_log(int32_t minX, int32_t width, int32_t minZ, int32_t height, uint32_t budget, uint32_t mapW, uint32_t mapH) {
    static const bool s_on = env_on("ROGUESQ_LOG_TERRAIN_GRID");
    if (!s_on) return;
    static int s_n = 0;
    static int32_t s_max = 0;
    s_max = std::max(s_max, std::max(width, height));
    if ((++s_n % 60) != 0) return;
    fprintf(stderr, "[tgrid] box=%dx%d at (%d,%d) map=%ux%u budget=%u maxDim=%d cellsPeak=%u budgetHits=%u\n", width, height, minX, minZ, mapW, mapH, budget, s_max, s_tgrid_cells_peak, s_tgrid_budget_hits);
    fflush(stderr);
}

// Terrain grid tables relocated into host-only RDRAM above the game's 8 MB, sized for the 128-stride patches (not optional).
// 0x80A00000-0x80A01FFF is the F5 renderer's vertex/viewport scratch, so the tables start at 0x80B00000.
static constexpr uint32_t RS64_TGRID_BASE = 0x80B00000u;
extern "C" uint32_t rs64_tgrid_base(uint32_t original) {
    switch (original) {
    case 0x80130C70u: return RS64_TGRID_BASE;
    case 0x80130D10u: return RS64_TGRID_BASE + 0x100u;
    case 0x80130DB0u: return RS64_TGRID_BASE + 0x1000u;
    case 0x80131DB0u: return RS64_TGRID_BASE + 0x5000u;
    case 0x80132DC0u: return RS64_TGRID_BASE + 0x10000u;
    default:          return original;
    }
}

// Zeroes the relocated terrain grid tables at level load, as a fresh level would find the originals.
extern "C" void rs64_tgrid_clear(uint8_t* rdram) {
    std::memset(rdram + (RS64_TGRID_BASE - 0x80000000u), 0, 0x20000);
}

// Keeps the terrain view box inside the 128-cell grid (far corners within 62 cells of the camera). ROGUESQ_TGRID_MAP_CLAMP=1 also clamps to the map bounds; off by default, since vanilla draws past the map edge by repeating edge tiles.
extern "C" void rs64_tgrid_clamp_polygon(uint8_t* rdram) {
    constexpr float R = 62.0f;
    auto rf = [&](uint32_t a) { uint32_t u = *reinterpret_cast<uint32_t*>(rdram + (a - 0x80000000u)); float f; std::memcpy(&f, &u, 4); return f; };
    auto wf = [&](uint32_t a, float f) { uint32_t u; std::memcpy(&u, &f, 4); *reinterpret_cast<uint32_t*>(rdram + (a - 0x80000000u)) = u; };
    auto rh = [&](uint32_t a) { return *reinterpret_cast<uint16_t*>(rdram + ((a ^ 2u) - 0x80000000u)); };
    static const bool s_map_clamp = env_on("ROGUESQ_TGRID_MAP_CLAMP", false);
    const float mapW = s_map_clamp ? (float)rh(0x80136DF8u) : 0.0f, mapH = s_map_clamp ? (float)rh(0x80136DFAu) : 0.0f;
    const float cx = rf(0x80130C10u), cz = rf(0x80130C18u);
    for (uint32_t i = 1; i <= 4; i++) {
        const uint32_t p = 0x80130C10u + i * 0xCu;
        float x = std::clamp(rf(p), cx - R, cx + R);
        float z = std::clamp(rf(p + 8), cz - R, cz + R);
        if (mapW > 1.0f && mapH > 1.0f) {
            x = std::clamp(x, 0.0f, mapW - 1.0f);
            z = std::clamp(z, 0.0f, mapH - 1.0f);
        }
        wf(p, x);
        wf(p + 8, z);
    }
}

// ---- Boot target ----

// Set by the present hook while the attract title is up; get_n64_input injects START while set.
extern "C" { volatile int g_boot_pulse_start = 0; }

// ROGUESQ_BOOT_TARGET=level:<id>[,craft]: level id (0..0x14) or -1; craft (0..8) or -1 via craft_out.
// Consumed by the runIdleFramesAndLoadSaveData epilogue hook.
extern "C" int rs64_boot_target_level(int* craft_out) {
    static int s_lvl = -2, s_craft = -1;
    if (s_lvl == -2) {
        s_lvl = -1;
        const char* v = env_str("ROGUESQ_BOOT_TARGET");
        if (v && strncmp(v, "level", 5) == 0) {
            const char* c = strchr(v, ':');
            int n = (c && c[1]) ? std::atoi(c + 1) : 0;
            if (n >= 0 && n <= 0x14) s_lvl = n;
            const char* comma = strchr(v, ',');
            if (comma && comma[1]) { int cc = std::atoi(comma + 1); if (cc >= 0 && cc <= 8) s_craft = cc; }
        }
    }
    if (craft_out) *craft_out = s_craft;
    return s_lvl;
}

// ---- State published for the GBI ----

// g_current_scene: menuOverlayInit action id, 9 = attribution. g_op_bf_count: bumped by the F5 GBI per explosion-bloom tri.
extern "C" { volatile int g_current_scene = -1; }
extern "C" { volatile unsigned g_op_bf_count = 0; }

// ROGUESQ_LOG_HOOKS=1: stderr for hook code.
extern "C" void rs64_dbg_log4(const char* tag, unsigned a, unsigned b, unsigned c, unsigned d) {
#if RS64_DIAG
    static const bool on = env_on("ROGUESQ_LOG_HOOKS");
    if (!on) return;
#ifdef _WIN32
    static uint64_t s_t0 = GetTickCount64();
    unsigned ms = (unsigned)(GetTickCount64() - s_t0);
#else
    unsigned ms = 0;
#endif
    fprintf(stderr, "[hook t=%ums] %s a=0x%08X b=0x%08X c=0x%08X d=0x%08X\n", ms, tag ? tag : "?", a, b, c, d);
    fflush(stderr);
#else
    (void)tag; (void)a; (void)b; (void)c; (void)d;
#endif
}

// ---- Cinematic loop landmark counter ----

// Ticked by the cinematicLoopBody hook; read by the f5-dl-validate landmark, the
// render-context RDRAM-on-iter snapshot, and the ROGUESQ_CINE_HOLD_ITER pacer.
static std::atomic<uint64_t> g_cine_iter{0};

extern "C" unsigned long long rs64_cine_iter_get(void) { return g_cine_iter.load(std::memory_order_relaxed); }
extern "C" void rs64_cine_iter_tick(unsigned iter) { g_cine_iter.store(iter, std::memory_order_relaxed); }

// ---- Pacing ----

// Every 16 cinematic iterations: yield and flush. A kernel call is what unsticks the loop.
extern "C" void rs64_cine_yield(void) {
    static int s_count = 0;
    if ((++s_count & 0xF) == 0) {
#ifdef _WIN32
        ::SwitchToThread();
#else
        std::this_thread::yield();
#endif
        std::fflush(stderr);
    }
}

// ROGUESQ_ATTRIBUTION_SLEEP_MS=N per attribution iteration (default 0: VI-paced).
extern "C" void rs64_idle_pace(void) {
#ifdef _WIN32
    static const int s_ms = [](){ int v = env_int("ROGUESQ_ATTRIBUTION_SLEEP_MS"); return v < 0 ? 0 : v > 5000 ? 5000 : v; }();
    if (s_ms) ::Sleep((DWORD)s_ms);
#endif
}

// ROGUESQ_CINE_TARGET_FPS=N paces cinematicLoopBody (default 0 when VI-driven, else 30).
// ROGUESQ_CINE_HOLD_ITER=N [+ ROGUESQ_CINE_HOLD_MS, default 15000]: park once at iteration N.
extern "C" void rs64_cine_pace(void) {
#ifdef _WIN32
    static const long long s_hold_iter = env_str("ROGUESQ_CINE_HOLD_ITER") ? std::atoll(env_str("ROGUESQ_CINE_HOLD_ITER")) : -1;
    static const uint32_t s_hold_ms = (uint32_t)env_int("ROGUESQ_CINE_HOLD_MS", 15000);
    static bool s_held = false;
    if (s_hold_iter >= 0 && !s_held && (long long)rs64_cine_iter_get() >= s_hold_iter) {
        s_held = true;
        fprintf(stderr, "[cine-hold] freezing at iter %llu for %u ms\n", rs64_cine_iter_get(), s_hold_ms);
        fflush(stderr);
        uint64_t end = GetTickCount64() + s_hold_ms;
        while (GetTickCount64() < end) ::Sleep(100);
        fprintf(stderr, "[cine-hold] released\n"); fflush(stderr);
    }
    static const int s_target_fps = [](){
        int v = env_int("ROGUESQ_CINE_TARGET_FPS", rs64_vi_driven() ? 0 : 30);
        v = v < 0 ? 0 : v > 240 ? 240 : v;
        if (v) timeBeginPeriod(1);   // so Sleep(33) is not quantized to 15.6 ms
        return v;
    }();
    if (s_target_fps == 0) return;
    const uint32_t interval_ms = 1000u / (uint32_t)s_target_fps;
    static uint64_t s_last_ms = 0;
    uint64_t now = GetTickCount64();
    if (s_last_ms == 0) { s_last_ms = now; return; }
    uint64_t elapsed = now - s_last_ms;
    if (elapsed < interval_ms) ::Sleep((DWORD)(interval_ms - elapsed));
    s_last_ms = GetTickCount64();
#endif
}

extern "C" void rs64_sleep_ms(unsigned ms) {
#ifdef _WIN32
    ::Sleep((DWORD)ms);
#endif
}

// Bumped once per host VI retrace (main.cpp).
extern "C" { volatile unsigned g_vi_tick = 0; }

// Block until the next VI tick (cap ~40 ms). Paces a menu fade loop (attribution screen,
// credits sequence) to real time so the VI-driven glyph-layout producer thread gets to run.
extern "C" void rs64_attrib_wait_vi(void) {
    unsigned start = g_vi_tick;
#ifdef _WIN32
    for (int i = 0; i < 40 && g_vi_tick == start; ++i) ::Sleep(1);
#else
    for (int i = 0; i < 40 && g_vi_tick == start; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
}

// ---- Service replies ----


// tickFormatMessageWorker sends every reply as a pointer to one stack buffer. Copy each reply
// into its own slot of a persistent ring so replies still queued keep their request ids.
// The ring lives in host RDRAM above the game's 8 MB (after the terrain grid tables): a game-heap block would be reallocated when the front end reloads between screens.
// Returns the address to send.
extern "C" uint32_t rs64_format_reply_slot(uint8_t* rdram, recomp_context* ctx, uint32_t src) {
    (void)ctx;
    constexpr uint32_t kSlots = 32, kSlotSize = 0x30, kRing = 0x80B20000u;
    static uint32_t s_next = 0;
    if ((src & 3u) != 0 || (src & 0xFF800000u) != 0x80000000u) return src;
    const uint32_t dst = kRing + (s_next++ % kSlots) * kSlotSize;
    std::memcpy(rdram + (dst - 0x80000000u), rdram + (src - 0x80000000u), kSlotSize);
    return dst;
}

// ---- NPC health guard ----

// ---- Mode-change blanking ----

// RT64's present queue presents black while the VI shows this CPU-zeroed buffer. The game has drained every frame before the clear, so the last workload id is stable here.
extern "C" { volatile uint32_t g_rs64_clear_seq = 0; }
extern "C" { volatile uint32_t g_rs64_clear_bytes = 0; }

extern "C" void rs64_fb_cpu_cleared(uint8_t* /*rdram*/, uint32_t fb, uint32_t bytes) {
    static const bool s_log = env_on("ROGUESQ_LOG_TRANSITION");
    g_rs64_cpu_cleared_wid.store(g_rs64_last_workload_id.load(std::memory_order_acquire), std::memory_order_release);
    g_rs64_cpu_cleared_fb.store(fb & 0x00FFFFFFu, std::memory_order_release);
    g_rs64_clear_bytes = bytes;
    g_rs64_clear_seq = g_rs64_clear_seq + 1;
    if (s_log) {
        fprintf(stderr, "[transition] cpu-clear fb=0x%08X bytes=0x%X\n", fb, bytes);
        fflush(stderr);
    }
}

// ---- Display-list walkers ----

// ROGUESQ_DUMP_FRAME_DL=N: dump the opcode stream of gfx tasks N..N+15 (per-command detail on N;
// TEXRECT count and bbox for all). ROGUESQ_DUMP_TEXTURES=1 also writes each SETTIMG source to
// dumps/tex/. Read-only.
extern "C" void rs64_dump_frame_dl(uint8_t* rdram, uint32_t dl_phys) {
#if RS64_DIAG
    static const int s_target = env_int("ROGUESQ_DUMP_FRAME_DL", -1);
    static const bool s_dump_tex = env_on("ROGUESQ_DUMP_TEXTURES");
    static int s_call = 0;
    const int call = ++s_call;

    // ROGUESQ_MEDAL_PROBE=1: on EVERY gfx task, scan the DL for the menu medal/insignia
    // layer (fmt4 textures 0x62xxxx) and log the color image active at that draw. Catches
    // the intermittent overlay-refresh pass a fixed dump window misses, and resolves whether
    // the medal draws are emitted-to-a-bad-target vs never emitted (walk desync).
    {
        static const int s_medal = env_on("ROGUESQ_MEDAL_PROBE") ? 1 : 0;
        if (s_medal && dl_phys != 0) {
            static uint8_t s_pv[0x800000 >> 3];
            memset(s_pv, 0, sizeof(s_pv));
            uint32_t pa = (dl_phys & 0x3FFFFFFu) | 0x80000000u;
            uint32_t p_cimg = 0, p_cimg_w0 = 0, p_cimg_at = 0;
            uint32_t pstack[16]; int psp = 0; int pjmp = 256;
            for (int i = 0; i < 16384; ++i) {
                if ((pa & 0x3FFFFFFu) + 8u > 0x800000u) break;
                uint32_t vi = (pa & 0x3FFFFFFu) >> 3;
                if (s_pv[vi]) break;
                s_pv[vi] = 1;
                uint32_t w0 = (uint32_t)MEM_W(0, (gpr)(int32_t)pa);
                uint32_t w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)pa);
                uint8_t op = (uint8_t)(w0 >> 24);
                if (op == 0xB4 || op == 0xBE) { pa += 16u; continue; }
                if (op == 0xB5 && (w1 & 0x00FFFFFFu) != 0u) { pa = (w1 & 0x00FFFFF8u) | 0x80000000u; continue; }
                if (op == 0xFF) { p_cimg = w1; p_cimg_w0 = w0; p_cimg_at = pa; }
                if (op == 0xFD) {
                    uint32_t timg = w1 & 0x00FFFFFFu;
                    if (timg >= 0x620000u && timg < 0x628000u) {
                        static int s_n = 0;
                        if (++s_n <= 60) {
                            uint32_t ca = p_cimg & 0x00FFFFFFu;
                            bool bad = (p_cimg == 0) || ca < 0x100000u || ca >= 0x800000u;
                            fprintf(stderr, "[medal-probe] task=%d tex=0x%06X activeCIMG=0x%08X (w0=0x%08X @0x%08X) %s texcmd@0x%08X\n",
                                    call, timg, p_cimg, p_cimg_w0, p_cimg_at, bad ? "GARBAGE" : "ok", pa);
                            fflush(stderr);
                        }
                        break;
                    }
                }
                if (op == 0xE4 || op == 0xE5) { pa += 24u; continue; }
                if (op == 0x03) { pa += 24u; continue; }
                if (op == 0xB8u || op == 0xDFu) { if (psp > 0) { pa = pstack[--psp]; continue; } break; }
                bool is_dl = ((op == 0x06u) && ((w0 & 0x00FEFFFFu) == 0u)) || (op == 0xDEu);
                if (is_dl && pjmp > 0) {
                    uint8_t br = (uint8_t)(w0 >> 16);
                    uint32_t tgt = (w1 & 0x00FFFFF8u) | 0x80000000u;
                    if (br == 0 && psp < 16) pstack[psp++] = pa + 8u;
                    pa = tgt; pjmp--; continue;
                }
                pa += 8u;
            }
        }
    }

    if (s_target < 0 || call < s_target || call >= s_target + 16 || dl_phys == 0) return;
    const bool full_detail = (call <= s_target + 1);

    uint32_t a = (dl_phys & 0x3FFFFFFu) | 0x80000000u;
    uint32_t dstack[16]; int dsp = 0; int djmp = 256;
    int tr_n = 0; int tr_minx = 99999, tr_miny = 99999, tr_maxx = -99999, tr_maxy = -99999;
    uint32_t last_cimg = 0;
    // The menu DL re-enters the same tile sub-DLs; skip any command address seen before.
    static uint8_t s_vis[0x800000 >> 3];
    memset(s_vis, 0, sizeof(s_vis));
    fprintf(stderr, "[frame-dl-dump call=%d] start=0x%08X\n", call, a);
    for (int i = 0; i < 16384; ++i) {
        if ((a & 0x3FFFFFFu) + 8u > 0x800000u) break;
        uint32_t vidx = (a & 0x3FFFFFFu) >> 3;
        if (s_vis[vidx]) break;
        s_vis[vidx] = 1;
        uint32_t w0 = (uint32_t)MEM_W(0, (gpr)(int32_t)a);
        uint32_t w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)a);
        uint8_t op = (uint8_t)(w0 >> 24);
        // 16-byte F5 commands (0xB4 as the GBI consumes it; 0xBE payload is not a command).
        if (op == 0xB4 || op == 0xBE) { a += 16u; continue; }
        // 0xB5 chunk link: follow w1 (0 = plain no-op).
        if (op == 0xB5 && (w1 & 0x00FFFFFFu) != 0u) { a = (w1 & 0x00FFFFF8u) | 0x80000000u; continue; }
        if (op == 0xE4 || op == 0xE5) {   // TEXRECT, 24 bytes
            int lrx = ((w0 >> 12) & 0xFFF), lry = (w0 & 0xFFF);
            int ulx = ((w1 >> 12) & 0xFFF), uly = (w1 & 0xFFF);
            int tile = (w1 >> 24) & 0x7;
            uint32_t t1w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)(a + 8u));
            uint32_t t2w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)(a + 16u));
            if (full_detail)
                fprintf(stderr, "  %08X: TEXRECT tile=%d ul=(%d,%d) lr=(%d,%d) px=(%.1f,%.1f)-(%.1f,%.1f) uls/t=0x%08X dsdx/y=0x%08X\n",
                        a, tile, ulx, uly, lrx, lry, ulx/4.0, uly/4.0, lrx/4.0, lry/4.0, t1w1, t2w1);
            ++tr_n;
            if (ulx/4 < tr_minx) tr_minx = ulx/4; if (uly/4 < tr_miny) tr_miny = uly/4;
            if (lrx/4 > tr_maxx) tr_maxx = lrx/4; if (lry/4 > tr_maxy) tr_maxy = lry/4;
            a += 24u; continue;
        }
        if (op == 0x03) { a += 24u; continue; }   // 8B cmd + 16B inline vertex
        if (op == 0xFD && s_dump_tex && full_detail) {
            uint32_t fmt = (w0 >> 21) & 0x7, siz = (w0 >> 19) & 0x3;
            uint32_t timg = w1 & 0x00FFFFFFu;
            char path[256];
            snprintf(path, sizeof(path), "dumps/tex/%06X_fmt%u_siz%u.bin", timg, fmt, siz);
            static int s_made = 0;
            if (!s_made) { s_made = 1; system("if not exist dumps\\tex mkdir dumps\\tex"); }
            FILE* tf = recomp::os::fopen(path, "wb");
            if (tf) {
                for (uint32_t k = 0; k < 0x2000u; ++k) { uint32_t off = timg + k; fputc(off < 0x800000u ? rdram[off ^ 3] : 0, tf); }
                fclose(tf);
            }
            fprintf(stderr, "  %08X: SETTIMG addr=0x%06X fmt=%u siz=%u -> %s\n", a, timg, fmt, siz, path);
        }
        if (op == 0xFF) last_cimg = w1;
        const char* tag = "";
        if (op == 0xFD) tag = " <-SETTIMG";
        else if (op == 0xF5) tag = " <-SETTILE";
        else if (op == 0xF3) tag = " <-LOADBLOCK";
        else if (op == 0xFF) tag = " <-SETCIMG";
        else if (op == 0xFC) tag = " <-SETCOMBINE";
        if (full_detail && (*tag || i < 64)) fprintf(stderr, "  %08X: %08X %08X op=%02X%s\n", a, w0, w1, op, tag);
        if (op == 0xB8u || op == 0xDFu) { if (dsp > 0) { a = dstack[--dsp]; continue; } break; }
        bool is_dl = ((op == 0x06u) && ((w0 & 0x00FEFFFFu) == 0u)) || (op == 0xDEu);
        if (is_dl && djmp > 0) {
            uint8_t br = (uint8_t)(w0 >> 16);
            uint32_t tgt = (w1 & 0x00FFFFF8u) | 0x80000000u;
            if (br == 0 && dsp < 16) dstack[dsp++] = a + 8u;
            a = tgt; djmp--; continue;
        }
        a += 8u;
    }
    if (tr_n == 0) tr_minx = tr_miny = tr_maxx = tr_maxy = 0;
    fprintf(stderr, "[frame-dl-dump call=%d] TEXRECT count=%d bbox px=(%d,%d)-(%d,%d) cimg=0x%08X\n",
            call, tr_n, tr_minx, tr_miny, tr_maxx, tr_maxy, last_cimg);
    fflush(stderr);
#else
    (void)rdram; (void)dl_phys;
#endif
}

// FB-guards bit 1 (default off): rewrite any G_SETCIMG whose 24-bit address falls outside the real
// framebuffer range to G_SPNOOP before RT64 parses the task. Real framebuffers all live at or above
// 0x804B7800; RDRAM ends at 0x80800000. Mutates the DL in RDRAM.
static constexpr uint32_t RS64_FB_MIN_ADDR = 0x80400000u;
static constexpr uint32_t RS64_FB_MAX_ADDR = 0x80800000u;

extern "C" void rs64_neutralize_matpool_cimg(uint8_t* rdram, uint32_t dl_phys) {
    rs64_dump_frame_dl(rdram, dl_phys);
    if (!(rs64_fb_guards_mask() & 1) || dl_phys == 0) return;

    uint32_t addr = (dl_phys & 0x3FFFFFFu) | 0x80000000u;
    uint32_t stack[16];
    int sp = 0;
    int jumps_left = 64;
    int cimg_seen = 0, cimg_killed = 0, cimg_passed = 0;
    static int s_logs = 0;
    static int s_summary_logs = 0;
    for (int i = 0; i < 4096; ++i) {
        if ((addr & 0x3FFFFFFu) + 8u > 0x800000u) break;
        uint32_t w0 = (uint32_t)MEM_W(0, (gpr)(int32_t)addr);
        uint32_t w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)addr);
        uint8_t  op = (uint8_t)(w0 >> 24);
        if (op == 0xBEu) { addr += 16u; continue; }   // 16B state cmd; payload is not a command
        if (op == 0xFFu) {
            cimg_seen++;
            uint32_t fmt = (w0 >> 21) & 0x7u;
            uint32_t img = (w1 & 0xFFFFFFu) | 0x80000000u;   // RDP addresses are 24-bit; F5 leaves garbage above
            if (fmt <= 4u && w1 != 0u && (img < RS64_FB_MIN_ADDR || img >= RS64_FB_MAX_ADDR)) {
                MEM_W(0, (gpr)(int32_t)addr) = w0 & 0x00FFFFFFu;
                cimg_killed++;
                if (s_logs++ < 16) {
                    fprintf(stderr, "[cimg-neutralize] killed G_SETCIMG @0x%08X img=0x%08X w=%u fmt=%u (%s fb_range=[0x%08X,0x%08X))\n",
                            addr, img, (w0 & 0xFFFu) + 1u, fmt, img < RS64_FB_MIN_ADDR ? "below" : "above",
                            RS64_FB_MIN_ADDR, RS64_FB_MAX_ADDR);
                    fflush(stderr);
                }
            } else if (fmt <= 4u && w1 != 0u) {
                cimg_passed++;
            }
        }
        if (op == 0xB8u || op == 0xDFu) {
            if (sp > 0) { addr = stack[--sp]; continue; }
            break;
        }
        // F5 reuses 0x06; it is G_DL only when w0 & 0x00FEFFFF == 0 (matches op_06_strict_dl).
        bool is_dl = ((op == 0x06u) && ((w0 & 0x00FEFFFFu) == 0u)) || (op == 0xDEu);
        if (is_dl && jumps_left > 0) {
            uint8_t branch = (uint8_t)(w0 >> 16);
            uint32_t target = (w1 & 0x00FFFFF8u) | 0x80000000u;
            if (branch == 0 && sp < 16) stack[sp++] = addr + 8u;
            addr = target;
            jumps_left--;
            continue;
        }
        addr += 8u;
    }
    if (s_summary_logs++ < 32 || cimg_killed > 0) {
        fprintf(stderr, "[cimg-walker] dl=0x%08X visited cimg=%d killed=%d passed=%d jumps_used=%d stack_max=%d\n",
                (dl_phys & 0x3FFFFFFu) | 0x80000000u, cimg_seen, cimg_killed, cimg_passed, 64 - jumps_left, sp);
        fflush(stderr);
    }
}

// ---- Asset overrides ----

// Files under <mod>/assets/<manifest path> (e.g. assets/pl_crafts/xwing_HOB) are appended past the ROM end; the game's RAM manifest points at them as raw entries.
// shortcut: folder mods only (not zipped), later mods win on the same path; read zips through the mod file handle if packaged asset mods appear.
namespace {
struct AssetOverride { uint32_t rom_off, size; };
std::unordered_map<std::string, AssetOverride> s_asset_overrides;
// Craft HOBs with recoloured exhaust, one per exhaust-color option (index 0 = default, unused).
std::unordered_map<std::string, std::vector<AssetOverride>> s_exhaust_variants;
constexpr const char* kExhaustMod = "exhaust-color";
constexpr uint32_t kExhaustTints[] = { 0, 0xFF2000, 0xFF8000, 0xFFE000, 0x20FF20, 0x20FFFF, 0x2060FF, 0xA040FF, 0xFF40C0, 0xFFFFFF };

uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

// Data-blob manifest walk, as tools/rs64-explorer openRom: path (without the segment name) -> decompressed bytes.
std::unordered_map<std::string, std::vector<uint8_t>> rom_assets(std::span<const uint8_t> rom, const std::function<bool(const std::string&)>& want) {
    std::unordered_map<std::string, std::vector<uint8_t>> out;
    constexpr uint32_t kBlobStart = 0x144340, kBlobData = kBlobStart + 0x40;
    if (rom.size() < kBlobData) return out;
    const uint32_t base = kBlobData + be32(&rom[kBlobStart + 0x1C]);
    const uint32_t manifest = base + be32(&rom[base]);
    const uint32_t count = (((uint32_t)rom[base + 6] << 8) | rom[base + 7]) / 0x20;
    std::vector<std::pair<std::string, uint32_t>> dirs;
    for (uint32_t i = 0; i < count && manifest + (i + 1) * 0x20 <= rom.size(); i++) {
        while (!dirs.empty() && i >= dirs.back().second) dirs.pop_back();
        const uint8_t* e = &rom[manifest + i * 0x20];
        const std::string name(reinterpret_cast<const char*>(e + 0x10), strnlen(reinterpret_cast<const char*>(e + 0x10), 16));
        std::string path;
        for (const auto& d : dirs) path += d.first + "/";
        path += name;
        if (e[0x0C] & 0x80) {
            dirs.emplace_back(name, i + ((((uint32_t)e[0x0E] << 8) | e[0x0F]) >> 5));
            continue;
        }
        if (!want(path)) continue;
        const uint32_t off = base + be32(e), size = be32(e + 4), csize = be32(e + 8);
        const uint32_t stored = csize == 0xFFFFFFFFu ? size : csize;
        if (off + stored > rom.size()) continue;
        std::vector<uint8_t> data(size);
        if (csize == 0xFFFFFFFFu) {
            std::memcpy(data.data(), &rom[off], size);
        } else if (tinfl_decompress_mem_to_mem(data.data(), size, &rom[off], stored, TINFL_FLAG_PARSE_ZLIB_HEADER) != size) {
            continue;
        }
        out.emplace(path, std::move(data));
    }
    return out;
}

// Tint the vertex colours of a HOB's eng_* parts (layout as tools/rs64-explorer parseHOB/parseFaces). The white core stays white: only the coloured part (max - min) takes the tint.
bool tint_exhaust(std::vector<uint8_t>& hob, uint32_t tint) {
    const size_t n = hob.size();
    auto ok = [&](size_t o, size_t len) { return o + len <= n; };
    if (!ok(0, 2)) return false;
    bool changed = false;
    const uint32_t objs = ((uint32_t)hob[0] << 8) | hob[1];
    for (uint32_t oi = 0; oi < objs && ok(8 + oi * 0x74, 0x74); oi++) {
        for (uint32_t p = be32(&hob[8 + oi * 0x74 + 0x2C]); p && ok(p, 12) && be32(&hob[p]); p += 12) {
            if (std::memcmp(&hob[p + 4], "eng_", 4) != 0) continue;
            const uint32_t part = be32(&hob[p]);
            if (!ok(part, 0x24)) continue;
            const uint32_t fg = be32(&hob[part + 0x20]);
            if (!ok(fg, 16)) continue;
            uint32_t o = be32(&hob[fg + 8]);
            for (uint32_t f = 0, nf = be32(&hob[fg + 12]); f < nf && ok(o, 20); f++) {
                const uint32_t flags = be32(&hob[o]), size = (hob[o + 4] & 0x3Fu) * 4u;
                const uint32_t nv = (flags & 8) ? 4 : 3;
                uint32_t c = o + 20 + ((be32(&hob[o + 4]) & 0x2000) ? 8 : 0);
                const uint32_t nc = (flags & 0x20) ? ((flags & 0x10) ? nv : 1) : 0;
                for (uint32_t k = 0; k < nc && ok(c, 4); k++, c += 4) {
                    uint8_t* rgb = &hob[c];
                    const int lo = std::min({ rgb[0], rgb[1], rgb[2] }), hi = std::max({ rgb[0], rgb[1], rgb[2] });
                    for (int ch = 0; ch < 3; ch++) rgb[ch] = (uint8_t)(lo + (hi - lo) * ((tint >> (16 - ch * 8)) & 0xFF) / 255);
                    changed = true;
                }
                if (!size) break;
                o += size;
            }
        }
    }
    return changed;
}

uint32_t exhaust_choice() {
    if (s_exhaust_variants.empty() || !recomp::mods::is_mod_enabled(kExhaustMod)) return 0;
    const recomp::mods::ConfigValueVariant v = recomp::mods::get_mod_config_value(kExhaustMod, "color");
    const uint32_t* idx = std::get_if<uint32_t>(&v);
    return idx ? *idx : 0;
}

uint32_t rd_w(const uint8_t* rdram, uint32_t a) {
    const uint32_t o = a - 0x80000000u;
    return ((uint32_t)rdram[(o + 0) ^ 3] << 24) | ((uint32_t)rdram[(o + 1) ^ 3] << 16) | ((uint32_t)rdram[(o + 2) ^ 3] << 8) | rdram[(o + 3) ^ 3];
}
void wr_w(uint8_t* rdram, uint32_t a, uint32_t v) {
    const uint32_t o = a - 0x80000000u;
    rdram[(o + 0) ^ 3] = (uint8_t)(v >> 24); rdram[(o + 1) ^ 3] = (uint8_t)(v >> 16);
    rdram[(o + 2) ^ 3] = (uint8_t)(v >> 8);  rdram[(o + 3) ^ 3] = (uint8_t)v;
}
}

// A file named <path>.bps is a BPS patch applied to the player's own copy of <path>, so mods built from game assets ship no game data.
extern "C" void rs64_asset_overrides_init() {
    std::vector<uint8_t> rom;
    auto append = [&rom](const std::vector<uint8_t>& data) {
        if (rom.empty()) rom.assign(recomp::get_rom().begin(), recomp::get_rom().end());
        rom.resize((rom.size() + 15) & ~size_t(15));
        const AssetOverride ov{ (uint32_t)rom.size(), (uint32_t)data.size() };
        rom.insert(rom.end(), data.begin(), data.end());
        return ov;
    };
    std::unordered_map<std::string, std::pair<std::string, std::vector<uint8_t>>> patches;
    for (const auto& d : recomp::mods::get_all_mod_details("rs64")) {
        if (!recomp::mods::is_mod_enabled(d.mod_id)) continue;
        const std::filesystem::path dir = recomp::mods::get_mod_filename(d.mod_id) / "assets";
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) continue;
        for (const auto& f : std::filesystem::recursive_directory_iterator(dir, ec)) {
            if (!f.is_regular_file()) continue;
            std::ifstream in(f.path(), std::ios::binary);
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (data.empty()) continue;
            std::string key = std::filesystem::relative(f.path(), dir).generic_string();
            if (f.path().extension() == ".bps") {
                key.resize(key.size() - 4);
                s_asset_overrides.erase(key);
                patches[key] = { d.mod_id, std::move(data) };
                continue;
            }
            patches.erase(key);
            s_asset_overrides[key] = append(data);
            fprintf(stderr, "[assets] %s overrides %s (%zu bytes)\n", d.mod_id.c_str(), key.c_str(), data.size());
        }
    }
    if (!patches.empty()) {
        for (const auto& [path, src] : rom_assets(recomp::get_rom(), [&](const std::string& p) { return patches.count(p) != 0; })) {
            const auto& [mod_id, bps] = patches[path];
            std::vector<uint8_t> out;
            const auto res = recomp::patcher::patch_rom(src, bps, out);
            if (res != recomp::patcher::PatcherResult::Success || out.empty()) {
                fprintf(stderr, "[assets] %s: patch for %s does not apply (%s)\n", mod_id.c_str(), path.c_str(), res == recomp::patcher::PatcherResult::WrongRom ? "made from a different asset" : "invalid patch");
                continue;
            }
            s_asset_overrides[path] = append(out);
            fprintf(stderr, "[assets] %s patches %s (%zu bytes)\n", mod_id.c_str(), path.c_str(), out.size());
        }
    }
    // Every colour variant is built up front so a change in the mod's config applies at the next level load.
    if (recomp::mods::is_mod_enabled(kExhaustMod)) {
        const auto is_craft = [](const std::string& p) { return p.rfind("pl_crafts/", 0) == 0 && p.size() > 4 && p.compare(p.size() - 4, 4, "_HOB") == 0; };
        for (auto& [path, hob] : rom_assets(recomp::get_rom(), is_craft)) {
            auto& variants = s_exhaust_variants[path];
            variants.resize(std::size(kExhaustTints));
            for (size_t t = 1; t < std::size(kExhaustTints); t++) {
                std::vector<uint8_t> data = hob;
                if (!tint_exhaust(data, kExhaustTints[t])) break;
                variants[t] = append(data);
            }
            if (!variants[1].size) s_exhaust_variants.erase(path);
        }
        fprintf(stderr, "[assets] exhaust colours for %zu craft models\n", s_exhaust_variants.size());
    }
    if (!rom.empty()) recomp::set_rom_contents(std::move(rom));
}

// findManifestEntryByName exit: slot idx of gManifestTable (0x80110A80, 0x50 each) has just had its manifest loaded.
extern "C" void rs64_asset_overrides_patch(uint8_t* rdram, uint32_t idx) {
    if ((s_asset_overrides.empty() && s_exhaust_variants.empty()) || idx >= 4) return;
    const uint32_t exhaust = exhaust_choice();
    const uint32_t slot = 0x80110A80u + idx * 0x50u;
    const uint32_t manifest = rd_w(rdram, slot + 0x40);
    const uint32_t block_rom = rd_w(rdram, slot + 0x44) & 0x0FFFFFFFu;
    const uint32_t count = rd_w(rdram, slot + 0x48);
    if ((manifest & 0xFF800000u) != 0x80000000u) return;
    std::vector<std::pair<std::string, uint32_t>> dirs;
    for (uint32_t i = 0; i < count; i++) {
        while (!dirs.empty() && i >= dirs.back().second) dirs.pop_back();
        const uint32_t e = manifest + i * 0x20u;
        char name[17] = {};
        for (int c = 0; c < 16; c++) name[c] = (char)rdram[(e + 0x10 + c - 0x80000000u) ^ 3];
        std::string path;
        for (const auto& d : dirs) path += d.first + "/";
        path += name;
        const uint32_t w3 = rd_w(rdram, e + 0x0C);
        if (w3 & 0x80000000u) {
            dirs.emplace_back(name, i + ((w3 & 0xFFFFu) >> 5));
            continue;
        }
        AssetOverride ov{};
        if (const auto it = s_asset_overrides.find(path); it != s_asset_overrides.end()) {
            ov = it->second;
        } else if (const auto ex = s_exhaust_variants.find(path); exhaust && ex != s_exhaust_variants.end() && exhaust < ex->second.size()) {
            ov = ex->second[exhaust];
        }
        if (!ov.size) continue;
        wr_w(rdram, e + 0x00, ov.rom_off - block_rom);
        wr_w(rdram, e + 0x04, ov.size);
        wr_w(rdram, e + 0x08, 0xFFFFFFFFu);
        wr_w(rdram, e + 0x0C, w3 & 0xFFFFu);
        fprintf(stderr, "[assets] slot %u: %s -> rom 0x%X\n", idx, path.c_str(), ov.rom_off);
    }
}
