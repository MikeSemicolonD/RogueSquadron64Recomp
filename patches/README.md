# Patches build

Hand-written C in `patches/*.c` is cross-compiled to MIPS, run back through
N64Recomp, and linked alongside the auto-generated
`RecompiledFuncs/funcs_*.c`. Symbols defined here **override** same-named
auto-generated functions, so the auto-generated source can be regenerated
clean from a fresh N64Recomp run without losing our work.

The pattern follows
[Zelda64Recompiled/patches](https://github.com/Zelda64Recomp/Zelda64Recomp/tree/dev/patches),
**but uses `mips64-elf-gcc` instead of clang** because the official LLVM
Windows installer no longer ships the MIPS backend (verified on LLVM 19,
20, and 22 — they include only x86/aarch64/arm/riscv/wasm). See [Toolchain
setup](#toolchain-setup) for the install we use.

## How the pipeline works

```
patches/heap_guards.c           ← you edit this (MIPS-side C)
    │
    │ mips64-elf-gcc -mips2 -mabi=32   (cross-compile to MIPS)
    ▼
patches/heap_guards.o
    │
    │ mips64-elf-ld -T patches.ld -T syms.ld
    ▼
patches/patches.elf             ← real MIPS executable, in extra RAM at 0x80801000+
    │
    │ N64Recomp.exe ../patches.toml
    ▼
RecompiledPatches/patches.c     ← auto-generated host C (recomp_func_t signature)
    │
    │ Compiled by clang-cl into PatchesLib.lib
    ▼
RogueSquadron64Recomp.exe       ← linked PatchesLib FIRST, before RecompiledFuncs
```

The linker (`lld-link` via ClangCL) processes static libraries in
command-line order and resolves each symbol at its first definition.
`PatchesLib` comes before `RecompiledFuncs` in our CMake target_link_libraries
list, so any symbol our patch defines wins over the same-named symbol in
the auto-generated output. We pass `/FORCE:MULTIPLE` to silence the
"duplicate symbol" error — this *is* a deliberate duplicate and the link
order picks the right one.

## Adding a patch

1. **Write C in `patches/*.c`.** Name the function exactly the same as the
   game function you're overriding. Use the `RECOMP_PATCH` attribute.

   ```c
   #define RECOMP_PATCH __attribute__((section(".recomp_patch")))

   // Override of game function func_80007D74 (heap free-list dequeue).
   RECOMP_PATCH void* func_80007D74(void) {
       // ... your replacement implementation ...
   }
   ```

   The function signature is what the **original game** uses (no register
   args usually, returns a pointer), not the recomp-host signature. After
   N64Recomp processes `patches.elf`, the host-side signature
   `void(uint8_t* rdram, recomp_context* ctx)` is generated automatically.

2. **Declare any game functions/data you reference** in
   [patches/syms.ld](syms.ld). Look up the address in
   `../syms/rogue_squadron.syms.toml`:

   ```ld
   func_8002221C       = 0x8002221C;   /* allocator helper */
   heap_free_head      = 0x801163B0;   /* (Block*) — heap free-list head */
   ```

   Then declare them as `extern` in your patch C:

   ```c
   extern Block* func_8002221C(void);
   extern Block* heap_free_head;   // 0x801163B0
   ```

   The linker resolves the symbol against the address; the C declaration
   gives it a type for type-checking.

3. **Build.** From the project root:
   `cmake --build build --config Debug --target PatchesLib` to rebuild
   only the patches lib, or the regular full build to relink everything.

4. **Verify the override fires.** Easiest way: run the game and look for
   different behavior. For a stronger test, temporarily strip the inline
   patch from the auto-generated source so only your `patches/` version
   defines the symbol; if the game still works correctly, the override is
   real.

### What you can't do

- **`#include <stddef.h>` / `<stdint.h>` / any libc header** — the
  Makefile uses `-nostdinc` (matching Zelda's pattern). MIPS gcc's
  freestanding headers aren't on the include path. Inline the typedefs
  you need:

  ```c
  typedef unsigned char uint8_t;
  typedef unsigned int  uint32_t;
  typedef unsigned long uintptr_t;   // mips32 ABI: long is 32-bit
  #define NULL ((void*)0)
  ```

- **`fprintf(stderr, ...)`** — there's no libc on the MIPS side. To call
  host code from a patch, declare a runtime stub at a fake `0x8FXXXXXX`
  address in `syms.ld` and provide the host implementation in `src/main/`.
  We haven't wired any host stubs yet — add as needed.

- **Static globals** — they go in `.bss` at addresses chosen by the linker
  in patch ELF extra RAM (0x80801000+). That's fine for patch-private state
  but be aware they don't share storage with same-named game globals.

## Toolchain setup

### MIPS cross-compiler (Windows)

LLVM's official Windows installer doesn't include the MIPS backend (any
version 19+). Use the **n64-tools** prebuilt mips64-elf-gcc 12.2.0
instead:

- Download: https://github.com/n64-tools/gcc-toolchain-mips64/releases
  (latest: `gcc-toolchain-mips64-win64.zip`)
- Extract to a stable path. The Makefile defaults to `E:/mips-toolchain`;
  override on the cmake command line if installed elsewhere:
  `cmake -DMIPS_TOOLCHAIN_DIR=C:/mips-toolchain ...`
- Verify: `& "E:/mips-toolchain/bin/mips64-elf-gcc.exe" --version` should
  print GCC 12.2.0.

End-state of `bin/`:

```
mips64-elf-gcc.exe
mips64-elf-ld.exe
mips64-elf-as.exe
mips64-elf-objcopy.exe
... etc
```

### `make` (Windows)

The Makefile uses GNU make syntax. CMake invokes whichever `make` it
finds on PATH; on most Windows dev machines that's
`mingw32-make.exe` from the MinGW-w64 install bundled with various IDEs.
If CMake reports "Patches build skipped: mingw32-make / make not on
PATH", install MinGW or pass `-DPATCHES_MAKE_EXE=...` to cmake.

### N64Recomp executable

The patches build invokes the **newer** N64Recomp built from the
submodule (`lib/N64ModernRuntime/N64Recomp/`). CMake builds it as the
`N64RecompCLI` target and points the patches custom command at
`build/Debug/N64Recomp.exe`. No separate install needed.

## Differences from Zelda64Recompiled's patches build

Zelda's
[patches/Makefile](https://github.com/Zelda64Recomp/Zelda64Recomp/blob/dev/patches/Makefile)
uses **clang** + `-target mips`. Ours uses **mips64-elf-gcc** because of
the LLVM-Windows-no-MIPS-backend issue. The flag differences:

| Zelda (clang)                             | Ours (mips64-elf-gcc)            |
|---|---|
| `-target mips`                            | (not needed — gcc is already mips) |
| `-mno-odd-spreg`                          | (clang-only — drop)              |
| `-mno-check-zero-division`                | (clang-only — drop)              |
| `-Wno-incompatible-library-redeclaration` | (clang-only — drop)              |
| `-Wno-unsupported-floating-point-opt`     | (clang-only — drop)              |
| `-mips2 -mabi=32 -G0 -mno-abicalls`       | same (gcc accepts these)         |
| `-O2 -fomit-frame-pointer -ffast-math`    | same                             |

Both use `ld.lld` / `mips64-elf-ld` with the same linker scripts.

Other differences:

- Zelda uses `strict_patch_mode = true` with a curated `func_reference_syms_file`. We do too, but ours is auto-generated by `N64Recomp --dump-context` (committed to `syms/`) rather than hand-curated by the decomp team.
- Zelda has `register_patches.cpp` that calls `recomp::overlays::register_patches(patches_bin, ...)` to register the patches at runtime, alongside generating a `patches_bin.c` from `patches.bin`. We don't (yet) need this — the static-library link is enough for our PoC. Add this layer later if we want runtime-registered mod-style patches.
- Zelda's decomp ships full game-side struct headers (`z64.h`, etc). Our decomp at `E:/Projects/rogue_squadron64/include/` is sparse — just `include_asm.h`, `labels.inc`, `macro.inc`. Patches can't symbolically dereference game struct fields; you'll usually be writing pointer arithmetic + `extern` declarations of individual data symbols.

## File map

| File | What it does |
|---|---|
| `Makefile` | Cross-compile `*.c` → MIPS ELF |
| `patches.ld` | Memory layout for the patch ELF (extra RAM at 0x80801000+) |
| `syms.ld` | Game-side symbols (functions + data) referenced from patches |
| `dummy_headers/` | Stand-in headers when the decomp doesn't provide a real one |
| `*.c` | Patch source files. `heap_guards.c` is the first migration. |
| `../patches.toml` | N64Recomp config for the patch ELF |
| `../syms/rogue_squadron.syms.toml` | Auto-generated game function symbols (validates `strict_patch_mode`) |
| `../RecompiledPatches/patches.c` | Generated host C — gets compiled into PatchesLib.lib |
