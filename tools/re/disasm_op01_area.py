"""Disassemble IMEM around 0x14D0..0x1530 (op 0x01/0x02/0x03 entry area)
to see the complete prefix sequence for each opcode.
"""

import os
import struct
import rabbitizer

ROOT = r"E:/Projects/RogueSquadron64Recomp"
with open(os.path.join(ROOT, "dumps", "f5_ucode.imem.bin"), "rb") as f:
    imem = f.read()

instr_category = getattr(rabbitizer.InstrCategory, "RSP",
                         rabbitizer.InstrCategory.CPU)

# Show what's BEFORE op 0x03's entry at 0x14D4. If op 0x01 enters at 0x1504,
# there may be code at 0x14B0..0x14D4 that op 0x01 *doesn't* execute (since
# it skips ahead to 0x1504) — or some code at 0x14B0..0x14D0 that DOES set
# up state before the dispatch table entries.
print("=== IMEM 0x14B0..0x1530 (op 0x01/0x02/0x03 entry area) ===")
for off in range(0x14B0, 0x1530, 4):
    if off + 4 > len(imem):
        break
    word = struct.unpack(">I", imem[off:off+4])[0]
    instr = rabbitizer.Instruction(word, 0x04001000 + off, category=instr_category)
    marker = ""
    if off == 0x14D4:
        marker = "  <-- op 0x03 entry (dispatch)"
    elif off == 0x14F0:
        marker = "  <-- op 0x02 entry (dispatch)"
    elif off == 0x1504:
        marker = "  <-- op 0x01 entry (dispatch)"
    print(f"  {0x04001000 + off:08X}: {word:08X}  {instr.disassemble()}{marker}")

# Also check what writes to $25, $26 — these are used inside the shared body
# and their initial values matter for understanding the data layout.
print("\n=== writes to $25 and $26 ===")
for target in [25, 26]:
    print(f"\n  $r{target}:")
    for off in range(0, len(imem), 4):
        word = struct.unpack(">I", imem[off:off+4])[0]
        op = (word >> 26) & 0x3F
        rt = (word >> 16) & 0x1F
        rd = (word >> 11) & 0x1F
        is_write = False
        if op == 0:
            if rd == target and (word & 0x3F) not in (0x08, 0x0C, 0x0D, 0x18, 0x19, 0x1A, 0x1B):
                is_write = True
        elif op in (0x02, 0x03, 0x04, 0x05, 0x06, 0x07):
            is_write = (op == 0x03 and target == 31)
        elif op == 0x10 and (word >> 21) & 0x1F == 0:  # mfc0
            is_write = (rt == target)
        elif op == 0x12 and (word >> 21) & 0x1F == 0:  # mfc2
            is_write = (rt == target)
        elif op not in (0x28, 0x29, 0x2A, 0x2B, 0x2E, 0x2F):
            is_write = (rt == target)
        if is_write:
            instr = rabbitizer.Instruction(word, 0x04001000 + off, category=instr_category)
            print(f"    {0x04001000 + off:08X}: {word:08X}  {instr.disassemble()}")
