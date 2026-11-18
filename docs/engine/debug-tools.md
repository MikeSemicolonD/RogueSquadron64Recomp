# Test, validation and RE tools

Crash and hang dumps are in [debugging-with-visual-studio.md](debugging-with-visual-studio.md#crashes-and-hangs). Live frame inspection is in [rt64-mcp.md](rt64-mcp.md).

## Stability / leak harness

- `tools/debug/run-stability.ps1 -Runs N -Timeout S -Tag <label>` — N timed launches, per-run stderr in `logs/stability/<tag>/`, outcomes classified in its `summary.csv`. `-EnvVars "A=1;B=1"` forwards debug vars. `-Runs 1` for data capture, `-Runs 3` for stability-rate.
- `tools/debug/measure-leak.ps1 -Timeout S -Tag <label>` — single run, per-second WS/Private/VM to `memory.csv`.
- `tools/debug/recordings/` — per-mission input recordings (folder prefix = level id) plus `run-replays.ps1`, which replays every mission recording against its `.hash` baseline. See [tools/debug/recordings/README.md](../../tools/debug/recordings/README.md).

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
