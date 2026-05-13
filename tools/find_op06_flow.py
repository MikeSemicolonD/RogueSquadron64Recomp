"""Trace op 0x06's handler at IMEM 0x12A0 looking for jumps into the
pre-prefix vector math area at 0x14B0 (where $27=0xCB4 is set).
"""

import os
import struct
import rabbitizer

ROOT = r"E:/Projects/RogueSquadron64Recomp"
with open(os.path.join(ROOT, "dumps", "f5_ucode.imem.bin"), "rb") as f:
    imem = f.read()

instr_category = getattr(rabbitizer.InstrCategory, "RSP",
                         rabbitizer.InstrCategory.CPU)

def jal_or_j_target(word):
    """If this is a j/jal instruction, return target IMEM offset."""
    op = (word >> 26) & 0x3F
    if op not in (0x02, 0x03):
        return None
    imm26 = word & 0x03FFFFFF
    target_rsp = imm26 << 2
    return target_rsp - 0x04001000

# Disassemble op 0x06's handler from 0x12A0 with jump targets called out
print("=== op 0x06 handler (IMEM 0x12A0 ..) — looking for jumps to 0x14B0 area ===\n")
hit_14b0 = False
for off in range(0x12A0, min(0x1500, len(imem)), 4):
    word = struct.unpack(">I", imem[off:off+4])[0]
    instr = rabbitizer.Instruction(word, 0x04001000 + off, category=instr_category)
    target = jal_or_j_target(word)
    note = ""
    if target is not None:
        note = f"  -> IMEM 0x{target:04X}"
        if target == 0x14B0:
            note += "  <<< JUMPS TO PRE-PREFIX!"
            hit_14b0 = True
        elif 0x14A0 <= target <= 0x14D0:
            note += "  <<< near pre-prefix"
        elif target == 0x14D4:
            note += "  (op 0x03 entry — sets $28)"
        elif target == 0x14F0:
            note += "  (op 0x02 entry)"
        elif target == 0x1504:
            note += "  (op 0x01 / shared body entry)"
    print(f"  {0x04001000 + off:08X}: {word:08X}  {instr.disassemble()}{note}")
    # Stop on B8 (G_ENDDL pattern) or jr $ra at end of function
    if word == 0x03E00008:  # jr $ra
        print("  [function end: jr $ra]")
        break

print(f"\nFound jump to 0x14B0: {hit_14b0}")

# Also dump all jumps to ANY address in 0x14A0..0x1510 range from ANYWHERE
# in IMEM, to identify all paths into the pre-prefix / op 0x03 / op 0x02
# / op 0x01 / shared-body area.
print("\n\n=== All j/jal instructions in IMEM targeting 0x14A0..0x1510 ===\n")
for off in range(0, len(imem), 4):
    word = struct.unpack(">I", imem[off:off+4])[0]
    target = jal_or_j_target(word)
    if target is None or not (0x14A0 <= target <= 0x1510):
        continue
    instr = rabbitizer.Instruction(word, 0x04001000 + off, category=instr_category)
    op = (word >> 26) & 0x3F
    kind = "jal" if op == 0x03 else "j  "
    print(f"  0x{0x04001000 + off:08X} ({kind} -> IMEM 0x{target:04X}): {instr.disassemble()}")
