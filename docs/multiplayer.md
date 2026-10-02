# Online co-op (ghost model)

Online co-op runs one game per player. Each instance flies its own player 1 in its own world and streams that state to the other, which poses a wingman NPC (the "imposter", see [AGENTS.md](../AGENTS.md#co-op-player-2-is-an-imposter-npc)) from it. The two worlds are independent: only a few things are shared on purpose (objectives, deaths and lives, picked-up upgrades, the mission result, cutscene skips). Env vars are in [debug-trace-env-vars.md](debug-trace-env-vars.md).

## What players see

- The host drives mission select and picks the mission; the client's screen follows. Each player then picks their own craft, and nobody launches until both have picked.
- Each player keeps the upgrades of the pilot profile they chose before entering MULTIPLAYER.
- An upgrade collected in a level is given to both players, and saved to both pilots if the mission succeeds (lost on failure, as in single player). The pickup disappears for the other player too, with its pickup sound.
- A cutscene skipped by one player is skipped for both.
- The other player's ship carries their pilot name (from the profile they chose), and shows on the radar as a purple dot.
- Lives are one shared pool. When the other player dies, WINGMATE DOWN appears and your lives counter ticks down; WINGMATE OUT when that took the last life, WINGMATE LEFT if they quit.
- A player who dies with no lives left spectates: their camera follows the survivor and SPECTATING is shown until the mission ends.
- The host's mission result (success, failure, abort) ends the mission for both.

## Code layout

| File | Role |
|---|---|
| [src/main/net_core.h](../src/main/net_core.h), [net_core.cpp](../src/main/net_core.cpp) | Message encoding, `Lobby` handshake state, `AddressEditor`, discovery packets, UPnP status text. No sockets. |
| [src/main/net_link.h](../src/main/net_link.h), [net_link.cpp](../src/main/net_link.cpp) | ENet6 transport (`lib/enet6`): `Link`, `DiscoveryResponder` / `DiscoveryFinder`, `PortMapper` (miniupnp). |
| [src/main/ghost.cpp](../src/main/ghost.cpp) | Session core: link events, message dispatch, per-frame hooks, shared objectives, deaths and lives, mission result, quit. |
| [src/main/ghost_lobby.cpp](../src/main/ghost_lobby.cpp) | Front-end lobby: host/join, discovery, address editing, mission select and craft select hooks, skip sharing. |
| [src/main/ghost_hud.cpp](../src/main/ghost_hud.cpp) | Co-op message line, name label, radar dot, puppet pose and S-foils. |
| [src/main/ghost_internal.h](../src/main/ghost_internal.h) | The `Ghost` state struct and helpers shared by the three files above. |
| [src/main/lockstep_core.cpp](../src/main/lockstep_core.cpp) | Pure logic with unit tests: `Puppet` interpolation, `SkipShare`, `CraftBarrier`, shared-life rules, NPC pool checks, the imposter's flight model. |
| [src/main/coop_imposter.cpp](../src/main/coop_imposter.cpp) | Imposter spawn and teardown. |
| [src/main/mp_register.cpp](../src/main/mp_register.cpp) | `rs64_mp_register`: the hooks and the quit handler (`rs64_ghost_quit`), then it calls `rs64_ghost_register_host` (in `ghost_lobby.cpp`), which registers the text sources, the input filter, the `mp_host` / `mp_join` / `mp_edit` / `mp_leave` actions, the `mp_ready` condition and the key handler. The built-ins call it when multiplayer is built in; the mod's `rs64_mod_init` calls it otherwise. |
| [src/main/mp_host.h](../src/main/mp_host.h) | The multiplayer sources' only route to the game: the host API table, wrapped (`rs64::mp::call`, `alloc`, `flag`, `add_hook`, ...). |
| [mods/multiplayer-native](../mods/multiplayer-native/) | The mod build: `mp_mod.cpp` (`rs64_mod_init`) plus the multiplayer sources and `lockstep_core.cpp`, built into `multiplayer_native.dll` / `.so`. |
| [mods/multiplayer](../mods/multiplayer/roguesq_menu.json) | The MULTIPLAYER menu page definition (the one source; the mod folder copies it). |

Base code sees multiplayer only through hooks, actions and flags.

### Flags

| Flag | Writer | Reader | Meaning |
|---|---|---|---|
| `ghost_mode` | `publish_modes` (`ghost_lobby.cpp`) | `lockstep.cpp` (mission init, per-frame) | Online co-op session active (`ROGUESQ_MP` or a menu-started session) |
| `coop_imposter` | `publish_modes` | `lockstep.cpp` (hash legend, pad2 replay, hash lines) | Player 2 is the imposter (`ROGUESQ_COOP_LOCAL` or ghost mode) |
| `mp_lobby_state` | `ghost_lobby.cpp` (on lobby state changes) | `nav_sequencer.cpp` | The `Lobby` handshake state |
| `text_entry` | `ghost_lobby.cpp` (per menu tick) | `main.cpp` `update_keyboard_shift` (Android) | The address editor is being typed in |
| `menu_page_shown` | `rs64_menu_pass` (`menu_config.cpp`) | `mp_register.cpp` `menu_pad` | A mod menu page is up |
| `mp_builtin` | `rs64_register_builtin_hooks` (`builtin_hooks.cpp`, ON builds) | `mp_mod.cpp` | Multiplayer is built into the exe; the mod stays off |
| `hangar_launch` | `skip_filter` (`ghost_lobby.cpp`) | `nav_sequencer.cpp` | The hangar is launching a mission after both players picked |

### Hook placement

Built-ins register first, then the multiplayer registration, so every multiplayer handler runs after the built-in on the same hook.

| Hook | Built-in part | Multiplayer part |
|---|---|---|
| 1 `MENU_INPUT` | `rs64_menu_pass`: sets `menu_page_shown`, dispatches hook 32, page row navigation, pilot-first exit | none (it uses hook 32) |
| 3 `MENU_FRAME` | `rs64_menu_frame`: menu period glyph, live labels | `menu_tick`: `rs64_lobby_tick` |
| 4 `MISSION_INIT` | `rs64_ls_on_mission_init`: lockstep mode, pad2 setup | `mission_init`: imposter reset, `rs64_ghost_mission_init` in ghost mode |
| 5 `MISSION_FRAME_DT` | `frame_dt`: fixed 1/30 s while a lockstep session is active | `frame_dt`: fixed 1/30 s for recording-driven ghost runs (not in demos) |
| 6 `MISSION_FRAME_PADS` | `rs64_ls_frame_pads`: pad2 replay | `frame_pads`: `rs64_ghost_frame` in ghost mode |
| 8 / 9 `SPEECH_RESPONSE_2` / `_1` | none (r2 holds the game's own speech-idle answer) | `speech_gate`: `rs64_ghost_gate_response` replaces r2 |
| 32 `MENU_PAD` | none (hook 1's built-in is the dispatch site, before page row navigation) | `menu_pad`: closes the address editor off the page, `rs64_lobby_input` takes the pad |
| 33 `MAIN_MENU` | none (`menu_config.cpp` dispatches it when the main menu is built) | `main_menu`: ends a menu-started session (`rs64_lobby_cancel`) |

Hooks 2 and 7 have only a built-in part; every other hook not listed above has only a multiplayer part.

Hooks the registration takes (sites and contracts in [modding-host-api.md](modding-host-api.md#hooks)):

| Area | Hooks |
|---|---|
| Front end | `MENU_FRAME`, `MENU_PAD`, `MAIN_MENU`, `MISSION_SELECT_INIT`, `MISSION_CONFIRMED`, `MISSION_SELECT_FONTS`, `MISSION_SELECT_TICK`, `CRAFT_SELECT_INIT`, `CRAFT_SELECT_TICK` |
| Mission frame | `MISSION_INIT`, `MISSION_FRAME_DT`, `MISSION_FRAME_PADS`, `MISSION_FRAME_NPCS`, `MISSION_END`, `CRAFT_ASSETS` |
| Imposter and streaming | `WINGMAN_TICK`, `NPC_ACTIVATION`, `GRID_STREAM` |
| Shared state | `TRANSITION_REQUEST`, `FREEZE_CHECK_A`, `FREEZE_CHECK_B`, `CUTSCENE_FREEZE_CHECK`, `OBJECTIVE_COUNT`, `RESULT_FAIL`, `RESULT_SUCCESS`, `SPEECH_RESPONSE_1`, `SPEECH_RESPONSE_2` |
| HUD | `HUD_FONTS`, `HUD_DRAW`, `RADAR` |

A new base-side need is a flag or a table entry, never a direct call.

## Build variants

`RS64_MULTIPLAYER` is ON by default (and forced ON on Android): the multiplayer sources are compiled into the exe and register through the built-ins; the data-only `multiplayer` mod supplies the menu page. Desktop releases build with it OFF: the exe has no multiplayer code and online co-op comes only from the `multiplayer-native` mod, which the game's CMake builds and stages (see [release-ci.md](release-ci.md#native-mods)). Local co-op (`ROGUESQ_COOP_LOCAL`) needs multiplayer present either way, so an OFF build needs that mod enabled.

Unit tests (`cmake --build build --config Release --target rs64_unit_tests` builds and runs all): `net_core_test`, `net_link_test`, `lockstep_core_test`, `nav_target_test`, `host_api_test` (the style checker runs too). `net_link_test` talks to the real router through UPnP.

## Modes and scripted runs

`ROGUESQ_MP=host|join` is the scripted path: the handshake starts at the first mission and `tools/lockstep/run-mp.ps1` drives a pair of instances. `run-mp.ps1 -Lobby` drives the real menus instead (`ROGUESQ_BOOT_TARGET=lobby:host|join`). Once the menus complete a handshake, ghost mode stays on for later missions without `ROGUESQ_MP`.

## Lobby flow

The lobby is the MULTIPLAYER entry added to the main menu by [mods/multiplayer](../mods/multiplayer/roguesq_menu.json): MAIN MENU, MULTIPLAYER, pilot select, then a page with HOST GAME and JOIN GAME. The lobby owns the link. `Lobby` (net_core) tracks Idle / Hosting / Joining / Connected / Failed: a join gives up after 15 s, a connected peer must say HELLO within 5 s, and a HELLO with a different `kBuildId` fails both sides (VERSION MISMATCH). A host whose player leaves goes back to waiting. The status line is menu-font-safe text (uppercase, digits, spaces). `rs64_lobby_status` (`ghost_lobby.cpp`, run each front-end frame by `rs64_lobby_tick`) builds the status text and publishes the lobby state as the `mp_lobby_state` flag; the nav sequencer reads that flag, not the text.

Link events reach the lobby through `link_poll_edges(up, was_up, connects, seen_connects)`. `Link::connect_count()` counts connects, so a connect that dropped before the poll is neither edge, and a reconnect inside one poll reports lost plus connected.

Once connected, the page leaves for mission select by itself. In a lobby session:

- Mission select: the client mirrors the host live. BROWSE carries the host's cursor and screen state, PICK its confirmed level; the client walks its cursor to the pick and confirms with injected presses. During the briefing, the client's A or START asks the host to end it (BRIEFING_DONE). The client can still back out with B, which ends the session.
- Craft select: neither player leaves until both picked. A/START records the pick and sends READY instead of confirming; once both are in, one A is injected until the hangar takes it. Backing out sends READY 0xFF and the other player follows. Level 0x10 forces the T-16 and skips craft select, so no READY comes.
- The puppet uses the other player's craft: the wingman mesh is loaded right after `choosePlayerCraftAssets` in `initMission`.

The hooks live in the menu overlay (`drawRadarIcon`, `tickCraftSelectScreen`, the level write after mission select, `fontAlloc` for the extra status-line slots); see the comments in `ghost_lobby.cpp` for addresses.

## Menu page layout

Mod menu pages are defined in `roguesq_menu.json` (format: [adding-menus-and-buttons.md](adding-menus-and-buttons.md)). Two fields exist for the multiplayer page:

- `title_y`: the page title's y offset (written at `gCurrentMenuData+0x52`).
- `row`: entries sharing a `row` string sit on one line (HOST GAME and JOIN GAME). `page_row_nav` in [menu_config.cpp](../src/main/menu_config.cpp) then moves the cursor by line (up/down, wrapping) and left/right within a line, remembering the column last used. It runs before the menu reads the pressed word and consumes the presses it handles. A line with a single entry leaves left/right to the entry (a slider's).

## LAN discovery

The host answers searches while it waits. Discovery is plain UDP on the game port + 1 (`kDiscoveryPortOffset`), outside ENet: JOIN broadcasts a query every 250 ms for 2 s (`kSearchMs`) and connects to the first host that answers (the reply carries the game port, the pilot name and the build). If nothing answers, it connects to the address the editor holds. A scripted `ROGUESQ_MP_ADDR` skips the search.

## Steam relay mod

[mods/steam-relay](../mods/steam-relay/) (desktop only, on by default) publishes the `mp.transport` service ([include/rs64/mp_transport.h](../include/rs64/mp_transport.h), see [modding-host-api.md](modding-host-api.md#services)). It runs the session over Steam's relay network as app id 480 (Spacewar), so nobody forwards a port or types an IP. The player supplies the Steamworks runtime library next to the mod (`steam_api64.dll`, `libsteam_api.so` or `libsteam_api.dylib`, from the SDK version the mod was built with); the mod loads it at run time and never links it.

- Multiplayer looks the service up on first use (`relay()` in `ghost_lobby.cpp`, after the registry seals) and uses it while `available()` is true; `ROGUESQ_MP_TRANSPORT=enet` ignores it. `Link` forwards to the table (`LinkConfig::external`), so the session code is unchanged; the simulated latency / loss / jitter apply to ENet only.
- Steam starts when the MULTIPLAYER page first reads the address row (`available()`), and shuts down (`relay_shutdown`) when the page is left without a session, when the main menu ends a session, and on quit after the BYE. STOP HOSTING / CANCEL JOIN keep it up.
- HOST GAME creates a public 2-player lobby tagged with a random 6-digit `code` and the build (`rs64r`), and listens with `CreateListenSocketP2P`; the address line shows `JOIN CODE <code>`. JOIN GAME searches worldwide for that code and build, joins the lobby, then `ConnectP2P`s to its owner. The address row holds the code (`CodeEditor`: typed digits, or the pad's up/down per digit).
- A Steam friends-list join or accepted invite while the game runs arrives as `GameLobbyJoinRequested_t` and is joined through `pending_join()`. With the game closed, Steam launches Spacewar instead.
- The online line shows the mod's status: while idle it says why Steam is not in use (`STEAM API DLL MISSING`, `STEAM API DLL WRONG VERSION`, `STEAM API DLL BLOCKED`, `STEAM IS NOT RUNNING`, `STEAM FLATPAK NOT SUPPORTED`), and HOST / JOIN then use ENet.

## Internet hosting (UPnP) and status lines

While hosting, `PortMapper` asks the router (UPnP) to map the game port. `ROGUESQ_MP_UPNP=0` leaves the router alone (test runs). `PortMapper::stop()` is non-blocking: it flags the worker and returns, and the worker removes the mapping in the background, before the next `start()`'s worker begins (`settled()` is true once it finished).

Status text comes from `net_core.cpp`:

| `online_label(state)` | Meaning |
|---|---|
| (empty) | Idle, or the port is open |
| OPENING INTERNET PORT | Mapping in progress |
| UPNP IS OFF ON YOUR ROUTER | No router answered |
| YOUR ROUTER REFUSED THE PORT | The router refused the mapping |
| YOUR ISP SHARES YOUR ADDRESS | The external address is not public (`public_ipv4`: private, carrier-grade NAT, loopback and link-local are not) |

`host_address_label` is the address line: `YOUR NET ADDRESS <ip>` once the router opened the port, else `YOUR LAN ADDRESS <ip>`, or `NO NETWORK ADDRESS`, with `:<port>` when the port is not 27064. Addresses are dotted; the menu font's period is added at runtime (see the last section).

## Address entry

The address row holds an `AddressEditor`, seeded from `roguesq_net.json` (`last_address`, `last_host`, `port`; path override `ROGUESQ_NET_CONFIG`). A on the row starts editing. JOIN connects to it and HOST GAME listens on it.

HOST GAME (`host_on_row` in `ghost_lobby.cpp`, choice in `choose_host_address`):

- The row is one of this machine's IPv4 addresses (or loopback): the host listens on that address only (`LinkConfig::bind_v4`), and UPnP searches for the router from it (none for loopback). Use this to pick a VPN (Tailscale, ZeroTier, Hamachi) or one adapter.
- Otherwise (the last JOIN target, a saved hostname): `last_host` if it is still local, else every address (IPv4 and IPv6) with the auto pick (`lan_ipv4`: default route, then `pick_lan_ipv4`). The row is replaced by the address in use.
- `0.0.0.0` asks for every address and keeps the row as typed.
- While hosting the row cannot be edited. STOP HOSTING or leaving the page puts the join address back.

A typed `:port` (keyboard only, 1-65535) applies to JOIN, HOST and the LAN search; without one, `port` (or `ROGUESQ_MP_PORT`) applies. A LAN-found host on another port is written to the row with its port.

- Pad: up/down step the octet (held: every 3 frames after 12, by 10 from 45), left/right move between octets, A or B ends editing. While editing, the editor takes the pad and clears the pressed word so the menu neither moves nor confirms.
- Keyboard (`typing`): digits, `.` and `:`, at most 21 characters, shown with a caret. `begin_typing` / `type_text` / `type_backspace` / `end_typing`; a typed address that parses replaces the octets. Editing starts in this mode. SDL text input is on exactly while typing (`ghost_lobby.cpp` calls `rs64::mp::text_input`, which reaches `SDL_StartTextInput` / `SDL_StopTextInput` in `host_api_game.cpp`; `main.cpp` passes `SDL_TEXTINPUT` and Backspace/Return to the lobby through `rs64::host::run_key_handlers`); on desktop and Android alike the pad only ends it. Presses from held keys (Backspace and Space are bound to B, Return to A) count as typing, and `swallow_frames` drops the A/START presses for a few frames after it ends so they do not reach the menu. On Android a keyboard hidden by the system ends typing.
- A saved address the octets cannot show (hostname, IPv6) is kept and joined as-is until the octets are edited. A hostname's `:port` is kept apart from it.

### Android keyboard

While typing on a phone, the picture slides up so the highlighted entry clears the on-screen keyboard. `MainActivity.imeFraction()` (the keyboard height as a fraction of the window) reaches the host as `rs64::android::ime_fraction()`; `update_keyboard_shift` in `main.cpp` computes the needed shift and stores it in `RT64::presentShiftY`, which `rt64_vi_renderer.cpp` applies when presenting. Touch taps add the same shift (`y += RT64::presentShiftY`) so they land where that spot is drawn unshifted. Android needs the `INTERNET` permission and `CHANGE_WIFI_MULTICAST_STATE`; `MainActivity` holds a multicast lock so the phone hears discovery broadcasts while hosting.

### Touch on mod pages

`rs64_menu_page_shown()` (menu_config.cpp) reports that a mod page is up. `touch_context()` in `main.cpp` then classifies the screen as a list menu, since the page's title lives in host RAM and the state classifier would say "unknown". Label entries (sub-type 13, `kSubtypeLabel` in `touch_menu.cpp`) are drawn and active but get no tap box.

## Session protocol

All messages ride the ENet link; `Msg` in net_core.h lists them. A session starts only once both sides said a matching HELLO. Per frame (`rs64_ghost_frame`, after the pad poll): remote messages in, the host's result applied, local state out, the remote pad written to port 1 for the imposter.

| Message | Direction / use |
|---|---|
| HELLO | Both: build id, pilot name (label over the ship). |
| MISSION, LAUNCH | Host picks the mission id first, then LAUNCH puts both on the same level and craft. `DAMAGE`, `OBJ` and `LIFE` carry the mission id. |
| PRESENCE | Entering or leaving a mission. A departed player's puppet is removed (`destroyNpcSlotByIndex`); a peer entering triggers a full health resync. |
| STATE | Unreliable, every frame: pose, velocity, pad buttons, down flag, S-foils (0 closed .. 250 open). |
| OBJ | Host to client: objective snapshot (booleans, counts, timers), sent on change for 5 frames and every 15. |
| DAMAGE | DAT item health drops (see Shared deaths). |
| LIFE, OUT | Client reports a death; host answers OUT when the shared lives are gone. |
| UPGRADES | Power-up bits a player picked up this mission (see Shared objectives, deaths and lives). |
| PICKUP | A power-up a player collected (DAT index); the other side collects its own copy. |
| RESULT | Host's mission result. |
| PICK, READY, BROWSE, BRIEFING_DONE | Lobby screens. |
| SKIP | Cutscene skip. |
| BYE | Quit or mismatch. |

`rs64_ghost_quit` (called from the quit path in `main.cpp` before `_Exit`) sends BYE (`Quit`) and flushes for up to 200 ms, so the other player is told at once instead of waiting for the connection timeout. The 200 ms wait is acceptable only on that exit path. `ROGUESQ_MP_TEST_QUIT` ends the host abruptly; with `ROGUESQ_MP_TEST_QUIT_CLEAN=1` it takes the menu quit path and sends BYE.

## Puppet interpolation

`Puppet` ([lockstep_core.cpp](../src/main/lockstep_core.cpp)) shows the remote player 50 ms behind the sender's clock by default (entity interpolation, `ROGUESQ_GHOST_INTERP_MS`; 0 selects dead reckoning). Remote states must be finite, inside a sane range and have roughly orthonormal axes before any of it reaches game code. A respawn, teleport or the client's start offset is treated as a jump, not motion. Measured on scripted pair runs at 0-200 ms latency with 5% loss and 40 ms jitter, 50 ms interpolation trails the real ship by latency + ~110 ms (the buffer plus the game's ~30 Hz send cadence) and is 4-6x smoother than dead reckoning whenever there is jitter. The pad goes idle when the peer stops sending (paused, results screen, stalled), so the puppet never keeps firing.

## Mission end and the host's result

The host's result ends the client's mission. On a client in session, `requestMissionTransitionMode` (0x800FB190) refuses the end modes except the host's: arg 3 = success, which leads to mode 3 and result 1; arg 2 = failure, mode 4 and result 2. The `setHudEnableBit8` / `setHudEnableBit4` names are misleading; they are the request bits for those modes. Abort and out-of-lives results are written directly, the way the game's abort does, and not while the pause menu is open. `requestSpeechResponseMode1/2` land the result only when it is the host's. The host announces its result as soon as it enters an end mode.

## Shared objectives, deaths and lives

- Objectives: the host streams its objective state and the client takes the newest snapshot before its level script runs each frame. On a client in session, `datItemSetObjectiveBooleanCount` drops local writes; the callers are death handlers and every death is mirrored into the host's world, which counts it once.
- Upgrades: a pickup ORs one bit into the settings word 0x80130B4C (`npcPowerUpUpdate` 0x800EBDB0-0x800EBE4C; `kUpgradeMask` 0x1FE00). `sync_upgrades` (ghost.cpp) takes the word's upgrade bits on the mission's first frame as the baseline (the pilot's own upgrades, never shared), sends bits gained after it as UPGRADES, and ORs the other player's into the word. Bits already shared either way are not sent again; a peer entering the mission gets them all again. The game's own commit then saves them to each pilot on success and reverts them otherwise.
- Pickups: power-ups are DAT items. `RS64_HOOK_POWERUP_COLLECT` (0x800EBD34) sends this player's collect as PICKUP (DAT index). The receiver adds that record to `pickups_forced`; `RS64_HOOK_POWERUP_TOUCH` (0x800EBD20) zeroes the touch test's squared distance for it, so the game runs its own pickup there (bonus count, upgrade bit, shrink and removal), with the sound (`play3DSoundEvent`, a1) moved to the local ship so it is heard. A pickup not spawned on the receiver (no ship near it) is collected when it spawns. A peer entering the mission late gets every PICKUP again. Pair test: `construction_yards_bonus.rec` (see tools/recordings/README.md).
- Shared deaths: level objects are DAT items. Each frame, every tracked item whose health dropped is sent as DAMAGE (a resync sends every item). The receiver applies the drop through the game's own damage path (`dealDamagetoDatItem` 0x800C7390: a hit for a spawned item, a stored-health cut for an unspawned one).
- Lives: one pool (`numLives`, 0x80130B10), owned by the host. A client never runs out by itself: it respawns and sends LIFE, and the host rules (`shared_life`: spend, out, game over, ignore). A player who loses the last shared life is out and spectates: the craft rides the survivor's pose, cannot steer, fire or die, and the survivor's lives counter is re-shown (HUD action 0xC). A message line (SPECTATING, WINGMATE DOWN, OUT, LEFT) and the other pilot's name label are mission-font slots 1-2 linked into the 2D overlay list (`runInMissionFrame` 0x800FAC70). The other player is a purple radar dot (palette index 15, unused by the radar's grey ramp and pure red, green and blue).

## Damage settle

The peer's damage is applied only after `kDamageSettleFrames` (30) frames into the mission and never during a cutscene (`rs64_state_in_cinematic`). A kill applied earlier runs the objective trigger (`triggerObjectiveCompleteWithVoiceLines`) on state that is not set up yet and faults. A late joiner's backlog waits the same way.

## NPC slots and the imposter spawn

The NPC slot table (0x80130BB0) has `kNpcSlots` = 0x800 entries of 8 bytes. `allocateNpcSlot` returns 0xFFFF when none is free, and `spawnNpcOfType` does not fail cleanly on an exhausted pool (it pops past its context stack or writes slot entry 0xFFFF). The imposter is therefore spawned only when `npc_pool_room` says both the slot table and the context stack have room; otherwise the spawn retries a few times and then the imposter is skipped for that mission. After a spawn, `imposter_spawn_ok` checks slot, context, block and mesh pointers before they are used. Slots are not stable across the two worlds, so nothing is keyed by NPC slot; the shared identity of an object is its DAT item.

## Cutscene skip sharing

A local skip is sent as SKIP. The other side pulses START once a cutscene plays (they may be ahead, still loading), for up to 10 s (`SkipShare::kWaitS`); a skip within 1 s of its own pulse is treated as an echo. Skippable scenes are a cutscene timeline (the crawl, in-mission cutscenes) and the hangar launch after both picked. `ROGUESQ_MP_TEST_NO_SKIP_SHARE=1` ignores the other player's skips and `ROGUESQ_NAV_WATCH_CUTSCENES=1` makes the nav sequencer sit through every cutscene; together (`run-mp.ps1 -ClientLate`) they model a late joiner.

## Menu font period

The italic menu font ("italic35") has no `.`, no period-wide blank and no caret. `ensure_menu_period` in menu_config.cpp builds them at runtime from the font's own `:` and `!` glyphs into unused umlaut slots, so address and status lines use them.
