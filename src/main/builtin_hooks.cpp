#include "host_api.h"
#include "recomp.h"

#ifndef RS64_MULTIPLAYER
#error RS64_MULTIPLAYER must be defined (0 or 1)
#endif

extern "C" {
void rs64_menu_pass(uint8_t*, recomp_context*);
void rs64_menu_pilot_chosen(uint8_t*, recomp_context*);
void rs64_menu_frame(uint8_t*, recomp_context*);
void rs64_ls_on_mission_init(uint8_t*, recomp_context*);
float rs64_ls_frame_dt(uint8_t*, float);
void rs64_ls_frame_pads(uint8_t*, recomp_context*);
uint32_t rs64_ls_song_active(uint8_t*, uint32_t);
#if RS64_MULTIPLAYER
void rs64_mp_register(const rs64_host_api* api);
#endif
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

int song_active(uint32_t, uint8_t* rdram, void* c, void*) {
    recomp_context* ctx = (recomp_context*)c;
    ctx->r2 = (gpr)(int32_t)rs64_ls_song_active(rdram, (uint32_t)ctx->r2);
    return RS64_HOOK_CONTINUE;
}
}

extern "C" void rs64_register_builtin_hooks() {
    using rs64::host::add_hook;
    add_hook(RS64_HOOK_MENU_INPUT, forward, (void*)rs64_menu_pass);
    add_hook(RS64_HOOK_MENU_PILOT_CHOSEN, forward, (void*)rs64_menu_pilot_chosen);
    add_hook(RS64_HOOK_MENU_FRAME, forward, (void*)rs64_menu_frame);
    add_hook(RS64_HOOK_MISSION_INIT, forward, (void*)rs64_ls_on_mission_init);
    add_hook(RS64_HOOK_MISSION_FRAME_DT, frame_dt, nullptr);
    add_hook(RS64_HOOK_MISSION_FRAME_PADS, forward, (void*)rs64_ls_frame_pads);
    add_hook(RS64_HOOK_SONG_ACTIVE, song_active, nullptr);
#if RS64_MULTIPLAYER
    // Multiplayer is built in: a multiplayer-native mod that sees this flag stays off.
    rs64::host::set_flag("mp_builtin", 1);
    rs64_mp_register(rs64::host::api());
#endif
}
