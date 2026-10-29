# AGENTS.md

Guidance for AI agents working on the Rogue Squadron 64 Recompiled project — a native port of *Star Wars: Rogue Squadron* (N64, USA v1.0) built with N64Recomp + RT64. The game is playable from the first level through the credits. What's left is the issues listed under Status in the README, plus symbol renaming. See [Status](README.md#status-playable) in the README for the authoritative what-works snapshot before starting anything — this file assumes it.

Rules come first; reference sections (Environment variables onward) follow. Some tools cut instruction files at 24 KB, so if yours stops before Architectural quirks, read the rest of this file directly.

## Replies and commits

- **No trailing summary blocks** in chat responses — one-line wrap-up max.
- Commit messages match the existing history: a short Title Case subject with no trailing period; when a body is needed, `-` bullets in a plain, first-person voice saying what changed and why. **No `Co-Authored-By` or other AI attribution trailers.** No semicolons tacking a one-off aside onto a bullet - make it its own bullet or cut it.

## Skills

Task guides live in `.claude/skills/<name>/SKILL.md`. Agents without skill support should read the matching file before working in that area:

- `roguesquadron-debug` — crash/hang triage, toml hooks, perception-free validation
- `roguesquadron-menu-drive` — scripted menu input to reach a specific screen
- `roguesquadron-frame-interpolation` — 60+ Hz jitter, matrix and sprite ids
- `roguesquadron-raytracing` — RT lights, presets, the explorer Lights panel
- `f5-dl-validate` — Factor 5 display-list walks and diffs
- `renderdoc-mcp` — GPU frame captures

The rest are general toolchain references (cmake, msvc-cl, lldb, ...).

## Build & run

```powershell
# Build (Debug is the tested configuration)
cmake --build build --config Debug --target RogueSquadron64Recomp

# Run with stdout/stderr capture (PowerShell — bash redirects don't flush before SIGTERM)
$job = Start-Job -ScriptBlock {
    Set-Location E:\Projects\RogueSquadron64Recomp\build\Debug
    & ".\RogueSquadron64Recomp.exe" *>&1
}
Start-Sleep -Seconds 30
Stop-Job $job
Receive-Job $job -Keep | Out-File -Encoding utf8 ..\probe_run.txt
Remove-Job $job
```

ROM lives at `build/Debug/rogue_squadron.z64` (USA v1.0, xxHash3-64 = `0x6B66A44153594DEA`).

For a headless run that drives past the title without a controller, pass `--fake-controller --auto-start <ms>` (or the env vars `ROGUESQ_FAKE_CONTROLLER=1` / `ROGUESQ_AUTO_START=<ms>`).

Unit tests: `cmake --build build --config Release --target rs64_unit_tests` builds and runs the host, multiplayer-mod and relay-server tests plus `style_check`. They are defined in `tests/CMakeLists.txt` (the mod's in `mods/multiplayer-native/tests/`, the relay's in `relay-server/tests/`), which also configures on its own with only the submodules: `cmake -S tests -B build-tests`. `touch_input_test`, `video_config_test`, `transition_gate_test`, `input_bindings_test` and `fault_guard_test` are separate `EXCLUDE_FROM_ALL` targets.

## Overriding recompiled functions

Base-game fixes are `[[patches.hook]]` (C inserted at the function's entry or `before_vram`) and `[[patches.instruction]]` entries in `rogue_squadron.toml`, with any non-trivial host logic in [src/game/hook_helpers.cpp](src/game/hook_helpers.cpp) as an `extern "C"` function the hook declares and calls. Re-run `regen_funcs` after editing the toml. Whole-function C goes in the patches ELF instead ([patches/README.md](patches/README.md)); it rebuilds with the game.

- A hook is compiled into the recompiled body, so it runs for direct and `LOOKUP_FUNC` (indirect, `func_map`) calls alike. No link-order tricks.
- A hook's text is one call, `{ if (rs64_th_<func>_<vram|entry>(rdram, ctx)) return; }` or `{ rs64_th_<func>_<vram|entry>(rdram, ctx); }`; the style check rejects anything else. The body lives in [src/game/toml_hooks.c](src/game/toml_hooks.c) (declared in `include/rs64_toml_hooks.h`), compiled with the recompiled code, so it sees `rdram`, `ctx`, the `MEM_*` macros and `funcs.h`. `return 1` exits the whole game function (the first form only); set `ctx->r2` (or `ctx->f0`) first to return a value.
- Call recompiled game functions from a hook only through `mips_call`.

N64Recomp is `build/Debug/N64Recomp.exe` (override `N64RECOMP_EXE`), built from `lib/N64ModernRuntime/N64Recomp` (a directory in the N64ModernRuntime fork) via `N64RecompCLI`; `regen_funcs` depends on it. It supports `--dump-context`, `func_reference_syms_file` and `patch_files`.

Self-contained gameplay mods are `.nrm` code mods built with RecompModTool and loaded by librecomp; see `mods/infinite-secondary/` and [docs/guides/modding.md](docs/guides/modding.md). Native-library mods that hook game code through the host API: [docs/engine/modding-host-api.md](docs/engine/modding-host-api.md).

`mods/platforms.json` decides which mods ship per platform: a mod not listed there ships nowhere. `desktop` entries (data-only folders and `.nrm` files) are staged next to the exe by `tools/mods/stage_mods.cmake`; `android` entries are packed into the APK. Release CI builds every `.nrm` listed under `desktop` via `tools/mods/build_code_mods.cmake`, matching each entry to the `mods/*/mod.toml` whose `mod_filename` equals the `.nrm` name, so a new code mod must be added to that list to ship ([docs/engine/release-ci.md](docs/engine/release-ci.md#code-mods)).

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

For a hang, or a recompiled function that faults on data that decodes fine offline, suspect a codegen mistranslation of a rare instruction on that path (the voice-freeze cause was the `*al` link-branches, see Audio) before deep subsystem RE.

For a bug that shows up only in Release (or only when the host is fast), suspect a game assumption that some producer is slower than a frame before suspecting the compiler. Example: `tickFormatMessageWorker` sent every reply as a pointer to one stack buffer; at -O2 it answered several requests before the menu polled once, the queued replies aliased, and SELECT GAME lost its medals/preview (fixed by the reply-ring hook in `rogue_squadron.toml`). `ROGUESQ_WATCH_ADDRS` and `ROGUESQ_DATA_BP` find the diverging write; the CMake `RS64_OD_TARGETS` / `RS64_OD_SOURCES` settings bisect by optimization level.

## Avoid these dead ends (already disproven)

### Rendering / GBI

- **`op_80` as a sub-DL call** — treating its 24-bit w0 as a call target infinite-loops and hangs after ~200 DLs. It's a state/param load.
- **Y-flip / component swap on model textures** — retired. The real cause was the missing `03 82` texcoord scale (see the Factor 5 GBI section).
- **Moving more work from the CPU to the GPU to raise the frame rate** — the game caps itself at 30 fps (see Frame pacing), and RT64 already transforms vertices in GPU compute. Draw coalescing cut recording 4-8x with no change in frame rate.
- **A `cv.wait` rewrite of RT64's present-queue busy-wait** (`advanceToNextPresent` in `rt64_present_queue.cpp`) — regressed natural-exit rate. Reverted.

### Boot flow / state machine

- **Force-menu bypass (poking fields to jump the state machine)** — skipping cinematic init crashes downstream or bails to attract. Use the nav sequencer.
- **Re-investigating a "missing state-1 writer" in the 5-slot table at `D_80154620`** — that's the speech/streamed-voice playback slots, not cinematic stages. The demo freeze it seemed to gate was the `bltzal` codegen bug.

### Build / regeneration

- **Hand-editing `funcs_*.c` for game-logic overrides** — next regen silently strips it. Use a `[[patches.hook]]`. Diagnostic `fprintf` probes are fine.
- **Regenerating `funcs_*.c` with a stale N64Recomp binary** — a binary built from old source errors on the `cache` instruction (its entrypoint recompile reads past the 0xC bound into `func_8000040C`) and **truncates `funcs.h` to ~7 lines**. `regen_funcs` rebuilds `N64RecompCLI` first, so this only happens with an `N64RECOMP_EXE` override pointing at an old exe; rebuild it and rerun `regen_funcs`. A good regen produces a ~2561-line `funcs.h`.
- **Never write repo files via Python `open(...,'w')`** — a bad Python write once truncated the entire GBI core and it had to be rebuilt from goldens. Use the editor tools.

### Miscellaneous

- **The `lld-link : warning : found both wmain and main`** — benign (SDL2main provides wmain, ours wins).
- **Shadowing `osPiStartDma_recomp` in `upstream_compat.cpp` with only the ROM-read branch** — boot needs the SRAM-read path too. Replicate `do_dma` in full or it regresses.

## Style conventions

`style_check` ([tests/style_check.cpp](tests/style_check.cpp), run by `rs64_unit_tests`) enforces these rules for `src/`, `relay-server/src` and `mods/multiplayer-native/src` (which may include only the base headers its CMakeLists lists); the baseline only shrinks (`build/Debug/style_check . --update-baseline` after fixing findings).

- **No emojis** in code, comments, or docs unless explicitly requested.
- **Minimal UI controls.** When placing buttons and toggles (RS64 Explorer, ImGui panels), keep them few and compact, use an icon with a tooltip instead of a text label wherever the action reads from an icon, and keep a toggle away from a destructive button. The RS64 Explorer's full design rules are in [tools/web/rs64-explorer/DESIGN.md](tools/web/rs64-explorer/DESIGN.md); follow them and update that file when a new rule is agreed.
- Default to **no comments**. Add one only when the WHY is non-obvious (a workaround for a specific bug, a hidden invariant, a non-visible constraint).
- **Comments are terse and matter-of-fact.** State the fact, not the reasoning journey. No multi-paragraph narration, no dated blow-by-blow history, no "we tried X then Y" storytelling in a comment — one or two plain lines. This applies to config comments (e.g. `rogue_squadron.toml`) too.
- **At most 1-2 lines per comment block, and don't hard-wrap at ~90 columns** — let a line run long rather than splitting one sentence across several. No trailing side comments after code (`x = 1;  // note`) and no extra notes tacked on after a semicolon; if it matters, it goes in the one comment above the code.
- Don't reference the current task or session in comments — they rot.
- **README and docs: write for the reader who needs it.** There are three readers: players (want a download and how to run it), builders (build, run, simple mods) and engine developers (internals, debugging). The README is curated by the maintainer: don't add sections to it without asking. A README build section holds only what a builder needs to act (commands, a gotcha that blocks the build, a link); player details (supported GPUs, distro/board lists, save locations, config keys) go in [docs/guides/building-all-platforms.md](docs/guides/building-all-platforms.md#what-players-need), and a runtime caveat is one bullet under Status. State each fact in one place and link to it; don't copy a doc's details into the README. Mark platforms that only build as untested. [docs/README.md](docs/README.md) is the docs index; put a new doc in `docs/guides/` (builders), `docs/engine/` (engine how-to) or `docs/reference/` (RE notes), and list it in the index under that reader.
- **Never mention or link `plans/` from committed files** (README, `docs/`, code and config comments). `plans/` is gitignored and never committed, so those references are dead for everyone else. State the fact or direction in plain words instead.
- Prefer **editing existing files** over creating new ones. The runtime is already large; new files attract drift.
- **Clean up after yourself** as soon as a test is done: scripts, logs, screenshots, temp folders and debug binaries you created, locally (`build/*/screenshots`, `%TEMP%`, scratch dirs) and on remote test machines such as the Pi. Leave only what the user asked for and the game's own data.
- For probe instrumentation in `funcs_*.c`, rate-limit:
  ```c
  { static int n=0; ++n; if (n<=10 || (n%50)==0) { fprintf(stderr, "..."); fflush(stderr); } }
  ```
- **Diagnostic accumulators must be bounded.** Any `static` set/map/vector that a debug probe grows keyed by an ever-changing value (address, hash, DL/frame id) leaks to runaway memory over a long logging session if it is never evicted — a distinct-hash set is the classic offender. Cap them keep-recent instead: in `lib/rt64` use `rt64diag::BoundedSet` / `BoundedMap` ([lib/rt64/src/common/rt64_diag_bounds.h](lib/rt64/src/common/rt64_diag_bounds.h), FIFO eviction, default 5000, override `ROGUESQ_DIAG_CAP`); once saturated a set reports a recent-window count (`<= cap`), not an all-time total. Address/config-keyed trackers are naturally small and need no cap. The fixed ~4.9 GB Private / ~11 GB Virtual at boot is RT64's GPU commit, not a leak.
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
- **Remove a debug env var once the fix or feature it served is done** — the gate, the code behind it, its row in [docs/engine/debug-trace-env-vars.md](docs/engine/debug-trace-env-vars.md), and any comment naming it. Keep one only if it can test, validate or probe for other bugs (a trace category, a dump, an address watch, an A/B toggle for a still-heuristic path). A probe for one frame, one address or one already-verified fix does not qualify.

## Environment variables

All trace categories are off by default and there is no catch-all log switch. The full catalog is in [docs/engine/debug-trace-env-vars.md](docs/engine/debug-trace-env-vars.md) — **read it before adding a new `fprintf` or asking the user to enable logs.** The common switches below also have `--flags` (`--help`); any variable can be set with `--set NAME=VALUE`.

| Env var | Effect |
|---|---|
| `ROGUESQ_GFX_API=vulkan\|d3d12\|d3d11\|d3d10` | Force the graphics API (default auto) |
| `ROGUESQ_HLE_DEV_MODE=0\|1` | RT64 ImGui inspector on F1 (default on in Debug, off in Release) |
| `ROGUESQ_VI_DRIVEN_LOOP=0` | Old host-paced frame loop instead of the hardware VI protocol (the stability baseline, default on) |
| `ROGUESQ_F5_NATIVE=0` | Parse F5 display lists without emitting geometry |
| `ROGUESQ_NO_AUDIO_UCODE=1` | Silent audio stub instead of the MusyX synth |
| `ROGUESQ_DUMP_PCM=<path>\|1` | Write the synth output to a 22050 Hz stereo WAV (`1` = `dumps/wav/capture.wav`) |
| `ROGUESQ_FAKE_CONTROLLER=1` / `ROGUESQ_AUTO_START=<ms>` | Headless runs: fake a controller, pulse START |
| `ROGUESQ_LOG_GBI=1` / `ROGUESQ_LOG_GFX_TASK=1` | Per-handler GBI logs (high volume) / one line per graphics task |
| `ROGUESQ_LOG_MESG_TRACE=1` (+ `ROGUESQ_MESG_TRACE_FRAMES=lo-hi`) | Thread/message-order trace for `tools/debug/validate/compare_mesg_trace.py` |
| `ROGUESQ_DUMP_FRAME_DL=N` / `ROGUESQ_DUMP_TEXTURES=1` | Debug builds only: log gfx tasks N..N+15 to stderr (full detail for N, N+1); textures of those tasks go to `dumps/tex/` |

With the inspector on, F1 toggles RT64's ImGui overlay, F3 toggles ViewRDRAM mode, F4 toggles texture replacement, and F12 writes a crash dump. F9 / Shift+F9 step through the ray-tracing debug views live (named in a bottom-right notice).

## Project layout

```
src/                                    Host code; the include root, so includes are folder-qualified ("input/touch_input.h")
  ├── main/                             main.cpp (game registration, window/gfx/VI callbacks, main()), cli (command-line flags), crash (Windows crash handlers, minidumps), debug_pollers (ROGUESQ_LOG_STATE/PHASE threads), ui_panels (ImGui controls/mods/touch overlay), android_host, renderdoc_capture, mcp_bridge (dev-only loopback socket for tools/debug/mcp)
  ├── audio/                            RSP microcode dispatch (MusyX synth runner) + SDL audio output
  ├── input/                            input_host (gamepads, joysticks, keyboard/mouse, touch, sensors; the SDL event loop and N64 pad polling), input_bindings, rumble, touch_input / touch_menu / touch_config
  ├── video/                            rt64_render_context (RT64 host context; HLE send_dl, present-side fixes, boot-target driver), video_config
  ├── game/                             Code that hooks the recompiled game: hook_helpers (entry points for rogue_squadron.toml hooks), toml_hooks.c, upstream_compat (libultra shims, scheduler overrides, rs64_load_overlay), register_overlays / register_patches, game_state + nav_sequencer (state classifier, BOOT_TARGET driver), menu_config, builtin_hooks, camera_shots.h, mod_levels.h
  ├── host_api/                         Mod host API (public header include/rs64/host_api.h)
  ├── lockstep/                         Input record/replay and frame hashing
  └── util/                             debug_logs.h (env_* helpers), mips_call.h, rdram_words.h
mods/                                   Data-only, .nrm and native mods; mods/platforms.json picks what ships
patches/                                Base-game C patches (patches/src), built into the patches ELF on every build; needs MIPS GCC
mods/multiplayer-native/                Online co-op as a native mod, always built by the game's CMake (the exe has no multiplayer code)
  ├── src/                              ghost*.cpp (session, lobby, HUD), net_*.cpp (ENet6), relay_client.cpp (rs64_relay transport), coop_imposter.cpp, mp_core.cpp (imposter flight model, puppet, lobby rules), mp_register.cpp + mp_host.h (host-API registrations, its only route to the game)
  ├── tests/                            mp_core_test, net_core_test, net_link_test (run by rs64_unit_tests)
  ├── mod.toml, tools/                  Manifest plus the per-player data relocation as [[patches.instruction]] (RecompModTool writes mod.json / instruction_patches.json at build time) and its generator
  └── lib/enet6/, lib/miniupnp/         Submodules for the transport, built only for this mod
relay-server/                           rs64_relay, the standalone co-op relay server (lists and pairs games, forwards packets); builds alone with only enet6, released by relay-release.yml
tests/                                  Host unit tests (rs64_unit_tests)
rogue_squadron.toml, patches.toml       N64Recomp configs (game ELF / patches ELF)
rsp/                                    RSPRecomp configs (MusyX synth, shared boot ucode)
tools/state/state_model.toml            Game-state descriptor (state_table.inl codegen + Python tools)
lib/N64ModernRuntime/                   Submodule — fork at MikeSemicolonD/N64ModernRuntime
  ├── librecomp/                        Recompiler runtime (overlay loading, get_function, SEH)
  └── ultramodern/                      libultra emulation (threads, mesgqueue, events)
lib/rt64/                               Submodule — fork at MikeSemicolonD/rt64
  └── src/gbi/rt64_gbi_f3dfactor5.cpp   Native Factor 5 GBI profile (the render core)
syms/                                   Submodule (RogueSquadron64RecompSyms): function + data symbols and the mods data variant. dump_syms rewrites them; commit inside syms/, then bump the pointer
android/, ci/                           Android Gradle project; release CI runner scripts
docs/                                   guides/ (build, mods, co-op), engine/ (debugging, env vars, host API, CI), reference/ (RE notes; its README is the on-ramp)
tools/
  ├── debug/                            Crash/hang triage, stability + leak harnesses, menu driving, validate/, lockstep/, recordings/
  ├── re/                               Reverse-engineering helpers: rz/, rename/, render/, MORT decoders, one-off disasm/decode probes
  ├── release/                          Versioning, release bundling, packaging and cross-builds (linux/, macos/, android/)
  ├── mods/, state/, style/, d3d11/     Build inputs read by CMake/CI (mod build scripts, state model, style baseline, D3D11 shader combos)
  └── web/                              Browser tools (rs64-explorer/, its data generator, save-editor/), deployed to GitHub Pages by .github/workflows/pages.yml on pushes to main that touch the site (the landing page is the root index.html)
build/                                  CMake out-of-source build dir
```

The recompiled MIPS code lives in-repo at `RecompiledFuncs/` — `funcs_*.c` plus `funcs.h`, `lookup.cpp`, and `recomp_overlays.inl`. It is **gitignored** (a derivative of the copyrighted ROM; generated locally, never committed) and regenerated from `rogue_squadron.toml` with `cmake --build build --config Debug --target regen_funcs`; the next build re-globs automatically (`CONFIGURE_DEPENDS`). Regen reads the decomp ELF at `../rogue_squadron64/build/roguesquadron.elf`, and rebuilds `N64RecompCLI` first. N64Recomp fails the regen if any patch (including the co-op relocation in `patch_files`) names a missing function or vram. These are auto-generated; hand-instrumenting them with diagnostic `fprintf` probes is routine, but load-bearing logic belongs in `rogue_squadron.toml` hooks (see Overriding recompiled functions), not here — regeneration silently discards inline edits.

The forked submodules under `lib/` carry intentional `if(false) fprintf(...)` debug-toggle cruft and game-specific defensive guards. **Do not propose stripping these** as cleanup; they are intentional.

## Tooling (under `tools/`)

### Crash / hang triage

1. **Crash**: the handlers in `src/main/crash.cpp` (SEH, SIGABRT, F12) write a full-memory `dumps/crash-dumps/crash_*.dmp`. Open in VS (File → Open → File → .dmp → Debug with Native Only).
2. **Hang**: from another shell, `pwsh tools/debug/dump-game.ps1` captures the running process even when the window is "Not Responding" (F12 won't fire if the message pump is starved). Run `tools/debug/reconstruct-freeze.py` on it (blocked game threads and their queues, from RDRAM) and `tools/debug/host-stacks.py` (symbolized host stacks; Debug build, refuses on a stale exe/PDB).
3. **Triage threads**: `python tools/debug/inspect-dump.py` lists threads in the most recent dump and tags ones running our exe code as `in_exe`. Those 2–4 are the only ones worth opening in VS; the rest are runtime workers parked in ntdll.

To read a dump from the shell, use WinDbg's console debugger (`winget install Microsoft.WinDbg`): `cdbX64.exe -z <dmp> -y build\Debug -c "~*k;q"` prints every thread's stack. The Windows SDK's own `cdb.exe` may not start (its `dbghelp.dll` was replaced by a newer, incompatible one).

### Stability / leak harness

- `tools/debug/run-stability.ps1 -Runs N -Timeout S -Tag <label>` — N timed launches, per-run stderr in `logs/stability/<tag>/`, outcomes classified in its `summary.csv`. `-EnvVars "A=1;B=1"` forwards debug vars. `-Runs 1` for data capture, `-Runs 3` for stability-rate.
- `tools/debug/measure-leak.ps1 -Timeout S -Tag <label>` — single run, per-second WS/Private/VM to `memory.csv`.
- `tools/debug/recordings/` — per-mission input recordings (folder prefix = level id) plus `run-replays.ps1`, which replays every mission recording against its `.hash` baseline. See [tools/debug/recordings/README.md](tools/debug/recordings/README.md).

### Validation harness (`tools/debug/validate/`)

The primary correctness workflow — diff a live run against a Project64 golden rather than eyeballing screenshots:

- `capture_pj64_golden.ps1` / `capture_menu_rdram.ps1` — scripted PJ64 goldens.
- `f5_dl_walk.py` — walks a Factor 5 display list offline; `--json` for machine diff, `--tex` for texture/UV inspection. `f5_dl_ndc.py` adds NDC projection.
- `dl_diff.py` — layer-by-layer DL diff vs golden.
- `rdram_golden_diff.py` — RDRAM diff vs PJ64 (`--focus ADDR:SIZE` for field-level compare).
- `state_diff.ps1` — **state-matched** decomp-vs-PJ64 memory diff: captures the recomp's RDRAM at several game states in one run (`ROGUESQ_DUMP_RDRAM_ON_STATE=a,b,c`, keyed to the game-state classifier) and diffs each vs a matching PJ64 golden (`dumps/pj64/rdram_state_<id>.bin`), focused via `focus_of.py` (state `focus` structs → `rdram_golden_diff --focus`). The two emulations compare by *logical state*, not wall-clock. PJ64 side needs state-tagged goldens (extend `pj64_rs64_dump.js`).
- `audio_cmd_walk.py` / `audio_diff.py` — MusyX voice-command walk and diff (perception-free audio validation).
- `compare_mesg_trace.py` / `symbolize_mesg_trace.py` — message-order trace comparison (pair with `ROGUESQ_LOG_MESG_TRACE`).
- `checkpoint.ps1` — orchestrates a capture + diff checkpoint.

### Frame-exact screenshots

The game runs on the user's machine and every tool call adds unknown latency, so never time a screenshot by wall clock (sleeping "about 90 s" lands seconds off). Hold the game on an exact frame instead, then capture it:

1. Launch with a pause list and a handshake base: `ROGUESQ_LS_PAUSE_AT=<frames>` for input replays (`tools/debug/recordings/capture-frames.ps1` does all of this), or `ROGUESQ_PAUSE_AT=<clock>[:<frame>],...` with `ROGUESQ_PAUSE_FILE=<base>` for anything else. A clock is a game state (`menu.options`, `mission`; frames since it was entered), `cutscene` (the cutscene's own frames) or `crawl` (the crawl clock at 30 a second, as the RS64 Explorer timelines count them).
2. Wait until `<base>.paused` exists (it holds the frame number); the game thread is held there, up to 30 s.
3. Capture the window by PID (`capture_window_by_pid`), never by title: other sessions may be running their own (often Debug) exe.
4. Write the frame number to `<base>.resume` to release it, then wait for the next `.paused`.

The RT64 MCP server below does the same hold interactively, with no 30 s cap, and can inspect the held frame.

### Live frame inspection (RT64 MCP, `tools/debug/mcp/`)

`tools/debug/mcp/rs64_mcp.py` is a stdio MCP server (`rs64` in the local `.mcp.json`) that launches the game itself and gives an agent what the F1 inspector shows; use it instead of asking the user for F1 screenshots. `pause_at` / `step` hold the game thread on exactly frame N of a clock (RT64's own F4 pause does not stop the game), then draws, textures, framebuffers, RDRAM and screenshots can be read from the held frame. Trace categories (`ROGUESQ_LOG_*`) go in `launch`'s `env`. Tool list and details: [docs/engine/rt64-mcp.md](docs/engine/rt64-mcp.md).

### Renaming (`tools/re/rename/`) and cross-refs (`tools/re/rz/`)

- `tools/re/rename/` — the symbol-renaming pipeline (`build_redefs.py`, `find_callers.py`, ...). After a rename batch, regen and build: a name that drifted between the ELF and `rogue_squadron.toml` hooks fails there.
- `tools/re/rz/rzq.py <cmd> <sym|0xADDR>` — rizin queries against the symbolized `rogue_squadron64` ELF: `xrefs`, `callees`, `disasm`, `strrefs`, `funcs`, `raw`. `--overlay mission|menu|cinematic` selects which overlay sits at 0x800A5130; `--project <file>` caches the ~25s analysis. See [tools/re/rz/README.md](tools/re/rz/README.md). Use it for tracing (who reads a global, a function's callers) before grepping `RecompiledFuncs/`; it needs rizin plus `pip install rzpipe`.

## Architectural quirks worth knowing

### Overlays: registered at boot, switched by a hook

librecomp's section table covers all three `.ovl.*` overlays (mission / menu / cinematic), which share `ram_addr 0x800A5130`. They are all registered at boot in [src/game/register_overlays.cpp](src/game/register_overlays.cpp) via `recomp::overlays::register_overlays` (the Zelda64Recomp pattern). Which one the `func_map` resolves to is switched at runtime by a `[[patches.hook]]` on `loadOverlay` (0x80000B3C, `s0` = id 0 mission / 1 menu / 2 cinematic) calling `rs64_load_overlay` in [upstream_compat.cpp](src/game/upstream_compat.cpp), which unloads and loads the overlay through librecomp's API. Keep overlay logic there; don't modify librecomp's DMA path.

### Terrain grid is 128x128 in host RAM

The flight terrain's view grid tables (span tables, two byte tables, the per-cell pointer table) are relocated to host RDRAM above the game's 8 MB, and their row strides are doubled by `[[patches.instruction]]` entries in `rogue_squadron.toml`. Any new code touching these tables must go through `rs64_tgrid_base`.

Host-side tables above 8 MB (keep any new one clear of all of these):

| Range | Contents |
|---|---|
| 0x80800000-0x8080FFFF | librecomp's cart PI handle; patches ELF data at 0x80801000 |
| 0x80810000-0x809FFFFF | Scene texture pool (language mod only) |
| 0x80A00000-0x80A01FFF | F5 renderer vertex scratch |
| 0x80A10000-0x80A4BFFF | Terrain edit API tile table (host API v7, `src/game/terrain_edit.h`) |
| 0x80B00000-0x80B1FFFF | 128x128 terrain grid tables |
| 0x80B38058-0x80B3901F, 0x80B58930-0x80B58D0F | Co-op relocated per-player ranges (mod only, `kMovedRanges` in `mp_core.h`) |
| 0x80B40000-0x80B4015F | Co-op imposter DAT record and message scratch (mod only) |
| 0x80C00000-0x80C3FFFF | Manifests grown with mod assets, 64 KB per `gManifestTable` slot ([hook_helpers.cpp](src/game/hook_helpers.cpp)) |
| 0x80C40000-0x80C400FF | Camera name for a mod-linked mid-mission shot |
| 0x80C40100-0x80C401FF | Mod level's redirected asset name |
| 0x80C40200-0x80C40223 | Mod level medal_info records |
| 0x80C40300-0x80C40367 | SELECT GAME mod medal text |
| 0x80C41000-0x80C41FFF | Mod level opening crawl text |
| 0x80C42000-0x80C420FF | Pause-list objective text |
| 0x80C42100-0x80C4217F | Objective camera names |
| 0x80C42200-0x80C4223F | Level rules object names |
| 0x80C43000-0x80C43FFF | Pause menu GAME SETTINGS, mod-page and root arrays |
| 0x80C44000-0x80CE3FFF | Language mod: extended fonts, enlarged texture/material tables, encoded text ([src/game/lang.cpp](src/game/lang.cpp)) |
| 0x80CE4000-0x80FFFFFF | Scene texture pool (language mod only) |

`ROGUESQ_DRAW_DIST` (0.25-8) scales the far plane and object culls; terrain reach and fog follow it capped at 2.5x (`ROGUESQ_TERRAIN_DIST` overrides), and the level cell budget doubles at 2x+ (`ROGUESQ_TGRID_BUDGET_MULT`).

### Co-op lives in the `multiplayer-native` mod

Online co-op is a native mod; the exe has no multiplayer code and the base build has the original data layout. The mod reaches the game only through `mp_host.h`, and base code sees it only through hooks, actions and flags, so a new base-side need is a flag or table entry, never a direct call. Player 2 is an imposter wingman NPC, and while the mod is on four ranges of per-player arrays are relocated to host RAM, so host code reads them through `rs64::ls::relocated()` and RDRAM goldens show them as zero at their old addresses. Imposter, relocation, protocol and gotchas: [docs/guides/multiplayer.md](docs/guides/multiplayer.md).

### Game-state model + BOOT_TARGET nav engine

A canonical game-state model classifies the current state each present from RDRAM: descriptor `tools/state/state_model.toml` → `tools/state/gen_state_table.cpp` (CMake `gen_state_table`) → `src/game/state_table.inl`, host classifier in [src/game/game_state.cpp](src/game/game_state.cpp) (`rs64_state_current_id`), Python tools read the same TOML. `ROGUESQ_LOG_GAMESTATE=1` prints the classified state. Discriminators are verified against live RDRAM (e.g. mission = `numMissionObjectives` 0x130B17 != 0; menu = `gCurrentMenuData` 0x800CE730; menu id at 0x800CE734; pilot sub-step 0x800CE626). A **headless scripted virtual controller** (`ROGUESQ_INPUT_SEQ`, injected at `get_n64_input` **before** `resolve()` so keyboard-active runs don't swallow it) drives menus without window focus.

`ROGUESQ_BOOT_TARGET=level:<id>[,craft]` is a state-gated **nav sequencer** ([src/game/nav_sequencer.cpp](src/game/nav_sequencer.cpp)) that drives the real menus to a mission. Never call a recompiled function from the host to force state — inject input + write fields the game's own confirm path reads (e.g. `gCurrentLevel` 0x130B70 is a **u32**, not a byte — a byte write = out-of-range id = crash). `demo:<n>` auto-disables the custom menu (it displaced the attract idle path) so the chosen attract demo plays; firing it *instantly* is unsolved (idle trigger is `(clock − lastInputFrame) > threshold`, `lastInputFrame` not locatable statically).

### Factor 5 GBI — custom opcodes

This game uses a Factor 5-customized F3DEX-derived ucode. The RT64 profile `GBI_F3DFACTOR5` lives in [lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp](lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp) and inherits from `GBI_F3DEX`. The canonical opcode map (with observed w0/w1 patterns and disproven interpretations) is in [docs/reference/factor5-gbi.md](docs/reference/factor5-gbi.md); the DL grammar itself is in [docs/reference/f5-model-dl-spec.md](docs/reference/f5-model-dl-spec.md). Confirmed Factor 5-specific behaviors:

| Opcode | Standard meaning | Factor 5 behavior | Handler |
|-------:|------------------|-------------------|---------|
| `0xB5` | F3DEX `G_QUAD` | **Next chunk**: continue in the chunk named by the current chunk header's first word (w1 ignored); `0xB8` is the pop/return | `op_b5_next_chunk` |
| `0xE4` | F3DEX `G_TEXRECT` | **LLE format (16 bytes)**, not HLE 24-byte | `texrectLLE_guarded` |
| `0xE5` | F3DEX `G_TEXRECTFLIP` | LLE format | `texrectFlipLLE_guarded` |
| `0xFF` | `G_SETCIMG` | Frequently emitted with bogus payloads (w1=0, fmt>4, out-of-FB addresses); rejected by `setColorImage_filtered` | `setColorImage_filtered` |
| `0x80` | unused | Chunk header (next-chunk pointer in 24-bit w0); never executed by the ucode, used by the HLE to record the chunk base | `op_80_header` |
| `0x02` | F3D `G_RDPHALF_2` | Per-vertex RGBA colors: DMA `(w0&0xFFFF)+1` bytes from w1 into DMEM 0xB70 | `op_02_colors` |

Chunks are contiguous 0x108-byte blocks. The interpreter walks chunk content linearly from +8; at +0x108, or on `0xB5`, it continues in the chunk named by the header's first word. `0x06` pushes and calls, `0x07` branches, and `0xB8` pops (or ends the task at depth 0). Models render as CI4 textures at palette bank 15. Per-face UVs are raw values scaled by the per-material `03 82` texcoord scale (16.16, DMEM 0x140; `ROGUESQ_F5_TC_SCALE=0` disables it for A/B). Use `f5_dl_walk.py --tex` to compare.

### Audio — MusyX synth and MORT voice

Rogue Squadron drives audio through Factor 5's **MusyX** engine, and **SFX and music work**: the CPU-side MusyX sequencer submits `M_AUDTASK`s, which run the **RSPRecomp'd MusyX synth ucode** (`musyx_audio_runner`, from `rsp/musyx_rsp.toml`) on the host audio-task thread; the synthesized PCM flows through `queue_samples` → SDL ([audio.cpp](src/audio/audio.cpp)). This is the default (`get_rsp_microcode` returns `musyx_audio_runner` for `M_AUDTASK`); `ROGUESQ_NO_AUDIO_UCODE=1` falls back to the silent `musyx_stub`. Stock `aspMain` is **not** on the audio path; MusyX has no shared format with the stock ucode. `ROGUESQ_DUMP_PCM` drives offline capture. See [docs/reference/game-architecture.md](docs/reference/game-architecture.md#audio-pipeline).

Subtitled **dialogue uses a separate codec, MORT** (per the [rerogue](https://github.com/dpethes/rerogue) PC-version RE), fully recompiled; `tools/re/mort_decode.py` / `tools/re/MORTDecoder.cpp` are the offline reference and `tools/re/extract_speech_table.py` extracts the voiceId→text table. The voice filters read `$ra` after `bltzal $zero` as the base of an embedded coefficient table, so N64Recomp must write `ctx->r31 = PC+8` for the conditional branch-and-link ops (`Generator::emit_link_address`); `jal`/`jalr` still do not write `r31`.

[patches/src/npc_health.c](patches/src/npc_health.c) replaces `getNpcCurrentHealth` and the other four health accessors and guards the slot read, which is NULL (`npc+0x190`) while a structure explodes. `getNpcCurrentHealth` is reached only through `LOOKUP_FUNC`; the patch covers it because `PatchesLib` is an OBJECT library (a STATIC one bound the func_map to the unguarded body).

### Scheduler queue plumbing

Disproven, don't chase: contention on `0x8011A7E8` (only `submitGfxFrame` 0x8000C07C uses it), lost DP messages (`mqdiag` shows 0 lost/requeued), DP throughput (the producer side is fine; 10x `dp_complete` changed nothing), and `0x80128EAE/F` as a sync token (they are the frame-pacing counters).

DP (`OS_EVENT_DP`) events arrive on a non-game thread → `enqueue_external_message_src` → drained on the next game-thread `osSendMesg`/`osRecvMesg`/`osJamMesg` via `dequeue_external_messages`. Queue 0x8011A408 (gate-thread DP queue, count=1) and 0x8011A7E8 (consumer) are the DP-pacing pair. Default `MessageQueueControl{}` has `requeue_dp = true`. The `mqdiag` instrumentation in [ultramodern/src/mesgqueue.cpp](lib/N64ModernRuntime/ultramodern/src/mesgqueue.cpp) tracks per-queue send/recv/external/delivered/blocked/lost/requeued counts; dump via `mqdiag_dump(path)`.

### Frame pacing: 30 fps is the game's own cap

The game is double-buffered (`0x80128EAD` = 2) with a minimum of 2 VIs per frame (`0x80128EAF` = 2, written once by `initVideoSubsystem` 0x8001A19C), so it presents at exactly 30 fps on any host. `viRetraceHandlerThread` bumps `0x80128EAE` every VI, zeroes it on a swap, and holds the next buffer until the counter reaches the minimum (0x80019AC8); buffer states are bytes at `0x80128EAA`. Every RDRAM golden (menu, cinematic, demos, missions) shows 2/2. On hardware heavy scenes dropped below 30; on PC the host never does, so the game thread idles on the swap and the F5 walk's `0xE9` wait is pacing, not render cost (render thread ~2-3 ms, GPU ~0.15 ms).

Game time is variable-timestep, not frame-counted: `timeSnapshotFiller` (0x8000BC00) measures the interval from `osGetTime` plus the VIs still owed before the swap, and `runInMissionFrame` rounds it to whole VI periods (`getViModePeriod` 0x80002710: 16.667 ms NTSC / 20 ms PAL; `floatModulo` 0x8001E20C), uses 1/30 s if it is <= 0, clamps to 0.1 s, and passes it as `$f12` to the per-frame ticks (`slotDispatcherIter`, `updateGridLayerScroll`, audio listeners). The lockstep/replay path uses fixed 1/30 s steps instead (`rs64_ls_frame_dt`).

### Exception handling pipeline

- **C++ exceptions** (SEH `0xE06D7363`): the `__except` filter in [librecomp/src/recomp.cpp](lib/N64ModernRuntime/librecomp/src/recomp.cpp) **must** let these propagate (`EXCEPTION_CONTINUE_SEARCH`). Catching them with `std::exit(1)` kills the game on any throw; the outer `try/catch` in [ultramodern/src/threads.cpp](lib/N64ModernRuntime/ultramodern/src/threads.cpp) recovers.
- **Hardware SEH** (AVs, illegal instructions): caught, the thread terminates, the process continues.
- **STL bounds checks** ("vector subscript out of range"): the `_CrtSetReportHook` in [crash.cpp](src/main/crash.cpp) returns 1 to suppress the abort. Trade-off: occasional visual glitch over a hard crash.
- **`get_function` on an unmapped address** (NULL or stray fn-ptr call): stubbed to a no-op in [librecomp/src/overlays.cpp](lib/N64ModernRuntime/librecomp/src/overlays.cpp) so the recompiled MIPS continues; logs `[null-call] addr=...` with the calling MIPS function (MSVC builds) or `Failed to find function at 0x...` (other compilers).

### RT64 interpreter safety

There is no global iteration cap in [rt64_interpreter.cpp](lib/rt64/src/hle/rt64_interpreter.cpp). F5 walks are bounded by the chunk-fetch rules in `rt64_gbi_f3dfactor5.cpp`: a per-task limit of 4096 chunk hops, exact chunk-revisit detection, and a back-link check. A runaway walk means one of those rules failed. `hleGBI` NULL checks exist around the reset path; the main HLE loop only `assert`s it.

