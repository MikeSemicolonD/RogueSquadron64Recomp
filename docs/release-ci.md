# Release CI

[.github/workflows/release.yml](../.github/workflows/release.yml) builds the Windows and Linux release packages on two self-hosted runners on the maintainer's machine, then attaches them to a **draft** GitHub release. The build needs ROM-derived inputs that are never committed and never uploaded to GitHub; they stay on the runner host.

## What the build needs that the repo does not have

| Input | Used by | Where it comes from |
|---|---|---|
| `roguesquadron.elf` | `regen_funcs` (N64Recomp, via `rogue_squadron.toml` `elf_path`) | the decomp build, `rogue_squadron64/build/roguesquadron.elf` |
| `factor5_ucode_recompiled.c`, `factor5_boot_recompiled.c`, `musyx_audio_recompiled.c` | the main exe | RSPRecomp output, `build/factor5_ucode/` |

The ROM itself is not a build input (N64Recomp reads code and data from the ELF). Players supply their ROM at runtime.

[ci/stage_inputs.py](../ci/stage_inputs.py) copies these into place at the start of each job and removes them, plus `RecompiledFuncs/` and `RecompiledPatches/`, at the end. It reads `RS64_INPUTS` (a directory holding `roguesquadron.elf` and `factor5_ucode/`), or `RS64_ELF` / `RS64_UCODE_DIR` for individual paths.

Each job builds N64Recomp as its own CMake project (`lib/N64ModernRuntime/N64Recomp` into `build-tools/`), because the game's configure fails while `RecompiledFuncs/` does not exist yet. It then regenerates `RecompiledFuncs/` from scratch, fails if `funcs.h` comes out truncated (the stale-recompiler symptom), and configures and builds the game with `-DN64RECOMP_EXE` pointing at the fresh recompiler.

## Linux runner (Docker, ephemeral)

[ci/linux/](../ci/linux/) holds an Ubuntu 24.04 image containing the build deps, Ubuntu's `mips-linux-gnu-gcc` for `patches/` (its `patches.elf` matches the `E:/mips-toolchain` output), and the Actions runner. Each container registers as an `--ephemeral` runner, takes one job, deregisters, and exits; [run-runner.ps1](../ci/linux/run-runner.ps1) then starts a fresh one. The ELF and ucode sources are bind-mounted read-only, and nothing ROM-derived goes into the image.

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

## Running it

- **Test build:** Actions > Release > Run workflow. Both packages are uploaded as run artifacts (kept 3 days); no release is created.
- **Release:** `git tag v0.1.0 && git push origin v0.1.0`. Both packages are attached to a **draft** release. Review it, then publish by hand.

## Security

- The workflow triggers only on `v*` tags and manual dispatch, and each job is gated on the repository owner. **Never add `pull_request`**: a fork PR would run arbitrary code on your machine with the inputs mounted. Also set **Settings > Actions > General > Fork pull request workflows** to require approval.
- Keep the self-hosted runners attached to this repository only, not to an organization.

## Legal

Keeping the ROM and ELF off GitHub does not make the package ROM-free. The exe statically links the recompiled game code (`RecompiledFuncs`), which is derived from the copyrighted game. This is the same position Zelda64Recomp's binaries are in. Decide deliberately whether a release is published or kept private. On a **public** repository, run artifacts can be downloaded by any signed-in GitHub user, including the ones from manual test builds.
