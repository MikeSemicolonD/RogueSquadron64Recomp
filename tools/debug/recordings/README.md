# Recordings

Recorded controller input, one folder per mission, for repeatable test runs: frame-interpolation checks, determinism regressions, and co-op harness runs. Folder prefixes are the game's level id, the same number `ROGUESQ_BOOT_TARGET=level:<id>` takes.

Run outputs (`.replay.hash`, logs) go to `logs/`, not here. Record and replay in the Release build; Debug adds background work that changes frame timing.

## Two kinds of recording

| Kind | Header | Starts at | Env vars | Used for |
| --- | --- | --- | --- | --- |
| **Mission** (lockstep) | `rs64-ls v2` | Mission frame 0, fixed 1/30 s step, forced RNG seed | `ROGUESQ_LS_RECORD` / `ROGUESQ_LS_REPLAY` | Bit-exact determinism checks against a `.hash` baseline; co-op runs (`ROGUESQ_MP_PAD`, `ROGUESQ_LS_PAD2_REPLAY`) |
| **Session** | `rs64-input v3`/`v4` | First input poll after launch (boot and menus included) | `ROGUESQ_INPUT_RECORD` / `ROGUESQ_INPUT_REPLAY` | Visual checks (interpolation, rendering) where the run only has to be close |

A mission recording `<name>.rec` sits next to its baseline `<name>.rec.hash`, the per-frame state hashes written while recording. A recording without a `.hash` is input-only.

Besides input, a mission recording stores the audio thread's answers the simulation branches on: `S` lines (song finished) and `P` lines (speech idle, which gates the mission-end result). A recording made before `P` lines existed plays that answer live, so its last ~33 frames, the mission end, match only when replayed at the speed it was recorded at.

## Missions

| Id | Mission | Folder | Recordings |
| ---: | --- | --- | --- |
| 0 | Ambush at Mos Eisley | [00-ambush-at-mos-eisley](00-ambush-at-mos-eisley) | see below |
| 1 | Rendezvous on Barkhesh | | none yet |
| 2 | The Search for the Nonnah | | none yet |
| 3 | Defection at Corellia | | none yet |
| 4 | Liberation of Gerrard V | [04-liberation-of-gerrard-v](04-liberation-of-gerrard-v) | see below |
| 5 | The Jade Moon | [05-the-jade-moon](05-the-jade-moon) | see below |
| 6 | Imperial Construction Yards | [06-imperial-construction-yards](06-imperial-construction-yards) | see below |
| 7 | Assault on Kile II | [07-assault-on-kile-ii](07-assault-on-kile-ii) | see below |
| 8 | Rescue on Kessel | [08-rescue-on-kessel](08-rescue-on-kessel) | see below |
| 9 | Prisons of Kessel | [09-prisons-of-kessel](09-prisons-of-kessel) | see below |
| 10 | Battle Above Taloraan | | none yet |
| 11 | Escape from Fest | | none yet |
| 12 | Blockade on Chandrila | | none yet |
| 13 | Raid on Sullust | | none yet |
| 14 | Moff Seerdon's Revenge | | none yet |
| 15 | The Battle of Calamari | | none yet |
| 16 | Beggar's Canyon (bonus) | [16-beggars-canyon](16-beggars-canyon) | see below |
| 17 | The Death Star Trench Run (bonus) | [17-the-death-star-trench-run](17-the-death-star-trench-run) | see below |
| 18 | The Battle of Hoth (bonus) | | none yet |
| 20 | CHICKEN (AT-ST passcode level) | [ai/20-chicken](ai/20-chicken) | see below |

Needs re-recording: `construction_yards_bonus` was recorded before the forced-craft secondary fix (secondary 3 whatever the craft). Replays now get the craft's own secondary from the menu driver, so they diverge from frame 0.

Upgrade pickups: these mission recordings fly through their level's upgrade (the collect at `npcPowerUpUpdate` fires during the replay): `defection_at_corellia_20261118_191957`, `the_jade_moon_20261119_171142`, `prisons_of_kessel_20261119_200534`, `battle_above_taloraan_20261007_134057`, `blockade_on_chandrila_20261007_135613`, `raid_on_sullust_20261007_140400` and `moff_seerdons_revenge_20261007_141118`. The `assault_on_kile_ii`, `escape_from_fest` and `the_search_for_the_nonnah` runs leave theirs. Every run starts with the pilot's upgrades from its `flags0` header, which already owns all of them in the 2026-11 recordings.

### 00 Ambush at Mos Eisley

| File | Kind | Contents |
| --- | --- | --- |
| `mos_eisley.rec` (+ `.hash`) | Mission | The determinism gate: 3766 frames, X-wing. Default pad for both sides of `tools/debug/lockstep/run-mp.ps1`; must replay IDENTICAL after any change that should not affect the simulation |
| `mos_eisley_coop.rec` (+ `.hash`) | Mission | Gate re-recorded with the co-op imposter on (`ROGUESQ_COOP_LOCAL=1`), so the baseline carries the song answers the imposter changes. Matches only with player 2 flying the solo run: replay it with `ROGUESQ_COOP_LOCAL=1` and `ROGUESQ_LS_PAD2_REPLAY=mos_eisley.rec` (idle player 2 diverges at frame 0). `run-replays.ps1` skips it |
| `mos_eisley_dive.rec` | Mission, no baseline | Stick held into a dive from frame 300; the co-op player-death test |

The `mos_eisley.rec` baseline was recorded with the `larger_object_pool.nrm` mod loaded, so replays must run with it staged; other gameplay mods (e.g. invincibility) must not be staged during replays.

### 04 Liberation of Gerrard V

| File | Kind | Contents |
| --- | --- | --- |
| `liberation_of_gerrard_v_20261119_170322.rec` (+ `.hash`) | Mission | 7335 frames, X-wing, no deaths: a full run to mission complete |

### 05 The Jade Moon

| File | Kind | Contents |
| --- | --- | --- |
| `the_jade_moon_20261119_171142.rec` (+ `.hash`) | Mission | 7578 frames, X-wing, one death mid-run, played to the mission end |

### 06 Imperial Construction Yards

| File | Kind | Contents |
| --- | --- | --- |
| `construction_yards_towcable.rec` (+ `.hash`) | Mission | 3209 frames in the snowspeeder (craft 4), firing the tow cable; frame-interpolation checks for the cable (`level:6,4`). Recorded with `larger_object_pool.nrm` staged and no gameplay mods (invincibility, long-tow-cable): replay the same way or player0 diverges from frame 0 |
| `construction_yards_bonus.rec` (+ `.hash`) | Mission | 3126 frames in the snowspeeder (craft 4): destroys the far silo past the landing platform and collects the Advanced Bombs (power-up DAT item 121). Co-op pickup sharing: `run-mp.ps1 -HostPad` this, `-ClientPad construction_yards_towcable.rec` (which never goes near the silo) |
| `imperial_construction_yards_20261119_192539.rec` (+ `.hash`) | Mission | 12985 frames in the snowspeeder with the long tow cable: a failed attempt, closed at the failure screen |

### 07 Assault on Kile II

| File | Kind | Contents |
| --- | --- | --- |
| `assault_on_kile_ii_20261119_194907.rec` (+ `.hash`) | Mission | 9932 frames, A-wing, no deaths, played to the mission end |

### 08 Rescue on Kessel

| File | Kind | Contents |
| --- | --- | --- |
| `rescue_on_kessel_20261119_194433.rec` (+ `.hash`) | Mission | 4194 frames, X-wing, no deaths, played to the mission end |

### 09 Prisons of Kessel

| File | Kind | Contents |
| --- | --- | --- |
| `prisons_of_kessel_20261119_200534.rec` (+ `.hash`) | Mission | 18007 frames, X-wing, no deaths, played to the mission end |
| `kessel.rec` | Session (v3) | Boot, menus, then flying past the searchlight towers and fighting (frame-interpolation checks) |

### 16 Beggar's Canyon

| File | Kind | Contents |
| --- | --- | --- |
| `beggars_canyon_20261119_200217.rec` (+ `.hash`) | Mission | 2392 frames, T-16, no deaths, played to the mission end |

### 17 The Death Star Trench Run

| File | Kind | Contents |
| --- | --- | --- |
| `the_death_star_trench_run_20261119_201651.rec` (+ `.hash`) | Mission | 6425 frames, X-wing, one death mid-run, played to the mission end |

### 20 CHICKEN (ai/)

`ai/` holds recordings an agent played through the rs64 MCP, one `<id>-<mission>` folder per level, kept apart from hand-played runs.

| File | Kind | Contents |
| --- | --- | --- |
| `chicken_try6.rec` | Mission, no baseline | Agent-played AT-ST run with recorded head aim, 3238 frames: first base cleared, bunker opened, rebel pilots leave it ~frame 2440. A checkpoint to continue from with `ROGUESQ_LS_RESUME`, not a test |

## Scripts

- `record.ps1`: records into the level's folder, creating `<id>-<mission>` if it is missing. Also the VS Code task **Record: Input recording (pick level / craft)**.

  ```powershell
  .\tools\debug\recordings\record.ps1 -Level 5 -Craft 0 -Name jade_moon
  .\tools\debug\recordings\record.ps1 -Level 9 -Kind session
  ```

- `run-replays.ps1`: replays every mission recording under a folder that has a `.hash` baseline (skips `*_coop.rec` and session recordings) and compares each with `compare_hashes.py`. Writes to `logs/lockstep/<tag>/` with a `summary.csv`; exits 0 only if every replay is IDENTICAL.

  ```powershell
  .\tools\debug\recordings\run-replays.ps1 -Tag release-a
  .\tools\debug\recordings\run-replays.ps1 -Tag gate -Recordings tools\debug\recordings\00-ambush-at-mos-eisley
  ```

  `-Speed 4` (default) runs the game clock 4x and matches 1x; 8 is too fast for the menu driver.
- `capture-frames.ps1`: replays one recording (level and craft from its header, 4x) and screenshots the window at each listed frame; the replay pauses there (`ROGUESQ_LS_PAUSE_AT`) until the capture is taken, so two runs with different `-Env` give pixel-identical frames for A/B of render changes. Output and the hash log go to `dumps/capture/<tag>/`, never next to the recording.

  ```powershell
  .\tools\debug\recordings\capture-frames.ps1 -Recording tools\debug\recordings\06-imperial-construction-yards\construction_yards_bonus.rec -Frames 2995,3025 -Tag pickup -Env "ROGUESQ_RT_LIGHTS=1"
  ```

  `-Cutscene <level>,<kind>` (kind 0 intro, 1 extro, 2 special) instead of `-Recording` boots into that cutscene and captures its own frames, the frame numbers the RS64 Explorer timeline shows (`ROGUESQ_PAUSE_AT=cutscene:<f>`), for comparing the game against the explorer frame for frame. It leaves other running game instances alone.

  ```powershell
  .\tools\debug\recordings\capture-frames.ps1 -Cutscene 5,0 -Frames 60,200,400 -Tag lv5intro -Speed 1
  ```

- `compare_hashes.py <baseline.hash> <replay.hash>`: prints the first diverging frame and column (`IDENTICAL` otherwise). Tests: `python tools\debug\recordings\test_compare_hashes.py`.
- `record-coop.ps1 -Level N [-Craft c] [-HostPad <rec>]` (VS Code task "Record: Co-op client recording"): you fly the co-op client while the host replays the level's solo recording (newest `<mission>_<timestamp>.rec`, else one with a `.hash` baseline); writes `<mission>_coop_client_<timestamp>.rec`.
- `tools/debug/lockstep/run-mp.ps1` (co-op, stays with the multiplayer tooling): `-HostPad` / `-ClientPad` take mission recordings from here. `-ClientLive` flies the client by hand (both sides at 1x) and `-ClientRecord <rec>` records the client's input; such a co-op client recording replays only in a pair, as `-ClientPad` against the same `-HostPad`.

## Record a mission recording

```powershell
$env:ROGUESQ_LS_RECORD = "$PWD\tools\debug\recordings\05-the-jade-moon\jade_moon.rec"
.\build\Release\RogueSquadron64Recomp.exe
```

Navigate the menus normally and play the mission (finish or fail it). Recording starts at mission frame 0 and writes `jade_moon.rec` plus the `jade_moon.rec.hash` baseline. Create the mission folder first, named `<id>-<mission-name>`, and add a row above.

## Replay a mission recording

```powershell
$env:ROGUESQ_BOOT_TARGET = 'level:0,0'
$env:ROGUESQ_LS_REPLAY = "$PWD\tools\debug\recordings\00-ambush-at-mos-eisley\mos_eisley.rec"
.\build\Release\RogueSquadron64Recomp.exe
python tools\debug\recordings\compare_hashes.py tools\debug\recordings\00-ambush-at-mos-eisley\mos_eisley.rec.hash tools\debug\recordings\00-ambush-at-mos-eisley\mos_eisley.rec.replay.hash
```

Use the `level=` and `craft=` values from the recording's header in `ROGUESQ_BOOT_TARGET`. The replay writes `<rec>.replay.hash` next to the recording and quits at the end; `run-replays.ps1` does all of this and keeps the output in `logs/`.

## Record and replay a session recording

```powershell
$env:ROGUESQ_INPUT_RECORD = "$PWD\tools\debug\recordings\09-prisons-of-kessel\kessel.rec"
.\build\Release\RogueSquadron64Recomp.exe
```

Launch without `ROGUESQ_BOOT_TARGET`, play from the title screen, then close the window. Replay with `ROGUESQ_INPUT_REPLAY` set to the same path, the same build and the same env settings. Replay overrides all controller-0 input from launch, holds the last input after the file ends, and closes the game when the recording ends.

Session format: lines are `<ms> <vi> <poll> <buttons hex> <stick x> <stick y>` after the version header.

- **v4** (written now): one line per input poll, full float precision. Replay serves the recorded sample nearest the current wall-clock time, so every input (mouse steering included) lands within half a poll (~8 ms) of when it was recorded and no lag builds up. The recorded stick values already include the mouse smoothing.
- **v3** (older files, e.g. `kessel.rec`): written only on change and replayed by wall-clock `ms`, with the stick blended between close samples. Mouse steering drifts slightly between runs.

Replay logs `[input-rec] ms=… vi=… recorded_vi=… poll=… recorded_poll=…` every 10 s. The game is not fully deterministic outside mission recordings, so a long session replay can still diverge from the recorded run.
