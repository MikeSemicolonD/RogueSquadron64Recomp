"""Remove redundant func_HHHHHHHH / D_HHHHHHHH placeholder entries from each
symbol file when a semantic name for that address already exists in the same
file.

Splat sometimes emits both `mainGameLoop = 0x8003DFA0` and `func_8003DFA0 =
0x8003DFA0; // defined:true` as separate entries; this script keeps the
semantic one and drops the placeholder. Same-file only — does not collapse
duplicates across files (those are intentional, e.g. an overlay placeholder
shadowing a main entry for the same address).

Usage:
    python tools/rename/cleanup_placeholders.py
"""
import re, glob, os
from collections import defaultdict

SYM_DIR = os.environ.get(
    "RSR_SYM_DIR",
    "E:/Projects/rogue_squadron64/symbol_files",
)

total_removed = 0
for sym_file in glob.glob(os.path.join(SYM_DIR, "*.txt")):
    if "ignored" in sym_file:
        continue
    with open(sym_file, "r", encoding="utf-8") as f:
        lines = f.readlines()

    addr_has_semantic = defaultdict(bool)
    for line in lines:
        m = re.match(r"\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*0x([0-9A-Fa-f]+)", line)
        if not m:
            continue
        name, addr = m.group(1), m.group(2).upper().rjust(8, "0")
        if not (name.startswith("func_") or name.startswith("D_") or name.startswith("fake_")):
            addr_has_semantic[addr] = True

    new_lines = []
    removed = 0
    for line in lines:
        m = re.match(r"\s*(func_|D_)([0-9A-Fa-f]+)\s*=\s*0x([0-9A-Fa-f]+)", line)
        if m:
            addr = m.group(3).upper().rjust(8, "0")
            if addr_has_semantic[addr]:
                removed += 1
                continue
        new_lines.append(line)

    if removed:
        with open(sym_file, "w", encoding="utf-8") as f:
            f.writelines(new_lines)
        print(f"{os.path.basename(sym_file)}: removed {removed} redundant placeholders")
        total_removed += removed

if total_removed == 0:
    print("No redundant placeholders found.")
else:
    print(f"\nTotal removed: {total_removed}")
