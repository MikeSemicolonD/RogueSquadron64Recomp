# Make a menu mod

A menu mod adds, hides, moves or renames buttons in the game's menus and the pause menu, and can add whole new pages.

## Open a menu

Open the **Menus** tab and pick a menu. It shows a preview of the menu as it looks in the game.

![The main menu in the Menus tab](images/menu-editor.png)

## Change it

- **Click** a button to hide it, replace it, or add a new button above or below it.
- **Drag** buttons and titles to move them.
- A new button can be a plain button, an on/off toggle, a list of choices or a slider, or it can open a new page of buttons.
- **Language** sets which language the preview and the text boxes show. Each button's text can be written once for every language, or separately per language.

## What a button can do

A button's **Type** decides what it is, and for most types you then pick what it runs or changes from a list:

| Type | What it is | Built-in choices |
| --- | --- | --- |
| `action` | Does something when pressed, optionally after a yes/no question you write | Quit the game (desktop only), reset the controls, open the mods folder, the touch layout editor (Android), or nothing (an info line) |
| `toggle` | An on/off switch for a setting | Fullscreen, widescreen, frame interpolation, the ray-traced lights, shadows, soft shadows and fog shafts, rumble, gyro aiming (Android), cutscene draw distance, frame writeback |
| `cycle` | Steps through a list of choices each time it's pressed | Draw distance, resolution, anti-aliasing, target frame rate, texture filter, upscale filter |
| `slider` | A bar the player drags or steps | Draw distance, mouse sensitivity, mouse smoothing, dead zone |
| `submenu` | Opens a page | One of your own new pages (up to 7 buttons, BACK is added for you) or one of the game's menus |
| `label` | A line of text the cursor skips | |

Settings change straight away and are saved with the game's own settings.

**Shows only if** hides a button unless something holds: the game runs on desktop, the page was opened from the front end rather than the pause menu, the graphics card can do ray tracing, the Expansion Pak is in use, or frame interpolation is on. It can also follow one of your mod's options.

**Custom…** in the choice list takes a key typed in: `config:<option id>` shows one of your mod's config options (a toggle steps through its values, a slider sets a number), and `@name` calls a function that your own code mod exports.

## Edit an existing menu mod

**Import** opens a menu mod that's already made, such as one from the game's `mods` folder, so you can change it.

## Splitting into several mods

Each new button belongs to one exported mod. The panel sets which one, and Export mod can move buttons between mods later.

## Finish

[Try it in the game and export it](Saving-Testing-and-Sharing).

## Not yet possible

Without a code mod, a button can't:

- **Do anything beyond the built-in choices**, such as starting a level or changing the game while you play. That needs a function in a code mod (`@name`).
- **Change a setting that isn't listed.** A mod's own config options can be shown and changed, but they only do something if code reads them.
- **Show text that updates by itself**, like the co-op lobby's status lines. Those come from native mods.

Some limits hold for every menu mod:

- The game's menus fit 8 lines, and **Options** and **Game Settings** are already full: a new button there replaces or hides one of the game's.
- A new page holds 7 buttons plus BACK. The pause menus fit about 16 lines.
- The pause menu's titles can't be changed.
- The menu font has capital letters, numbers and some punctuation only; lower case and other symbols draw as blanks.
