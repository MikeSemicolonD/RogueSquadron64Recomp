# AGENTS.md

Guidance for AI agents working on the Rogue Squadron 64 Recompiled project. This is an active port of *Star Wars: Rogue Squadron* (N64, USA v1.0) using N64Recomp + RT64 LLE rendering (Factor 5 ucode recompiled to C, output forwarded to RT64 via the DPC bridge).

## Project layout

```
src/main/main.cpp                       Game registration + RSP/audio/input/gfx callbacks
src/rsp/dpc_bridge.cpp                  DPC_START/DPC_END bridge: Factor 5 LLE → RT64
src/rsp/aspMain.cpp                     Audio RSP microcode stub (silent)
lib/N64ModernRuntime/                   Submodule — fork at MikeSemicolonD/N64ModernRuntime
  ├── librecomp/                        Recompiler runtime (overlay loading, get_function, SEH)
  └── ultramodern/                      libultra emulation (threads, mesgqueue, events)
lib/rt64/                               Submodule — fork at MikeSemicolonD/rt64
docs/                                   Project-specific notes (Factor5 GBI, debug-trace env vars)
tools/                                  PowerShell + Python diagnostic helpers
build/                                  CMake out-of-source build dir
```

The recompiled MIPS code lives **outside** the project at `E:\Projects\N64Recomp\RecompiledFuncs\`. Files there are named `funcs_N.c` (N=0..42) plus `funcs.h`, `recomp_overlays.inl`. Edits there are routine and expected — they are auto-generated but commonly hand-instrumented during debugging.

The forked submodules under `lib/` have intentional `if(false) fprintf(...)` debug-toggle cruft and a number of game-specific defensive guards. **Do not propose stripping these** as part of cleanup; they are intentional.

## Build & run

```powershell
# Build (Debug)
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

## Diagnostic artifacts

Files written to `logs/` and `dumps/crash-dumps/` during a run:

- `logs/cine_yield_sink.log` — per-iter yield burst sink (CINE_YIELD macro
  in `funcs_27.c`).
- `logs/stability/<tag>/run_N.log` — per-run stderr captured by
  `tools/run-stability.ps1`. Companion `summary.csv` classifies outcomes.
- `logs/stability/<tag>/memory.csv` — per-second WS / private bytes / VM
  samples written by `tools/measure-leak.ps1`.
- `mqdiag_NNN.txt` — per-queue counters dumped every 3s by the watchdog
  thread (`start_mqdiag_watchdog` in [src/main/main.cpp](src/main/main.cpp)).
  Each file is a snapshot at that point in time; diff two snapshots to
  see which queues are still moving.
- `dumps/crash-dumps/crash_YYYYMMDD_HHMMSS.dmp` — full-memory minidump
  written by either the SEH crash handler, the SIGABRT/abort handler,
  the F12 hotkey, or external `tools/dump-game.ps1`.

## Tooling (under `tools/`)

When the game freezes or crashes, the fastest route is:

1. **For a crash**: a `crash_*.dmp` is written automatically by the
   handlers in `src/main/main.cpp`. Open it in VS (File → Open →
   File → .dmp → Debug with Native Only).
2. **For a hang**: from another shell, run
   `pwsh tools/dump-game.ps1` to capture the running process even
   when its window is "Not Responding". Don't try F12 — if the
   message pump is starved, F12 won't fire.
3. **Triage threads**: `python tools/inspect-dump.py` lists every
   thread in the most recent dump and tags ones running our exe
   code as `in_exe`. The 2–4 `in_exe` threads are the only ones
   worth opening in VS Threads window — the rest are runtime workers
   parked in ntdll waits.

For stability or leak measurements, prefer the harness over manual runs:

- `tools/run-stability.ps1 -Runs N -Timeout S -Tag <label>` — launches
  the binary N times, captures per-run stderr, classifies each outcome
  by grepping markers (`[CRASH]`, `[ABORT]`, `[L_627C-FIRST]` for
  natural exit, `[cine-tick]` for max iter/fc, `[guard]` count, menu-init
  reached). Use `-EnvVars "ROGUESQ_LOG_X=1;..."` to forward debug-trace
  env vars. Use `-Runs 1` for data-capture diagnostics; `-Runs 3` for
  stability-rate measurement.
- `tools/measure-leak.ps1 -Timeout S -Tag <label>` — single run, polls
  Working Set / Private Bytes / Virtual Memory once per second to
  `memory.csv`. Useful when correlating per-second growth with stderr
  trace timestamps.

Note: the Windows debugger CLI (`cdb.exe`) is currently broken on
this machine (fails with `STATUS_DLL_INIT_FAILED`). Don't waste time
trying to drive it from PowerShell — use VS interactively, or read
the dump with the `minidump` Python package (already covered by
`inspect-dump.py`).

## Debug trace env vars

All `[*]` log tags are gated behind `ROGUESQ_LOG_*` env vars. Canonical
catalog at [docs/debug-trace-env-vars.md](docs/debug-trace-env-vars.md)
— **read it before adding a new `fprintf` or asking the user to enable
logs**. Notable vars:

| Env var | What it enables |
|---|---|
| `ROGUESQ_LOG_ALL` | Master switch — turns on every category |
| `ROGUESQ_LOG_CINE_CP` | Per-call-site checkpoints inside `func_800A5D80`'s cinematic loop body. Pair with `ROGUESQ_LOG_CINE_CP_FROM=N` to start verbose at iter N |
| `ROGUESQ_LOG_RT64_ALLOC` | RT64 allocation hotspots (interpolatedColorTargets, nativeSwappedRAM, rdramData, BufferPair, RenderTarget setup) — finds allocation spikes |
| `ROGUESQ_LOG_DPC` | DPC bridge submission events |
| `ROGUESQ_LOG_THREADS`, `ROGUESQ_LOG_INIT`, `ROGUESQ_LOG_PRESENT`, `ROGUESQ_LOG_SP_TASKS`, etc. | See `docs/debug-trace-env-vars.md` |
| `ROGUESQ_SUPPRESS_OOB_CIMG` | **Default OFF.** When set, drops Factor 5 ucode emissions of bogus SET_COLOR_IMAGE commands at HIGH (≥ 0x800000) and LOW (< 0x100000) addresses before they reach RT64. Reduces the iter-810 memory spike but causes a visual regression — the 3D Factor 5 logo no longer renders because some legitimate lowmem CIMGs are dropped along with the garbage. Existing writeback guard at `rt64_state.cpp:1494` (`addressStart >= 0x100000`) is the right place to filter without dropping the CIMG itself |
| `ROGUESQ_HWBP` + `ROGUESQ_HWBP_ADDR` | Win32 DR0 hardware breakpoint on a configurable RDRAM address |

## Patches build (overriding auto-generated functions)

**Do not hand-edit `E:/Projects/N64Recomp/RecompiledFuncs/funcs_*.c` for
defensive guards or game-logic overrides.** Those edits are
regeneration-hostile (lost on the next N64Recomp run) and made the
codebase brittle for months. We have a real `patches/` build now —
follow the Zelda64Recompiled pattern, with one difference:

**We use `mips64-elf-gcc`, not `clang -target mips`.** The official LLVM
Windows installer ships without the MIPS backend (verified on 19, 20,
22-rc — `clang -print-targets | findstr mips` returns nothing). Linux
LLVM packages do include MIPS; if you're on Linux just `apt install clang
lld make` and override `MIPS_GCC` / `MIPS_LD` cache vars. On Windows,
install the [n64-tools/gcc-toolchain-mips64](https://github.com/n64-tools/gcc-toolchain-mips64/releases)
prebuilt MIPS GCC 12.2.0 — default install path the CMake config expects
is `E:/mips-toolchain`. Override with `cmake -DMIPS_TOOLCHAIN_DIR=...`.

### Pipeline at a glance

```
patches/heap_guards.c           ← you write MIPS-side C (game pointers, externs)
  ↓ mips64-elf-gcc -mips2 -mabi=32 -nostdinc
patches/heap_guards.o
  ↓ mips64-elf-ld -T patches.ld -T syms.ld
patches/patches.elf
  ↓ N64Recomp.exe ../patches.toml  (single_file_output, strict_patch_mode)
RecompiledPatches/patches.c     ← host C with recomp_func_t signatures
  ↓ clang-cl
PatchesLib.lib
  ↓ linked FIRST in target_link_libraries (before RecompiledFuncs)
RogueSquadron64Recomp.exe       ← /FORCE:MULTIPLE picks our overrides at link
```

### Adding a new override

1. Write the function in `patches/somefile.c`. Name it the same as the
   game function (`func_80007D74`). Annotate with `RECOMP_PATCH`:
   ```c
   #define RECOMP_PATCH __attribute__((section(".recomp_patch")))
   RECOMP_PATCH void* func_80007D74(void) { /* ... */ }
   ```
2. Game functions/data referenced by the patch get added to
   `patches/syms.ld`:
   ```ld
   func_8002221C  = 0x8002221C;
   heap_free_head = 0x801163B0;
   ```
   Look up addresses in `syms/rogue_squadron.syms.toml` (auto-generated by
   `N64Recomp.exe rogue_squadron.toml --dump-context`).
3. Declare them as `extern` in your patch C — the linker resolves the
   symbol; the C declaration only adds type info.
4. Build: `cmake --build build --config Debug --target PatchesLib` builds
   just the patches lib; the regular full build relinks everything.

### Constraints (gotchas)

- **`-nostdinc` means no libc headers.** No `<stddef.h>`, `<stdint.h>`,
  `<string.h>`. Inline the typedefs you need:
  ```c
  typedef unsigned char uint8_t;
  typedef unsigned int  uint32_t;
  typedef unsigned long uintptr_t;   // mips32 ABI: long is 32-bit
  #define NULL ((void*)0)
  ```
- **No `printf` / `stderr`.** To call host code from a patch, declare a
  runtime stub at a fake `0x8FXXXXXX` address in `syms.ld` and provide
  the host implementation in `src/main/`. We haven't wired any host
  stubs yet — add as needed.
- **Function signatures match the game, not the recomp.** Write the
  patch with the original MIPS-style signature (no `recomp_context*`
  arg). N64Recomp's recompile pass produces the host signature
  `void(uint8_t*, recomp_context*)` automatically.
- **Duplicate symbols are EXPECTED** when an override exists in both
  PatchesLib and RecompiledFuncs. We pass `/FORCE:MULTIPLE` to the
  linker; it takes the first definition (PatchesLib comes first in
  `target_link_libraries`). The build emits a warning per duplicate;
  that's fine.

### Diff vs Zelda's clang-based Makefile

| Zelda flag | Ours | Why |
|---|---|---|
| `-target mips` | (drop) | mips64-elf-gcc is already MIPS |
| `-mno-odd-spreg` | (drop) | clang-only |
| `-mno-check-zero-division` | (drop) | clang-only |
| `-Wno-incompatible-library-redeclaration` | (drop) | clang-only |
| `-Wno-unsupported-floating-point-opt` | (drop) | clang-only |
| `-mips2 -mabi=32 -G0 -mno-abicalls -O2 -fomit-frame-pointer -ffast-math` | same | gcc accepts these |

Both pipelines use `ld.lld` / `mips64-elf-ld` with the same linker scripts.

### Toolchain quick-reference

| Component | Path |
|---|---|
| MIPS GCC | `E:/mips-toolchain/bin/mips64-elf-gcc.exe` |
| MIPS LD | `E:/mips-toolchain/bin/mips64-elf-ld.exe` |
| `make` | `mingw32-make` (any MinGW install) |
| N64Recomp | `build/Debug/N64Recomp.exe` (built from submodule via `N64RecompCLI` target — newer than the older `E:/Projects/N64Recomp/Debug/N64Recomp.exe` and supports `--dump-context` + `func_reference_syms_file` toml options) |

See [patches/README.md](patches/README.md) for the full how-to.

## Architectural quirks worth knowing

### Factor5 GBI — custom opcodes

This game uses a Factor5-customized F3DEX-derived ucode (`gspF3DEXMain` symbol but heavily modified). Documented mappings live in [docs/factor5-gbi.md](docs/factor5-gbi.md). Confirmed Factor5-specific behaviors:

| Opcode | Standard meaning   | Factor5 behavior                            | Handler in our tree |
|-------:|--------------------|---------------------------------------------|---------------------|
| `0xB5` | F3DEX `G_QUAD`     | **Chunk/DL terminator** (= F3DEX `G_ENDDL`) | `op_B5_endDl`       |
| `0xE4` | F3DEX `G_TEXRECT`  | **LLE format (16 bytes)**, not HLE 24-byte  | `texrectLLE`        |
| `0xE5` | F3DEX `G_TEXRECTFLIP` | LLE format                              | `texrectFlipLLE`    |
| `0xFF` | `G_SETCIMG`        | Frequently emitted with bogus payloads (w1=0, fmt>4, addresses outside FB region). `setColorImage_filtered` rejects fmt>4 and w1==0. Optional `[cimg-drop]` filter in `src/rsp/dpc_bridge.cpp` (env-gated, OFF by default) drops HIGH (≥ 0x800000) and LOW (< 0x100000) addresses — but enabling it kills the 3D Factor 5 logo, so use only for leak-mitigation experiments | `setColorImage_filtered` (always-on) + `[cimg-drop]` (env-gated) |
| `0x80` | unused in F3DEX    | Chunk metadata header (next/prev pointers)  | `op80_unknown` (no-op) — see note |
| `0x02` | F3D `G_RDPHALF_2`  | Custom — constant payload `0x028001C0/0x01FF0000` | `op02_unknown` (no-op) |
| `0x12, 0x16, 0x1E, 0x22, 0x26, 0x2A, 0x2E, 0x32, 0x36, 0x3A` | unused | Custom Factor5 family — opcode byte encodes operand bits 6:2 | unmapped |

**Important note on `op_80`**: chunks are 0x108-byte blocks packed contiguously; w0 of the header points to the next chunk. The header is treated as a no-op and the interpreter walks linearly through chunk content. The `0xB5` terminator at offset 0x100 returns control to the parent DL.

### MusyX audio

Rogue Squadron uses Factor5's **MusyX** audio ucode, NOT stock `aspMain`. Running `aspMain` on MusyX-formatted task data hangs the audio thread (no shared format). Until MusyX has its own RSP recomp pass, [src/main/main.cpp](src/main/main.cpp) routes `M_AUDTASK` to a stub that returns `RspExitReason::Broke` immediately. Trade-off: no audio.

### Cooperative-scheduler queue plumbing

DP (`OS_EVENT_DP`) events arrive on a non-game thread → `enqueue_external_message_src` → drained on the next game-thread `osSendMesg`/`osRecvMesg`/`osJamMesg` call via `dequeue_external_messages`. Queue 0x8011A408 (gate-thread DP queue, count=1) and 0x8011A7E8 (consumer queue) are the DP-pacing pair. Default `MessageQueueControl{}` has `requeue_dp = true` — DP messages get re-queued if the destination is full at delivery time.

`mqdiag` instrumentation in [lib/N64ModernRuntime/ultramodern/src/mesgqueue.cpp](lib/N64ModernRuntime/ultramodern/src/mesgqueue.cpp) tracks every queue's send/recv/external/delivered/blocked/lost/requeued counts. Dump via `mqdiag_dump(path)` (called from `events.cpp` per frame).

### Exception handling pipeline

- **C++ exceptions** (SEH code `0xE06D7363`): the `__except` filter in [librecomp/src/recomp.cpp](lib/N64ModernRuntime/librecomp/src/recomp.cpp) **must** let these propagate (returns `EXCEPTION_CONTINUE_SEARCH` for that code). Catching them with `EXCEPTION_EXECUTE_HANDLER` and `std::exit(1)` kills the game on any throw. The outer `try/catch` in [ultramodern/src/threads.cpp](lib/N64ModernRuntime/ultramodern/src/threads.cpp) recovers.
- **Hardware SEH** (access violations, illegal instructions): caught and the thread terminates, but the process continues.
- **STL bounds checks** ("vector subscript out of range"): the `_CrtSetReportHook` in [src/main/main.cpp](src/main/main.cpp) returns 1 to **suppress** the abort. Trade-off: occasional visual glitches over a hard crash.
- **`get_function(0)` (NULL function pointer call)**: stubbed to a no-op in [librecomp/src/overlays.cpp](lib/N64ModernRuntime/librecomp/src/overlays.cpp) so the recompiled MIPS continues. Logs the host return address for later mapping.

### RT64 interpreter safety

The interpreter loop in [rt64_interpreter.cpp](lib/rt64/src/hle/rt64_interpreter.cpp) has a 5-million-iter safety limit. Without it, a missing DL terminator causes `dl++` to march past the end of RDRAM and AV. Triggered = visual artifacts in that scene.

The main loop also guards against `hleGBI` becoming NULL mid-task (F3DEX op `0xAF` `loadUCode` can fail to find a matching ucode and zero out `hleGBI`).

## When investigating a new crash

1. **Capture full output to a file**. Do NOT rely on terminal output — Bash redirects don't flush before SIGTERM kill on Windows.
2. **Look at the LAST few hundred lines** of the trace before the crash marker. The crash type tells you the bug class:
   - `Access violation reading address 0x1F8` (or similar small address) — NULL+offset deref. Almost always `hleGBI->map[opcode]` with NULL `hleGBI`. Already guarded.
   - `Access violation reading address 0x1F2_xxxxxxxxx` (huge) — stale GPU resource handle. Renderer use-after-free.
   - `Assertion failed: ... rt64_gbi_f3d.cpp` — F3D handler hit unimplemented case (e.g., `moveMem` with unknown idx). Convert to log+skip.
   - `Assertion failed: ... rt64_native_target.cpp` — RT64 hit unimplemented readback format. Convert to skip.
   - `vector subscript out of range` — STL bounds check. Currently suppressed via `_CrtSetReportHook` returning 1.
   - `Failed to find function at 0x...` — recompiled MIPS called via NULL function pointer. Stubbed.
   - `Unable to find a matching GBI in the current database` — game submitted task with an unrecognized ucode. The mid-task NULL guard catches the resulting NULL deref; the missing geometry doesn't render.
3. **Check the recent `processDisplayLists ENTER` logs**. The most recent `dlStart` address tells you which DL is being processed. Use the `NEW DL @` and `NEW sub-DL @` dumps to inspect the first commands.
4. **Check `submit_rsp_task` counts**. `n_gfx` counts M_GFXTASK enqueues; `n_other` counts audio. Compare with `dp_complete` events on `0x8011A408` (mqdiag `Dp` column) to detect tasks stuck in RT64.
5. **Use `mqdiag_frame.txt`**. Validates queue-level theories before instrumenting.

## Avoid these dead ends (already disproven)

- 13-way contention on `0x8011A7E8` — only one thread calls `func_8000C07C`.
- cE/cF bytes at `0x80128EAE/F` as a frame-sync counter — they're slot-type bytes in a scheduler table.
- 10× `dp_complete` to fix DP-event throughput — producer-side fine; bottleneck was downstream (interpreter loop).
- Cooperative scheduler losing DP messages between yields — `mqdiag` shows 0 lost/requeued for DP queue.
- "iter 3 hangs" interpretation of `func_8000C07C` — counter misread; the loop runs 30+ iters under normal conditions.
- The `wmain/main` link warning matters — it's benign.
- Synthetic per-halt FULL_SYNC injection in dpc_bridge — corrupts RT64 tile state mid-frame, produces white-bounding-box artifacts and AVs in `loadTileOperation`. The cinematic's natural ~5/s submission rate is correct. Don't retry.
- Force-menu bypass via `ROGUESQ_FORCE_MENU_AT_SEC` — skipping the cinematic init causes downstream crashes. Find what gates `bit 25 of MEM[0x80130B58]` naturally instead.
- A `cv.wait` rewrite of RT64's present-queue busy-wait at `lib/rt64/src/hle/rt64_present_queue.cpp:38-46` — looked surgical, regressed natural-exit rate from ~30% to 0%. Reverted.
- Re-investigating "missing state-1 writer" in the 5-slot table at `D_80154620` — that table is the speech-sample playback slots, not cinematic stages. Audio is stubbed so the slots stay zero.
- Repeatedly iterating cinematic-explosion shader probes — frame rate, pipeline drops, early-z, VI selection, vertex w, mux family, and FillRect-not-firing are all ruled out. Next step is a RenderDoc/PIX capture, not another shader iteration.
- `[cimg-drop]` LOW-region filter (`addr < 0x100000`) as default-on — Factor 5 LLE legitimately emits some lowmem CIMGs for the 3D logo render path, so dropping them at dpc_bridge kills the visual. The right place to filter is the existing writeback guard at `rt64_state.cpp:1494` (`addressStart >= 0x100000`), which lets the CIMG produce a render target but skips the RDRAM writeback. Keep `ROGUESQ_SUPPRESS_OOB_CIMG` env-gated.
- **Hand-editing `funcs_*.c` (the auto-generated MIPS-to-C output).** Looks fast in the moment, but the next regeneration silently strips your work and you don't realise until something downstream breaks weeks later. Always use the `patches/` build instead — write the override in `patches/foo.c`, point `syms.ld` at the game-side data it touches, rebuild. See [patches/README.md](patches/README.md).
- **Regenerating `funcs_*.c` with the newer N64Recomp (`build/Debug/N64Recomp.exe`).** It errors out on the unsupported `cache 0x0D` MIPS instruction in `func_8000040C` and `func_80018D80`, then **truncates `funcs.h` and the affected `funcs_N.c` mid-write**. If this happens, restore the broken files via `cd E:/Projects/N64Recomp && git checkout -- RecompiledFuncs/`. Use the *older* N64Recomp at `E:/Projects/N64Recomp/Debug/N64Recomp.exe` for full main-tree regeneration; the newer one is only for the patches pipeline.

## Style conventions

- **No emojis** in code, comments, or written documentation unless explicitly requested.
- **No trailing summary blocks** in chat responses — one-line wrap-up max.
- Default to writing **no comments**. Add a comment only when the WHY is non-obvious (a workaround for a specific bug, a hidden invariant, a constraint that won't be visible from reading the code).
- Don't reference the current task or session in comments. Comments rot if they describe ephemeral context.
- Prefer **editing existing files** over creating new ones. The runtime is already large; new files attract drift.
- For probe instrumentation in `funcs_*.c`, use the rate-limited pattern:
  ```c
  { static int n=0; ++n; if (n<=10 || (n%50)==0) { fprintf(stderr, "..."); fflush(stderr); } }
  ```

## Open work

- **iter ~810 cinematic freeze**: ~70% of runs time out at iter 832-835 inside random functions in `func_800A5D80`'s loop body. Empirically only stderr writes unstick it; `Sleep`, `SwitchToThread`, and `cursorCondition` waits all do not. Likely OS-level thread starvation. CP markers gated by `ROGUESQ_LOG_CINE_CP=1` are already wired into the loop body.
- **Menu-init heap-walker bugs**: when natural exit fires (~30% of runs), execution lands in `func_800C58A0` `menu_overlay_init` which has its own KSEG0-pointer-validation issues.
- **Bogus RDP commands from Factor 5 DMA buffer**: the ucode emits uninitialized DMEM bytes (e.g., `FF FF FF FF 00 00 07 E0`) as 8-byte DMA submissions. The `[cimg-drop]` filter in `src/rsp/dpc_bridge.cpp` neutralises the worst of them, but a wider-width family still slips through. Hunt the ucode-side cause at `build/factor5_ucode/factor5_ucode_recompiled.c:427` (the DMA write call site).
- **Audio**: stubbed; per the rerogue PC-version reversing notes the codec is **MORT**, not MusyX as previously assumed. Either way needs a separate RSPRecomp pass.
- **Keyboard input**: port Zelda64Recompiled's bind/rebind UI so keyboard can replace gamepad. Defer until past the cinematic-freeze blocker.

The team explicitly chose NOT to pursue full HLE for Factor 5 — see [docs/lle-spike-report.md](docs/lle-spike-report.md).
