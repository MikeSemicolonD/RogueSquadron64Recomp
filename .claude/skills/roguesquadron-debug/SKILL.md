---
name: roguesquadron-debug
description: Debug and iterate on RogueSquadron64Recomp (the Star Wars Rogue Squadron N64 static-recompilation port; N64Recomp + N64ModernRuntime + RT64, Windows/MSVC-clang) using evidence, not screenshots. Use for ANY crash/hang/freeze/AV triage, for landing a durable game-logic override, or for proving a change correct WITHOUT asking the user to eyeball a frame. Covers: symbolizing access violations to the exact recompiled function (llvm-symbolizer against the PDB) instead of trusting a memorized RVA; reading the [cine-progress]/[vi]/[musyx-run] freeze signature; killing the game with taskkill (never TaskStop); reaching any attract demo fast (ROGUESQ_SKIP_DEMO + ROGUESQ_CINE_FASTFWD); landing a game-logic override as a rogue_squadron.toml entry hook (covers func_map/LOOKUP_FUNC indirect calls); and perception-free validation via RDRAM dumps (ROGUESQ_DUMP_RDRAM_AT_VI), offline DL walks (f5_dl_walk.py --tex), offline texture decode from a dump, the TMEM-state trace (ROGUESQ_LOG_CI4_TMEM), PJ64 golden diffs (rdram_golden_diff.py / dl_diff.py) and MusyX audio diffs (audio_cmd_walk.py / audio_diff.py). Triggers on: access violation / C0000005 / av_rva, SEH caught, demo/cinematic freeze or deadlock, "which function crashed", getNpcCurrentHealth, patches build / PatchesLib / /FORCE:MULTIPLE / override didn't take effect, ROGUESQ_* env vars, RDRAM dump, TMEM, taskkill, and "how do I know this is correct without a screenshot". Complements f5-dl-validate (F5 DL/opcode diffing), n64recomp-integration (recompiler + patches pipeline) and rt64-integration (renderer internals); this skill is the top-level debug/validate loop that ties them together. Read AGENTS.md first for the authoritative project map.
---

# RogueSquadron64Recomp — Debugging & Validation

Audience: AI agents working in this repo. This is the top-level "how to make progress without
guessing" skill. It encodes methods that were learned the hard way; the sibling skills go deeper on
their sub-domains (`f5-dl-validate` = F5 DL diffing, `n64recomp-integration` = recompiler/patches
pipeline, `rt64-integration` = renderer). **Always read `AGENTS.md` first** — it is the authoritative
map (layout, build/run, env-var catalog, architectural quirks, disproven dead ends).

## The prime directive: evidence over eyeballs

Graphics are HLE, so a frame cannot be pixel-compared to hardware and you cannot reliably judge "looks
right." Do **not** build a loop of screenshot -> ask the user "is this correct?" That wastes the user
and produces unfalsifiable conclusions. Every claim must rest on a diff, a symbolized address, a walked
DL, decoded bytes, or a golden comparison. A screenshot is a *lead* (something looks off), never a
*verdict*. When you must look at a frame, immediately convert the lead into a byte-level check below.

Corollary learned this session: **never trust a remembered address across a rebuild.** An av_rva that
mapped to function X in one binary maps elsewhere after any regen/relink. Re-symbolize every time
(see below). A whole "fix validated" conclusion was once wrong because it grep'd a stale RVA.

## Running and killing the game

```bash
# Reach a specific attract demo in ~2 min and mute audio (headless-friendly):
cd build/Debug
ROGUESQ_AUDIO_GAIN=0 ROGUESQ_CINE_FASTFWD=200 ROGUESQ_SKIP_DEMO=<0-5> \
  ./RogueSquadron64Recomp.exe > /path/to/run.txt 2>&1 &
```
- `ROGUESQ_SKIP_DEMO`: 0=Mos Eisley/Tatooine, 1=Jade Moon, 2=Kile II, 3=Taloraan, 4=Fest, 5=Trench Run.
  Pins `demoId` every present so the attract mode loops that demo, skipping the gauntlet.
- `ROGUESQ_CINE_FASTFWD=200` blasts the intro cinematic. `ROGUESQ_AUDIO_GAIN=0` mutes.
- Capture stdout/stderr to a FILE (`> run.txt 2>&1`). Bash redirects don't flush before SIGTERM, but a
  detached run you later `taskkill` flushes fine. In PowerShell use the Start-Job pattern in AGENTS.md.
- **KILL WITH `taskkill //F //IM RogueSquadron64Recomp.exe`** (Git-Bash double-slash). The Claude
  `TaskStop`/killing the background task does NOT kill the detached game exe — it orphans it, and the
  orphan holds a lock on the .exe that blocks the next relink. Always taskkill before rebuilding.
- The game never self-exits; wrap a run as: launch detached, wait (background `sleep`), grep the log,
  taskkill. Don't foreground-`sleep` (blocked); use `run_in_background: true` on the wait+grep command.

## Crash / hang / freeze triage

### The freeze signature (read the log, don't watch the window)
A hang shows a distinctive pattern in the run log:
- `[cine-progress] iter=N delta=0 in 2000ms (idle=<growing>ms)` — the GAME thread is stuck; `delta=0`
  with `idle` climbing (to tens of seconds) = frozen. A healthy run shows `delta=~60 idle=~16ms`.
- `[vi] update_screen #N` KEEPS advancing and `[musyx-run]` keeps ticking even while cine is frozen.
  That combination = the game/CPU thread died or blocked while the render + audio threads run on. Almost
  always a **SEH-caught access violation on the game thread** left a subsystem inconsistent (e.g. the
  gfx-frame barrier), deadlocking the cooperative scheduler.

### Find the faulting instruction
Grep the log for the SEH line:
```
[recomp] SEH caught: thread_entry=0x... code=0xC0000005 kind=READ av_target=0x... av_pc=0x... av_rva=0xNNNNNN
```
- `code=0xC0000005` = hardware AV (our bug class). `code=0xE06D7363` = a C++ exception (usually benign,
  recovered by the outer catch — see AGENTS.md exception pipeline). Focus on C0000005 on the game thread.
- A **small** `av_target` (< 0x1000 + offset) = NULL+offset deref. A **huge** `av_target`
  (e.g. 0x1A2...) = a wild pointer computed from a bad base, then run through `MEM_W` (recomp.h) which
  does NO masking: `*(int32_t*)(rdram + ((reg+offset) - 0xFFFFFFFF80000000))`. An out-of-RDRAM guest
  address therefore dereferences host memory and AVs. This is the canonical guard target.
- **Symbolize the RVA against the PDB — every time, fresh:**
```bash
echo 0xNNNNNN | "C:/Program Files/LLVM/bin/llvm-symbolizer.exe" \
   --obj=build/Debug/RogueSquadron64Recomp.exe --relative-address
# -> functionName  E:\Projects\N64Recomp\RecompiledFuncs\funcs_NN.c:LINE:0
```
`llvm-nm` on the exe returns nothing (symbols live in the PDB, not the COFF table); the symbolizer uses
the PDB. Do this for EVERY distinct `av_rva` in the run (`grep -oE "av_rva=0x[0-9A-Fa-f]+" | sort -u`).

### Hang dumps (game "Not Responding")
- `pwsh tools/dump-game.ps1` captures the running process (works when the window is starved and F12
  can't fire). `python tools/inspect-dump.py` lists threads and tags the 2-4 `in_exe` ones worth opening
  in Visual Studio (File > Open > .dmp > Debug Native). `cdb.exe` is broken on this machine — use VS
  interactively or the `minidump` python package via inspect-dump.py; do not try to drive cdb.
- Crash dumps land in `dumps/crash-dumps/crash_*.dmp` (SEH handler / SIGABRT / F12 / dump-game.ps1).

### Crash-class cheatsheet
AGENTS.md "When investigating a new crash" lists the AV-address -> bug-class table (small vs huge AV,
the RT64 assertion classes, `vector subscript out of range`, `Failed to find function`, GBI-not-found).
Read it before instrumenting.

## Where and how to apply fixes

### NEVER hand-edit `E:/Projects/N64Recomp/RecompiledFuncs/funcs_*.c` for logic/guards.
Those files are auto-generated; the next regen silently strips inline edits. This has burned the project
repeatedly. Diagnostic `fprintf` probes there are fine (rate-limit them), but every load-bearing
override belongs in a `[[patches.hook]]` in `rogue_squadron.toml`.

### Hooks (the durable override mechanism)
See AGENTS.md "Overriding recompiled functions". A hook's C is compiled into the recompiled body (at entry
or `before_vram`), so it covers direct and `LOOKUP_FUNC`/`func_map` indirect calls alike. A bare `return`
exits the game function (set `ctx->r2`/`ctx->f0` first). Put real logic in an `extern "C"` helper in
`src/main/hook_helpers.cpp`. Regen (`regen_funcs`) after editing the toml, then grep `funcs_*.c` for the
helper name to confirm the hook landed.

Case study (jade-moon freeze, [[project-jade-moon-freeze-musyx-ucode-2026-09-13]]): `getNpcCurrentHealth`
has ZERO direct C callers; it is reached ONLY via `LOOKUP_FUNC(0x800F20EC)`. Its guard used to be a
`patches/` link-time override, which silently never ran while PatchesLib was a static lib. The guard is now
an entry hook on all five health accessors, which cannot lose the indirect path.

## Perception-free validation (prove correctness without a screenshot)

Pick the layer that matches the bug. All of these produce a byte/opcode/address-level verdict.

### 1. Symbolized crash proof
"Did my guard stop the crash?" -> re-run, `grep -cE "C0000005.*kind=READ" run.txt` on the game thread and
symbolize any survivors. Zero game-thread AVs + `[cine-progress]` advancing (delta>0, idle small) =
freeze fixed. This is the primary validation for stability fixes; no visual needed.

### 2. RDRAM dump + offline DL walk
Dump the parser-visible RDRAM at a chosen moment, then walk the display list offline:
```bash
# In the run, one-shot dump (un-swizzled, PJ64 big-endian layout):
ROGUESQ_DUMP_RDRAM_AT_VI=<n>  ROGUESQ_DUMP_RDRAM_PATH=dumps/foo.bin      # by present/VI count
#   other triggers: ROGUESQ_DUMP_RDRAM_ON_CINE_ITER=<n>, ON_SCENE=<id>, ON_SCREEN=<n|menu>, ON_CINE_STALL=<ms>
# Then walk it (auto-resolves the DL start from the task pointer W(0x377C8+0x30)):
python tools/validate/f5_dl_walk.py dumps/foo.bin --tex          # per-face UV + material summary
python tools/validate/f5_dl_walk.py dumps/foo.bin --json         # normalized records for dl_diff.py
```
`--tex` prints, per bound texture: `timg fmt siz tile WxH cms/cmt mask faces` and the UV range. Face
count + `end=clean/unknown` tells you the DL is structurally healthy. The `[vi] ... screen=N` log line
tells you what you dumped: `screen=4` = in-mission gameplay, `screen>=6` = cinematic. A gameplay dump is
now reachable because the structure-destruction demo freeze is fixed — earlier notes that say "can't
reach a demo frame" are stale.

The N64 format enum (do not misread): **0=RGBA, 1=YUV, 2=CI, 3=IA, 4=I**. `fmt=2 siz=0` = CI4 (the
dominant model format, palette bank 15), NOT RGBA16.

### 3. Offline texture decode from a dump (settle a "texture looks wrong" lead)
Decode the exact texel bytes from the RDRAM dump and inspect the pixels directly, instead of judging the
rendered frame. Read `dump[physAddr]` (physAddr = guest & 0xFFFFFF); CI4/I4 are 4bpp, 2px/byte, 32
bytes/row for a 64-wide tile; TLUT entries are RGBA5551 big-endian.
- **Get the palette address from evidence, not a guess.** The TLUT source is in the `loadTLUT` SETTIMG
  record (see the TMEM trace below); a wrong palette base makes a correct texture look like noise. This
  session wasted a decode on `0x627200` when the real bank was `0x6272A0`.
- To test the N64 odd-line swizzle: decode twice, once linear and once with odd rows' 32-bit (4-byte)
  halves swapped (`byte_in_row ^= 4` when `y & 1`). The version that yields a coherent image tells you
  the correct interpretation. A working reference generator lives in the scratchpad
  (`ci4_decode.py` / `i4_decode.py`) — copy and retarget `TEX`/`PAL`/`W`/`H`.

### 4. TMEM-state trace (settle load vs palette vs sample)
`ROGUESQ_LOG_CI4_TMEM=1` (target `ROGUESQ_CI4_TMEM_SRC=0xADDR`, default the hull CI4 `0x555490`) in
`lib/rt64/src/gbi/rt64_gbi_f5_rdpstate.cpp` prints the full chain for a texture: the `loadBlock`
params (src/tile/dxt/imgsiz/words), the load-tile descriptor, where `loadTLUT` deposited the palette
(`tmem=` word), the RENDER-TILE descriptor (fmt/siz/line/tmem/**pal bank**/mask/cms/cmt), and the first
CI4 rows of SRC vs the loaded TMEM. Use it to answer, with hard state:
- Is the palette bank right? N64 CI4 bank *b* is at TMEM word `256 + b*16`; compare to where loadTLUT
  landed. (Session result: bank 15 -> word 496, matched -> palette was NEVER the bug.)
- Is the source pre-swizzled, and does RT64's decoder handle it? RT64's `TextureDecoder.hlsli`
  (`implLoadTMEM`, `sampleTMEM`) already applies the odd-line `wordIndex ^ 1` swap with
  `stride = line<<3`; decode params are built in `rt64_texture_cache.cpp:~1199` (`decodeCB.stride =
  loadTile.line << 3`, siz, palette). If the swap logic + params are correct, the texture renders
  correctly and the visible "streak/blocky" is elsewhere (very often the **absent N64 VI post-filter**,
  not a mapping fault — see the model-texture memory). The blanket `ROGUESQ_CI4_DESWIZZLE` is a REFUTED
  dead end: it double-swaps already-correct paths and breaks terrain.

### 5. PJ64 golden diffs (the strongest oracle)
Capture a Project64 golden at a game-state LANDMARK (not a frame number — the VI loop is not
bit-deterministic) and diff:
- `tools/validate/capture_pj64_golden.ps1` / `capture_menu_rdram.ps1` — scripted goldens.
- `tools/validate/rdram_golden_diff.py` — RDRAM diff vs PJ64.
- `tools/validate/dl_diff.py` — layer-by-layer DL diff (pair with `f5_dl_walk.py --json` both sides).
- `tools/validate/checkpoint.ps1` — one-command capture+diff.
See `f5-dl-validate` for the full parser/DL/RDRAM-layer method and the landmark-not-timing rule.

### 6. Audio (perception-free)
`tools/validate/audio_cmd_walk.py` + `audio_diff.py` walk and diff the MusyX voice-command stream; a
cinematic golden is an active-voice oracle. `ROGUESQ_DUMP_PCM=<wav>` drives
offline capture. You never have to listen.

## Fast iteration loop
1. Reproduce with the tightest repro (`ROGUESQ_SKIP_DEMO` + `CINE_FASTFWD`), capturing to a log file.
2. Classify from the log: crash (symbolize the AV) vs hang (`[cine-progress]` delta=0) vs render (walk
   the DL / dump the texture).
3. Localize to a function (symbolizer) or an opcode/material (f5_dl_walk) or state (TMEM trace) — never
   stop at "looks wrong."
4. Fix in the right place: `rogue_squadron.toml` hooks for game logic, `lib/rt64` for render (scoped, env-
   gated; respect the "ask before new lib/rt64 changes" preference), never inline in `funcs_*.c`.
5. `taskkill` the game, rebuild (`cmake --build build --config Debug --target RogueSquadron64Recomp`),
   re-run, and validate with the SAME byte/address-level check — not a new screenshot.

## Known dead ends (don't re-derive)
AGENTS.md "Avoid these dead ends" is authoritative. Session-specific additions: link-time overrides
of recompiled functions (indirect calls silently missed them; use hooks); blanket CI4 deswizzle by address range (breaks terrain);
trusting a memorized `av_rva` across a rebuild; and treating a screenshot as a verdict. The KSEG0 pointer
guards are a tracked retirement, not cleanup fodder — leave them until the toml+regen migration.
