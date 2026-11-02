# rs64 MCP server

Lets an MCP client (Claude Code) launch the game, freeze it on an exact frame and read RT64's inspector data. How it works: [docs/engine/rt64-mcp.md](../../../docs/engine/rt64-mcp.md).

Needs Python 3 with the `mcp` package (`pip install mcp`) and a built game in `build/<config>/` that has the bridge: Debug does by default, Release only with `-DRS64_MCP=ON` ([rt64-mcp.md](../../../docs/engine/rt64-mcp.md)).

Register it in the repo's `.mcp.json` (gitignored, per machine):

```json
"rs64": { "type": "stdio", "command": "python", "args": ["tools/debug/mcp/rs64_mcp.py"] }
```

`python tools/debug/mcp/rs64_mcp.py --self-test` launches the Debug build and checks the bridge end to end.

Outputs (the game's stdout/stderr, dumps, screenshots) go to `dumps/mcp/<session>/`; RenderDoc captures to `dumps/RenderDoc/mcp-<session>/`.

## Tools

| Tool | What it does |
|---|---|
| `launch` | Start the game (cwd `build/<config>`, RenderDoc injected, muted, dev mode) and connect. `env` adds `ROGUESQ_*` variables. Refuses while its game is still running. |
| `attach` | Connect to a game the user started (a Debug build serves the bridge on port 47064). Everything but `wait_log` works; `close` only disconnects. |
| `close` | Kill the game (an attached one is only disconnected). |
| `status` | `running`, `pid`, `port`, `exit_code`, `log_path`, `session_dir`. |
| `call` | Send any bridge method directly. |
| `pause_at` | Freeze the game and the picture on exactly frame `frame` of `clock` (game state id, `cutscene`, `crawl`, or `dl` = display lists since boot). |
| `step` / `resume` | From a freeze, run `frames` more and re-freeze / let it run. |
| `pause_when` | Freeze when an RDRAM value changes or meets eq/ne/gt/lt (checked once a frame). A state change is `pause_at(<state>, 0)`. |
| `pause_when_drawn` | Freeze once a model part or effect sprite is on screen, by light-preset key prefix (`r_pow`, `xwing#s0.p4`, `spr:explo_tex`; `\|` separates several). One or two frames after it first shows. |
| `wait_log` | Wait for a regex in the game's log (e.g. `SEH caught\|null-call\|Assertion`). |
| `pick` / `highlight` | The draw calls under a framebuffer pixel (F1's right-click picker); tint one on screen or clear tints. |
| `read_symbol` | Read memory at a `syms/` symbol name. |
| `walk_dl` | RDRAM dump + `f5_dl_walk.py` over the current task (or a `frame_summary` `dl_addr`). |
| `hang_dump` | Full-memory minidump + `reconstruct-freeze.py` + `host-stacks.py` (about 2 GB; delete it after). |
| `launch(record=...)` | Record the next mission's input, `press` included, as a `.rec` that replays frame-identically. |
| `frame_info` | Game state, every clock, held / paused flags, the frozen picture. |
| `rdram_read` / `rdram_write` | Game memory as hex at KSEG0 addresses (relocated ranges handled). |
| `rdram_dump` | The 8 MB RDRAM in the `ROGUESQ_DUMP_RDRAM_AT_VI` format, for `rdram_golden_diff.py` / `state_diff`. |
| `press` / `release` | Hold controller-1 buttons or the stick for N frames (or until release); with `step`, plays exact frames of input. |
| `screenshot` | PNG of the game window, captured by the game's PID. |
| `frame_summary` / `list_draws` / `get_draw` | The frozen frame's framebuffer pairs and draw calls, everything the F1 Debugger shows. |
| `dump_texture` / `texture_dumping` | One draw tile's TMEM as `.tmem` + `.json` + `.png` / RT64's whole-run texture dumper. |
| `view_framebuffer` / `view_draw` | Show a framebuffer pair's color or depth image, or the frame drawn up to one draw call. |
| `renderdoc_capture` | Capture presents with RenderDoc; returns the `.rdc` path for the renderdoc MCP. |
| `free_cam` / `get_camera` | RT64's free camera on the frozen frame: place, move (`[right, up, forward]`), rotate. |
| `lights_list` / `lights_set` / `lights_remove` / `lights_save` | RT light presets (shown after the next `step`; needs `ROGUESQ_RT_LIGHTS=1` to render). |
| `lights_active` | The lights shining in the last drawn frame (after the 32-slot selection: kind, priority, camera-space position, radius, colour) and the sun the shadow pass traced. |
| `frame_times` | RT64's Frametimes plot series (min/avg/max/p99, raw samples on request). |
| `frame_warnings` / `sprite_commands` / `rsp_state` / `vi_info` | The rest of the F1 Debugger: command warnings, S2D sprites, RSP fog/lights/LookAt/modifies, VI registers. `get_draw` also returns RDP colors and, with `shaders`, writes the call's HLSL. |
| `get_config` / `set_config` | RT64's Configuration tab live (not saved unless `save`). |
| `rt_features` / `render_toggles` | Toggle the RT passes (lights, shadows, AO, GI, reflections, ...) and texture replacement / ViewRDRAM. |

`python tools/debug/mcp/tmem_decode.py --self-test` checks the TMEM decoder; `python tmem_decode.py <file.tmem>` decodes a dump.
