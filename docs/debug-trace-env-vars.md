# Debug Trace Environment Variables

The recompile accumulates many `fprintf(stderr, "[tag] ...")` trace points
during bug investigation. Most fire dozens or hundreds of lines per second
and are silent on a healthy run. To keep the terminal usable they are
gated behind environment-variable flags. This file lists every flag, what
trace tags it controls, where the code lives, and when you'd want to turn
it on.

## How to enable

Set the env var to a non-zero value before launching the executable:

**PowerShell / cmd**
```
set ROGUESQ_LOG_DPC=1
build\Debug\RogueSquadron64Recomp.exe
```

**bash / git-bash**
```
ROGUESQ_LOG_DPC=1 ./build/Debug/RogueSquadron64Recomp.exe
```

**Catch-all**: `ROGUESQ_LOG_ALL=1` enables every category at once.

To disable: unset, set to `0`, or leave undefined.

## Categories

| Env Var | Default | Tags Enabled | When To Enable |
|---|---|---|---|
| `ROGUESQ_LOG_ALL` | off | everything below | Quick "show me everything" — heavy output, OK for one-shot diagnostic. |
| `ROGUESQ_LOG_THREADS` | off | `[NAME] set_native_thread_name`, `[NAME] wstring`, `[NAME] SetThreadDescription`, `[RT64] Thread::setCurrentThreadName begin/done`, `[RT64] wstring ok` | Thread-name registration crashed (utf-8/utf-16 conversion, COM init). Useful only when investigating thread-naming code. |
| `ROGUESQ_LOG_THREAD_LIFECYCLE` | off | `[DEBUG] osCreateThread/osStartThread`, `[DEBUG] thread N starting`, `[Thread] _thread_func`, `[Thread] about to set name` | Thread-system bring-up, scheduler-deadlock, or "which thread is doing X" investigations. Fires ~30 lines at startup. |
| `ROGUESQ_LOG_INIT` | off | `[DEBUG] init_heap done`, `[DEBUG] calling init_saving`, `[DEBUG] init_saving done`, `[DEBUG] calling entrypoint`, `[DEBUG] entrypoint returned` | Game hangs before reaching the entrypoint — these breadcrumbs tell you whether heap setup or save-system init was the last thing to run. |
| `ROGUESQ_LOG_DPC` | off | `[task-brief]`, `[task]`, `[task#N gfx]`, `[task#N gfx-FULL]`, `[task#N setcombine]`, `[task#N movemem-idx]`, `[task#N hist]`, `[dpc] FULL_SYNC byte sent`, `[dpc] G_SETPRIM/ENV_COLOR`, `[dpc] PRIM override`, `[dpc] SET_COLOR/DEPTH/TEXTURE_IMAGE`, `[dpc-pak] *`, `[dpc-cine] ENTER`, `[dpc-64tri] ENTER`, `[trace] cinematic_drv ENTRY/BEFORE/AFTER` | Tracing what GFX commands the Factor5 LLE ucode emits. **Highest-volume category** — easily 1000+ lines/sec during cinematic. Use when investigating combiner muxes, texture loads, task pacing, or per-task tri/texrect counts. |
| `ROGUESQ_LOG_RDP_STATE` | off | `[trace] setCombine #N mux=...`, `[trace] setOtherMode #N cycleType=...`, `[texfilt] tf=...`, `[particle-mux] otherMode...` | RDP-state diagnosis — which combiner/cycleType is active when a specific draw fires. Useful when correlating "what mux runs at frame X" or "did texfilt change between expected and actual". |
| `ROGUESQ_LOG_PRESENT` | off | `[trace] RT64::Present #N swapIdx=...`, `[trace] PresentQueue::frame #N`, `[trace] Present::lookup`, `[trace] PresentQ::regfb/scratch`, `[trace] fbReg`, `[vi-status] word=...` | VI presentation / framebuffer routing investigations. The buffer-arbiter bug was diagnosed via these traces. Fires ~60 lines/sec. |
| `ROGUESQ_LOG_SP_TASKS` | off | `[sp] osSpTaskStartGo #N kind=GFX...` | Task-scheduling rate (cinematic ~30/s, normal play 1-2/frame). Useful when tracking how often the game submits GFX tasks to the RSP. |
| `ROGUESQ_HWBP` | off | `[hwbp] watching VA ...`, `[hwbp-hit #N] tid=... stack trace` | Hardware-breakpoint watchdog on rdram+0x3CBC4 — fires a symbolicated stack trace on every write to that address. Use when tracking memory corruption to find the writer. Heavy: kernel-mode debug-register churn. |
| `ROGUESQ_PROBES` | n/a | (Reserved — `[wp@*]` watchpoint probes in recompiled funcs are currently always-on but only fire on value change, so volume is naturally bounded. Mark for future gating if needed.) | — |

## Workaround / experiment env vars (separate from logging)

These control *behavior*, not just logging. Listed here for convenience —
they're orthogonal to the log gates above but commonly co-used.

| Env Var | Default | Effect |
|---|---|---|
| `ROGUESQ_SHADE_FIX` | `10` | Per-vertex SHADE attribute transform. `0` = disabled. `4-10` = various transforms (see [rt64_gbi_rdp.cpp:563-707](../../../Projects/RogueSquadron64Recomp/lib/rt64/src/gbi/rt64_gbi_rdp.cpp#L563-L707)). Mode 10 broadcasts SHADE = 1.0 (TEXEL passthrough) and is the current default that lets the cinematic render. |
| `ROGUESQ_PARTICLE_FIX` | on | Surgically rewrites alpha-D in mux `0xFC11FE23` (cinematic-particle TEXRECT setup variant) from ZERO to TEXEL0_ALPHA. See [rt64_rdp.cpp:360+](../../../Projects/RogueSquadron64Recomp/lib/rt64/src/hle/rt64_rdp.cpp#L360). Found redundant in practice — actual rendering uses a different mux. |
| `ROGUESQ_PARTICLE_DEBUG` | off | Forces alpha-D = ONE (opaque blocks) in mux `0xFC11FE23`. High-visibility diagnostic — particle rectangles render as solid colored squares. Found ineffective (mux mismatch). |
| `ROGUESQ_TEXRECT_ALPHA_FIX` | on | Rewrites alpha-A from SHADE → ONE in mux `0xFC119623` (the actual cinematic-particle render mux). Confirmed firing 10000+ times during cinematic. Whether visually effective is still being investigated. |
| `ROGUESQ_VI_FOLLOW_DRAW` | `1` | VI-presentation override picker. `0` = original VI/0x66A000 lookup. `1` (default) = override only when VI's fb is stale (not in recently-written set). `2` = aggressive — always pick most-recent color fb. `3` = freshness mode — pick whichever Framebuffer in the manager has the highest `lastWriteTimestamp`, decoupled from `colorImageAddressVector` (use when modes 1/2 don't keep VI on a fresh fb because the workload's pairs aren't `interpolationCandidate` and so the vector stays empty). Workaround for the cinematic buffer-arbiter bug. |
| `ROGUESQ_LOG_VI_FRESH` | off | Diagnostic. At each VI present, log the chosen fb's `lastWriteTimestamp` vs the freshest color fb in the manager (positive `lag` = VI is presenting a stale fb while a newer one exists). Also logs `colorImageAddressVector` size to confirm whether VI-follow modes 1/2 even have candidates to pick. Throttled to first 20 + every 60th. |
| `ROGUESQ_PRIM_FF` | off | Force PRIM_COLOR RGB to (FF,FF,FF) keeping alpha. Tests whether the warm off-white tint accounts for "less saturated reds" gap from ideal. |
| `ROGUESQ_NO_SYNTH_FULLSYNC` | off | Disables the synthetic-fullsync injection in dpc_bridge.cpp. |
| `ROGUESQ_SHADE_FALLOFF` / `ROGUESQ_SHADE_AMBIENT` / `ROGUESQ_SHADE_SLOPE` | various | Tuning knobs for SHADE_FIX modes 8-9 (env-mapped magnitude). |
| `ROGUESQ_FILLCOLOR_DEBUG` | off | Forces every FILL_COLOR write to a fixed value. `1` = red (`0xF801F801`, two 5551 reds). Pass an arbitrary 32-bit hex (`0xAABBCCDD`) for any color. Reveals where FILL-mode draws land — the cinematic uses FILL-mode for its background, so setting red shows the whole cutscene bg as red with geometry on top. Doubles as a useful sanity check that the rasterizer reaches the displayed fb. |
| `ROGUESQ_PARTICLE_VISIBLE_DEBUG` | off | Diagnostic. Rewrites the cinematic-pass mux family (low-half `0xFC127FFF` or `0xFC11FE23`) so it outputs PRIM color at alpha=1.0 unconditionally. If sprites turn into PRIM-colored blocks, the pipeline reaches the displayed fb; if no change, sprites land in a non-displayed fb or aren't being rasterized. |
| `ROGUESQ_VI_FORCE_FB` | off | Diagnostic. Forces VI to present a specific RDRAM fb regardless of VI_ORIGIN. Use as `ROGUESQ_VI_FORCE_FB=0x80695C00`. Bypasses the cinematic buffer-arbiter bug to test "explosion sprites land in fb X but VI never shows X" hypotheses. Strips upper-half virtual prefix automatically. |
| `ROGUESQ_NO_FULLSCREEN_FILLRECT` | off | Diagnostic. Suppresses full-screen FillRect (rect covers entire color target). Modes: `1`/`all` = skip every full-screen FillRect (causes Memory Pak attribution to ghost-trail since clears are needed there); `cinematic` = skip only when target is cinematic color fb 0x0062B800 / 0x00695C00 (preserves attribution clears, exposes cinematic content). Tests sub-frame-overwrite hypothesis for cinematic explosions. Partial FillRects always execute. |
| `ROGUESQ_VI_FOLLOW_INCLUDE_FILLS` | off | Restores legacy VI-follow behavior. By default, fillOnly fbpairs are excluded from the VI-follow candidate list (so VI doesn't pick a just-cleared cinematic buffer when the actual content is in a sprite-rendering fbpair earlier in the workload). Set to `1` to include fillOnly pairs again. |

## How to add a new debug trace

1. Pick the right category from the table above (or invent a new one and add it here).
2. At the call site, gate the `fprintf(stderr, ...)` behind a static-cached env-var check:

   **C++ (lambda + static)**
   ```cpp
   static const bool log_x = []{
       const char *a = std::getenv("ROGUESQ_LOG_ALL");
       if (a && *a && *a != '0') return true;
       const char *e = std::getenv("ROGUESQ_LOG_DPC");  // your category
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
       const char *e = getenv("ROGUESQ_LOG_DPC");
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
- **`[exp mode=N]`, `[particle-fix]`, `[texrect-alpha-fix]`, `[vi-follow-draw mode=N]`** — workaround/experiment confirmation messages, throttled to first 3-5 hits. Telemetry that an opt-in workaround actually fired.
- **`[RT64] setCurrentThreadName threw`** — exception that the gated success path was supposed to avoid; you want to see this.