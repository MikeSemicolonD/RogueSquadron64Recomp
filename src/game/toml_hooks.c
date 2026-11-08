// Bodies of the [[patches.hook]] entries in rogue_squadron.toml, one function per hook site, named rs64_th_<func>_<before_vram or entry>.
// The toml text only calls them: { if (f(rdram, ctx)) return; } when the body can return from the game function (return 1), else { f(rdram, ctx); }.
// Compiled in the RecompiledFuncs target, so bodies see rdram, ctx, the MEM_* macros and funcs.h exactly as they did inside the recompiled function.
#include "recomp.h"
#include "../../RecompiledFuncs/funcs.h"
#include "rs64_toml_hooks.h"
#include "rs64/host_api.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
// cinematicLoopBody entry
void rs64_th_cinematicLoopBody_entry(uint8_t* rdram, recomp_context* ctx)
{
    // Cinematic is running, so attribution is over: clear g_current_scene here
    // (the NTSC N64-logo branch that would clear it is skipped) or the
    // attribution VI redirect steals the cinematic's swaps.
    g_current_scene = -1;

    // Periodic yield to break the cinematic thread-starvation stall.
    rs64_cine_yield();

    if (rs64_dbg_hooks_on()) {
        static int s_count = 0;
        ++s_count;

        if (s_count <= 4 || (s_count & 63) == 0)
        {
            rs64_dbg_log4("800A5D80 a0 a1 a2 count", (unsigned)ctx->r4, (unsigned)ctx->r5, (unsigned)ctx->r6, (unsigned)s_count);
        }
    }
}

// load_asset_with_malloc_flags 0x80064884
void rs64_th_load_asset_with_malloc_flags_80064884(uint8_t* rdram, recomp_context* ctx) {

    uint32_t buf_ptr = (uint32_t)ctx->r2;

    if (buf_ptr >= 0x80000000u && buf_ptr < 0x80800000u) {
        int32_t* word_ptr = (int32_t*)(rdram + (buf_ptr - 0x80000000u));

        if (*word_ptr == 0) {
            *word_ptr = -1;
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
        rs64_dbg_log4("game_frame_iter", n, 0, 0, 0);
    }

    // All-displayed deadlock rescue: buffers all in state 5 with none in 1-4 is
    // an illegal steady state (5->0 demotion can't fire, producer can't alloc) =
    // the cinematic freeze. After it persists 32 consumer passes, demote the 5s
    // to 0. r22 = buffer-state base (0x80128EAC).
    static int s_persist = 0;
    static unsigned s_rescued = 0;
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

            rs64_dbg_log4("bufstuck_rescue", s_rescued, 0, 0, 0);
        }
    } else {
        s_persist = 0;
    }
}

// submitGfxFrame 0x8000C07C
void rs64_th_submitGfxFrame_entry(uint8_t* rdram, recomp_context* ctx)
{

    rs64_pause_frame_point();
}

// runMenuModelViewerScreen 0x800C29F8
void rs64_th_runMenuModelViewerScreen_800C29F8(uint8_t* rdram, recomp_context* ctx)
{

    static int n = 0;

    if (rs64_dbg_hooks_on() && ++n <= 40) {
        rs64_dbg_log4("attrib_iter n f22x1k f24x1k", (unsigned)n, (unsigned)(ctx->f22.fl * 1000.0f), (unsigned)(ctx->f24.fl * 1000.0f), 0);
    }

    rs64_attrib_wait_vi_yield(rdram, ctx);
}

// tickFormatMessageWorker 0x800B380C
void rs64_th_tickFormatMessageWorker_800B380C(uint8_t* rdram, recomp_context* ctx)
{

    ctx->r6 = (gpr)(int32_t)rs64_format_reply_slot(rdram, ctx, (uint32_t)ctx->r6);
}

// initVideoSubsystem 0x8001A2EC
void rs64_th_initVideoSubsystem_8001A2EC(uint8_t* rdram, recomp_context* ctx) {

    rs64_fb_cpu_cleared(rdram, (uint32_t)ctx->r17, (uint32_t)ctx->r16);
}

// updateXwingFlightControls 0x800B48EC
void rs64_th_updateXwingFlightControls_800B48EC(uint8_t* rdram, recomp_context* ctx)
{

    ctx->f20.fl = rs64_throttle_hook(rdram, ctx->f20.fl, (uint32_t)ctx->r18 + 0x14, 0x800A58C4, 0x800A58D0, 0x800A58DC, (uint32_t)ctx->r18 + 0x4);
}

// updateAwingFlightControls 0x800B5D54
void rs64_th_updateAwingFlightControls_800B5D54(uint8_t* rdram, recomp_context* ctx)
{

    ctx->f20.fl = rs64_throttle_hook(rdram, ctx->f20.fl, 0, 0x800A5A44, 0x800A5A4C, 0x800A5A54, 0);
}

// updateYwingFlightControls 0x800B7984
void rs64_th_updateYwingFlightControls_800B7984(uint8_t* rdram, recomp_context* ctx)
{

    ctx->f20.fl = rs64_throttle_hook(rdram, ctx->f20.fl, 0, 0x800A5B88, 0, 0x800A5B90, 0);
}

// updateVwingFlightControls 0x800B9280
void rs64_th_updateVwingFlightControls_800B9280(uint8_t* rdram, recomp_context* ctx)
{

    ctx->f24.fl = rs64_throttle_hook(rdram, ctx->f24.fl, 0, 0x800A5CB0, 0x800A5CB8, 0x800A5CC0, (uint32_t)ctx->r21 + 0x190);
}

// updateFalconFlightControls 0x800BA8F4
void rs64_th_updateFalconFlightControls_800BA8F4(uint8_t* rdram, recomp_context* ctx)
{

    ctx->f20.fl = rs64_throttle_hook(rdram, ctx->f20.fl, (uint32_t)ctx->r18 + 0x4, 0x800A5DD8, 0x800A5DE0, 0x800A5DE8, 0);
}

// updateTieInterceptorFlightControls 0x800BC190
void rs64_th_updateTieInterceptorFlightControls_800BC190(uint8_t* rdram, recomp_context* ctx)
{

    ctx->f20.fl = rs64_throttle_hook(rdram, ctx->f20.fl, (uint32_t)ctx->r18 + 0x4, 0x800A5F40, 0x800A5F48, 0x800A5F50, 0);
}

// updateT16FlightControls 0x800BD148
void rs64_th_updateT16FlightControls_800BD148(uint8_t* rdram, recomp_context* ctx)
{

    ctx->f20.fl = rs64_throttle_hook(rdram, ctx->f20.fl, (uint32_t)ctx->r18 + 0x4, 0x800A604C, 0x800A6054, 0x800A605C, 0);
}

// updateNabooStarfighterFlightControls 0x800BDEB0
void rs64_th_updateNabooStarfighterFlightControls_800BDEB0(uint8_t* rdram, recomp_context* ctx)
{

    ctx->f20.fl = rs64_throttle_hook(rdram, ctx->f20.fl, 0, 0x800A6144, 0x800A6150, 0x800A6154, 0);
}

// updateSnowSpeederFlightControls 0x800ABDA0
void rs64_th_updateSnowSpeederFlightControls_800ABDA0(uint8_t* rdram, recomp_context* ctx)
{

    const int half = MEM_BU(0x2D, ctx->r18) != 0;
    const uint16_t held = MEM_HU(0, (gpr)(int32_t)0x80130B88);
    const uint16_t brakes = MEM_HU(0x14, (gpr)(int32_t)0x8010BE18) | MEM_HU(0x16, (gpr)(int32_t)0x8010BE18);

    if (!(held & brakes)) ctx->f22.fl = rs64_throttle_hook(rdram, ctx->f22.fl, 0, half ? 0x800A5318 : 0x800A531C, half ? 0x800A5308 : 0x800A530C, half ? 0x800A5328 : 0x800A532C, 0);
}

// Set just before the render paths call the view test; the view test takes it on entry, so its gameplay callers keep the exact test.
static int s_render_view_test_next;
static int s_render_view_test;

// While recording or replaying, the render paths' own tests stay stock so game state never depends on the display settings; a second draw-only pass applies the widening.
static int s_npc_draw_only_pass;
static uint32_t s_npc_view_args[4];

// ROGUESQ_LOG_POP: where the last view test put the object, 1.0 being the 4:3 screen edge.
static float s_view_nx, s_view_ny;
static int s_view_seen;

void rs64_npc_draw_only_mark(int on) {
    s_npc_draw_only_pass = on;
}

// Set just before the effect handlers' view tests, which take the draw distance far plane like the render paths. Saved $ra can't tell callers apart: jal doesn't write r31.
static int s_effect_view_test_next;
static int s_effect_view_test;

// Set when the last view test failed the game's own far plane; such an object passes only through the draw distance.
static int s_past_stock_far;

// randomBurstNpcHandlerVariant 0x8004093C
void rs64_th_randomBurstNpcHandlerVariant_8004093C(uint8_t* rdram, recomp_context* ctx) {

    s_effect_view_test_next = 1;
}

// sprayEffectEmitterNpcHandler 0x8005E1A4
void rs64_th_sprayEffectEmitterNpcHandler_8005E1A4(uint8_t* rdram, recomp_context* ctx) {

    s_effect_view_test_next = 1;
}

// updateWalkerParticlePositions 0x8005A9B8
void rs64_th_updateWalkerParticlePositions_8005A9B8(uint8_t* rdram, recomp_context* ctx) {

    s_effect_view_test_next = 1;
}

// randomlyScaledEffectNpcHandler 0x800607D8
void rs64_th_randomlyScaledEffectNpcHandler_800607D8(uint8_t* rdram, recomp_context* ctx) {

    s_effect_view_test_next = 1;
}

// randomlyScaledColoredEffectNpcHandler 0x80060C40
void rs64_th_randomlyScaledColoredEffectNpcHandler_80060C40(uint8_t* rdram, recomp_context* ctx) {

    s_effect_view_test_next = 1;
}

// minimalRandomNpcHandler 0x80060FE0
void rs64_th_minimalRandomNpcHandler_80060FE0(uint8_t* rdram, recomp_context* ctx) {

    s_effect_view_test_next = 1;
}

// explosionEffectNpcHandler 0x80074520
void rs64_th_explosionEffectNpcHandler_80074520(uint8_t* rdram, recomp_context* ctx) {

    s_effect_view_test_next = 1;
}

// randomParticleWithParentNpcHandler 0x800751AC
void rs64_th_randomParticleWithParentNpcHandler_800751AC(uint8_t* rdram, recomp_context* ctx) {

    s_effect_view_test_next = 1;
}

// heavyRandomBurstNpcHandler 0x80075AF4
void rs64_th_heavyRandomBurstNpcHandler_80075AF4(uint8_t* rdram, recomp_context* ctx) {

    s_effect_view_test_next = 1;
}

// heavyMechSubEffectNpcHandler 0x80079958
void rs64_th_heavyMechSubEffectNpcHandler_80079958(uint8_t* rdram, recomp_context* ctx) {

    s_effect_view_test_next = 1;
}

// addNpcToVisibilityBucket 0x80057D84
void rs64_th_addNpcToVisibilityBucket_80057D84(uint8_t* rdram, recomp_context* ctx) {

    s_render_view_test_next = 1;
}

// addNpcToVisibilityBucket 0x80057E08
void rs64_th_addNpcToVisibilityBucket_80057E08(uint8_t* rdram, recomp_context* ctx) {

    s_render_view_test_next = 1;
}

// addNpcToVisibilityBucket 0x80057E48
void rs64_th_addNpcToVisibilityBucket_80057E48(uint8_t* rdram, recomp_context* ctx) {

    s_render_view_test_next = 1;
}


// transformVec3ToNpcLocalWithScale entry
void rs64_th_transformVec3ToNpcLocalWithScale_entry(uint8_t* rdram, recomp_context* ctx) {

    s_render_view_test = s_render_view_test_next && (s_npc_draw_only_pass || !rs64_ls_session());
    s_render_view_test_next = 0;
    s_effect_view_test = s_effect_view_test_next;
    s_effect_view_test_next = 0;
    s_past_stock_far = 0;
}

// Render-path view test, RT shadows on: true when the object (camera space at sp+0x10, radius r) is within the shadow keep distance in any direction.
static int shadow_keep(uint8_t* rdram, recomp_context* ctx, float r)
{

    const float d = s_render_view_test ? rs64_shadow_keep_dist() : 0.0f;
    union { uint32_t u; float f; } x, y, z;

    if (!(d > 0.0f))
    {
        return 0;
    }

    x.u = (uint32_t)MEM_W(0x10, ctx->r29);
    y.u = (uint32_t)MEM_W(0x14, ctx->r29);
    z.u = (uint32_t)MEM_W(0x18, ctx->r29);

    return (x.f * x.f + y.f * y.f + z.f * z.f) < (d + r) * (d + r);
}

// Moves a kept object to the screen centre at the near plane (sp+0x10/0x14/0x18), so the rest of the test passes; the caller's depth out was already written.
static void shadow_keep_centre(uint8_t* rdram, recomp_context* ctx)
{

    MEM_W(0x10, ctx->r29) = 0;
    MEM_W(0x14, ctx->r29) = 0;
    MEM_W(0x18, ctx->r29) = MEM_W(0x24, ctx->r17);
}

// transformVec3ToNpcLocalWithScale 0x8001DD70: the sphere test's behind-the-camera check ($f0 = z + r); the near clamp that follows stores near when $f4 < near.
void rs64_th_transformVec3ToNpcLocalWithScale_8001DD70(uint8_t* rdram, recomp_context* ctx)
{

    if (!shadow_keep(rdram, ctx, ctx->f22.fl))
    {
        return;
    }

    shadow_keep_centre(rdram, ctx);
    ctx->f0.fl = 1.0f;
    ctx->f4.fl = 0.0f;
}

// transformVec3ToNpcLocalWithScale 0x8001E064: the point test's behind-the-camera check ($f2 = z, reused as the projection depth).
void rs64_th_transformVec3ToNpcLocalWithScale_8001E064(uint8_t* rdram, recomp_context* ctx)
{

    if (!shadow_keep(rdram, ctx, 0.0f))
    {
        return;
    }

    shadow_keep_centre(rdram, ctx);
    ctx->f2.u32l = (uint32_t)MEM_W(0x24, ctx->r17);
}

// transformVec3ToNpcLocalWithScale 0x8001DEA0: the sphere test's projected x, y and radius ($f10, $f12, $f22); shrinking them widens its field of view.
void rs64_th_transformVec3ToNpcLocalWithScale_8001DEA0(uint8_t* rdram, recomp_context* ctx) {

    s_view_nx = ctx->f10.fl / (ctx->f0.fl * ctx->f2.fl);
    s_view_ny = ctx->f12.fl / ctx->f6.fl;
    s_view_seen = 1;

    if (s_render_view_test || s_effect_view_test || !rs64_ls_session()) ctx->f2.fl *= rs64_cull_widen();

    if (s_render_view_test) {
        const float m = rs64_shadow_cull_margin();

        ctx->f10.fl /= m;
        ctx->f12.fl /= m;
        ctx->f22.fl /= m;
    }
}

// transformVec3ToNpcLocalWithScale 0x8001E168: the point test's projected x ($f8) and the scale its y is projected with next ($f20).
void rs64_th_transformVec3ToNpcLocalWithScale_8001E168(uint8_t* rdram, recomp_context* ctx) {

    if (s_render_view_test || s_effect_view_test || !rs64_ls_session()) ctx->f2.fl *= rs64_cull_widen();

    if (s_render_view_test) {
        const float m = rs64_shadow_cull_margin();

        ctx->f8.fl /= m;
        ctx->f20.fl /= m;
    }
}

// Units out of the active range for 21 frames are destroyed and respawned at their start, a loop the stock far plane hid: their range test
// widens with the draw distance in normal play, so a unit in view stays where it is.
static int s_unit_active_range = 0;

// isNpcWithinActiveReferenceRange entry
void rs64_th_isNpcWithinActiveReferenceRange_80047D40(uint8_t* rdram, recomp_context* ctx)
{

    s_unit_active_range = 1;
}

// isPointXzWithinRangeOfRef 0x80047DF0: the squared range ($f0) against the squared distance ($f2).
void rs64_th_isPointXzWithinRangeOfRef_80047DF0(uint8_t* rdram, recomp_context* ctx)
{

    const float d = rs64_draw_dist();

    if (s_unit_active_range && d > 1.0f && !rs64_ls_session()) ctx->f0.fl *= d * d;

    s_unit_active_range = 0;
}

// setupCameraMatrices 0x80016CE0
void rs64_th_setupCameraMatrices_80016CE0(uint8_t* rdram, recomp_context* ctx) {

    ctx->f0.fl *= rs64_draw_dist();
}

// setupCameraMatrices 0x80016D50
void rs64_th_setupCameraMatrices_80016D50(uint8_t* rdram, recomp_context* ctx) {

    ctx->f0.fl *= rs64_draw_dist();
}

// drawSubtitleText 0x80015EF8
void rs64_th_drawSubtitleText_80015EF8(uint8_t* rdram, recomp_context* ctx) {

    rs64_hud_mark_element(rdram, ctx);
}

// refreshEffectObjectCamerasAndState 0x80053148
void rs64_th_refreshEffectObjectCamerasAndState_80053148(uint8_t* rdram, recomp_context* ctx) {

    rs64_hud_shift_radar(rdram, ctx);
}

// The view test's far check ($f2 = depth < $f0 = far): scaled by the draw distance for the render and effect callers.
static void far_check(recomp_context* ctx)
{

    s_past_stock_far |= !(ctx->f2.fl < ctx->f0.fl);

    if (s_render_view_test || s_effect_view_test) ctx->f0.fl *= rs64_draw_dist();
}

// The view test's camera-space box (+-125 per axis, the reach of convertMat3x4ToN64Mtx's fixed point): widened by the draw distance for the
// render callers, whose far translations RT64 takes from the far-matrix table. Notes an object outside the stock box (camera space at sp+0x10, radius r).
static float view_box_scale(uint8_t* rdram, recomp_context* ctx, float r)
{

    union { uint32_t u; float f; } v;

    for (int i = 0; i < 3; ++i) {
        v.u = (uint32_t)MEM_W(0x10 + 4 * i, ctx->r29);
        s_past_stock_far |= (v.f - r < -125.0f) || (v.f + r > 125.0f);
    }

    return s_render_view_test ? fmaxf(rs64_draw_dist(), 1.0f) : 1.0f;
}

// transformVec3ToNpcLocalWithScale 0x8001DCEC: the sphere test's low bound ($f2), radius $f22.
void rs64_th_transformVec3ToNpcLocalWithScale_8001DCEC(uint8_t* rdram, recomp_context* ctx)
{
    ctx->f2.fl *= view_box_scale(rdram, ctx, ctx->f22.fl);
}

// transformVec3ToNpcLocalWithScale 0x8001DD38: the sphere test's high bound ($f2).
void rs64_th_transformVec3ToNpcLocalWithScale_8001DD38(uint8_t* rdram, recomp_context* ctx)
{

    if (s_render_view_test) ctx->f2.fl *= fmaxf(rs64_draw_dist(), 1.0f);
}

// transformVec3ToNpcLocalWithScale 0x8001DFF4: the point test's low bound ($f0).
void rs64_th_transformVec3ToNpcLocalWithScale_8001DFF4(uint8_t* rdram, recomp_context* ctx)
{
    ctx->f0.fl *= view_box_scale(rdram, ctx, 0.0f);
}

// transformVec3ToNpcLocalWithScale 0x8001E034: the point test's high bound ($f0).
void rs64_th_transformVec3ToNpcLocalWithScale_8001E034(uint8_t* rdram, recomp_context* ctx)
{

    if (s_render_view_test) ctx->f0.fl *= fmaxf(rs64_draw_dist(), 1.0f);
}

// Camera-space translations an N64 matrix can't hold (t * 2^24 must fit 32 bits, so |t| < 128), 16 bytes per slot by matrix address:
// address, x, y, z. RT64's op_01_matrix takes the translation from a slot whose address matches.
#define RS64_FAR_MTX_TABLE 0x80A4C000u
#define RS64_FAR_MTX_SLOTS 0x1000u

// convertMat3x4ToN64Mtx 0x80018DEC: translation in $f4/$f6/$f8, destination in $a0. Out of range: saved to the table and clamped for the pack.
void rs64_th_convertMat3x4ToN64Mtx_80018DEC(uint8_t* rdram, recomp_context* ctx)
{

    static int s_registered;
    const uint32_t dst = (uint32_t)ctx->r4;
    const gpr slot = (gpr)(int32_t)(RS64_FAR_MTX_TABLE + ((dst >> 6) & (RS64_FAR_MTX_SLOTS - 1)) * 16);
    fpr* t[3] = { &ctx->f4, &ctx->f6, &ctx->f8 };
    int outside = 0;

    for (int i = 0; i < 3; ++i) {
        outside |= !(fabsf(t[i]->fl) < 127.0f);
    }

    if (!outside) {
        if ((uint32_t)MEM_W(0, slot) == dst) MEM_W(0, slot) = 0;

        return;
    }

    if (!s_registered) {
        rs64_snapshot_add_range(RS64_FAR_MTX_TABLE - 0x80000000u, RS64_FAR_MTX_TABLE - 0x80000000u + RS64_FAR_MTX_SLOTS * 16);
        s_registered = 1;
    }

    MEM_W(0, slot) = (int32_t)dst;

    for (int i = 0; i < 3; ++i) {
        MEM_W(4 + 4 * i, slot) = (int32_t)t[i]->u32l;
        t[i]->fl = fminf(fmaxf(t[i]->fl, -127.0f), 127.0f);
    }
}

// transformVec3ToNpcLocalWithScale 0x8001DDA0
void rs64_th_transformVec3ToNpcLocalWithScale_8001DDA0(uint8_t* rdram, recomp_context* ctx)
{
    far_check(ctx);
}

// transformVec3ToNpcLocalWithScale 0x8001E078
void rs64_th_transformVec3ToNpcLocalWithScale_8001E078(uint8_t* rdram, recomp_context* ctx)
{
    far_check(ctx);
}

// selectMeshLodAndSubmitForRender 0x80058264: the model's per-frame flag toggle runs when $v1 == 0x80000; skipped for an object past the game's far plane.
void rs64_th_selectMeshLodAndSubmitForRender_80058264(uint8_t* rdram, recomp_context* ctx)
{

    if (s_past_stock_far) {
        ctx->r3 = 0;
    }
}

// Objects past the game's far plane drawn with the far-plane scale: their instance matrix (+0x28) rotation rows as the culled path built them,
// put back once the frame's scene graph has been converted, so gameplay reads the stock matrix.
#define RS64_INFLATED_MAX 256
static struct { uint32_t addr; uint32_t rows[9]; } s_inflated[RS64_INFLATED_MAX];
static int s_inflated_count;

// selectMeshLodAndSubmitForRender 0x80058304: after the instance matrix ($s1+0x28) is built with scale $f22. An object past the game's far plane
// gets the scale a drawn object has at the far plane ($f22 + $f20) for this frame's draw only.
void rs64_th_selectMeshLodAndSubmitForRender_80058304(uint8_t* rdram, recomp_context* ctx)
{

    const gpr m = ctx->r17 + 0x28;
    union { uint32_t u; float f; } v;

    if (!s_past_stock_far || s_inflated_count >= RS64_INFLATED_MAX || !(ctx->f22.fl != 0.0f))
    {
        return;
    }

    const float k = (ctx->f22.fl + ctx->f20.fl) / ctx->f22.fl;

    s_inflated[s_inflated_count].addr = (uint32_t)m;

    for (int i = 0; i < 9; ++i) {
        v.u = (uint32_t)MEM_W(4 * i, m);
        s_inflated[s_inflated_count].rows[i] = v.u;
        v.f *= k;
        MEM_W(4 * i, m) = (int32_t)v.u;
    }

    s_inflated_count++;
}

// runInMissionFrame 0x800FAC68
void rs64_th_runInMissionFrame_800FAC68(uint8_t* rdram, recomp_context* ctx) {

    rs64_standins_submit(rdram, ctx);
}

// setupCameraMatrices 0x800178E4: the scene graph has been converted to the display list, so the radar node and the inflated matrices go back.
void rs64_th_setupCameraMatrices_800178E4(uint8_t* rdram, recomp_context* ctx)
{

    rs64_hud_radar_restore(rdram);

    for (int n = 0; n < s_inflated_count; ++n) {
        for (int i = 0; i < 9; ++i) {
            MEM_W(4 * i, (gpr)(int32_t)s_inflated[n].addr) = (int32_t)s_inflated[n].rows[i];
        }
    }

    s_inflated_count = 0;
}

// selectMeshLodAndSubmitForRender 0x800582F0: a drawn object's instance matrix scale gains a depth term ($f0); collision reads that matrix, so an
// object past the game's far plane keeps the culled path's scale (the 0x80058304 hook draws it inflated).
void rs64_th_selectMeshLodAndSubmitForRender_800582F0(uint8_t* rdram, recomp_context* ctx)
{

    if (s_past_stock_far) {
        ctx->f0.fl = 0.0f;
    }
}

// addNpcToVisibilityBucket 0x80057C90
void rs64_th_addNpcToVisibilityBucket_80057C90(uint8_t* rdram, recomp_context* ctx)
{

    if (s_npc_draw_only_pass)
    {
        return;
    }

    s_npc_view_args[0] = (uint32_t)ctx->r4;
    s_npc_view_args[1] = (uint32_t)ctx->r5;
    s_npc_view_args[2] = (uint32_t)ctx->r6;
    s_npc_view_args[3] = (uint32_t)ctx->r7;
    s_view_seen = 0;

    rs64_f5_cable_submit((uint32_t)MEM_W(0, (gpr)(int32_t)0x8010B6E0), (uint32_t)ctx->r5, (uint32_t)MEM_W(0, (gpr)(int32_t)0x8010B6E8));
}

// LOD depth ($f2): divided by the draw distance; an object kept behind the camera for shadows (negative depth) uses its distance, not the closest LOD.
static void lod_depth(recomp_context* ctx)
{

    if ((ctx->f2.fl < 0.0f) && (rs64_shadow_keep_dist() > 0.0f)) ctx->f2.fl = -ctx->f2.fl;

    ctx->f2.fl /= rs64_draw_dist();
}

// addNpcToVisibilityBucket 0x80057EAC
void rs64_th_addNpcToVisibilityBucket_80057EAC(uint8_t* rdram, recomp_context* ctx) {

    lod_depth(ctx);
}

// selectMeshLodAndSubmitForRender 0x8005813C
void rs64_th_selectMeshLodAndSubmitForRender_8005813C(uint8_t* rdram, recomp_context* ctx) {

    lod_depth(ctx);
}

// buildVisibleTerrainGridAroundCamera 0x8004392C
void rs64_th_buildVisibleTerrainGridAroundCamera_8004392C(uint8_t* rdram, recomp_context* ctx) {

    ctx->f2.fl *= rs64_terrain_dist();
}

// scanConvertTriangleToSpanTable 0x800425D8
void rs64_th_scanConvertTriangleToSpanTable_800425D8(uint8_t* rdram, recomp_context* ctx) {

    ctx->r12 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r12);
}

// scanConvertTriangleToSpanTable 0x800425E0
void rs64_th_scanConvertTriangleToSpanTable_800425E0(uint8_t* rdram, recomp_context* ctx) {

    ctx->r8 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r8);
}

// scanConvertTriangleToSpanTable 0x800425E8
void rs64_th_scanConvertTriangleToSpanTable_800425E8(uint8_t* rdram, recomp_context* ctx) {

    ctx->r7 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r7);
}

// scanConvertTriangleToSpanTable 0x80042720
void rs64_th_scanConvertTriangleToSpanTable_80042720(uint8_t* rdram, recomp_context* ctx) {

    ctx->r12 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r12);
}

// scanConvertTriangleToSpanTable 0x80042728
void rs64_th_scanConvertTriangleToSpanTable_80042728(uint8_t* rdram, recomp_context* ctx) {

    ctx->r8 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r8);
}

// scanConvertTriangleToSpanTable 0x80042730
void rs64_th_scanConvertTriangleToSpanTable_80042730(uint8_t* rdram, recomp_context* ctx) {

    ctx->r7 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r7);
}

// rasterizeEdgeXSpansAscending 0x80042000
void rs64_th_rasterizeEdgeXSpansAscending_80042000(uint8_t* rdram, recomp_context* ctx) {

    ctx->r2 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r2);
}

// rasterizeEdgeXSpansAscending 0x80042050
void rs64_th_rasterizeEdgeXSpansAscending_80042050(uint8_t* rdram, recomp_context* ctx) {

    ctx->r2 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r2);
}

// rasterizeEdgeXSpansAscending 0x80042078
void rs64_th_rasterizeEdgeXSpansAscending_80042078(uint8_t* rdram, recomp_context* ctx) {

    ctx->r3 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r3);
}

// rasterizeEdgeXSpansAscending 0x800420A4
void rs64_th_rasterizeEdgeXSpansAscending_800420A4(uint8_t* rdram, recomp_context* ctx) {

    ctx->r2 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r2);
}

// rasterizeEdgeXSpansAscending 0x800420CC
void rs64_th_rasterizeEdgeXSpansAscending_800420CC(uint8_t* rdram, recomp_context* ctx) {

    ctx->r2 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r2);
}

// rasterizeEdgeXSpansDescending 0x80042174
void rs64_th_rasterizeEdgeXSpansDescending_80042174(uint8_t* rdram, recomp_context* ctx) {

    ctx->r3 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r3);
}

// rasterizeEdgeXSpansDescending 0x800421A0
void rs64_th_rasterizeEdgeXSpansDescending_800421A0(uint8_t* rdram, recomp_context* ctx) {

    ctx->r2 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r2);
}

// rasterizeEdgeXSpansDescending 0x800421CC
void rs64_th_rasterizeEdgeXSpansDescending_800421CC(uint8_t* rdram, recomp_context* ctx) {

    ctx->r2 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r2);
}

// rasterizeEdgeXSpansDescending 0x80042224
void rs64_th_rasterizeEdgeXSpansDescending_80042224(uint8_t* rdram, recomp_context* ctx) {

    ctx->r2 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r2);
}

// rasterizeEdgeXSpansDescending 0x80042274
void rs64_th_rasterizeEdgeXSpansDescending_80042274(uint8_t* rdram, recomp_context* ctx) {

    ctx->r2 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r2);
}

// binPolygonIntoTerrainGrid 0x80042A4C
void rs64_th_binPolygonIntoTerrainGrid_80042A4C(uint8_t* rdram, recomp_context* ctx) {

    ctx->r10 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r10);
}

// binPolygonIntoTerrainGrid 0x80042A54
void rs64_th_binPolygonIntoTerrainGrid_80042A54(uint8_t* rdram, recomp_context* ctx) {

    ctx->r9 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r9);
}

// streamTerrainGridCellsByDistance 0x80042CF4
void rs64_th_streamTerrainGridCellsByDistance_80042CF4(uint8_t* rdram, recomp_context* ctx) {

    ctx->r13 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r13);
}

// streamTerrainGridCellsByDistance 0x80042D24
void rs64_th_streamTerrainGridCellsByDistance_80042D24(uint8_t* rdram, recomp_context* ctx) {

    ctx->r10 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r10);
}

// streamTerrainGridCellsByDistance 0x80042D4C
void rs64_th_streamTerrainGridCellsByDistance_80042D4C(uint8_t* rdram, recomp_context* ctx) {

    ctx->r14 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r14);
}

// buildGridCellMeshForFace 0x80043534
void rs64_th_buildGridCellMeshForFace_80043534(uint8_t* rdram, recomp_context* ctx) {

    ctx->r8 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r8);
}

// buildGridCellMeshForFace 0x8004368C
void rs64_th_buildGridCellMeshForFace_8004368C(uint8_t* rdram, recomp_context* ctx) {

    ctx->r7 = (gpr)(int32_t)rs64_tgrid_base((uint32_t)ctx->r7);
}

// loadLevelAssets 0x800453C8
void rs64_th_loadLevelAssets_800453C8(uint8_t* rdram, recomp_context* ctx) {

    rs64_tgrid_clear(rdram);
}

// buildVisibleTerrainGridAroundCamera 0x80043A84
void rs64_th_buildVisibleTerrainGridAroundCamera_80043A84(uint8_t* rdram, recomp_context* ctx) {

    rs64_tgrid_clamp_polygon(rdram);
}

// loadLevelAssets 0x800453D8
void rs64_th_loadLevelAssets_800453D8(uint8_t* rdram, recomp_context* ctx) {

    ctx->r16 = (gpr)(int32_t)rs64_tgrid_budget((uint32_t)ctx->r16);
}

// loadLevelAssets 0x800454D0
void rs64_th_loadLevelAssets_800454D0(uint8_t* rdram, recomp_context* ctx) {

    rs64_level_fog_frac(ctx->f0.fl);
}

// computeGridLayerScrollOffset 0x800456D4
void rs64_th_computeGridLayerScrollOffset_800456D4(uint8_t* rdram, recomp_context* ctx) {

    const float td = rs64_terrain_dist();
    ctx->f0.fl *= td * td;
}

// setupCutsceneLevel 0x800A6D3C
void rs64_th_setupCutsceneLevel_800A6D3C(uint8_t* rdram, recomp_context* ctx) {

    rs64_cutscene_level_load(1);
}

// setupCutsceneLevel 0x800A6D44
void rs64_th_setupCutsceneLevel_800A6D44(uint8_t* rdram, recomp_context* ctx) {

    rs64_cutscene_level_load(0);
}

// streamTerrainGridCellsByDistance 0x80042DDC
void rs64_th_streamTerrainGridCellsByDistance_80042DDC(uint8_t* rdram, recomp_context* ctx) {

    rs64_tgrid_budget_probe((uint32_t)ctx->r7, (uint32_t)ctx->r2);
}

// buildVisibleTerrainGridAroundCamera 0x80043A8C
void rs64_th_buildVisibleTerrainGridAroundCamera_80043A8C(uint8_t* rdram, recomp_context* ctx)
{

    rs64_terrain_grid_log(MEM_W(0, (gpr)(int32_t)0x80132DB0), MEM_W(0, (gpr)(int32_t)0x80132DB4), MEM_W(0, (gpr)(int32_t)0x80132DB8), MEM_W(0, (gpr)(int32_t)0x80132DBC), MEM_HU(0, (gpr)(int32_t)0x80130C4C), MEM_HU(0, (gpr)(int32_t)0x80136DF8), MEM_HU(0, (gpr)(int32_t)0x80136DFA));
}

// setupCameraMatrices 0x800170D0
void rs64_th_setupCameraMatrices_800170D0(uint8_t* rdram, recomp_context* ctx) {

    rs64_fog_color_note(rdram);
    ctx->f4.fl *= rs64_fog_dist();
}

// setupCameraMatrices 0x80017168
void rs64_th_setupCameraMatrices_80017168(uint8_t* rdram, recomp_context* ctx) {

    ctx->f4.fl *= rs64_fog_dist();
}

// cutsceneActorNpcHandler 0x80044924
void rs64_th_cutsceneActorNpcHandler_80044924(uint8_t* rdram, recomp_context* ctx) {

    rs64_fog_probe(rdram);
}

// linkDirectionalTrailSegments 0x8005B418
void rs64_th_linkDirectionalTrailSegments_8005B418(uint8_t* rdram, recomp_context* ctx)
{
    if (rs64_cull_widen() > 1.0f) {
        ctx->r7 = 0;
        ctx->r6 = 7;
    }
}

// buildVisibleTerrainGridAroundCamera 0x8004395C
void rs64_th_buildVisibleTerrainGridAroundCamera_8004395C(uint8_t* rdram, recomp_context* ctx) {

    ctx->f6.fl *= rs64_cull_widen();
}

// addNpcToVisibilityBucket 0x8005804C (exit): an object the stock test rejected gets the widened draw-only pass.
void rs64_th_addNpcToVisibilityBucket_8005804C(uint8_t* rdram, recomp_context* ctx) {

    if (s_npc_draw_only_pass)
    {
        return;
    }

    const int drawn = (ctx->r2 != 0) || (rs64_ls_session() && rs64_npc_draw_only_pass(rdram, ctx, s_npc_view_args));

    rs64_pop_note(rdram, ctx, s_npc_view_args[1], s_npc_view_args[2], drawn, s_view_seen, s_view_nx, s_view_ny);
}

// projectModelBoundsToScreen 0x80065FE4: the corner just moved to camera space ($s0, x first).
void rs64_th_projectModelBoundsToScreen_80065FE4(uint8_t* rdram, recomp_context* ctx) {

    union { uint32_t u; float f; } x;

    x.u = (uint32_t)MEM_W(0, ctx->r16);
    x.f /= rs64_cull_widen();
    MEM_W(0, ctx->r16) = (int32_t)x.u;
}

// runMenuShipSelectScreen 0x800C14A8
void rs64_th_runMenuShipSelectScreen_800C14A8(uint8_t* rdram, recomp_context* ctx)
{

    rs64_credits_wait_vi_cadence(rdram, ctx);
}

// menuOverlayInit entry
void rs64_th_menuOverlayInit_entry(uint8_t* rdram, recomp_context* ctx)
{

    rs64_dbg_log4("menuOverlayInit", (unsigned)ctx->r5, (unsigned)ctx->r6, 0, 0);
    g_current_scene = (int)(unsigned)ctx->r5;
    rs64_mod_levels_menu((int)ctx->r5);
}

// musyxKeyOnVoice entry
int rs64_th_musyxKeyOnVoice_entry(uint8_t* rdram, recomp_context* ctx)
{

    return MEM_HU(ctx->r4, 0X60) == 0 ? 1 : 0;
}

// musyxSynthFrame 0x80091798
void rs64_th_musyxSynthFrame_80091798(uint8_t* rdram, recomp_context* ctx) {

    uint32_t cb = (uint32_t)(uint64_t)ctx->r2;

    if (cb >= 0x80000400u && cb < 0x80110000u) {
        ctx->r4 = ctx->r16;   // delay slot: a0 = s0
        LOOKUP_FUNC(ctx->r2)(rdram, ctx);
    }
}

// loadOverlay 0x80000B3C
void rs64_th_loadOverlay_80000B3C(uint8_t* rdram, recomp_context* ctx) {

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
    return 1;
}

// cinematicLoopBody 0x800A6024
void rs64_th_cinematicLoopBody_800A6024(uint8_t* rdram, recomp_context* ctx)
{
    // Freeze watchdog: host poll dumps the stack if no tick for 2s.
    static unsigned s_watchdog_count = 0;
    rs64_cine_iter_tick(++s_watchdog_count);

    rs64_cine_pause_point(rdram);

    // Diagnostic: iterations + gate/cineState. ROGUESQ_LOG_HOOKS=1.
    if (rs64_dbg_hooks_on()) {
        static int s_count = 0;
        int n = ++s_count;

        if (n <= 4 || (n & 127) == 0)
        {

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

    rs64_wait_gfx_parse_yield(rdram, ctx);
}

// drainMeshReleaseQueue entry
void rs64_th_drainMeshReleaseQueue_entry(uint8_t* rdram, recomp_context* ctx) {

    rs64_wait_gfx_parse_yield(rdram, ctx);
}

// heapFreeListInsert entry
void rs64_th_heapFreeListInsert_entry(uint8_t* rdram, recomp_context* ctx) {

    rs64_wait_gfx_parse_yield(rdram, ctx);
}

// beginFrameDLChunk entry
void rs64_th_beginFrameDLChunk_entry(uint8_t* rdram, recomp_context* ctx) {

    rs64_wait_gfx_parse_yield(rdram, ctx);
}

// emitFrameRdpInitDl entry
void rs64_th_emitFrameRdpInitDl_entry(uint8_t* rdram, recomp_context* ctx) {

    rs64_wait_gfx_parse_yield(rdram, ctx);
}

// setupMenuData 0x800BAF70
void rs64_th_setupMenuData_800BAF70(uint8_t* rdram, recomp_context* ctx) {

    const int n = rs64_menu_install_main(rdram);

    // A mod page (id 13+) skipped the per-menu builders with $s3 = 0; the tail's active-mask loop counts $s3, not +0x95.
    if (n >= 0) ctx->r19 = n;
}

// applyLevelScreenAndCameraSettings entry
void rs64_th_applyLevelScreenAndCameraSettings_entry(uint8_t* rdram, recomp_context* ctx) {

    rs64_menu_hires_apply(rdram);
}

// A volume slider end label ($a1) is centred on x ($a3), so one longer than the game's 3-letter OFF/MAX moves out by half its extra width (about 8 units a letter).
static int slider_label_shift(uint8_t* rdram, recomp_context* ctx)
{

    int len = 0;

    while (len < 32 && MEM_BU(len, ctx->r5) != 0) len++;

    return len > 3 ? (len - 3) * 4 : 0;
}

// menuControllerInput 0x800B7620: draws the slider's low-end label.
void rs64_th_menuControllerInput_800B7620(uint8_t* rdram, recomp_context* ctx) {

    ctx->r7 = (gpr)(int32_t)((int32_t)ctx->r7 - slider_label_shift(rdram, ctx));
}

// menuControllerInput 0x800B7640: draws the slider's high-end label.
void rs64_th_menuControllerInput_800B7640(uint8_t* rdram, recomp_context* ctx) {

    ctx->r7 = (gpr)(int32_t)((int32_t)ctx->r7 + slider_label_shift(rdram, ctx));
}

// menuControllerInput 0x800B51B8
void rs64_th_menuControllerInput_800B51B8(uint8_t* rdram, recomp_context* ctx) {

    rs64_menu_confirm_sound(rdram, ctx);
    rs64_menu_confirm_main(rdram);
}

// bsearchFactor5TableByU16Key entry
int rs64_th_bsearchFactor5TableByU16Key_entry(uint8_t* rdram, recomp_context* ctx)
{

    return rs64_menu_sfx_swap(ctx);
}

// menuControllerInput 0x800B4CA8
int rs64_th_menuControllerInput_800B4CA8(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_MENU_INPUT, rdram, ctx)) return 1;

    return 0;
}

// menuControllerInput 0x800B5428
int rs64_th_menuControllerInput_800B5428(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_MENU_PILOT_CHOSEN, rdram, ctx)) return 1;

    return 0;
}

// updateMenuPerFrame entry
int rs64_th_updateMenuPerFrame_entry(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_MENU_FRAME, rdram, ctx)) return 1;

    return 0;
}

// initAllPlayersHudTracking entry
void rs64_th_initAllPlayersHudTracking_entry(uint8_t* rdram, recomp_context* ctx) {

    rs64_controls_force_luke(rdram);
}

// controllerSettingsScreen entry
int rs64_th_controllerSettingsScreen_entry(uint8_t* rdram, recomp_context* ctx) {

    return rs64_controls_menu_active();
}

// loadControllerModel 0x800C4BD8
void rs64_th_loadControllerModel_800C4BD8(uint8_t* rdram, recomp_context* ctx) {

    rs64_controls_load_models(rdram, ctx);
}

// initOrUpdatePauseScreenDim 0x800C2660
void rs64_th_initOrUpdatePauseScreenDim_800C2660(uint8_t* rdram, recomp_context* ctx) {

    rs64_pause_slider_load(rdram, ctx);
}

// initOrUpdatePauseScreenDim 0x800C27CC
void rs64_th_initOrUpdatePauseScreenDim_800C27CC(uint8_t* rdram, recomp_context* ctx) {

    rs64_pause_slider_store(rdram, ctx);
}

// runPauseMenuStateMachine 0x800C3418
void rs64_th_runPauseMenuStateMachine_800C3418(uint8_t* rdram, recomp_context* ctx) {

    rs64_pause_slider_bar(rdram, ctx);
}

// runPauseMenuStateMachine 0x800C3314
void rs64_th_runPauseMenuStateMachine_800C3314(uint8_t* rdram, recomp_context* ctx) {

    rs64_pause_slider_ends(rdram, ctx);
}

// easeT16AnimationKeyframe entry
int rs64_th_easeT16AnimationKeyframe_entry(uint8_t* rdram, recomp_context* ctx) {

    return rs64_controls_menu_active();
}

// menuControllerInput 0x800B5920
void rs64_th_menuControllerInput_800B5920(uint8_t* rdram, recomp_context* ctx) {

    rs64_slider_redirect(rdram, (unsigned)ctx->r29);
}

// menuControllerInput 0x800B4E64
void rs64_th_menuControllerInput_800B4E64(uint8_t* rdram, recomp_context* ctx)
{

    rs64_menu_input_state(rdram, ctx);
}

// menuControllerInput 0x800B6700
void rs64_th_menuControllerInput_800B6700(uint8_t* rdram, recomp_context* ctx)
{

    rs64_wheel_typed(rdram, ctx);
}

// menuControllerInput 0x800B6DE8
void rs64_th_menuControllerInput_800B6DE8(uint8_t* rdram, recomp_context* ctx)
{

    rs64_wheel_typed(rdram, ctx);
}

// menuControllerInput 0x800B6DA4
void rs64_th_menuControllerInput_800B6DA4(uint8_t* rdram, recomp_context* ctx)
{

    rs64_passcode_row(rdram, ctx);
}

// menuControllerInput 0x800B5830
void rs64_th_menuControllerInput_800B5830(uint8_t* rdram, recomp_context* ctx)
{

    (void)rdram;

    rs64_set_language((int)ctx->r2);
}

// loadTxtFile entry
void rs64_th_loadTxtFile_entry(uint8_t* rdram, recomp_context* ctx)
{

    const int lang = rs64_language();

    if (lang >= 0) MEM_B(0x6, (gpr)(int32_t)0x80130B40u) = (int8_t)lang;

    rs64_menu_text_language((int)MEM_BU(0x6, (gpr)(int32_t)0x80130B40u));

    rs64_lang_txt_file(rdram, (uint32_t)ctx->r4, (int)ctx->r5);
}

// buildAndRegisterDefaultMaterial 0x80017D04
void rs64_th_buildAndRegisterDefaultMaterial_80017D04(uint8_t* rdram, recomp_context* ctx)
{

    (void)ctx;

    rs64_lang_texture_pool(rdram);
}

// initTextureLUTAndAllocBuffers 0x8001E304
void rs64_th_initTextureLUTAndAllocBuffers_8001E304(uint8_t* rdram, recomp_context* ctx)
{

    (void)ctx;

    rs64_lang_record_tables(rdram);
}

// initTextureMaterialPools entry
void rs64_th_initTextureMaterialPools_entry(uint8_t* rdram, recomp_context* ctx)
{

    (void)ctx;

    rs64_lang_record_tables_release(rdram);
}

// initTextureMaterialPools 0x80022920
void rs64_th_initTextureMaterialPools_80022920(uint8_t* rdram, recomp_context* ctx)
{

    (void)ctx;

    rs64_lang_record_tables(rdram);
}

// destroyTextureLUTs entry
void rs64_th_destroyTextureLUTs_entry(uint8_t* rdram, recomp_context* ctx)
{

    (void)ctx;

    rs64_lang_record_tables_release(rdram);
}

// buildAndRegisterDefaultMaterial 0x80017D94
void rs64_th_buildAndRegisterDefaultMaterial_80017D94(uint8_t* rdram, recomp_context* ctx)
{

    (void)ctx;

    rs64_lang_slot_tables(rdram);
}

// freeRenderSubsystemResources entry
void rs64_th_freeRenderSubsystemResources_entry(uint8_t* rdram, recomp_context* ctx)
{

    (void)ctx;

    rs64_lang_slot_tables_release(rdram);
}

// loadCharTextureSet entry
void rs64_th_loadCharTextureSet_entry(uint8_t* rdram, recomp_context* ctx)
{

    rs64_lang_font_load(rdram, (unsigned)ctx->r5);
}

// loadCharTextureSet 0x80061A54
void rs64_th_loadCharTextureSet_80061A54(uint8_t* rdram, recomp_context* ctx)
{

    rs64_lang_font_loaded(rdram, (unsigned)ctx->r16, (unsigned)ctx->r20);
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
    unsigned s = rs64_gof_override(rdram, (unsigned)ctx->r4);

    if (!s) s = rs64_lang_text(rdram, (uint32_t)ctx->r4, (uint32_t)(ctx->r3 + ctx->r2));

    if (s)
    {
        ctx->r3 = 0;
        ctx->r2 = (gpr)(int32_t)s;
    }
}

// initOrUpdatePauseScreenDim entry
void rs64_th_initOrUpdatePauseScreenDim_entry(uint8_t* rdram, recomp_context* ctx)
{

    rs64_pause_install(rdram, ctx);
    rs64_pause_hover_sound(rdram, ctx);
}

// runPauseMenuStateMachine 0x800C3550
void rs64_th_runPauseMenuStateMachine_800C3550(uint8_t* rdram, recomp_context* ctx)
{

    unsigned v = rs64_pause_label_for(rdram, MEM_HU(0x2, ctx->r20), (unsigned)ctx->r20 - 2);

    if (v) ctx->r2 = (gpr)(int32_t)v;
}

// initOrUpdatePauseScreenDim 0x800C2458
void rs64_th_initOrUpdatePauseScreenDim_800C2458(uint8_t* rdram, recomp_context* ctx) {

    rs64_pause_confirm(rdram, MEM_HU(0x4, ctx->r18));
}

// initOrUpdatePauseScreenDim 0x800C214C: $s2 = the array for HUD+0xD6A ($s1 = HUD).
void rs64_th_initOrUpdatePauseScreenDim_800C214C(uint8_t* rdram, recomp_context* ctx) {
    ctx->r18 = (gpr)(int32_t)rs64_pause_array_for(rdram, MEM_BU(0xD6A, ctx->r17), (unsigned)ctx->r18);
}

// initOrUpdatePauseScreenDim 0x800C24A8: $s0 = the array a confirmed link ($s2) opens.
void rs64_th_initOrUpdatePauseScreenDim_800C24A8(uint8_t* rdram, recomp_context* ctx) {
    ctx->r16 = (gpr)(int32_t)rs64_pause_array_for(rdram, MEM_HU(0x4, ctx->r18), (unsigned)ctx->r16);
}

// runPauseMenuStateMachine 0x800C2A74 / 0x800C3794: $s7 = the array for HUD+0xD6A ($fp = HUD).
void rs64_th_runPauseMenuStateMachine_800C2A74(uint8_t* rdram, recomp_context* ctx) {
    ctx->r23 = (gpr)(int32_t)rs64_pause_array_for(rdram, MEM_BU(0xD6A, ctx->r30), (unsigned)ctx->r23);
}

void rs64_th_runPauseMenuStateMachine_800C3794(uint8_t* rdram, recomp_context* ctx) {
    ctx->r23 = (gpr)(int32_t)rs64_pause_array_for(rdram, MEM_BU(0xD6A, ctx->r30), (unsigned)ctx->r23);
}

// initOrUpdatePauseScreenDim 0x800C23EC / 0x800C240C: B in a submenu, before and after the game resets HUD+0xD6A to the root.
void rs64_th_initOrUpdatePauseScreenDim_800C23EC(uint8_t* rdram, recomp_context* ctx) {

    rs64_pause_back_begin(rdram, (unsigned)ctx->r17);
}

void rs64_th_initOrUpdatePauseScreenDim_800C240C(uint8_t* rdram, recomp_context* ctx) {

    rs64_pause_back_end(rdram, (unsigned)ctx->r17);
}

// traverseSceneGraphRecursive 0x800155E4
void rs64_th_traverseSceneGraphRecursive_800155E4(uint8_t* rdram, recomp_context* ctx) {

    rs64_f5_map_node((unsigned)ctx->r4, (unsigned)ctx->r18);
}

// renderLitMeshFaceGroup 0x800137D4
void rs64_th_renderLitMeshFaceGroup_800137D4(uint8_t* rdram, recomp_context* ctx) {

    rs64_f5_map_sprite((unsigned)ctx->r5, (unsigned)ctx->r18);
}

// renderLitMeshFaceGroup 0x80014620
void rs64_th_renderLitMeshFaceGroup_80014620(uint8_t* rdram, recomp_context* ctx) {

    rs64_f5_map_sprite((unsigned)ctx->r17, (unsigned)ctx->r18);
}

// renderLitMeshFaceGroup 0x800138A4
void rs64_th_renderLitMeshFaceGroup_800138A4(uint8_t* rdram, recomp_context* ctx) {

    rs64_f5_map_sprite((unsigned)ctx->r5, (unsigned)ctx->r18);
}

// renderLitMeshFaceGroup 0x800146F0
void rs64_th_renderLitMeshFaceGroup_800146F0(uint8_t* rdram, recomp_context* ctx) {

    rs64_f5_map_sprite((unsigned)ctx->r17, (unsigned)ctx->r18);
}

// buildOrientedFaceGeometry 0x80012F14
void rs64_th_buildOrientedFaceGeometry_80012F14(uint8_t* rdram, recomp_context* ctx) {

    rs64_f5_map_sprite((unsigned)ctx->r18, (unsigned)ctx->r19);
}

// initMission 0x800FA28C
int rs64_th_initMission_800FA28C(uint8_t* rdram, recomp_context* ctx) {

    rs64_camera_shots_mission_start(rdram);
    rs64_rules_mission_start(rdram);

    if (rs64_hook(RS64_HOOK_MISSION_INIT, rdram, ctx)) return 1;

    return 0;
}

// runInMissionFrame 0x800FA7D0
int rs64_th_runInMissionFrame_800FA7D0(uint8_t* rdram, recomp_context* ctx) {

    rs64_camera_shots_frame(rdram, ctx);
    rs64_rules_frame(rdram, ctx);

    if (rs64_hook(RS64_HOOK_MISSION_FRAME_DT, rdram, ctx)) return 1;

    return 0;
}

// applyDatObjectiveTriggerEffect 0x80065A0C
void rs64_th_applyDatObjectiveTriggerEffect_80065A0C(uint8_t* rdram, recomp_context* ctx)
{

    rs64_camera_shots_trigger(rdram, (uint32_t)ctx->r4, (uint32_t)ctx->r5);
    rs64_rules_area(rdram, (uint32_t)ctx->r4, (uint32_t)ctx->r5);
}

// runInMissionFrame 0x800FA7D8
int rs64_th_runInMissionFrame_800FA7D8(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_MISSION_FRAME_PADS, rdram, ctx)) return 1;

    return 0;
}

// tickSongFadeTimer 0x800EE82C
int rs64_th_tickSongFadeTimer_800EE82C(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_SONG_ACTIVE, rdram, ctx)) return 1;

    return 0;
}

// requestSpeechResponseMode2 0x800FBB08
int rs64_th_requestSpeechResponseMode2_800FBB08(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_SPEECH_RESPONSE_2, rdram, ctx)) return 1;

    return 0;
}

// requestSpeechResponseMode1 0x800FBB64
int rs64_th_requestSpeechResponseMode1_800FBB64(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_SPEECH_RESPONSE_1, rdram, ctx)) return 1;

    return 0;
}

// runInMissionFrame 0x800FA94C
int rs64_th_runInMissionFrame_800FA94C(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_MISSION_FRAME_NPCS, rdram, ctx)) return 1;

    return 0;
}

// endMissionCleanup 0x800FB9E4
int rs64_th_endMissionCleanup_800FB9E4(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_MISSION_END, rdram, ctx)) return 1;

    return 0;
}

// requestMissionTransitionMode 0x800FB190
int rs64_th_requestMissionTransitionMode_800FB190(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_TRANSITION_REQUEST, rdram, ctx)) return 1;

    return 0;
}

// runInMissionFrame 0x800FA8E4
int rs64_th_runInMissionFrame_800FA8E4(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_FREEZE_CHECK_A, rdram, ctx)) return 1;

    return 0;
}

// runInMissionFrame 0x800FACA8
int rs64_th_runInMissionFrame_800FACA8(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_FREEZE_CHECK_B, rdram, ctx)) return 1;

    return 0;
}

// cutsceneActorNpcHandler 0x80044970
int rs64_th_cutsceneActorNpcHandler_80044970(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_CUTSCENE_FREEZE_CHECK, rdram, ctx)) return 1;

    return 0;
}

// datItemSetObjectiveBooleanCount 0x80065914
int rs64_th_datItemSetObjectiveBooleanCount_80065914(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_OBJECTIVE_COUNT, rdram, ctx)) return 1;

    return 0;
}

// setHudEnableBit4 0x800C7738
int rs64_th_setHudEnableBit4_800C7738(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_RESULT_FAIL, rdram, ctx)) return 1;

    return 0;
}

// setHudEnableBit8 0x800C776C
int rs64_th_setHudEnableBit8_800C776C(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_RESULT_SUCCESS, rdram, ctx)) return 1;

    return 0;
}

// npcWingmanUpdate 0x800D9A00
int rs64_th_npcWingmanUpdate_800D9A00(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_WINGMAN_TICK, rdram, ctx)) return 1;

    return 0;
}

// isNpcWithinActiveReferenceRange 0x80047D9C
int rs64_th_isNpcWithinActiveReferenceRange_80047D9C(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_NPC_ACTIVATION, rdram, ctx)) return 1;

    return 0;
}

// lookupActivePlayerCraftGridCell 0x80047E44
int rs64_th_lookupActivePlayerCraftGridCell_80047E44(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_GRID_STREAM, rdram, ctx)) return 1;

    return 0;
}

// drawRadarIcon 0x800C5E84
int rs64_th_drawRadarIcon_800C5E84(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_MISSION_SELECT_INIT, rdram, ctx)) return 1;

    rs64_mod_levels_select_open(rdram, ctx);

    return 0;
}

// drawRadarIcon 0x800C5ED8
int rs64_th_drawRadarIcon_800C5ED8(uint8_t* rdram, recomp_context* ctx)
{

    if (rs64_hook(RS64_HOOK_MISSION_CONFIRMED, rdram, ctx)) return 1;

    rs64_mod_levels_select_confirm(rdram, ctx);

    return 0;
}

// drawRadarIcon 0x800C5EE0
void rs64_th_drawRadarIcon_800C5EE0(uint8_t* rdram, recomp_context* ctx) {

    rs64_mod_levels_select_close();
}

// initCraftSelectScreen 0x800AF18C / handleCraftSelectInput 0x800AFDDC: v0 = the entry's title.
void rs64_th_initCraftSelectScreen_800AF18C(uint8_t* rdram, recomp_context* ctx) {

    rs64_mod_levels_text(rdram, ctx, 0);
}
void rs64_th_handleCraftSelectInput_800AFDDC(uint8_t* rdram, recomp_context* ctx) {

    rs64_mod_levels_text(rdram, ctx, 0);
}

// handleCraftSelectInput 0x800AFB58: v0 = the entry's description.
void rs64_th_handleCraftSelectInput_800AFB58(uint8_t* rdram, recomp_context* ctx) {

    rs64_mod_levels_text(rdram, ctx, 1);
}

// initCraftSelectScreen 0x800AF270 / handleCraftSelectInput 0x800AF878: a2 = the preview image's level number.
void rs64_th_initCraftSelectScreen_800AF270(uint8_t* rdram, recomp_context* ctx) {

    uint32_t v = (uint32_t)ctx->r6;
    rs64_mod_levels_entry_level(&v);
    ctx->r6 = v;
}
void rs64_th_handleCraftSelectInput_800AF878(uint8_t* rdram, recomp_context* ctx) {

    uint32_t v = (uint32_t)ctx->r6;
    rs64_mod_levels_entry_level(&v);
    ctx->r6 = v;
}

// getAvailablePlayerCraftFlags{Consider,Ignore}Unlocks entry: a0 = level; a mod level's approved craft are returned in v0.
int rs64_th_getAvailablePlayerCraftFlagsConsiderUnlocks_entry(uint8_t* rdram, recomp_context* ctx) {

    uint32_t v = (uint32_t)ctx->r4;
    int m = rs64_mod_levels_craft(&v);
    ctx->r4 = v;

    if (m < 0) return 0;

    ctx->r2 = m;

    return 1;
}
int rs64_th_getAvailablePlayerCraftFlagsIgnoreUnlocks_entry(uint8_t* rdram, recomp_context* ctx) {

    uint32_t v = (uint32_t)ctx->r4;
    int m = rs64_mod_levels_craft(&v);
    ctx->r4 = v;

    if (m < 0) return 0;

    ctx->r2 = m;

    return 1;
}

// Mod level debrief and progress (see rogue_squadron.toml).
void rs64_th_postLevelStatScreen_entry(uint8_t* rdram, recomp_context* ctx) {

    rs64_mod_levels_debrief_begin(rdram, (uint32_t)ctx->r5);
}
void rs64_th_postLevelStatScreen_800C092C(uint8_t* rdram, recomp_context* ctx) {

    rs64_mod_levels_medal_earned((int)(uint8_t)ctx->r2);
}
void rs64_th_postLevelStatScreen_800C09C4(uint8_t* rdram, recomp_context* ctx)
{

    if (rs64_mod_levels_debriefing()) ctx->r2 = MEM_BU((int32_t)0x80130B11, 0);
}
void rs64_th_postLevelStatScreen_800C0A78(uint8_t* rdram, recomp_context* ctx) {

    rs64_mod_levels_title(rdram, ctx);
}
void rs64_th_postLevelStatScreen_800C0A9C(uint8_t* rdram, recomp_context* ctx) {

    rs64_mod_levels_title(rdram, ctx);
}
int rs64_th_highScoreBubbleSort_entry(uint8_t* rdram, recomp_context* ctx) {

    return rs64_mod_levels_debriefing();
}
int rs64_th_unlockMissleUpgradeOnLevelCompletion_entry(uint8_t* rdram, recomp_context* ctx) {

    return rs64_mod_levels_debriefing();
}
int rs64_th_unlockLevelsAndCraftsBasedOnMedalsEarned_entry(uint8_t* rdram, recomp_context* ctx) {

    return rs64_mod_levels_debriefing();
}
int rs64_th_loadAccountDataIntoStruct_entry(uint8_t* rdram, recomp_context* ctx) {

    return rs64_mod_levels_debriefing();
}
int rs64_th_generateUnlockString_entry(uint8_t* rdram, recomp_context* ctx) {

    if (!rs64_mod_levels_debriefing()) return 0;

    ctx->r2 = 0;

    return 1;
}
void rs64_th_runMenuRandomBackdropScreen_800BF9E4(uint8_t* rdram, recomp_context* ctx) {

    rs64_mod_levels_debrief_end(rdram);
}

// npcWingmanUpdate 0x800D9A90: a wingman's model went into the render list
void rs64_th_npcWingmanUpdate_800D9A90(uint8_t* rdram, recomp_context* ctx) {

    rs64_hook(RS64_HOOK_WINGMAN_DRAW, rdram, ctx);
}

// mainGameLoop 0x8003E138: the front end is about to start
void rs64_th_mainGameLoop_8003E138(uint8_t* rdram, recomp_context* ctx) {

    rs64_hook(RS64_HOOK_FRONT_END, rdram, ctx);
}

// runMenuRandomBackdropScreen 0x800BFC4C: the debrief's continue check
void rs64_th_runMenuRandomBackdropScreen_800BFC4C(uint8_t* rdram, recomp_context* ctx) {

    rs64_hook(RS64_HOOK_DEBRIEF_PAD, rdram, ctx);
}
void rs64_th_mainGameLoop_8003E424(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_mod_levels_playing()) ctx->r5 = ctx->r16;
}
void rs64_th_mainGameLoop_8003E444(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_mod_levels_playing()) ctx->r3 = 0x10;
}
void rs64_th_applyPlayerSlotAnimChannelKeyframe_800B1FD4(uint8_t* rdram, recomp_context* ctx)
{

    int m = rs64_mod_levels_entry_medal(rdram, (uint32_t)ctx->r18 - 0x80130B10u);

    if (m >= 0) ctx->r3 = m;
}

// setupCraftAnimationFlagSet 0x800B9334: the mod level medals beside the profile's medal totals.
void rs64_th_setupCraftAnimationFlagSet_800B9334(uint8_t* rdram, recomp_context* ctx) {

    rs64_mod_levels_select_game(rdram, ctx);
}

// getLevelIntroCrawlId entry / buildModelViewerTextPage 0x800CA77C: a mod level's opening crawl.
int rs64_th_getLevelIntroCrawlId_entry(uint8_t* rdram, recomp_context* ctx) {

    int c = rs64_mod_levels_crawl_id((int)(ctx->r4 & 0xFF));

    if (c == -2) return 0;

    ctx->r2 = c < 0 ? 0xFF : c;

    return 1;
}
void rs64_th_buildModelViewerTextPage_800CA77C(uint8_t* rdram, recomp_context* ctx) {

    uint32_t s = rs64_mod_levels_crawl_text(rdram);

    if (s) ctx->r2 = (gpr)(int32_t)s;
}

// setModelViewerObjectTransform entry: a crawl file's keyed camera (v0 = the camera matrix).
int rs64_th_setModelViewerObjectTransform_entry(uint8_t* rdram, recomp_context* ctx) {

    return rs64_crawl_camera(rdram, ctx);
}

// runModelViewerLoop 0x800CA080: the pan's camera drop f26 follows a crawl file's keyed camera.
void rs64_th_runModelViewerLoop_800CA080(uint8_t* rdram, recomp_context* ctx) {

    rs64_crawl_drop(rdram, ctx);
}

// find_manifest_entry entry: a1 = the asset path looked up in manifest slot a0.
void rs64_th_find_manifest_entry_entry(uint8_t* rdram, recomp_context* ctx)
{

    ctx->r5 = (gpr)(int32_t)rs64_mod_levels_lookup(rdram, (uint32_t)ctx->r5);
}

// startSlotLoopAnimWithVoiceSync 0x800B1C00 / 0x800B1D18: v0 = the briefing voice id.
void rs64_th_startSlotLoopAnimWithVoiceSync_800B1C00(uint8_t* rdram, recomp_context* ctx) {

    rs64_mod_levels_voice(rdram, ctx);
}
void rs64_th_startSlotLoopAnimWithVoiceSync_800B1D18(uint8_t* rdram, recomp_context* ctx) {

    rs64_mod_levels_voice(rdram, ctx);
}

// allocAndConfigureNewVoice 0x8009B114 (s3 = speech index, s2 = rate) / 0x8009B1F4 (a1 = stream address): a mod level's briefing recording.
void rs64_th_allocAndConfigureNewVoice_8009B114(uint8_t* rdram, recomp_context* ctx) {

    rs64_mod_levels_brief_rate(ctx);
}
void rs64_th_allocAndConfigureNewVoice_8009B1F4(uint8_t* rdram, recomp_context* ctx) {

    rs64_mod_levels_brief_stream(rdram, ctx);
}

// initCraftSelectScreen 0x800AF0D0
int rs64_th_initCraftSelectScreen_800AF0D0(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_MISSION_SELECT_FONTS, rdram, ctx)) return 1;

    return 0;
}

// tickCraftSelectScreen 0x800AF3EC
int rs64_th_tickCraftSelectScreen_800AF3EC(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_MISSION_SELECT_TICK, rdram, ctx)) return 1;

    return 0;
}

// hangarInitialize 0x800AAC44
int rs64_th_hangarInitialize_800AAC44(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_CRAFT_SELECT_INIT, rdram, ctx)) return 1;

    return 0;
}

// runHangarSelectionFrame 0x800AADD8
int rs64_th_runHangarSelectionFrame_800AADD8(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_CRAFT_SELECT_TICK, rdram, ctx)) return 1;

    return 0;
}

// initMission 0x800FA5BC
int rs64_th_initMission_800FA5BC(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_CRAFT_ASSETS, rdram, ctx)) return 1;

    return 0;
}

// initVoiceSubtitleSystem 0x80055B08
int rs64_th_initVoiceSubtitleSystem_80055B08(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_HUD_FONTS, rdram, ctx)) return 1;

    return 0;
}

// runInMissionFrame 0x800FAC70
int rs64_th_runInMissionFrame_800FAC70(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_HUD_DRAW, rdram, ctx)) return 1;

    return 0;
}

// renderRadarMinimap 0x800C6290
int rs64_th_renderRadarMinimap_800C6290(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_RADAR, rdram, ctx)) return 1;

    return 0;
}

// npcPowerUpUpdate 0x800EBD20
void rs64_th_npcPowerUpUpdate_800EBD20(uint8_t* rdram, recomp_context* ctx) {

    rs64_hook(RS64_HOOK_POWERUP_TOUCH, rdram, ctx);
}

// npcPowerUpUpdate 0x800EBD34
void rs64_th_npcPowerUpUpdate_800EBD34(uint8_t* rdram, recomp_context* ctx) {

    rs64_hook(RS64_HOOK_POWERUP_COLLECT, rdram, ctx);
}

// applyDatObjectiveTriggerEffect 0x80065980
int rs64_th_applyDatObjectiveTriggerEffect_80065980(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_TRIGGER_EFFECT, rdram, ctx)) return 1;

    return 0;
}

// npcAtAtUpdate 0x800CED7C
void rs64_th_npcAtAtUpdate_800CED7C(uint8_t* rdram, recomp_context* ctx) {

    rs64_hook(RS64_HOOK_WALKER_TRIPPED, rdram, ctx);
}

// updatePlayerAtStBehavior entry
int rs64_th_updatePlayerAtStBehavior_entry(uint8_t* rdram, recomp_context* ctx) {

    if (rs64_hook(RS64_HOOK_ATST_TICK, rdram, ctx)) return 1;

    return 0;
}

// updatePlayerAtStBehavior 0x800D1AE0 ($s1 = the walker)
void rs64_th_updatePlayerAtStBehavior_800D1AE0(uint8_t* rdram, recomp_context* ctx) {

    union { float f; uint32_t u; } yaw, pitch;

    if (!rs64_atst_head(&yaw.f, &pitch.f)) return;

    MEM_W(0x68, ctx->r17) = yaw.u;
    MEM_W(0x48, ctx->r17) = yaw.u;
    MEM_W(0x6C, ctx->r17) = pitch.u;
    MEM_W(0x4C, ctx->r17) = pitch.u;
}

// mainGameLoop 0x8003E10C
void rs64_th_mainGameLoop_8003E10C(uint8_t* rdram, recomp_context* ctx) {

    uint32_t lv = (uint32_t)ctx->r4, kd = (uint32_t)ctx->r5;
    rs64_nav_boot_cutscene(rdram, &lv, &kd);
    ctx->r4 = (gpr)(int32_t)lv;
    ctx->r5 = (gpr)(int32_t)kd;
}

// mainGameLoop 0x8003E114
void rs64_th_mainGameLoop_8003E114(uint8_t* rdram, recomp_context* ctx) {

    rs64_nav_boot_cutscene_done();
}

// findManifestEntryByName 0x8000496C
void rs64_th_findManifestEntryByName_8000496C(uint8_t* rdram, recomp_context* ctx) {

    rs64_asset_overrides_patch(rdram, (uint32_t)ctx->r20);
}

// freeManifestSegmentAssets 0x80004A0C / 0x80004A80 (the rs_free calls)
void rs64_th_freeManifestSegmentAssets_80004A0C(uint8_t* rdram, recomp_context* ctx)
{

    ctx->r4 = (gpr)(int32_t)rs64_asset_manifest_to_free((uint32_t)ctx->r4);
}
void rs64_th_freeManifestSegmentAssets_80004A80(uint8_t* rdram, recomp_context* ctx)
{

    ctx->r4 = (gpr)(int32_t)rs64_asset_manifest_to_free((uint32_t)ctx->r4);
}

// Mod level objectives: initializeObjectiveTracking / checkComplexObjectives ($f12 = dt) / calculateFriendliesSaved entries skip the
// template's handlers; the pause list's line text (v0 at 0x800C2F1C, s4 -> its index) is the level's own.
int rs64_th_initializeObjectiveTracking_entry(uint8_t* rdram, recomp_context* ctx) {

    return rs64_mod_levels_objectives_init(rdram);
}
int rs64_th_checkComplexObjectives_entry(uint8_t* rdram, recomp_context* ctx)
{

    return rs64_mod_levels_objectives_tick(rdram, ctx);
}
int rs64_th_calculateFriendliesSaved_entry(uint8_t* rdram, recomp_context* ctx)
{

    return rs64_mod_levels_objectives_friendlies(rdram, ctx);
}
void rs64_th_runPauseMenuStateMachine_800C2F1C(uint8_t* rdram, recomp_context* ctx)
{

    rs64_mod_levels_objective_text(rdram, ctx);
}

// initOrUpdatePauseScreenDim 0x800C227C: the voice the pause screen reads the current objective line with (a0, less 0x2C0).
void rs64_th_initOrUpdatePauseScreenDim_800C227C(uint8_t* rdram, recomp_context* ctx)
{

    rs64_mod_levels_pause_voice(rdram, ctx);
}

// initMission entry: a mod level's starting song (per-level song table 0x8010B2EC).
void rs64_th_initMission_entry(uint8_t* rdram, recomp_context* ctx)
{

    rs64_mod_levels_music(rdram);
}

// updateDestructionDebrisNpc 0x8006D6F0 / npcPowerGeneratorUpdate 0x80078CC8: a unit's death effect (v1 = its DAT record).
void rs64_th_updateDestructionDebrisNpc_8006D6F0(uint8_t* rdram, recomp_context* ctx)
{

    rs64_rules_destroyed(rdram, ctx);
}
void rs64_th_npcPowerGeneratorUpdate_80078CC8(uint8_t* rdram, recomp_context* ctx)
{

    rs64_rules_destroyed(rdram, ctx);
}

// getVoiceText entry (a0 = text id) / 0x80055A10 (the text decrypted): a level-rule message replaces a blank line's text.
void rs64_th_getVoiceText_entry(uint8_t* rdram, recomp_context* ctx)
{

    rs64_rules_voice_text_id(ctx);
    rs64_lang_voice_id((uint32_t)ctx->r4);
}
void rs64_th_getVoiceText_80055A10(uint8_t* rdram, recomp_context* ctx)
{

    rs64_lang_voice_text(rdram);
    rs64_rules_voice_text(rdram);
}

// updateNpcEffectIntensity entry (a1 = the path event an NPC reached).
void rs64_th_updateNpcEffectIntensity_entry(uint8_t* rdram, recomp_context* ctx)
{

    rs64_rules_path(rdram, (uint32_t)ctx->r5);
}

// selectMeshLodAndSubmitForRender 0x80058104
void rs64_th_selectMeshLodAndSubmitForRender_80058104(uint8_t* rdram, recomp_context* ctx) {

    s_render_view_test_next = 1;
}
