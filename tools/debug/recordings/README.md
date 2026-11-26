# Recordings

Recorded controller input, one folder per mission, for repeatable test runs: frame-interpolation checks, determinism regressions, and co-op harness runs. Folder prefixes are the game's level id, the same number `ROGUESQ_BOOT_TARGET=level:<id>` takes.

Run outputs (`.replay.hash`, logs) go to `logs/`, not here. Record and replay in the Release build; Debug adds background work that changes frame timing.

## Mission recordings

A recording (header `rs64-ls v2`, `ROGUESQ_LS_RECORD` / `ROGUESQ_LS_REPLAY`) starts at mission frame 0 with the game's own RNG state (saved as `seed=` and restored by the replay; `ROGUESQ_LS_SEED` forces one) and plays like normal play: the draw distance's wider unit active range and the widescreen view tests stay on (`live=1`), and the replay takes the recorded widescreen and draw distance. Recordings without `live=1` replay with those stock, as they were recorded. It runs at the logic rate it was recorded at (`hz=` in the header: the TARGET FPS setting, up to 240; 30 when absent). Above 30 it records on real time and stores each frame's dt, so it plays at normal speed even when the frame rate dips, and a replay steps the same dts frame-exact at any `ROGUESQ_SPEED`. Used for bit-exact determinism checks against a `.hash` baseline, exact-frame repros and co-op runs (`ROGUESQ_MP_PAD`, `ROGUESQ_LS_PAD2_REPLAY`).

A mission recording `<name>.rec` sits next to its baseline `<name>.rec.hash`, the per-frame state hashes written while recording. A recording without a `.hash` is input-only.

Besides input, a mission recording stores the audio thread's answers the simulation branches on: `S` lines (song finished) and `P` lines (speech idle, which gates the mission-end result). A recording made before `P` lines existed plays that answer live, so its last ~33 frames, the mission end, match only when replayed at the speed it was recorded at.

## Missions

| Id | Mission | Folder | Recordings |
| ---: | --- | --- | --- |
| 0 | Ambush at Mos Eisley | [00-ambush-at-mos-eisley](00-ambush-at-mos-eisley) | see below |
| 1 | Rendezvous on Barkhesh | [01-rendezvous-on-barkhesh](01-rendezvous-on-barkhesh) | in folder |
| 2 | The Search for the Nonnah | [02-the-search-for-the-nonnah](02-the-search-for-the-nonnah) | in folder |
| 3 | Defection at Corellia | [03-defection-at-corellia](03-defection-at-corellia) | in folder |
| 4 | Liberation of Gerrard V | [04-liberation-of-gerrard-v](04-liberation-of-gerrard-v) | see below |
| 5 | The Jade Moon | [05-the-jade-moon](05-the-jade-moon) | see below |
| 6 | Imperial Construction Yards | [06-imperial-construction-yards](06-imperial-construction-yards) | see below |
| 7 | Assault on Kile II | [07-assault-on-kile-ii](07-assault-on-kile-ii) | see below |
| 8 | Rescue on Kessel | [08-rescue-on-kessel](08-rescue-on-kessel) | see below |
| 9 | Prisons of Kessel | [09-prisons-of-kessel](09-prisons-of-kessel) | see below |
| 10 | Battle Above Taloraan | [10-battle-above-taloraan](10-battle-above-taloraan) | in folder |
| 11 | Escape from Fest | [11-escape-from-fest](11-escape-from-fest) | in folder |
| 12 | Blockade on Chandrila | [12-blockade-on-chandrila](12-blockade-on-chandrila) | in folder |
| 13 | Raid on Sullust | [13-raid-on-sullust](13-raid-on-sullust) | in folder |
| 14 | Moff Seerdon's Revenge | [14-moff-seerdons-revenge](14-moff-seerdons-revenge) | in folder |
| 15 | The Battle of Calamari | [15-the-battle-of-calamari](15-the-battle-of-calamari) | in folder |
| 16 | Beggar's Canyon (bonus) | [16-beggars-canyon](16-beggars-canyon) | see below |
| 17 | The Death Star Trench Run (bonus) | [17-the-death-star-trench-run](17-the-death-star-trench-run) | see below |
| 18 | The Battle of Hoth (bonus) | [18-the-battle-of-hoth](18-the-battle-of-hoth) | in folder |
| 20 | CHICKEN (AT-ST passcode level) | [ai/20-chicken](ai/20-chicken) | see below |

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
  ```

- `run-replays.ps1`: replays every mission recording under a folder that has a `.hash` baseline (skips `*_coop.rec`) and compares each with `compare_hashes.py`. Writes to `logs/lockstep/<tag>/` with a `summary.csv`; exits 0 only if every replay is IDENTICAL.

  ```powershell
  .\tools\debug\recordings\run-replays.ps1 -Tag release-a
  .\tools\debug\recordings\run-replays.ps1 -Tag gate -Recordings tools\debug\recordings\00-ambush-at-mos-eisley
  ```

  `-Speed 4` (default) runs the game clock 4x and matches 1x; 8 is too fast for the menu driver.
- `run-level-perf.ps1`: replays each level's dated recording (`mos_eisley.rec` for level 0) in its own launch at 4x and tabulates game fps, DL walk ms per frame, hitches and working set at mission start and end. Writes to `logs/levelperf/<tag>/` with a `summary.csv`. The latest table is in [debug-tools.md](../../../docs/engine/debug-tools.md#per-level-performance).

  ```powershell
  .\tools\debug\recordings\run-level-perf.ps1 -Tag current
  .\tools\debug\recordings\run-level-perf.ps1 -Tag taloraan -Levels 10
  ```

- `capture-frames.ps1`: replays one recording (level and craft from its header, 4x) and screenshots the window at each listed frame; the replay pauses there (`ROGUESQ_LS_PAUSE_AT`) until the capture is taken, so two runs with different `-Env` give pixel-identical frames for A/B of render changes. Output and the hash log go to `dumps/capture/<tag>/`, never next to the recording.

  ```powershell
  .\tools\debug\recordings\capture-frames.ps1 -Recording tools\debug\recordings\16-beggars-canyon\beggars_canyon_20261119_200217.rec -Frames 600,1200 -Tag canyon -Env "ROGUESQ_RT_LIGHTS=1"
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

Several recordings play in one launch as a `;`-separated `ROGUESQ_LS_REPLAY` list, booted with `ROGUESQ_BOOT_TARGET=abort:<level>,<craft>` (the first recording's) and `ROGUESQ_NAV_CHAIN=1`. Set `ROGUESQ_LS_HASH_OUT` to a folder and each recording's `.replay.hash` is written there. Every entry must replay under the same staged mods.
