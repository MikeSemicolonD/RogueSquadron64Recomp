---
name: f5-dl-validate
description: Perception-free validation and bug-fixing of the Factor 5 (F3DFACTOR5) HLE display-list pipeline in RogueSquadron64Recomp by diffing recomp output against Project64 goldens at the RDRAM / DL-seam / parser layers, so rendering bugs are localized to an opcode and chunk without ever judging pixels. Use when investigating cinematic or in-mission rendering desyncs, garbage geometry after a sub-DL return, missing or exploding face counts, runaway / capped gfx-task parses, or validating any change to rt64_gbi_f3dfactor5.cpp or the F5 GBI. Covers capturing the parser-visible RDRAM snapshot at a game-state landmark, walking the DL to normalized records (tools/validate/f5_dl_walk.py --json), two-sided diffing (dl_diff.py), the one-command checkpoint driver (checkpoint.ps1), and the walker-adjudicates-grammar -> fix-live-parser -> rebuild -> validate-live loop. Triggers on f5_dl_walk, dl_diff, checkpoint.ps1, ROGUESQ_DUMP_PARSE_SNAPSHOT_ON_ITER, op_b4_quad / op_13_quad / f5_emit_face, f5_enter_chunk, F5 chunk/opcode/stride desyncs, and PJ64 golden RDRAM diffs.
---

# F5 Display-List Validation (perception-free)

## Purpose

This port is **HLE for graphics**: the CPU is a faithful recompilation but the graphics RSP ucode is
reimplemented by RT64's `f3dfactor5` GBI module, so a rendered frame cannot be pixel-compared to
hardware and an agent cannot reliably judge whether it "looks right." This skill validates and fixes
the graphics pipeline **without looking at pixels**, by comparing the byte-exact layers that *produce*
the picture against a Project64 golden and localizing any divergence to a specific opcode and chunk.

## When to use

- A scene renders garbage, is missing geometry, or shows "garbage right after a material sub-DL returns."
- Face counts explode or a gfx task is capped / trips the per-task budget (runaway parse).
- You changed `lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp` (or `f5_rdpstate`) and need to prove it.
- You want to know whether a "desync" is a real bug or a capture artifact.

## Core principle: partition, and stop at the first divergent layer

The bug is always in one of these layers. Check them in order; **stop at the first that diverges** and
never move to a lower layer until the higher ones are clean:

1. **LOGIC** (faithful CPU / game state) - `rdram_golden_diff.py`. A diff here = a recomp/logic bug; go RE it, do not touch graphics.
2. **DL SEAM** (the display list the game emitted) - does the recomp emit the same DL bytes as hardware?
3. **PARSER** (does `f3dfactor5` walk those bytes correctly) - `f5_dl_walk.py` + `dl_diff.py`.
4. **PIXELS** - HLE, not byte-comparable; human glance only. Never debug here from the terminal.

## Tools (all in `tools/validate/`)

- **`f5_dl_walk.py <dump.bin> [addr|task] [--json] [--b4 auto|16|32]`** - walks an 8 MB big-endian RDRAM
  dump with the F5 ucode's chunk rules. `--json` emits a normalized, address-normalized record stream
  (chunk addresses -> first-seen ordinals) that is diffable across dumps with different allocations.
  `--b4` is a stride-rule A/B knob (see the worked example).
- **`dl_diff.py <GOLDEN> <CANDIDATE> [addr|task]`** - runs the walker on both, reports the FIRST
  divergence (op + chunk ordinal + differing fields + context) and summary deltas. Same walker both
  sides, so walker blind-spots cancel for the seam question. Exit 1 on divergence.
- **`checkpoint.ps1 <cine_frameN|menu> [-SkipCapture]`** - one command: capture a recomp dump at a
  landmark, then run LOGIC + PARSER diffs and print a pass/fail table.
- **`rdram_golden_diff.py <GOLDEN> <CANDIDATE> [--focus A:S] [--json out]`** - the LOGIC layer (known
  globals, page regions, code/rodata integrity). Symbols auto-load from the decomp `symbol_files`.

Goldens live in `dumps/pj64/rdram_<tag>.bin` (cine_frame{1,78,79,120,300,600,900}, cine_iter1,
menuinit1_screen9, ...); capture more with `capture_pj64_golden.ps1` + `pj64_rs64_dump.js`.

## The parser-visible snapshot dump (the correct DL oracle)

**A live-RDRAM dump is NOT a valid oracle for the DL region** - the game recycles DL chunks a few ms
after the RSP reads them, and RT64's parse is slower, so a live dump catches the pool mid-rebuild and
diffs are non-deterministic. Dump the exact bytes the parser walked instead:

```
ROGUESQ_DUMP_PARSE_SNAPSHOT_ON_ITER=<n>   # arm on the cinematic-loop iteration (rs64_cine_iter_get);
ROGUESQ_DUMP_PARSE_SNAPSHOT_COUNT=<m>     #   NOT the raw 0x13889C tick - see gotchas
ROGUESQ_DUMP_PARSE_SNAPSHOT_PATH=<base>   # writes <base>_task00.bin .. numbered, big-endian
```
Implemented in `lib/N64ModernRuntime/ultramodern/src/events.cpp` right after `send_dl` (the
`g_rs64_parse_rdram` / task-submit snapshot). Off by default.

## The loop (a worked capture)

1. **Capture** the recomp's parser snapshots at the target scene, several tasks (the game runs several
   gfx tasks per frame - 3D scene + 2D overlay). Launch the Debug binary headless with
   `ROGUESQ_FAKE_CONTROLLER=1`, `ROGUESQ_DUMP_PARSE_SNAPSHOT_ON_ITER=<n>`, `_COUNT=8+`, `_PATH=...`,
   optionally `ROGUESQ_LOG_GFX_TASK=1`; poll for the dump files; kill.
2. **Profile** each `_taskNN.bin`: `f5_dl_walk.py <f> --json` -> read `summary.faces/rects/end`. Pick
   the task whose **face count matches the golden's** (the 3D scene, not the 2D/rect-heavy task).
3. **Diff** it: `dl_diff.py dumps/pj64/rdram_cine_frameN.bin <matched_task>.bin`. Identical = the DL is
   byte-faithful to hardware. A named divergence = a real bug at that op/chunk.
4. If the LOGIC layer is in doubt, run `checkpoint.ps1 cine_frameN` for the full pass/fail table.

## Critical gotchas (each one cost a wrong turn once)

- **Endianness.** All dumps are hardware big-endian (`rdram[i^3]`; PJ64 stores little-endian
  internally, N64Recomp byteswapped-per-word). The walker/diff read big-endian; word reads honor `^3`.
- **Landmark counter.** `rs64_cine_iter_get()` (host atomic, ticks with the cinematic loop) is the SAME
  landmark as the PJ64 `cine_frame<n>` goldens and `ROGUESQ_DUMP_RDRAM_ON_CINE_ITER`. RDRAM `0x13889C`
  is a DIFFERENT counter that lands in the 2D intro - do not anchor to it.
- **Multi-task-per-frame.** Match the golden's task by face profile, not by "first task."
- **Face count animates** (~+5 per cine_iter). To hit the golden's exact count, arm a few iters early
  and `_COUNT` a bracket, then pick the matching task.
- **`g_current_scene` is never written** at runtime (stays -1); `ROGUESQ_DUMP_RDRAM_ON_SCENE` never
  fires. Use `g_active_overlay` / `screenState` (0x130B14) / cine_iter.
- **`ROGUESQ_DUMP_FRAME_DL` is stderr text**, not a byte artifact - don't diff it; dump RDRAM instead.
- Anchor to a **game-state landmark**, not wall-clock/frame timing - the VI loop is not bit-deterministic,
  but RDRAM content at a landmark is.

## Fixing a parser bug: walker adjudicates the grammar, then match the live module to it

The walker encodes the DL grammar and is validated byte-for-byte against hardware. When the live
`f3dfactor5` module disagrees with the walker, the live module is usually wrong. Prove it, fix it,
validate it:

1. **Find** the divergence with `dl_diff.py` (or by inspecting handler code vs the walker rules).
2. **Prove** which rule is correct: find a golden that exercises the case, and show the walker's rule
   walks it `end=clean` while the wrong rule (via an A/B knob like `--b4 32`) `RUNAWAY`s. A control
   dump that doesn't exercise the case should be identical either way.
3. **Fix** the live handler in `rt64_gbi_f3dfactor5.cpp` to match the proven grammar. Mirror a sibling
   handler that already does it right (e.g. `op_13_quad` for quad variants).
4. **Rebuild** `cmake --build build --config Debug` (incremental ~10 s; expect the benign duplicate
   `*_recomp` symbol warnings - lld picks the override).
5. **Validate live**: capture snapshots at a scene that exercises the case; confirm they walk clean and
   the live parser's `g_f5_task_faces` (via `ROGUESQ_LOG_GFX_TASK=1`) is bounded with no budget trip /
   runaway. Confirm an unaffected scene is unchanged (no regression).

### Reference example (2026-09-09): the 0xB4 stride bug

`op_b4_quad` advanced an unconditional 32 bytes and always read UVs, but `0xB4` is a quad variant like
`0x13`: textured (`w0&2`) = 32B with a UV block, untextured = 16B without. On untextured `0xB4` the old
code read the next command as bogus UVs and over-advanced 16B -> the parse ran away.
- Proof: `f5_dl_walk.py cine_frame300.bin` (has 32 untextured `0xB4`) -> clean, 267 faces; same with
  `--b4 32` (the live rule) -> RUNAWAY, 11382 faces. `cine_frame78` (all textured) identical either way.
- Fix: made `op_b4_quad` conditional, mirroring `op_13_quad`.
- Validated: at cine_iter 250 (44-202 untextured faces/task) snapshots walk clean, live max faces 1433
  (was ~11k), no budget trips; textured path unchanged.

## What this proves, and what it doesn't

Proves the DL **content and parse** for the captured task/frame are faithful to hardware. Does NOT by
itself prove other frames, the 2D/overlay task, texturing, or the final HLE pixels - point the same
loop at those next. See memory `project_validation_harness_2026_09_09` for the full history.
