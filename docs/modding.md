# Modding

This page is the starting point for making a mod. It covers the kinds of mod the game supports, where mods live, and how to build each kind. Menu buttons and the native host API have their own pages:

- [adding-menus-and-buttons.md](adding-menus-and-buttons.md): adding buttons, toggles, sliders and pages to the menus with a JSON file.
- [modding-host-api.md](modding-host-api.md): the host API for native (DLL / `.so`) mods.

## Kinds of mod

| Kind | What it can do | What you write | Example |
|---|---|---|---|
| Menu mod | Add, hide, reorder or replace menu entries | `roguesq_menu.json` | [mods/quit-prompt/](../mods/quit-prompt/) |
| Asset mod | Replace models, textures, levels and other game files | Files under `assets/` | Exported from the RS64 Explorer |
| Code mod (`.nrm`) | Replace or hook game functions, patch single instructions | MIPS C (or assembly) built with RecompModTool | [mods/infinite-secondary/](../mods/infinite-secondary/) |
| Native mod | Use the host API: hooks, menu text, pad filters, shared flags | A C/C++ shared library | [mods/native-demo/](../mods/native-demo/) |

These can be combined. A code mod or native mod can also ship a `roguesq_menu.json`, and a folder mod can hold both assets and a menu file.

## Where mods live and how they are enabled

Mods go in the `mods/` folder next to the game executable. A mod is either:

- **a folder** with a `mod.json` manifest (menu, asset and native mods), or
- **a `.nrm` file** (code mods; the manifest is packed inside).

A minimal `mod.json`:

```json
{
  "game_id": "rs64",
  "id": "fullscreen-toggle",
  "display_name": "Fullscreen Toggle",
  "description": "Adds a FULLSCREEN on/off toggle to Game Settings.",
  "version": "1.0.0",
  "authors": ["You"],
  "minimum_recomp_version": "0.0.0",
  "enabled_by_default": true
}
```

A folder without a `mod.json` is not loaded.

Players turn mods on and off in the **Mods** window, which opens with **F6** alongside the Controls window (in Debug builds press **F1** first, since the RT64 inspector owns the overlay). The choice is saved in `mods.json` in the game's config folder. The first time a player has a mod, it is on unless its manifest sets `enabled_by_default` to `false` (cheats such as `invincibility` do). After that, `mods.json` decides. Mods are loaded at startup, so changes take effect on the next launch.

A mod can declare config options (`config_schema` in `mod.json`, or `[[manifest.config_options]]` in a code mod's `mod.toml`). They appear in the Mods window, and a menu button can show them too (see `config:` in [adding-menus-and-buttons.md](adding-menus-and-buttons.md#behavior-keys)).

### Shipping a mod with the game

[mods/platforms.json](../mods/platforms.json) lists which mods in this repo ship with each build. The `desktop` list is copied next to the exe (and into `dist/` for Release) by `tools/mods/stage_mods.cmake`. The `android` list is packed into the APK. A mod not listed there ships nowhere. For a code mod, list the built `.nrm` (for example `long_tow_cable.nrm`), not its source folder. Your own mod folders in a build's `mods/` folder are never overwritten.

## Asset mods

An asset mod replaces game files. Put the uncompressed file in the mod folder at `assets/<manifest path>`, where the manifest path is the path the RS64 Explorer shows without the leading `data/`:

```text
mods/my-xwing/
  mod.json
  assets/pl_crafts/xwing_HOB
  assets/frontend/factor/factor_HMT
```

A mod built from game assets should not include game data. It can ship `assets/<manifest path>.bps` instead: a BPS patch that the game applies to the player's own copy of that file at startup.

A mod can also add files the game does not have. A file at a new path (for example `assets/reb_stuff/my_ship_HOB` with its `_HMT`) is added to the game's asset list. To add a copy of an existing file under a new name, without shipping it, put a one-line `assets/<new path>.from` naming the existing file (`imp_stuff/prbdroid_HOB`); a `<new path>.bps` next to it patches the copy.

Cutscene actors can name any model by its asset path, such as `imp_stuff/tankdroid`. The game loads that path and uses the model whose object name is the part after the last `/`, so a model whose object name differs from its file name is added under its object name with `.from` (the RS64 Explorer's Actor menu does this for you, for example `imp_stuff/probe_droid`).

A mod can also play an in-level camera shot during a mission. A `camera_shots.txt` at the mod root links a level's trigger to a camera item, one `<level> <trigger> <camera>` per line (`lv_0 canyon_entry cam_atat_reveal`; `#` starts a comment). The first time the trigger fires in a mission, the game cuts to that camera, follows its next cameras, then hands control back while the fight keeps running. A later line or mod wins for the same trigger. In online co-op both players see the shot whoever set the trigger off; a fourth word `personal` (`lv_0 canyon_entry cam_reveal personal`) shows it only to the player who did, for example when the camera chain ends at a restart point that moves the player. Both players need the mod. In the RS64 Explorer, pick the trigger in the level editor's Events mode and set *Play camera shot*; the cameras are the level's in-mission camera items.

The same trigger can destroy level objects by name, one `<level> <trigger> destroy <object>` per object (`lv_0 canyon_entry destroy tower_1`); these lines add up rather than replace each other. The objects die with their own explosion and count for objectives like any kill, and the trigger's camera shot (if any) starts half a second later so the explosion is in frame. An object not loaded at that moment (far from the player) only loses its health and shows as rubble when it streams in.

The easiest way to make one is the RS64 Explorer (`tools/rs64-explorer`), which writes these patches for you:

1. Make your changes:
   - **Textures:** right-click a textured face and pick *Replace texture from image…*
   - **Object placement:** in a level, right-click a placed object or marker and edit its *Placement* (position, rotation, scale or zone size). Objects can also be duplicated or deleted, except ones the mission script refers to.
   - **Terrain:** tick *Edit terrain* to sculpt and repaint the ground (placed objects follow it), or import a grey heightmap PNG.
   - **Craft models:** on a player craft in the Models tab, pick *Import craft model (.glb)…*. The engines, glows and canopy stay; the model's first texture goes into the craft's largest texture slot.
2. Click *Export mod* and extract the zip into `mods/`.

Rules:

- When two enabled mods replace the same file, the one loaded later wins.
- A replacement still loads into the game's 8 MB heap, so keep it close to the original's size.

### New levels

A mod can add levels. They show in SELECT LEVEL after the last level the player has unlocked, and each plays as a stock *template* level: its mission script (objectives), craft rules and per-level settings, with its own files, intro, extro, title, briefing, approved craft and medals. Its medals are kept apart from the game's in `roguesq_mod_medals.json` (never in the cartridge save, and they stay when the mod is disabled); finishing it changes no stock progress and returns to SELECT LEVEL.

A `levels.json` at the mod root lists them:

```json
{ "levels": [ {
  "id": "dune_sea", "title": "Dune Sea Ambush", "template": 0, "folder": "dune_sea",
  "description": "Walkers are crossing the Dune Sea.\n\nStop them.", "voice": 636,
  "craft": ["xwing", "awing"],
  "medals": { "bronze": { "time": 20, "kills": 10 }, "silver": { "time": 12, "kills": 25 }, "gold": { "time": 8, "kills": 40, "accuracy": 50 } },
  "crawl": { "chapter": "Chapter V", "title": "THE DUNE SEA", "body": "Imperial walkers march on Anchorhead...", "planet": 0 }
} ] }
```

- `template`: a playable stock level, 0-18. Its mission script runs, so keep the objects it uses (start from a copy of its files). Templates 0, 5, 10 and 15 play their chapter crawl first; Beggar's Canyon (16) has no craft select.
- `folder`: the level's files are `assets/level/<folder>/` (the template's file names: `dat`, `hmp`, ...), its cutscenes `assets/cuts/<folder>_intro` and `_extro`. A file the folder lacks is the template's; `.from` and `.bps` work as anywhere in `assets/`.
- `title` and `description` show in capitals; characters the menu fonts lack are left out (letters, digits and `! ( ) _ - / \ , . : '` show).
- `voice`: a stock voice line played as the SELECT LEVEL briefing (none if left out). A recording of your own goes in `assets/level/<folder>/brief` and plays instead: a MORT stream (the game's 8 kHz GSM 06.10 voice codec). The explorer's *Import recording…* makes it from a .wav or .mp3.
- `craft`: approved craft, any of `xwing`, `ywing`, `awing`, `vwing`, `speeder`, `falcon`, `tie`, `t16`, `naboo` (the template's if left out).
- `crawl`: an opening crawl played before the level. Left out, the template's plays (if it has one); `false` plays none; an object gives `chapter` (the small line, may be empty), `title` and `body` (wrapped to the crawl width), and `planet` 0-3, the planet the camera pans down to (the one after the crawl of level 0, 5, 10 or 15). The crawl font has letters in both cases, digits and `! ? , . : ; ' / \ - _ ( )`; anything else is left out.
- `medals`: a medal needs all of time (minutes) at most and kills, accuracy (%) and friendlies saved at least; all three tiers or none (the template's). The bonus threshold (secret pickups) is always the template's. Medals earned on mod levels show on SELECT GAME as a second set of counts (blue digits) beside the game's, per save slot.
- `objectives`: the level's own goals (1-4, shown on the pause screen). With them, none of the template's mission script runs and the level is won when every `destroy`, `reach` and `survive` goal is done:
  - `{ "text": "Destroy the walkers", "goal": "destroy", "counter": 120, "count": 6 }`: objective counter 120 reaches 6. Units add to a counter when destroyed (DAT record +0x80/+0x84).
  - `{ "goal": "reach", "flag": 121 }`: objective flag 121 is set, by an area trigger's on-enter block.
  - `{ "goal": "protect", "flag": 122 }`: the mission fails when flag 122 is set, by the unit's death (DAT record +0x78/+0x7C).
  - `{ "goal": "survive", "seconds": 120 }` / `{ "goal": "timelimit", "seconds": 600 }`: time passes / the mission fails when it runs out.

  The explorer's *Level settings* writes all of this: pick the units or area for each objective and it chooses free counters and flags.

The RS64 Explorer makes all of this: open a stock level and click *New level from this one…* (a copy to change) or *New blank level…* (only a player start on flat ground, blank 3-second intro and outro, and that level's sky, textures and music; give it objectives), edit it like any level, set *Level settings…* (title, description, briefing voice or recording, craft, objectives, medals, opening crawl), then *Export mod*. Camera-shot links (`camera_shots.txt`) for a new level use its folder name in place of `lv_<n>`.

The [exhaust-color](../mods/exhaust-color/) mod is a different approach: it recolors every craft's exhaust from the player's ROM at startup, driven by a config option.

## Code mods (`.nrm`)

A code mod changes game behavior by patching game functions. It is MIPS C compiled with GCC and packed into a `.nrm` by RecompModTool; the game recompiles it at startup with LiveRecomp. It needs no changes to the game itself.

### Getting started

The quickest start is the [mod template](https://github.com/MikeSemicolonD/RogueSquadron64RecompModTemplate), which builds a working example in one step and does not need this repo. The [syms README](../syms/README.md) explains the template's layout and toolchain.

To build a mod inside this repo instead:

1. Install MIPS GCC (`mips64-elf-gcc`, `mips64-elf-ld`).
2. Build RecompModTool once:

   ```sh
   cmake --build build/N64ModernRuntime/librecomp/N64Recomp --config Debug --target RecompModTool
   ```

3. Create `mods/<name>/` with three files. [mods/infinite-secondary/](../mods/infinite-secondary/) is the smallest complete example:
   - `mod.toml`, the RecompModTool manifest. Its `[inputs]` point at `../../syms/rogue_squadron.syms.toml` (function names) and `../../syms/rogue_squadron.datasyms.toml` (data names). The mod id must be a C identifier (`infinite_secondary`).
   - `CMakeLists.txt`, two lines that call `add_code_mod` from [tools/mods/code_mod.cmake](../tools/mods/code_mod.cmake).
   - `src/*.c`, your code. Include `rs64_api.h` from [syms/include/](../syms/include/).
4. Build it:

   ```sh
   cmake -S mods/<name> -B mods/<name>/build
   cmake --build mods/<name>/build
   ```

   The `.nrm` lands in `mods/`. Add it to `mods/platforms.json` to have the game build stage it next to the exe.

The whole of `infinite-secondary`:

```c
#include "rs64_api.h"

RECOMP_PATCH unsigned int getSecondaryWeaponCount(unsigned int craft, unsigned int weapon) {
    return 0xFF;
}
```

### What you can use

The SDK headers in [syms/include/](../syms/include/):

- `rs64_api.h`: `RECOMP_PATCH` (replace a game function), `RECOMP_HOOK` / `RECOMP_HOOK_RETURN` (run code before a function or when it returns), `RECOMP_EXPORT`, and `RECOMP_CALLBACK` for the game events listed in [modding-host-api.md](modding-host-api.md#events-for-nrm-mods).
- The standard recomp modding API: `recomputils.h` (`recomp_alloc`, `recomp_printf`), `recompconfig.h` (`recomp_get_config_u32`, `recomp_get_config_double`, save and mod paths), `recompdata.h` (hashmaps, sets, slotmaps).

Call game functions by their name in `syms/rogue_squadron.syms.toml` (for example `rs_malloc`, `destroyTransientNpcSlots`). A `RECOMP_PATCH` name must match a function there. Reach game data by name from the datasyms file or by absolute address. While the multiplayer mod is on, a few per-player fields (such as `gObjectiveCounts`) are moved to host RAM; read them through `rs64_relocated(&field)`.

After symbols are renamed in the game, `cmake --build build --config Debug --target dump_syms` refreshes the syms files.

More examples:

- [mods/any-craft/](../mods/any-craft/): lets you pick any craft on any level.
- [mods/larger-object-pool/](../mods/larger-object-pool/): calls game functions.
- [mods/long-tow-cable/](../mods/long-tow-cable/): patches in assembly (`src/*.s`) and with instruction patches, for functions too large to rewrite in C.
- [tests/mods/](../tests/mods/): small probe mods used by the tests.

### Functions the game already patches

About 90 game functions carry fixes from the game's own recompiler config (`[[patches.hook]]` or `[[patches.instruction]]` entries in `rogue_squadron.toml`). After a regen they are listed in `RecompiledFuncs/patched_funcs.inl`, one `RECOMP_PATCHED_FUNC(name, kinds)` per function (kinds: 1 = hook, 2 = instruction patch, 4 = a hook that can't be replayed). For one of these functions:

- **`RECOMP_HOOK` / `RECOMP_HOOK_RETURN` work.** The game rebuilds the function from the ROM with your hook and re-applies its own fixes. Your `RECOMP_HOOK` runs first, before the game's entry hook, so it sees the arguments before any base guard acts on them. If a base hook makes the function return early, your `RECOMP_HOOK_RETURN` does not run for that call. Only a kind-4 function refuses hooks ("Hooks a function the base game patches in its recompiler config").
- **`RECOMP_PATCH` is refused** ("Attempted to replace a function that's been patched by the base recomp"), because your replacement would drop the fix (a crash guard, a pacing change or a relocated address).
- **`RECOMP_FORCE_PATCH`** replaces it anyway and logs `Mod <id> force-replaces 0x...`. Your copy must carry every base fix itself; find them by function name in `rogue_squadron.toml`.
- To change a few instructions, use an instruction patch (next section) instead of copying the function.

[tests/mods/replay-probe/](../tests/mods/replay-probe/) hooks `mainGameLoop` and `runInMissionFrame` this way.

### Instruction patches

A mod that changes only a few instructions (a constant, a limit, a load address) can list them in its `mod.toml` instead of copying the function:

```toml
[[patches.instruction]]
func = "loadEffectModelInstancePool"
vram = 0x800AE1B0
expect_asm = "sltiu $v0, $s1, 60"
asm = "sltiu $v0, $s1, 160"
```

`func` is a name from `syms/rogue_squadron.syms.toml`, `vram` the instruction's address, `expect_asm` the original instruction (checked at build and load time) and `asm` its replacement. This one is from `long-tow-cable`.

Use `value` / `expect` (raw instruction words) instead of `asm` / `expect_asm` for branches and jumps. When `build/Debug/rogue_squadron.z64` exists, the build fills in a missing `expect` from the ROM and checks every one. A mod can be patches only: `add_code_mod(<name>)` with no sources and no `elf_path` in `mod.toml`.

RecompModTool writes the patches into the `.nrm` as `instruction_patches.json` (a folder mod can ship that file directly). At load the game:

- refuses a patch whose original word doesn't match the ROM (naming the address),
- refuses two mods writing different words to one instruction,
- rebuilds each patched function with the patches, the game's own fixes and any mod hooks applied, and logs `Mod instruction patches: N in M functions`.

[mods/long-tow-cable/mod.toml](../mods/long-tow-cable/mod.toml) is 21 commented patches that raise the tow cable's 60-segment ring to 160. The multiplayer mod's per-player data relocation is 200 more, generated by `mods/multiplayer-native/tools/gen_relocation.py`.

### Gotchas

- **Linker script.** A code mod links with `syms/include/mod.ld` unless its folder has its own `mod.ld`. RecompModTool finds each hook's target from its output section name (`.recomp_hook.<func>`), so a custom script must not merge hook sections into one output section. If it does, every hook is dropped without an error.
- **`$ra` as data.** LiveRecomp does not set the link register for `bltzal` / `bgezal`, so a `RECOMP_HOOK` on a function that reads `$ra` as a data pointer (some MusyX and voice filters do) will misbehave.

## Native mods

A native mod is a shared library (`.dll` / `.so`) listed under `native_libraries` in its folder's `mod.json`. It can provide menu behavior (`@export` names, below) and take the host API, a table of functions for hooking game code, supplying live menu text, filtering the pad and sharing flags. See [modding-host-api.md](modding-host-api.md), and [mods/native-demo/](../mods/native-demo/) for a complete example. A native library must export `uint32_t recomp_api_version = 1`.

## Menu behavior from your own code (`@export`)

A menu button in your mod's `roguesq_menu.json` can call your own code by writing its export name with a leading `@`:

| Button | Calls |
|---|---|
| `"action": "@doThing"` | `doThing` |
| `"toggle": "@myToggle"` | `myToggle` to flip, `myToggle_get` to read the state |
| `"slider": "@myVolume"` | `myVolume` to set (value in `min..max`), `myVolume_get` to read |

In a `.nrm` mod these are `RECOMP_EXPORT` functions: `void f(void)`, `unsigned int f_get(void)`, `void f(int value)`. In a native library they have the recomp signature `void f(uint8_t* rdram, recomp_context* ctx)`: a get returns its value in `ctx->r2` and a slider set reads its value from `ctx->r4`. They run on a 16 KB guest stack of their own, so MIPS code can call game and runtime functions.

Working examples: [mods/native-demo/](../mods/native-demo/) (native) and [tests/mods/menu-probe/](../tests/mods/menu-probe/) (`.nrm`).

A `.nrm` mod ships its menu file by packing it: add `additional_files = [ "roguesq_menu.json" ]` under `[inputs]` in `mod.toml`.
