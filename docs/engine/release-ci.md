# Release CI

This page is for the maintainer who runs the release pipeline. [.github/workflows/release.yml](../../.github/workflows/release.yml) builds the release packages on two self-hosted runners on the maintainer's machine and attaches them to a **draft** GitHub release:

| Package | Built by |
|---|---|
| Windows x64 zip | Windows runner |
| Windows x86 (32-bit) zip | Windows runner, from the same recompiled code |
| Linux package (needs glibc 2.38+, e.g. Ubuntu 24.04) | Linux runner (Docker) |
| macOS zip: one universal `.app`, arm64 + x86_64 (untested on a Mac) | Windows runner, cross-built in WSL from the same recompiled code |
| Android arm64 APK | Windows runner, Gradle, from the same recompiled code |

Every zip and tarball carries `README.txt` ([ci/release-readme.txt](../../ci/release-readme.txt)) and the repo's `LICENSE` as `LICENSE.txt`; the APK is a single file.

## Making a release

- **Version:** the repo-root `VERSION` file (`major.minor.patch`, each part 0-99) is the one place it lives; CMake (`--version`, `--help`, the startup log), the Android `versionName`/`versionCode` and the local package names read it. Raise it with the VS Code task **Package: Bump version** or `tools/release/bump-version.ps1 -Part patch|minor|major` (`-Set 1.2.3` for an exact number).
- **Test build:** Actions > Release > Run workflow. The three packages are uploaded as run artifacts (kept 3 days); no release is created.
- **Release:** push a version tag:

  ```sh
  git tag v0.1.0
  git push origin v0.1.0
  ```

  The packages are attached to a draft release with generated notes, plus a `SHA256SUMS.txt` covering every package. Each package also gets a build provenance attestation; anyone can check a download with `gh attestation verify <file> --repo MikeSemicolonD/RogueSquadron64Recomp`. Review it, then publish it by hand.

## RecompModTool release

[.github/workflows/modtool-release.yml](../../.github/workflows/modtool-release.yml) builds RecompModTool from the N64Recomp fork for Windows x64 and Linux x64 on GitHub-hosted runners (it needs nothing ROM-derived) and runs on a `modtool-v*` tag (`git tag modtool-v1 && git push origin modtool-v1`) or by manual dispatch. It makes a draft release with `SHA256SUMS.txt` and attestations and never marks it Latest. The VS Code task **Package: RecompModTool (Windows + Linux)** (`tools/release/package-modtool.ps1`) builds the same packages locally, Linux through WSL.

## Why self-hosted runners

The build needs files derived from the ROM that are never committed and never uploaded to GitHub. They stay on the runner machine:

| Input | Used for | Where it comes from |
|---|---|---|
| `roguesquadron.elf` | Regenerating `RecompiledFuncs/` (N64Recomp reads it via `elf_path` in `rogue_squadron.toml`) | The decomp build, `rogue_squadron64/build/roguesquadron.elf` |
| `factor5_boot_recompiled.c`, `musyx_audio_recompiled.c` | The main exe | RSPRecomp output in `build/factor5_ucode/` ([README](../../README.md#3-generate-the-recompiled-c) step 3) |

The ROM itself is not a build input; players supply theirs at runtime.

## One-time setup

### Linux runner (Docker, ephemeral)

[ci/linux/](../../ci/linux/) holds an Ubuntu 24.04 image with the build dependencies, `mips-linux-gnu-gcc` for the code mods, and the GitHub Actions runner. Each container registers as an ephemeral runner, takes one job, deregisters and exits; [run-runner.ps1](../../ci/linux/run-runner.ps1) then starts a fresh one. The ELF and microcode sources are mounted read-only, and nothing ROM-derived goes into the image.

1. Create a fine-grained personal access token scoped to this repository only, with **Administration: Read and write** (that permission is what lets it register runners).
2. Copy `ci\linux\runner.env.example` to `ci\linux\runner.env` (gitignored) and put the token in it.
3. With Docker Desktop running: `pwsh ci/linux/run-runner.ps1`. Add `-Rebuild` to refresh the image and runner version, or `-Once` to run a single job.

The token stays in root-owned processes. The runner and every job run as the unprivileged `runner` user with a clean environment, so a job can't read it.

### Windows runner (no container)

Windows containers would need Hyper-V isolation and a multi-GB image with the Visual Studio Build Tools, so the runner runs directly on the dev machine and reuses its toolchain.

1. In the repo: **Settings > Actions > Runners > New self-hosted runner > Windows**. Follow the download and `config.cmd` steps and add the labels `rs64,windows`.
2. Install it as a service running as **your own account** (not NETWORK SERVICE), so it finds `cmake`, `python`, `mingw32-make`, Visual Studio 2022 with ClangCL, and `E:/mips-toolchain`.
3. Set these environment variables for that account, then restart the service:
   - `RS64_ELF=E:\Projects\rogue_squadron64\build\roguesquadron.elf` (the live decomp build, so symbol renames reach CI)
   - `RS64_UCODE_DIR=E:\Projects\RogueSquadron64Recomp\build\factor5_ucode`
4. Optional: the repo variable `RS64_MIPS_TOOLCHAIN_DIR` overrides the `E:/mips-toolchain` default for both the code mods and the game's patches ELF.
5. For the macOS package: the account's default WSL distro needs the osxcross setup from the README's macOS notes (`~/osxcross`, `cmake`, `ninja`, `zip`, `mips-linux-gnu-gcc`), and Windows needs Apple's Metal Developer Tools for Windows in `C:\Program Files\Metal Developer Tools`. The job builds a Linux N64Recomp into `~/rs64-ci-tools`, then runs `tools/release/macos/cross-build.sh` for arm64 and x86_64 (Release, into `~/rs64-ci-macos-<arch>`) and merges them with `tools/release/macos/package.sh`.
6. For the APK: Android Studio's SDK with NDK 29.0.14206865, found through the account's `ANDROID_HOME` (a CI checkout has no `local.properties`). `gradlew assembleRelease` gets the Release host tools through `-Prs64FileToC`, `-Prs64N64Recomp`, `-Prs64RecompModTool` and `-Prs64MipsDir`, and signs with the account's `~/.android/debug.keystore`, the same key as local builds, so CI APKs install over them.

## What each job does

1. [ci/stage_inputs.py](../../ci/stage_inputs.py) copies the ROM-derived inputs into place. It reads `RS64_INPUTS` (a directory holding `roguesquadron.elf` and `factor5_ucode/`), or `RS64_ELF` / `RS64_UCODE_DIR` separately.
2. N64Recomp and RecompModTool are built as their own CMake project (`lib/N64ModernRuntime/N64Recomp` into `build-tools/`), because the game's configure step fails until `RecompiledFuncs/` exists.
3. `RecompiledFuncs/` is regenerated from scratch. The job fails if `funcs.h` comes out truncated, the sign of a stale recompiler.
4. The code mods are built (see below), so the game build can stage them.
5. The game is configured with `-DN64RECOMP_EXE` pointing at the fresh recompiler and built in Release. On Windows the exe first precompiles the D3D11 shaders (feature level 11 and 10 sets) for `tools/d3d11/shader_combos.txt` into `build/dist/d3d11_shader_cache/` (`--precompile-d3d11-shaders`; falls back to WARP without a GPU), then `build/dist/` plus a README becomes the x64 zip. A 32-bit copy is built into `build-x86`, gets the same shader cache (the compiled shaders don't depend on the CPU architecture), and `build-x86/dist/` becomes the x86 zip.
6. The multiplayer mod check and unit tests run (below).
7. At the end, the staged inputs, `RecompiledFuncs/` and `RecompiledPatches/` are removed.

### Code mods

Both jobs build every `.nrm` listed under `desktop` in [mods/platforms.json](../../mods/platforms.json), using [tools/mods/build_code_mods.cmake](../../tools/mods/build_code_mods.cmake), which finds each mod's folder by the `mod_filename` in its `mod.toml`. To ship a new code mod, add its `.nrm` to that list. To run the same build locally:

```sh
cmake -DPLATFORM=desktop -P tools/mods/build_code_mods.cmake
```

Optional: `-DRECOMP_MOD_TOOL=`, `-DMIPS_GCC=` / `-DMIPS_LD=`.

### The multiplayer mod

The exe has no multiplayer code; online co-op ships as the native mod [mods/multiplayer-native](../../mods/multiplayer-native/). The game's CMake builds it (`multiplayer_native.dll` / `.so`) into `build/built_mods/<Config>/multiplayer-native`, and [tools/mods/stage_mods.cmake](../../tools/mods/stage_mods.cmake) stages it: a mod listed in `platforms.json` that is found in `build/built_mods/<Config>` is staged from there instead of from `mods/`. The mod also restages after its own builds, so a multiplayer-only edit that doesn't relink the exe still reaches `mods/`. The Release dist gets the mod without its PDB, and the Linux package script strips every staged `.so`.

After the game build, a "Check the multiplayer mod" step fails the job if the mod wasn't staged (`build/mods/...` on Linux, `build/dist/mods/...` on Windows) or if the exe still contains multiplayer strings. The Windows step uses `Select-String` and ends with an explicit `exit 0`, because the `shell: powershell` wrapper exits with `$LASTEXITCODE` and a `findstr` that finds nothing (the passing case) would leave it at 1.

With the mod disabled or missing there is no online or local co-op (`ROGUESQ_COOP_LOCAL` needs the mod). Android packs `libmultiplayer_native.so` into the APK, so multiplayer is a mod there too.

### Unit tests

Both jobs run `cmake --build build --config Release --target rs64_unit_tests` (without `--config` on Linux; the Windows job also runs them on the x86 build), and a failing test fails the job. The target runs `lockstep_core_test`, `mp_core_test`, `net_core_test`, `net_link_test`, `nav_target_test`, `host_api_test`, `cable_alias_test`, `crosshair_match_test` and `rs64_lights_test` (each prints `<name> OK`), then `style_check` against `tools/style/baseline.txt`. The tests use `CHECK` from [tests/check.h](../../tests/check.h), which is never compiled out, so they check in Release too.

`net_link_test` adds and removes a UPnP port mapping on the runner's router, which needs a LAN address; without an IPv4 address it skips the address-text check.

## Expected build warnings

- `lld-link : warning : found both wmain and main; using latter`: SDL2main provides `wmain`; ours wins.
- `duplicate symbol` warnings for `upstream_compat.cpp` (overriding librecomp) and `zmemcpy` (over `RecompiledFuncs`): the intended `/FORCE:MULTIPLE` override set in [CMakeLists.txt](../../CMakeLists.txt).

## Security

- The workflow runs only on `v*` tags and manual dispatch, and each job checks that the repository owner started it. **Never add a `pull_request` trigger**: a fork's pull request would run arbitrary code on your machine with the ROM-derived inputs mounted. Also set **Settings > Actions > General > Fork pull request workflows** to require approval.
- Keep the self-hosted runners attached to this repository only, not to an organization.

## Legal

Keeping the ROM and ELF off GitHub doesn't make the package ROM-free. The exe statically links the recompiled game code, which is derived from the copyrighted game; Zelda64Recomp's binaries are in the same position. Decide deliberately whether a release is published or kept private. On a **public** repository any signed-in GitHub user can download run artifacts, including those from manual test builds.
