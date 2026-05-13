"""Scan unnamed recompiled-C functions for state-machine shape.

Two signals:
  1. Jump-table state machine: body contains a `switch_error(...)` call
     (N64Recomp's marker for an unrecognized jump-via-register, almost always
     a compiler-generated switch table).
  2. Repeated MEM_W writes to the same offset (3+ times) — characteristic of
     a function that assigns the state byte/word multiple times.

Usage:
    python tools/rename/find_state_machines.py

Prints jump-table candidates first (highest confidence), then state-write
candidates. Override the recompiled-C directory with RSR_FUNCS_DIR.
"""
import os, re, glob

FUNCS_DIR = os.environ.get(
    "RSR_FUNCS_DIR",
    "E:/Projects/N64Recomp/RecompiledFuncs",
)

func_def = re.compile(r"^RECOMP_FUNC void (\w+)\(uint8_t\* rdram, recomp_context\* ctx\) \{", re.M)

jump_table = []
state_writes = []

for cfile in sorted(glob.glob(os.path.join(FUNCS_DIR, "funcs_*.c"))):
    with open(cfile, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    matches = list(func_def.finditer(text))
    for i, m in enumerate(matches):
        name = m.group(1)
        if not name.startswith("func_8"):
            continue
        start = m.end()
        end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        body = text[start:end]
        if len(body) < 800 or len(body) > 20000:
            continue

        if "switch_error" in body:
            jump_table.append((name, len(body)))
            continue

        # Repeated writes to the same offset hint at a state variable
        writes = re.findall(r"MEM_W\(([-0-9A-FX]+),\s*ctx->r\d+\)\s*=\s*ctx->r\d+;", body)
        write_counts = {}
        for w in writes:
            write_counts[w] = write_counts.get(w, 0) + 1
        state_offsets = {o: c for o, c in write_counts.items() if c >= 3 and not o.startswith("-")}
        if state_offsets:
            state_writes.append((name, len(body), state_offsets))

print(f"Jump-table state machines: {len(jump_table)}")
for name, sz in sorted(jump_table, key=lambda r: r[1]):
    print(f"  {name}  body={sz}B")

print(f"\nState-write candidates: {len(state_writes)}")
for name, sz, offs in sorted(state_writes, key=lambda r: r[1])[:25]:
    sample = list(offs.items())[:3]
    print(f"  {name}  body={sz}B  writes={sample}")
