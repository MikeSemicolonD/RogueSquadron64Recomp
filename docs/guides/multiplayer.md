# Online co-op (ghost model)

Online co-op runs one game per player. Each instance flies its own player 1 in its own world and streams that state to the other, which poses a wingman NPC (the "imposter", see [AGENTS.md](#the-imposter)) from it. The two worlds are independent: only a few things are shared on purpose (objectives, deaths and lives, picked-up upgrades, the mission result, cutscene skips). Env vars are in [debug-trace-env-vars.md](../engine/debug-trace-env-vars.md).

## What players see

- With Steam running (the steam-relay mod) or a relay server set up ([relay-server.md](relay-server.md)), FIND GAME lists open games of the same version; picking one joins it, no code needed.
- HOST SETTINGS names your game in that list and can give it a password; joining a game with one asks for it.
- The host drives mission select and picks the mission; the client's screen follows. Each player then picks their own craft, and nobody launches until both have picked.
- Each player keeps the upgrades of the pilot profile they chose before entering MULTIPLAYER.
- An upgrade collected in a level is given to both players, and saved to both pilots if the mission succeeds (lost on failure, as in single player). The pickup disappears for the other player too, with its pickup sound.
- A cutscene skipped by one player is skipped for both.
- A trigger area either player flies into advances the mission for both (the host runs it once, however many of you are inside).
- An AT-AT tripped by either player's tow cable falls in both worlds and counts once.
- The other player's ship carries their pilot name (from the profile they chose), and shows on the radar as a purple dot.
- Lives are one shared pool. When the other player dies, WINGMATE DOWN appears and your lives counter ticks down; WINGMATE OUT when that took the last life, WINGMATE LEFT if they quit.
- A player who dies with no lives left spectates: their camera follows the survivor and SPECTATING is shown until the mission ends.
- The host's mission result (success, failure, abort) ends the mission for both.

## Code layout

| File | Role |
|---|---|
| [mods/multiplayer-native/src/net_core.h](../../mods/multiplayer-native/src/net_core.h), [net_core.cpp](../../mods/multiplayer-native/src/net_core.cpp) | Message encoding, `Lobby` handshake state, `AddressEditor`, discovery packets, UPnP status text. No sockets. |
| [mods/multiplayer-native/src/net_link.h](../../mods/multiplayer-native/src/net_link.h), [net_link.cpp](../../mods/multiplayer-native/src/net_link.cpp) | ENet6 transport (`mods/multiplayer-native/lib/enet6`): `Link`, `DiscoveryResponder` / `DiscoveryFinder`, `PortMapper` (miniupnp). |
| [mods/multiplayer-native/src/relay_client.h](../../mods/multiplayer-native/src/relay_client.h), [relay_client.cpp](../../mods/multiplayer-native/src/relay_client.cpp) | `RelayClient`: the relay server transport (an `mp.transport` table over two `Link`s to an `rs64_relay`; protocol in [relay-server/src/relay_protocol.h](../../relay-server/src/relay_protocol.h)). |
| [mods/multiplayer-native/src/ghost.cpp](../../mods/multiplayer-native/src/ghost.cpp) | Session core: link events, message dispatch, per-frame hooks, shared objectives, deaths and lives, mission result, quit. |
| [mods/multiplayer-native/src/ghost_lobby.cpp](../../mods/multiplayer-native/src/ghost_lobby.cpp) | Front-end lobby: host/join, discovery, address editing, mission select and craft select hooks, skip sharing. |
| [mods/multiplayer-native/src/ghost_hud.cpp](../../mods/multiplayer-native/src/ghost_hud.cpp) | Co-op message line, name label, radar dot, puppet pose and S-foils. |
| [mods/multiplayer-native/src/ghost_internal.h](../../mods/multiplayer-native/src/ghost_internal.h) | The `Ghost` state struct and helpers shared by the three files above. |
| [mods/multiplayer-native/src/mp_core.cpp](../../mods/multiplayer-native/src/mp_core.cpp) | Pure logic with unit tests (`mp_core_test`): `Puppet` interpolation, `SkipShare`, `CraftBarrier`, shared-life rules, NPC pool checks, the imposter's flight model. |
| [mods/multiplayer-native/src/coop_imposter.cpp](../../mods/multiplayer-native/src/coop_imposter.cpp) | Imposter spawn and teardown. |
| [mods/multiplayer-native/src/mp_register.cpp](../../mods/multiplayer-native/src/mp_register.cpp) | `rs64_mp_register`: the hooks and the quit handler (`rs64_ghost_quit`), then it calls `rs64_ghost_register_host` (in `ghost_lobby.cpp`), which registers the text sources, the input filter, the `mp_host` / `mp_join` / `mp_edit` / `mp_leave` actions, the `mp_ready` condition and the key handler. The built-ins call it when multiplayer is built in; the mod's `rs64_mod_init` calls it otherwise. |
| [mods/multiplayer-native/src/mp_host.h](../../mods/multiplayer-native/src/mp_host.h) | The multiplayer sources' only route to the game: the host API table, wrapped (`rs64::mp::call`, `alloc`, `flag`, `add_hook`, ...). |
| [mods/multiplayer-native](../../mods/multiplayer-native/) | The mod: `src/` (`mp_mod.cpp` is `rs64_mod_init`) and `tests/`. Its CMakeLists builds `src/` plus the base files it lists (`lockstep_core.cpp` and three headers) into `multiplayer_native.dll` / `.so`; the style check rejects any other base include. |
| [mods/multiplayer-native/roguesq_menu.json](../../mods/multiplayer-native/roguesq_menu.json) | The MULTIPLAYER menu page definition. |

Base code sees multiplayer only through hooks, actions and flags.

### Flags

| Flag | Writer | Reader | Meaning |
|---|---|---|---|
| `ghost_mode` | `publish_modes` (`ghost_lobby.cpp`) | `lockstep.cpp` (mission init: record only, no hashes) | Online co-op session active (`ROGUESQ_MP` or a menu-started session) |
| `coop_imposter` | `publish_modes` | the `imposter` hash part's gate (`add_hash_part`), `mp_register.cpp` (pad2 replay) | Player 2 is the imposter (`ROGUESQ_COOP_LOCAL` or ghost mode) |
| `mp_lobby_state` | `ghost_lobby.cpp` (on lobby state changes) | `nav_sequencer.cpp` | The `Lobby` handshake state |
| `text_entry` | `ghost_lobby.cpp` (per menu tick) | `input_host.cpp` `update_keyboard_shift` (Android) | The address editor is being typed in |
| `menu_page_shown` | `rs64_menu_pass` (`menu_config.cpp`) | `mp_register.cpp` `menu_pad` | A mod menu page is up |
| `hangar_launch` | `skip_filter` (`ghost_lobby.cpp`) | `nav_sequencer.cpp` | The hangar is launching a mission after both players picked |
| `mp_trigger_for_peer` | `apply_remote_triggers` (`ghost.cpp`) | `hook_helpers.cpp` (mid-mission camera shots) | The host is applying the other player's trigger, so a personal camera shot goes to that player only |

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

Hooks the registration takes (sites and contracts in [modding-host-api.md](../engine/modding-host-api.md#hooks)):

| Area | Hooks |
|---|---|
| Front end | `MENU_FRAME`, `MENU_PAD`, `MAIN_MENU`, `MISSION_SELECT_INIT`, `MISSION_CONFIRMED`, `MISSION_SELECT_FONTS`, `MISSION_SELECT_TICK`, `CRAFT_SELECT_INIT`, `CRAFT_SELECT_TICK` |
| Mission frame | `MISSION_INIT`, `MISSION_FRAME_DT`, `MISSION_FRAME_PADS`, `MISSION_FRAME_NPCS`, `MISSION_END`, `CRAFT_ASSETS` |
| Imposter and streaming | `WINGMAN_TICK`, `NPC_ACTIVATION`, `GRID_STREAM` |
| Shared state | `TRANSITION_REQUEST`, `FREEZE_CHECK_A`, `FREEZE_CHECK_B`, `CUTSCENE_FREEZE_CHECK`, `OBJECTIVE_COUNT`, `RESULT_FAIL`, `RESULT_SUCCESS`, `SPEECH_RESPONSE_1`, `SPEECH_RESPONSE_2`, `TRIGGER_EFFECT`, `WALKER_TRIPPED` |
| HUD | `HUD_FONTS`, `HUD_DRAW`, `RADAR` |

A new base-side need is a flag or a table entry, never a direct call.

## Build variants

The game has no multiplayer code on any platform: online and local co-op come only from the `multiplayer-native` mod, which the game's CMake builds as a dependency of the exe and stages (see [release-ci.md](../engine/release-ci.md#the-multiplayer-mod)). On Android, Gradle packs it as `libmultiplayer_native.so` with the game's libraries, `bundleMods` ships the mod's `mod.json` and menu, and librecomp loads the library by that name when the mod folder has none. Local co-op (`ROGUESQ_COOP_LOCAL`) needs the mod enabled.

Unit tests (`cmake --build build --config Release --target rs64_unit_tests` builds and runs all): the mod's `mp_core_test`, `net_core_test`, `net_link_test` and `relay_client_test` (`mods/multiplayer-native/tests`) next to the base tests (the style checker runs too). `net_link_test` talks to the real router through UPnP; `relay_client_test` runs an `rs64_relay` in-process on UDP 27181 and plays host, joiner and lister against it.

## Modes and scripted runs

`ROGUESQ_MP=host|join` is the scripted path: the handshake starts at the first mission and `tools/debug/lockstep/run-mp.ps1` drives a pair of instances. `run-mp.ps1 -Lobby` drives the real menus instead (`ROGUESQ_BOOT_TARGET=lobby:host|join`). Once the menus complete a handshake, ghost mode stays on for later missions without `ROGUESQ_MP`.

## Lobby flow

The lobby is the MULTIPLAYER entry added to the main menu by [mods/multiplayer-native/roguesq_menu.json](../../mods/multiplayer-native/roguesq_menu.json): MAIN MENU, MULTIPLAYER, pilot select, then a page with HOST GAME and JOIN GAME. The lobby owns the link. `Lobby` (net_core) tracks Idle / Hosting / Joining / Connected / Failed: a join gives up after 15 s (COULD NOT CONNECT TO HOST; at once, with the transport's reason, when the link or relay reports it cannot connect), a connected peer must say HELLO within 5 s, and a HELLO with a different `kBuildId` fails both sides (VERSION MISMATCH). A host whose player leaves goes back to waiting. The status line is menu-font-safe text (uppercase, digits, spaces). `rs64_lobby_status` (`ghost_lobby.cpp`, run each front-end frame by `rs64_lobby_tick`) builds the status text and publishes the lobby state as the `mp_lobby_state` flag; the nav sequencer reads that flag, not the text.

Link events reach the lobby through `link_poll_edges(up, was_up, connects, seen_connects)`. `Link::connect_count()` counts connects, so a connect that dropped before the poll is neither edge, and a reconnect inside one poll reports lost plus connected.

Once connected, the page leaves for mission select by itself. In a lobby session:

- Mission select: the client mirrors the host live. BROWSE carries the host's cursor and screen state, PICK its confirmed level; the client walks its cursor to the pick and confirms with injected presses. During the briefing, the client's A or START asks the host to end it (BRIEFING_DONE). The client can still back out with B, which ends the session.
- Craft select: neither player leaves until both picked. A/START records the pick and sends READY instead of confirming; once both are in, one A is injected until the hangar takes it. Backing out sends READY 0xFF and the other player follows. Level 0x10 forces the T-16 and skips craft select, so no READY comes.
- The puppet uses the other player's craft: the wingman mesh is loaded right after `choosePlayerCraftAssets` in `initMission`.

The hooks live in the menu overlay (`drawRadarIcon`, `tickCraftSelectScreen`, the level write after mission select, `fontAlloc` for the extra status-line slots); see the comments in `ghost_lobby.cpp` for addresses.

## Menu page layout

Mod menu pages are defined in `roguesq_menu.json` (format: [adding-menus-and-buttons.md](adding-menus-and-buttons.md)). Two fields exist for the multiplayer page:

- `title_y`: the page title's y offset (written at `gCurrentMenuData+0x52`).
- `row`: entries sharing a `row` string sit on one line (JOIN GAME and HOST GAME). `page_row_nav` in [menu_config.cpp](../../src/game/menu_config.cpp) then moves the cursor by line (up/down, wrapping) and left/right within a line, remembering the column last used. It runs before the menu reads the pressed word and consumes the presses it handles. A line with a single entry leaves left/right to the entry (a slider's).

## LAN discovery

The host answers searches while it waits. Discovery is plain UDP on the game port + 1 (`kDiscoveryPortOffset`), outside ENet: JOIN broadcasts a query every 250 ms for 2 s (`kSearchMs`) and connects to the first host that answers (the reply carries the game port, the pilot name and the build). If nothing answers, it connects to the address the editor holds. A scripted `ROGUESQ_MP_ADDR` skips the search.

## Steam relay mod

[mods/steam-relay](../../mods/steam-relay/) (desktop only, off by default) publishes the `mp.transport` service ([include/rs64/mp_transport.h](../../include/rs64/mp_transport.h), see [modding-host-api.md](../engine/modding-host-api.md#services)). It runs the session over Steam's relay network as app id 480 (Spacewar), so nobody forwards a port or types an IP. The player supplies the Steamworks runtime library next to the mod (`steam_api64.dll`, `libsteam_api.so` or `libsteam_api.dylib`, from the SDK version the mod was built with); the mod loads it at run time and never links it.

- Multiplayer looks the service up on first use (`relay()` in `ghost_lobby.cpp`, after the registry seals) and uses it while `available()` is true; `ROGUESQ_MP_TRANSPORT=enet` ignores it. `Link` forwards to the table (`LinkConfig::external`), so the session code is unchanged; the simulated latency / loss / jitter apply to ENet only.
- Steam starts when the MULTIPLAYER page first reads the address row (`available()`), and shuts down (`relay_shutdown`) when the page is left without a session, when the main menu ends a session, and on quit after the BYE. STOP HOSTING / CANCEL JOIN keep it up.
- HOST GAME creates a public 2-player lobby tagged with a random 6-digit `code` and the build (`rs64r`), and listens with `CreateListenSocketP2P`; the address line shows `JOIN CODE <code>`. JOIN GAME searches worldwide for that code and build, joins the lobby, then `ConnectP2P`s to its owner. The address row holds the code (`CodeEditor`: typed digits, or the pad's up/down per digit).
- A Steam friends-list join or accepted invite while the game runs arrives as `GameLobbyJoinRequested_t` and is joined through `pending_join()`. With the game closed, Steam launches Spacewar instead.
- The online line shows the mod's status: while idle it says why Steam is not in use (`STEAM API DLL MISSING`, `STEAM API DLL WRONG VERSION`, `STEAM API DLL BLOCKED`, `STEAM IS NOT RUNNING`, `STEAM FLATPAK NOT SUPPORTED`), and HOST / JOIN then use ENet.

## FIND GAME (game list)

A transport with the version 2 table (`RS64_MP_TRANSPORT_HAS_LIST`; the Steam mod has it) adds FIND GAME to the MULTIPLAYER page (`requires: mp_list`, true while that transport is available). Opening it searches every listing relay (`refresh(build)`), and the page searches again every 5 s while it is up (`list_update`, run by its status line), which doubles as the relay server's heartbeat. The rows show a snapshot of the last finished search (`s_listed`: name, code, lock, transport), swapped in only when every search is done, so they never blank or shift mid-search and a pick joins exactly the row shown; only the first search after opening shows SEARCHING. The page shows a status line (`list_status`: SEARCHING, NO GAMES FOUND, `<n> GAMES FOUND  PAGE x OF y`), four game rows (`LOCKED` after a game with a password), the PASSWORD row this player sends when joining, and NEXT PAGE (more pages) or REFRESH (searches now; the page stays on the current list until the new one arrives). Picking a row joins that game (`game_peer`, passed to `start()` like a join code); a join already in progress gives way, a host does nothing. Picking a locked game with no password typed opens the PASSWORD row instead (ENTER THE PASSWORD); pick it again once typed. A failed list join's reason stays on the status line until the page is opened again. Once connected, the MULTIPLAYER page's `exit_when` takes this page (or HOST SETTINGS) on to mission select too (`menu_config.cpp` checks the pilot page's whole page chain).

- Steam: `RequestLobbyList` with the build key (`rs64r`), at least one free slot, worldwide, at most 50; our own lobby is skipped. A host publishes `name` and `locked` (`1` with a password) as lobby data through `set_info`; names are cut to menu-font text (A-Z, 0-9, space, 16 characters), with STEAM GAME for none.
- A game in the list stays joinable by its code too.

## Relay server transport

With `relay_server` in `roguesq_net.json` (`"host"` or `"host:port"`, default port 27070; `ROGUESQ_MP_RELAY` overrides it), `server_relay()` in `ghost_lobby.cpp` offers a second relay transport: `RelayClient` ([relay_client.cpp](../../mods/multiplayer-native/src/relay_client.cpp)) over an `rs64_relay` ([relay-server.md](relay-server.md)).

- Precedence: HOST GAME and a typed join code use Steam when it is available, else the relay server (`relay_ready()`). FIND GAME lists Steam's games first, then the server's, and a picked game joins over the transport it came from (`relay_t`). `pending_join` (friends-list joins) is Steam's only.
- A game's id on the server is a random 6-digit number and is its join code: the host's line shows `JOIN CODE <id>`, and a code join sends `JOIN(id, build)`.
- The client keeps one `Link` to the server for the session (REGISTER or JOIN once connected; Data packets carry the game's messages) and a second one only while fetching the list (LIST, reply, hang up; 5 s timeout). The server's refusals and close reasons (`Link::close_data`, the ENet disconnect data) become the status line: GAME FULL, NO GAME WITH THAT CODE, VERSION MISMATCH, RELAY SERVER FULL, RELAY SERVER CLOSED, RELAY SERVER LOST.
- A server that does not answer: `available()` is false once a background check (started by the first `available()` call, which the MULTIPLAYER page makes) or a connection gets no answer in 5 s, and the server is checked again every 5 s (on FIND GAME, its 5 s refresh does the same). Joining fails at once with COULD NOT CONNECT TO SERVER (`Lobby::on_join_failed`, from the transport's `lost()` and status), and the code row falls back to the address row. HOST GAME falls back to hosting directly (`rs64_lobby_tick`: a relay host that is lost and unavailable before anyone joined restarts as a direct host), with no message. FIND GAME stays on the MULTIPLAYER page while a server is configured and says COULD NOT CONNECT TO SERVER over whatever Steam lists.

## HOST SETTINGS and passwords

HOST SETTINGS (a page off MULTIPLAYER) holds SERVER NAME (`NameEditor`: typed, or the pad's up/down per character; empty = the pilot's name), PASSWORD (`CodeEditor`, 6 digits; NONE until set) and REMOVE PASSWORD. Both are saved to `roguesq_net.json` (`host_name`, `host_password`) and, while hosting, apply at once: the password to `Lobby::set_password`, the name and lock to the relay (`sync_host_settings` in `ghost_lobby.cpp`).

The host checks the password; relays and servers only know a game is locked:

- On connect the client sends HELLO, then AUTH with its password (empty when none was typed). The host's `Lobby` is Connected after the client's matching HELLO and an AUTH with its password (any, without one), and then sends ACCEPT; the client is Connected on the host's HELLO plus ACCEPT.
- A wrong or missing password: the host sends BYE `WrongPassword` and goes back to waiting; the client fails with WRONG PASSWORD or PASSWORD NEEDED and sets `pin_mode`, so the MULTIPLAYER page's address row becomes the password (A to type it) until the next JOIN GAME, which sends it. FIND GAME's PASSWORD row is the same password.
- A refused client is trusted to disconnect on that BYE; one that does not keeps the host's single slot until it leaves.

## Internet hosting (UPnP)

While hosting, `PortMapper` asks the router (UPnP) to map the game port. Players are never shown how that went: if the router allows it, it just works and the address line turns into the NET address; if not, the LAN address stays. `ROGUESQ_MP_UPNP=0` leaves the router alone (test runs). `PortMapper::stop()` is non-blocking: it flags the worker and returns, and the worker removes the mapping in the background, before the next `start()`'s worker begins (`settled()` is true once it finished).

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
- Keyboard (`typing`): digits, `.` and `:`, at most 21 characters, shown with a caret. `begin_typing` / `type_text` / `type_backspace` / `end_typing`; a typed address that parses replaces the octets. Editing starts in this mode. SDL text input is on exactly while typing (`ghost_lobby.cpp` calls `rs64::mp::text_input`, which reaches `SDL_StartTextInput` / `SDL_StopTextInput` in `host_api_game.cpp`; `input_host.cpp` passes `SDL_TEXTINPUT` and Backspace/Return to the lobby through `rs64::host::run_key_handlers`); on desktop and Android alike the pad only ends it. Presses from held keys (Backspace and Space are bound to B, Return to A) count as typing, and `swallow_frames` drops the A/START presses for a few frames after it ends so they do not reach the menu. On Android a keyboard hidden by the system ends typing.
- A saved address the octets cannot show (hostname, IPv6) is kept and joined as-is until the octets are edited. A hostname's `:port` is kept apart from it.

### Android keyboard

While typing on a phone, the picture slides up so the highlighted entry clears the on-screen keyboard. `MainActivity.imeFraction()` (the keyboard height as a fraction of the window) reaches the host as `rs64::android::ime_fraction()`; `update_keyboard_shift` in `input_host.cpp` computes the needed shift and stores it in `RT64::presentShiftY`, which `rt64_vi_renderer.cpp` applies when presenting. Touch taps add the same shift (`y += RT64::presentShiftY`) so they land where that spot is drawn unshifted. Android needs the `INTERNET` permission and `CHANGE_WIFI_MULTICAST_STATE`; `MainActivity` holds a multicast lock so the phone hears discovery broadcasts while hosting.

### Touch on mod pages

`rs64_menu_page_shown()` (menu_config.cpp) reports that a mod page is up. `touch_context()` in `input_host.cpp` then classifies the screen as a list menu, since the page's title lives in host RAM and the state classifier would say "unknown". Label entries (sub-type 13, `kSubtypeLabel` in `touch_menu.cpp`) are drawn and active but get no tap box.

## Session protocol

All messages ride the ENet link; `Msg` in net_core.h lists them. A session starts only once both sides said a matching HELLO and the host accepted the client's AUTH (see [HOST SETTINGS and passwords](#host-settings-and-passwords)). Per frame (`rs64_ghost_frame`, after the pad poll): remote messages in, the host's result applied, local state out, the remote pad written to port 1 for the imposter.

| Message | Direction / use |
|---|---|
| HELLO | Both: build id, pilot name (label over the ship). |
| AUTH, ACCEPT | Client to host right after HELLO: the password typed (empty: none). Host to client: accepted. |
| MISSION, LAUNCH | Host picks the mission id first, then LAUNCH puts both on the same level and craft. `DAMAGE`, `OBJ` and `LIFE` carry the mission id. |
| PRESENCE | Entering or leaving a mission. A departed player's puppet is removed (`destroyNpcSlotByIndex`); a peer entering triggers a full health resync. |
| STATE | Unreliable, every frame: pose, velocity, pad buttons, down flag, S-foils (0 closed .. 250 open). |
| OBJ | Host to client: objective snapshot (booleans, counts, timers), sent on change for 5 frames and every 15. |
| DAMAGE | DAT item health drops (see Shared deaths). |
| LIFE, OUT | Client reports a death; host answers OUT when the shared lives are gone. |
| UPGRADES | Power-up bits a player picked up this mission (see Shared objectives, deaths and lives). |
| PICKUP | A power-up a player collected (DAT index); the other side collects its own copy. |
| TRIGGER | Client to host: the client's craft entered or left a player trigger volume (DAT event index). |
| TOW_TRIP | A player's tow cable tripped a walker (DAT index); the other side trips its own copy. |
| SHOT | Host to client: a mod-linked mid-mission camera shot for the client (camera item and trigger names; a shared one the host also plays, or a personal one the client set off); the client plays it through the `camera.shots` service unless that trigger already fired in its own world. |
| RESULT | Host's mission result. |
| PICK, READY, BROWSE, BRIEFING_DONE | Lobby screens. |
| SKIP | Cutscene skip. |
| BYE | Quit, mismatch, or the host refusing a password (`WrongPassword`). |

`rs64_ghost_quit` (called from the quit path in `input_host.cpp` before `_Exit`) sends BYE (`Quit`) and flushes for up to 200 ms, so the other player is told at once instead of waiting for the connection timeout. The 200 ms wait is acceptable only on that exit path. `ROGUESQ_MP_TEST_QUIT` ends the host abruptly; with `ROGUESQ_MP_TEST_QUIT_CLEAN=1` it takes the menu quit path and sends BYE.

## Puppet interpolation

`Puppet` ([mp_core.cpp](../../mods/multiplayer-native/src/mp_core.cpp)) shows the remote player 50 ms behind the sender's clock by default (entity interpolation, `ROGUESQ_GHOST_INTERP_MS`; 0 selects dead reckoning). Remote states must be finite, inside a sane range and have roughly orthonormal axes before any of it reaches game code. A respawn, teleport or the client's start offset is treated as a jump, not motion. Measured on scripted pair runs at 0-200 ms latency with 5% loss and 40 ms jitter, 50 ms interpolation trails the real ship by latency + ~110 ms (the buffer plus the game's ~30 Hz send cadence) and is 4-6x smoother than dead reckoning whenever there is jitter. The pad goes idle when the peer stops sending (paused, results screen, stalled), so the puppet never keeps firing.

## Mission end and the host's result

The host's result ends the client's mission. On a client in session, `requestMissionTransitionMode` (0x800FB190) refuses the end modes except the host's: arg 3 = success, which leads to mode 3 and result 1; arg 2 = failure, mode 4 and result 2. The `setHudEnableBit8` / `setHudEnableBit4` names are misleading; they are the request bits for those modes. Abort and out-of-lives results are written directly, the way the game's abort does, and not while the pause menu is open. `requestSpeechResponseMode1/2` land the result only when it is the host's. The host announces its result as soon as it enters an end mode.

## Shared objectives, deaths and lives

- Objectives: the host streams its objective state and the client takes the newest snapshot before its level script runs each frame. On a client in session, `datItemSetObjectiveBooleanCount` drops local writes; the callers are death handlers and every death is mirrored into the host's world, which counts it once.
- Upgrades: a pickup ORs one bit into the settings word 0x80130B4C (`npcPowerUpUpdate` 0x800EBDB0-0x800EBE4C; `kUpgradeMask` 0x1FE00). `sync_upgrades` (ghost.cpp) takes the word's upgrade bits on the mission's first frame as the baseline (the pilot's own upgrades, never shared), sends bits gained after it as UPGRADES, and ORs the other player's into the word. Bits already shared either way are not sent again; a peer entering the mission gets them all again. The game's own commit then saves them to each pilot on success and reverts them otherwise.
- Pickups: power-ups are DAT items. `RS64_HOOK_POWERUP_COLLECT` (0x800EBD34) sends this player's collect as PICKUP (DAT index). The receiver adds that record to `pickups_forced`; `RS64_HOOK_POWERUP_TOUCH` (0x800EBD20) zeroes the touch test's squared distance for it, so the game runs its own pickup there (bonus count, upgrade bit, shrink and removal), with the sound (`play3DSoundEvent`, a1) moved to the local ship so it is heard. A pickup not spawned on the receiver (no ship near it) is collected when it spawns. A peer entering the mission late gets every PICKUP again. Pair test: `construction_yards_bonus.rec` (see tools/debug/recordings/README.md).
- Shared deaths: level objects are DAT items. Each frame, every tracked item whose health dropped is sent as DAMAGE (a resync sends every item). The receiver applies the drop through the game's own damage path (`dealDamagetoDatItem` 0x800C7390: a hit for a spawned item, a stored-health cut for an unspawned one).
- Trigger volumes: `ply` DAT events live in player 1's trigger list only (0x80137DF0); `applyDatObjectiveTriggerEffect` (0x80065980) writes the objective state directly, not through `datItemSetObjectiveBooleanCount`. `RS64_HOOK_TRIGGER_EFFECT` makes a client send its edges as TRIGGER and drop them; the host applies them through the same function with `TeamTriggers` (mp_core): one effect on the first enter and on the last exit. Test: `run-mp.ps1 -TestTriggerSolo` on the level 3 recording.
- Tow-cable trips: `RS64_HOOK_WALKER_TRIPPED` (0x800CED7C, an AT-AT accepting the harpoon's action 9 kind 0xE) sends TOW_TRIP; the receiver sends its copy the same message (sender slot 0xFFFF, so the release reply goes nowhere), and its fall kills it and counts the objective. Mirrored DAMAGE skips a tripped walker (DAT item +0x4C bit 31): AT-ATs take x0.1 from a hit with no hit part, and a hit on a tripped one skips the objective count. The trip camera and the cable are seen only by the player who towed.
- Lives: one pool (`numLives`, 0x80130B10), owned by the host. A client never runs out by itself: it respawns and sends LIFE, and the host rules (`shared_life`: spend, out, game over, ignore). A player who loses the last shared life is out and spectates: the craft rides the survivor's pose, cannot steer, fire or die, and the survivor's lives counter is re-shown (HUD action 0xC). A message line (SPECTATING, WINGMATE DOWN, OUT, LEFT) and the other pilot's name label are mission-font slots 1-2 linked into the 2D overlay list (`runInMissionFrame` 0x800FAC70). The other player is a purple radar dot (palette index 15, unused by the radar's grey ramp and pure red, green and blue).

## Damage settle

The peer's damage is applied only after `kDamageSettleFrames` (30) frames into the mission and never during a cutscene (`rs64_state_in_cinematic`). A kill applied earlier runs the objective trigger (`triggerObjectiveCompleteWithVoiceLines`) on state that is not set up yet and faults. A late joiner's backlog waits the same way.

## NPC slots and the imposter spawn

The NPC slot table (0x80130BB0) has `kNpcSlots` = 0x800 entries of 8 bytes. `allocateNpcSlot` returns 0xFFFF when none is free, and `spawnNpcOfType` does not fail cleanly on an exhausted pool (it pops past its context stack or writes slot entry 0xFFFF). The imposter is therefore spawned only when `npc_pool_room` says both the slot table and the context stack have room; otherwise the spawn retries a few times and then the imposter is skipped for that mission. After a spawn, `imposter_spawn_ok` checks slot, context, block and mesh pointers before they are used. Slots are not stable across the two worlds, so nothing is keyed by NPC slot; the shared identity of an object is its DAT item.

## Cutscene skip sharing

A local skip is sent as SKIP, tagged with the scene it was made in: a phase (both sides start one at craft select and at mission init) and the number of skippable scenes entered in that phase. The other side pulses START when it plays that scene (it may be behind, still loading), and drops a skip for a scene it already finished; a skip within 1 s of its own pulse is treated as an echo. Skippable scenes are a cutscene timeline (the crawl, in-mission cutscenes) and the hangar launch after both picked. `ROGUESQ_MP_TEST_NO_SKIP_SHARE=1` ignores the other player's skips and `ROGUESQ_NAV_WATCH_CUTSCENES=1` makes the nav sequencer sit through every cutscene; together (`run-mp.ps1 -ClientLate`) they model a late joiner.

## Menu font period

The italic menu font ("italic35") has no `.`, no period-wide blank and no caret. `ensure_menu_period` in menu_config.cpp builds them at runtime from the font's own `:` and `!` glyphs into the `&`, `"` and `/` slots (no language's menu text uses them; German needs the umlauts), so address and status lines use them.

## The imposter

`ROGUESQ_COOP_LOCAL=1` (or ghost mode) spawns player 2 as a level-wingman NPC (`npcWingmanUpdate` 0x800D93C0) from a fake DAT record at 0x80B40000 with no flight path, so the wingman AI idles and [coop_imposter.cpp](../../mods/multiplayer-native/src/coop_imposter.cpp) flies it from port 1 (hook at the handler's post-state join 0x800D9A00, action 3 only). The spawn must be followed by one `slotDispatcherIter(slot, 3, &dt)` tick or the NPC never enters its tick chains. There is no gPlayers[1], view[1] or second camera, and local split-screen is out of scope.

- Hooks call recompiled functions only through `mips_call` (saves the context, runs the callee below the hook's stack frame).
- The flight model in `mp_core.cpp` is a port of the player X-wing's handling (`updateXwingFlightControls` 0x800B4588) and uses polynomial sin/cos with FP contraction off so every platform rounds the same.
- The NPC health patch (`patches/src/npc_health.c`) accepts the record's health slot in host RAM because the mod registers it with the host API's `add_valid_ram`; its lockstep hash column is an `add_hash_part`.
- The wingman dereferences its mesh during the spawn, so the record's model index (+0x8C) is the first wingman model the level loaded (`wingman_model`, a side-effect-free copy of `walkMeshdef0List`'s name lookup); with none, the imposter is skipped.

## Per-player data relocation

While the mod is enabled, four ranges of per-player arrays live in host RAM so index 1 fits (`kMovedRanges` in `mp_core.h`): R1 0x80138058-0x80138267 (`gObjectiveCounts` 0x80138060 at 0x80B38060), R2 0x80138268-0x80138837 (camera node[0]) and R3 0x80138E5C-0x8013901F (`txtFileHeader` .. `voiceTxtString`) at +0x00A00000, and R4 0x80138930-0x80138D0F (a per-view 4 x 0xF8 block) at +0x00A20000.

- Only `lui` immediates change (`0x8013`/`0x8014` -> `0x80B3`..`0x80B6`), 200 sites in 107 functions, as `[[patches.instruction]]` entries at the end of the mod's `mod.toml`. At build time RecompModTool turns them and the manifest into the mod folder's `instruction_patches.json` and `mod.json` (mod id `multiplayer_native`).
- librecomp reads that file from any enabled mod (folder or `.nrm`), checks each site's original word (`expect`), refuses conflicts and replacements of patched functions, and regenerates those functions with LiveRecomp (logs `Mod instruction patches: N in M functions`).
- Regenerate the entries with `python mods/multiplayer-native/tools/gen_relocation.py` (reads an unpatched `RecompiledFuncs`, rewrites `mod.toml` below its marker line) after anything that changes the recompiled code of those functions; ambiguous sites are settled in `tools/relocation_resolve.txt`.
- The mod registers the ranges with the host API's `add_moved_range`, so host code goes through `rs64::ls::relocated()` (identity while the mod is off). `ROGUESQ_RELOC_GUARD=1` reports writes to the old addresses.
- RDRAM diffs against PJ64 goldens see these fields as zero at their old addresses while the mod is on. `.nrm` mods link against the plain `syms/rogue_squadron.datasyms.toml`; one that reads these fields while multiplayer may be on translates the address with the `rs64_relocated` export.
