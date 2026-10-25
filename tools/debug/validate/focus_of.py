#!/usr/bin/env python3
"""Print `--focus ADDR:SIZE` args for a state's focus structures, for state_diff.ps1.

  python tools/debug/validate/focus_of.py <state_model.toml> <state-id>

Maps each name in the state's `focus` list to an address/size via rdram_golden_diff.py's
KNOWN_GLOBALS table, and prints the rdram_golden_diff `--focus` arguments (one flag + value pair
per line, ready to splat onto the command line).
"""
import sys
import pathlib
import importlib.util

here = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(here.parents[1] / "state"))
sys.path.insert(0, str(here))

from state_model import load_model  # noqa: E402


def _known_globals():
    spec = importlib.util.spec_from_file_location("rgd", str(here / "rdram_golden_diff.py"))
    rgd = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(rgd)   # safe: main() only runs under __main__
    return {name: (addr, size) for (name, addr, size) in rgd.KNOWN_GLOBALS}


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: focus_of.py <state_model.toml> <state-id>")
    model = load_model(sys.argv[1])
    state = model.by_id.get(sys.argv[2])
    if state is None:
        return
    name2 = _known_globals()
    for name in state.focus:
        if name in name2:
            addr, size = name2[name]
            print("--focus")
            print(f"0x{addr:08X}:0x{size:X}")


if __name__ == "__main__":
    main()
