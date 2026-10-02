# Patches

Other recomps (Zelda64Recomp and similar) keep base-game overrides here as MIPS-side C that is cross-compiled and run back through N64Recomp. This project does not: there is no MIPS patches build, and this folder holds no sources.

Base-game fixes live in [rogue_squadron.toml](../rogue_squadron.toml):

- `[[patches.hook]]` inserts C at a function's entry (or at `before_vram`). It is compiled into the recompiled function, so it runs for direct and indirect (`LOOKUP_FUNC`) calls alike. A bare `return;` exits the game function; set `ctx->r2` (or `ctx->f0`) first to return a value.
- `[[patches.instruction]]` replaces a single instruction word.

Keep hook text short and put the logic in an `extern "C"` helper in [src/main/hook_helpers.cpp](../src/main/hook_helpers.cpp). Example: the NPC health accessors (`getNpcCurrentHealth` and four siblings) call `rs64_npc_health_slot_ok` on entry and only take over when the per-difficulty slot address is bad.

After editing the toml, regenerate and build:

```
cmake --build build --config Debug --target regen_funcs
cmake --build build --config Debug --target RogueSquadron64Recomp
```

Gameplay changes that should stay optional go in mods instead: `.nrm` code mods (MIPS-side C, see `mods/infinite-secondary/`) or native mods through the host API ([docs/modding-host-api.md](../docs/modding-host-api.md)).
