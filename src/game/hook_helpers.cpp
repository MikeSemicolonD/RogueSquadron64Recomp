// Host entry points for the [[patches.hook]] entries in rogue_squadron.toml. Hook code is
// generated MIPS C with no host headers, so anything it needs from the host is extern "C" here.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <thread>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

#include "librecomp/game.hpp"
#include "recomp.h"
#include "rs64_toml_hooks.h"
#include "video/video_config.h"
#include "util/debug_logs.h"
#include "game/hook_helpers.h"      // declares the exports defined below (g_vi_tick, rs64_cine_iter_get, …)
#include "game/upstream_compat.h"   // rs64_fb_guards_mask
#include "game/game_state.h"
#include "game/lang/lang.h"
#include "util/mips_call.h"
#include "util/rdram_words.h"
#include "hle/rt64_rs64_hud.h"
#include "hle/rt64_rs64_transition.h"

using recomp::dbg::env_str;
using recomp::dbg::env_on;
using recomp::dbg::env_int;
using namespace rs64::mips;

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

extern "C" int rs64_static_terrain_mode(void);

// Draw distance multiplier for the camera far plane and object culls: the in-game DRAW DISTANCE setting (roguesq_video.json), live; ROGUESQ_DRAW_DIST overrides.
// Capped while the game draws its own terrain, whose grid breaks up further out.
static float draw_dist_setting() {

    static const float s_env = env_mult("ROGUESQ_DRAW_DIST", 0.0f);
    const float d = (s_env > 0.0f) ? s_env : rs64::video::draw_distance();

    return (rs64_static_terrain_mode() == 1) ? d : std::min(d, rs64::video::kLegacyTerrainDrawDistanceMax);
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

// Terrain renderer for RT64: 1 = the whole-map pass draws the HMP map and drops the game's terrain records (default), 0 = the game's own
// terrain ("legacyTerrain"), 2 = each record draws its cell from the map (validation). ROGUESQ_F5_STATIC_TERRAIN overrides the setting.
extern "C" int rs64_static_terrain_mode(void)
{

    static const int s_env = env_int("ROGUESQ_F5_STATIC_TERRAIN", -1);

    if (s_env >= 0) return s_env;

    return rs64::video::legacy_terrain() ? 0 : 1;
}

static bool terrain_pass_on() {
    return rs64_static_terrain_mode() == 1;
}

// Terrain grid reach, following the draw distance unless ROGUESQ_TERRAIN_DIST overrides. Capped at 2.5 (128-cell grid); stock reach while the
// terrain pass draws the map, since the grid's records are dropped.
extern "C" float rs64_terrain_dist(void)
{

    if (terrain_pass_on()) return 1.0f;

    return rs64::video::effective_draw_distance(terrain_dist_setting(), game_owns_view(), rs64_state_level());
}

// Fog start/end multiplier: the terrain reach, or the whole draw distance while the terrain pass draws the map.
extern "C" float rs64_fog_dist(void)
{

    if (terrain_pass_on()) return rs64_draw_dist();

    return rs64_terrain_dist();
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

    const uint32_t level = rw(rdram, 0x80130B70u);
    const uint32_t rgb = (rb(rdram, 0x8011A873u) << 16) | (rb(rdram, 0x8011A874u) << 8) | rb(rdram, 0x8011A875u);
    const int cine = rs64_state_in_cinematic();
    const float cur[6] = { rf(rdram, 0x8009DEACu), rf(rdram, 0x80130C54u), rf(rdram, 0x80130C58u), rf(rdram, 0x8011A878u), rf(rdram, 0x8011A87Cu), rs64_draw_dist() * 1000.0f + rs64_terrain_dist() };
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
// Stock under the terrain pass, whose grid keeps the stock reach: a bigger budget moves the game's heap allocations and so the game state.
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
    const uint32_t s_mult = cutscene_level ? 1u : (uint32_t)std::clamp(s_env > 0 ? s_env : ((!terrain_pass_on() && terrain_dist_setting() >= 2.0f) ? 2 : 1), 1, 2);
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
    static const bool s_map_clamp = env_on("ROGUESQ_TGRID_MAP_CLAMP", false);
    const float mapW = s_map_clamp ? (float)rh(rdram, 0x80136DF8u) : 0.0f, mapH = s_map_clamp ? (float)rh(rdram, 0x80136DFAu) : 0.0f;
    const float cx = rf(rdram, 0x80130C10u), cz = rf(rdram, 0x80130C18u);

    for (uint32_t i = 1; i <= 4; i++) {
        const uint32_t p = 0x80130C10u + i * 0xCu;
        float x = std::clamp(rf(rdram, p), cx - R, cx + R);
        float z = std::clamp(rf(rdram, p + 8), cz - R, cz + R);

        if (mapW > 1.0f && mapH > 1.0f) {
            x = std::clamp(x, 0.0f, mapW - 1.0f);
            z = std::clamp(z, 0.0f, mapH - 1.0f);
        }

        wf(rdram, p, x);
        wf(rdram, p + 8, z);
    }
}

// ---- Widescreen HUD placement ----

extern "C" float rs64_aspect_scale(void);

// Native pixels the edge HUD moves outward this frame in a view `width` wide: 0 outside missions (numMissionObjectives 0x80130B17), in 4:3 or at placement 0.
static float hud_shift(const uint8_t* rdram, float width)
{

    if (rb(rdram, 0x80130B17u) == 0) return 0.0f;

    return rs64hud::shiftNative(width, rs64::video::hud_placement(), rs64_aspect_scale());
}

// drawSubtitleText 0x80015EF8, before the element's first command goes to the display-list pointer it is already called at (0x8011DC58; s3 = element, s1 = view):
// the RT64 HUD marker naming its side goes first. Every element gets one, so a centred element clears the last side.
extern "C" void rs64_hud_mark_element(uint8_t* rdram, recomp_context* ctx)
{


    const float width = (float)(int32_t)rw(rdram, (uint32_t)ctx->r17 + 0x10u);

    if (hud_shift(rdram, width) <= 0.0f) return;

    const uint32_t item = (uint32_t)ctx->r19;
    const rs64hud::Side side = rs64hud::classify(item, rf(rdram, item + 0x18u), rw(rdram, item + 0x14u), width);
    const uint32_t p = rw(rdram, 0x8011DC58u);

    ww(rdram, p, rs64hud::markerW0(side));
    ww(rdram, p + 4u, 0);
    ww(rdram, 0x8011DC58u, p + 8u);
}

// refreshEffectObjectCamerasAndState 0x80053148: the HUD pass (s1 + 8 = its view, scene root = the radar node) with its camera just reset to identity.
// Moving the radar node by the distance RT64 gives the 2D pieces keeps the disc inside its ring.
static uint32_t s_radar_node = 0;
static uint32_t s_radar_restore = 0;
static uint32_t s_radar_restore_bits = 0;

extern "C" void rs64_hud_shift_radar(uint8_t* rdram, recomp_context* ctx)
{


    const uint32_t view = (uint32_t)ctx->r17 + 8u;
    const float width = (float)(int32_t)rw(rdram, view + 0x10u);
    const float height = (float)(int32_t)rw(rdram, view + 0x14u);
    const float shift = hud_shift(rdram, width);
    const uint32_t root = rw(rdram, view);
    const uint32_t div = rw(rdram, 0x8011A838u);

    if (shift <= 0.0f || height <= 0.0f || div == 0 || root < 0x80000000u || root >= 0x80800000u) return;

    // setupCameraMatrices' perspective: fovy at view + 0x20, aspect = W/H * [0x8011A844] / [0x8011A838] * [0x8011A840].
    const float aspect = width / height * (float)rw(rdram, 0x8011A844u) / (float)div * rf(rdram, 0x8011A840u);
    const float p00 = 1.0f / (std::tan(rf(rdram, view + 0x20u) * 3.14159265f / 360.0f) * aspect);

    if (!(p00 > 0.0f)) return;

    // The radar node is the pass's root, except while the pause emblem comes in or goes out: the root is then the emblem's scene, which still
    // draws the radar, so the last radar node is moved instead. The node, not the camera, moves, so the emblem stays centred.
    const float halfWidth = width * 0.5f;
    auto side_of = [&](uint32_t node) {

        const float z = rf(rdram, node + 0x48u);

        return z > 0.0f ? rs64hud::classify(node, p00 * rf(rdram, node + 0x40u) / z * halfWidth, 0, width) : rs64hud::None;
    };

    if (side_of(root) != rs64hud::None) s_radar_node = root;

    const rs64hud::Side side = s_radar_node != 0 ? side_of(s_radar_node) : rs64hud::None;

    if (side == rs64hud::None) return;

    // The node's translation in its 3x4 transform at +0x1C; put back by rs64_hud_radar_restore once the pass is drawn.
    const uint32_t tx = s_radar_node + 0x40u;
    const float dx = shift * rf(rdram, tx + 8u) / (p00 * halfWidth);
    const float moved = rf(rdram, tx) + ((side == rs64hud::Right) ? dx : -dx);

    s_radar_restore = tx;
    s_radar_restore_bits = rw(rdram, tx);
    wf(rdram, tx, moved);
}

// setupCameraMatrices after the scene traversal: the radar node's own translation back.
extern "C" void rs64_hud_radar_restore(uint8_t* rdram)
{

    if (s_radar_restore == 0) return;

    rs64::mips::ww(rdram, s_radar_restore, s_radar_restore_bits);
    s_radar_restore = 0;
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

// ROGUESQ_LOG_HOOKS=1 in a Debug build: hook code that gathers extra state for rs64_dbg_log4 checks this first.
extern "C" int rs64_dbg_hooks_on(void)
{

    static const bool on = RS64_DIAG && env_on("ROGUESQ_LOG_HOOKS");
    return on ? 1 : 0;
}

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
// render-context RDRAM-on-iter snapshot.
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

// Bumped once per host VI retrace (main.cpp).
extern "C" { volatile unsigned g_vi_tick = 0; }

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
