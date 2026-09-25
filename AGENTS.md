# AGENTS.md

Guidance for AI agents working on the Rogue Squadron 64 Recompiled project — a native port of *Star Wars: Rogue Squadron* (N64, USA v1.0) built with N64Recomp + RT64. The game is playable from the first level through the credits. The current frontier is the remaining visual and pacing issues (credits text clipping, hitching, frame-interpolation quality), per-frame DL desyncs, and symbol renaming. See [Status](README.md#status-playable) in the README for the authoritative what-works snapshot before starting anything — this file assumes it.

## Project layout

```
src/main/main.cpp                       Game registration + RSP/audio/input/gfx callbacks
src/main/register_overlays.cpp          Boot-time overlay registration (all 3 .ovl.* at once)
src/main/rt64_render_context.cpp        RT64 host context; HLE send_dl, present-side fixes, boot-target driver
src/main/upstream_compat.cpp            libultra shims and scheduler overrides
src/main/hook_helpers.cpp               Host entry points for rogue_squadron.toml hooks (watchdog, pacing, matpool, DL walkers)
src/rsp/dpc_bridge.cpp                  DPC_START/DPC_END bridge into RT64
src/rsp/aspMain.cpp                     Audio RSP microcode stub
lib/N64ModernRuntime/                   Submodule — fork at MikeSemicolonD/N64ModernRuntime
  ├── librecomp/                        Recompiler runtime (overlay loading, get_function, SEH)
  └── ultramodern/                      libultra emulation (threads, mesgqueue, events)
lib/rt64/                               Submodule — fork at MikeSemicolonD/rt64
  └── src/gbi/rt64_gbi_f3dfactor5.cpp   Native Factor 5 GBI profile (the render core)
docs/                                   Project notes (GBI, DL spec, data structures, env vars)
tools/                                  PowerShell + Python diagnostic and validation helpers
build/                                  CMake out-of-source build dir
```

The recompiled MIPS code lives in-repo at `RecompiledFuncs/` — `funcs_*.c` plus `funcs.h`, `lookup.cpp`, and `recomp_overlays.inl`. It is **gitignored** (a derivative of the copyrighted ROM; generated locally, never committed) and regenerated from `rogue_squadron.toml` with `cmake --build build --config Debug --target regen_funcs`; the next build re-globs automatically (`CONFIGURE_DEPENDS`). These are auto-generated; hand-instrumenting them with diagnostic `fprintf` probes is routine, but load-bearing logic belongs in the `patches/` build (see below), not here — regeneration silently discards inline edits.

The forked submodules under `lib/` carry intentional `if(false) fprintf(...)` debug-toggle cruft and game-specific defensive guards. **Do not propose stripping these** as cleanup; they are intentional. (The inline KSEG0 pointer guards in `rogue_squadron.toml` have already been retired.)

## Build & run

```powershell
# Build (Debug is the tested configuration)
cmake --build build --config Debug --target RogueSquadron64Recomp

# Run with stdout/stderr capture (PowerShell — bash redirects don't flush before SIGTERM)
$job = Start-Job -ScriptBlock {
    Set-Location E:\Projects\RogueSquadron64Recomp\build\Debug
    & ".\RogueSquadron64Recomp.exe" *>&1
}
Start-Sleep -Seconds 30
Stop-Job $job
Receive-Job $job -Keep | Out-File -Encoding utf8 ..\probe_run.txt
Remove-Job $job
```

Ignore the `lld-link : warning : found both wmain and main; using latter` — benign (SDL2main provides wmain, our main wins).

ROM lives at `build/Debug/rogue_squadron.z64` (USA v1.0, xxHash3-64 = `0x6B66A44153594DEA`).

For a headless run that drives past the title without a controller, pass `--fake-controller --auto-start <ms>` (or the env vars `ROGUESQ_FAKE_CONTROLLER=1` / `ROGUESQ_AUTO_START=<ms>`).

## Environment variables

All trace categories are off by default. The full catalog is in
[docs/debug-trace-env-vars.md](docs/debug-trace-env-vars.md) — **read it before
adding a new `fprintf` or asking the user to enable logs.** The common
user-facing switches below also have `--flags` (run the exe with `--help`); the
env-var name is kept here since that is what the code reads and what you grep
for. Any variable can be set on the command line with `--set NAME=VALUE`.

| Env var | Effect |
|---|---|
| `ROGUESQ_GFX_API=vulkan\|d3d12` | Force the graphics API (default auto) |
| `ROGUESQ_HLE_DEV_MODE=0\|1` | RT64 ImGui inspector on F1 (default on in Debug, off in Release) |
| `ROGUESQ_VI_DRIVEN_LOOP=0` | Old host-paced frame loop instead of the hardware VI protocol (default on). The VI-driven loop matches hardware message order and is the current stability baseline |
| `ROGUESQ_F5_NATIVE=0` | Parse F5 display lists without emitting geometry |
| `ROGUESQ_F5_CHUNK_BOUND=0` | Disable the F5 DL chunk-bounded fetch rule (default on) |
| `ROGUESQ_F5_TERRAIN_SUB` / `ROGUESQ_F5_TERRAIN_SUB_FAR` | Terrain subdivision for near (`shift==0`) and far (`shift>=1`) tiles (default 2 / same as near). A lower far value leaves T-junction cracks at the LOD boundary; terrain emission is cheap (~0.3 ms/walk) |
| `ROGUESQ_F5_CULL_DIST=<units>` | **Default off.** RT64-side per-object distance cull (camera-space): drops a model's draws when its `0x01` modelview origin exceeds the threshold. Trades far-object pop-in for fewer draws. Benefit scales with aggressiveness (~3000 = ~30% fewer hitches but visible pop-in; ~10000 = visually clean but marginal). Terrain/effects unaffected. `ROGUESQ_F5_CULL_LOG=1` logs per-object distances |
| `ROGUESQ_FB_GUARDS=0` | Disable the host framebuffer-window guards for A/B against goldens |
| `ROGUESQ_NO_AUDIO_UCODE=1` | Silent audio stub instead of the MusyX synth |
| `ROGUESQ_DUMP_PCM=<path>` | Write the synth output to a 22050 Hz stereo WAV |
| `ROGUESQ_RENDER_SONG=<key>` | Force a specific song (0 = the N64-logo music) |
| `ROGUESQ_FAKE_CONTROLLER=1` / `ROGUESQ_AUTO_START=<ms>` | Headless runs: fake a controller, pulse START |
| `ROGUESQ_SUPPRESS_OOB_CIMG` | **Inactive.** Read only by the LLE `dpc_bridge` path, which graphics tasks no longer take (they go to the HLE `send_dl`). Historically dropped F5 SET_COLOR_IMAGE at HIGH/LOW addresses (regressed the 3D logo) |
| `ROGUESQ_LOG_ALL=1` | Every trace category |
| `ROGUESQ_LOG_GBI=1` / `ROGUESQ_LOG_GFX_TASK=1` | Per-handler GBI logs (high volume) / one line per graphics task |
| `ROGUESQ_LOG_MESG_TRACE=1` (+ `ROGUESQ_MESG_TRACE_FRAMES=lo-hi`) | Thread/message-order trace for `tools/validate/compare_mesg_trace.py` |
| `ROGUESQ_DUMP_FRAME_DL=N` / `ROGUESQ_DUMP_TEXTURES=1` | One-shot display-list and texture dumps |

With the inspector on, F1 toggles RT64's ImGui overlay, F3 toggles ViewRDRAM mode, F4 toggles texture replacement, and F12 writes a crash dump.

## Diagnostic artifacts

Files written to `logs/` and `dumps/crash-dumps/` during a run:

- `logs/stability/<tag>/run_N.log` — per-run stderr captured by `tools/run-stability.ps1`. Companion `summary.csv` classifies outcomes.
- `logs/stability/<tag>/memory.csv` — per-second WS / private / VM samples from `tools/measure-leak.ps1`.
- `mqdiag_NNN.txt` — no longer written (the periodic watchdog was removed). For a hang, take a full dump with `tools/dump-game.ps1` and run `tools/reconstruct-freeze.py` (game threads and queues from RDRAM) plus `tools/host-stacks.py` (symbolized host stacks; Debug build only).
- `dumps/crash-dumps/crash_YYYYMMDD_HHMMSS.dmp` — full-memory minidump from the SEH handler, the SIGABRT handler, the F12 hotkey, or external `tools/dump-game.ps1`.

## Tooling (under `tools/`)

### Crash / hang triage

1. **Crash**: a `crash_*.dmp` is written automatically by the handlers in `src/main/main.cpp`. Open in VS (File → Open → File → .dmp → Debug with Native Only).
2. **Hang**: from another shell, `pwsh tools/dump-game.ps1` captures the running process even when the window is "Not Responding". Don't use F12 if the message pump is starved — it won't fire.
3. **Triage threads**: `python tools/inspect-dump.py` lists threads in the most recent dump and tags ones running our exe code as `in_exe`. Those 2–4 are the only ones worth opening in VS; the rest are runtime workers parked in ntdll.

Note: the Windows debugger CLI (`cdb.exe`) is currently broken on this machine (`STATUS_DLL_INIT_FAILED`). Don't try to drive it from PowerShell — use VS interactively or the `minidump` Python package via `inspect-dump.py`.

### Stability / leak harness

- `tools/run-stability.ps1 -Runs N -Timeout S -Tag <label>` — N timed launches, per-run stderr, outcome classification by marker grep. `-EnvVars "A=1;B=1"` forwards debug vars. `-Runs 1` for data capture, `-Runs 3` for stability-rate.
- `tools/measure-leak.ps1 -Timeout S -Tag <label>` — single run, per-second WS/Private/VM to `memory.csv`.

### Validation harness (`tools/validate/`)

The primary correctness workflow — diff a live run against a Project64 golden rather than eyeballing screenshots:

- `capture_pj64_golden.ps1` / `capture_menu_rdram.ps1` — scripted PJ64 goldens.
- `f5_dl_walk.py` — walks a Factor 5 display list offline; `--json` for machine diff, `--tex` for texture/UV inspection. `f5_dl_ndc.py` adds NDC projection.
- `dl_diff.py` — layer-by-layer DL diff vs golden.
- `rdram_golden_diff.py` — RDRAM diff vs PJ64 (`--focus ADDR:SIZE` for field-level compare).
- `state_diff.ps1` — **state-matched** decomp-vs-PJ64 memory diff: captures the recomp's RDRAM at several game states in one run (`ROGUESQ_DUMP_RDRAM_ON_STATE=a,b,c`, keyed to the game-state classifier) and diffs each vs a matching PJ64 golden (`dumps/pj64/rdram_state_<id>.bin`), focused via `focus_of.py` (state `focus` structs → `rdram_golden_diff --focus`). The two emulations compare by *logical state*, not wall-clock. PJ64 side needs state-tagged goldens (extend `pj64_rs64_dump.js`).
- `audio_cmd_walk.py` / `audio_diff.py` — MusyX voice-command walk and diff (perception-free audio validation).
- `compare_mesg_trace.py` / `symbolize_mesg_trace.py` — message-order trace comparison (pair with `ROGUESQ_LOG_MESG_TRACE`).
- `checkpoint.ps1` — orchestrates a capture + diff checkpoint.

### Renaming (`tools/rename/`) and cross-refs (`tools/rz/`)

- `tools/rename/` — the symbol-renaming pipeline. **Run `tools/rename/lint_toml_syms.py` after every rename batch** to catch symbol drift before a regen.
- `tools/rz/rzq.py <cmd> <sym|0xADDR>` — rizin queries against the symbolized `rogue_squadron64` ELF: `xrefs`, `callees`, `disasm`, `strrefs`, `funcs`, `raw`. `--overlay mission|menu|cinematic` selects which overlay sits at 0x800A5130; `--project <file>` caches the ~25s analysis. See [tools/rz/README.md](tools/rz/README.md).

## Patches build (overriding auto-generated functions)

**Do not hand-edit `RecompiledFuncs/funcs_*.c` for defensive guards or game-logic overrides.** Those edits are regeneration-hostile and made the codebase brittle for months. Use the `patches/` build — the Zelda64Recompiled pattern, with one difference:

**We use `mips64-elf-gcc`, not `clang -target mips`.** The official LLVM Windows installer ships without the MIPS backend (`clang -print-targets | findstr mips` returns nothing on 19/20/22-rc). Linux LLVM packages do include MIPS; on Linux just `apt install clang lld make` and override `MIPS_GCC` / `MIPS_LD`. On Windows, install the [n64-tools/gcc-toolchain-mips64](https://github.com/n64-tools/gcc-toolchain-mips64/releases) prebuilt MIPS GCC 12.2.0 — the CMake config expects `E:/mips-toolchain` (override with `-DMIPS_TOOLCHAIN_DIR=...`).

### Pipeline at a glance

```
patches/npc_health_guard.c      ← MIPS-side C (game pointers, externs)
  ↓ mips64-elf-gcc -mips2 -mabi=32 -nostdinc
patches/npc_health_guard.o
  ↓ mips64-elf-ld -T patches.ld -T syms.ld
patches/patches.elf
  ↓ N64Recomp.exe ../patches.toml  (single_file_output, strict_patch_mode)
RecompiledPatches/patches.c     ← host C with recomp_func_t signatures
  ↓ clang-cl  (PatchesLib = OBJECT library, NOT static)
PatchesLib objects              ← spliced directly onto the exe link line
  ↓ ahead of RecompiledFuncs (the .lib)
RogueSquadron64Recomp.exe       ← /FORCE:MULTIPLE: object beats archive member
```

**PatchesLib MUST be an OBJECT library, not STATIC.** With patches wrapped in a
static `.lib`, `/FORCE:MULTIPLE` does not reliably let the override win an
*address-of* reference — the `func_map`'s `&getNpcCurrentHealth` (used by every
`LOOKUP_FUNC`/indirect call) bound to the RecompiledFuncs body, so the patch
silently never ran (see [plans/jade-moon-demo-freeze-plan.md](plans/jade-moon-demo-freeze-plan.md)).
An object's symbols are always on the link line and beat archive members
regardless of order — the documented Zelda pattern (patches = .obj,
RecompiledFuncs = .lib).

### Adding a new override

1. Write the function in `patches/somefile.c`, named the same as the game function (`func_80007D74`), annotated `RECOMP_PATCH`:
   ```c
   #define RECOMP_PATCH __attribute__((section(".recomp_patch")))
   RECOMP_PATCH void* func_80007D74(void) { /* ... */ }
   ```
2. Add game symbols the patch references to `patches/syms.ld`:
   ```ld
   func_8002221C  = 0x8002221C;
   heap_free_head = 0x801163B0;
   ```
   Look up addresses in `syms/rogue_squadron.syms.toml` (produced by `N64Recomp.exe rogue_squadron.toml --dump-context`).
3. Declare them `extern` in your patch C — the linker resolves the symbol; the C declaration only adds type info.
4. Build: `cmake --build build --config Debug --target PatchesLib` builds just the lib; the full build relinks everything.

### Constraints (gotchas)

- **`-nostdinc` means no libc headers.** Inline the typedefs you need:
  ```c
  typedef unsigned char uint8_t;
  typedef unsigned int  uint32_t;
  typedef unsigned long uintptr_t;   // mips32 ABI: long is 32-bit
  #define NULL ((void*)0)
  ```
- **No `printf` / `stderr`.** To call host code from a patch, declare a runtime stub at a fake `0x8FXXXXXX` address in `syms.ld` and provide the host impl in `src/main/`.
- **Signatures match the game, not the recomp.** Write the original MIPS-style signature (no `recomp_context*` arg); N64Recomp produces the host `void(uint8_t*, recomp_context*)` automatically.
- **Duplicate symbols are EXPECTED** when an override exists in both PatchesLib and RecompiledFuncs. We pass `/FORCE:MULTIPLE`; the linker takes PatchesLib's (it comes first). One warning per duplicate — fine.

### Toolchain quick-reference

| Component | Path |
|---|---|
| MIPS GCC | `E:/mips-toolchain/bin/mips64-elf-gcc.exe` |
| MIPS LD | `E:/mips-toolchain/bin/mips64-elf-ld.exe` |
| `make` | `mingw32-make` (any MinGW install) |
| N64Recomp | `build/Debug/N64Recomp.exe` — built from the `lib/N64ModernRuntime/N64Recomp` submodule via `N64RecompCLI`; used for both the patches pipeline and the main regen (`regen_funcs` target). Supports `--dump-context` + `func_reference_syms_file`. If a regen truncates `funcs.h` to ~7 lines, the exe is stale — rebuild it (`cmake --build build --config Debug --target N64RecompCLI`); see dead ends |

See [patches/README.md](patches/README.md) for the full how-to.

`patches/` is for base-game fixes. Self-contained gameplay mods are `.nrm` code mods built with RecompModTool and loaded by librecomp; see `mods/infinite-secondary/` and the "Code mods" paragraph in [docs/adding-menus-and-buttons.md](docs/adding-menus-and-buttons.md).

## Architectural quirks worth knowing

### Overlays register at boot

librecomp's section table covers all three `.ovl.*` overlays (mission / menu / cinematic), which share `ram_addr 0x800A5130`. They are all registered at boot in [src/main/register_overlays.cpp](src/main/register_overlays.cpp) via `recomp::overlays::register_overlays` — the Zelda64Recomp pattern. The per-DMA `load_overlays` callback that earlier builds patched into librecomp is **non-canonical** and was removed. If runtime DMA-driven overlay switching ever proves necessary, the correct place is a thin wrapper inside our own `load_overlays`, not a librecomp modification.

### Terrain grid is 128x128 in host RAM

The flight terrain's view grid tables (span tables, two byte tables, the per-cell pointer table) are relocated to 0x80B00000-0x80B1FFFF, host RDRAM above the game's 8 MB (0x80A00000-0x80A01FFF is the F5 renderer's vertex scratch; keep host-side tables clear of it), and their row strides are doubled by `[[patches.instruction]]` entries in `rogue_squadron.toml`. Any new code touching these tables must go through `rs64_tgrid_base`. `ROGUESQ_DRAW_DIST` scales reach (terrain capped at 2.5x); the level cell budget doubles at 2x+. See [plans/2026-09-24-terrain-grid-expansion-plan.md](plans/2026-09-24-terrain-grid-expansion-plan.md).

### Game-state model + BOOT_TARGET nav engine

A canonical game-state model classifies the current state each present from RDRAM: descriptor `state_model.toml` → `tools/state/gen_state_table.py` (CMake `gen_state_table`) → `src/main/state_table.inl`, host classifier in [src/main/game_state.cpp](src/main/game_state.cpp) (`rs64_state_current_id`), Python tools read the same TOML. `ROGUESQ_LOG_GAMESTATE=1` prints the classified state. Discriminators are verified against live RDRAM (e.g. mission = `numMissionObjectives` 0x130B17 != 0; menu = `gCurrentMenuData` 0x800CE730; menu id at 0x800CE734; pilot sub-step 0x800CE626). A **headless scripted virtual controller** (`ROGUESQ_INPUT_SEQ`, injected at `get_n64_input` **before** `resolve()` so keyboard-active runs don't swallow it) drives menus without window focus.

`ROGUESQ_BOOT_TARGET=level:<id>[,craft]` is a state-gated **nav sequencer** ([src/main/nav_sequencer.cpp](src/main/nav_sequencer.cpp)) that drives the real menus to a mission (replacing the old field-poke that jumped the state machine and bailed to attract). Never call a recompiled function from the host to force state — inject input + write fields the game's own confirm path reads (e.g. `gCurrentLevel` 0x130B70 is a **u32**, not a byte — a byte write = out-of-range id = crash). `demo:<n>` auto-disables the custom menu (it displaced the attract idle path) so the chosen attract demo plays; firing it *instantly* is unsolved (idle trigger is `(clock − lastInputFrame) > threshold`, `lastInputFrame` not locatable statically). See project memory `project_boot_target_nav_engine_2026_09_22` and `project_game_state_model_2026_09_22`, and `plans/2026-09-22-*`.

### Factor 5 GBI — custom opcodes

This game uses a Factor 5-customized F3DEX-derived ucode. The RT64 profile `GBI_F3DFACTOR5` lives in [lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp](lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp) and inherits from `GBI_F3DEX`. The canonical opcode map (with observed w0/w1 patterns and disproven interpretations) is in [docs/factor5-gbi.md](docs/factor5-gbi.md); the DL grammar itself is in [docs/f5-model-dl-spec.md](docs/f5-model-dl-spec.md). Confirmed Factor 5-specific behaviors:

| Opcode | Standard meaning | Factor 5 behavior | Handler |
|-------:|------------------|-------------------|---------|
| `0xB5` | F3DEX `G_QUAD` | **Next chunk**: continue in the chunk named by the current chunk header's first word (w1 ignored); `0xB8` is the pop/return | `op_b5_next_chunk` |
| `0xE4` | F3DEX `G_TEXRECT` | **LLE format (16 bytes)**, not HLE 24-byte | `texrectLLE_guarded` |
| `0xE5` | F3DEX `G_TEXRECTFLIP` | LLE format | `texrectFlipLLE_guarded` |
| `0xFF` | `G_SETCIMG` | Frequently emitted with bogus payloads (w1=0, fmt>4, out-of-FB addresses); rejected by `setColorImage_filtered` | `setColorImage_filtered` |
| `0x80` | unused | Chunk header (next-chunk pointer in 24-bit w0); never executed by the ucode, used by the HLE to record the chunk base | `op_80_header` |
| `0x02` | F3D `G_RDPHALF_2` | Per-vertex RGBA colors: DMA `(w0&0xFFFF)+1` bytes from w1 into DMEM 0xB70 | `op_02_colors` |

Chunks are contiguous 0x108-byte blocks. The interpreter walks chunk content linearly from +8; at +0x108, or on `0xB5`, it continues in the chunk named by the header's first word. `0x06` pushes and calls, `0x07` branches, and `0xB8` pops (or ends the task at depth 0). Models render as CI4 textures at palette bank 15. Per-face UVs are raw values scaled by the per-material `03 82` texcoord scale (16.16, DMEM 0x140); applying it fixed the old "zoomed/stretched model texture" bug (`ROGUESQ_F5_TC_SCALE=0` reverts for A/B). Judge any remaining UV defect after that scale. Use `f5_dl_walk.py --tex` to compare.

### Audio — MusyX synth and MORT voice

Rogue Squadron drives audio through Factor 5's **MusyX** engine, and **SFX and music work**: the CPU-side MusyX sequencer submits `M_AUDTASK`s, which run the **RSPRecomp'd MusyX synth ucode** (`musyx_audio_runner`, from `musyx_rsp.toml`) on the host audio-task thread; the synthesized PCM flows through `queue_samples` → SDL ([main.cpp](src/main/main.cpp)). This is the default (`get_rsp_microcode` returns `musyx_audio_runner` for `M_AUDTASK`); `ROGUESQ_NO_AUDIO_UCODE=1` falls back to the silent `musyx_stub`. Stock `aspMain` is **not** on the audio path — MusyX has no shared format with the stock ucode; `src/rsp/aspMain.cpp` is a vestigial `Broke`-returning stub kept only to satisfy the symbol. `ROGUESQ_DUMP_PCM` / `ROGUESQ_RENDER_SONG` drive offline capture. See [docs/game-architecture.md](docs/game-architecture.md#audio-pipeline).

Subtitled **dialogue uses a separate codec, MORT** (per the [rerogue](https://github.com/dpethes/rerogue) PC-version RE). Contrary to earlier notes, MORT is **fully recompiled and works** — `tools/mort_decode.py` / `tools/MORTDecoder.cpp` are the offline reference. Voice-decode freezes were an **N64Recomp codegen bug** in the branch-and-link (`bgezal`/`bltzal`/`jal`) `$ra`-as-data-pointer idiom: the MusyX/voice filters do `bltzal $zero, T` then read `$ra` (= PC+8) as the base of an embedded coefficient table (`addiu $t7, $ra, 0xD4; lb …`). The recompiler emitted the branch but **never materialized the link *value***, so `$ra` stayed 0 (indirect-call entry), the table pointer computed to `~0x800000DB`, and the read AV'd — SEH-swallowed, killing the thread and deadlocking the frame pipeline (`filterVoiceSampleBlock`, on `viRetraceHandlerThread`, was the SELECT-LEVEL→load voiceline freeze; `applyVoiceDelayFilter` was the earlier demo/FrontEnd one). **Fixed (2026-09-20)**: `recompilation.cpp` now emits `ctx->r31 = PC+8` unconditionally for branch-and-link (new `Generator::emit_link_address`, implemented in `CGenerator`), before the branch condition for the regimm links — plus a full regen. Diagnose these from a full-memory `dump-game.ps1` dump with `tools/reconstruct-freeze.py` (frozen-machine state from RDRAM) + `tools/host-stacks.py` (symbolized host stacks; refuses on a stale-exe/PDB mismatch). See project memory `craftselect-voiceline-freeze-2026-09-16` (root cause + the fix) and `demo-voiceline-freeze-2026-09-13`. The old `ROGUESQ_VOICE_UNSTICK` host watchdog has been removed. `tools/extract_speech_table.py` extracts the voiceId→text table.

The **structure-destruction attract-demo freeze** (jade moon and any demo that blows up a structure) is **FIXED** ([plans/jade-moon-demo-freeze-plan.md](plans/jade-moon-demo-freeze-plan.md)): during an explosion an NPC has `npc+0x190 == NULL`, so `getNpcCurrentHealth` derefs a wild address and AVs; the SEH-swallowed AV leaves the gfx-frame barrier inconsistent → deadlock. The `patches/npc_health_guard.c` override guards the read. Note it only took effect once `PatchesLib` was made an **OBJECT** library (see the patches section) — getNpcCurrentHealth is reached only via the `func_map`/`LOOKUP_FUNC` indirect path, and a static-lib override does not win that address-of reference. When a recompiled function hangs on data that decodes fine offline, suspect a codegen mistranslation of a rare instruction (especially the `*al` link-branches) before deep subsystem RE.

### Cooperative-scheduler queue plumbing

DP (`OS_EVENT_DP`) events arrive on a non-game thread → `enqueue_external_message_src` → drained on the next game-thread `osSendMesg`/`osRecvMesg`/`osJamMesg` via `dequeue_external_messages`. Queue 0x8011A408 (gate-thread DP queue, count=1) and 0x8011A7E8 (consumer) are the DP-pacing pair. Default `MessageQueueControl{}` has `requeue_dp = true`. The `mqdiag` instrumentation in [ultramodern/src/mesgqueue.cpp](lib/N64ModernRuntime/ultramodern/src/mesgqueue.cpp) tracks per-queue send/recv/external/delivered/blocked/lost/requeued counts; dump via `mqdiag_dump(path)`.

### Exception handling pipeline

- **C++ exceptions** (SEH `0xE06D7363`): the `__except` filter in [librecomp/src/recomp.cpp](lib/N64ModernRuntime/librecomp/src/recomp.cpp) **must** let these propagate (`EXCEPTION_CONTINUE_SEARCH`). Catching them with `std::exit(1)` kills the game on any throw; the outer `try/catch` in [ultramodern/src/threads.cpp](lib/N64ModernRuntime/ultramodern/src/threads.cpp) recovers.
- **Hardware SEH** (AVs, illegal instructions): caught, the thread terminates, the process continues.
- **STL bounds checks** ("vector subscript out of range"): the `_CrtSetReportHook` in [main.cpp](src/main/main.cpp) returns 1 to suppress the abort. Trade-off: occasional visual glitch over a hard crash.
- **`get_function(0)`** (NULL fn-ptr call): stubbed to a no-op in [librecomp/src/overlays.cpp](lib/N64ModernRuntime/librecomp/src/overlays.cpp) so the recompiled MIPS continues; logs the host return address for later mapping.

### RT64 interpreter safety

There is no global iteration cap in [rt64_interpreter.cpp](lib/rt64/src/hle/rt64_interpreter.cpp). F5 walks are bounded by the chunk-fetch rules in `rt64_gbi_f3dfactor5.cpp`: a per-task limit of 4096 chunk hops, exact chunk-revisit detection, and a back-link check. A runaway walk means one of those rules failed. `hleGBI` NULL checks exist around the reset path; the main HLE loop only `assert`s it.

## When investigating a new crash

1. **Capture full output to a file.** Bash redirects don't flush before SIGTERM on Windows.
2. **Read the last few hundred lines** before the crash marker. The crash type is the bug class:
   - `Access violation reading address 0x1F8` (small) — NULL+offset deref, usually `hleGBI->map[opcode]` with NULL `hleGBI`. Guarded.
   - `Access violation reading address 0x1F2_xxxxxxxx` (huge) — stale GPU resource handle; renderer use-after-free.
   - `Assertion failed: ... rt64_gbi_f3d.cpp` — F3D handler hit an unimplemented case. Convert to log+skip.
   - `Assertion failed: ... rt64_native_target.cpp` — RT64 hit an unimplemented readback format. Convert to skip.
   - `vector subscript out of range` — STL bounds check, suppressed via `_CrtSetReportHook`.
   - `Failed to find function at 0x...` — recompiled MIPS called via NULL fn-ptr. Stubbed.
   - `Unable to find a matching GBI in the current database` — unrecognized ucode task; the mid-task NULL guard catches the resulting deref, that geometry doesn't render.
3. **Check recent `processDisplayLists ENTER` logs** — the latest `dlStart` names the DL; `NEW DL @` / `NEW sub-DL @` dumps show the first commands.
4. **Check `submit_rsp_task` counts.** `n_gfx` = M_GFXTASK enqueues, `n_other` = audio. Compare with `dp_complete` on 0x8011A408 (mqdiag `Dp` column) to find tasks stuck in RT64.
5. **Use `tools/reconstruct-freeze.py` on a full dump** to validate queue-level theories before instrumenting: it lists every blocked game thread and its wait queue.

For a hang specifically: if it's a cutscene/demo, suspect a recompiler codegen mistranslation of a rare instruction on the hung path (the demo-freeze root cause — see the Audio quirk) before deep subsystem RE.

For a bug that shows up only in Release (or only when the host is fast), suspect a game assumption that some producer is slower than a frame before suspecting the compiler. Example: `tickFormatMessageWorker` sent every reply as a pointer to one stack buffer; at -O2 it answered several requests before the menu polled once, the queued replies aliased, and SELECT GAME lost its medals/preview (fixed by the reply-ring hook in `rogue_squadron.toml`). `ROGUESQ_WATCH_ADDRS` and `ROGUESQ_DATA_BP` find the diverging write; the CMake `RS64_OD_TARGETS` / `RS64_OD_SOURCES` settings bisect by optimization level.

## Avoid these dead ends (already disproven)

### Rendering / GBI

- **`op_80` as a sub-DL call** — treating its 24-bit w0 as a call target infinite-loops and hangs after ~200 DLs. It's a state/param load.
- **Y-flip / component swap on model textures** — retired. The real cause was the missing `03 82` texcoord scale (see the Factor 5 GBI section).
- **`ROGUESQ_SUPPRESS_OOB_CIMG` LOW-region filter as default-on** — Factor 5 LLE legitimately emits some lowmem CIMGs; keep it env-gated.
- **Synthetic per-halt FULL_SYNC injection in dpc_bridge** — corrupts RT64 tile state mid-frame; white-bounding-box artifacts and AVs in `loadTileOperation`.
- **A `cv.wait` rewrite of RT64's present-queue busy-wait** (`rt64_present_queue.cpp:38-46`) — regressed natural-exit rate. Reverted.

### libultra / scheduler

- **13-way contention on `0x8011A7E8`** — only one thread calls `func_8000C07C`.
- **cE/cF bytes at `0x80128EAE/F` as a frame-sync counter** — they're slot-type bytes in a scheduler table.
- **10× `dp_complete` to fix DP throughput** — producer side is fine.
- **Cooperative scheduler losing DP messages** — `mqdiag` shows 0 lost/requeued for the DP queue.
- **"iter 3 hangs" in `func_8000C07C`** — counter misread; the loop runs 30+ iters normally.
- **"iter ~810 cinematic freeze"** — was a symptom of the old LLE pipeline. The current VI-driven HLE path runs steady; not observable. Don't chase it.

### Boot flow / state machine

- **Force-menu bypass via `ROGUESQ_FORCE_MENU_AT_SEC`** — skipping cinematic init crashes downstream. Don't jump the state machine.
- **Re-investigating a "missing state-1 writer" in the 5-slot table at `D_80154620`** — that's the speech/streamed-voice playback slots, not cinematic stages. This is where the streamed-voice active byte lives; the demo-freeze it seemed to gate was actually the `bgezal`/`bltzal` codegen bug (now fixed), not a missing scheduler writer.

### Build / regeneration

- **Hand-editing `funcs_*.c` for game-logic overrides** — next regen silently strips it. Use `patches/`. Diagnostic `fprintf` probes are fine.
- **Regenerating `funcs_*.c` with a stale N64Recomp binary** — a binary built from old source (e.g. `build/Debug/N64Recomp.exe`, the patches binary) errors on the `cache` instruction (its entrypoint recompile reads past the 0xC bound into `func_8000040C`) and **truncates `funcs.h` to ~7 lines**. Restore via `cd E:/Projects/N64Recomp && git checkout -- RecompiledFuncs/`, then rebuild the main-regen binary from current source: `cmake --build build_new --config Debug --target N64RecompCLI` and copy `build_new/Debug/N64Recomp.exe` to `Debug/N64Recomp.exe`. A current-source build bounds the entrypoint correctly and produces the committed 2561-line `funcs.h` (verified 2026-09-13).
- **Never write repo files via Python `open(...,'w')`** — a bad Python write once truncated the entire GBI core and it had to be rebuilt from goldens. Use the editor tools.

### Miscellaneous

- **The `wmain/main` link warning** — benign.
- **Shadowing `osPiStartDma_recomp` in `upstream_compat.cpp` with only the ROM-read branch** — boot needs the SRAM-read path too. Replicate `do_dma` in full or it regresses.

## Style conventions

- **No emojis** in code, comments, or docs unless explicitly requested.
- **No trailing summary blocks** in chat responses — one-line wrap-up max.
- Default to **no comments**. Add one only when the WHY is non-obvious (a workaround for a specific bug, a hidden invariant, a non-visible constraint).
- **Comments are terse and matter-of-fact.** State the fact, not the reasoning journey. No multi-paragraph narration, no dated blow-by-blow history, no "we tried X then Y" storytelling in a comment — one or two plain lines. This applies to config comments (e.g. `rogue_squadron.toml`) too.
- **At most 1-2 lines per comment block, and don't hard-wrap at ~90 columns** — let a line run long rather than splitting one sentence across several. No trailing side comments after code (`x = 1;  // note`) and no extra notes tacked on after a semicolon; if it matters, it goes in the one comment above the code.
- Don't reference the current task or session in comments — they rot.
- Prefer **editing existing files** over creating new ones. The runtime is already large; new files attract drift.
- For probe instrumentation in `funcs_*.c`, rate-limit:
  ```c
  { static int n=0; ++n; if (n<=10 || (n%50)==0) { fprintf(stderr, "..."); fflush(stderr); } }
  ```
- **Diagnostic accumulators must be bounded.** Any `static` set/map/vector that a debug probe grows keyed by an ever-changing value (address, hash, DL/frame id) leaks to runaway memory over a long logging session if it is never evicted — a distinct-hash set is the classic offender. Cap them keep-recent instead: in `lib/rt64` use `rt64diag::BoundedSet` / `BoundedMap` ([lib/rt64/src/common/rt64_diag_bounds.h](lib/rt64/src/common/rt64_diag_bounds.h), FIFO eviction, default 5000, override `ROGUESQ_DIAG_CAP`); once saturated a set reports a recent-window count (`<= cap`), not an all-time total. Address/config-keyed trackers are naturally small and need no cap; hash/id-keyed ones do. (Note: normal runs are memory-flat in both Debug and Release — the fixed ~4.9GB Private / ~11GB Virtual at boot is RT64's GPU commit, not a leak; runaway shows up only under diag flags.)
- When renaming symbols, avoid address-encoded names (`clearByteAt801128CC`) — they add nothing over `func_HHHHHHHH`. If you can't see the semantics, leave it as `func_*`.
- **One statement per line for control flow.** An `if` (or other statement) that begins a new logical statement gets its own line — don't trail it after another statement on the same physical line separated by `;`. Split the guard onto the next line:
  ```c
  static int s_lo = -1;
  if (s_lo < 0) s_lo = env_on("ROGUESQ_LOG_AUDIO_OUT");
  ```
  Exempt: single-line loop bodies, aligned lookup/return ladders and tabular min/max updates, and the env-gated diagnostic probe blocks — keep those terse.
- **Env gates on hot paths must read the environment once.** Use a `static const` initializer, or a distinct uninitialized sentinel checked with `== -1`. Never cache "off" as a negative value that a `< 0` check re-reads: 12 such gates in the F5 GBI ran `getenv()` on every command and were half the walk time.
- **Read env vars through the shared `recomp::dbg::env_*` helpers** (`src/main/debug_logs.h`: `env_on`/`env_int`/`env_str`/`env_u32`), not open-coded `getenv` parsing.

## Open work

Priorities (user-visible issues are listed under [Status](README.md#status-playable) in the README):

1. **Display-list desyncs** — about a dozen per run, typically garbage right after a material sub-DL returns. Root-cause with [docs/f5-model-dl-spec.md](docs/f5-model-dl-spec.md) and `tools/validate/f5_dl_walk.py`.
2. **Retire render heuristics that a known microcode rule can replace** — e.g. the 0xBD sprite path (the ucode emits a screen-space texrect via overlay 0x2C) and the terrain grid shape. Verify each with the DL/RDRAM validation harness, not screenshots alone.
3. **Symbol renaming** — e.g. the "debris cell" functions in `funcs_36.c` (`buildDebrisMeshFromCells`, `emitDebrisCellFaces`, …) are the JFIF/JPEG decoder used by `tickFormatMessageWorker`. Run `tools/rename/lint_toml_syms.py` after each batch.

The render path is HLE through the `GBI_F3DFACTOR5` profile.
