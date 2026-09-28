<div align="center">
  <img alt="favicon made by thedoctor45" src="https://github.com/MikeSemicolonD/RogueSquadron64Recomp/blob/main/favicon.ico">

  Icon by [thedoctor45 on DeviantArt](https://www.deviantart.com/thedoctor45/art/Star-Wars-Rogue-Squadron-3D-Custom-Icon-535469296)

# Star Wars: Rogue Squadron 64 Recompiled

</div>

A static recompilation of **Star Wars: Rogue Squadron** (N64, USA v1.0) built with [N64Recomp](https://github.com/N64Recomp/N64Recomp) and [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime), rendering through a forked [RT64](https://github.com/MikeSemicolonD/rt64) that understands Factor 5's custom display-list format.

> [!IMPORTANT]
> **Work in progress & Heavily AI-assisted.**
> Most of the debugging, architectural decisions, and code here (the F3DFACTOR5 GBI module, the runtime patches inside `lib/`, `src/main/`, the `patches/` pipeline, the diagnostic env vars) were produced with Claude.

---

<div align="center">

### Screenshots

<table>
  <tr>
    <td><img alt="Recomp screenshot" src="./screenshots/progress2/Capture1.PNG"></td>
    <td><img alt="Recomp screenshot" src="./screenshots/progress2/Capture2.PNG"></td>
    <td><img alt="Recomp screenshot" src="./screenshots/progress2/Capture3.PNG"></td>
    <td><img alt="Recomp screenshot" src="./screenshots/progress2/Capture4.PNG"></td>
    <td><img alt="Recomp screenshot" src="./screenshots/progress2/Capture5.PNG"></td>
  </tr>
  <tr>
    <td><img alt="Recomp screenshot" src="./screenshots/progress2/Capture6.PNG"></td>
    <td><img alt="Recomp screenshot" src="./screenshots/progress2/Capture7.PNG"></td>
    <td><img alt="Recomp screenshot" src="./screenshots/progress2/Capture8.PNG"></td>
    <td><img alt="Recomp screenshot" src="./screenshots/progress2/Capture9.PNG"></td>
    <td><img alt="Recomp screenshot" src="./screenshots/progress2/Capture10.PNG"></td>
  </tr>
</table>
</div>

---

## Requirements

| Requirement | Notes |
| --- | --- |
| **ROM** | `rogue_squadron.z64`, USA v1.0 (16 MB, xxHash3-64 `0x6B66A44153594DEA`) |
| **OS / GPU** | Windows 10+, Linux, or macOS 11+ with a D3D12, Vulkan, or Metal capable GPU; Android 8+ arm64 with Vulkan 1.1 (see [Android notes](#android-notes)); macOS is compile-only so far (see [macOS notes](#macos-notes)) |
| **CMake** | 3.20+ |
| **Compiler** | MSVC with the ClangCL toolset (Windows), Clang or GCC (Linux/macOS) |
| **N64Recomp output** | `RecompiledFuncs/`, generated locally from the companion [rogue_squadron64](https://github.com/MikeSemicolonD/rogue_squadron64) decomp (started by [Tmcg2](https://github.com/Tmcg2/rogue_squadron64)) via the `regen_funcs` target |
| **MIPS cross-compiler** *(optional)* | `mips64-elf-gcc` for the [`patches/` build](patches/README.md). Windows builds are at [n64-tools](https://github.com/n64-tools/gcc-toolchain-mips64/releases); the official LLVM Windows installers lack the MIPS backend. Default path `E:/mips-toolchain` (override with `-DMIPS_TOOLCHAIN_DIR`). Without it CMake warns and skips the patches build. |
| **GNU make** *(optional)* | For `patches/Makefile`. `mingw32-make` works. |

---

## Building

### 1. Clone with submodules

```sh
git clone --recurse-submodules https://github.com/MikeSemicolonD/RogueSquadron64Recomp.git
cd RogueSquadron64Recomp
```

`lib/` holds forks of [N64ModernRuntime](https://github.com/MikeSemicolonD/N64ModernRuntime) and [rt64](https://github.com/MikeSemicolonD/rt64). Forked because these libraries don't support Factor 5's custom microcode.

### 2. Produce the decomp ELF

The recompiler needs the ELF from the companion [rogue_squadron64](https://github.com/MikeSemicolonD/rogue_squadron64) decomp:

```sh
# In the rogue_squadron64 repo:
splat split roguesquadron.yaml
python tools/make_elf.py
```

### 3. Configure, generate the recompiled C, and build

```sh
# Windows (Visual Studio + ClangCL). Debug is the tested configuration.
cmake -B build -T ClangCL
cmake --build build --config Debug --target regen_funcs   # generate RecompiledFuncs/ from your ROM
cmake --build build --config Debug

# Linux / macOS
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target regen_funcs
cmake --build build
```

`regen_funcs` runs the recompiler (built from the `lib/N64ModernRuntime/N64Recomp` submodule) on `rogue_squadron.toml`, producing the gitignored `RecompiledFuncs/`. Re-run it when `rogue_squadron.toml`, the symbols, or the decomp ELF changes.

| CMake option | Default | Purpose |
| --- | --- | --- |
| `DMIPS_TOOLCHAIN_DIR=path` | `E:/mips-toolchain` | Location of `mips64-elf-gcc` for `patches/` |
| `DROGUESQ_DX12_DEBUG=ON` | OFF | D3D12 debug layer (Debug builds only) |
| `DROGUESQ_NO_ITER_DEBUG=ON` | OFF | Disable MSVC debug iterators in `lib/rt64` for faster Debug runs |

### Linux / WSL notes

Builds and can run on **Ubuntu 24.04 under WSL2** (GCC 13 / Ninja); native Linux should behave the same with a real Vulkan driver. Non-Windows targets render through **Vulkan**. (D3D12 is Windows-only)

Install the dependencies, then configure with Ninja and build the game target:

```sh
sudo apt install build-essential cmake ninja-build libsdl2-dev libvulkan-dev libgtk-3-dev python3
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target RogueSquadron64Recomp
```

The `patches/` override layer is skipped automatically without `mips64-elf-gcc` (Linux clang/gcc ships the MIPS backend, so it *can* be built — see [patches/README.md](patches/README.md)); without it the game links and runs, minus the `npc_health_guard` explosion fix.

#### About WSL2

- Keep the build directory on the Linux filesystem (e.g. `~/rs64-build`), **not** under `/mnt/…` — CMake's compiler checks can fail with `Operation not permitted` on the Windows drive mount. Source can stay on `/mnt`.
- WSLg supplies the display, PulseAudio (sound), and keyboard/mouse, so the game runs directly — no extra setup. A physical gamepad needs `usbipd-win`; the keyboard works out of the box.
- WSL's default Vulkan is **llvmpipe (software)** — it runs but hitches. For GPU acceleration, build [Mesa's](https://gitlab.freedesktop.org/mesa/mesa) **[Dozen (`dzn`)](https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/microsoft/vulkan?ref_type=heads)** driver (Vulkan → D3D12 → the real GPU via `/dev/dxg`). If the game doesn't detect `dzn` it'll default to software (CPU) rendering.
- The `tools/run-wsl-gpu.sh` helper and the `Configure` / `Build` / `Run (Linux/WSL, GPU via dzn)` VS Code tasks wrap this. (`dzn` has no ray-tracing extensions — irrelevant to the current raster path.)

### Android notes

Builds as an arm64 APK and runs on a **(Snapdragon 8 Gen 3 / Adreno 750, Android 16)**; other arm64 phones with Vulkan 1.1 should work, but driver quality varies. The Gradle project in `android/` builds the same `CMakeLists.txt` with the NDK.

Needs **Android Studio** (for its SDK, bundled JDK and `adb`) with **NDK 29.0.14206865**; Gradle fetches SDL 2.32.10 and installs its CMake itself.

```powershell
$env:JAVA_HOME = "C:\Program Files\Android\Android Studio\jbr"
cd android
.\gradlew.bat assembleRelease   # app\build\outputs\apk\release\app-release.apk
adb install -r app\build\outputs\apk\release\app-release.apk
```

- **Use the Release APK.** The Debug APK compiles the recompiled game code at `-O0` and stutters. Release is signed with the debug key, so it installs over a Debug build and keeps your data. The VS Code tasks `Build (Android): APK (Release)` and `Install + Run (Android, Release)` wraps the commands above.
- **ROM:** on first launch the system file picker asks for `rogue_squadron.z64`; the ROM is checked and imported once. (Pushing it to `/sdcard/Android/data/com.rs64recomp.app/files/` with `adb push` also works.)
- **Controls:** touch controls are on by default: tap menu entries directly, swipe carousels, and fly with a floating stick plus N64-style buttons. The user can move and resize the buttons by using the gear button in corner of the main menu or **TOUCH LAYOUT** (Requires [touch-layout](mods/touch-layout) mod) in the pause menu. **GYRO STEERING** (Requires [gyro-toggle](mods/gyro-toggle) mod) in Game Settings turns on tilt steering with mouse-like up/down aiming. Android Back is B; a paired gamepad works and hides the touch overlay. Settings live in `roguesq_touch.json`.
- **Mods:** the APK will build and include the mods listed under `android` in [mods/platforms.json](mods/platforms.json) installing them into the app's `mods/` folder.
- **Debugging:** logs go to logcat under the tag `RS64` (the `Logcat (Android)` task filters it). Environment variables go one `NAME=VALUE` per line in `roguesq_env.txt` in the app's files folder.

### macOS notes

**Compile-only so far.** An arm64 (Apple Silicon) Mach-O builds and links for macOS 11+, but it hasn't been run on a Mac yet. macOS renders through RT64's **Metal** backend.

On a Mac, the Linux / macOS commands above should apply (Xcode Command Line Tools + `brew install cmake ninja sdl2`)

Without a Mac, the game cross-compiles from **WSL2 / Linux** with [osxcross](https://github.com/tpoechtrager/osxcross):

1. Download **Command Line Tools for Xcode 16.4** (`.dmg`) from [developer.apple.com/download/all](https://developer.apple.com/download/all/)
2. Build the toolchain and its Darwin runtime (`libssl-dev cpio libxml2-dev` needed):

   ```sh
   git clone https://github.com/tpoechtrager/osxcross ~/osxcross && cd ~/osxcross
   ./tools/gen_sdk_package_tools_dmg.sh /path/to/Command_Line_Tools_for_Xcode_16.4.dmg
   mv MacOSX15.5.sdk.tar.xz tarballs/
   UNATTENDED=1 SDK_VERSION=15.5 OSX_VERSION_MIN=11.0 ./build.sh
   ./build_compiler_rt.sh   # no need to run the install commands it prints
   ```

3. Build the game with `tools/macos/cross-build.sh [arm64|x86_64] [Debug|Release]`, or the `Build (macOS cross, WSL/osxcross)` VS Code task. The script builds the host tools and SDL2 on first run. Output: `~/rs64-build-macos-<arch>/RogueSquadron64Recomp`.

- **Can't render when cross-built.** Metal shaders need `xcrun metal`, which exists only on macOS, so a cross-build embeds the MSL source (`RT64_METAL_COMPILE=OFF`) instead of compiled metallibs. A build meant to run has to be made on a Mac (or a macOS CI runner).
- **Duplicate symbols:** Apple's `ld64` has no `--allow-multiple-definition`, so the runtime functions this game overrides are declared weak on Apple (`ULTRAMODERN_OVERRIDABLE` in the N64ModernRuntime fork).
- **Not yet done:** an `.app` bundle with SDL2 inside.

---

## The recompiler config (`rogue_squadron.toml`)

`rogue_squadron.toml` is the N64Recomp config: it names the input ROM/ELF and defines the override layer applied during `regen_funcs`. Three directives shape the generated output without hand-editing it:

| Directive | Effect |
| --- | --- |
| `stubs = [...]` | Replace a function body with an empty no-op (RSP blobs, cache-instruction leaves, splat fragments) |
| `[[patches.instruction]]` | Overwrite one instruction at a `vram` with a raw `value` (e.g. NOP a `cache` op or a busy-wait branch) |
| `[[patches.hook]]` | Inject C at a function's entry or before a `vram` — guards, pacing, logging. Host helpers live in `src/main/hook_helpers.cpp` |

Edit the toml, then re-run `regen_funcs` to apply. For larger game-logic overrides, write MIPS-side C in the [`patches/`](patches/README.md) build instead — see Patching below.

---

## Running

Put `rogue_squadron.z64` next to the executable and launch it. The ROM hash is checked at startup.

### Controls

Keyboard, mouse, gamepad, and flight sticks / HOTAS all work; no controller is required. The keyboard and joystick defaults follow the PC version (*Rogue Squadron 3D*). Actions are for the game's default **Luke** controller setting. The other controller presets in Options rearrange them.

<div align="center">

| Action | Keyboard | N64 | Gamepad | Joystick / H.O.T.A.S. |
| --- | --- | --- | --- | --- |
| Steer | <kbd>↑</kbd>,<kbd>↓</kbd>,<kbd>←</kbd>,<kbd>→</kbd> (<kbd>A</kbd>/<kbd>D</kbd> turns) | Analog stick | Left stick | Stick X/Y |
| Fire blasters | <kbd>Space</kbd> | B | X | Button 1 (trigger) |
| Fire secondary | <kbd>Alt</kbd> | C-Left | Back | Button 2 |
| Fire mode | <kbd>X</kbd> | C-Down | B | Button 6 |
| Thrust | <kbd>W</kbd> | A | A | Button 3 |
| Brake | <kbd>S</kbd> | Z | Left trigger | Button 4 |
| Speed (positional) | — | — | — | Throttle lever |
| Roll | <kbd>E</kbd> | R | Right shoulder | Button 7 + twist joystick |
| Special | <kbd>F</kbd> | C-Right | Guide | Button 5 |
| Cockpit | <kbd>F1</kbd> | D-Pad Up | D-Pad | — |
| Standard | <kbd>F2</kbd> | D-Pad  Down | D-Pad | — |
| Close view | <kbd>F3</kbd> | D-Pad Right | D-Pad | — |
| Switch view | <kbd>F4</kbd> | L | Left shoulder | Button 8 |
| In-Game Profiler HUD | <kbd>F5</kbd> | — | — | — |
| Look around | <kbd>Q</kbd> | C-Up | Y | Hat |
| Drop camera | <kbd>Z</kbd> | D-Pad Left | D-Pad | — |
| Menu confirm | <kbd>Enter</kbd> | A | A | Button 3 |
| Back | <kbd>Backspace</kbd> | B | X | Button 1 |
| Pause | <kbd>Esc</kbd> | Start | Start | — |

</div>

**Flight sticks and HOTAS:** any joystick that isn't a gamepad (flight stick, throttle unit, pedals) is picked up automatically, several at once. The first time one is connected it gets the PC version's layout above. A stick with exactly three axes, or a device named "throttle", gets its lever bound to **Throttle**. The throttle sets your speed by position: back is the craft's slowest, forward its fastest, and the **Cruise at** point (middle by default) its normal speed. Other axes such as twist, rudder, or a throttle slider on the stick are bound from the Controls window (**F6**) For the throttle, pull the lever back, click **Rebind** on Throttle, then push it forward. A twist grip or rudder pedals can do one of two jobs, depending on how you bind them. Bind them to **StickLeft/StickRight** and they'll add to `stick X` since inputs on the same stick direction adds together. Bind them to **RollLeft/RollRight** and they'll give a dedicated roll axis: hold the Roll button and twist to roll.

**Rumble:** the game's own Rumble Pak effects (hits, collisions, terrain scrapes, weapons, the death spiral, and crashing) play on gamepads and on joysticks with rumble or force feedback. Hits get stronger with the damage taken, and the rumble keeps going through the death spiral. Turn rumble off, change its strength, or turn off individual effects in the Controls window, or in the `rumble` section of `roguesq_input.json`.

**Mouse flight steering:** mouse capture is automatic while the game window is focused.
Mouse motion steers the craft, left click fires blasters, right click fires the secondary weapon. Capture releases when the window loses focus, the controls window (**F6**) is open, or when RT64's **F1** inspector is up.

In Debug builds **F1/F3/F4** also toggle RT64 developer tools.

**F1** toggles RT64's ImGui overlay (configuration, texture dumping, per-call debugger, render-target view). **F3** toggles ViewRDRAM mode and **F4** toggles texture replacement.

#### Rebinding controls

Press **F6** to open the **Controls** window. Click **Rebind** (replace) or **Add** (keep the existing ones) on any action, then press a key, gamepad button, mouse button, joystick button or hat, or move a joystick axis to assign it. **Clear** removes a binding. The **Joysticks / HOTAS** section shows each connected joystick's axes live, with invert and deadzone settings. The **Rumble** section holds the rumble settings. Adjust mouse sensitivity and invert there too, then **Save** (or **Restore defaults**). Bindings persist to `roguesq_input.json` next to the executable, which you can also hand-edit.

> [!TIP]
> In Debug builds (developer mode on by default) the RT64 inspector owns the ImGui overlay, so press **F1** once before **F6**.
> Release builds open the Controls window with **F6** directly.

---

## Status (PLAYABLE)

> [!NOTE]
> I have personally managed to play it (on windows) all the way through from the first level to the credits sequence.

#### Recomp. specific Issues

- Text and background during the Credit sequence renders incorrectly, with it clipping letters. The background has some slight visual artifacts as well. (Happens after completing the game, NOT when the 'CREDITS' passcode is entered. Meaning that the issue probably stems from the ending cutscene that plays prior to the credits) This issue might've been fixed already it just needs to be verified.

- Some slight graphical glitches in spots like how the final cutscene after completing "Battle of Calamari" will display the edge of the terrain as it renders when the fog should be covering it. (I think on real hardware during this cutscene in particular fog settings change to account for the perspective)

- Frame interpolation *can be enabled* **BUT** it still causes visual glitches if meshes/objects aren't ID'd properly. The performance hitches (frame rate drops) will also cause a frame stutter. It's better than it was but still needs work.

- Low Resolution mode works but affects how cutscenes are displayed with them appearing more wide than they probably should be.

#### Game specific Issues

- Some CPU performance hitching and slow down (~20fps) in spots (although not as bad as the real n64 game) This is expected since the game is very CPU heavy. So anything to optimize CPU rendering or even offload the work onto the GPU would be greatly beneficial and would eliminate a lot of the frame rate issues.

- Cutscenes having screen sizes of varying widths which could genuinely be an issue with the game itself. (Some cutscenes have black bars on the sides or additional padding, but some don't)

- Very slight cut off at the top of text rects BUT this also existed in the original game.

- Terrain tile textures don't align perfectly and seem to have a slight cut off which again is probably a game issue than a recomp. issue.

---

## Architecture

Recompiled game code (`RecompiledFuncs/`, generated) and hand-written overrides (`patches/`, linked first so their symbols win) compile into one static library, linked against forked builds of N64ModernRuntime (the libultra/runtime host) and RT64 (the renderer). `src/main/` wires it together.

| Path | Role |
| --- | --- |
| `src/main/main.cpp` | Entry point — SDL2 window/audio/input, RSP task dispatch |
| `src/main/rt64_render_context.cpp` | RT64 integration — `send_dl`, VI registers, framebuffer sanitizer |
| `src/main/register_overlays.cpp` | Boot-time overlay function-table registration |
| `src/main/upstream_compat.cpp` | libultra shims, overlay loader, HMT-load capture |
| `lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp` | The Factor 5 GBI module (the render core) |
| `patches/` | Game-function overrides, cross-compiled to MIPS |
| `src/rsp/`, `*_rsp.toml` | RSPRecomp configs and the DPC bridge that forwards the Factor 5 ucode's RDP bytes into RT64 |

**Graphics.** Rogue Squadron uses Factor 5's own display-list format, which stock RT64 cannot parse; the forked RT64 carries a GBI module for it. `M_GFXTASK` goes straight to RT64's HLE processor, which emits native RT64 geometry. The grammar is in [docs/f5-model-dl-spec.md](docs/f5-model-dl-spec.md) and validated offline against Project64 dumps with `tools/validate/f5_dl_walk.py`. The recompiled RSP ucode (RSPRecomp) forwards its RDP bytes into RT64 through `dpc_bridge.cpp`.

**Frame pacing.** By default the game's own VI / SP / DP message protocol runs as on hardware, with SP-done delivered after RT64 parses the list. `--no-vi-driven-loop` (`ROGUESQ_VI_DRIVEN_LOOP=0`) restores the older host-paced loop.

**Audio.** MusyX drives SFX and music; samples stream from the cartridge via PI DMA as on hardware.

**Patching.** Overrides live in [`patches/`](patches/) and as `[[patches.hook]]` entries in `rogue_squadron.toml`, never as hand edits to the generated `RecompiledFuncs/`, so regeneration is safe. The pattern follows [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp/tree/dev/patches) but uses `mips64-elf-gcc` instead of clang. See [patches/README.md](patches/README.md).

---

## Debugging

[docs/debugging-with-visual-studio.md](docs/debugging-with-visual-studio.md) covers attaching Visual Studio to the recompiled output and telling a recompile bug from a game-logic bug. Helpers under [tools/](tools/):

- `dump-game.ps1` writes a full-memory minidump of a running instance, even when the window is unresponsive. F12 in-game does the same.
- `inspect-dump.py` and `dump_stackscan.py` list and symbolize threads from a minidump.
- `run-stability.ps1` launches N timed runs and classifies each by stderr markers.
- `validate/` captures Project64 goldens, diffs RDRAM, compares message-order traces, and walks display lists offline.

A watchdog thread writes `mqdiag_NNN.txt` message-queue snapshots every 3 seconds.

### Command-line options

Run `RogueSquadron64Recomp.exe --help` for the full list. The common options:

| Option | Effect |
| --- | --- |
| `--gfx-api <vulkan\|d3d12>` | Force the graphics API (default auto) |
| `--[no-]hle-dev-mode` | RT64 ImGui inspector on F1 (default on in Debug, off in Release) |
| `--no-vi-driven-loop` | Old host-paced frame loop instead of the hardware protocol (default is VI-driven) |
| `--no-f5-native` | Parse F5 display lists without emitting geometry |
| `--no-audio-ucode` | Silent audio stub instead of the MusyX synth |
| `-m` / `--mute`, `--audio-gain <f>` | Silence output, or scale master gain |
| `--audio-latency-ms <n>` | Audio buffer latency in milliseconds |
| `--dump-pcm <path>` | Write the synth output to a 22050 Hz stereo WAV |
| `--render-song <key>` | Force a specific song (0 = the N64-logo music) |
| `--fake-controller`, `--auto-start <ms>` | Headless runs: fake a controller, pulse START |
| `--set NAME=VALUE` | Set any `ROGUESQ_*` variable directly |

Each option maps to a `ROGUESQ_*` environment variable. The full debug/trace/experiment catalog, logging categories, DL/texture dumps, message-order traces, and rendering A/B toggles lives in [docs/debug-trace-env-vars.md](docs/debug-trace-env-vars.md); reach any of those from the command line with `--set NAME=VALUE`.

**F5** toggles Factor 5's own built-in frame-profiler HUD (a dormant retail feature, gated by one RDRAM byte).

<img alt="Factor5's built-in profiler" src="./docs/ProfilerBars.PNG">

- **Yellow Bar**  = CPU; it grows toward full width as a scene exceeds 'frame budget'.
- **Blue bar**    = render/geometry cost (RSP + display-list processing)
- **Red bar**     = rasterization/fill cost (total RDP)
- **Magenta Bar** = RDP* cmd-buffer busy (DPC_BUFBUSY) -> draw-call count (per State::flush)
- **White Bar**   = RDP* pipe busy (DPC_PIPEBUSY)      -> drawCall.triangleCount
- **Green Bar**   = RDP* TMEM busy (DPC_TMEM)          -> drawCall.loadCount
- **Cyan Bar**    = Not used

*The recomp re-uses the bars by basing them on the draw calls from rt64 while also scaling them with `ROGUESQ_DRAW_SCALE` / `ROGUESQ_TRIS_SCALE` / `ROGUESQ_TEX_SCALE`. `ROGUESQ_PROFILER_DUMP=1` can log the raw slot values for debugging purposes.

**(It's an approximation and is not representative of how DPC performs on *actual hardware*)**

### Local save editor tool

A save editor also exists in *`tools/save-editor`* as an HTML page. Allowing a user to easily create, edit and export a save into any of the following formats: .*`.bin, .eep, .srm, .sra and .raw`*

---

## 'Proper' AI Agent Usage

Using an AI Agent properly comes down to handing it the *right set of tools*, *the right resources/knowledge to perform the work* and *instructions*. **AGENTS.MD** provides the baseline instructions for your agent to make changes to this project. In addition to that, **skill** files *(just like AGENTS.MD)* are use to provide instructions for things like tools, specific task and/or processes. **MCP** servers/tools can further augment existing application by giving handles/methods for agents to *better* perform tasks.

This repo already provides **AGENTS.MD** and **skill** files. **MCP** servers are configured and setup by the user themselves and is something we can't force/mandate. Beyond MCP are things called **harnesses** which can be a tool to perform tasks or orchestrate agents to perform a set of tasks at once. This repo contains some harness in `tools` to performs test, perform multiple runs to verify robustness or drive an agent to particular menu to chase a bug.

Once your agent is setup, tasks that would've taken weeks/months/years to do can be done in a single day/week. (This reason alone is why trillions are being spent in this sector)

Here's a list of MCPs that could be useful for this project:

- [renderdoc](https://github.com/Linkingooo/renderdoc-mcp) (Requires [RenderDoc source code](https://github.com/baldurk/renderdoc) and [python 3.10+](https://www.python.org/downloads/) to compile the `renderdoc.pyd` that this MCP needs)
- [windows-screenshot-mcp-server](https://github.com/MikeSemicolonD/windows-screenshot-mcp-server) (Requires [go 1.25.2](https://go.dev/dl/))

> [!CAUTION]
> `windows-screenshot-mcp-server` usage can be finicky because Claude (as of September 2026) has no visual capabilities, meaning it can miss things that are visually obvious to a human like visual glitches/artifacts. Claude can *at most* look at the pixel values in the image to figure out what it's looking at. (Technically not vision capable but surprisingly good enough to be dangerous)

---

## Tooling

[RenderDoc](https://renderdoc.org/) to debug graphics/rendering issues.

[rizin](https://rizin.re/) to assist in validating/checking the game's assembly.

[pj64](https://www.pj64-emu.com/nightly-builds) to pull memory dumps for checks and comparisons. (Development builds are recommended since they provide more ways to debug/validate *even though it performs slower*)

## Acknowledgements

- **[Dávid Pethes](https://github.com/dpethes/rerogue)**: the rerogue tools and the [satd.sk write-up](https://satd.sk/pages/rs/) documenting the PC build's HOB, HMT, HMP, and MORT formats, which the N64 build shares.
- **[jrra](https://github.com/jrra/rerogue)**: a community fork of rerogue.
- **[Tmcg2](https://github.com/Tmcg2/rogue_squadron64)**: started the companion decomp project.

## License

See [LICENSE](LICENSE). This is a hobby project.

This project is in **no way** associated with, sponsored or endorsed by Nintendo, Disney, LucasArts (now known as Lucasfilm Games LLC), "Factor 5, Inc."/"Factor5 GmbH" or "Eggebrecht, Engel, Schmidt GbR".

This project contains no ROM data and requires a legally obtained copy of the game.
