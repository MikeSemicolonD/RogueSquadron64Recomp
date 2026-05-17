<div align="center">
  <img src="https://github.com/MikeSemicolonD/RogueSquadron64Recomp/blob/main/favicon.ico">
  
   Icon created by [thedoctor45 on DeviantArt](https://www.deviantart.com/thedoctor45/art/Star-Wars-Rogue-Squadron-3D-Custom-Icon-535469296)

# Star Wars: Rogue Squadron 64 Recompiled

</div>

A naive static recomp of **Star Wars: Rogue Squadron** (N64, USA v1.0) built with the [N64Recomp](https://github.com/N64Recomp/N64Recomp) static recompilation toolchain and [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime).

> **Shelved by the original author.** The project boots, reaches the attribution screen, then advances to the N64 logo phase — but the **attribution text and N64-logo content never render** because of an architectural issue (see [Status](#status) and [Where it actually stops](#where-it-actually-stops)). The original author burned through many sessions debugging downstream symptoms before the root cause was identified, then handed the project off. Audio is stubbed (silent).
>
> The repository is being kept open as a starting point for someone with more time. The next contributor's most productive work is likely (a) the overlay-dispatch fix outlined in [Where it actually stops](#where-it-actually-stops), (b) continuing the function-renaming pass against `RecompiledFuncs/funcs_*.c` to make the codebase more legible, and (c) the MORT audio recompile that hasn't been started.
>
> **Heads-up on AI-assisted development.** Almost all the debugging, architectural decisions, and code in this repository — including the Factor 5 LLE/HLE bridge work, the runtime patches inside `lib/rt64` and `lib/N64ModernRuntime`, large parts of `src/main/main.cpp`, the `patches/` build pipeline, and most of the diagnostic env-var infrastructure — were produced with heavy AI assistance (Claude). Things to be aware of as a reader or contributor:
>
> - Many choices are pragmatic workarounds (defensive KSEG0 guards, env-gated diagnostic toggles, dead-code logging scaffolding) rather than principled fixes. Some of these may not be necessary once the overlay-dispatch issue is addressed.
> - Manual edits inside the `lib/rt64` and `lib/N64ModernRuntime` submodules, and inside generated files like `build/factor5_ucode/factor5_ucode_recompiled.c` and `RecompiledFuncs/`, are not committed to those upstream repos and can be clobbered on regeneration.
> - The architectural conclusions (especially around overlay dispatch and Factor 5 GBI handling) appear well-supported by the code evidence but should not be treated as final without scrutiny. Issues, corrections, and second opinions are very welcome.

<div align="center">
### [first-attempt](https://github.com/MikeSemicolonD/RogueSquadron64Recomp/tree/first-attempt)
<table>
  <tr>
    <td>
		<img src="https://github.com/MikeSemicolonD/RogueSquadron64Recomp/blob/main/screenshots/initial-screenshot.PNG">
    </td>
    <td>
		<img src="https://github.com/MikeSemicolonD/RogueSquadron64Recomp/blob/main/screenshots/past-initial-screenshot.PNG">
    </td>
  </tr>
  <tr>
    <td>
		<img src="https://github.com/MikeSemicolonD/RogueSquadron64Recomp/blob/main/screenshots/progress/past-initial-xwing.png">
    </td>
    <td>
		<img src="https://github.com/MikeSemicolonD/RogueSquadron64Recomp/blob/main/screenshots/progress/past-initial-factor5-logo.png">
    </td>
  </tr>
</table>
</div>

---

## Requirements

| Requirement | Notes |
|---|---|
| **ROM** | `rogue_squadron.z64` — USA v1.0 (16 MB, xxHash3-64: `0x6B66A44153594DEA`) |
| **OS** | Windows 10+, Linux, or macOS 11+ |
| **GPU** | D3D12 / Vulkan / Metal capable |
| **CMake** | 3.20+ |
| **Compiler** | MSVC with ClangCL toolset (Windows), Clang/GCC (Linux/macOS) |
| **N64Recomp output** | Pre-generated `RecompiledFuncs/` from the companion [rogue_squadron64](https://github.com/MikeSemicolonD/rogue_squadron64) decomp project (originally started by [Tmcg2](https://github.com/Tmcg2/rogue_squadron64))|
| **MIPS cross-compiler** *(optional but strongly recommended)* | `mips64-elf-gcc` for the [`patches/` build](patches/README.md). Windows: download [`gcc-toolchain-mips64-win64.zip`](https://github.com/n64-tools/gcc-toolchain-mips64/releases). The official LLVM Windows installers ship without the MIPS backend, so clang doesn't work as a substitute. Default install path: `E:/mips-toolchain` (override with `cmake -DMIPS_TOOLCHAIN_DIR=...`). Skip-able: cmake will warn and disable the patches build if the toolchain isn't found, and the rest of the project still builds. |
| **GNU make** | Used by `patches/Makefile`. `mingw32-make` from a MinGW install is fine. |

---

## Building

### 1 : Clone with submodules

```sh
git clone --recurse-submodules https://github.com/MikeSemicolonD/RogueSquadron64Recomp.git
cd RogueSquadron64Recomp
```

The `lib/` directory contains:
- [`lib/N64ModernRuntime`](https://github.com/MikeSemicolonD/N64ModernRuntime) — ultramodern + librecomp runtime
- [`lib/rt64`](https://github.com/MikeSemicolonD/rt64) — RT64 N64-compatible renderer

> The repos in `lib/` are forked to provide the maximum amount of developer freedom due to Factor5's custom byte code.

### 2 : Generate the recompiled C output

Before building this project you need the N64Recomp output. From the [rogue_squadron64](https://github.com/MikeSemicolonD/rogue_squadron64) decomp project:

```sh
# In the rogue_squadron64 repo:
splat split roguesquadron.yaml
python tools/make_elf.py
# Then in the N64Recomp repo:
N64Recomp rogue_squadron.toml
```

The output goes to `../N64Recomp/RecompiledFuncs/` (relative to this repo).

### 3 : Configure and build

**Windows (Visual Studio + ClangCL):**
```sh
cmake -B build -T ClangCL
cmake --build build --config Debug    # for development (recommended while debugging)
cmake --build build --config Release  # for performance
```

**Linux / macOS:**
```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

The binary is written to `build/Debug/RogueSquadron64Recomp.exe` (Windows) or
`build/RogueSquadron64Recomp` (Linux/macOS).

#### Build options

| CMake option | Default | Purpose |
|---|---|---|
| `-DMIPS_TOOLCHAIN_DIR=path` | `E:/mips-toolchain` | Override the mips64-elf-gcc install path for `patches/`. |
| `-DROGUESQ_DX12_DEBUG=ON` | OFF | Enable the D3D12 debug layer (use only with Debug builds). |
| `-DROGUESQ_NO_ITER_DEBUG=ON` | OFF | Disable MSVC debug iterators in `lib/rt64` (defines `_HAS_ITERATOR_DEBUGGING=0 _ITERATOR_DEBUG_LEVEL=0`). Speeds up Debug runs that exercise lots of std::vector access in the renderer. |

#### Incremental builds

When iterating on host-side code (`src/main/*`, `lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp`), Debug builds typically rebuild + link in ~10–30s. Regenerating `RecompiledFuncs/` (step 2) is only required when the game's TOML changes — for day-to-day work, run only step 3.

#### Build pipeline summary

1. `RecompiledFuncs/` (from N64Recomp) → game's MIPS recompiled to C
2. `patches/` (optional MIPS GCC step) → `patches.elf` → recompiled-back → linked ahead of `RecompiledFuncs/` so override symbols win
3. `lib/rt64` + `lib/N64ModernRuntime` → static-linked host runtime
4. `src/main/*` → entry point wiring everything together

---

## Running

Place `rogue_squadron.z64` in the same directory as the executable, then launch it. The runtime verifies the ROM hash on startup.

```
RogueSquadron64Recomp.exe
```

---

## Controls (Gamepad)

| N64 | Action | Gamepad |
|---|---|---|
| Analog stick | Steer / bank | Left stick |
| A | Fire lasers | A |
| B | Drop bombs | X |
| Z trigger | Brake / decelerate | Left analog trigger |
| R | Boost / accelerate | Right shoulder |
| L | Targeting computer | Left shoulder |
| D-Pad | Throttle / trim | D-Pad |
| C-Up / C-Down | Cycle views | Y / B |
| C-Left / C-Right | Roll | Back / Guide |
| Start | Pause | Start |

---

## Project Structure

```
src/
  main/main.cpp                — Entry point, SDL2 audio/input/window, RSP ucode dispatch
  main/rt64_render_context.cpp — RT64 renderer integration
  main/register_overlays.cpp   — Game function-table registration
  rsp/dpc_bridge.cpp           — DPC_START/DPC_END bridge for Factor5 LLE → RT64
  rsp/aspMain.cpp              — Audio RSP microcode stub (MusyX, silent)
include/
  recomp_game.h                — Forward declarations for runtime functions
lib/
  N64ModernRuntime/            — Runtime (ultramodern + librecomp)
  rt64/                        — RT64 N64-compatible renderer
factor5_rsp.toml               — RSPRecomp config for the Factor5 graphics ucode
factor5_boot_rsp.toml          — RSPRecomp config for the Factor5 boot ucode
factor5_boot_text.bin          — Boot-ucode text segment (input to RSPRecomp)
```

### Graphics path: Factor 5 LLE recompile

Rogue Squadron uses Factor 5's custom GBI, which RT64's HLE pipeline cannot
recognise. To work around that, the game's RSP graphics ucode is statically
recompiled to C with [RSPRecomp](https://github.com/N64Recomp/N64Recomp) and
runs as `factor5_ucode()` on the SP task thread. RDP commands the ucode emits
via `mtc0` to `DPC_START` / `DPC_END` are forwarded to RT64's RDP interpreter
through `src/rsp/dpc_bridge.cpp`, giving low-level (LLE) parity without
requiring a custom HLE GBI profile.

### Patching auto-generated functions: the `patches/` build

Defensive guards and overrides for game functions live in [`patches/`](patches/),
**not** as hand-edits to the auto-generated `RecompiledFuncs/funcs_*.c`. Hand-
written C in `patches/*.c` is cross-compiled to MIPS, run back through
N64Recomp, and linked ahead of the auto-generated output so its symbols win
the duplicate-resolution at link time. Auto-generated source can therefore be
regenerated cleanly without losing our work.

The pattern follows
[Zelda64Recompiled/patches](https://github.com/Zelda64Recomp/Zelda64Recomp/tree/dev/patches),
**but uses `mips64-elf-gcc` instead of `clang -target mips`** — current
official LLVM Windows builds (19, 20, and 22.x release candidates) ship
without the MIPS backend, while Linux/Mac LLVM packages include it. We use
the [n64-tools](https://github.com/n64-tools/gcc-toolchain-mips64/releases)
prebuilt MIPS GCC for Windows. See [patches/README.md](patches/README.md) for:

- Toolchain install steps and verification
- The full pipeline (`mips64-elf-gcc` → ELF → N64Recomp → host C → link)
- How to write an override + the syms.ld pattern
- Flag-by-flag differences from Zelda's clang-based Makefile (we drop the
  clang-only flags `-target mips`, `-mno-odd-spreg`, `-mno-check-zero-division`,
  `-Wno-incompatible-library-redeclaration`, `-Wno-unsupported-floating-point-opt`)
- Constraints (`-nostdinc` means no `stddef.h` / `stdint.h` — inline the
  typedefs you need)

---

## Status

| System | Status |
|---|---|
| Boot sequence | Reaches the attribution screen, advances to the N64 logo phase, no crash |
| Attribution screen | **Visible only as a coloured clear** (default black with the canonical `0x00010001` fill, or bright green if `ROGUESQ_HLE_OP02_EXPERIMENTAL=1` is set as a visibility marker). The actual "STAR WARS: ROGUE SQUADRON / LucasArts / Factor 5" text is **not rendered** — see [Where it actually stops](#where-it-actually-stops) |
| N64 logo | Phase is reached without crashing, but no logo content renders (white screen) |
| Cinematic | Not reached in a useful state — depends on N64 logo phase completing visibly |
| Main menu | Not reached |
| Video pipeline (host side) | RT64 + custom F3DFACTOR5 GBI module dispatching cleanly. SDL window, swapchain, ImGui inspector (F1), VI presentation timing all working. The pixels-from-RDRAM-to-screen path is intact — verified via `ROGUESQ_LOG_VI_FB_CONTENT=1` which shows whatever the game writes to RDRAM does reach the swapchain |
| Input | Working (SDL2 gamepad). Keyboard support not implemented |
| Save data | EEPROM 4K via librecomp |
| Audio | Stubbed — silent. Per [rerogue](https://github.com/jrra/rerogue) the codec is **MORT**, not MusyX as previously assumed; needs a separate RSPRecomp pass for either codec |
| Memory pak | Stubbed — returns no-pak |

### Where it actually stops

The attribution screen and N64 logo both fail to render their actual content because the game's CPU code never produces any pixel data beyond the canonical-black framebuffer clear. `ROGUESQ_LOG_VI_FB_CONTENT=1` traces confirm this directly: during the attribution-display loop the VI framebuffer at `0x806BA000` has every single pixel (71680 of 71680) set to `0x0001` (canonical N64 black with alpha=1). No text bytes, no glyphs, no draw commands. The host renderer is faithfully showing what's in RDRAM — and what's in RDRAM is just a clear.

The **root cause** is an overlay-dispatch issue rather than a rendering issue:

1. The game ships three overlays that all load at VA `0x800A5130`: `.ovl.mission` (ROM 0xA5D30), `.ovl.menu` (ROM 0x10C2D0), `.ovl.cinematic` (ROM 0x137580). At runtime they swap in and out as the game progresses.
2. N64Recomp's output (`RecompiledFuncs/recomp_overlays.inl`) **does** generate separate function arrays for all three overlays (`section_4_ovl_mission_funcs`, `section_5_ovl_menu_funcs`, `section_6_ovl_cinematic_funcs`).
3. **But** the recompiled C emits **direct C function calls** for `jal` instructions targeting the overlay VA range — each bound at link time to one overlay's version (whichever the recompiler picked). When the game loads the menu overlay and calls `jal 0x800A5D80`, the C code still calls the mission/cinematic-overlay function it was bound to. The menu overlay's distinct code — including the attribution-text draw — never executes.

`func_map` (librecomp's runtime address→function table) is only consulted for *indirect* calls. The `loadOverlay` hook added in `src/main/upstream_compat.cpp` (`rs64_load_overlay`) keeps `func_map` correct at runtime, but direct calls never look at it, so the hook alone changes nothing.

**Two fixes were evaluated (2026-05-16); neither works as-is:**

- **`use_lookup_for_all_function_calls`** — an N64Recomp config flag that converts *every* `jal` into a `LOOKUP_FUNC(addr)` runtime dispatch. The toml key was a dead key (the recompiler's `config.cpp` never parsed it); wiring it up + regenerating produced 14,377 lookup calls and correctly converted overlay calls. **But** it also converts calls to runtime-provided libultra functions (`osYieldThread` at 0x80037510, etc.), which are *not* in `func_map` — so those calls hit `get_function` → assert → abort early in boot.
- **`relocatable_sections_path`** — declaring the three overlay sections relocatable so only overlay-target calls become lookups (libultra calls stay direct). This is the *cleaner* mechanism, but the recompiler needs ELF **relocations** on the cross-section calls to resolve them; the companion `rogue_squadron64` decomp ELF carries none on the overlay sections, so `jal`-into-overlay resolves to `NoMatch` and recompilation fails (`No function found for jal target: 0x800C58A0`).

**The genuine fix needs one of:**

1. Register the runtime-provided libultra functions in `func_map` (by VA), then `use_lookup_for_all_function_calls` works cleanly. This is a librecomp-side change to how `func_map` is populated.
2. Rebuild the `rogue_squadron64` decomp ELF so the overlay sections carry relocations on cross-section calls, then `relocatable_sections_path` works. This is a change to the decomp project's splat config / linker script.

Both are real infrastructure work, not a same-session patch. The N64Recomp source change that wires up the `use_lookup_for_all_function_calls` toml key (`src/config.cpp`, `src/config.h`, `src/main.cpp`) is a correct improvement and can stay — it just isn't sufficient alone.

### Other known issues

- **Audio**: stubbed entirely. Per [rerogue](https://github.com/jrra/rerogue) PC-version reversing, the codec is **MORT**, not MusyX. Either codec needs an RSPRecomp pass against the audio ucode segment in the ROM. No starting work has been done.
- **Defensive KSEG0 guards in `RecompiledFuncs/funcs_*.c`**: many functions have hand-instrumented pointer-validity guards to survive wild-pointer dereferences. Most of these were added while chasing downstream symptoms of the overlay-dispatch issue and may not be needed once the overlays dispatch correctly. Leave them for now — strip them only after the overlay fix is verified.
- **`func_80022048` stub** and the **SEH wrap of the recompiled thread entry** in `lib/N64ModernRuntime/librecomp/src/recomp.cpp` are similarly downstream-symptom guards that should be revisited after the overlay fix.

---

## Contributing / Debugging

If you want to investigate a crash or behavior bug, see
[docs/debugging-with-visual-studio.md](docs/debugging-with-visual-studio.md)
for how to attach Visual Studio to the recompiled output, set conditional
breakpoints inside `funcs_*.c`, walk back through a bad register value,
and tell a recompile bug apart from a game-logic bug.

For hangs or post-mortem analysis there's a helper toolkit under
[tools/](tools/):

- `tools/dump-game.ps1` — captures a full-memory minidump of a running
  `RogueSquadron64Recomp.exe` even when the GUI is unresponsive. Uses
  the kernel `MiniDumpWriteDump` API so it doesn't depend on the
  target's SDL message pump.
- `tools/inspect-dump.py` — pip `minidump` reader. Lists every thread
  in the dump and tags it `in_exe` (running our recompile),
  `kernel_wait` (parked in ntdll), or other-module. The two or three
  `in_exe` threads are usually the only ones worth opening in VS.
- `tools/run-stability.ps1` — N-run harness. Launches the binary
  `-Runs N` times with `-Timeout` seconds each, captures per-run
  stderr to `logs/stability/<tag>/run_N.log`, and classifies outcomes
  by grepping markers (`[CRASH]`, `[ABORT]`, `[L_627C-FIRST]`,
  `[cine-tick]`, `[guard]`). Prints a summary table + CSV. Use
  `-EnvVars "FOO=1;BAR=2"` to forward debug-trace env vars.
- `tools/measure-leak.ps1` — single-run harness that polls Working
  Set / Private Bytes / Virtual Memory once per second and writes
  `memory.csv`. Useful when correlating per-second memory growth with
  stderr trace timestamps.

The runtime writes `mqdiag_NNN.txt` snapshots every 3 seconds via a
watchdog thread (`start_mqdiag_watchdog` in
[src/main/main.cpp](src/main/main.cpp)). Useful for finding queues
with backed-up events or threads stuck on a missing message. F12
inside the game window writes an ad-hoc minidump.

Most diagnostic logs are gated behind `ROGUESQ_LOG_*` env vars (see
[docs/debug-trace-env-vars.md](docs/debug-trace-env-vars.md) — read
before adding new `fprintf` or asking which trace to enable). Notable:

| Env var | What it enables |
|---|---|
| `ROGUESQ_LOG_ALL` | Master switch — turns on everything |
| `ROGUESQ_LOG_GBI` | Per-handler logs in the F3DFACTOR5 GBI (texrect / setCombine / setOtherMode / setScissor / setCIMG accepts + rejects). High volume — locks the ImGui inspector after a few seconds if left on |
| `ROGUESQ_LOG_CIMG` | Per-fb setCIMG + per-fb texrect frequency counters. Bounded output, useful for correlating render targets with VI's sampled fb |
| `ROGUESQ_LOG_CINE_CP` | Per-call-site checkpoints inside `func_800A5D80`'s cinematic loop body. Pair with `ROGUESQ_LOG_CINE_CP_FROM=N` to start verbose logging at iter N |
| `ROGUESQ_LOG_RT64_ALLOC` | RT64 allocation hotspots (interpolatedColorTargets, nativeSwappedRAM, rdramData, BufferPair, RenderTarget) — use to find allocation spikes |
| `ROGUESQ_HLE_DEV_MODE` | Default ON. Set to `0` to disable RT64's ImGui developer overlay (F1 inspector) |
| `ROGUESQ_HLE_AUTO_FULLSYNC` | Default OFF. Set to `1` to force an extra `state->fullSync()` after every `processDisplayLists`. Factor 5 already emits its own G_RDPFULLSYNC, so this usually overwrites the committed workload with an empty one — only useful as a diagnostic toggle |
| `ROGUESQ_HLE_PRESENT_EARLY` | Default OFF. Set to `1` to switch RT64 into `PresentEarly` presentation mode (Zelda64Recomp's default). Causes instability with our Factor 5 flow — keep off unless investigating |
| `ROGUESQ_HLE_NO_AA` | Default OFF. When set, strips `AA_EN` (bit 14) from any setOtherModeL write that covers it. Use to test whether anti-aliasing + zero combiner alpha is suppressing pixels |
| `ROGUESQ_HLE_NO_CVGA` | Default OFF. Strips `CVG_X_ALPHA` (bit 23) and `ALPHA_CVG_SEL` (bit 24) from otherModeL writes. Same diagnostic class as `NO_AA` but targets the coverage-from-alpha path |
| `ROGUESQ_HLE_FORCE_OPAQUE` | Default OFF. Strips AA_EN + CVG_X_ALPHA + ALPHA_CVG_SEL all at once. Broadest "make pixels visible regardless of combiner alpha" sledgehammer |
| `ROGUESQ_HLE_FORCE_VISIBLE` | Default OFF. Replaces every Factor 5 setCombine + setFillColor with a known-rendering "solid magenta" setup. If magenta appears on screen, the GPU pipeline works and the bug is purely combiner-mux-specific |
| `ROGUESQ_GFX_API` | Force a specific graphics API. Values: `vulkan` or `d3d12`. Default is `auto` (D3D12 on Windows) |
| `ROGUESQ_SUPPRESS_OOB_CIMG` | **Default OFF.** When set, drops Factor 5 ucode emissions of bogus SET_COLOR_IMAGE commands at HIGH (≥ 0x800000) and LOW (< 0x100000) addresses before they reach RT64. Reduces the iter-810 memory spike but causes a visual regression — the 3D Factor 5 logo no longer renders, since some legitimate Factor 5 lowmem CIMGs are dropped along with the garbage |
| `ROGUESQ_HWBP` + `ROGUESQ_HWBP_ADDR` | Win32 DR0 hardware breakpoint on a configurable RDRAM address |

### RT64 developer overlay (F1)

With `ROGUESQ_HLE_DEV_MODE` enabled (default), pressing **F1** in the window toggles RT64's ImGui inspector:

- **Configuration** — resolution / aspect / antialiasing / framebuffer settings.
- **Textures** — load texture packs; **Start dumping textures** writes every TMEM load + palette to a directory of your choice (useful for confirming TMEM populates correctly).
- **Debugger** — pause/resume (F4), Frame stats, per-fbPair → per-rectangle → per-Call inspector with vertex/pixel shader dump buttons.
- **Render** — render-target visualization.

Other developer hotkeys: **F2** ray tracing toggle (not yet public), **F3** ViewRDRAM mode, **F4** texture replacement toggle.

---

## License

See [LICENSE](LICENSE). This project contains no ROM data and requires a legally-obtained copy of the game.
