---
name: rs64-mcp-usage
description: Drive the running game through the rs64 MCP server (tools/debug/mcp/rs64_mcp.py) to see or prove something in a real frame - launch to a level, freeze on an exact frame, fly the craft with press/step, screenshot, inspect draws/textures/framebuffers/RDRAM, then close and clean up. Use whenever a change must be checked in-game (a renderer/terrain/lighting fix, a "looks wrong at a distance" report, a menu or mission screen), when the user asks to "try it", "look at", "fly to" or "capture" something, or before asking the user for an F1 screenshot. Covers the launch -> pause_at -> press/step -> screenshot loop, how the craft responds to stick input, frame-clock choice, the free_cam limitation with the terrain pass, and the gotchas (already paused, one launch, env per launch, cleanup). Triggers on mcp__rs64__*, pause_at, press, step, free_cam, screenshot of the game, fly/steer the ship, check in game. Complements roguesquadron-debug (crash/hang triage), roguesquadron-menu-drive (keyboard menus) and renderdoc-mcp (GPU captures, via renderdoc_capture).
---

# Using the rs64 MCP

What each tool does: [tools/debug/mcp/README.md](../../../tools/debug/mcp/README.md). How the bridge and the pause model work: [docs/engine/rt64-mcp.md](../../../docs/engine/rt64-mcp.md). This file covers how to use the tools together.

## Before launching

- Build first (Debug has the bridge). A running exe blocks the link, and so does a game another session launched, so check with `Get-Process RogueSquadron64Recomp` and ask before killing one you did not start.
- Launch once. `launch` refuses while its own game runs, so if a launch seems to fail, check `status` and the process list before retrying.
- All settings go in `launch`'s `env`, every launch. Nothing carries over between launches, and trace categories (`ROGUESQ_LOG_*`) are read once at startup.
  - `ROGUESQ_BOOT_TARGET=level:<id>[,craft]` drives the real menus into a mission. `menu:...`, `hangar:<id>`, `pause:<id>`, `cutscene:<level>,<kind>`: [debug-trace-env-vars.md](../../../docs/engine/debug-trace-env-vars.md).
  - `ROGUESQ_DRAW_DIST=<x>` overrides the draw distance setting (1-8 with the terrain pass).
  - Level ids seen so far: 3 = Corellia (78x78 map, water). The boot loads a 32x32 map (level 19) for the front end; ignore it.

## The core loop

```
launch(env={"ROGUESQ_BOOT_TARGET": "level:3"})
pause_at("mission", 200)          # frame 200 since the mission state was entered
press(stick=[-1, 0], frames=30)   # queue input
step(frames=30, clock="mission")  # play exactly those frames, freeze again
screenshot()                      # PNG path; open it with Read to see it
...
close()
```

- `pause_at` only works when running. Once frozen, move with `step`, or `resume` and then `pause_at` again; calling `pause_at` while paused errors with "already paused".
- Use `clock="mission"` (or the state you are in) for `step`, so frame numbers match `pause_at`. The default `dl` counts display lists since boot.
- `press` with `frames=N` then `step N` plays exactly N frames of that input. `frames=0` holds until `release`.
- A mission's first ~200 frames are the opening fly-in. Steer after that.
- `screenshot` returns a path, so `Read` it to look. Judge one image per step; don't batch ten blind steps.

## Flying the craft (X-wing class, measured on Corellia)

| Input | Result |
|---|---|
| stick x = -1 for 30 frames | Hard banked turn left: ~30 deg roll, large heading change |
| stick x = -0.4 for 20 frames | Gentle ~15 deg bank; response is roughly proportional |
| release after a turn | Wings level again within ~30 frames, new heading kept |
| stick y = +1 (up) | Nose DOWN (plane style); -1 pulls the nose up |
| pitch | Fast and it holds: 40 frames full back went from a dive to near vertical. Use 8-15 frame taps |

- The camera trails the craft, so step ~10 frames with no input before judging a view.
- High, level view across the map: ~25 frames back, then 15 + 8 frames forward.
- With no input the craft flies on; on Corellia it leaves the map by mission frame ~1600 (handy for checking the edge past the map).

## Inspecting a frozen frame

- `frame_summary` -> `list_draws(fb, proj)` -> `get_draw` / `dump_texture` / `view_draw(index)` + `screenshot`. `pick(x, y)` takes native framebuffer coordinates (`vi_info` fb size, e.g. 320x240).
- `rdram_read` / `read_symbol` / `pause_when(addr, op=...)` for game state; `walk_dl` for the F5 display list.
- `renderdoc_capture` for GPU-level questions, then the renderdoc MCP (`renderdoc-mcp` skill).
- `free_cam` does not work with the whole-map terrain pass (the default renderer): the pass keeps drawing with the game camera, so the picture comes out mixed. Steer the craft to the view instead. Disable it again with `free_cam(enabled=false)`.
- Light preset edits and `rt_debug_view` show only after the next `step`.

## Finishing

- `close()` kills the game; an `exit_code` of 1 after `close` is the kill, not a crash. Check the log for crashes instead: scan `log_path` for `Assertion|Access violation|SEH caught|null-call`.
- Delete the session folder (`dumps/mcp/<session>/`) and any RenderDoc captures once you've looked at them, and confirm no game process is left running.
- Report what the frames showed and what was not covered (other levels, other angles, other settings).
