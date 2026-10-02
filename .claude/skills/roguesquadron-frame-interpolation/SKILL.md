---
name: roguesquadron-frame-interpolation
description: Use when working on RT64 frame interpolation in RogueSquadron64Recomp - objects jitter, wobble, pulse, smear, flicker or snap at 60/120/144 Hz while the game runs ~30 fps; adding a new geometry path that must interpolate; touching matrixId / TransformGroup / G_EX_ID_* / ROGUESQ_F5_NODE_ID / sprite ids / regrid; or deciding whether interpolation can be enabled by default.
---

# Rogue Squadron Frame Interpolation

## Overview

RT64 buffers one real game frame and draws in-between frames by pairing each transform with its previous-frame counterpart and blending prev -> cur. **Pairing quality is everything**: a correct pair looks perfect, a wrong pair smears, a missing pair snaps. Factor 5 has no separate camera: projection is pure perspective, view = identity, every modelview is camera-space. So camera motion only interpolates per object, and an unpaired object stutters against a smooth world.

## Enabling

- `roguesq_video.json`: `"frameInterpolation": true`, `"targetFramerate": 60` ([src/main/video_config.cpp:69](../../../src/main/video_config.cpp)). Code default is OFF ([src/main/rt64_render_context.cpp:210](../../../src/main/rt64_render_context.cpp)); its "effect quads flicker" rationale predates the sprite-id work and is stale.
- Env: `ROGUESQ_RT_INTERP=<hz>` (`0` = off). Deterministic repro: `ROGUESQ_BOOT_TARGET=level:N` (attract demo is too noisy) plus a recorded input replay (`ROGUESQ_INPUT_REPLAY`, files in `tools/recordings/`); mute with `ROGUESQ_NO_AUDIO_UCODE=1`. **Test in the Release build**: Debug adds background work that distorts frame timing.

## Pipeline (lib/rt64/src)

| Stage | Where | Notes |
|---|---|---|
| Rate | `hle/rt64_vi.cpp:193` | Majority vote over the VI factor ring (`ROGUESQ_INTERP_RATE=strict\|<hz>`) |
| Per-frame timing | `hle/rt64_workload_queue.cpp:997` | `ROGUESQ_INTERP_VARIABLE`: each real frame tweens over its own VI count |
| ID pairing | `hle/rt64_game_frame.cpp:359` | Sorted-id merge; `ROGUESQ_INTERP_ID_MAXJUMP` jump guard, default off (it rejected steady close flybys) |
| AUTO pairing | `hle/rt64_game_frame.cpp:595-692` | Hash (combiner, othermode, geomode, **triangleCount**) + greedy nearest; `f5AutoMatchTooFar` 0.2 NDC gate (`:29`) |
| Blend | `hle/rt64_rigid_body.cpp` | Decompose; lerp translation (chord), slerp rotation |
| Draw | `render/rt64_transform_processor.cpp:42-58` | Mapped: `lerp(prev,cur,w)`. **Unmapped: drawn at cur** |

## Identity sources (lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp)

| Geometry | ID | Where |
|---|---|---|
| Ships, lasers, rigid objects, menu/overlay models | `0x2xxxxxxx` = scene-node ptr + regrid generation; node recorded by the `traverseSceneGraphRecursive` hook @0x800155E4 -> `rs64_f5_map_node`, for both matrix rings (`0x700000`/`0x710000` flight, `0x770000`/`0x780000` menus/overlays; `f5_ring_buffer`). Id from a node's first frame (`ROGUESQ_F5_NODE_STABLE_ONLY=1` restores waiting a frame) | `f5_map_node_impl` ~381, stamp ~1179 |
| Tow cable segments | `0x1xxxxxxx` = generation + position from the ship end (submit order), from the `addNpcToVisibilityBucket` entry hook -> `rs64_f5_cable_submit(pool, instance, state word 0x8010B6E8)`; only drawn slots count; new generation on a gap, re-fire (phase 3/4 -> 1/2) or falling count; scene node = segment mesh instance + 0x0C; `rs64cable::CableRoles` in `hle/rt64_rs64_cable.h`. Ids by role, not slot: a head-only alias left the previous head unpaired after every advance. Covers the long-tow-cable mod | `f5_cable_submit_impl`, `f5_lookup_node_alias`, alias applied after the node-id block in op_01 |
| Terrain | Root node id; re-centers compensated (`ROGUESQ_F5_REGRID_COMP`) or generation-bumped (`REGRID_SNAP`) | `f5_node_regrid_generation` ~143, `f5_regrid_after_load` ~184 |
| Crosshair rings | `0x80000000` outer / `0x80000001` inner. The rings are texrects (no rect interpolation in RT64); a texrect whose upper-left is a shown HUD ring element (`D_8010CA30` +0x28 / +0x58, xy at +0x18) plus (256, 224), size = tile x 1024/dsdx, draws as a camera-space quad with a translation-carrying transform. A ring clamped at the screen edge matches on its far edge. `ROGUESQ_F5_XHAIR_QUAD=0` keeps the texrect | `f5_crosshair_quad`, `hle/rt64_rs64_crosshair.h`, called from `texrectLLE_guarded` |
| 0xBD sprites | `0x6xxxxxxx` face id (renderLitMeshFaceGroup hooks) -> `0x4xxxxxxx` sparse node+ordinal -> `0x5xxxxxxx` world tracker -> AUTO | `f5_face_sprite_id` ~245, `f5_world_sprite_id` ~274, op_bd ~1437 |

Nodes get an id from their first frame (`ROGUESQ_F5_NODE_STABLE_ONLY=1` waits a frame); an op_01 without an id resets to `TransformGroup()` (AUTO) so it can't inherit the previous object's group. Taken id prefixes: `0x0001` (slot, off), `0x1` tow cable segments, `0x2` nodes, `0x3` baked faces, `0x4`/`0x5`/`0x6` sprites, `0x7` flattened PRIM faces, `0x8` crosshair rings. The comment near op_01 ~1274 saying "AUTO alone is best" is stale.

## Giving new geometry an identity

```cpp
state->rsp->matrixId(id, /*push*/false, /*proj*/false, /*decompose*/true,
    G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE,  // pos rot scale
    G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE,                              // skew persp
    G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP,                                            // vtx pos / texcoord
    G_EX_COMPONENT_AUTO, G_EX_COMPONENT_AUTO, G_EX_ORDER_LINEAR, G_EX_ASPECT_AUTO, G_EX_EDIT_NONE, false, false);
```

The id must be: **the same** for the same logical object across frames; **different** when the slot becomes a different thing (pool reuse, flipbook restart, teleport, grid re-center -> bump a generation); never `0` (IGNORE) or `0xFFFFFFFF` (AUTO); in its own top-nibble namespace. Never derive it from submit order, ring slot, DL address or a bare heap pointer. Unique within a frame. A pushed group (`push=true`, as op_bd does) must be popped after the draw. Use `G_EX_ID_IGNORE` (push) for things that must never blend. Motion must live in the **transform**, not baked into vertices (vertex interp is SKIP).

## Diagnosing jitter

| Symptom | Mechanism | Check / fix |
|---|---|---|
| Object oscillates back and forth along its motion ("jitter in place") | Flips mapped <-> unmapped between frames. Mapped lags up to 1 frame, unmapped is drawn at cur (ahead). Camera-space velocity makes the gap large | Log per-transform mapped state per frame. Measure with `ROGUESQ_LOG_INTERP_PAIR=1`. Flip triggers: `ID_MAXJUMP` guard if enabled, 0.2 NDC auto gate, sprite generation bumps (gap / >4x size jump; a flipbook restart only counts if it also moved >1.5x size) |
| Id-less object pops for one frame when it starts moving or reverses | AUTO position: `RigidBody::updateLinear` disables translation lerp on start-from-rest or >90 deg velocity reversal | Give it an id (INTERPOLATE bypasses the heuristic) |
| Searchlight shafts/glows on structures jitter only while the camera moves | Game bakes their camera-space position into the verts every frame under a static matrix (`0x80037780`, never written in-mission; confirmed with `ROGUESQ_DATA_BP`); vertex interp is SKIP so they step at game rate | `ROGUESQ_F5_BAKED_VTX_INTERP` (default on): per-face id + vertex interpolation for faces under a non-ring, id-less matrix. Find such cases by checking which matrix (`s_mv_last_w1`) an id-less draw sits under |
| Glow / light shaft on a structure flickers at 30 Hz; fine on real frames | zSource=PRIM draws use one constant depth from the current frame while the surface under them is drawn at its blended depth (`shaders/RasterVS.hlsl:34`) | `ROGUESQ_F5_SPRITE_PIXEL_Z` and `ROGUESQ_F5_PRIM_FLATTEN` (default on) give these faces per-vertex depth that follows interpolation |
| Growing fire puff pulses while its position is smooth | Sprite size baked into int16 verts, vertex interp SKIP -> size steps at game rate | Build a unit quad and put size in the transform scale (`SPRITE_XFORM` only moves the centre into the transform; size stays in verts) |
| Smear / warp between unrelated objects | Mispair: id reused across instances or AUTO matched the wrong neighbour | Add a generation; tighten id; never loosen the AUTO hash |
| Two unrelated things pair | Id namespace collision: `ROGUESQ_F5_TERRAIN_ID` terrain ids (`0x6`+cell, ~1215) share the `0x6` prefix with face-sprite ids | Keep `F5_TERRAIN_ID` off or move one namespace |
| Sprite ids all reset at once | `f5_face_sprite_id` clears its map at >8192 entries (~250) | Bounded-map eviction instead of clear |
| Periodic whole-terrain lurch | Grid re-center tweened | Confirm `REGRID_COMP`/`REGRID_SNAP` on (default) |
| Whole-frame lurch / speed spike | Rate dropped or a stretched frame timed as 1 VI | A/B `ROGUESQ_INTERP_RATE=strict` / `ROGUESQ_INTERP_VARIABLE=0` |
| Uneven motion that survives correct pairing | Game-side unevenness (e.g. camera and objects updated on different cadences) or late real frames | Not an interp bug; interpolation shows it faithfully |

**Measuring coverage:** replay a recording (`tools/recordings/`, Release) with `ROGUESQ_LOG_INTERP_PAIR=1` and aggregate `[pair]` per class. `[autodraw]` counts draws made with no id per draw site (tile_quad, tile_grid, sprite, face; every mesh face goes through `f5_emit_face`), and is the real coverage number. AUTO transforms left in `[pair]` are `op_04` vertex-cache loads that are never drawn directly (faces copy them into their own group), so they don't matter. Unpaired id'd transforms are first-frame spawns, which can't pair. Remaining gaps show up as trackers issuing fresh ids; `[facesprite]` and `[baked-miss]` say why.

**First step for jitter:** toggle one path at a time on the same scene (`ROGUESQ_F5_SPRITE_FACE_ID=0`, `ROGUESQ_F5_SPRITE_WORLD_MATCH=0`, `ROGUESQ_INTERP_ID_MAXJUMP=0`, `ROGUESQ_INTERP_AUTO_MAX_SCREEN=0`). `ROGUESQ_F5_SPRITE_XFORM=0` is not a single-path toggle: it also re-enables the dense-sprite no-id path and isolates id-less sprites. `ROGUESQ_LOG_INTERP_PAIR=1` logs per-class paired/total, flips and in-frame duplicate ids (`[pair]`), jump rejects, and face-sprite id outcomes; aggregate it over gameplay frames with awk. Judge fixes by eye on a live A/B (`mcp__windows-screenshot__capture_burst`, title "Rogue Squadron 64 Recompiled"). A higher match rate that visibly warps is a failure.

## Dead ends (do not retry)

- Submit-order / ring-slot ids (`ROGUESQ_F5_SLOT_ID`), address-derived ids, persistent entity-pointer maps: mispair on spawn/despawn.
- Dropping triangleCount from the AUTO hash (`ROGUESQ_INTERP_HASH_TRIS`): wider mispairs, smear.
- Vertex interpolation for terrain (`ROGUESQ_F5_VTX_INTERP`): tile vertex sets change per frame, flicker.
- Classifying ship vs particle by node fields (owner8, unique drawable): drops formation ships.
- Hardcoding heap address pool ranges: level-fragile.

## Inherent costs

~1 real frame (~33 ms) extra latency; mispairs smear rather than pop; vertex-baked motion and late real frames stay at game rate.

Env var catalog: [docs/debug-trace-env-vars.md](../../../docs/debug-trace-env-vars.md). History: plans/frame-interpolation-plan.md, plans/terrain-interpolation-plan.md.
