---
name: roguesquadron-raytracing
description: Ray-traced lighting in RogueSquadron64Recomp (RT64's RS64 light pass on top of the game's prelit N64 image). Use for ANY work on RT lights, light presets (roguesq_lights.json, src/video/default_lights.json, the F1 Lights tab), mesh / sprite / level / lamp presets, explosion and effect-sprite lights, engine exhausts, blinking lamps, the RS64 Explorer's Lights panel and shaded preview, RT shadows, or when a light "does not show", "is in the wrong place", "does not blink", "is too big", or the GPU hangs after a lighting change. Covers the pipeline (F5 parse -> Candidate -> Resolved -> 32 nearest -> RS64LightsPS), every coordinate space and unit, how presets key onto meshes / sprites / levels, the game data behind effects and lamps, the explorer side, and a debugging method that avoids build-run-guess loops. Triggers on: ROGUESQ_RT_LIGHTS, ROGUESQ_LOG_RT_LIGHTS, fillRS64Lights, rs64lights::, RS64LightsPS, PresetLights, spr:, meshKeyName, cameraUnits, lamp preset, glw hardpoint, LAS texture, lights.js, rtLit, lightsPreviewTick. Complements roguesquadron-debug (crash/hang triage), rt64-integration (renderer internals) and renderdoc-mcp (GPU captures).
---

# RogueSquadron64Recomp — Ray-Traced Lighting

Audience: AI agents. Read `AGENTS.md` first (project map, env vars, dead ends), then this. The field reference for players and modders is `docs/engine/debug-trace-env-vars.md` (row `roguesq_lights.json`) and
`docs/guides/modding.md` (Light mods).

## 0. How to work (read this first, it is where time was lost)

1. **Get a frozen frame before theorizing.** The game is not deterministic without an input recording, and
   flying around with `press` rarely puts the thing you need on screen (`stick [-0.6, y]` turns forever: you fly
   in circles). Use, in order of preference:
   - a cutscene clock: `pause_at(clock="cutscene", frame=N)`; the boot intro (`id19_special`, no boot target) has the
     Factor 5 explosion at cutscene ~150, bright snow (hard to see warm light; good for vivid test colours);
   - a mission clock with a known view: `ROGUESQ_BOOT_TARGET=level:13` (Raid on Sullust, dark lava, fireballs in view
     around mission 400 on most runs), `level:3` (Defection at Corellia, buildings with blinking lamps, night);
   - an input recording (`tools/debug/recordings/`, `capture-frames.ps1`) when you need the exact same frame twice.
   `cutscene:<n>,<k>` boot targets can fall through to an attract demo; check `frame_info().state`.
2. **Print the two things you are comparing, side by side, on that frame, once.** When a match / position /
   transform is wrong, do not rebuild-and-retry with a guessed fix. Add ONE log line that prints both values (e.g.
   the projected light position and the lamp mark in the same space, with the screen map), read it, fix once.
   Three build-run cycles that each test a guess cost far more than one diagnostic.
3. **Verify visually with an unmistakable test light**: `lights_set` a preset with `diffuseColor [0, 6, 0]`,
   radius large, then `step` 1 frame and `screenshot`; `lights_remove` and step again to see it go away. Light edits
   show after the next `step`. Read screenshots yourself (Read tool) and crop/side-by-side with PIL when comparing.
4. **Use the RT debug views** on the frozen frame: MCP `rt_debug_view(name="light reach")` (or `step=1` to cycle,
   same as F9 / Shift+F9 in game), then `step` 1 frame and `screenshot`. Views: off, shadow mask (+ self-hits, +
   cutout casters), shadow casters, soft shadow, ambient occlusion, indirect light, reflection strength / hit colour /
   hit instance, view distance, **light reach** (magenta where each light reaches: where is it, how big), **light
   visibility** (green = lit, red = shadowed). Turn it back to "off" before judging the final image. A new MCP tool needs
   the MCP server restarted; until then `call(method="rt_debug_view", params={...})` reaches it.
5. **Measure, do not eyeball**: `ROGUESQ_LOG_RT_LIGHTS=1` prints every new sprite key (`[rt-lights] sprite spr:...
   prim=`), mesh registry contents, and lamp matching (`lamps: N drawn, M lamp lights, K lit`). Grep the session log
   (`status().log_path`) instead of waiting on regexes that may never match.
6. **wait_log hygiene**: it only sees lines printed after it starts unless `from_start`; it gives up at 120 s and
   on crash markers (`fail_pattern`, pass `""` to disable). A line printed before the call is missed: grep instead.
7. **Launch hygiene**: one game at a time; `close` before rebuilding (a running exe locks `build/Debug/mods/*` and
   the link fails with MSB3073 copy errors); clean `dumps/mcp/<session>` afterwards. Test with RT features on, audio muted, from the exe dir,
   RenderDoc injected (the MCP `launch` does all of this).
8. **IDE diagnostics lag**: "undeclared identifier" right after an edit is usually stale; trust the build.
   `near`/`far` are Windows macros: never name a variable or lambda `near`.

## 1. Pipeline (game side, `lib/rt64`)

```
F5 GBI parse (gbi/rt64_gbi_f3dfactor5.cpp, rt64_gbi_f5_rdpstate.cpp)
  -> rs64lights::Candidate pushed to workload.rs64Lights (f5_push_light, max 512)
render thread: FramebufferRenderer::fillRS64Lights (render/rt64_framebuffer_renderer.cpp)
  -> resolve each Candidate to camera space (world[transformIndex], or CameraSpace)
  -> level presets (map space -> camera via the terrain frame), mesh/sprite presets already candidates
  -> lamp gating (lamp presets vs lamp marks), dedupe (same colour within 1 unit), keep the 32 nearest
  -> RS64LightsCB (shared/rt64_rs64_lights_cb.h) -> shaders/RS64LightsPS.hlsl (additive over the prelit frame)
```

- Light formula (RS64LightsPS): `sum += colour * intensity * (1 - d/r)^falloff * saturate((N.L + wrap)/(1 + wrap)) * spot`,
  output `sum * gain`, added to the frame (NOT multiplied by albedo). Defaults gain 1, wrap 0.3
  (`ROGUESQ_RT_LIGHTS_GAIN/WRAP`). Spot uses RT64's convention: negative cosines, lit where
  `dot(surface->light, dir) <= spotMaxCos`, fading from `spotFalloffCos`; 1 = no cone.
- Flicker: `rs64lights::flickerGain` (smooth value noise, 12 Hz, per-light seed). It dims; it cannot switch off.
- `RT_ENABLED` is never defined: RT64's own path-traced RT, Scenes/Materials tabs are compiled out. The F1 **Lights**
  tab was moved out of that guard and drives OUR pass. Im3d gizmos are projected into ImGui's background list
  (`rt64_inspector.cpp endFrame`), RT64's Im3d renderer is dead D3D12 code.
- RT64 parses from an **RDRAM snapshot** (`events.cpp take_rdram_snapshot`): the first 8 MB plus ranges registered
  with `rs64_snapshot_add_range`. Host tables above 8 MB (language mod: texture record table at 0x80C54000, texture
  pools 0x80CE4000+) must be registered or they read as zeros. `f5_host_vaddr_ok` allows up to 16 MB.
- Shadows: `ROGUESQ_RT_SHADOWS`, per-light `shadowOffset` (occluders nearer than this are ignored).

## 2. Spaces and units (get this right before anything else)

| Thing | Space | Units |
|---|---|---|
| Level preset (no mesh/sprite) | level map space | 512 per terrain cell, **Y down**; explorer world = map/256 (x,z), y x HMP heightScale/16 |
| Mesh preset | the matched part's node (model) space | model units (HOB vertex units x256); radius scaled by the draw transform |
| Mesh preset + `cameraUnits` | same position | radius / shadow offset in camera (world) units: for models drawn at varying scale (laser bolts 0.25-0.5 of a probe; craft ~0.0126; vehicles ~0.2; buildings 0.25-1.3) |
| Sprite preset | at the sprite | radius = attenuationRadius x the sprite's camera size (capped `ROGUESQ_RT_LIGHTS_SPRITE_RADIUS_MAX`, 1200); brightness x the sprite's PRIM alpha |
| Front end (no terrain) | camera space | `level` codes: -1 outside missions, -2 any, -3 briefing room, -4 hangar (`workload.rs64PresetScene` wins over a terrain map: the briefing hologram draws terrain) |

One game world unit = one map unit = 1/256 explorer level-world unit (so camera-unit reach on a placed level model is exact
in the explorer; in the model view it is estimated at a flying craft's scale 0.014 / HOB `scaleDiv`).

## 3. Presets (`roguesq_lights.json`)

- Schema: RT64 `PresetLights` + RS64 fields `level`, `mesh`, `cameraUnits`, `sprite`, `lamp`
  (`preset/rt64_preset_light.h/.cpp`; JSON read/write there; F1 UI there).
- Load order (later wins by name): the built-in presets first, other enabled mods in MODS-list order, the exe-dir copy
  last (`rs64_queue_light_presets` in `src/video/rt64_render_context.cpp`). A watcher thread re-reads all of them
  within 1 s of any change (mtime of exe-dir file and `mods/*/roguesq_lights.json`) and **replaces** the library
  (`queuePresetReload`); F1's "Reload light files" does the same; unsaved F1 edits are dropped.
- Built-in defaults: `src/video/default_lights.json`, compiled into the exe (lasers, torpedo, pickup, exhausts with wingman variant
  keys, briefing hologram/ring, `fx_fire` / `fx_ion` sprite presets, 53 `glow_*` presets on `glw` hardpoints).
  Edit it through the explorer's `lightsParse/lightsWrite` (node script) to keep formatting; NEVER Python `open('w')`.

### Mesh keys
`<hob object name>#s<lodslot>.p<node>.<j>`; a preset `mesh` is `|`-separated keys or prefixes ending at `#`/`.`
(`PresetLights::meshMatches`, mirrored by explorer `lightsMeshMatches`). `rs64lights::meshKeyName`: pl_crafts/rmobil's
7-node object named "vwing" keys as `rmobil`. Craft variants (player xwing / wingman wmxwng ...) share model space and
named joints but number nodes differently: one preset lists the same joint's parts in each (explorer `CRAFT_VARIANTS`).
F1's "Pick a mesh drawn now" lists keys seen this frame.

### Sprite keys
`spr:<texture record name>` from the game's texture record table (count u16 0x80128EF0, table ptr 0x80128F08, 0x24 per
record: size u16 +0x0C, texels +0x10, name +0x14). The F5 sprite path (0xBD) and texture rectangles find the record
whose texels the last `loadBlock`/`loadTile` read (`rdp->rs64BlockAddress`). Explosion sheets register frames as
prefix + index: explo2_IMGS `explo_tex0..19`, explo2hi `explh_tex0..18`, ionexp `iexplo_tx0..5`, ionex2 `iexpl2_tx0..7`,
smkexp `sexplo_tx0`; part_spr and others keep frame names (`srp_s_01`, `halo`, ...). Explorer `spriteKeys()` computes
them. A matching preset replaces the sprite's automatic light (which takes the PRIM colour: white for fire).
Dead ends: hashing texel bytes (transparent borders collide); naming by source file (the file is freed after its
frames are converted into the texture pool; manifest +8 is not a load address).

### Lamp presets (`"lamp": true`) — the game's blinking lights
- The lamp texture is material **slot** 20, which is texture **record 19 `"halo"`** (32x32): `D_80130C08` is a u16 table of
  material-slot numbers; slots map to records through the slot table at `*(0x8011A444)` (4 bytes per slot: u16 flags,
  u16 record). Never read a slot number as a record number (that mistake gave "LAS", a HUD texture drawn as 2D texrects
  on the bottom row of the screen, and cost hours of matching against HUD elements).
- How the halo lamps reach the screen is NOT confirmed yet: no `spr:halo` was logged on lv_3 runs. First step: a frozen
  frame with lamps visible, `pick` on a lit lamp pixel, `get_draw` to see which F5 path drew it.
- Game logic (traced): `func_80071308` builds per-object lamp records (`func_80071064`, 0x38 bytes, kind = state &
  0x1F, mode +1, counter +2, slot +6); per frame `func_80071928` sets alpha = facing (dot of hardpoint dir and view)
  x mode factor (0 on, 1 fade out, 2 off, 3 fade in) x colour alpha `D_800A0B38[kind]`, skips < 4, links visible
  lamps into the effect list (camera+0x48 -> +0x20); `func_80072BE0` advances `counter += D_800A0AA0[kind][mode] x dt`,
  rolling the mode at 256. Colours per kind in `D_800A0B38` (white, red FF3C3C, green BEFFBE, yellow ...).
- RT side (built, unproven on real lamps): each `spr:halo` texture rectangle becomes a lamp mark (screen x/y, half
  width, PRIM colour/alpha) in a cross-workload store (`rs64lights::addLampMark` / `lampMarksFor`). **Workload ids are
  assigned at `State::advanceWorkload`, after parsing**: during the parse the id is `state->workloadId + 1`. Lamp preset
  lights are projected (`projectToScreen`, final NDC) and kept only if a mark covers them (`lampMatch`), taking its
  colour x preset peak and its alpha. `ROGUESQ_LOG_RT_LIGHTS` prints `lamps: N drawn, M lamp lights, K lit` with each
  mark beside the nearest projected light. If the halo lamps turn out to be 0xBD sprites, hook the mark there instead.
- Shipped state: the 53 `glow_*` presets are steady (no `lamp` flag) until the halo path is confirmed in game.

## 4. Effect sprites and explosions

- Sprite lights come from the F5 0xBD billboard path (`f5_push_sprite_presets`, `spriteLight`). Placement Candidate
  uses the sprite's transform/centre; radius from camera size.
- Intro explosion and Sullust lava fireballs both use explo/explh frames. The ground explosion's fireballs are
  part_spr `srp_f` frames, not explo2.

## 5. RS64 Explorer (`tools/web/rs64-explorer`)

- `lights.js`: panel, bulb icons, gizmo, presets on models (`meshLightsOn`), levels (level presets and placed models,
  exact scale), effects (`lightsShowFx`, bulbs following live sprites by texture `userData.sprKey`), cutscenes
  (`lightsCutFx`, scoped to the cutscene's fx group), game presets read from the granted game folder
  (`loadGameLights`, shown under the mod's; editing one makes a same-named override; delete = disabled override),
  "Lights from hardpoints" (`glw*` only: `trl*` are contrail/wingtip points, not lights).
- Shaded preview: `rtLit(mat)` adds the RS64LightsPS sum to opaque node materials' `outputNode`;
  `lightsPreviewTick` fills 32 uniform slots from collected icons, nearest to the camera first.
  **three r170 gotcha: `THREE.uniform(0, 'int')` uploads float bits; a loop bound of 1 read as ~1e9 and hung the GPU
  (device lost, Chrome then dropped WebGPU).** Use a float uniform -> `int(min(count, MAX))`, always cap loops.
- Effects timeline holds at 0 until `renderer.compileAsync` resolves and treats frames > 50 ms as stalls (else first
  shader compiles skip an effect's opening frames).
- Testing in the browser: serve from the repo root (`python -m http.server 8765`), drive with chrome-devtools MCP;
  the folder picker cannot be automated, so stand in for the game folder with OPFS
  (`navigator.storage.getDirectory()`) and store it as IndexedDB `gameDir`. Reload with `ignoreCache` after changing
  `index.html` (only scripts are cache-busted). Clean up OPFS/IDB and servers afterwards.
- UI rules (AGENTS.md "Minimal UI controls"): icons with tooltips, on/off box left of the section heading, delete
  icon apart at the far right.

## 6. Practice from shipped games (what the design follows)

- Lights are authored data, not code: presets keyed to content (meshes, sprite frames, levels) so artists/modders
  tune without rebuilding; hot reload makes tuning a seconds-long loop.
- Budget: cap the light list (32 nearest), dedupe coincident lights, cap per-light reach (sprite max radius) so a
  close explosion cannot light the whole screen.
- Tie emissive visuals to their lights (sprite lights fade with PRIM alpha; lamp lights follow the lamp draw) instead
  of steady approximations, or the lighting lies about what the player sees.
- Keep every knob tunable at runtime (env vars, presets) and every heuristic switchable for A/B.

## 7. Files

`lib/rt64/src/hle/rt64_rs64_lights.h` (Candidate/Resolved, config, keys, lamp store, screen maps),
`lib/rt64/src/render/rt64_framebuffer_renderer.cpp` (`fillRS64Lights`), `lib/rt64/src/shaders/RS64LightsPS.hlsl`,
`lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp` (mesh registry, sprite path, `f5_sprite_key`, `f5_lamp_light`),
`lib/rt64/src/gbi/rt64_gbi_f5_rdpstate.cpp` (texrect handler), `lib/rt64/src/hle/rt64_rdp.cpp` (load addresses),
`lib/rt64/src/preset/rt64_preset_light.*`, `lib/rt64/src/hle/rt64_state.cpp` (Lights tab, preset merge),
`src/video/rt64_render_context.cpp` (file reader + watcher), `src/video/default_lights.json`, `tests/rs64_lights_test.cpp`,
`tools/web/rs64-explorer/{lights.js,effects.js,rs64assets.js}` + `tests/test_lights.mjs`.
