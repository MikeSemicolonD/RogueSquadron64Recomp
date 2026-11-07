# Saving, testing and sharing

Every guide ends the same way: save your work, try it in the game, then export it as a mod. The buttons for all of this sit at the bottom of the left panel.

## Undo and redo

Every change is one undo step, including changes made in other windows such as Level settings. Use Ctrl+Z and Ctrl+Y, or the arrow buttons.

## Saving your work as a project

**Save project** (Ctrl+S) writes a `.rs64proj` file with every edit and its full undo history. **Open project** brings it back later, with undo still working.

A project holds only your changes, stored as patches against the ROM, so it contains no game data. You can send it to someone else who has the same ROM.

## Play in game

**Play in game** puts your edits into the game so you can try them right away:

1. The first time, it asks for the game's folder (the one with `RogueSquadron64Recomp.exe` in it). It remembers the folder after that.
2. It writes your edits into that folder as a mod called `rs64-explorer-preview`, replacing the previous preview.
3. It copies a command to your clipboard that starts the game straight at what you have open: the mission you're editing, or the cutscene you're watching.

Open a terminal in the game's folder, paste the command and press Enter. In Windows, right-click the folder in File Explorer, pick *Open in Terminal*, then paste.

This needs Chrome or Edge. In other browsers, use **Export mod** and install it by hand.

## Exporting a mod

**Export mod** saves your edits as a `.zip`. Unzip it into the `mods` folder next to the game, start the game, and check the mod is on in the **Mods** window (press **F6**).

If you changed more than one kind of thing (levels, models, audio, menus, fonts), the export asks how to split them: one mod per kind, groups you name yourself, or everything in one mod. Some changes can't work without each other, such as a level and the custom ship placed in it. Those always export together, and the dialog tells you so. The project remembers your groups.

The exported mod holds no game data, only patches that the game applies to the player's own copy of the game, so it's safe to share.

## Sharing your mod

Upload the `.zip` that **Export mod** made. It holds patches, not game data, so it's safe to share. Never share the ROM or any file taken out of it.

In the description, say:

- what the mod changes: which levels, ships, cutscenes, menus or sounds
- that it's made for the USA v1.0 version of the game, because its patches only fit that version
- any other mods it needs, and any it doesn't work with because they change the same thing

Players install it the same way you test an export: unzip it into the `mods` folder next to the game, start the game, and turn the mod on in the **Mods** window (**F6**).

## When two mods change the same thing

If two mods replace the same file, the one loaded last wins. Keep a replacement close to the original's size: the game still loads it into the N64's limited memory.
