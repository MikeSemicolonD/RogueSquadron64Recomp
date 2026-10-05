// Online co-op's registrations with the host API: called by the built-ins when multiplayer is built in, else by the multiplayer-native mod's rs64_mod_init.
#include "mp_host.h"
#include "ghost_internal.h"
#include "recomp.h"

namespace {
const rs64_host_api* g_api = nullptr;
using CtxFn = void (*)(uint8_t*, recomp_context*);
using CtxOnlyFn = void (*)(recomp_context*);

int forward(uint32_t, uint8_t* rdram, void* ctx, void* user) {
    ((CtxFn)user)(rdram, (recomp_context*)ctx);
    return RS64_HOOK_CONTINUE;
}

int forward_ctx(uint32_t, uint8_t*, void* ctx, void* user) {
    ((CtxOnlyFn)user)((recomp_context*)ctx);
    return RS64_HOOK_CONTINUE;
}

// The address editor takes the pad before the mod page's row navigation reads it; off the page it is closed.
int menu_pad(uint32_t, uint8_t* rdram, void*, void*) {
    if (!rs64::mp::flag("menu_page_shown")) {
        rs64_lobby_edit_off();
    }
    rs64_lobby_input(rdram);
    return RS64_HOOK_CONTINUE;
}

int menu_tick(uint32_t, uint8_t*, void*, void*) {
    rs64_lobby_tick();
    return RS64_HOOK_CONTINUE;
}

// The main menu ends a menu-started session (a later START is single player again).
int main_menu(uint32_t, uint8_t*, void*, void*) {
    if (rs64_lobby_menu_session()) {
        rs64_lobby_cancel();
    }
    return RS64_HOOK_CONTINUE;
}

int mission_init(uint32_t, uint8_t* rdram, void* c, void*) {
    rs64_imposter_reset(rdram);
    if (rs64_ghost_mode()) {
        rs64_ghost_mission_init(rdram, (recomp_context*)c);
    }
    return RS64_HOOK_CONTINUE;
}

// Recording-driven ghost runs keep the fixed 1/30 s step (the built-in already set it for an active lockstep session).
int frame_dt(uint32_t, uint8_t* rdram, void* c, void*) {
    if (rs64_ghost_fixed_dt() && !rs64::ghost::demo_active(rdram)) {
        ((recomp_context*)c)->f20.fl = 1.0f / 30.0f;
    }
    return RS64_HOOK_CONTINUE;
}

int frame_pads(uint32_t, uint8_t* rdram, void* c, void*) {
    if (rs64_ghost_mode()) {
        rs64_ghost_frame(rdram, (recomp_context*)c);
    }
    return RS64_HOOK_CONTINUE;
}

// user = the response mode's result (2 = failure responder, 1 = success); r2 already holds the built-in's speech-idle answer.
int speech_gate(uint32_t, uint8_t*, void* c, void* user) {
    recomp_context* ctx = (recomp_context*)c;
    ctx->r2 = (gpr)(int32_t)rs64_ghost_gate_response((uint32_t)(uintptr_t)user, (uint32_t)ctx->r2);
    return RS64_HOOK_CONTINUE;
}

int transition_request(uint32_t, uint8_t* rdram, void* c, void*) {
    recomp_context* ctx = (recomp_context*)c;
    if (rs64_ghost_block_transition(rdram, (uint32_t)ctx->r4)) {
        ctx->r2 = 0;
        return RS64_HOOK_RETURN;
    }
    return RS64_HOOK_CONTINUE;
}

int unfreeze(uint32_t, uint8_t* rdram, void* c, void*) {
    if (rs64_ghost_unfreeze(rdram)) {
        ((recomp_context*)c)->r2 = 0;
    }
    return RS64_HOOK_CONTINUE;
}

int objective_count(uint32_t, uint8_t* rdram, void* c, void*) {
    return rs64_ghost_objective_event(rdram, (recomp_context*)c) ? RS64_HOOK_RETURN : RS64_HOOK_CONTINUE;
}

int trigger_effect(uint32_t, uint8_t* rdram, void* c, void*) {
    return rs64_ghost_trigger_effect(rdram, (recomp_context*)c) ? RS64_HOOK_RETURN : RS64_HOOK_CONTINUE;
}

int block_result(uint32_t, uint8_t*, void*, void*) {
    return rs64_ghost_block_result() ? RS64_HOOK_RETURN : RS64_HOOK_CONTINUE;
}

int npc_activation(uint32_t, uint8_t* rdram, void* c, void*) {
    recomp_context* ctx = (recomp_context*)c;
    rs64_imposter_activation(rdram, ctx, (uint32_t)ctx->r16);
    return RS64_HOOK_CONTINUE;
}
}

namespace rs64::mp {
const rs64_host_api* api() { return g_api; }
}

extern "C" void rs64_mp_register(const rs64_host_api* api) {
    g_api = api;
    using rs64::mp::add_hook;
    add_hook(RS64_HOOK_MENU_FRAME, menu_tick);
    add_hook(RS64_HOOK_MISSION_INIT, mission_init);
    add_hook(RS64_HOOK_MISSION_FRAME_DT, frame_dt);
    add_hook(RS64_HOOK_MISSION_FRAME_PADS, frame_pads);
    add_hook(RS64_HOOK_SPEECH_RESPONSE_2, speech_gate, (void*)(uintptr_t)2);
    add_hook(RS64_HOOK_SPEECH_RESPONSE_1, speech_gate, (void*)(uintptr_t)1);
    add_hook(RS64_HOOK_MISSION_FRAME_NPCS, forward, (void*)rs64_imposter_frame);
    add_hook(RS64_HOOK_MISSION_END, forward, (void*)rs64_imposter_release);
    add_hook(RS64_HOOK_TRANSITION_REQUEST, transition_request);
    add_hook(RS64_HOOK_FREEZE_CHECK_A, unfreeze);
    add_hook(RS64_HOOK_FREEZE_CHECK_B, unfreeze);
    add_hook(RS64_HOOK_CUTSCENE_FREEZE_CHECK, unfreeze);
    add_hook(RS64_HOOK_OBJECTIVE_COUNT, objective_count);
    add_hook(RS64_HOOK_TRIGGER_EFFECT, trigger_effect);
    add_hook(RS64_HOOK_RESULT_FAIL, block_result);
    add_hook(RS64_HOOK_RESULT_SUCCESS, block_result);
    add_hook(RS64_HOOK_WINGMAN_TICK, forward, (void*)rs64_imposter_tick);
    add_hook(RS64_HOOK_NPC_ACTIVATION, npc_activation);
    add_hook(RS64_HOOK_GRID_STREAM, forward, (void*)rs64_imposter_stream);
    add_hook(RS64_HOOK_MISSION_SELECT_INIT, forward, (void*)rs64_lobby_mission_select_init);
    add_hook(RS64_HOOK_MISSION_CONFIRMED, forward, (void*)rs64_lobby_mission_confirmed);
    add_hook(RS64_HOOK_MISSION_SELECT_FONTS, forward_ctx, (void*)rs64_lobby_mission_select_fonts);
    add_hook(RS64_HOOK_MISSION_SELECT_TICK, forward, (void*)rs64_lobby_mission_select_tick);
    add_hook(RS64_HOOK_CRAFT_SELECT_INIT, forward_ctx, (void*)rs64_lobby_craft_select_init);
    add_hook(RS64_HOOK_CRAFT_SELECT_TICK, forward, (void*)rs64_lobby_craft_select_tick);
    add_hook(RS64_HOOK_CRAFT_ASSETS, forward, (void*)rs64_ghost_craft_assets);
    add_hook(RS64_HOOK_HUD_FONTS, forward_ctx, (void*)rs64_ghost_hud_fonts);
    add_hook(RS64_HOOK_HUD_DRAW, forward, (void*)rs64_ghost_hud_draw);
    add_hook(RS64_HOOK_RADAR, forward, (void*)rs64_ghost_radar);
    add_hook(RS64_HOOK_POWERUP_TOUCH, forward, (void*)rs64_ghost_powerup_touch);
    add_hook(RS64_HOOK_POWERUP_COLLECT, forward, (void*)rs64_ghost_powerup_collect);
    add_hook(RS64_HOOK_WALKER_TRIPPED, forward, (void*)rs64_ghost_walker_tripped);
    add_hook(RS64_HOOK_MENU_PAD, menu_pad);
    add_hook(RS64_HOOK_MAIN_MENU, main_menu);
    rs64::mp::add_quit_handler([](void*) { rs64_ghost_quit(); });
    rs64_ghost_register_host();
}
