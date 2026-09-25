# Adding menus and buttons

How to add or modify menu entries in the front-end menu and the in-mission
pause menu. Both systems are data-driven: a new button is mostly writing a few
fields into the right struct and intercepting its selection.

Custom buttons are declared in JSON (see
[Configuring menus](#configuring-menus-the-button-system)) and applied to the live
menus by [src/main/menu_config.cpp](../src/main/menu_config.cpp) via
`[[patches.hook]]` hooks; custom label strings live in extended RDRAM via
`recomp::alloc`. The rest of this doc explains the underlying game structures the
button system drives, for when you need to extend it.

Nothing here requires editing `RecompiledFuncs/funcs_*.c` — those are regenerated
and any hand edits are discarded.

---

## Two separate menu systems

| | Front-end menu | Pause menu |
|---|---|---|
| Where | Title/options screens | In-mission (START during gameplay) |
| Overlay | `.ovl.menu` | `.ovl.mission` |
| State struct | `gCurrentMenuData` @ `0x800CE730` | HUD-ext struct `func_800C0084_type` |
| Entry model | `menu_entries[8]` + per-entry sub-type | `PauseMenuStuff[]` arrays, `0xFFFF`-terminated |
| Builder | `setupMenuData` `0x800BA0F0` | `func_800C298C` `0x800C298C` |
| Input/dispatch | `menuControllerInput` `0x800B47C4` | `func_800C1D3C` `0x800C1D3C` |

They share no state. Pick the one you want and read that section.

---

## Configuring menus: the button system

Menu customization is data-driven through JSON, resolved by
[src/main/menu_config.cpp](../src/main/menu_config.cpp). A **button** is one typed
menu entry — `{ menu, type, behavior, labels, placement }` — and the same model
covers the front-end and pause menus. Sources are layered in this order:

1. Internal defaults (`default_config()`): the main-menu `QUIT` action and the
   pause `QUIT TO DESKTOP` action. No shipped default JSON.
2. A single `roguesq_menu.json` next to the exe, if present.
3. Each **mod** is a subfolder of `mods/` (next to the exe); every mod folder with
   a `roguesq_menu.json` is applied on top, in folder-name order.

A button with the same **`id`** as an earlier one **replaces it in place**, so a
mod overrides a default (or an earlier mod) by reusing its id. Editing JSON needs
no rebuild — relaunch. Two example mods ship to `build/Debug/mods/`:
[quit-prompt](../mods/quit-prompt/) (adds a confirm to the default QUIT by id) and
[fullscreen-toggle](../mods/fullscreen-toggle/) (`enabled_by_default: true`, adds
the FULLSCREEN toggle). Your own mod folders alongside them are never overwritten.

### Schema

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

**Common fields**

- `id` — stable identifier; a later button with the same id overrides it. Also the
  handle other buttons anchor to.
- `menu` — a named location, resolved internally to a menu id or pause array:
  front-end `main_menu`, `options`, `game_settings`, `sound_settings`,
  `controller_settings`; pause `pause`, `pause_game_settings`. Mods never touch
  ids or addresses.
- `type` — `action`, `toggle`, `slider`, or `submenu`.
- `menu` — also names a custom **page** (any name that is not a built-in location);
  buttons with that name are the page's entries.
- `x`/`y` — position offsets (front-end appended entries).
- `flags` — pause entry flag word (default `16385` = `0x4001`, drawn + selectable).

**Per type**

- `action` — `action` (a behavior key), `label`, optional `confirm` (a non-empty
  string shows a YES/NO prompt; YES runs the action).
- `toggle` — `toggle` (a behavior key), `label_on`, `label_off`. The label reflects
  the state live; selecting it flips the state. A `game_settings` toggle that
  `replace`s a native entry relabels **in place** (the native handler re-renders
  the row on confirm, no full-menu rebuild) — like the native STEREO/MONO switch.
  `replace: crosshairs` is the supported slot. Set `ROGUESQ_NO_TOGGLE_INPLACE=1`
  to fall back to a full rebuild. Toggles elsewhere (pages, other menus) rebuild.
- `slider` — `slider` (a behavior key), `label` (title), `label_off`/`label_on`
  (endpoint text, default OFF/MAX), `min`/`max`/`step`. **Only in `sound_settings`**,
  and it must `replace` a native entry (the menu is full). It becomes the real
  native volume-slider widget (left/right edits the bar, orange fill, endpoint
  labels), with an independent value that does not affect audio. The value maps to
  `min..max` and is pushed to the behavior's slider-set each frame; the current
  value is read from slider-get at menu open. Elsewhere (`game_settings`, pause) a
  `slider` falls back to a text bar in the label, stepped on select.
- `submenu` — opens a custom **page**. `page` (the page name), `label` (the opener
  entry text), optional `title` (the page's heading, default = `label`). See
  **Custom pages** below.

**Placement** (optional; default appends after existing entries)

- `order` — integer sort key. Native entries get implicit orders (index × 10), so
  `order: 45` lands between natives 4 and 5.
- `before` / `after` — anchor to a native alias or another button id
  (`"after": "high_resolution"`).
- `replace` — take a native entry's position and remove the original.
- top-level `hide` — `{ "<menu>": ["alias", …] }` removes native entries.

### Behavior keys

`action` / `toggle` values are keys into a host registry in `menu_config.cpp`:

- actions: `quit`.
- toggles: `fullscreen` (`{ get, toggle }`).
- sliders: `draw_distance`, the `drawDistance` multiplier from `roguesq_video.json` as a percent (use `"min": 100, "max": 250`). It applies live and is saved. The base game shows no draw-distance UI; a menu mod adds one with this key.

**Code mods (`.nrm`).** To change game behavior (not just menus), write a code mod: it patches game functions directly (`RECOMP_PATCH` replaces a base function, `RECOMP_HOOK` / `RECOMP_HOOK_RETURN` run around one) and needs no base changes. [mods/infinite-secondary/](../mods/infinite-secondary/), [mods/any-craft/](../mods/any-craft/) and [mods/larger-object-pool/](../mods/larger-object-pool/) (calls game functions) are complete examples: MIPS C in `src/`, `mod.ld`, a RecompModTool manifest `mod.toml` that references `syms/rogue_squadron.syms.toml` (patch names must match a function there), and a two-line `CMakeLists.txt` calling `add_code_mod` from [tools/mods/code_mod.cmake](../tools/mods/code_mod.cmake). Build RecompModTool once with `cmake --build build/N64ModernRuntime/librecomp/N64Recomp --config Debug --target RecompModTool`, then `cmake -S mods/<name> -B mods/<name>/build` and `cmake --build mods/<name>/build`. The `.nrm` lands in `mods/` and is staged beside the exe; any `mods/<name>/` holding a `mod.toml` is stripped from staging automatically. Call game functions by their name in the syms file (e.g. `rs_malloc`, `func_8004028C`) and reach game data by absolute address. Mod ids must be C identifiers (`infinite_secondary`). Enable it in the Mods panel (F1) or `mods.json`; code mods apply at startup. LiveRecomp does not materialize branch-and-link `$ra` values, so a `RECOMP_HOOK` on a function that reads `$ra` as data will misbehave.

Add built-ins by extending `actions()` / `toggles()`.

**Mod-provided behavior (`@export`)** — a mod can ship a **native library**
(`native_libraries` in `mod.json`) and reference its exports by name with a
leading `@`. A native export is native C `void(uint8_t* rdram, recomp_context*
ctx)` (no guest stack needed); `menu_config.cpp` resolves the name via
`recomp::mods::get_mod_export(mod_id, name)` and calls it with a zeroed `ctx`,
marshaling through registers: a **toggle get** and **slider get** return their
value in `ctx->r2`, a **slider set** reads its value from `ctx->r4`. Naming:
`"action": "@doThing"` calls export `doThing`; `"toggle": "@myToggle"` calls
`myToggle` to flip and `myToggle_get` to read state; `"slider": "@myVolume"` calls
`myVolume` to set (value in `r4`, in `min..max`) and `myVolume_get` to read the
current value. The DLL must also export `uint32_t recomp_api_version = 1`. See
[mods/native-demo/](../mods/native-demo/) for a working example.

### Native aliases

Anchors (`before`/`after`/`replace`) and `hide` reference **native entries** by a
per-menu alias, mapped in `native_aliases()` to a match on sub-type (+ param):

- `main_menu`: `start`, `options`.
- `game_settings`: `auto_roll`, `auto_level`, `free_camera`, `crosshairs`,
  `high_resolution`, `restore_defaults`, `back`.
- `sound_settings` (slider `replace` targets): `music`, `sound_fx`, `speech`,
  `subtitles`, `stereo`, `restore_defaults`, `back`. The menu is a hard 8 slots, so
  a slider replaces one; pick an expendable entry (the demo replaces `stereo`).

Extend the tables to expose more native entries as anchors.

### How the Sound Settings slider is wired

The native volume slider is welded to the three audio channels, so a mod slider
reuses that widget rather than building one. `sound_settings_install` (in the
`setupMenuData` tail) rewrites the `replace` target to sub-type 21 with the mod's
title, assigns it an unused channel byte (`0x80130B60 + ch`, ch ≥ 3, past
music/sfx/speech), and seeds that byte from the behavior's slider-get. Three hooks
do the rest: the common slider handler entry redirects the channel index at
`sp+0xEF` to the mod channel (independent value); `getGameOrFrontText` for textId
`0xB7`/`0xB8` is overridden with the endpoint labels; `updateMenuPerFrame` reads
the channel byte and pushes it to the behavior's slider-set. All are inert unless a
mod actually declares a Sound Settings slider.

### Custom pages

A `submenu` button opens a mod-defined **page**: its own titled screen of entries,
in the native style, with Back. Buttons whose `menu` is the page name (any name
that is not a built-in location) are the page's entries; a Back is appended
automatically.

```json
{ "type": "submenu", "menu": "main_menu", "page": "mymod", "label": "MY MOD",
  "title": "MY MOD SETTINGS" },
{ "type": "toggle",  "menu": "mymod", "toggle": "@myFlag",
  "label_on": "FLAG: ON", "label_off": "FLAG: OFF" },
{ "type": "action",  "menu": "mymod", "action": "quit", "label": "QUIT",
  "confirm": "QUIT THE GAME?" }
```

Implementation: a fresh menu id (≥ 13) renders blank, because `setupMenuData` skips
the per-menu builder that sets up the entry text. So a page is **hosted on a real
menu id** (`game_settings`) whose builder runs the setup first; our install hook
then overrides its title and entries. It is **flag-gated** by `s_active_page`
(`menu_config.cpp`): the opener's confirm records the page name and the parent to
return to, the sub-type-1 transition lands on the host, and the host is rendered
natively whenever no page is active. See `plans/custom-menu-pages-plan.md` and
`mods/menu-example/` for a working example. Limits: 7 entries per page (the 8th is
Back), and a page cannot be opened from the host menu itself.

---

## Front-end menu

### Data model — `gCurrentMenuData` @ `0x800CE730` (0xF8 bytes)

| off | field | notes |
|----|----|----|
| +0x00 | `char* screen_title` | title string |
| +0x04 | `u8 current_menu` | `enum Menu` (0 = MAIN_MENU) |
| +0x05 | `u8 back_menu` | menu the "Back" entry returns to |
| +0x08 | `char* menu_entries[8]` | one pointer per visible line |
| +0x28 | `u8 unk28[8]` | per-entry **sub-type** (drives selection) |
| +0x30 | `s16 entry_xy_offsets[8][2]` | per-entry x,y |
| +0x54 | `f32 entry_size_scaler[8]` | per-entry text scale (default set by builder) |
| +0x74 | `u32 unk74[8]` | per-entry param; for sub-type 1 = target `enum Menu` |
| +0x94 | `u8 current_menu_entry` | highlighted index |
| +0x95 | `u8 num_menu_entries` | visible count |
| +0x98 | `u16 active_entries` | bitmask, bit i set if entry i is selectable |
| +0xA0 | `f32 highlight_timer` | pulse animation |

The `enum Menu`: `MAIN_MENU=0, ACCOUNT_SELECTION=1, OPTIONS=2, GAME_SETTINGS=3,
ELITE_ROGUES=4, CONTROLLER_SETTINGS=5, SOUND_SETTINGS=6, CHEAT_MENU=7,
BIOGRAPHIES=8, LANGUAGE_SELECT=9, SHOWROOM=10, CONCERT_HALL=11, AT_THE_MOVIES=12`.

Ids 13 and up are custom pages (see above) and `0xFF` means none. The game calls `setupMenuData` with constants 0, 2, 4 and 9 (`menuControllerInput`) and 10 (`menuSubtype02Handler`); every other id comes from an entry's `unk74` target. `gCurrentMenuData` is menu-overlay BSS, so before the front end loads (the boot intro) `0x800CE730` is heap: values read there, such as `current_menu = 0x80`, are model data, not menu ids.

### How a menu is built — `setupMenuData(menuId, controller, flags)` @ `0x800BA0F0`

A 13-arm jump table (one arm per `enum Menu`). Each arm fills `screen_title`,
`menu_entries[]`, `unk28[]`, `unk74[]`, and offsets, incrementing a running
entry counter. It writes `current_menu` and `back_menu` (+0x04/+0x05) from the
passed `menuId`.

The **MAIN_MENU arm** is at `0x800BA1B4`. It fills exactly two of the eight
slots:

- Entry 0 "START": `menu_entries[0] = getGameOrFrontText(0xA2)`, `unk28[0]=1`
  (sub-menu), `unk74[0]=1` (→ ACCOUNT_SELECTION).
- Entry 1 "OPTIONS": `menu_entries[1] = getGameOrFrontText(0xA3)`, `unk28[1]=1`,
  `unk74[1]=2` (→ OPTIONS).

So **six entry slots are free** on the main menu, and START does *not* launch a
mission directly — it opens the account/pilot submenu; gameplay begins several
menus deeper.

All 13 arms fall through to a **common tail at `0x800BAF64`** that finalizes the
menu generically:

1. `num_menu_entries (+0x95) = counter`.
2. For each entry `i < num_menu_entries`: `strlen(menu_entries[i])`; if nonzero,
   set bit i in `active_entries (+0x98)`.

That is the whole activation rule: **an entry becomes visible and selectable
simply by having a non-empty `char*` and being within `num_menu_entries`.** No
per-menu magic counts to update.

### How the install engine places entries — full-list rebuild

`menu_entries[8]` is a **hard cap** — writing index 8 overwrites the adjacent
`unk28[]` array — and some menus already use all 8:

- **OPTIONS (menu 2)** is a full hub: Biographies, Elite Rogues, Passcodes,
  Game Settings, Sound Settings, Controller Settings, Back (+ one blank spacer).
- **GAME SETTINGS (menu 3)** is full: Auto Roll, Auto Level, Free Camera,
  Crosshairs, High Resolution, a spacer, Restore Defaults, Back.

Rather than poke a single slot, `rebuild_frontend_menu()` in `menu_config.cpp`
**owns the whole list**: after the game builds a menu, it reads the native entries,
merges this menu's mod buttons by `order`/anchor, drops `hide`/`replace`d natives
and the invisible **spacer** entries (empty-label, `label_is_empty()`) to fit 8,
sorts by order, and writes the final list back with a new `num_menu_entries`. This
is what makes reorder/override/hide work and where a mod button lands next to a
native anchor (e.g. FULLSCREEN `after high_resolution` reclaims the spacer's room).

A mod button is a synthesized slot: its label from `label`/`label_on|off`, and a
**self-targeting sub-type 1** (`unk74 = current menu id`) for toggles and
confirm-actions so selecting it re-runs `setupMenuData` — the only way the label
re-commits (the menu measures text once per build, not per frame) and how a live
toggle shows its new state after the confirm intercept flips it. Plain actions use
sub-type 4 (a no-op the intercept owns). Each written mod-button slot records a
binding (`s_slot[]`) the confirm intercept reads.

**Beyond 8**: the front-end is hard-capped, so a genuinely large front-end menu
needs a scrolling list or a custom-rendered submenu (not yet built). The pause
menus scale to 16 visible lines via array relocation.

### Input and selection dispatch — `menuControllerInput` @ `0x800B47C4`

New button presses are read via `getControllerNewButtonsPressed` (`0x80079F50`).
Masks: **A = 0x8000, Start = 0x1000** (`0x9000` = "confirm"), **B = 0x4000**
("back").

On confirm, the handler:

- `0x800B51B0`: loads `current_menu_entry (+0x94)`.
- `0x800B51B8`: **`lbu` the selected entry's sub-type `unk28[current_menu_entry]`**.
- `0x800B51BC`: range-checks the sub-type to **[1, 26]**; 0 or ≥27 do nothing.
- `0x800B51DC`: `jr` through a jump table at `0x800A66F8` indexed by
  `sub-type - 1`.

Sub-types (abridged):

| sub-type | action on confirm |
|---|---|
| **1** | go to sub-menu (target = `unk74[entry]`) |
| 3 | account name/rank select |
| 5 | on/off setting toggle |
| 6/7/8 | biography back / left / right |
| 4, 20 | no-op on A |
| 13 | non-selectable separator (nav skips it) |
| 21/22/23 | music / sfx / speech volume |
| 25 | "Back" |
| 26 | quit-to-attract style transition |

**Sub-type 1 transition** (handler `0x800B521C`): reads `unk74[entry]` low byte
→ `spBF` (target menu), sets the transition state `var_s6 = 1` (begin fade-out).
It does **not** call `setupMenuData` immediately. The per-frame state machine
later, when the fade completes (`var_s6 == 2`), runs
`setupMenuData(spBF, controller, flags)` at `0x800B5B40`, which rebuilds
`gCurrentMenuData` for the target menu.

### Entry text

`menu_entries[i]` is a plain null-terminated `char*`. The vanilla arms fill it
from `getGameOrFrontText(textId)` (`0x8005589C`), which returns a pointer into a
loaded `.txt` bank (`0xA2` = "START/NEW GAME", `0xA3` = "OPTIONS"). The common
tail `strlen`s it and the glyph builder (`buildScaledFormatTextElement`
`0x800B3AFC`) walks it byte-by-byte emitting one TEXRECT per character. You can
point an entry at **any** valid ASCII buffer in RDRAM using glyphs the font
supports — see [where to store a custom label](#where-to-store-a-custom-label).

---

## Pause menu

Separate system, living in the mission overlay's HUD struct. Three functions,
all driven every frame from `handleHUD` (`0x800C0084`):

- **`func_800C1D3C` @ `0x800C1D3C`** — state machine + input + selection
  dispatch (the interactive work is its `case 3`).
- **`func_800C298C` @ `0x800C298C`** — layout/text builder (`sprintf` +
  glyph rasterizer per line, highlight coloring).
- **`func_800C1B64` @ `0x800C1B64`** — button-edge query
  (`func_800C1B64(controllerIdx, mode)`; mode 0/1 up/down, 2/3 left/right, 4
  back, 5/6 confirm).

Pause is gated on the master word **`D_8010CA20` (`0x8010CA20`)**: `5` = paused.
The pause phase is `unk258` in the HUD struct (0 closed, 1 enter anim, 2 exit
anim, 3 interactive).

### Data model — `PauseMenuStuff` arrays

`struct PauseMenuStuff { u16 flags; u16 textId; u16 nextMenu; }` (6 bytes).
Four contiguous rodata arrays, **`0xFFFF`-terminated** (no count field):

| array | address | role |
|---|---|---|
| `pauseMenuMissionObjectives` | `0x80109DD4` | root (objectives + links) |
| `pauseMenuGameSettings` | `0x80109E24` | game settings |
| `pauseMenuAudioSettings` | `0x80109E68` | audio settings |
| `pauseMenuAbortLevel` | `0x80109EEC` | abort confirm |

`current_menu` (u8 @ HUD +0xD6A) selects the array; `current_entry` (u16 @ HUD
+0xD68) is the highlighted selectable index.

Flag bits (selection cascade in `func_800C1D3C` case 3):

| bit | meaning |
|---|---|
| 0x0001 | selectable (participates in up/down nav) |
| 0x0010 | mission-objective line (`textId` indexes objective table) |
| 0x0020 | submenu link (target array from `nextMenu`) |
| 0x0040 | **Continue/Resume** — sets `unk258 = 2` (begin exit) |
| 0x0080 | **Abort Level** — see below |
| 0x0200 | volume slider (left/right edits) |
| 0x1000 | non-drawn spacer |
| 0xFFFF | terminator |

(0x0002/0x0004/0x0008 toggle setting bits; 0x0100 inverts displayed state.)

**Abort Level (0x0080)** does not directly tear down the mission. It sets
`D_80130B14 (0x80130B14) = 0` and `D_8010C9E0 (0x8010C9E0) |= 1`, a
request-pending flag polled by the outer game-state handler which performs the
return-to-front-end. Mimic those two writes to trigger quit-to-menu.

### Rendering budget

`func_800C298C` renders each non-terminator entry into a **fixed** array
`menu_elements[16]` (HUD +0x264), with glyph pools sized `0x12C`. So **at most
16 visible lines per menu**, and total glyphs across visible lines ≤ 0x12C.
Because the four arrays are contiguous in rodata you cannot grow one in place —
for a new entry, relocate `current_menu` to point at an extended copy in your
own memory (patches build), or append before the terminator of an array that has
slack, keeping the visible count ≤ 16.

### Best injection point

Hook `func_800C1D3C` at **`0x800C2458`** (the `andi $v0,$v1,0x20` on the
selected entry's flags). There `ctx->r18` = selected `PauseMenuStuff*`,
`ctx->r3` = its flags, `ctx->r17` = the HUD struct (`current_entry` +0xD68,
`current_menu` +0xD6A). Detect a custom flag/`textId` and run your logic, or
fire quit-to-menu via the two Abort writes above.

---

## Override mechanisms

Two ways to inject behavior, both regen-safe:

### 1. TOML hook (`[[patches.hook]]`) — inline host C

In `rogue_squadron.toml`. Runs host C at a function entry (`func = "name"`) or a
specific instruction (`before_vram = 0xADDR`). The block gets `rdram` and `ctx`,
can read/write game RAM with the `MEM_*` macros, and can call `extern "C"` host
helpers. Existing examples: the `mainGameLoop` and `cinematicLoopBody` hooks.

```toml
[[patches.hook]]
func = "setupMenuData"
before_vram = 0x800BAF6C   # common tail, just after num_menu_entries is written
text = '''{
    extern void rs64_menu_inject_main(uint8_t* rdram, recomp_context* ctx);
    rs64_menu_inject_main(rdram, ctx);
}'''
```

Host helpers live in [src/main/hook_helpers.cpp](../src/main/hook_helpers.cpp)
as `extern "C"` functions (see `rs64_cine_yield`, `rs64_dbg_log4`).

### 2. `patches/` MIPS build — new game-RAM data / full overrides

Use when you need data resident in game RAM (e.g. a custom label string at a
stable KSEG0 address) or a whole-function `RECOMP_PATCH`. See
[AGENTS.md](../AGENTS.md) "Patches build" and
[patches/README.md](../patches/README.md). A patch `char[]` lands in RDRAM at a
real game address the menu code can read. To call host code from a patch,
declare a stub at a fake `0x8FXXXXXX` address in `patches/syms.ld` and implement
it in `src/main/`.

### The quit path

The SDL event loop ([src/main/main.cpp](../src/main/main.cpp), the `SDL_QUIT`
case) already performs a graceful shutdown + `exit(EXIT_SUCCESS)`. A "Quit"
button just needs a host helper that requests it — push an `SDL_QUIT` event or
call `exit`.

### Where to store a custom label

`menu_entries[i]` must be a valid **RDRAM** address (the game reads it via
`MEM_B` = `rdram + (vaddr - 0x80000000)`, with no masking); a host pointer will
not work. Options:

- **Reuse a text id**: `getGameOrFrontText(textId)` for an existing string — zero
  new data, but limited to strings the game already has.
- **Extended RDRAM (recommended, what the QUIT example uses).** The runtime
  commits **512 MB** (`recomp::mem_size`) and reserves regions *above* the game's
  8 MB for host/mod use — see `librecomp/addresses.hpp` (`cart_handle`
  `0x80800000`, `patch_rdram_start` `0x80801000`, `mod_rdram_start` `0x81000000`).
  A managed allocator, `recomp::alloc(rdram, size)`, hands out buffers from the
  recomp heap at `0x81000000`+ and returns a pointer into `rdram`; convert it to
  a KSEG0 vaddr with `(uint8_t*)p - rdram + 0x80000000`. The game can dereference
  it, and it never collides with game allocations (the game believes it owns only
  8 MB). This is the clean "our own space" — no runtime surgery needed.
- **Patch data**: define `char kQuitLabel[] = "QUIT";` in a `patches/` `.c`; it
  gets a stable game address in the static-data region. Good when the string is
  fixed at build time; less flexible than `recomp::alloc` for user-configurable
  text.
- **Host scratch (avoid)**: writing to some "unused" high address like
  `0x807FFF00` works in practice but the game thinks it owns all 8 MB, so it can
  collide. Prefer `recomp::alloc`.

---

## How the front-end implementation works

The whole front-end feature is [src/main/menu_config.cpp](../src/main/menu_config.cpp)
plus two one-line hooks. It is fully implemented and configured by the
[button system](#configuring-menus-the-button-system).

**Two hooks** in `rogue_squadron.toml`:

```toml
[[patches.hook]]
func = "setupMenuData"
before_vram = 0x800BAF70   # common tail, after num_menu_entries; before the active-bit loop
text = '''{ extern void rs64_menu_install_main(uint8_t* rdram); rs64_menu_install_main(rdram); }'''

[[patches.hook]]
func = "menuControllerInput"
before_vram = 0x800B51B8   # the sub-type load; A/Start already masked at 0x800B51A8
text = '''{ extern void rs64_menu_confirm_main(uint8_t* rdram); rs64_menu_confirm_main(rdram); }'''
```

**`rs64_menu_install_main`** runs inside `setupMenuData`'s common tail on every
MAIN_MENU (re)build, so its writes are committed by the same setup that renders
the vanilla labels. It:
- reads the built-in START/OPTIONS slots, drops the `hide`-listed ones, and
  **compacts** the kept ones from slot 0 (no gap);
- appends each configured `add` entry: `alloc_str()` puts the label in extended
  RDRAM (cached), and the slot gets sub-type **4** (a no-op-on-confirm the
  intercept owns) — or sub-type **1** if the entry has a `confirm`;
- sets `num_menu_entries`, so the tail's `strlen` loop lights up the active bits.

RDRAM writes use the recomp `MEM_*` address convention (word: no xor, half `^2`,
byte `^3`) so what it writes is what the game reads.

**`rs64_menu_confirm_main`** runs at the sub-type load when A/Start is pressed. A
non-confirm custom entry fires its action immediately (the `actions()` registry →
e.g. `rs64_menu_request_quit()`, which pushes `SDL_QUIT` for the graceful exit path
in [main.cpp](../src/main/main.cpp)); a toggle flips its state instead.

**The YES/NO confirmation** is the subtle part. The front-end measures its text
**once at setup**, not per frame, so a live label swap does *not* redraw — the
only way to change the on-screen labels is to re-run `setupMenuData`. So confirm
is a small state machine that rides the engine's own menu transition:
- a `confirm` entry is a **self-targeting sub-type-1 submenu** (`unk74` = MAIN_MENU);
  selecting it sets `ConfirmState::Pending` and the vanilla fade → `setupMenuData`
  re-runs → `install` sees Pending and builds a two-entry **YES/NO** menu (labels
  commit normally);
- YES and NO are themselves self-targeting submenus: YES fires the action and
  clears the state, NO just clears it — each rides another transition back, so the
  normal menu's labels redraw too.

This is why a confirm shows the game's menu-swipe animation between steps.

**Build**: hook `text` is baked in at recompile time, so changes to the hooks or
`menu_config.cpp` need `cmake --build build --config Debug --target regen_funcs`
then `--target RogueSquadron64Recomp` (the module itself only needs the exe
relink, but the hooks need the regen). Editing a mod's JSON needs no rebuild.

To add a new built-in **action** or **toggle**, register it in `actions()` /
`toggles()` in `menu_config.cpp` and reference it by name from a mod's `action` /
`toggle` field.

---

## How pause buttons are wired

Pause buttons are placed by `rs64_pause_install` (`menu_config.cpp`), hooked at
`initOrUpdatePauseScreenDim` and run **only while the menu is open**
(`D_8010CA20 == 5`) so it never touches the arrays during level-load/briefing
(which starves the audio scheduler). Each button targets a slot in a
`PauseMenuStuff` array:

- `menu: "pause"` → the root objectives terminator slot @ 0x80109E1C.
- `menu: "pause_game_settings"` → the GAME SETTINGS spacer @ 0x80109E54.

Each installed button keeps a **valid `textId`** (an out-of-range sentinel makes
`getGameOrFrontText` return NULL and the layout `sprintf("%s", NULL)` fault) and is
tagged with a unique sentinel in the unused **`nextMenu`** field, assigned
dynamically from `0xF010` up. `rs64_pause_install` records `sentinel -> behavior`
in a map, and two generic hooks dispatch through it:

- **render** at `runPauseMenuStateMachine` @ 0x800C3550 (`ctx->r20` =
  `&entry.textId`, `nextMenu` at +0x2): calls `rs64_pause_label_for(rdram, marker)`
  and overrides `ctx->r2` with the returned label pointer (live ON/OFF for
  toggles), or leaves it if the marker isn't ours. The pause menu rebuilds text
  each frame, so no front-end-style re-setup is needed.
- **confirm** at `initOrUpdatePauseScreenDim` @ 0x800C2458 (`ctx->r18` = `&entry`,
  `nextMenu` at +0x4): calls `rs64_pause_confirm(rdram, marker)`, which runs the
  button's action or flips its toggle.

Adding a pause button is therefore pure JSON — no new hooks. Phase 1 supports one
button per target slot; more than that needs a relocated `PauseMenuStuff` array
(`current_menu`/base redirected), keeping the visible line count ≤ 16.

---

## Gotchas

- **Front-end text is measured once at setup, not per frame.** Changing a
  `menu_entries[i]` pointer live updates the nav data but **not** the drawn label.
  To change on-screen labels you must re-run `setupMenuData` for the current menu
  (a self-targeting sub-type-1 entry does this) — that is exactly why the YES/NO
  confirm works and why a naive live swap does not.
- **Sub-type range check**: front-end sub-types outside `[1, 26]` are ignored on
  confirm. A data-only new sub-type cannot trigger custom behavior — you must
  hook (or extend the `0x800A66F8` jump table + range check). Use an in-range
  no-op (4 or 20) plus an intercept hook.
- **A leading non-selectable entry hangs the nav loop.** Keep active entries
  contiguous from slot 0 (the confirm YES/NO uses two active entries, not a
  question-line-plus-choices layout).
- **Activation is `strlen`-based**: a front-end entry with an empty/`NULL`
  pointer is skipped. Ensure the label pointer is valid before the common tail
  runs.
- **`menu_entries` has 8 slots**; the main menu uses 2. Do not exceed 8.
- **Pause menu caps at 16 visible lines** and shares a `0x12C` glyph pool.
- **Contiguous pause arrays**: cannot grow in place; relocate for new entries.
- **Never hand-edit `RecompiledFuncs/funcs_*.c`** — regen discards it. Use TOML
  hooks or the `patches/` build.
- **Label strings must be in RDRAM**, not host memory.
