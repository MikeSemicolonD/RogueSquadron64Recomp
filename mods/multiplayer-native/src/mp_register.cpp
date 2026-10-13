// Online co-op's registrations with the host API, called by the multiplayer-native mod's rs64_mod_init.
#include "mp_host.h"
#include "ghost_internal.h"
#include "mp_core.h"
#include "debug_logs.h"
#include "rdram_words.h"
#include "recomp.h"
#include <cstdio>
#include <map>
#include <vector>

namespace {
using rs64::mips::rf;
using rs64::mips::rw;
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

// ROGUESQ_LS_PAD2_REPLAY (local co-op test harness): port 1 flies a recorded port-0 run from mission frame 0.
struct Pad2 {
    bool on = false;
    uint32_t frame = 0;
    std::vector<rs64::ls::FrameInput> frames;
} g_pad2;

void pad2_mission_init(uint8_t* rdram) {
    using namespace rs64::ls;
    g_pad2.on = false;
    const char* path = recomp::dbg::env_str("ROGUESQ_LS_PAD2_REPLAY");
    if (!path || rs64_ghost_mode() || !rs64::mp::flag("coop_imposter") || rs64::ls::demo_active(rdram)) {
        return;
    }
    SessionHeader h;
    std::map<uint32_t, std::vector<uint8_t>> song;
    g_pad2.frames.clear();
    if (!load_replay(path, &h, &g_pad2.frames, &song)) {
        return;
    }
    const SessionHeader running = read_session(rdram, 0);
    if (running.level != h.level || running.craft != h.craft) {
        fprintf(stderr, "[coop] player 2 disabled: %s is level %u craft %u, running level %u craft %u\n", path, h.level, h.craft, running.level, running.craft);
        return;
    }
    g_pad2.on = true;
    g_pad2.frame = 0;
    fprintf(stderr, "[coop] player 2 replays %zu frames from %s\n", g_pad2.frames.size(), path);
}

void pad2_frame(uint8_t* rdram) {
    if (!g_pad2.on || rs64::ls::demo_active(rdram)) {
        return;
    }
    uint8_t pad[6];
    rs64::ls::pad2_bytes(g_pad2.frame < g_pad2.frames.size() ? &g_pad2.frames[g_pad2.frame] : nullptr, pad);
    for (uint32_t i = 0; i < 6; ++i) {
        rdram[(0x80130B8Eu + i - 0x80000000u) ^ 3] = pad[i];
    }
    if (g_pad2.frame == 0 || g_pad2.frame == 30) {
        const uint32_t blk = rw(rdram, rs64::ls::kImposterRec + 0xE0);
        fprintf(stderr, "[coop] frame %u: p1 pos=%.1f,%.1f,%.1f  imposter pos=%.1f,%.1f,%.1f\n", g_pad2.frame,
            rf(rdram, 0x80137DC0u), rf(rdram, 0x80137DC4u), rf(rdram, 0x80137DC8u),
            blk ? rf(rdram, blk) : 0.0f, blk ? rf(rdram, blk + 4) : 0.0f, blk ? rf(rdram, blk + 8) : 0.0f);
    }
    g_pad2.frame++;
}

uint64_t imposter_hash(const uint8_t* rdram, void*) { return rs64::ls::hash_imposter(rdram); }

int mission_init(uint32_t, uint8_t* rdram, void* c, void*) {
    pad2_mission_init(rdram);
    rs64_imposter_reset(rdram);
    if (rs64_ghost_mode()) {
        rs64_ghost_mission_init(rdram, (recomp_context*)c);
    }
    return RS64_HOOK_CONTINUE;
}

// Recording-driven ghost runs keep the fixed 1/30 s step (the built-in already set it for an active lockstep session).
int frame_dt(uint32_t, uint8_t* rdram, void* c, void*) {
    if (rs64_ghost_fixed_dt() && !rs64::ls::demo_active(rdram)) {
        ((recomp_context*)c)->f20.fl = 1.0f / 30.0f;
    }
    return RS64_HOOK_CONTINUE;
}

int frame_pads(uint32_t, uint8_t* rdram, void* c, void*) {
    pad2_frame(rdram);
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
    api->add_hash_part("imposter", "coop_imposter", imposter_hash, nullptr);
    api->add_valid_ram(rs64::ls::kImposterRec, 0x100);
    // The mod's instruction_patches.json moved these; the host's lockstep code and this library's copy both follow them.
    for (const rs64::ls::MovedRange& r : rs64::ls::kMovedRanges) {
        api->add_moved_range(r.lo, r.hi, r.delta);
    }
    rs64::ls::set_moved_ranges({std::begin(rs64::ls::kMovedRanges), std::end(rs64::ls::kMovedRanges)});
    rs64_ghost_register_host();
}
