# Star Wars: Rogue Squadron 64 — Game Architecture

> **Reference document for the renaming effort.** The project is shelved at
> the attribution screen because of an overlay-dispatch issue (see "Where it
> actually stops" in the [README](../README.md#where-it-actually-stops)). This
> doc remains valuable as a subsystem map for a contributor doing the
> function-renaming work in `RecompiledFuncs/funcs_*.c`.
>
> Source of truth for function names: `E:/Projects/rogue_squadron64/symbol_files/main_overlay.txt`.
> Partial m2c decomp + RE notes live in `E:/Projects/rogue_squadron64/docs/`.

## At a glance

- **Engine**: in-house Factor 5 tooling (SN Systems toolchain, NOT SGI). Custom audio ucode (MusyX-derived). Custom graphics ucode ("F3DFACTOR5") that emits Factor-5-specific opcodes alongside standard F3D.
- **OS**: stock libultra. Threads, message queues, timers, controllers, EEPROM, PI DMA.
- **Layout**: monolithic main code segment + 3 overlays (in-mission, mempak/menu, cinematic).
- **Boot sequence**: entry (0x80000400) → `main` (0x8000161C) → idle thread → `mainBootstrapWorker` (0x80000C68, priority 0xA).
- **Game loop**: `mainGameLoop` (0x8003DFA0) drives per-frame work; entered from `gBootConfig+0x04` ptr after bootstrap.

## Memory map (key globals)

| Addr | Name | Purpose |
|------|------|---------|
| 0x80037560 | `gBootConfig` | 0x50-byte boot config struct; +0x04 = game-loop entry ptr |
| 0x800AF92C | `gCutsceneAssetPaths` | 32 × 0x5C cutscene asset path entries |
| 0x800B1900 | `gCutsceneEntries` | In-mem cutscene entry list (per `unk13D8_active_count`) |
| 0x800B1904 | `gCurrentCutsceneFile` | Active cutscene file ptr (0x25AC bytes + variable Vec3f list) |
| 0x80110741 | (SI busy flag) | Serial Interface in-progress flag |
| 0x80110A80 | `gManifestTable` | Per-segment {manifest_ptr, entry_count} array (data + dbg_data) |
| 0x80110AA0 | (path prefix table) | 80-byte path prefixes for manifest lookup |
| 0x80111240 | (event ring base?) | Adjacent to event ring head/free count |
| 0x80111244 | (event ring head) | Head index for 8-slot × 116B ring at 0x80111D60 |
| 0x80111BC0 | (subscriber table) | 16 × 24B event-subscriber slots |
| 0x80111D60 | (event ring) | 8-slot × 116B event ring buffer |
| 0x80112910 | (callback worker queue) | Queue feeding `func_80007480` worker |
| 0x801128E0 | (callback fn ptr) | Function pointer dispatched by `func_80007480` worker |
| 0x801128D0/D4/D8 | (service registry) | Service-worker linked list head/tail/count |
| 0x80130B40 | `gGameSettings` | 0x30-byte game state struct (level, craft, cheats, volumes, etc) |
| 0x80130B70 | `gCurrentLevel` | Current level enum (0..0x14) |
| 0x80130BB0 | `gNpcSlotList` | 0x800-entry NPC slot list (8-byte entries) |
| 0x80130BB8 | `gNpcContextArray` | 0xC0 × 0x3C-byte NPC contexts, `update_func` at +0x00 |
| 0x80139560 | (cinematic slot indices) | 6 halfwords of active cinematic slot indices |
| 0x8013A5C0 | `gSaveDataBody` | 0xB0-byte in-mem save data body (accounts, scores, settings) |

## Subsystems

### 1. Boot / bootstrap
```
0x80000400 (entry) → 0x8000161C (main)
                       ↓ creates
                     idle_thread (priority 10)
                       ↓ creates
                     mainBootstrapWorker (0x80000C68, priority 0xA)
                       ↓
                     getGameConfig → gBootConfig → loadDebugAssets → initSiQueue → setDisplayMode
                       ↓ jumps to gBootConfig+0x04
                     mainGameLoop (0x8003DFA0)
```

### 2. Service-worker pattern (generic)

Per-thread message-queue workers, registry at 0x801128D0 (linked list).

| Function | Role |
|----------|------|
| `initServiceRegistry` (0x80006C00) | Zeros head/tail/count |
| `registerServiceWorker` (0x80006C28) | (numMsgSlots, stackSize, prio, entryFn) → uniqueId; creates thread + queue |
| `startServiceWorker` (0x80006D9C) | Looks up by id, `osStartThread` |
| `recvServiceMessage` (0x80006F24) | Blocking recv (id, OSMesgQueue\*, mesgOut\*) |
| `tryRecvServiceMessage` (0x80006EC4) | Non-blocking variant |
| `sendServiceMessage` (0x80006F78) | Producer side |

### 3. Save / account subsystem

EEPROM 256 bytes, dual-buffer (`save_data[2]`), adler32 checksums. Layout: 32B header + 2 × 0xC8 bodies + 0x140 zeros padding.

| Function | Role |
|----------|------|
| `saveServiceWorker` (0x8006F2CC) | Service-worker thread (key=auto, slots=4); **stubbed in recomp** |
| `registerSaveService` (0x8006EF98) | `registerServiceWorker` for save thread |
| `triggerSaveMessage` (0x8006F274) | Allocates 20B msg, sends to save worker |
| `saveLoadDispatcher` (0x80006798) | Inside save thread: SAVE=1 (adler32+EEPROM write), LOAD=2 |
| `initSaveData` (0x80006338) | EEPROM sanity check + reinit on mismatch; uses `gamesave_asset` |
| `writeSaveBodyToEeprom` (0x80005B9C) | Writes `save_data_body` (0xC8 bytes) |
| `writeSaveDataBodyToEeprom` (0x80005F18) | Writes `gSaveDataBody` content |
| `copySaveUnk08` (0x80006198) | Copies `save_data_body.unk08[4]` (purpose unclear) |
| `loadAccountDataIntoStruct` (0x8006EAAC) | Copies account bytes from 0x80130B47 → 0x80145AB0 |
| `parseAccountDataBytes` (0x8006EB48) | Unpacks 2-bit medal-per-level data |
| `getAccountDataPtr` (0x8006EE5C) | Returns ptr to account_data from gSaveDataBody |
| `getActiveAccountsBitmask` (0x8006E8AC) | Returns `D_8013A5C0_type.unk50` |
| `getAccountUnk51` (0x8006E8BC) | Returns `D_8013A5C0_type.unk51` |
| `loadDefaultHighScores` (0x8006DCCC) | Loads 10 default high scores from `D_8003CB10` |
| `highScoreBubbleSort` (0x8006DD50) | Bubble sort for high score list |
| `readAccountForSelectionScreen` (0x800B85B4) | Reads account data for account selection |
| `eliteRoguesMenuHandler` (0x800BEA00) | "Elite Rogues" highscore screen handler |
| `postLevelSaveInit` (0x800C06D0) | Post-level save init (medal screen path) |
| `initMemoryPack` (0x80005960) | Initializes the controller mempak |

### 4. DMA / asset loader

Round-robin 8-slot DMA via `osPiStartDma`. Files DMA'd in 0x4000-byte chunks.

| Function | Role |
|----------|------|
| `initDmaSlots` (0x800045E8) | Initializes 8 slots + msg queues + flags |
| `submitDmaSlot` (0x80003480) | Kicks off a DMA chain (round-robin slot allocator) |
| `waitDmaSlotComplete` (0x80003638) | Blocking variant: drains entire DMA chain |
| `pollDmaSlotStep` (0x80003824) | Non-blocking step; returns 1 when chain done |
| `setDmaSlotMaxTxStepSize` (0x80005938) | Sets `D_80111254` (unused in practice) |
| `setupAssetDma` (0x80004E70) | Setup for asset-loading DMAs (uses service registry) |
| `teardownAssetDma` (0x80004C70) | Teardown for asset DMAs |
| `findManifestEntryByName` (0x800047F4) / `find_manifest_entry` (0x80003A0C) | Walk manifest by name string |
| `loadDebugAssets` (0x800246E8) | Loads "dbg_data"/"dbg_font" at boot |
| `processOverlayDmaStruct` (0x800033A0) | Handles overlay-DMA struct (src/dest/sizes + BSS zero-fill) |
| `loadOverlay` (0x80000B20) | High-level overlay loader (in-mission / mempak / cinematic) |
| `piDmaWorker` (0x80005570) | PI DMA service worker; 256 × 20B ring at 0x801112B0 |

### 5. Asset formats

| Format | Purpose | Loader |
|--------|---------|--------|
| HOB | 3D models (object/meshdef/face/vertex) | `load_hmt_and_hob` (0x8005645C) |
| HMT | Material/texture collections | `load_hmt_and_hob` |
| HMP | Level height-maps (terrain) | `load_level_hmp` (0x80043D74) |
| DAT | In-mission spawn/spline/event data | `load_level_dat` (per dat_files.md) |
| TXT | Text (Front/Game/Voice), XOR-obfuscated | `loadTxtFile` (0x800556A0) |
| SND | Audio (pool/proj/sdir/samp) | `loadSndFiles` (0x800663B0) → `parseSndFiles` (0x80097518) |
| IMG/ANM/SPR/TM | Image variants | `parseImageFile` (0x8001EB24) |

Asset blob structure (ROM 0x144340):
- 2 segments (`data`, `dbg_data`) → `block_header` → manifest entries (32B each) → asset data
- `compressed_size = 0xFFFFFFFF` = uncompressed; otherwise zlib stream (oversized by 10B)
- Directory flag bit 7 in `manifest_entry.flags`

HOB-specific helpers:
- `meshdef0_offset_convert` (0x80058948) / `meshdef1_offset_convert` (0x800587F0)
- `scaleHobVertexColors` (named separately as `func_80058B40`) — RGB color scale per face
- `walkMeshdef0List` (0x80056EB0) — walks 0x4C-sized meshdef0 linked list

HMT-specific helpers:
- `registerHmtTextureInTable` (0x80022B90) — translates per-HMT texture_index → global D_80128F08 index via D_8011A444
- `parseHmtMaterials` (0x80022A00) — load_hmt_and_hob inner helper (count, material_entry\*, texture_entry\*)

zlib decompressor (embedded v0.99-1.08, per docs/unsorted_thoughts/notes.txt):
- `adler32` (0x800269B0), `inflateInit2_` (0x80026C7C), `inflate_blocks_new` (0x800276C4), `inflate_trees_free` (0x80029A68)
- `rs_zcalloc` (0x8000527C), `rs_zcfree` (0x8000525C), `rs_memset` (0x800078E0) — zlib's memory + memset bindings, prefixed `rs_` (Rogue Squadron) by splat

### 6. Display / video

| Function | Role |
|----------|------|
| `setDisplayMode` (0x8008EA14) | Validates mode<0x21, stores at 0x80151AD0, calls inner setup chain |
| `getViModeType` / `setViModeType` / `getViModePeriod` / `isViModeTypePal` | VI mode getters/setters (already named in splat) |

### 7. Audio subsystem (Factor 5 / MusyX)

Custom Factor 5 audio ucode. Currently stubbed in the recomp — no audio plays. Uses recursive mutex.

| Function | Role |
|----------|------|
| `initAudioSubsystem` (0x80091B3C) | Inits mutex + osAiSetFrequency + audio thread/mq/event; called from setDisplayMode |
| `initFactor5Mutex` (0x80091FC4) | Creates queue D_80149990 + posts initial token |
| `factor5MutexAcquire` (0x80092010) | Recursive: if counter>0 increment, else `osRecvMesg(BLOCK)` |
| `factor5MutexRelease` (0x8009205C) | Decrement; on zero → `osSendMesg` to wake one waiter |
| `factor5QueueBlock` (0x800920A4) | Low-level `osRecvMesg(BLOCK)`, no counter touch |
| `factor5QueueSignal` (0x800920D0) | Low-level `osSendMesg(NOBLOCK)` |
| `waitForMusyXAudioTaskDone` (0x80091EB0) | Busy-spins on `D_8014998A`; hangs without real MusyX RSP task |
| `loadSndFiles` (0x800663B0) | Loads 4 SND files (pool/proj/sdir/samp) |
| `parseSndFiles` (0x80097518) | Parses SND file sections |
| `parseModelAnimType2Entry` (0x80083468) | Parses singlet/triplet/quadlet animation lists |
| `isAudioSlotActive` (0x800920FC) | Returns 1 if entry r4 in audio slot table is active (MEM_BU(\*0x801487F8 + r4\*0x88, 0) != 0) |
| `anyAudioSlotActive` (0x8008E57C) | Loops i=0..count, OR-aggregates `isAudioSlotActive(i)` |
| `waitForAnyAudioSlot` (0x800668B0) | Busy-wait on `anyAudioSlotActive`; cinematic/menu init blocker under HLE (NOPed in toml) |

### 7a. VI / DP post-swap handshake

Three helpers around the libultra DP-event signal, called by `submitGfxFrame`
after `osViSwapBuffer`:

| Function | Role |
|----------|------|
| `setPostSwapPendingFlags` (0x8001C244) | Sets both bytes at 0x8011A8C8/C9 to 1 — "frame submitted, awaiting DP completion" |
| `waitForPostSwapAck` (0x8001C260) | Polls flag at 0x8011A8C9 in osRecvMesg/yield loop until cleared |
| `clearPostSwapPendingFlags` (0x8001C2F8) | Clears both flag bytes to 0 (called by func_8001818C + initVideoBootWrapper) |

Under HLE, `dpInterruptHandlerThread` is supposed to forward DP-event signals
to the game's queue (D_8011A7E8) to unblock `waitForPostSwapAck` — but its
forwarding was historically gated on another flag that never became 1. A gate-NOP
patch was applied to unblock it during earlier investigation.

### 8. Controllers

| Function | Role |
|----------|------|
| `siServiceThread` (0x80002E10) | Controller polling thread |
| `initSiQueue` (0x80003308) | Initializes SI message queue + flags |
| `waitForSerialIdle` (0x80003284) | Spin-yields until SI busy flag (0x80110741) clears |
| `releaseSerialLock` (0x800032E0) | Releases the SI lock |
| `getControllerButtonAndStick` (0x80079D54) | Reads button + stick state |
| `controllerStickXToPercent` (0x8007A068) | stick_x / 128 → percent |
| `controllerStickYToPercent` (0x8007A0A8) | stick_y / 128 → percent |
| `setNewAndPreviousButtonsPressed` (0x80079CE0) | Sets D_8013A950 (prev) + D_8013A960 (new) |
| `readControllerInputs` (0x80079F20) | Inner fetcher called by setNewAndPrev |
| `initNewAndPreviousButtonsPressed` (0x80079EB0) | Zero-init both arrays |
| `getControllerNewButtonsPressed` (0x80079F50) | Returns D_8013A960[idx] |
| `unsetControllerNewButtonsPressed` (0x80079FD8) | Clears flags in D_8013A960 |
| `initControllerSettingsStructs` (0x800BCE2C) | Mallocs/inits controller settings UI |
| `controllerSettingsScreen` (0x800BD274) | Controller settings menu handler |
| `getControllerSettingsTitle` (0x800C49BC) | Title generator for controller settings screen |

Controller settings — 4 named profiles: LUKE / WEDGE / JANSON / HOBBIE. 18 inputs (pause, fire, brakes, thrust, roll, etc).

### 9. Menus

Main menu state at 0x800CE730 (0xF8 bytes). 13 menus enumerated (MAIN_MENU through AT_THE_MOVIES).

| Function | Role |
|----------|------|
| `menuOverlayInit` (0x800C58A0) | Menu overlay init (calls registerSaveService) |
| `menuControllerInput` (0x800B47C4) | Big menu controller input handler |
| `setupMenuData` (0x800BA0F0) | Sets up D_800CE730 per current menu |
| `updateMenuPerFrame` (0x800BB394) | Per-frame menu update (highlighting etc) |
| `handleGameSettingsMenu` (0x800BB234) | Game settings menu input |
| `menuSubtype02Handler` (0x800C30C8) | ~0xF18 sub-type 02 menu handler |
| `readAccountForSelectionScreen` (0x800B85B4) | Account selection screen |
| `eliteRoguesMenuHandler` (0x800BEA00) | Elite Rogues high-score screen |

### 10. HUD

Two double-buffered HUD structs at `D_8010CA30[2]` (0x278 bytes each). Material table (`D_8011A444`) → texture table (`D_80128F08`, 0x24B per entry).

| Function | Role |
|----------|------|
| `initHudStruct` (0x800C0084) | Initializes 0xF80-byte HUD struct |
| `hudDisplayUpdateWorker` (0x800495FC) | Service worker; processes HUD material/texture msgs |
| `getTextureDataByMaterialId` (0x800232F8) | material_id → D_8011A444 → D_80128F08 texture ptr |
| `setHudSecondaryWeaponInfo` (0x800BFDC4) | Sets secondary weapon type/level fields |

### 11. NPC / slot dispatcher

0x800-entry slot list at `gNpcSlotList`, each entry → 0xC0-entry NPC context array. Update function pointer at NPC context +0x00 runs every frame.

| Function | Role |
|----------|------|
| `slotDispatcherIter` (0x8003E8DC) | Walks gNpcSlotList, calls each NPC update_func |
| `slotDispatcherInner` (0x8003EA4C) | Per-slot dispatch; mallocs D_80130BC0/BC4 entries |
| `slotEffectHandlerDispatch` (0x800A71B8) | Effect handler dispatch from cinematic loop |
| `allocNpcContextArrays` (0x8003FD54) | Mallocs gNpcContextArray + ptrs |
| `setupNpcUpdateFunctions` (0x800653B4) | Sets update fn ptrs (DAT Type 6 setup) |

### 12. Cinematic system

Multi-stage cutscene playback. State ptr at 0x800B0934, stage counter at 0x800B0938. Active slot indices at 0x80139560.

| Function | Role |
|----------|------|
| `cinematicLoopBody` (0x800A5D80) | Inner cinematic frame loop |
| `cinematicInitializer` (0x800AF408) | Sets state ptr at 0x800B0934 |
| `cinematicStageAdvancer` (0x800AF550) | Per-frame stage counter advance |
| `cinematicDeactivator` (0x800AF60C) | Clears cinematic state |
| `cinematicComputeDt` (0x800AF360) | Per-frame delta-time computer |
| `cinematicInterpRatio` (0x800AF668) | Computes interpolation ratio (byte/float) |
| `cinematicSlotBatchDispatch` (0x800A70E4) | Calls slotDispatcherIter for each active cinematic slot |
| `load_cutscene` (0x800A6620) | Loads cutscene file (0x25AC fixed + variable Vec3f list) |

Cutscene file structure: `cuts_file_constant` (0x25AC) containing filename, 200 cuts_0058_type entries (0x18 each), 6 cuts_1318_type, 60 cuts_13D8_type (0x4C asset entries with flags), followed by variable Vec3f list.

### 13. DAT files (in-mission level data)

5 data types in DAT files:
- Type 0: spawn positions / building / props
- Type 2: unknown (Sullust LAVAFART##)
- Type 3: splines
- Type 6: LOD bounding boxes + MUSICRNG entries
- Type 7: event triggers

| Function | Role |
|----------|------|
| `parseDatSpawnPositions` (0x80065790) | Parses DAT Type 0 |
| `parseDatEventTriggers` (0x800AA870) | Parses DAT Type 7 |
| `parseDatItemCommon` (0x80046620) | Common helper used by Type 0 + Type 7 parsers |
| `setupNpcUpdateFunctions` (0x800653B4) | NPC update fn setup (DAT Type 6 setup) |

### 14. Cheat codes

Stored as CRC32 hashes at 0x800A0ED0. Active flags at 0x80130B58. Seed = 0xFAC5FAC5 ("FAC5" = Factor 5). 30 cheats total.

| Function | Role |
|----------|------|
| `rs_crc32` (0x800824F8) | CRC32 hash function |
| `make_crc32_lut` (0x80082544) | Generates reflected CRC32 lookup table |
| `decrypt_ns_hmt` (0x8006AFC0) | Naboo Starfighter HMT decryption |
| `load_naboo_starfighter` (0x8006C780) | NS asset loader with sanity check |

### 15. Text subsystem

3 text categories: Front (main menus), Game (in-mission), Voice (subtitles). Header at `txtFileHeader` (0x80138E60). All text XOR-obfuscated with rolling key seeded at 0xF5.

| Function | Role |
|----------|------|
| `loadTxtFile` (0x800556A0) | Loads/decrypts text file (Front/Game) |
| `getVoiceText` (0x80055978) | Voice text decrypt (forces uppercase) |
| `getGameOrFrontText` (0x8005589C) | Get string by ID (Front/Game) |

### 16. Player crafts

9 craft types (XWING, YWING, AWING, VWING, SNOWSPEEDER, FALCON, TIEINTER, T16, KOELSCH). Selection at byte 0x80130B41. Per-level default at 0x8009EC50.

| Function | Role |
|----------|------|
| `choosePlayerCraftAssets` (0x800FB6C0) | DMAs HMT/HOB for chosen craft |
| `mallocCraftSelectionData` (0x800A9364) | Mallocs `D_800CDA5C` selection structs (56 entries) |
| `buildSecondaryWeaponString` (0x800AE530) | Builds secondary-weapon display string |
| `printLevelSelectWeaponText` (0x800B19EC) | Prints weapon/shield text on level select |
| `getAvailableShipsForLevel` (0x800C63C0) | Returns bitmask of available ships per level |
| `getSecondaryWeaponForCraftLevel` (0x800C6728) | Returns secondary weapon ID |
| `getSecondaryWeaponCount` (0x8006F43C) | Max secondary weapon count (-1 = infinity) |
| `craftSelectionVoiceLineHelper` (0x800AE264) | Voice-line helper for craft selection |
| `mallocVoiceLineStruct` (0x800CBF60) | Mallocs voice-line struct for craft selection |
| `playerXwingUpdate` (0x800B5434) | X-Wing per-frame update (one of many craft-specific) |

### 17. Mission objectives

3 tracking primitives: 128 booleans (`gObjectiveBooleans`), 128 counts (`gObjectiveCounts`), 8 timers (`gObjectiveTimers`). 0x30-entry `simpleCheckHandles` array. Per-level dispatch via `D_8010A450` (4 fn ptrs × 21 levels).

### 18. Image/texture handling

| Function | Role |
|----------|------|
| `parseImageFile` (0x8001EB24) | Big switch on image type + flags |
| `loadTextureToMemory` (0x8001F954) | Loads texture into D_80128F08 chain |
| `full_header_image_offset_convert` | Offset-to-pointer for full-header images |
| `registerHmtTextureInTable` (0x80022B90) | HMT texture index → global table index |

## Rendering pipeline & per-frame loops

> **This is the key to fixing the black screen.** The rendering path involves
> nested loops; understanding when each emits a display list, when it
> programs VI registers, and how RT64's PresentEarly matcher interacts with
> the resulting timeline is what we need to make pixels appear.

### Outer loop: `mainGameLoop` (0x8003DFA0)

State machine driven by bytes in `gGameSettings` (0x80130B40). One iteration runs the screen for an arbitrary number of frames, returns when the screen state transitions, then mainGameLoop checks the next set of state bytes.

Pattern per iteration (`cd .` + `E:/Projects/N64Recomp/RecompiledFuncs/funcs_8.c` near `mainGameLoop`):

```
loop:
    func_80023D30()              # init (probably scene reset)
    func_80003104(1, &gSettings.unk88)  # ?
    func_80063B58()              # reset 0x8009FC10 array (game state objects)
    func_80000B20()              # loadOverlay - argument varies

    # Check sub-state flags and call appropriate scene handler:
    if gSettings.unk20:           # menu screens
        loadOverlay(1)            # mempak/menu overlay
        menuOverlayInit(s3, 9, 0)
        func_8006F01C() / func_8006ED90(5) / etc — sub-state checks
        loadOverlay(1)
        menuOverlayInit(s3, 2, 0xB)
        func_8006E360(5)

    if gSettings.unk21 and gSettings.unk4 != 5 and != 3:
        loadOverlay(2)            # cinematic overlay
        cinematicLoopBody(2, 0x13, 0)  # inner per-frame loop

    if gSettings.unk22:
        # similar pattern

    ...

    # End-of-frame "common" work (calls visible in disasm 0x8003E280-0x8003E3FF):
    func_8006F044()               # advance demoId, modulo 6
    func_80000B20()               # loadOverlay
    func_8006B000(gCurrentLevel)  # level-specific work
    func_80001354()               # ?
    findManifestEntryByName(...)  # asset name lookup
    func_800FA250()               # ?
    func_800FA6A4()               # ?
    func_800FB9E4()               # checks float comparison gSettings+0xBA4 vs 0x800A908C
    func_80004994(-1)             # manifest cleanup/unsubscribe-all
    jump back to loop start
```

The per-frame **gfx submission is NOT done by mainGameLoop directly**. It happens inside the inner-loop callees (`menuOverlayInit`, `cinematicLoopBody`, in-mission update). mainGameLoop is the state arbiter that picks which screen handler runs.

### Inner loop A: `cinematicLoopBody` (0x800A5D80)

In `cinematicLoopBody`, each iteration:
1. Reads `gCurrentCutsceneFile` (0x800B1904)
2. Calls `cinematicComputeDt` to get frame delta
3. Calls `cinematicStageAdvancer` to advance stage counter (per-frame)
4. For each active cinematic slot index in `0x80139560` (6 halfwords):
   - `cinematicSlotBatchDispatch` calls `slotDispatcherIter` for the slot
   - `slotDispatcherIter` walks `gNpcSlotList` (0x80130BB0, 0x800 entries)
   - Each NPC's `update_func` (at NPC ctx + 0x00) runs, emitting gfx/audio
5. Emits per-frame DPC commands (the display list)
6. Loops until cinematic deactivates (skip flag or fc threshold)

Stability/timing observations from previous instrumented runs (before the
overlay-dispatch root cause was identified — these were measured on the
prior LLE pipeline, useful only as historical reference):
- Healthy cinematic frame rate: swap=30/s, sample=60/s (2:1 cadence).
- ~215k cinematic RegularRects pushed → 215k reach GPU DrawInstanced (Stage 3 viewport-skip=0).
- Host-side pipeline does NOT drop content; the absent content was never produced by the game in the first place.

### Inner loop B: `menuOverlayInit` (0x800C58A0)

Menu screen handler. Sets up `D_800CE730` (0xF8 bytes) for the current menu, then runs per-frame:
1. `menuControllerInput` (0x800B47C4) — reads sticks/buttons
2. `setupMenuData` (0x800BA0F0) — refreshes `D_800CE730` based on menu state
3. `updateMenuPerFrame` (0x800BB394) — highlights, scrolls, etc
4. Emits text + UI texrects via `getTextureDataByMaterialId` chain
5. Loops until user navigates / state changes

### Inner loop C: in-mission (`.ovl.mission`)

Not yet fully mapped. Loaded via `loadOverlay(0)`. Per-craft updaters in vtable (e.g., `playerXwingUpdate` at 0x800B5434). Per-level objective fns in `D_8010A450[21]` table (4 fn ptrs/level, all named lvN_*).

### Display-list submission path

Game emits DPC commands via direct writes to GBI buffer + DPC register pokes. Submission triggers `osSpTaskStart`/equivalent. In the recomp:
- LLE path: game's writes hit `dpc_bridge.cpp` (`src/rsp/dpc_bridge.cpp:252+`) which forwards to RT64.
- HLE path (current): GFX tasks routed via `action_queue → send_dl → F3DFACTOR5` GBI module in `lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp`.

Current mode: **HLE via F3DFACTOR5** (post-2026-05-10 pivot).

### `submitGfxFrame` — the per-frame orchestrator (0x8000C07C)

Every screen handler (cinematic, menu, in-mission) calls `submitGfxFrame`
once per visual frame. There are ~14 call sites, located in the overlay
sections (`.ovl.cinematic`, `.ovl.menu`, `.ovl.mission`).

Internal call sequence:
```
submitGfxFrame:
    getTimeSinceLastFrame()          # ΔT for frame pacing
    osRecvMesg(...)                  # wait for "previous frame done" signal
    yieldFromSpThread()              # osYieldThread wrapper
    bufferArbiterMarkSlotReady()     # mark previous slot READY (1→2)
    func_8000815C(), func_80008550() # gfx state mgmt
    bufferArbiterAllocSlot()         # alloc next slot (0→1)
    getInactiveBufferIndex()         # which buffer is currently NOT visible
    osStartThread(spTaskSchedulerThread) # kick SP/RSP task
    osSendMesg(...)                  # signal task submitted
    osRecvMesg(...)                  # wait for task done
    yieldFromGfxFrame()              # osYieldThread wrapper
    osWritebackDCache(...)
    osCreateThread(...)
    bufferArbiterProducerMark()      # mark CONSUMED (2→3)
    osRecvMesg(...)
    yieldFromGfxFrame()
    osWritebackDCache(...)
    bufferArbiterProducerMark()      # call osViSwapBuffer → PRESENTED (X→4)
    setPostSwapPendingFlags()        # mark frame-submitted (flags at 0x8011A8C8/C9 = 1)
    bufferArbiterProducerMark()      # final mark
    func_80000C50()                  # ?
    recordFrameTimestamp()           # save osGetTime to 0x80138F10 for next ΔT
```

**Why this matters for the black screen**: every screen calls
`submitGfxFrame`, and the chain ends with `osViSwapBuffer` (inside the
2nd `bufferArbiterProducerMark`) writing a new fb addr. The buffer
arbiter has been empirically verified to cycle correctly. So the game IS
submitting display lists every frame, IS calling `osViSwapBuffer` every
frame, IS rotating buffers. The screen is black because the display
lists being submitted contain only the framebuffer clear and no
content commands — see the overlay-dispatch root cause in the README's
"Where it actually stops" section.

The next investigation should look at:
1. What does the inner "SP/RSP task kick" actually do in our recomp? Is
   `osStartThread`/`osSpTaskStartGo` reaching RT64's dpc_bridge?
2. After `osViSwapBuffer` writes `0x80138E98[slot]`, does that propagate to
   RT64's VI register pointers via `Application::Core`?
3. RT64's PresentEarly matcher: what specific criteria fail? Add logging
   in `lib/rt64/src/hle/rt64_state.cpp` PresentEarly to print rejected
   workloads and their reasons.

### VI register programming + per-frame VI loop

Game uses libultra's standard VI architecture. **3 dedicated rendering threads** are created by `initVideoSubsystem` (0x8001A098):

1. **`viRetraceHandlerThread`** (0x80019868, priority 0x75) — VI retrace handler.
   Receives VI interrupts via `osViSetEvent` → message queue. Per iteration:
   ```
   L_800198E8:                              # top of per-frame loop
       osRecvMesg(viEventQ, BLOCK)          # wait for VI retrace
       osViGetCurrentField()                # NTSC field (top/bottom)
       osRecvMesg(...)                      # wait for "buffer ready" signal
       <large state machine — see below>
       osViSwapBuffer(fb)                   # fb loaded from 0x80138E98
       func_8001C12C()                      # post-swap: osDpSetStatus +
                                            # osWritebackDCacheAll + notify
       osSendMesg(swapDoneQ)                # notify game thread swap happened
       osSendMesg(retraceDoneQ)             # notify other consumers
       j L_800198E8                         # loop
   ```
   The "buffer ready" handshake is what lets the game tell the VI thread
   that a fresh framebuffer is ready to be presented. The state machine
   between recv and swap manages double/triple-buffer rotation.

2. **`spTaskSchedulerThread`** (0x80019BF4) — RSP task dispatcher.
   Calls `osSpTaskLoad`, `osSpTaskStartGo`, `osSpTaskYield`, `osSpTaskYielded`.
   Receives task submission messages from the game thread, dispatches them
   to the RSP, handles yields (e.g. when GFX task interrupts AUDIO task).

3. **`dpInterruptHandlerThread`** (0x8001C328) — RDP done handler.
   Registers `osSetEventMesg(OS_EVENT_DP=9, queue)`. When RDP finishes a
   draw command list, this thread receives the event and notifies waiters.

These three threads form the **classic libultra rendering setup**. The game
thread (mainGameLoop or its inner loops) prepares fb + display list, kicks
RSP via spTaskScheduler, RSP processes the DL and hands to RDP, RDP raster
into fb, dpInterruptHandler signals "done", the game updates `0x80138E98`
with the new fb addr, and viRetraceHandlerThread swaps it on next retrace.

The `osViSwapBuffer` wrapper lives in `src/main/upstream_compat.cpp:65`
(defined twice — see build warning to clean up).

**Earlier hypothesis about RT64's PresentEarly matcher** — superseded. Initial
investigation suggested RT64's PresentEarly matcher was failing because the
game was emitting display lists before programming VI registers. Direct
`ROGUESQ_LOG_VI_FB_CONTENT=1` traces later showed the framebuffer at the
expected VI scanout address is uniformly black (`0x0001` × 71680 pixels),
proving the present path itself is fine — the game just isn't drawing any
content. The actual root cause is overlay dispatch (see the README).

### Framebuffer arbiter (the actual swap selection logic)

The fb addr passed to `osViSwapBuffer` is NOT a single global. It's selected from
an **array of fb slots** by a state-machine arbiter living inside
`viRetraceHandlerThread` at L_80019AB4–L_80019B14.

Layout:
- `0x80138E98[N]` — array of fb POINTERS (one per buffer slot, 4 bytes each).
  Initial slot's ptr written by `initVideoSubsystem` at 0x8001A140.
- `0x80138EAA[N]` — parallel array of STATE BYTES (one per slot, 1 byte each).
- `0x80138EAD` (byte at `-0x7153` from 0x80140000) — total slot count.
- `s6` reg in retrace thread points to a 2-byte pair (`+0x0`, `+0x1`) used as
  thresholds in the slot-selection inner loop — likely "frames waited" /
  "max frames to wait before forcing a swap".

State machine (per VI retrace iteration):
```
L_80019AB4 (search):
    i = 0
    while i < total_slots:
        if state[i] == fp:        # fp = expected state (game-controlled)
            check secondary criteria (s6+0, s6+1 thresholds)
            if criteria met:
                accept slot i; goto L_80019AF8 with t2=4 (visible)
        i++

L_80019AF4 (no match): t2 = 4

L_80019AF8 (commit swap):
    state[a1] = t2 (= 4)          # mark chosen slot as visible
    fb = fb_ptrs[a1]
    osViSwapBuffer(fb)
```

**State machine — 6 states**:

| Value | Meaning | Writer |
|-------|---------|--------|
| `0` | FREE — slot available for allocation | `viRetraceHandlerThread` (5→0) |
| `1` | ALLOC — slot reserved, not yet ready | `bufferArbiterAllocSlot` (0→1) |
| `2` | READY — slot has content, awaiting submission | `bufferArbiterMarkSlotReady` (1→2) |
| `3` | CONSUMED — submitted to RSP/RDP | `bufferArbiterProducerMark` (2→3) |
| `4` | PRESENTED — handed to VI (called osViSwapBuffer) | `bufferArbiterProducerMark` (X→4) |
| `5` | DISPLAYED — actually scanned out | `viRetraceHandlerThread` (4→5) |

State machine flow (one buffer's lifecycle):
```
[FREE 0] ──bufferArbiterAllocSlot──▶ [ALLOC 1]
[ALLOC 1] ──bufferArbiterMarkSlotReady──▶ [READY 2]
[READY 2] ──bufferArbiterProducerMark──▶ [CONSUMED 3]
[CONSUMED 3]──bufferArbiterProducerMark──▶ [PRESENTED 4]    (osViSwapBuffer)
[PRESENTED 4]──viRetraceHandlerThread──▶ [DISPLAYED 5]      (VI retrace tick)
[DISPLAYED 5]──viRetraceHandlerThread──▶ [FREE 0]           (next retrace)
```

**The buffer arbiter is FINE** (per 2026-05-07 empirical verification — slots
cycle through all states during cinematic just like boot). Earlier "stuck on
single buffer" diagnoses turned out to be wrong. The black screen is downstream
of the arbiter — likely in RT64's PresentEarly matcher or the HLE GFX
submission path, not in this state machine.

The state-byte writers in the game code:
- Read state: `MEM_BU(addr, -0X7156)` (lbu pattern, sign-extended offset)
- Write state: `MEM_B(-0X7156, ctx->rX) = value`
- The slot index encodes into the address register before the load/store

### Why the screen is black (current state)

Direct measurement with `ROGUESQ_LOG_VI_FB_CONTENT=1` shows the framebuffer
the VI is sampling (`0x806BA000` during attribution) is uniformly `0x0001` —
71680 of 71680 pixels are canonical N64 black. The game's CPU code isn't
producing any pixel content at all, only the framebuffer clear. The host
display path is faithfully scanning out whatever's in RDRAM; what's in
RDRAM is just a clear.

Earlier hypotheses (RT64 PresentEarly matcher rejecting the workload, VI
ordering mismatch, etc.) were investigated and ruled out — they were
chasing a symptom of the same upstream problem. The real bug is
overlay-dispatch: the menu and cinematic overlays' distinct functions
never register in librecomp's `func_map` at runtime, so the game's
direct calls into the overlay VA range always resolve to the mission
overlay's version (whichever the linker bound). The function that
should draw the attribution text never runs. See the README's "Where
it actually stops" section for the full diagnosis and a proposed fix.

### Host runtime bridge — game ↔ RT64 plumbing

When the recompiled game thread calls `osSpTaskStartGo` (inside
`spTaskSchedulerThread` at L_80019D58), the host runtime intercepts and
routes the task. This is THE bridge from "game gfx code" to "RT64 render
backend". The full chain (post-2026-05-10 HLE pivot):

```
spTaskSchedulerThread (game thread)
    osSpTaskLoad(task)
    osSpTaskStartGo(task)
            │
            ▼  intercepted in:
    librecomp/src/sp.cpp :: osSpTaskStartGo_recomp
            │
            ▼
    ultramodern::submit_rsp_task(rdram, taskPtr)
            │
            ▼  (events.cpp ~line 280)
    if (task->t.type == M_GFXTASK):
        events_context.action_queue.enqueue(SpTaskAction{*task})   ← HLE path
    else:  // M_AUDTASK, etc.
        events_context.sp_task_queue.enqueue(task)                 ← LLE path
            │
            ▼  (HLE path picked up by:)
    gfx_thread_func (events.cpp:328)
        wait_dequeue → SpTaskAction
        sp_complete()                            # tell game RSP done
        on_displaylist_submitted(displaylist)
        renderer_context->send_dl(&task_action->task)   ← HOST RT64 ENTRY
        dp_complete()                            # tell game RDP done
        on_displaylist_parsed/completed
            │
            ▼
    rt64_render_context.cpp :: send_dl
            │
            ▼
    RT64::Application::processDisplayLists(rdram, dlStart, dlEnd, isHLE=true)
            │
            ▼  (RT64 internal HLE dispatch)
    GBI_F3DFACTOR5 handlers (rt64_gbi_f3dfactor5.cpp)
```

For VI presentation:
```
ultramodern's vi_thread (events.cpp:180+, host VI tick at 60Hz)
    sleep_until(next VI time)
    events_context.action_queue.enqueue(ScreenUpdateAction{vi.regs})  ← copies current VI state
    events_context.vi.update_vi()                                     ← advance state machine
            │
            ▼
    gfx_thread_func (same thread that handles SpTaskAction)
        wait_dequeue → ScreenUpdateAction
        events_context.vi.update_screen_regs = action.regs   ← snapshot for RT64
        renderer_context->update_screen()                    ← TRIGGERS RT64 PRESENT
```

RT64 reads VI registers via pointers from `events_context.vi.update_screen_regs`
(set up in `Application::Core`). The pointer indirection lets RT64 see live
VI state every present, but ultramodern only updates it just-before
`update_screen()`.

**Critical timing observation**: SpTaskActions and ScreenUpdateActions go
through the SAME `action_queue` processed by the SAME `gfx_thread_func`.
They are serialized. So when the game submits a DL via `osSpTaskStartGo`
and then the VI tick fires before the DL is dequeued, the DL processes
FIRST (queue order), and THEN the screen update.

What this means for the present-path black screen:
1. Game calls `submitGfxFrame` once per frame (~14 callers).
2. Inside, `osSpTaskStartGo` enqueues an SpTaskAction.
3. Later, `osViSwapBuffer` updates VI origin but does NOT enqueue a screen
   update (only the host's 60Hz vi_thread enqueues ScreenUpdateAction).
4. The gfx_thread sees SpTaskAction first → `send_dl` → RT64 builds a
   Workload tied to the CURRENT fb addr.
5. Eventually a ScreenUpdateAction arrives with the **same** VI regs RT64
   already saw → RT64's PresentEarly matcher should fire and present.

The matcher in `rt64_state.cpp` PresentEarly compares the workload's
`colorImg.address/width/siz` against the VI regs about to be presented. If
they don't match exactly (e.g. game wrote to fb=0x5D4000 but VI is still
on 0x66A000 from a previous swap), the matcher rejects the workload and
nothing presents. The fix is either to widen the matcher, force a present,
or fix the timing so the game's VI swap happens before the DL is built.

### Diagnostic env vars (rendering)

| Var | Effect |
|-----|--------|
| `ROGUESQ_LOG_DPC=1` | Log DPC submissions (cmd buffer addrs, sizes) |
| `ROGUESQ_LOG_PRESENT=1` | Log RT64 present events (matched fb, skip reason) |
| `ROGUESQ_LOG_RDP_STATE=1` | Log RDP state changes (combiner mux, fb, scissor) |
| `ROGUESQ_LOG_SP_TASKS=1` | Log SP task starts/yields |
| `ROGUESQ_LOG_RT64_ALLOC=1` | Log RT64 GPU resource allocations |
| `ROGUESQ_LOG_PIPELINE=1` | Log per-stage rect counters (push/dispatch/draw) |

Run via `tools/run-stability.ps1 -Runs 1 -Timeout 30 -EnvVars "ROGUESQ_LOG_PRESENT=1"`.

### How to fix the black screen (action items)

We now know the whole pipeline. The game is correctly:

1. Allocating buffer slots (`bufferArbiterAllocSlot`)
2. Building display lists into them
3. Submitting via `osSpTaskStartGo` (→ `gfx_thread_func` → `send_dl` → RT64
   `processDisplayLists`)
4. Updating VI registers via libultra calls (state stored in `events_context.vi.next_state`)
5. Marking slots PRESENTED then DISPLAYED via the arbiter
6. ultramodern's vi_thread enqueuing `ScreenUpdateAction` at 60Hz
   (→ `update_screen` → RT64 PresentEarly matcher)

The break is in step 6 → matcher: RT64 fails to match the workload's
fb to the VI scanout fb. Concrete experiments to try, in order of effort:

**1. Add logging at the matcher boundary (low effort, high info).**
   Edit `lib/rt64/src/hle/rt64_state.cpp` around line 1773 (PresentEarly
   matcher). Log: workload's `colorImg.address/width/siz`, the current
   `VI_ORIGIN_REG` ultramodern is asking RT64 to scan, the reason for
   rejection (mismatch field by field). Run interactively, dump log,
   compare. Should immediately tell us *what* doesn't match.

**2. Hook the game's `osViSwapBuffer` calls to also enqueue a
   `ScreenUpdateAction`.** The current flow only schedules screen updates
   on the host's 60Hz timer. If the game programs VI faster (some titles
   do), RT64 sees stale state. Add a hook in
   `librecomp/src/vi.cpp::osViSwapBuffer_recomp` to call
   `events_context.action_queue.enqueue(ScreenUpdateAction{...})`. This
   guarantees the matcher gets a fresh VI state right after the game's
   own VI update.

**3. Force the matcher to accept on fb-address-only.** A workaround:
   ignore width/siz mismatches and match purely on `VI_ORIGIN` ==
   workload `colorImg.address`. Single edit in `rt64_state.cpp`. Risky if
   it triggers false matches on hi-res mode, but quickest test of "is
   the matcher being too strict?" hypothesis.

**4. Verify SpTaskAction ordering vs ScreenUpdateAction.** Add a counter
   to `gfx_thread_func` that increments separately for each action type
   and logs the ratio. If we see N SpTaskActions per ScreenUpdateAction
   for N >> 1, RT64 might be ignoring all but the last DL. If N << 1,
   RT64 is presenting before the game has anything to show.

**5. Use RenderDoc / PIX.** Capture one frame interactively (post-boot,
   when the game is in the menu screen and pixels are expected). Inspect
   the actual GPU draws. If draws are happening to an off-screen RT and
   never blitted, that's the bug. If there are no draws at all, the DL
   isn't reaching RT64. If draws happen but go to the wrong fb, the
   matcher needs fixing.

**6. Rename more functions to expose the pattern.** Each rename
   (`submitGfxFrame`, `viRetraceHandlerThread`, etc.) makes the trace
   logs readable. The high-leverage targets now:
   - `func_8000815C` / `func_80008550` (the gfx-state mgmt called inside
     `submitGfxFrame`)
   - `func_8000B6F4` / `func_8000A8A0` (called from `cinematicLoopBody`
     just before `submitGfxFrame`)
   - The remaining cinematic overlay helpers (`func_800A6904`,
     `func_800A6CE4`, `func_800A6FC0`, `func_800A73E4`, etc.)
   - The mission overlay update functions (per-craft + per-level vtables)

## Threading topology

| Thread | Priority | Purpose |
|--------|----------|---------|
| idle_thread | 10 | Bootstrap-only; creates main worker, then idles |
| `mainBootstrapWorker` | 0xA | Boot init, then tail-calls `mainGameLoop` |
| `siServiceThread` | (varies) | Controller polling, SI-busy flag (0x80110741) |
| Audio thread | (created by `initAudioSubsystem`) | Factor 5 audio dispatch |
| Service workers | (per-worker) | Save, HUD, event-queue, PI DMA, etc — registered via `registerServiceWorker` |

## Concurrency primitives

- **libultra `OSMesgQueue`** — used everywhere for inter-thread messaging
- **Factor 5 recursive mutex** — used by audio code; depth counter at `D_800A1780`, queue at `D_80149990`
- **SI busy flag** (0x80110741) — spin-wait by other subsystems for serial idle (save vs controller)
- **Service-worker registry** (0x801128D0) — linked-list of registered service workers

## Build/regen workflow

After renaming a symbol via `llvm-objcopy --redefine-sym`:

1. **Regen recompiled C**:
   ```
   cd E:/Projects/N64Recomp && ./build_new/Release/N64Recomp.exe rogue_squadron.toml
   ```
   ⚠ **Use the Release binary at `build_new/Release/`**. The older `Debug/N64Recomp.exe`
   fails with `Unhandled instruction: cache` on `recomp_entrypoint` and aborts.
2. **Rebuild**:
   ```
   cd E:/Projects/RogueSquadron64Recomp && cmake --build build --config Debug -j 4
   ```
3. **Smoke test** (30s):
   ```
   ./tools/run-stability.ps1 -Runs 1 -Timeout 30
   ```

## Stability state (project shelved)

- Boot completes without crashing. Reaches the attribution screen, then the N64 logo phase.
- Graphics: DLs are submitted but contain only framebuffer-clear commands (no content); see the README's "Where it actually stops" for why.
- Working set: ~370 MB peak (game code + RT64 + RDRAM).
- Audio: silent — ucode stubbed; the codec is **MORT** per [rerogue](https://github.com/jrra/rerogue) PC-version notes, not MusyX as previously assumed.
- Save: works in-memory; EEPROM writes stubbed.

## What's NOT yet renamed / understood

- ~2050 functions still as `func_XXXXXXXX` (out of ~3690 total).
- Big unmapped areas:
  - **Mission overlay** (`.ovl.mission`) — ~900 funcs, mostly per-craft update / per-level objectives.
  - **Cinematic overlay** (`.ovl.cinematic`) — ~80 unnamed helpers (`func_800A6904`, `func_800A6CE4`, etc).
  - **Audio decoder** around 0x8009D7D0-0x8009DD7C (MORT codec area).
  - **HMP terrain** — `load_level_hmp` named, but inner helpers not.
  - **HOB/HMT relocators** — `meshdef0/1_offset_convert` named, but `func_80059A80`, `func_800596C4`, `func_800599C0`, `func_800599EC` (related ops) still unnamed.
  - **DMA mutex** — `func_800058C0`, `func_800072AC` purpose unclear despite multiple call-site analyses.

## How to extend this document

1. When renaming a function: look up the subsystem it belongs to and add it to the matching table.
2. When discovering a new subsystem: add a new numbered section under "Subsystems".
3. When mapping a global address: add to "Memory map" table.
4. Keep entries terse — one line per function/global, link out to `E:/Projects/rogue_squadron64/docs/` for deeper specs.

## Host-side integration audit (2026-05-11)

Cross-check: does the host glue in `src/` correctly interpret what the game expects, given everything documented above?

### Seam 1 — SP task routing (M_GFXTASK split)

**Game expectation**: `osSpTaskStartGo(task)` with `task.type == M_GFXTASK (1)` should dispatch to the Factor 5 graphics ucode; `M_AUDTASK (2)` should dispatch to MusyX-derived audio ucode.

**Host implementation**:
- [src/main/upstream_compat.cpp:178](src/main/upstream_compat.cpp#L178) intercepts `osSpTaskStartGo_recomp` and forwards to `ultramodern::submit_rsp_task`.
- [lib/N64ModernRuntime/ultramodern/src/events.cpp:569](lib/N64ModernRuntime/ultramodern/src/events.cpp#L569) routes `M_GFXTASK` → `action_queue → gfx_thread_func → send_dl → RT64 processDisplayLists(isHLE=true)`. Everything else → `sp_task_queue → task_thread_func → rsp::run_task` (LLE).
- [src/main/main.cpp:239](src/main/main.cpp#L239) `get_rsp_microcode`: M_GFXTASK→`factor5_gfx_runner`, M_AUDTASK→`musyx_stub`, default→`unknown_task_stub`.

**Verdict**: ✅ correct. GFX never falls through to `factor5_gfx_runner` because the queue split happens before `run_task` is called. Audio is silently stubbed (intentional — no MusyX recomp yet).

### Seam 2 — Display-list submission (HLE vs LLE)

**Game expectation**: One DL per "submitGfxFrame" call, originating from the screen handler (cinematic/menu/in-mission). FullSync at end-of-DL signals frame complete.

**Host implementation**:
- HLE path active. RT64's F3DFACTOR5 GBI registered. `send_dl` → `processDisplayLists(isHLE=true)`.
- LLE path ([src/rsp/dpc_bridge.cpp:199](src/rsp/dpc_bridge.cpp#L199) `rsp_dpc_submit`) is **dormant** under HLE. Only invoked via `RSP_DPC_END` macro from inside `factor5_gfx_runner`, which doesn't run for M_GFXTASK any more.

**Verdict**: ✅ no double-submit. LLE infrastructure is dead code for GFX but kept for non-GFX RSP work (none currently). Could be deleted later but is harmless.

### Seam 3 — VI register pointer wiring

**Game expectation**: RT64 reads VI scanout state via 14 register pointers in `RT64::Application::Core`. Pointers must stay valid for the renderer's lifetime and reflect live VI state at every `update_screen`.

**Host implementation** ([src/main/rt64_render_context.cpp:87-101](src/main/rt64_render_context.cpp#L87)):
```
ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
appCore.VI_STATUS_REG = &vi->VI_STATUS_REG;  // ...all 14 regs...
```
`get_vi_regs` returns `&events_context.vi.update_screen_regs`. The `update_screen_regs` struct is assigned from each `ScreenUpdateAction` ([events.cpp:396](lib/N64ModernRuntime/ultramodern/src/events.cpp#L396)) just before `update_screen()` is called on the gfx thread.

**Verdict**: ✅ correct. Matches Zelda64Recomp's wiring pattern.

### Seam 4 — ScreenUpdateAction timing

**Game expectation**: Each VI retrace, RT64 should see the registers that the game programmed for *this* scanout.

**Host implementation** ([events.cpp:219-224](lib/N64ModernRuntime/ultramodern/src/events.cpp#L219)):
```
events_context.action_queue.enqueue(ScreenUpdateAction{ events_context.vi.regs });   // snapshot
events_context.vi.update_vi();                                                       // rotate next→cur
```
The snapshot is taken **before** `update_vi`. That means each `ScreenUpdateAction` carries the registers that were live during the *previous* scanout (which the game wrote one frame ago).

**Empirical investigation (2026-05-11)**: Ran with `ROGUESQ_LOG_PRESENT=1` and inspected the existing `[vi] update_screen #N ... pq.wc=X wq.wc=Y` log line. Observations across a 20s run:
- `wq.wc` (workloadQueue write cursor): advances from 0 → 3 within the first ~400 VIs and stays there.
- `pq.wc` (presentQueue write cursor): **stays at 0 for the entire run**.
- `fs` (cumulative LLE fullSyncs through dpc_bridge): **stays at 0** (consistent with Seam 2 — dpc_bridge is dormant under HLE).

Workloads are being built but **no presents land in the presentQueue**. The 1-frame lag in this seam is not the root cause — even with the lag, presents should still eventually fire if PresentEarly is doing its job.

**Verdict**: ⚠ upstream behavior preserved, but the bigger problem is RT64 isn't pushing presents at all. See "Black-screen synthesis" at the end of this audit.

### Seam 5 — `osViSwapBuffer_recomp` duplicate definition

**Initial observation**: Both [lib/N64ModernRuntime/librecomp/src/vi.cpp:37](lib/N64ModernRuntime/librecomp/src/vi.cpp#L37) and [src/main/upstream_compat.cpp:65](src/main/upstream_compat.cpp#L65) define `osViSwapBuffer_recomp`.

**Empirical investigation (2026-05-11)**: The build emits 7 such `lld-link : warning : duplicate symbol` warnings — for `osViSwapBuffer_recomp`, `osYieldThread_recomp`, `osSendMesg_recomp`, `osSpTaskStartGo_recomp`, `osStopThread_recomp`, `zmemcpy`, and `func_80007D74`. This is a **deliberate project convention** using `/FORCE:MULTIPLE` to override upstream symbols. The override that wins (confirmed by `[osViSwapBuffer #N]` log lines appearing in run output) is always ours, because our object files are linked before `lib/N64ModernRuntime`. The pattern is also used by [patches/heap_guards.c](patches/heap_guards.c) and called out in [src/main/upstream_compat.cpp:57](src/main/upstream_compat.cpp#L57) inline comments.

**Verdict**: ✅ not a mismatch — deliberate convention. The "fragile" verdict in earlier drafts overstated the risk. The single concrete improvement would be silencing the warning via a linker flag for the documented duplicates; not urgent.

### Seam 6 — Buffer-arbiter non-interference

**Game expectation**: The 6-state buffer arbiter at `0x80138E98[]`/`0x80138EAA[]` is owned entirely by the game; only `viRetraceHandlerThread`, `bufferArbiterAllocSlot`, `bufferArbiterMarkSlotReady`, and `bufferArbiterProducerMark` mutate it.

**Audit**: grep for `0x80138E98` / `0x80138EAA` / `bufferArbiter` across `src/` and `patches/` returns zero matches.

**Verdict**: ✅ host doesn't touch the arbiter. Good.

### Seam 7 — PresentEarly mode-agnostic enablement

**Game does** rotate fbs via `osViSwapBuffer` (run-1 log: `0x806DD000` ↔ `0x806BA000` ping-pong during boot, then transitions to a fixed `0x8062B800` during cinematic phase).

**Host comment in [rt64_render_context.cpp:117](src/main/rt64_render_context.cpp#L117)** says "our cinematic stays on a single VI fb address" — that's correct for the *cinematic* phase but not for boot. PresentEarly mode is enabled unconditionally.

**Empirical investigation (2026-05-11)**: ran a 45s stability run with `ROGUESQ_HLE_PRESENT_EARLY=0` and compared peak Working Set vs the default (PresentEarly enabled):

| Mode | Peak WS | Peak Private Bytes | Notes |
|------|---------|--------------------|-------|
| PresentEarly = 1 (default) | 391 MB | 4611 MB | steady-state |
| PresentEarly = 0 | **2599 MB** | **7256 MB** | 6.6× WS growth |

PresentEarly is **load-bearing**, not a phase-specific hack. Without it, RT64 accumulates workloads/framebuffer pairs that can't get released through the VI-match path, and memory grows unbounded — exactly the pre-pivot pattern that produced the iter-810 "freeze." The phase-agnostic enablement is the right call; the inline comment understates *why*.

**Verdict**: ✅ correct (and load-bearing). Comment in [rt64_render_context.cpp:117](src/main/rt64_render_context.cpp#L117) should be updated to reflect that this is required for memory bounds in all phases, not just cinematic.

### Seam 8 — Audio (silent)

**Game expectation**: M_AUDTASK runs Factor 5 MusyX ucode, fills AI buffer; ultramodern's audio callbacks push samples to SDL.

**Host implementation**: M_AUDTASK → `musyx_stub` → returns `Broke` immediately → `sp_complete` fires. Game thinks task ran; no actual audio.

**Verdict**: ✅ intentional. Documented in `docs/factor5-gbi.md`. Note: per [rerogue](https://github.com/jrra/rerogue) PC-version reversing, the audio codec is **MORT**, not MusyX as previously assumed. Either codec needs a separate RSPRecomp pass against the audio ucode.

### Seam 9 — `osStopThread` semantics

**Game expectation**: `osStopThread` on a non-self target should remove it from any queue and mark it STOPPED, matching libultra.

**Host implementation** ([src/main/upstream_compat.cpp:305](src/main/upstream_compat.cpp#L305)): Overrides upstream's `assert(false)`-on-non-self with the actual libultra-equivalent semantics.

**Verdict**: ✅ correct. `mainBootstrapWorker` stops other threads during boot; without this override the recomp would abort.

### Audit summary

After empirical investigation, the host-side glue is **architecturally consistent** with what the game expects. The earlier "three mismatches" framing turned out wrong on inspection:

1. **Seam 5 (`osViSwapBuffer_recomp` duplicate)** — deliberate `/FORCE:MULTIPLE` override convention. Our version wins. ✅
2. **Seam 7 (PresentEarly always-on)** — load-bearing for memory bounds. Disabling it produces 6.6× WS growth. Comment in code understates importance. ✅
3. **Seam 4 (ScreenUpdateAction stale regs)** — upstream behavior; the 1-frame lag isn't the root cause because PresentEarly isn't VI-gated. The real problem is upstream of timing.

### Black-screen synthesis (superseded by overlay-dispatch finding)

This section previously concluded the bottleneck was inside RT64's PresentEarly handling — that turned out to be wrong. The actual root cause is upstream of rendering: the game's CPU code isn't producing any pixel data beyond the framebuffer clear because the menu overlay's distinct functions never execute. See the [README's "Where it actually stops"](../README.md#where-it-actually-stops) section for the full diagnosis.

The earlier observations remain accurate as data — workloads are built, presents are queued or not depending on configuration, RT64's pipeline produces faithful output of whatever it's given — but the framing was upside down. With `ROGUESQ_LOG_VI_FB_CONTENT=1` we can directly observe the VI framebuffer at `0x806BA000`: 71680 of 71680 pixels are `0x0001` (canonical N64 black). That's the game writing only a clear, with nothing else. No RT64 fix would have made the missing content appear.

The fix that's expected to actually move the needle is restoring the per-DMA `load_overlays(...)` call in `lib/N64ModernRuntime/librecomp/src/pi.cpp:do_dma` (it was in commit `e532b90`, removed in a later cleanup). With that in place, `loadOverlay(1)` and `loadOverlay(2)` would register the menu and cinematic overlays' functions into `func_map` at the moment their bytes DMA in, and direct calls to overlay-region functions would resolve correctly.

## See also

- `E:/Projects/rogue_squadron64/symbol_files/main_overlay.txt` — authoritative rename list (Functions section, "RogueSquadron64Recomp renames" block).
- `E:/Projects/rogue_squadron64/docs/` — per-subsystem detail (HOB/HMT/SND/DAT/save/menus specs + partial m2c decomp).
- `docs/factor5-gbi.md` — Factor 5 graphics microcode docs.
- `docs/factor5-ucode-dispatch.md` — LLE/HLE GFX path docs.
- `docs/debug-trace-env-vars.md` — ROGUESQ_LOG_* environment variables for runtime tracing.
