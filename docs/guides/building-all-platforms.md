# Building every platform

What each release target needs installed, and how it is built. Two hosts cover everything: **Windows** (Windows builds, Android) and **Ubuntu 24.04**, native or WSL2 (every Linux build). `tools/release/bundle-release.ps1` (VS Code task **Package: Release bundle**) drives both from Windows and writes the packages to `build/packages/<version>/` (the VS Code task asks for a folder, default your Desktop).

All targets compile the same generated game code, so every host needs the shared inputs first.

## Targets

| Package | Host | Script / task | Status |
| --- | --- | --- | --- |
| `RogueSquadron64Recomp-windows-x64-<ver>.zip` | Windows | `bundle-release.ps1 -Only windows-x64` | Release CI |
| `RogueSquadron64Recomp-windows-x86-<ver>.zip` | Windows | `bundle-release.ps1 -Only windows-x86` | Release CI; replay-verified |
| `RogueSquadron64Recomp-android-arm64-<ver>.apk` | Windows | `bundle-release.ps1 -Only android` | Device-tested |
| `RogueSquadron64Recomp-linux-x64-<ver>.tar.xz` + `.AppImage` + `.flatpak` | Ubuntu | `bundle-release.ps1 -Only linux-x64` | Release CI (tarball); Flatpak boots to the menus |
| `RogueSquadron64Recomp-linux-arm64-<ver>.tar.xz` + `.AppImage` + `.flatpak` | Ubuntu | `tools/release/linux/cross-build.sh arm64` | Runs on a Raspberry Pi 4 (Ubuntu 26.04) |
| `RogueSquadron64Recomp-linux-armhf-<ver>.tar.xz` + `.AppImage` | Ubuntu | `tools/release/linux/cross-build.sh armhf` | Runs on a Raspberry Pi 4 (32-bit libraries on Ubuntu 26.04 arm64) |
| `RogueSquadron64Recomp-linux-i386-<ver>.tar.xz` + `.AppImage` | Ubuntu | `tools/release/linux/cross-build.sh i386` | Runs to the menus on an x64 PC (WSL, 32-bit libraries, software Vulkan); untested with a GPU driver |

Windows ARM is not built: Windows on ARM runs the x64 build under emulation. Flatpak only exists for x86_64 and aarch64 (the Flathub runtimes have no 32-bit builds).

`LiveRecompDiffTest` (in the N64Recomp tree) runs every instruction LiveRecomp handles with random inputs and prints a hash per case; build it per architecture (under qemu for the cross targets) and diff the outputs against x64. x86 and i386 match x64 exactly; arm64 and armhf match each other and differ from x86 only in `sqrt` and `trunc.w` of NaN or out-of-range values, which is ARM's float hardware.

## Shared inputs (every host)

| What | Why |
| --- | --- |
| Git with submodules (`git clone --recurse-submodules`) | `lib/` forks, `syms/` |
| `rogue_squadron.z64` (USA v1.0) and the [rogue_squadron64](https://github.com/MikeSemicolonD/rogue_squadron64) decomp next to the repo | `regen_funcs` reads the decomp ELF; the generated `RecompiledFuncs/` is never committed |
| Python 3.11+ | Producing the decomp ELF, `ci/stage_inputs.py`, validation tools (not used by the build itself) |
| A MIPS GCC: `mips64-elf-gcc` on Windows ([gcc-toolchain-mips64](https://github.com/n64-tools/gcc-toolchain-mips64/releases)), `gcc-mips-linux-gnu` on Ubuntu | Patches ELF (`patches/`) and `.nrm` code mods |
| Steamworks SDK in `lib/steamworks/sdk` (optional) | The `steam-relay` mod; skipped with a warning without it |

## Windows host

| Install | Used by |
| --- | --- |
| Visual Studio 2022 with **Desktop development with C++**, **C++ Clang Compiler for Windows** and **MSBuild support for LLVM (clang-cl)** | Windows x64 and x86 (`-T ClangCL`) |
| CMake 3.20+ | All |
| Android Studio (SDK, bundled JDK 17 or newer, `adb`) with **NDK 29.0.14206865**; compile/target SDK 34, min SDK 28 | Android. Gradle 9.6 (Android Gradle Plugin 9.4.1) fetches SDL 2.32.10 and its own CMake |
| WSL2 with Ubuntu-24.04 | Linux targets from `bundle-release.ps1` |

Downloaded at configure time (no install): SDL2 2.30.3 VC zip (x64 and x86 libs), and for x86 Microsoft's DXC v1.8.2403.2 zip (the in-tree `dxc-bin` has no x86 `dxcompiler.dll`).

D3D11 shader cache: Windows packages carry `d3d11_shader_cache/`, built by `RogueSquadron64Recomp.exe --precompile-d3d11-shaders tools/d3d11/shader_combos.txt` (no ROM; `PLUME_D3D11_SHADER_CACHE` sets the output folder; WARP is used without a GPU). It compiles the list twice into the same folder, for feature level 11 (shader model 5) and feature level 10 (shader model 4, plus the raw-TMEM variant of each textured combination), about 5 minutes and 4 MB. Release CI and `tools/release/bundle-release.ps1` run it. The list was collected by playing the mission recordings, the other levels, the demos and the cutscenes on D3D11 and D3D10; to extend it, merge new lines from a play session's `d3d11_shader_cache/rt64_combos.txt`.

Configure once per architecture: `cmake -B build -T ClangCL` (x64), `cmake -B build-x86 -A Win32 -T ClangCL` (x86). Android reuses `RecompiledFuncs/`, `src/game/state_table.inl` and `build/factor5_ucode/*.c` from a Windows Debug build.

## Ubuntu 24.04 host (native or WSL2)

### Linux x64

```sh
sudo apt install build-essential cmake ninja-build git curl libsdl2-dev libvulkan-dev libgtk-3-dev gcc-mips-linux-gnu binutils-mips-linux-gnu
```

Keep build trees on the Linux filesystem (`~/rs64-build`, `~/rs64-build-release`), not under `/mnt`. The Debug tree also provides the host `N64Recomp` and `RecompModTool` that the cross builds below reuse.

### Linux arm64 (cross)

Adds arm64 packages from `ports.ubuntu.com` beside the amd64 ones (about 300 packages, mostly arm64 copies of GTK, SDL and Mesa):

```sh
sudo dpkg --add-architecture arm64
sudo sed -i 's/^Types: deb$/Types: deb\nArchitectures: amd64/' /etc/apt/sources.list.d/ubuntu.sources
sudo tee /etc/apt/sources.list.d/arm64-ports.sources > /dev/null <<'EOF'
Types: deb
URIs: http://ports.ubuntu.com/ubuntu-ports
Suites: noble noble-updates noble-security
Components: main universe
Architectures: arm64
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
EOF
sudo apt update
sudo apt -o Dpkg::Options::="--force-overwrite" install g++-aarch64-linux-gnu binutils-aarch64-linux-gnu qemu-user \
    libsdl2-dev:arm64 libvulkan-dev:arm64 libgtk-3-dev:arm64
```

- `--force-overwrite`: Ubuntu's `libpango1.0-dev` ships an arch-specific `Pango-1.0.gir`, so the amd64 and arm64 copies clash. The build never reads it.
- The installs bring 30-odd amd64 updates with them (systemd, udev): multiarch keeps every architecture's copy of a library at the same version.
- `qemu-user` runs the arm64 build-time tools (state table, ucode fixup); `tools/release/linux/cross-build.sh` builds a host `file_to_c` into `~/rs64-host-tools` on first use.
- Undo: delete `arm64-ports.sources`, drop the `Architectures: amd64` lines, `sudo apt remove '*:arm64'`.

### Linux armhf and i386 (cross)

After the arm64 setup, armhf comes from the same ports mirror and i386 from the regular Ubuntu mirror (Ubuntu still publishes i386 libraries for Steam and Wine):

```sh
sudo dpkg --add-architecture armhf
sudo dpkg --add-architecture i386
sudo sed -i 's/^Architectures: amd64$/Architectures: amd64 i386/' /etc/apt/sources.list.d/ubuntu.sources
sudo sed -i 's/^Architectures: arm64$/Architectures: arm64 armhf/' /etc/apt/sources.list.d/arm64-ports.sources
sudo apt update
sudo apt -o Dpkg::Options::="--force-overwrite" install \
    g++-arm-linux-gnueabihf binutils-arm-linux-gnueabihf libsdl2-dev:armhf libvulkan-dev:armhf libgtk-3-dev:armhf \
    g++-i686-linux-gnu binutils-i686-linux-gnu libsdl2-dev:i386 libvulkan-dev:i386 libgtk-3-dev:i386
```

- armhf builds in ARM mode (not Thumb) with NEON (`tools/release/linux/arm-linux-gnueabihf.cmake`): librecomp's patch trampolines need plain function addresses, and the RSP vector code goes through sse2neon. That needs ARMv7 with NEON, i.e. a Raspberry Pi 2 or newer.
- i386 builds with `-msse4.1 -mfpmath=sse` (`tools/release/linux/i686-linux-gnu.cmake`): x87 math would round differently from every other build.

### AppImage

Nothing to install on the build host: `tools/release/linux/make-appimage.sh` downloads appimagetool 1.9.1 and the type-2 runtime 20251108 into `~/.cache/rs64-appimage`. On WSL it runs them with `APPIMAGE_EXTRACT_AND_RUN=1` (no FUSE). Players need `libfuse2t64` (or the same variable) to run it.

### Flatpak

```sh
sudo apt install flatpak flatpak-builder
```

`tools/release/linux/make-flatpak.sh` adds Flathub for the current user and installs `org.freedesktop.Platform` and `org.freedesktop.Sdk` 26.08 for the target architecture on first use, a few hundred MB each. aarch64 bundles also need `sudo apt install qemu-user-static`: its emulator registrations carry the `F` (fix-binary) flag, without which the arm64 SDK's `/bin/sh` can't start inside Flatpak's sandbox (`bwrap: execvp /bin/sh: No such file or directory`).

## What players need

| Platform | Requirement |
| --- | --- |
| Windows x64 / x86 | Windows 10+, SSE4.1 CPU, GPU with a D3D12, Vulkan or D3D11 driver (x86: 32-bit driver) |
| Linux x64 / arm64 | glibc 2.38+ (Ubuntu 24.04, Raspberry Pi OS Trixie; not Bookworm), a Vulkan driver (Mesa `v3dv` on a Raspberry Pi) |
| Linux armhf | As above, on a 32-bit OS; ARMv7 with NEON (Raspberry Pi 2 or newer) |
| Linux i386 | As above; SSE4.1 CPU |
| Flatpak | Flatpak with Flathub; the runtime installs with the bundle |
| Android | Android 9+, arm64, Vulkan 1.1 (expected on phones launched with Android 10; optional on Android 9, untested there) |

The Linux tarball and AppImage bundle the game's own libraries (SDL2, GTK and their dependencies) but take glibc, `libstdc++` and everything GPU drivers load into the process (Vulkan, Wayland, X11/xcb, `libdrm`, `libz`, fonts) from the host: a bundled older copy would shadow the host's and the drivers would fail to load. The Flatpak also drops every library the runtime provides.

The AppImage keeps saves, configs and mods in `~/.local/share/RogueSquadron64Recomp` (`RS64_DATA_DIR` overrides) and links a `rogue_squadron.z64` placed next to the AppImage on first launch. The Flatpak (`io.github.MikeSemicolonD.RogueSquadron64Recomp`) uses `~/.var/app/io.github.MikeSemicolonD.RogueSquadron64Recomp/data`; copy the ROM there before the first launch.

**Raspberry Pi** (and other slow Linux GPUs): on a Pi the game starts at the N64's native resolution and with `"framebufferWriteback": false` in `roguesq_video.json`, which skips copying each rendered frame back to the emulated RAM (the game never reads it) and raises the frame rate there by about half. Setting it to `false` can help any GPU-limited machine. Resolution matters most: on a Pi 4 at a 1080p window, Auto resolution costs about three times the GPU time of 1x, so keep Resolution at 1x unless the GPU has headroom. The first runs compile shaders in the background ("Compiling shaders" panel); later runs reuse them.

**Touchscreens** (Windows, Linux): set `"enabled": true` in `roguesq_touch.json` (written next to the saves on first launch) to get the Android touch controls. The mouse pointer then acts as a finger and its game bindings are off, since many desktops deliver touches as mouse input (Raspberry Pi OS does for every touchscreen).

**D3D11** is for GPUs with a D3D11 driver but no D3D12 or Vulkan one (NVIDIA GTX 400/500, AMD HD 5000/6000, Intel Ivy Bridge). Select it with `--gfx-api d3d11` or `"graphicsAPI": "d3d11"` in `roguesq_video.json`; automatic mode uses it only when neither D3D12 nor Vulkan starts. It needs `d3dcompiler_47.dll` (part of Windows 10), and ray-traced effects are off. MSAA isn't available on D3D11 (RT64 needs programmable sample positions); use SSAA or FXAA instead. D3D10-class GPUs (feature level 10_0/10_1: NVIDIA GeForce 8/9/200, AMD HD 2000-4000) get the same backend at feature level 10, with vertex processing on the CPU and without SSAA; `--gfx-api d3d10` or `"graphicsAPI": "d3d10"` forces that path on any GPU; this is tested only on a modern GPU capped at feature level 10, not on real hardware. Its shaders ship precompiled in `d3d11_shader_cache/` next to the exe; keep that folder. A render state the shipped list doesn't cover compiles the first time it appears (the affected objects are missing until then, up to about 18 s for some texture modes, and a "Compiling shaders" progress panel shows in the bottom-right corner meanwhile) and is cached for later runs.
