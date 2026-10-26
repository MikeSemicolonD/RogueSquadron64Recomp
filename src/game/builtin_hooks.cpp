#include "host_api/host_api.h"
#include "recomp.h"

extern "C" {
void rs64_menu_pass(uint8_t*, recomp_context*);
void rs64_menu_pilot_chosen(uint8_t*, recomp_context*);
void rs64_menu_frame(uint8_t*, recomp_context*);
void rs64_ls_on_mission_init(uint8_t*, recomp_context*);
float rs64_ls_frame_dt(uint8_t*, float);
void rs64_ls_frame_pads(uint8_t*, recomp_context*);
void rs64_ls_frame_hash(uint8_t*, recomp_context*);
uint32_t rs64_ls_song_active(uint8_t*, uint32_t);
void rs64_pause_state_point();
}

namespace {
using CtxFn = void (*)(uint8_t*, recomp_context*);

// Handlers that just forward (rdram, ctx) take the target as the hook's user pointer.
int forward(uint32_t, uint8_t* rdram, void* ctx, void* user) {

    ((CtxFn)user)(rdram, (recomp_context*)ctx);

    return RS64_HOOK_CONTINUE;
}

int frame_dt(uint32_t, uint8_t* rdram, void* c, void*) {

    recomp_context* ctx = (recomp_context*)c;
    ctx->f20.fl = rs64_ls_frame_dt(rdram, ctx->f20.fl);

    return RS64_HOOK_CONTINUE;
}

// atst_active: 1 from the player AT-ST's first tick until the mission ends or the next one loads.
int atst_flag(uint32_t hook, uint8_t*, void*, void*) {

    rs64::host::set_flag("atst_active", hook == RS64_HOOK_ATST_TICK);

    return RS64_HOOK_CONTINUE;
}

// ROGUESQ_PAUSE_AT's game-state clock, ticked once per menu or mission frame on the game thread.
int pause_state(uint32_t, uint8_t*, void*, void*) {

    rs64_pause_state_point();

    return RS64_HOOK_CONTINUE;
}

int song_active(uint32_t, uint8_t* rdram, void* c, void*)
{

    recomp_context* ctx = (recomp_context*)c;
    ctx->r2 = (gpr)(int32_t)rs64_ls_song_active(rdram, (uint32_t)ctx->r2);

    return RS64_HOOK_CONTINUE;
}
}

extern "C" void rs64_register_builtin_hooks()
{

    using rs64::host::add_hook;
    add_hook(RS64_HOOK_MENU_INPUT, forward, (void*)rs64_menu_pass);
    add_hook(RS64_HOOK_MENU_PILOT_CHOSEN, forward, (void*)rs64_menu_pilot_chosen);
    add_hook(RS64_HOOK_MENU_FRAME, forward, (void*)rs64_menu_frame);
    add_hook(RS64_HOOK_MISSION_INIT, forward, (void*)rs64_ls_on_mission_init);
    add_hook(RS64_HOOK_MISSION_FRAME_DT, frame_dt, nullptr);
    add_hook(RS64_HOOK_MISSION_FRAME_PADS, forward, (void*)rs64_ls_frame_pads);
    add_hook(RS64_HOOK_MENU_FRAME, pause_state, nullptr);
    add_hook(RS64_HOOK_MISSION_FRAME_PADS, pause_state, nullptr);
    add_hook(RS64_HOOK_SONG_ACTIVE, song_active, nullptr);
    add_hook(RS64_HOOK_ATST_TICK, atst_flag, nullptr);
    add_hook(RS64_HOOK_MISSION_INIT, atst_flag, nullptr);
    add_hook(RS64_HOOK_MISSION_END, atst_flag, nullptr);
}

// After the mods, so the frame hash sees what their frame-pads handlers wrote.
extern "C" void rs64_register_builtin_late_hooks()
{
    rs64::host::add_hook(RS64_HOOK_MISSION_FRAME_PADS, forward, (void*)rs64_ls_frame_hash);
}
