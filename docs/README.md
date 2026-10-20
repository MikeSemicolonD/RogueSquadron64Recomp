# Documentation

Pick the section that matches what you want to do.

## I want to build it, run it, or make a simple mod

- [building-all-platforms.md](building-all-platforms.md): building for Windows, Linux, Android and macOS, and what players need installed.
- [modding.md](modding.md): the kinds of mod, where mods go, asset mods, and an introduction to code mods.
- [adding-menus-and-buttons.md](adding-menus-and-buttons.md): adding buttons, toggles, sliders and pages to the menus with a JSON file.
- [multiplayer.md](multiplayer.md): online co-op, from what players see to the session protocol.

## I want to work on the engine

How-to:

- [debugging-with-visual-studio.md](debugging-with-visual-studio.md): stepping through the recompiled code.
- [debug-trace-env-vars.md](debug-trace-env-vars.md): every `ROGUESQ_*` trace, dump and A/B switch.
- [modding-host-api.md](modding-host-api.md): the host API for native mods.
- [release-ci.md](release-ci.md): the release pipeline and its runners.
- [../patches/README.md](../patches/README.md): base-game C patches.

Reference (reverse-engineering notes, dense by design):

- [game-architecture.md](game-architecture.md): start here. Port shape, memory map, threads, frame loop, audio and overlays.
- [data-structures.md](data-structures.md): struct layouts for entities, HUD, cinematics, save data, models and audio.
- [factor5-gbi.md](factor5-gbi.md): Factor 5's custom display-list opcodes and how RT64 handles them.
- [factor5-ucode-dispatch.md](factor5-ucode-dispatch.md): the RSP microcode's dispatch loop and handler map.
- [f5-model-dl-spec.md](f5-model-dl-spec.md): the model display-list grammar.
- [f5-terrain-tile.md](f5-terrain-tile.md): the terrain tile opcode and its LOD seams.
- [musyx-voice-mixing.md](musyx-voice-mixing.md): MusyX per-voice modulation and mixing.
- [adding-menus-and-buttons.md#internals](adding-menus-and-buttons.md#internals): the front-end and pause menu structures.

[AGENTS.md](../AGENTS.md) is the condensed project guide written for AI agents; it is also a useful map of the codebase.
