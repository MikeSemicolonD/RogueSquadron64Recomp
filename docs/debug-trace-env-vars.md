# Debug Trace Environment Variables

Every runtime switch is a `ROGUESQ_*` environment variable. User-facing options and on-by-default kill switches get a full row (name, default, effect); diagnostic probes are listed per section as `NAME` (file) with a few words. Probes are silent on a healthy run and often high-volume when enabled. CMake build options (`RS64_*`, `ROGUESQ_NO_ITER_DEBUG`, ...) are in the [README](../README.md), not here.

## How to set

PowerShell:

```powershell
$env:ROGUESQ_LOG_VI=1
build\Debug\RogueSquadron64Recomp.exe
```

bash / git-bash:

```bash
ROGUESQ_LOG_VI=1 ./build/Debug/RogueSquadron64Recomp.exe
```

Command line, no environment needed: `--set NAME=VALUE` or a bare `NAME=VALUE` argument:

```bash
RogueSquadron64Recomp.exe --set ROGUESQ_LOG_VI=1
RogueSquadron64Recomp.exe ROGUESQ_LOG_VI=1
```

The user-facing options also have `--flags` (run with `--help`); see the first table below.

### How values are read

- Most gates are on when the variable is non-empty and does not start with `0` (`recomp::dbg::env_on` in `src/main/debug_logs.h`, and the local `env_on` in `lib/rt64`). `false` and `no` count as on for these.
- `recomp::dbg::env_flag` (`LOG_VI`, `LOG_THREADS`, `LOG_RUMBLE`, `LOG_THROTTLE`) also treats `false` and `no` as off.
- Some gates are on only when the value starts with `1` (e.g. `F5_SLOT_ID`, `F5_UNIQUE_ONLY`, `F5_NO_NEAR_CLIP`, `F5_NO_TILE_SPLIT`, `SKY_NOCULL`, `INTERP_HASH_TRIS`). Default-on kill switches mostly turn off only for a leading `0`.
- A few test only that the variable exists, so even `0` turns them on: `LOG_STATE`, `LOG_PHASE`, `LOG_UCODESRC`, `BRIDGE_WORD2`, `SKIP_DEPTH_READBACK`, `D3D12_DEBUG`, `D3D12_GPUVAL`.

Use `1` to enable and `0` to disable a default-on switch; unset a variable to return to its default.

## User options (CLI-backed)

| Env var | Flag | Default | Effect |
|---|---|---|---|
| `ROGUESQ_GFX_API` | `--gfx-api` | auto | `vulkan` or `d3d12` forces the backend. |
| `ROGUESQ_HLE_DEV_MODE` | `--[no-]hle-dev-mode` | on in Debug, off in Release | RT64 ImGui inspector on F1 (F6 = Controls window). |
| `ROGUESQ_VI_DRIVEN_LOOP` | `--[no-]vi-driven-loop` | on | Hardware VI/SP/DP frame protocol. `0` restores the host-paced loop (A/B only). |
| `ROGUESQ_F5_NATIVE` | `--[no-]f5-native` | on | Emit Factor 5 geometry. `0` parses display lists without drawing. |
| `ROGUESQ_NO_AUDIO_UCODE` | `--no-audio-ucode` | off | `1` replaces the MusyX synth with the silent stub. |
| `ROGUESQ_AUDIO_GAIN` | `--audio-gain`, `-m`/`--mute` | 0.35 | Master gain, 0.0-1.0 (out of range = 0.35). `--mute` sets 0. |
| `ROGUESQ_AUDIO_LATENCY_MS` | `--audio-latency-ms` | 60 | SDL audio buffer latency; 0 = off. |
| `ROGUESQ_DUMP_PCM` | `--dump-pcm` | off | Write the synth output to a 22050 Hz stereo WAV; `1` writes `dumps/wav/capture.wav`, any other value is the path. The header is patched every buffer, so a killed run still leaves a valid file. |
| `ROGUESQ_FAKE_CONTROLLER` | `--[no-]fake-controller` | off | Report a connected controller (headless runs). |
| `ROGUESQ_AUTO_START` | `--auto-start` | 0 | Once past the no-controller gate, pulse START every `<ms>`. |
| `ROGUESQ_HIDE_WINDOW` | `--[no-]hide-window` | off | Create the window hidden. RT64 still renders and presents; window screenshots are lost. |
| `ROGUESQ_MAXIMIZED` / `ROGUESQ_WINDOW_SIZE=WxH` | `--maximized`, `--window-size` | off | Start maximized / initial client size. |
| `ROGUESQ_WIDESCREEN` | `--[no-]widescreen` | off | `aspectRatio=Expand` (fill the window aspect). |
| `ROGUESQ_DRAW_DIST=<mult>` | `--draw-distance` | `drawDistance` in `roguesq_video.json` (1.0) | Accepts 0.25-8 (outside that = unset). Camera far plane and object far cull; terrain reach and fog follow it through `ROGUESQ_TERRAIN_DIST`, capped at 2.5. |
| `ROGUESQ_FORCE_LEVEL` / `ROGUESQ_FORCE_CRAFT` | `--force-level`, `--force-craft` | unset | Level id (0-18) / craft id (0-8) pinned while you navigate the menus yourself (no injected input, unlike `BOOT_TARGET`): from SELECT LEVEL, AVAILABLE CRAFT or the end of ENTER NAME (a new pilot auto-launches level 0) until the mission starts. Writes the same fields as `level:<id>,<craft>` (0x130B40, 0x130B70 u32, 0x130B41). |
| `ROGUESQ_BOOT_TARGET` | `--boot-target` | unset | Drive the game to a target; see [Boot / attract-demo navigation](#boot--attract-demo-navigation). |

Other user options without flags:

| Env var | Default | Effect |
|---|---|---|
| `ROGUESQ_INPUT_SEQ` / `ROGUESQ_INPUT_DELAY` | unset / 12000 | Scripted virtual controller injected at `get_n64_input` (no window focus needed): `name:holdMs:afterMs,...` with names `start a b z l r dup ddown dleft dright cup cdown cleft cright up down left right wait` (`up`..`right` = analog stick). `INPUT_DELAY` waits that many ms before the first step. |
| `ROGUESQ_NO_MENU_BUTTONS` | off | Disables the custom front-end menu (also off for a `demo:` boot target). |
| `ROGUESQ_NO_PAUSE_BUTTONS` | off | Disables the custom pause-menu buttons. |
| `ROGUESQ_NO_TOGGLE_INPLACE` | off | Toggles that replace a native slot reload the menu instead of flipping in place. |
| `ROGUESQ_INPUT_RESET` | off | `1` overwrites `roguesq_input.json` with the default bindings at startup. |
| `ROGUESQ_TEXTURE_PACK` | unset | Load an RT64 texture-replacement pack (a directory or `.zip` containing `rt64.json` + DDS/PNG) and enable replacements. F5 loads textures through the normal RDP TMEM path, so packs resolve as for a stock-ucode game. CPU-regenerated textures (e.g. the HUD radar buffer at `0x4D1BF0`) get a new hash nearly every frame and cannot be replaced. |
| `ROGUESQ_TEXTURE_DUMP_DIR` | unset | Start RT64's texture dumper at boot into `<dir>` (same as the inspector's "Start dumping textures"): one `<hash>.v5.tmem` plus `.tile.json` / `.rice.*` per distinct texture. The hash in the filename is the one a pack's `rt64.json` keys on. |
| `ROGUESQ_TEX_HASH_ALL_TMEM` | off | `1` restores upstream RT64 texture hashing. By default TMEM words written before the tile's own load hash as zero, so a partially loaded texture (e.g. probe droid lenses) hashes the same every run. Packs must be dumped with the same setting. |
| `ROGUESQ_FULL_DUMP` | off | Crash dumps (SEH, SIGABRT, F12) are full-memory (~5 GB) instead of lite. |
| `ROGUESQ_ANDROID_FRAME_RATE` | 60 | Android: display rate requested via `ANativeWindow_setFrameRate(FIXED_SOURCE)` (API 30+). `0` leaves it to the system. |
| `ROGUESQ_ANDROID_PERF_HINT` | on | Android: ADPF `APerformanceHint` session on the render threads (Gfx, RT64 Workload/Present), fed each display list's CPU time against 16.67 ms (API 33+). `0` disables. |
| `ROGUESQ_TOUCH_DEBUG` | off | Draw the touch menu tap boxes (`roguesq_touch.json`). |

### RT64 display overrides

Applied in `create_render_context` (`src/main/rt64_render_context.cpp`) after `roguesq_video.json` is loaded; each acts only when set and wins over the file. Raster-only (`RT_ENABLED` is off: no RT/DLSS/FSR/XeSS).

| Env var | Default | Effect |
|---|---|---|
| `ROGUESQ_MSAA` | file | `2`/`4`/`8` -> MSAA 2x/4x/8x (rounds down; `<2` = none). |
| `ROGUESQ_RES_SCALE` | file | Internal resolution multiplier (`resolution=Manual`), e.g. `3.0`; clamped by RT64. |
| `ROGUESQ_SSAA` | file | Downsample factor; `>=2` renders higher then downsamples. |
| `ROGUESQ_ASPECT` | unset | Force aspect `w/h` as a decimal (e.g. `1.7777`); `aspectRatio=Manual`. Wins over `ROGUESQ_WIDESCREEN`. |
| `ROGUESQ_HDR` | off | `1` -> `internalColorFormat=High`. |
| `ROGUESQ_TEX_FILTER` | file | `nearest` / `linear` / `aa` (AntiAliasedPixelScaling) for the VI upscale. `nearest` also turns three-point texture filtering on and `linear` turns it off (GPU bilinear on every tile, including G_TF_POINT sprites); `aa` leaves it. |
| `ROGUESQ_THREE_POINT` | file | `1` = N64 three-point filtering per texture (point on G_TF_POINT tiles); `0` = GPU bilinear on all. Applied after `TEX_FILTER`. |
| `ROGUESQ_RT_INTERP` | file | Frame interpolation: `0` off (`refreshRate=Original`), `<hz>` on at that target (`<20` = 60). |
| `ROGUESQ_VI_PIXEL_ASPECT` | on | Present hi-res (512-wide) framebuffers at the VI's displayed aspect instead of square pixels; also sets the widescreen expansion factor. `0` restores square pixels. |
| `ROGUESQ_CULL_WIDEN` | on | Widescreen: scale the game's object and terrain frustum culls by RT64's horizontal expansion. `0` disables. |
| `ROGUESQ_VI_FILTER` | 0 | VI-style soften radius in color-target texels in the present pass; 0 = off. |
| `ROGUESQ_VI_GAMMA` | 0 (VI gamma) | Override the present gamma (`pow(color, g)`); `< 1` brightens. |
| `ROGUESQ_VI_OVERSCAN` | 1 | Pull the sampled right edge in by N native pixels so the composite never samples a pooled target's stale last column. `0` disables. |

## Boot / attract-demo navigation

Driven from `update_screen` (`src/main/rt64_render_context.cpp`) and the nav sequencer (`src/main/nav_sequencer.cpp`); both poke game RDRAM or inject input each present.

| Env var | Default | Effect |
|---|---|---|
| `ROGUESQ_BOOT_TARGET` | unset | `menu`: fast-forward the intro to the front end. `level:<id>[,craft]`: drive the game's own menus (state-gated) into a mission for level `<id>` (0-0x14), optional craft (0-8). `abort:<id>[,craft]`: the same, then pause, ABORT MISSION, YES (cursor written at HUD+0xD68 like touch taps), and A through MISSION FAILED to SELECT LEVEL; logs `[nav] abort ...` per step (`ROGUESQ_NAV_ABORT_YES=<row>` overrides the YES row). `demo:<n>`: disables the custom front-end menu and pins demo `<n>`, which then plays on the front-end idle. `lobby:host[,level[,craft]]` / `lobby:join[,level[,craft]]`: MAIN MENU -> MULTIPLAYER -> pilot -> HOST/JOIN (address from `roguesq_net.json`) -> mission select (the client mirrors the host) -> craft select -> the mission, pressing START through its opening cutscene; logs `[nav] lobby ...`. `lobby:page` stops on the multiplayer page and hands the pad back. `cutscene:<level>[,<kind>]`: plays that cutscene (kind 0 intro, 1 outro for levels 0-15; `3,2` epilogue, `19,2` boot intro; table below) in place of the boot intro after the attribution screens, then continues to the front end; logs `[nav] boot cutscene ...`. The Calamari outro is `cutscene:15,1`. Headless needs `ROGUESQ_FAKE_CONTROLLER=1`; the level paths need no existing save (a pilot is auto-created). |
| `ROGUESQ_CINE_FASTFWD` | 400 with any `BOOT_TARGET`, else 0 | Adds N ticks to the VI-retrace count (`0x8011A890`) each present while the intro cutscene runs and its `gateCtr` (`0x800B0B28`) is below the end threshold (`cutscene[0x44]-0xA`). The intro completes through the game's own logic in seconds. |
| `ROGUESQ_SKIP_DEMO` | unset | Pins `gGameSettings.demoId` (`0x80130B54`) = N (0-5) and its wrap counter (`0x80130B55`) = 0 every present, so attract mode plays and loops that demo. 0 = Mos Eisley, 1 = Jade Moon, 2 = Kile II, 3 = Taloraan, 4 = Fest, 5 = Trench Run (`dLevelByDemoId` `0x800CD404`). Example: `ROGUESQ_SKIP_DEMO=1 ROGUESQ_CINE_FASTFWD=200` reaches Jade Moon in about 2 min. |
| `ROGUESQ_SKIP_TO_JADEMOON` | off | Alias for `ROGUESQ_SKIP_DEMO=1`. |
| `ROGUESQ_NAV_STEP_BUDGET` | 1200 | Presents per nav step before the sequencer gives up. Raise for slow builds (e.g. `12000` for Linux Debug under WSL). |
| `ROGUESQ_NAV_WATCH_CUTSCENES` | off | `1`: the nav sequencer presses nothing while a cutscene plays. |

### Cutscene targets

`ROGUESQ_BOOT_TARGET=cutscene:<level>,<kind>` values. `cinematicLoopBody(level, kind)` loads the asset `cuts/id<level>_<name>`, where kind 0 = `intro`, 1 = `extro`, 2 = `special` (`load_cutscene`, name table 0x800AF6B0), and `shouldShowCutsceneForLevelStage` accepts only kinds 0-1 for levels 0-15 plus `3,2` and `19,2`. The ROM holds exactly those 34 files: every mission has one intro and one outro, and there are two specials. The bonus levels (16-18) have none.

Mid-mission cutscenes are not cutscene files: they play in-engine inside the running mission (camera takeover, scripted actors), so this system can't reach them; use `level:<id>` and play to that mid-mission cutscene trigger.

<table>
  <thead>
    <tr><th>Level</th><th>Mission</th><th>Intro (kind 0, <code>intro</code>)</th><th>Outro (kind 1, <code>extro</code>)</th><th>Special (kind 2, <code>special</code>)</th></tr>
  </thead>
  <tbody>
    <tr><td>0</td><td>Ambush at Mos Eisley</td><td><code>cutscene:0,0</code></td><td><code>cutscene:0,1</code></td><td>-</td></tr>
    <tr><td>1</td><td>Rendezvous on Barkhesh</td><td><code>cutscene:1,0</code></td><td><code>cutscene:1,1</code></td><td>-</td></tr>
    <tr><td>2</td><td>The Search for the Nonnah</td><td><code>cutscene:2,0</code></td><td><code>cutscene:2,1</code></td><td>-</td></tr>
    <tr><td>3</td><td>Defection at Corellia</td><td><code>cutscene:3,0</code></td><td><code>cutscene:3,1</code></td><td><code>cutscene:3,2</code>: end-of-game epilogue (played after the final mission)</td></tr>
    <tr><td>4</td><td>Liberation of Gerrard V</td><td><code>cutscene:4,0</code></td><td><code>cutscene:4,1</code></td><td>-</td></tr>
    <tr><td>5</td><td>The Jade Moon</td><td><code>cutscene:5,0</code></td><td><code>cutscene:5,1</code></td><td>-</td></tr>
    <tr><td>6</td><td>Imperial Construction Yards</td><td><code>cutscene:6,0</code></td><td><code>cutscene:6,1</code></td><td>-</td></tr>
    <tr><td>7</td><td>Assault on Kile II</td><td><code>cutscene:7,0</code></td><td><code>cutscene:7,1</code></td><td>-</td></tr>
    <tr><td>8</td><td>Rescue on Kessel</td><td><code>cutscene:8,0</code></td><td><code>cutscene:8,1</code></td><td>-</td></tr>
    <tr><td>9</td><td>Prisons of Kessel</td><td><code>cutscene:9,0</code></td><td><code>cutscene:9,1</code></td><td>-</td></tr>
    <tr><td>10</td><td>Battle Above Taloraan</td><td><code>cutscene:10,0</code></td><td><code>cutscene:10,1</code></td><td>-</td></tr>
    <tr><td>11</td><td>Escape from Fest</td><td><code>cutscene:11,0</code></td><td><code>cutscene:11,1</code></td><td>-</td></tr>
    <tr><td>12</td><td>Blockade on Chandrila</td><td><code>cutscene:12,0</code></td><td><code>cutscene:12,1</code></td><td>-</td></tr>
    <tr><td>13</td><td>Raid on Sullust</td><td><code>cutscene:13,0</code></td><td><code>cutscene:13,1</code></td><td>-</td></tr>
    <tr><td>14</td><td>Moff Seerdon's Revenge</td><td><code>cutscene:14,0</code></td><td><code>cutscene:14,1</code></td><td>-</td></tr>
    <tr><td>15</td><td>The Battle of Calamari</td><td><code>cutscene:15,0</code></td><td><code>cutscene:15,1</code></td><td>-</td></tr>
    <tr><td>19</td><td>Boot/Logo Sequence</td><td>-</td><td>-</td><td><code>cutscene:19,2</code></td></tr>
  </tbody>
</table>

## Frame loop and pacing

| Env var | Default | Effect |
|---|---|---|
| `ROGUESQ_PARSE_SNAPSHOT` | on | Parse each graphics task from a copy of RDRAM taken at task start, so the game recycling DL chunks cannot change what RT64 walks. `0` parses live RDRAM. |
| `ROGUESQ_VI_WAIT_PARSE` | on only without the snapshot | VI waits for an in-flight parse. `1`/`0` overrides. |
| `ROGUESQ_SP_COMPLETE_AFTER_PARSE` | on with the VI-driven loop | Deliver SP-done only after the list is parsed, as the RSP would. |
| `ROGUESQ_HLE_PRESENT_EARLY` | on | RT64 PresentEarly mode (the cinematic stays on one VI address, so a VI-change present would never fire). `0` opts out. |
| `ROGUESQ_HLE_AUTO_FULLSYNC` | on | Flush uncommitted RT64 work after each task (F5's mid-DL full sync leaves the trailing workload growing). `0` disables. |
| `ROGUESQ_AI_CONSUMPTION_PACED` | on | Fire the AI event at the consumption cadence of the last queued buffer. `0` restores one per VI. |
| `ROGUESQ_AI_FIXED_CLOCK` | on | Free-running AI consumption clock (no per-buffer drift). `0` resyncs to now every buffer. |
| `ROGUESQ_GFX_TASK_DELAY_MS` | 0 | Sleep this long before each graphics task (timing experiments). |
| `ROGUESQ_SPEED` | 1 | Game clock multiplier (1-16): VI clock and CPU counter run N times faster. For replays under the fixed-timestep harness; 4 replays about 3.5x faster with identical hashes, 8 is too fast for `BOOT_TARGET` navigation. |
| `ROGUESQ_FB_GUARDS` | 6 | Bitmask: 1 = CIMG neutralizer (rewrites DL payload words, opt-in), 2 = retired, 4 = RT64 framebuffer-registry sanitizer. `0` disables all for A/B against goldens. |

Host-paced loop only (`ROGUESQ_VI_DRIVEN_LOOP=0`), all in `src/main/main.cpp` unless noted: `ROGUESQ_VI_BARRIER_SIGNAL` (on), `ROGUESQ_VI_RETRACE_SIGNAL` (on), `ROGUESQ_VI_FRAMESYNC_SIGNAL` (on), `ROGUESQ_VI_VIDEOQ_SIGNAL` (off) — per-VI tokens to the game's barrier queues; `ROGUESQ_GFX_BARRIER_NOBLOCK` (on, upstream_compat.cpp) — non-blocking gfx-barrier receives; `ROGUESQ_FORCE_BUFFER_PROGRESS` (on, rt64_render_context.cpp) — advance the cinematic buffer arbiter; `ROGUESQ_CINE_TARGET_FPS` (30 host-paced, 0 VI-driven, hook_helpers.cpp) — cinematic loop pacing.

Probes:
- `ROGUESQ_GFX_SYNC_SUBMIT` (events.cpp) — submit graphics tasks synchronously.
- `ROGUESQ_ATTRIBUTION_SLEEP_MS` (hook_helpers.cpp) — host sleep per attribution iteration, 0-5000.
- `ROGUESQ_CINE_HOLD_ITER` / `ROGUESQ_CINE_HOLD_MS` (hook_helpers.cpp) — park once at cinematic iteration N for MS (15000).
- `ROGUESQ_MEM_SIZE_MB` (upstream_compat.cpp) — reported RDRAM size, default 8; 4 crashes boot.
- `ROGUESQ_DUMP_ARBITER` (rt64_render_context.cpp) — log the buffer-arbiter progress pokes.
- `ROGUESQ_TRIS_SCALE` / `ROGUESQ_DRAW_SCALE` / `ROGUESQ_TEX_SCALE` (upstream_compat.cpp) — scale of the RT64 workload proxies shown on the F5 profiler HUD (64 / 256 / 256).
- `ROGUESQ_PROFILER_DUMP` (main.cpp) — sample the F5 profiler HUD's timing slots.

## Rendering and the F5 GBI

Kill switches and A/B switches (`lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp`, `rt64_gbi_f5_rdpstate.cpp`, `lib/rt64/src/hle/rt64_rsp.cpp`):

| Env var | Default | Effect |
|---|---|---|
| `ROGUESQ_F5_CHUNK_BOUND` | on | Chunk-bounded DL fetch rule. `0` disables when diagnosing a desync. |
| `ROGUESQ_F5_CHAIN_CAP` | 256 | Max chunks in one next-link chain before the walker ends the list (backstop against a stale call walking the free list). 256 is the window `f5_chunk_revisit` can still detect a cycle in; dense frames chain about 84. |
| `ROGUESQ_F5_ENTRY_CAP` / `ROGUESQ_F5_FACE_CAP` | 2048 / 4096 | Per-task chunk-entry and face budgets; a trip ends the task (hardware goldens stay under 400 / 700). |
| `ROGUESQ_F5_CHUNK_LINK_LOOSE` | off | Follow a forward-valid link even when the back-link is not written yet. |
| `ROGUESQ_F5_OP05_32` | on | 0x05 runs the 32-byte record handler (terrain tiles, sprite records). |
| `ROGUESQ_F5_BDBE` / `ROGUESQ_F5_BE16` | on / on | Install the 0xBD sprite and 16-byte 0xBE handlers; `BE16=0` maps both to no-ops. |
| `ROGUESQ_F5_SPRITES` | on | Emit 0xBD billboard sprites (fire, smoke, explosions). |
| `ROGUESQ_F5_TC_SCALE` | on | Apply the per-material `03 82` texcoord scale. `0` = legacy raw/8. |
| `ROGUESQ_F5_MESH_PRIMDEPTH` | on | zSource=PRIM mesh faces take primDepth from their own transformed verts. |
| `ROGUESQ_F5_TERRAIN` / `ROGUESQ_F5_TILES` | on / on | Draw the heightfield grid / the 0x05 terrain tile quads. |
| `ROGUESQ_F5_TERRAIN_SUB` / `ROGUESQ_F5_TERRAIN_SUB_FAR` | 2 / same as near | Subdivision for near (`shift==0`) and far (`shift>=1`) tiles. A lower far value leaves T-junction cracks at the LOD boundary. |
| `ROGUESQ_F5_TERRAIN_SEAMS` | on | Grid-tile LOD seams and geomorph as the ucode does them (overlays 0x14 rows, 0x18 columns, 0x10 interior; fixed point). Per edge (neighbour byte +4 x-min, +5 x-max, +6 z-min, +7 z-max; weights +0x16/+0x18/+0x1A/+0x1C): if the byte differs from the tile's LOD, odd samples become the mean of their neighbours and then blend toward the 4-sample line by the weight; if equal and the weight is nonzero, odd samples blend toward their neighbours' midpoint. The interior weight (+0x14) then blends odd-row/column points toward horizontal/vertical/main-diagonal midpoints. Colors follow the same steps. `0` disables. |
| `ROGUESQ_F5_TERRAIN_FULL_UV` | on | Terrain tiles span the full texture (dim texels) instead of the ucode's dim-1, removing the one-texel jump at tile edges. |
| `ROGUESQ_F5_CULL` | on | F5 cull semantics in `RSP::drawIndexedTri`: only geometry-mode bit 0x2000 culls (back faces); 0x1000 is the texcoord-perspective flag; both bits = back-face cull. `0` restores the F3DEX reading. |
| `ROGUESQ_F5_CULLBOTH_DRAW` | on | Only with `ROGUESQ_F5_CULL=0`: cull=BOTH draws double-sided instead of dropping the triangle. |
| `ROGUESQ_F5_FAR_CLIP` | on | `0` disables the F5 CPU far-plane clip (z <= w, as the ucode's trivial reject + clipper do). RT64's own depth clip sits at ndc 513/511 with the 1/1024 depth mapping, so without it geometry past the far plane renders. |
| `ROGUESQ_F5_NO_NEAR_CLIP` | off | `1` disables the CPU near/guard-band clip. |
| `ROGUESQ_F5_NON` | off | `1` forces `NoN` (GPU near clip off, far-plane manual clamp) for the F5 ucode, which ships `NoN=false`. Applied in `RSP::setGBI`. |
| `ROGUESQ_F5_DOME_NEAR_DROP` | on | Sky-dome triangles touching the near plane are dropped. `0` near-clips them. |
| `ROGUESQ_F5_SKY_NO_ZWRITE` | on | Sky depth pin. The game pins every sky draw (zSource=PRIM, Z_UPD, no Z_CMP, primDepth 0x7FBE/0x7FBF, about 0.998); this raises primDepth to 0.99999 so distant geometry and transparent materials past 0.998 are not hidden behind the sky. `0` disables. |
| `ROGUESQ_F5_CLIPRATIO` | 2 | Clip ratio (guard band) for the F5 path, 1-15. |
| `ROGUESQ_F5_FLIP_Y` | on | Hand RT64 `-vscale.y` (F5 NDC is y-down). |
| `ROGUESQ_F5_NOFOG` | off | Leave RT64 fog untouched instead of applying the F5 fog words. F5 fog is two signed 16.16 words, M (moveword 8 / DMEM 0x160) and O (moveword 0x0A / DMEM 0x164); the ucode computes alpha = 255*clamp(depth*M+O, 0, 1), applied as RT64 fog mul = 255*M, offset = 255*O at float precision. |
| `ROGUESQ_F5_NO_FOG` | off | Zero the applied F5 fog (mul and offset 0). |
| `ROGUESQ_F5_CULL_DIST=<units>` | off | Camera-space per-object distance cull: drops a model's draws when its `0x01` modelview origin is past the threshold. Terrain and effects unaffected. ~3000 = about 30% fewer hitches with visible pop-in; ~10000 = clean but marginal. |
| `ROGUESQ_F5_B4_LEN16` | off | Force the 16-byte 0xB4 path. |
| `ROGUESQ_F5_NOFILLERGUARD` | off | Disable the 0xB4 chunk-tail filler guard. |
| `ROGUESQ_F5_NO_TILE_SPLIT` | off | `1` stops splitting the draw call on a TMEM reload (the skybox one-texture bug). |
| `ROGUESQ_F5_CIMG_STRICT` | on | Reject SETCIMG with width < 16 or a non-64-byte-aligned address. |
| `ROGUESQ_F5_OVERLAY_WIDEN` | on | Grow untextured 4-vertex quads that exactly cover the frame (the pause-menu dim) 4x in plane so they cover widescreen. |
| `ROGUESQ_HLE_PATCH_ALPHA` | on | Rewrite the all-7s alpha combiner D to 1 so F5 texrects are not blended to black. |
| `ROGUESQ_ATTRIB_DEFLICKER` | on | Skip the attribution partial-frame clear so glyphs accumulate. |
| `ROGUESQ_FBTEX_FIXED_CAP` | on | Fixed-capacity framebuffer-texture descriptor set (the cinematic descriptor-heap crash). `0` = legacy growth. |
| `ROGUESQ_TGRID_MAP_CLAMP` | off | Also clamp the terrain view box to the map bounds (vanilla repeats edge tiles past the map). |

Draw distance and terrain grid (`src/main/hook_helpers.cpp`):

| Env var | Default | Effect |
|---|---|---|
| `ROGUESQ_CINE_DRAW_DIST` | `keepCutsceneDrawDistance` | `1`/`0` overrides `keepCutsceneDrawDistance` in `roguesq_video.json` (default on). On: while a cutscene plays, the draw distance, terrain reach, fog and the cutscene level's terrain cell budget use the game's own values (multiplier 1.0), so per-shot view distances stay as authored. Off: cutscenes use the player's setting. Battle Above Taloraan (level 10) always runs at 1.0: it pins its own far plane and fog and has no terrain. |
| `ROGUESQ_TERRAIN_DIST=<mult>` | draw dist | Scales terrain reach and fog start/end. Accepts 0.25-8, capped at 2.5 (the terrain grid is 128x128 cells). |
| `ROGUESQ_LOG_FOG=1` | off | One `[fog]` line per change: level, far-range base D (0x8009DEAC) and its bounds, the fog start/end/color the game set, the cutscene flag, our draw/terrain multipliers. |
| `ROGUESQ_LOG_TERRAIN_GRID` | off | `[tgrid]` terrain view bounding box (cells), level cell budget, running max dimension, peak cells streamed and budget hits, every 60 grid builds. |
| `ROGUESQ_TGRID_BUDGET_MULT=<1-2>` | 2 when terrain dist >= 2, else 1 | Scales the level's terrain cell budget (and its heap pools) at level load. 3x exhausts the game heap and stalls the level load. |

Presentation (`lib/rt64/src/hle/rt64_present_queue.cpp`, `src/main/rt64_render_context.cpp`):

| Env var | Default | Effect |
|---|---|---|
| `ROGUESQ_VI_FOLLOW_DRAW` | 1 | Picks the presented buffer when VI's is stale. `0` = VI lookup only. `1` = override only when VI's buffer is not in the recently-written set. `2` = always the most recent color buffer. `3` = the buffer with the largest `lastWriteRect`. `4` = the buffer the GBI reports as most draw-active (`g_most_drawn_fb`). Modes 1-4 need a non-empty `colorImageAddressVector`. |
| `ROGUESQ_NO_MENU_PRESENT_FIX` / `ROGUESQ_MENU_FIX_STALE_MS` | off / 250 | The menu present fix scans out the GBI's most-drawn buffer at its own width when it differs from VI's (512-wide menu vs 640/1024 VI), only while that buffer got a texrect within the last N ms. |
| `ROGUESQ_NO_MENU_VSCALE_FIX` | off | Disable halving the VI Y scale for the 512x448 interlaced menu frame. |
| `ROGUESQ_NO_TRANSITION_BLANK` | off | A/B: disable presenting black while VI shows the buffer `initVideoSubsystem` just zeroed on the CPU. |
| `ROGUESQ_PRESENT_DEPTH_AS_COLOR` | off | A/B: RT64's depth-to-color conversion when the presented buffer was last written as depth (cleared Z shows white). Default presents black. |
| `ROGUESQ_NO_VI_BLANK_MODE` | off | A/B: stop treating the zero-height mode-change VI mode (vStart == vEnd) as blank. |
| `ROGUESQ_VI_ZERO_SIZE_DRAW` | off | Draw a zero-size VI anyway (it paints the whole swapchain). |
| `ROGUESQ_FOLLOW_DRAW_KEEP_ZCLEAR` | off | A/B: let `VI_FOLLOW_DRAW` pick depth-clear passes (color image == depth image). |

Probes:
- `ROGUESQ_VI_FORCE_FB=0xADDR` (rt64_present_queue.cpp) — always present that RDRAM framebuffer.
- `ROGUESQ_FORCE_SWAP_FB` / `ROGUESQ_FORCE_VI_ADDR` / `ROGUESQ_FORCE_VI_WIDTH` (rt64_render_context.cpp) — scan out the last swap target / pin origin / width.
- `ROGUESQ_NO_FULLSCREEN_FILLRECT` (rt64_framebuffer_renderer.cpp) — skip every FillRect covering the whole target (`1`/`all`; any non-0 value). Partial FillRects always run.
- `ROGUESQ_PRESENT_EARLY_FORCE` (rt64_state.cpp, rt64_present_queue.cpp) — ignore `viVisible` for the PresentEarly history and force-advance.
- `ROGUESQ_RT64_WB_STRICT` / `ROGUESQ_RT64_WB_LOG` (rt64_state.cpp) — restrict / log framebuffer write-backs.
- `ROGUESQ_SKIP_DEPTH_READBACK` (rt64_state.cpp) — no depth copy back to RDRAM.
- `ROGUESQ_NO_COALESCE` (rt64_framebuffer_renderer.cpp) — disable merging same-pipeline draws.
- `ROGUESQ_HLE_FORCE_VISIBLE` (f3dfactor5) — magenta fill/combiner to test GPU output.
- `ROGUESQ_HLE_FORCE_COMB` / `ROGUESQ_HLE_FORCE_PRIM_OUTPUT` / `ROGUESQ_HLE_NO_AA` / `ROGUESQ_HLE_NO_CVGA` / `ROGUESQ_HLE_FORCE_OPAQUE` (rdpstate) — combiner and coverage-bit overrides for texrect visibility.
- `ROGUESQ_SKY_NOCULL` (rt64_rsp.cpp) — disable all per-triangle back-face culling.
- `ROGUESQ_F5_SPRITE_NORESTORE` (f3dfactor5) — A/B: don't restore the RDP state a sprite overrides.
- `ROGUESQ_F5_TERRAIN_HSCALE` (f3dfactor5) — extra heightfield height multiplier (1.0).

## Interpolation

| Env var | Default | Effect |
|---|---|---|
| `ROGUESQ_INTERP_RATE` | auto | `<hz>` (10-60) pins the game's logical rate; `strict` restores RT64's all-equal factor rule instead of the majority vote. |
| `ROGUESQ_INTERP_VARIABLE` | on | Time each real frame by its own VI count, so a frame stretched to 3-4 VIs tweens over that long. `0` = fixed-rate scheduler. |
| `ROGUESQ_F5_NODE_ID` | on | Pair objects by scene-node pointer (hooked in `traverseSceneGraphRecursive`). `0` falls back to RT64's auto matcher. |
| `ROGUESQ_F5_NODE_STABLE_ONLY` | off | `1`: a node gets an id only once it existed the previous frame. |
| `ROGUESQ_F5_REGRID_SNAP` / `ROGUESQ_F5_REGRID_COMP` | on / on | Terrain re-center handling. With `REGRID_COMP` the id is kept and the accumulated shift is folded into the vertices (scale learned from the first re-center, rebase every ~32 cells); with it off a re-center bumps the id generation. `REGRID_SNAP=0` disables both. |
| `ROGUESQ_F5_SPRITE_XFORM` | on | Each 0xBD sprite draws under its own transform (node transform moved to the sprite centre) so particles pair individually. |
| `ROGUESQ_F5_SPRITE_SCALE_XFORM` | on | With `SPRITE_XFORM`, build the quad at a reference size and put the size in the transform so growth interpolates. |
| `ROGUESQ_F5_SPRITE_FACE_ID` | on | Identify each sprite by the mesh face that emitted it plus its node, with a new generation on gap, flipbook restart or jump. |
| `ROGUESQ_F5_SPRITE_WORLD_MATCH` | on | Track particle sprites across frames in node-local then terrain-root space with velocity prediction. |
| `ROGUESQ_F5_DENSE_SPRITE_NOID` | on | Only with `SPRITE_XFORM=0`: a node that drew more than 2 sprites last frame gets no id. |
| `ROGUESQ_F5_SPRITE_INTERP` | off | `1` lets id-less sprites auto-pair (they flicker). |
| `ROGUESQ_F5_SPRITE_PIXEL_Z` | on | Sprites draw with per-vertex depth so depth follows interpolation. |
| `ROGUESQ_F5_PRIM_FLATTEN` / `ROGUESQ_F5_PRIM_FLATTEN_VTX` | on / on | zSource=PRIM faces are moved onto the primDepth plane and drawn with per-vertex depth (sky pins >= 0x7F00 stay PRIM); under an id'd node they get their own id with vertex interpolation. |
| `ROGUESQ_F5_BAKED_VTX_INTERP` | on | Faces under a fixed non-node matrix with no id (searchlights, glows baked in camera space) get an id and vertex interpolation. |
| `ROGUESQ_F5_CABLE_IDS` | on | Tow cable segments get interpolation ids by position from the ship end (prefix `0x1`, new generation on a gap, a re-fire or a falling segment count), so the segment at the ship tweens with it and no segment loses its pair when the cable pays out; fed by a hook at `addNpcToVisibilityBucket` entry, so it also covers the long-tow-cable mod. `=0` disables. |
| `ROGUESQ_F5_XHAIR_QUAD` | on | The crosshair rings (texrects matched to the HUD struct's ring elements) draw as camera-space quads with interpolation ids `0x80000000`/`0x80000001`, so they move at the display rate instead of the game's 30 fps. `=0` draws the texrects. |
| `ROGUESQ_INTERP_AUTO_MAX_SCREEN` | 0.2 | Auto-matched transforms further apart than this (NDC) are not paired. `0` disables. |
| `ROGUESQ_INTERP_ID_MAXJUMP` | 0 (off) | Skip id-matched pairs that moved more than frac x camera distance in one frame. |
| `ROGUESQ_INTERP_MAX_SCREEN` / `ROGUESQ_INTERP_MAX_POS` | 0.35 / 0.25 | Screen and position limits for the experimental hash-tris matcher. |

Probes and experiments: `ROGUESQ_F5_VTX_INTERP` (f3dfactor5) — interpolate per-vertex positions for id-matched objects; `ROGUESQ_F5_TERRAIN_ID` (f3dfactor5) — id terrain tiles by quantized world cell, with `ROGUESQ_F5_TERRAIN_CELL` (4.0) and `ROGUESQ_F5_TERRAIN_ABS` (on); `ROGUESQ_F5_SLOT_ID` (f3dfactor5) — id by matrix slot; `ROGUESQ_F5_UNIQUE_ONLY` (f3dfactor5) — id only nodes with a unique drawable; `ROGUESQ_INTERP_HASH_TRIS` (rt64_game_frame.cpp) — drop triangle count from the pairing hash; `ROGUESQ_LOG_INTERP_PAIR` — per-frame `[pair]`, `[jumprej]`, `[facesprite]`, `[flatten]` stats.

## RDP state and textures

Probes (`lib/rt64/src/gbi/rt64_gbi_f5_rdpstate.cpp` unless noted):
- `ROGUESQ_LOG_CI4_TMEM` + `ROGUESQ_CI4_TMEM_SRC` (default 0x555490) — TMEM-state trace for one CI4 source.
- `ROGUESQ_CI4_DESWIZZLE` — odd-row de-swizzle of the CI4 fire tiles.
- `ROGUESQ_DUMP_TEX` — dump CI4 flipbook source bytes and TMEM.
- `ROGUESQ_LOG_OTHERMODE` — raw otherMode writes and the decoded texture filter.
- `ROGUESQ_LOG_FILL` — clear color, rect and target.
- `ROGUESQ_LOG_CIMG` — texrect counts per color image.
- `ROGUESQ_FX_PROBE` — RGBA32 effect texrects and draws (also in f3dfactor5).
- `ROGUESQ_LOG_FACE_UV` (f3dfactor5) — per-face texture, UVs and matrix loads.
- `ROGUESQ_REINT_DIAG` (rt64_state.cpp) — CI reinterpretation cache hits and misses.

## Display lists, walks and desyncs

| Env var | Default | Effect |
|---|---|---|
| `ROGUESQ_DUMP_FRAME_DL=N` | off | Debug builds only (a no-op in Release). Counts graphics tasks from launch and prints the opcode streams of tasks N..N+15 to stderr: per-command detail for N and N+1, texrect count and bounding box for all. `src/main/hook_helpers.cpp`. |
| `ROGUESQ_DUMP_TEXTURES` | off | With `DUMP_FRAME_DL`: writes 0x2000 bytes of each SETTIMG source in the detailed tasks to `dumps/tex/<addr>_fmt<f>_siz<s>.bin`. Debug only. |
| `ROGUESQ_LOG_DL_HEALTH` | off | Every 10 s: `[dl-health] consume=… texrect=N followupOp=N cimgReject hi= low= past= width= window= ops: OP:count…`. Counts texrect follow-up words whose top byte is a nonzero opcode, and garbage color-image rejects (the walk-desync signal). |
| `ROGUESQ_DESYNC_TRACE` | off | Keep a ring of the last 96 walked opcodes (`rt64_interpreter.cpp`) to dump at a desync. |
| `ROGUESQ_LOG_HEURISTICS` | off | Every 256 VIs, `[heur]` firing counts for render heuristics under retirement review: op `0x0A` seen, B4 filler-guard pops, attribution de-flicker skips. |

Probes:
- `ROGUESQ_DUMP_ON_BUDGET=<path>` (f3dfactor5) — RDRAM snapshot at the first per-task budget trip.
- `ROGUESQ_F5_LINK_PROBE` (f3dfactor5) — forward-valid links with a back-link mismatch.
- `ROGUESQ_DROP_PROBE` (f3dfactor5) — dropped or no-op'd commands, deduped by opcode.
- `ROGUESQ_F5_CULL_LOG` (f3dfactor5) — sampled `[f5-cull]` distances for `F5_CULL_DIST`.
- `ROGUESQ_CHECK_CHUNKLIST` (upstream_compat.cpp) — walk the DL chunk free list at each SP task; dumps RDRAM on the first bad node.
- `ROGUESQ_MEDAL_PROBE` (hook_helpers.cpp, Debug) — color image active at each medal draw.
- `ROGUESQ_DUMP_UCODE=<path>` (rt64_render_context.cpp) — one-shot IMEM + DMEM dump.
- `ROGUESQ_DUMP_PARSE_SNAPSHOT_ON_ITER=<n>` + `ROGUESQ_DUMP_PARSE_SNAPSHOT_COUNT` (1) + `ROGUESQ_DUMP_PARSE_SNAPSHOT_PATH` (ultramodern events.cpp) — dump the exact RDRAM snapshot a parse walked.

## Plume / GPU device

| Env var | Default | Effect |
|---|---|---|
| `ROGUESQ_VK_QUEUE_PRIORITY` | `high` on Android, none elsewhere | Vulkan global queue priority: `low` / `medium` / `high` / `realtime`. |
| `ROGUESQ_NO_DEVICE_LOST_RECOVERY` | off | `1` disables rebuilding RT64 after a lost device (rebuilds back off 1 s doubling to 16 s). |
| `ROGUESQ_NO_ACQUIRE_BARRIER_FIX` | off | A/B: drop the swapchain acquire-barrier stage fix in plume Vulkan. |
| `ROGUESQ_D3D12_DEBUG` / `ROGUESQ_D3D12_GPUVAL` | off | Enable the D3D12 debug layer / plus GPU-based validation (presence only). Heavy CPU cost. |

Probes: `ROGUESQ_TEST_DEVICE_LOST_AT_SUBMIT=<n>` (plume_vulkan.cpp) — report device lost on the n-th submit once; `ROGUESQ_LOG_SWAPCHAIN` (plume_vulkan.cpp, rt64_present_queue.cpp) — acquire/present results, swapchain recreation, suspend/resume; `ROGUESQ_DESC_DIAG` (plume_d3d12.cpp, rt64_framebuffer_renderer.cpp) — descriptor-set counts; `ROGUESQ_DIAG_CAP` (rt64_diag_bounds.h) — capacity of bounded diagnostic sets and maps (5000).

## Audio

| Env var | Default | Effect |
|---|---|---|
| `ROGUESQ_SONGTABLE_FIX` | on | Pre-fill the uninitialized song table at 0x80139A00 with 0xFFFFFFFF so the intro song loads. |

Probes (`src/main/main.cpp` unless noted):
- `ROGUESQ_AUDIO_ASYNC` (ultramodern events.cpp) — run the MusyX synth without blocking the frame.
- `ROGUESQ_LOG_AUDIO_OUT` (also librecomp ai.cpp) — PCM flow, silence vs content, AI buffer addresses.
- `ROGUESQ_LOG_DMEM` — synth output buffers in DMEM.
- `ROGUESQ_LOG_UCODESRC` — live synth ucode vs the recompiled source.
- `ROGUESQ_DUMP_AUDIO_UCODE` — dump the MusyX ucode text and data once.
- `ROGUESQ_DUMP_SYNTH_FRAME=<n>` + `ROGUESQ_DUMP_SYNTH_PATH` — RDRAM image and OSTask of the n-th audio task for `tools/musyx_replay`.
- `ROGUESQ_BRIDGE_WORD2` — point task word2 at data_ptr.

## Input and menus

| Env var | Default | Effect |
|---|---|---|
| `ROGUESQ_THROTTLE` | unset | Headless tests: force the throttle lever position (`0` back / slowest, `1` forward / fastest) as if a throttle were bound. |

Keyboard, mouse and gamepad bindings load from `roguesq_input.json` next to the exe (written with defaults on first run); rebind in-app with F1 then F6. Mouse-steering capture is automatic while the window is focused.

## RDRAM dump triggers

One-shot dumps in MIPS byte order (same layout as a PJ64 dump) for `tools/validate/rdram_golden_diff.py`, from `src/main/rt64_render_context.cpp`. First match wins.

| Env var | Effect |
|---|---|
| `ROGUESQ_DUMP_RDRAM_ON_STATE=<id>[,<id>...]` | Dump `dumps/rdram_state_<id>.bin` the first present each listed game state becomes current, all in one run. Feeds `tools/validate/state_diff.ps1`. |
| `ROGUESQ_DUMP_RDRAM_STATE_SETTLE` | Presents a state must stay current before its dump fires (default 0). |
| `ROGUESQ_DUMP_RDRAM_AT_VI=<n>` | At present n. |
| `ROGUESQ_DUMP_RDRAM_ON_CINE_ITER=<n>[,<n>...]` | At cinematic loop iterations; one file per entry. |
| `ROGUESQ_DUMP_RDRAM_ON_CINE_STALL=<ms>` | When the cinematic frame counter (0x8013889C) has not moved for that long. |
| `ROGUESQ_DUMP_RDRAM_ON_SCENE=<id>[,settle]` | When `g_current_scene` has held `id` for `settle` presents (60). |
| `ROGUESQ_DUMP_RDRAM_ON_SCREEN=<n\|menu>[,settle]` | When screenState == n, or the menu overlay after the cinematic. |
| `ROGUESQ_DUMP_RDRAM_PATH` | Output path (default `dumps/rdram_<tag>.bin`); also used by `CHECK_CHUNKLIST`. |

## Logs

| Env var | Default | Tags | When to enable |
|---|---|---|---|
| `ROGUESQ_LOG_VI` | off | `[osViSetMode #N ...]`, `[osViSwapBuffer #N fb=...]`, `[osViSetXScale/YScale]`, `[osViBlack]` | The game's VI mode and swap calls as they reach the libultra shims. |
| `ROGUESQ_LOG_TRANSITION` | off | `[transition] cpu-clear fb=...`, `[transition] present vi=... chosen=... lastWrite=... branch=... black=... armWid=... drawnWid=...`, `[transition] vi-lum`, `rdp-wrote-white` | White/black flashes on transitions. `vi-lum` is the whiteness of the scanned-out buffer per VI (240 VIs before each mode-change clear and 180 after; more than ~300/768 `white` = a white flash); `rdp-wrote-white` is a mostly-white write-back, throttled per address. Every present for 120 presents after each video-mode change. `lastWrite=2` means a depth buffer was presented. |
| `ROGUESQ_LOG_THREADS` | off | `[osDestroyThread] t=... queue=... qok=... chain_ok=...`, `[thr] vi=... start/stop/destroy t=... caller=...` | Thread teardown with the queue-chain check (a failed check always prints), plus one line per start/stop/destroy with the symbolized recompiled caller. |
| `ROGUESQ_LOG_THREAD_LIFECYCLE` | off | `[Thread] _thread_func ...` | Host thread entry lines at startup (ultramodern threads.cpp). |
| `ROGUESQ_LOG_INIT` / `ROGUESQ_LOG_SP_TASKS` | off | boot-init breadcrumbs / SP task dispatch | librecomp `recomp.cpp` / `sp.cpp`. |
| `ROGUESQ_LOG_PIPELINE` | off | `[pipe-1]`, `[pipe-2]`, `[pipe-3]` | Per-stage texrect counters in the RT64 framebuffer renderer (push -> GPU draw). |
| `ROGUESQ_LOG_GBI` | off | per-handler GBI logs | Every F5 GBI handler as it fires. Very high volume. |
| `ROGUESQ_LOG_GFX_TASK` | off | one line per graphics task | Low-volume task-submission trace. |
| `ROGUESQ_LOG_TASKSUBMIT` | off | every gfx task, sampled audio tasks, first DL bytes | `upstream_compat.cpp`. F5 tasks have `data_size=0` by design. |
| `ROGUESQ_LOG_FRAME_PROFILE` | off | `[frameprof]`, `[frameprof HITCH]` | Real fps and per-hitch phase attribution (walk / snapshot memcpy / present). |
| `ROGUESQ_LOG_WALK_PROFILE` | off | `[walkprof]` | Per-opcode timing for walks over 20 ms. |
| `ROGUESQ_LOG_BATCH` | off | batching counters | Pipeline/scissor binds a per-scene sort could save. |
| `ROGUESQ_LOG_MESG_TRACE` | off | message-order trace | For `tools/validate/compare_mesg_trace.py`. Scope with `ROGUESQ_MESG_TRACE_FRAMES=lo-hi`. |
| `ROGUESQ_LOG_FRAMEQ` | off | `[frameq]` | Every send/recv on the frame-protocol queues; the last line shows which thread stopped calling the OS. |
| `ROGUESQ_LOG_PRESENT_EARLY` | off | PresentEarly matcher | Why `viHistory` stays empty or no workload matches. |
| `ROGUESQ_LOG_MENU_FIX` | off | menu present fix | Decisions of the menu present fix. |
| `ROGUESQ_LOG_ZPRIM` | off | zSource=PRIM states | Each distinct PRIM-depth draw state once (and at 1000 / 100000 hits): otherMode, Z bits, cull bits, the game's primDepth and the value sent. |
| `ROGUESQ_LOG_TERRAIN_CRACKS` | off | `[tcrack]` | Terrain tile-edge height mismatches every 5 s; expect 0 (thousands with `F5_TERRAIN_SEAMS=0`). |
| `ROGUESQ_LOG_GAMESTATE` | off | engine globals | Level, craft, cinematic stage, screen, menu id on change and every 512 presents; `gateCtr` vs `cuts44` shows whether a cinematic is advancing. |
| `ROGUESQ_LOG_STATE` / `ROGUESQ_LOG_PHASE` | off (presence) | scene state / slot dispatcher | Poller threads in `main.cpp`. |
| `ROGUESQ_LOG_HOOKS` | off | `[hook t=...]` | Debug only: stderr from hook code (`rs64_dbg_log4`). |
| `ROGUESQ_LOG_THROTTLE` | off | throttle | Positional HOTAS throttle about once a second: lever position, the craft's speed range, game vs. applied target. |
| `ROGUESQ_LOG_RUMBLE` | off | rumble | Rumble Pak motor calls, effect id (0-12), health drops with hit scale, death-spiral sustain, host output; a state line every 2 s in missions. |
| `ROGUESQ_WATCH_ADDRS` | off | `[watch] vi=#N addr old -> new` | Up to 16 comma-separated hex RDRAM addresses, sampled once per VI on the renderer thread. Addresses up to 16 MB, so relocated fields can be watched. |
| `ROGUESQ_DATA_BP` | off | `[data-bp] hit#N vi=#N tid=… value=… at <fn>+off (funcs_N.c:line)` | Windows only. Hardware read/write watch (DR0, RW=11, so readers are logged too) on one RDRAM word, armed at `ROGUESQ_DATA_BP_ARM_VI` (1) on every thread and re-armed every 30 VIs. `ROGUESQ_DATA_BP2` watches a second word; `ROGUESQ_DATA_BP_WHITE=1` records only near-white values. Arming near a race can perturb it. |

## Record / replay and co-op

Inert unless set. Record and replay are active only in real missions (never attract demos); one recording covers one mission attempt. Precedence: replay, record, delay. See [tools/recordings/README.md](../tools/recordings/README.md).

| Env var | Default | Effect |
|---|---|---|
| `ROGUESQ_INPUT_RECORD` / `ROGUESQ_INPUT_REPLAY` | off | Record controller 0 from the first poll after launch (menus included) to a file, or replay one; replay closes the game when the recording ends. Record and replay in Release. |
| `ROGUESQ_LS_RECORD` | unset | `<path.rec>`: record the next mission from frame 0 (seed forced, fixed 1/30 s dt): session header plus per-frame pad bytes and throttle. Writes the hash baseline to `<path.rec>.hash`. |
| `ROGUESQ_LS_REPLAY` | unset | `<path.rec>`: replay a recording (enter the same level and craft, e.g. with `ROGUESQ_BOOT_TARGET`); writes `<path.rec>.replay.hash` and quits at the end. |
| (both `LS_REPLAY` and `LS_RECORD`) | | Re-record: replays the first file's inputs and writes an upgraded copy to the second path (header, frames, live audio answers such as `S <frame> <0\|1>` song events), plus its hash baseline. Run at 1x. |
| `ROGUESQ_LS_KEEP_RUNNING` | unset | `1`: a replay keeps running after its last frame, to reach mission-end teardown and the ending cutscene. |
| `ROGUESQ_LS_HASH_OUT` | unset | Replay hash log path. |
| `ROGUESQ_LS_DUMP_FRAMES` | unset | `lo-hi` (or one frame): also write the raw bytes of every hashed region for those frames to `<hash log>.dump`. |
| `ROGUESQ_LS_SEED` | 1 | Seed for record and delay modes. |
| `ROGUESQ_LS_INPUT_DELAY` | 0 | Feel test: live play with local input delayed N frames, fixed dt. |
| `ROGUESQ_COOP_LOCAL` | unset | `1`: player 2 is an imposter, an allied X-wing wingman NPC spawned 1.5 units right of player 1 on the first mission frame from a fake DAT record at 0x80B40000. It flies a port of the player X-wing's handling from port 1 through control-map slot 1 (layout 0: B fire, A boost, Z brake, C-up hold rotation, C-right S-foils), is invulnerable and is targeted like any ally. Activation range and grid streaming also follow it outside cutscenes. Off = the original single player, bit-identical. A `RS64_MULTIPLAYER=OFF` build needs the `multiplayer-native` mod enabled. |
| `ROGUESQ_LS_PAD2_REPLAY` | unset | `<path.rec>` (needs `COOP_LOCAL=1`): port 1 plays a recorded port-0 run from mission frame 0, START masked. Must match the running level and craft. The hash log gains an `imposter` column. Single-player recordings' song answers stop matching once the imposter changes the mission; re-record with co-op on. |
| `ROGUESQ_MP` | unset | `host` or `join`: online co-op, ghost model. Each instance flies its own player 1 in its own world; the other player is the imposter, posed from the state that player streams at 30 Hz (dead reckoning up to 0.5 s, corrections blended over 0.1 s, snaps past 20 units). Both must enter the same level and craft; the host sends LAUNCH at `initMission` and the client checks it. The host owns the mission result, objectives and the lives pool; DAT health is mirrored both ways (drops sent as DAMAGE batches through the game's own hit). The same session starts from the front end (MAIN MENU -> MULTIPLAYER). A HELLO handshake checks protocol + `kBuildId` (`VERSION MISMATCH` otherwise). Implies `COOP_LOCAL`. Logs `[ghost]`. Pairs: `tools/lockstep/run-mp.ps1`. Details in [multiplayer.md](multiplayer.md). |
| `ROGUESQ_GHOST_OBJ_TRACE` | unset | `1`: every 150 frames, hashes of objective booleans, counts and DAT health plus the alive count, to compare the two worlds. |
| `ROGUESQ_GHOST_INTERP_MS` | 50 | Show the remote player N ms in the past between buffered states (0 = dead reckoning from the latest state). |
| `ROGUESQ_GHOST_TRACE` | unset | `<path.csv>`: per frame `ms,local_x,local_y,local_z,puppet_x,puppet_y,puppet_z`; `tools/lockstep/ghost_error.py` compares two traces. |
| `ROGUESQ_WINDOW_POS` | centered | `x,y`, `left` or `right` (either side of the primary display's centre, 8 px apart). |
| `ROGUESQ_NET_CONFIG` | `roguesq_net.json` next to the exe | Lobby config `{ "last_address": "a.b.c.d", "port": 27064 }` (JOIN address saved on each JOIN; the port both roles use unless `MP_PORT` is set). |
| `ROGUESQ_MP_ADDR` / `ROGUESQ_MP_PORT` | 127.0.0.1 / 27064 | Host address for `join`, and the UDP port. |
| `ROGUESQ_MP_UPNP` | 1 | `0`: hosting does not request a UPnP port mapping. |
| `ROGUESQ_MP_TRANSPORT` | unset | `enet`: ignore a relay transport (the steam-relay mod) and host / join over ENet. |
| `ROGUESQ_MP_CODE` | unset | Join code JOIN GAME uses over a relay transport (scripted runs). |
| `ROGUESQ_STEAM_APPID` | 480 | Steam app id the steam-relay mod initializes as. |
| `ROGUESQ_MP_PAD` | unset | `<path.rec>`: this instance's port-0 input comes from a recording. |
| `ROGUESQ_MP_SIM_LATENCY_MS` / `ROGUESQ_MP_SIM_LOSS_PCT` / `ROGUESQ_MP_SIM_JITTER_MS` | 0 / 0 / 0 | Simulated one-way latency, unreliable-send loss, and 0..N ms jitter (order kept). |
| `ROGUESQ_RELOC_GUARD` | unset | `1`: at the first present, fill the old addresses of the relocated per-player ranges (R1-R4, see AGENTS.md "Per-player data relocation") with 0xA5, then log `[reloc] write to old address X` on any write there. Pair with `ROGUESQ_DATA_BP=X` to catch the writer. |

Test knobs used by `tools/lockstep/run-mp.ps1` (flag in brackets):
- `ROGUESQ_MP_TEST_PAUSE=<frame>` [-TestPause] — press START on that frame; the world and camera keep running.
- `ROGUESQ_MP_TEST_END=<frame>:<result>` [-TestEnd] — host ends the mission (0 abort, 1 success, 2 failed, 3 out of lives).
- `ROGUESQ_MP_TEST_VERSION=<n>` [-TestVersion] — send and compare build id n.
- `ROGUESQ_MP_TEST_UNLOCK=<n>` [-TestUnlock] — host mission select shows levels up to n.
- `ROGUESQ_MP_TEST_ALLCRAFT=1` [-TestAllCraft] — set the all-crafts unlock bit (gGameSettings+0xC 0x80000).
- `ROGUESQ_MP_TEST_QUIT=<frame>` [-TestQuit] / `ROGUESQ_MP_TEST_QUIT_CLEAN=1` [-TestQuitClean] — host exits abruptly, or through QUIT (sends BYE).
- `ROGUESQ_MP_TEST_LIVES=<n>` [-TestLives] — start the shared lives pool at n.
- `ROGUESQ_MP_TEST_UPGRADE=<frame>:<hex>` [-TestUpgrade] — host ORs power-up bits (mask 0x1FE00) into its settings word on that mission frame, as a pickup does.
- `ROGUESQ_MP_TEST_PICKUP=<frame>` [-TestPickup] — host collects the first power-up that ticks from that mission frame on (needs one spawned near the flown path).
- `ROGUESQ_MP_TEST_NO_SKIP_SHARE=1` [-ClientLate] — ignore the other player's cutscene skips.

## How to add a new debug trace

1. Reuse an existing category if one fits; otherwise pick a `ROGUESQ_LOG_<AREA>` name and add it to this file.
2. Read the variable once and cache it. In `src/main`, use the helpers in `src/main/debug_logs.h`:

   ```cpp
   static const bool s_log = recomp::dbg::env_on("ROGUESQ_LOG_X");
   if (s_log) {
       fprintf(stderr, "[your-tag] ...\n");
       fflush(stderr);
   }
   ```

   In `lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp`, use its local `env_on(name, default)` the same way. In other `lib/` files, cache a one-time `std::getenv` read in a `static const` initializer.
3. Never cache "off" as a negative value that a `< 0` check re-reads (that re-runs `getenv` on every call). Bound any diagnostic set or map keyed by a changing value (`rt64diag::BoundedSet` / `BoundedMap`).
4. Add one comment above the gate saying what the trace shows.

## What is not gated (intentional)

These always print; don't gate them:

- `[CRASH]`, `[ABORT]` — crash handler / SIGABRT path.
- `[CRT_REPORT]`, `[INVALID_PARAM]` — CRT debug-report hooks; rare and serious.
- `[Audio] SDL_OpenAudioDevice failed`, `[ROM] Imported`, `[F12] manual minidump requested` — one-shot user-facing events.
- `[vi-follow-draw mode=N]` — confirmation that a follow-draw override fired, throttled.
- `[RT64] setCurrentThreadName threw` — an exception the gated success path should have avoided.
