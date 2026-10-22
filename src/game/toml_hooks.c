// Bodies of the [[patches.hook]] entries in rogue_squadron.toml, one function per hook site, named rs64_th_<func>_<before_vram or entry>.
// The toml text only calls them: { if (f(rdram, ctx)) return; } when the body can return from the game function (return 1), else { f(rdram, ctx); }.
// Compiled in the RecompiledFuncs target, so bodies see rdram, ctx, the MEM_* macros and funcs.h exactly as they did inside the recompiled function.
#include "recomp.h"
#include "../../RecompiledFuncs/funcs.h"
#include "rs64_toml_hooks.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
// cinematicLoopBody entry
void rs64_th_cinematicLoopBody_entry(uint8_t* rdram, recomp_context* ctx)
{
    // Cinematic is running, so attribution is over: clear g_current_scene here
    // (the NTSC N64-logo branch that would clear it is skipped) or the
    // attribution VI redirect steals the cinematic's swaps.
    extern volatile int g_current_scene;
    g_current_scene = -1;

    // Periodic yield to break the cinematic thread-starvation stall.
    // ROGUESQ_DISABLE_CINE_YIELD=1 disables.
    extern void rs64_cine_yield(void);
    static int s_disable = -1;

    if (s_disable < 0) {
        const char* e = getenv("ROGUESQ_DISABLE_CINE_YIELD");
        s_disable = (e && *e && *e != '0') ? 1 : 0;
    }

    if (!s_disable) rs64_cine_yield();

    static int s_log = -1;

    if (s_log < 0) {
        const char* e = getenv("ROGUESQ_LOG_HOOKS");
        s_log = (e && *e && *e != '0') ? 1 : 0;
    }

    if (s_log) {
        static int s_count = 0;
        ++s_count;

        if (s_count <= 4 || (s_count & 63) == 0)
        {
            extern void rs64_dbg_log4(const char* tag, unsigned a, unsigned b, unsigned c, unsigned d);
            rs64_dbg_log4("800A5D80 a0 a1 a2 count", (unsigned)ctx->r4, (unsigned)ctx->r5, (unsigned)ctx->r6, (unsigned)s_count);
        }
    }
}

// load_asset_with_malloc_flags 0x80064884
void rs64_th_load_asset_with_malloc_flags_80064884(uint8_t* rdram, recomp_context* ctx) {

    static int s_force = -1;

    if (s_force < 0) {
        const char* e = getenv("ROGUESQ_DISABLE_EMPTY_POOL_SENTINEL");
        s_force = (e && *e && *e != '0') ? 0 : 1;
    }

    if (s_force) {
        uint32_t buf_ptr = (uint32_t)ctx->r2;

        if (buf_ptr >= 0x80000000u && buf_ptr < 0x80800000u) {
            int32_t* word_ptr = (int32_t*)(rdram + (buf_ptr - 0x80000000u));

            if (*word_ptr == 0) {
                *word_ptr = -1;
            }
        }
    }
}

// viRetraceHandlerThread 0x800198E8
void rs64_th_viRetraceHandlerThread_800198E8(uint8_t* rdram, recomp_context* ctx)
{

    static int s_count = 0;
    int n = ++s_count;

    if (n <= 4 || (n & 63) == 0)
    {
        extern void rs64_dbg_log4(const char* tag, unsigned a, unsigned b, unsigned c, unsigned d);
        rs64_dbg_log4("game_frame_iter", n, 0, 0, 0);
    }

    // All-displayed deadlock rescue: buffers all in state 5 with none in 1-4 is
    // an illegal steady state (5->0 demotion can't fire, producer can't alloc) =
    // the cinematic freeze. After it persists 32 consumer passes, demote the 5s
    // to 0. r22 = buffer-state base (0x80128EAC). ROGUESQ_BUFSTUCK_RESCUE=0 disables.
    extern char* getenv(const char*);
    static int s_resc = -1;
    static int s_persist = 0;
    static unsigned s_rescued = 0;

    if (s_resc < 0) {
        const char* e = getenv("ROGUESQ_BUFSTUCK_RESCUE");
        s_resc = (e && e[0] == '0') ? 0 : 1;
    }

    if (s_resc) {
        unsigned cnt = (unsigned)MEM_BU(ctx->r22, -0X1);

        if (cnt > 3) cnt = 3;

        int fives = 0, inflight = 0;
        unsigned k;

        for (k = 0; k < cnt; ++k) {
            unsigned st = (unsigned)MEM_BU(ctx->r22, -0X4 + (int)k);

            if (st == 5) ++fives; else if (st >= 1 && st <= 4) ++inflight;
        }

        if (cnt >= 2 && fives >= 2 && inflight == 0) {
            if (++s_persist >= 32)
            {
                s_persist = 0;
                ++s_rescued;

                for (k = 0; k < cnt; ++k)
                    if ((unsigned)MEM_BU(ctx->r22, -0X4 + (int)k) == 5) MEM_B(-0X4 + (int)k, ctx->r22) = 0;

                extern void rs64_dbg_log4(const char* tag, unsigned a, unsigned b, unsigned c, unsigned d);
                rs64_dbg_log4("bufstuck_rescue", s_rescued, 0, 0, 0);
            }
        } else {
            s_persist = 0;
        }
    }
}

// runMenuModelViewerScreen 0x800C29F8
void rs64_th_runMenuModelViewerScreen_800C29F8(uint8_t* rdram, recomp_context* ctx)
{

    static int s_ms = -1;

    if (s_ms < 0) { const char* e = getenv("ROGUESQ_ATTRIB_HOLD_MS"); s_ms = (e && *e) ? atoi(e) : 50; if (s_ms < 0) s_ms = 0; if (s_ms > 2000) s_ms = 2000; }

    static int s_log = -1;

    if (s_log < 0) { const char* e = getenv("ROGUESQ_LOG_HOOKS"); s_log = (e && *e && *e != '0') ? 1 : 0; }
    if (s_log) { static int n = 0; ++n; if (n <= 40) { extern void rs64_dbg_log4(const char*, unsigned, unsigned, unsigned, unsigned); rs64_dbg_log4("attrib_iter n f22x1k f24x1k", (unsigned)n, (unsigned)(ctx->f22.fl*1000.0f), (unsigned)(ctx->f24.fl*1000.0f), 0); } }

    static int s_force_sleep = -1;

    if (s_force_sleep < 0) { const char* e = getenv("ROGUESQ_ATTRIB_HOLD_MS"); s_force_sleep = (e && *e) ? 1 : 0; }

    if (s_force_sleep) { if (s_ms > 0) { extern void rs64_sleep_ms(unsigned); rs64_sleep_ms((unsigned)s_ms); } }
    else { extern void rs64_attrib_wait_vi_yield(uint8_t*, recomp_context*); rs64_attrib_wait_vi_yield(rdram, ctx); }
}

// tickFormatMessageWorker 0x800B380C
void rs64_th_tickFormatMessageWorker_800B380C(uint8_t* rdram, recomp_context* ctx)
{

    extern uint32_t rs64_format_reply_slot(uint8_t*, recomp_context*, uint32_t);
    ctx->r6 = (gpr)(int32_t)rs64_format_reply_slot(rdram, ctx, (uint32_t)ctx->r6);
}

// initVideoSubsystem 0x8001A2EC
void rs64_th_initVideoSubsystem_8001A2EC(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_fb_cpu_cleared(uint8_t*, uint32_t, uint32_t);
    rs64_fb_cpu_cleared(rdram, (uint32_t)ctx->r17, (uint32_t)ctx->r16);
}

// updateXwingFlightControls 0x800B48EC
void rs64_th_updateXwingFlightControls_800B48EC(uint8_t* rdram, recomp_context* ctx)
{

    extern float rs64_throttle_hook(uint8_t*, float, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    ctx->f20.fl = rs64_throttle_hook(rdram, ctx->f20.fl, (uint32_t)ctx->r18 + 0x14, 0x800A58C4, 0x800A58D0, 0x800A58DC, (uint32_t)ctx->r18 + 0x4);
}

// updateAwingFlightControls 0x800B5D54
void rs64_th_updateAwingFlightControls_800B5D54(uint8_t* rdram, recomp_context* ctx)
{

    extern float rs64_throttle_hook(uint8_t*, float, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    ctx->f20.fl = rs64_throttle_hook(rdram, ctx->f20.fl, 0, 0x800A5A44, 0x800A5A4C, 0x800A5A54, 0);
}

// updateYwingFlightControls 0x800B7984
void rs64_th_updateYwingFlightControls_800B7984(uint8_t* rdram, recomp_context* ctx)
{

    extern float rs64_throttle_hook(uint8_t*, float, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    ctx->f20.fl = rs64_throttle_hook(rdram, ctx->f20.fl, 0, 0x800A5B88, 0, 0x800A5B90, 0);
}

// updateVwingFlightControls 0x800B9280
void rs64_th_updateVwingFlightControls_800B9280(uint8_t* rdram, recomp_context* ctx)
{

    extern float rs64_throttle_hook(uint8_t*, float, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    ctx->f24.fl = rs64_throttle_hook(rdram, ctx->f24.fl, 0, 0x800A5CB0, 0x800A5CB8, 0x800A5CC0, (uint32_t)ctx->r21 + 0x190);
}

// updateFalconFlightControls 0x800BA8F4
void rs64_th_updateFalconFlightControls_800BA8F4(uint8_t* rdram, recomp_context* ctx)
{

    extern float rs64_throttle_hook(uint8_t*, float, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    ctx->f20.fl = rs64_throttle_hook(rdram, ctx->f20.fl, (uint32_t)ctx->r18 + 0x4, 0x800A5DD8, 0x800A5DE0, 0x800A5DE8, 0);
}

// updateTieInterceptorFlightControls 0x800BC190
void rs64_th_updateTieInterceptorFlightControls_800BC190(uint8_t* rdram, recomp_context* ctx)
{

    extern float rs64_throttle_hook(uint8_t*, float, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    ctx->f20.fl = rs64_throttle_hook(rdram, ctx->f20.fl, (uint32_t)ctx->r18 + 0x4, 0x800A5F40, 0x800A5F48, 0x800A5F50, 0);
}

// updateT16FlightControls 0x800BD148
void rs64_th_updateT16FlightControls_800BD148(uint8_t* rdram, recomp_context* ctx)
{

    extern float rs64_throttle_hook(uint8_t*, float, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    ctx->f20.fl = rs64_throttle_hook(rdram, ctx->f20.fl, (uint32_t)ctx->r18 + 0x4, 0x800A604C, 0x800A6054, 0x800A605C, 0);
}

// updateNabooStarfighterFlightControls 0x800BDEB0
void rs64_th_updateNabooStarfighterFlightControls_800BDEB0(uint8_t* rdram, recomp_context* ctx)
{

    extern float rs64_throttle_hook(uint8_t*, float, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    ctx->f20.fl = rs64_throttle_hook(rdram, ctx->f20.fl, 0, 0x800A6144, 0x800A6150, 0x800A6154, 0);
}

// updateSnowSpeederFlightControls 0x800ABDA0
void rs64_th_updateSnowSpeederFlightControls_800ABDA0(uint8_t* rdram, recomp_context* ctx)
{

    extern float rs64_throttle_hook(uint8_t*, float, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    const int half = MEM_BU(0x2D, ctx->r18) != 0;
    const uint16_t held = MEM_HU(0, (gpr)(int32_t)0x80130B88);
    const uint16_t brakes = MEM_HU(0x14, (gpr)(int32_t)0x8010BE18) | MEM_HU(0x16, (gpr)(int32_t)0x8010BE18);

    if (!(held & brakes)) ctx->f22.fl = rs64_throttle_hook(rdram, ctx->f22.fl, 0, half ? 0x800A5318 : 0x800A531C, half ? 0x800A5308 : 0x800A530C, half ? 0x800A5328 : 0x800A532C, 0);
}

// transformVec3ToNpcLocalWithScale 0x8001DEA0
void rs64_th_transformVec3ToNpcLocalWithScale_8001DEA0(uint8_t* rdram, recomp_context* ctx) {

    extern float rs64_cull_widen(void);
    ctx->f2.fl *= rs64_cull_widen();
}

// transformVec3ToNpcLocalWithScale 0x8001E168
void rs64_th_transformVec3ToNpcLocalWithScale_8001E168(uint8_t* rdram, recomp_context* ctx) {

    extern float rs64_cull_widen(void);
    ctx->f2.fl *= rs64_cull_widen();
}

// setupCameraMatrices 0x80016CE0
void rs64_th_setupCameraMatrices_80016CE0(uint8_t* rdram, recomp_context* ctx) {

    extern float rs64_draw_dist(void);
    ctx->f0.fl *= rs64_draw_dist();
}

// setupCameraMatrices 0x80016D50
void rs64_th_setupCameraMatrices_80016D50(uint8_t* rdram, recomp_context* ctx) {

    extern float rs64_draw_dist(void);
    ctx->f0.fl *= rs64_draw_dist();
}

// transformVec3ToNpcLocalWithScale 0x8001DDA0
void rs64_th_transformVec3ToNpcLocalWithScale_8001DDA0(uint8_t* rdram, recomp_context* ctx)
{

    extern float rs64_draw_dist(void);

    if (MEM_W(0x3C, ctx->r29) != (int32_t)0x800F27B4) ctx->f0.fl *= rs64_draw_dist();
}

// transformVec3ToNpcLocalWithScale 0x8001E078
void rs64_th_transformVec3ToNpcLocalWithScale_8001E078(uint8_t* rdram, recomp_context* ctx)
{

    extern float rs64_draw_dist(void);

    if (MEM_W(0x3C, ctx->r29) != (int32_t)0x800F27B4) ctx->f0.fl *= rs64_draw_dist();
}

// addNpcToVisibilityBucket 0x80057C90
void rs64_th_addNpcToVisibilityBucket_80057C90(uint8_t* rdram, recomp_context* ctx)
{

    extern void rs64_f5_cable_submit(uint32_t, uint32_t, uint32_t);
    rs64_f5_cable_submit((uint32_t)MEM_W(0, (gpr)(int32_t)0x8010B6E0), (uint32_t)ctx->r5, (uint32_t)MEM_W(0, (gpr)(int32_t)0x8010B6E8));
}

// addNpcToVisibilityBucket 0x80057EAC
void rs64_th_addNpcToVisibilityBucket_80057EAC(uint8_t* rdram, recomp_context* ctx) {

    extern float rs64_draw_dist(void);
    ctx->f2.fl /= rs64_draw_dist();
}

// selectMeshLodAndSubmitForRender 0x8005813C
void rs64_th_selectMeshLodAndSubmitForRender_8005813C(uint8_t* rdram, recomp_context* ctx) {

    extern float rs64_draw_dist(void);
    ctx->f2.fl /= rs64_draw_dist();
}

// buildVisibleTerrainGridAroundCamera 0x8004392C
void rs64_th_buildVisibleTerrainGridAroundCamera_8004392C(uint8_t* rdram, recomp_context* ctx) {

    extern float rs64_terrain_dist(void);
    ctx->f2.fl *= rs64_terrain_dist();
}

// scanConvertTriangleToSpanTable 0x800425D8
void rs64_th_scanConvertTriangleToSpanTable_800425D8(uint8_t* rdram, recomp_context* ctx) {

    extern uint32_t rs64_tgrid_base(uint32_t);
    ctx->r12 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r12);
}

// scanConvertTriangleToSpanTable 0x800425E0
void rs64_th_scanConvertTriangleToSpanTable_800425E0(uint8_t* rdram, recomp_context* ctx) {

    extern uint32_t rs64_tgrid_base(uint32_t);
    ctx->r8 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r8);
}

// scanConvertTriangleToSpanTable 0x800425E8
void rs64_th_scanConvertTriangleToSpanTable_800425E8(uint8_t* rdram, recomp_context* ctx) {

    extern uint32_t rs64_tgrid_base(uint32_t);
    ctx->r7 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r7);
}

// scanConvertTriangleToSpanTable 0x80042720
void rs64_th_scanConvertTriangleToSpanTable_80042720(uint8_t* rdram, recomp_context* ctx) {

    extern uint32_t rs64_tgrid_base(uint32_t);
    ctx->r12 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r12);
}

// scanConvertTriangleToSpanTable 0x80042728
void rs64_th_scanConvertTriangleToSpanTable_80042728(uint8_t* rdram, recomp_context* ctx) {

    extern uint32_t rs64_tgrid_base(uint32_t);
    ctx->r8 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r8);
}

// scanConvertTriangleToSpanTable 0x80042730
void rs64_th_scanConvertTriangleToSpanTable_80042730(uint8_t* rdram, recomp_context* ctx) {

    extern uint32_t rs64_tgrid_base(uint32_t);
    ctx->r7 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r7);
}

// rasterizeEdgeXSpansAscending 0x80042000
void rs64_th_rasterizeEdgeXSpansAscending_80042000(uint8_t* rdram, recomp_context* ctx) {

    extern uint32_t rs64_tgrid_base(uint32_t);
    ctx->r2 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r2);
}

// rasterizeEdgeXSpansAscending 0x80042050
void rs64_th_rasterizeEdgeXSpansAscending_80042050(uint8_t* rdram, recomp_context* ctx) {

    extern uint32_t rs64_tgrid_base(uint32_t);
    ctx->r2 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r2);
}

// rasterizeEdgeXSpansAscending 0x80042078
void rs64_th_rasterizeEdgeXSpansAscending_80042078(uint8_t* rdram, recomp_context* ctx) {

    extern uint32_t rs64_tgrid_base(uint32_t);
    ctx->r3 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r3);
}

// rasterizeEdgeXSpansAscending 0x800420A4
void rs64_th_rasterizeEdgeXSpansAscending_800420A4(uint8_t* rdram, recomp_context* ctx) {

    extern uint32_t rs64_tgrid_base(uint32_t);
    ctx->r2 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r2);
}

// rasterizeEdgeXSpansAscending 0x800420CC
void rs64_th_rasterizeEdgeXSpansAscending_800420CC(uint8_t* rdram, recomp_context* ctx) {

    extern uint32_t rs64_tgrid_base(uint32_t);
    ctx->r2 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r2);
}

// rasterizeEdgeXSpansDescending 0x80042174
void rs64_th_rasterizeEdgeXSpansDescending_80042174(uint8_t* rdram, recomp_context* ctx) {

    extern uint32_t rs64_tgrid_base(uint32_t);
    ctx->r3 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r3);
}

// rasterizeEdgeXSpansDescending 0x800421A0
void rs64_th_rasterizeEdgeXSpansDescending_800421A0(uint8_t* rdram, recomp_context* ctx) {

    extern uint32_t rs64_tgrid_base(uint32_t);
    ctx->r2 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r2);
}

// rasterizeEdgeXSpansDescending 0x800421CC
void rs64_th_rasterizeEdgeXSpansDescending_800421CC(uint8_t* rdram, recomp_context* ctx) {

    extern uint32_t rs64_tgrid_base(uint32_t);
    ctx->r2 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r2);
}

// rasterizeEdgeXSpansDescending 0x80042224
void rs64_th_rasterizeEdgeXSpansDescending_80042224(uint8_t* rdram, recomp_context* ctx) {

    extern uint32_t rs64_tgrid_base(uint32_t);
    ctx->r2 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r2);
}

// rasterizeEdgeXSpansDescending 0x80042274
void rs64_th_rasterizeEdgeXSpansDescending_80042274(uint8_t* rdram, recomp_context* ctx) {

    extern uint32_t rs64_tgrid_base(uint32_t);
    ctx->r2 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r2);
}

// binPolygonIntoTerrainGrid 0x80042A4C
void rs64_th_binPolygonIntoTerrainGrid_80042A4C(uint8_t* rdram, recomp_context* ctx) {

    extern uint32_t rs64_tgrid_base(uint32_t);
    ctx->r10 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r10);
}

// binPolygonIntoTerrainGrid 0x80042A54
void rs64_th_binPolygonIntoTerrainGrid_80042A54(uint8_t* rdram, recomp_context* ctx) {

    extern uint32_t rs64_tgrid_base(uint32_t);
    ctx->r9 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r9);
}

// streamTerrainGridCellsByDistance 0x80042CF4
void rs64_th_streamTerrainGridCellsByDistance_80042CF4(uint8_t* rdram, recomp_context* ctx) {

    extern uint32_t rs64_tgrid_base(uint32_t);
    ctx->r13 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r13);
}

// streamTerrainGridCellsByDistance 0x80042D24
void rs64_th_streamTerrainGridCellsByDistance_80042D24(uint8_t* rdram, recomp_context* ctx) {

    extern uint32_t rs64_tgrid_base(uint32_t);
    ctx->r10 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r10);
}

// streamTerrainGridCellsByDistance 0x80042D4C
void rs64_th_streamTerrainGridCellsByDistance_80042D4C(uint8_t* rdram, recomp_context* ctx) {

    extern uint32_t rs64_tgrid_base(uint32_t);
    ctx->r14 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r14);
}

// buildGridCellMeshForFace 0x80043534
void rs64_th_buildGridCellMeshForFace_80043534(uint8_t* rdram, recomp_context* ctx) {

    extern uint32_t rs64_tgrid_base(uint32_t);
    ctx->r8 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r8);
}

// buildGridCellMeshForFace 0x8004368C
void rs64_th_buildGridCellMeshForFace_8004368C(uint8_t* rdram, recomp_context* ctx) {

    extern uint32_t rs64_tgrid_base(uint32_t);
    ctx->r7 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r7);
}

// loadLevelAssets 0x800453C8
void rs64_th_loadLevelAssets_800453C8(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_tgrid_clear(uint8_t*);
    rs64_tgrid_clear(rdram);
}

// buildVisibleTerrainGridAroundCamera 0x80043A84
void rs64_th_buildVisibleTerrainGridAroundCamera_80043A84(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_tgrid_clamp_polygon(uint8_t*);
    rs64_tgrid_clamp_polygon(rdram);
}

// loadLevelAssets 0x800453D8
void rs64_th_loadLevelAssets_800453D8(uint8_t* rdram, recomp_context* ctx) {

    extern uint32_t rs64_tgrid_budget(uint32_t);
    ctx->r16 = (gpr)(int32_t)rs64_tgrid_budget((uint32_t)ctx->r16);
}

// computeGridLayerScrollOffset 0x800456D4
void rs64_th_computeGridLayerScrollOffset_800456D4(uint8_t* rdram, recomp_context* ctx) {

    extern float rs64_terrain_dist(void);
    const float td = rs64_terrain_dist();
    ctx->f0.fl *= td * td;
}

// setupCutsceneLevel 0x800A6D3C
void rs64_th_setupCutsceneLevel_800A6D3C(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_cutscene_level_load(int);
    rs64_cutscene_level_load(1);
}

// setupCutsceneLevel 0x800A6D44
void rs64_th_setupCutsceneLevel_800A6D44(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_cutscene_level_load(int);
    rs64_cutscene_level_load(0);
}

// streamTerrainGridCellsByDistance 0x80042DDC
void rs64_th_streamTerrainGridCellsByDistance_80042DDC(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_tgrid_budget_probe(uint32_t, uint32_t);
    rs64_tgrid_budget_probe((uint32_t)ctx->r7, (uint32_t)ctx->r2);
}

// buildVisibleTerrainGridAroundCamera 0x80043A8C
void rs64_th_buildVisibleTerrainGridAroundCamera_80043A8C(uint8_t* rdram, recomp_context* ctx)
{

    extern void rs64_terrain_grid_log(int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, uint32_t);
    rs64_terrain_grid_log(MEM_W(0, (gpr)(int32_t)0x80132DB0), MEM_W(0, (gpr)(int32_t)0x80132DB4), MEM_W(0, (gpr)(int32_t)0x80132DB8), MEM_W(0, (gpr)(int32_t)0x80132DBC), MEM_HU(0, (gpr)(int32_t)0x80130C4C), MEM_HU(0, (gpr)(int32_t)0x80136DF8), MEM_HU(0, (gpr)(int32_t)0x80136DFA));
}

// setupCameraMatrices 0x800170D0
void rs64_th_setupCameraMatrices_800170D0(uint8_t* rdram, recomp_context* ctx) {

    extern float rs64_terrain_dist(void);
    ctx->f4.fl *= rs64_terrain_dist();
}

// setupCameraMatrices 0x80017168
void rs64_th_setupCameraMatrices_80017168(uint8_t* rdram, recomp_context* ctx) {

    extern float rs64_terrain_dist(void);
    ctx->f4.fl *= rs64_terrain_dist();
}

// cutsceneActorNpcHandler 0x80044924
void rs64_th_cutsceneActorNpcHandler_80044924(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_fog_probe(uint8_t*);
    rs64_fog_probe(rdram);
}

// linkDirectionalTrailSegments 0x8005B418
void rs64_th_linkDirectionalTrailSegments_8005B418(uint8_t* rdram, recomp_context* ctx)
{
    extern float rs64_cull_widen(void); if (rs64_cull_widen() > 1.0f && (int32_t)ctx->r6 - (int32_t)ctx->r7 == 4) { ctx->r7 = (gpr)((int32_t)ctx->r7 - 1); ctx->r6 = (gpr)((int32_t)ctx->r6 + 1); }
}

// buildVisibleTerrainGridAroundCamera 0x8004395C
void rs64_th_buildVisibleTerrainGridAroundCamera_8004395C(uint8_t* rdram, recomp_context* ctx) {

    extern float rs64_cull_widen(void);
    ctx->f6.fl *= rs64_cull_widen();
}

// runMenuShipSelectScreen 0x800C14A8
void rs64_th_runMenuShipSelectScreen_800C14A8(uint8_t* rdram, recomp_context* ctx)
{

    static int s_off = -1;

    if (s_off < 0) { const char* e = getenv("ROGUESQ_CREDITS_NOPACE"); s_off = (e && *e && *e != '0') ? 1 : 0; }
    if (!s_off) { extern void rs64_attrib_wait_vi(void); rs64_attrib_wait_vi(); }
}

// menuOverlayInit entry
void rs64_th_menuOverlayInit_entry(uint8_t* rdram, recomp_context* ctx)
{

    extern void rs64_dbg_log4(const char*, unsigned, unsigned, unsigned, unsigned);
    rs64_dbg_log4("menuOverlayInit", (unsigned)ctx->r5, (unsigned)ctx->r6, 0, 0);
    extern volatile int g_current_scene;
    g_current_scene = (int)(unsigned)ctx->r5;
    extern void rs64_mod_levels_menu(int);
    rs64_mod_levels_menu((int)ctx->r5);
}

// musyxKeyOnVoice entry
int rs64_th_musyxKeyOnVoice_entry(uint8_t* rdram, recomp_context* ctx)
{

    if (getenv("ROGUESQ_STUB_VOICESTART")) { return 1; } if (MEM_HU(ctx->r4, 0X60) == 0) { return 1; }

    return 0;
}

// musyxSynthFrame 0x80091798
void rs64_th_musyxSynthFrame_80091798(uint8_t* rdram, recomp_context* ctx) {

    uint32_t cb = (uint32_t)(uint64_t)ctx->r2;

    if (cb >= 0x80000400u && cb < 0x80110000u) {
        ctx->r4 = ctx->r16;   // delay slot: a0 = s0
        LOOKUP_FUNC(ctx->r2)(rdram, ctx);
    }
}

// runIdleFramesAndLoadSaveData 0x8006D980
void rs64_th_runIdleFramesAndLoadSaveData_8006D980(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_idle_pace(void);
    rs64_idle_pace();
}

// loadOverlay 0x80000B3C
void rs64_th_loadOverlay_80000B3C(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_load_overlay(unsigned int);
    rs64_load_overlay((unsigned int)ctx->r16);
}

// rs_malloc 0x80001C74
void rs64_th_rs_malloc_80001C74(uint8_t* rdram, recomp_context* ctx)
{

    gpr addr = ctx->r18;
    gpr size = ctx->r16;
    uint32_t addr32 = (uint32_t)addr;
    uint32_t size32 = (uint32_t)size;

    if ((addr32 & 0xE0000000u) == 0x80000000u && addr32 < 0x80800000u && size32 > 0 && size32 < 0x100000u && addr32 + size32 <= 0x80800000u)
    {
        for (uint32_t i = 0; i < size32; ++i) MEM_B(i, addr) = 0;
    }
}

// saveServiceWorker 0x8006F2CC
int rs64_th_saveServiceWorker_8006F2CC(uint8_t* rdram, recomp_context* ctx) {

    static int s_enable = -1;

    if (s_enable < 0) {
        const char* e = getenv("ROGUESQ_ENABLE_F8006F2CC");
        s_enable = (e && *e && *e != '0') ? 1 : 0;
    }

    if (!s_enable) return 1;

    return 0;
}

// cinematicLoopBody 0x800A6024
void rs64_th_cinematicLoopBody_800A6024(uint8_t* rdram, recomp_context* ctx)
{
    // Freeze watchdog: host poll dumps the stack if no tick for 2s.
    extern void rs64_cine_iter_tick(unsigned iter);
    static unsigned s_watchdog_count = 0;
    rs64_cine_iter_tick(++s_watchdog_count);

    // Pace this internal loop to a target FPS. ROGUESQ_CINE_TARGET_FPS=0 disables.
    extern void rs64_cine_pace(void);
    rs64_cine_pace();

    extern void rs64_cine_pause_point(uint8_t* rdram);
    rs64_cine_pause_point(rdram);

    // Diagnostic: iterations + gate/cineState. ROGUESQ_LOG_HOOKS=1.
    static int s_log = -1;

    if (s_log < 0) {
        const char* e = getenv("ROGUESQ_LOG_HOOKS");
        s_log = (e && *e && *e != '0') ? 1 : 0;
    }

    if (s_log) {
        static int s_count = 0;
        int n = ++s_count;

        if (n <= 4 || (n & 127) == 0)
        {
            extern void rs64_dbg_log4(const char* tag, unsigned a, unsigned b, unsigned c, unsigned d);

            // gate (0x800A6268): 0x800B0B28 < cutscene[0x44]-0xA
            unsigned gateCtr = (unsigned)MEM_W(0XB28, S32(0X800B << 16));
            unsigned cutscene  = (unsigned)MEM_W(0X1904, S32(0X800B << 16));
            unsigned cuts_44 = 0;

            if (cutscene >= 0x80000000u && cutscene < 0x80800000u) {
                cuts_44 = (unsigned)MEM_W(0X44, (int64_t)(int32_t)cutscene);
            }

            unsigned cineState = (unsigned)MEM_W(0X934, S32(0X800B << 16));
            rs64_dbg_log4("cineLoopIter n gateCtr cuts44 cineState", n, gateCtr, cuts_44, cineState);
        }
    }
}

// waitForMusyXAudioTaskDone 0x80091ED0
void rs64_th_waitForMusyXAudioTaskDone_80091ED0(uint8_t* rdram, recomp_context* ctx) {
    osYieldThread_recomp(rdram, ctx);
}

// anyAudioSlotActive entry
void rs64_th_anyAudioSlotActive_entry(uint8_t* rdram, recomp_context* ctx) {
    osYieldThread_recomp(rdram, ctx);
}

// shutdownMusyXVoices 0x8009BCEC
void rs64_th_shutdownMusyXVoices_8009BCEC(uint8_t* rdram, recomp_context* ctx) {
    osYieldThread_recomp(rdram, ctx);
}

// reclaimDisplayListChunk entry
void rs64_th_reclaimDisplayListChunk_entry(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_wait_gfx_parse_yield(uint8_t*, recomp_context*);
    rs64_wait_gfx_parse_yield(rdram, ctx);
}

// drainMeshReleaseQueue entry
void rs64_th_drainMeshReleaseQueue_entry(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_wait_gfx_parse_yield(uint8_t*, recomp_context*);
    rs64_wait_gfx_parse_yield(rdram, ctx);
}

// heapFreeListInsert entry
void rs64_th_heapFreeListInsert_entry(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_wait_gfx_parse_yield(uint8_t*, recomp_context*);
    rs64_wait_gfx_parse_yield(rdram, ctx);
}

// beginFrameDLChunk entry
void rs64_th_beginFrameDLChunk_entry(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_wait_gfx_parse_yield(uint8_t*, recomp_context*);
    rs64_wait_gfx_parse_yield(rdram, ctx);
}

// emitFrameRdpInitDl entry
void rs64_th_emitFrameRdpInitDl_entry(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_wait_gfx_parse_yield(uint8_t*, recomp_context*);
    rs64_wait_gfx_parse_yield(rdram, ctx);
}

// setupMenuData 0x800BAF70
void rs64_th_setupMenuData_800BAF70(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_menu_install_main(uint8_t* rdram);
    rs64_menu_install_main(rdram);
}

// menuControllerInput 0x800B51B8
void rs64_th_menuControllerInput_800B51B8(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_menu_confirm_main(uint8_t* rdram);
    rs64_menu_confirm_main(rdram);
}

// menuControllerInput 0x800B4CA8
int rs64_th_menuControllerInput_800B4CA8(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(1u, rdram, ctx)) return 1;

    return 0;
}

// menuControllerInput 0x800B5428
int rs64_th_menuControllerInput_800B5428(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(2u, rdram, ctx)) return 1;

    return 0;
}

// updateMenuPerFrame entry
int rs64_th_updateMenuPerFrame_entry(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(3u, rdram, ctx)) return 1;

    return 0;
}

// menuControllerInput 0x800B5920
void rs64_th_menuControllerInput_800B5920(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_slider_redirect(uint8_t* rdram, unsigned sp);
    rs64_slider_redirect(rdram, (unsigned)ctx->r29);
}

// menuControllerInput 0x800B4E64
void rs64_th_menuControllerInput_800B4E64(uint8_t* rdram, recomp_context* ctx)
{

    extern void rs64_menu_input_state(uint8_t* rdram, recomp_context* ctx);
    rs64_menu_input_state(rdram, ctx);
}

// integrateTieInterceptorPhysics entry
void rs64_th_integrateTieInterceptorPhysics_entry(uint8_t* rdram, recomp_context* ctx)
{

    (void)rdram;

    // 0.095 s * 10.5 columns/s (D_800A6C30) stays under one column per tick.
    union { uint32_t u; float f; } dt = { (uint32_t)ctx->r5 };

    if (dt.f > 0.095f) {
        dt.f = 0.095f;
        ctx->r5 = (gpr)(int32_t)dt.u;
    }
}

// playerTieInterceptorMainUpdate 0x800BCE00
void rs64_th_playerTieInterceptorMainUpdate_800BCE00(uint8_t* rdram, recomp_context* ctx)
{
    // Wipe strip at W->0x14: faces at +0xBC (stride 0x34, st words +0x14..+0x20 in vertex order 0,1,3,2), vertices at +0xA7C (xyz floats, 4 per face).
    // Vertices are in tile units centred on the picture (tile edges on integers); the builder rewrites x, y and s every call, t only once.
    // The game maps each 64x32 tile to texels 0..63 x 0..31 and its camera lands the strip a fraction of a native pixel off the static tiles; Scale/Offset are fitted from RenderDoc against the texrect grid.
    const gpr w = ctx->r22;
    const gpr strip = MEM_W(w, 0x14);
    const uint32_t faces = (uint32_t)MEM_W(strip, 0xB8);

    if (faces == 0 || faces > 64)
    {
        return;
    }

    static const int StOrder[4] = { 0, 1, 3, 2 };
    const float SRawPerTile = 4161.1f;
    const float TRawFull = 4228.2f;
    const float ScaleX = 1.00179f, OffsetX = 0.00438f, ScaleY = 1.00122f, OffsetY = 0.0055f;

    // Bilinear centres texels on integers; the point-sampled static texrects sample at each output pixel's corner, so line up at half a texel minus half an output pixel.
    extern float rs64_res_scale(void);
    const float SOffsetTexels = 0.5f - 0.5f / rs64_res_scale(), TOffsetTexels = SOffsetTexels;
    const float Push = 0.003f;
    const float half = (float)MEM_HU(MEM_W(MEM_W(w, 0x4), 0xC), 0x8) * 0.5f;
    union { uint32_t u; float f; } v;

    float lo = 1e30f, hi = -1e30f;

    for (uint32_t i = 0; i < faces * 4; i++) {
        v.u = (uint32_t)MEM_W(strip, 0xA7C + i * 12);
        lo = (v.f < lo) ? v.f : lo;
        hi = (v.f > hi) ? v.f : hi;
    }

    for (uint32_t f = 0; f < faces; f++)
    {
        const gpr st = strip + 0xBC + f * 0x34 + 0x14;
        v.u = (uint32_t)MEM_W(strip, 0xA7C + f * 4 * 12);
        const float colStart = floorf(v.f - (float)MEM_HU(st, StOrder[0] * 4) / 4096.0f + 0.5f);

        for (int j = 0; j < 4; j++)
        {
            const uint32_t vx = 0xA7C + (f * 4 + j) * 12;
            const gpr stj = st + StOrder[j] * 4;
            v.u = (uint32_t)MEM_W(strip, vx);
            float x = v.f;
            x += (x == lo) ? -Push : ((x == hi) ? Push : 0.0f);
            x = (x < -half) ? -half : ((x > half) ? half : x);
            MEM_H(stj, 0) = (int16_t)lrintf((x - colStart - SOffsetTexels / 64.0f) * SRawPerTile);

            if (MEM_HU(stj, 2) == 0)
            {
                MEM_H(stj, 2) = (int16_t)lrintf(-TRawFull * TOffsetTexels / 32.0f);
            } else if (MEM_HU(stj, 2) == 0x1000) {
                MEM_H(stj, 2) = (int16_t)lrintf(TRawFull - TRawFull * TOffsetTexels / 32.0f);
            }

            v.f = x * ScaleX + OffsetX;
            MEM_W(strip, vx) = (int32_t)v.u;
            v.u = (uint32_t)MEM_W(strip, vx + 4);
            v.f = v.f * ScaleY + OffsetY;
            MEM_W(strip, vx + 4) = (int32_t)v.u;
        }
    }
}

// getGameOrFrontText 0x800558C4
void rs64_th_getGameOrFrontText_800558C4(uint8_t* rdram, recomp_context* ctx)
{
    extern unsigned rs64_gof_override(uint8_t* rdram, unsigned textId); unsigned s = rs64_gof_override(rdram, (unsigned)ctx->r4); if (s) { ctx->r3 = 0; ctx->r2 = (gpr)(int32_t)s; }
}

// initOrUpdatePauseScreenDim entry
void rs64_th_initOrUpdatePauseScreenDim_entry(uint8_t* rdram, recomp_context* ctx)
{

    extern void rs64_pause_install(uint8_t* rdram);
    rs64_pause_install(rdram);
    extern void rs64_pause_hover_sound(uint8_t* rdram, recomp_context* ctx);
    rs64_pause_hover_sound(rdram, ctx);
}

// runPauseMenuStateMachine 0x800C3550
void rs64_th_runPauseMenuStateMachine_800C3550(uint8_t* rdram, recomp_context* ctx)
{

    extern unsigned rs64_pause_label_for(uint8_t* rdram, unsigned marker);
    unsigned v = rs64_pause_label_for(rdram, MEM_HU(0x2, ctx->r20));

    if (v) ctx->r2 = (gpr)(int32_t)v;
}

// initOrUpdatePauseScreenDim 0x800C2458
void rs64_th_initOrUpdatePauseScreenDim_800C2458(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_pause_confirm(uint8_t* rdram, unsigned marker);
    rs64_pause_confirm(rdram, MEM_HU(0x4, ctx->r18));
}

// traverseSceneGraphRecursive 0x800155E4
void rs64_th_traverseSceneGraphRecursive_800155E4(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_f5_map_node(unsigned, unsigned);
    rs64_f5_map_node((unsigned)ctx->r4, (unsigned)ctx->r18);
}

// renderLitMeshFaceGroup 0x800137D4
void rs64_th_renderLitMeshFaceGroup_800137D4(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_f5_map_sprite(unsigned, unsigned);
    rs64_f5_map_sprite((unsigned)ctx->r5, (unsigned)ctx->r18);
}

// renderLitMeshFaceGroup 0x80014620
void rs64_th_renderLitMeshFaceGroup_80014620(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_f5_map_sprite(unsigned, unsigned);
    rs64_f5_map_sprite((unsigned)ctx->r17, (unsigned)ctx->r18);
}

// renderLitMeshFaceGroup 0x800138A4
void rs64_th_renderLitMeshFaceGroup_800138A4(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_f5_map_sprite(unsigned, unsigned);
    rs64_f5_map_sprite((unsigned)ctx->r5, (unsigned)ctx->r18);
}

// renderLitMeshFaceGroup 0x800146F0
void rs64_th_renderLitMeshFaceGroup_800146F0(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_f5_map_sprite(unsigned, unsigned);
    rs64_f5_map_sprite((unsigned)ctx->r17, (unsigned)ctx->r18);
}

// initMission 0x800FA28C
int rs64_th_initMission_800FA28C(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_camera_shots_mission_start(uint8_t*);
    rs64_camera_shots_mission_start(rdram);
    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(4u, rdram, ctx)) return 1;

    return 0;
}

// runInMissionFrame 0x800FA7D0
int rs64_th_runInMissionFrame_800FA7D0(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_camera_shots_frame(uint8_t*, recomp_context*);
    rs64_camera_shots_frame(rdram, ctx);
    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(5u, rdram, ctx)) return 1;

    return 0;
}

// applyDatObjectiveTriggerEffect 0x80065A0C
void rs64_th_applyDatObjectiveTriggerEffect_80065A0C(uint8_t* rdram, recomp_context* ctx)
{

    extern void rs64_camera_shots_trigger(uint8_t*, uint32_t, uint32_t);
    rs64_camera_shots_trigger(rdram, (uint32_t)ctx->r4, (uint32_t)ctx->r5);
}

// runInMissionFrame 0x800FA7D8
int rs64_th_runInMissionFrame_800FA7D8(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(6u, rdram, ctx)) return 1;

    return 0;
}

// tickSongFadeTimer 0x800EE82C
int rs64_th_tickSongFadeTimer_800EE82C(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(7u, rdram, ctx)) return 1;

    return 0;
}

// requestSpeechResponseMode2 0x800FBB08
int rs64_th_requestSpeechResponseMode2_800FBB08(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(8u, rdram, ctx)) return 1;

    return 0;
}

// requestSpeechResponseMode1 0x800FBB64
int rs64_th_requestSpeechResponseMode1_800FBB64(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(9u, rdram, ctx)) return 1;

    return 0;
}

// runInMissionFrame 0x800FA94C
int rs64_th_runInMissionFrame_800FA94C(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(10u, rdram, ctx)) return 1;

    return 0;
}

// endMissionCleanup 0x800FB9E4
int rs64_th_endMissionCleanup_800FB9E4(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(11u, rdram, ctx)) return 1;

    return 0;
}

// requestMissionTransitionMode 0x800FB190
int rs64_th_requestMissionTransitionMode_800FB190(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(12u, rdram, ctx)) return 1;

    return 0;
}

// runInMissionFrame 0x800FA8E4
int rs64_th_runInMissionFrame_800FA8E4(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(13u, rdram, ctx)) return 1;

    return 0;
}

// runInMissionFrame 0x800FACA8
int rs64_th_runInMissionFrame_800FACA8(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(14u, rdram, ctx)) return 1;

    return 0;
}

// cutsceneActorNpcHandler 0x80044970
int rs64_th_cutsceneActorNpcHandler_80044970(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(15u, rdram, ctx)) return 1;

    return 0;
}

// datItemSetObjectiveBooleanCount 0x80065914
int rs64_th_datItemSetObjectiveBooleanCount_80065914(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(16u, rdram, ctx)) return 1;

    return 0;
}

// setHudEnableBit4 0x800C7738
int rs64_th_setHudEnableBit4_800C7738(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(17u, rdram, ctx)) return 1;

    return 0;
}

// setHudEnableBit8 0x800C776C
int rs64_th_setHudEnableBit8_800C776C(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(18u, rdram, ctx)) return 1;

    return 0;
}

// npcWingmanUpdate 0x800D9A00
int rs64_th_npcWingmanUpdate_800D9A00(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(19u, rdram, ctx)) return 1;

    return 0;
}

// isNpcWithinActiveReferenceRange 0x80047D9C
int rs64_th_isNpcWithinActiveReferenceRange_80047D9C(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(20u, rdram, ctx)) return 1;

    return 0;
}

// lookupActivePlayerCraftGridCell 0x80047E44
int rs64_th_lookupActivePlayerCraftGridCell_80047E44(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(21u, rdram, ctx)) return 1;

    return 0;
}

// drawRadarIcon 0x800C5E84
int rs64_th_drawRadarIcon_800C5E84(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(22u, rdram, ctx)) return 1;

    extern void rs64_mod_levels_select_open(uint8_t*, recomp_context*);
    rs64_mod_levels_select_open(rdram, ctx);

    return 0;
}

// drawRadarIcon 0x800C5ED8
int rs64_th_drawRadarIcon_800C5ED8(uint8_t* rdram, recomp_context* ctx)
{

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(23u, rdram, ctx)) return 1;

    extern void rs64_mod_levels_select_confirm(uint8_t*, recomp_context*);
    rs64_mod_levels_select_confirm(rdram, ctx);

    return 0;
}

// drawRadarIcon 0x800C5EE0
void rs64_th_drawRadarIcon_800C5EE0(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_mod_levels_select_close(void);
    rs64_mod_levels_select_close();
}

// initCraftSelectScreen 0x800AF18C / handleCraftSelectInput 0x800AFDDC: v0 = the entry's title.
void rs64_th_initCraftSelectScreen_800AF18C(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_mod_levels_text(uint8_t*, recomp_context*, int);
    rs64_mod_levels_text(rdram, ctx, 0);
}
void rs64_th_handleCraftSelectInput_800AFDDC(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_mod_levels_text(uint8_t*, recomp_context*, int);
    rs64_mod_levels_text(rdram, ctx, 0);
}

// handleCraftSelectInput 0x800AFB58: v0 = the entry's description.
void rs64_th_handleCraftSelectInput_800AFB58(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_mod_levels_text(uint8_t*, recomp_context*, int);
    rs64_mod_levels_text(rdram, ctx, 1);
}

// initCraftSelectScreen 0x800AF270 / handleCraftSelectInput 0x800AF878: a2 = the preview image's level number.
void rs64_th_initCraftSelectScreen_800AF270(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_mod_levels_entry_level(uint32_t*);
    uint32_t v = (uint32_t)ctx->r6;
    rs64_mod_levels_entry_level(&v);
    ctx->r6 = v;
}
void rs64_th_handleCraftSelectInput_800AF878(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_mod_levels_entry_level(uint32_t*);
    uint32_t v = (uint32_t)ctx->r6;
    rs64_mod_levels_entry_level(&v);
    ctx->r6 = v;
}

// getAvailablePlayerCraftFlags{Consider,Ignore}Unlocks entry: a0 = level; a mod level's approved craft are returned in v0.
int rs64_th_getAvailablePlayerCraftFlagsConsiderUnlocks_entry(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_mod_levels_craft(uint32_t*);
    uint32_t v = (uint32_t)ctx->r4;
    int m = rs64_mod_levels_craft(&v);
    ctx->r4 = v;

    if (m < 0) return 0;

    ctx->r2 = m;

    return 1;
}
int rs64_th_getAvailablePlayerCraftFlagsIgnoreUnlocks_entry(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_mod_levels_craft(uint32_t*);
    uint32_t v = (uint32_t)ctx->r4;
    int m = rs64_mod_levels_craft(&v);
    ctx->r4 = v;

    if (m < 0) return 0;

    ctx->r2 = m;

    return 1;
}

// Mod level debrief and progress (see rogue_squadron.toml).
void rs64_th_postLevelStatScreen_entry(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_mod_levels_debrief_begin(uint8_t*, uint32_t);
    rs64_mod_levels_debrief_begin(rdram, (uint32_t)ctx->r5);
}
void rs64_th_postLevelStatScreen_800C092C(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_mod_levels_medal_earned(int);
    rs64_mod_levels_medal_earned((int)(uint8_t)ctx->r2);
}
void rs64_th_postLevelStatScreen_800C09C4(uint8_t* rdram, recomp_context* ctx)
{

    extern int rs64_mod_levels_debriefing(void);

    if (rs64_mod_levels_debriefing()) ctx->r2 = MEM_BU((int32_t)0x80130B11, 0);
}
void rs64_th_postLevelStatScreen_800C0A78(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_mod_levels_title(uint8_t*, recomp_context*);
    rs64_mod_levels_title(rdram, ctx);
}
void rs64_th_postLevelStatScreen_800C0A9C(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_mod_levels_title(uint8_t*, recomp_context*);
    rs64_mod_levels_title(rdram, ctx);
}
int rs64_th_highScoreBubbleSort_entry(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_mod_levels_debriefing(void);

    return rs64_mod_levels_debriefing();
}
int rs64_th_unlockMissleUpgradeOnLevelCompletion_entry(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_mod_levels_debriefing(void);

    return rs64_mod_levels_debriefing();
}
int rs64_th_unlockLevelsAndCraftsBasedOnMedalsEarned_entry(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_mod_levels_debriefing(void);

    return rs64_mod_levels_debriefing();
}
int rs64_th_loadAccountDataIntoStruct_entry(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_mod_levels_debriefing(void);

    return rs64_mod_levels_debriefing();
}
int rs64_th_generateUnlockString_entry(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_mod_levels_debriefing(void);

    if (!rs64_mod_levels_debriefing()) return 0;

    ctx->r2 = 0;

    return 1;
}
void rs64_th_runMenuRandomBackdropScreen_800BF9E4(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_mod_levels_debrief_end(uint8_t*);
    rs64_mod_levels_debrief_end(rdram);
}
void rs64_th_mainGameLoop_8003E424(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_mod_levels_playing(void);

    if (rs64_mod_levels_playing()) ctx->r5 = ctx->r16;
}
void rs64_th_mainGameLoop_8003E444(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_mod_levels_playing(void);

    if (rs64_mod_levels_playing()) ctx->r3 = 0x10;
}
void rs64_th_applyPlayerSlotAnimChannelKeyframe_800B1FD4(uint8_t* rdram, recomp_context* ctx)
{

    extern int rs64_mod_levels_entry_medal(uint8_t*, uint32_t);
    int m = rs64_mod_levels_entry_medal(rdram, (uint32_t)ctx->r18 - 0x80130B10u);

    if (m >= 0) ctx->r3 = m;
}

// setupCraftAnimationFlagSet 0x800B9334: the mod level medals beside the profile's medal totals.
void rs64_th_setupCraftAnimationFlagSet_800B9334(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_mod_levels_select_game(uint8_t*, recomp_context*);
    rs64_mod_levels_select_game(rdram, ctx);
}

// getLevelIntroCrawlId entry / buildModelViewerTextPage 0x800CA77C: a mod level's opening crawl.
int rs64_th_getLevelIntroCrawlId_entry(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_mod_levels_crawl_id(void);
    int c = rs64_mod_levels_crawl_id();

    if (c == -2) return 0;

    ctx->r2 = c < 0 ? 0xFF : c;

    return 1;
}
void rs64_th_buildModelViewerTextPage_800CA77C(uint8_t* rdram, recomp_context* ctx) {

    extern uint32_t rs64_mod_levels_crawl_text(uint8_t*);
    uint32_t s = rs64_mod_levels_crawl_text(rdram);

    if (s) ctx->r2 = (gpr)(int32_t)s;
}

// find_manifest_entry entry: a1 = the asset path looked up in manifest slot a0.
void rs64_th_find_manifest_entry_entry(uint8_t* rdram, recomp_context* ctx)
{

    extern uint32_t rs64_mod_levels_lookup(uint8_t*, uint32_t);
    ctx->r5 = (gpr)(int32_t)rs64_mod_levels_lookup(rdram, (uint32_t)ctx->r5);
}

// startSlotLoopAnimWithVoiceSync 0x800B1C00 / 0x800B1D18: v0 = the briefing voice id.
void rs64_th_startSlotLoopAnimWithVoiceSync_800B1C00(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_mod_levels_voice(uint8_t*, recomp_context*);
    rs64_mod_levels_voice(rdram, ctx);
}
void rs64_th_startSlotLoopAnimWithVoiceSync_800B1D18(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_mod_levels_voice(uint8_t*, recomp_context*);
    rs64_mod_levels_voice(rdram, ctx);
}

// allocAndConfigureNewVoice 0x8009B114 (s3 = speech index, s2 = rate) / 0x8009B1F4 (a1 = stream address): a mod level's briefing recording.
void rs64_th_allocAndConfigureNewVoice_8009B114(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_mod_levels_brief_rate(recomp_context*);
    rs64_mod_levels_brief_rate(ctx);
}
void rs64_th_allocAndConfigureNewVoice_8009B1F4(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_mod_levels_brief_stream(uint8_t*, recomp_context*);
    rs64_mod_levels_brief_stream(rdram, ctx);
}

// initCraftSelectScreen 0x800AF0D0
int rs64_th_initCraftSelectScreen_800AF0D0(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(24u, rdram, ctx)) return 1;

    return 0;
}

// tickCraftSelectScreen 0x800AF3EC
int rs64_th_tickCraftSelectScreen_800AF3EC(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(25u, rdram, ctx)) return 1;

    return 0;
}

// hangarInitialize 0x800AAC44
int rs64_th_hangarInitialize_800AAC44(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(26u, rdram, ctx)) return 1;

    return 0;
}

// runHangarSelectionFrame 0x800AADD8
int rs64_th_runHangarSelectionFrame_800AADD8(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(27u, rdram, ctx)) return 1;

    return 0;
}

// initMission 0x800FA5BC
int rs64_th_initMission_800FA5BC(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(28u, rdram, ctx)) return 1;

    return 0;
}

// initVoiceSubtitleSystem 0x80055B08
int rs64_th_initVoiceSubtitleSystem_80055B08(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(29u, rdram, ctx)) return 1;

    return 0;
}

// runInMissionFrame 0x800FAC70
int rs64_th_runInMissionFrame_800FAC70(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(30u, rdram, ctx)) return 1;

    return 0;
}

// renderRadarMinimap 0x800C6290
int rs64_th_renderRadarMinimap_800C6290(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(31u, rdram, ctx)) return 1;

    return 0;
}

// npcPowerUpUpdate 0x800EBD20
void rs64_th_npcPowerUpUpdate_800EBD20(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);
    rs64_hook(34u, rdram, ctx);
}

// npcPowerUpUpdate 0x800EBD34
void rs64_th_npcPowerUpUpdate_800EBD34(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);
    rs64_hook(35u, rdram, ctx);
}

// applyDatObjectiveTriggerEffect 0x80065980
int rs64_th_applyDatObjectiveTriggerEffect_80065980(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);

    if (rs64_hook(36u, rdram, ctx)) return 1;

    return 0;
}

// npcAtAtUpdate 0x800CED7C
void rs64_th_npcAtAtUpdate_800CED7C(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_hook(uint32_t, uint8_t*, recomp_context*);
    rs64_hook(37u, rdram, ctx);
}

// mainGameLoop 0x8003E10C
void rs64_th_mainGameLoop_8003E10C(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_nav_boot_cutscene(uint8_t*, uint32_t*, uint32_t*);
    uint32_t lv = (uint32_t)ctx->r4, kd = (uint32_t)ctx->r5;
    rs64_nav_boot_cutscene(rdram, &lv, &kd);
    ctx->r4 = (gpr)(int32_t)lv;
    ctx->r5 = (gpr)(int32_t)kd;
}

// mainGameLoop 0x8003E114
void rs64_th_mainGameLoop_8003E114(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_nav_boot_cutscene_done(void);
    rs64_nav_boot_cutscene_done();
}

// findManifestEntryByName 0x8000496C
void rs64_th_findManifestEntryByName_8000496C(uint8_t* rdram, recomp_context* ctx) {

    extern void rs64_asset_overrides_patch(uint8_t*, uint32_t);
    rs64_asset_overrides_patch(rdram, (uint32_t)ctx->r20);
}

// freeManifestSegmentAssets 0x80004A0C / 0x80004A80 (the rs_free calls)
void rs64_th_freeManifestSegmentAssets_80004A0C(uint8_t* rdram, recomp_context* ctx)
{

    extern uint32_t rs64_asset_manifest_to_free(uint32_t);
    ctx->r4 = (gpr)(int32_t)rs64_asset_manifest_to_free((uint32_t)ctx->r4);
}
void rs64_th_freeManifestSegmentAssets_80004A80(uint8_t* rdram, recomp_context* ctx)
{

    extern uint32_t rs64_asset_manifest_to_free(uint32_t);
    ctx->r4 = (gpr)(int32_t)rs64_asset_manifest_to_free((uint32_t)ctx->r4);
}

// Mod level objectives: initializeObjectiveTracking / checkComplexObjectives ($f12 = dt) / calculateFriendliesSaved entries skip the
// template's handlers; the pause list's line text (v0 at 0x800C2F1C, s4 -> its index) is the level's own.
int rs64_th_initializeObjectiveTracking_entry(uint8_t* rdram, recomp_context* ctx) {

    extern int rs64_mod_levels_objectives_init(uint8_t*);

    return rs64_mod_levels_objectives_init(rdram);
}
int rs64_th_checkComplexObjectives_entry(uint8_t* rdram, recomp_context* ctx)
{

    extern int rs64_mod_levels_objectives_tick(uint8_t*, recomp_context*);

    return rs64_mod_levels_objectives_tick(rdram, ctx);
}
int rs64_th_calculateFriendliesSaved_entry(uint8_t* rdram, recomp_context* ctx)
{

    extern int rs64_mod_levels_objectives_friendlies(uint8_t*, recomp_context*);

    return rs64_mod_levels_objectives_friendlies(rdram, ctx);
}
void rs64_th_runPauseMenuStateMachine_800C2F1C(uint8_t* rdram, recomp_context* ctx)
{

    extern void rs64_mod_levels_objective_text(uint8_t*, recomp_context*);
    rs64_mod_levels_objective_text(rdram, ctx);
}
