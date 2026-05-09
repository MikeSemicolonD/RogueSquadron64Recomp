<div align="center">
  <img src="https://github.com/MikeSemicolonD/RogueSquadron64Recomp/blob/main/favicon.ico">
  
   Icon created by [thedoctor45 on DeviantArt](https://www.deviantart.com/thedoctor45/art/Star-Wars-Rogue-Squadron-3D-Custom-Icon-535469296)

# Star Wars: Rogue Squadron 64 Recompiled

</div>

A native PC port of **Star Wars: Rogue Squadron** (N64, USA v1.0) built with the [N64Recomp](https://github.com/N64Recomp/N64Recomp) static recompilation toolchain and [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime).

> **Work in progress.** Boot reaches the N64 logo and the post-logo
> cinematic begins — explosion particles and Factor 5 3D logo elements
> are visible. The cinematic CPU thread runs to completion in ~30% of
> attempts (reaching the natural-exit path at fc=1120 and entering
> menu-init), but the other ~70% of attempts time out around iter ~810
> due to an intermittent OS-level thread-starvation issue. The main
> menu has been reached but is not yet stable. Audio is stubbed
> (silent — Factor 5's audio ucode is not yet recompiled). See
> [Status](#status) for details.
>
> **Heads-up on AI-assisted development.** Most of the debugging, architectural
> decisions, and code in this repository — including the Factor 5 LLE recompile
> bridge, the runtime patches inside `lib/rt64` and `lib/N64ModernRuntime`, and
> large parts of `src/main/main.cpp` — were produced with heavy assistance from
> AI coding tools (Claude). Things to be aware of as a reader or contributor:
>
> - Some choices are pragmatic workarounds (e.g. the `if(0) fprintf(...)`
>   dead-code logging scattered through the renderer, the PIPESYNC-only RDP
>   filter, the HLE-arm-for-signaling-only path) rather than principled fixes.
> - Manual edits inside the `lib/rt64` and `lib/N64ModernRuntime` submodules,
>   and inside generated files like `build/factor5_ucode/factor5_ucode_recompiled.c`,
>   are not committed upstream and can be clobbered on regeneration.
> - Both the LLE bridge approach and several runtime decisions are novel and
>   underverified — they appear to work for the boot sequence but should not
>   be assumed correct without scrutiny. Issues, corrections, and second
>   opinions are very welcome.

<div align="center">
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
cmake --build build --config Release
```

**Linux / macOS:**
```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The binary is written to `build/Release/RogueSquadron64Recomp` (or `build/Debug/` for debug builds).

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
| Video / RDP | Working — RT64 driven by Factor 5 LLE recompile + DPC bridge |
| Boot sequence | Reaches LucasArts attribution → N64 logo → post-logo cinematic. Factor 5 logo + explosion particles render. Cinematic reaches natural exit (fc=1120) in ~30% of runs |
| Main menu | Intermittently reached after natural exit. `func_800C58A0` menu_overlay_init still has heap-walker issues |
| Cinematic loop freeze | Open. ~70% of runs time out at iter ~810 due to OS-level thread starvation (cinematic CPU thread blocks). Empirical: stderr writes unstick it, `Sleep` does not |
| Input | Working (SDL2 gamepad — game shows "NO CONTROLLER" until one is plugged in). Keyboard support pending (Zelda-style bind/rebind UI) |
| Save data | EEPROM 4K via librecomp |
| Audio | Stubbed — silent. Per [rerogue](https://github.com/jrra/rerogue) the codec is **MORT**, not MusyX as previously assumed; both still need a separate recompile pass |
| Memory pak | Stubbed — returns no-pak |

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
| `ROGUESQ_LOG_CINE_CP` | Per-call-site checkpoints inside `func_800A5D80`'s cinematic loop body. Pair with `ROGUESQ_LOG_CINE_CP_FROM=N` to start verbose logging at iter N |
| `ROGUESQ_LOG_RT64_ALLOC` | RT64 allocation hotspots (interpolatedColorTargets, nativeSwappedRAM, rdramData, BufferPair, RenderTarget) — use to find allocation spikes |
| `ROGUESQ_SUPPRESS_OOB_CIMG` | **Default OFF.** When set, drops Factor 5 ucode emissions of bogus SET_COLOR_IMAGE commands at HIGH (≥ 0x800000) and LOW (< 0x100000) addresses before they reach RT64. Reduces the iter-810 memory spike but causes a visual regression — the 3D Factor 5 logo no longer renders, since some legitimate Factor 5 lowmem CIMGs are dropped along with the garbage |
| `ROGUESQ_HWBP` + `ROGUESQ_HWBP_ADDR` | Win32 DR0 hardware breakpoint on a configurable RDRAM address |

---

## License

See [LICENSE](LICENSE). This project contains no ROM data and requires a legally-obtained copy of the game.
