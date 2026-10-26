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
#include <unordered_set>
#include <array>
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
#include "video/video_config.h"
#include "util/debug_logs.h"
#include "game/hook_helpers.h"      // declares the exports defined below (g_vi_tick, rs64_cine_iter_get, …)
#include "game/upstream_compat.h"   // rs64_vi_driven, rs64_fb_guards_mask
#include "game/game_state.h"
#include "game/camera_shots.h"
#include "game/mod_levels.h"
#include "game/lang.h"
#include "game/level_rules.h"
#include "lockstep/lockstep_core.h"
#include "rs64/camera_shots.h"
#include "host_api/host_api.h"
#include <deque>
#include "util/mips_call.h"
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
static bool game_owns_view_setting()
{

    static const int s_env = env_int("ROGUESQ_CINE_DRAW_DIST", -1);

    return (s_env >= 0) ? (s_env != 0) : rs64::video::keep_cutscene_draw_distance();
}

static bool game_owns_view() {
    return game_owns_view_setting() && rs64_state_in_cinematic();
}

extern "C" float rs64_draw_dist(void)
{
    return rs64::video::effective_draw_distance(draw_dist_setting(), game_owns_view(), rs64_state_level());
}

// Terrain reach and fog multiplier, following the draw distance unless ROGUESQ_TERRAIN_DIST overrides. Capped at 2.5 (128-cell grid).
extern "C" float rs64_terrain_dist(void)
{
    return rs64::video::effective_draw_distance(terrain_dist_setting(), game_owns_view(), rs64_state_level());
}

// The game's text slot for the language setting: a built-in's own slot, 0 (English fallback) for a mod language, -1 when unset.
extern "C" int rs64_language(void)
{

    const std::string code = rs64::video::language();

    if (code.empty()) return -1;

    return rs64::lang::active() ? 0 : rs64::lang::builtin_slot(code);
}

extern "C" void rs64_set_language(int lang)
{

    const std::vector<std::string> codes = rs64::lang::codes();

    if (lang < 0 || lang >= (int)codes.size()) return;

    rs64::video::set_language(codes[lang]);

    fprintf(stderr, "[lang] language %d chosen in the menu\n", lang);
}

// ROGUESQ_LOG_FOG=1: level, far-range base D and its bounds, fog start/end/color the game set, cutscene flag and our multipliers, on change.
extern "C" void rs64_fog_probe(uint8_t* rdram)
{

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

extern "C" uint32_t rs64_tgrid_budget(uint32_t original)
{

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
extern "C" void rs64_terrain_grid_log(int32_t minX, int32_t width, int32_t minZ, int32_t height, uint32_t budget, uint32_t mapW, uint32_t mapH)
{

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
extern "C" void rs64_tgrid_clamp_polygon(uint8_t* rdram)
{

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

        if (v && strncmp(v, "level", 5) == 0)
        {
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
extern "C" void rs64_dbg_log4(const char* tag, unsigned a, unsigned b, unsigned c, unsigned d)
{

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

    (void)tag;
    (void)a;
    (void)b;
    (void)c;
    (void)d;

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
extern "C" void rs64_idle_pace(void)
{

#ifdef _WIN32

    static const int s_ms = [](){ int v = env_int("ROGUESQ_ATTRIBUTION_SLEEP_MS"); return v < 0 ? 0 : v > 5000 ? 5000 : v; }();

    if (s_ms) ::Sleep((DWORD)s_ms);

#endif
}

// ROGUESQ_CINE_TARGET_FPS=N paces cinematicLoopBody (default 0 when VI-driven, else 30).
// ROGUESQ_CINE_HOLD_ITER=N [+ ROGUESQ_CINE_HOLD_MS, default 15000]: park once at iteration N.
extern "C" void rs64_cine_pace(void)
{

#ifdef _WIN32

    static const long long s_hold_iter = env_str("ROGUESQ_CINE_HOLD_ITER") ? std::atoll(env_str("ROGUESQ_CINE_HOLD_ITER")) : -1;
    static const uint32_t s_hold_ms = (uint32_t)env_int("ROGUESQ_CINE_HOLD_MS", 15000);
    static bool s_held = false;

    if (s_hold_iter >= 0 && !s_held && (long long)rs64_cine_iter_get() >= s_hold_iter)
    {
        s_held = true;

        fprintf(stderr, "[cine-hold] freezing at iter %llu for %u ms\n", rs64_cine_iter_get(), s_hold_ms);
        fflush(stderr);
        uint64_t end = GetTickCount64() + s_hold_ms;

        while (GetTickCount64() < end) ::Sleep(100);

        fprintf(stderr, "[cine-hold] released\n");
        fflush(stderr);
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
extern "C" void rs64_attrib_wait_vi(void)
{

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
extern "C" uint32_t rs64_format_reply_slot(uint8_t* rdram, recomp_context* ctx, uint32_t src)
{

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

extern "C" void rs64_fb_cpu_cleared(uint8_t* /*rdram*/, uint32_t fb, uint32_t bytes)
{

    static const bool s_log = env_on("ROGUESQ_LOG_TRANSITION");
    g_rs64_cpu_cleared_wid.store(g_rs64_last_workload_id.load(std::memory_order_acquire), std::memory_order_release);
    g_rs64_cpu_cleared_fb.store(fb & 0x00FFFFFFu, std::memory_order_release);
    g_rs64_clear_bytes = bytes;
    g_rs64_clear_seq = g_rs64_clear_seq + 1;

    if (s_log)
    {
        fprintf(stderr, "[transition] cpu-clear fb=0x%08X bytes=0x%X\n", fb, bytes);
        fflush(stderr);
    }
}

// ---- Display-list walkers ----

// ROGUESQ_DUMP_FRAME_DL=N: dump the opcode stream of gfx tasks N..N+15 (per-command detail on N;
// TEXRECT count and bbox for all). ROGUESQ_DUMP_TEXTURES=1 also writes each SETTIMG source to
// dumps/tex/. Read-only.
extern "C" void rs64_dump_frame_dl(uint8_t* rdram, uint32_t dl_phys)
{

#if RS64_DIAG

    static const int s_target = env_int("ROGUESQ_DUMP_FRAME_DL", -1);
    static const bool s_dump_tex = env_on("ROGUESQ_DUMP_TEXTURES");
    static int s_call = 0;
    const int call = ++s_call;

    // ROGUESQ_MEDAL_PROBE=1: on EVERY gfx task, scan the DL for the menu medal/insignia
    // layer (fmt4 textures 0x62xxxx) and log the color image active at that draw. Catches
    // the intermittent overlay-refresh pass a fixed dump window misses, and resolves whether
    // the medal draws are emitted-to-a-bad-target vs never emitted (walk desync).
    static const int s_medal = env_on("ROGUESQ_MEDAL_PROBE") ? 1 : 0;

    if (s_medal && dl_phys != 0) {
        static uint8_t s_pv[0x800000 >> 3];
        memset(s_pv, 0, sizeof(s_pv));
        uint32_t pa = (dl_phys & 0x3FFFFFFu) | 0x80000000u;
        uint32_t p_cimg = 0, p_cimg_w0 = 0, p_cimg_at = 0;
        uint32_t pstack[16];
        int psp = 0;
        int pjmp = 256;

        for (int i = 0; i < 16384; ++i)
        {
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

                    if (++s_n <= 60)
                    {
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

                pa = tgt;
                pjmp--;
                continue;
            }

            pa += 8u;
        }
    }

    if (s_target < 0 || call < s_target || call >= s_target + 16 || dl_phys == 0) return;

    const bool full_detail = (call <= s_target + 1);

    uint32_t a = (dl_phys & 0x3FFFFFFu) | 0x80000000u;
    uint32_t dstack[16];
    int dsp = 0;
    int djmp = 256;
    int tr_n = 0;
    int tr_minx = 99999, tr_miny = 99999, tr_maxx = -99999, tr_maxy = -99999;
    uint32_t last_cimg = 0;

    // The menu DL re-enters the same tile sub-DLs; skip any command address seen before.
    static uint8_t s_vis[0x800000 >> 3];
    memset(s_vis, 0, sizeof(s_vis));

    fprintf(stderr, "[frame-dl-dump call=%d] start=0x%08X\n", call, a);

    for (int i = 0; i < 16384; ++i)
    {
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

            a += 24u;
            continue;
        }

        if (op == 0x03) { a += 24u; continue; }   // 8B cmd + 16B inline vertex

        if (op == 0xFD && s_dump_tex && full_detail)
        {
            uint32_t fmt = (w0 >> 21) & 0x7, siz = (w0 >> 19) & 0x3;
            uint32_t timg = w1 & 0x00FFFFFFu;
            char path[256];
            snprintf(path, sizeof(path), "dumps/tex/%06X_fmt%u_siz%u.bin", timg, fmt, siz);
            static int s_made = 0;

            if (!s_made) { s_made = 1; system("if not exist dumps\\tex mkdir dumps\\tex"); }

            FILE* tf = recomp::os::fopen(path, "wb");

            if (tf)
            {
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

            a = tgt;
            djmp--;
            continue;
        }

        a += 8u;
    }

    if (tr_n == 0) tr_minx = tr_miny = tr_maxx = tr_maxy = 0;

    fprintf(stderr, "[frame-dl-dump call=%d] TEXRECT count=%d bbox px=(%d,%d)-(%d,%d) cimg=0x%08X\n",
            call, tr_n, tr_minx, tr_miny, tr_maxx, tr_maxy, last_cimg);
    fflush(stderr);

#else

    (void)rdram;
    (void)dl_phys;

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

    for (int i = 0; i < 4096; ++i)
    {
        if ((addr & 0x3FFFFFFu) + 8u > 0x800000u) break;

        uint32_t w0 = (uint32_t)MEM_W(0, (gpr)(int32_t)addr);
        uint32_t w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)addr);
        uint8_t  op = (uint8_t)(w0 >> 24);

        if (op == 0xBEu) { addr += 16u; continue; }   // 16B state cmd; payload is not a command

        if (op == 0xFFu)
        {
            cimg_seen++;
            uint32_t fmt = (w0 >> 21) & 0x7u;
            uint32_t img = (w1 & 0xFFFFFFu) | 0x80000000u;   // RDP addresses are 24-bit; F5 leaves garbage above

            if (fmt <= 4u && w1 != 0u && (img < RS64_FB_MIN_ADDR || img >= RS64_FB_MAX_ADDR))
            {
                MEM_W(0, (gpr)(int32_t)addr) = w0 & 0x00FFFFFFu;
                cimg_killed++;

                if (s_logs++ < 16)
                {
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

    if (s_summary_logs++ < 32 || cimg_killed > 0)
    {
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
// Cart offsets of the silent 1-10 s voice streams level-rule messages play.
uint32_t s_silence_rom[10] = {};

// Craft HOBs with recoloured exhaust, one per exhaust-color option (index 0 = default, unused).
std::unordered_map<std::string, std::vector<AssetOverride>> s_exhaust_variants;
constexpr const char* kExhaustMod = "exhaust-color";
constexpr uint32_t kExhaustTints[] = { 0, 0xFF2000, 0xFF8000, 0xFFE000, 0x20FF20, 0x20FFFF, 0x2060FF, 0xA040FF, 0xFF40C0, 0xFFFFFF };

uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

// Data-blob manifest walk, as tools/web/rs64-explorer openRom: path (without the segment name) -> decompressed bytes.
std::unordered_map<std::string, std::vector<uint8_t>> rom_assets(std::span<const uint8_t> rom, const std::function<bool(const std::string&)>& want)
{

    std::unordered_map<std::string, std::vector<uint8_t>> out;
    constexpr uint32_t kBlobStart = 0x144340, kBlobData = kBlobStart + 0x40;

    if (rom.size() < kBlobData) return out;

    const uint32_t base = kBlobData + be32(&rom[kBlobStart + 0x1C]);
    const uint32_t manifest = base + be32(&rom[base]);
    const uint32_t count = (((uint32_t)rom[base + 6] << 8) | rom[base + 7]) / 0x20;
    std::vector<std::pair<std::string, uint32_t>> dirs;

    for (uint32_t i = 0; i < count && manifest + (i + 1) * 0x20 <= rom.size(); i++)
    {
        while (!dirs.empty() && i >= dirs.back().second) dirs.pop_back();

        const uint8_t* e = &rom[manifest + i * 0x20];
        const std::string name(reinterpret_cast<const char*>(e + 0x10), strnlen(reinterpret_cast<const char*>(e + 0x10), 16));
        std::string path;

        for (const auto& d : dirs) path += d.first + "/";

        path += name;

        if (e[0x0C] & 0x80)
        {
            dirs.emplace_back(name, i + ((((uint32_t)e[0x0E] << 8) | e[0x0F]) >> 5));
            continue;
        }

        if (!want(path)) continue;

        const uint32_t off = base + be32(e), size = be32(e + 4), csize = be32(e + 8);
        const uint32_t stored = csize == 0xFFFFFFFFu ? size : csize;

        if (off + stored > rom.size()) continue;

        std::vector<uint8_t> data(size);

        if (csize == 0xFFFFFFFFu)
        {
            std::memcpy(data.data(), &rom[off], size);
        } else if (tinfl_decompress_mem_to_mem(data.data(), size, &rom[off], stored, TINFL_FLAG_PARSE_ZLIB_HEADER) != size)
        {
            continue;
        }

        out.emplace(path, std::move(data));
    }

    return out;
}

// Tint the vertex colours of a HOB's eng_* parts (layout as tools/web/rs64-explorer parseHOB/parseFaces). The white core stays white: only the coloured part (max - min) takes the tint.
bool tint_exhaust(std::vector<uint8_t>& hob, uint32_t tint) {

    const size_t n = hob.size();
    auto ok = [&](size_t o, size_t len) { return o + len <= n; };

    if (!ok(0, 2)) return false;

    bool changed = false;
    const uint32_t objs = ((uint32_t)hob[0] << 8) | hob[1];

    for (uint32_t oi = 0; oi < objs && ok(8 + oi * 0x74, 0x74); oi++)
    {
        for (uint32_t p = be32(&hob[8 + oi * 0x74 + 0x2C]); p && ok(p, 12) && be32(&hob[p]); p += 12)
        {
            if (std::memcmp(&hob[p + 4], "eng_", 4) != 0) continue;

            const uint32_t part = be32(&hob[p]);

            if (!ok(part, 0x24)) continue;

            const uint32_t fg = be32(&hob[part + 0x20]);

            if (!ok(fg, 16)) continue;

            uint32_t o = be32(&hob[fg + 8]);

            for (uint32_t f = 0, nf = be32(&hob[fg + 12]); f < nf && ok(o, 20); f++)
            {
                const uint32_t flags = be32(&hob[o]), size = (hob[o + 4] & 0x3Fu) * 4u;
                const uint32_t nv = (flags & 8) ? 4 : 3;
                uint32_t c = o + 20 + ((be32(&hob[o + 4]) & 0x2000) ? 8 : 0);
                const uint32_t nc = (flags & 0x20) ? ((flags & 0x10) ? nv : 1) : 0;

                for (uint32_t k = 0; k < nc && ok(c, 4); k++, c += 4)
                {
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

uint32_t exhaust_choice()
{

    if (s_exhaust_variants.empty() || !recomp::mods::is_mod_enabled(kExhaustMod)) return 0;

    const recomp::mods::ConfigValueVariant v = recomp::mods::get_mod_config_value(kExhaustMod, "color");
    const uint32_t* idx = std::get_if<uint32_t>(&v);

    return idx ? *idx : 0;
}

uint32_t rd_w(const uint8_t* rdram, uint32_t a)
{

    const uint32_t o = a - 0x80000000u;

    return ((uint32_t)rdram[(o + 0) ^ 3] << 24) | ((uint32_t)rdram[(o + 1) ^ 3] << 16) | ((uint32_t)rdram[(o + 2) ^ 3] << 8) | rdram[(o + 3) ^ 3];
}
void wr_w(uint8_t* rdram, uint32_t a, uint32_t v) {

    const uint32_t o = a - 0x80000000u;
    rdram[(o + 0) ^ 3] = (uint8_t)(v >> 24);
    rdram[(o + 1) ^ 3] = (uint8_t)(v >> 16);
    rdram[(o + 2) ^ 3] = (uint8_t)(v >> 8);
    rdram[(o + 3) ^ 3] = (uint8_t)v;
}

// Manifests grown with mod-added files live in host RAM above the game's 8 MB, one 64 KB block per gManifestTable slot: {copy, game's buffer}.
constexpr uint32_t kManifestCopies = 0x80C00000u, kManifestCopySize = 0x10000u;
std::pair<uint32_t, uint32_t> s_manifest_copies[4];

// A manifest entry (0x20 bytes, 8 words): +0 offset in block, +4 size, +8 stored size (-1 raw), +0xC flags (0x80 directory) and, for a
// directory, its own entry plus everything inside in bytes at +0xE; +0x10 name[16].
struct ManifestNode {
    uint32_t w[8] = {};
    std::string name;
    std::vector<ManifestNode> kids;
    bool dir() const { return (w[3] & 0x80000000u) != 0; }
};

// Rewrites slot idx's manifest as a copy with the mod-added files inside their directories (missing directories created, every enclosing
// directory's size grown), then points the slot at the copy.
void add_manifest_files(uint8_t* rdram, uint32_t idx, uint32_t block_rom, const std::vector<std::pair<std::string, AssetOverride>>& files)
{

    const uint32_t slot = 0x80110A80u + idx * 0x50u, manifest = rd_w(rdram, slot + 0x40), count = rd_w(rdram, slot + 0x48);
    std::vector<ManifestNode> top;
    std::function<ManifestNode(uint32_t&)> read = [&](uint32_t& i)
    {

        ManifestNode n;

        for (int k = 0; k < 8; k++) n.w[k] = rd_w(rdram, manifest + i * 0x20u + k * 4u);

        for (int c = 0; c < 16; c++)
        {
            const char ch = (char)rdram[(manifest + i * 0x20u + 0x10 + c - 0x80000000u) ^ 3];

            if (!ch) break;

            n.name += ch;
        }

        const uint32_t end = i + ((n.w[3] & 0xFFFFu) >> 5);
        i++;

        if (n.dir()) {
            while (i < end && i < count) n.kids.push_back(read(i));
        }

        return n;
    };

    for (uint32_t i = 0; i < count;) top.push_back(read(i));

    auto set_name = [](ManifestNode& n) {

        char buf[16] = {};
        std::memcpy(buf, n.name.data(), std::min<size_t>(n.name.size(), 15));

        for (int k = 0; k < 4; k++) n.w[4 + k] = be32(reinterpret_cast<const uint8_t*>(buf) + k * 4);
    };

    for (const auto& [path, ov] : files)
    {
        std::vector<ManifestNode>* level = &top;
        size_t start = 0;

        for (size_t slash; (slash = path.find('/', start)) != std::string::npos; start = slash + 1)
        {
            const std::string part = path.substr(start, slash - start);
            auto it = std::find_if(level->begin(), level->end(), [&](const ManifestNode& n) { return n.dir() && n.name == part; });

            if (it == level->end()) {
                ManifestNode d;
                d.name = part;
                d.w[3] = 0x80000000u;
                set_name(d);
                level->push_back(std::move(d));
                it = level->end() - 1;
            }

            level = &it->kids;
        }

        ManifestNode f;
        f.name = path.substr(start);
        f.w[0] = ov.rom_off - block_rom;
        f.w[1] = ov.size;
        f.w[2] = 0xFFFFFFFFu;
        set_name(f);
        level->push_back(std::move(f));

        fprintf(stderr, "[assets] slot %u: adds %s -> rom 0x%X\n", idx, path.c_str(), ov.rom_off);
    }

    std::vector<std::array<uint32_t, 8>> out;
    size_t largest_dir = 0;

    std::function<void(ManifestNode&)> emit = [&](ManifestNode& n) {

        const size_t at = out.size();
        out.emplace_back();

        for (auto& k : n.kids) emit(k);

        if (n.dir())
        {
            largest_dir = std::max(largest_dir, (out.size() - at) * 0x20u);
            n.w[3] = (n.w[3] & 0xFFFF0000u) | (uint32_t)((out.size() - at) * 0x20u);
        }

        std::copy(std::begin(n.w), std::end(n.w), out[at].begin());
    };

    for (auto& n : top) emit(n);

    // A directory's byte size is 16 bits.
    if (out.size() * 0x20u > kManifestCopySize || largest_dir > 0xFFFFu)
    {
        fprintf(stderr, "[assets] slot %u: %zu manifest entries do not fit the 64 KB copy; mod-added files skipped\n", idx, out.size());

        return;
    }

    const uint32_t copy = kManifestCopies + idx * kManifestCopySize;

    for (size_t i = 0; i < out.size(); i++)
    {
        for (int k = 0; k < 8; k++) wr_w(rdram, copy + (uint32_t)i * 0x20u + k * 4u, out[i][k]);
    }

    if (manifest != copy) s_manifest_copies[idx] = { copy, manifest };

    wr_w(rdram, slot + 0x40, copy);
    wr_w(rdram, slot + 0x48, (uint32_t)out.size());
}

// assets/from.json: { "<new path>": "<existing asset it copies>", ... }, the same as one <new path>.from file per entry.
constexpr const char* kFromMap = "from.json";

std::vector<std::pair<std::string, std::string>> read_from_map(const std::filesystem::path& assets)
{

    std::vector<std::pair<std::string, std::string>> out;
    std::ifstream in(assets / kFromMap);

    if (!in) return out;

    const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);

    if (!j.is_object()) {
        fprintf(stderr, "[assets] %s: not a JSON object\n", (assets / kFromMap).generic_string().c_str());

        return out;
    }

    for (const auto& [k, v] : j.items()) {
        if (v.is_string()) out.emplace_back(k, v.get<std::string>());
    }

    return out;
}
}

// A file named <path>.bps is a BPS patch applied to the player's own copy of <path>, so mods built from game assets ship no game data.
extern "C" void rs64_asset_overrides_init()
{

    std::vector<uint8_t> rom;

    auto append = [&rom](const std::vector<uint8_t>& data) {

        if (rom.empty()) rom.assign(recomp::get_rom().begin(), recomp::get_rom().end());

        rom.resize((rom.size() + 15) & ~size_t(15));
        const AssetOverride ov{ (uint32_t)rom.size(), (uint32_t)data.size() };
        rom.insert(rom.end(), data.begin(), data.end());

        return ov;
    };

    std::unordered_map<std::string, std::pair<std::string, std::vector<uint8_t>>> patches;
    std::unordered_map<std::string, std::pair<std::string, std::string>> derived;

    for (const auto& d : recomp::mods::get_all_mod_details("rs64"))
    {
        if (!recomp::mods::is_mod_enabled(d.mod_id)) continue;

        const std::filesystem::path dir = recomp::mods::get_mod_filename(d.mod_id) / "assets";
        std::error_code ec;

        if (!std::filesystem::is_directory(dir, ec)) continue;

        for (const auto& f : std::filesystem::recursive_directory_iterator(dir, ec))
        {
            if (!f.is_regular_file()) continue;

            std::ifstream in(f.path(), std::ios::binary);
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

            if (data.empty()) continue;

            std::string key = std::filesystem::relative(f.path(), dir).generic_string();

            if (key == kFromMap) continue;

            // <path>.from names an existing asset that <path> is a copy of (patched by <path>.bps when present): a new asset made from game data without shipping it.
            if (f.path().extension() == ".from")
            {
                key.resize(key.size() - 5);
                std::string src(data.begin(), data.end());
                src.erase(std::remove_if(src.begin(), src.end(), [](char c) { return c == '\r' || c == '\n' || c == ' ' || c == '\t'; }), src.end());
                s_asset_overrides.erase(key);
                derived[key] = { d.mod_id, src };
                continue;
            }

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

        for (const auto& [key, src] : read_from_map(dir))
        {
            s_asset_overrides.erase(key);
            derived[key] = { d.mod_id, src };
        }
    }

    if (!derived.empty())
    {
        const auto sources = rom_assets(recomp::get_rom(), [&](const std::string& p)
        {
            return std::any_of(derived.begin(), derived.end(), [&](const auto& kv) { return kv.second.second == p; });
        });

        for (const auto& [path, from] : derived)
        {
            const auto& [mod_id, src_path] = from;
            const auto src = sources.find(src_path);

            if (src == sources.end())
            {
                fprintf(stderr, "[assets] %s: %s is made from %s, which the game does not have\n", mod_id.c_str(), path.c_str(), src_path.c_str());
                continue;
            }

            std::vector<uint8_t> out = src->second;

            if (const auto p = patches.find(path); p != patches.end())
            {
                std::vector<uint8_t> patched;

                if (recomp::patcher::patch_rom(src->second, p->second.second, patched) != recomp::patcher::PatcherResult::Success || patched.empty())
                {
                    fprintf(stderr, "[assets] %s: patch for %s does not apply to %s\n", mod_id.c_str(), path.c_str(), src_path.c_str());
                    patches.erase(p);
                    continue;
                }

                out = std::move(patched);
                patches.erase(p);
            }

            s_asset_overrides[path] = append(out);

            fprintf(stderr, "[assets] %s adds %s from %s (%zu bytes)\n", mod_id.c_str(), path.c_str(), src_path.c_str(), out.size());
        }
    }

    if (!patches.empty())
    {
        for (const auto& [path, src] : rom_assets(recomp::get_rom(), [&](const std::string& p) { return patches.count(p) != 0; }))
        {
            const auto& [mod_id, bps] = patches[path];
            std::vector<uint8_t> out;
            const auto res = recomp::patcher::patch_rom(src, bps, out);

            if (res != recomp::patcher::PatcherResult::Success || out.empty())
            {
                fprintf(stderr, "[assets] %s: patch for %s does not apply (%s)\n", mod_id.c_str(), path.c_str(), res == recomp::patcher::PatcherResult::WrongRom ? "made from a different asset" : "invalid patch");
                continue;
            }

            s_asset_overrides[path] = append(out);

            fprintf(stderr, "[assets] %s patches %s (%zu bytes)\n", mod_id.c_str(), path.c_str(), out.size());
        }
    }

    // A crawl's own files are also served inside the crawl scene's directories, where its lookups find them.
    std::vector<std::pair<std::string, AssetOverride>> served;

    for (const auto& [path, ov] : s_asset_overrides)
    {
        if (const std::string s = rs64levels::crawlServedPath(path); !s.empty()) served.emplace_back(s, ov);
    }

    for (const auto& [path, ov] : served) s_asset_overrides[path] = ov;

    // Every colour variant is built up front so a change in the mod's config applies at the next level load.
    if (recomp::mods::is_mod_enabled(kExhaustMod))
    {
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

    // Language mods: extended fonts get blank cells for their code page glyphs (rs64_lang_font_load raises the glyph count to match).
    if (rs64::lang::installed())
    {
        const auto& r = recomp::get_rom();
        const auto rom_word = [&r](uint32_t vram) { return be32(&r[vram - 0x80000400u + 0x1000u]); };
        std::unordered_map<std::string, uint16_t> cell_h;

        for (uint32_t id = 0; id < 12; id++)
        {
            const uint32_t obj = rom_word(0x8009FC10u + id * 4);
            const uint32_t name = rom_word(obj + 0x10) - 0x80000400u + 0x1000u;
            const std::string font(reinterpret_cast<const char*>(&r[name]), strnlen(reinterpret_cast<const char*>(&r[name]), 16));

            if (rs64::lang::extends_font(font)) cell_h["fonts/" + font] = (uint16_t)(rom_word(obj + 4) >> 16);
        }

        for (const auto& [path, data] : rom_assets(r, [&cell_h](const std::string& p) { return cell_h.count(p) != 0; }))
        {
            const std::vector<uint8_t> out = rs64::lang::extend_font_strip(data, cell_h[path], rs64::lang::kExtraGlyphs);

            if (out.empty()) continue;
            if (s_asset_overrides.count(path)) fprintf(stderr, "[lang] language glyphs replace a mod's %s\n", path.c_str());

            s_asset_overrides[path] = append(out);
        }

        fprintf(stderr, "[lang] %zu fonts extended for language mods\n", cell_h.size());
    }

    // Silent voice streams of 1-10 s for level-rule messages (a subtitle stays up while its line plays).
    for (int s = 1; s <= 10; s++) s_silence_rom[s - 1] = append(rs64rules::silentMort(s)).rom_off;

    if (!rom.empty()) recomp::set_rom_contents(std::move(rom));
}

// findManifestEntryByName exit: slot idx of gManifestTable (0x80110A80, 0x50 each) has just had its manifest loaded.
extern "C" void rs64_asset_overrides_patch(uint8_t* rdram, uint32_t idx)
{

    if ((s_asset_overrides.empty() && s_exhaust_variants.empty()) || idx >= 4) return;

    const uint32_t exhaust = exhaust_choice();
    const uint32_t slot = 0x80110A80u + idx * 0x50u;

    // Loaded slots have +0x4E = 1; findManifestEntryByName also reaches this exit (from 0x800048D0) with a free slot.
    if (rdram[(slot + 0x4E - 0x80000000u) ^ 3] != 1) return;

    const uint32_t manifest = rd_w(rdram, slot + 0x40);
    const uint32_t block_rom = rd_w(rdram, slot + 0x44) & 0x0FFFFFFFu;
    const uint32_t count = rd_w(rdram, slot + 0x48);

    if ((manifest & 0xFF800000u) != 0x80000000u) return;

    std::vector<std::pair<std::string, uint32_t>> dirs;
    std::unordered_set<std::string> seen, top_dirs;

    for (uint32_t i = 0; i < count; i++)
    {
        while (!dirs.empty() && i >= dirs.back().second) dirs.pop_back();

        const uint32_t e = manifest + i * 0x20u;
        char name[17] = {};

        for (int c = 0; c < 16; c++) name[c] = (char)rdram[(e + 0x10 + c - 0x80000000u) ^ 3];

        std::string path;

        for (const auto& d : dirs) path += d.first + "/";

        path += name;
        const uint32_t w3 = rd_w(rdram, e + 0x0C);

        if (w3 & 0x80000000u) {
            if (dirs.empty()) top_dirs.insert(name);

            dirs.emplace_back(name, i + ((w3 & 0xFFFFu) >> 5));
            continue;
        }

        seen.insert(path);
        AssetOverride ov{};

        if (const auto it = s_asset_overrides.find(path); it != s_asset_overrides.end())
        {
            ov = it->second;
        } else if (const auto ex = s_exhaust_variants.find(path); exhaust && ex != s_exhaust_variants.end() && exhaust < ex->second.size())
        {
            ov = ex->second[exhaust];
        }

        if (!ov.size) continue;

        wr_w(rdram, e + 0x00, ov.rom_off - block_rom);
        wr_w(rdram, e + 0x04, ov.size);
        wr_w(rdram, e + 0x08, 0xFFFFFFFFu);
        wr_w(rdram, e + 0x0C, w3 & 0xFFFFu);

        fprintf(stderr, "[assets] slot %u: %s -> rom 0x%X\n", idx, path.c_str(), ov.rom_off);
    }

    std::vector<std::pair<std::string, AssetOverride>> added;

    for (const auto& [path, ov] : s_asset_overrides)
    {
        if (!seen.count(path) && top_dirs.count(path.substr(0, path.find('/')))) added.emplace_back(path, ov);
    }

    if (!added.empty()) add_manifest_files(rdram, idx, block_rom, added);
}

// rs_free of a manifest copied to host RAM frees the game's own buffer instead (freeManifestSegmentAssets 0x80004A0C / 0x80004A80).
extern "C" uint32_t rs64_asset_manifest_to_free(uint32_t ptr) {

    for (auto& [copy, orig] : s_manifest_copies) {
        if (copy && ptr == copy) {
            copy = 0;

            return orig;
        }
    }

    return ptr;
}

// ---- Mid-mission camera shots: mods link a level trigger to an in-level camera item in camera_shots.txt ----
namespace {
std::vector<rs64shots::Link> s_shot_links;
rs64shots::Queue s_shot_queue;
uint32_t s_shot_frame = 0;

// Shots queued through the camera.shots service (another player's, in co-op); they never call the listener back.
std::deque<rs64shots::Shot> s_remote_shots;
rs64_shot_fn s_shot_listener = nullptr;
void* s_shot_listener_user = nullptr;

// Host RAM for the camera name the game's shot starter reads (above the manifest copies).
constexpr uint32_t kShotName = 0x80C40000u;
std::string rd_cstr(uint8_t* rdram, uint32_t a, size_t max) {

    std::string s;

    for (size_t i = 0; i < max; i++) {
        const char c = (char)rdram[(a + i - 0x80000000u) ^ 3];

        if (!c) break;

        s += c;
    }

    return s;
}
}

extern "C" const char* rs64_mod_levels_current_folder();

extern "C" void rs64_camera_shots_init() {

    for (const auto& d : recomp::mods::get_all_mod_details("rs64"))
    {
        if (!recomp::mods::is_mod_enabled(d.mod_id)) continue;

        std::ifstream in(recomp::mods::get_mod_filename(d.mod_id) / "camera_shots.txt", std::ios::binary);

        if (!in) continue;

        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const size_t before = s_shot_links.size();

        for (const auto& old : rs64shots::addLinks(s_shot_links, text))
        {
            fprintf(stderr, "[shots] %s: overrides the earlier link %s %s -> %s\n", d.mod_id.c_str(), old.level.c_str(), old.trigger.c_str(), old.camera.c_str());
        }

        fprintf(stderr, "[shots] %s: camera_shots.txt read (%zu links in all, %zu new)\n", d.mod_id.c_str(), s_shot_links.size(), s_shot_links.size() - before);
    }
}

extern "C" void rs64_camera_shots_mission_start(uint8_t* rdram)
{

    s_remote_shots.clear();

    if (s_shot_links.empty()) return;

    static const char* digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    const uint32_t level = rd_w(rdram, 0x80130B70u);
    std::string id;

    for (uint32_t v = level; ; v /= 36) {
        id.insert(id.begin(), digits[v % 36]);

        if (v < 36) break;
    }

    // A mod level (playing as its template) has its own links, under its folder name.
    const char* mod_folder = rs64_mod_levels_current_folder();
    s_shot_queue.reset(mod_folder ? std::string(mod_folder) : "lv_" + id);
    s_shot_frame = 0;
}

extern "C" void rs64_camera_shots_trigger(uint8_t* rdram, uint32_t item, uint32_t phase)
{

    if (s_shot_links.empty() || phase != 1 || (item & 0xFF800000u) != 0x80000000u) return;

    // Like the function itself, only subtypes 0x18 / 0x19 are trigger items. Loading turns the file's name offset (+0x0C) into a pointer.
    const uint32_t sub = rd_w(rdram, item) >> 16, name = rd_w(rdram, item + 0x0C);

    if ((sub != 0x18 && sub != 0x19) || (name & 0xFF800000u) != 0x80000000u) return;

    const std::string trigger = rd_cstr(rdram, name, 255);

    // Co-op: the multiplayer mod raises mp_trigger_for_peer while the host applies the other player's trigger.
    const bool for_peer = rs64::host::flag("mp_trigger_for_peer") != 0;

    if (s_shot_queue.fired(s_shot_links, trigger, for_peer)) fprintf(stderr, "[shots] %s: %s fired%s (mission frame %u)\n", s_shot_queue.level().c_str(), trigger.c_str(), for_peer ? " by the other player" : "", s_shot_frame);
}

// The DAT item named name (getLevelDatItemByName 0x80047B48, the name written to kShotName), or 0.
static uint32_t level_object(uint8_t* rdram, recomp_context* ctx, const std::string& name)
{

    const std::string n = name.substr(0, 255);

    for (size_t i = 0; i <= n.size(); i++) rdram[(kShotName + i - 0x80000000u) ^ 3] = (uint8_t)(i < n.size() ? n[i] : 0);

    const uint32_t item = rs64::mips::mips_call(rdram, ctx, 0x80047B48u, { kShotName }).v0;

    return (item & 0xFF800000u) == 0x80000000u ? item : 0;
}

// Destroys the level object named name as a lethal hit would; tag starts each log line.
static void destroy_level_object(uint8_t* rdram, recomp_context* ctx, const std::string& name, const std::string& tag)
{

    const uint32_t item = level_object(rdram, ctx, name);

    if (!item)
    {
        fprintf(stderr, "%s: no object %s; skipped\n", tag.c_str(), name.c_str());
        return;
    }

    const uint32_t slot = rd_w(rdram, item + 4) & 0xFFFFu, table = rd_w(rdram, 0x80130BB0u);
    const int32_t health = (int32_t)rd_w(rdram, item + 0x6C + (rd_w(rdram, 0x80137CE4u) & 3u) * 4);

    // The slot's NPC context (table + slot * 8) must name the same slot back (+0x16), as the co-op damage sync checks.
    const uint32_t npc = slot < 0x800u && table >= 0x80000000u && table < 0x80800000u ? rd_w(rdram, table + slot * 8) : 0;
    const bool consistent = npc >= 0x80000000u && npc < 0x807FFFC0u && (rd_w(rdram, npc + 0x14) & 0xFFFFu) == slot;
    const auto kind = rs64shots::destroyKind(health, slot, consistent, (rd_w(rdram, item + 0x4C) & 0x80000000u) != 0);

    if (kind == rs64shots::Destroy::Skip)
    {
        fprintf(stderr, "%s: %s is already destroyed or mid-teardown; skipped\n", tag.c_str(), name.c_str());
        return;
    }

    if (kind == rs64shots::Destroy::CutHealth)
    {
        // Not loaded (far from the player): dealDamagetoDatItem only cuts its stored health, so it streams in as rubble.
        rs64::mips::mips_call(rdram, ctx, 0x800C7390u, { kShotName, (uint32_t)health });

        fprintf(stderr, "%s: %s is not loaded; health cut, no explosion\n", tag.c_str(), name.c_str());
        return;
    }

    // dealDamagetoDatItem's lethal HIT (action 5, weapon 7, damage -1) through the unit's own handler, without its follow-up
    // lookup, which reads the freed slot after the kill. The name scratch holds the message.
    const uint32_t msg[3] = { 0xFFFFFFFFu, (7u << 16) | 0xFFFFu, 0u };

    for (int i = 0; i < 3; i++) *(uint32_t*)(rdram + kShotName + i * 4 - 0x80000000u) = msg[i];

    rs64::mips::mips_call(rdram, ctx, 0x8003E8DCu, { slot, 5u, kShotName });

    fprintf(stderr, "%s: destroyed %s\n", tag.c_str(), name.c_str());
}

extern "C" void rs64_camera_shots_frame(uint8_t* rdram, recomp_context* ctx)
{

    if (s_shot_links.empty() && s_remote_shots.empty()) return;

    s_shot_frame++;
    const bool busy = rs64shots::busy(rd_w(rdram, 0x8010C9E0u), rd_w(rdram, 0x8010CA1Cu));
    auto shot = s_shot_queue.take(busy);

    const auto put_name = [&](const std::string& name) {

        for (size_t i = 0; i <= name.size(); i++) rdram[(kShotName + i - 0x80000000u) ^ 3] = (uint8_t)(i < name.size() ? name[i] : 0);

        return rs64::mips::mips_call(rdram, ctx, 0x80047B48u, { kShotName }).v0;
    };

    if (shot && !shot->destroy.empty()) {
        for (const auto& obj : shot->destroy) destroy_level_object(rdram, ctx, obj, "[shots] " + s_shot_queue.level());

        return;
    }

    const bool local = shot.has_value();

    if (local && !rs64shots::playsHere(*shot))
    {
        // Personal, set off by the other player: theirs only.
        fprintf(stderr, "[shots] %s: %s handed to the other player\n", s_shot_queue.level().c_str(), shot->camera.c_str());

        if (s_shot_listener) s_shot_listener(shot->camera.c_str(), shot->trigger.c_str(), s_shot_listener_user);

        return;
    }

    if (!shot && !busy && !s_remote_shots.empty()) {
        shot = std::move(s_remote_shots.front());
        s_remote_shots.pop_front();
    }

    if (!shot) return;

    const std::string name = shot->camera.substr(0, 255);
    const uint32_t item = put_name(name);

    if ((item & 0xFF800000u) != 0x80000000u || !rs64shots::isCameraItem(rd_w(rdram, item)))
    {
        fprintf(stderr, "[shots] %s: no camera %s; dropped\n", s_shot_queue.level().c_str(), name.c_str());

        return;
    }

    rs64::mips::mips_call(rdram, ctx, 0x800C7884u, { kShotName });

    fprintf(stderr, "[shots] %s: playing %s (mission frame %u)%s\n", s_shot_queue.level().c_str(), name.c_str(), s_shot_frame, local ? "" : " for the other player");

    if (local && rs64shots::tellsPeer(*shot) && s_shot_listener) s_shot_listener(name.c_str(), shot->trigger.c_str(), s_shot_listener_user);
}

// ---- Mod levels: levels.json entries shown in SELECT LEVEL after the stock ones, played as their template level ----
namespace recomp { void* alloc(uint8_t* rdram, size_t size); }
namespace {
std::vector<rs64levels::ModLevel> s_mod_levels;
rs64levels::Entries s_level_entries;
bool s_level_select_open = false;

// The mod level being played (index into s_mod_levels), -1 for a stock level.
int s_current_mod_level = -1;

// Manifest paths the enabled mods ship under assets/ (files, .from and .bps), so a lookup is redirected only to a file that exists.
std::set<std::string> s_mod_level_files;

// Host RAM for a redirected asset name (after the mid-mission shot name).
constexpr uint32_t kLookupName = 0x80C40100u;

// The SELECT LEVEL cursor (menu overlay, u8 at 0x800CDAC1).
const rs64levels::ModLevel* entry_at_cursor(uint8_t* rdram)
{

    if (!s_level_select_open) return nullptr;

    const int i = rdram[(0x800CDAC1u - 0x80000000u) ^ 3] - s_level_entries.base;

    return i >= 0 && i < s_level_entries.count ? &s_mod_levels[i] : nullptr;
}

// ROGUESQ_MOD_LEVEL=<id>: that mod level plays whenever its template does (replays and BOOT_TARGET runs); -1 without it.
int forced_mod_level()
{

    static const char* forced = recomp::dbg::env_str("ROGUESQ_MOD_LEVEL");

    for (size_t k = 0; forced && k < s_mod_levels.size(); k++) {
        if (s_mod_levels[k].id == forced) return (int)k;
    }

    return -1;
}
void write_cstr(uint8_t* rdram, uint32_t addr, const std::string& s)
{
    for (size_t i = 0; i <= s.size(); i++) rdram[(addr + i - 0x80000000u) ^ 3] = (uint8_t)(i < s.size() ? s[i] : 0);
}
uint32_t level_str(uint8_t* rdram, const std::string& s)
{

    static std::unordered_map<std::string, uint32_t> cache;

    if (auto it = cache.find(s); it != cache.end()) return it->second;

    auto* p = (uint8_t*)recomp::alloc(rdram, s.size() + 1);

    if (!p) return 0;

    const uint32_t off = (uint32_t)(p - rdram);

    for (size_t i = 0; i <= s.size(); i++) rdram[(off + i) ^ 3] = (uint8_t)(i < s.size() ? s[i] : 0);

    return cache[s] = off + 0x80000000u;
}
}

extern "C" void rs64_rules_init();
extern "C" void rs64_crawls_init();
extern "C" void rs64_pause_point(const char* clock, uint32_t frame);

extern "C" void rs64_mod_levels_init() {

    for (const auto& d : recomp::mods::get_all_mod_details("rs64"))
    {
        if (!recomp::mods::is_mod_enabled(d.mod_id)) continue;

        // Every enabled mod's files count, so a crawl-only mod's own files are found too.
        std::ifstream in(recomp::mods::get_mod_filename(d.mod_id) / "levels.json", std::ios::binary);
        std::vector<std::string> errors;

        if (in)
        {
            for (auto& l : rs64levels::parseModLevels(d.mod_id, std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), errors))
            {
                fprintf(stderr, "[levels] %s: %s (\"%s\", plays as level %d, folder %s)\n", d.mod_id.c_str(), l.id.c_str(), l.title.c_str(), l.tmpl, l.folder.c_str());
                s_mod_levels.push_back(std::move(l));
            }
        }

        for (const auto& e : errors) fprintf(stderr, "[levels] %s\n", e.c_str());

        std::error_code ec;
        const auto assets = recomp::mods::get_mod_filename(d.mod_id) / "assets";

        for (auto it = std::filesystem::recursive_directory_iterator(assets, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            if (!it->is_regular_file()) continue;

            std::string p = std::filesystem::relative(it->path(), assets, ec).generic_string();

            if (p == kFromMap) continue;

            for (const char* ext : { ".from", ".bps" })
            {
                if (p.size() > strlen(ext) && p.compare(p.size() - strlen(ext), strlen(ext), ext) == 0) p.resize(p.size() - strlen(ext));
            }

            s_mod_level_files.insert(p);
        }

        for (const auto& [key, src] : read_from_map(assets)) s_mod_level_files.insert(key);
    }

    rs64_rules_init();
    rs64_crawls_init();
    s_current_mod_level = forced_mod_level();
}

// SELECT LEVEL opens (a2 = highest stock entry shown): mod levels take the next entries.
extern "C" void rs64_mod_levels_select_open(uint8_t*, recomp_context* ctx)
{

    if (s_mod_levels.empty()) return;

    s_level_entries = rs64levels::layout((int)ctx->r6, s_mod_levels.size());

    if (s_level_entries.count < (int)s_mod_levels.size()) fprintf(stderr, "[levels] %zu mod levels do not fit SELECT LEVEL; %d shown\n", s_mod_levels.size(), s_level_entries.count);

    ctx->r6 = s_level_entries.base + s_level_entries.count - 1;
    s_level_select_open = true;
}

// Confirmed (s0 = entry): a mod entry plays as its template; a stock one clears the mod level.
extern "C" void rs64_mod_levels_select_confirm(uint8_t*, recomp_context* ctx)
{

    s_current_mod_level = forced_mod_level();

    if (!s_level_select_open) return;

    const int i = (int)ctx->r16 - s_level_entries.base;

    if (i < 0 || i >= s_level_entries.count) return;

    s_current_mod_level = i;
    ctx->r16 = s_mod_levels[i].tmpl;

    fprintf(stderr, "[levels] playing %s (as level %d)\n", s_mod_levels[i].id.c_str(), s_mod_levels[i].tmpl);
}

extern "C" void rs64_mod_levels_select_close() {
    s_level_select_open = false;
}

// SELECT LEVEL text for the entry under the cursor: v0 = its title (desc 0) or description (desc 1).
extern "C" void rs64_mod_levels_text(uint8_t* rdram, recomp_context* ctx, int desc)
{

    const auto* l = entry_at_cursor(rdram);

    if (!l) return;

    const std::string& s = desc ? l->description : l->title;

    if (const uint32_t p = level_str(rdram, s.empty() ? " " : rs64levels::menuText(s))) ctx->r2 = (gpr)(int32_t)p;
}

// A per-level lookup made for a mod entry while SELECT LEVEL is open (preview image, craft icons) uses its template.
extern "C" void rs64_mod_levels_entry_level(uint32_t* level)
{

    if (!s_level_select_open) return;

    const int i = (int)*level - s_level_entries.base;

    if (i >= 0 && i < s_level_entries.count) *level = (uint32_t)s_mod_levels[i].tmpl;
}

// Available craft for level: a mod level's approved craft mask (a SELECT LEVEL mod entry, or the template id while one plays), else -1
// with a mod entry's index turned into its template's.
extern "C" int rs64_mod_levels_craft(uint32_t* level) {

    const rs64levels::ModLevel* l = nullptr;
    const int i = (int)*level - s_level_entries.base;

    if (s_level_select_open && i >= 0 && i < s_level_entries.count)
    {
        l = &s_mod_levels[i];
        *level = (uint32_t)l->tmpl;
    } else if (s_current_mod_level >= 0 && (int)*level == s_mod_levels[s_current_mod_level].tmpl)
    {
        l = &s_mod_levels[s_current_mod_level];
    }

    return l ? l->craftMask : -1;
}

// A mod level's own briefing recording (a MORT stream at assets/level/<folder>/brief) plays through its template's briefing voice id:
// s_brief_index is that voice's speech index while the recording waits to start, s_brief_rom the stream's cart offset.
int32_t s_brief_index = -1;
uint32_t s_brief_rom = 0;
bool s_brief_now = false;

// SELECT LEVEL briefing voice (v0): a mod entry's recording or chosen stock line, or 0xFFFF (an empty line list) for none.
extern "C" void rs64_mod_levels_voice(uint8_t* rdram, recomp_context* ctx)
{

    s_brief_index = -1;
    const auto* l = entry_at_cursor(rdram);

    if (!l) return;

    ctx->r2 = l->voice >= 0 ? (uint32_t)l->voice : 0xFFFFu;
    const auto it = s_asset_overrides.find("level/" + l->folder + "/brief");

    if (it == s_asset_overrides.end()) return;

    const uint32_t carrier = 0x279u + (uint32_t)l->tmpl;
    ctx->r2 = carrier;
    s_brief_index = (int16_t)MEM_H(0, (gpr)(int32_t)(0x8009FFE0u + carrier * 2u));
    s_brief_rom = it->second.rom_off;
}

// allocAndConfigureNewVoice 0x8009B114 (s3 = speech index, s2 = rate or 0 for the table's): the recording is 8 kHz.
extern "C" void rs64_mod_levels_brief_rate(recomp_context* ctx)
{

    s_brief_now = s_brief_index >= 0 && (int32_t)ctx->r19 == s_brief_index;

    if (s_brief_now) ctx->r18 = 8000;
}

// allocAndConfigureNewVoice 0x8009B1F4: a1 = the stream's table offset; the call's delay slot adds the speech base, so the recording's
// cart offset goes in relative to that base.
extern "C" void rs64_mod_levels_brief_stream(uint8_t* rdram, recomp_context* ctx) {

    if (!s_brief_now) return;

    s_brief_now = false;
    s_brief_index = -1;
    const uint32_t base = (uint32_t)MEM_W(0, (gpr)(int32_t)0x80154684u);
    ctx->r5 = (gpr)(int32_t)(s_brief_rom - (base & 0x0FFFFFFFu));
}

// Back in the menus (menuOverlayInit state; 3 = a mission's debrief, after which a death replays the level): no mod level is being
// played until SELECT LEVEL confirms one.
extern "C" void rs64_mod_levels_menu(int state) {
    if (state != 3) s_current_mod_level = forced_mod_level();
}

extern "C" int rs64_mod_levels_playing() {
    return s_current_mod_level >= 0;
}

// Crawl files from enabled mods (assets/crawl/<name>/crawl.json), in load order, and the one picked for the crawl now playing.
std::vector<rs64levels::Crawl> s_crawls;
const rs64levels::Crawl* s_playing_crawl = nullptr;

extern "C" void rs64_crawls_init()
{

    for (const auto& d : recomp::mods::get_all_mod_details("rs64"))
    {
        if (!recomp::mods::is_mod_enabled(d.mod_id)) continue;

        std::error_code ec;
        const auto dir = recomp::mods::get_mod_filename(d.mod_id) / "assets" / "crawl";

        for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
        {
            std::ifstream in(it->path() / "crawl.json", std::ios::binary);

            if (!in) continue;

            std::vector<std::string> errors;
            const std::string name = it->path().filename().string();
            auto c = rs64levels::parseCrawl(name, std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), errors);

            for (const auto& e : errors) fprintf(stderr, "[crawl] %s: %s\n", d.mod_id.c_str(), e.c_str());

            if (!c) continue;

            c->folder = "crawl/" + name;
            fprintf(stderr, "[crawl] %s: %s\n", d.mod_id.c_str(), name.c_str());
            s_crawls.push_back(std::move(*c));
        }
    }
}

// The opening crawl for level (getLevelIntroCrawlId's a0): -2 the game's own, -1 none, 0-3 the crawl to play (it picks the backdrop
// and fly-bys). A crawl file wins over the game's crawl; a mod level's inline crawl is its own.
extern "C" int rs64_mod_levels_crawl_id(int level)
{

    const auto* mod = s_current_mod_level >= 0 ? &s_mod_levels[s_current_mod_level] : nullptr;

    s_playing_crawl = rs64levels::pickCrawl(level, mod, s_crawls);

    int crawl = mod ? mod->crawl : rs64levels::kCrawlTemplate;

    if (s_playing_crawl)
    {
        fprintf(stderr, "[crawl] level %d plays %s\n", level, s_playing_crawl->name.c_str());
        crawl = s_playing_crawl->planet;
    } else if (mod && !mod->crawlName.empty())
    {
        static std::string s_warned;

        if (s_warned != mod->crawlName) fprintf(stderr, "[crawl] %s: no crawl \"%s\"; the template's plays\n", mod->id.c_str(), mod->crawlName.c_str());

        s_warned = mod->crawlName;
    }

    // A crawl plays now (the game's own for levels 0, 5, 10 and 15); the nav sequencer watches this flag.
    const bool stock = level == 0 || level == 5 || level == 10 || level == 15;
    rs64::host::set_flag("crawl", crawl >= 0 || (crawl == rs64levels::kCrawlTemplate && stock));

    return crawl;
}

// The crawl's text, written to host RAM (0x80C41000-0x80C41FFF); 0 when the game's own plays.
extern "C" uint32_t rs64_mod_levels_crawl_text(uint8_t* rdram)
{

    constexpr uint32_t kCrawlText = 0x80C41000u;

    if (s_playing_crawl)
    {
        write_cstr(rdram, kCrawlText, rs64levels::crawlString(*s_playing_crawl).substr(0, 0xFFF));
        return kCrawlText;
    }

    if (s_current_mod_level < 0 || s_mod_levels[s_current_mod_level].crawl < 0) return 0;

    write_cstr(rdram, kCrawlText, rs64levels::crawlString(s_mod_levels[s_current_mod_level]).substr(0, 0xFFF));

    return kCrawlText;
}

namespace {
// The playing crawl's keyed camera at runModelViewerLoop's clock (seconds at its sp + 0xD4); false for the game's own camera.
float crawl_clock(uint8_t* rdram, uint32_t loop_sp)
{

    const uint32_t bits = rd_w(rdram, loop_sp + 0xD4);
    float s;

    std::memcpy(&s, &bits, 4);

    return s;
}

bool crawl_camera(uint8_t* rdram, uint32_t loop_sp, float pos[3], float rot[3])
{
    return s_playing_crawl && rs64levels::crawlCameraAt(s_playing_crawl->camera, crawl_clock(rdram, loop_sp), pos, rot);
}

uint32_t float_bits(float v)
{

    uint32_t u;

    std::memcpy(&u, &v, 4);

    return u;
}
}

// setModelViewerObjectTransform entry (the crawl camera from its drop f12): a crawl file's camera keys build it instead, with the game's
// own calls (scale 0x800A7BC4 x it x 1, rotations x/y/z, translation) into the matrix at 0x800CFFB0 and its inverse at 0x800D039C.
extern "C" int rs64_crawl_camera(uint8_t* rdram, recomp_context* ctx)
{

    float pos[3], rot[3];

    rs64_pause_point("crawl", (uint32_t)(crawl_clock(rdram, (uint32_t)ctx->r29) * 30.0f));

    if (!crawl_camera(rdram, (uint32_t)ctx->r29, pos, rot)) return 0;

    static const rs64levels::Crawl* s_logged = nullptr;

    if (s_logged != s_playing_crawl) fprintf(stderr, "[crawl] %s: camera from its %zu keys\n", s_playing_crawl->name.c_str(), s_playing_crawl->camera.size());

    s_logged = s_playing_crawl;

    constexpr uint32_t kCam = 0x800CFFB0u, kView = 0x800D039Cu;
    const uint32_t scale = rd_w(rdram, 0x800A7BC4u);

    rs64::mips::mips_call(rdram, ctx, 0x8001CB64u, { kCam, scale, scale, float_bits(1.0f) });
    rs64::mips::mips_call(rdram, ctx, 0x8001CB9Cu, { kCam, float_bits(rot[0]) });
    rs64::mips::mips_call(rdram, ctx, 0x8001CCCCu, { kCam, float_bits(rot[1]) });
    rs64::mips::mips_call(rdram, ctx, 0x8001CDFCu, { kCam, float_bits(rot[2]) });
    rs64::mips::mips_call(rdram, ctx, 0x8001CA90u, { kCam, float_bits(pos[0]), float_bits(pos[1]), float_bits(pos[2]) });
    rs64::mips::mips_call(rdram, ctx, 0x8001C774u, { kView, kCam });
    ctx->r2 = (gpr)(int32_t)kCam;

    return 1;
}

// runModelViewerLoop 0x800CA080, after the pan sets the camera drop f26: the backdrop's rise follows the keyed camera's height.
extern "C" void rs64_crawl_drop(uint8_t* rdram, recomp_context* ctx)
{

    float pos[3], rot[3];

    if (crawl_camera(rdram, (uint32_t)ctx->r29, pos, rot)) ctx->f26.fl = pos[1];
}

extern "C" const char* rs64_mod_levels_current_folder()
{
    return s_current_mod_level >= 0 ? s_mod_levels[s_current_mod_level].folder.c_str() : nullptr;
}

// ---- Mod level medals: their own thresholds and their own record (roguesq_mod_medals.json), never the game's medals ----
namespace {
rs64levels::MedalStore s_mod_medals;
bool s_mod_medals_loaded = false;
bool s_debrief = false;
int s_earned_medal = -1;
uint32_t s_debrief_level = 0, s_saved_medal_info = 0;
uint8_t s_saved_medal = 0, s_saved_rank = 0;

// Host RAM for a mod level's three medal_info records (after the redirected asset name).
constexpr uint32_t kModMedals = 0x80C40200u;
constexpr const char* kModMedalsFile = "roguesq_mod_medals.json";

// Per profile: the account (0-2) the game plays with is the byte at 0x80130B44, the same number as its SELECT GAME slot.
std::string medal_key(int profile, const rs64levels::ModLevel& l) {
    return "p" + std::to_string(profile) + "/" + l.mod + "/" + l.id;
}
int current_profile(uint8_t* rdram) {
    return rdram[(0x80130B44u - 0x80000000u) ^ 3];
}
rs64levels::MedalStore& mod_medals() {

    if (!s_mod_medals_loaded)
    {
        std::ifstream in(kModMedalsFile, std::ios::binary);
        s_mod_medals.load(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
        s_mod_medals_loaded = true;
    }

    return s_mod_medals;
}
uint8_t& ram_b(uint8_t* rdram, uint32_t addr) {
    return rdram[(addr - 0x80000000u) ^ 3];
}
uint32_t& ram_w(uint8_t* rdram, uint32_t addr) {
    return *(uint32_t*)(rdram + addr - 0x80000000u);
}
}

// postLevelStatScreen entry (level): for a mod level, its thresholds (medal_info pointer at 0x800D03DC, indexed level * 3 + medal - 1)
// and its best medal (0x80130B1C[level], what "new medal" compares against) stand in for the template's until the debrief returns.
extern "C" void rs64_mod_levels_debrief_begin(uint8_t* rdram, uint32_t level)
{

    if (s_current_mod_level < 0) return;

    const auto& l = s_mod_levels[s_current_mod_level];
    s_debrief = true;
    s_earned_medal = -1;
    s_debrief_level = level & 0xFF;
    s_saved_medal_info = ram_w(rdram, 0x800D03DCu);
    s_saved_medal = ram_b(rdram, 0x80130B1Cu + s_debrief_level);
    s_saved_rank = ram_b(rdram, 0x80130B11u);

    if (l.hasMedals)
    {
        const auto rec = rs64levels::medalRecords(l);

        for (size_t i = 0; i < rec.size(); i++) ram_b(rdram, kModMedals + (uint32_t)i) = rec[i];

        // Bonuses are the level's secret pickups, which the template's mission script awards: keep its thresholds (u16 at +0xA).
        for (uint32_t t = 0; t < 3; t++)
        {
            for (uint32_t k = 0; k < 2; k++) ram_b(rdram, kModMedals + t * 0xCu + 0xAu + k) = ram_b(rdram, s_saved_medal_info + s_debrief_level * 0x24u + t * 0xCu + 0xAu + k);
        }

        ram_w(rdram, 0x800D03DCu) = kModMedals - s_debrief_level * 0x24u;
    }

    ram_b(rdram, 0x80130B1Cu + s_debrief_level) = (uint8_t)mod_medals().best(medal_key(current_profile(rdram), l));
}

extern "C" int rs64_mod_levels_debriefing() {
    return s_debrief;
}

extern "C" void rs64_mod_levels_medal_earned(int medal) {
    if (s_debrief) s_earned_medal = medal;
}

// The debrief header: the mod level's title.
extern "C" void rs64_mod_levels_title(uint8_t* rdram, recomp_context* ctx)
{

    if (!s_debrief) return;
    if (const uint32_t p = level_str(rdram, rs64levels::menuText(s_mod_levels[s_current_mod_level].title))) ctx->r2 = (gpr)(int32_t)p;
}

// The debrief returned: the template's medal_info, medal and rank come back; a better medal goes to the mod medal record.
extern "C" void rs64_mod_levels_debrief_end(uint8_t* rdram)
{

    if (!s_debrief) return;

    s_debrief = false;
    ram_w(rdram, 0x800D03DCu) = s_saved_medal_info;
    ram_b(rdram, 0x80130B1Cu + s_debrief_level) = s_saved_medal;
    ram_b(rdram, 0x80130B11u) = s_saved_rank;
    const auto& l = s_mod_levels[s_current_mod_level];

    fprintf(stderr, "[levels] %s debrief: medal %d (best %d)\n", l.id.c_str(), s_earned_medal, mod_medals().best(medal_key(current_profile(rdram), l)));

    if (s_earned_medal > 0 && mod_medals().record(medal_key(current_profile(rdram), l), s_earned_medal))
    {
        std::ofstream(kModMedalsFile, std::ios::binary) << mod_medals().save();
    }
}

// SELECT GAME (setupCraftAnimationFlagSet, after it drew the profile's medal totals as text group 8 entries 1-3; s1 = the slot record at
// 0x800CF180 + slot * 0x48, +0xC = account data or 0): the profile's mod level medals as a second set, entries 4-6, beside the game's.
extern "C" void rs64_mod_levels_select_game(uint8_t* rdram, recomp_context* ctx)
{

    if (s_mod_levels.empty()) return;

    const uint32_t rec = (uint32_t)ctx->r17;

    if (rec < 0x800CF180u || (rec - 0x800CF180u) % 0x48u || (rec - 0x800CF180u) / 0x48u >= 3 || !ram_w(rdram, rec + 0xC)) return;

    const int profile = (int)((rec - 0x800CF180u) / 0x48u);
    int count[4] = {};

    for (const auto& l : s_mod_levels) count[mod_medals().best(medal_key(profile, l)) & 3]++;

    constexpr uint32_t kIcons = 0x80C40300u, kCounts = 0x80C40320u, kColour = 0x80C40360u;
    write_cstr(rdram, kIcons, rd_cstr(rdram, 0x800A6964u, 31));
    char counts[64];
    snprintf(counts, sizeof(counts), "~%d\n\n~~~~~~~~~%d\n\n~~~~~~~~%d", count[3], count[2], count[1]);
    write_cstr(rdram, kCounts, counts);

    // Counts in light blue, so the mod set reads apart from the game's medals; icons untinted.
    ram_w(rdram, kColour) = 0x90C8FFFFu;
    ram_w(rdram, kColour + 4) = 0xFFFFFFFFu;
    constexpr int kDx = 64;
    const uint32_t scale = ram_w(rdram, 0x800A6970u);

    const struct { uint32_t entry, str; int x, y; uint32_t key, colour, scale; } e[3] = {
        { 4, kIcons, -0x74 + kDx, -0x7F, 0xF, kColour + 4, 0x3F800000u },
        { 5, kCounts, -0x7C + kDx, -0x7F, 0xA, kColour, scale },
        { 6, kCounts, -0x7B + kDx, -0x7D, 0xC, 0x800CC870u, scale },
    };

    for (const auto& x : e)
    {
        rs64::mips::mips_call(rdram, ctx, 0x80063CFCu, { 8, x.str, x.entry, (uint32_t)x.x }, { (uint32_t)x.y });
        const uint32_t elem = rs64::mips::mips_call(rdram, ctx, 0x80063C3Cu, { 8, x.entry }).v0;
        rs64::mips::mips_call(rdram, ctx, 0x800646F8u, { ram_w(rdram, 0x800CF078u), elem, x.key });
        rs64::mips::mips_call(rdram, ctx, 0x80063E54u, { 8, x.entry, x.colour });
        rs64::mips::mips_call(rdram, ctx, 0x80061F34u, { 8, x.entry, x.scale });
    }
}

// ---- Mod level objectives: a level with its own objectives runs none of its template's mission script ----
namespace {
float s_obj_elapsed = 0;
unsigned s_obj_done = 0, s_obj_failed = 0, s_obj_forced_done = 0, s_obj_forced_failed = 0;
const std::vector<rs64levels::Objective>* own_objectives() {

    if (s_current_mod_level < 0) return nullptr;

    const auto& o = s_mod_levels[s_current_mod_level].objectives;

    return o.empty() ? nullptr : &o;
}
}

// initializeObjectiveTracking entry: the pause list shows the level's objectives (missionObjectiveTextIds row non-zero, count, none
// hidden or complete). Returns 1 to skip the template's handlers.
extern "C" int rs64_mod_levels_objectives_init(uint8_t* rdram)
{

    const auto* objs = own_objectives();

    if (!objs) return 0;

    s_obj_elapsed = 0;
    s_obj_done = 0;
    s_obj_failed = 0;
    s_obj_forced_done = 0;
    s_obj_forced_failed = 0;
    const uint32_t row = 0x80109D2Cu + ram_w(rdram, 0x80130B70u) * 4u;

    for (uint32_t i = 0; i < rs64levels::kMaxObjectives; i++) ram_b(rdram, row + i) = i < objs->size() ? 1 : 0;

    // The pause screen reads a line aloud unless its byte in this table is 0xFF; the voice itself comes from rs64_mod_levels_pause_voice.
    const uint32_t voices = 0x80109D80u + ram_w(rdram, 0x80130B70u) * 4u;

    for (uint32_t i = 0; i < rs64levels::kMaxObjectives; i++)
    {
        const auto* o = i < objs->size() ? &(*objs)[i] : nullptr;
        const bool rec = o && !o->pauseRecording.empty() && s_asset_overrides.count("level/" + s_mod_levels[s_current_mod_level].folder + "/" + o->pauseRecording);

        ram_b(rdram, voices + i) = o && (o->pauseVoice >= 0 || rec) ? 0 : 0xFF;
    }

    ram_b(rdram, 0x80130B17u) = (uint8_t)objs->size();
    ram_b(rdram, 0x80130B16u) = 0;
    ram_b(rdram, 0x80130B15u) = 0;

    return 1;
}

// A voice id whose speech and text are entry 0 (silence, a blank subtitle): what recordings and messages play as.
static int mod_level_voice_carrier(uint8_t* rdram)
{

    for (uint32_t v = 1; v < 0x2F4; v++)
    {
        if (MEM_H(0, (gpr)(int32_t)(0x8009FFE0u + v * 2u)) == 0) return (int)v;
    }

    return rs64levels::kVoiceNone;
}

// The voice id to play for the mod level: its recording (level/<folder>/<recording>) when it ships one, else the stock voice; a
// recording streams through the carrier line, redirected like the briefing's.
static int mod_level_voice(uint8_t* rdram, int voice, const std::string& recording)
{

    const auto& l = s_mod_levels[s_current_mod_level];
    const auto rec = recording.empty() ? s_asset_overrides.end() : s_asset_overrides.find("level/" + l.folder + "/" + recording);

    if (rec == s_asset_overrides.end()) return voice;

    s_brief_index = 0;
    s_brief_rom = rec->second.rom_off;

    return mod_level_voice_carrier(rdram);
}

// A cut to the level's camera named camera (setHudFlagBit40AndStoreArg 0x800C7884), with the name kept at scratch: only when the
// level has it, since the cut reads the item without a check.
static void cut_to_camera(uint8_t* rdram, recomp_context* ctx, const std::string& camera, uint32_t scratch)
{

    write_cstr(rdram, scratch, camera.substr(0, 0x1F));

    if (rs64::mips::mips_call(rdram, ctx, 0x80047B48u, { scratch }).v0) rs64::mips::mips_call(rdram, ctx, 0x800C7884u, { scratch });
}

// An objective's callout, as the stock scripts make one: its voice line through playObjectiveVoiceLine2 0x800C7D1C (half a second
// late) and a cut to its DAT camera, which needs a name the level has.
static void objective_callout(uint8_t* rdram, recomp_context* ctx, uint32_t i)
{

    const auto& o = s_mod_levels[s_current_mod_level].objectives[i];
    const int voice = mod_level_voice(rdram, o.voice, o.recording);
    constexpr uint32_t kHalfSecond = 0x3F000000u;

    if (voice >= 0) rs64::mips::mips_call(rdram, ctx, 0x800C7D1Cu, { (uint32_t)voice, kHalfSecond });

    if (!o.camera.empty()) cut_to_camera(rdram, ctx, o.camera, 0x80C42100u + i * 0x20u);
}

// The per-level song table (0x8010B2EC, the song id per level startLevelBackgroundMusic plays); the original byte comes back at
// the next mission start.
namespace {
constexpr uint32_t kLevelSongs = 0x8010B2ECu;
int s_song_saved_level = -1;
uint8_t s_song_saved = 0;

void set_level_song(uint8_t* rdram, int song)
{

    const uint32_t level = ram_w(rdram, 0x80130B70u);

    if (s_song_saved_level < 0)
    {
        s_song_saved_level = (int)level;
        s_song_saved = ram_b(rdram, kLevelSongs + level);
    }

    ram_b(rdram, kLevelSongs + level) = (uint8_t)song;
}
}

// initMission entry, before the level's songs are preloaded: a mod level's starting song.
extern "C" void rs64_mod_levels_music(uint8_t* rdram)
{

    if (s_song_saved_level >= 0) ram_b(rdram, kLevelSongs + (uint32_t)s_song_saved_level) = s_song_saved;

    s_song_saved_level = -1;

    if (s_current_mod_level >= 0 && s_mod_levels[s_current_mod_level].music >= 0) set_level_song(rdram, s_mod_levels[s_current_mod_level].music);
}

// Pause screen 0x800C227C (initOrUpdatePauseScreenDim; s1 + 0x7C4 = the line it is reading aloud): the line's own voice. The call's
// delay slot adds 0x2C0 to a0 after this, so the id goes in less 0x2C0.
extern "C" void rs64_mod_levels_pause_voice(uint8_t* rdram, recomp_context* ctx)
{

    const auto* objs = own_objectives();

    if (!objs) return;

    const uint32_t i = ram_b(rdram, (uint32_t)ctx->r17 + 0x7C4u);

    if (i >= objs->size()) return;

    const auto& o = (*objs)[i];
    const int voice = mod_level_voice(rdram, o.pauseVoice, o.pauseRecording);

    if (voice >= 0) ctx->r4 = (gpr)(int32_t)(voice - 0x2C0);
}

// ---- Level rules: assets/level/<folder>/rules.json from enabled mods, run against the level being played ----
namespace {
std::map<std::string, std::vector<rs64rules::Rule>> s_rules_by_folder;
rs64rules::Runner s_rules;
std::string s_rules_folder;
std::string s_message_text;
float s_message_left = 0;
uint32_t s_message_text_id = 0xFFFFFFFFu;
bool s_rules_pending = false;
float s_rules_elapsed = 0;
constexpr uint32_t kRuleName = 0x80C42200u;

int rule_int(const rs64rules::Action& a, const char* key, int fallback = 0)
{
    return rs64rules::paramNumber(a, key, fallback);
}

std::string rule_str(const rs64rules::Action& a, const char* key)
{
    return rs64rules::paramText(a, key);
}

// An action a native mod registered with add_rule_action, given the rule's parameters as strings.
bool run_mod_rule_action(uint8_t* rdram, recomp_context* ctx, const rs64rules::Action& a)
{

    std::vector<std::string> values;
    std::vector<const char*> keys, vals;

    for (const auto& p : a.params) values.push_back(rule_str(a, p.key.c_str()));

    for (size_t i = 0; i < a.params.size(); i++)
    {
        keys.push_back(a.params[i].key.c_str());
        vals.push_back(values[i].c_str());
    }

    return rs64::host::run_rule_action(a.kind.c_str(), rdram, ctx, keys.data(), vals.data(), (int)keys.size());
}

void rules_run(uint8_t* rdram, recomp_context* ctx)
{

    const std::string tag = "[rules] " + s_rules_folder;

    for (const auto& a : s_rules.take())
    {
        const std::string& k = a.kind;

        fprintf(stderr, "%s: %s\n", tag.c_str(), k.c_str());

        if (k == "voice" || k == "recording")
        {
            const int voice = s_current_mod_level >= 0 ? mod_level_voice(rdram, rule_int(a, "id", -1), rule_str(a, "file")) : rule_int(a, "id", -1);

            if (voice >= 0) rs64::mips::mips_call(rdram, ctx, 0x800C7D1Cu, { (uint32_t)voice, 0u });
        }
        else if (k == "music")
        {
            // A new song in the level's table, and startLevelBackgroundMusic's once flag cleared so it stages that song again.
            set_level_song(rdram, rule_int(a, "song"));
            ram_b(rdram, 0x8010C424u) = 0;
        }
        else if (k == "win") rs64::mips::mips_call(rdram, ctx, 0x800C776Cu, {});
        else if (k == "fail") rs64::mips::mips_call(rdram, ctx, 0x800C7738u, {});
        else if (k == "objectiveDone" || k == "objectiveFail")
        {
            const int i = rule_int(a, "objective") - 1;

            if (own_objectives() && i >= 0 && i < (int)own_objectives()->size()) (k == "objectiveDone" ? s_obj_forced_done : s_obj_forced_failed) |= 1u << i;
        }
        else if (k == "flag" || k == "wave" || k == "spawn")
        {
            // A wave or spawn starts when its gate flag (written by the explorer) is raised.
            const int f = rule_int(a, "flag");

            if (f >= 1 && f <= 128) ram_b(rdram, rs64::ls::relocated(0x801388A0u) + (uint32_t)(f - 1)) = (uint8_t)rule_int(a, "value", 1);
        }
        else if (k == "counter")
        {
            const int c = rule_int(a, "counter");
            const uint32_t at = rs64::ls::relocated(0x80138060u) + (uint32_t)(c - 1) * 4u;

            if (c >= 1 && c <= 128) ram_w(rdram, at) = ram_w(rdram, at) + (uint32_t)rule_int(a, "add", 1);
        }
        else if (k == "message")
        {
            // A subtitle: a silent line (speech 0, blank text) streamed for seconds, its text swapped in by getVoiceText's hooks.
            const int seconds = std::clamp(rule_int(a, "seconds", 4), 1, 10);
            const int carrier = mod_level_voice_carrier(rdram);

            if (carrier < 0) continue;

            s_message_text = rs64levels::menuText(rule_str(a, "text")).substr(0, 0x17F);
            s_message_left = (float)seconds + 0.5f;
            s_brief_index = 0;
            s_brief_rom = s_silence_rom[seconds - 1];
            rs64::mips::mips_call(rdram, ctx, 0x800C7D1Cu, { (uint32_t)carrier, 0u });
        }
        else if (k == "destroy") destroy_level_object(rdram, ctx, rule_str(a, "unit"), tag);
        else if (k == "camera") cut_to_camera(rdram, ctx, rule_str(a, "camera"), kRuleName);
        else if (!run_mod_rule_action(rdram, ctx, a)) fprintf(stderr, "%s: no action %s; skipped\n", tag.c_str(), k.c_str());
    }
}
}

// Boot: every enabled mod's assets/level/<folder>/rules.json; later mods add to a level's list.
extern "C" void rs64_rules_init()
{

    for (const auto& d : recomp::mods::get_all_mod_details("rs64"))
    {
        if (!recomp::mods::is_mod_enabled(d.mod_id)) continue;

        std::error_code ec;
        const auto levels = recomp::mods::get_mod_filename(d.mod_id) / "assets" / "level";

        for (auto it = std::filesystem::directory_iterator(levels, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
        {
            std::ifstream in(it->path() / "rules.json", std::ios::binary);

            if (!in) continue;

            std::vector<std::string> errors;
            auto rules = rs64rules::parseRules(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), errors);
            const std::string folder = it->path().filename().string();

            for (const auto& e : errors) fprintf(stderr, "[rules] %s %s: %s\n", d.mod_id.c_str(), folder.c_str(), e.c_str());

            fprintf(stderr, "[rules] %s: %zu rules for %s\n", d.mod_id.c_str(), rules.size(), folder.c_str());

            auto& list = s_rules_by_folder[folder];
            list.insert(list.end(), rules.begin(), rules.end());
        }
    }
}

// Mission start (initMission): the level's rules start on the first mission frame, once its DAT is loaded to check names against.
extern "C" void rs64_rules_mission_start(uint8_t* rdram)
{

    const char* mod = rs64_mod_levels_current_folder();

    rs64::host::set_flag("crawl", 0);
    s_playing_crawl = nullptr;
    s_rules_folder = rs64rules::rulesFolder(mod ? mod : "", (int)ram_w(rdram, 0x80130B70u));
    s_rules_pending = s_rules_by_folder.count(s_rules_folder) != 0;
    s_rules_elapsed = 0;
    s_rules.reset({}, [](const std::string&) { return true; });

    // A message or mod event from the last mission does not carry into this one.
    s_message_text.clear();
    s_message_left = 0;
    rs64::host::take_rule_events();
}

// Each mission frame (runInMissionFrame).
extern "C" void rs64_rules_frame(uint8_t* rdram, recomp_context* ctx)
{

    if (s_rules_pending)
    {
        s_rules_pending = false;

        for (const auto& e : s_rules.reset(s_rules_by_folder[s_rules_folder], [&](const std::string& n) { return level_object(rdram, ctx, n) != 0; })) {
            fprintf(stderr, "[rules] %s: %s; skipped\n", s_rules_folder.c_str(), e.c_str());
        }
    }

    // shortcut: a fixed 1/30 s per mission frame (the game's cap); read the frame dt if 60 fps logic ever lands.
    constexpr float kFrame = 1.0f / 30.0f;

    s_rules_elapsed += kFrame;

    if (s_message_left > 0 && (s_message_left -= kFrame) <= 0) s_message_text.clear();

    const auto condition = [&](const rs64rules::Rule& r) {

        const uint32_t f = rs64::ls::relocated(0x801388A0u), c = rs64::ls::relocated(0x80138060u);

        if (r.when == rs64rules::When::FlagIs) return r.value >= 1 && r.value <= 128 && ram_b(rdram, f + (uint32_t)(r.value - 1)) != 0;

        if (r.when == rs64rules::When::CounterAtLeast) return r.value >= 1 && r.value <= 128 && (int32_t)ram_w(rdram, c + (uint32_t)(r.value - 1) * 4u) >= r.count;

        // getDatItemHealthPercentage 0x800C7C28 (100 when the name is missing).
        write_cstr(rdram, kRuleName, r.object.substr(0, 0x3F));

        return (int32_t)rs64::mips::mips_call(rdram, ctx, 0x800C7C28u, { kRuleName }).v0 < r.value;
    };

    for (const auto& [name, object] : rs64::host::take_rule_events()) s_rules.event({ rs64rules::When::Custom, object, "", name });

    s_rules.tick(kFrame, s_rules_elapsed, condition);
    rules_run(rdram, ctx);
}

// applyDatObjectiveTriggerEffect past its gates (a0 = the trigger item, a1 = 1 entering / 0 leaving).
extern "C" void rs64_rules_area(uint8_t* rdram, uint32_t item, uint32_t enter)
{

    const uint32_t name = rd_w(rdram, item + 0x0C);

    if ((item & 0xFF800000u) != 0x80000000u || (name & 0xFF800000u) != 0x80000000u) return;

    s_rules.event({ enter ? rs64rules::When::Enter : rs64rules::When::Leave, rd_cstr(rdram, name, 255) });
}

// A path event as an NPC reaches it (updateNpcEffectIntensity entry, a1 = the event's DAT record), by the event's name.
extern "C" void rs64_rules_path(uint8_t* rdram, uint32_t item)
{

    const uint32_t name = (item & 0xFF800000u) == 0x80000000u ? rd_w(rdram, item + 0x0C) : 0;

    if ((name & 0xFF800000u) == 0x80000000u) s_rules.event({ rs64rules::When::PathPoint, rd_cstr(rdram, name, 255) });
}

// A unit's death effect (before datItemSetObjectiveBooleanCount at 0x8006D6F0 / 0x80078CC8, v1 = its DAT record).
extern "C" void rs64_rules_destroyed(uint8_t* rdram, recomp_context* ctx)
{

    const uint32_t item = (uint32_t)ctx->r3, name = (item & 0xFF800000u) == 0x80000000u ? rd_w(rdram, item + 0x0C) : 0;

    if ((name & 0xFF800000u) == 0x80000000u) s_rules.event({ rs64rules::When::Destroyed, rd_cstr(rdram, name, 255) });
}

// getVoiceText entry (a0 = the text id) and before its return (0x80055A10, the text decrypted into voiceTxtString 0x80138E90):
// while a message shows, the blank line's text is the message.
extern "C" void rs64_rules_voice_text_id(recomp_context* ctx)
{
    s_message_text_id = (uint32_t)ctx->r4;
}

extern "C" void rs64_rules_voice_text(uint8_t* rdram)
{

    if (s_message_text.empty() || s_message_text_id != 0) return;

    write_cstr(rdram, rs64::ls::relocated(0x80138E90u), s_message_text);
}

extern "C" void rs64_rules_objective(int done, int index)
{
    s_rules.event({ done ? rs64rules::When::ObjectiveDone : rs64rules::When::ObjectiveFailed, "", "", "", index });
}

// checkComplexObjectives entry ($f12 = frame time): green each objective as it completes, then end the mission through the game's
// own setters (fail 0x800C7738 wins over success 0x800C776C). Returns 1 to skip the template's checks.
extern "C" int rs64_mod_levels_objectives_tick(uint8_t* rdram, recomp_context* ctx)
{

    const auto* objs = own_objectives();

    if (!objs) return 0;

    s_obj_elapsed += ctx->f12.fl;
    int32_t counts[128];
    uint8_t flags[128];
    const uint32_t c = rs64::ls::relocated(0x80138060u), f = rs64::ls::relocated(0x801388A0u);

    for (uint32_t i = 0; i < 128; i++) {
        counts[i] = (int32_t)ram_w(rdram, c + i * 4u);
        flags[i] = ram_b(rdram, f + i);
    }

    auto st = rs64levels::evaluateObjectives(*objs, counts, flags, s_obj_elapsed);
    unsigned completing = 0;

    for (uint32_t i = 0; i < objs->size(); i++)
    {
        const auto g = (*objs)[i].goal;

        if (g != rs64levels::Goal::Protect && g != rs64levels::Goal::TimeLimit) completing |= 1u << i;
    }

    // Level rules can complete or fail an objective themselves.
    st.done |= s_obj_forced_done;
    st.failed |= s_obj_forced_failed;
    st.fail = st.failed != 0;
    st.success = !st.fail && completing && (st.done & completing) == completing;

    if (st.success) st.done = (1u << objs->size()) - 1;

    for (uint32_t i = 0; i < objs->size(); i++)
    {
        if ((st.done & ~s_obj_done) & (1u << i)) rs64_rules_objective(1, (int)i + 1);

        if ((st.failed & ~s_obj_failed) & (1u << i)) rs64_rules_objective(0, (int)i + 1);
    }

    // The outcome first: its setter resets speech playback, so a callout queued before it in the same frame would be dropped.
    if (st.fail) rs64::mips::mips_call(rdram, ctx, 0x800C7738u, {});
    else if (st.success) rs64::mips::mips_call(rdram, ctx, 0x800C776Cu, {});

    for (uint32_t i = 0; i < objs->size(); i++)
    {
        if (!((st.done & ~s_obj_done) & (1u << i))) continue;

        rs64::mips::mips_call(rdram, ctx, 0x800C76B0u, { i + 1 });

        const auto g = (*objs)[i].goal;

        if (g != rs64levels::Goal::Protect && g != rs64levels::Goal::TimeLimit) objective_callout(rdram, ctx, i);
    }

    s_obj_done |= st.done;

    for (uint32_t i = 0; i < objs->size(); i++)
    {
        if ((st.failed & ~s_obj_failed) & (1u << i)) objective_callout(rdram, ctx, i);
    }

    s_obj_failed |= st.failed;

    return 1;
}

// calculateFriendliesSaved entry: a level with its own objectives counts none. Returns 1 to skip the template's count.
extern "C" int rs64_mod_levels_objectives_friendlies(uint8_t* rdram, recomp_context* ctx) {

    if (!own_objectives()) return 0;

    rs64::mips::mips_call(rdram, ctx, 0x800C77B0u, { 0 });

    return 1;
}

// Pause list 0x800C2F1C (v0 = the line's text, s4 -> its 0-based index): the level's own objective text.
extern "C" void rs64_mod_levels_objective_text(uint8_t* rdram, recomp_context* ctx)
{

    const auto* objs = own_objectives();

    if (!objs) return;

    const uint32_t i = (uint32_t)(int16_t)MEM_H(0, ctx->r20);

    if (i >= objs->size()) return;

    constexpr uint32_t kObjectiveText = 0x80C42000u;
    write_cstr(rdram, kObjectiveText + i * 0x40u, rs64levels::menuText((*objs)[i].text).substr(0, 0x3F));
    ctx->r2 = (gpr)(int32_t)(kObjectiveText + i * 0x40u);
}

// SELECT LEVEL medal for the entry at index: a mod entry's own best medal, -1 for a stock entry.
extern "C" int rs64_mod_levels_entry_medal(uint8_t* rdram, uint32_t index)
{

    if (!s_level_select_open) return -1;

    const int i = (int)index - s_level_entries.base;

    return i >= 0 && i < s_level_entries.count ? mod_medals().best(medal_key(current_profile(rdram), s_mod_levels[i])) : -1;
}

// find_manifest_entry: while a mod level plays, a lookup of a template file it ships returns the mod's copy (the name is rewritten
// into kLookupName); returns the name address to use.
extern "C" uint32_t rs64_mod_levels_lookup(uint8_t* rdram, uint32_t name)
{

    if ((s_current_mod_level < 0 && !s_playing_crawl) || (name & 0xFF800000u) != 0x80000000u) return name;

    const std::string cur = rd_cstr(rdram, name, 255);

    if (s_playing_crawl)
    {
        if (const auto own = rs64levels::crawlAsset(*s_playing_crawl, cur, s_mod_level_files))
        {
            write_cstr(rdram, kLookupName, *own);

            fprintf(stderr, "[crawl] %s -> %s\n", cur.c_str(), own->c_str());

            return kLookupName;
        }
    }

    if (s_current_mod_level < 0) return name;

    const std::string mod = rs64levels::remapPath(cur, s_mod_levels[s_current_mod_level], s_mod_level_files);

    if (mod.empty()) return name;

    write_cstr(rdram, kLookupName, mod);

    fprintf(stderr, "[levels] %s -> %s\n", cur.c_str(), mod.c_str());

    return kLookupName;
}

// The camera.shots service (include/rs64/camera_shots.h), published before mods init.
extern "C" void rs64_camera_shots_register_service() {

    static const rs64_camera_shots svc = {
        RS64_CAMERA_SHOTS_VERSION, (uint32_t)sizeof(rs64_camera_shots),
        [](const char* camera, const char* trigger) {
            if (!camera || !*camera || !trigger) return;

            if (!s_shot_queue.remote(s_shot_links, trigger))
            {
                fprintf(stderr, "[shots] %s: %s from the other player already played here\n", s_shot_queue.level().c_str(), trigger);

                return;
            }

            s_remote_shots.push_back(rs64shots::Shot{ camera, false, false, trigger });
        },
        [](rs64_shot_fn fn, void* user) {
            s_shot_listener = fn;
            s_shot_listener_user = user;
        },
    };

    rs64::host::add_service(RS64_CAMERA_SHOTS_SERVICE, &svc);
}
