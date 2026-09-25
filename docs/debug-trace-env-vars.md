# Debug Trace Environment Variables

Trace points are gated behind environment-variable flags; most are silent on a
healthy run and high-volume when enabled. This file lists every flag, the trace
tags it controls, and when to turn it on.

## How to enable

Set the env var to a non-zero value before launching the executable:

**PowerShell / cmd**
```
set ROGUESQ_LOG_VI=1
build\Debug\RogueSquadron64Recomp.exe
```

**bash / git-bash**
```
ROGUESQ_LOG_VI=1 ./build/Debug/RogueSquadron64Recomp.exe
```

**Catch-all**: `ROGUESQ_LOG_ALL=1` enables every category at once.

To disable: unset, set to `0`, or leave undefined.

**From the command line**: every variable here can also be set without touching
the environment, by passing it to the executable as `--set NAME=VALUE` (or a
bare `NAME=VALUE`). These are equivalent:

```
RogueSquadron64Recomp.exe --set ROGUESQ_LOG_VI=1
ROGUESQ_LOG_VI=1 RogueSquadron64Recomp.exe
```

The common *user-facing* runtime options (graphics API, frame loop, audio,
headless helpers) have dedicated `--flags` instead — see `--help` and the README.
Everything below is debug/diagnostic and stays variable-only.

## Categories

| Env Var | Default | Tags Enabled | When To Enable |
|---|---|---|---|
| `ROGUESQ_LOG_ALL` | off | everything below | Quick "show me everything" — heavy output, OK for one-shot diagnostic. |
| `ROGUESQ_LOG_VI` | off | `[osViSetMode #N ...]`, `[osViSwapBuffer #N fb=...]`, `[osViSetXScale/YScale]`, `[osViBlack]` | The game's VI mode and swap calls as they reach the libultra shims. |
| `ROGUESQ_LOG_THREADS` | off | `[osDestroyThread] t=... queue=... qok=... chain_ok=...`, `[thr] vi=... start/stop/destroy t=... caller=...` | Thread teardown with the queue-chain check (a failed check is always printed), plus one line per start/stop/destroy with the target's id/priority/state/queue and the symbolized recompiled caller, for lost-thread and scheduler-deadlock diagnosis. |
| `ROGUESQ_LOG_PIPELINE` | off | `[pipe-1]`, `[pipe-2]`, `[pipe-3]` stage counters in RT64 framebuffer renderer | Per-stage TEXRECT pipeline counters (push → GPU draw). Used to verify the pipeline isn't dropping cinematic content between submission and rasterization. |
| `ROGUESQ_LOG_GBI` | off | per-handler GBI command logs | Every Factor 5 GBI handler as it fires. **Very high volume.** Use when auditing which opcodes run in a phase. |
| `ROGUESQ_LOG_GFX_TASK` | off | one line per graphics task | Low-volume task-submission trace; pair with queue diagnostics. |
| `ROGUESQ_LOG_FRAME_PROFILE` | off | `[frameprof]` per-second + `[frameprof HITCH]` per-spike | Frame period (real fps) and per-hitch phase attribution (walk / snapshot memcpy / present) — locates whether a busy-scene drop is walk-bound, GPU-bound or pacing-bound. |
| `ROGUESQ_LOG_WALK_PROFILE` | off | `[walkprof]` on walks over 20ms | Per-opcode timing inside the F5 display-list walk; dumps command count and the hottest opcodes for a slow walk. |
| `ROGUESQ_LOG_MESG_TRACE` | off | thread/message-order trace | Message-order trace for `tools/validate/compare_mesg_trace.py`. Scope to frames with `ROGUESQ_MESG_TRACE_FRAMES=lo-hi`. |
| `ROGUESQ_LOG_FRAMEQ` | off | `[frameq]` | Every send/recv on the frame-protocol queues (SP/DP done, DP event, VI event, video queue, frame mutex, task queue). The last line tells which thread stopped calling the OS. |
| `ROGUESQ_MESG_TRACE_FRAMES` | all | (companion to `ROGUESQ_LOG_MESG_TRACE`) | Frame window `lo-hi` to limit the message-order trace. |
| `ROGUESQ_DUMP_FRAME_DL` | off | one-shot display-list dump | Dump the display list for frame `N` to disk for offline `f5_dl_walk.py` inspection. Debug builds only. |
| `ROGUESQ_DUMP_TEXTURES` | off | one-shot texture dump | Write loaded textures to disk once, for asset diffing. |

## Workaround / experiment env vars (separate from logging)

These control *behavior*, not just logging. Listed here for convenience —
they're orthogonal to the log gates above but commonly co-used.

| Env Var | Default | Effect |
|---|---|---|
| `ROGUESQ_VI_FOLLOW_DRAW` | `1` | VI-presentation override picker. `0` = original VI/0x66A000 lookup. `1` (default) = override only when VI's fb is stale (not in recently-written set). `2` = aggressive — always pick most-recent color fb. `3` = freshness mode — pick whichever Framebuffer in the manager has the highest `lastWriteTimestamp`, decoupled from `colorImageAddressVector` (use when modes 1/2 don't keep VI on a fresh fb because the workload's pairs aren't `interpolationCandidate` and so the vector stays empty). Workaround for the cinematic buffer-arbiter bug. |
| `ROGUESQ_NO_MENU_PRESENT_FIX` / `ROGUESQ_MENU_FIX_STALE_MS` | off / `250` | The menu present fix scans out the GBI's most-drawn buffer at its own width when that width differs from VI's (512-wide menu vs 640/1024 VI). It only fires while that buffer received a texrect within the last N ms, so a stale 512-wide leader cannot black out a later 640-wide screen (the mission text crawl). |
| `ROGUESQ_PRIM_FF` | off | Force PRIM_COLOR RGB to (FF,FF,FF) keeping alpha. Tests whether the warm off-white tint accounts for "less saturated reds" gap from ideal. |
| `ROGUESQ_NO_SYNTH_FULLSYNC` | off | Disables the synthetic-fullsync injection in dpc_bridge.cpp. |
| `ROGUESQ_VI_FORCE_FB` | off | Diagnostic. Forces VI to present a specific RDRAM fb regardless of VI_ORIGIN. Use as `ROGUESQ_VI_FORCE_FB=0x80695C00`. Bypasses the cinematic buffer-arbiter bug to test "explosion sprites land in fb X but VI never shows X" hypotheses. Strips upper-half virtual prefix automatically. |
| `ROGUESQ_NO_FULLSCREEN_FILLRECT` | off | Diagnostic. Suppresses full-screen FillRect (rect covers entire color target). Modes: `1`/`all` = skip every full-screen FillRect (causes Memory Pak attribution to ghost-trail since clears are needed there); `cinematic` = skip only when target is cinematic color fb 0x0062B800 / 0x00695C00 (preserves attribution clears, exposes cinematic content). Tests sub-frame-overwrite hypothesis for cinematic explosions. Partial FillRects always execute. |
| `ROGUESQ_FULL_DUMP` | off | When a crash dump is written (SEH handler, SIGABRT, F12), `=1` produces a full-memory minidump (~5 GB) instead of the default lite dump. Use only when you need RDRAM contents for postmortem. |
| `ROGUESQ_DUMP_RDRAM_STATE_SETTLE` | 0 | Companion to `ROGUESQ_DUMP_RDRAM_ON_STATE`: presents a state must stay current before its dump fires. Use to capture a settled screen instead of the entry frame. |
| `ROGUESQ_ATTRIB_HOST_SLEEP` | off | A/B switch. Restores the old attribution-loop pacing (a host sleep until the next VI), which holds the cooperative run slot and could deadlock the frame-buffer arbiter (the intermittent attribution-screen freeze). The default yields through the N64 scheduler instead. |
| `ROGUESQ_WATCH_ADDRS` | off | Comma-separated hex RDRAM addresses (up to 16). Logs `[watch] vi=#N addr old -> new` whenever a watched word changes, sampled once per VI on the renderer thread (does not slow the game thread). |
| `ROGUESQ_DATA_BP` | off | Windows only. Hex RDRAM address of a word to hardware write-watch (DR0). Armed on all threads at `ROGUESQ_DATA_BP_ARM_VI` (default 1). Threads created later are not armed, and arming near a race can perturb it. Each hit logs `[data-bp] hit#N vi=#N tid=… value=… at <recompiled fn>+off (funcs_N.c:line)`, and the source line's comment gives the MIPS address. |
| `ROGUESQ_LOG_HEURISTICS` | off | Every 256 VIs, prints `[heur]` firing counts for F5 render heuristics under retirement review: op `0x0A` seen, op_01 shape-rule mismatches (only counted with `ROGUESQ_F5_PROJ_SHAPE=1`), B4 filler-guard pops, attribution de-flicker skips. |
| `ROGUESQ_F5_SKY_NO_ZWRITE` | on | Sky-dome depth pin. The game pins the dome itself (zSource=PRIM, Z_UPD, primDepth 0x7FBF); this raises primDepth to 0.99999 so distant terrain in RT64's mis-mapped fog band stays in front of the sky, and drops dome triangles that touch the near plane. Applies only to cull=BOTH draws where the game selected zSource=PRIM. `0` disables. |
| `ROGUESQ_F5_SKY_PIN_ALL` | off | A/B switch. Applies the sky pin (and the near-plane triangle drop) to every cull=BOTH draw, as before. That includes attribution and menu geometry, which the game draws with no depth. |
| `ROGUESQ_LOG_TERRAIN_TEX` | off | `[ttex]` terrain tile texture state per op-05 record (tile descriptor, clamp/mask, filter, UV span). First 40, then every 5000th. |
| `ROGUESQ_F5_TERRAIN_FULL_UV` | on | Terrain tiles span the full texture (dim texels) instead of the ucode's dim-1, removing the one-texel pattern jump at tile edges. `=0` restores the ucode span. |
| `ROGUESQ_LOG_TERRAIN_CRACKS` | off | Per graphics task, compares every terrain tile-edge point's world height with the neighbouring tile's value at the same position and logs `[tcrack]` mismatches every 5 s. Expect 0 cracks; with `ROGUESQ_F5_TERRAIN_SEAMS=0` it reports thousands (use as a control). |
| `ROGUESQ_F5_BD16` | off | A/B switch. Restores the old 0xBD sprite reading: 16-byte walk (which ran the record's texture-extent word as a command), one half-size for both axes, UVs spanning tile 0, and flip/swap flags ignored. The default follows overlay 0x2C: a 24-byte record. |
| `ROGUESQ_F5_PROJ_SHAPE` | off | A/B switch. Restores the old op_01 projection guess (byte1 == 3 or m33 == 0 && m23 != 0, read from the unresolved segmented address) in place of the ucode rule (byte1 bit0). |
| `ROGUESQ_NO_FORMAT_REPLY_FIX` | off | A/B switch. Disables the `tickFormatMessageWorker` reply-ring hook, which restores the Release-only SELECT GAME overlay dropout (medals, preview, insignia). |
| `ROGUESQ_GFX_TASK_DELAY_MS` | 0 | Sleeps this many ms before each graphics task. Timing experiments only (e.g. slow Release to Debug's frame rate). |
| `ROGUESQ_SUPPRESS_OOB_CIMG` | off | When set, drops Factor 5 ucode emissions of bogus SET_COLOR_IMAGE commands at HIGH (≥ 0x800000) and LOW (< 0x100000) addresses before they reach RT64. Reduces the iter-810 memory spike but causes a visual regression — the 3D Factor 5 logo no longer renders, since some legitimate Factor 5 lowmem CIMGs are dropped along with the garbage. |
| `ROGUESQ_FB_GUARDS` | on | Host framebuffer-window guards. Set `0` to disable for A/B comparison against hardware goldens. |
| `ROGUESQ_VI_PIXEL_ASPECT` | on | Present hi-res (512-wide) framebuffers at the VI's displayed aspect instead of square pixels. Also sets the widescreen expansion factor. `0` restores square pixels. |
| `ROGUESQ_CULL_WIDEN` | on | Widescreen: scale the game's object and terrain frustum culls by RT64's horizontal expansion so the extra width is populated. `0` disables. |
| `ROGUESQ_DRAW_DIST=<mult>` | `drawDistance` | Overrides `drawDistance` in `roguesq_video.json` (1.0-2.5, default 1.0): camera far plane, object far cull, and, via `ROGUESQ_TERRAIN_DIST`, terrain reach and fog. Also `--draw-distance`. |
| `ROGUESQ_TERRAIN_DIST=<mult>` | draw dist | Scales terrain reach and fog start/end. Capped at 2.5 (the terrain grid is 128x128 cells). |
| `ROGUESQ_LOG_MOD_FILTERS` | off | `[mod-filter] <filter>: <mod> old -> new` whenever an enabled native mod changes a game filter value (e.g. `hangar_craft_mask`). |
| `ROGUESQ_LOG_TERRAIN_GRID` | off | `[tgrid]` terrain view bounding box (cells), level cell budget, running max dimension, peak cells streamed and budget hits, every 60 grid builds. |
| `ROGUESQ_TGRID_BUDGET_MULT=<1-2>` | 2 when terrain dist >= 2, else 1 | Scales the level's terrain cell budget (and its heap pools) at level load. 3x exhausts the game heap and stalls the level load. |
| `ROGUESQ_WIDESCREEN` / `ROGUESQ_MAXIMIZED` / `ROGUESQ_WINDOW_SIZE=WxH` | off | Expand aspect to the window / start maximized / initial window size (`--widescreen`, `--maximized`, `--window-size`). |
| `ROGUESQ_F5_CHUNK_BOUND` | on | Factor 5 DL chunk-bounded fetch grammar rule. Set `0` to disable when diagnosing a DL desync. |
| `ROGUESQ_F5_NOFOG` | off | Leave RT64 fog untouched instead of applying the F5 fog words. F5 fog is two signed 16.16 words, M (moveword 8 / DMEM 0x160) and O (moveword 0x0A / DMEM 0x164); the ucode computes alpha = 255*clamp(depth*M+O, 0, 1), applied as RT64 fog mul = 255*M, offset = 255*O at float precision. |
| `ROGUESQ_F5_TERRAIN_SEAMS` | on | Grid-tile LOD seams and geomorph, as the ucode does them (overlays 0x14 rows, 0x18 columns, 0x10 interior; fixed point). Per edge (neighbour byte +4 x-min, +5 x-max, +6 z-min, +7 z-max; weights +0x16/+0x18/+0x1A/+0x1C): if the byte differs from the tile's LOD, odd samples become the mean of their neighbours and then blend toward the 4-sample line by the weight; if equal and the weight is nonzero, odd samples blend toward their neighbours' midpoint. The interior weight (+0x14) then blends odd-row/column points toward horizontal/vertical/main-diagonal midpoints. Colors follow the same steps. The game gives the finer tile the coarser tile's byte and weight, so shared edges match and LOD changes are continuous. `0` disables. |
| `ROGUESQ_F5_CHAIN_CAP` | 256 | Max chunks in one F5 next-link chain before the walker ends the display list (backstop against a stale call walking the free list). 256 = the window `f5_chunk_revisit` can still detect a cycle in. A dense frame legitimately chains ~84 chunks; the previous cap of 64 truncated those lists mid-frame and dropped a contiguous block of terrain (the LucasArts flyover hole). |
| `ROGUESQ_F5_CULL` | on | Factor 5 cull semantics in `RSP::drawIndexedTri`: only geometry-mode bit 0x2000 culls (back faces); bit 0x1000 is the ucode's texcoord-perspective flag, not G_CULL_FRONT, so it never swaps or culls; both bits = back-face cull. `0` restores the F3DEX reading (0x1000 = CULL_FRONT, both = double-sided via `ROGUESQ_F5_CULLBOTH_DRAW`). |
| `ROGUESQ_F5_NON` | off | Force `NoN` (No-Near-clipping) for the Factor 5 ucode: `1` disables the GPU hard near-plane clip and uses the far-plane manual clamp instead. F5 uniquely ships `NoN=false`, so `depthClipEnabled=true` discards large geometry as it approaches the camera (objects "culled / lower-detail up close"). A/B fix for that symptom; matches every other `.NoN` ucode. Applied in `RSP::setGBI` (`lib/rt64/src/hle/rt64_rsp.cpp`). |

### Boot / attract-demo navigation

Levers to reach a specific attract-mode demo fast (instead of the ~5-min boot → intro → menu → demo-1 →
transition → demo-2 gauntlet). All live in `update_screen` (`src/main/rt64_render_context.cpp`) and poke
game RDRAM each present, same pattern as the other pokes.

| Env Var | Default | Effect |
|---|---|---|
| `ROGUESQ_CINE_FASTFWD` | `0` | Adds N extra ticks to the VI-retrace count (`0x8011A890`) each present while the intro cutscene is active and its `gateCtr` (`0x800B0B28`) is below the end threshold (`cutscene[0x44]-0xA`). Inflates the cinematic dt so the intro completes NATURALLY in seconds instead of ~6 min headless. Not a skip — the game still sets its own done bits. Use e.g. `=200`. |
| `ROGUESQ_SKIP_DEMO` | (unset) | Pins `gGameSettings.demoId` (`0x80130B54`) = N (0-5) every present, and pins the wrap counter `unk15` (`0x80130B55`) = 0 so `cycleIdleDemoId` (`0x8006F044`) can't drift the selector — so the attract mode plays (and LOOPS) that demo directly, skipping earlier demos + transitions. `demoId`→level: **0**=Mos Eisley/Tatooine, **1**=Jade Moon, **2**=Kile II, **3**=Taloraan, **4**=Fest, **5**=Trench Run (`dLevelByDemoId` `0x800CD404` = `00 05 07 0A 0B 11`; `gDemoFilenames` `0x80109AE4`). Combine with `ROGUESQ_CINE_FASTFWD` to blast the intro. Example: `ROGUESQ_SKIP_DEMO=1 ROGUESQ_CINE_FASTFWD=200` → Jade Moon in ~2 min. |
| `ROGUESQ_SKIP_TO_JADEMOON` | off | Convenience alias for `ROGUESQ_SKIP_DEMO=1` (the Jade Moon demo). |
| `ROGUESQ_BOOT_TARGET` | (unset) | `menu` — fast-forward the intro to the front-end. `level:<id>[,craft]` — drive the game's own menus (state-gated `nav_sequencer`) into an actual mission for level `<id>` (0-0x14), optional craft (0-8). Requires no existing save (a pilot is auto-created; saves don't persist headless). `demo:<n>` — auto-disables the custom front-end menu (which displaces the attract path) and pins demo `<n>`; the chosen attract demo then plays on the front-end idle (pair with `ROGUESQ_CINE_FASTFWD`). The `level` path REPLACED the old field-poke (which stalled in mission-prep and bailed to attract). See `nav_sequencer.cpp` and `plans/2026-09-22-boot-target-nav-engine-design.md`. |

### RT64 raster enhancements

Opt-in `UserConfiguration` knobs applied in `create_render_context` (`src/main/rt64_render_context.cpp`)
before `app->setup()`. Raster-only (this build has `RT_ENABLED` off — no RT/DLSS/FSR/XeSS). Default
baseline is unchanged unless set.

| Env Var | Default | Effect |
|---|---|---|
| `ROGUESQ_MSAA` | off | Multisample anti-aliasing: `2`/`4`/`8` → MSAA 2x/4x/8x (rounds down; `<2` = None). |
| `ROGUESQ_RES_SCALE` | (RT64 2x) | Internal render-resolution multiplier; sets `resolution=Manual` + `resolutionMultiplier=<mult>` (e.g. `3.0`). Clamped by RT64's limit in `validate()`. |
| `ROGUESQ_SSAA` | `1` | Supersample downsample factor (`>=2` renders higher then downsamples — sharper, costlier). |
| `ROGUESQ_WIDESCREEN` | off | `1` → `aspectRatio=Expand` (fills the window aspect). F5 HUD/2D may need alignment work at non-4:3. |
| `ROGUESQ_ASPECT` | (unset) | Force a specific aspect ratio as a decimal `w/h` (e.g. `1.7777`); sets `aspectRatio=Manual`. Takes precedence over `ROGUESQ_WIDESCREEN`. |
| `ROGUESQ_HDR` | off | `1` → `internalColorFormat=High` (higher-precision internal color target). |
| `ROGUESQ_TEX_FILTER` | `aa` | Texture filtering: `nearest` / `linear` / `aa` (AntiAliasedPixelScaling). |
| `ROGUESQ_RT_INTERP` | off | Enable RT64 frame interpolation to `<hz>` (default 60): `refreshRate=Manual`, `refreshRateTarget=hz`. Gives smooth 60fps motion via RT64's built-in AUTO geometric matcher (validated on the Factor 5 path: smooth, nearly flicker-free) with no per-object id stamping needed. Off by default (`refreshRate=Original` = no interpolation). |
| `ROGUESQ_INTERP_VARIABLE` | on | Frame interpolation times each real frame by its own VI count (VIs between workload submissions) instead of the averaged rate, so a frame the game stretched to 3-4 VIs tweens over that long instead of lurching. `=0` restores the fixed-rate scheduler. `ROGUESQ_LOG_INTERP_TICKS=1` logs non-2-VI frames. |
| `ROGUESQ_F5_SPRITE_INTERP` | off | `1` lets frame interpolation pair F5 billboard sprites (smoke/fire/explosions). Off by default: pooled particles have no stable identity, so pairing flickers; they draw on a non-interpolated transform at the game's rate. |
| `ROGUESQ_LOG_VI_FACTORS` | off | `[vifactor]` histogram and sequence of VIs per presented frame every 120 frames. |
| `ROGUESQ_TEXTURE_PACK` | (unset) | Load an RT64 texture-replacement pack (a directory or `.zip` containing `rt64.json` + DDS/PNG) and enable replacements. F5 loads textures through the normal RDP TMEM path so RT64's content hashes are stable — packs resolve exactly as for a stock-ucode game. Applied after `app->setup()` via `textureCache->loadReplacementDirectories`. Authoring a pack uses RT64's developer dump workflow (see Zelda64Recomp's texture-pack tooling); the loader here is the runtime consumer. |

### Input / controls

Keyboard, mouse, and gamepad bindings load from `roguesq_input.json` next to the
exe (written with defaults on first run). Rebind in-app via **F1 then F6**
(the Controls window). Mouse-steering capture is automatic while the window is
focused (released on defocus, when the Controls window is open, or when the F1
inspector is up).

| Env Var | Default | Effect |
|---|---|---|
| `ROGUESQ_INPUT_RESET` | off | `1` overwrites `roguesq_input.json` with the default bindings at startup. |

## How to add a new debug trace

1. Pick the right category from the table above (or invent a new one and add it here).
2. At the call site, gate the `fprintf(stderr, ...)` behind a static-cached env-var check:

   **C++ (lambda + static)**
   ```cpp
   static const bool log_x = []{
       const char *a = std::getenv("ROGUESQ_LOG_ALL");
       if (a && *a && *a != '0') return true;
       const char *e = std::getenv("ROGUESQ_LOG_VI");  // your category
       return e && *e && *e != '0';
   }();
   if (log_x) {
       fprintf(stderr, "[your-tag] ...\n");
       fflush(stderr);
   }
   ```

   **C (manual init flag)**
   ```c
   static int log_x_init = 0, log_x = 0;
   if (!log_x_init) {
       const char *a = getenv("ROGUESQ_LOG_ALL");
       const char *e = getenv("ROGUESQ_LOG_VI");
       log_x = ((a && *a && *a != '0') || (e && *e && *e != '0')) ? 1 : 0;
       log_x_init = 1;
   }
   if (log_x) { fprintf(stderr, "[your-tag] ...\n"); fflush(stderr); }
   ```

3. Add a comment above the gate stating *what* the trace shows and *when* to enable it.
4. If you introduce a new env-var category (rare — prefer reusing existing ones), update both this file and `src/main/debug_logs.h`.

## What is NOT gated (intentional)

These always print regardless of env vars — don't gate them:

- **`[CRASH]`, `[ABORT]`** — crash handler / SIGABRT path. Always need these for postmortem.
- **`[CRT_REPORT]`, `[INVALID_PARAM]`** — CRT debug-report hooks; rare and serious.
- **`[Audio] SDL_OpenAudioDevice failed`, `[ROM] Imported`, `[F12] manual minidump requested`** — one-shot user-facing events.
- **`[vi-follow-draw mode=N]`** — workaround/experiment confirmation message, throttled to first 3-5 hits. Telemetry that an opt-in workaround actually fired.
- **`[RT64] setCurrentThreadName threw`** — exception that the gated success path was supposed to avoid; you want to see this.
