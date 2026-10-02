# Release CI

[.github/workflows/release.yml](../.github/workflows/release.yml) builds the Windows and Linux release packages on two self-hosted runners on the maintainer's machine, then attaches them to a **draft** GitHub release. The build needs ROM-derived inputs that are never committed and never uploaded to GitHub; they stay on the runner host.

## What the build needs that the repo does not have

| Input | Used by | Where it comes from |
|---|---|---|
| `roguesquadron.elf` | `regen_funcs` (N64Recomp, via `rogue_squadron.toml` `elf_path`) | the decomp build, `rogue_squadron64/build/roguesquadron.elf` |
| `factor5_boot_recompiled.c`, `musyx_audio_recompiled.c` | the main exe | RSPRecomp output, `build/factor5_ucode/` ([README](../README.md#3-generate-the-recompiled-c) step 3) |

The ROM itself is not a build input (N64Recomp reads code and data from the ELF). Players supply their ROM at runtime.

[ci/stage_inputs.py](../ci/stage_inputs.py) copies these into place at the start of each job and removes them, plus `RecompiledFuncs/` and `RecompiledPatches/`, at the end. It reads `RS64_INPUTS` (a directory holding `roguesquadron.elf` and `factor5_ucode/`), or `RS64_ELF` / `RS64_UCODE_DIR` for individual paths.

Each job builds N64Recomp and RecompModTool as their own CMake project (`lib/N64ModernRuntime/N64Recomp` into `build-tools/`), because the game's configure fails while `RecompiledFuncs/` does not exist yet. It then regenerates `RecompiledFuncs/` from scratch, fails if `funcs.h` comes out truncated (the stale-recompiler symptom) or if `tools/coop/gen_relocation.py --verify` finds the co-op relocation patches unapplied, and configures and builds the game with `-DN64RECOMP_EXE` pointing at the fresh recompiler.

## Linux runner (Docker, ephemeral)

[ci/linux/](../ci/linux/) holds an Ubuntu 24.04 image containing the build deps, Ubuntu's `mips-linux-gnu-gcc` for the `.nrm` code mods, and the Actions runner. Each container registers as an `--ephemeral` runner, takes one job, deregisters, and exits; [run-runner.ps1](../ci/linux/run-runner.ps1) then starts a fresh one. The ELF and ucode sources are bind-mounted read-only, and nothing ROM-derived goes into the image.

1. Create a fine-grained PAT scoped to this repository only, with **Administration: Read and write** (that permission is what mints runner registration tokens).
2. `copy ci\linux\runner.env.example ci\linux\runner.env` and fill in the PAT. `runner.env` is gitignored.
3. With Docker Desktop running: `pwsh ci/linux/run-runner.ps1` (`-Rebuild` refreshes the image and runner version; `-Once` runs a single job).

The entrypoint runs as root and keeps the PAT in root-owned processes. The runner and every job run as the unprivileged `runner` user with a clean environment, so a job cannot read the PAT.

The package is built against glibc 2.39, so players need Ubuntu 24.04 or equivalent.

## Windows runner (plain, no container)

Windows containers would need Hyper-V isolation and a multi-GB image with VS Build Tools. Running the runner directly on the dev machine reuses the toolchain that already builds the game.

1. Repo **Settings > Actions > Runners > New self-hosted runner > Windows**. Follow the download and `config.cmd` steps, and add the labels `rs64,windows`.
2. Install it as a service running as **your own account** (not NETWORK SERVICE), so it finds `cmake`, `python`, `mingw32-make`, VS 2022 with ClangCL, and `E:/mips-toolchain`.
3. Set a machine or user environment variable for the service account, then restart the service:
   - `RS64_ELF=E:\Projects\rogue_squadron64\build\roguesquadron.elf` (point at the live decomp build so symbol renames reach CI)
   - `RS64_UCODE_DIR=E:\Projects\RogueSquadron64Recomp\build\factor5_ucode`
4. Optional: the repo variable `RS64_MIPS_TOOLCHAIN_DIR` overrides the `E:/mips-toolchain` default.

The job builds the game in Release; `build/dist/` plus a README becomes the zip.

## Code mods

Both jobs build every `.nrm` that [mods/platforms.json](../mods/platforms.json) lists under `desktop`, using [tools/mods/build_code_mods.cmake](../tools/mods/build_code_mods.cmake). It finds each mod's folder by the `mod_filename` in its `mod.toml`. The mods are built before the game, so the game build stages them next to the exe. To ship a new code mod, add its `.nrm` to the `desktop` list. Locally: `cmake -DPLATFORM=desktop -P tools/mods/build_code_mods.cmake` (optionally `-DRECOMP_MOD_TOOL=`, `-DMIPS_GCC=`/`-DMIPS_LD=`).

## Native mods

Both desktop jobs configure with `-DRS64_MULTIPLAYER=OFF`, so the exe has no multiplayer code and online co-op ships as the `multiplayer-native` mod ([mods/multiplayer-native](../mods/multiplayer-native/)). The game's CMake builds that mod (`multiplayer_native.dll` / `.so`) as a dependency of the exe into `build/built_mods/<Config>/multiplayer-native` and [tools/mods/stage_mods.cmake](../tools/mods/stage_mods.cmake) stages it. The mod target also restages after its own builds (a multiplayer-source edit that does not relink the exe), and the Release dist gets the mod without its PDB. The Linux package script strips every staged `.so`.

The `desktop` list in `mods/platforms.json` names only `multiplayer-native`; the data-only `multiplayer` (the menu page JSON) is listed under `android` alone. `stage_mods.cmake` takes three arguments that decide what desktop stages:

| Argument | Used when | Effect |
|---|---|---|
| `BUILT=build/built_mods/<Config>` | always | a listed mod found there (the built native mod) is staged from it instead of from `mods/` |
| `SWAP=multiplayer-native=multiplayer` | `RS64_MULTIPLAYER=ON` | stages the data-only `multiplayer` where the list says `multiplayer-native` and removes any staged `multiplayer-native`, including a hand-copied one, on every relink |
| `DROP=multiplayer` | `RS64_MULTIPLAYER=OFF` | removes a stale staged `multiplayer`; the built native mod's folder carries the menu JSON itself |

Each desktop job has a "Check the multiplayer mod" step after the game build. It fails the job if the mod is not staged (`build/mods/...` on Linux, `build/dist/mods/...` on Windows) or if the exe still contains the multiplayer strings. The Windows step uses `Select-String` and ends with an explicit `exit 0`: the `shell: powershell` wrapper ends with `exit $LASTEXITCODE`, and `findstr` finding nothing (the passing case) would leave it at 1.

Platform decisions:

- OFF desktop builds are mod-only for multiplayer: with the mod disabled or missing there is no online co-op and no local co-op (`ROGUESQ_COOP_LOCAL` needs the mod).
- Android is not built by this workflow; its CMake forces `RS64_MULTIPLAYER` on, so multiplayer stays built in there.
- macOS is not covered yet: there is no export restriction check for the mod's library and no signing step.

## Unit tests

Both jobs run `cmake --build build --config Release --target rs64_unit_tests` (no `--config` on Linux) after the game build, and a failing test fails the job. The target builds and runs `lockstep_core_test`, `net_core_test`, `net_link_test`, `nav_target_test` and `host_api_test` (each prints `<name> OK`), then runs the style check (`tools/style/check_style.py`). The tests use [tests/check.h](../tests/check.h) (`CHECK`, never compiled out) so Release checks them too. `net_link_test` adds and removes a UPnP port mapping on the runner's router, which needs the runner to have a LAN address; on a runner with no IPv4 address it skips the address-text check.

## Expected build warnings

- `lld-link : warning : found both wmain and main; using latter`: SDL2main provides `wmain`, ours wins.
- lld-link `duplicate symbol` warnings for `upstream_compat.cpp` (overriding librecomp) and `zmemcpy` (over `RecompiledFuncs`): the intended `/FORCE:MULTIPLE` override pattern (AGENTS.md "Patches build").

## Running it

- **Test build:** Actions > Release > Run workflow. Both packages are uploaded as run artifacts (kept 3 days); no release is created.
- **Release:** `git tag v0.1.0 && git push origin v0.1.0`. Both packages are attached to a **draft** release. Review it, then publish by hand.

## Security

- The workflow triggers only on `v*` tags and manual dispatch, and each job is gated on the repository owner. **Never add `pull_request`**: a fork PR would run arbitrary code on your machine with the inputs mounted. Also set **Settings > Actions > General > Fork pull request workflows** to require approval.
- Keep the self-hosted runners attached to this repository only, not to an organization.

## Legal

Keeping the ROM and ELF off GitHub does not make the package ROM-free. The exe statically links the recompiled game code (`RecompiledFuncs`), which is derived from the copyrighted game. This is the same position Zelda64Recomp's binaries are in. Decide deliberately whether a release is published or kept private. On a **public** repository, run artifacts can be downloaded by any signed-in GitHub user, including the ones from manual test builds.
