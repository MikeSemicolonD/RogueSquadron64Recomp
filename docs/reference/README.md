# Reference notes

These are reverse-engineering notes: addresses, struct offsets and opcode tables recovered from the game. They are dense because they are lookups, not tutorials. You don't read them front to back. Read the overview once, then open the others when a question sends you there.

## Before you open them

- **Read [game-architecture.md](game-architecture.md) through "Memory map" first.** Its "At a glance" and "How the recompilation works" sections are enough to follow everything else. Stop there and skip the numbered subsystems until you need one.
- **Addresses are N64 virtual addresses** (`0x80xxxxxx`). Code at `0x800A5130` and up belongs to whichever overlay is loaded: mission, menu or cinematic.
- **Names come from the symbol files** in [syms/](../../syms/). An unnamed global is `D_<addr>` and an unnamed function is `func_<addr>`. A function name in these notes is also the name in `RecompiledFuncs/funcs_*.c`, so grep for it there.
- **`funcs_N.c:line` citations go stale** whenever the code is regenerated. Search for the function name, not the line number.
- **Some claims carry confidence tags**, such as (confirmed), (doc), (guess), READ or UNRESOLVED. Each doc explains its tags at the top. Treat (guess) as a lead to check, not a fact.
- [AGENTS.md](../../AGENTS.md) is the shortest summary of the whole port, including the dead ends that have already been ruled out.

## Vocabulary

| Term | Meaning |
|---|---|
| RDRAM | The N64's 8 MB of main memory. The game's data structures live here. |
| RSP / RDP | The N64's vector coprocessor, which runs microcode, and its rasterizer. |
| ucode | Microcode, the RSP program. This game has custom graphics ucode (F3DFACTOR5) and audio ucode (MusyX). |
| DL / GBI | Display list: the command stream the CPU builds for the graphics ucode. GBI is the command set. |
| HLE | The port does not run the graphics ucode. RT64 interprets the display list directly. |
| Overlay | A code segment swapped into the same address range at runtime. |
| DAT / HOB / HMP | Level object data, model files and terrain heightmaps (see game-architecture "Asset formats"). |
| Golden | A RDRAM or display-list capture from Project64, used to diff against the port's output. |

## Which doc answers what

| Question | Doc |
|---|---|
| How does the port fit together? Which thread does what? Where is global X? | [game-architecture.md](game-architecture.md) |
| What are the fields of the player, NPC, HUD, save or cutscene struct? | [data-structures.md](data-structures.md) |
| What does display-list opcode `0xNN` do, and how does RT64 handle it? | [factor5-gbi.md](factor5-gbi.md) (summary table), then [f5-model-dl-spec.md](f5-model-dl-spec.md) §7 (authoritative) |
| How does the game turn a model into a display list? | [f5-model-dl-spec.md](f5-model-dl-spec.md) §1-6 |
| Why do the terrain LOD seams crack, or not? | [f5-terrain-tile.md](f5-terrain-tile.md) |
| Where is a handler in the RSP microcode? | [factor5-ucode-dispatch.md](factor5-ucode-dispatch.md) |
| How are a sound's pitch, pan and volume computed? | [musyx-voice-mixing.md](musyx-voice-mixing.md) |
| How are the front-end and pause menus laid out in memory? | [adding-menus-and-buttons.md#internals](../guides/adding-menus-and-buttons.md#internals) |
