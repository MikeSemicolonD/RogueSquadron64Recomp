---
name: renderdoc-mcp
description: Use the RenderDoc MCP server to GPU-debug RogueSquadron64Recomp render bugs from a captured .rdc frame — when a game object is submitted but NOT appearing (or appears wrong) and you need to see, at the GPU level, whether it rasterized, where its verts landed, what state/shader/texture it used, and which pixels it wrote. Use for ANY "why isn't X rendering / why does X look wrong" question where CPU-side traces (DL walks, RDRAM diffs) already show the geometry reaches RT64 but the pixels are wrong or absent — i.e. the drop is RT64/GPU-side. The decisive move is pixel_history to split "rasterized-but-wrong" from "never-rasterized". Covers: opening a capture, mapping passes, saving/reading render targets (and the RGBA16 white-out gotcha), enumerating draws, reading post-VS clip positions, per-pixel write history, and the F5 pass layout + y-down NDC. Triggers on: RenderDoc, .rdc capture, GPU frame debug, pixel_history, get_post_vs_data, "black sky"/skybox, model texture/UV bug, effect/debris not appearing, "submitted but not rasterized", "rasterized-not-presented vs never-rasterized", open_capture, dumps/RenderDoc. Complements roguesquadron-debug (top-level evidence loop) and rt64-integration (renderer internals); this is the GPU-capture arm. Read AGENTS.md first.
---

# RenderDoc MCP — GPU frame debugging for RogueSquadron64Recomp

Audience: AI agents in this repo. This is the **GPU-side** arm of the evidence loop. Reach for it when
CPU-side traces (DL walk, RDRAM golden diff) prove the geometry reaches RT64 but the pixels are absent or
wrong — the drop is then RT64/GPU-side, and the capture shows *exactly* where. It broke open the skybox
bug in one session (proved the sky never rasterizes, then localized the cause to culling). Read
`AGENTS.md` and the `roguesquadron-debug` skill first.

## Setup facts (see `reference_renderdoc_mcp_setup_2026_09_14` memory)
- The server is configured in **user-scope** `~/.claude.json` (`mcpServers.renderdoc`), Python 3.11 host,
  `mcp<2`. If its tools are missing, the memory has the rebuild recipe.
- **It is an ANALYSIS server, not a capture trigger.** `open_capture` takes a `.rdc` filepath; the MCP
  cannot hook the running game. You need a capture first (RenderDoc UI / `renderdoccmd`, or an in-app
  `TriggerCapture()`). Captures already on disk:
  - `dumps/RenderDoc/progress2/*.rdc` — kept flight/cinematic frames (e.g. `*_frame1159` = logo explosion,
    `*_frame2989` = flight with the black sky).
  - `%LOCALAPPDATA%\Temp\RenderDoc\` — RenderDoc's auto/temp captures (ephemeral).

## The core workflow
```
open_capture(filepath)                      # D3D12; returns api/action/texture/buffer counts
get_frame_overview()                        # resolution, draw_calls, render_targets[] (id, size, format, draw_count)
analyze_render_passes()                     # pass list: event ranges, RT per pass, clears
save_render_target(event_id, out.png)       # dump a RT to view it (see gotcha below)
find_draws(min_vertices=1, max_results=N)   # enumerate draws -> exact event_ids (needed by every other call)
get_draw_call_state(event_id)               # topology, vertex_count, textures, RTs, shaders
get_post_vs_data(event_id, stage="vsout")   # per-vertex SV_Position (clip x,y,z,w) + TEXCOORD + COLOR
pixel_history(rt_id, x, y, event_id)        # every write to one pixel: pass/fail, pre/post value
read_texture_pixels(rt_id, x, y, w, h)      # ACTUAL float RGBA of a region (<=64x64)
```

## The decisive tool: `pixel_history`
Splits the two hypotheses that env-var iteration cannot: pick a pixel in the region that should show the
object; if the modification list is **only clears**, the object **never rasterized** (bug is upstream in
vertex/clip/cull); if a draw wrote it and was then overwritten/failed a test, it **rasterized but is
wrong** (blend/combiner/depth). Skybox win: `pixel_history` on a black sky pixel showed only clears ->
never-rasterized -> ruled out present/writeback and pointed at cull.

## Gotchas learned the hard way
- **`save_render_target` white-out.** RT64's native/2x targets are `R16G16B16A16_UNORM` (HDR); the PNG
  save applies a display transform that can blow real color to solid white. **Do not trust the saved
  PNG's color** — confirm with `read_texture_pixels` (it returns true float RGBA). The final present RT is
  `B8G8R8A8_UNORM` (id ~321/322, 640x480) and saves faithfully — use it to confirm which frame you have.
- **Broken state accessors.** This pyrenderdoc build throws `AttributeError: 'PipeState' object has no
  attribute GetColorBlend/GetDepthState/GetStencilState/GetRasterizer`. So `get_draw_call_state` cannot
  report blend/depth/cull. Work around with pixel/vertex data (`pixel_history`, `get_post_vs_data`) and,
  for cull specifically, an A/B env toggle in the game (e.g. `ROGUESQ_SKY_NOCULL`).
- **No per-object tag in the stream.** Draws are not labeled by game object. Identify them by content:
  terrain = tan vertex COLOR (R>G>B ~0.5-1.0), large positive w (near ~4500 -> far ~10000). Sky/effect =
  different color / near-plane or negative w. `find_draws` gives event_ids + index counts only (no color),
  so you must `get_post_vs_data`/`get_draw_call_state` a candidate to classify it.
- **Event ids are exact.** `get_*` calls fail `INVALID_EVENT_ID` on a non-draw event. Get real ids from
  `find_draws` (they step ~2-3). `find_draws` always enumerates from the start; there is no offset, so
  raise `max_results` to reach later draws.

## RS64 / F5 specifics
- **Pass layout** (flight frame): native scene -> RT `5889` (640x340), then a 2x copy -> RT `5899`
  (1280x680) rendering the same draws, then present -> RT `321`/`322` (640x480). Inspect the native
  `5889` pass for the real geometry.
- **F5 NDC is y-DOWN.** In clip space after perspective divide: **y < -1 = above the top edge**,
  **y > +1 = below the bottom edge**, y in [-1,1] = on screen. (An X-wing at hw ndc y +0.8 is at the
  bottom.) Get this backwards and you will misread which faces are the sky.
- **The skybox dome** wraps the camera (verts with negative w / huge NDC, matrix `mv=0x80700040`). Its
  faces are the inside of a sphere (back faces); RT64 was culling them (root cause found 2026-09-14).

## Recipes for the open render bugs
- **Skybox / effect-not-appearing:** `pixel_history` the empty region -> only clears => never-rasterized.
  Then check whether faces reach the CPU clip (`rt64_rsp.cpp` `drawIndexedTri`), and
  A/B the cull (`ROGUESQ_SKY_NOCULL`, or `ROGUESQ_F5_CULL=0` for the double-sided path).
- **Model texture/UV:** `get_draw_call_state` the model draw for its bound texture, `read_texture_pixels`
  the texture, `get_post_vs_data`/TEXCOORD to compare UVs vs the byte-faithful golden.
- **Confirm which frame a capture is:** `save_render_target` the present RT (`321`/`322`).
</content>
