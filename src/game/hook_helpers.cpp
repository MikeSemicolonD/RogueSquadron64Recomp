// Host entry points for the [[patches.hook]] entries in rogue_squadron.toml. Hook code is
// generated MIPS C with no host headers, so anything it needs from the host is extern "C" here.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <chrono>
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
#include "ultramodern/ultramodern.hpp"
#include "recomp.h"
#include "rs64_toml_hooks.h"
#include "video/video_config.h"
#include "util/debug_logs.h"
#include "game/hook_helpers.h"      // declares the exports defined below (g_vi_tick, rs64_cine_iter_get, …)
#include "game/upstream_compat.h"   // rs64_fb_guards_mask
#include "game/game_state.h"
#include "game/game_rate.h"
#include "host_api/host_api.h"
#include "lockstep/lockstep_core.h"
#include "game/mod_content/asset_overrides.h"
#include "game/mod_content/level_environment.h"
#include "game/lang/lang.h"
#include "util/mips_call.h"
#include "util/rdram_words.h"
#include "util/host_ram_map.h"
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

extern "C" float rs64_draw_dist_setting(void) {
    return draw_dist_setting();
}

static float terrain_dist_setting() {

    static const float s_env = env_mult("ROGUESQ_TERRAIN_DIST", 0.0f);

    return std::min((s_env > 0.0f) ? s_env : draw_dist_setting(), 2.5f);
}

extern "C" int rs64_crawl_playing(void);

// Cutscenes and the opening crawl keep the game's own view distance and fog per shot unless the player opts out (keepCutsceneDrawDistance;
// ROGUESQ_CINE_DRAW_DIST=0/1 overrides).
static bool game_owns_view_setting()
{

    static const int s_env = env_int("ROGUESQ_CINE_DRAW_DIST", -1);

    return (s_env >= 0) ? (s_env != 0) : rs64::video::keep_cutscene_draw_distance();
}

static float s_level_fog_frac = 0.9f;

extern "C" void rs64_level_fog_frac(float frac) {
    s_level_fog_frac = frac;
}

static std::atomic<int> s_level_fog_reach{(int)rs64env::FogReach::Auto};

extern "C" void rs64_level_fog_reach(int reach) {
    s_level_fog_reach = reach;
}

static bool game_owns_view() {
    return game_owns_view_setting() && (rs64_state_in_cinematic() || rs64_crawl_playing());
}

extern "C" float rs64_draw_dist(void)
{
    return rs64::video::effective_draw_distance(draw_dist_setting(), game_owns_view());
}

// Terrain renderer for RT64: 1 = the whole-map pass draws the HMP map and drops the game's terrain records (default), 0 = the game's own terrain ("legacyTerrain"),
// 2 = each record draws its cell from the map (validation). ROGUESQ_F5_STATIC_TERRAIN overrides.
extern "C" int rs64_static_terrain_mode(void)
{

    static const int s_env = env_int("ROGUESQ_F5_STATIC_TERRAIN", -1);

    if (s_env >= 0) return s_env;

    return rs64::video::legacy_terrain() ? 0 : 1;
}

extern "C" int rs64_terrain_tessellation(void)
{

    static const int s_env = env_int("ROGUESQ_F5_STATIC_TERRAIN_SUB", 0);

    if (s_env > 0) return std::min(s_env, rs64::video::kTerrainTessellationMax);

    return rs64::video::terrain_tessellation();
}

// The map pass drops land past the fog end, where the game draws no terrain and its sky sits below the map's horizon.
extern "C" int rs64_terrain_fog_clip(void) {
    return 1;
}

static bool terrain_pass_on() {
    return rs64_static_terrain_mode() == 1;
}

// Terrain grid reach, following the draw distance unless ROGUESQ_TERRAIN_DIST overrides. Capped at 2.5 (128-cell grid); stock reach while the
// terrain pass draws the map, since the grid's records are dropped.
extern "C" float rs64_terrain_dist(void)
{

    if (terrain_pass_on()) return 1.0f;

    return rs64::video::effective_draw_distance(terrain_dist_setting(), game_owns_view());
}

static std::atomic<uint32_t> s_level_fog_rgb{0};

// The fog colour the game set this frame (setFog bytes 0x8011A873..75), for the draws RT64 fogs itself.
extern "C" void rs64_fog_color_note(uint8_t* rdram) {
    s_level_fog_rgb = (rb(rdram, 0x8011A873u) << 16) | (rb(rdram, 0x8011A874u) << 8) | rb(rdram, 0x8011A875u);
}

extern "C" uint32_t rs64_level_fog_rgb(void) {
    return s_level_fog_rgb;
}

static float fog_reach() {
    return terrain_pass_on() ? rs64_draw_dist() : rs64_terrain_dist();
}

// A level whose fog starts early (Nonnah, 0.3 of the far plane), or one set to it, keeps its stock fog past the stock view and thins it out to the draw distance instead.
static bool fog_tail_on()
{

    const auto reach = (rs64env::FogReach)s_level_fog_reach.load();
    const bool thin = (reach == rs64env::FogReach::Thin) || ((reach == rs64env::FogReach::Auto) && (s_level_fog_frac < 0.85f));

    return thin && (fog_reach() > 1.0f);
}

// Fog start/end multiplier: the terrain reach, or the whole draw distance while the terrain pass draws the map. A level set to Stock holds its fog wall.
extern "C" float rs64_fog_dist(void)
{

    if (((rs64env::FogReach)s_level_fog_reach.load() == rs64env::FogReach::Stock) || fog_tail_on())
    {
        return 1.0f;
    }

    return fog_reach();
}

// The linear fog value at the scaled far plane, where the fog tail reaches full (0 = stock fog). Fog is linear in 1/depth from start f*e to end e.
extern "C" float rs64_fog_tail_end(void)
{

    if (!fog_tail_on()) return 0.0f;

    const float f = s_level_fog_frac;

    return (1.0f / f - 1.0f / fog_reach()) / (1.0f / f - 1.0f);
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
// Set around setupCutsceneLevel's loadLevelAssets call (the classifier may not report "cinematic" yet at load); cutscene levels keep the stock budget while cutscenes keep their own view, doubled on legacy terrain in widescreen, whose extra cells run the stock mesh pool dry.
static bool s_cutscene_level_load = false;
extern "C" void rs64_cutscene_level_load(int on) {
    s_cutscene_level_load = on != 0;
}

extern "C" uint32_t rs64_tgrid_budget(uint32_t original)
{

    static const int s_env = env_int("ROGUESQ_TGRID_BUDGET_MULT", 0);
    const bool cutscene_level = s_cutscene_level_load && game_owns_view_setting();
    const bool wide = !terrain_pass_on() && rs64_cull_widen() > 1.0f;
    const uint32_t s_mult = cutscene_level ? (wide ? 2u : 1u) : (uint32_t)std::clamp(s_env > 0 ? s_env : ((!terrain_pass_on() && terrain_dist_setting() >= 2.0f) ? 2 : 1), 1, 2);
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
static constexpr uint32_t RS64_TGRID_BASE = RS64_HR_TGRID;
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
    std::memset(rdram + (RS64_TGRID_BASE - 0x80000000u), 0, RS64_HR_TGRID_END - RS64_HR_TGRID);
}

// VR: a spin-out sets up the crash camera (spawnRandomizedEffectBurst: holds the current camera pose, looks at the craft) instead of a random orbit
// or the cockpit spinning with the craft. a0 of the hooked function ($s4) is the player.
extern "C" void rs64_vr_death_camera(uint8_t* rdram, recomp_context* ctx)
{

    constexpr uint32_t kCrashCamera = 0x800514A0u;

    if (!rs64_vr_active())
    {
        return;
    }

    mips_call(rdram, ctx, kCrashCamera, { (uint32_t)ctx->r20 });
}

// VR: the head can look anywhere, so the forward view trapezoid becomes a square around the camera cell (cx, cz) reaching as far as the trapezoid did.
// The corners keep the trapezoid's winding.
static void vr_terrain_square(uint8_t* rdram, float cx, float cz)
{

    float px[4], pz[4];
    float reach = 0.0f;
    float area = 0.0f;

    for (uint32_t i = 0; i < 4; i++) {
        const uint32_t p = 0x80130C10u + (i + 1) * 0xCu;

        px[i] = rf(rdram, p);
        pz[i] = rf(rdram, p + 8);
        reach = std::max(reach, std::max(std::fabs(px[i] - cx), std::fabs(pz[i] - cz)));
    }

    for (uint32_t i = 0; i < 4; i++) {
        const uint32_t j = (i + 1) % 4;

        area += px[i] * pz[j] - px[j] * pz[i];
    }

    const float sx[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
    const float sz[4] = { -1.0f, -1.0f, 1.0f, 1.0f };

    for (uint32_t i = 0; i < 4; i++) {
        const uint32_t k = (area < 0.0f) ? 3 - i : i;
        const uint32_t p = 0x80130C10u + (i + 1) * 0xCu;

        wf(rdram, p, cx + sx[k] * reach);
        wf(rdram, p + 8, cz + sz[k] * reach);
    }
}

// Keeps the terrain view box inside the 128-cell grid (far corners within 62 cells of the camera). ROGUESQ_TGRID_MAP_CLAMP=1 also clamps to the map bounds; off by default, since vanilla draws past the map edge by repeating edge tiles.
extern "C" void rs64_tgrid_clamp_polygon(uint8_t* rdram)
{

    constexpr float R = 62.0f;
    static const bool s_map_clamp = env_on("ROGUESQ_TGRID_MAP_CLAMP", false);
    const float mapW = s_map_clamp ? (float)rh(rdram, 0x80136DF8u) : 0.0f, mapH = s_map_clamp ? (float)rh(rdram, 0x80136DFAu) : 0.0f;
    const float cx = rf(rdram, 0x80130C10u), cz = rf(rdram, 0x80130C18u);

    if (rs64_vr_active())
    {
        vr_terrain_square(rdram, cx, cz);
    }

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

// Native pixels the edge HUD moves outward this frame in a view `width` wide: 0 outside missions, in 4:3 or at placement 0.
// The results screen and menus (menu overlay) keep numMissionObjectives 0x80130B17 set after the last mission, so the overlay decides too.
static float hud_shift(const uint8_t* rdram, float width)
{

    if (g_active_overlay != 0 || rb(rdram, 0x80130B17u) == 0) return 0.0f;

    return rs64hud::shiftNative(width, rs64::video::hud_placement(), rs64_aspect_scale());
}

// drawSubtitleText 0x80015EF8, before the element's first command goes to the display-list pointer it is already called at (0x8011DC58; s3 = element, s1 = view):
// the RT64 HUD marker naming its side goes first. Every element gets one, so a centred element clears the last side.
extern "C" void rs64_hud_mark_element(uint8_t* rdram, recomp_context* ctx)
{


    const float width = (float)(int32_t)rw(rdram, (uint32_t)ctx->r17 + 0x10u);
    const bool shifted = hud_shift(rdram, width) > 0.0f;

    // VR needs the marker in every display mode: it is how RT64 knows a texrect is HUD.
    if (!shifted && !rs64_vr_active()) return;

    const uint32_t item = (uint32_t)ctx->r19;
    const rs64hud::Side side = shifted ? rs64hud::classify(item, rf(rdram, item + 0x18u), rw(rdram, item + 0x14u), width) : rs64hud::None;
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

// A mod cutscene's fade in borrows the ending's full-screen overlay. It starts at zero speed so the game's own ramp, which would end the cutscene, leaves it alone.
static constexpr int kCutFadeFrames = 30;
static bool s_cut_fade_wanted = false;
static int s_cut_fade_left = 0;

extern "C" void rs64_cut_fade_request(void) { s_cut_fade_wanted = true; }
extern "C" int rs64_cut_fade_active(void) { return s_cut_fade_left > 0; }

extern "C" void rs64_cut_fade_tick(uint8_t* rdram, recomp_context* ctx)
{

    using namespace rs64::mips;

    static constexpr uint32_t kOverlay = 0x800B0934u;
    static constexpr uint32_t kOverlayTable = 0x800B0AFCu;

    if (s_cut_fade_wanted)
    {
        s_cut_fade_wanted = false;

        // Only the loop's frame holds the screen size at sp+0x18 that cinematicInitializer reads.
        if (!rw(rdram, kOverlay))
        {
            mips_call(rdram, ctx, 0x800AF408u, { rw(rdram, kOverlayTable), (uint32_t)ctx->r29 + 0x18u, 0u });
            s_cut_fade_left = kCutFadeFrames;
        }
    }

    const uint32_t overlay = rw(rdram, kOverlay);

    if (!s_cut_fade_left || !overlay)
    {
        s_cut_fade_left = 0;

        return;
    }

    --s_cut_fade_left;

    if (s_cut_fade_left)
    {
        wb(rdram, rw(rdram, overlay + 0xCu) + 0x2Fu, (uint8_t)(255 * s_cut_fade_left / kCutFadeFrames));

        return;
    }

    mips_call(rdram, ctx, 0x800AF60Cu, { rw(rdram, kOverlayTable), (uint32_t)ctx->r29 + 0x18u });
}

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
    constexpr uint32_t kSlots = 32, kSlotSize = 0x30, kRing = RS64_HR_REPLY_RING;
    static_assert(kRing + kSlots * kSlotSize <= RS64_HR_REPLY_RING_END, "reply ring outgrew its host RAM range");
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

// Runs addNpcToVisibilityBucket again with the display widening on, for an object the stock test rejected: it joins the draw list and its LOD updates, but the fields game logic reads go back to their stock values.
extern "C" int rs64_npc_draw_only_pass(uint8_t* rdram, recomp_context* ctx, const uint32_t* args)
{

    if (!(rs64_cull_widen() > 1.0f) && !(rs64_draw_dist() > 1.0f) && !(rs64_shadow_cull_margin() > 1.0f) && !(rs64_shadow_keep_dist() > 0.0f))
    {
        return 0;
    }

    const uint32_t obj = args[1];
    const uint32_t model = rw(rdram, obj);
    const uint16_t flags = rh(rdram, obj + 8u);
    const uint16_t even = rh(rdram, 0x801394E2u);
    const uint16_t odd = rh(rdram, 0x801394E4u);
    const uint16_t modelFlags = rh(rdram, model + 0x38u);
    const uint32_t modelFrame = rw(rdram, model + 0x3Cu);

    // Without the every-other-frame bits the object is tested this frame instead of reusing its last answer.
    wh(rdram, obj + 8u, flags & 0xFE7Fu);
    rs64_npc_draw_only_mark(1);
    const uint32_t drawn = rs64::mips::mips_call(rdram, ctx, 0x80057C8Cu, { args[0], args[1], args[2], args[3] }).v0;
    rs64_npc_draw_only_mark(0);

    wh(rdram, obj + 8u, flags);
    wh(rdram, 0x801394E2u, even);
    wh(rdram, 0x801394E4u, odd);
    wh(rdram, model + 0x38u, modelFlags);
    ww(rdram, model + 0x3Cu, modelFrame);

    return drawn != 0;
}

// Whether a hill stands between the camera and the world position at pos: the game's terrain height at points along the line. Debug only (ROGUESQ_LOG_POP).
static bool terrain_hides(uint8_t* rdram, recomp_context* ctx, uint32_t pos)
{

    const uint32_t cam = rw(rdram, 0x80138D1Cu);
    const float cx = rf(rdram, cam + 0x10u), cy = rf(rdram, cam + 0x14u), cz = rf(rdram, cam + 0x18u);
    const float ox = rf(rdram, pos), oy = rf(rdram, pos + 4u), oz = rf(rdram, pos + 8u);
    const uint32_t scratch = (uint32_t)ctx->r29 - 0x200u;

    const auto height = [&](float x, float z) {
        return rs64::mips::mips_call(rdram, ctx, 0x80068180u, { 0u, 0u, scratch, scratch + 0x40u }, {}, x, z).f0;
    };

    const float side = (cy >= height(cx, cz)) ? 1.0f : -1.0f;

    for (int i = 1; i < 15; ++i) {
        const float t = (float)i / 16.0f;

        if (side * ((cy + (oy - cy) * t) - height(cx + (ox - cx) * t, cz + (oz - cz) * t)) < 0.0f) return true;
    }

    return false;
}

namespace {
struct PopTrack {
    uint32_t lastDrawn = 0;
    uint32_t goneAt = 0;
    float nx = 0.0f;
    float ny = 0.0f;
};
}

// ROGUESQ_LOG_POP=1: an object still being processed that stops drawing away from the screen edges. Keyed by object, so naturally small.
extern "C" void rs64_pop_note(uint8_t* rdram, recomp_context* ctx, uint32_t obj, uint32_t pos, int drawn, int seen, float nx, float ny)
{

    static const bool s_on = env_on("ROGUESQ_LOG_POP");

    if (!s_on)
    {
        return;
    }

    static std::unordered_map<uint32_t, PopTrack> s_objects;

    constexpr float kInsideScreen = 0.85f;
    constexpr uint32_t kBackWindow = 900u;
    const uint32_t frame = rw(rdram, 0x8013889Cu);
    const uint32_t model = rw(rdram, obj);
    PopTrack& t = s_objects[obj];
    char name[17] = {};

    for (int i = 0; i < 16; ++i) name[i] = (char)rb(rdram, model + (uint32_t)i);

    if (drawn) {
        if (t.goneAt && ((frame - t.goneAt) <= kBackWindow)) {
            fprintf(stderr, "[pop] back %s obj=%08X frame=%u after %u\n", name, obj, frame, frame - t.goneAt);
        }

        t.goneAt = 0;
        t.lastDrawn = frame;

        if (seen) {
            t.nx = nx;
            t.ny = ny;
        }

        return;
    }

    const float edge = std::max(std::fabs(t.nx) / std::max(rs64_cull_widen(), 1.0f), std::fabs(t.ny));

    if (t.lastDrawn && !t.goneAt && ((frame - t.lastDrawn) <= 2u) && (edge < kInsideScreen) && !terrain_hides(rdram, ctx, pos)) {
        t.goneAt = frame;
        fprintf(stderr, "[pop] gone %s obj=%08X frame=%u at %.2f,%.2f\n", name, obj, frame, t.nx, t.ny);
    }
}

// Adds display list chunks in free host RAM to the game's free list when a mod replaces a player craft, whose model can outgrow the game's pool.
// Never for stock craft: effects spawn only while enough chunks are free, so more of them would change stock replays.
extern "C" void rs64_grow_dl_pool(uint8_t* rdram)
{

    static const bool s_on = [] {

        static constexpr const char* kCrafts[] = {
            "pl_crafts/xwing_HOB", "pl_crafts/ywing_HOB", "pl_crafts/awing_HOB", "pl_crafts/vwing_HOB", "pl_crafts/rmobil_HOB",
            "pl_crafts/snowspeeder_HOB", "pl_crafts/falcon_HOB", "imp_stuff/tieinter_HOB", "pl_crafts/t16_HOB", "pl_crafts/koelsch_HOB"
        };

        return std::any_of(std::begin(kCrafts), std::end(kCrafts), [](const char* path) { return rs64::assets::find(path) != nullptr; });
    }();

    if (!s_on)
    {
        return;
    }

    constexpr uint32_t kFreeHead = 0x801163B0u;
    constexpr uint32_t kStart = RS64_HR_DL_CHUNKS;
    constexpr uint32_t kChunk = 0x108u;
    constexpr uint32_t kCount = (RS64_HR_DL_CHUNKS_END - kStart) / kChunk;

    const auto rd = [&](uint32_t a) { return (uint32_t)MEM_W(0, (gpr)(int32_t)a); };
    const auto wr = [&](uint32_t a, uint32_t v) { MEM_W(0, (gpr)(int32_t)a) = (int32_t)v; };

    uint32_t head = rd(kFreeHead);

    for (uint32_t i = 0; i < kCount; ++i)
    {
        const uint32_t c = kStart + i * kChunk;

        wr(c, head);
        wr(c + 4, 0);

        if (head)
        {
            wr(head + 4, c);
        }

        head = c;
    }

    wr(kFreeHead, head);
    fprintf(stderr, "[dl-pool] added %u chunks at %08X\n", kCount, kStart);
}

// ---- Mission frame rate ----

namespace {
rs64::rate::State g_rate;
}

extern "C" int rs64_ls_session(void);

extern "C" float rs64_rate_scale(void) {
    return g_rate.scale;
}

extern "C" int rs64_rate_tick(void) {
    return g_rate.tick ? 1 : 0;
}

extern "C" float rs64_rate_drag(float k) {
    return rs64::rate::drag(k, g_rate.scale);
}

// An overlay load starts or ends a mission: back to the game's own 2 VIs per frame.
extern "C" void rs64_rate_reset(uint8_t* rdram) {

    g_rate = rs64::rate::State{};
    rs64::mips::wb(rdram, 0x80128EAFu, 2);
    ultramodern::set_vi_rate(60);
}

extern "C" uint32_t rs64_vi_rate(void) {
    return ultramodern::get_vi_rate();
}

static bool g_rate_frame_seen = false;

// Each submitted frame: a screen whose loop doesn't run the rate (menus, the pause menu) gets the game's own 30 fps back.
extern "C" void rs64_rate_submit(uint8_t* rdram) {

    if (!g_rate_frame_seen && (rs64::mips::rb(rdram, 0x80128EAFu) != 2 || ultramodern::get_vi_rate() != 60))
    {
        rs64_rate_reset(rdram);
    }

    g_rate_frame_seen = false;
}

// ROGUESQ_LOG_RATE: one line per game second with the mission frames in it and the wall time it took.
static void rate_log(float dt)
{

    static float s_total = 0.0f;
    static float s_sec = 0.0f;
    static int s_frames = 0;
    static int s_one_vi = 0;
    static auto s_wall = std::chrono::steady_clock::now();

    s_sec += dt;
    s_frames++;
    s_one_vi += dt < 0.02f ? 1 : 0;

    if (s_sec < 1.0f)
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(now - s_wall).count();

    s_total += s_sec;

    fprintf(stderr, "[rate] t=%.1f frames=%d one_vi=%d wall_ms=%d scale=%.2f\n", s_total, s_frames, s_one_vi, ms, g_rate.scale);

    s_sec = 0.0f;
    s_frames = 0;
    s_one_vi = 0;
    s_wall = now;
}

// ROGUESQ_FIXED_STEP=1: game time advances one paced frame per frame instead of following real time (timeSnapshotFiller hook), so frame numbers repeat run to run.
extern "C" int rs64_fixed_step(void)
{

    static const bool s_on = env_on("ROGUESQ_FIXED_STEP");

    return s_on ? 1 : 0;
}

// Each mission frame, once its dt is final; returns the dt the frame uses. 0x80128EAF is the game's minimum VIs per frame: 2 = 30 fps.
// Above 30 the video clock ticks several times per frame, so a late frame waits a fraction of a frame for its swap instead of a whole one.
extern "C" float rs64_rate_frame(uint8_t* rdram, float dt)
{

    static const int s_env = env_int("ROGUESQ_GAME_HZ", 0);
    static const int s_sub = std::clamp(env_int("ROGUESQ_VI_SUBTICKS", 4), 1, 8);
    static const bool s_log = env_on("ROGUESQ_LOG_RATE");
    static auto s_last = std::chrono::steady_clock::now();

    g_rate_frame_seen = true;

    const int display_hz = (int)ultramodern::get_display_refresh_rate();
    const int vr_hz = rs64_vr_game_hz();
    const int hz = s_env > 0 ? std::clamp(s_env, 30, 240) : (vr_hz > 0 ? std::clamp(vr_hz, 30, 240) : rs64::rate::logic_hz(rs64::video::frame_interpolation_on(), rs64::video::target_fps(display_hz)));
    const bool on = hz > 30 && !rs64_ls_session() && !rs64::ls::demo_active(rdram) && rs64::host::flag("ghost_mode") == 0;
    const int sub = on ? s_sub : 1;
    const auto now = std::chrono::steady_clock::now();
    const float real = std::chrono::duration<float>(now - s_last).count();

    s_last = now;

    // With several ticks per frame the game's dt mispredicts the swap tick and makes it up a frame later, and the wall-clock interval jitters: both judder fast motion.
    // A smoothed interval plus the time it still owes keeps steps even and game time on real time.
    static float s_smooth = 0.0f;
    static float s_debt = 0.0f;

    if (sub > 1 && real > 0.0f && real <= 0.1f && !rs64_fixed_step())
    {
        s_smooth = s_smooth > 0.0f ? s_smooth + (real - s_smooth) * 0.1f : real;
        s_debt = std::clamp(s_debt + real, -0.05f, 0.05f + s_smooth);
        dt = std::clamp(s_smooth + (s_debt - s_smooth) * 0.05f, 0.25f * s_smooth, 4.0f * s_smooth);
        s_debt -= dt;
    }
    else
    {
        s_smooth = 0.0f;
        s_debt = 0.0f;
    }

    rs64::rate::update(g_rate, dt, on);
    rs64::mips::wb(rdram, 0x80128EAFu, on ? (uint8_t)sub : 2);
    ultramodern::set_vi_rate(on ? (uint32_t)(hz * sub) : 60u);

    if (s_log) {
        rate_log(dt);
    }

    return dt;
}

// A player's A/START ended the crawl (the toml hook at the game's skip branch): the host flag counts them, which co-op reads.
extern "C" void rs64_scene_skipped(const char* flag)
{
    rs64::host::set_flag(flag, rs64::host::flag(flag) + 1);
}
