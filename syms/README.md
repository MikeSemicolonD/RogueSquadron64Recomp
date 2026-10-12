# RogueSquadron64RecompSyms

Symbol files for [Rogue Squadron 64 Recompiled](https://github.com/MikeSemicolonD/RogueSquadron64Recomp) (*Star Wars: Rogue Squadron*, N64, USA v1.0). Code mods (`.nrm`) are built against them, so you can write a mod without cloning or building the game.

| File | Contents |
|---|---|
| `rogue_squadron.syms.toml` | Every game function, by section (main code and the three overlays): name, address, size. |
| `rogue_squadron.datasyms.toml` | Data symbols at their original addresses. |
| `rogue_squadron.datasyms.mods.toml` | The data symbols mods link against: the same list, with the co-op per-player fields moved to where the recomp keeps them. |

The files hold only names, addresses and sizes, no game code.

## Making a code mod

A code mod is MIPS C that RecompModTool packs into a `.nrm`. The game loads it at startup and recompiles it with LiveRecomp. You need:

- **MIPS GCC** (`mips64-elf-gcc` and `mips64-elf-ld`). Windows builds are at [n64-tools/gcc-toolchain-mips64](https://github.com/n64-tools/gcc-toolchain-mips64/releases).
- **RecompModTool**, built from [N64ModernRuntime](https://github.com/MikeSemicolonD/N64ModernRuntime) (`N64Recomp/`, target `RecompModTool`).
- **`rs64_api.h`**, the macros and base-game imports, from the game repository's [mods/sdk/](https://github.com/MikeSemicolonD/RogueSquadron64Recomp/tree/main/mods/sdk).

Add this repository to your mod as a submodule:

```
git submodule add https://github.com/MikeSemicolonD/RogueSquadron64RecompSyms.git syms
```

### Layout

```
my-mod/
  mod.toml       RecompModTool manifest
  mod.ld         linker script (copy one from the game's mods/*/mod.ld)
  rs64_api.h
  src/my_mod.c
  syms/          this repository
```

`mod.toml`:

```toml
[manifest]
id = "my_mod"                 # a C identifier
version = "1.0.0"
display_name = "My Mod"
description = "What it does."
short_description = "What it does."
authors = ["you"]
game_id = "rs64"
minimum_recomp_version = "0.0.0"
enabled_by_default = true

[inputs]
elf_path = "build/mod.elf"
mod_filename = "my_mod"
func_reference_syms_file = "syms/rogue_squadron.syms.toml"
data_reference_syms_files = [ "syms/rogue_squadron.datasyms.mods.toml" ]
```

### Writing the code

Refer to game functions and data by their names in these files. A function still called `func_XXXXXXXX` is one nobody has named yet; use that name.

```c
#include "rs64_api.h"

// Replace a game function.
RECOMP_PATCH unsigned int getSecondaryWeaponCount(unsigned int craft, unsigned int weapon) {
    return 0xFF;
}

// Run before a game function (RECOMP_HOOK_RETURN runs after it returns).
RECOMP_HOOK("runInMissionFrame") void on_mission_frame(void) {
}

// Subscribe to one of the game's hook points; the full list is in the game's docs/modding-host-api.md.
RECOMP_CALLBACK("*", rs64_on_mission_frame_pads) void on_pads(void) {
}
```

`rs64_api.h` also imports the base game's helpers: `rs64_draw_dist`, `rs64_terrain_dist`, `rs64_tgrid_base` (the relocated terrain-grid tables) and `rs64_relocated` (where a co-op per-player field moved). Use `rs64_event_return(v)` from a callback at a function-entry hook point to make the game function return `v`.

### Building

```
mips64-elf-gcc -mips2 -mabi=32 -O2 -G0 -mno-abicalls -fomit-frame-pointer -fno-builtin -fno-optimize-sibling-calls -nostdinc -I . -c src/my_mod.c -o build/my_mod.o
mips64-elf-ld -nostdlib -T mod.ld --unresolved-symbols=ignore-all --emit-relocs --noinhibit-exec -e 0 build/my_mod.o -o build/mod.elf
RecompModTool mod.toml build
```

ld reports every call into the game as out of range; that's expected, because RecompModTool resolves those calls by name. Keep `-fno-optimize-sibling-calls`: GCC otherwise turns a final call into a tail jump that LiveRecomp can't recompile. Copy `build/my_mod.nrm` into the game's `mods/` folder and enable it in the Mods panel (F1) or `mods.json`.

The game repository's [tools/mods/code_mod.cmake](https://github.com/MikeSemicolonD/RogueSquadron64Recomp/blob/main/tools/mods/code_mod.cmake) wraps these steps (`add_code_mod`). Its `mods/infinite-secondary`, `mods/any-craft` and `mods/larger-object-pool` folders are complete examples.

### Functions the base game patches

The recomp fixes about 190 game functions with hooks and instruction patches of its own (crash guards, pacing, the co-op data relocation).

- **`RECOMP_HOOK` on one works.** The game rebuilds the function with your hook and re-applies its own patches.
- **`RECOMP_PATCH` on one is refused** with "Attempted to replace a function that's been patched by the base recomp", because your copy would drop the fix.
- **`RECOMP_FORCE_PATCH`** replaces it anyway. Your copy must then carry the base patches itself, as the game's `mods/long-tow-cable` does.

## Updating

These files follow the game's function names, which change as the decomp names more functions. Before releasing a mod, update the submodule (`git submodule update --remote syms`) and rebuild. A renamed function shows up as a RecompModTool error, not as a broken mod.

Maintainers regenerate the files from the game repository with `cmake --build build --config Debug --target dump_syms`, commit them here, then bump the game's `syms` submodule.
