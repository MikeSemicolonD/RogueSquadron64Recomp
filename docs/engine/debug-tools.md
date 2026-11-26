# Test, validation and RE tools

Crash and hang dumps are in [debugging-with-visual-studio.md](debugging-with-visual-studio.md#crashes-and-hangs). Live frame inspection is in [rt64-mcp.md](rt64-mcp.md).

## Tests

| Suite | Command | Checks |
| --- | --- | --- |
| Unit tests | `cmake --build build --config Release --target rs64_unit_tests` | Host, co-op mod and relay server logic, plus `style_check` |
| Separate unit tests | `cmake --build build --config Release --target <name>` | `touch_input_test`, `video_config_test`, `transition_gate_test`, `input_bindings_test`, `fault_guard_test` |
| Smoke test | `python -I tools/debug/smoke/smoke.py run`, then `compare` | Screenshots, game state, frame rate, memory and chase camera against a baseline run |
| Replay hashes | `tools/debug/recordings/run-replays.ps1` | Each mission recording still plays out the same, frame hash by frame hash |

Run the unit tests and the smoke test's `perf,memory` sections before committing anything that could change frame rate, memory or game behaviour ([AGENTS.md](../../AGENTS.md#build--run) has the exact commands).

A fix lands with the check that would have caught it: a unit test for logic, a smoke target for a picture, menu or setting, or a mission recording for gameplay.

### Unit tests

`rs64_unit_tests` builds and runs every test in `tests/CMakeLists.txt`, the mod's in `mods/multiplayer-native/tests/` and the relay's in `relay-server/tests/`, then `style_check`. Each test prints its name and `OK`; `style` prints how many findings are new. CI runs the same target ([release-ci.md](release-ci.md#unit-tests)).

The separate tests are `EXCLUDE_FROM_ALL` targets, built and run one at a time from `build\Release\<name>.exe`:

- `input_bindings_test` checks with `assert`, which Release compiles out: run it from the Debug build.
- `fault_guard_test` covers the Linux and macOS guest-fault guard (Windows uses SEH), so it only exists in a Linux or macOS build.

### Smoke test

`smoke.py run` stages the Release exe in `build/Smoke`, runs every target in [manifest.toml](../../tools/debug/smoke/manifest.toml) with a pinned profile and save (never yours), and writes `dumps/smoke/<tag>/<platform>/`. `compare` checks that against `dumps/smoke/baseline` and writes `results.js` and the report page from [tools/debug/smoke/report](../../tools/debug/smoke/report/) next to it. Open `report.html` for a breakdown by section, sortable by biggest change, with each image beside its baseline and its changed pixels. Make a new baseline with `run --tag baseline`.

| Section | What it captures | Fails when |
| --- | --- | --- |
| `levels` | Each level's mission frames | Over 0.05% of pixels change, or the player, camera, wingmen or NPCs move |
| `changes` | Each setting changed mid-mission, against an unchanged run | As `levels`; a `sim` setting is only compared with its own baseline |
| `menus` | Each menu path | Over 0.05% of pixels change, or the game state differs |
| `pause` | Every pause-page setting through each value | As `menus` |
| `cutscenes` | Cutscene frames | Over 20% of pixels change (water, ground and subtitles follow the voice stream) |
| `perf` | Each `video-presets/` preset at real speed | Average frame time grows over 15% |
| `memory` | Private bytes at the same mission frame over six replays of level 3 | Median growth per replay from the third on is over 16 MB |
| `camera` | Chase-camera distance per craft at each frame rate | Mean distance differs from the 30 fps run by over 5% |

- `--sections` and `--filter` (folder names, such as `level03`) narrow a run. Images of targets you didn't run show as "not in this run", and `compare` exits non-zero because of them. An image only the current run has shows as "new".
- A crash, assertion or SEH line in the game log fails its target and leaves `faults.txt`. A target that fails to run leaves `error.txt`.
- `memory` takes the median because level 3 steps up once by about 64 MB, on the third or fourth replay, when the GPU allocator adds a block that it then keeps. A leak grows on every replay.
- Windows is the default platform. `--platforms linux,android,pi` runs the WSL build, the installed APK or the Pi over ssh, each configured with the MCP bridge on. `memory` runs on Windows only. The WSL game draws on a hidden Xvfb display (install `xvfb` in WSL), so it never takes focus. Set `RS64_WSL_SHOW=1` to see its window.

## Stability / leak harness

- `tools/debug/run-stability.ps1 -Runs N -Timeout S -Tag <label>` — N timed launches, per-run stderr in `logs/stability/<tag>/`, outcomes classified in its `summary.csv`. `-EnvVars "A=1;B=1"` forwards debug vars. `-Runs 1` for data capture, `-Runs 3` for stability-rate.
- `tools/debug/measure-leak.ps1 -Timeout S -Tag <label>` — single run, per-second WS/Private/VM to `memory.csv`.
- `tools/debug/recordings/` — per-mission input recordings (folder prefix = level id) plus `run-replays.ps1`, which replays every mission recording against its `.hash` baseline. See [tools/debug/recordings/README.md](../../tools/debug/recordings/README.md).

## Per-level performance

`tools/debug/recordings/run-level-perf.ps1` replays every level's mission recording, each in its own Release launch at 4x. Game fps is recorded frames over the mission's wall time, so 120 means the level kept up with 4x. Walk is RT64's display-list walk per frame (`ROGUESQ_LOG_FRAME_PROFILE`), hitches its `[frameprof HITCH]` lines, RAM the working set at mission start and end. The table also goes into the smoke report at `dumps/smoke/<ReportTag>/windows/report.html` (`-ReportTag`, default `current`), sortable by any column.

Windows, Release, 2026-11-25 (commit `7169b076`):

| Level                 | Frames | Secs | Game fps | Walk ms/f | Hitches | RAM MB    |
| --------------------- | ------ | ---- | -------- | --------- | ------- | --------- |
| 0 Mos Eisley          | 3766   | 31   | 119.9    | 2.02      | 1       | 498 → 499 |
| 1 Barkhesh            | 9840   | 82   | 119.9    | 2.14      | 0       | 487 → 501 |
| 2 Nonnah              | 8762   | 73   | 119.6    | 1.63      | 0       | 473 → 496 |
| 3 Corellia            | 15056  | 126  | 119.6    | 2.18      | 1       | 494 → 505 |
| 4 Gerrard V           | 7336   | 62   | 118.2    | 2.64      | 0       | 493 → 503 |
| 5 Jade Moon           | 7579   | 63   | 119.4    | 2.17      | 0       | 486 → 503 |
| 6 Construction Yards  | 14325  | 121  | 117.9    | 2.47      | 0       | 490 → 511 |
| 7 Kile II             | 9933   | 83   | 119.5    | 2.41      | 0       | 482 → 503 |
| 8 Rescue on Kessel    | 4195   | 35   | 118.5    | 2.37      | 1       | 488 → 502 |
| 9 Prisons of Kessel   | 18008  | 150  | 119.7    | 2.31      | 2       | 486 → 510 |
| 10 Taloraan           | 10272  | 86   | 119.9    | 1.94      | 0       | 493 → 499 |
| 11 Fest               | 10500  | 88   | 119.9    | 1.82      | 1       | 484 → 495 |
| 12 Chandrila          | 9040   | 75   | 120.1    | 1.95      | 0       | 495 → 501 |
| 13 Sullust            | 8989   | 75   | 119.6    | 2.00      | 0       | 487 → 501 |
| 14 Moff Seerdon       | 8832   | 74   | 119.9    | 1.77      | 1       | 494 → 497 |
| 15 Calamari           | 10323  | 88   | 117.2    | 2.57      | 0       | 493 → 509 |
| 16 Beggar's Canyon    | 2393   | 20   | 119.5    | 1.63      | 0       | 460 → 480 |
| 17 Trench Run         | 6426   | 58   | 111.7    | 3.00      | 0       | 470 → 498 |
| 18 Hoth               | 6585   | 55   | 119.8    | 2.26      | 0       | 487 → 502 |

Chaining the levels in one launch (a `ROGUESQ_LS_REPLAY` playlist) carries mission state over, so a later replay can fail its mission and then wait on the results screen. Chained, the first four levels grew 2-13 MB each, Barkhesh included.

## Validation harness (`tools/debug/validate/`)

The primary correctness workflow — diff a live run against a Project64 golden rather than eyeballing screenshots:

- `capture_pj64_golden.ps1` / `capture_menu_rdram.ps1` — scripted PJ64 goldens.
- `f5_dl_walk.py` — walks a Factor 5 display list offline; `--json` for machine diff, `--tex` for texture/UV inspection. `f5_dl_ndc.py` adds NDC projection.
- `dl_diff.py` — layer-by-layer DL diff vs golden.
- `rdram_golden_diff.py` — RDRAM diff vs PJ64 (`--focus ADDR:SIZE` for field-level compare).
- `state_diff.ps1` — **state-matched** decomp-vs-PJ64 memory diff: captures the recomp's RDRAM at several game states in one run (`ROGUESQ_DUMP_RDRAM_ON_STATE=a,b,c`, keyed to the game-state classifier) and diffs each vs a matching PJ64 golden (`dumps/pj64/rdram_state_<id>.bin`), focused via `focus_of.py` (state `focus` structs → `rdram_golden_diff --focus`). The two emulations compare by *logical state*, not wall-clock. PJ64 side needs state-tagged goldens (extend `pj64_rs64_dump.js`).
- `audio_cmd_walk.py` / `audio_diff.py` — MusyX voice-command walk and diff (perception-free audio validation).
- `compare_mesg_trace.py` / `symbolize_mesg_trace.py` — message-order trace comparison (pair with `ROGUESQ_LOG_MESG_TRACE`).
- `checkpoint.ps1` — orchestrates a capture + diff checkpoint.

## Frame-exact screenshots

The game runs on the user's machine and every tool call adds unknown latency, so never time a screenshot by wall clock (sleeping "about 90 s" lands seconds off). Hold the game on an exact frame instead, then capture it:

1. Launch with a pause list and a handshake base: `ROGUESQ_LS_PAUSE_AT=<frames>` for input replays (`tools/debug/recordings/capture-frames.ps1` does all of this), or `ROGUESQ_PAUSE_AT=<clock>[:<frame>],...` with `ROGUESQ_PAUSE_FILE=<base>` for anything else. A clock is a game state (`menu.options`, `mission`; frames since it was entered), `cutscene` (the cutscene's own frames) or `crawl` (the crawl clock at 30 a second, as the RS64 Explorer timelines count them).
2. Wait until `<base>.paused` exists (it holds the frame number); the game thread is held there, up to 30 s.
3. Capture the window by PID (`capture_window_by_pid`), never by title: other sessions may be running their own (often Debug) exe.
4. Write the frame number to `<base>.resume` to release it, then wait for the next `.paused`.

The [RT64 MCP server](rt64-mcp.md) does the same hold interactively, with no 30 s cap, and can inspect the held frame.

## Renaming (`tools/re/rename/`) and cross-refs (`tools/re/rz/`)

- `tools/re/rename/` — the symbol-renaming pipeline (`build_redefs.py`, `find_callers.py`, ...). After a rename batch, regen and build: a name that drifted between the ELF and `rogue_squadron.toml` hooks fails there.
- `tools/re/rz/rzq.py <cmd> <sym|0xADDR>` — rizin queries against the symbolized `rogue_squadron64` ELF: `xrefs`, `callees`, `disasm`, `strrefs`, `funcs`, `raw`. `--overlay mission|menu|cinematic` selects which overlay sits at 0x800A5130; `--project <file>` caches the ~25s analysis. See [tools/re/rz/README.md](../../tools/re/rz/README.md). Use it for tracing (who reads a global, a function's callers) before grepping `RecompiledFuncs/`; it needs rizin plus `pip install rzpipe`.
