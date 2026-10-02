#!/usr/bin/env python3
"""Stage the ROM-derived build inputs a release build needs into the checkout.

The inputs never live in the repo. They come from a directory on the runner host
(RS64_INPUTS, bind-mounted read-only into the Linux container):

  roguesquadron.elf             decomp ELF that rogue_squadron.toml recompiles
  factor5_ucode/*.c             RSPRecomp output for the boot and MusyX ucodes

RS64_ELF and RS64_UCODE_DIR override the individual paths (point RS64_ELF at the
decomp's live build/roguesquadron.elf so symbol renames reach CI).

  python ci/stage_inputs.py            copy inputs into place
  python ci/stage_inputs.py --clean    remove the staged copies
"""
import argparse
import os
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ELF_DST = REPO.parent / "rogue_squadron64" / "build" / "roguesquadron.elf"
STAGED_MARKER = ELF_DST.parent.parent / ".rs64-ci-staged"
UCODE_DST = REPO / "build" / "factor5_ucode"
UCODE_FILES = ("factor5_boot_recompiled.c", "musyx_audio_recompiled.c")


def fail(msg):
    print(f"stage_inputs: {msg}", file=sys.stderr)
    sys.exit(1)


def resolve(env_name, fallback):
    val = os.environ.get(env_name)
    if val:
        return Path(val)
    inputs = os.environ.get("RS64_INPUTS")
    if not inputs:
        fail(f"set RS64_INPUTS (or {env_name}) on the runner")
    return Path(inputs) / fallback


def stage():
    elf = resolve("RS64_ELF", "roguesquadron.elf")
    ucode = resolve("RS64_UCODE_DIR", "factor5_ucode")
    if not elf.is_file():
        fail(f"decomp ELF not found at {elf}")
    if elf.read_bytes()[:4] != b"\x7fELF":
        fail(f"{elf} is not an ELF file")
    for name in UCODE_FILES:
        if not (ucode / name).is_file():
            fail(f"missing ucode source {ucode / name}")

    if ELF_DST.parent.parent.exists() and not STAGED_MARKER.exists():
        fail(f"{ELF_DST.parent.parent} exists and was not created by this script; refusing to overwrite a real decomp checkout")
    ELF_DST.parent.mkdir(parents=True, exist_ok=True)
    STAGED_MARKER.touch()
    shutil.copyfile(elf, ELF_DST)
    UCODE_DST.mkdir(parents=True, exist_ok=True)
    for name in UCODE_FILES:
        shutil.copyfile(ucode / name, UCODE_DST / name)
    print(f"stage_inputs: ELF -> {ELF_DST}")
    print(f"stage_inputs: {len(UCODE_FILES)} ucode sources -> {UCODE_DST}")


def clean():
    if os.environ.get("GITHUB_ACTIONS") != "true":
        fail("--clean only runs inside GitHub Actions (it deletes RecompiledFuncs/)")
    if STAGED_MARKER.exists():
        shutil.rmtree(STAGED_MARKER.parent, ignore_errors=True)
    for name in UCODE_FILES:
        (UCODE_DST / name).unlink(missing_ok=True)
    shutil.rmtree(REPO / "RecompiledFuncs", ignore_errors=True)
    shutil.rmtree(REPO / "RecompiledPatches", ignore_errors=True)
    print("stage_inputs: removed staged inputs and generated sources")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clean", action="store_true")
    args = ap.parse_args()
    clean() if args.clean else stage()


if __name__ == "__main__":
    main()
