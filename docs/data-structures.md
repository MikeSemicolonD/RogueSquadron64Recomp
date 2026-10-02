# Star Wars: Rogue Squadron 64 — Data Structure Layouts

> Field-offset tables derived from the recompiled C (`RecompiledFuncs/funcs_*.c`,
> literal MIPS→C with `MEM_W`/`MEM_BU`/`MEM_HU` accesses) cross-referenced with the
> sister decomp repo `E:/Projects/rogue_squadron64/docs/` + `symbol_files/`. Companion
> to [game-architecture.md](game-architecture.md), which holds the subsystem/control-flow
> map; this doc holds the struct *memory layouts*.
>
> Confidence is flagged inline: **(confirmed)** = derived from an actual recomp
> `MEM_*` access (cited `funcs_N.c:line`); **(doc)** = asserted only by the sister-repo
> decomp notes; **(guess)** = inferred from math/context, not directly proven.

## Entity / gameplay-object system

The gameplay object model is a three-level chain:

```
gNpcSlotList (0x80130BB0, ptr) → slot[0x800] (8B each)
   slot+0x00 ─→ NPC context  gNpcContextArray[0xC0] (0x3C each)
                  ctx+0x00 = update_func(self, action_code, msg)   ← called per frame
                  ctx+0x04 ─→ per-instance state block (craft state / transient-pool record)
```

`slotDispatcherIter` (0x8003E8DC) walks the slot list and calls each context's
`update_func`. All gameplay objects (NPCs, player craft, effects) share the
**handler protocol** `(self, action_code, msg)` with action codes:
`1`=spawn/init, `2`=tick, `3`/`4`/`12`=teardown, `0x1000`=pos-update.

### NPC slot entry — `*gNpcSlotList + idx*8` (stride 8, 0x800 entries) (confirmed)

| off | size | field | evidence |
|----|----|----|----|
| +0x00 | ptr | NPC context ptr (0 = empty) | funcs_9.c:3795; funcs_8.c:12118 |
| +0x04 | u16 | next-slot chain link index | funcs_8.c:12152 |
| +0x06 | u16 | chain key / free sentinel (0xFFFF = freed) | funcs_8.c:12155,12177 |

Globals: `gNpcSlotList` 0x80130BB0 (ptr), `gNpcSlotListTail` 0x80130BB4,
`gNpcContextArray` 0x80130BB8 (ptr), `gNpcContextArrayPtrs` 0x80130BBC,
transient-pool heads 0x80130BC0/0x80130BC4, free count/next 0x80130BC8 (init 0xBF).

### NPC context — `gNpcContextArray[0xC0]` (stride 0x3C) (mixed)

Allocated by `allocNpcContextArrays` (0x8003FD54): `rs_malloc(0x2D00)` = 0xC0 × 0x3C
contexts + `rs_malloc(0x300)` = 0xC0 × 4 ptr array (funcs_9.c:3093-3165).

| off | size | field | confidence / evidence |
|----|----|----|----|
| +0x00 | ptr | `update_func` — `s32 (*)(self, action_code, msg)` | confirmed funcs_8.c:12148; npc_stuff.md |
| +0x04 | ptr | per-instance data block (pool record / craft state) | confirmed funcs_8.c:12488; funcs_31.c:15587 |
| +0x14 | u16 | flags: bit0=transient-pool select, bit2=skip dispatch, bit3=alive/pending-destroy | confirmed funcs_8.c:12130,12248 |
| +0x18 | u8 | scratch/handle byte from pool node | confirmed funcs_8.c:12440 |
| +0x1A | u8 | re-entrancy counter (++ at entry, −− at exit; 0 ⇒ freeable) | confirmed funcs_8.c:12128,12230 |
| +0x08..+0x1C | — | other fields (+0x08 ptr, +0x0C/+0x10 u32, +0x1C u16[16]) | doc only (npc_stuff.md `D_80130BB8_type`) |
| size | 0x3C | | confirmed (alloc math) |

Transient slot pools (`slotDispatcherInner` 0x8003EA4C): two pool heads
(0x80130BC0 flag-clear, 0x80130BC4 flag-set), each `rs_malloc(0x204C)` with an
internal free-stack (index byte +0x00, 0x40-byte table +0x01, link +0x44/+0x48,
sub-records at `base + tableEntry*0x80/0x100 + 0x4C`). Chosen record → ctx+0x04.

### Player-craft struct — `player_struct` @ 0x80137DB8 (stride 0x2A0) (mixed)

Base = `MEM_W(0x80137DB8)`; per-craft slot = `base + vehicleId*0x2A0`. Names from
sister-repo `docs/mips_to_c/ctx.c` (`player_struct` / `inner_player_struct`),
offsets cross-checked against `playerXwingUpdate` (0x800B5434).

`player_struct` header:

| off | size | field | confidence |
|----|----|----|----|
| +0x000 | u8 | `unk000` slot-active gate | confirmed (func_800FB6C0.c:71) |
| +0x002 | u16 | `unk002` | doc (ctx.c) |
| +0x004 | 0x29C | `inner` (`inner_player_struct`) | doc (ctx.c) |
| size | 0x2A0 | | confirmed (stride math funcs_31.c) |

`inner_player_struct` (base = `player+0x4` = 0x80137DBC):

| off | size | field | confidence / evidence |
|----|----|----|----|
| +0x004 | f32×3 | `posX/posY/posZ` | doc ctx.c; confirmed funcs_31.c:15683 (+0xB8 trunc of pos) |
| +0x010..+0x030 | f32×9 | orientation/velocity scratch | doc ctx.c |
| +0x034 | u16[0x40] | anim/blend table | doc ctx.c |
| +0x0B6 | u16 | `unk0B6` (← self+0x16 on spawn) | confirmed funcs_31.c:15623 |
| +0x0B8 | s32 | int Z / frame (trunc.w.s of pos) | confirmed funcs_31.c:15683 |
| +0x0BC | ptr | craft NPC render-context (malloc 0x1E4, §below) | confirmed funcs_31.c:15790 |
| +0x0C0 | f32 | `currentHealth` | doc ctx.c |
| +0x0C4 | f32 | `maxHealth` | doc ctx.c |
| +0x178 | u32[0x35] | weapon/state block | confirmed funcs_31.c:15735 |
| +0x180 | u16 | flags (0x40 active, 0x100 cleanup-done) — overlaps ctx.c's +0x178 array, treat as flag field | confirmed funcs_31.c:15857 (low-confidence vs ctx.c) |
| +0x290 | u8 | `vehicleId` (PlayerCraft 0..8) | confirmed (func_800FB6C0.c) |
| +0x294 | u32 | `unk294` | confirmed funcs_42.c:26106 |
| size | 0x29C | | doc ctx.c |

`PlayerCraft`: XWING=0, YWING=1, AWING=2, VWING=3, SNOWSPEEDER=4, FALCON=5,
TIEINTER=6, T16=7, KOELSCH=8 (ctx.c:523). Per-craft updaters share the NPC handler
protocol and dispatch action_code via a per-craft jump table.

**Craft NPC render-context** (`allocateAndInitCraftNpcContext` 0x8006BD18,
malloc 0x1E4, pointed to by inner+0xBC): self-referential sub-list heads at +0x30
(→+0x6C), +0x8C (→+0xC8), +0x90 (→+0x178); type const 0x2960001 @ +0x78; count 9 @
+0x80; shadow-texture id keyed by `arg2 & 0xF`; init floats from `D_800A09C0`.

### Actor / effect struct (0x800F subsystem) — polymorphic by type byte +0x112 (mixed)

`dispatchEffectByActorType` (0x800F9A14) reads `MEM_BU(actor, 0x112)` and branches:
type1→0x800F410C, type2→0x800F4E74, type3→0x800F538C, type4→0x800F8398
(funcs_42.c:20553). Type-keyed setters guard on the same byte.

| off | size | field | confidence |
|----|----|----|----|
| +0x00 | f32×3 | XYZ position | note (resolveNpcAttachmentPos) |
| +0x08 | ptr | base-position ref (NPC-attached) / type-2 XYZ | confirmed funcs_42.c:19697 |
| +0x104..+0x10C | bytes/u32/f32 | type-1 params (a/b/c byte, word, intensity) | confirmed funcs_42.c:20712 |
| +0x110 | u16 | flags halfword | note |
| **+0x112** | u8 | **actor type byte (1..4 → executor)** | confirmed funcs_42.c:20553 |
| +0x130 | u16 | NPC slot handle (anchor → getNpcContextByIndex) | note |
| +0x14C..+0x1A7 | mixed | effect intensity, progress channels, cue id, flags | note (project_actor_effect_struct) |

Type-1 extended block is based at `actor+0x8` (ramp current/target at abs +0x50/+0x58,
rate +0x34, anchor +0x2C). See [[project_actor_effect_struct]].

## Global game state

### `gGameSettings` @ 0x80130B40 — `D_80130B40_type`, size 0x30 (confirmed)

Size proven by `resetGameSettingsAndMissionStats` memset of 0x30 (funcs_17.c:5113).

| off | size | field | confidence / evidence |
|----|----|----|----|
| +0x00 | u8 | currentLevel (enum Level) | confirmed funcs_16.c:12037 |
| +0x01 | u8 | vehicleId (enum PlayerCraft) | confirmed funcs_17.c:5961 |
| +0x03 | u8 | secondaryWeapon | confirmed funcs_17.c:5955 |
| +0x05 | u8 | controllerSetting | doc only |
| +0x06 | u8 | languageSelect | doc only |
| +0x07 | char[3] | pilot name (no NUL) | confirmed funcs_17.c:3533 |
| +0x0C | u32 | unlock/config flags (bits 9..22 = level/craft unlocks); high byte +0x0F = display settings | confirmed funcs_17.c:3560,6641 |
| +0x10 | u32 | flags/mute: bit0=hi-res/widescreen, bit2/4/8=music/sfx/speech muted, bit0x20=demo active | confirmed funcs_16.c:12071; funcs_17.c:4972 |
| +0x13 | u8 | sound-resolution settings (high byte of +0x10; bit0x80=voice-tick gate) | confirmed funcs_17.c:5070 |
| +0x14 | u8 | demoId (cycles 0..5) | confirmed funcs_17.c:7040 |
| +0x15 | u8 | demo wrap counter (cap 4) | confirmed funcs_17.c:7076 |
| +0x16 | u8 | subtitle fallback gate | confirmed main_overlay.txt:1779 |
| +0x18 | u32[2] | **`gActiveCheatFlags`** (= 0x80130B58) — cheat bits live INSIDE gGameSettings | confirmed funcs_20.c:10964 |
| +0x20 | u8 | musicVolume (init 0x7F) | confirmed funcs_17.c:4974 |
| +0x21 | u8 | soundFxVolume | confirmed funcs_17.c:4988 |
| +0x22 | u8 | speechVolume | confirmed funcs_17.c:4990 |
| +0x23 | u8 | Expansion-Pak present flag (rdramSize>4MB) / time-limit override | confirmed funcs_17.c:5200 |
| +0x28 | u32 | `gNabooStarfighterCheatHash` (overlaps unk28) | confirmed funcs_20.c:10988 |

### `gMissionState` @ 0x80130B10 — `D_80130B10_type`, size 0x28 (confirmed)

Size proven by memset of 0x28 (funcs_17.c:5122).

| off | size | field | confidence / evidence |
|----|----|----|----|
| +0x00 | u8 | numLives (default 3) | confirmed funcs_17.c:6038 |
| +0x01 | u8 | playerRank / save-status | confirmed funcs_17.c:3554 |
| +0x02 | u8 | secondaryWeapon | confirmed funcs_17.c:5959 |
| +0x03 | u8 | secondaryWeaponMax | confirmed funcs_17.c:5975 |
| +0x04 | u8 | **speechResponseModeRequest** (1 or 2 when speech busy) | confirmed funcs_42.c:26816 |
| +0x05 | u8 | completedObjectiveFlags | confirmed funcs_17.c:5993 |
| +0x06 | u8 | hiddenObjectiveFlags | confirmed funcs_17.c:5995 |
| +0x07 | u8 | numMissionObjectives | confirmed funcs_17.c:5997 |
| +0x08 | u32 | activeUnlockFlags (bits 9..22 mirror gGameSettings+0xC) | confirmed funcs_17.c:6630 |
| +0x0C | u8[0x13] | medalPerLevel[19] | doc only |
| +0x24 | u8 | accountNumber | confirmed funcs_17.c:3868 |

**Gap bytes 0x80130B38/39/3A** are *standalone scene-state bytes*, not part of either
struct. `0x80130B39` is a very hot per-frame scene/cinematic branch flag (read across
funcs_10..20). `0x80130B38` matches the cutscene/scene gate in
cinematic_overlay.txt:25 (processCutsceneActions).

Caveat: `initializeNumLives`/`applySaveSlotToMissionState` fill **256 bytes** from
the base with 3, overrunning both structs before later re-init — relevant if treating
either struct as isolated.

### Cheat system — `gCheatCodeCrc32Table` @ 0x800A0ED0, flags @ 0x80130B58 (confirmed)

`applyCheatCodeFromInput` (0x80082270): `rs_crc32(make_crc32_lut(), code, 8)` with seed
0xFAC5FAC5, linear-scans the 29-entry table at 0x800A0ED0; on `table[i]==crc` sets bit
`(1<<i)` in `gGameSettings+0x18` (0x80130B58). Naboo bit 0x10 pre-set → calls
`load_naboo_starfighter`, stashes CRC at +0x28.

Bit → cheat (full table in `docs/cheat_codes/cheat_codes.md` in the sister decomp repo, not this one; entry N @ 0x800A0ED0+N*4):
bit0 GAMEFLO! (unlock missions), 0x2 KOELSCH, 0x4 FARMBOY (Falcon), 0x8 TIEDUP (TIE),
0x10 HALIFAX? (Naboo step1), 0x20 BLAMEUS, 0x40 ACE (hard), 0x80 ICHHELD, 0x100 PSYLOCK,
0x200 WUTZI, 0x400 BERGLOWE, 0x800 TIECK, 0x1000 RUDIBUBI, 0x2000 CHIPPIE, 0x4000 TOBIASS,
0x8000 SIRHISS, 0x10000 HARDROCK, 0x20000 THBPILOT, 0x40000 FLYDODGE, 0x80000 MAESTRO
(music menu), 0x100000 DIRECTOR (cutscenes), 0x200000 CREDITS, 0x400000 IGIVEUP (∞ lives),
0x800000 RADAR, 0x1000000 TOUGHGUY, 0x2000000 CHICKEN (AT-ST), 0x4000000 WOMPRAT! (Tatooine
race), 0x8000000 WOISTHAN (Trench bonus), 0x10000000 DEADDACK (Hoth bonus). `!YNGWIE!` is
the Naboo HMT decryption key (not a flag bit).

### The "per-scene/mission runtime struct" IS `gPlayers` / `player_struct` (correction)

The pointer passed in `$a0` to `destroySceneNpcsAndUnlink`, `cancelSceneQueueHandle`,
`initPlayerCraftMovementMode` etc. is **an element of the static BSS array `gPlayers`
@ 0x80137DB8 (stride/size 0x2A0)** — the *same record* documented above as
`player_struct`. There is **no per-mission malloc**; `initPlayerStructs` (0x800555F0,
funcs_12.c:1753, stride `addiu 0x2A0` funcs_12.c:1791) zeroes it in place and `initMission`
(0x800FA250) calls it. The old "handle at 0x800CC858 / size ~0x250" note is wrong:
0x800CC858 is the FormatMessageWorker registry handle (unrelated), and the size is 0x2A0
(main_overlay.txt:2517). `initPlayerCraftMovementMode` re-derives the element from
`+0xB8` (vehicle type index) via the `×0x2A0` multiply chain over base 0x80137DB8
(funcs_30.c:20964), proving the scene-lifecycle fields and the craft-state fields live in
one 0x2A0 record.

Scene-lifecycle fields (absolute outer offsets; the `inner_player_struct` craft-state
fields above sit at +0x4):

| off | size | field | evidence |
|----|----|----|----|
| +0x00 | u8 | per-element active marker (=1 for player[0]) | confirmed funcs_12.c:1785 |
| +0x02 | u16 | per-player state/team/status flags | confirmed (getPlayerField2) |
| +0x0C | ptr | child HUD/2D-position sub-struct (+0x18 x, +0x1C y) | confirmed read funcs_30.c:23028 (writer not found) |
| +0xB8 | u32 | craft/vehicle TYPE INDEX (re-derives the element; indexes craft-stat tables) | confirmed funcs_30.c:20973 |
| +0xBB | u8 | rumble / controller-Pak slot index | confirmed funcs_28.c:33841 |
| +0x184 | u16 | craft flags (bit 0x40 tested) | confirmed funcs_30.c:20994 |
| +0x1C0 | u32 | **rumble effect handle** (−1 = none) — NOT a queue handle | confirmed funcs_30.c:23583; init −1 funcs_28.c:33905 |
| +0x1C4 | u32 | rumble param (last intensity) | confirmed funcs_30.c:23402 |
| +0x1C8 | u32 | movement mode (set to 2) | confirmed funcs_30.c:21010 |
| +0x1CC | u16 | NPC slot-chain head (sentinel 0xFFFF) | confirmed funcs_31.c:18 |
| +0x208 | listnode | Factor5 intrusive list node (→factor5RemoveListNode) | confirmed funcs_31.c:92 |
| +0x24C | u8 | active flag (gates +0x208 unlink) | confirmed funcs_31.c:78 |
| +0x254 | sub | inner vec4-quad sub-struct (clearVec4QuadStruct) | confirmed funcs_12.c:1772 |
| size | 0x2A0 | | confirmed funcs_12.c:1791; main_overlay.txt:2517 |

The +0x1CC chain indexes the *global* NPC pools (`gNpcSlotList`/`gNpcContextArray`), not
local storage.

## HUD / menu

### `hud_sub_struct` (0x30) — repeated HUD element (mixed)

| off | size | field | confidence |
|----|----|----|----|
| +0x00 | ptr | next | confirmed func_800FBE20.c:62 |
| +0x04 | ptr | prev | confirmed |
| +0x08 | u16 | texture_count | confirmed |
| +0x0C | ptr | texture_id_pointer (u16*) | confirmed |
| +0x10 | ptr | xy_offset_pointer | doc |
| +0x14 | u32 | flags | confirmed |
| +0x18 | f32 | xpos | doc |
| +0x1C | f32 | ypos | doc |
| +0x20 | f32 | zero/zpos | confirmed |
| +0x24 | f32 | width_scale | doc |
| +0x28 | f32 | height_scale | doc |
| +0x2C | rgba | {r,g,b,a} | confirmed |

### `hud_struct` — `D_8010CA30[2]` (0x278 each, double-buffered) (mixed)

Secondary-weapon block at +0x00..+0x06, `crosshairOnOff` @ +0x05, `texture_ids[10]`
(u16, indices into `D_8011A444`) @ +0x0A, `alpha_scaling` (f32) @ +0x24, two crosshair-ring
`hud_sub_struct`s @ +0x28/+0x58, `hud_elements[8]` @ +0x88. Source: `hud_stuff.md`,
written by `configurePlayerSecondaryWeaponHud` (0x800FBE20). Note: `setHudSecondaryWeaponInfo`
(0x800BFDC4) does NOT touch this array — it reads globals 0x80130B12/0x80130B4C and
returns weapon UI codes.

A separate, larger **0xF80 HUD/menu struct** (`initHudStruct`/`handleHUD` 0x800C0084):
`hud_texture_ids[0x1E]` @ 0, 9 named sub-structs @ 0x03C, `menu_elements[16]` @ 0x264,
`current_menu` @ 0xD6A, `secondaryWeaponType/Level` @ 0xF7D/0xF7E (full layout in
hud_stuff.md, doc-only).

### Material → texture lookup chain (confirmed)

```
material id → D_8011A444[id] (4B)  +0x0 material_type (bit0=valid), +0x2 texIdx
           → D_80128F08[texIdx] (0x24B)
```

`D_80128F08` entry (stride 0x24, derived as `texIdx*9<<2`): +0x00 word = RDP image-format
field (feeds `decodeRdpFormatFlags`), +0x08 width, +0x0A height, +0x0C size, +0x0E TLUT idx
(→ `D_80128EFC`), +0x10 texture_data ptr, +0x14 name[16]. Confirmed offsets: format/W/H/
unk0E/data (funcs_6.c:16475-16557); size/name doc-only. `getTextureDataByMaterialId`
(0x800232F8) returns the +0x10 ptr.

### `gCurrentMenuData` @ 0x800CE730 (0xF8) (confirmed)

| off | field | off | field |
|----|----|----|----|
| +0x00 | screen_title (char*) | +0x54 | entry_size_scaler[8] (f32) |
| +0x04 | current_menu | +0x74 | unk74[8] (target menu) |
| +0x05 | back_menu | +0x94 | current_menu_entry |
| +0x08 | menu_entries[8] (char*) | +0x95 | num_menu_entries |
| +0x28 | unk28[8] (per-entry sub-type) | +0x96 | overall_y_offset (s16) |
| +0x30 | entry_xy_offsets[8][2] (s16) | +0x98 | active_entries (bitmask) |
| +0x50 | title_xy_offset[2] (s16) | +0xA0 | highlight_timer (f32) |

`setupMenuData` (0x800BA0F0) is a 13-arm jump table (one per `enum Menu`) that fills
title/entries/sub-types. Offsets confirmed funcs_32.c:5418-5543; field names from menus.md.

## Cinematic / scene graph

### Scene-graph node (confirmed)

Walked by `traverseSceneGraphRecursive` (0x80015548) → `processSceneNode` (0x80014FA0)
→ `submitSceneNodeRender` (0x80010014). Corroborated by main_overlay.txt:1109-1111.

| off | size | field | evidence |
|----|----|----|----|
| +0x00 | ptr | sibling node (0 = end) | funcs_4.c:16652 |
| +0x08 | ptr | render-object | funcs_4.c:15344 |
| +0x0C | ptr | child node (recursed) | funcs_4.c:16621 |
| +0x10 | ptr | light/render-data list head (walked via +0x0) | funcs_4.c:16490 |
| +0x1C | 0x30 | embedded 3×4 transform matrix | funcs_4.c:16432 |

Render-object (node+0x08): +0x0C flags (bit0x4 early-out, bit0x8 full render),
+0x2C DL/matrix command word (OR'd 0xC0000000), +0x30 per-frame "processed" stamp
(from frame counter 0x80127894), +0x34 render-entry ptr (queued into render table
0x80116400), +0x38 secondary render-data; +0x3C..+0x58 render scratch sub-fields.

### Cutscene file — `cuts_file_constant` @ `*gCurrentCutsceneFile` (0x800B1904 is a ptr) (mixed)

| off | field | confidence |
|----|----|----|
| +0x0044 | cutscene length / frame count (int→float divisor) | confirmed funcs_27.c:8062 |
| +0x004A | active count for cuts_13D8[] | confirmed funcs_27.c:7698 |
| +0x004E | active count for cuts_0058[] (cap 200) | confirmed funcs_27.c:15993 |
| +0x0050 | f32 playback time-scale divisor | confirmed funcs_27.c:6415 |
| +0x0054 | f32 playback time multiplier | confirmed funcs_27.c:6458 |
| +0x0058 | cuts_0058_type[200] (stride 0x18) — event/cue list | confirmed |
| +0x1318 | cuts_1318_type[6] (stride 0x20) — vec3f-index sets | confirmed |
| +0x13D8 | cuts_13D8_type[60] (stride 0x4C) — named records | confirmed |
| +0x25AC | Vec3f[] (stride 12) — position list | confirmed funcs_27.c:10278 |

`cuts_0058_type` entry (0x18): +0x00 sub_type (`<0x17`, 23-case jump table @0x800A5340),
+0x04 trigger time, +0x08 param (speech clip id), +0x0C/+0x10 enable flags, +0x0F volume
(0x7F), +0x13 pan (0x40), +0x14 routing slot (`&0x3`). Dispatched by
`spawnCutsceneEffectsAndCues` (0x800A89B0). `cuts_1318_type` (0x20) = `u32 vec3f_indices[8]`
resolving into the +0x25AC Vec3f list (cinematicLoopBody uses only the first active of 6).

### Cinematic timing state (confirmed)

`D_800B0934` holds the literal base 0x800B0000; the live timing fields are a **sub-object**
at `*(D_800B0934 + 0xC)` (`getCinematicStateSubObject` 0x800AF65C). `cinematicComputeDt`
(0x800AF360) produces the `dt` arg; it does not touch this struct.

Timing sub-object: +0x14 flags (init OR 0x2400003), +0x18..+0x28 screen-rect floats (origin/
extent, guess), +0x2F live stage timer / phase accumulator (bit0x80 = wrapped, 0xFF = saturated).
Siblings: `D_800B0938` u8 stage counter (0→6, ends at 7 = 7 stages), `D_800B1EE8` f32 per-frame
increment factor (= dt·const). Advance math: `phase = D_800B1EE8·dt + timer`; if `phase>C_5D70`
→ timer=0xFF, stage++; else if `phase≥C_5D74` → timer = trunc(phase−C_5D74)|0x80; else
trunc(phase). Interp ratio ≈ `1 − timer/255`.

### `gActiveSlots` @ 0x80139560 (correction)

0x80130BB0 is **`gNpcSlotList`** (a ptr), NOT a 6-slot effect table. The "6 cinematic
effect slots" come from `gActiveSlots` @ 0x80139560 (u16[32], chain-head indices, 0xFFFF=empty),
allocated 32× by `allocAllInitialNpcSlots` (funcs_13.c:9008). `cinematicSlotBatchDispatch`
(0x800A70E4) reads elements [3,4,6,7,8,10] and calls `slotDispatcherIter(a1=4)` per slot.

## Save data

EEPROM is 256 bytes: a 0x20 `save_file_header` + 2 × 0xC8 `save_data_body` (dual copy)
+ padding. Body holds an adler32-checksummed `gSaveDataBody`. Verified against
`initSaveData` (0x80006338), `saveLoadDispatcher` (0x80006798) and the account helpers.

### `gSaveDataBody` / `D_8013A5C0_type` @ 0x8013A5C0 (0xB0) (mixed)

Size 0xB0 confirmed by memset (funcs_17.c:3164). Settings header (+0x00..+0x13) and tail
(+0xA0..) are copied opaquely (doc names). Confirmed body:

| off | size | field | evidence |
|----|----|----|----|
| +0x14 | 0x3C | `accounts[3]` (account_data, 0x14 each) | confirmed funcs_17.c:6703 (`getAccountDataPtr` base 0x8013A5D4, stride 0x14) |
| +0x50 | u8 | active-account bitmask (`1<<slot`) | confirmed funcs_17.c:6806 |
| +0x51 | u8 | current/selected account id | confirmed funcs_17.c:6810 |
| +0x52 | 0x50 | `highscores[10]` (EliteRogueData, 0x8 each) | confirmed funcs_17.c:7694 |

### `account_data` (0x14) @ body+0x14 (mixed)

| off | size | field | evidence |
|----|----|----|----|
| +0x00 | char[4] | name (null-term) | confirmed funcs_17.c:6086 |
| +0x05 | u8 | current_level (clamp ≤0x10) | confirmed funcs_17.c:6716 |
| +0x06 | u8[5] | level_medals — 2 bits/level, 19 levels (`parseAccountDataBytes`) | confirmed funcs_17.c:6174 |
| +0x0C | u32 | unlock word (`&0x7FFE00`) — overlaps doc's separate +0x08 | confirmed funcs_17.c:6111 |
| +0x10 | u8 | accountNumber | confirmed funcs_17.c:6113 |

### `EliteRogueData` (0x8) @ body+0x52 (mixed)

+0x00 name[3], +0x03 current_level, +0x04 u16 medal bitfield (bits 0-4 bronze ×1, 5-9
silver ×2, 10-14 gold ×4 → score `(v&0x1F)+((v>>4)&0x3E)+((v>>8)&0x7C)`), +0x06
accountNumber. Confirmed funcs_17.c:7698-7715.

### `save_data_body` (0xC8) — EEPROM record (confirmed, with checksum-order correction)

| off | size | field | evidence |
|----|----|----|----|
| +0x00 | u32 | adler32 of the **0xB0 body** (NOT the unk08 block — doc has these swapped) | confirmed funcs_2.c:2884 |
| +0x04 | u32 | adler32 of the **0x10 unk08 block** | confirmed funcs_2.c:2852 |
| +0x08 | 0x10 | unk08[4] extra block | confirmed funcs_2.c:2806 |
| +0x18 | 0xB0 | body (`D_8013A5C0_type`) | confirmed funcs_2.c:2792 |

EEPROM map: 0x000 header (0x20), 0x020 body copy 0, 0x0E8 body copy 1, per-copy offset
`i*(0xB0+0x18)+0x20`. `save_file_header` = two adler32+payload halves with magics
`GAME`/`GSYS`/`0xF5F5F5F5`/`0xAAAAAAAA`/`0x55555555`. MEDAL_TYPE: NO_MEDAL/BRONZE/SILVER/GOLD
(2-bit). All confirmed in funcs_2.c.

## Mission objectives

Three global tracking arrays + a check-handle table + a per-level vtable. Derived from
`checkObjectiveHandles` (funcs_42.c:21662) and the lv0 implementations.

| global | addr | element | evidence |
|----|----|----|----|
| `gObjectiveBooleans` | 0x801388A0 | u8[128], 1-based (`[idx-1]`) | confirmed funcs_42.c:21746 |
| `gObjectiveCounts` | 0x80138060 | u32[128], 1-based | confirmed funcs_42.c:21787 |
| `gObjectiveTimers` | 0x8010C9F8 | f32[8] | doc + funcs_43.c:14 |

### `simpleCheckHandle` — `simpleCheckHandles[0x30]` @ 0x8010C6E0 (stride 0x10) (confirmed)

| off | size | field | evidence |
|----|----|----|----|
| +0x00 | u32 | `handle` (fn ptr; jalr when check passes) | confirmed funcs_42.c:21882 |
| +0x04 | union | u8 objectiveBooleanIndex / f32 timer | confirmed funcs_42.c:21742,21847 |
| +0x05 | u8 | objectiveBooleanValue | confirmed funcs_42.c:21754 |
| +0x06 | u8 | objectiveCountIndex | confirmed funcs_42.c:21767 |
| +0x07 | u8 | count-progress cursor | confirmed funcs_42.c:21777 |
| +0x08 | u32 | objectiveCountValue (target) | confirmed funcs_42.c:21793 |
| +0x0C | u8 | checkType (0=count/bool, 1=timer) | confirmed funcs_42.c:21717 |
| +0x0D | u8 | handler-fired latch (0 ⇒ fire) | confirmed funcs_42.c:21872 |
| +0x0E | u8 | active (1 = slot in use) | confirmed funcs_42.c:21703 |

Built by `addBooleanCountHandle` (funcs_42.c:21963) / `addTimerHandle` (funcs_42.c:22050);
cleared by `initializeObjectiveHandles` (funcs_42.c:21932).

### `gMissionObjectiveVtable` @ 0x8010A450 (stride 0x10, 21 levels) (confirmed)

Indexed by current level id (`MEM_W(0x80130B70)<<4`). Four slots:
+0x0 `initializeObjectiveTracking`, +0x4 per-frame objective tick (`lvN_objectiveSlot1`),
+0x8 `calculateFriendliesSaved`, +0xC `checkComplexObjectives`. Confirmed funcs_35.c:6061-6194;
`lv0_calculateFriendliesSaved` computes `0x33 − (count[2]+count[3])` (funcs_43.c:17642).

## DAT-file runtime records

All DAT items share a 0x10-byte header; the discriminator is `sub_type` (u16 @+0x00,
`DAT_SUBTYPE` enum). The four "parsers" are sub_type dispatchers over a variable-size
record; fields above +0x10 are sub_type-specific overlays (flag as guess unless proven).
Common header: +0x00 sub_type, +0x06/+0x34 runtime fields (init 0xFFFF), +0x08 size/next,
+0x0C offset→item_name (fixed to ptr), +0x10 f32×3 position.

- **Type 0 (spawn/props)** confirmed extras: +0x4C flags (bit0x2), +0x54 runtime ptr (init
  0x80000000), +0x5C offset→ptr, +0x8A variant/craft id (funcs_10.c:3633-3675, funcs_15.c:3152).
- **Type 6 (LOD/MUSICRNG)**: sub_type 0x04/0x05/0x10 select; +0x8A variant (funcs_15.c:3041).
- **Type 7 (event triggers)**: +0x04 flags (bit0x4000 set on match), +0x34/+0x44 counts,
  +0x58/+0x68 offset→child, +0x80 offset→name (funcs_28.c:2545-2682).

DAT header global `D_801375D8`; 0x48-byte file header (count@+0, offset@+4 per type) parsed
by `load_dat_file_assets` (funcs_15.c:1110).

## MusyX runtime state

Three live in-RAM structs (distinct from the SND file format). The CPU `tickAudioChannel`
runs an 81-entry SoundMacro opcode dispatch — the engine `tools/musyx_vm.py` ports.

### Voice slot — `*0x801496F8 + idx*0x88` (confirmed)

Synth voice. Key fields: +0x00 active flag, +0x01 allocated flag, +0x04 pitch (0x1000=unity,
≤0x2000), +0x08 f64 position accumulator, +0x28 ADSR attack, +0x2A decay, +0x2C f32 sustain,
+0x30 release, +0x34 sample-desc ptr, +0x38/+0x3C/+0x40 loop start/end/len, +0x58..+0x6C env
ramp (level/incr/target/phase), +0x6D note number. Confirmed funcs_24.c:6955-7354. ADSR maps
1:1 to musyx_vm.py attack/decay/sustain/release.

### Song slot — `*0x80149A00 + handle*0xEF8` (8 slots) (mixed)

(Base is 0x80149A00 = `lui 0x8015; -0x6600`; an agent mis-subtracted this to "0x8014A900" —
the existing value stands.) Confirmed fields: +0x004 group-data ptr, +0x10C control-header ptr,
+0x110/+0x114 region cursors, +0x120 region/time cursor, +0x12C event-stream read ptr,
+0x134 event-time accumulator, +0x528 voice-assignment table (u8[0x40], init 0xFF), per-channel
0x24-stride records at +0x56C (+0x570 stream ptr, +0x57C pitch/vol 0x2000, +0x588 program),
control block +0xEC0 (auto/loop), +0xEC2 master volume (0x100), +0xEC5 quiesce flag, +0xEC6
region-loop counter, +0xEF4 state flag. Confirmed funcs_25.c:2606-5548. `sgid@+0x14`/`sid@+0x16`
and cmd-flag@+0xEEE were NOT confirmed on the slot base (they are caller-command-struct fields) —
**guess**.

### Audio channel — `D_8013D880 + idx*0x17C` (confirmed)

SoundMacro channel. +0x00 macro base ptr (active gate), +0x04 macro cursor (8-byte cmds),
+0x08/+0x0C GoSub continuation, +0x24 flags, +0x28 base pitch, +0x2E pan, +0x30/+0x34 volume
current/target, +0x40 wait countdown, +0x4E program id, +0x6D-region note, +0xC0..+0x150 nine
modulator/envelope records (stride 0x12). Op = `MEM_W(cursor,0)&0x7F`, table @0x8003DBF0.
Confirmed funcs_23.c:2755-5264.

Note: `D_80149710` is a **u16 scalar array** (per-voice active-note count, stride 2), not a
struct array. The per-frame DSP render blocks are two heap arrays (stride 0xA10 and 0x10) at
`*0x80149758`/`*0x8014975C`, capped (~20) by the count at 0x8014974E (funcs_23.c:11951,
funcs_24.c:4217).

## Actor / effect struct — correction to progress-channel fields

Re-derived from `funcs_41.c` (most field accesses) + the executors. Embedded record (≥0x1A8),
no standalone allocator — populated in place via handler tables. Corrections to older notes:
**+0x1A4 flag mask is `0xFF00FF00`** (not 0xFF00FF — zero hits in the recomp); **+0x198 is a
u16** NPC/cue handle (not 32-bit). Confirmed: +0x112 type byte, +0x14C f32 intensity, +0x158/
+0x161 progress channel mode-0, +0x184/+0x18D mode-1, +0x1A5 secondary-init flag, +0x1A7
render-init flag (funcs_41.c:5510-10870, funcs_42.c:20554).

## HOB model / HMT material (in-RAM, post-fixup)

The HOB loader (`load_hmt_and_hob` 0x8005645C) DMAs the model and converts file offsets to
pointers in place. The scene-graph nodes (above) reference these. Note the two fixup
functions are *swapped* vs older naming: `meshdef1_offset_convert` (0x800587F0) is the
leaf/facegroup fixer; `meshdef0_offset_convert` (0x80058948) recurses meshdef0 children.

- **object_entry** (size 0x74): name[16]@0, **meshdef0 ptr@+0x18** (in-RAM; file offset is
  +0x10), prelude@+0x1C, meshdef1-prelude[4]@+0x20, material/name index table@+0x34 (used for
  face material resolution), global-table ptr@+0x44, flags@+0x40. Confirmed funcs_12.c:5055-5171.
  Registered in a hash collection (`hobObjectListItem` 0xC: nextIdx@0, object*@4, hobfile*@8;
  hash = sum(name) % 25).
- **meshdef0 node** (0x4C, linked list): next@0, prev@4, child0@8, child1@0xC (recursed),
  **meshdef1 ptr@+0x10**, transform/AABB floats @+0x14, flags@+0x4C (bit0/bit1), vec3@+0x50;
  embedded Vec4-quad @+0x1C. Confirmed funcs_12.c:10665-10884.
- **meshdef1 node** (0x5C): end_of_facegroup@−0x4, next@0, prev@4, flags@+0xC (OR 0x100008),
  **facegroup ptr@+0x20**, vertex-list ptrs@+0x24/+0x28. Confirmed funcs_12.c:10476-10662.
- **facegroup** (0x10, linked list): next@0, prev@4, **face ptr@+0x8**, **face_count@+0xC**.
  Confirmed funcs_12.c:10549-10630.
- **face** (variable stride): flags@+0x00 (bit0x10 = per-vertex colors), packed bitfield@+0x04
  (**stride = `(w4 & 0x3F000000)>>22`**, vertex-color offset = `(w4>>10)&0xFC`), **material
  index@+0x08** (rewritten in place to `resolved_index<<16` via object+0x34 table). Then vertex
  indices/colors/texcoords. Confirmed funcs_12.c:10597-11232. Vertex = 8B (x,y,z s16 + pad, doc).
- **HMT material_entry** (file stride 0x24): material_type/flags@+0x0 (bit0=has texture),
  texture_index@+0x2, name[16]@+0x14. Confirmed funcs_12.c:4700-4758. Parsed into a runtime
  16-byte row (`parseHmtMaterials` 0x80022A00): flags@0 (bit0 textured), texture_id@+2 (0xFFFF=none).
- **HMT texture_entry** (stride 0x34) — all confirmed funcs_6.c:15091-15141: pixel_offset[8]@0,
  palette_offset@+0x20, name_offset@+0x24, W@+0x28, H@+0x2A, always_1@+0x2C, bit_depth@+0x2D,
  flags_type@+0x2E (**image_type = flags_type & 0xF**, matches rerogue's +0x2E enum and the
  `D_80128F08` resolver's format field), pixel data in-RAM at entry+0x30.

## Render submission

### Material cache pool (header @ 0x80128EF4; 512 × 0x18 nodes) (confirmed)

`createMaterialPool` (0x80021F38) mallocs 0x3000 = 512 × 0x18. Header globals: 0x80128EF0 hash
size, 0x80128EF4 pool base, 0x80128EF8 free-list head, 0x80128F00 hash-table base, 0x80128F08
global texture descriptor table (0x24 stride; the material→texture chain's target).

Node (0x18): next@+0x00 (hash chain / free next), prev@+0x04, age/last-used@+0x08 (0 on hit →
LRU), material key@+0x0A, render-state bytes@+0x0C/+0x0D, format/key byte@+0x0E, descriptor
class@+0x10 (`a2&0x7`), cached DL buffer ptr@+0x14. Confirmed funcs_6.c:12955-14197. Miss →
`allocOrEvictMaterialNode` (pop free list, else evict lowest-age) → `emitMaterialRenderStateDL`
(builds SETTILE/LOADTILE from the texture descriptor).

### Render-pass array @ 0x80116400 + per-frame mesh-batch queue (confirmed)

Pass array: stride 0x0C, indexed by a frame pass counter @0x80128C6C; entry+0x00 = per-pass DL
write cursor (the pass DL buffer is filled with GBI words: PIPESYNC, viewport@+0x24, scissor).
Confirmed funcs_4.c:21027-21110.

Scene nodes with flag bit0x8 (lit) pull a mesh-draw-batch node from the free list @0x801163B0
(alloc cursor @0x801163FC); node+0x34 holds the queued batch ref. `submitSceneNodeRender`
(0x80010014) exports node+0x3C..+0x58 (mesh-batch list heads) to render globals
(0x8012A4AC..0x8012A4C4); `submitGfxFrame` drains them. Batch/free node = {next@0, prev@4}.
Confirmed funcs_4.c:1824-2085, funcs_3.c:2320-2440. (`setupCameraMatrices` is 0x80016C44, NOT
0x80010014; it operates on a camera struct: viewport W/H@+0x10/+0x14, fov@+0x20, near@+0x24,
far@+0x28.)

### Buffer arbiter slot arrays — ADDRESS CORRECTION

The 6-state arbiter arrays are at **0x80128E98 / 0x80128EAA**, not 0x80138E98/EAA (the recomp
reaches them via `lui 0x8013, -0x7168/-0x7156`, underflowing into the 0x8012 bank; host
`rt64_render_context.cpp` reads `STATE_BASE = 0x128EAA`). Layout: fb-target ptr array @0x80128E98 (stride
4), state byte array @0x80128EAA (0=free, 1=alloc/in-flight, 2=ready), slot count @0x80128EAD,
secondary record-ptr array @0x80128EC0, active index @0x80128D2C (XOR 1 = inactive). `submitGfxFrame`
populates a standard OSTask (data_ptr@0x800377D0, data_size@0x800377D4, ucode@0x800377D8) and
osSendMesg's it to the SP scheduler. Confirmed funcs_5.c:5449-5719, funcs_4.c:29353.

## Text / subtitle / HMP

### Text subsystem (mixed)

`txtFileHeader` @0x80138E60 (file header, 0x1C): language_count@0, string_count@+0x2,
language_offset[]@+0x4. String lookup (`getGameOrFrontText` 0x8005589C): bounds-check id <
string_count, `base = *0x8009ECA0`, `return base + MEM_HU(base + id*2)` (u16 offset table).
Text is XOR-decrypted with a rolling key seeded 0xF5 (`key=0xF5; b^=key; key^=b`); voice text
(`getVoiceText` 0x80055978) re-seeds per string + forces uppercase. Confirmed funcs_12.c:2026-2433.

### Subtitle / speech queue (mixed)

`subtitleSlots` @0x80139BB0 = 16 slots × 0x0C: timer/display-time(f32)@0, voiceId/clip@+0x4,
flags@+0x6, priority@+0x8. Queue globals: head@0x80139C70, tail@0x80139C71, available@0x80139C72
(init 16), voice-handle array @0x80139B80 (u32[8], −1 = empty). `voiceIdtoTextIdMap` @0x8009FFE0
(u16[756]) maps voiceId→textId. Speech file: numSpeechSamples@0x80154670,
speechSampleOffsets@0x8015467C (u32: top byte = type, low 24 = offset). Confirmed funcs_15.c:8944-9167.
`drawSubtitleText` (0x800159B4 / funcs_4.c:17135) is the glyph rasterizer (its arg is a glyph-layout
object, not a queue slot).

### HMP terrain (mixed)

File header `hmp_header` (0x28): height_scale@+0x10, tile_count@+0x18, tile_offset@+0x1C
(→ptr), lighting_offset@+0x20 (→ptr), width@+0x24, height@+0x26. Runtime descriptor @0x80136DC0:
tile-index array@+0x00 (filebuf+0x28), tile table@+0x04, per-column buffer@+0x08 (malloc
width*height u16, init 0xFFFF), scaled height_scale@+0x10, world extents@+0x24/+0x30, grid
width@+0x38, height@+0x3A. Per-tile `hmp_tile` (stride 0x1E): texmap_idx@0, flags@+0x2 (bit0=steep),
min/max height@+0x3/+0x4 (s8), **height_values[25]@+0x5** (5×5 signed grid). Tile-index entries
are u16, top 3 bits = flags, mask `&0x1FFF`. Confirmed funcs_9.c:15409-15848.

## Texture / image decode (runtime tables)

The face→texture resolution chain and its backing tables, fully derived. All four are
**pointer globals** holding heap arrays:

- `D_80128F08` (0x80128F08) → texture descriptor array (0x24/entry): +0x00 u16 RDP fmt/size
  (also occupied flag), +0x02 bit_depth, +0x03 flags, +0x04 transparency word, +0x08 W,
  +0x0A H, +0x0C decoded-size, +0x0E TLUT idx (0xFFFF=none), +0x10 pixel-data ptr, +0x14
  name[16]. Confirmed funcs_6.c:7180-7387, 16545-16558.
- `D_80128EFC` (0x80128EFC) → TLUT/palette ptr array (s32/entry; converted-palette RDRAM ptrs);
  count @0x80128F04; alloc 0x10 (CI4) or 0x100 (CI8) entries × 2B.
- `D_8011A444` → material→texture index rows (short[2]/material): +0x00 flags (bit0=has texture),
  +0x02 texture_id (→D_80128F08). Confirmed funcs_6.c:15234-15248, 16535-16544.
- `D_801163B4` → 16-byte material rows (misc_float/one/zero/0x0A000000 from material_entry);
  `D_801143AC` → material name[16] rows. Confirmed funcs_6.c:15174-15224.

Chain (`getTextureDataByMaterialId` 0x800232F8): `face.material_id` → `D_8011A444[id].texture_id`
→ `D_80128F08[texture_id].pixel_data`. `parseImageFile` (0x8001EB24) dispatches on `image_type =
flags_type & 0xF` (`<6`, jump table): 0=CI4, 1=CI8, 2=RGBA5551(ARGB1555), 3=RGBA32, 4=greyscale 4/8,
5=greyscale 16; upper flag bits (0x8000 alpha-embedded, 0x4000 color-key, 0x800/0x400 aux plane,
0x20 palette-extra) select sub-paths. Confirmed funcs_6.c:4477-4519, 7276-7368.

## Camera + matrix

### Camera struct (arg to `setupCameraMatrices` 0x80016C44) — size 0x5C (confirmed)

| off | size | field | evidence |
|----|----|----|----|
| +0x10 | s32 | viewport width (0x140) | confirmed funcs_4.c:20500 |
| +0x14 | s32 | viewport height (0xE0) | confirmed funcs_4.c:20506 |
| +0x18 | s32 | viewport center X (0xA0) | confirmed funcs_4.c:20691 |
| +0x1C | s32 | viewport center Y (0x70) | confirmed funcs_4.c:20697 |
| +0x20 | f32 | FOV (deg) → guPerspective | confirmed funcs_4.c:20641 |
| +0x24 | f32 | near plane | confirmed funcs_4.c:20593 |
| +0x28 | f32 | far plane | confirmed funcs_4.c:20607 |

There is **no global active camera** (35 callers each pass their own struct) and **no look-at/
target/up in this struct** — `setupCameraMatrices` only builds perspective + viewport; position/
target/up are caller/cutscene state (cutscene cameras use the `cuts_1318_type` vec3f indices).
Defaults from globals 0x8003CB04/08/0C. Confirmed funcs_17.c:3028-3084.

N64 `Mtx` = 0x40 bytes, 4×4 s15.16 split into a 0x20-byte integer half + 0x20-byte fractional
half (`guMtxF2L` scale 65536.0, funcs_8.c:1323). The composed projection×scale L-matrix is written
through the per-frame DL/matrix cursor `*0x8011DC5C`; the render-pass DL buffer node embeds
PIPESYNC/viewport@+0x24/scissor GBI words. Confirmed funcs_4.c:20661-21110.

## Spline path-following

**Basis = uniform Catmull-Rom** (definitive — coefficient fingerprint `0.5·{1,3,5,4,2}`).
`evalCubicSplineAtTime` (0x80018F98 / funcs_4.c:26575): binary-search a knot table, normalize t,
blend 4 control points with `0.5(−t³+2t²−t)` etc. Confirmed funcs_4.c:26729-26834.

- **Spline descriptor** (arg to evaluator): +0x00 count, +0x04 → control-point array (vec3f,
  stride 0xC), +0x08 → knot/time array. No loop flag (caller-side). Confirmed funcs_4.c:26610-26868.
- **DAT Type-3 spline item**: common header + waypoint count@+0x1C, offset→waypoint array@+0x20
  (vec3f each). NPCs link by name string (spline-following @+0x8C, wingmen @+0x94). Confirmed
  dat_file_parse.c:330-351.
- **Spline-walker NPC state** (craft context @NPC+0xC, malloc 0x1E4): speed@+0x10, parametric
  t/phase@+0x14, turn-rate@+0x18, segment countdown@+0x1C, cached pos@+0x60. The path/segment ptrs
  live on the DAT item (passed in), not cached. `advanceNpcOnCurvedPath` (funcs_16.c:14856).
  A transform-actor variant `updatePlayerDeathSpiral` (funcs_30.c:5982) walks t@+0x14, speed@+0xE8.

Open gap: the on-disk waypoint list (plain vec3f) and the in-RAM Catmull-Rom descriptor (count/
points/knots) differ; the conversion step in `load_level_dat` was not located.

## SND file / song-bank (synth source data)

`parseSndFiles` (0x80097518) parses one proj section per call; `loadSndFiles` loads pool/proj/sdir/samp.

- **proj section** (song-bank record; table of ptrs @0x801529D0, count @0x80152A10): +0x00 size,
  +0x04 u16 **sgid**, +0x06 type (0=song), +0x08 common subseg offsets[5] (pool/sdir), +0x1C/+0x20/
  +0x24 song-region/MIDIsetup offsets. Per-song sub-record (subseg7, **stride 0x84**): +0x00 u16
  **sid** (0xFFFF terminates). Confirmed funcs_26.c:628-2072.
- **sdir entry** (0x18): +0x00 id, +0x04 offset into samp_SND, +0x08 root key, +0x0A rate, +0x0C
  num_samples (low 24 bits; top byte flags), +0x10 loopStart, +0x14 loopLen. Samp data = 256B ADPCM
  coefficient book + 0x28-byte frames. Confirmed (cross-ref musyx_n64_decode.py / musyx_group.py).
- **pool sections**: section 0 = SoundMacro VM programs (the 81-op engine in musyx_vm.py), 1 keymaps,
  3 Layers. Sample descriptor → `musyxInitVoiceFromSample`: +0x04 data ptr+flags, +0x08 length,
  +0x0C/+0x10/+0x14 loop start/end/len, +0x18 key (maps 1:1 to the sdir entry).

**Open question resolved**: the song-slot `+0x00` is a **monotonic handle** `(counter+1)&0x7FFFFFFF`
(funcs_25.c:3509), NOT sgid/sid. `processSongByHandle` matches that handle; sgid/sid exist only as
`playSongById` args that resolve the song-bank record (sgid→record+0x4, sid→0x84-entry+0x0) before
`startSongSequence` writes the resolved data/setup ptrs + a fresh handle into the slot.

## DAT file loader + hash map

`load_level_dat` reads the file and fixes offsets→pointers in place. File header (0x48): per-type
{count, offset-list ptr} pairs at +0x00 (Type-0), +0x10 (Type-2), +0x18 (Type-3), +0x30 (Type-6),
+0x38 (Type-7); +0x40/+0x44 = hash-map {size 0x400, 256-word table ptr}. Types 1/4/5 are reserved
padding slots. Per item: `item = offset_list_ptr + offset_list[i]`; name offset@+0xC → ptr;
instance-id@+0x6 reset to 0xFFFF. Confirmed load_level_dat.c + funcs_15.c:1030.

**Hash map** (`getDatItemByName` 0x80047B70): hash = `h=0xFFFF; h = c + h*0x21; … &0xFF`. Table[hash] →
bucket-list ptr; bucket entry (stride 8): item ptr@+0x00, name ptr@+0x04, {0xFFFFFFFF,0xFFFFFFFF}
terminator. Walk comparing the query name to each entry's name ptr. Confirmed funcs_10.c:7419-7564.

**Spline conversion — resolved: there is NO runtime knot-builder.** The Catmull-Rom descriptor's
points and knots arrays ARE the DAT Type-3 item's two offline-authored arrays (waypoints
count@+0x1C/ptr@+0x20; knot/time table count@+0x28/ptr@+0x2C, both fixed to pointers at load). The
descriptor {count@0, points@4, knots@8} is filled by copying that triple — `load_level_dat` does no
malloc/knot computation, and every `evalCubicSplineAtTime` caller forwards a descriptor pointer
unmodified. NPCs reach their spline by name (`SPLINE%d` → hash lookup). Confirmed funcs_4.c:26610-26868,
load_level_dat.c:320-349 (high-confidence inference; the exact item-array→descriptor-slot copy line
was not byte-traced).

## Material → RDP render state (closing the texture-render path)

- **RDP format/size field** (texture descriptor +0x00 u16): `decodeRdpFormatFlags` (0x8001E978) reads
  bits[5:0] (valid 1..6 → enum 0..5); `decodeRdpSizeFlags` (0x8001EA50) reads bits[3:0] (0..5 → 1..6);
  upper bits 0x40..0x8000 = sampler (clamp/mirror/wrap) flags passed through unchanged. The enum→
  concrete `G_IM_FMT`/`G_IM_SIZ` nibble lives in rodata jump tables @0x80000990/0x800009A8 (needs a
  data dump to pin). Confirmed funcs_6.c:4030-4458.
- **`D_801163B4` material-row reader FOUND + its fields are SOFTWARE-T&L LIGHTING, not RDP state**:
  `emitTexturedFaceGeometry` (funcs_3.c:17374, 16856) computes `row = D_801163B4 + face[+0x8].u16<<4`
  and passes it as a **stack arg** (`0x14($sp)` → `$t9`, NOT `$a2`) into `transformAndEmitFaceVertices`
  (0x80018818, funcs_4.c:25129-26336). That function is the **CPU vertex lighting** path and emits **no
  GBI command** — the 4 row fields are scalar multipliers for the per-vertex RGB it computes and writes
  into the vertex color buffer: +0x00 f32 diffuse-mul, +0x04 f32 second light weight (≈1.0), +0x08 f32
  gated emissive add (skipped if 0), +0x0C **byte** transform/lighting scalar (read as `lbu`+`cvt.s.w`,
  not the 0x0A000000 word). Per-vertex color (face flag bit0x10) RGB is multiplied by this computed
  lighting; alpha passes through unlit. Array heap-allocated by `buildAndRegisterDefaultMaterial`
  (`rs_malloc(count<<4)`, funcs_4.c:23371). So the material row drives lighting, not combiner/env/blend.
- **`emitMaterialRenderStateDL`** (0x80021A1C) emits G_SETOTHERMODE_H (0xBA001001) + G_SETOTHERMODE_L
  (when descr&0x4000) + G_SETCOMBINE (combiner word-pair from table @0x80037860, indexed by assembled
  mode bits) + G_TEXTURE (tile = `((descr+0x2)-1 &7)<<11`) + tile-size words from descr W@+0x8/H@+0xA.
  `loadTextureTile` (0x8001FE74) emits G_RDPLOADSYNC/G_SETTIMG/G_RDPTILESYNC/G_SETTILE (+ per-format
  LOADTILE/LOADBLOCK/LOADTLUT). Other-mode LUT @~0x80048860 (128-entry byte). Confirmed funcs_6.c:7827-12774.

## pool_SND instrument data (MusyX synth source)

Pool header = 4 section offsets (0=SoundMacros, 1=Keymaps, 3=Layers); each subsection wrapped
`{size:u32@0, id:u16@+4, pad@+6, data@+8}`.

- **SoundMacro** (section 0): 8-byte commands, `op = w0 & 0x7F`, operand halfword `(w0>>8)&0xFFFF`,
  w1 @+0x4. 81-entry dispatch @0x8003DBF0 in `tickAudioChannel`. Key ops: 0x00 End, 0x04/0x07 Wait
  (0xFFFE/0xFFFF=key-off), 0x06 GoSub (return ptr→ch+0x8/+0xC), 0x0C SetAdsr (table idx = w0>>8),
  0x10 StartSample (**operand is a KEYMAP id, not a direct sample id**), 0x11 KeyOff. Confirmed
  funcs_23.c:3208-3348, funcs_22.c:4896-5076; matches musyx_vm.py.
- **Keymap** (section 1, stride 0xC): descriptor ptr@+0x00, param@+0x04, key/note@+0x08 (bin-search
  key). Runtime table @0x8013B048. Confirmed funcs_22.c:5027, funcs_21.c:3251.
- **Layer** (section 3, stride 0xC): `[count:u32][macro_id@+0, key_lo@+2, key_hi@+3, transpose@+4(s8),
  vol@+5]`. Selected via proj subseg5 when `unk00>>24==0x80` → `layer_id = unk00>>16`. (python-only.)
- **ADSR table** (bin-searched @0x80148C68, stride 8): attack@+0, decay@+2, sustain@+4 (also search
  key, 0..0x1000), release@+6 (≥0x8000 = no release), all u16 LE. Confirmed funcs_22.c:5767-5958,
  funcs_21.c:3461.

## Effects / particle system

- **Slot dispatch**: `cinematicSlotBatchDispatch` (0x800A70E4) calls `slotDispatcherIter` (0x8003E8DC)
  over the slot table @0x80130BB0 (`gNpcSlotList`). Slot entry (8B): handler-node ptr@+0x00,
  next-index@+0x04, group id@+0x06. Handler-node = the actor/effect object: handler fn@+0x00, flags@
  +0x14 (bit3 = skip/destroy-after), reentrancy byte@+0x1A. Confirmed funcs_8.c:12064-12277.
- **Type-1 particle/billboard sub-struct** (actor base, type byte@+0x112): pos vec3@+0xD8/+0xDC/+0xE0,
  source/anim ptr@+0x0C, model-frame ptr@+0x10, initial scale@+0x24, age-increment (1/lifetime)@+0x2C,
  age accumulator@+0x34, normalized progress 0→1@+0x38, current scale@+0x50, target scale@+0x58,
  flipbook frame byte@+0x104, lifetime-remaining@+0x10C. Confirmed funcs_42.c:2514-3078, 19664-19778.
- Billboard facing via `computeBillboardFacingVector` (0x80059E00); a byte-indexed ramp table (in the
  0x800B segment, ~`-0x7760`) maps the flipbook frame to a scale/alpha multiplier (the "16-step ramp" —
  length unconfirmed, **guess**). The explosion-CB / slot-34 `explosionEffectNpcHandler` (~0x8007413C)
  is a distinct per-burst emitter record (does not share the +0x112 layout; field table low-confidence).

## Render rodata tables (decoded)

These tables live in ROM rodata and are fetched at runtime via `MEM_W` (the recomp
does NOT inline them). Dumped/decoded from the built ELF by
`tools/render/decode_rodata_tables.py`; full output in
[factor5-render-rodata.txt](factor5-render-rodata.txt).

### Combiner table @0x80037860 (512 × 8B `gsDPSetCombine{w0,w1}`)

Indexed by the assembled material-mode value `s0` (`s0<<3`). 458 of 512 slots hold the
default `fcffffff/ffffffff`; **54 distinct real recipes**. The mode bits select a
recipe family — `c1` (cycle-1 color mux) is `*SHADE` for mode bit 0x20 clear vs `*PRIM`
for the 0x100-family, and the alpha cycle picks SHADE/PRIM/TEX0/COMBINED. The recurring
recipes (the combiners RT64's native F5 path must honor):

| recipe | combiner | typical use |
|----|----|----|
| `fc127e24` | `(TEX0)*SHADE` color, no alpha mod | flat textured + vertex light (modulate) |
| `fc16fe04 2ffc…` | `(TEX0-TEX1)*LOD_FRAC+TEX0`, cyc1 `*SHADE` | trilinear-mip texture × shade |
| `fcffffff fffcfe7f` | `+TEX0` (pass texel) | unlit textured |
| `fc117e04` | `(TEX0)*TEX1` cyc0, `*SHADE` cyc1 | detail/2-texture × shade |
| `fc11fe23 / fc11ffff` (0x180-fam) | `(TEX0)*PRIM` | textured × primitive color |

Alpha variants append `+SHADE` / `+PRIM` / `+TEX0` to the alpha cycle (the `fffff9fc`,
`fffff638`, `fffff279` low words). Full 54-recipe list in the generated file.

### Other-mode LUT @0x80038860 (128 × u8, index = mode & 0x7F)

Maps the low 7 mode bits → an other-mode class byte (values **0/1/2** only). Pattern:
`01 01 01 01 02 02 02 02 | 01×4 02×4 | 00×16 | 02×4 02×4 01×4 02×4 | 00×16 | …`. Bit 0x4
of the mode toggles 1→2; bits 0x10/0x20 zero it (opaque/no-blend class). The byte feeds
`emitMaterialRenderStateDL`'s `G_SETOTHERMODE_H` selection and is cached at `D_8012A908`.

### Texture format → RDP fmt/siz (loadTextureTile case arms)

The `decodeRdpFormatFlags` jump table @0x80000990 holds **code addresses** (case labels),
not nibbles; the concrete `G_SETTIMG` opcode per format index is emitted in the case arms:

| image_type | format | G_SETTIMG opcode | fmt/siz |
|----|----|----|----|
| 0 CI4 / 1 CI8 | CI | `0xFD50` | CI, palette load |
| 2 RGBA5551 | RGBA16 | `0xFD10` | RGBA / 16b |
| 3 RGBA32 | RGBA32 | `0xFD18` | RGBA / 32b |
| 4 I4/I8 | I | `0xFD90` | I (intensity) |
| 5 grey16 | IA16 | `0xFD70` | IA / 16b |

(Confirmed funcs_6.c:8138-11827; matching `G_SETTILE` 0xF5xx words follow each.)

### Cubic-spline basis coeffs @0x800186F4

`0.5, 1.0, 1.5, 2.0, 2.5` — confirms the **uniform Catmull-Rom** basis used by
`evalCubicSplineAtTime` (the `0.5·{1,3,5,4,2}` polynomial fingerprint).

## See also

- [game-architecture.md](game-architecture.md) — subsystem + control-flow map.
- `E:/Projects/rogue_squadron64/docs/` — per-subsystem decomp notes (the field-name source).
- `E:/Projects/rogue_squadron64/docs/mips_to_c/ctx.c` — m2c struct definitions (`player_struct`, etc).
- `E:/Projects/rogue_squadron64/symbol_files/` — authoritative symbol names.
