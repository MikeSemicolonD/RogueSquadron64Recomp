# Patches

Base-game C patches, built the Zelda64Recomp way: every build compiles `src/**/*.c` to MIPS with the code-mod toolchain, links `patches.elf` with [patches.ld](patches.ld), recompiles it with N64Recomp ([../patches.toml](../patches.toml)) into `RecompiledPatches/`, embeds `patches.bin`, and registers it with librecomp at boot ([src/game/register_patches.cpp](../src/game/register_patches.cpp)). The build needs MIPS GCC: set `MIPS_TOOLCHAIN_DIR` (default `E:/mips-toolchain`) or `MIPS_GCC` and `MIPS_LD`.

Patch sources include `rs64_api.h` ([syms/include/](../syms/include/rs64_api.h)), the same header code mods use:

- `RECOMP_EXPORT` makes a function a base export that mods import with `RECOMP_IMPORT("*", ...)`. [src/sanity.c](src/sanity.c) exports `rs64_patches_sanity` as a pipeline check.
- `RECOMP_DECLARE_EVENT` declares a base event; mods subscribe with `RECOMP_CALLBACK("*", ...)`.
- `RECOMP_PATCH` replaces a game function by name: [src/npc_health.c](src/npc_health.c) (the five NPC health accessors, guarding the per-difficulty slot) [src/factor5_mutex.c](src/factor5_mutex.c) (`initFactor5Mutex` runs once) and [src/profiler_hud.c](src/profiler_hud.c) (the F5 profiler HUD's bars keep one size across framebuffer resolutions). A patched name that doesn't exist fails the build (`strict_patch_mode`). Mods can `RECOMP_HOOK` these functions: the hook is rebuilt from the patch, so the fix stays.
- Game functions and data are reached by name (`syms/`) or absolute address. Calls to libultra functions go to ultramodern's `_recomp` versions, as in the game's own code. Patch RAM runs from 0x80801000 to 0x80A00000 (`patches.ld`), below the host-only tables.

## Where most base-game fixes live today

Base-game fixes live in [rogue_squadron.toml](../rogue_squadron.toml):

- `[[patches.hook]]` inserts a call at a function's entry (or at `before_vram`). It is compiled into the recompiled function, so it runs for direct and indirect (`LOOKUP_FUNC`) calls alike. The text is only `{ if (rs64_th_<func>_<vram|entry>(rdram, ctx)) return; }` or `{ rs64_th_<func>_<vram|entry>(rdram, ctx); }`; the body is that function in [src/game/toml_hooks.c](../src/game/toml_hooks.c), declared in `include/rs64_toml_hooks.h`. `return 1` exits the game function; set `ctx->r2` (or `ctx->f0`) first to return a value.
- `[[patches.instruction]]` replaces a single instruction word.

Larger host logic goes in an `extern "C"` helper in [src/game/hook_helpers.cpp](../src/game/hook_helpers.cpp) that the hook body calls.

After editing the toml, regenerate and build:

```
cmake --build build --config Debug --target regen_funcs
cmake --build build --config Debug --target RogueSquadron64Recomp
```

## Why most fixes aren't patches yet

A patch function replaces a whole function with C. Most of our fixes don't fit that:

- **The decomp is mostly assembly.** Only a few of the patched functions have decompiled C to copy and edit, and the rest would have to be decompiled first.
- **Most hooks are mid-function** (`before_vram`). A patches ELF can only replace a whole function.
- **Layout changes are instruction-level.** The co-op relocation (200 `lui` rewrites) and the 128x128 terrain-grid strides move data. Restating them as C would mean rewriting about 110 functions.

Whole-function fixes will move here once more of the decomp is C, and everything else once the decomp is complete.

## What this means for mods

librecomp knows which functions the toml patches: N64Recomp writes `RecompiledFuncs/patched_funcs.inl`, and the game registers that list at boot. A `.nrm` mod that `RECOMP_PATCH`es one of these functions is refused with an error, because loading it would silently drop the base fix; `RECOMP_FORCE_PATCH` is the deliberate override. A `RECOMP_HOOK` loads: N64Recomp also writes the hooks and instruction patches as tables in `recomp_overlays.inl`, and when a hook makes LiveRecomp rebuild the function from the ROM, librecomp re-applies the instruction patches and LiveRecomp calls the `rs64_th_` hook functions at the same places. See [Functions the game already patches](../docs/guides/modding.md#functions-the-game-already-patches).

Gameplay changes that should stay optional go in mods instead: `.nrm` code mods (MIPS-side C, see `mods/infinite-secondary/`) or native mods through the host API ([docs/engine/modding-host-api.md](../docs/engine/modding-host-api.md)).
