"""Dump the full F3DFACTOR5 dispatch table from DMEM.

Two tables exist:
  - Table 1 at DMEM 0xD6: opcodes 0x00..0x3F via DMEM[0xD6 + (X << 1)]
  - Table 2 at DMEM 0x64: opcodes 0x40..0xBF via DMEM[0x64 + ((0x200 - (X<<1)) & 0x1FE)]
  - Opcodes 0xC0..0xFF go to RDP buffering (not dispatch).

Also annotate which handlers are known (from previous analysis) and which
opcodes appear in the attribution DL histogram.
"""

import os
import struct

ROOT = r"E:/Projects/RogueSquadron64Recomp"
with open(os.path.join(ROOT, "dumps", "f5_ucode.dmem.bin"), "rb") as f:
    dmem = f.read()
with open(os.path.join(ROOT, "dumps", "f5_ucode.imem.bin"), "rb") as f:
    imem = f.read()

TABLE1_BASE = 0xD6
TABLE2_BASE = 0x64

# Known handler annotations
KNOWN = {
    0x0000: "back-to-main-loop (no-op return)",
    0x10D8: "G_NOOP handler",
    0x12A0: "op 0x06 reuse (vector ops, NOT G_DL)",
    0x12C4: "op 0xB8 (G_ENDDL)",
    0x12E4: "op 0xBE",
    0x12FC: "op 0xBA setOtherMode_H",
    0x1304: "op 0xB9 setOtherMode_L",
    0x1350: "op 0xB7 G_SETGEOMETRYMODE",
    0x135C: "op 0xB6 G_CLEARGEOMETRYMODE",
    0x1390: "op 0xBC G_MOVEWORD",
    0x146C: "op 0xBF",
    0x14D4: "op 0x03 (Factor 5 - sets $28=0xB70, $29=0)",
    0x14F0: "op 0x02 (Factor 5 - sets $11=1, $13=$27+2)",
    0x1504: "op 0x01 (Factor 5 - SHARED LOOP BODY entry, no prefix)",
    0x14B0: "PRE-PREFIX vector math (sets $27=0xCB4 at 0x14D0)",
}

# Opcodes seen in the attribution DL histogram
ATTRIB_DL_OPCODES = {0x00, 0x01, 0x02, 0x03, 0x06, 0x80,
                     0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBC,
                     0xE6, 0xE7, 0xE8, 0xE9, 0xED,
                     0xF6, 0xF7, 0xF8, 0xF9, 0xFE, 0xFF}

def handler_offset(opcode):
    top2 = (opcode >> 6) & 3
    if top2 == 0:
        off = TABLE1_BASE + (opcode << 1)
    elif top2 == 3:
        return None
    else:
        off = TABLE2_BASE + ((0x200 - (opcode << 1)) & 0x1FE)
    return struct.unpack(">H", dmem[off:off+2])[0]

print("Full F3DFACTOR5 dispatch table:\n")
print(f"{'OP':>4}  {'IMEM':>6}  {'in DL':>6}  notes")
print("-" * 70)
# Cluster opcodes by handler (so we see opcode-reuse patterns)
by_handler = {}
for op in range(0x100):
    h = handler_offset(op)
    by_handler.setdefault(h, []).append(op)

# Print full table, marking attribution-DL opcodes
for op in range(0x100):
    h = handler_offset(op)
    if h is None:
        if op in ATTRIB_DL_OPCODES:
            print(f"  {op:02X}  RDP-buf  YES   (RDP command buffering, top 2 bits = 0b11)")
        continue
    in_dl = "YES" if op in ATTRIB_DL_OPCODES else ""
    note = KNOWN.get(h, "")
    other_ops = [f"{x:02X}" for x in by_handler[h] if x != op]
    if other_ops:
        note += f"  (shared with op {','.join(other_ops)})"
    print(f"  {op:02X}  0x{h:04X}  {in_dl:>6}   {note}")

# Highlight which opcodes share the IMEM 0x14B0 entry point (the pre-prefix
# code that sets $27=0xCB4 — if any opcode jumps there, it would set $27).
print("\n\nIMEM 0x14B0 (pre-prefix code that sets $27=0xCB4) — opcodes dispatching here:")
print(f"  {by_handler.get(0x14B0, '(none — only reached by fall-through)')}")

# Find handlers near 0xE3C and 0x16E8 (other $27 writers)
print("\n\nIMEM 0x0E3C (sets $27 = $8) — opcodes dispatching near:")
near_e3c = []
for h, ops in by_handler.items():
    if h and 0x0E00 <= h <= 0x0E80:
        near_e3c.append((h, ops))
for h, ops in sorted(near_e3c):
    print(f"  0x{h:04X}: opcodes {[f'{x:02X}' for x in ops]}")

print("\nIMEM 0x16E8 (sets $27 = 0xCB4) — opcodes dispatching near:")
near_16e8 = []
for h, ops in by_handler.items():
    if h and 0x1680 <= h <= 0x1720:
        near_16e8.append((h, ops))
for h, ops in sorted(near_16e8):
    print(f"  0x{h:04X}: opcodes {[f'{x:02X}' for x in ops]}")
