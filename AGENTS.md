# AGENTS.md

Guidance for AI agents working on the Rogue Squadron 64 Recompiled project — a static recompilation of *Star Wars: Rogue Squadron* (N64, USA v1.0) built with N64Recomp + RT64. The game is playable from the first level through the credits. What's left is the issues listed under Status in the README, plus symbol renaming. See [Status](README.md#status) in the README for the authoritative what-works snapshot before starting anything — this file assumes it.

Rules come first; the Reference section at the end links the docs. Some tools cut instruction files at 24 KB, so keep this file under that: explanations go in the docs.

## Replies and commits

- **No trailing summary blocks** in chat responses — one-line wrap-up max.
- Commit messages match the existing history: a short Title Case subject with no trailing period, under about 60 characters (GitHub cuts subjects at 72 and wraps the rest into the body; name the main change and leave the rest to the bullets); when a body is needed, `-` bullets stating what changed and why as plain fact, without "I". **No `Co-Authored-By` or other AI attribution trailers.** No semicolons tacking a one-off aside onto a bullet - make it its own bullet or cut it.
- **Never undo an edit with `git checkout` or `git restore`** when the file has other uncommitted work. Check `git diff --stat <file>` first and reverse your change with an edit.

## Skills

Task guides live in `.claude/skills/<name>/SKILL.md`. Agents without skill support should read the matching file before working in that area:

- `roguesquadron-debug` — crash/hang triage, toml hooks, perception-free validation
- `roguesquadron-menu-drive` — scripted menu input to reach a specific screen
- `roguesquadron-frame-interpolation` — 60+ Hz jitter, matrix and sprite ids
- `roguesquadron-raytracing` — RT lights, presets, the explorer Lights panel
- `f5-dl-validate` — Factor 5 display-list walks and diffs
- `renderdoc-mcp` — GPU frame captures
- `rs64-mcp-usage` — drive the live game: exact-frame pauses, flying the craft, screenshots, frame inspection

The rest are general toolchain references (cmake, msvc-cl, lldb, ...).

## Build & run

```powershell
# Build (Debug is the tested configuration)
cmake --build build --config Debug --target RogueSquadron64Recomp

# Run with stdout/stderr capture (PowerShell — bash redirects don't flush before SIGTERM)
$job = Start-Job -ScriptBlock {
    Set-Location "$using:PWD\build\Debug"
    & ".\RogueSquadron64Recomp.exe" *>&1
}
Start-Sleep -Seconds 30
Stop-Job $job
Receive-Job $job -Keep | Out-File -Encoding utf8 ..\probe_run.txt
Remove-Job $job
```

ROM lives at `build/Debug/rogue_squadron.z64` (USA v1.0, xxHash3-64 = `0x6B66A44153594DEA`).

Launch the game from its exe folder. From the repo root it uses and leaves `roguesq_*.json` files there.

Stop only the process you launched, by PID or the MCP `close`. Never stop the game by image name (`taskkill /IM`, `Get-Process ... | Stop-Process`): that closes the user's own game too. If a running exe you didn't start blocks the link, ask or build the other config.

For a headless run that drives past the title without a controller, pass `--fake-controller --auto-start <ms>` (or the env vars `ROGUESQ_FAKE_CONTROLLER=1` / `ROGUESQ_AUTO_START=<ms>`).

**Test against a mission recording in Release at `ROGUESQ_SPEED=16`, reading the log, whenever the check is a log or counter** (pairing rates, light/terrain traces, hashes): the 3126-frame `construction_yards_bonus.rec` runs from launch to exit in under a minute (`ROGUESQ_BOOT_TARGET=level:<id>,<craft>` from the `.rec` header, `ROGUESQ_LS_REPLAY`, `ROGUESQ_FAKE_CONTROLLER=1`, the trace vars, stderr redirected to a file under `dumps/`; the game exits when the replay ends). Debug is about 10x slower per frame. Use the Debug MCP session only when a frozen frame has to be inspected. Session recordings (`rs64-input`) diverge; use mission (`rs64-ls`) recordings.

**Keep one test session on one configuration.** Don't pair a Release instance with a Debug one (a Release co-op host with a Debug MCP client, say), and don't switch configs mid-session: their staged mods, saves and speeds differ. Use Debug only when it has something the test needs that Release lacks. MCP instance `p2` is for co-op tests; single-player tests use `p1`.

Unit tests: `cmake --build build --config Release --target rs64_unit_tests` builds and runs the host, multiplayer-mod and relay-server tests plus `style_check`. They are defined in `tests/CMakeLists.txt` (the mod's in `mods/multiplayer-native/tests/`, the relay's in `relay-server/tests/`), which also configures on its own with only the submodules: `cmake -S tests -B build-tests`. `touch_input_test`, `video_config_test`, `transition_gate_test`, `input_bindings_test` and `fault_guard_test` are separate `EXCLUDE_FROM_ALL` targets.

**Before committing code that could change frame rate, memory or game behaviour**, run the unit tests above, then `python -I tools/debug/smoke/smoke.py run --tag current --sections perf,memory --filter perf_level03_ultra,memory_level03` and `smoke.py compare --tag current`. Its `perf_level03_ultra` and `memory_level03` lines must read `ok`; ignore the "missing" rows. Delete `dumps/smoke/current` after. Docs, comments and small tool-only changes skip the smoke run.

## Overriding recompiled functions

Base-game fixes are `[[patches.hook]]` (C inserted at the function's entry or `before_vram`) and `[[patches.instruction]]` entries in `rogue_squadron.toml`, with any non-trivial host logic in [src/game/hook_helpers.cpp](src/game/hook_helpers.cpp) as an `extern "C"` function the hook declares and calls. Re-run `regen_funcs` after editing the toml. Whole-function C goes in the patches ELF instead ([patches/README.md](patches/README.md)); it rebuilds with the game.

- A hook is compiled into the recompiled body, so it runs for direct and `LOOKUP_FUNC` (indirect, `func_map`) calls alike. No link-order tricks.
- A hook's text is one call, `{ if (rs64_th_<func>_<vram|entry>(rdram, ctx)) return; }` or `{ rs64_th_<func>_<vram|entry>(rdram, ctx); }`; the style check rejects anything else. The body lives in [src/game/toml_hooks.c](src/game/toml_hooks.c) (declared in `include/rs64_toml_hooks.h`), compiled with the recompiled code, so it sees `rdram`, `ctx`, the `MEM_*` macros and `funcs.h`. `return 1` exits the whole game function (the first form only); set `ctx->r2` (or `ctx->f0`) first to return a value.
- Call recompiled game functions from a hook only through `mips_call`.

N64Recomp is `build/Debug/N64Recomp.exe` (override `N64RECOMP_EXE`), built from `lib/N64ModernRuntime/N64Recomp` (a directory in the N64ModernRuntime fork) via `N64RecompCLI`; `regen_funcs` depends on it. It supports `--dump-context`, `func_reference_syms_file` and `patch_files`.

Build a new gameplay mod as an `.nrm` unless a patch can't express it. Self-contained gameplay mods are `.nrm` code mods built with RecompModTool and loaded by librecomp; see `mods/infinite-secondary/` and [docs/guides/modding.md](docs/guides/modding.md). Native-library mods that hook game code through the host API: [docs/engine/modding-host-api.md](docs/engine/modding-host-api.md).

A mod not listed in `mods/platforms.json` ships nowhere, and a new code mod's `.nrm` must be listed under `desktop` to ship ([docs/guides/modding.md](docs/guides/modding.md), [docs/engine/release-ci.md](docs/engine/release-ci.md#code-mods)).

## When investigating a new crash

1. **Capture full output to a file.** Bash redirects don't flush before SIGTERM on Windows.
2. **Read the last few hundred lines** before the crash marker. The crash type is the bug class:
   - `Access violation reading address 0x1F8` (small) — NULL+offset deref, usually `hleGBI->map[opcode]` with NULL `hleGBI`. Guarded.
   - `Access violation reading address 0x1F2_xxxxxxxx` (huge) — stale GPU resource handle; renderer use-after-free.
   - `Assertion failed: ... rt64_gbi_f3d.cpp` — F3D handler hit an unimplemented case. Convert to log+skip.
   - `Assertion failed: ... rt64_native_target.cpp` — RT64 hit an unimplemented readback format. Convert to skip.
   - `vector subscript out of range` — STL bounds check, suppressed via `_CrtSetReportHook`.
   - `[null-call] addr=0x...` / `Failed to find function at 0x...` — recompiled MIPS called an unmapped fn-ptr. Stubbed.
   - `Unable to find a matching GBI in the current database` — unrecognized ucode task; the mid-task NULL guard catches the resulting deref, that geometry doesn't render.
3. **Rerun with the gfx traces**: `ROGUESQ_LOG_GFX_TASK=1` (one line per graphics task), `ROGUESQ_LOG_DL_HEALTH=1` (garbage color-image rejects, a desync signal) and `ROGUESQ_DESYNC_TRACE=1` (recent F5 commands ring) show which DL was in flight.
4. **Use `tools/debug/reconstruct-freeze.py` on a full dump** to validate queue-level theories before instrumenting: it lists every blocked game thread and its wait queue. `tools/debug/host-stacks.py` gives the symbolized host stacks.

For a hang, or a recompiled function that faults on data that decodes fine offline, suspect a codegen mistranslation of a rare instruction on that path (the voice-freeze cause was the `*al` link-branches) before deep subsystem RE.

For a bug that shows up only in Release (or only when the host is fast), suspect a game assumption that some producer is slower than a frame before suspecting the compiler. Example: `tickFormatMessageWorker` sent every reply as a pointer to one stack buffer; at -O2 it answered several requests before the menu polled once, the queued replies aliased, and SELECT GAME lost its medals/preview (fixed by the reply-ring hook in `rogue_squadron.toml`). `ROGUESQ_WATCH_ADDRS` and `ROGUESQ_DATA_BP` find the diverging write; the CMake `RS64_OD_TARGETS` / `RS64_OD_SOURCES` settings bisect by optimization level.

## Avoid these dead ends (already disproven)

### Rendering / GBI

- **`op_80` as a sub-DL call** — treating its 24-bit w0 as a call target infinite-loops and hangs after ~200 DLs. It's a state/param load.
- **Y-flip / component swap on model textures** — retired. The real cause was the missing `03 82` texcoord scale.
- **Moving more work from the CPU to the GPU to raise the frame rate** — the game caps itself at 30 fps, and RT64 already transforms vertices in GPU compute. Draw coalescing cut recording 4-8x with no change in frame rate.
- **Hand-built glyphs or shapes standing in for game text** — fix the game's own text path. One solid rect or triangle to prove the plumbing is the limit.
- **A `cv.wait` rewrite of RT64's present-queue busy-wait** (`advanceToNextPresent` in `rt64_present_queue.cpp`) — regressed natural-exit rate. Reverted.

### Boot flow / state machine

- **Force-menu bypass (poking fields to jump the state machine)** — skipping cinematic init crashes downstream or bails to attract. Use the nav sequencer.
- **Re-investigating a "missing state-1 writer" in the 5-slot table at `D_80154620`** — that's the speech/streamed-voice playback slots, not cinematic stages. The demo freeze it seemed to gate was the `bltzal` codegen bug.

### Build / regeneration

- **Hand-editing `funcs_*.c` for game-logic overrides** — next regen silently strips it. Use a `[[patches.hook]]`. Diagnostic `fprintf` probes are fine.
- **Regenerating `funcs_*.c` with a stale N64Recomp binary** — a binary built from old source errors on the `cache` instruction (its entrypoint recompile reads past the 0xC bound into `func_8000040C`) and **truncates `funcs.h` to ~7 lines**. `regen_funcs` rebuilds `N64RecompCLI` first, so this only happens with an `N64RECOMP_EXE` override pointing at an old exe; rebuild it and rerun `regen_funcs`. A good regen produces a ~2561-line `funcs.h`.
- **Never write repo files via Python `open(...,'w')`** — a bad Python write once truncated the entire GBI core and it had to be rebuilt from goldens. Use the editor tools.
- **Python steps in CMake or the build/regen pipeline** — a plain build needs no Python. Generate at build time with C++ (N64Recomp) or CMake script logic. Python is fine for offline tools under `tools/`.

### Miscellaneous

- **The `lld-link : warning : found both wmain and main`** — benign (SDL2main provides wmain, ours wins).
- **Shadowing `osPiStartDma_recomp` in `upstream_compat.cpp` with only the ROM-read branch** — boot needs the SRAM-read path too. Replicate `do_dma` in full or it regresses.

## Style conventions

`style_check` ([tests/style_check.cpp](tests/style_check.cpp), run by `rs64_unit_tests`) enforces these rules for `src/`, `relay-server/src` and `mods/multiplayer-native/src` (which may include only the base headers its CMakeLists lists), and a subset for `tools/web` ([web-js-style.md](docs/engine/web-js-style.md)); the baseline only shrinks (`build/Debug/style_check . --update-baseline` after fixing findings).

- **No emojis** in code, comments, or docs unless explicitly requested.
- **Comments: default to none.** Add one only when the WHY is non-obvious (a workaround for a specific bug, a hidden invariant, a non-visible constraint). At most 1-2 lines per block, stating the fact: no reasoning journey, dated history or "we tried X then Y", and nothing about the current task or session. Don't hard-wrap at ~90 columns; let a line run long. No trailing side comments after code (`x = 1;  // note`) and no notes tacked on after a semicolon. Config comments (e.g. `rogue_squadron.toml`) follow the same rules.
- When renaming symbols, avoid address-encoded names (`clearByteAt801128CC`) — they add nothing over `func_HHHHHHHH`. If you can't see the semantics, leave it as `func_*`.
- **One statement per line.** Every statement gets its own line — don't trail one after another on the same physical line separated by `;` (`extern T* g; g = x;` is two lines). Split a trailing guard onto the next line:
  ```c
  static int s_lo = -1;
  if (s_lo < 0) s_lo = env_on("ROGUESQ_LOG_AUDIO_OUT");
  ```
  Exempt: single-line loop bodies, aligned lookup/return ladders and tabular min/max updates, and the env-gated diagnostic probe blocks — keep those terse.
- **Name repeated strings.** A literal used more than once goes in a `static constexpr const char*` (`kAppName`) or a small helper (`rom_problem_text`); a local string that never changes is `const`. Env var names stay literal at each use so they can be grepped.
- **No bare `{ ... }` blocks.** A brace block needs a statement that owns it (`if`, loop, `switch` case, function, lambda). Unwrap a bare block into the surrounding code, renaming any locals that then clash.
- **Separate logical steps with blank lines.** Put a blank line after the opening brace of a function body, or of a lambda assigned to a variable (unless the body is one line or starts with a comment, which sits right under the brace); a lambda passed inline (an argument or an initializer field like `.message_box = [](...) {`) gets none. Add one before and after each `if`/loop block, and between steps (type aliases, setup, a guard, the call, its log line, the return). Keep declarations that belong together on adjacent lines, and leave no blank line before a closing brace. The opening brace goes on its own line for an early-exit guard whose body is only a `return`, and for an `if`, loop, `switch` or named function whose condition / parameter list or any direct body line is 70+ characters (or whose header spans lines); every other block, and every lambda, keeps it on the statement's line:
  ```cpp
  void set_window_frame_rate(void* native_window)
  {

      using SetFrameRate = int32_t (*)(ANativeWindow*, float, int8_t);

      static const auto s_fn = [] {

          void* lib = dlopen("libnativewindow.so", RTLD_NOW);
          void* sym = lib ? dlsym(lib, "ANativeWindow_setFrameRate") : nullptr;

          if (!sym) {
              sym = dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate");
          }

          return reinterpret_cast<SetFrameRate>(sym);
      }();

      if (!s_fn || !native_window)
      {
          return;
      }

      int32_t rc = s_fn(static_cast<ANativeWindow*>(native_window), 60.0f, 1);

      fprintf(stderr, "[perf] ANativeWindow_setFrameRate = %d\n", rc);
  }
  ```
  A comment that follows a statement starts a new step, so it gets a blank line above it; so does a log line (`fprintf`) after a non-log statement, and a multi-line statement ending in `};` (a lambda, struct or initializer) gets one before and after it.
  Preprocessor conditionals follow the same rule: a blank line before `#if` and after `#endif`, including between two conditionals in a row. A branch that holds more than a one-line statement (and its comment) also gets a blank line after its `#if`/`#elif`/`#else` and before the next directive, on both sides; a one-line branch stays tight:
  ```cpp
  #if defined(__linux__) && !defined(__ANDROID__)
      set_window_icon(sdl_window);
  #endif

      SDL_StopTextInput();

  #if defined(_WIN32)

      SDL_SysWMinfo wm{};

      SDL_VERSION(&wm.version);
      SDL_GetWindowWMInfo(sdl_window, &wm);

      return { wm.info.win.window };

  #else
      return sdl_window;
  #endif
  ```
- **Env gates on hot paths must read the environment once.** Use a `static const` initializer, or a distinct uninitialized sentinel checked with `== -1`. Never cache "off" as a negative value that a `< 0` check re-reads: 12 such gates in the F5 GBI ran `getenv()` on every command and were half the walk time.
- **Read env vars through the shared `recomp::dbg::env_*` helpers** (`src/util/debug_logs.h`: `env_on`/`env_int`/`env_str`/`env_u32`), not open-coded `getenv` parsing.
- Prefer **editing existing files** over creating new ones. The runtime is already large; new files attract drift.
- **Web JS** (`tools/web`): run `node tools/web/rs64-explorer/tests/jsfmt.mjs --write` after editing it. Its layout rules are in [docs/engine/web-js-style.md](docs/engine/web-js-style.md); read them before writing JS there.

## UI and docs

- **Minimal UI controls.** When placing buttons and toggles (RS64 Explorer, ImGui panels), keep them few and compact, use an icon with a tooltip instead of a text label wherever the action reads from an icon, and keep a toggle away from a destructive button. The RS64 Explorer's full design rules are in [tools/web/rs64-explorer/DESIGN.md](tools/web/rs64-explorer/DESIGN.md); follow them and update that file when a new rule is agreed.
- **Text a user reads in a tool says only what they need to use it**: VS Code task `detail`s and prompts, tooltips, help and status lines. Leave out how it works inside, such as how a path is resolved.
- **README and docs: write for the reader who needs it.** There are three readers: players (want a download and how to run it), builders (build, run, simple mods) and engine developers (internals, debugging). The README is curated by the maintainer: don't add sections to it without asking. A README build section holds only what a builder needs to act (commands, a gotcha that blocks the build, a link); player details (supported GPUs, distro/board lists, save locations, config keys) go in [docs/guides/building-all-platforms.md](docs/guides/building-all-platforms.md#what-players-need), and a runtime caveat is one bullet under Status. State each fact in one place and link to it; don't copy a doc's details into the README. Mark platforms that only build as untested. [docs/README.md](docs/README.md) is the docs index; put a new doc in `docs/guides/` (builders), `docs/engine/` (engine how-to) or `docs/reference/` (RE notes), and list it in the index under that reader.
- **When a fix resolves or changes a README Status issue**, update that entry in the same change.
- **Never mention or link `plans/` from committed files** (README, `docs/`, code and config comments). `plans/` is gitignored and never committed, so those references are dead for everyone else. State the fact or direction in plain words instead.

## Diagnostics and cleanup

- **Clean up after yourself** as soon as a test is done: scripts, logs, screenshots, temp folders and debug binaries you created, locally (`build/*/screenshots`, `dumps/`, scratch dirs in the repo) and on remote test machines such as the Pi. Never delete anything under `%TEMP%` or the agent's session scratchpad, even your own files. Leave only what the user asked for and the game's own data.
- For probe instrumentation in `funcs_*.c`, rate-limit:
  ```c
  { static int n=0; ++n; if (n<=10 || (n%50)==0) { fprintf(stderr, "..."); fflush(stderr); } }
  ```
- **Diagnostic accumulators must be bounded.** A `static` set/map/vector that a debug probe grows keyed by an ever-changing value (address, hash, DL/frame id) leaks over a long logging session. In `lib/rt64` use `rt64diag::BoundedSet` / `BoundedMap` ([lib/rt64/src/common/rt64_diag_bounds.h](lib/rt64/src/common/rt64_diag_bounds.h), FIFO eviction, default 5000, override `ROGUESQ_DIAG_CAP`); once saturated a set reports a recent-window count (`<= cap`), not an all-time total. Address/config-keyed trackers need no cap. The fixed ~4.9 GB Private / ~11 GB Virtual at boot is RT64's GPU commit, not a leak.
- **Remove a debug env var once the fix or feature it served is done** — the gate, the code behind it, its row in [docs/engine/debug-trace-env-vars.md](docs/engine/debug-trace-env-vars.md), and any comment naming it. Keep one only if it can test, validate or probe for other bugs (a trace category, a dump, an address watch, an A/B toggle for a still-heuristic path). A probe for one frame, one address or one already-verified fix does not qualify.

## Settings and identity

- **No env var, `--set` key or config field may pick an external identity** (Steam app id, store or account ids). Use a constant.
- **Hardware detection only seeds first-run defaults.** Never cap or override a saved setting because of detected hardware.

## Reference

Read the linked doc before working in that area.

- **Env vars and traces**: [docs/engine/debug-trace-env-vars.md](docs/engine/debug-trace-env-vars.md). Read it before adding a new `fprintf` or asking the user to enable logs. All traces are off by default, and any variable can be set with `--set NAME=VALUE`.
- **Where code lives**: [docs/engine/project-layout.md](docs/engine/project-layout.md). The forks under `lib/` carry intentional `if(false) fprintf(...)` toggles and game-specific guards: don't propose stripping them.
- **Test, validation and RE tools**: [docs/engine/debug-tools.md](docs/engine/debug-tools.md). Never time a screenshot by wall clock: hold an exact frame, then capture the window by PID. Trace callers and globals with `tools/re/rz/rzq.py` before grepping `RecompiledFuncs/`.
- **Crash and hang dumps**: [docs/engine/debugging-with-visual-studio.md](docs/engine/debugging-with-visual-studio.md#crashes-and-hangs).
- **Live frames**: [docs/engine/rt64-mcp.md](docs/engine/rt64-mcp.md). Use it instead of asking the user for F1 screenshots.
- **Factor 5 display lists**: [docs/reference/factor5-gbi.md](docs/reference/factor5-gbi.md) (opcodes) and [docs/reference/f5-model-dl-spec.md](docs/reference/f5-model-dl-spec.md) (grammar).
- **Co-op**: [docs/guides/multiplayer.md](docs/guides/multiplayer.md). The mod reaches the game only through `mp_host.h`, and a new base-side need is a flag or table entry, never a direct call. Host code reads the relocated per-player arrays through `rs64::ls::relocated()`.
- **Engine internals**: [docs/reference/game-architecture.md](docs/reference/game-architecture.md) (overlays, host RAM, state model, frame pacing, audio, scheduler queues, exception handling). Rules from it:
  - Overlay switching stays in `rs64_load_overlay`. Don't modify librecomp's DMA path.
  - Read the terrain grid tables through `rs64_tgrid_base`. A new fixed host RAM range goes in `src/util/host_ram_map.h`, and a mod gets memory from the host API's `alloc`.
  - Never call a recompiled function from the host to force state: inject input and write the fields the game's own confirm path reads. `gCurrentLevel` (0x130B70) is a u32.
  - The game caps itself at 30 fps. Per-frame logic that must run above 30 goes through `rs64_rate_scale()`, `rs64_rate_tick()` or `rs64_rate_drag`.
  - The `__except` filter in `librecomp/src/recomp.cpp` must let C++ exceptions (`0xE06D7363`) propagate.
  - N64Recomp must keep writing `ctx->r31 = PC+8` for the conditional branch-and-link ops.
