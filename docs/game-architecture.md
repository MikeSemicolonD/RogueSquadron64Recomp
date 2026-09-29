# Star Wars: Rogue Squadron 64 — Game Architecture

> Subsystem map covering both the *game* (how Rogue Squadron's N64 code is
> structured, for the renaming effort in `RecompiledFuncs/funcs_*.c`) and the
> *port* (how it bridges to RT64 + ultramodern). Current status is in
> [Current status & known blockers](#current-status--known-blockers).
>
> Symbol-name source of truth: `E:/Projects/rogue_squadron64/symbol_files/`
> (per-overlay `.txt`); unnamed globals use the splat `D_<ADDR>` convention.
> Hardware truth comes from Project64 RDRAM goldens and message-order traces
> (`tools/validate/`), and the graphics ucode grammar from its IMEM listing
> ([f5-model-dl-spec.md §7](f5-model-dl-spec.md)).

## At a glance

- **Engine**: in-house Factor 5 tooling (SN Systems toolchain, NOT SGI). Custom audio ucode (MusyX-derived). Custom graphics ucode ("F3DFACTOR5") with its own chunked display-list format and opcode set; a stock F3DEX parser misreads it.
- **OS**: stock libultra. Threads, message queues, timers, controllers, EEPROM, PI DMA.
- **Layout**: monolithic main code segment + 3 overlays (in-mission, mempak/menu, cinematic), all loaded at VA `0x800A5130`.
- **Boot sequence**: entry (`0x80000400`) → `main` (`0x8000161C`) → idle thread → `mainBootstrapWorker` (`0x80000C68`, priority 0xA).
- **Game loop**: `mainGameLoop` (`0x8003DFA0`) drives per-frame work; entered from `gBootConfig+0x04` ptr after bootstrap.
- **Frame protocol**: two VIs per frame on hardware. The game thread builds the list and waits for the previous task's SP-done and DP-done; the VI retrace thread swaps buffers and submits the graphics task to the RSP scheduler thread. The recomp runs this protocol as-is (no host-injected tokens) — see [Frame loop](#frame-loop-hardware-protocol).
- **Graphics path**: HLE — the graphics task is parsed by RT64's F3DFACTOR5 GBI module, which follows the ucode's chunk-fetch rules and feeds RT64's native transform (see [Rendering pipeline](#rendering-pipeline)).
- **Audio path**: LLE — the MusyX RSP synth is statically recompiled by RSPRecomp (`musyx_rsp.toml`) and runs on the host audio task thread; samples stream from the cartridge as on hardware (see [Audio pipeline](#audio-pipeline)).

## How the recompilation works (port shape)

The game's MIPS code is statically recompiled to C by **N64Recomp**: one C
function per game function, each a literal translation of the MIPS with the
original disassembly inline as comments (see any function in
`RecompiledFuncs/funcs_3.c` — e.g. `findAndZeroTableSlotMatching`). These run
against a `recomp_context` (the guest register file) and an 8 MB `rdram[]`
backing store via the `MEM_W` / `MEM_BU` accessors.

Two RSP microcodes are recompiled separately by **RSPRecomp** and run as host
functions on the SP-task thread: the Factor 5 graphics ucode (used only by the
dormant LLE path — see below) and the MusyX audio ucode (live).

The host glue lives in `src/`:

| File | Role |
|---|---|
| `src/main/main.cpp` | Entry point; SDL2 window/audio/input; `get_rsp_microcode` task dispatch (M_GFXTASK→gfx, M_AUDTASK→MusyX synth); minidump/watchdog infra |
| `src/main/rt64_render_context.cpp` | `RendererContext` subclass — `send_dl`, `update_screen`, VI-register wiring, fb-registry sanitizer, RDRAM dump triggers |
| `src/main/register_overlays.cpp` | Registers all overlay function tables at boot (`recomp::overlays::register_overlays`) |
| `src/main/upstream_compat.cpp` | `/FORCE:MULTIPLE` overrides of libultra shims (`osRecvMesg`, `osSendMesg`, `osYieldThread`, `osDestroyThread`, `osInitialize`, `osViBlack`, `osStopThread`, …), `rs64_load_overlay`, the message trace, the watchdog with all-thread stack dumps |
| `src/rsp/dpc_bridge.cpp` | DPC_START/DPC_END LLE→RT64 bridge (dormant for GFX under HLE; kept for non-GFX RSP work) |
| `patches/` | Hand-written C cross-compiled to MIPS, run back through N64Recomp, linked *ahead* of `RecompiledFuncs/` so its symbols win — the regen-safe place for guards/overrides |

`lib/rt64` and `lib/N64ModernRuntime` are MikeSemicolonD forks carrying the
Factor 5 GBI backend and a handful of runtime hooks; see
[docs/audit-changes.md](audit-changes.md) for a full inventory of every fork
modification.

## Memory map (key globals)

Names are the recomp symbols where they exist; `D_<ADDR>` denotes a global the
recomp accesses via raw address arithmetic with no assigned symbol.

| Addr | Name | Purpose |
|------|------|---------|
| 0x80037560 | `gBootConfig` | 0x50-byte boot config struct; +0x04 = game-loop entry ptr |
| 0x8009FCD0 | `.samp` base ptr | Sample-bank base pointer; 0 on hardware and in the recomp (samples stream from the cartridge) |
| 0x800AF92C | `gCutsceneAssetPaths` | 32 × 0x5C cutscene asset path entries |
| 0x800B1900 | `gCutsceneEntries` | In-mem cutscene entry list (per `unk13D8_active_count`) |
| 0x800B1904 | `gCurrentCutsceneFile` | Active cutscene file ptr (0x25AC bytes + variable Vec3f list) |
| 0x80110741 | `D_80110741` | Serial Interface busy flag; spin-waited by `waitForSerialIdle` until it clears |
| 0x80110A80 | `gManifestTable` | Per-segment {dir RAM ptr +0x40, segment ROM base +0x44, count +0x48} array (data + dbg_data) |
| 0x80110AA0 | `D_80110AA0` | 80-byte path-prefix table for manifest name lookup (within the `gManifestTable` region) |
| 0x80111244 | `D_80111244` | Head index into the 8-slot × 116B event ring at `D_80111D60` |
| 0x80111BC0 | `D_80111BC0` | 16 × 24B event-subscriber slots (claimed by `subscribeEventHandler`) |
| 0x80111D60 | `D_80111D60` | 8-slot × 116B event ring buffer (producer `pushEventToRingBuffer`) |
| 0x801128D0 | `D_801128D0` | Service-worker registry linked list: head (+0x00) / tail (+0x04) / count (+0x08) |
| 0x80128CF0 | `D_80128CF0` | Video queue (one slot): `viRetraceHandlerThread` sends it each VI when a consumer is waiting; received by `waitForPostSwapAck` (game thread) and `waitOnVideoQueue` (video path) |
| 0x80128F08 | `D_80128F08` | Ptr to the global texture registry (0x24-byte entries: W@8, H@0xA, size@0xC, data*@0x10, name[16]@0x14) |
| 0x80130B40 | `gGameSettings` | 0x30-byte game state struct (level, craft, cheats, volumes, etc) |
| 0x80130B70 | `gCurrentLevel` | Current level enum (0..0x14) |
| 0x80130BB0 | `gNpcSlotList` | 0x800-entry NPC slot list (8-byte entries) |
| 0x80130BB8 | `gNpcContextArray` | 0xC0 × 0x3C-byte NPC contexts, `update_func` at +0x00 |
| 0x80128E98 | `D_80128E98` | Per-slot framebuffer pointers (the swap arbiter); recomp reaches it as `lui 0x8013, -0x7168` |
| 0x80128EAA | `D_80128EAA` | Per-slot 6-state buffer-arbiter state bytes (`lui 0x8013, -0x7156`); host reads them for diagnostics only |
| 0x80139560 | `gActiveSlots` | Active NPC-slot index array (halfwords, ~32 entries); per-scene slot pool, read by cinematic dispatch (`cinematicSlotBatchDispatch`) |
| 0x80149710 | `D_80149710` | 20-slot DSP voice table `musyxSynthFrame` fills each VI from active channels |
| 0x80149A00 | `D_80149A00` | 8 × 0xEF8 MusyX song slots (`startSongSequence` / `tickActiveSongs`) |
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

Per-thread message-queue workers, registry `D_801128D0` (linked list).

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
| `saveServiceWorker` (0x8006F2CC) | Service-worker thread (key=auto, slots=4) |
| `registerSaveService` (0x8006EF98) | `registerServiceWorker` for save thread |
| `triggerSaveMessage` (0x8006F274) | Allocates 20B msg, sends to save worker |
| `saveLoadDispatcher` (0x80006798) | Inside save thread: SAVE=1 (adler32+EEPROM write), LOAD=2 |
| `initSaveData` (0x80006338) | EEPROM sanity check + reinit on mismatch; uses `gamesave_asset` |
| `writeSaveBodyToEeprom` (0x80005B9C) | Writes `save_data_body` (0xC8 bytes) |
| `writeSaveDataBodyToEeprom` (0x80005F18) | Writes `gSaveDataBody` content |
| `copySaveUnk08` (0x80006198) | Copies `save_data_body.unk08[4]` (purpose unclear) |
| `loadAccountDataIntoStruct` (0x8006EAAC) | Copies account bytes from `gGameSettings+0x7` (0x80130B47) → `D_80145AB0` |
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

EEPROM save works (4K via librecomp). The controller Memory Pak path is stubbed (returns no-pak).

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
| `load_asset_with_malloc_flags` | High-level: get_asset_size_extra → rs_malloc → find → DMA into buffer |
| `loadDebugAssets` (0x800246E8) | Loads "dbg_data"/"dbg_font" at boot |
| `processOverlayDmaStruct` (0x800033A0) | Handles overlay-DMA struct (src/dest/sizes + BSS zero-fill) |
| `loadOverlay` (0x80000B20) | High-level overlay loader (in-mission / mempak / cinematic) |
| `piDmaWorker` (0x80005570) | PI DMA service worker; 256 × 20B ring `D_801112B0` |

`zmemcpy` is host-implemented in `upstream_compat.cpp` (the recomp's stub would otherwise zero all assets).

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

**HOB** (model) format: object → meshdef0 (`+0x18`, 0x4C linked-list) → meshdef0-prelude (`+0x1C`) → meshdef1 (`md0+0x10`) → facegroup → face. Vertex = 8 bytes (x,y,z s16 + pad). Face = flags + indices; `face+0x8` (u16) = material index. The HOB object's `+0x44` points at the material table (`D_8013889C` for that load).

**HMT** (material/texture) format:
- header: material_count u16, pad, texture_offset u32.
- material_entry (0x24 B) at HMT+8 + idx*0x24: flags u16 (bit0=has texture), texture_index u16, … name[16].
- texture section at HMT+texture_offset: count u32, then texture_entry (0x34 B): pixel_offset[8] (mip levels), palette_offset, image_name_offset, W u16, H u16, bit_depth u8, flags_type u16, transparency_color. image_type = flags_type & 0xF (low nibble): 0=CI4, 1=CI8, 2=RGBA5551, 3=RGBA32, 4=greyscale (I4/I8 by flag bit), 5=greyscale16.

HOB/HMT helpers: `meshdef0_offset_convert` (0x80058948), `meshdef1_offset_convert` (0x800587F0), `applyMeshdef1AnimColors` (0x80058B40), `walkMeshdef0List` (0x80056EB0), `registerHmtTextureInTable` (0x80022B90), `parseHmtMaterials` (0x80022A00).

zlib decompressor (embedded v0.99-1.08): `adler32` (0x800269B0), `inflateInit2_` (0x80026C7C), `inflate_blocks_new` (0x800276C4), `inflate_trees_free` (0x80029A68); memory bindings `rs_zcalloc`/`rs_zcfree`/`rs_memset` (prefixed `rs_` by splat).

### 6. Display / video

| Function | Role |
|----------|------|
| `setDisplayMode` (0x8008EA14) | Validates mode<0x21, stores the mode byte at `D_80141AD0`, calls inner setup chain |
| `getViModeType` / `setViModeType` / `getViModePeriod` / `isViModeTypePal` | VI mode getters/setters |

`osViBlack` is overridden to a no-op in the runtime: the game calls `osViBlack(1)` but never `osViBlack(0)`, which would otherwise leave the VI stuck in `VI_STATE_BLACK` and present nothing.

Video-mode changes (menu<->menu, menu<->mission) go through `advanceVideoFrame` (0x8001818C) and blank themselves without `osViBlack`:
1. `swapBufferWithViMode` applies a copy of the current mode with vStart == vEnd (zero active lines).
2. `initVideoSubsystem` (0x8001A098) re-partitions the framebuffers top-down and zeroes the first new buffer on the CPU (memset at 0x8001A2E4).
3. It swaps to that zeroed buffer.
4. The new mode is set.

Buffers of one mode reuse addresses of another. For example, a 512x448 no-Z menu buffer at 0x790000 covers the mission Z buffer. So RT64 can still hold an old target, possibly depth, at the zeroed address.

The host mirrors the blanking:
- `apply_vi_overrides` zeroes hStart for the zero-height mode.
- A `rogue_squadron.toml` hook at 0x8001A2EC arms a present-side gate that shows black while the VI shows the zeroed buffer and no later workload has drawn it.
- The present never converts a depth buffer to color.

`ROGUESQ_LOG_TRANSITION=1` traces it.

### 7. Audio subsystem (Factor 5 / MusyX)

Custom Factor 5 audio ucode, MusyX-derived; see [Audio pipeline](#audio-pipeline)
for the host integration. The per-frame synth tick (`musyxSynthFrame`,
0x8009123C) is **not** a thread of its own: it is one of the four SI callbacks
in the table at `D_8011A8A4` (`registerSiCallback`, 0x80007910) that
`viRetraceHandlerThread` runs after every VI retrace, next to
`wakeSerialThreadOnSi` and `serviceVoicePoolFrame`. The only dedicated audio
thread is the AI stream loop (`runAiAudioStreamPlaybackLoop`, 0x8008ED70,
priority 122), which refills the output buffer on each AI event.

| Function | Role |
|----------|------|
| `initAudioSubsystem` (0x80091B3C) | Inits mutex + osAiSetFrequency + audio queues/events; called from setDisplayMode |
| `shutdownMusyXEngine` / `func_8008ED00` | MusyX shutdown, called on every display-mode change: waits for the audio task, unregisters `musyxSynthFrame` from the SI table, frees the mixer. It was a toml stub until 2026-09-07; stubbing it stacked the synth callback three times and starved the voice-pool callback |
| `initFactor5Mutex` (0x80091FC4) | Creates queue `D_80149990` + posts initial token |
| `factor5MutexAcquire` (0x80092010) / `factor5MutexRelease` (0x8009205C) | Recursive mutex on `D_80149990`; hardware takes it ~7×/frame from the game thread. **Still toml stubs in the recomp** (re-stubbed after an early deadlock; candidate to restore now that `osYieldThread` is a real yield) |
| `loadSndFiles` (0x800663B0) | Loads the SND files (pool/proj/sdir); the sample bank stays on the cartridge |
| `parseSndFiles` (0x80097518) | Parses SND file sections |
| `tickAudioSequencerFrame` (0x8008D960) | Per-frame: computes time delta + `tickAllAudioChannels` (0x8008C9AC) → `tickAudioChannel` |
| `tickAudioChannel` | 81-entry fn-ptr dispatch; drives the active music/SFX channels (`D_8013D880`) |
| `musyxMixActiveVoices` (0x80090E04) | Per-frame DSP cmd builder: iterates the 20-slot voice table `D_80149710`, calls the per-voice mixer |
| `musyxRenderVoiceSamples` (0x8008FFAC) | Per-voice mixer (pitch/sample-pos, emits DSP descriptors, queues cartridge sample DMAs) |
| `musyxKeyOnVoice` (0x80090A3C) | Synth voice key-on; un-stubbed by default so music voices flow |
| `postViThreadCommand` (0x8001A058) | Sends the audio task (type 2) to the RSP scheduler queue `D_8011A420`, blocking; the scheduler answers on `D_8011A800` |

### 7a. Frame handshake helpers

Three helpers around the swap handshake, used by the frame loop (see
[Frame loop](#frame-loop-hardware-protocol) for where they sit):

| Function | Role |
|----------|------|
| `setPostSwapPendingFlags` (0x8001C244) | Sets both bytes at `D_8011A8C8` (swap requested) / `D_8011A8C9` (ack pending) to 1 |
| `waitForPostSwapAck` (0x8001C260) | At the start of a frame: sets the "consumer waiting" flag `D_80037808`, receives the video queue `D_80128CF0` (blocking), yields, and repeats until `D_8011A8C9` clears — two receives per frame on hardware |
| `clearPostSwapPendingFlags` (0x8001C2F8) | Clears both flag bytes to 0 |

### 8. Controllers

| Function | Role |
|----------|------|
| `siServiceThread` (0x80002E10) | Controller polling thread |
| `initSiQueue` (0x80003308) | Initializes SI message queue + flags |
| `waitForSerialIdle` (0x80003284) | Spin-yields until SI busy flag `D_80110741` clears |
| `getControllerButtonAndStick` (0x80079D54) | Reads button + stick state |
| `getControllerStickXPercentage` (0x8007A068) / `getControllerStickYPercentage` (0x8007A0A8) | stick / 128 → percent |
| `setNewAndPreviousButtonsPressed` (0x80079CE0) | Sets `D_8013A950` (prev) + `D_8013A960` (new) |
| `readControllerInputs` (0x80079F20) | Inner fetcher |
| `getControllerNewButtonsPressed` (0x80079F50) | Returns `D_8013A960[idx]` |
| `initControllerSettingsStructs` (0x800BCE2C) / `controllerSettingsScreen` (0x800BD274) | Controller settings UI |

Controller settings — 4 named profiles: LUKE / WEDGE / JANSON / HOBBIE, 18 inputs each. SDL2 gamepad input works; the host can fake a controller for headless runs (`ROGUESQ_FAKE_CONTROLLER=1`, with `ROGUESQ_AUTO_START=<ms>` to pulse START) so automated runs clear the "NO CONTROLLER" gate. Keyboard support is not implemented.

### 9. Menus

Main menu state `gCurrentMenuData` (0x800CE730, 0xF8 bytes). 13 menus enumerated (MAIN_MENU through AT_THE_MOVIES).

| Function | Role |
|----------|------|
| `menuOverlayInit` (0x800C58A0) | Menu overlay init (calls registerSaveService) |
| `menuControllerInput` (0x800B47D0) | Big menu controller input handler |
| `setupMenuData` (0x800BA0F0) | Sets up `gCurrentMenuData` per current menu |
| `updateMenuPerFrame` (0x800BB394) | Per-frame menu update (highlighting etc) |
| `handleGameSettingsMenu` (0x800BB234) | Game settings menu input |
| `readAccountForSelectionScreen` (0x800B85B4) | Account selection screen |
| `eliteRoguesMenuHandler` (0x800BEA00) | Elite Rogues high-score screen |

Menu UI text is G_TEXRECT (one textured rect per IA16 glyph); the title strip ("STAR WARS / ROGUE SQUADRON") and the photographic background tiles decode and draw. Per-tile positioning is open (see [Rendering pipeline](#rendering-pipeline)).

To add or modify menu entries (front-end or pause menu), see
[adding-menus-and-buttons.md](adding-menus-and-buttons.md) — the entry data
model, sub-type dispatch, and a worked "Quit button" example.

### 10. HUD

Two double-buffered HUD structs `D_8010CA30[2]` (0x278 bytes each). Material table (`D_8011A444`) → texture table (`D_80128F08`, 0x24B per entry).

| Function | Role |
|----------|------|
| `initHudStruct` (0x800C0084) | Initializes 0xF80-byte HUD struct |
| `hudDisplayUpdateWorker` (0x800495FC) | Service worker; processes HUD material/texture msgs |
| `getTextureDataByMaterialId` (0x800232F8) | material_id → D_8011A444 → D_80128F08 texture ptr |
| `setHudSecondaryWeaponInfo` (0x800BFDC4) | Sets secondary weapon type/level fields |

### 11. NPC / slot dispatcher

0x800-entry slot list `gNpcSlotList`, each entry → 0xC0-entry NPC context array. Update function pointer at NPC context +0x00 runs every frame.

| Function | Role |
|----------|------|
| `slotDispatcherIter` (0x8003E8DC) | Walks gNpcSlotList, calls each NPC update_func |
| `slotDispatcherInner` (0x8003EA4C) | Per-slot dispatch; mallocs `D_80130BC0`/`D_80130BC4` entries |
| `slotEffectHandlerDispatch` (0x800A71B8) | Effect handler dispatch from cinematic loop |
| `allocNpcContextArrays` (0x8003FD54) | Mallocs gNpcContextArray + ptrs |
| `setupNpcUpdateFunctions` (0x800653B4) | Sets update fn ptrs (DAT Type 6 setup) |

### 12. Cinematic system

Multi-stage cutscene playback. State ptr `D_800B0934`, stage counter `D_800B0938`. Active slot indices in `gActiveSlots` (0x80139560). The cinematic is a scene graph, not a flat list: `cinematicLoopBody` → `traverseSceneGraphRecursive` (0x80015548) → `processSceneNode` (0x80014FA0); geometry is RSP T&L over meshdef1→facegroup→face.

| Function | Role |
|----------|------|
| `cinematicLoopBody` (0x800A5D80) | Inner cinematic frame loop |
| `cinematicInitializer` (0x800AF408) | Sets state ptr `D_800B0934` |
| `cinematicStageAdvancer` (0x800AF550) | Per-frame stage counter advance |
| `cinematicDeactivator` (0x800AF60C) | Clears cinematic state |
| `cinematicComputeDt` (0x800AF360) | Per-frame delta-time computer |
| `cinematicSlotBatchDispatch` (0x800A70E4) | Calls slotDispatcherIter for each active cinematic slot |
| `load_cutscene` (0x800A6620) | Loads cutscene file (0x25AC fixed + variable Vec3f list) |

Cutscene file structure: `cuts_file_constant` (0x25AC) containing filename, 200 cuts_0058_type entries (0x18 each), 6 cuts_1318_type, 60 cuts_13D8_type (0x4C asset entries with flags), followed by variable Vec3f list.

### 13. DAT files (in-mission level data)

5 data types: Type 0 (spawn/building/props), Type 2 (unknown, Sullust LAVAFART##), Type 3 (splines), Type 6 (LOD boxes + MUSICRNG), Type 7 (event triggers).

| Function | Role |
|----------|------|
| `parseDatSpawnPositions` (0x80065790) | Parses DAT Type 0 |
| `parseDatEventTriggers` (0x800AA870) | Parses DAT Type 7 |
| `parseDatItemCommon` (0x80046620) | Common helper (Type 0 + Type 7) |
| `setupNpcUpdateFunctions` (0x800653B4) | NPC update fn setup (DAT Type 6) |

Enemies fly spline paths — large NPC functions are spline-walkers, not physics integrators. AT-AT / World Devastator leg articulation is phase-driven (`computeWalkerLimbStrideTargets`, 0x8007602C).

### 14. Cheat codes

Stored as CRC32 hashes in `gCheatCodeCrc32Table` (0x800A0ED0). Active flags in `gActiveCheatFlags` (0x80130B58). Seed = 0xFAC5FAC5 ("FAC5" = Factor 5). 30 cheats total.

| Function | Role |
|----------|------|
| `rs_crc32` (0x800824F8) / `make_crc32_lut` (0x80082544) | Reflected CRC32 |
| `decrypt_ns_hmt` (0x8006AFC0) / `load_naboo_starfighter` (0x8006C780) | Naboo Starfighter (cheat-unlock craft) HMT decrypt + load |

### 15. Text subsystem

3 categories: Front (menus), Game (in-mission), Voice (subtitles). Header at `txtFileHeader` (0x80138E60). All text XOR-obfuscated with a rolling key seeded at 0xF5.

| Function | Role |
|----------|------|
| `loadTxtFile` (0x800556A0) | Loads/decrypts text file (Front/Game) |
| `getVoiceText` (0x80055978) | Voice text decrypt (forces uppercase) |
| `getGameOrFrontText` (0x8005589C) | Get string by ID (Front/Game) |

Subtitles: queue `subtitleSlots` (0x80139BB0), voiceId→textId map `voiceIdtoTextIdMap` (0x8009FFE0, 756 entries). `drawSubtitleText` (0x800159B4). The voice/speech ROM data uses the MORT codec (the same codec the PC version uses); `tools/extract_speech_table.py` extracts the table (419 MORT clips + 549 strings + voiceId map).

### 16. Player crafts

9 craft types (XWING, YWING, AWING, VWING, SNOWSPEEDER, FALCON, TIEINTER, T16, KOELSCH). Selection at byte `gGameSettings+0x1` (0x80130B41). Per-level default `dDefaultCraftForLevel` (0x8009EC50).

| Function | Role |
|----------|------|
| `choosePlayerCraftAssets` (0x800FB6C0) | DMAs HMT/HOB for chosen craft |
| `mallocCraftSelectionData` (0x800A9364) | Mallocs `gHangarBayDescriptorArrayPtr` (0x800CDA5C) selection structs (56 entries) |
| `getAvailableShipsForLevel` (0x800C63C0) | Returns bitmask of available ships per level |
| `getSecondaryWeaponForCraftLevel` (0x800C6728) / `getSecondaryWeaponCount` (0x8006F43C) | Secondary weapon ID / max count |
| `playerXwingUpdate` (0x800B5434) | X-Wing per-frame update (one of many craft-specific) |

### 17. Mission objectives

3 tracking primitives: 128 booleans (`gObjectiveBooleans`, 0x801388A0), 128 counts (`gObjectiveCounts`, 0x80138060), 8 timers (`gObjectiveTimers`, 0x8010C9F8). 0x30-entry `simpleCheckHandles` (0x8010C6E0) array. Per-level dispatch via `gMissionObjectiveVtable` (0x8010A450) — 4 fn ptrs × 21 levels, all named `lvN_*`.

### 18. Image/texture handling

| Function | Role |
|----------|------|
| `parseImageFile` (0x8001EB24) | Big switch on `image_type & 0xF` + flags |
| `loadTextureToMemory` (0x8001F954) | Decodes a texture into `D_80128F08` chain; for CI formats, appends the palette at the end of the decoded data |
| `registerHmtTextureInTable` (0x80022B90) | HMT per-texture index → global D_80128F08 index (via D_8011A444) |

---

## Rendering pipeline

> The game's graphics ucode is F3DFACTOR5 — Factor 5's custom GBI. Its display
> lists are 0x108-byte chunks in a doubly linked pool, with Factor 5's own
> opcodes sharing byte values with F3DEX (`0x04` = vertex DMA, `0xBF` = triangle,
> `0xB4`/`0x13` = quad, `0x05` = terrain record, `0xB5` = next chunk …), so a
> stock F3DEX parser misreads the stream. RT64 dispatches it through the custom
> GBI module in `lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp` (geometry and chunk
> flow), `rt64_gbi_f5_rdpstate.cpp` (RDP state, the color-image filter) and
> `rt64_gbi_f5_diag.cpp` (desync ring). The grammar below was read off the
> ucode's IMEM listing on 2026-09-07 and is validated offline by
> `tools/validate/f5_dl_walk.py`, which walks any RDRAM dump's task list with
> the same rules (every Project64 golden walks clean).

### Ucode facts the HLE reproduces

- **Fetch model.** The RSP DMAs one 0x108-byte chunk into DMEM `0x170` and executes from `+8`; the first 8 bytes are the chunk's next/prev pointers and are never executed (op `0x80` dispatches to IMEM 0). When the cursor reaches `+0x108`, or on `B5`/`0x12`, it fetches the chunk named by the current chunk's first word and continues at `+8`. `06` pushes `{chunk, cursor}` and fetches w1; `07` fetches w1 without pushing; `B8` pops or ends the task. `B5`'s own w1 is never read. Chunks in the allocated list point back to each other (next chunk's second word = current chunk).
- **Matrices.** `01`: 64-byte N64 matrix DMA'd from w1; byte 1 bit 0 selects modelview (DMEM `0x5D0`, then MVP = MV × P into `0x610`) or projection (`0x590`). Standard split int/frac layout, row-vector convention.
- **Vertices and colors.** `04`: `(w0>>10)&0x3F` 8-byte vertices (x, y, z int16, pad) DMA'd from w1. `02`: `(w0&0xFFFF)+1` bytes of RGBA colors DMA'd into `0xB70`.
- **Faces.** `BF`: w1 bytes 1..3 are vertex slots (byte/5 = index), the next word's bytes are per-vertex byte offsets into the color buffer, then flags; with `w0&2` a 16-byte block of (s,t) 8.8 texcoords follows (32 bytes total, else 16). `B4`/`13`: the same plus a fourth vertex from w1 byte 0, emitted as two triangles.
- **State.** `03`: 24 bytes; byte 1 picks a DMEM slot for the inline 16 bytes (`03 80` = viewport vscale/vtrans in 2-bit fixed, `03 82` = light/lookat). `14`, `BD`, `BE`: 16-byte state words. `E4`/`E5` texrects copy 16 bytes to the RDP.
- **Terrain (`05`).** 40-byte records handled by a ucode overlay. `05 05 02 ..` is a flat tile: corner heights in the first two words, corner colors in words 3–6, texcoord size in word 7, position in word 8, z/size in word 9. `05 05 00 ..` DMAs a height grid and color rows and tessellates them (overlays 0x14/0x18).
- **NDC is y-down.** The ucode adds `ndc × vscale` with no negation; the hardware frame-300 golden has the X-wing at the bottom where the un-negated math puts it at +0.8.

### What the HLE does with it

| Piece | Implementation |
|---|---|
| Chunk flow | Every opcode runs through a trampoline that checks the per-level chunk bound; `06`/`07`/`B5`/chunk-end enter the target at `+8` and record the chunk base per call depth. A next-link must be back-linked, may not revisit its own chain, and a chain is capped at 64 chunks per level (hardware never exceeds 19). Per-task budgets (`ROGUESQ_F5_ENTRY_CAP`=2048 chunk entries, `ROGUESQ_F5_FACE_CAP`=4096 faces) end a task that walks recycled memory |
| Transform | RT64's own matrix stack: `01 03` → projection load, `01 02` → modelview load. No software T&L |
| Vertices | `04` converts the 8-byte vertices into RT64 vertices in a scratch area at RDRAM `0xA00000` (above the 8 MB the game can see; the recomp heap uses `0x71E000`) and calls `setVertex` |
| Faces | `BF`/`B4`/`13` copy the referenced vertices into temp slots, apply the color offsets and inline texcoords, and call `drawIndexedTri` |
| Viewport | From the stream's `03 80` block (synthesized from the scissor only if the stream never sends one); `vscale.y` is negated so RT64's F3D y-flip cancels out (`ROGUESQ_F5_FLIP_Y=0` reverts) |
| Terrain | Flat `05 05 02` tiles are drawn as two triangles (`ROGUESQ_F5_TILES=0` disables); heightfield records are skipped (40 bytes) |
| RDP state | Combiner, other-mode, tiles, loads and images are taken from the stream as sent. `setColorImage_filtered` rejects garbage images (width ≤ 1, outside `[0x400000, 0x800000)`, non-standard widths) and, with `ROGUESQ_DESYNC_TRACE=1`, dumps the last commands that led there |

### Validation

- `tools/validate/f5_dl_walk.py dump.bin task` walks a task list offline with the ucode rules and reports faces, rects, unknown opcodes and runaway; `f5_dl_ndc.py` adds the matrices and prints where each face group lands in NDC.
- Project64 goldens (`tools/validate/capture_pj64_golden.ps1`, script `pj64_rs64_dump.js`) dump RDRAM at cinematic frames 1/78/79/120/300/600/900; the recomp's chunk pool was byte-identical to hardware at frame 78. Rendering the golden's framebuffer (RGBA16 at the arbiter's displayed slot) gives a reference image for the same frame.
- `ROGUESQ_LOG_GFX_TASK=1` logs each task's parse time, chunk transitions and faces; `ROGUESQ_LOG_GBI=1` adds per-handler logs.

### Rendered output

- Attribution screen: the legal text as IA16 glyph texrects on black (274 rects per frame, matching the hardware walk).
- Intro cinematic: TIE fighter, X-wing with lasers in the Death Star trench, the N64 logo exploding into debris, the Factor 5 letters assembling with the fire "5", flat terrain tiles and the grey sky. Missing: the heightfield surface tiles (gaps in the ground) and the fire/smoke billboards, which are the unimplemented `05` forms.
- Main menu: background tiles and title text.

## Frame loop (hardware protocol)

From the Project64 message-order trace of cinematic frames 60–70
(`tools/validate/compare_mesg_trace.py` shows the recomp emits the same
functions, queues and order). Two VIs per frame:

1. **Game thread** (`mainBootstrapWorker`, priority 10): `pollControllerInputs` → `waitForPostSwapAck` (two blocking receives on the video queue `D_80128CF0`, one per VI) → builds the display list into pool chunks → `submitGfxFrame` (0x8000C07C): blocking receive of SP-done on `D_8011A818` (only if `D_8011A89E` is set) … blocking receive of DP-done on `D_8011A7E8` (if `D_8011A89D`) → `bufferArbiterProducerMark` marks the buffer ready under the frame-sync mutex `D_80128D10` → `awaitFrameSyncMesgBlock`. The game thread never kicks the RSP task or swaps buffers itself.
2. **VI retrace thread** (`viRetraceHandlerThread`, priority 117), every VI: receive `D_80114388` → take `D_80128D10` → pick the oldest state-3 buffer, mark it 4, `osViSwapBuffer` → `postSwapRdpReset`: if a swap was requested (`D_8011A8C8`) set `D_8011A89D/9E` and send the graphics task (type 1) to the scheduler queue `D_8011A420` → release `D_80128D10` → if a consumer is waiting (`D_80037808`) send the video queue → yield → run the four SI callbacks (`musyxSynthFrame` among them, which blocks on audio-done `D_8011A800` and sends the audio task, type 2).
3. **RSP scheduler** (`spTaskSchedulerThread`, priority 122): one queue `D_8011A420` for task requests and the SP-done event (type `0x81`, registered with `osSetEventMesg`); a graphics task yields to an audio task; on SP-done it sends `D_8011A818` (graphics) or `D_8011A800` (audio).
4. **DP handler** (`dpInterruptHandlerThread`, priority 122): on `OS_EVENT_DP` forwards DP-done to `D_8011A7E8` (non-blocking) when `D_8011A89D` is set.

The recomp default (`ROGUESQ_VI_DRIVEN_LOOP`, on) runs exactly this: no host-injected tokens, no forced non-blocking receives, and SP-done delivered after RT64 has parsed the list (`ROGUESQ_SP_COMPLETE_AFTER_PARSE`, implied). Setting `ROGUESQ_VI_DRIVEN_LOOP=0` restores the old mode in which `rs64_vi_callback` posted SP-done/DP-done/VI/frame-sync tokens every VI and the barrier receives were forced non-blocking — that mode is the chunk-reuse race by construction. Remaining difference: the recomp spends three VIs per frame where hardware spends two.

### Buffer arbiter

`D_80128E98[]` holds the framebuffer pointers and `D_80128EAA[]` the per-slot state bytes (count at `D_80128EAD`):

| Value | Meaning | Writer |
|-------|---------|--------|
| `0` | FREE | `viRetraceHandlerThread` (5→0 once scanned out) |
| `1` | ALLOC | `bufferArbiterAllocSlot` (0x8001BF20) |
| `2` | READY | `bufferArbiterMarkSlotReady` (0x8001BEC8) |
| `3` | SUBMITTED | `bufferArbiterProducerMark` (0x8001BCE4), game thread after the DP-done wait |
| `4` | SWAPPED | `viRetraceHandlerThread`, just before `osViSwapBuffer` |
| `5` | DISPLAYED | `viRetraceHandlerThread`, next retrace |

The host never writes these bytes in the default mode (the old
`ROGUESQ_FORCE_BUFFER_PROGRESS` host consumer is gated off when VI-driven).

### Diagnostic env vars (rendering)

| Var | Effect |
|-----|--------|
| `ROGUESQ_F5_NATIVE=0` | Parse without emitting geometry |
| `ROGUESQ_F5_FLIP_Y=0` / `ROGUESQ_F5_TILES=0` | Revert the y-down viewport / disable flat terrain tiles |
| `ROGUESQ_F5_CHAIN_CAP` / `ROGUESQ_F5_ENTRY_CAP` / `ROGUESQ_F5_FACE_CAP` | Walk budgets (64 / 2048 / 4096) |
| `ROGUESQ_F5_CHUNK_BOUND=0` | Disable the per-level chunk bound (linear fall-through, as the old core did) |
| `ROGUESQ_DESYNC_TRACE=1` | Ring dump of the commands leading to a rejected color image |
| `ROGUESQ_LOG_GBI` / `ROGUESQ_LOG_GFX_TASK` / `ROGUESQ_LOG_CIMG` | Per-handler logs / per-task parse timing / color-image counters |
| `ROGUESQ_VI_DRIVEN_LOOP=0` | Old host-token frame loop (see above) |
| `ROGUESQ_FB_GUARDS=<mask>` | 1 = display-list neutralizer (off by default: it rewrote payload words), 2 = matpool repair, 4 = framebuffer-registry sanitizer (default 6) |
| `ROGUESQ_DUMP_RDRAM_ON_SCENE/ON_SCREEN/AT_VI/ON_CINE_ITER/ON_CINE_STALL` | RDRAM dumps for golden diffs (`tools/validate/rdram_golden_diff.py`) |
| `ROGUESQ_LOG_MESG_TRACE=1` (+`ROGUESQ_MESG_TRACE_FRAMES=lo-hi`) | Message-order trace in the PJ64 script's format for `compare_mesg_trace.py` |


---

## Audio pipeline

> The MusyX synth outputs SFX and music. MORT is the codec for the voice/speech
> samples and the PC version; the in-game music/SFX runs on the MusyX-derived RSP
> synth, recompiled here via RSPRecomp.

### Audio pipeline components

1. **Sample bank streams from cartridge (hardware truth, 2026-09-07).** The 2.94 MB `samp_SND` bank (ROM `0x2B60B8`, stored raw) is never loaded into RDRAM by the game. Project64 dumps at the start screen, the attribution screen, the cinematic and the main menu all show the bank absent, `.samp` base `0x8009FCD0` = 0, and the asset table holding the bank's cartridge address `0xB02B60B8`. Voice slots carry cartridge sample addresses (`0xB0000000 + ROM offset`); `relocateSndPointer` accepts both `0x80` and `0xB0` pointers; `musyxRenderVoiceSamples` → `fillSampleStreamRange` queues ranges into 0x600-byte stream blocks (`acquireSampleStreamBlock`), and a DMA pump issues 256-byte `osPiStartDma` cart→RDRAM transfers (a PI-register trace on PJ64 logs thousands of them during the cinematic). This path works unchanged in the recomp through librecomp's ROM read, with audio level equal to the old forced load and the heap layout matching hardware (`languageData` at `0x80194300` as on hardware, no allocations spilling above `0x400000`). The old forced load in `loadSndFiles` (`funcs_15.c` hand edit) is now opt-in via `ROGUESQ_SAMP_LOAD=1` and should be considered retired, along with the `musyx_stub` permanent-region relocation and the raised librecomp DMA read-bound.

2. **Voice key-on.** The synth voice key-on handler (`musyxKeyOnVoice`, 0x80090A3C) is un-stubbed by default (plus div-by-zero/period guards), so music voices transition to ready/counted and the DSP task build runs; re-stub with `ROGUESQ_STUB_VOICESTART=1`. This edit lives in the regen-fragile `RecompiledFuncs/funcs_*.c` and must be re-applied (or migrated to `patches/`) after any N64Recomp regeneration — the active renaming pass regenerates those files.

3. **Synth ucode task-data DMA.** The boot ucode DMAs the main synth text to IMEM 0x80, not IMEM 0, so `text_address` in `musyx_rsp.toml` is `0x04001080`; otherwise `jal` targets resolve to the wrong offset (the header-DMA call lands on the resampler instead of the DMA routine) and the synth mixes silence. The config:
   ```toml
   text_offset  = 0x9ABE0
   text_size    = 0xF80          # only 0xF80 bytes load to IMEM 0x80..0xFFF
   text_address = 0x04001080     # synth text loads to IMEM 0x80
   ```
   Regenerate with `RSPRecomp musyx_rsp.toml`, then `python tools/fixup_factor5_ucode.py build/factor5_ucode/musyx_audio_recompiled.c`, then rebuild. The synth then DMAs sample waveforms from the bank and the WAV output is dynamic (music and SFX).

### Host dispatch & flow

- `src/main/main.cpp` `get_rsp_microcode`: `M_AUDTASK` → `musyx_audio_runner` by default (opt out `ROGUESQ_NO_AUDIO_UCODE=1` to fall back to the silent `musyx_stub`). The runner DMAs the ucode to DMEM, writes the OSTask, runs `factor5_boot` then the recompiled `musyx_audio`.
- CPU side (`musyxSynthFrame`, 0x8009123C, run as an SI callback by the VI retrace thread each VI): `tickAudioSequencerFrame` drives channels each frame; `musyxMixActiveVoices` (0x80090E04) builds DSP descriptors from the 20-slot voice table `D_80149710`; `musyxBuildVoiceCommandList` (0x80091034) assembles the DSP task; key-on via `musyxKeyOnVoice`.
- PCM/WAV capture: `ROGUESQ_DUMP_PCM=<path>` writes a streaming 22050 Hz stereo s16 WAV of the real synth output (the output dir must exist). `ROGUESQ_RENDER_SONG=<key>` force-renders a specific song in-game (key 0 = `logo1_SNG`, the N64-logo music).

---

## Overlay loading

The game ships three overlays that all load at VA `0x800A5130` and swap in and
out at runtime: `.ovl.mission` (ROM 0xA5D30), `.ovl.menu` (ROM 0x10C2D0),
`.ovl.cinematic` (ROM 0x137580).

Overlay loading has two parts. (1) At boot, `rs64_register_overlays`
(`register_overlays.cpp`) registers all three overlays' function tables with
librecomp (`recomp::overlays::register_overlays`). (2) At runtime, a regen-safe
`[[patches.hook]]` in `rogue_squadron.toml` on the game's `loadOverlay`
(`before_vram = 0x80000B3C`, just past its already-loaded skip-check) calls
`rs64_load_overlay(overlay_id)` (`upstream_compat.cpp`), which unloads the shared
overlay region then loads the requested one by ROM offset so `func_map` points at
the live overlay's functions:

```c
case 0: load_overlays(0x000A5D30, 0x800A5130, 0x000665A0); // .ovl.mission
case 1: load_overlays(0x0010C2D0, 0x800A5130, 0x000283F0); // .ovl.menu
case 2: load_overlays(0x00137580, 0x800A5130, 0x0000B810); // .ovl.cinematic
```

The menu and cinematic overlays' distinct functions execute and the screens
render their content. Overlays are registered at boot only, matching
Zelda64Recomp.

---

## Threading topology

Priorities read from `__osActiveQueue` in a hardware dump (higher runs first):

| Thread | Priority | Purpose |
|--------|----------|---------|
| libultra VI manager (`viMgrMain`) | 254 | Forwards the retrace interrupt to `D_80114388` (HLE'd by ultramodern's VI thread) |
| `piDmaWorker` | 150 | PI DMA service worker; 256 × 20B ring `D_801112B0` |
| `spTaskSchedulerThread` (0x80019BF4) | 122 | RSP task dispatch (`osSpTaskLoad`/`StartGo`/`Yield`), one queue for requests and SP-done |
| `dpInterruptHandlerThread` (0x8001C328) | 122 | RDP-done handler (`OS_EVENT_DP`) |
| `runAiAudioStreamPlaybackLoop` (0x8008ED70) | 122 | AI buffer streaming |
| Service workers (save, HUD, event queue) | 120 | via `registerServiceWorker` |
| `siServiceThread` (0x80002E10) | 118 | Controller polling, SI-busy flag `D_80110741` |
| `viRetraceHandlerThread` (0x80019868) | 117 | Swap arbiter, task submission, the four SI callbacks (including the MusyX synth tick) |
| `mainBootstrapWorker` → `mainGameLoop` | 10 | The game thread: builds display lists, waits on the frame barriers |
| idle thread | 9 | Bootstrap only |

`initVideoSubsystem` (0x8001A098) creates the retrace, scheduler and DP threads.
ultramodern runs one N64 thread at a time and switches only at message-queue
sync points; `osYieldThread` is a real requeue-and-switch in this port (the
upstream host yield kept the scheduler slot and starved every other thread
during game spin loops). One known divergence: ultramodern queues an
equal-priority yielder ahead of its peers where libultra queues it behind —
harmless here because every thread has a distinct priority.

## Concurrency primitives

- **libultra `OSMesgQueue`** — inter-thread messaging everywhere. Key queues: `D_80114388` VI event, `D_80128D10` frame-sync mutex (one token), `D_80128CF0` video queue (one slot, two consumers: `waitForPostSwapAck` and `waitOnVideoQueue`, sent only when `D_80037808` says a consumer is waiting), `D_8011A420` RSP scheduler, `D_8011A818`/`D_8011A800` SP-done for graphics/audio, `D_8011A7E8` DP-done, `D_8011A408` DP event.
- **Factor 5 recursive mutex** — audio code; depth counter `D_800A1780`, queue `D_80149990`. Still stubbed in the recomp (see §7).
- **SI busy flag** `D_80110741` — spin-wait for serial idle (save vs controller).
- **Service-worker registry** `D_801128D0` — linked list of registered workers.
- **Buffer-arbiter state machine** `D_80128EAA[]` — producer/consumer between the game thread and `viRetraceHandlerThread` (see [Buffer arbiter](#buffer-arbiter)).

---

## Host-runtime bridge (game ↔ RT64)

```
spTaskSchedulerThread → osSpTaskStartGo → ultramodern::submit_rsp_task
   ├─ M_GFXTASK → action_queue → gfx_thread_func → send_dl
   │              → rt64_render_context.cpp::send_dl
   │              → RT64::Application::processDisplayLists (HLE)
   │              → F3DFACTOR5 handlers (rt64_gbi_f3dfactor5.cpp)
   │              → sp_complete() + dp_complete() after the parse
   └─ M_AUDTASK → get_rsp_microcode → musyx_audio_runner (LLE synth) → sp_complete()
```

`sp_complete`/`dp_complete` post the `0x81` SP-done event into the scheduler
queue and the DP event into `D_8011A408`; the scheduler and DP handler forward
them to the game thread as described in the [frame loop](#frame-loop-hardware-protocol).
SP-done is sent *after* the parse (upstream sends it before), because the game
recycles chunks once it sees SP-done.

For presentation, ultramodern's host `vi_thread` enqueues a `ScreenUpdateAction`
each VI tick and posts the VI event to the game's registered queue; the same
`gfx_thread_func` consumes the action and calls `update_screen()`, which
triggers RT64's present. RT64 reads VI scanout state via register pointers wired
in `rt64_render_context.cpp`. `PresentEarly` mode is on by default (opt out with
`ROGUESQ_HLE_PRESENT_EARLY=0`); without it RT64 accumulates unreleased workloads.

### Host-side integration seams

| Seam | Behavior |
|------|----------|
| SP task routing (M_GFXTASK→HLE, M_AUDTASK→synth) | Queue split happens before `run_task` |
| SP/DP completion | Delivered after the parse; no host tokens in the default mode (`rs64_vi_callback` only counts VIs unless `ROGUESQ_VI_DRIVEN_LOOP=0`) |
| Barrier receives | Real blocking receives; the generated C keeps the original `OS_MESG_BLOCK` flags when VI-driven |
| Buffer arbiter | Host only reads `D_80128E98`/`D_80128EAA` in the default mode; the old host consumer is gated off |
| `osViBlack` | No-op override (the game never calls `osViBlack(0)`) |
| `osYieldThread` | Real requeue-and-switch (`ROGUESQ_HOST_YIELD_ONLY=1` restores the host yield) |
| `osDestroyThread` | Guards a queued thread with a null queue pointer (the boot-time thread 4 teardown) |
| `osInitialize` | Sets `osClockRate` to 46875000 as on hardware (librecomp left the ROM's 62500000, slowing every time computation by 0.75×) |
| `osStopThread` non-self semantics | Overrides upstream `assert(false)` with libultra-equivalent |
| Audio (M_AUDTASK) | Runs the recompiled MusyX synth; samples stream from the cartridge through librecomp's ROM read |

---

## Current status & known blockers

| System | Status |
|---|---|
| Boot → attribution → logo → cinematic → menu | Reaches the main menu on every recent run without host tokens or forced non-blocking receives |
| Attribution text | Renders (glyph rect count matches the hardware walk) |
| Intro cinematic | Ships, trench, exploding logo, Factor 5 letters + fire, flat terrain tiles, grey sky; heightfield ground and fire/smoke billboards missing |
| Main menu | Renders (bg + title) |
| In-mission gameplay | Not yet exercised past the menu |
| Audio (music + SFX) | Recompiled MusyX synth, cartridge-streamed samples, synth tick and voice pool registered as on hardware |
| Input | SDL2 gamepad (keyboard not implemented); `ROGUESQ_FAKE_CONTROLLER=1` for headless runs |
| Save (EEPROM 4K) | Works; Memory Pak stubbed |

### Open items

1. **Parse-versus-rebuild race (fixed 2026-09-08).** The game frees and reuses material/frame chunks at frame start before `submitGfxFrame` waits for SP-done; hardware is safe because the RSP finishes in a few ms, but RT64's 10–100 ms parse read rebuilt chunks (garbage walks, and a bad color-image at 0x760000 that corrupted the heap free list — the LucasArts-reveal crash). Fix: a graphics task is in flight from the request message until the parse completes; the frame-start receive and chunk-release wait for it (`ROGUESQ_VI_WAIT_PARSE=0` disables); the host VI thread holds a retrace during a parse; color images < 16 px wide or not 64-byte aligned are rejected. Post-fix: zero capped tasks, hardware chunk-hop count, 0.9 ms average parse.
2. **Heightfield terrain and billboards.** The `05 05 00` record (height grid + color rows, overlays 0x14/0x18) and the fire/smoke sprites are not drawn. Record layouts and overlay disassembly are in [f5-model-dl-spec.md §7](f5-model-dl-spec.md) and the scratch notes.
3. **Frame pacing.** Three VIs per frame vs two on hardware; with the race fixed the parse averages under 1 ms, so the remaining VI is on the game side of the frame (to be measured).
4. **Regen-fragile hand edits** in `RecompiledFuncs/funcs_*.c`: the VI-driven gating of the four barrier receives, the yield in `waitForMusyXAudioTaskDone`, the voice key-on un-stub, the guard counters and probes. The KSEG0 guard hooks measured zero fallbacks in a full boot→menu run and can be retired with a regen. The `menuOverlayInit` instruction patch at 0x800C593C nops a callee-save store rather than the branch its comment describes.
5. **Factor 5 mutex/queue stubs** in the toml (audio) — hardware takes the mutex every frame; restore now that yields are real.

The white background during the N64 logo and the grey sky in the trench are
intended content.

---

## Build / regen workflow

After renaming a symbol (`llvm-objcopy --redefine-sym`):

1. **Regen recompiled C** — use the **Release** binary (Debug aborts on `recomp_entrypoint`):
   ```
   cd E:/Projects/N64Recomp && ./build_new/Release/N64Recomp.exe rogue_squadron.toml
   ```
2. **Regen audio ucode** (only if `musyx_rsp.toml` changed):
   ```
   ./build/Debug/RSPRecomp.exe musyx_rsp.toml
   python tools/fixup_factor5_ucode.py build/factor5_ucode/musyx_audio_recompiled.c
   ```
3. **Rebuild** (Debug is the dev/debug target; Release for perf):
   ```
   cd E:/Projects/RogueSquadron64Recomp && cmake --build build --config Debug -j 4
   ```
   Check the executable's timestamp afterwards: a failed build leaves the previous binary in place and the run scripts will happily launch it.
4. **Smoke test**: `tools/validate/capture_menu_rdram.ps1 -Screen "menu,400" -Timeout 420 -FastFwd 0` boots headless to the menu and dumps RDRAM there; `tools/validate/rdram_golden_diff.py` compares it with a Project64 golden. `./tools/run-stability.ps1 -Runs 1 -Timeout 30` is the shorter check.

> Regen overwrites hand-edits in `RecompiledFuncs/funcs_*.c` (see open item 4);
> run `tools/rename/lint_toml_syms.py` after every rename batch.


## What's NOT yet renamed / understood

- ~2000 functions still as `func_XXXXXXXX` (an active renaming pass is in progress — see `tools/rename/`).
- Big unmapped areas: the **mission overlay** (~900 funcs, per-craft/per-level), remaining **cinematic** helpers, **HMP terrain** inner helpers, the **DMA mutex** functions, and the **music→synth voice-table gather** (the channel→voice-table copy). (The **MORT voice codec** is recompiled and works — see the streamed-voice pipeline; the demo freeze once attributed to it was an N64Recomp link-branch codegen bug, fixed 2026-09-13.)

## How to extend this document

1. When renaming a function: add it to the matching subsystem table.
2. When discovering a new subsystem: add a numbered section under "Subsystems".
3. When mapping a global address: add to the "Memory map" table, using the recomp symbol (or `D_<ADDR>` if unnamed).
4. Keep entries terse — one line each; link out to `E:/Projects/rogue_squadron64/docs/` for deeper specs.

## See also

- [docs/data-structures.md](data-structures.md) — field-offset layouts for the entity/gameplay objects, global game state, HUD/menu, and cinematic/scene-graph structs.
- `E:/Projects/rogue_squadron64/symbol_files/` — authoritative symbol names (per-overlay).
- `E:/Projects/rogue_squadron64/docs/` — per-subsystem detail (HOB/HMT/SND/DAT/save/menus + partial m2c decomp).
- [docs/audit-changes.md](audit-changes.md) — inventory of fork modifications by category.
- [docs/f5-model-dl-spec.md](f5-model-dl-spec.md) — display-list grammar; §7 is the ucode-derived truth (chunk fetch, opcodes, terrain records).
- [docs/factor5-gbi.md](factor5-gbi.md) — Factor 5 graphics microcode docs (older runtime priors).
- [docs/factor5-ucode-dispatch.md](factor5-ucode-dispatch.md) — dispatch-table notes; its handler readings are offset by 0x80 (see the spec §7).
- [docs/debug-trace-env-vars.md](debug-trace-env-vars.md) — `ROGUESQ_LOG_*` runtime tracing.
- `musyx_rsp.toml` — RSPRecomp config for the MusyX audio synth (`text_address = 0x04001080`).
</content>
