# Native mod host API

A native mod is a shared library (`.dll` / `.so`) written in C or C++ and loaded by the game at startup. It can provide menu behavior through `@export` names ([modding.md](../guides/modding.md#menu-behavior-from-your-own-code-export)) and take the **host API**: a table of functions for hooking game code at fixed points, supplying live menu text, filtering controller input, sharing flags and services with other mods, and calling game functions.

Pick a native mod over a `.nrm` code mod ([modding.md](../guides/modding.md#code-mods-nrm)) when you need host-side features: networking, files, OS APIs, or a large amount of native code. Online co-op ([mods/multiplayer-native/](../../mods/multiplayer-native/)) is one.

The contract is [include/rs64/host_api.h](../../include/rs64/host_api.h). The host side is [src/host_api/host_api.cpp](../../src/host_api/host_api.cpp) (the registry) and [src/host_api/host_api_game.cpp](../../src/host_api/host_api_game.cpp) (handing the table to mods).

## Your first native mod

[mods/native-demo/](../../mods/native-demo/) counts front-end frames and shows the count on a menu page. To build and try it:

```sh
cmake -S mods/native-demo -B mods/native-demo/build
cmake --build mods/native-demo/build
```

The DLL lands beside `mod.json`. Copy the folder into the game's `mods/` directory, enable **Native Demo** in the Mods window (it is off by default), and open MAIN MENU > NATIVE DEMO: the label reads `DEMO FRAMES n` and counts up. The log shows `[native-demo] host API v7 taken` and `[host] mod native-demo took the host API (...)`.

How [native_demo.cpp](../../mods/native-demo/native_demo.cpp) works:

1. It includes `rs64/host_api.h` (its CMake file adds the repo's `include/` directory) and exports `rs64_mod_init` plus `uint32_t recomp_api_version = 1` (librecomp's loader version, separate from the host API version).
2. `rs64_mod_init` reads the table from `r4` and the host's version from `r5`. If the table is unusable it sets `r2 = 1` and returns, and the mod is skipped. On every return it sets `r3 = RS64_HOST_API_VERSION`.
3. It registers a `RS64_HOOK_MENU_FRAME` handler that increments a counter and returns `RS64_HOOK_CONTINUE`, and a text source `demo_frames` that formats `DEMO FRAMES n` into a static buffer. Then it sets `r2 = 0`.
4. [mod.json](../../mods/native-demo/mod.json) lists `rs64_mod_init` in its `native_libraries` export array, next to the menu exports. An export that isn't listed can't be loaded.
5. [roguesq_menu.json](../../mods/native-demo/roguesq_menu.json) shows the text: a `label` entry with `"label_src": "demo_frames"` on a page opened from the main menu.

The rest of this page is the reference.

## Initialization

A mod exports `rs64_mod_init` (`RS64_MOD_INIT_EXPORT`), listed in its `mod.json` export array. It has the recomp signature `void(uint8_t* rdram, void* ctx)`, and registers carry the contract:

| Register | Meaning |
|---|---|
| `r4` (in) | `const rs64_host_api*`, the table |
| `r5` (in) | `RS64_HOST_API_VERSION` the host implements (currently 7) |
| `r2` (out) | 0 to accept, non-zero to refuse |
| `r3` (out) | the `RS64_HOST_API_VERSION` the mod was built against (0 = not set, treated as 1) |

Check `table->size >= sizeof(rs64_host_api)` before using fields, and always return the version you were built against in `r3`.

**Versioning.** The version goes up only when a field is appended to the table; existing fields never change. The host accepts a mod built for its own version or an older one, and skips a mod built for a newer one (logging both versions).

**When init runs.** On the first hook call after boot: the built-in hooks register, then every enabled mod that exports `rs64_mod_init` is initialized in mod-load order, then the registry is **sealed**. A mod that refuses (non-zero `r2`) or is skipped has everything it registered rolled back, and a log line says so.

**What works when.** All the `add_*` functions work only inside `rs64_mod_init`; after the seal they refuse and log. `set_flag`, `get_flag` and `get_service` work any time. During init `rdram` is real and `alloc` works, but `call` has no game context and `yield` must not be used: init runs inside `rs64_hook`'s static initializer, so yielding can deadlock other game threads entering it.

## The table

`rs64_host_api` fields, in order:

| Field | Contract |
|---|---|
| `uint32_t version` | `RS64_HOST_API_VERSION` |
| `uint32_t size` | `sizeof(rs64_host_api)` in the host build |
| `log(const char* line)` | Writes one line to stderr |
| `add_hook(hook, fn, user)` | Registers a handler for a hook id. Returns 0, or -1 for an invalid id, a null `fn`, or a sealed registry |
| `add_text_source(name, fn, user)` | Registers a menu string source (see Text sources). Returns 0, or -1 for a null argument, a sealed registry, or a name already taken (the first registration wins) |
| `add_input_filter(fn, user)` | Registers a pad filter (see Input filters). Returns 0, or -1 for a null `fn` or a sealed registry |
| `set_flag(name, value)` | Sets a process-wide int flag |
| `get_flag(name)` | Reads a flag; unset flags read 0 |
| `call(rdram, ctx, vram, args, nargs, stack, nstack, f12, f14, f0_out)` | Calls the recompiled function at `vram` (`a0`-`a3` = the first four args, `stack` = up to 12 words stored at `sp+0x10..`, `f12`/`f14` = the float arguments), restores every register after, and returns `v0`. `f0` goes to `*f0_out` when non-null. Needs the `ctx` a hook handler received; returns 0 with a null `ctx` |
| `alloc(rdram, size)` | RDRAM allocation the game can read; returns a KSEG0 address, never freed |
| `state_id()` | The classified game-state id string (for example the menu or mission state) |
| `in_cutscene()` | Non-zero while a cinematic is playing |
| `add_action(name, fn, user)` | v2. Registers a menu action (see Menu actions). Returns 0, or -1 for a null argument, a sealed registry, or a name already taken (first wins, the second is logged) |
| `add_condition(name, fn, user)` | v2. Registers an `exit_when` condition, same return rules as `add_action` |
| `add_key_handler(fn, user)` | v2. Registers a typed-text / Backspace / Enter handler. Returns 0, or -1 for a null `fn` or a sealed registry |
| `add_quit_handler(fn, user)` | v2. Registers a handler run on window close or the menu QUIT, just before the process exits. Returns 0, or -1 as above |
| `cutscene_skips()` | v2. Cutscenes skipped so far (only grows) |
| `yield(rdram, ctx)` | v2. Lets the other game threads run; from a hook only (never host-sleep in a hook) |
| `request_quit()` | v2. Asks the host to quit, as the menu QUIT does |
| `text_input(on)` | v2. `on` non-zero starts SDL text input (and a phone's keyboard), zero stops it |
| `screen_keyboard_shown()` | v2. -1 = no on-screen keyboard on this platform, else 1 while it is up |
| `any_key_held()` | v2. Non-zero while any keyboard key is down. `text_input`, `screen_keyboard_shown` and `any_key_held` are for the game thread (a hook handler), as the built-in lobby uses them |
| `base_path()` | v2. The exe's directory with a trailing separator, `""` if unknown |
| `add_service(name, table)` | v3. Publishes a named table for other mods (see Services). Returns 0, or -1 for a null argument, a sealed registry, or a name already taken (first wins, the second is logged) |
| `get_service(name)` | v3. The table published under `name`, or NULL. Works any time; look a service up after the seal (on first use), since mods init in load order and the provider may come after you |
| `add_hash_part(name, gate_flag, fn, user)` | v4. Adds a column to the lockstep hash log (`ROGUESQ_LS_HASH`): `fn(rdram, user)` returns a 64-bit hash of the mod's state, counted only while the flag `gate_flag` is non-zero. Parts follow the base parts in registration order. Returns 0, or -1 for a null argument or a sealed registry |
| `add_valid_ram(addr, size)` | v4. Declares a host-RAM range above the game's 8 MB (KSEG0 address) that holds game records the mod built, so base guards such as the NPC health patch accept pointers into it. Returns 0, or -1 for size 0 or a sealed registry |
| `add_moved_range(lo, hi, delta)` | v5. Declares game data `[lo, hi)` the mod moved to `lo + delta` (its `instruction_patches.json` repoints the game's code), so host code that reads it (lockstep hashes, `rs64_relocated`) follows. Returns 0, or -1 for an empty range, a zero delta, an overlap with a registered range, or a sealed registry |
| `add_rule_action(name, fn, user)` | v6. Adds a level rules action that a level's `rules.json` names with `"do": name` (see Level rules actions). Returns 0, or -1 for a null argument, a sealed registry, or a name already taken (first wins) |
| `fire_rule_event(name, object)` | v6. Raises the level rules "when" `custom` named `name` for `object` (may be `""`). Works any time and from any thread; the rules see it on the next mission frame, and events fired outside a mission are dropped. Returns 0, or -1 for a null name |
| `terrain_get_cell(rdram, col, row, heights, tex)` | v7. Reads the level map's cell (`col`, `row`): its 25 heights (`int8_t`, the HMP's units; larger is lower, 16 record units a step) and, when `tex` is non-null, its texture id (an `lv_X_TEX` index). Returns 0, -1 with no map loaded, or -2 for a cell outside the map |
| `terrain_set_cell(rdram, col, row, heights, tex)` | v7. Writes the cell's 25 heights and, when `tex` >= 0, its texture id into the game's own map, so its height queries (collision) and the renderer (the whole-map terrain pass and its ray-traced mesh) follow. Cells share tiles, so the map's first edit moves its tile table to host RAM (0x80A10000) and an edited cell that shares its tile gets its own copy. Neighbouring cells share their edge heights (a cell's last row and column of 5 are the next cell's first): keep them equal, or the legacy terrain renderer shows cracks between the cells. The legacy renderer shows height edits at once and a texture change when the cell next streams in. From the game thread (a hook). Returns 0, -1 / -2 as above, or -3 when the map has 8192 tiles already |

## Services

A service is a versioned C table one mod hands another through the host, so a mod can extend another mod without either linking the other. The provider calls `add_service` in `rs64_mod_init`; the consumer calls `get_service` on first use (after the seal, so load order does not matter) and checks the table's own `version` / `size` fields before using it. A refused mod's services are rolled back with its other registrations.

| Name | Header | Provider | Consumer |
|---|---|---|---|
| `mp.transport` | [include/rs64/mp_transport.h](../../include/rs64/mp_transport.h) | [mods/steam-relay](../../mods/steam-relay/) | Multiplayer's lobby (`relay()` in `ghost_lobby.cpp`): while `available()` is true, HOST / JOIN run over it instead of ENet; a version 2 table (`RS64_MP_TRANSPORT_HAS_LIST`) also feeds the FIND GAME list; see [multiplayer.md](../guides/multiplayer.md#steam-relay-mod) |

## Hooks

A handler is `int fn(uint32_t hook, uint8_t* rdram, void* ctx, void* user)`. `ctx` is the game's `recomp_context*` for the hooked function, `rdram` the RDRAM base. Return `RS64_HOOK_CONTINUE` (0) to fall through to the game's code, or `RS64_HOOK_RETURN` (1) to make the hooked function return now; set `ctx->r2` first if it returns a value. Hook ids are stable and never renumbered. `RS64_HOOK_NONE` (0) is invalid, and `RS64_HOOK_COUNT` is the end marker.

Register names below are the game's MIPS registers in `ctx` (`ctx->r2`, `ctx->f20`, ...). A site is the function and address where the host calls the hook.

### Front end

| Id | Site | Contract |
|---|---|---|
| `RS64_HOOK_MENU_INPUT` (1) | `menuControllerInput` 0x800B4CA8, after the pad poll | The pressed-buttons word at 0x8013A960 + 4 * port may be edited |
| `RS64_HOOK_MENU_PILOT_CHOSEN` (2) | `menuControllerInput` 0x800B5428 | A pilot was confirmed on the account screen |
| `RS64_HOOK_MENU_FRAME` (3) | `updateMenuPerFrame` entry | Once per front-end frame |
| `RS64_HOOK_MISSION_SELECT_INIT` (22) | `drawRadarIcon` 0x800C5E84 (menu overlay) | The mission select screen was built |
| `RS64_HOOK_MISSION_CONFIRMED` (23) | `drawRadarIcon` 0x800C5ED8 (menu overlay) | `s0` = the confirmed level, may be replaced |
| `RS64_HOOK_MISSION_SELECT_FONTS` (24) | `initCraftSelectScreen` 0x800AF0D0 (menu overlay) | `fontAlloc` args: `a2` = slot count, may be raised |
| `RS64_HOOK_MISSION_SELECT_TICK` (25) | `tickCraftSelectScreen` 0x800AF3EC (menu overlay) | Once per mission-select frame |
| `RS64_HOOK_CRAFT_SELECT_INIT` (26) | `hangarInitialize` 0x800AAC44 (menu overlay) | `a2` = font slot count, may be raised |
| `RS64_HOOK_CRAFT_SELECT_TICK` (27) | `runHangarSelectionFrame` 0x800AADD8 (menu overlay) | After its pad poll |

### Mission

| Id | Site | Contract |
|---|---|---|
| `RS64_HOOK_MISSION_INIT` (4) | `initMission` 0x800FA28C | Before the level loads |
| `RS64_HOOK_MISSION_FRAME_DT` (5) | `runInMissionFrame` 0x800FA7D0 | `ctx->f20` = frame dt in seconds, may be replaced |
| `RS64_HOOK_MISSION_FRAME_PADS` (6) | `runInMissionFrame` 0x800FA7D8 | After the pad read, before the simulation |
| `RS64_HOOK_MISSION_FRAME_NPCS` (10) | `runInMissionFrame` 0x800FA94C | Before the NPC tick pass |
| `RS64_HOOK_MISSION_END` (11) | `endMissionCleanup` 0x800FB9E4 | The mission is being torn down |
| `RS64_HOOK_TRANSITION_REQUEST` (12) | `requestMissionTransitionMode` 0x800FB190 | `a0` = requested mode; `RETURN` with `r2` = 0 refuses it |
| `RS64_HOOK_FREEZE_CHECK_A` (13) | `runInMissionFrame` 0x800FA8E4 | Set `r2` = 0 to keep the world running (pause/freeze test) |
| `RS64_HOOK_FREEZE_CHECK_B` (14) | `runInMissionFrame` 0x800FACA8 | As `RS64_HOOK_FREEZE_CHECK_A` |
| `RS64_HOOK_CUTSCENE_FREEZE_CHECK` (15) | `cutsceneActorNpcHandler` 0x80044970 | As `RS64_HOOK_FREEZE_CHECK_A` |
| `RS64_HOOK_OBJECTIVE_COUNT` (16) | `datItemSetObjectiveBooleanCount` 0x80065914 | `RETURN` drops the update |
| `RS64_HOOK_RESULT_FAIL` (17) | `setHudEnableBit4` 0x800C7738 | Mission failure request; `RETURN` drops it |
| `RS64_HOOK_RESULT_SUCCESS` (18) | `setHudEnableBit8` 0x800C776C | Mission success request; `RETURN` drops it |
| `RS64_HOOK_WINGMAN_TICK` (19) | `npcWingmanUpdate` 0x800D9A00 | Post-state join: `s3` = ctx, `s2` = action (3 = tick), `s1` = &dt |
| `RS64_HOOK_NPC_ACTIVATION` (20) | `isNpcWithinActiveReferenceRange` 0x80047D9C | `s0` = the NPC being range-tested |
| `RS64_HOOK_GRID_STREAM` (21) | `lookupActivePlayerCraftGridCell` 0x80047E44 | The player's terrain cell lookup |
| `RS64_HOOK_CRAFT_ASSETS` (28) | `initMission` 0x800FA5BC | After `choosePlayerCraftAssets`; extra meshes may load here |

### Audio and speech

| Id | Site | Contract |
|---|---|---|
| `RS64_HOOK_SONG_ACTIVE` (7) | `tickSongFadeTimer` 0x800EE82C | `ctx->r2` = the `isSongHandleActive` result, may be replaced |
| `RS64_HOOK_SPEECH_RESPONSE_2` (8) | `requestSpeechResponseMode2` 0x800FBB08 | `ctx->r2` = speech idle (1) / busy (0), may be replaced |
| `RS64_HOOK_SPEECH_RESPONSE_1` (9) | `requestSpeechResponseMode1` 0x800FBB64 | As `RS64_HOOK_SPEECH_RESPONSE_2` |

### HUD

| Id | Site | Contract |
|---|---|---|
| `RS64_HOOK_HUD_FONTS` (29) | `initVoiceSubtitleSystem` 0x80055B08 | Subtitle `fontAlloc`: `a2` = slot count, may be raised |
| `RS64_HOOK_HUD_DRAW` (30) | `runInMissionFrame` 0x800FAC70 | The 2D overlay list is built for this frame |
| `RS64_HOOK_RADAR` (31) | `renderRadarMinimap` 0x800C6290 | After `placeRadarDots`: `s0` = radar, `s2` = centre |
| `RS64_HOOK_POWERUP_TOUCH` (34) | `npcPowerUpUpdate` 0x800EBD20 | Before the touch test: `s1` = the power-up (`s1+4` its DAT record), `f4` = squared distance to player 1, `f0` = squared radius; `f4 = 0` collects it. Never RETURN (mid-function) |
| `RS64_HOOK_POWERUP_COLLECT` (35) | `npcPowerUpUpdate` 0x800EBD34 | A power-up is being collected: `s1` = it, `a1` = its pickup sound position, may be replaced. Never RETURN (mid-function) |
| `RS64_HOOK_TRIGGER_EFFECT` (36) | `applyDatObjectiveTriggerEffect` 0x80065980 | A DAT trigger volume's effect is about to run: `a0` = the event, `a1` = 1 enter / 0 exit, `s0` = the caller's trigger-list entry (player 1's list is 0x80137DF0, 63 entries). RETURN drops the effect |
| `RS64_HOOK_WALKER_TRIPPED` (37) | `npcAtAtUpdate` 0x800CED7C | An AT-AT accepts a tow-cable trip (action 9, message kind 0xE): `s1` = its ext (`+0x34` its DAT item), `s0` = the message. Never RETURN (mid-function) |
| `RS64_HOOK_ATST_TICK` (38) | `updatePlayerAtStBehavior` entry | The player's AT-ST (level 20, CHICKEN) is about to read the pads (0x80130B88, 6 bytes per port) and the normalized stick (0x8010BDD8, x/y floats per port): `a0` = the walker, `a1` = dt (float bits). Stick Y sets walk speed (`+0x60`), X times Y turns, holding BRAKES1 moves the head (`+0x68` yaw, `+0x6C` pitch) instead; the cannons auto-aim inside a cone. RETURN skips the step. The built-in sets the flag `atst_active` |

### Host-dispatched

These ids have no `rogue_squadron.toml` site: the host runs them from inside the named built-in.

| Id | Where | Contract |
|---|---|---|
| `RS64_HOOK_MENU_PAD` (32) | Inside the `RS64_HOOK_MENU_INPUT` built-in, before mod-page row navigation | The pressed word at 0x8013A960 may be edited; the flag `menu_page_shown` is 1 while a mod page is up |
| `RS64_HOOK_MAIN_MENU` (33) | `rs64_menu_install_main` | The main menu is being built; `ctx` is NULL |

`RS64_HOOK_COUNT` is an end marker that grows as ids are added; mods must not rely on its value.

### Events for `.nrm` mods

Every hook with a `rogue_squadron.toml` site is also a librecomp base event, named `rs64_on_` plus the id in lower case without `RS64_HOOK_` (`RS64_HOOK_MISSION_FRAME_PADS` is `rs64_on_mission_frame_pads`). A code mod subscribes with `RECOMP_CALLBACK("*", rs64_on_mission_frame_pads) void f(void) { ... }` from [syms/include/rs64_api.h](../../syms/include/rs64_api.h). The host-dispatched ids above have no event.

- Native handlers run first. If one returns `RS64_HOOK_RETURN`, the event does not fire.
- The callback gets the hook site's registers, so `a0`-`a3` arrive as its arguments. librecomp restores every register after each callback, so a register change goes through `rs64_event_set_gpr(reg, value)` (any GPR but `r0` and `sp`) or `rs64_event_set_fpr(reg, value)` (a single-precision float). Those writes are applied after the last callback, at every hook point, up to 8 per event; at `mission_frame_dt`, `rs64_event_set_fpr(20, dt)` replaces the frame time.
- At the function-entry sites (`menu_frame`, `mission_end`, `transition_request`, `objective_count`, `result_fail`, `result_success`, `trigger_effect`) a callback can call `rs64_event_return(r2)`: the game function returns at once with `v0 = r2`, as a native `RS64_HOOK_RETURN` would. Everywhere else the call is ignored, because returning mid-function would skip its epilogue.
- [tests/mods/event-probe/](../../tests/mods/event-probe/) is a working example (counts frames, returns once from `menu_frame`, calls the exports below).

`rs64_api.h` also imports these base exports:

| Export | Returns |
|---|---|
| `float rs64_draw_dist(void)` | The draw-distance multiplier in effect (1.0 = the game's own; cutscenes may keep 1.0) |
| `float rs64_terrain_dist(void)` | The terrain reach and fog multiplier, which follows the draw distance capped at 2.5 |
| `unsigned rs64_tgrid_base(unsigned original)` | Where a terrain-grid table now lives (host RAM, doubled strides); other addresses pass through |
| `unsigned rs64_relocated(unsigned address)` | Where a game field lives now: a per-player field the multiplayer mod moved (ranges R1-R4, only while that mod is enabled); other addresses pass through. `.nrm` mods link against the original addresses, so translate through this before reading such a field |

## Menu actions, conditions, key and quit handlers

- `add_action("name", fn, user)` registers `void fn(void* user)`, run by a menu `action` or `leave_action` of that name in `roguesq_menu.json`.
- `add_condition("name", fn, user)` registers `int fn(void* user)`; a menu `exit_when` of that name is met while it returns non-zero.
- Names are first-wins for both: a later registration of the same name is refused and logged, and the first stays.
- `add_key_handler(fn, user)` registers `int fn(const char* utf8, int key, void* user)`. Typed text arrives as `utf8` non-NULL with `key` 0; Backspace and Enter arrive as `utf8` NULL with `key` `'\b'` / `'\r'`. Handlers run in registration order and the first that returns 1 takes the event; later ones and the game do not see it.
- `add_quit_handler(fn, user)` registers `void fn(void* user)`. Quit handlers run in registration order before `_Exit`, on window close or the menu QUIT.

## Level rules actions

A native mod can add actions and "when"s to the [level rules](../guides/modding.md#level-rules) a level author builds in the explorer.

- `add_rule_action("shake", fn, user)` registers `void fn(uint8_t* rdram, void* ctx, const char* const* keys, const char* const* values, int count, void* user)`. A rule action `{ "do": "shake", "seconds": 2 }` calls it on the game thread with the action's other fields as `count` key/value strings (`"seconds"`, `"2"`). Call game functions from it only through `call`. A built-in action of the same name runs instead of yours.
- `fire_rule_event("mod.tripped", "gate")` raises a `custom` when. A rule `{ "when": "custom", "custom": "mod.tripped" }` fires for any object; one that also names `"object": "gate"` fires only for that object.
- A rule that names an action no loaded mod registered is skipped, and the log says `no action <name>; skipped`.

[native-demo](../../mods/native-demo/) has a working example: its `demo_log` action logs the rule's fields and then fires `demo.ping`, and its `rules_ext.json` describes both for the explorer.

The explorer offers a mod's actions and events once it knows them: put a `rules_ext.json` in the mod's folder and press *Mod actions…* in the Level events window (it reads `mods/*/rules_ext.json` in the game folder). A rule using one of them shows "needs a mod" when that mod is not there.

```json
{
  "actions": [ { "do": "shake", "label": "Shake the camera", "params": [ { "key": "seconds", "picker": "seconds" } ] } ],
  "events": [ { "custom": "mod.tripped", "label": "The tripwire is crossed" } ]
}
```

A parameter's `picker` is one of `area`, `path`, `pathPoint`, `unit`, `camera`, `song`, `enemyKind`, `objective`, `voice`, `recording`, `text`, `number`, `seconds` or `flag`; any other value becomes a text box.

## Text sources

`add_text_source("name", fn, user)` registers `const char* fn(void* user)`. A menu label in a mod's `roguesq_menu.json` with `"label_src": "name"` shows the string live. See [Live labels](../guides/adding-menus-and-buttons.md#live-labels). Rules:

- Return a NUL-terminated string in the menu font's character set (uppercase).
- The pointer must stay valid until the next call of that source; a static buffer is the usual answer.
- The source is read on the game thread.
- Names are first-wins. A later registration of the same name is refused, logged, and the first stays.

## Input filters

`add_input_filter(fn, user)` registers `void fn(int port, uint16_t* buttons, float* x, float* y, void* user)`. It runs after the host reads the pad, with the N64 button bits and a stick in -1..1, and may change any of them. Only port 0 is filtered today (`input_host.cpp` calls `run_input_filters(0, ...)`); check `port` in the filter anyway. Filters run in registration order.

## Rules

- Dispatch order is registration order: the built-in handlers first, then mods in load order. The first handler that returns `RS64_HOOK_RETURN` stops the chain; later handlers do not run.
- A mod's handler on a hook that has a built-in runs after the whole built-in, so it sees (and can override) what the built-in decided.
- Handlers run on the game thread inside the hooked function. They must not block or sleep (a host sleep in a game-thread hook holds the run slot and can deadlock the frame pipeline), and may call game functions only through `call`, which saves and restores the context.
- Registration is init-time only, as above. Keep per-hook state in `user` or your own statics.
- Text-source strings stay valid until the next call of that source.
- Flags are process-wide ints, set and read by name from any thread (mutex-guarded). `hangar_launch` is the first one: the lobby sets it while the hangar is launching a mission, and the nav sequencer reads it. All current flags (writer, reader in [multiplayer.md](../guides/multiplayer.md#flags)): `hangar_launch`, `ghost_mode`, `coop_imposter`, `mp_lobby_state` and `text_entry` are set by the multiplayer code; `menu_page_shown` by the menu (for `RS64_HOOK_MENU_PAD`); `atst_active` by the `RS64_HOOK_ATST_TICK` built-in (1 while the player drives an AT-ST, cleared by the next mission init or end; while it is set and the mouse is captured, the mouse aims the head instead of driving the stick, the layout's THRUST and BRAKES1 buttons walk forward and back, and a hook at 0x800D1AE0 writes the head angles after `RS64_HOOK_ATST_TICK` handlers ran).
- Nothing runs before the first hook fires: pad polls and menu text drawn during boot see no filters or sources (an empty source falls back to its JSON label).
- `rs64_mod_init` runs inside a game-thread hook: it must not block or sleep.
- Flags set by a mod whose init then refuses are not rolled back.
- An access violation inside a mod's init or handler is caught per game thread and can stall the game; mods must not fault.
- `log` goes to stderr, which is where `[host]` lines about refused mods and sealed-registry rejections also appear.

## What stays in the base game

The host API covers behavior added at the sites above. These stay base-game code and are not exposed:

- Every `[[patches.hook]]` site in `rogue_squadron.toml`. Each is a fixed call into `rs64_hook`; mods choose handlers for the ids above, they cannot add new sites.
- The functions those patches touch. `.nrm` mods can `RECOMP_HOOK` them (the patches are replayed) but can't `RECOMP_PATCH` them; see [Functions the game already patches](../guides/modding.md#functions-the-game-already-patches).

## A larger example: multiplayer-native

[mods/multiplayer-native/src/mp_mod.cpp](../../mods/multiplayer-native/src/mp_mod.cpp) is online co-op as a native mod: the whole of it, its sources in the mod's own `src/`, reaching the game only through the host API table ([mods/multiplayer-native/src/mp_host.h](../../mods/multiplayer-native/src/mp_host.h) wraps it).

1. `rs64_mod_init` checks the table and version as in the native-demo steps, sets `r3 = RS64_HOST_API_VERSION`, then calls `rs64_mp_register(api)` ([mods/multiplayer-native/src/mp_register.cpp](../../mods/multiplayer-native/src/mp_register.cpp)), which makes the `add_hook` and `add_quit_handler` calls and then calls `rs64_ghost_register_host` ([mods/multiplayer-native/src/ghost_lobby.cpp](../../mods/multiplayer-native/src/ghost_lobby.cpp)) for the text sources, the input filter, the `mp_*` actions, the `mp_ready` condition and the key handler.
2. The game's CMake builds the library (`mods/multiplayer-native/CMakeLists.txt`), so it uses the exe's CRT and toolchain and the same `lockstep_core.cpp` / `mp_core.cpp` rounding (`-ffp-contract=off`). On Linux only `recomp_api_version` and `rs64_mod_init` are exported (version script, `--no-undefined`, `-Bsymbolic`).
3. Its `mod.toml` holds the manifest and its 200 instruction patches; the game's CMake runs RecompModTool on it to write the folder's `mod.json` and `instruction_patches.json`, so a native mod can use the same `mod.toml` format as a code mod. The folder (library, those two files and `roguesq_menu.json`) is staged by [tools/mods/stage_mods.cmake](../../tools/mods/stage_mods.cmake); see [release-ci.md](release-ci.md#the-multiplayer-mod).
