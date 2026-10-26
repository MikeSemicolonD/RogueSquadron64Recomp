# Documentation

Pick the section that matches what you want to do.

## I want to build it, run it, or make a simple mod

- [building-all-platforms.md](guides/building-all-platforms.md): building for Windows, Linux, Android and macOS, and what players need installed.
- [modding.md](guides/modding.md): the kinds of mod, where mods go, asset mods, and an introduction to code mods.
- [adding-menus-and-buttons.md](guides/adding-menus-and-buttons.md): adding buttons, toggles, sliders and pages to the menus with a JSON file.
- [modding-languages.md](guides/modding-languages.md): adding a language (text, subtitles and glyphs for any script) as a mod.
- [multiplayer.md](guides/multiplayer.md): online co-op, from what players see to the session protocol.
- [relay-server.md](guides/relay-server.md): running the co-op relay server (`rs64_relay`): download, settings, config file and console commands.
- Browser tools, opened from disk or the project's site: the RS64 Explorer ([tools/web/rs64-explorer](../tools/web/rs64-explorer/index.html)) views models, levels, effects, audio and fonts and makes asset mods, new levels, level rules, opening crawls, menu mods and language glyph packs, and in a browser with WebMCP offers AI agents tools that read, search, move and capture the view, play cutscenes, and edit levels (terrain, objects, links, waves, names, mod level settings, rules and opening crawls), cutscenes (actors, cameras, animation keys and level objects made into actors) and menu mods through the page's undo history (with a read-only export plan) ([webmcp.js](../tools/web/rs64-explorer/webmcp.js)) (see [modding.md](guides/modding.md), [adding-menus-and-buttons.md](guides/adding-menus-and-buttons.md) and [modding-languages.md](guides/modding-languages.md)); the [save editor](../tools/web/save-editor/README.md) edits pilots, high scores, settings and unlocks in a save, also through WebMCP tools.

## I want to work on the engine

How-to:

- [debugging-with-visual-studio.md](engine/debugging-with-visual-studio.md): stepping through the recompiled code.
- [debug-trace-env-vars.md](engine/debug-trace-env-vars.md): every `ROGUESQ_*` trace, dump and A/B switch.
- [modding-host-api.md](engine/modding-host-api.md): the host API for native mods.
- [release-ci.md](engine/release-ci.md): the release pipeline and its runners.
- [../patches/README.md](../patches/README.md): base-game C patches.

Reference (reverse-engineering notes, dense by design). Read [reference/README.md](reference/README.md) first: it covers what to read in what order, the vocabulary, and which doc answers which question.

- [game-architecture.md](reference/game-architecture.md): start here. Port shape, memory map, threads, frame loop, audio and overlays.
- [data-structures.md](reference/data-structures.md): struct layouts for entities, HUD, cinematics, save data, models and audio.
- [factor5-gbi.md](reference/factor5-gbi.md): Factor 5's custom display-list opcodes and how RT64 handles them.
- [factor5-ucode-dispatch.md](reference/factor5-ucode-dispatch.md): the RSP microcode's dispatch loop and handler map.
- [f5-model-dl-spec.md](reference/f5-model-dl-spec.md): the model display-list grammar.
- [f5-terrain-tile.md](reference/f5-terrain-tile.md): the terrain tile opcode and its LOD seams.
- [musyx-voice-mixing.md](reference/musyx-voice-mixing.md): MusyX per-voice modulation and mixing.
- [adding-menus-and-buttons.md#internals](guides/adding-menus-and-buttons.md#internals): the front-end and pause menu structures.

[AGENTS.md](../AGENTS.md) is the condensed project guide written for AI agents; it is also a useful map of the codebase.
