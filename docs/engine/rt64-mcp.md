# RT64 MCP: live debugging from an agent

An MCP server ([tools/debug/mcp/rs64_mcp.py](../../tools/debug/mcp/rs64_mcp.py)) that launches the game, freezes it on an exact frame and gives an agent what the F1 inspector shows: draw calls, textures, framebuffers, the free camera, RT light presets and frame times. Setup and the tool list: [tools/debug/mcp/README.md](../../tools/debug/mcp/README.md).

## How it fits together

- The bridge is compiled in by the CMake option `RS64_MCP`: `AUTO` (default) builds it into Debug only, `ON` into every config (the VS Code task "Build: RogueSquadron64Recomp (Release, MCP)"), `OFF` into none. Release builds, and so every shipped build, have none of it.
- The server starts the exe with `--mcp-port <n>`; the game opens a loopback socket there (127.0.0.1, one client, newline-delimited JSON: `{"id", "method", "params"}` in, `{"id", "ok", "result" | "error"}` out). A Debug build started by hand serves port 47064 by default (`ROGUESQ_MCP_PORT=0` turns it off), so `attach` can connect to it; other builds open no port without the flag.
- Host methods (pause, step, RDRAM, RenderDoc) are served by [src/main/mcp_bridge.cpp](../../src/main/mcp_bridge.cpp) and [src/lockstep/lockstep.cpp](../../src/lockstep/lockstep.cpp).
- RT64 methods are queued to [lib/rt64/src/hle/rt64_rs64_mcp.cpp](../../lib/rt64/src/hle/rt64_rs64_mcp.cpp) and run on the gfx thread at the top of `State::updateScreen`, which runs every VI, paused or not, with F1 open or closed.
- The server owns the process: `launch` runs from `build/<config>` with RenderDoc injected, audio muted and dev mode on; `close` kills it. Output lands in `dumps/mcp/<session>/`.
- `attach(port)` connects to a game the user started instead; every tool works on it except `wait_log` (its console is not captured), and `close` only disconnects.

## The pause model

RT64's own pause (F4) only freezes the picture: it drops new display lists while the game keeps running. `pause_at` freezes both:

1. The game thread holds at the first pause point (the same clocks as `ROGUESQ_PAUSE_AT`: game states, `cutscene`, `crawl`, plus `dl`, the display-list count since boot) after a display list from frame N was sent. In missions and menus one display list follows each frame's pause point, so the picture is exactly frame N; cutscene and crawl clocks tick slower than display lists, so there it is the first display list at or past N.
2. RT64 then pauses and redraws that frame every VI.

`step` unpauses RT64, releases the game and re-freezes `frames` later (default clock `dl`). `resume` lets it run. Pause points run once a frame on every screen: the menu and mission frame hooks, and a `submitGfxFrame` hook on screens neither ticks (the craft-select hangar). While a client is connected the hold has no time limit; a disconnect releases it.

Inspection tools (`frame_summary`, `list_draws`, `get_draw`, `dump_texture`, `view_framebuffer`, `view_draw`, `free_cam`) need the frozen frame. Light preset edits change RT64's library at once but show after the next `step`, because presets reach the picture only on newly drawn frames. `lights_active` reads what the last light pass actually used (`rs64lights::publishActiveLights` / `publishSun`).

## Events, picking and captures

- `pause_when` holds the game the same way `pause_at` does, once an RDRAM value (1/2/4 bytes, relocated ranges handled) changes or compares eq/ne/gt/lt; it is checked at each pause point, so once a frame, and the picture records the value seen. `pause_when_drawn` holds it once a mesh part or sprite whose light-preset key starts with a given prefix puts a vertex inside the view: the F5 GBI reports keys to `rs64mcp::drawWatchSeen` while the watch is armed, and the next pause point freezes (one or two frames after the part first shows). A crash can't be paused on (the faulting thread is gone), so `wait_log` waits for its log line instead, and `hang_dump` captures a hung run. `wait_log` gives up after at most 120 s, when the game exits, or at a `fail_pattern` line (crash markers by default), so a wait for an event that never happens fails fast; drive the game toward it and wait again. `rdram_read` / `rdram_write` reach the full 16 MB the host tables use (0x80000000-0x80FFFFFF).
- `pick` reuses the F1 right-click hit test (`DebuggerInspector::pickCalls`) on native framebuffer coordinates; `highlight` sets a draw's `debuggerDesc.highlightColor` on the frozen workload.
- `launch(record=...)` sets `ROGUESQ_LS_RECORD`; input injected by `press` lands in the recording like a pad, and a session with holds and steps replays with identical frame hashes.

## Input, settings and dumps

- `press` holds controller-1 buttons or the stick for N display lists (or until `release`), injected where the BOOT_TARGET driver injects, ahead of keyboard and gamepad. From a freeze, `press` then `step N` plays exactly N frames of that input. Menus and levels are still reached with `launch`'s `ROGUESQ_BOOT_TARGET`: the nav sequencer only runs from boot.
- `set_config` applies Configuration-tab settings as the F1 tab does; `rt_features` flips the RS64 RT passes (a feature pinned by its env var stays put); `render_toggles` sets texture replacement (F4) and ViewRDRAM (F3); `rt_debug_view` cycles the ray-tracing debug views as F9 / Shift+F9 do (`step` +1 / -1) or picks one by `name` ("shadow mask", "light reach", "light visibility", "off", ...), shown after the next `step`.
- `rdram_dump` writes the same file as `ROGUESQ_DUMP_RDRAM_AT_VI`. Trace categories are read once at startup by design, so they are set through `launch`'s `env`, not toggled live.

## Notes

- `free_cam` moves are `[right, up, forward]`; RT64's view matrix looks along +row 2 of the inverse view, and `lookAtPerspective` faces away from its focus, so the bridge mirrors the target.
- `dump_texture` writes the raw TMEM (`.tmem`), its tile parameters (`.tmem.json`) and a PNG decoded by [tmem_decode.py](../../tools/debug/mcp/tmem_decode.py), which follows `TextureDecoder.hlsli` (odd-row word swap, TLUT at 0x800).
- RT light presets render only with `ROGUESQ_RT_LIGHTS=1`.
- `ROGUESQ_LOG_MCP=1` logs every pause point and display list with the clock values, for diagnosing a pause that lands on the wrong frame.
