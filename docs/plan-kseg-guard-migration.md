# KSEG0 inline guard → patches.hook migration plan

## Why

There are ~25 KSEG0 pointer guards inlined into `E:/Projects/N64Recomp/RecompiledFuncs/funcs_*.c`.
Every regen of N64Recomp wipes them. The N64Recomp skill is explicit:
diagnostic and small surgical hooks belong in `[[patches.hook]]` entries
in the recompile toml, not edited into the auto-generated output.

This plan migrates the inline guards to declarative `[[patches.hook]]`
(and where needed, `[[patches.instruction]]`) entries in
`E:/Projects/N64Recomp/rogue_squadron.toml`. After migration, regens
produce clean output and the guards survive.

## The four tiers

Each guard falls into one of four migration tiers based on its body shape.

### Tier 0: dead code (delete on regen, no migration needed)

The function is already overridden by `patches/*.c` (e.g. `heap_guards.c`).
The inline guard exists in the auto-generated source but the linker uses
the patch override, so the inline version never executes.

| File:Line | Function | Status |
|---|---|---|
| funcs_3.c:869 | func_80007D74 | overridden by patches/heap_guards.c |
| funcs_3.c:932 | func_80007D74 | overridden by patches/heap_guards.c |

**Action**: delete on next regen. No toml entry needed.

### Tier 1: entry-bail (single hook, simplest)

Guard at function entry that returns/bails when an arg is invalid:

```c
RECOMP_FUNC void func_X(uint8_t* rdram, recomp_context* ctx) {
    if (((uint64_t)ctx->r4 & 0xFFFFFFFFE0000000ULL) != 0xFFFFFFFF80000000ULL) {
        return;
    }
    // function body
}
```

**Recipe**: single `[[patches.hook]]` with no `before_vram` (= function entry).

```toml
[[patches.hook]]
func = "func_800646AC"
text = '''{ if (((uint64_t)ctx->r4 & 0xFFFFFFFFE0000000ULL) != 0xFFFFFFFF80000000ULL) return; }'''
```

| File:Line | Function | Notes |
|---|---|---|
| funcs_15.c:550 | func_800646AC | r4 only |
| funcs_4.c:27265 | func_800191C4 | r4 AND r5 (matrix-mul guard) |

### Tier 2: goto-existing-label (single hook)

Guard mid-function that uses `goto L_X` where L_X is already a real label
in the recompiled output. `goto` works inside the hook because the spliced
C is part of the same function scope.

```c
// 0x80007A1C: lw $v1, 0x0($v1)
ctx->r3 = MEM_W(ctx->r3, 0X0);
// guard:
if (((uint64_t)ctx->r3 & 0xFFFFFFFFE0000000ULL) != 0xFFFFFFFF80000000ULL) {
    ctx->r2 = 0;
    goto L_80007A2C;
}
// 0x80007A20: lw $v0, 0x0($v1)
ctx->r2 = MEM_W(ctx->r3, 0X0);
```

**Recipe**: single `[[patches.hook]]` with `before_vram` = address of the
instruction the guard precedes.

```toml
[[patches.hook]]
func = "func_800079F0"
before_vram = 0x80007A20
text = '''{ if (((uint64_t)ctx->r3 & 0xFFFFFFFFE0000000ULL) != 0xFFFFFFFF80000000ULL) { ctx->r2 = 0; goto L_80007A2C; } }'''
```

| File:Line | Function | before_vram | Goto label |
|---|---|---|---|
| funcs_3.c:184 | func_800079F0 | 0x80007A20 | L_80007A2C |
| funcs_3.c:205 | func_800079F0 | (after L_80007A2C) | L_80007A40 |
| funcs_8.c:12139 | func_8003E8DC | (TBD lookup) | L_8003EA08 |
| funcs_8.c:12587 | func_8003EA4C | 0x8003EBA4 | L_8003EBFC |

### Tier 3: replace-load-with-default (instruction NOP + hook)

Guard that wraps a single load: when the pointer is invalid, substitute
`r2 = 0` instead of dereferencing. The original load at vram X must NOT
execute.

```c
// 0x80007DE8: lw $v0, 0x0($v0)
if (((uint64_t)ctx->r2 & 0xFFFFFFFFE0000000ULL) != 0xFFFFFFFF80000000ULL) {
    ctx->r2 = 0;
} else {
    ctx->r2 = MEM_W(ctx->r2, 0X0);
}
```

**Recipe**: TWO toml entries.

1. `[[patches.instruction]]` to overwrite the LW with NOP (`0x00000000`):
   ```toml
   [[patches.instruction]]
   func = "func_8000CFD4"
   vram = 0x8000D324
   value = 0x00000000   # was: lw $v0, 0x0($v0)
   ```
2. `[[patches.hook]]` BEFORE the (now-NOP'd) instruction to do the conditional load:
   ```toml
   [[patches.hook]]
   func = "func_8000CFD4"
   before_vram = 0x8000D324
   text = '''{ ctx->r2 = (((uint64_t)ctx->r2 & 0xFFFFFFFFE0000000ULL) == 0xFFFFFFFF80000000ULL) ? MEM_W(ctx->r2, 0X0) : 0; }'''
   ```

| File:Line | Function | vram (load) | Notes |
|---|---|---|---|
| funcs_3.c:15862 | func_8000CFD4 | 0x8000D324 | heap free-list head deref |
| funcs_4.c:3179 | func_80010014 | 0x80010830 | heap free-list head deref |
| funcs_4.c:21004 | func_80016C44 | 0x80016F18 | heap free-list head deref |
| funcs_27.c:21020 | func_800A98AC | (TBD lookup) | list-walk MEM_W with diagnostic |

### Tier 4: conditional-skip-store (instruction NOP + hook variant)

Guard that wraps a STORE: when the pointer is invalid, skip the write.
Same 2-step pattern as Tier 3 but for `sw` not `lw`.

```c
// 0x80016F28: sw $zero, 0x4($v0)
if (((uint64_t)ctx->r2 & 0xFFFFFFFFE0000000ULL) == 0xFFFFFFFF80000000ULL) {
    MEM_W(0X4, ctx->r2) = 0;
}
```

**Recipe**:
```toml
[[patches.instruction]]
func = "func_80016C44"
vram = 0x80016F28
value = 0x00000000   # was: sw $zero, 0x4($v0)

[[patches.hook]]
func = "func_80016C44"
before_vram = 0x80016F28
text = '''{ if (((uint64_t)ctx->r2 & 0xFFFFFFFFE0000000ULL) == 0xFFFFFFFF80000000ULL) MEM_W(0X4, ctx->r2) = 0; }'''
```

| File:Line | Function | vram (store) |
|---|---|---|
| funcs_4.c:3197 | func_80010014 | 0x80010840 |
| funcs_4.c:21016 | func_80016C44 | 0x80016F28 (first occurrence) |
| funcs_4.c:21023 | func_80016C44 | 0x80016F28 (second/skip path) |

## Per-guard inventory (full list)

Total: ~25 guards across 7 files (excluding 2 dead-code guards in func_80007D74).

| File | Line | Function | Tier | Notes |
|---|---|---|---|---|
| funcs_0.c | 3307 | func_80001A48 | 2 or 3 | heap walker bounds |
| funcs_0.c | 4495 | func_800022F8 | 2 or 3 | heap-block walker |
| funcs_0.c | 4553 | func_800022F8 | 2 or 3 | same family |
| funcs_3.c | 184 | func_800079F0 | 2 | goto L_80007A2C |
| funcs_3.c | 205 | func_800079F0 | 2 | goto L_80007A40 |
| funcs_3.c | 869 | func_80007D74 | 0 | DEAD (patches/heap_guards.c) |
| funcs_3.c | 932 | func_80007D74 | 0 | DEAD (patches/heap_guards.c) |
| funcs_3.c | 15862 | func_8000CFD4 | 3 | replace lw at 0x8000D324 |
| funcs_4.c | 3179 | func_80010014 | 3 | replace lw at 0x80010830 |
| funcs_4.c | 3197 | func_80010014 | 4 | skip sw at 0x80010840 |
| funcs_4.c | 21004 | func_80016C44 | 3 | replace lw at 0x80016F18 |
| funcs_4.c | 21016 | func_80016C44 | 4 | skip sw at 0x80016F28 |
| funcs_4.c | 21023 | func_80016C44 | 4 | skip sw at 0x80016F28 (second copy) |
| funcs_4.c | 27265 | func_800191C4 | 1 | entry-bail (r4 AND r5) |
| funcs_4.c | 27266 | func_800191C4 | 1 | (same block, lines 27265-27266) |
| funcs_8.c | 12139 | func_8003E8DC | 2 | goto L_8003EA08, sets r17 |
| funcs_8.c | 12587 | func_8003EA4C | 2 | goto L_8003EBFC |
| funcs_9.c | 16 | func_8003EC10 | TBD | re-read |
| funcs_9.c | 17 | func_8003EC10 | TBD | re-read |
| funcs_9.c | 212 | func_8003EC10 | TBD | re-read |
| funcs_9.c | 260 | func_8003EC10 | TBD | re-read |
| funcs_9.c | 3745 | func_8003FFEC | TBD | bail with full epilogue |
| funcs_10.c | 6271 | func_80047368 | TBD | re-read |
| funcs_15.c | 550 | func_800646AC | 1 | entry-bail (r4) |
| funcs_27.c | 6298 | func_800A5D80 | TBD | loop-top scrub |
| funcs_27.c | 13620 | func_800A8420 | TBD | chain-deref |
| funcs_27.c | 13629 | func_800A8420 | TBD | chain-deref second |
| funcs_27.c | 21020 | func_800A98AC | 3 | replace MEM_W with diagnostic |

## Execution order (recommended)

1. **Tier 1** first (2 guards): test the pattern with the simplest cases.
   Build, run, verify behavior unchanged.
2. **Tier 2** (~5-7 guards): exercise `before_vram` + `goto`. Build, run.
3. **Tier 3** (~5-6 guards): exercise the NOP + hook pair. Build, run.
4. **Tier 4** (~3 guards): variant of Tier 3. Build, run.
5. **TBD-marked guards**: re-read each, classify, migrate.
6. **Regenerate** N64Recomp output. Verify no inline guards remain.
7. **Final build + cinematic test** end-to-end. Behavior should match
   pre-migration runs.

## Migration status (2026-05-09 afternoon)

**Done** — already in `E:/Projects/N64Recomp/rogue_squadron.toml`:
- Tier 1: 2/2 (`func_800646AC`, `func_800191C4`)
- Tier 2: 4/4 (`func_800079F0` ×2, `func_8003E8DC`, `func_8003EA4C`)
- Tier 3: 4/4 (`func_8000CFD4`, `func_80010014`, `func_80016C44`,
  `func_8003EC10` lb-guard at 0x8003ED04)
- Tier 4: 3/3 (`func_80010014`, `func_80016C44`, `func_8003EC10`
  sw-guard at 0x8003ED3C)
- TBD-migrated:
  - `func_8003EC10` 0xFFFFFFFF→0 normalize ×3 at 0x8003ECB8/0x8003ECC4/0x8003ED18
  - `func_800A5D80` loop-top free-list scrub at L_800A6024
  - `rs_free` walker bounds at 0x80001D84
  - `func_800022F8` walker bounds at 0x80002434

**Still TODO** — complex multi-instruction patterns; need careful
disassembly + matching label resolution:
- `func_800A8420` chain-deref guards (multi-instruction NOP at
  0x800A87A0/0x800A87A4/0x800A87A8 + conditional logic)
- `func_8003FFEC` bail-with-full-epilogue at 0x800400AC
- `func_80047368` chain-deref at 0x80047538

**Verified post-migration** (2026-05-09):
- Regen runs clean (0 errors).
- Build runs clean.
- Game launches and runs without crashing.
- Cinematic still produces empty GFX tasks (tri=0 texrect=0). The
  remaining TODO guards may be needed to break the cinematic stall —
  pre-rebase fork had all 25 inline; migrated subset is ~22.

## Critical files

- `E:/Projects/N64Recomp/rogue_squadron.toml` — where all hook entries go
- `E:/Projects/N64Recomp/RecompiledFuncs/funcs_*.c` — current inline guards
- `e:/Projects/RogueSquadron64Recomp/patches/heap_guards.c` — pattern reference for Tier 0

## Open questions

- **~~Does our N64Recomp build still work for full regen?~~** **RESOLVED 2026-05-09 morning.**
  Regen now runs clean. Required adding to `rogue_squadron.toml`:
  - 4 `[[patches.instruction]]` NOPs in `func_80018D80` for cache ops at 0x80018D88-0x80018D94
  - Stub for `func_80018ED4` (broken-looking cache-flush loop)
  - Stub for `zmemcpy` (libultra name; works as stub-by-libultra-name, runtime provides)
  - 23 `[[patches.instruction]]` NOPs across 8 game functions (`func_8009264C`,
    `func_800928D4`, `func_80092BA8`, `func_80092E48`, `func_80092F10`,
    `func_8009314C`, `func_800932C4`, `func_800934A4`) for their cache ops.
  - Note: `osInvalDCache`/`osInvalICache`/`osWritebackDCache`/`osWritebackDCacheAll` are
    runtime-provided (N64Recomp treats them as imports), so their cache instructions
    are NOT recompiled and don't need patches.
  - Result: clean regen, funcs.h stays at 151KB (full size), 0 errors.
  - **Caveat**: regen WIPES the inline KSEG0 guards. Don't run regen until
    the `[[patches.hook]]` migration entries are in place.
- **Tier-3 hook + NOP semantics**: still need to confirm via build + runtime test
  that the hook executes BEFORE the NOP (i.e. that `[[patches.hook]]` injects
  strictly before `[[patches.instruction]]`'s output for the same vram).

## Out of scope (defer)

- Migrating `lib/N64ModernRuntime` fork's tracing fprintfs (cont.cpp, dp.cpp,
  pak.cpp, etc.) to `[[patches.hook]]`. Same pattern, larger surface — own plan.
- Auditing `set_post_pi_dma_callback` necessity vs. standard `register_overlays`.
- Stripping `lib/rt64` fork mods (already on hold per "no more RT64 mods" rule).
