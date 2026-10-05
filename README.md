<div align="center">
  <img alt="favicon made by thedoctor45" src="https://github.com/MikeSemicolonD/RogueSquadron64Recomp/blob/main/favicon.ico">

  Icon by [thedoctor45 on DeviantArt](https://www.deviantart.com/thedoctor45/art/Star-Wars-Rogue-Squadron-3D-Custom-Icon-535469296)

# Star Wars: Rogue Squadron 64 Recompiled

</div>

A static recompilation of **Star Wars: Rogue Squadron** (N64, USA v1.0) built with [N64Recomp](https://github.com/N64Recomp/N64Recomp) and [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime), rendering through a forked [RT64](https://github.com/MikeSemicolonD/rt64) that understands Factor 5's custom display-list format.

> [!IMPORTANT]
> **Work in progress & Heavily AI-assisted.**
> Most of the debugging, architectural decisions, and code here (the F3DFACTOR5 GBI module, the runtime patches inside `lib/`, `src/main/`, the recompiler hooks, the diagnostic env vars) were produced with Claude.

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

## Features

#### Graphics

- **High resolutions:** render at any resolution your GPU can handle, with optional MSAA anti-aliasing and supersampling.
- **Widescreen:** the 3D world, sky, and terrain extend to fill the screen. The game no longer hides objects just outside the old 4:3 frame.
- **Longer draw distance:** see terrain and objects farther away (up to 2.5x the original), set with `drawDistance` in `roguesq_video.json`. Has some some caveats; see [Status](#status-playable).
- **High frame rates (experimental):** frame interpolation smooths the game's native ~30 fps up to your monitor's refresh rate. It still has some visual glitches; see [Status](#status-playable).
- **HDR, texture filtering, and upscale options**, all set in `roguesq_video.json`.
- **Texture packs:** RT64 texture-replacement packs load at startup.
- **Vulkan or Direct3D 12:** pick the graphics API that runs best on your PC.

#### Input

- **Keyboard and mouse:** fly with the mouse and fire with the mouse buttons.
- **Gamepads:** any SDL-supported controller works, with gamepad lightbar support.
- **Flight sticks and H.O.T.A.S.:** several devices at once, with a throttle lever that sets your speed by position, and twist or rudder roll.
- **Rumble:** the game's own Rumble Pak effects play on gamepads and force-feedback joysticks. Fully adjustable in `roguesq_input.json`.
- **Full rebinding:** press **F6** in game to rebind any action and change sensitivity, deadzones, and rumble. See [Controls](#controls).
- **Touch controls on Android:** tap menus directly and fly with an on-screen stick. You can move and resize the buttons, and turn on gyro (tilt) steering.

#### Multiplayer (experimental)

- **Online two-player co-op:** host or join from a **MULTIPLAYER** page on the main menu. Your partner flies alongside you as a wingman, and you share objectives, lives, and the mission result. Works over LAN (with automatic discovery) and over the internet (with UPnP port forwarding). Uses a fork of [ENet](https://github.com/lsalzman/enet) called [ENet6](https://github.com/SirLynix/enet6) and [miniupnp](https://github.com/miniupnp/miniupnp). Desktop releases ship co-op as the **Multiplayer** mod (`multiplayer-native`, on by default); it must stay enabled in the **Mods** panel. See [docs/multiplayer.md](docs/multiplayer.md).
- **Co-op over Steam (Steam Relay mod, on by default):** with Steam running, HOST GAME shows a 6-digit join code and the other player enters it under JOIN GAME; Steam's relay network connects you with no port forwarding. Steam friends can also join from the Steam friends list while the game is running. Steam sees the game as Spacewar (app id 480). **The Steam API library is not included:** download the Steamworks SDK (version 1.65, from [partner.steamgames.com](https://partner.steamgames.com/downloads/list), any Steam login) and copy `redistributable_bin/win64/steam_api64.dll` (Windows), `redistributable_bin/linux64/libsteam_api.so` (Linux) or `redistributable_bin/osx/libsteam_api.dylib` (macOS) into `mods/steam-relay/` next to the game. Without it, or with Steam closed, the MULTIPLAYER page says why and co-op uses direct connections. Linux needs the native Steam package (Flatpak and Snap Steam are not supported); on Steam Deck, add the Linux build as a non-Steam game; on macOS, remove the download quarantine with `xattr -d com.apple.quarantine mods/steam-relay/libsteam_api.dylib`. Steam on Linux, Steam Deck and macOS is untested.

#### Mods

- **Mod support:** turn mods on and off in the **Mods** panel of the **F6** window.
- **Included mods (desktop):**
  - **Invincibility:** your craft takes no damage.
  - **Larger Object Pool:** doubles the number of ships, lasers, and explosions the game can track at once, so busy missions stop resetting objects.
  - **Long Tow Cable:** the snowspeeder's tow cable is long enough to wrap an AT-AT several times.
  - **Fullscreen toggle** and a **quit confirmation** in the game's settings menus.
  - **Multiplayer:** online co-op (see above).
- **More mods in [mods/](mods/):** Infinite Secondary (weapons never run out) and Any Craft, Any Mission (fly any ship on any level). Each build ships what [mods/platforms.json](mods/platforms.json) lists for it. See [docs/modding-host-api.md](docs/modding-host-api.md) and [docs/adding-menus-and-buttons.md](docs/adding-menus-and-buttons.md).

---

## Requirements

| Requirement | Notes |
| --- | --- |
| **ROM** | `rogue_squadron.z64`, USA v1.0 (16 MB, xxHash3-64 `0x6B66A44153594DEA`) |
| **OS / GPU** | Windows 10+, Linux, or macOS 11+ with a D3D12, Vulkan, or Metal capable GPU; Android 8+ arm64 with Vulkan 1.1 (see [Android notes](#android-notes)); macOS is compile-only so far (see [macOS notes](#macos-notes)) |
| **CMake** | 3.20+ |
| **Compiler** | MSVC with the ClangCL toolset (Windows), Clang or GCC (Linux/macOS) |
| **N64Recomp output** | `RecompiledFuncs/`, generated locally from the companion [rogue_squadron64](https://github.com/MikeSemicolonD/rogue_squadron64) decomp (started by [Tmcg2](https://github.com/Tmcg2/rogue_squadron64)), plus the RSPRecomp'd audio microcode; see [Building](#building) |
| **MIPS cross-compiler** *(optional)* | `mips64-elf-gcc`, only for building `.nrm` code mods (`tools/mods/`); the game itself does not need it. Windows builds are at [n64-tools](https://github.com/n64-tools/gcc-toolchain-mips64/releases); the official LLVM Windows installers lack the MIPS backend. Default path `E:/mips-toolchain`. |

---

## Building

### 1. Clone with submodules

```sh
git clone --recurse-submodules https://github.com/MikeSemicolonD/RogueSquadron64Recomp.git
cd RogueSquadron64Recomp
```

`lib/` holds forks of [N64ModernRuntime](https://github.com/MikeSemicolonD/N64ModernRuntime) and [rt64](https://github.com/MikeSemicolonD/rt64), forked because the upstream libraries don't support Factor 5's custom microcode, plus [enet6](https://github.com/SirLynix/enet6) and [miniupnp](https://github.com/miniupnp/miniupnp) for co-op. All four are required.

### 2. Produce the decomp ELF

The recompiler needs the ELF from the companion [rogue_squadron64](https://github.com/MikeSemicolonD/rogue_squadron64) decomp, cloned next to this repo (`rogue_squadron.toml` reads `../rogue_squadron64/build/roguesquadron.elf`):

```sh
# In the rogue_squadron64 repo:
splat split roguesquadron.yaml
python tools/make_elf.py
```

### 3. Generate the recompiled C

The game's CMake project needs `RecompiledFuncs/` and the recompiled audio microcode before it configures, so build N64Recomp and RSPRecomp as their own project first (the release CI does the same). RSPRecomp reads the microcode from the ROM, so put `rogue_squadron.z64` in the repo root.

```sh
cmake -S lib/N64ModernRuntime/N64Recomp -B build-tools -DCMAKE_BUILD_TYPE=Release
cmake --build build-tools --config Release --target N64RecompCLI RSPRecomp

# From the repo root. Visual Studio puts the tools in build-tools/Release/, Ninja/Make in build-tools/.
build-tools/Release/N64Recomp rogue_squadron.toml          # RecompiledFuncs/
python tools/coop/gen_relocation.py --verify              # fails if the co-op relocation patches were not applied
build-tools/Release/RSPRecomp rsp/factor5_boot_rsp.toml    # build/factor5_ucode/factor5_boot_recompiled.c
build-tools/Release/RSPRecomp rsp/musyx_rsp.toml           # build/factor5_ucode/musyx_audio_recompiled.c
```

Configuring the game runs `tools/fixup_factor5_ucode.py` on both microcode files. Re-run RSPRecomp only when a config under `rsp/` changes.

### 4. Configure and build

```sh
# Windows (Visual Studio + ClangCL). Debug is the tested configuration.
cmake -B build -T ClangCL -DN64RECOMP_EXE=build-tools/Release/N64Recomp.exe
cmake --build build --config Debug

# Linux / macOS
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DN64RECOMP_EXE=build-tools/N64Recomp
cmake --build build
```

After that, the `regen_funcs` target (`cmake --build build --config Debug --target regen_funcs`) regenerates `RecompiledFuncs/` and runs the relocation check. Re-run it when `rogue_squadron.toml`, the symbols, or the decomp ELF changes.

| CMake option | Default | Purpose |
| --- | --- | --- |
| `N64RECOMP_EXE=path` | `N64Recomp` in the build dir (`Debug/` for Visual Studio) | Recompiler used by `regen_funcs` and the patches build |
| `RS64_MULTIPLAYER=OFF` | ON | Leave online co-op out of the exe; it then comes from the `multiplayer-native` mod. Desktop releases build OFF; Android forces ON |
| `ROGUESQ_NO_ITER_DEBUG=ON` | OFF | Disable MSVC debug iterators across the whole build for faster Debug runs (Windows) |

Debug builds compile in the D3D12 debug layer; turn it on at runtime with `ROGUESQ_D3D12_DEBUG=1` (`ROGUESQ_D3D12_GPUVAL=1` adds GPU-based validation).

### Linux / WSL notes

Builds and can run on **Ubuntu 24.04 under WSL2** (GCC 13 / Ninja); native Linux should behave the same with a real Vulkan driver. Non-Windows targets render through **Vulkan**. (D3D12 is Windows-only)

Install the dependencies, generate the recompiled C (step 3), then configure with Ninja and build the game target:

```sh
sudo apt install build-essential cmake ninja-build libsdl2-dev libvulkan-dev libgtk-3-dev python3
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DN64RECOMP_EXE=build-tools/N64Recomp
cmake --build build --target RogueSquadron64Recomp
```

`gcc-mips-linux-gnu binutils-mips-linux-gnu` are only needed to build `.nrm` code mods, as in the Linux release CI.

#### About WSL2

- Keep the build directory on the Linux filesystem (e.g. `~/rs64-build`), **not** under `/mnt/…` — CMake's compiler checks can fail with `Operation not permitted` on the Windows drive mount. Source can stay on `/mnt`.
- WSLg supplies the display, PulseAudio (sound), and keyboard/mouse, so the game runs directly — no extra setup. A physical gamepad needs `usbipd-win`; the keyboard works out of the box.
- WSL's default Vulkan is **llvmpipe (software)** — it runs but hitches. For GPU acceleration, build [Mesa's](https://gitlab.freedesktop.org/mesa/mesa) **[Dozen (`dzn`)](https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/microsoft/vulkan?ref_type=heads)** driver (Vulkan → D3D12 → the real GPU via `/dev/dxg`). If the game doesn't detect `dzn` it'll default to software (CPU) rendering.
- The `tools/run-wsl-gpu.sh` helper and the `Configure` / `Build` / `Run (Linux/WSL, GPU via dzn)` VS Code tasks wrap this. (`dzn` has no ray-tracing extensions — irrelevant to the current raster path.)

### Android notes

Builds as an arm64 APK and runs on a **(Snapdragon 8 Gen 3 / Adreno 750, Android 16)**; other arm64 phones with Vulkan 1.1 should work, but driver quality varies. The Gradle project in `android/` builds the same `CMakeLists.txt` with the NDK.

Needs **Android Studio** (for its SDK, bundled JDK and `adb`) with **NDK 29.0.14206865**; Gradle fetches SDL 2.32.10 and installs its CMake itself. Run the Windows Debug build first: the Android build reuses the generated `RecompiledFuncs/`, `src/main/state_table.inl` and `build/factor5_ucode/*.c`, and CMake stops with an error if any is missing.

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

3. Build the game with `tools/macos/cross-build.sh [arm64|x86_64] [Debug|Release]`, or the `Build (macOS cross, WSL/osxcross)` VS Code task. The script builds the host tools and SDL2 on first run. Output: `~/rs64-build-macos-<arch>/RogueSquadron64Recomp` for Debug, `~/rs64-build-macos-<arch>-<config>/` (lowercase, e.g. `-release`) otherwise.

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

Edit the toml, then re-run `regen_funcs` to apply. For larger game-logic overrides, put the logic in a host function in `src/main/hook_helpers.cpp` and call it from the hook; see Patching below.

---

## Running

Put `rogue_squadron.z64` in the working directory (the executable's folder when you double-click it) and launch. The ROM hash is checked and the ROM imported on first launch.

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

- Some 'highlights' from the lights or explosions when you destroy the AT-AT will sometimes show the wrong color.

- Forcing a ship selection won't respect it's secondary (for example selecting a y-wing but then selecting an A-wing in the hangar)

- {Multiplayer} The other player's tow cable isn't drawn: an AT-AT they trip falls in both games, but only they see the cable and the trip camera.

- A very high draw distance causes the terrain renderer to render more tiles which degrades performance *severely*. `1.3` is a good distance but at `2.0` or more the performance will suffer. At that distance you'll noticed structure and NPCs popping into existance at about the `1.5` mark. Increasing the distance for structures and NPCs to pop in will affect how AI behaves and how events trigger.

- Frame interpolation *can be enabled* **BUT** it still causes visual glitches if meshes/objects aren't ID'd properly. The performance hitches (frame rate drops) will also cause a frame stutter.

- Low Resolution mode works but affects how cutscenes are displayed with them appearing more wide than they probably should be.

#### Game specific Issues

- Some CPU performance hitching and slow down (~20fps) in spots. (although not as bad as the real n64 game) This is expected since the game is very CPU heavy with a lot of systems running off of counters based on a variable timestep.

- Cutscenes having screen sizes of varying widths which could genuinely be an issue with the game itself. (Some cutscenes have black bars on the sides or additional padding, but some don't)

- Very slight cut off at the top of text rects **BUT** this also existed in the original game.

- Terrain tile textures don't align perfectly and seem to have a slight cut off which is probably a game issue than a recomp. issue.

---

## Architecture

Recompiled game code (`RecompiledFuncs/`, generated, with the toml's hooks compiled in) compiles into one static library, linked against forked builds of N64ModernRuntime (the libultra/runtime host) and RT64 (the renderer). `src/main/` wires it together.

| Path | Role |
| --- | --- |
| `src/main/main.cpp` | Entry point — SDL2 window/audio/input, RSP task dispatch |
| `src/main/rt64_render_context.cpp` | RT64 integration — `send_dl`, VI registers, framebuffer sanitizer |
| `src/main/register_overlays.cpp` | Boot-time overlay function-table registration |
| `src/main/upstream_compat.cpp` | libultra shims, overlay loader, HMT-load capture |
| `lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp` | The Factor 5 GBI module (the render core) |
| `rsp/*_rsp.toml` | RSPRecomp configs for the boot and MusyX audio ucodes |
| `include/rs64/host_api.h`, `src/main/host_api*.cpp`, `builtin_hooks.cpp` | Host API for native mods ([docs/modding-host-api.md](docs/modding-host-api.md)) |
| `src/main/ghost*.cpp`, `net_core.cpp`, `net_link.cpp`, `mp_register.cpp`, `mods/multiplayer-native/` | Online co-op ([docs/multiplayer.md](docs/multiplayer.md)) |
| `src/main/lockstep*.cpp` | Mission input record/replay with per-frame state hashes |

**Graphics.** Rogue Squadron uses Factor 5's own display-list format, which stock RT64 cannot parse; the forked RT64 carries a GBI module for it. `M_GFXTASK` goes straight to RT64's HLE processor, which emits native RT64 geometry. The grammar is in [docs/f5-model-dl-spec.md](docs/f5-model-dl-spec.md) and validated offline against Project64 dumps with `tools/validate/f5_dl_walk.py`.

**Frame pacing.** By default the game's own VI / SP / DP message protocol runs as on hardware, with SP-done delivered after RT64 parses the list. `--no-vi-driven-loop` (`ROGUESQ_VI_DRIVEN_LOOP=0`) restores the older host-paced loop.

**Audio.** MusyX drives SFX and music; samples stream from the cartridge via PI DMA as on hardware.

**Patching.** Overrides live as `[[patches.hook]]` and `[[patches.instruction]]` entries in `rogue_squadron.toml`, with host logic in `src/main/hook_helpers.cpp`, never as hand edits to the generated `RecompiledFuncs/`, so regeneration is safe. A hook is compiled into the recompiled function itself, so it covers indirect (`LOOKUP_FUNC`) calls too.

---

## Debugging

[docs/debugging-with-visual-studio.md](docs/debugging-with-visual-studio.md) covers attaching Visual Studio to the recompiled output and telling a recompile bug from a game-logic bug. Helpers under [tools/](tools/):

- `dump-game.ps1` writes a full-memory minidump of a running instance, even when the window is unresponsive. F12 in-game does the same.
- `inspect-dump.py` and `dump_stackscan.py` list and symbolize threads from a minidump.
- `reconstruct-freeze.py` rebuilds blocked game threads and queues from a hang dump; `host-stacks.py` symbolizes host stacks (Debug build).
- `run-stability.ps1` launches N timed runs and classifies each by stderr markers.
- `recordings/` holds per-mission input recordings; `run-replays.ps1` replays them against their state-hash baselines ([README](tools/recordings/README.md)).
- `validate/` captures Project64 goldens, diffs RDRAM, compares message-order traces, and walks display lists offline.

`cmake --build build --config Release --target rs64_unit_tests` builds and runs the unit tests and the style check.

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
| `--dump-pcm <path>` | Write the synth output to a 22050 Hz stereo WAV (`1` = `dumps/wav/capture.wav`) |
| `--fake-controller`, `--auto-start <ms>` | Headless runs: fake a controller, pulse START |
| `--boot-target <target>` | Skip the intro to `menu`, `demo:N`, `level:N[,craft]`, `abort:N`, `cutscene:N` or `lobby:host\|join[,level]` |
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

## ~~License~~

This is a hobby project.

This project is in **no way** associated with, sponsored or endorsed by Nintendo, Disney, LucasArts (now known as Lucasfilm Games LLC), "Factor 5, Inc."/"Factor5 GmbH" or "Eggebrecht, Engel, Schmidt GbR".

This project contains no ROM data and requires a legally obtained copy of the game.
