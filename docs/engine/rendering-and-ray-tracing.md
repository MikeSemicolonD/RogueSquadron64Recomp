# Where rendering changes go

A map for contributors who want to tune ray tracing, put ray tracing on more geometry, or change how something draws. It names the file for each kind of change; the details live in the files and the docs linked here.

The game builds Factor 5 display lists on the CPU (recompiled MIPS). RT64 parses them in its Factor 5 GBI profile, turns them into draw calls, rasterizes the prelit N64 image, then runs our ray-traced passes (shadows, lights, AO, GI, reflections, fog shafts) on top. So a change lands in one of three layers:

| Layer | Where | Change it when |
| --- | --- | --- |
| Game code | `rogue_squadron.toml` hooks, [src/game/toml_hooks.c](../../src/game/toml_hooks.c), [patches/](../../patches/README.md) | The game decides it: what is drawn, when, which texture frame, culls, timers |
| F5 GBI parse | [lib/rt64/src/gbi/](../../lib/rt64/src/gbi/) | The display list is right but RT64 reads it wrong: geometry, UVs, textures, sprites, terrain |
| RT passes | [lib/rt64/src/render/rt64_framebuffer_renderer_rs64.cpp](../../lib/rt64/src/render/rt64_framebuffer_renderer_rs64.cpp), [lib/rt64/src/shaders/](../../lib/rt64/src/shaders/) `RS64*` | How the traced lighting looks or what it traces against |

## Ray-tracing settings

| To change | Edit |
| --- | --- |
| A light, its colour, radius or what it is attached to (no rebuild) | `roguesq_lights.json`: the built-in set is [src/video/default_lights.json](../../src/video/default_lights.json) (compiled in), a mod or the exe-dir copy overrides it by name. Edit live in the F1 inspector's Lights tab or the RS64 Explorer's Lights panel. Format: [Light mods](../guides/modding.md) |
| Which features are on, light slots per category | `rayTracing` in `roguesq_video.json`, read and written in [src/video/video_config.cpp](../../src/video/video_config.cpp); the RAY TRACING menu page is [mods/display-settings/roguesq_menu.json](../../mods/display-settings/roguesq_menu.json); shipped defaults per tier are in [video-presets/](../../video-presets/README.md) |
| A tuning default (strength, bias, ray counts, temporal weight) | `rs64lights::config()` in [lib/rt64/src/hle/rt64_rs64_config.h](../../lib/rt64/src/hle/rt64_rs64_config.h); each has a `ROGUESQ_RT_*` override for A/B runs, listed in [debug-trace-env-vars.md](debug-trace-env-vars.md) |
| A new on/off feature | `rs64lights::Feature` and `featureEnv` in `rt64_rs64_config.h`, then `rt_setting` in `video_config.cpp` and a row on the menu page |
| Light selection (the 64 kept, priorities, dedupe, fades) | `fillRS64Lights` in `rt64_framebuffer_renderer_rs64.cpp`, `selectLights` in [rt64_rs64_point_lights.h](../../lib/rt64/src/hle/rt64_rs64_point_lights.h); unit tests in [tests/rs64_lights_test.cpp](../../tests/rs64_lights_test.cpp) |
| The shading itself | `RS64LightsPS.hlsl`, `RS64ShadowsPS.hlsl` (+ `RS64ShadowBlurPS`, `RS64ShadowTemporalPS`), `RS64FogShaftsPS.hlsl`, `RS64WaterMirrorPS.hlsl` and the shared `RS64Trace.hlsli` in [lib/rt64/src/shaders/](../../lib/rt64/src/shaders/); the pass recording is `recordRS64Lights` / `recordRS64Shadows` / `recordRS64FogShafts` in `rt64_framebuffer_renderer_rs64.cpp` |

## Putting ray tracing on geometry

What the rays hit is built each frame from the main view's draws plus a static terrain heightfield.

| To change | Edit |
| --- | --- |
| Make a model part self-lit (glows, casts no shadow) | A mesh preset with `"emissive": true` in `roguesq_lights.json`; no code. Keys come from the F1 Lights tab's mesh picker |
| Which draws cast shadows (opaque, cutout, none) | `rs64lights::casterClass` / `casterAfterDeny` in [rt64_rs64_shadows.h](../../lib/rt64/src/hle/rt64_rs64_shadows.h), applied per draw in `FramebufferRenderer::fillRS64ScenePasses` (`rt64_framebuffer_renderer_rs64.cpp`) |
| Per-draw flags the parser sets (`rs64NoCast`, `rs64NoCastCutout`, `rs64EmissiveXlu`) | Filled during the parse in [rt64_gbi_f3dfactor5.cpp](../../lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp) (`f5_push_mesh_presets`), cleared per workload in `hle/rt64_workload.cpp` |
| The per-frame object BLAS / the terrain BLAS | `recordRS64ShadowScene` and `recordRS64TerrainBLAS` in `rt64_framebuffer_renderer_rs64.cpp`; the terrain map comes from `f5_refresh_terrain_map` in the F5 GBI |
| Colours the reflection and GI rays see | `buildRS64HitColors` in `rt64_framebuffer_renderer_rs64.cpp` |
| Lights that follow sprites or meshes | `f5_push_light`, `f5_push_sprite_presets`, `f5_push_mesh_presets` in the F5 GBI |

The [roguesquadron-raytracing skill](../../.claude/skills/roguesquadron-raytracing/SKILL.md) covers the light pipeline, coordinate spaces and preset keys in depth.

## How objects render

| To change | Edit |
| --- | --- |
| How an F5 opcode is read (geometry, chunks, calls, sprites) | [rt64_gbi_f3dfactor5.cpp](../../lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp): `op_06_strict_dl` (mesh calls), `op_05_record` (terrain tiles), the `0xBD` billboard sprite path. Opcode meanings: [factor5-gbi.md](../reference/factor5-gbi.md), [f5-model-dl-spec.md](../reference/f5-model-dl-spec.md) |
| Texture loads, palettes, tiles, texture rectangles | [rt64_gbi_f5_rdpstate.cpp](../../lib/rt64/src/gbi/rt64_gbi_f5_rdpstate.cpp) (`loadTile_guarded`, `loadBlock_guarded`, TLUT, texrects), then [hle/rt64_rdp.cpp](../../lib/rt64/src/hle/rt64_rdp.cpp) and `rt64_rdp_tmem.cpp` for the TMEM model |
| Model UVs | The `03 82` texcoord scale (DMEM 0x140) handled in `rt64_gbi_f3dfactor5.cpp`; compare against `f5_dl_walk.py --tex` |
| Terrain drawing | See [Terrain renderers](#terrain-renderers) |
| What is drawn at all, culls, matrices | Game code through `rogue_squadron.toml` hooks; far-away objects are covered under [Far-distance objects](#far-distance-objects) |
| Smoothness at 60 Hz and above | Matrix and sprite ids in the F5 GBI; see the [roguesquadron-frame-interpolation skill](../../.claude/skills/roguesquadron-frame-interpolation/SKILL.md) |
| A texture's pixels (no code) | A texture pack (F4 toggles replacement) or an asset mod: [modding.md](../guides/modding.md) |

### Terrain renderers

There are two, picked by `legacyTerrain` in `roguesq_video.json` (default off) and returned by `rs64_static_terrain_mode` in [src/game/hook_helpers.cpp](../../src/game/hook_helpers.cpp) (`ROGUESQ_F5_STATIC_TERRAIN` overrides it). A terrain fix usually belongs in one of them only, so check which one the bug shows in.

| Renderer | What it is | Edit |
| --- | --- | --- |
| New (default, mode 1) | RT64 draws the whole HMP height map in one GPU pass at the first terrain record and drops the game's records. Reach and detail follow the draw distance and `terrainTessellation` | `f5_terrain_pass_record` (called from `op_05_record`), `f5_refresh_terrain_map` and `f5_terrain_colors` in [rt64_gbi_f3dfactor5.cpp](../../lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp); `recordRS64TerrainSetup` / `recordRS64TerrainFrame` / `recordRS64Terrain` in `rt64_framebuffer_renderer.cpp`; shaders `RS64TerrainVS.hlsl`, `RS64TerrainPS.hlsl`, `RS64Terrain.hlsli`, `RS64TerrainNormal.hlsli` |
| Legacy (mode 0) | The game's own terrain: its 128x128 view grid emits `op_05` tile records, which RT64 turns into geometry. Its grid breaks up past 1.3x draw distance, so turning it on clamps the setting (`kLegacyTerrainDrawDistanceMax`) | `op_05_record`, `f5_tile_grid`, `f5_tile_quad` and `f5_emit_terrain_strips` in `rt64_gbi_f3dfactor5.cpp`; the grid relocation and budget (`rs64_tgrid_base`, `rs64_tgrid_budget`) in `hook_helpers.cpp` and the stride `[[patches.instruction]]` entries in `rogue_squadron.toml`. Tile format: [f5-terrain-tile.md](../reference/f5-terrain-tile.md) |

Mode 2 (`f5_static_terrain_tile`) draws each record's own cell from the map; it is a scaffold for checking the new renderer against the legacy one cell by cell. Ray-traced shadows take terrain from the height-map BLAS (`recordRS64TerrainBLAS`) whichever renderer draws it; `ROGUESQ_RT_SHADOWS_TERRAIN=0` uses the drawn terrain instead.

### Far-distance objects

Above 1x draw distance the game would still hide objects it has not spawned yet, so the port draws stand-ins for them and widens its culls.

| To change | Edit |
| --- | --- |
| The draw distance multiplier, terrain reach, fog | `rs64_draw_dist` and the related getters in [src/game/hook_helpers.cpp](../../src/game/hook_helpers.cpp); the setting is `drawDistance` in `roguesq_video.json` ([video_config.cpp](../../src/video/video_config.cpp)) |
| Stand-ins for unspawned props, ships and ground units, and the wrecks of destroyed structures, turrets and AT-STs (which items, their pose, model, LOD) | [src/game/static_standins.cpp](../../src/game/static_standins.cpp): `rebuild` builds a model instance and prop state per DAT item, plus its wreck as the game respawns it (`build_wreck` for structures, as `attachDebrisModelVariant` does; `build_unit_wreck` for turrets and AT-STs), `rs64_standins_submit` adds the ones still unspawned to the game's visibility bucket each frame, skipping a shield whose collapse flag is set and turning the radar dish, power generator and power-up parts by mission time (`add_spins`, `pose_node`). Called from the `runInMissionFrame` 0x800FAC68 hook in `rogue_squadron.toml`; host memory is `RS64_HR_STANDINS` in [src/util/host_ram_map.h](../../src/util/host_ram_map.h). Draw-only: nothing the game reads is written |
| Objects seen only through the longer distance must not change gameplay | The `selectMeshLodAndSubmitForRender` 0x80058264 hook in `rogue_squadron.toml` / `toml_hooks.c` |
| Far matrices losing precision | The `convertMat3x4ToN64Mtx` hook in `toml_hooks.c`, which keeps far camera-space translations in a host table for RT64 |
| Face and chunk budgets that far stand-ins can exceed | `ROGUESQ_F5_ENTRY_CAP` / `ROGUESQ_F5_FACE_CAP` in the F5 GBI ([debug-trace-env-vars.md](debug-trace-env-vars.md)) |

### Textures and animated textures

Decide first whether the display list is wrong or RT64 reads it wrong. Walk the frame's list offline with `tools/debug/validate/f5_dl_walk.py --tex`, or hold the frame with the [RT64 MCP](rt64-mcp.md) (`get_draw`, `dump_texture`).

- **The list names the right texture and UVs, but it draws wrong** (wrong palette, smeared, wrong frame shown): the fix is in the GBI, in `rt64_gbi_f5_rdpstate.cpp` for loads and palettes, or `rt64_gbi_f3dfactor5.cpp` for UVs and sprites.
- **The list itself names the wrong frame or offset** (an animation runs at the wrong speed, stalls, or skips): the game picks the frame, so the fix is a `[[patches.hook]]` in `rogue_squadron.toml`. Mission texture animations and timers advance in `runInMissionFrame`; HMT materials and texture records are set up by `parseHmtMaterials` / `registerHmtTextureInTable` ([game-architecture.md](../reference/game-architecture.md), [data-structures.md](../reference/data-structures.md)). A per-frame counter must follow the frame-rate rules (`rs64_rate_tick` / `rs64_rate_scale`) in AGENTS.md's Frame pacing section.

### From a model to its texture

A model and its textures are not one block with the textures at a fixed offset. `load_hmt_and_hob` (0x8005645C) loads two files, each into its own buffer, and turns the file offsets inside each into RDRAM pointers. A face reaches its pixels through tables:

1. **HOB face**: the material index at +0x08, rewritten on load through the object's table at +0x34.
2. **HMT material**: flags (bit 0 = textured) and a `texture_id` (0xFFFF = none).
3. **Remap**: `registerHmtTextureInTable` turns the HMT's texture index into a global one through the table at `*(0x8011A444)`.
4. **Global texture table** at `0x80128F08`, 0x24 bytes per texture: width +0x08, height +0x0A, texel pointer +0x10, name +0x14.
5. **Draw**: `findOrCreateMaterial` emits a cached material display list (called with `0x06`) that sets the texture image to that pointer and loads it into TMEM.

RT64 never sees the model, only that display list, and it identifies a texture by a hash of what reached TMEM, not by its address. Layouts: [data-structures.md](../reference/data-structures.md#hob-model--hmt-material-in-ram-post-fixup), [f5-model-dl-spec.md](../reference/f5-model-dl-spec.md).

### Example: the shield dome

**Find the model.** DAT units of subtype 0x32 (handler `npcShieldUpdate` 0x8007AEF8) draw from `data/gnrc_stuff`: `shield_HOB` (a full sphere, radius 256) and `shield_h_HOB` (its top half), each with an `_HMT`. Levels 5, 9 (`Shield_P1` to `Shield_P3`, `Shield_Power`), 11, 13 and 20 place one. The RS64 Explorer's model view opens either file.

**Read the parts.** Each HOB has one object, `shield`, with one node and one part, so its mesh key is `shield#s0.p0.0`. The sphere has 200 faces and the half 100, all on material 0.

**Follow the material.** The HMT has two materials, `mat_0` and `mat_1`, both type 2: bit 0 is clear, and the texture section is empty. The chain stops at step 2: there is no global texture entry, and `emitMaterialTexturedDL` emits no texture load. The dome is untextured.

**Where its look comes from.** Each face carries per-vertex RGBA colours, such as `0x646E9D7E`: blue at about half alpha, which makes it see-through. They reach RT64 through the F5 `0x02` command (`op_02_colors` in `rt64_gbi_f3dfactor5.cpp`). The game also loads `data/shield_PAL` (16 RGBA colours) through `loadShieldPalette` (0x8007C268) into the pointer at `0x8013A970`. `npcShieldUpdate` reads that pointer in four places and builds colour fades from it: a start colour plus a per-channel step to the next colour. Where those fades are applied is not traced yet.

**So a dome change goes here:**

| Symptom or change | Where |
| --- | --- |
| Different colour, opacity or shape | The vertex colours or geometry in `shield_HOB` / `shield_h_HOB`, as an asset mod ([modding.md](../guides/modding.md)) |
| Colours are right in the display list but draw wrong | `op_02_colors` and the combiner state in the F5 GBI |
| A fade or flash runs at the wrong speed (for example above 30 fps) | A `[[patches.hook]]` on `npcShieldUpdate`, following the frame-rate rules |
| Glow under ray tracing | The `emissive_shield_dome` preset (mesh `shield`) in [default_lights.json](../../src/video/default_lights.json), already shipped |

To see it live, hold a frame in one of those levels with the [RT64 MCP](rt64-mcp.md), `pick` a dome pixel, then `get_draw`: it shows the mesh key and that no texture is bound.

## Checking a change

- Offline: `f5_dl_walk.py` and `dl_diff.py` against a golden ([f5-dl-validate skill](../../.claude/skills/f5-dl-validate/SKILL.md)).
- In game: the [RT64 MCP](rt64-mcp.md) freezes an exact frame for screenshots, draw and texture reads; F9 / Shift+F9 cycle the RT debug views (light reach, shadow casters, light visibility).
- Logs: `ROGUESQ_LOG_RT_LIGHTS=1` for light selection and keys; the rendering traces in [debug-trace-env-vars.md](debug-trace-env-vars.md).
- Lights: `cmake --build build --config Release --target rs64_unit_tests` runs `rs64_lights_test`.
