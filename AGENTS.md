# AGENTS.md

Guidance for AI agents working on the Rogue Squadron 64 Recompiled project — a native port of *Star Wars: Rogue Squadron* (N64, USA v1.0) built with N64Recomp + RT64. The game is playable from the first level through the credits. The current frontier is the remaining visual and pacing issues (credits text clipping, hitching, frame-interpolation quality), per-frame DL desyncs, and symbol renaming. See [Status](README.md#status-playable) in the README for the authoritative what-works snapshot before starting anything — this file assumes it.

## Project layout

```
src/main/main.cpp                       Game registration + RSP/audio/input/gfx callbacks
src/main/register_overlays.cpp          Overlay registration (all 3 .ovl.* at boot; switched at runtime by the loadOverlay hook)
src/main/rt64_render_context.cpp        RT64 host context; HLE send_dl, present-side fixes, boot-target driver
src/main/upstream_compat.cpp            libultra shims, scheduler overrides, rs64_load_overlay
src/main/hook_helpers.cpp               Host entry points for rogue_squadron.toml hooks (pacing, matpool, draw distance / terrain grid, DL walkers)
src/main/game_state.cpp, nav_sequencer.cpp  Game-state classifier and the BOOT_TARGET menu driver
src/main/host_api*.cpp, builtin_hooks.cpp   Mod host API (public header include/rs64/host_api.h)
src/main/ghost*.cpp, net_*.cpp, coop_imposter.cpp  Online co-op (ghost model, ENet6 transport, player-2 imposter)
src/main/lockstep*.cpp                  Input record/replay and the shared flight model
src/main/mp_register.cpp, mp_host.h     Multiplayer's host-API registrations and its only route to the game
mods/                                   Data-only, .nrm and native mods; mods/platforms.json picks what ships
patches/                                README only: base-game overrides are rogue_squadron.toml hooks
mods/multiplayer-native/                Online co-op as a native mod (built by the game's CMake when RS64_MULTIPLAYER=OFF)
tests/                                  Host unit tests (rs64_unit_tests)
rogue_squadron.toml, patches.toml       N64Recomp configs (game ELF / patches ELF)
rsp/                                    RSPRecomp configs (MusyX synth, shared boot ucode)
tools/coop/coop_relocation.toml         Generated co-op relocation patches (included via patch_files)
tools/state/state_model.toml            Game-state descriptor (state_table.inl codegen + Python tools)
lib/N64ModernRuntime/                   Submodule — fork at MikeSemicolonD/N64ModernRuntime
  ├── librecomp/                        Recompiler runtime (overlay loading, get_function, SEH)
  └── ultramodern/                      libultra emulation (threads, mesgqueue, events)
lib/rt64/                               Submodule — fork at MikeSemicolonD/rt64
  └── src/gbi/rt64_gbi_f3dfactor5.cpp   Native Factor 5 GBI profile (the render core)
lib/enet6/, lib/miniupnp/               Submodules for the multiplayer transport (always built)
syms/                                   N64Recomp --dump-context output (function + data symbols)
android/, ci/                           Android Gradle project; release CI runner scripts
docs/                                   Project notes (GBI, DL spec, data structures, env vars)
tools/                                  PowerShell + Python diagnostic and validation helpers
build/                                  CMake out-of-source build dir
```

The recompiled MIPS code lives in-repo at `RecompiledFuncs/` — `funcs_*.c` plus `funcs.h`, `lookup.cpp`, and `recomp_overlays.inl`. It is **gitignored** (a derivative of the copyrighted ROM; generated locally, never committed) and regenerated from `rogue_squadron.toml` with `cmake --build build --config Debug --target regen_funcs`; the next build re-globs automatically (`CONFIGURE_DEPENDS`). Regen reads the decomp ELF at `../rogue_squadron64/build/roguesquadron.elf`, rebuilds `N64RecompCLI` first, and runs `tools/coop/gen_relocation.py --verify` afterwards (fails if the relocation patches were not applied). These are auto-generated; hand-instrumenting them with diagnostic `fprintf` probes is routine, but load-bearing logic belongs in `rogue_squadron.toml` hooks (see below), not here — regeneration silently discards inline edits.

The forked submodules under `lib/` carry intentional `if(false) fprintf(...)` debug-toggle cruft and game-specific defensive guards. **Do not propose stripping these** as cleanup; they are intentional. (The inline KSEG0 pointer guards in `rogue_squadron.toml` have already been retired.)

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

Ignore the `lld-link : warning : found both wmain and main; using latter` — benign (SDL2main provides wmain, our main wins).

ROM lives at `build/Debug/rogue_squadron.z64` (USA v1.0, xxHash3-64 = `0x6B66A44153594DEA`).

For a headless run that drives past the title without a controller, pass `--fake-controller --auto-start <ms>` (or the env vars `ROGUESQ_FAKE_CONTROLLER=1` / `ROGUESQ_AUTO_START=<ms>`).

## Environment variables

All trace categories are off by default. The full catalog is in
[docs/debug-trace-env-vars.md](docs/debug-trace-env-vars.md) — **read it before
adding a new `fprintf` or asking the user to enable logs.** The common
user-facing switches below also have `--flags` (run the exe with `--help`); the
env-var name is kept here since that is what the code reads and what you grep
for. Any variable can be set on the command line with `--set NAME=VALUE`.

| Env var | Effect |
|---|---|
| `ROGUESQ_GFX_API=vulkan\|d3d12` | Force the graphics API (default auto) |
| `ROGUESQ_HLE_DEV_MODE=0\|1` | RT64 ImGui inspector on F1 (default on in Debug, off in Release) |
| `ROGUESQ_VI_DRIVEN_LOOP=0` | Old host-paced frame loop instead of the hardware VI protocol (default on). The VI-driven loop matches hardware message order and is the current stability baseline |
| `ROGUESQ_F5_NATIVE=0` | Parse F5 display lists without emitting geometry |
| `ROGUESQ_F5_CHUNK_BOUND=0` | Disable the F5 DL chunk-bounded fetch rule (default on) |
| `ROGUESQ_F5_TERRAIN_SUB` / `ROGUESQ_F5_TERRAIN_SUB_FAR` | Terrain subdivision for near (`shift==0`) and far (`shift>=1`) tiles (default 2 / same as near). A lower far value leaves T-junction cracks at the LOD boundary; terrain emission is cheap (~0.3 ms/walk) |
| `ROGUESQ_F5_CULL_DIST=<units>` | **Default off.** RT64-side per-object distance cull (camera-space): drops a model's draws when its `0x01` modelview origin exceeds the threshold. Trades far-object pop-in for fewer draws. Benefit scales with aggressiveness (~3000 = ~30% fewer hitches but visible pop-in; ~10000 = visually clean but marginal). Terrain/effects unaffected. `ROGUESQ_F5_CULL_LOG=1` logs per-object distances |
| `ROGUESQ_FB_GUARDS=<mask>` | Host framebuffer guards, bitmask (default 6: 4 = RT64 fb-registry sanitizer, 1 = CIMG neutralizer, opt-in). `0` disables all for A/B against goldens |
| `ROGUESQ_NO_AUDIO_UCODE=1` | Silent audio stub instead of the MusyX synth |
| `ROGUESQ_DUMP_PCM=<path>\|1` | Write the synth output to a 22050 Hz stereo WAV (`1` = `dumps/wav/capture.wav`) |
| `ROGUESQ_FAKE_CONTROLLER=1` / `ROGUESQ_AUTO_START=<ms>` | Headless runs: fake a controller, pulse START |
| `ROGUESQ_LOG_GBI=1` / `ROGUESQ_LOG_GFX_TASK=1` | Per-handler GBI logs (high volume) / one line per graphics task |
| `ROGUESQ_LOG_MESG_TRACE=1` (+ `ROGUESQ_MESG_TRACE_FRAMES=lo-hi`) | Thread/message-order trace for `tools/validate/compare_mesg_trace.py` |
| `ROGUESQ_DUMP_FRAME_DL=N` / `ROGUESQ_DUMP_TEXTURES=1` | Debug builds only: log gfx tasks N..N+15 to stderr (full detail for N, N+1); textures of those tasks go to `dumps/tex/` |

There is no catch-all log switch; enable each category by name.

With the inspector on, F1 toggles RT64's ImGui overlay, F3 toggles ViewRDRAM mode, F4 toggles texture replacement, and F12 writes a crash dump.

## Diagnostic artifacts

Files written to `logs/` and `dumps/crash-dumps/` during a run:

- `logs/stability/<tag>/run_N.log` — per-run stderr captured by `tools/run-stability.ps1`. Companion `summary.csv` classifies outcomes.
- `logs/stability/<tag>/memory.csv` — per-second WS / private / VM samples from `tools/measure-leak.ps1`.
- `mqdiag_NNN.txt` — no longer written (the periodic watchdog was removed). For a hang, take a full dump with `tools/dump-game.ps1` and run `tools/reconstruct-freeze.py` (game threads and queues from RDRAM) plus `tools/host-stacks.py` (symbolized host stacks; Debug build only).
- `dumps/crash-dumps/crash_YYYYMMDD_HHMMSS.dmp` — full-memory minidump from the SEH handler, the SIGABRT handler, the F12 hotkey, or external `tools/dump-game.ps1`.

## Tooling (under `tools/`)

### Crash / hang triage

1. **Crash**: a `crash_*.dmp` is written automatically by the handlers in `src/main/main.cpp`. Open in VS (File → Open → File → .dmp → Debug with Native Only).
2. **Hang**: from another shell, `pwsh tools/dump-game.ps1` captures the running process even when the window is "Not Responding". Don't use F12 if the message pump is starved — it won't fire.
3. **Triage threads**: `python tools/inspect-dump.py` lists threads in the most recent dump and tags ones running our exe code as `in_exe`. Those 2–4 are the only ones worth opening in VS; the rest are runtime workers parked in ntdll.

Note: the Windows debugger CLI (`cdb.exe`) is currently broken on this machine (`STATUS_DLL_INIT_FAILED`). Don't try to drive it from PowerShell — use VS interactively or the `minidump` Python package via `inspect-dump.py`.

### Stability / leak harness

- `tools/run-stability.ps1 -Runs N -Timeout S -Tag <label>` — N timed launches, per-run stderr, outcome classification by marker grep. `-EnvVars "A=1;B=1"` forwards debug vars. `-Runs 1` for data capture, `-Runs 3` for stability-rate.
- `tools/measure-leak.ps1 -Timeout S -Tag <label>` — single run, per-second WS/Private/VM to `memory.csv`.
- `tools/recordings/` — per-mission input recordings (folder prefix = level id) plus `run-replays.ps1`, which replays every mission recording against its `.hash` baseline. See [tools/recordings/README.md](tools/recordings/README.md).

### Validation harness (`tools/validate/`)

The primary correctness workflow — diff a live run against a Project64 golden rather than eyeballing screenshots:

- `capture_pj64_golden.ps1` / `capture_menu_rdram.ps1` — scripted PJ64 goldens.
- `f5_dl_walk.py` — walks a Factor 5 display list offline; `--json` for machine diff, `--tex` for texture/UV inspection. `f5_dl_ndc.py` adds NDC projection.
- `dl_diff.py` — layer-by-layer DL diff vs golden.
- `rdram_golden_diff.py` — RDRAM diff vs PJ64 (`--focus ADDR:SIZE` for field-level compare).
- `state_diff.ps1` — **state-matched** decomp-vs-PJ64 memory diff: captures the recomp's RDRAM at several game states in one run (`ROGUESQ_DUMP_RDRAM_ON_STATE=a,b,c`, keyed to the game-state classifier) and diffs each vs a matching PJ64 golden (`dumps/pj64/rdram_state_<id>.bin`), focused via `focus_of.py` (state `focus` structs → `rdram_golden_diff --focus`). The two emulations compare by *logical state*, not wall-clock. PJ64 side needs state-tagged goldens (extend `pj64_rs64_dump.js`).
- `audio_cmd_walk.py` / `audio_diff.py` — MusyX voice-command walk and diff (perception-free audio validation).
- `compare_mesg_trace.py` / `symbolize_mesg_trace.py` — message-order trace comparison (pair with `ROGUESQ_LOG_MESG_TRACE`).
- `checkpoint.ps1` — orchestrates a capture + diff checkpoint.

### Renaming (`tools/rename/`) and cross-refs (`tools/rz/`)

- `tools/rename/` — the symbol-renaming pipeline (`build_redefs.py`, `find_callers.py`, ...). After a rename batch, regen and build: a name that drifted between the ELF and `rogue_squadron.toml` hooks fails there.
- `tools/rz/rzq.py <cmd> <sym|0xADDR>` — rizin queries against the symbolized `rogue_squadron64` ELF: `xrefs`, `callees`, `disasm`, `strrefs`, `funcs`, `raw`. `--overlay mission|menu|cinematic` selects which overlay sits at 0x800A5130; `--project <file>` caches the ~25s analysis. See [tools/rz/README.md](tools/rz/README.md).

## Overriding recompiled functions

**Do not hand-edit `RecompiledFuncs/funcs_*.c` for defensive guards or game-logic overrides.** Those edits are regeneration-hostile and made the codebase brittle for months. Base-game fixes are `[[patches.hook]]` (C inserted at the function's entry or `before_vram`) and `[[patches.instruction]]` entries in `rogue_squadron.toml`, with any non-trivial host logic in [src/main/hook_helpers.cpp](src/main/hook_helpers.cpp) as an `extern "C"` function the hook declares and calls. Re-run `regen_funcs` after editing the toml.

- A hook is compiled into the recompiled body, so it runs for direct and `LOOKUP_FUNC` (indirect, `func_map`) calls alike. No link-order tricks.
- Hook text sees `rdram`, `ctx` and the `MEM_*` macros. A bare `return` exits the whole game function; set `ctx->r2` (or `ctx->f0`) first to return a value. Example: the NPC health accessor guards (`getNpcCurrentHealth` and its four siblings) take over only when the per-difficulty slot address is bad and otherwise fall through to the original body.
- Call recompiled game functions from a hook only through `mips_call`.

N64Recomp is `build/Debug/N64Recomp.exe` (override `N64RECOMP_EXE`), built from `lib/N64ModernRuntime/N64Recomp` (a directory in the N64ModernRuntime fork) via `N64RecompCLI`; `regen_funcs` depends on it. It supports `--dump-context`, `func_reference_syms_file` and `patch_files`. If a regen truncates `funcs.h` to ~7 lines, the exe is stale; see dead ends.

Base-game fixes go in the toml. Self-contained gameplay mods are `.nrm` code mods built with RecompModTool and loaded by librecomp; see `mods/infinite-secondary/` and the "Code mods" paragraph in [docs/adding-menus-and-buttons.md](docs/adding-menus-and-buttons.md). Native-library mods that hook game code through the host API: [docs/modding-host-api.md](docs/modding-host-api.md).

`mods/platforms.json` decides which mods ship per platform: a mod not listed there ships nowhere. `desktop` entries (data-only folders and `.nrm` files) are staged next to the exe by `tools/mods/stage_mods.cmake`; `android` entries are packed into the APK. Release CI builds every `.nrm` listed under `desktop` via `tools/mods/build_code_mods.cmake`, matching each entry to the `mods/*/mod.toml` whose `mod_filename` equals the `.nrm` name, so a new code mod must be added to that list to ship ([docs/release-ci.md](docs/release-ci.md#code-mods)).

## Architectural quirks worth knowing

### Overlays: registered at boot, switched by a hook

librecomp's section table covers all three `.ovl.*` overlays (mission / menu / cinematic), which share `ram_addr 0x800A5130`. They are all registered at boot in [src/main/register_overlays.cpp](src/main/register_overlays.cpp) via `recomp::overlays::register_overlays` (the Zelda64Recomp pattern). Which one the `func_map` resolves to is switched at runtime by a `[[patches.hook]]` on `loadOverlay` (0x80000B3C, `s0` = id 0 mission / 1 menu / 2 cinematic) calling `rs64_load_overlay` in [upstream_compat.cpp](src/main/upstream_compat.cpp), which unloads and loads the overlay through librecomp's API. Keep overlay logic there; don't modify librecomp's DMA path.

### Terrain grid is 128x128 in host RAM

The flight terrain's view grid tables (span tables, two byte tables, the per-cell pointer table) are relocated to 0x80B00000-0x80B1FFFF, host RDRAM above the game's 8 MB (0x80A00000-0x80A01FFF is the F5 renderer's vertex scratch; keep host-side tables clear of it), and their row strides are doubled by `[[patches.instruction]]` entries in `rogue_squadron.toml`. Any new code touching these tables must go through `rs64_tgrid_base`. `ROGUESQ_DRAW_DIST` (0.25-8) scales the far plane and object culls; terrain reach and fog follow it capped at 2.5x (`ROGUESQ_TERRAIN_DIST` overrides), and the level cell budget doubles at 2x+ (`ROGUESQ_TGRID_BUDGET_MULT`).

### Per-player data relocation (co-op)

Four ranges after the per-player arrays moved into host RAM so index 1 fits: R1 0x80138058-0x80138267 (`gObjectiveCounts` 0x80138060 is now at 0x80B38060), R2 0x80138268-0x80138837 (camera node[0]), R3 0x80138E5C-0x8013901F (`txtFileHeader` .. `voiceTxtString`) at +0x00A00000, and R4 0x80138930-0x80138D0F (a per-view 4 x 0xF8 block) at +0x00A20000. Only `lui` immediates change (`0x8013`/`0x8014` -> `0x80B3`..`0x80B6`), one `[[patches.instruction]]` each (200) in the generated `tools/coop/coop_relocation.toml`, included through `[input] patch_files`. Regenerate it with `python tools/coop/gen_relocation.py` after anything that changes the recompiled code of those functions; ambiguous sites are settled in `tools/coop/relocation_resolve.txt`. Host code must go through `rs64::ls::relocated()` for these addresses. `ROGUESQ_RELOC_GUARD=1` reports writes to the old addresses. Tools that diff the 8 MB RDRAM against PJ64 goldens will see these fields as zero at their old addresses. `.nrm` mods still address the per-player data at its old addresses, through `syms/rogue_squadron.datasyms.toml`. `coop_relocation.toml` is generated but load-bearing, so it must stay in the repository.

### Co-op player 2 is an imposter NPC

`ROGUESQ_COOP_LOCAL=1` spawns player 2 as a level-wingman NPC (`npcWingmanUpdate` 0x800D93C0) from a fake DAT record at 0x80B40000 with no flight path, so the wingman AI idles and [src/main/coop_imposter.cpp](src/main/coop_imposter.cpp) flies it from port 1 (hook at the handler's post-state join 0x800D9A00, action 3 only). The spawn must be followed by one `slotDispatcherIter(slot, 3, &dt)` tick or the NPC never enters its tick chains. There is no gPlayers[1], view[1] or second camera, and local split-screen is out of scope. Hooks call recompiled functions only through `mips_call` (saves the context, runs the callee below the hook's stack frame). The flight model in `lockstep_core.cpp` is a port of the player X-wing's handling (`updateXwingFlightControls` 0x800B4588) and uses polynomial sin/cos with FP contraction off so every platform rounds the same. The NPC health guard hooks (`rs64_npc_health_slot_ok`) accept the record's health slot in host RAM. The wingman dereferences its mesh during the spawn, so the record's model index (+0x8C) is the first wingman model the level loaded (`wingman_model`, a side-effect-free copy of `walkMeshdef0List`'s name lookup); with none, the imposter is skipped. A `RS64_MULTIPLAYER=OFF` build needs the `multiplayer-native` mod enabled for this.

Online co-op (`ROGUESQ_MP=host|join`, or the MULTIPLAYER lobby: `mods/multiplayer` menu page in ON/Android builds, `mods/multiplayer-native` in desktop OFF builds) is the ghost model: each instance flies its own player 1 and poses the other player's imposter from the state it streams over ENet6, and the two worlds are independent. Code: `ghost.cpp` (session), `ghost_lobby.cpp`, `ghost_hud.cpp`, `net_core.cpp`, `net_link.cpp`; details, protocol and lobby flow are in [docs/multiplayer.md](docs/multiplayer.md). Desktop releases build with `-DRS64_MULTIPLAYER=OFF` and ship online co-op as the `multiplayer-native` mod, which the game's CMake builds. The multiplayer sources reach the game only through `mp_host.h` and base code sees them only through hooks, actions and flags, so a new base-side need is a flag or table entry, never a direct call. Gotchas:

- Hooks call game functions only through `mips_call`.
- There are `0x800` NPC slots and spawns are pool-checked (`npc_pool_room`); an unchecked spawn on a full pool corrupts the slot table.
- Incoming damage waits for the mission to settle (30 frames, no cutscene), or the objective trigger faults.
- The menu font has no period; `ensure_menu_period` builds it at runtime, so use the existing dot and blank glyph helpers, not hardcoded glyphs.
- Android needs the `INTERNET` permission and a Wi-Fi multicast lock (`MainActivity`) for LAN discovery.
- Unit tests: `cmake --build build --config Release --target rs64_unit_tests` builds and runs `net_core_test`, `net_link_test`, `lockstep_core_test`, `nav_target_test`, `host_api_test` (plus the style checker). `touch_input_test`, `video_config_test`, `transition_gate_test`, `input_bindings_test` and `fault_guard_test` are separate `EXCLUDE_FROM_ALL` targets.

### Game-state model + BOOT_TARGET nav engine

A canonical game-state model classifies the current state each present from RDRAM: descriptor `tools/state/state_model.toml` → `tools/state/gen_state_table.py` (CMake `gen_state_table`) → `src/main/state_table.inl`, host classifier in [src/main/game_state.cpp](src/main/game_state.cpp) (`rs64_state_current_id`), Python tools read the same TOML. `ROGUESQ_LOG_GAMESTATE=1` prints the classified state. Discriminators are verified against live RDRAM (e.g. mission = `numMissionObjectives` 0x130B17 != 0; menu = `gCurrentMenuData` 0x800CE730; menu id at 0x800CE734; pilot sub-step 0x800CE626). A **headless scripted virtual controller** (`ROGUESQ_INPUT_SEQ`, injected at `get_n64_input` **before** `resolve()` so keyboard-active runs don't swallow it) drives menus without window focus.

`ROGUESQ_BOOT_TARGET=level:<id>[,craft]` is a state-gated **nav sequencer** ([src/main/nav_sequencer.cpp](src/main/nav_sequencer.cpp)) that drives the real menus to a mission (replacing the old field-poke that jumped the state machine and bailed to attract). Never call a recompiled function from the host to force state — inject input + write fields the game's own confirm path reads (e.g. `gCurrentLevel` 0x130B70 is a **u32**, not a byte — a byte write = out-of-range id = crash). `demo:<n>` auto-disables the custom menu (it displaced the attract idle path) so the chosen attract demo plays; firing it *instantly* is unsolved (idle trigger is `(clock − lastInputFrame) > threshold`, `lastInputFrame` not locatable statically). See project memory `project_boot_target_nav_engine_2026_09_22` and `project_game_state_model_2026_09_22`.

### Factor 5 GBI — custom opcodes

This game uses a Factor 5-customized F3DEX-derived ucode. The RT64 profile `GBI_F3DFACTOR5` lives in [lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp](lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp) and inherits from `GBI_F3DEX`. The canonical opcode map (with observed w0/w1 patterns and disproven interpretations) is in [docs/factor5-gbi.md](docs/factor5-gbi.md); the DL grammar itself is in [docs/f5-model-dl-spec.md](docs/f5-model-dl-spec.md). Confirmed Factor 5-specific behaviors:

| Opcode | Standard meaning | Factor 5 behavior | Handler |
|-------:|------------------|-------------------|---------|
| `0xB5` | F3DEX `G_QUAD` | **Next chunk**: continue in the chunk named by the current chunk header's first word (w1 ignored); `0xB8` is the pop/return | `op_b5_next_chunk` |
| `0xE4` | F3DEX `G_TEXRECT` | **LLE format (16 bytes)**, not HLE 24-byte | `texrectLLE_guarded` |
| `0xE5` | F3DEX `G_TEXRECTFLIP` | LLE format | `texrectFlipLLE_guarded` |
| `0xFF` | `G_SETCIMG` | Frequently emitted with bogus payloads (w1=0, fmt>4, out-of-FB addresses); rejected by `setColorImage_filtered` | `setColorImage_filtered` |
| `0x80` | unused | Chunk header (next-chunk pointer in 24-bit w0); never executed by the ucode, used by the HLE to record the chunk base | `op_80_header` |
| `0x02` | F3D `G_RDPHALF_2` | Per-vertex RGBA colors: DMA `(w0&0xFFFF)+1` bytes from w1 into DMEM 0xB70 | `op_02_colors` |

Chunks are contiguous 0x108-byte blocks. The interpreter walks chunk content linearly from +8; at +0x108, or on `0xB5`, it continues in the chunk named by the header's first word. `0x06` pushes and calls, `0x07` branches, and `0xB8` pops (or ends the task at depth 0). Models render as CI4 textures at palette bank 15. Per-face UVs are raw values scaled by the per-material `03 82` texcoord scale (16.16, DMEM 0x140); applying it fixed the old "zoomed/stretched model texture" bug (`ROGUESQ_F5_TC_SCALE=0` reverts for A/B). Judge any remaining UV defect after that scale. Use `f5_dl_walk.py --tex` to compare.

### Audio — MusyX synth and MORT voice

Rogue Squadron drives audio through Factor 5's **MusyX** engine, and **SFX and music work**: the CPU-side MusyX sequencer submits `M_AUDTASK`s, which run the **RSPRecomp'd MusyX synth ucode** (`musyx_audio_runner`, from `rsp/musyx_rsp.toml`) on the host audio-task thread; the synthesized PCM flows through `queue_samples` → SDL ([main.cpp](src/main/main.cpp)). This is the default (`get_rsp_microcode` returns `musyx_audio_runner` for `M_AUDTASK`); `ROGUESQ_NO_AUDIO_UCODE=1` falls back to the silent `musyx_stub`. Stock `aspMain` is **not** on the audio path; MusyX has no shared format with the stock ucode. `ROGUESQ_DUMP_PCM` drives offline capture. See [docs/game-architecture.md](docs/game-architecture.md#audio-pipeline).

Subtitled **dialogue uses a separate codec, MORT** (per the [rerogue](https://github.com/dpethes/rerogue) PC-version RE). Contrary to earlier notes, MORT is **fully recompiled and works** — `tools/mort_decode.py` / `tools/MORTDecoder.cpp` are the offline reference. Voice-decode freezes were an **N64Recomp codegen bug** in the conditional branch-and-link (`bgezal`/`bltzal`) `$ra`-as-data-pointer idiom: the MusyX/voice filters do `bltzal $zero, T` then read `$ra` (= PC+8) as the base of an embedded coefficient table (`addiu $t7, $ra, 0xD4; lb …`). The recompiler emitted the branch but **never materialized the link *value***, so `$ra` stayed 0 (indirect-call entry), the table pointer computed to `~0x800000DB`, and the read AV'd — SEH-swallowed, killing the thread and deadlocking the frame pipeline (`filterVoiceSampleBlock`, on `viRetraceHandlerThread`, was the SELECT-LEVEL→load voiceline freeze; `applyVoiceDelayFilter` was the earlier demo/FrontEnd one). **Fixed (2026-09-20)**: `recompilation.cpp` now emits `ctx->r31 = PC+8` for the conditional branch-and-link ops before the branch condition (`Generator::emit_link_address`, implemented in `CGenerator`); `jal`/`jalr` still do not write `r31`. Diagnose these from a full-memory `dump-game.ps1` dump with `tools/reconstruct-freeze.py` (frozen-machine state from RDRAM) + `tools/host-stacks.py` (symbolized host stacks; refuses on a stale-exe/PDB mismatch). See project memory `craftselect-voiceline-freeze-2026-09-16` (root cause + the fix) and `demo-voiceline-freeze-2026-09-13`. The old `ROGUESQ_VOICE_UNSTICK` host watchdog has been removed. `tools/extract_speech_table.py` extracts the voiceId→text table.

The **structure-destruction attract-demo freeze** (jade moon and any demo that blows up a structure) is **FIXED**: during an explosion an NPC has `npc+0x190 == NULL`, so `getNpcCurrentHealth` derefs a wild address and AVs; the SEH-swallowed AV leaves the gfx-frame barrier inconsistent → deadlock. Entry hooks on `getNpcCurrentHealth` and the other four health accessors (`rogue_squadron.toml`, helper in `hook_helpers.cpp`) guard the slot read/write. getNpcCurrentHealth is reached only through the `func_map`/`LOOKUP_FUNC` indirect path, which a hook covers because it lives in the recompiled body. When a recompiled function hangs on data that decodes fine offline, suspect a codegen mistranslation of a rare instruction (especially the `*al` link-branches) before deep subsystem RE.

### Cooperative-scheduler queue plumbing

DP (`OS_EVENT_DP`) events arrive on a non-game thread → `enqueue_external_message_src` → drained on the next game-thread `osSendMesg`/`osRecvMesg`/`osJamMesg` via `dequeue_external_messages`. Queue 0x8011A408 (gate-thread DP queue, count=1) and 0x8011A7E8 (consumer) are the DP-pacing pair. Default `MessageQueueControl{}` has `requeue_dp = true`. The `mqdiag` instrumentation in [ultramodern/src/mesgqueue.cpp](lib/N64ModernRuntime/ultramodern/src/mesgqueue.cpp) tracks per-queue send/recv/external/delivered/blocked/lost/requeued counts; dump via `mqdiag_dump(path)`.

### Frame pacing: 30 fps is the game's own cap

The game is double-buffered (`0x80128EAD` = 2) with a minimum of 2 VIs per frame (`0x80128EAF` = 2, written once by `initVideoSubsystem` 0x8001A19C), so it presents at exactly 30 fps on any host. `viRetraceHandlerThread` bumps `0x80128EAE` every VI, zeroes it on a swap, and holds the next buffer until the counter reaches the minimum (0x80019AC8); buffer states are bytes at `0x80128EAA`. Every RDRAM golden (menu, cinematic, demos, missions) shows 2/2. On hardware heavy scenes dropped below 30; on PC the host never does, so the game thread idles on the swap and the F5 walk's `0xE9` wait is pacing, not render cost (render thread ~2-3 ms, GPU ~0.15 ms).

Game time is variable-timestep, not frame-counted: `timeSnapshotFiller` (0x8000BC00) measures the interval from `osGetTime` plus the VIs still owed before the swap, and `runInMissionFrame` rounds it to whole VI periods (`getViModePeriod` 0x80002710: 16.667 ms NTSC / 20 ms PAL; `floatModulo` 0x8001E20C), uses 1/30 s if it is <= 0, clamps to 0.1 s, and passes it as `$f12` to the per-frame ticks (`slotDispatcherIter`, `updateGridLayerScroll`, audio listeners). Native 60 fps is therefore a one-byte change (`0x80128EAF` = 1) plus an audit for logic that counts frames instead of using dt. Untested. Fixed 1/30 s steps already exist in the lockstep/replay path (`rs64_ls_frame_dt`, `mp_register.cpp`, `ghost.cpp`), so recordings and co-op would need their own 60 Hz mode. Frame interpolation is the alternative: it keeps 30 Hz logic and smooths the display.

### Exception handling pipeline

- **C++ exceptions** (SEH `0xE06D7363`): the `__except` filter in [librecomp/src/recomp.cpp](lib/N64ModernRuntime/librecomp/src/recomp.cpp) **must** let these propagate (`EXCEPTION_CONTINUE_SEARCH`). Catching them with `std::exit(1)` kills the game on any throw; the outer `try/catch` in [ultramodern/src/threads.cpp](lib/N64ModernRuntime/ultramodern/src/threads.cpp) recovers.
- **Hardware SEH** (AVs, illegal instructions): caught, the thread terminates, the process continues.
- **STL bounds checks** ("vector subscript out of range"): the `_CrtSetReportHook` in [main.cpp](src/main/main.cpp) returns 1 to suppress the abort. Trade-off: occasional visual glitch over a hard crash.
- **`get_function` on an unmapped address** (NULL or stray fn-ptr call): stubbed to a no-op in [librecomp/src/overlays.cpp](lib/N64ModernRuntime/librecomp/src/overlays.cpp) so the recompiled MIPS continues; logs `[null-call] addr=...` with the calling MIPS function (MSVC builds) or `Failed to find function at 0x...` (other compilers).

### RT64 interpreter safety

There is no global iteration cap in [rt64_interpreter.cpp](lib/rt64/src/hle/rt64_interpreter.cpp). F5 walks are bounded by the chunk-fetch rules in `rt64_gbi_f3dfactor5.cpp`: a per-task limit of 4096 chunk hops, exact chunk-revisit detection, and a back-link check. A runaway walk means one of those rules failed. `hleGBI` NULL checks exist around the reset path; the main HLE loop only `assert`s it.

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
4. **Use `tools/reconstruct-freeze.py` on a full dump** to validate queue-level theories before instrumenting: it lists every blocked game thread and its wait queue. `tools/host-stacks.py` gives the symbolized host stacks.

For a hang specifically: if it's a cutscene/demo, suspect a recompiler codegen mistranslation of a rare instruction on the hung path (the demo-freeze root cause — see the Audio quirk) before deep subsystem RE.

For a bug that shows up only in Release (or only when the host is fast), suspect a game assumption that some producer is slower than a frame before suspecting the compiler. Example: `tickFormatMessageWorker` sent every reply as a pointer to one stack buffer; at -O2 it answered several requests before the menu polled once, the queued replies aliased, and SELECT GAME lost its medals/preview (fixed by the reply-ring hook in `rogue_squadron.toml`). `ROGUESQ_WATCH_ADDRS` and `ROGUESQ_DATA_BP` find the diverging write; the CMake `RS64_OD_TARGETS` / `RS64_OD_SOURCES` settings bisect by optimization level.

## Avoid these dead ends (already disproven)

### Rendering / GBI

- **`op_80` as a sub-DL call** — treating its 24-bit w0 as a call target infinite-loops and hangs after ~200 DLs. It's a state/param load.
- **Y-flip / component swap on model textures** — retired. The real cause was the missing `03 82` texcoord scale (see the Factor 5 GBI section).
- **Moving more work from the CPU to the GPU to raise the frame rate** — the game caps itself at 30 fps (see Frame pacing), and RT64 already transforms vertices in GPU compute. Draw coalescing cut recording 4-8x with no change in frame rate.
- **A `cv.wait` rewrite of RT64's present-queue busy-wait** (`advanceToNextPresent` in `rt64_present_queue.cpp`) — regressed natural-exit rate. Reverted.

### libultra / scheduler

- **13-way contention on `0x8011A7E8`** — only one thread calls `submitGfxFrame` (0x8000C07C).
- **`0x80128EAE/F` as the cause of a frame-sync deadlock** — they are the VI-since-swap counter and the min-VIs-per-frame cap (see Frame pacing), not a sync token.
- **10× `dp_complete` to fix DP throughput** — producer side is fine.
- **Cooperative scheduler losing DP messages** — `mqdiag` shows 0 lost/requeued for the DP queue.
- **"iter 3 hangs" in `submitGfxFrame`** — counter misread; the loop runs 30+ iters normally.
- **"iter ~810 cinematic freeze"** — was a symptom of the old LLE pipeline. The current VI-driven HLE path runs steady; not observable. Don't chase it.

### Boot flow / state machine

- **Force-menu bypass via `ROGUESQ_FORCE_MENU_AT_SEC`** — skipping cinematic init crashes downstream. Don't jump the state machine.
- **Re-investigating a "missing state-1 writer" in the 5-slot table at `D_80154620`** — that's the speech/streamed-voice playback slots, not cinematic stages. This is where the streamed-voice active byte lives; the demo-freeze it seemed to gate was actually the `bgezal`/`bltzal` codegen bug (now fixed), not a missing scheduler writer.

### Build / regeneration

- **Hand-editing `funcs_*.c` for game-logic overrides** — next regen silently strips it. Use a `[[patches.hook]]`. Diagnostic `fprintf` probes are fine.
- **Regenerating `funcs_*.c` with a stale N64Recomp binary** — a binary built from old source errors on the `cache` instruction (its entrypoint recompile reads past the 0xC bound into `func_8000040C`) and **truncates `funcs.h` to ~7 lines**. `regen_funcs` rebuilds `N64RecompCLI` first, so this only happens with an `N64RECOMP_EXE` override pointing at an old exe; rebuild it and rerun `regen_funcs`. A good regen produces a ~2561-line `funcs.h`.
- **Never write repo files via Python `open(...,'w')`** — a bad Python write once truncated the entire GBI core and it had to be rebuilt from goldens. Use the editor tools.

### Miscellaneous

- **The `wmain/main` link warning** — benign.
- **Shadowing `osPiStartDma_recomp` in `upstream_compat.cpp` with only the ROM-read branch** — boot needs the SRAM-read path too. Replicate `do_dma` in full or it regresses.

## Style conventions

`tools/style/check_style.py` enforces these rules for `src/main`; the baseline only shrinks (`--update-baseline` after fixing findings).

- **No emojis** in code, comments, or docs unless explicitly requested.
- **No trailing summary blocks** in chat responses — one-line wrap-up max.
- Default to **no comments**. Add one only when the WHY is non-obvious (a workaround for a specific bug, a hidden invariant, a non-visible constraint).
- **Comments are terse and matter-of-fact.** State the fact, not the reasoning journey. No multi-paragraph narration, no dated blow-by-blow history, no "we tried X then Y" storytelling in a comment — one or two plain lines. This applies to config comments (e.g. `rogue_squadron.toml`) too.
- **At most 1-2 lines per comment block, and don't hard-wrap at ~90 columns** — let a line run long rather than splitting one sentence across several. No trailing side comments after code (`x = 1;  // note`) and no extra notes tacked on after a semicolon; if it matters, it goes in the one comment above the code.
- Don't reference the current task or session in comments — they rot.
- Prefer **editing existing files** over creating new ones. The runtime is already large; new files attract drift.
- For probe instrumentation in `funcs_*.c`, rate-limit:
  ```c
  { static int n=0; ++n; if (n<=10 || (n%50)==0) { fprintf(stderr, "..."); fflush(stderr); } }
  ```
- **Diagnostic accumulators must be bounded.** Any `static` set/map/vector that a debug probe grows keyed by an ever-changing value (address, hash, DL/frame id) leaks to runaway memory over a long logging session if it is never evicted — a distinct-hash set is the classic offender. Cap them keep-recent instead: in `lib/rt64` use `rt64diag::BoundedSet` / `BoundedMap` ([lib/rt64/src/common/rt64_diag_bounds.h](lib/rt64/src/common/rt64_diag_bounds.h), FIFO eviction, default 5000, override `ROGUESQ_DIAG_CAP`); once saturated a set reports a recent-window count (`<= cap`), not an all-time total. Address/config-keyed trackers are naturally small and need no cap; hash/id-keyed ones do. (Note: normal runs are memory-flat in both Debug and Release — the fixed ~4.9GB Private / ~11GB Virtual at boot is RT64's GPU commit, not a leak; runaway shows up only under diag flags.)
- When renaming symbols, avoid address-encoded names (`clearByteAt801128CC`) — they add nothing over `func_HHHHHHHH`. If you can't see the semantics, leave it as `func_*`.
- **One statement per line for control flow.** An `if` (or other statement) that begins a new logical statement gets its own line — don't trail it after another statement on the same physical line separated by `;`. Split the guard onto the next line:
  ```c
  static int s_lo = -1;
  if (s_lo < 0) s_lo = env_on("ROGUESQ_LOG_AUDIO_OUT");
  ```
  Exempt: single-line loop bodies, aligned lookup/return ladders and tabular min/max updates, and the env-gated diagnostic probe blocks — keep those terse.
- **Env gates on hot paths must read the environment once.** Use a `static const` initializer, or a distinct uninitialized sentinel checked with `== -1`. Never cache "off" as a negative value that a `< 0` check re-reads: 12 such gates in the F5 GBI ran `getenv()` on every command and were half the walk time.
- **Read env vars through the shared `recomp::dbg::env_*` helpers** (`src/main/debug_logs.h`: `env_on`/`env_int`/`env_str`/`env_u32`), not open-coded `getenv` parsing.
- **Remove a debug env var once the fix or feature it served is done** — the gate, the code behind it, its row in [docs/debug-trace-env-vars.md](docs/debug-trace-env-vars.md), and any comment naming it. Keep one only if it can test, validate or probe for other bugs (a trace category, a dump, an address watch, an A/B toggle for a still-heuristic path). A probe for one frame, one address or one already-verified fix does not qualify.

## Open work

Priorities (user-visible issues are listed under [Status](README.md#status-playable) in the README):

1. **Display-list desyncs** — mostly fixed: the texrect handlers left the 16-byte LLE texrect's second word in the stream, so it ran as a command (0x06 calls, 0x07 branches, 0x0D othermode). An `abort:0` run went from 19 garbage color-image rejects to 0 (`ROGUESQ_LOG_DL_HEALTH=1`). Recheck longer runs and other levels; root-cause any remaining ones with [docs/f5-model-dl-spec.md](docs/f5-model-dl-spec.md) and `tools/validate/f5_dl_walk.py`.
2. **Retire render heuristics that a known microcode rule can replace** — e.g. the 0xBD sprite path (the ucode emits a screen-space texrect via overlay 0x2C) and the terrain grid shape. Verify each with the DL/RDRAM validation harness, not screenshots alone.
3. **Symbol renaming** — e.g. the "debris cell" functions in `funcs_36.c` (`buildDebrisMeshFromCells`, `emitDebrisCellFaces`, …) are the JFIF/JPEG decoder used by `tickFormatMessageWorker`. Regen and build after each batch.

The render path is HLE through the `GBI_F3DFACTOR5` profile.
