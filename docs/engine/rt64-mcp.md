# RT64 MCP: live debugging from an agent

An MCP server ([tools/debug/mcp/rs64_mcp.py](../../tools/debug/mcp/rs64_mcp.py)) that launches the game, freezes it on an exact frame and gives an agent what the F1 inspector shows: draw calls, textures, framebuffers, the free camera, RT light presets and frame times. The full tool list is in [tools/debug/mcp/README.md](../../tools/debug/mcp/README.md).

## Setup

1. Install Python 3 and the `mcp` package: `pip install mcp`.
2. Build a config that has the bridge: Debug has it by default (see [Build options](#build-options)).
3. Add the server to `.mcp.json` in the repo root (gitignored, one per machine). The path is relative to the repo root, so start the agent from there:

   ```json
   {
     "mcpServers": {
       "rs64": {
         "type": "stdio",
         "command": "python",
         "args": ["tools/debug/mcp/rs64_mcp.py"]
       }
     }
   }
   ```

   If `.mcp.json` already lists other servers, add the `rs64` entry to its `mcpServers`. Use `python3` or a full interpreter path as `command` if `python` isn't the one with `mcp` installed.
4. Check it: `python tools/debug/mcp/rs64_mcp.py --self-test` launches the Debug build and tests the bridge end to end.

## Build options

The CMake option `RS64_MCP` controls the bridge:

| Value | Configs with the bridge |
| --- | --- |
| `AUTO` (default) | Debug only |
| `ON` | Every config (VS Code task "Build: RogueSquadron64Recomp (Release, MCP)") |
| `OFF` | None |

Release builds, and so every shipped build, have none of it.

## How it fits together

- The server starts the exe with `--mcp-port <n>`. The game opens a loopback socket there (127.0.0.1, one client, newline-delimited JSON: `{"id", "method", "params"}` in, `{"id", "ok", "result" | "error"}` out).
- A Debug build started by hand serves port 47064 (`ROGUESQ_MCP_PORT=0` turns it off), so `attach` can connect to it. Other builds open no port without the flag.
- Host methods (pause, step, RDRAM, RenderDoc) are served by [src/main/mcp_bridge.cpp](../../src/main/mcp_bridge.cpp) and [src/lockstep/mcp_pause.cpp](../../src/lockstep/mcp_pause.cpp).
- RT64 methods are queued to [lib/rt64/src/hle/rt64_rs64_mcp.cpp](../../lib/rt64/src/hle/rt64_rs64_mcp.cpp) and run on the gfx thread at the top of `State::updateScreen`, which runs every VI, paused or not, with F1 open or closed.
- `launch` runs the game from `build/<config>` with RenderDoc injected, audio muted and dev mode on; `close` kills it. Output (stdout/stderr, dumps, screenshots) lands in `dumps/mcp/<session>/`.
- `attach(port)` connects to a game the user started instead. Every tool works on it except `wait_log` (its console is not captured), and `close` only disconnects.
- Co-op pairs: every tool takes `instance` (default `p1`). `launch(coop="host")` and then `launch(coop="join", instance="p2")` start a pair, the second in its own copy of the game folder ([README](../../tools/debug/mcp/README.md#two-games-at-once)). `tools/debug/lockstep/run-mp.ps1` serves the host's bridge on 47065 and the client's on 47066 (Debug builds), so `attach(47065)` and `attach(47066, instance="p2")` drive both.
- Holding one game of a pair does not stall the other: co-op is the ghost model, and the link runs on its own thread. The one exception is the mission-start handshake, where a hold longer than 10 s drops the other game to solo, so wait for `[ghost] peer in mission` on both first.

## The pause model

RT64's own pause (F4) only freezes the picture: it drops new display lists while the game keeps running. `pause_at` freezes both:

1. The game thread holds at the first pause point after a display list from frame N was sent. The clocks are the same as `ROGUESQ_PAUSE_AT`: game states, `cutscene`, `crawl`, plus `dl` (display lists since boot). In missions and menus one display list follows each frame's pause point, so the picture is exactly frame N. Cutscene and crawl clocks tick slower than display lists, so there it is the first display list at or past N.
2. RT64 then pauses and redraws that frame every VI.

Clocks:

- `ingame` counts mission frames while recording or replaying (`ROGUESQ_LS_RECORD`, `ROGUESQ_LS_REPLAY`); these are the recording's frame numbers. With `ROGUESQ_LS_EACH_MISSION=1` it starts over at every mission. `ls` is still accepted as its old name.
- The overlay's `Clocks` line shows one clock: the cutscene (with its level and intro / outro / special) or crawl when one plays, plus `ingame` in a recording or replay, otherwise the current game state.
- `nav` counts game frames since the game read the A press that ends a `menu:` path (the boot target's or a `nav {menu}`). A raw `call("pause_at", {..., "after": n})` holds only once the clock has restarted more than n times, so a client can read the count, send `nav`, and pause without racing the press.
- A clock that starts over drops its old frame. `pause_at(..., next=true)` holds only after its clock has started over: frame N of the next cutscene or the next chained mission. `frame_info` and the held picture list each clock's restart count (`restarts`).

Stepping:

- `step` unpauses RT64, releases the game and re-freezes `frames` later (default clock `dl`). `resume` lets it run.
- Pause points run once a frame on every screen: the menu and mission frame hooks, and a `submitGfxFrame` hook on screens neither ticks (the craft-select hangar).
- While a client is connected the hold has no time limit; a disconnect releases it.

Inspection tools (`frame_summary`, `list_draws`, `get_draw`, `dump_texture`, `view_framebuffer`, `view_draw`, `free_cam`) need the frozen frame. Light preset edits change RT64's library at once but show after the next `step`, because presets reach the picture only on newly drawn frames. `lights_active` reads what the last light pass actually used (`rs64lights::publishActiveLights` / `publishSun`).

## Events, picking and captures

- `pause_when` holds the game like `pause_at` once an RDRAM value (1/2/4 bytes, relocated ranges handled) changes or compares eq/ne/gt/lt. It is checked at each pause point, so once a frame, and the picture records the value seen.
- `pause_when_drawn` holds it once a mesh part or sprite whose light-preset key starts with a given prefix puts a vertex inside the view. The F5 GBI reports keys to `rs64mcp::drawWatchSeen` while the watch is armed, and the next pause point freezes (one or two frames after the part first shows).
- A crash can't be paused on (the faulting thread is gone), so `wait_log` waits for its log line instead, and `hang_dump` captures a hung run.
- `wait_log` gives up after at most 120 s, when the game exits, or at a `fail_pattern` line (crash markers by default), so a wait for an event that never happens fails fast: drive the game toward it and wait again. A pattern already in the log fails at once with that line (`repeat=true` waits for another).
- On a launched game, `wait_log`, `pause_at` and `step` also stop after 20 s on one menu screen (other than the main menu) with no boot target running and no held input, since nothing will change it. `pause_at(..., next=true)` on the state already shown pauses in this visit if its frame is still ahead and errors if it has passed. `frame_info`'s `nav` is true while the boot target is still driving the game.
- `pick` reuses the F1 right-click hit test (`DebuggerInspector::pickCalls`) on native framebuffer coordinates. `highlight` sets a draw's `debuggerDesc.highlightColor` on the frozen workload.
- `launch(record=...)` sets `ROGUESQ_LS_RECORD`. Input injected by `press` lands in the recording like a pad, and a session with holds and steps replays with identical frame hashes.
- RenderDoc captures go to `dumps/RenderDoc/mcp-<session>/`.

## Input

- `press` holds controller-1 buttons or the stick for N display lists (or until `release`), injected where the BOOT_TARGET driver injects, ahead of keyboard and gamepad. From a freeze, `press` then `step N` plays exactly N frames of that input. Menus and levels are still reached with `launch`'s `ROGUESQ_BOOT_TARGET`: the nav sequencer only runs from boot.
- `mouse(actions, delay_ms=150)` sends mouse input in client pixels: `move:x:y`, `click:x:y`, `down`/`up` (optional `:x:y`), `rclick`, `wheel:n` (horizontal, + right) and `vwheel:n` (vertical, + up). The bridge pushes the events into SDL's queue with their own mouse id, which the menus accept without window focus, so the real cursor is untouched.
- `type_text(text, backspaces, delay_ms=350)` types on the PASSCODES and ENTER NAME letter wheels. Each character is posted to the game window as `WM_CHAR` (SDL_TEXTINPUT) and Backspace as a key down/up, so the window needs no focus. The game must be running, not frozen. Enter on PASSCODES submits the code (ENTER CODE); on ENTER NAME it adds the current letter.

## Settings and menus

- `launch(settings={...})` merges `roguesq_video.json` keys over the player's file into a copy in the session folder and starts the game on it (`ROGUESQ_VIDEO_CONFIG`), so `set_config` and in-game changes stay in that copy.
- `call("setting", {})` lists the in-game menu settings (the DISPLAY SETTINGS, RAY TRACING, ADVANCED and input keys). `{key}` steps a toggle or cycle as its button does, `{key, value}` sets a slider.
- `call("nav", {"menu": "OPTIONS/GAME SETTINGS"})` backs out to the main menu and selects that path. `{"next_level": n}` starts the next mission of a `ROGUESQ_NAV_CHAIN` run.
- `call("pause_menu", {})` reports the in-mission pause page (0 root, 1 GAME SETTINGS, 2 SOUND SETTINGS, 3 abort confirm, 4+ mod pages, -1 closed) and its cursor. `{"page": n}` or `{"page": "display"}` (a mod page name) moves the open page's cursor onto the entry that opens that page, so a `press` of A and a `step` from a held frame open it on the same frame every run.
- `set_config` applies Configuration-tab settings as the F1 tab does.
- `rt_features` flips the RS64 RT passes (a feature pinned by its env var stays put).
- `render_toggles` sets texture replacement (F4) and ViewRDRAM (F3).
- `rt_debug_view` cycles the ray-tracing debug views as F9 / Shift+F9 do (`step` +1 / -1) or picks one by `name` ("shadow mask", "light reach", "light visibility", "off", ...), shown after the next `step`.
- Trace categories are read once at startup by design, so set them through `launch`'s `env`, not live.

## Memory

- `rdram_read` / `rdram_write` reach the full 16 MB the host tables use (0x80000000-0x80FFFFFF).
- `rdram_dump` snapshots all 16 MB by default (the host tables above 8 MB included). `full=false` writes the 8 MB `ROGUESQ_DUMP_RDRAM_AT_VI` / PJ64 golden format. `tools/debug/validate/rdram_golden_diff.py` diffs two snapshots of either size.
- `rdram_find` searches the 16 MB for a byte pattern (a glyph name, a pointer) instead of paging through `rdram_read`.
- `loaded_models` lists every model the level has loaded, by name, from the game's own model table.

## Notes

- `free_cam` moves are `[right, up, forward]`. RT64's view matrix looks along +row 2 of the inverse view, and `lookAtPerspective` faces away from its focus, so the bridge mirrors the target.
- `dump_texture` writes the raw TMEM (`.tmem`), its tile parameters (`.tmem.json`) and a PNG decoded by [tmem_decode.py](../../tools/debug/mcp/tmem_decode.py), which follows `TextureDecoder.hlsli` (odd-row word swap, TLUT at 0x800).
- RT light presets render only with `ROGUESQ_RT_LIGHTS=1`.
- `ROGUESQ_LOG_MCP=1` logs every pause point and display list with the clock values, for diagnosing a pause that lands on the wrong frame.
