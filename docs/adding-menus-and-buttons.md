# Adding menus and buttons

A mod can add, hide, reorder and replace entries in the front-end menus (title screen and options) and the in-mission pause menu by shipping a `roguesq_menu.json`. No code and no rebuild are needed: edit the JSON and relaunch.

The first half of this page is the guide for mod authors. The second half, [Internals](#internals), documents the game's menu structures and how the button system drives them, for when you need to extend the system itself. For mod packaging in general (folders, `mod.json`, code mods), see [modding.md](modding.md).

## Quick start

1. Make a folder in `mods/` with a `mod.json` (see [modding.md](modding.md#where-mods-live-and-how-they-are-enabled)). Without it the folder is not loaded.
2. Add a `roguesq_menu.json` next to it:

   ```json
   {
     "buttons": [
       { "id": "fs_menu", "menu": "game_settings", "type": "toggle",
         "toggle": "fullscreen", "label_on": "FULLSCREEN: ON", "label_off": "FULLSCREEN: OFF",
         "after": "high_resolution" }
     ]
   }
   ```

3. Launch the game and enable the mod in the Mods window (**F6**).

That is the whole of [mods/fullscreen-toggle/](../mods/fullscreen-toggle/). Other working examples:

- [mods/quit-prompt/](../mods/quit-prompt/): adds a YES/NO confirm to the built-in QUIT by reusing its id.
- [mods/menu-example/](../mods/menu-example/): a custom page whose entries call the mod's own native library.
- [mods/native-demo/](../mods/native-demo/): buttons, toggles and a Sound Settings slider backed by a native library.
- [mods/ray-tracing/](../mods/ray-tracing/): a RAY TRACING page under Options.
- [mods/multiplayer-native/roguesq_menu.json](../mods/multiplayer-native/roguesq_menu.json): the co-op lobby, with live status lines.

## Where menu files are read from

Menu files are layered in this order. A button with the same `id` as an earlier one replaces it in place, so a mod overrides a default (or an earlier mod) by reusing its id.

1. Built-in defaults: the main-menu `QUIT` and the pause `QUIT TO DESKTOP` (desktop only).
2. A `roguesq_menu.json` next to the exe, if present.
3. Each enabled mod's `roguesq_menu.json`, in mod order. A folder mod keeps it in its folder; a `.nrm` code mod packs it with `additional_files = [ "roguesq_menu.json" ]` under `[inputs]` in its `mod.toml`.

## Button reference

### Example

```json
{
  "buttons": [
    { "id": "quit", "menu": "main_menu", "type": "action",
      "action": "quit", "label": "QUIT", "confirm": "QUIT THE GAME?", "x": -165, "y": 0 },

    { "id": "fs_menu", "menu": "game_settings", "type": "toggle",
      "toggle": "fullscreen", "label_on": "FULLSCREEN: ON", "label_off": "FULLSCREEN: OFF",
      "after": "high_resolution" },

    { "id": "vol", "menu": "sound_settings", "type": "slider", "slider": "@myVolume",
      "label": "MOD VOL", "label_off": "MIN", "label_on": "FULL",
      "min": 0, "max": 100, "step": 10, "replace": "restore_defaults" }
  ],
  "hide": { "game_settings": ["crosshairs"] }
}
```

### Common fields

| Field | Meaning |
|---|---|
| `id` | Stable identifier. A later button with the same id overrides this one; other buttons can anchor to it. |
| `menu` | Where the button goes. Front end: `main_menu`, `options`, `game_settings`, `sound_settings`, `controller_settings`. Pause: `pause`, `pause_game_settings`. Any other name is a custom page (see [Custom pages](#custom-pages)). |
| `type` | `action`, `toggle`, `slider`, `submenu`, or `label` (a line the cursor skips, such as a status line). |
| `x` / `y` | Position offsets for front-end entries. |
| `flags` | Pause entry flag word (default `16385` = `0x4001`, drawn and selectable). |

Page entries also take:

| Field | Meaning |
|---|---|
| `gap_after` | Extra pixels below the line. |
| `scale` | Text scale (default 1). |
| `row` | Entries with the same row name share one line, left to right in JSON order. Place them with `x` and put `gap_after` on the last one. Up/down moves by line; left/right moves within the row. |
| `label_src` | Name of a live text source; the label follows it every frame (see [Live labels](#live-labels)). |

The button that opens a page can set `title_y`, the page title's y offset.

### Fields per type

- **`action`**: `action` (a [behavior key](#behavior-keys)), `label`, and optional `confirm`. A non-empty `confirm` shows a YES/NO prompt with that text; YES runs the action.
- **`toggle`**: `toggle` (a behavior key), `label_on`, `label_off`. The label shows the current state; selecting it flips the state. On `game_settings`, a toggle that `replace`s a native entry relabels in place, like the native STEREO/MONO switch (`replace: crosshairs` is the supported slot). Toggles elsewhere rebuild the menu.
- **`slider`**: `slider` (a behavior key), `label` (title), `label_off` / `label_on` (endpoint text, default OFF/MAX), `min` / `max` / `step`. A real slider bar is only available in `sound_settings`, and it must `replace` a native entry because that menu is full. It uses the native volume widget with its own value, which does not affect audio. Elsewhere a slider is a text bar in the label, stepped on select; `bar_width` (cells, default 10), `bar_filled` (default `|`) and `bar_empty` (default a space) shape it.
- **`submenu`**: opens a custom page. `page` (the page name), `label` (the opener's text), optional `title` (the page heading, default `label`), optional `action` (run when the page opens) and `leave_action` (run when the player leaves it).

### Placement

By default a button is appended after the existing entries. To place it:

| Field | Effect |
|---|---|
| `order` | Integer sort key. Native entries have implicit orders (index × 10), so `order: 45` lands between natives 4 and 5. |
| `before` / `after` | Anchor to a [native alias](#native-aliases) or another button's id. |
| `replace` | Take a native entry's position and remove it. |
| top-level `hide` | `{ "<menu>": ["alias", ...] }` removes native entries. |

### Native aliases

Anchors and `hide` name native entries by alias:

- `main_menu`: `start`, `options`.
- `game_settings`: `auto_roll`, `auto_level`, `free_camera`, `crosshairs`, `high_resolution`, `restore_defaults`, `back`.
- `sound_settings` (slider `replace` targets): `music`, `sound_fx`, `speech`, `subtitles`, `stereo`, `restore_defaults`, `back`. The menu has a hard limit of 8 slots, so a slider must replace an entry you can spare (the demo replaces `stereo`).

The tables are `native_aliases()` and `snd_alias_slot()` in [src/game/menu_config.cpp](../src/game/menu_config.cpp); extend them to expose more entries.

### Behavior keys

The value of `action`, `toggle` or `slider` names what the button does.

Built in:

- **Actions:** `quit`, `touch_layout` (opens the touch-control layout editor, Android) and `none` (no-op, for info rows). The multiplayer mod adds `mp_host`, `mp_join`, `mp_edit`, `mp_leave`.
- **Toggles:** `fullscreen`, `gyro` (Android gyro aiming), `cutscene_draw_distance` (cutscenes keep the game's own view distance and fog), `framebuffer_writeback` (copy rendered frames back to the emulated RAM; off is faster on slow GPUs), and the ray-tracing switches `rt_lights`, `rt_shadows`, `rt_soft_shadows`, `rt_fog_shafts`. Settings toggles are saved to `roguesq_video.json` and apply live. A set `ROGUESQ_RT_*` env var pins a ray-tracing switch without changing the file.
- **Sliders:** `draw_distance`, the draw-distance multiplier as a percent (use `"min": 100, "max": 250`). The base game has no draw-distance UI; a menu mod adds one with this key.

From your own mod:

- **`config:<option id>`** shows one of your mod's config options. An `Enum` option is a toggle that steps to the next value when selected: with two values and `label_on` / `label_off` it shows those, otherwise `LABEL: VALUE`. A `Number` option is a slider over the button's `min` / `max` / `step` (match them to the option). The value is saved with the mod's config; code reads it with `recomp_get_config_u32` / `recomp_get_config_double`.
- **`@name`** calls a function your mod exports. See [modding.md](modding.md#menu-behavior-from-your-own-code-export) for the naming and signatures.
- Native mods can register named actions and conditions with the host API (`add_action`, `add_condition`; see [modding-host-api.md](modding-host-api.md)).

### Custom pages

A `submenu` button opens a page: its own titled screen in the native style. Buttons whose `menu` is the page name are its entries, and a BACK entry is added automatically.

```json
{ "type": "submenu", "menu": "main_menu", "page": "mymod", "label": "MY MOD",
  "title": "MY MOD SETTINGS" },
{ "type": "toggle",  "menu": "mymod", "toggle": "@myFlag",
  "label_on": "FLAG: ON", "label_off": "FLAG: OFF" },
{ "type": "action",  "menu": "mymod", "action": "quit", "label": "QUIT",
  "confirm": "QUIT THE GAME?" }
```

Limits: 7 entries per page (the 8th is BACK), and a page can't be opened from Game Settings (pages are hosted on it; see [Internals](#how-custom-pages-are-hosted)). B returns to the menu that opened the page.

### Pilot-first pages

A `main_menu` action with `"transition": "account"` goes to the pilot select, like START. Add `page` (and optionally `title`, `leave_action`) and choosing a pilot opens that page instead of mission select. When the page's `exit_when` condition holds (a host condition such as `mp_ready`), the game continues to mission select with the pilot loaded. The multiplayer lobby works this way.

### Live labels

A page entry with `label_src` shows text from a named host text source, updated every frame without rebuilding the menu. Sources are registered by native mods with `add_text_source` ([modding-host-api.md](modding-host-api.md#text-sources)). The only ones today are the multiplayer lobby's: `mp_status`, `mp_address`, `mp_host_address`, `mp_host_label`, `mp_join_label`, `mp_online`.

### Mouse and touch

Mouse and touch need no extra fields. Hit areas come from each entry's drawn text, position and scale, so they follow your `x`, `y`, `scale`, rows and live labels. Hovering or tapping highlights an entry and clicking confirms it; a `label` gets no hit area; right click is back. A `confirm` prompt's YES and NO are clickable too.

Sliders:

- A `sound_settings` slider uses the native volume bar: click or drag it to set the value.
- Any other slider is a text bar. Clicking on the bar part of the label sets the value at that point, rounded to `step`; clicking the label text steps it once, like confirming with a pad. The position is exact only when `bar_filled` and `bar_empty` are strings of the same length (the defaults are).

### Text the menu font can draw

Labels use the front-end font: upper-case `A`-`Z`, `0`-`9`, spaces and `;:_'/&",!?-`. `<` and `>` draw as filled arrows, `{` and `}` as hollow ones, and `.` is built at runtime. Lower case and other characters such as `(` `)` `+` `#` draw as blanks.

### Gotchas for mod authors

- A folder mod needs a `mod.json`, or its menu file is never read.
- The front-end menus hold at most 8 entries, and Options and Game Settings are already full. A new entry there must `replace` or `hide` a native one, or the host drops spacer lines to make room. The main menu fits three entries under the logo; past three the list moves up one line per extra entry.
- The pause menus show at most 16 lines. Each pause target (`pause`, `pause_game_settings`) currently takes one button.
- Mods that ship with the game are listed in [mods/platforms.json](../mods/platforms.json); see [modding.md](modding.md#shipping-a-mod-with-the-game).

---

## Internals

The rest of this page is for people changing the button system or the game's menus directly. It describes the game structures (from the decomp) and how [src/game/menu_config.cpp](../src/game/menu_config.cpp) drives them through `[[patches.hook]]` entries in `rogue_squadron.toml`. Nothing here requires editing `RecompiledFuncs/funcs_*.c`.

### Two separate menu systems

| | Front-end menu | Pause menu |
|---|---|---|
| Where | Title/options screens | In-mission (START during gameplay) |
| Overlay | `.ovl.menu` | `.ovl.mission` |
| State struct | `gCurrentMenuData` @ `0x800CE730` | HUD-ext struct `func_800C0084_type` |
| Entry model | `menu_entries[8]` + per-entry sub-type | `PauseMenuStuff[]` arrays, `0xFFFF`-terminated |
| Builder | `setupMenuData` `0x800BA0F0` | `func_800C298C` `0x800C298C` |
| Input/dispatch | `menuControllerInput` `0x800B47D0` | `func_800C1D3C` `0x800C1D3C` |

They share no state.

### Front-end data model: `gCurrentMenuData` @ `0x800CE730` (0xF8 bytes)

| off | field | notes |
|----|----|----|
| +0x00 | `char* screen_title` | title string |
| +0x04 | `u8 current_menu` | `enum Menu` (0 = MAIN_MENU) |
| +0x05 | `u8 back_menu` | menu the "Back" entry returns to |
| +0x08 | `char* menu_entries[8]` | one pointer per visible line |
| +0x28 | `u8 unk28[8]` | per-entry **sub-type** (drives selection) |
| +0x30 | `s16 entry_xy_offsets[8][2]` | per-entry x,y; +0x50/+0x52 right after are the title's x,y |
| +0x54 | `f32 entry_size_scaler[8]` | per-entry text scale (default set by builder) |
| +0x74 | `u32 unk74[8]` | per-entry param; for sub-type 1 = target `enum Menu` |
| +0x94 | `u8 current_menu_entry` | highlighted index |
| +0x95 | `u8 num_menu_entries` | visible count |
| +0x96 | `s16 list_base` | list y origin, set per menu (default 40; main 165, options 20/30, 4 and 5 are -90); entry i's y = `list_base + 36*i - 18*n + entry_xy_offsets[i].y`, x = `entry_xy_offsets[i].x` centered (setupMenuData tail 0x800C4CF0) |
| +0x98 | `u16 active_entries` | bitmask, bit i set if entry i is selectable |
| +0xA0 | `f32 highlight_timer` | pulse animation |

`enum Menu`: `MAIN_MENU=0, ACCOUNT_SELECTION=1, OPTIONS=2, GAME_SETTINGS=3, ELITE_ROGUES=4, CONTROLLER_SETTINGS=5, SOUND_SETTINGS=6, CHEAT_MENU=7, BIOGRAPHIES=8, LANGUAGE_SELECT=9, SHOWROOM=10, CONCERT_HALL=11, AT_THE_MOVIES=12`.

Ids 13 and up are custom pages and `0xFF` means none. The game calls `setupMenuData` with constants 0, 2, 4 and 9 (`menuControllerInput`) and 10 (`menuSubtype02Handler`); every other id comes from an entry's `unk74` target. `gCurrentMenuData` is menu-overlay BSS, so before the front end loads (the boot intro) `0x800CE730` is heap: values read there, such as `current_menu = 0x80`, are model data, not menu ids.

### How a menu is built: `setupMenuData(menuId, controller, flags)` @ `0x800BA0F0`

A 13-arm jump table, one arm per `enum Menu`. Each arm fills `screen_title`, `menu_entries[]`, `unk28[]`, `unk74[]` and offsets, incrementing a running entry counter, and writes `current_menu` / `back_menu` from `menuId`.

The MAIN_MENU arm (`0x800BA1B4`) fills two of the eight slots:

- Entry 0 "START": `menu_entries[0] = getGameOrFrontText(0xA2)`, `unk28[0]=1` (sub-menu), `unk74[0]=1` (ACCOUNT_SELECTION).
- Entry 1 "OPTIONS": `menu_entries[1] = getGameOrFrontText(0xA3)`, `unk28[1]=1`, `unk74[1]=2` (OPTIONS).

START opens the pilot submenu; gameplay begins several menus deeper.

All arms fall through to a common tail at `0x800BAF64`, which sets `num_menu_entries` (+0x95) to the counter and, for each entry with a non-zero `strlen`, sets its bit in `active_entries` (+0x98). That is the whole activation rule: an entry is visible and selectable if it has a non-empty `char*` and is within `num_menu_entries`.

### How buttons are placed: full-list rebuild

`menu_entries[8]` is a hard cap (index 8 overwrites `unk28[]`), and some menus already use all 8:

- OPTIONS (menu 2): Biographies, Elite Rogues, Passcodes, Game Settings, Sound Settings, Controller Settings, Back, and a blank spacer.
- GAME SETTINGS (menu 3): Auto Roll, Auto Level, Free Camera, Crosshairs, High Resolution, a spacer, Restore Defaults, Back.

`rebuild_frontend_menu()` in `menu_config.cpp` owns the whole list. After the game builds a menu it reads the native entries, merges the menu's mod buttons by `order` / anchor, drops `hide` / `replace`d natives and empty spacer entries (`label_is_empty()`) to fit 8, sorts by order, and writes the list back with a new `num_menu_entries`. It runs from the hook on the common tail (`setupMenuData` at `0x800BAF70`, which calls `rs64_menu_install_main`).

A mod button is a synthesized slot. Toggles and confirm actions get a self-targeting sub-type 1 (`unk74` = current menu id), so selecting one re-runs `setupMenuData`. That is the only way a label re-commits, because the menu measures text once per build, not per frame. Plain actions use sub-type 4, a no-op the confirm intercept owns. Each written slot records a binding in `s_slot[]` for the intercept.

A front-end menu with more than 8 entries would need a scrolling list or a custom-rendered submenu, which doesn't exist yet.

### Input and selection: `menuControllerInput` @ `0x800B47D0`

New presses come from `getControllerNewButtonsPressed` (`0x80079F50`): A = `0x8000`, Start = `0x1000` (`0x9000` = confirm), B = `0x4000` (back).

On confirm:

- `0x800B51B0` loads `current_menu_entry` (+0x94).
- `0x800B51B8` loads the entry's sub-type `unk28[current_menu_entry]`. The confirm intercept hooks here (`rs64_menu_confirm_main`).
- `0x800B51BC` range-checks the sub-type to [1, 26]; 0 or 27+ do nothing.
- `0x800B51DC` jumps through the table at `0x800A66F8`, indexed by `sub-type - 1`.

| sub-type | action on confirm |
|---|---|
| 1 | go to sub-menu (target = `unk74[entry]`) |
| 3 | account name/rank select |
| 5 | on/off setting toggle |
| 6/7/8 | biography back / left / right |
| 4, 20 | no-op on A |
| 13 | non-selectable separator (nav skips it) |
| 21/22/23 | music / sfx / speech volume |
| 25 | "Back" |
| 26 | quit-to-attract style transition |

Sub-type 1 (handler `0x800B521C`) stores `unk74[entry]`'s low byte as the target menu and starts the fade-out (`var_s6 = 1`). When the fade completes (`var_s6 == 2`) the per-frame state machine calls `setupMenuData(target, ...)` at `0x800B5B40`.

### The YES/NO confirm

Because labels are measured once per build, the confirm prompt rides the game's own menu transition:

- A `confirm` entry is a self-targeting sub-type-1 submenu. Selecting it sets `ConfirmState::Pending`; the fade re-runs `setupMenuData`, and the install hook sees Pending and builds a two-entry YES/NO menu.
- YES and NO are also self-targeting submenus: YES runs the action and clears the state, NO just clears it. Each transition back redraws the normal labels.

This is why a confirm shows the menu-swipe animation between steps. Live labels (`label_src`) avoid a rebuild by relaying out one entry with `0x800C4C8C(&gCurrentMenuData, 1 << entry, 0)` from the `updateMenuPerFrame` hook (`rs64_menu_frame`).

### Pilot-first page hooks

The pilot's confirm opens the page through a hook at `menuControllerInput` `0x800B5428`, which replays the sub-type-1 steps including the menu wipe (`0x800B3F24`). Leaving it once `exit_when` holds goes through a hook at `0x800B4CA8`, which sets `$s6 = 0xA`, `sp+0x80 = sp+0x9F = 1` and starts the fade from `0x800A6760`, the same path a chosen pilot takes.

### How custom pages are hosted

A fresh menu id (13+) renders blank, because `setupMenuData` skips the per-menu builder that sets up entry text. So a page is hosted on a real menu id (`game_settings`) whose builder runs first; the install hook then overrides its title and entries. It is gated by `s_active_page` in `menu_config.cpp`: the opener's confirm records the page name and the parent to return to, the sub-type-1 transition lands on the host menu, and the host renders natively whenever no page is active. The page sets `gCurrentMenuData+0x05` (B's target) to its parent. `gCurrentMenuData+0x50/+0x52` are the title's x/y offsets (title text slot 9); the game zeroes them on every build.

### How the Sound Settings slider is wired

The native volume slider is tied to the three audio channels, so a mod slider reuses that widget. `sound_settings_install` (in the `setupMenuData` tail) rewrites the `replace` target to sub-type 21 with the mod's title, gives it an unused channel byte (`0x80130B60 + ch`, ch ≥ 3, past music/sfx/speech), and seeds that byte from the behavior's get. Three hooks do the rest: the common slider handler redirects the channel index at `sp+0xEF` to the mod channel; `getGameOrFrontText` for textId `0xB7` / `0xB8` returns the endpoint labels; `updateMenuPerFrame` pushes the channel byte to the behavior's set. All are inert unless a mod declares a Sound Settings slider.

### Entry text and the font

`menu_entries[i]` is a null-terminated `char*` in RDRAM. The vanilla arms fill it from `getGameOrFrontText(textId)` (`0x8005589C`), which returns a pointer into a loaded `.txt` bank (`0xA2` = "START/NEW GAME", `0xA3` = "OPTIONS"). The glyph builder (`buildScaledFormatTextElement`, `0x800B3AFC`) emits one TEXRECT per character.

The front-end font is `italic35` (charset at `0x8003B450`: `A`-`Y`, `0`-`9`, then `;:_'/&"Z,!?-<>{}ÄÖÜ`). It has no period, so `ensure_menu_period` (`menu_config.cpp`) turns the unused `Ü` glyph into one at runtime from the colon's lower dot, and maps `.` to that slot in the font's char-to-glyph table (pointer at the loaded font's charset pointer + 0x14).

### Pause menu

The pause menu lives in the mission overlay's HUD struct. Three functions run every frame from `handleHUD` (`0x800C0084`):

- `func_800C1D3C`: state machine, input and selection dispatch (the interactive work is its `case 3`).
- `func_800C298C`: layout and text builder (`sprintf` plus the glyph rasterizer per line, highlight coloring).
- `func_800C1B64(controllerIdx, mode)`: button-edge query (mode 0/1 up/down, 2/3 left/right, 4 back, 5/6 confirm).

Pause is gated on `D_8010CA20` (`0x8010CA20`): `5` = paused. The pause phase is `unk258` in the HUD struct (0 closed, 1 enter animation, 2 exit animation, 3 interactive).

#### Data model: `PauseMenuStuff` arrays

`struct PauseMenuStuff { u16 flags; u16 textId; u16 nextMenu; }` (6 bytes). Four contiguous rodata arrays, `0xFFFF`-terminated (no count field):

| array | address | role |
|---|---|---|
| `pauseMenuMissionObjectives` | `0x80109DD4` | root (objectives + links) |
| `pauseMenuGameSettings` | `0x80109E24` | game settings |
| `pauseMenuAudioSettings` | `0x80109E68` | audio settings |
| `pauseMenuAbortLevel` | `0x80109EEC` | abort confirm |

`current_menu` (u8 @ HUD +0xD6A) selects the array; `current_entry` (u16 @ HUD +0xD68) is the highlighted selectable index.

Flag bits (selection cascade in `func_800C1D3C` case 3):

| bit | meaning |
|---|---|
| 0x0001 | selectable (participates in up/down nav) |
| 0x0010 | mission-objective line (`textId` indexes objective table) |
| 0x0020 | submenu link (target array from `nextMenu`) |
| 0x0040 | Continue/Resume: sets `unk258 = 2` (begin exit) |
| 0x0080 | Abort Level (below) |
| 0x0200 | volume slider (left/right edits) |
| 0x1000 | non-drawn spacer |
| 0xFFFF | terminator |

0x0002/0x0004/0x0008 are setting toggle bits; 0x0100 inverts the displayed state.

Abort Level does not tear down the mission directly. It sets `D_80130B14` (`0x80130B14`) = 0 and `D_8010C9E0` (`0x8010C9E0`) |= 1, a request flag the outer game-state handler polls before returning to the front end. Making those two writes triggers quit-to-menu.

#### Rendering limits

`func_800C298C` renders each entry into a fixed `menu_elements[16]` (HUD +0x264) with glyph pools of `0x12C`, so a pause menu shows at most 16 lines and `0x12C` glyphs in total. The four arrays are contiguous in rodata, so one can't grow in place: point `current_menu` at an extended copy, or append before the terminator of an array with slack.

#### How pause buttons are wired

`rs64_pause_install` (`menu_config.cpp`), hooked at `initOrUpdatePauseScreenDim`, places pause buttons only while the menu is open (`D_8010CA20 == 5`), so it never touches the arrays during level load or briefing (which starves the audio scheduler). Each button takes a slot:

- `menu: "pause"`: the root objectives terminator slot @ `0x80109E1C`.
- `menu: "pause_game_settings"`: the GAME SETTINGS spacer @ `0x80109E54`.

Each installed button keeps a valid `textId` (an out-of-range one makes `getGameOrFrontText` return NULL and the layout `sprintf("%s", NULL)` fault) and is tagged with a sentinel in the unused `nextMenu` field, assigned from `0xF010` up. Two hooks dispatch through the sentinel map:

- **Render** at `runPauseMenuStateMachine` `0x800C3550` (`ctx->r20` = `&entry.textId`, `nextMenu` at +0x2): `rs64_pause_label_for` overrides `ctx->r2` with the button's label (live ON/OFF for toggles). The pause menu rebuilds text every frame, so no re-setup is needed.
- **Confirm** at `initOrUpdatePauseScreenDim` `0x800C2458` (`ctx->r18` = `&entry`, `ctx->r3` = its flags, `ctx->r17` = the HUD struct): `rs64_pause_confirm` runs the button's action or flips its toggle.

More than one button per slot needs a relocated `PauseMenuStuff` array, keeping the visible count at 16 or fewer.

### Changing the system itself

- **Hooks.** Each hook in `rogue_squadron.toml` is one call into [src/game/toml_hooks.c](../src/game/toml_hooks.c), which calls the host code in `menu_config.cpp`:

  ```toml
  [[patches.hook]]
  func = "setupMenuData"
  before_vram = 0x800BAF70
  text = '{ rs64_th_setupMenuData_800BAF70(rdram, ctx); }'
  ```

  Hook text is baked in at recompile time, so after changing a hook run `cmake --build build --config Debug --target regen_funcs` before building the game. Changes to `menu_config.cpp` alone need only a normal build.
- **New built-in behaviors.** Add a toggle to `toggles()` or an action to `actions()` in `menu_config.cpp`, then reference it by name from a menu file. Native mods can add actions without touching the base game (`add_action`).
- **Label storage.** `menu_entries[i]` must be an RDRAM address; the game reads it with no masking, so a host pointer won't work. Reuse a game string with `getGameOrFrontText(textId)`, or allocate with `recomp::alloc(rdram, size)` (the recomp heap at `0x81000000`+, above the game's 8 MB) and convert with `(uint8_t*)p - rdram + 0x80000000`. Don't write to "unused" addresses inside the game's 8 MB; the game believes it owns all of it.
- **Quit.** A QUIT button pushes `SDL_QUIT` (`rs64_menu_request_quit`); the event loop in [src/input/input_host.cpp](../src/input/input_host.cpp) runs the quit handlers and then `_Exit`. Don't call `exit`: the game threads and RT64 are still running, so static destructors hang or crash.
- **Writes** use the recomp `MEM_*` address convention (word: no xor, half `^2`, byte `^3`).

### Engine gotchas

- Front-end text is laid out at setup, not per frame. Changing a `menu_entries[i]` pointer updates navigation but not the drawn label; relay the entry out with `0x800C4C8C` or re-run `setupMenuData`.
- Front-end sub-types outside [1, 26] are ignored on confirm. Use an in-range no-op (4 or 20) plus an intercept hook rather than a new sub-type.
- A non-selectable entry in slot 0 hangs the navigation loop. Keep active entries contiguous from slot 0 (page labels are stored after BACK and moved into place with a y offset for this reason).
- An entry with an empty or NULL label is skipped; make sure the pointer is valid before the common tail runs.
