# Native mod host API

A native mod is a shared library (`.dll` / `.so`) listed under `native_libraries` in its `mod.json`. Besides the menu exports described in [adding-menus-and-buttons.md](adding-menus-and-buttons.md), it can take the host API: a table of function pointers for hooking game code, supplying menu text, filtering the pad, and sharing flags. The contract lives in [include/rs64/host_api.h](../include/rs64/host_api.h); this page explains it. The registry is [src/main/host_api.cpp](../src/main/host_api.cpp) and the delivery code is [src/main/host_api_game.cpp](../src/main/host_api_game.cpp). A complete example is [mods/native-demo/](../mods/native-demo/).

## Delivery

A mod exports `rs64_mod_init` (`RS64_MOD_INIT_EXPORT`), listed in its `mod.json` export array. It has the recomp signature `void(uint8_t* rdram, void* ctx)`, and registers carry the contract:

| Register | Meaning |
|---|---|
| `r4` (in) | `const rs64_host_api*`, the table |
| `r5` (in) | `RS64_HOST_API_VERSION` the host implements (currently 2) |
| `r2` (out) | 0 to accept, non-zero to refuse |
| `r3` (out) | the `RS64_HOST_API_VERSION` the mod was built against (0 = not set, treated as 1) |

Check `table->size >= sizeof(rs64_host_api)` before using fields, and always return the version you were built against in `r3`. The version is bumped only when a field is appended; existing fields never change.

Init order, on the first hook call after boot: the built-in hooks register, then every enabled mod that exports `rs64_mod_init` is initialised in mod-load order, then the registry is sealed. A mod that refuses (non-zero `r2`) is skipped and everything it registered is rolled back (a log line says so). The host also skips (and rolls back) a mod whose `r3` is newer than the host's version, logging the mod and both versions; a mod built for an older version is accepted.

`add_hook`, `add_text_source`, `add_input_filter`, `add_action`, `add_condition`, `add_key_handler`, `add_quit_handler` and `add_service` work only inside `rs64_mod_init`. After the seal they refuse and log. Flags (`set_flag`, `get_flag`) and `get_service` work any time.

During `rs64_mod_init`, `rdram` is real and `alloc` works, but `call` has no game context and `yield` must not be used: init runs inside `rs64_hook`'s static initializer, so yielding can deadlock other game threads entering `rs64_hook`.

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

## Services

A service is a versioned C table one mod hands another through the host, so a mod can extend another mod without either linking the other. The provider calls `add_service` in `rs64_mod_init`; the consumer calls `get_service` on first use (after the seal, so load order does not matter) and checks the table's own `version` / `size` fields before using it. A refused mod's services are rolled back with its other registrations.

| Name | Header | Provider | Consumer |
|---|---|---|---|
| `mp.transport` | [include/rs64/mp_transport.h](../include/rs64/mp_transport.h) | [mods/steam-relay](../mods/steam-relay/) | Multiplayer's lobby (`relay()` in `ghost_lobby.cpp`): while `available()` is true, HOST / JOIN run over it instead of ENet; see [multiplayer.md](multiplayer.md#steam-relay-mod) |

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

### Host-dispatched

These ids have no `rogue_squadron.toml` site: the host runs them from inside the named built-in.

| Id | Where | Contract |
|---|---|---|
| `RS64_HOOK_MENU_PAD` (32) | Inside the `RS64_HOOK_MENU_INPUT` built-in, before mod-page row navigation | The pressed word at 0x8013A960 may be edited; the flag `menu_page_shown` is 1 while a mod page is up |
| `RS64_HOOK_MAIN_MENU` (33) | `rs64_menu_install_main` | The main menu is being built; `ctx` is NULL |

`RS64_HOOK_COUNT` is a sentinel that grows as ids are added (32 in v1, 34 in v2, 36 in v3); mods must not rely on its value.

## Menu actions, conditions, key and quit handlers

- `add_action("name", fn, user)` registers `void fn(void* user)`, run by a menu `action` or `leave_action` of that name in `roguesq_menu.json`.
- `add_condition("name", fn, user)` registers `int fn(void* user)`; a menu `exit_when` of that name is met while it returns non-zero.
- Names are first-wins for both: a later registration of the same name is refused and logged, and the first stays.
- `add_key_handler(fn, user)` registers `int fn(const char* utf8, int key, void* user)`. Typed text arrives as `utf8` non-NULL with `key` 0; Backspace and Enter arrive as `utf8` NULL with `key` `'\b'` / `'\r'`. Handlers run in registration order and the first that returns 1 takes the event; later ones and the game do not see it.
- `add_quit_handler(fn, user)` registers `void fn(void* user)`. Quit handlers run in registration order before `_Exit`, on window close or the menu QUIT.

## Text sources

`add_text_source("name", fn, user)` registers `const char* fn(void* user)`. A menu label in a mod's `roguesq_menu.json` with `"label_src": "name"` shows the string live. See "Live labels" in [adding-menus-and-buttons.md](adding-menus-and-buttons.md). Rules:

- Return a NUL-terminated string in the menu font's character set (uppercase).
- The pointer must stay valid until the next call of that source; a static buffer is the usual answer.
- The source is read on the game thread.
- Names are first-wins. A later registration of the same name is refused, logged, and the first stays.

## Input filters

`add_input_filter(fn, user)` registers `void fn(int port, uint16_t* buttons, float* x, float* y, void* user)`. It runs after the host reads the pad, with the N64 button bits and a stick in -1..1, and may change any of them. Only port 0 is filtered today (`main.cpp` calls `run_input_filters(0, ...)`); check `port` in the filter anyway. Filters run in registration order.

## Rules

- Dispatch order is registration order: the built-in handlers first, then mods in load order. The first handler that returns `RS64_HOOK_RETURN` stops the chain; later handlers do not run.
- A mod's handler on a hook that has a built-in runs after the whole built-in, so it sees (and can override) what the built-in decided.
- A v1 mod still loads on a v2 host: the host accepts any `r3` up to its own version, and fields are only ever appended to the table.
- Handlers run on the game thread inside the hooked function. They must not block or sleep (a host sleep in a game-thread hook holds the run slot and can deadlock the frame pipeline), and may call game functions only through `call`, which saves and restores the context.
- Registration is init-time only, as above. Keep per-hook state in `user` or your own statics.
- Text-source strings stay valid until the next call of that source.
- Flags are process-wide ints, set and read by name from any thread (mutex-guarded). `hangar_launch` is the first one: the lobby sets it while the hangar is launching a mission, and the nav sequencer reads it. All current flags (writer, reader in [multiplayer.md](multiplayer.md#flags)): `hangar_launch`, `ghost_mode`, `coop_imposter`, `mp_lobby_state` and `text_entry` are set by the multiplayer code; `menu_page_shown` by the menu (for `RS64_HOOK_MENU_PAD`); `mp_builtin` by the built-in hook registration when multiplayer is built in (the multiplayer-native mod reads it and stays off).
- Nothing runs before the first hook fires: pad polls and menu text drawn during boot see no filters or sources (an empty source falls back to its JSON label).
- `rs64_mod_init` runs inside a game-thread hook: it must not block or sleep.
- Flags set by a mod whose init then refuses are not rolled back.
- An access violation inside a mod's init or handler is caught per game thread and can stall the game; mods must not fault.
- `log` goes to stderr, which is where `[host]` lines about refused mods and sealed-registry rejections also appear.

## What stays in the base game

The host API covers behavior added at the sites above. These stay base-game code and are not exposed:

- Per-player data relocation (the four ranges moved into host RAM, see AGENTS.md "Per-player data relocation"). Code that touches those fields from a native mod must account for the moved addresses.
- Every `[[patches.hook]]` site in `rogue_squadron.toml`. Each is a fixed call into `rs64_hook`; mods choose handlers for the ids above, they cannot add new sites.

## Worked example: native-demo

[mods/native-demo/native_demo.cpp](../mods/native-demo/native_demo.cpp) counts front-end frames and shows the count on a menu page.

1. Include `rs64/host_api.h` (the CMake file adds the repo `include/` directory) and export `rs64_mod_init` plus `uint32_t recomp_api_version = 1`.
2. In `rs64_mod_init`, read the table from `r4` and the version from `r5`. If the version is below 1, the table is null or `size < sizeof(rs64_host_api)`, set `r2 = 1` and return (the mod is skipped). On every return set `r3 = RS64_HOST_API_VERSION`; the host uses it to skip mods built for a newer API. `recomp_api_version` (step 1) is librecomp's loader version, separate from `RS64_HOST_API_VERSION`.
3. Register a `RS64_HOOK_MENU_FRAME` handler that increments a counter and returns `RS64_HOOK_CONTINUE`, and a text source `demo_frames` that formats `DEMO FRAMES n` into a static buffer. Log a line, set `r2 = 0`.
4. List `rs64_mod_init` in the `native_libraries` array of [mod.json](../mods/native-demo/mod.json), next to the other exports. A library export that is not listed is not loadable.
5. Reference the source from [roguesq_menu.json](../mods/native-demo/roguesq_menu.json): a `label` entry on the `native_demo` page with `"label_src": "demo_frames"`, reached through the NATIVE DEMO submenu on the main menu.
6. Build with `cmake -S mods/native-demo -B mods/native-demo/build` then `cmake --build mods/native-demo/build`. The DLL lands beside `mod.json`; copy the whole folder into the game's `mods/` directory and enable the mod (it is off by default).

In game, open MAIN MENU, NATIVE DEMO: the label reads `DEMO FRAMES n` and counts up while the front end runs. The log shows `[native-demo] host API v2 taken` and `[host] mod native-demo took the host API (host v2, mod built for v2, r2=0)`.

## Worked example: multiplayer-native

[mods/multiplayer-native/mp_mod.cpp](../mods/multiplayer-native/mp_mod.cpp) is online co-op as a native mod: the whole of it, built from the game's own multiplayer sources, reaching the game only through the host API table ([src/main/mp_host.h](../src/main/mp_host.h) wraps it).

1. `rs64_mod_init` checks the table and version as in the native-demo steps, sets `r3 = RS64_HOST_API_VERSION`, then calls `rs64_mp_register(api)` ([src/main/mp_register.cpp](../src/main/mp_register.cpp)), which makes the `add_hook` and `add_quit_handler` calls and then calls `rs64_ghost_register_host` ([src/main/ghost_lobby.cpp](../src/main/ghost_lobby.cpp)) for the text sources, the input filter, the `mp_*` actions, the `mp_ready` condition and the key handler.
2. A game built with `RS64_MULTIPLAYER=ON` already ran that registration from the built-ins and sets the flag `mp_builtin`. The mod reads it first, logs `built into this game; the multiplayer-native mod stays off`, sets `r2 = 1` and returns, so no hook answers twice.
3. The game's CMake builds the library (`mods/multiplayer-native/CMakeLists.txt`), so it uses the exe's CRT and toolchain and the same `lockstep_core.cpp` rounding (`-ffp-contract=off`). On Linux only `recomp_api_version` and `rs64_mod_init` are exported (version script, `--no-undefined`, `-Bsymbolic`).
4. The mod folder carries `mod.json` and `roguesq_menu.json` (copied from `mods/multiplayer`), staged by [tools/mods/stage_mods.cmake](../tools/mods/stage_mods.cmake); see [release-ci.md](release-ci.md#native-mods).
