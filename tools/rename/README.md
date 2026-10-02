# Symbol-rename tooling

Helpers for propagating semantic names through the
`symbol_files/*.txt` → `roguesquadron.elf` → `N64Recomp` → `RecompiledFuncs/`
pipeline. `symbol_files/` and the ELF live in the sibling decomp repo
(`../rogue_squadron64`), not this one.

## Paths

All scripts default to these paths (relative to a sibling-repo layout):

| Var               | Default                                           |
|-------------------|---------------------------------------------------|
| `RSR_SYM_DIR`     | `E:/Projects/rogue_squadron64/symbol_files`       |
| `RSR_ASM_DATA_DIR`| `E:/Projects/rogue_squadron64/asm/data`           |
| `RSR_FUNCS_DIR`   | `E:/Projects/N64Recomp/RecompiledFuncs`           |
| `RSR_REDEFS_PATH` | `E:/Projects/rogue_squadron64/build/redefs.txt`   |

Override any of them with an environment variable before running.

## Scripts

### `build_redefs.py`
Generates the `--redefine-syms` file for `llvm-objcopy` from every semantic
entry in `symbol_files/*.txt`. For each `name = 0xADDR;` emits three rename
lines (`func_HHHHHHHH`, `D_HHHHHHHH`, `fake_func_HHHHHHHH` → `name`) so the
pass catches whichever placeholder splat actually emitted. Appends explicit
reverts for the `setGlobalByte_8011A8XX` cluster (splat-provided names that
violate the "no address-encoded identifiers" rule).

```sh
python tools/rename/build_redefs.py
```

### `find_callers.py`
Lists every function in the recompiled C that calls a given target. Used
constantly when deciding whether a target function has enough call-site
context to name confidently.

```sh
python tools/rename/find_callers.py initMission
python tools/rename/find_callers.py func_8006E468
```

### `find_state_machines.py`
Scans unnamed functions (`func_8XXXXXXX`) for state-machine shape. Two signals:
1. Jump-table dispatch: body contains a `switch_error(...)` call (N64Recomp's
   marker for compiler-generated switch tables).
2. Repeated `MEM_W` writes to the same offset — usually a state variable.

Jump-table candidates print first (highest confidence).

```sh
python tools/rename/find_state_machines.py
```

### `cleanup_placeholders.py`
Removes redundant `func_HHHHHHHH` / `D_HHHHHHHH` entries from each symbol
file when a semantic name for the same address already exists in the same
file. Safe to re-run; idempotent.

```sh
python tools/rename/cleanup_placeholders.py
```

### `rename_asciz_strings.py`
Bulk-renames `D_HHHHHHHH = ...; // type:asciz` entries by reading the actual
literal text from splat's `.rodata.s` output and producing `strXxxx`
identifiers. Skipped entries: too short (<3 chars), too long (>40), non-printable,
already-used name, or name with no camel-case content. Per-overlay prefix:
`strMiss` / `strMenu` / `strCin` / bare `str` for main_overlay.

```sh
python tools/rename/rename_asciz_strings.py
```

## Pipeline

The typical loop after editing a `symbol_files/*.txt` in the decomp repo:

```sh
# 1. Regenerate redefs and apply to the ELF (elf_path in rogue_squadron.toml)
python tools/rename/build_redefs.py
llvm-objcopy \
    --redefine-syms="$RSR_REDEFS_PATH" \
    "$ELF" "$ELF"

# 2. Re-run N64Recomp to refresh RecompiledFuncs/. It aborts on the first
#    rogue_squadron.toml func=/stub name the renamed ELF no longer has.
cmake --build build --config Debug --target regen_funcs

# 3. Build
cmake --build build --config Debug --target RogueSquadron64Recomp
```

A clean-state baseline ELF is kept at
`E:/Projects/rogue_squadron64/build/roguesquadron.elf.preBatchContinue` —
copy it back before each `llvm-objcopy` run if you want renames to compose
cleanly across batches.

## Pending candidates

Functions whose current name is known to be wrong, with the role found by RE. Not applied yet. Names used in `rogue_squadron.toml` hooks (`func =`) and in mods' `RECOMP_HOOK` / `RECOMP_PATCH` must be renamed in the same batch, or regen fails. Verify a proposed name against the disassembly (`tools/rz/rzq.py disasm <addr>`) before applying.

| Address | Current name | Proposed | Role | Source |
|---|---|---|---|---|
| 0x80017AE0 | `setViewStateBytesAndFadeScale` | `setFog` | (r,g,b,start,density): fog color 0x8011A873, start/end | fog trace 2026-10-01 |
| 0x80017B48 | `setMissionLevelInitByte` | `setFogEnable` | fog on/off byte read by the RDP state builder | fog trace 2026-10-01 |
| 0x80044724 | `cutsceneActorNpcHandler` | `terrainFogFrameCallback` | per-frame callback: terrain grid build, far plane, fog (registered by initMission and setupCutsceneLevel). toml hooks use the name | fog trace 2026-10-01 |
| 0x800455C8 | `computeGridLayerScrollOffset` | `computeViewFarRange` | far range D from camera altitude, terrain detail radius 0x8009DEB4. toml hooks use the name | fog trace 2026-10-01 |
| 0x800457B8 | `setGridLayerScrollBounds` | `setViewFarRange` | sets the far range lo/hi (0x80130C54/58) | fog trace 2026-10-01 |
| 0x800AF04C | `tickCutsceneObject` (mission overlay) | `updateGliderTerrainFollow` | glider (V-wing/snowspeeder) hover above terrain, ceiling/floor 0x8010B7A0/A4 | Taloraan V-wing 2026-10-01 |
| 0x80067D90 | `wrapAngleToCyclicRange` | `getTerrainHeightAt` | terrain height at (x, z, unused, surface out) | Taloraan V-wing 2026-10-01 |
| 0x80068180 | `sampleTerrainSurfaceAtWorldXz` | (check) | terrain surface sample used by the glider code; name may be close | Taloraan V-wing 2026-10-01 |
| 0x8005A4B4 | `updateWalkerParticlePositions` | `updateSkyClouds` | moves the sky cloud layer | Taloraan horizon 2026-10-01 |
| 0x8005BCC4 | `cinematicSplineWalkerNpcHandler` | `skyHorizonNpcHandler` | sky/horizon/clouds/background handler (also noted in `syms/rogue_squadron.syms.toml`) | skybox plan |
| 0x8005B3A8 | `linkDirectionalTrailSegments` | `selectSkyRingSectors` | picks the 8 yaw sectors of the horizon ring to draw | widescreen 2026-09-24 |
| 0x8004E140 | `updateEffectObjectOrientationQuat` | `updateCameraLookAt` | camera look-at orientation update | Taloraan camera 2026-10-01 |
| 0x8001DC34 | `transformVec3ToNpcLocalWithScale` | `isSphereInViewFrustum` | object sphere/point frustum test (aspect, far) | widescreen 2026-09-24 |
| 0x8005C874 | `audioCueDispatcherNpcHandler` | `projectileNpcHandler` | projectile handler | frame interp 2026-09-20 |
| 0x80014DE0 | `processMeshdef1ForLod` | (check) | primitive-depth setup, not a LOD selector | LOD map 2026-09-14 |
| 0x800AD418 | `updateCinematicCameraStateMachine` (mission overlay) | `updateTowCable` | tow cable simulation (60-segment ring) | tow cable 2026-10-01 |
| 0x800ADD5C | `submitCameraTrackedObjectToRender` | `drawTowCable` | tow cable renderer | tow cable 2026-10-01 |
| 0x800AE138 / 0x800AE218 | `loadEffectModelInstancePool` / `freeEffectModelInstancePool` | `loadTowCablePool` / `freeTowCablePool` | cable segment pool | tow cable 2026-10-01 |
| 0x800AE280 | `getEffectModelPoolHandle` | `getTowCableShotId` | returns the cable shot id | tow cable 2026-10-01 |
| 0x800AC7B4 | `tickCinematicEffectObject` | `towHarpoonUpdate` | harpoon handler (called from playerSnowSpeederUpdate) | tow cable 2026-10-01 |
| 0x800ADF2C | `emitEffectParticlesAlongPath` | `updateTowCableWrapCount` | counts wraps around the AT-AT legs, triggers the trip | tow cable 2026-10-01 |
| `funcs_36.c` | `buildDebrisMeshFromCells`, `emitDebrisCellFaces`, ... | JPEG/JFIF decoder names | decoder used by `tickFormatMessageWorker` | AGENTS.md open work |
| 0x800B4588 | `updateXwingFlightControls` (mission overlay) | `updateXwingSpeed` | X-wing speed update; 0x4($s2) != 0 is a scripted boost. toml hooks use the name | rogue_squadron.toml, hotas plan |
| 0x800B5B4C | `updateAwingFlightControls` (mission overlay) | `updateAwingSpeed` | A-wing speed update. toml hooks use the name | rogue_squadron.toml |
| 0x800B77D0 | `updateYwingFlightControls` (mission overlay) | `updateYwingSpeed` | Y-wing speed update. toml hooks use the name | rogue_squadron.toml |
| 0x800BDC9C | `updateNabooStarfighterFlightControls` (mission overlay) | `updateNabooStarfighterSpeed` | Naboo starfighter speed update. toml hooks use the name | rogue_squadron.toml |
| 0x800AADD4 | `updateSnowSpeederFlightControls` (mission overlay) | `updateSnowSpeederSpeed` | snowspeeder speed update. toml hooks use the name. The V-wing, Falcon, TIE interceptor and T-16 copies likely need the same rename (check) | rogue_squadron.toml |
| 0x800C131C | `runMenuShipSelectScreen` (menu overlay) | `runCreditsScreen` | credits sequence. toml hook uses the name | rogue_squadron.toml credits pacing |
| 0x800C7738 | `setHudEnableBit4` | `requestMissionFailure` | sets request bit 4 in 0x8010C9E0 when mode 0x8010CA20 == 0; the mode tick at 0x800FB538 turns bit 4 into mode 4 (failure). Called from level scripts | disasm 2026-10-01, docs/multiplayer.md |
| 0x800C776C | `setHudEnableBit8` | `requestMissionSuccess` | same, bit 8 -> mode 3 (success) | disasm 2026-10-01, docs/multiplayer.md |
| 0x80067A80 | `isSpeechBusyOrQueued` | `isSpeechIdle` | returns 1 when the speech queue is empty and the current voice handle is inactive (`isVoiceHandleActive`) | disasm 2026-10-01, co-op research notes |
| 0x800C794C | `getScaledPlayerCraftSpeed` | `getPlayerCraftScaledY` | `getCraftRecordByIdx(0)`+4 (Y) times the constant at 0x800A6964 | disasm 2026-10-01, co-op research notes |
| 0x800668B0 | `waitForAnyAudioSlot` | `freeAudioBuffers` (check) | ticks MusyX, calls `anyAudioSlotActive` (result unused), then `rs_free`s 0x80139B4C/50/54 unless byte 0x80130B39 is set | disasm 2026-10-01, co-op research notes |
| 0x8006CBB0 | `updateDestructionDebrisNpc` | `buildingNpcHandler` | building (DAT subtype 0x14) handler | co-op research notes |
| 0x800477A0 | `parseDatItemSubtypes1To5` | `staticPropNpcHandler` | stateless static prop handler (subtype 0 / unmapped) | co-op research notes |
| 0x8006BEC8 | `advanceNpcOnCurvedPath` | `emitGroundShadowDust` | ground shadow/dust emitter; calls `rand` only when in the frustum | co-op research notes |
| 0x800B1024 | `computeOrientedTransformFromQuat` (mission overlay) | (check) | contains the death-spiral end (0x800B111C-0x800B1140: sets 0x8010CA18 and request bit 1) | co-op research notes |
| 0x800FADEC | `flashMissionAlertObjectsAndPrint` | `onMissionStartButton` | START press handler, requests pause | co-op research notes |
| 0x800FAF0C | `beginMissionEndTransition` | `beginMissionPause` | mode 5, 0x8010CA1C \|= 0x101 (freeze) | co-op research notes |
| 0x800FB05C | `endMissionEndTransition` | `endMissionPause` | pause resume | co-op research notes |
| 0x8000CFD4 | `heapFreeListDequeue` | (check) | material/GBI display-list emitter | co-op research notes |
| 0x800B42D4 | `spawnProjectileObject` (menu overlay) | (check) | menu element spawner | co-op research notes |
| 0x800B448C | `spawnHudReticleAtMeshBounds` (menu overlay) | `spawnMenuElementSprite` (check) | sprite from a text element's texture desc (w/h at +0x10) and position (+0x20); used for the mod-menu slider bar | native slider 2026-09-20 |
