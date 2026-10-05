# Recordings

Recorded controller input, one folder per mission, for repeatable test runs: frame-interpolation checks, determinism regressions, and co-op harness runs. Folder prefixes are the game's level id, the same number `ROGUESQ_BOOT_TARGET=level:<id>` takes.

Run outputs (`.replay.hash`, logs) go to `logs/`, not here. Record and replay in the Release build; Debug adds background work that changes frame timing.

## Two kinds of recording

| Kind | Header | Starts at | Env vars | Used for |
|---|---|---|---|---|
| **Mission** (lockstep) | `rs64-ls v2` | Mission frame 0, fixed 1/30 s step, forced RNG seed | `ROGUESQ_LS_RECORD` / `ROGUESQ_LS_REPLAY` | Bit-exact determinism checks against a `.hash` baseline; co-op runs (`ROGUESQ_MP_PAD`, `ROGUESQ_LS_PAD2_REPLAY`) |
| **Session** | `rs64-input v3`/`v4` | First input poll after launch (boot and menus included) | `ROGUESQ_INPUT_RECORD` / `ROGUESQ_INPUT_REPLAY` | Visual checks (interpolation, rendering) where the run only has to be close |

A mission recording `<name>.rec` sits next to its baseline `<name>.rec.hash`, the per-frame state hashes written while recording. A recording without a `.hash` is input-only.

## Missions

| Id | Mission | Folder | Recordings |
|---:|---|---|---|
| 0 | Ambush at Mos Eisley | [00-ambush-at-mos-eisley](00-ambush-at-mos-eisley) | see below |
| 1 | Rendezvous on Barkhesh | | none yet |
| 2 | The Search for the Nonnah | | none yet |
| 3 | Defection at Corellia | | none yet |
| 4 | Liberation of Gerrard V | | none yet |
| 5 | The Jade Moon | | none yet |
| 6 | Imperial Construction Yards | [06-imperial-construction-yards](06-imperial-construction-yards) | see below |
| 7 | Assault on Kile II | | none yet |
| 8 | Rescue on Kessel | | none yet |
| 9 | Prisons of Kessel | [09-prisons-of-kessel](09-prisons-of-kessel) | see below |
| 10 | Battle Above Taloraan | | none yet |
| 11 | Escape from Fest | | none yet |
| 12 | Blockade on Chandrila | | none yet |
| 13 | Raid on Sullust | | none yet |
| 14 | Moff Seerdon's Revenge | | none yet |
| 15 | The Battle of Calamari | | none yet |
| 16 | Beggar's Canyon (bonus) | | none yet |
| 17 | The Death Star Trench Run (bonus) | | none yet |
| 18 | The Battle of Hoth (bonus) | | none yet |

### 00 Ambush at Mos Eisley

| File | Kind | Contents |
|---|---|---|
| `mos_eisley.rec` (+ `.hash`) | Mission | The determinism gate: 3766 frames, X-wing. Default pad for both sides of `tools/lockstep/run-mp.ps1`; must replay IDENTICAL after any change that should not affect the simulation |
| `mos_eisley_coop.rec` (+ `.hash`) | Mission | Gate re-recorded with the co-op imposter on (`ROGUESQ_COOP_LOCAL=1`), so the baseline carries the song answers the imposter changes. `run-replays.ps1` skips it |
| `mos_eisley_dive.rec` | Mission, no baseline | Stick held into a dive from frame 300; the co-op player-death test |

The `mos_eisley.rec` baseline was recorded with the `larger_object_pool.nrm` mod loaded, so replays must run with it staged; other gameplay mods (e.g. invincibility) must not be staged during replays.

### 06 Imperial Construction Yards

| File | Kind | Contents |
|---|---|---|
| `construction_yards_towcable.rec` (+ `.hash`) | Mission | 3209 frames in the snowspeeder (craft 4), firing the tow cable; frame-interpolation checks for the cable (`level:6,4`). Recorded with `larger_object_pool.nrm` staged and no gameplay mods (invincibility, long-tow-cable): replay the same way or player0 diverges from frame 0 |
| `construction_yards_bonus.rec` (+ `.hash`) | Mission | 3126 frames in the snowspeeder (craft 4): destroys the far silo past the landing platform and collects the Advanced Bombs (power-up DAT item 121). Co-op pickup sharing: `run-mp.ps1 -HostPad` this, `-ClientPad construction_yards_towcable.rec` (which never goes near the silo) |

### 09 Prisons of Kessel

| File | Kind | Contents |
|---|---|---|
| `kessel.rec` | Session (v3) | Boot, menus, then flying past the searchlight towers and fighting (frame-interpolation checks) |

## Scripts

- `record.ps1`: records into the level's folder, creating `<id>-<mission>` if it is missing. Also the VS Code task **Record: Input recording (pick level / craft)**.

  ```powershell
  .\tools\recordings\record.ps1 -Level 5 -Craft 0 -Name jade_moon
  .\tools\recordings\record.ps1 -Level 9 -Kind session
  ```

- `run-replays.ps1`: replays every mission recording under a folder that has a `.hash` baseline (skips `*_coop.rec` and session recordings) and compares each with `compare_hashes.py`. Writes to `logs/lockstep/<tag>/` with a `summary.csv`; exits 0 only if every replay is IDENTICAL.

  ```powershell
  .\tools\recordings\run-replays.ps1 -Tag release-a
  .\tools\recordings\run-replays.ps1 -Tag gate -Recordings tools\recordings\00-ambush-at-mos-eisley
  ```

  `-Speed 4` (default) runs the game clock 4x and matches 1x; 8 is too fast for the menu driver.
- `capture-frames.ps1`: replays one recording (level and craft from its header, 4x) and screenshots the window at each listed frame; the replay pauses there (`ROGUESQ_LS_PAUSE_AT`) until the capture is taken, so two runs with different `-Env` give pixel-identical frames for A/B of render changes. Output and the hash log go to `dumps/capture/<tag>/`, never next to the recording.

  ```powershell
  .\tools\recordings\capture-frames.ps1 -Recording tools\recordings\06-imperial-construction-yards\construction_yards_bonus.rec -Frames 2995,3025 -Tag pickup -Env "ROGUESQ_RT_LIGHTS=1"
  ```

- `compare_hashes.py <baseline.hash> <replay.hash>`: prints the first diverging frame and column (`IDENTICAL` otherwise). Tests: `python tools\recordings\test_compare_hashes.py`.
- `tools/lockstep/run-mp.ps1` (co-op, stays with the multiplayer tooling): `-HostPad` / `-ClientPad` take mission recordings from here. `-ClientLive` flies the client by hand (both sides at 1x) and `-ClientRecord <rec>` records the client's input; such a co-op client recording replays only in a pair, as `-ClientPad` against the same `-HostPad`.

## Record a mission recording

```powershell
$env:ROGUESQ_LS_RECORD = "$PWD\tools\recordings\05-the-jade-moon\jade_moon.rec"
.\build\Release\RogueSquadron64Recomp.exe
```

Navigate the menus normally and play the mission (finish or fail it). Recording starts at mission frame 0 and writes `jade_moon.rec` plus the `jade_moon.rec.hash` baseline. Create the mission folder first, named `<id>-<mission-name>`, and add a row above.

## Replay a mission recording

```powershell
$env:ROGUESQ_BOOT_TARGET = 'level:0,0'
$env:ROGUESQ_LS_REPLAY = "$PWD\tools\recordings\00-ambush-at-mos-eisley\mos_eisley.rec"
.\build\Release\RogueSquadron64Recomp.exe
python tools\recordings\compare_hashes.py tools\recordings\00-ambush-at-mos-eisley\mos_eisley.rec.hash tools\recordings\00-ambush-at-mos-eisley\mos_eisley.rec.replay.hash
```

Use the `level=` and `craft=` values from the recording's header in `ROGUESQ_BOOT_TARGET`. The replay writes `<rec>.replay.hash` next to the recording and quits at the end; `run-replays.ps1` does all of this and keeps the output in `logs/`.

## Record and replay a session recording

```powershell
$env:ROGUESQ_INPUT_RECORD = "$PWD\tools\recordings\09-prisons-of-kessel\kessel.rec"
.\build\Release\RogueSquadron64Recomp.exe
```

Launch without `ROGUESQ_BOOT_TARGET`, play from the title screen, then close the window. Replay with `ROGUESQ_INPUT_REPLAY` set to the same path, the same build and the same env settings. Replay overrides all controller-0 input from launch, holds the last input after the file ends, and closes the game when the recording ends.

Session format: lines are `<ms> <vi> <poll> <buttons hex> <stick x> <stick y>` after the version header.

- **v4** (written now): one line per input poll, full float precision. Replay serves the recorded sample nearest the current wall-clock time, so every input (mouse steering included) lands within half a poll (~8 ms) of when it was recorded and no lag builds up. The recorded stick values already include the mouse smoothing.
- **v3** (older files, e.g. `kessel.rec`): written only on change and replayed by wall-clock `ms`, with the stick blended between close samples. Mouse steering drifts slightly between runs.

Replay logs `[input-rec] ms=… vi=… recorded_vi=… poll=… recorded_poll=…` every 10 s. The game is not fully deterministic outside mission recordings, so a long session replay can still diverge from the recorded run.
