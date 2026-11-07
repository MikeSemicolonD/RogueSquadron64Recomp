# Getting started

## What you need

- **Your own copy of the game**, as a ROM file: *Star Wars: Rogue Squadron* (USA, version 1.0, `.z64`). The explorer contains nothing from the game, so it reads everything from your ROM. Your ROM never leaves your computer.
- **Chrome or Edge** on a desktop computer. Other browsers can view and export, but only Chrome and Edge can write straight into the game's folder for [Play in game](Saving-Testing-and-Sharing#play-in-game).
- **The game itself** ([Rogue Squadron 64 Recompiled](https://github.com/MikeSemicolonD/RogueSquadron64Recomp/releases)), to play what you make.

Open the [RS64 Explorer](https://mikesemicolond.github.io/RogueSquadron64Recomp/tools/web/rs64-explorer/) and drop your ROM on the page.

## Moving around the view

![The RS64 Explorer with the X-wing open in the Models tab](images/overview.png)

The help box in the corner of the view always lists the controls for what you are doing. The common ones:

| Do this | To |
| --- | --- |
| Left- or right-drag | Orbit around the view |
| Middle-drag | Pan |
| Mouse wheel | Zoom |
| W A S D, Q E | Fly (hold Shift to go faster). While editing, hold the right mouse button as you fly |
| R | Reset the camera (when not editing) |
| Right-click | Inspect what's under the cursor: a part, a texture, a ground tile. While editing, pick from everything under the cursor |

The cube of axes in the corner turns the view to face along an axis. Its camera button switches to a flat (orthographic) view for precise placement; numpad 5 does the same.

## Keyboard shortcuts

| Keys | Action |
| --- | --- |
| Ctrl+Z / Ctrl+Y (or Ctrl+Shift+Z) | Undo / redo |
| Ctrl+S | Save project |
| Q / W / E / R | While editing: select, move, rotate, scale |
| X | While editing: switch between world and local axes |
| Tab / Shift+Tab | Next / previous edit mode (terrain, objects, events) |
| Ctrl+D | Duplicate the selected item |
| Delete | Delete the selected item |
| V | In the cutscene editor: snap the picked camera or actor to the view |
| Esc | Cancel placing or dragging something |

## Looking before you change

Everything you can change, you can view first. These tabs are useful for finding what you want to mod, even though most mods are made from the Models, Levels and Menus tabs:

- **Effects**: explosions, fire, smoke and other animated sprites, played the way the game plays them.
- **Audio**: sound effects, voice lines (listed by what they say) and music, with a timeline of each song's instrument layers.
- **Fonts**: the game's fonts. It also builds the letters for a translation; see the repo's [language modding guide](https://github.com/MikeSemicolonD/RogueSquadron64Recomp/blob/main/docs/guides/modding-languages.md).

## Troubleshooting

**The ROM is rejected.** The reason shows under the drop box. The explorer needs Rogue Squadron for the N64, the USA v1.0 version. Any of the usual file types works (`.z64`, `.v64` or `.n64`); it puts the bytes in order itself. *Not a Rogue Squadron N64 ROM* usually means a zipped ROM, so unzip it first, or a file that isn't an N64 ROM.

**The view stays blank.** The explorer draws with your graphics card (WebGPU, or WebGL 2 where the browser has no WebGPU). Use a recent Chrome, Edge or Firefox with hardware acceleration on: in Chrome it's *Settings > System > Use graphics acceleration when available*, and `chrome://gpu` shows whether it's working.

**My edits are gone.** Edits live in the page until you save them. **Save project** (Ctrl+S) writes a `.rs64proj` file with every edit and its undo history, and **Open project** brings them back. Closing or reloading the page warns you when there are unsaved edits. A project opens only with the ROM it was made with. An exported mod can't be opened back as a project (only its menu and language files can be imported), so keep the project file as well.
