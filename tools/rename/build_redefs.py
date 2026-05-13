"""Generate redefs.txt for llvm-objcopy --redefine-syms from all symbol_files/*.txt.

For each semantic name in the symbol files, emit three rename lines so the
llvm-objcopy pass catches whichever placeholder splat actually emitted:
  func_HHHHHHHH      -> semantic_name
  D_HHHHHHHH         -> semantic_name
  fake_func_HHHHHHHH -> semantic_name

Also appends explicit reverts for address-encoded splat-provided names that
user feedback asked us not to use (the setGlobalByte_8011A8XX cluster).

Usage:
    python tools/rename/build_redefs.py
"""
import re, glob, os, sys

# Resolve paths relative to repo root (parent-parent of this file)
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SYM_DIR = os.path.join(os.path.dirname(REPO), "rogue_squadron64", "symbol_files")
REDEFS = os.path.join(os.path.dirname(REPO), "rogue_squadron64", "build", "redefs.txt")

# Allow overrides via env
SYM_DIR = os.environ.get("RSR_SYM_DIR", SYM_DIR)
REDEFS = os.environ.get("RSR_REDEFS_PATH", REDEFS)

out = []
for sf in sorted(glob.glob(os.path.join(SYM_DIR, "*.txt"))):
    if "ignored" in sf:
        continue
    with open(sf, "r", encoding="utf-8") as f:
        for line in f:
            m = re.match(r"\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*0x([0-9A-Fa-f]+)", line)
            if not m:
                continue
            name, addr = m.group(1), m.group(2).upper().rjust(8, "0")
            if name.startswith("func_") or name.startswith("D_") or name.startswith("fake_"):
                continue
            out.append(f"func_{addr} {name}")
            out.append(f"D_{addr} {name}")
            out.append(f"fake_func_{addr} {name}")

os.makedirs(os.path.dirname(REDEFS), exist_ok=True)
with open(REDEFS, "w", encoding="utf-8") as f:
    f.write("\n".join(out) + "\n")
print(f"Wrote {len(out)} redef lines to {REDEFS}")

# Revert address-encoded splat-provided names that user directive forbade.
# These exist in splat's symbol_addrs.txt (outside our reach) so we have to
# undo them at the llvm-objcopy stage every build.
EXTRA_REVERTS = {
    "setGlobalByte_8011A870":    "func_80017AB0",
    "setGlobalByte_8011A871":    "func_80017AC0",
    "setGlobalByte_8011A872":    "func_80017AD0",
    "setGlobalBytes_8011A873_5": "func_80017B28",
    "setGlobalByte_8011A898":    "func_80017B48",
    "setGlobalFloat_8011A840":   "func_80017B78",
}
with open(REDEFS, "a", encoding="utf-8") as f:
    for old, new in EXTRA_REVERTS.items():
        f.write(f"{old} {new}\n")
print(f"Appended {len(EXTRA_REVERTS)} reverts")
