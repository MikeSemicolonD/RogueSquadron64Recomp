# AGENTS.md

Guidance for AI agents working on the Rogue Squadron 64 Recompiled project. This is an active port of *Star Wars: Rogue Squadron* (N64, USA v1.0) using N64Recomp + RT64 HLE rendering, modeled after Zelda64Recomp.

## Project layout

```
src/main/main.cpp                       Game registration + RSP/audio/input/gfx callbacks
lib/N64ModernRuntime/                   Junction → E:\Projects\N64ModernRuntime (shared runtime)
  ├── librecomp/                        Recompiler runtime (overlay loading, get_function, SEH)
  └── ultramodern/                      libultra emulation (threads, mesgqueue, events)
lib/rt64/                               Symlink → E:\Projects\Zelda64Recomp\lib\rt64
docs/                                   Project-specific notes (Factor5 GBI, LLE spike)
build/                                  CMake out-of-source build dir
```

The recompiled MIPS code lives **outside** the project at `E:\Projects\N64Recomp\RecompiledFuncs\`. Files there are named `funcs_N.c` (N=0..42) plus `funcs.h`, `recomp_overlays.inl`. Edits there are routine and expected — they are auto-generated but commonly hand-instrumented during debugging.

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

Files written to the working directory during a run:

- `mqdiag_NNN.txt` — per-queue counters dumped every 3s by the watchdog
  thread (`start_mqdiag_watchdog` in [src/main/main.cpp](src/main/main.cpp)).
  Each file is a snapshot at that point in time; diff two snapshots to
  see which queues are still moving.
- `mqdiag_frame.txt` — single-frame snapshot from
  [rt64_interpreter.cpp](lib/rt64/src/hle/rt64_interpreter.cpp) (HLE path).
- `mqfocus.txt` — focused queue events for `0x80114388, 0x8011A408, 0x8011A7E8`.
- `dlhist_frame.txt` / `dlhist_op02.txt` — DL command history.
- `rdram_frame.bin` / `rdram_op02.bin` — 8 MB RDRAM dumps.
- `crash_YYYYMMDD_HHMMSS.dmp` — full-memory minidump written by either
  the SEH crash handler, the SIGABRT/abort handler, the F12 hotkey, or
  external `tools/dump-game.ps1`. Captures all of RDRAM so post-mortem
  inspection of MIPS-side data structures works.

## Post-mortem dump tooling (under `tools/`)

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

Note: the Windows debugger CLI (`cdb.exe`) is currently broken on
this machine (fails with `STATUS_DLL_INIT_FAILED`). Don't waste time
trying to drive it from PowerShell — use VS interactively, or read
the dump with the `minidump` Python package (already covered by
`inspect-dump.py`).

## Architectural quirks worth knowing

### Factor5 GBI — custom opcodes

This game uses a Factor5-customized F3DEX-derived ucode (`gspF3DEXMain` symbol but heavily modified). Documented mappings live in [docs/factor5-gbi.md](docs/factor5-gbi.md). Confirmed Factor5-specific behaviors:

| Opcode | Standard meaning   | Factor5 behavior                            | Handler in our tree |
|-------:|--------------------|---------------------------------------------|---------------------|
| `0xB5` | F3DEX `G_QUAD`     | **Chunk/DL terminator** (= F3DEX `G_ENDDL`) | `op_B5_endDl`       |
| `0xE4` | F3DEX `G_TEXRECT`  | **LLE format (16 bytes)**, not HLE 24-byte  | `texrectLLE`        |
| `0xE5` | F3DEX `G_TEXRECTFLIP` | LLE format                              | `texrectFlipLLE`    |
| `0xFF` | `G_SETCIMG`        | Sometimes emitted with bogus payload (w1=0) | `setColorImage_filtered` (filters bogus) |
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

## Memory system & sessions

When invoked via Claude Code, AI agents have a persistent memory at `C:\Users\Michael\.claude\projects\e--Projects-RogueSquadron64Recomp\memory\`. Key memory files:

- `MEMORY.md` — index of all memories, loaded into every conversation
- `project_factor5_workload_drain.md` — RESOLVED. Captures the journey from black-screen credits to legible text rendering.
- `project_post_credits.md` — current blocker. Post-credits scenes (LucasArts, N64 logo, X-wing intro) reach further with each fix but still expose new unimplemented Factor5/RT64 paths.
- `project_audio_musyx.md` — audio is stubbed; full MusyX HLE needs a separate RSPRecomp pass.

Memory is for facts that survive across sessions (project state, decisions, dead ends). Don't store code conventions or per-conversation work there.

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

The Factor5 RSP ucode reverse-engineering would unlock:
- Full text rendering (currently sparse — many glyphs render as the right shape at one position but the per-glyph TEXRECT loops emit the same UV coords)
- 3D triangle rendering for explosions, X-wings, ships, terrain
- Audio (separate MusyX pass)

Each one is a multi-day effort assuming you have the ucode binary and a disassembler. The team explicitly chose NOT to pursue full LLE — see [docs/lle-spike-report.md](docs/lle-spike-report.md).
