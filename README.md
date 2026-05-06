<div align="center">
  <img src="https://github.com/MikeSemicolonD/RogueSquadron64Recomp/blob/main/favicon.ico">
  
   Icon created by [thedoctor45 on DeviantArt](https://www.deviantart.com/thedoctor45/art/Star-Wars-Rogue-Squadron-3D-Custom-Icon-535469296)

# Star Wars: Rogue Squadron 64 Recompiled

</div>

A native PC port of **Star Wars: Rogue Squadron** (N64, USA v1.0) built with the [N64Recomp](https://github.com/N64Recomp/N64Recomp) static recompilation toolchain and [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime).

> **Work in progress.** Boot reaches the N64 logo screen and stays there.
> On real hardware the N64 logo is followed by an explosion, a camera pan
> with an X-wing and a TIE fighter revealing the LucasArts logo imprinted
> against terrain, and finally the X-wing flying in to display the
> Factor 5 logo in 3D — none of that post-logo cinematic renders here.
> The recompile actually progresses internally past the logo (the menu /
> credits state machine dispatches and emits text-render commands), but
> the game's per-frame state driver isn't getting onto the dispatch
> table, so its framebuffer never wins the swap arbitration and the
> screen visibly stays on the N64 logo. Audio is stubbed (silent).
> See [Status](#status) for details.
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

---

## Building

### 1 : Clone with submodules

```sh
git clone --recurse-submodules <this-repo>
cd RogueSquadron64Recomp
```

The `lib/` directory contains:
- [`lib/N64ModernRuntime`](https://github.com/MikeSemicolonD/N64ModernRuntime) — ultramodern + librecomp runtime
- [`lib/rt64`](https://github.com/MikeSemicolonD/rt64) — RT64 N64-compatible renderer

> The repos in `lib/` are forked to provide the maximum amount of developer freedom due to Factor5's custom byte code.

### 2 : Generate the recompiled C output

Before building this project you need the N64Recomp output. From the companion decomp project:

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

---

## Status

| System | Status |
|---|---|
| Video / RDP | Working — RT64 driven by Factor 5 LLE recompile + DPC bridge |
| Boot sequence | Reaches the N64 logo and visibly halts there. None of the post-logo cinematic plays — on real hardware that's an explosion, a camera pan with X-wing + TIE fighter showing the LucasArts logo on terrain, then the X-wing flying in to display the Factor 5 3D logo. Internally the menu/credits state machine dispatches (state=9, glyph commands emitted) but its framebuffer never wins the swap-priority arbitration, so the displayed image stays on the N64 logo |
| Input | Working (SDL2 gamepad — game shows "NO CONTROLLER" until one is plugged in) |
| Save data | EEPROM 4K via librecomp |
| Audio | Stubbed — silent (Factor 5's MusyX ucode is not yet recompiled) |
| Memory pak | Stubbed — returns no-pak (Yet the game seems to detect one) |

---

## Contributing / Debugging

If you want to investigate a crash or behavior bug, see
[docs/debugging-with-visual-studio.md](docs/debugging-with-visual-studio.md)
for how to attach Visual Studio to the recompiled output, set conditional
breakpoints inside `funcs_*.c`, walk back through a bad register value,
and tell a recompile bug apart from a game-logic bug.

For hangs or post-mortem analysis there's a small helper toolkit under
[tools/](tools/):

- `tools/dump-game.ps1` — captures a full-memory minidump of a running
  `RogueSquadron64Recomp.exe` even when the GUI is unresponsive. Uses
  the kernel `MiniDumpWriteDump` API so it doesn't depend on the
  target's SDL message pump.
- `tools/inspect-dump.py` — pip `minidump` reader. Lists every thread
  in the dump and tags it `in_exe` (running our recompile),
  `kernel_wait` (parked in ntdll), or other-module. The two or three
  `in_exe` threads are usually the only ones worth opening in VS.

The runtime also writes `mqdiag_NNN.txt` snapshots every 3 seconds via
a watchdog thread (`start_mqdiag_watchdog` in [src/main/main.cpp](src/main/main.cpp)).
That file shows per-message-queue send/recv/external/delivered/lost
counts — useful for finding queues with backed-up events or threads
stuck on a missing message. F12 inside the game window writes an
ad-hoc minidump (when the message pump is alive).

---

## License

See [LICENSE](LICENSE). This project contains no ROM data and requires a legally-obtained copy of the game.
