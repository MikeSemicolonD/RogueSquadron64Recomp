// Host entry points for the [[patches.hook]] entries in rogue_squadron.toml. Hook code is
// generated MIPS C with no host headers, so anything it needs from the host is extern "C" here.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

#include "recomp.h"
#include "librecomp/mods.hpp"
#include "video_config.h"
#include "debug_logs.h"
#include "hook_helpers.h"      // declares the exports defined below (g_vi_tick, rs64_cine_iter_get, …)
#include "upstream_compat.h"   // rs64_vi_driven, rs64_fb_guards_mask

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
extern "C" float rs64_draw_dist(void) {
    static const float s_env = env_mult("ROGUESQ_DRAW_DIST", 0.0f);
    return (s_env > 0.0f) ? s_env : rs64::video::draw_distance();
}

// Terrain reach and fog multiplier, following the draw distance unless ROGUESQ_TERRAIN_DIST overrides. Capped at 2.5 (128-cell grid).
extern "C" float rs64_terrain_dist(void) {
    static const float s_env = env_mult("ROGUESQ_TERRAIN_DIST", 0.0f);
    return std::min((s_env > 0.0f) ? s_env : rs64_draw_dist(), 2.5f);
}

// Level terrain cell budget scale, evaluated at each level load: ROGUESQ_TGRID_BUDGET_MULT (1-2), default 2 when terrain distance is 2x or more.
// 3x exhausts the game heap and the level load stalls; N is also capped at 6553 (u16 vertex-block indices up to 10N-4).
extern "C" uint32_t rs64_tgrid_budget(uint32_t original) {
    static const int s_env = env_int("ROGUESQ_TGRID_BUDGET_MULT", 0);
    const uint32_t s_mult = (uint32_t)std::clamp(s_env > 0 ? s_env : (rs64_terrain_dist() >= 2.0f ? 2 : 1), 1, 2);
    const uint32_t n = original * s_mult;
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
extern "C" uint32_t rs64_tgrid_base(uint32_t original) {
    switch (original) {
    case 0x80130C70u: return 0x80A00000u;
    case 0x80130D10u: return 0x80A00100u;
    case 0x80130DB0u: return 0x80A01000u;
    case 0x80131DB0u: return 0x80A05000u;
    case 0x80132DC0u: return 0x80A10000u;
    default:          return original;
    }
}

// Zeroes the relocated terrain grid tables at level load, as a fresh level would find the originals.
extern "C" void rs64_tgrid_clear(uint8_t* rdram) {
    std::memset(rdram + (0x80A00000u - 0x80000000u), 0, 0x20000);
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

// ---- Mods ----

// Mod filter extension point: every enabled mod whose native library exports `name` may rewrite a u32 value (passed in r4, returned in r2), chained in mod order.
// Filters: hangar_craft_mask (crafts selectable in the hangar), level_craft_icons (crafts shown on SELECT LEVEL). Bit n = craft n. See docs/adding-menus-and-buttons.md.
extern "C" uint32_t rs64_mod_filter_u32(uint8_t* rdram, const char* name, uint32_t value) {
    for (const auto& d : recomp::mods::get_all_mod_details("rs64")) {
        if (!recomp::mods::is_mod_enabled(d.mod_id)) {
            continue;
        }

        recomp_func_t* fn = recomp::mods::get_mod_export(d.mod_id, name);
        if (fn == nullptr) {
            continue;
        }

        recomp_context c{};
        c.r4 = value;
        c.r2 = value;
        fn(rdram, &c);
        static const bool s_log = env_on("ROGUESQ_LOG_MOD_FILTERS");
        if (s_log && (uint32_t)c.r2 != value) {
            fprintf(stderr, "[mod-filter] %s: %s 0x%X -> 0x%X\n", name, d.mod_id.c_str(), value, (uint32_t)c.r2);
            fflush(stderr);
        }
        value = (uint32_t)c.r2;
    }
    return value;
}

// ---- Boot target ----

// Set by the present hook while the attract title is up; get_n64_input injects START while set.
extern "C" volatile int g_boot_pulse_start = 0;

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

extern "C" volatile int g_current_scene = -1;      // menuOverlayInit action id; 9 = attribution
extern "C" volatile unsigned g_op_bf_count = 0;    // bumped by the F5 GBI per explosion-bloom tri

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
extern "C" volatile unsigned g_vi_tick = 0;

// Block until the next VI tick (cap ~40 ms). Paces a menu fade loop (attribution screen,
// credits sequence) to real time so the VI-driven glyph-layout producer thread gets to run.
extern "C" void rs64_attrib_wait_vi(void) {
#ifdef _WIN32
    unsigned start = g_vi_tick;
    for (int i = 0; i < 40 && g_vi_tick == start; ++i) ::Sleep(1);
#endif
}

// ---- Service replies ----

extern "C" void rs_malloc(uint8_t* rdram, recomp_context* ctx);

// tickFormatMessageWorker sends every reply as a pointer to one stack buffer. Copy each reply
// into its own slot of a persistent ring so replies still queued keep their request ids.
// Returns the address to send; ROGUESQ_NO_FORMAT_REPLY_FIX=1 returns src unchanged.
extern "C" uint32_t rs64_format_reply_slot(uint8_t* rdram, recomp_context* ctx, uint32_t src) {
    static const bool s_off = env_on("ROGUESQ_NO_FORMAT_REPLY_FIX");
    constexpr uint32_t kSlots = 32, kSlotSize = 0x30;
    static uint32_t s_ring = 0;
    static uint32_t s_next = 0;
    if (s_off || (src & 3u) != 0 || (src & 0xFF800000u) != 0x80000000u) return src;
    if (s_ring == 0) {
        recomp_context saved = *ctx;
        ctx->r4 = kSlots * kSlotSize;
        ctx->r5 = 0;
        rs_malloc(rdram, ctx);
        const uint32_t p = (uint32_t)ctx->r2;
        *ctx = saved;
        if ((p & 0xFF800000u) != 0x80000000u) return src;
        s_ring = p;
    }
    const uint32_t dst = s_ring + (s_next++ % kSlots) * kSlotSize;
    std::memcpy(rdram + (dst - 0x80000000u), rdram + (src - 0x80000000u), kSlotSize);
    return dst;
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
