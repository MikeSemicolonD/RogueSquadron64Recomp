# Code-Modification Audit (2026-05-08)

> **Cleanup landed since this audit was written:**
> - **Tier 1**: Removed `cinematic_throttle` namespace (~50 LOC) from `lib/N64ModernRuntime/ultramodern/src/events.cpp` (was disabled May 2026; on_swap_request was a no-op store, on_vi_tick incremented an unread counter). Removed per-mux color-key diagnostic (~50 LOC) from `lib/rt64/src/shaders/RasterPS.hlsl`.
> - **Tier 2**: Moved Rogue-Squadron-specific load_overlays-on-PI-DMA call out of `lib/N64ModernRuntime/librecomp/src/pi.cpp`. Submodule now exposes a generic `recomp::set_post_pi_dma_callback()` hook (default null, no game-specific behavior). Game-specific call lives in `src/main/register_overlays.cpp`.
> - **Tier 3**: Updated `docs/debug-trace-env-vars.md` to document 9 missing env vars + flagged the legacy `SWAP_SHADE` alias.
> - **Tier 4 infrastructure (DONE)**: Patches build pipeline is fully wired — `patches/` directory + `Makefile` + linker scripts + `patches.toml` + CMake integration. Cross-compiles `patches/*.c` via `mips64-elf-gcc` (decompals/n64-tools 12.2.0 at `E:\mips-toolchain`), runs `N64Recomp` on the resulting ELF, and links the output as `PatchesLib` ahead of `RecompiledFuncs` so patches win at link time. Verified end-to-end with an empty placeholder patch; full `Debug` build clean.
> - **Tier 4 first real migration (DONE 2026-05-08)**: `func_80007D74` (heap free-list dequeue + queue insertion) with both KSEG0 guards now lives at [patches/heap_guards.c](../patches/heap_guards.c). Generated `syms/rogue_squadron.syms.toml` via `N64Recomp.exe rogue_squadron.toml --dump-context` so the patch can reference game functions/data symbolically. Wired `/FORCE:MULTIPLE` so PatchesLib's override wins the duplicate-symbol resolution against the auto-generated `funcs_3.c` definition. Verified: full Debug build clean, game runs normally with the override active. The inline guards in `funcs_3.c` are now redundant — left in place for safety until we've stress-tested the override under all observed corruption paths, but should be stripped on the next regen.
>
> Build is still clean. All deletions had no behavior change. Tier 5 (root-cause investigations) is still open.

A categorisation of every modification we've made to `lib/rt64`,
`lib/N64ModernRuntime`, our own `src/`, and the recompiled MIPS output at
`E:/Projects/N64Recomp/RecompiledFuncs/funcs_*.c`. The motivating concern:
other recomp projects (e.g. Harvest Moon recomp) make zero changes to RT64
and minimal changes to the runtime. We've layered ~50+ patches across these
surfaces, almost all of them empirical workarounds rather than rooted fixes.
This document is the inventory before any cleanup — read it once, decide
what stays, what moves, what gets deleted, and what gets a real
investigation.

## Categories

| Code | Meaning | Cleanup posture |
|---|---|---|
| **A** | Integration glue or necessary support — Factor 5 ucode handlers, DPC bridge, ROM-hash check, gamepad init, overlay loading. | Keep. May be relocatable to our repo if currently in a submodule. |
| **B** | Defensive guard papering over a real bug whose root cause we don't understand. Works empirically. | Keep for now (load-bearing) but document the unknown bug. Each one is a real investigation we've deferred. |
| **C** | Pure debug instrumentation — log lines, env-gated traces, dump probes, watchdogs. | Most should be removed or env-gated. Audit what's still actively useful. |
| **D** | Forgotten / orphaned / superseded — disabled hypotheses, dead `if(false)` blocks, whitespace tweaks. | Delete. |

## Totals at a glance

| Surface | Cat A | Cat B | Cat C | Cat D |
|---|---|---|---|---|
| `lib/rt64` | ~380 LOC across 4 files | ~900 LOC across 12 files | ~600 LOC across 8 files | small |
| `lib/N64ModernRuntime` | ~35 items | ~6 items | ~45 items | ~2 items |
| `src/` (our repo) | 8 items | 1 item | 6 items | 0 |
| `src/rsp/dpc_bridge.cpp` | 3 items | 4 items | 3 items | 0 |
| `funcs_*.c` (auto-gen) | 0 | ~12 items | ~8 items | 0 |

Roughly: **60% of what we've added is load-bearing (A+B), 40% is debug
cruft or orphaned (C+D)**. Roughly half of the load-bearing items have
unknown root causes (B), which is the architectural debt the user is
sensing.

## Surface 1 — `lib/rt64` (forked at MikeSemicolonD/rt64)

Fork point vs upstream: `1bcbda33`. 7 commits since fork. Diff vs upstream
includes upstream Vulkan support merged in (~50k lines) which is **not
ours** — exclude that from this audit.

### Cat A — Factor 5 ucode support (necessary)

- `src/gbi/rt64_gbi_f3dfactor5.cpp` + `.h` — Factor 5 GBI backend. The custom
  opcodes (0x80, 0x02, 0xB0, 0xB2, 0xB4, 0xB5, 0xBF, 0xFF) all need handlers
  that don't exist in standard F3DEX. Without this file, the parser misreads
  the bytestream as F3DEX and crashes. Roughly 240 LOC.
- Light hardening of `src/gbi/rt64_gbi_f3d.cpp` and `_f3dex.cpp` (G_DL / G_VTX
  bounds checks) so drift into garbage doesn't AV.
- `include/rt64_extended_gbi.h` — `GBIUCode::F3DFACTOR5` enum + segment
  registration.
- CMakeLists registration.

### Cat B — Defensive guards (root cause unknown for each)

| Location | Guard | Unknown bug |
|---|---|---|
| `rt64_rdp.cpp:240–310` (setColorImage / setDepthImage) | Reject addr < 0x100000 or addr ≥ 0x800000; reject width > 1024 | Why does the LLE pipeline see CIMGs with garbage payloads at all? Some are uninitialized DMA-buffer bytes (we proved this); others (wide widths in valid addr range) we still don't understand. |
| `rt64_rdp.cpp:520–560` (particle alpha rewrite) | Detects cinematic-particle combiner with alpha-D=ZERO and rewrites to TEXEL0_ALPHA. Default-on via `ROGUESQ_PARTICLE_FIX`. | Why does the cinematic combiner+blender combo produce zero alpha specifically in this path? Surgical fix; root cause unknown. |
| `rt64_rdp.cpp:970–1000` | RDRAM bounds checks on loadTile/loadBlock | Why do tile loads sometimes have wild source addresses? Likely the same uninitialized-DMA story but unproven. |
| `rt64_present_queue.cpp:160–340` | VI-follow override modes. Mode 3 (pick freshest by timestamp) is the working one. | Does Factor 5 cinematic violate RT64's VI heuristic assumptions about address pattern / update frequency? Unproven. |
| `rt64_state.cpp:1095–1110` | Defensive bounds check on combiner stack during late cinematic | Why does the cinematic workload sometimes overflow the combiner stack? |
| `rt64_framebuffer_manager.cpp:414–433` | Skip zero-width framebuffers / pixelSize==0 (G_IM_SIZ_4b) | Factor 5 LLE registers zero-width or 4b-sized framebuffers during cinematic. Probably the same garbage-bytes-as-CIMG issue. |
| `rt64_native_target.cpp:71–95` | Null guards on D3D12 resources + safer 64-bit address arithmetic | Guards prevent AVs but mean upstream is feeding bad pointers. Source unidentified. |
| `rt64_interpreter.cpp` | 50k-iter cap + drift detection on DL parse | Why does the DL drift into ASCII / pixel data sometimes? |
| Shade-fix (mode 1/2/3 selectable via `ROGUESQ_SHADE_FIX`) | Rewrites SHADE values to handle "black model top" symptom | Recompiled Factor 5 RSP produces SHADE values with zeros at top vertices. Either a recompiler RSP-translation bug or a mismatch in how SHADE is computed vs. the original ucode. |

### Cat C — Pure debug instrumentation

- ROGUESQ_LOG_RDP_STATE / LOG_TEXBYTES / LOG_PIPELINE / LOG_RT64_ALLOC / LOG_VI_FRESH / FILLCOLOR_DEBUG / DISABLE_Z_CMP env-gated traces (~600 LOC across rt64_rdp, rt64_framebuffer, rt64_workload_queue, rt64_buffer_uploader, rt64_present_queue).
- Per-mux color-keying diagnostic in `RasterPS.hlsl`, `RasterVS.hlsl`, `VideoInterfacePS.hlsl` (orange/purple/blue/pink overrides).
- `rt64_thread.cpp:50–65` env-gated thread-naming logs.
- Particle-visibility-debug shader pass.

### Cat D — Forgotten / dead

- `if(false) fprintf(...)` dead-code prints (~15 instances in rt64_gbi_*).
- `#if 0` blocks in shaders (~80 LOC).

### What could leave the fork

- The entire Factor 5 backend (`rt64_gbi_f3dfactor5.cpp` + the enum hook) **could** live in our repo behind a build flag if RT64 grew an extension API. It doesn't (yet), so for now it has to stay in the fork — but it's a candidate for the smallest-possible "register a custom GBI from outside" upstream contribution.
- The shade-fix / particle-alpha-fix / VI-follow-freshness logic could be wrapped in a `RogueSquadronPatches` module called from RT64 hooks. Same caveat — RT64 needs hooks first.

## Surface 2 — `lib/N64ModernRuntime` (forked at MikeSemicolonD/N64ModernRuntime)

Fork point: `0bb76b0f`. 9 commits since fork. 18 files modified, +888 / -28
lines.

### Cat A — Necessary runtime extension

- **DPC bridge declarations** in `librecomp/include/librecomp/rsp.hpp:107–122` (`g_rsp_dpc_start/end`, `rsp_dpc_submit`, `RSP_DPC_*` macros). LLE-from-recompiled-ucode requires this; could be upstreamed since other custom-ucode games would need it too.
- **RDP range submission API** added to `ultramodern/include/ultramodern/events.hpp` and `renderer_context.hpp` (`submit_rdp_range`, `submit_rdp_range_batch`, virtual `send_rdp_range`).
- **Thread context magic sentinel** at `ultramodern/include/ultramodern/ultramodern.hpp:25–30` and validation in `threads.cpp:159–222`. SEH-wrapped magic-load + VirtualQuery page-state check. Detects corrupt OSThread context pointers before signal() deref. Solid defensive programming.
- **SEH exception propagation on Windows threads** (`recomp.cpp:478–514`). Earlier behavior was catch-and-exit which lost minidumps. Necessary fix.
- **Overlay auto-load on PI DMA** (`librecomp/src/pi.cpp:288–302`). When a ROM-to-RDRAM DMA brings in overlay sections, we trigger `load_overlays()` to register the recompiled functions in the func map. **This is Rogue-Squadron-specific policy** — most games preregister overlays at boot. Could/should move to our repo.
- **Stub functions** for things the game expects to succeed: `osViGetCurrentField` (returns 0), pak/PFS no-pak stubs, eep stubs, cont rumble stubs.
- **VI mode deep-copy** in `events.cpp:95–98, 104–108`. Factor 5 reuses the OSViMode pointer; without the deep-copy we read garbage on next VI tick.
- Dummy VI mode initialization to avoid null on first `update_vi()`.

### Cat B — Defensive guards (root cause unknown)

| Location | Guard | Unknown bug |
|---|---|---|
| `rsp.hpp:125–140` (`dma_rdram_to_dmem`, `dma_dmem_to_rdram`) | Clamp dram_addr to RDRAM bounds; skip IMEM-bit DMAs | Why does the graphics ucode run with uninitialized GPRs early in boot and issue DMAs to dram_addr ≥ 0x800000? Bootloader not running? Recomp GPR init bug? |
| `dp.cpp:45–54` (`osDpGetCounters`) | 64-bit VA safety on the buffer pointer; was previously truncating to uint32_t and AV'ing | Why does uint32_t truncation AV here specifically? Likely game-side pointer corruption. |
| `overlays.cpp:207–225` | Inverted-bounds guard on overlay section iteration (`it < upper` instead of `it != upper`) | When/why do overlay loads sometimes fail to match? Alignment? Misaligned overlay table? |
| `overlays.cpp:388–430` (`get_function(0)`) | Returns a stub thunk + caller-stack log instead of `assert(false) + exit` | Why are MIPS functions calling `get_function(0)` or unmapped addresses? Tail-return $ra leak suspected — recomp may be missing ctx->r31 updates at JAL sites. |
| `ultra_translation.cpp:68–72` (`osYieldThread`) | `std::this_thread::yield()` instead of asserting | Earlier `check_running_queue()` caused null deref. Why? Cooperative-yield invariant we don't hold? |
| `mesgqueue.cpp:219–237` (`do_send`) | Bails if mq->msg is non-canonical or msgCount==0 | Why does the message queue struct in RDRAM become corrupt during cinematic? Game-side scribble? |

### Cat C — Pure debug instrumentation (mostly env-gated)

- mqdiag telemetry + CSV export (`mesgqueue.cpp:7–70`).
- mqfocus.txt logging for 3 specific queues (`mesgqueue.cpp:190–205`).
- `[trace]` log lines across `cont.cpp`, `eep.cpp`, `pak.cpp`, `sp.cpp`, `vi.cpp`, `events.cpp`, `ultra_translation.cpp` — most are `if(false)` dead-code prints.
- `ROGUESQ_LOG_THREAD_LIFECYCLE`, `LOG_INIT`, `LOG_SP_TASKS`, `LOG_FRAME_RATE`, `LOG_THREADS` env-gated traces.
- Static counters in `cont.cpp` (g_cont_query_count, etc.).
- `[dma-7800]` DMA-trace logger with pattern filtering (`rsp.hpp:144–176`).

### Cat D — Forgotten / dead

- **Cinematic throttle scaffolding** in `events.cpp:27–66`. Disabled in May with a comment that the issue was SHADE=(0,0,0,0) not pacing. Just remove.
- Whitespace tweak in `recomp.cpp:466`.

### What could leave the fork

- **Overlay auto-load on PI DMA** (`pi.cpp:288–302`) is policy specific to Rogue Squadron — move it to our repo as a hook the runtime calls, or as a wrapper around `osPiStartDma`.
- **All `if(false)` dead-code traces across cont.cpp, eep.cpp, pak.cpp** — just delete them.
- **Cinematic throttle scaffolding** in events.cpp — delete; superseded.
- The mqdiag / mqfocus / DMA-trace instrumentation could move into our repo if the runtime exposed enough hooks. For now it lives in the fork by necessity, but it's all env-gated so it doesn't hurt.
- **Thread context magic** is genuinely useful and could plausibly upstream to N64ModernRuntime — it's a defense any recomp wants.

## Surface 3 — Our repo (`src/`) and recompiled MIPS output

### `src/main/main.cpp`

- **Cat A**: RSP microcode dispatch + audio task stub, Factor 5 boot ucode manual DMEM setup, SDL2 audio/window/gamepad infrastructure, ROM hash check.
- **Cat B**: STL bounds-check abort suppression (`_CrtSetReportHook` returning 1). The recompile occasionally triggers `vector subscript out of range`; suppressing it lets the game continue with possible visual glitches. Why? Unknown.
- **Cat C**: F12 minidump hotkey, mqdiag watchdog (3s interval), HWBP DR0 setup + VEH handler (env-gated), DbgHelp symbol resolution, minidump writer, SEH crash handler, SIGABRT handler. About 6 distinct subsystems, all useful for debugging but bloat in a release build.

### `src/rsp/dpc_bridge.cpp`

- **Cat A**: DPC protocol globals + FULL_SYNC detection, PIPESYNC filter (drops opcode 0x27 — without it RT64's action queue chokes and frame rate drops to ~5 fps), task-end FULL_SYNC injection.
- **Cat B**: 4 distinct guards
  - **Mid-frame FULL_SYNC** — paired with `rt64_state.cpp` skip in `fullSyncFramebufferPairTiles`. Cinematic emits FULL_SYNC mid-tile sometimes; we don't know why.
  - **Slot-dispatcher tracking** (`g_cine_current_slot`) — observes which cinematic slot owns which framebuffer. Helps catch corruption. Unknown bug: what writes a bad pointer to free-list head at iter ~750?
  - **CIMG OOB detection + drop** — high and low region addresses are dropped (env-gated, off by default — enabling it kills the 3D Factor 5 logo, which means some "OOB" CIMGs are real-but-weird Factor 5 emissions). Unknown root cause.
  - **Synthetic FULL_SYNC injection** at synthetic-halt — without it RT64 never sees frame completion. Why does the cinematic loop emit zero real FULL_SYNCs?
- **Cat C**: per-task RDP histogram, [early-dump] / [mem-dump] periodic probes, [dpc-cine] / [dpc-64tri] / [dpc-pak] / [trace] pretty-printers (all env-gated by `ROGUESQ_LOG_DPC`).

### `funcs_*.c` (auto-generated, hand-patched — REGENERATION-HOSTILE)

22 `// PATCH (2026-` markers + extensive macro instrumentation in
`funcs_27.c`. **All of this gets clobbered when N64Recomp regenerates.**

- **Cat B (~12 items)**: KSEG0 pointer validation across funcs_0/3/4/8/9/10/15 (heap allocator, linked lists, free-list dequeue, matrix multiply pointer). Pattern is consistent: defensive bounds-check on next-pointers before dereference; if non-canonical, bail or normalize. **Root cause for all of them: unknown.** The game's heap allocator is producing or consuming bad pointers somewhere upstream and we've been catching them at the use sites.
- **Cat B**: 0xFFFFFFFF→0 normalization in funcs_9 (3 sites). Sentinel interpretation issue — game treats -1 as "no link" but recomp doesn't.
- **Cat B**: zero-init memory guard in funcs_0. Game assumes zeroed heap; missing memset somewhere.
- **Cat C (~8 items)**: `[wp@*]` watchpoint probes, CINE_YIELD (per-iter I/O burst), CANARY_CHECK (free-list head monitor), CINE_BC (per-loop-call checkpoint), wp_chk helper.

### Top 5 most concerning Cat-B unknowns (across all surfaces)

1. **Slot-dispatcher / free-list corruption at ~iter 750**. Affects funcs_8, funcs_27, dpc_bridge.cpp. CANARY_CHECK observes a non-KSEG0 value at MEM[0x801163B0] (free-list head). What writes it? Unknown after weeks of instrumentation.
2. **KSEG0 pointer corruption in heap allocator / linked lists** (funcs_0, 3, 4, 9). Same family as above. We've never traced a single one of these to a writer.
3. **Synthetic FULL_SYNC required for cinematic** (dpc_bridge). Why does the cinematic emit zero real FULL_SYNCs? Is the ucode dispatch loop exiting early?
4. **OOB CIMG addresses from Factor 5 ucode** (rt64_rdp + dpc_bridge). Some are demonstrably uninitialized-DMA-buffer garbage (we proved this). Others (wide widths in valid addr range) are still mysterious.
5. **`get_function(0)` calls**: where does $ra come from when the game calls a null function pointer? Probably a recomp bug at JAL sites; never confirmed.

These are linked. (1), (2), (3) all worsen at the same iter ~750–810 mark
where the cinematic freeze happens. (4) is the same time window. They may
all be downstream symptoms of a single upstream issue — heap corruption,
memory-aliasing, thread-scheduling, or a missing init somewhere — that we
haven't isolated.

## Recommended cleanup plan (priority order)

### Tier 1 — Safe deletions (do anytime)

- **`if(false) fprintf(...)` dead code** in lib/rt64 (~15 instances) and lib/N64ModernRuntime (~9 instances in cont/eep/pak/sp). Just delete.
- **Cinematic throttle scaffolding** in `events.cpp:27–66` — disabled with a "this isn't the issue" comment. Delete.
- **Whitespace tweak** in `recomp.cpp:466`.
- **`#if 0` blocks** in shaders (~80 LOC).

Estimated: ~250 LOC of pure deletion, no behavior change.

### Tier 2 — Move into our repo (low risk)

- **Overlay auto-load on PI DMA** (`pi.cpp:288–302`) — Rogue-Squadron-specific. Move to a hook in our repo. Fork submodule loses one game-specific patch.
- **mqdiag / mqfocus / DMA-trace** infrastructure — could move if N64ModernRuntime exposed mq-event hooks; for now it stays.

### Tier 3 — Triage debug instrumentation (medium effort)

For every Cat-C item, ask:
1. What hypothesis was it hunting?
2. Was that hypothesis resolved, ruled out, or stalled?
3. Is the env var still useful?

Likely-deletable after triage:
- HWBP DR0 watchdog (rdram+0x3CBC4 — months of instrumentation, root cause unfound; if no plan to keep investigating, delete).
- `[wp@*]` watchpoint probes in funcs_*.c (the per-call wp_chk helpers).
- Most `ROGUESQ_LOG_*` env vars that haven't been used in weeks.

A single `docs/debug-flags.md` should list every active flag and what it does.

### Tier 4 — `funcs_*.c` regeneration plan (real architectural decision)

The 22 PATCH markers + ~60 CINE_BC/CANARY_CHECK insertions are
**ephemeral**. Hand-editing `funcs_*.c` is **not normal recomp practice** —
Zelda64Recompiled, the reference implementation for this toolchain, never
edits the generated output. Instead:

#### How Zelda64Recomp does it (the gold standard)

A separate `patches/` directory contains hand-written C files that are
**cross-compiled to MIPS ELF**, then run through N64Recomp's single-file
mode to produce `RecompiledPatches/patches.c`, which gets linked alongside
the main recompiled output. The infrastructure pieces:

- `patches/Makefile` — invokes `clang -target mips -mips2 -mabi=32` on each `.c`, then `ld.lld` to link to `patches.elf`. Uses the decomp's headers (`-I ../lib/mm-decomp/include`).
- `patches/patches.ld` + `syms.ld` — linker scripts that resolve patches against the original game's symbols.
- `patches.toml` — N64Recomp config in single-file mode with `strict_patch_mode = true` (validates that patched function names correspond to real game symbols).
- `RecompiledPatches/patches.c` (generated) — gets compiled and linked into the main binary. Symbols in this output **override** the same names in `funcs_*.c` because they appear earlier in the link order.

When you write a function named `func_80007D74` in `patches/fixes.c`, the
linker picks it over the generated `func_80007D74` in `funcs_3.c`. Direct
C calls and indirect calls both go to your version. Generated `funcs_*.c`
is never touched.

#### Prerequisites for adopting this pattern

| Piece | Status |
|---|---|
| Decomp repo with headers | ✓ exists at `E:/Projects/rogue_squadron64/` |
| Decomp linker script with all game symbols | ✓ `roguesquadron.ld` |
| MIPS-targeting clang (`-target mips -mips2`) | ✗ not on PATH (Visual Studio's bundled clang may work; needs verification or WSL) |
| `ld.lld` for linker | ✓ ships with VS clang |
| Patches `Makefile` or CMake | ✗ to write |
| `patches.toml` | ✗ to write (template from Zelda's) |
| `patches.ld` + `syms.ld` | ✗ to derive from decomp's `roguesquadron.ld` |
| CMakeLists.txt integration | ✗ to wire in (invoke patches build, link result) |

#### Migration plan (multi-day, in this order)

1. **Stand up the patches build.** Get one trivial patch compiling end-to-end — e.g. a no-op replacement of any leaf function. Verifies toolchain + linker + N64Recomp single-file mode + integration into our build. No game-behavior change yet.
2. **Migrate one real Cat-B guard as proof.** The KSEG0-guard at `funcs_3.c:L_80007E30` (added in this session) is small and well-understood — copy the original generated `func_80007D74` body into `patches/heap_guards.c`, add the guard, link. Verify the guard fires in a run and the game still works.
3. **Migrate the remaining 21 PATCH markers** one-by-one across `funcs_0/3/4/8/9/10/15/27.c`. Each becomes a function in `patches/`. Generated `funcs_*.c` go back to clean N64Recomp output.
4. **Delete the Cat-C debug macros** (`CINE_YIELD`, `CINE_BC`, `CANARY_CHECK`, `wp_chk`) from `funcs_27.c` outright. They were debug-only; nothing in `patches/` needs them.
5. **Regenerate funcs_*.c from a clean N64Recomp run** to confirm the migration is complete — none of our work should be lost.

#### Honest cost estimate

Setting up the toolchain (step 1) is probably 2-4 hours assuming clang+lld
work without surprises. Migrating each guard (steps 2-3) is ~15-30
minutes per function once the pattern is established — call it half a day
for all 22. So **a full day's work, possibly two**.

The output: `funcs_*.c` becomes regeneration-safe. Future N64Recomp updates
just work. The patch surface is visible (everything in `patches/` is "our
modifications"), reviewable, and version-controllable independently.

#### Interim recommendation if not committing to the full migration yet

- **Stop adding new patches to `funcs_*.c`.** Anything new goes in `src/` (for what runtime hooks support) or waits on the patches infrastructure.
- **Document each existing patch.** A short note next to each `// PATCH (2026-` marker explaining the unknown bug it guards. Currently most have inline comments — verify each one is informative.
- **Remove the Cat-C debug macros from `funcs_27.c`.** They're debugging cruft that has no business surviving regen, and removing them is a no-risk no-build-change cleanup.

### Tier 5 — Real root-cause work (the deferred investigations)

For each of the Top 5 Cat-B unknowns, the audit recommends:
- Document the hypothesis tree (what's been ruled out, what hasn't).
- Pick one and commit to a real investigation — heap-layout audit, malloc/free instrumentation, RDRAM-write tracking, or a comparison capture against PJ64.

The slot-dispatcher hang at iter 750 is the most-instrumented and
longest-deferred — it's the place where rooting out one upstream bug would
likely retire 5–10 of our defensive guards downstream.

## Honest takeaway

The user's intuition is correct: we patched in too many places, and a lot
of those patches are bug workarounds whose root cause we never proved. The
codebase isn't broken — but it's brittle, the surface area of "things that
might have been right" is large, and the ratio of debug cruft to real
integration is uncomfortable.

Tier 1 + Tier 2 + the `funcs_27.c` macro removal would be a one-day
cleanup that drops ~400 LOC of cruft without risking the build. Tier 3
(debug-flag triage) is a half-day. Tier 4 (regeneration plan) is a real
architectural decision that should happen before any future N64Recomp
update is taken.

Tier 5 — actually investigating the root causes — is what the user has
been deferring all session. None of the cleanup tiers above replace it.
The defensive guards stay load-bearing until we understand what they're
guarding against.
