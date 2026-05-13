# Symbol-rename tooling

Helpers for propagating semantic names through the
`symbol_files/*.txt` → `roguesquadron.elf` → `N64Recomp` → `RecompiledFuncs/`
pipeline.

## Paths

All scripts default to these paths (relative to a sibling-repo layout):

| Var               | Default                                           |
|-------------------|---------------------------------------------------|
| `RSR_SYM_DIR`     | `E:/Projects/rogue_squadron64/symbol_files`       |
| `RSR_ASM_DATA_DIR`| `E:/Projects/rogue_squadron64/asm/data`           |
| `RSR_FUNCS_DIR`   | `E:/Projects/N64Recomp/RecompiledFuncs`           |
| `RSR_REDEFS_PATH` | `E:/Projects/rogue_squadron64/build/redefs.txt`   |

Override any of them with an environment variable before running.

## Scripts

### `build_redefs.py`
Generates the `--redefine-syms` file for `llvm-objcopy` from every semantic
entry in `symbol_files/*.txt`. For each `name = 0xADDR;` emits three rename
lines (`func_HHHHHHHH`, `D_HHHHHHHH`, `fake_func_HHHHHHHH` → `name`) so the
pass catches whichever placeholder splat actually emitted. Appends explicit
reverts for the `setGlobalByte_8011A8XX` cluster (splat-provided names that
violate the "no address-encoded identifiers" rule).

```sh
python tools/rename/build_redefs.py
```

### `find_callers.py`
Lists every function in the recompiled C that calls a given target. Used
constantly when deciding whether a target function has enough call-site
context to name confidently.

```sh
python tools/rename/find_callers.py initMission
python tools/rename/find_callers.py func_8006E468
```

### `find_state_machines.py`
Scans unnamed functions (`func_8XXXXXXX`) for state-machine shape. Two signals:
1. Jump-table dispatch: body contains a `switch_error(...)` call (N64Recomp's
   marker for compiler-generated switch tables).
2. Repeated `MEM_W` writes to the same offset — usually a state variable.

Jump-table candidates print first (highest confidence).

```sh
python tools/rename/find_state_machines.py
```

### `cleanup_placeholders.py`
Removes redundant `func_HHHHHHHH` / `D_HHHHHHHH` entries from each symbol
file when a semantic name for the same address already exists in the same
file. Safe to re-run; idempotent.

```sh
python tools/rename/cleanup_placeholders.py
```

### `rename_asciz_strings.py`
Bulk-renames `D_HHHHHHHH = ...; // type:asciz` entries by reading the actual
literal text from splat's `.rodata.s` output and producing `strXxxx`
identifiers. Skipped entries: too short (<3 chars), too long (>40), non-printable,
already-used name, or name with no camel-case content. Per-overlay prefix:
`strMiss` / `strMenu` / `strCin` / bare `str` for main_overlay.

```sh
python tools/rename/rename_asciz_strings.py
```

## Pipeline

The typical loop after editing a `symbol_files/*.txt`:

```sh
# 1. Regenerate redefs and apply to the ELF
python tools/rename/build_redefs.py
llvm-objcopy \
    --redefine-syms="$RSR_REDEFS_PATH" \
    "$ELF" "$ELF"

# 2. Re-run N64Recomp to refresh the C
"$N64RECOMP_BIN" rogue_squadron.toml

# 3. Build
cmake --build build --config Debug --target RecompiledFuncs -j
```

A clean-state baseline ELF is kept at
`E:/Projects/rogue_squadron64/build/roguesquadron.elf.preBatchContinue` —
copy it back before each `llvm-objcopy` run if you want renames to compose
cleanly across batches.
