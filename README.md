<div align="center">
  <img src="https://github.com/MikeSemicolonD/RogueSquadron64Recomp/blob/main/favicon.ico">
  
   Icon created by [thedoctor45 on DeviantArt](https://www.deviantart.com/thedoctor45/art/Star-Wars-Rogue-Squadron-3D-Custom-Icon-535469296)

# Star Wars: Rogue Squadron 64 Recompiled

</div>

A native PC port of **Star Wars: Rogue Squadron** (N64, USA v1.0) built with the [N64Recomp](https://github.com/N64Recomp/N64Recomp) static recompilation toolchain and [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime).

> **Work in progress.** Audio HLE is stubbed; the game may reach the title screen but gameplay is not yet fully functional.

<div align="center">
  <img src="https://github.com/MikeSemicolonD/RogueSquadron64Recomp/blob/main/screenshots/initial-screenshot.PNG">
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
| **N64Recomp output** | Pre-generated `RecompiledFuncs/` from the companion [rogue_squadron64](https://github.com/MikeSemicolonD/rogue_squadron64) decomp project |

---

## Building

### 1 — Clone with submodules

```sh
git clone --recurse-submodules <this-repo>
cd RogueSquadron64Recomp
```

The `lib/` directory contains:
- `lib/N64ModernRuntime` — ultramodern + librecomp runtime
- `lib/rt64` — RT64 N64-compatible renderer

### 2 — Generate the recompiled C output

Before building this project you need the N64Recomp output. From the companion decomp project:

```sh
# In the rogue_squadron64 repo:
splat split roguesquadron.yaml
python tools/make_elf.py
# Then in the N64Recomp repo:
N64Recomp rogue_squadron.toml
```

The output goes to `../N64Recomp/RecompiledFuncs/` (relative to this repo).

### 3 — Configure and build

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
  main/main.cpp               — Entry point, SDL2 audio/input/window, game registration
  main/rt64_render_context.cpp — RT64 renderer integration
  rsp/aspMain.cpp             — Audio RSP microcode stub (TODO: full HLE)
include/
  recomp_game.h               — Forward declarations for runtime functions
lib/
  N64ModernRuntime/           — Runtime (ultramodern + librecomp)
  rt64/                       — RT64 renderer
```

---

## Status

| System | Status |
|---|---|
| Video / RDP | Working (RT64) |
| Input | Working (SDL2 gamepad) |
| Save data | EEPROM 4K via librecomp |
| Audio | Stubbed — silent |
| Memory pak | Stubbed — returns no-pak |

---

## License

See [LICENSE](LICENSE). This project contains no ROM data and requires a legally-obtained copy of the game.
