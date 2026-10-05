#ifndef RS64_HOST_API_H
#define RS64_HOST_API_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// Bump only when a field is appended to rs64_host_api; existing fields never change.
#define RS64_HOST_API_VERSION 3

// A hook handler returns CONTINUE to fall through to the game's code, RETURN to make the hooked function return now (set ctx->r2 first if it returns a value).
enum { RS64_HOOK_CONTINUE = 0, RS64_HOOK_RETURN = 1 };

// Registration (add_hook, add_text_source, add_input_filter, add_action, add_condition, add_key_handler, add_quit_handler) works only during rs64_mod_init; flags (set_flag, get_flag) are runtime and may be set/read any time.
// Stable hook ids (docs/modding-host-api.md lists each one's site and register contract). Never renumber.
typedef enum rs64_hook_id {
    RS64_HOOK_NONE = 0,
    RS64_HOOK_MENU_INPUT = 1,            // menuControllerInput 0x800B4CA8, after the pad poll: pressed word 0x8013A960 + 4*port may be edited
    RS64_HOOK_MENU_PILOT_CHOSEN = 2,     // menuControllerInput 0x800B5428: a pilot was confirmed on the account screen
    RS64_HOOK_MENU_FRAME = 3,            // updateMenuPerFrame entry: once per front-end frame
    RS64_HOOK_MISSION_INIT = 4,          // initMission 0x800FA28C: before the level loads
    RS64_HOOK_MISSION_FRAME_DT = 5,      // runInMissionFrame 0x800FA7D0: ctx->f20 = frame dt (seconds), may be replaced
    RS64_HOOK_MISSION_FRAME_PADS = 6,    // runInMissionFrame 0x800FA7D8: after the pad read, before the simulation
    RS64_HOOK_SONG_ACTIVE = 7,           // tickSongFadeTimer 0x800EE82C: ctx->r2 = isSongHandleActive result, may be replaced
    RS64_HOOK_SPEECH_RESPONSE_2 = 8,     // requestSpeechResponseMode2 0x800FBB08: ctx->r2 = speech idle (1) / busy (0), may be replaced
    RS64_HOOK_SPEECH_RESPONSE_1 = 9,     // requestSpeechResponseMode1 0x800FBB64: as RS64_HOOK_SPEECH_RESPONSE_2
    RS64_HOOK_MISSION_FRAME_NPCS = 10,   // runInMissionFrame 0x800FA94C: before the NPC tick pass
    RS64_HOOK_MISSION_END = 11,          // endMissionCleanup 0x800FB9E4: the mission is being torn down
    RS64_HOOK_TRANSITION_REQUEST = 12,   // requestMissionTransitionMode 0x800FB190: a0 = requested mode; RETURN with r2 = 0 refuses it
    RS64_HOOK_FREEZE_CHECK_A = 13,       // runInMissionFrame 0x800FA8E4: set r2 = 0 to keep the world running (pause/freeze test)
    RS64_HOOK_FREEZE_CHECK_B = 14,       // runInMissionFrame 0x800FACA8: as RS64_HOOK_FREEZE_CHECK_A
    RS64_HOOK_CUTSCENE_FREEZE_CHECK = 15,// cutsceneActorNpcHandler 0x80044970: as RS64_HOOK_FREEZE_CHECK_A
    RS64_HOOK_OBJECTIVE_COUNT = 16,      // datItemSetObjectiveBooleanCount 0x80065914: RETURN drops the update
    RS64_HOOK_RESULT_FAIL = 17,          // setHudEnableBit4 0x800C7738: mission failure request; RETURN drops it
    RS64_HOOK_RESULT_SUCCESS = 18,       // setHudEnableBit8 0x800C776C: mission success request; RETURN drops it
    RS64_HOOK_WINGMAN_TICK = 19,         // npcWingmanUpdate 0x800D9A00: post-state join, s3 = ctx, s2 = action (3 = tick), s1 = &dt
    RS64_HOOK_NPC_ACTIVATION = 20,       // isNpcWithinActiveReferenceRange 0x80047D9C: s0 = the NPC being range-tested
    RS64_HOOK_GRID_STREAM = 21,          // lookupActivePlayerCraftGridCell 0x80047E44: the player's terrain cell lookup
    RS64_HOOK_MISSION_SELECT_INIT = 22,  // drawRadarIcon 0x800C5E84 (menu overlay): mission select screen built
    RS64_HOOK_MISSION_CONFIRMED = 23,    // drawRadarIcon 0x800C5ED8 (menu overlay): s0 = the confirmed level, may be replaced
    RS64_HOOK_MISSION_SELECT_FONTS = 24, // initCraftSelectScreen 0x800AF0D0 (menu overlay): fontAlloc args (a2 = slot count) may be raised
    RS64_HOOK_MISSION_SELECT_TICK = 25,  // tickCraftSelectScreen 0x800AF3EC (menu overlay): once per mission-select frame
    RS64_HOOK_CRAFT_SELECT_INIT = 26,    // hangarInitialize 0x800AAC44 (menu overlay): a2 = font slot count, may be raised
    RS64_HOOK_CRAFT_SELECT_TICK = 27,    // runHangarSelectionFrame 0x800AADD8 (menu overlay): after its pad poll
    RS64_HOOK_CRAFT_ASSETS = 28,         // initMission 0x800FA5BC: after choosePlayerCraftAssets (extra meshes may load here)
    RS64_HOOK_HUD_FONTS = 29,            // initVoiceSubtitleSystem 0x80055B08: subtitle fontAlloc, a2 = slot count, may be raised
    RS64_HOOK_HUD_DRAW = 30,             // runInMissionFrame 0x800FAC70: 2D overlay list is built for this frame
    RS64_HOOK_RADAR = 31,                // renderRadarMinimap 0x800C6290: after placeRadarDots, s0 = radar, s2 = centre
    // Host-dispatched (no toml site): the built-in named in each comment runs them from inside its own body.
    RS64_HOOK_MENU_PAD = 32,             // inside RS64_HOOK_MENU_INPUT's built-in, before mod-page row navigation: pressed word 0x8013A960 may be edited; flag menu_page_shown = 1 while a mod page is up
    RS64_HOOK_MAIN_MENU = 33,            // rs64_menu_install_main: the main menu is being built; ctx is NULL
    // toml sites again.
    RS64_HOOK_POWERUP_TOUCH = 34,        // npcPowerUpUpdate 0x800EBD20: before the touch test, s1 = the power-up (s1+4 its DAT record), f4 = squared distance to player 1, f0 = squared radius; f4 = 0 collects it
    RS64_HOOK_POWERUP_COLLECT = 35,      // npcPowerUpUpdate 0x800EBD34: s1 = the power-up being collected, a1 = its pickup sound's position, may be replaced
    RS64_HOOK_TRIGGER_EFFECT = 36,       // applyDatObjectiveTriggerEffect 0x80065980: a0 = DAT trigger event, a1 = 1 enter / 0 exit, s0 = the caller's trigger-list entry (player 1's list is 0x80137DF0); RETURN drops the effect
    RS64_HOOK_WALKER_TRIPPED = 37,       // npcAtAtUpdate 0x800CED7C: an AT-AT accepts a tow-cable trip (action 9, kind 0xE); s1 = its ext (+0x34 DAT item), s0 = the message. Never RETURN (mid-function)
    RS64_HOOK_COUNT
} rs64_hook_id;

// ctx is the game's recomp_context*; rdram is the game's RDRAM base.
typedef int (*rs64_hook_fn)(uint32_t hook, uint8_t* rdram, void* ctx, void* user);
// Returns a NUL-terminated menu string (uppercase menu font); the pointer must stay valid until the next call.
typedef const char* (*rs64_text_fn)(void* user);
// Pad of `port` after the host read it: buttons (N64 bits), stick -1..1; filters may change them.
typedef void (*rs64_input_fn)(int port, uint16_t* buttons, float* x, float* y, void* user);
// A menu action or leave_action named in roguesq_menu.json.
typedef void (*rs64_action_fn)(void* user);
// A menu exit_when condition: non-zero = met.
typedef int (*rs64_condition_fn)(void* user);
// Typed text (utf8 non-NULL, key 0) or Backspace / Enter (utf8 NULL, key '\b' / '\r'); return 1 to take the event from the game.
typedef int (*rs64_key_fn)(const char* utf8, int key, void* user);
typedef void (*rs64_event_fn)(void* user);

typedef struct rs64_host_api {
    uint32_t version;
    uint32_t size;
    void (*log)(const char* line);
    int (*add_hook)(uint32_t hook, rs64_hook_fn fn, void* user);
    int (*add_text_source)(const char* name, rs64_text_fn fn, void* user);
    int (*add_input_filter)(rs64_input_fn fn, void* user);
    void (*set_flag)(const char* name, int value);
    int (*get_flag)(const char* name);
    // Calls the recompiled function at vram (a0-a3 = args, stack words at sp+0x10.., f12, f14), restoring every register after; returns v0, f0 into *f0_out when non-null.
    uint32_t (*call)(uint8_t* rdram, void* ctx, uint32_t vram, const uint32_t* args, uint32_t nargs, const uint32_t* stack, uint32_t nstack, float f12, float f14, float* f0_out);
    // RDRAM allocation the game can read (KSEG0 address), never freed.
    uint32_t (*alloc)(uint8_t* rdram, uint32_t size);
    const char* (*state_id)(void);
    int (*in_cutscene)(void);
    // v2
    int (*add_action)(const char* name, rs64_action_fn fn, void* user);
    int (*add_condition)(const char* name, rs64_condition_fn fn, void* user);
    int (*add_key_handler)(rs64_key_fn fn, void* user);
    // Runs on window close or the menu QUIT, just before the process exits.
    int (*add_quit_handler)(rs64_event_fn fn, void* user);
    // Cutscenes skipped so far (only grows).
    uint32_t (*cutscene_skips)(void);
    // Lets the other game threads run; from a hook only (never host-sleep in a hook).
    void (*yield)(uint8_t* rdram, void* ctx);
    void (*request_quit)(void);
    // on = SDL text input (and a phone's keyboard) on. text_input, screen_keyboard_shown and any_key_held: call from the game thread (a hook handler).
    void (*text_input)(int on);
    // -1 = no on-screen keyboard on this platform, else 1 while it is up.
    int (*screen_keyboard_shown)(void);
    int (*any_key_held)(void);
    // The exe's directory with a trailing separator, "" if unknown.
    const char* (*base_path)(void);
    // v3: a named table one mod publishes for another (e.g. "mp.transport", include/rs64/mp_transport.h). add_service only before the registry seals, first provider wins; get_service any time, NULL if none.
    int (*add_service)(const char* name, const void* table);
    const void* (*get_service)(const char* name);
} rs64_host_api;

// Mods dispatch after the built-ins, in mod-load order. During rs64_mod_init `call` and `yield` have no usable game ctx and init runs inside rs64_hook's static initializer (yielding can deadlock other game threads entering rs64_hook): do not use them there (alloc is fine).
// A native mod library exports this as a recomp function: r4 = const rs64_host_api*, r5 = RS64_HOST_API_VERSION the host implements; return r2 = 0 to accept (non-zero refuses) and r3 = the RS64_HOST_API_VERSION the mod was built against (0 = 1). The host skips and rolls back a mod that refuses or was built for a newer API.
#define RS64_MOD_INIT_EXPORT "rs64_mod_init"

#ifdef __cplusplus
}
#endif
#endif
