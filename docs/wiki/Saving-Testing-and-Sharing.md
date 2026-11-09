# Saving, testing and sharing

The buttons are at the bottom of the left panel.

## Undo and redo

Every change is one undo step, including ones in other windows such as Level settings. Ctrl+Z / Ctrl+Y, or the arrow buttons.

## Saving your work as a project

**Save project** (Ctrl+S) writes a `.rs64proj` file with your edits and undo history; **Open project** restores both. It holds only patches against the ROM, so anyone with the same ROM can open it.

## Play in game

**Play in game** (Chrome or Edge only):

1. The first time, pick the game's folder (with `RogueSquadron64Recomp.exe`). It's remembered.
2. Your edits are written there as the mod `rs64-explorer-preview`, replacing the last one.
3. The game starts at the mission or cutscene you have open. Your browser asks to open it the first time.

The first time, a command is copied instead: paste it in a terminal in the game's folder (in Windows: right-click the folder, *Open in Terminal*). After that one run the button starts the game itself, on Windows. Elsewhere, keep using the command. Close the game before testing again. In other browsers, use **Export mod**.

## Exporting a mod

**Export mod** saves a `.zip`. Unzip it into the game's `mods` folder and check it's on in the **Mods** window (**F6**).

If you changed several kinds of thing (levels, models, audio, menus, fonts), it asks how to split them: one mod per kind, your own groups, or one mod. Changes that need each other, such as a level and the custom ship in it, always stay together. The project remembers your groups.

## Sharing your mod

Share the exported `.zip`: it holds only patches, no game data. Never share the ROM or files from it.

In the description, say:

- what it changes
- that it needs the USA v1.0 game
- mods it needs or clashes with

Players install it like an export (above).

## When two mods change the same thing

The one loaded last wins. Keep a replacement near the original's size: the N64's memory is small.
