"""Disassemble op 0x06's handler at IMEM 0x12A0 until we find its exit
point(s) — looking for either:
  - jr $ra (return from function call)
  - Unconditional j to main loop continuation (0x1088 or similar)
  - DL-push pattern (sw $17, X($Y) followed by lw $17, Y($X))

This tells us whether op 0x06's handler is acting as a G_DL chain (with
DL push+jump semantics) or just as a setup function that returns to the
dispatch loop without changing the DL pointer.
"""

import os
import struct
import rabbitizer

ROOT = r"E:/Projects/RogueSquadron64Recomp"
with open(os.path.join(ROOT, "dumps", "f5_ucode.imem.bin"), "rb") as f:
    imem = f.read()

instr_category = getattr(rabbitizer.InstrCategory, "RSP",
                         rabbitizer.InstrCategory.CPU)

# Disassemble from IMEM 0x12A0 forward, looking for terminators
# (jr $ra = 0x03E00008, unconditional j, b instructions)
start = 0x12A0
print(f"=== Trace of op 0x06 handler from IMEM 0x{start:04X} ===\n")
exits = []
for off in range(start, len(imem), 4):
    word = struct.unpack(">I", imem[off:off+4])[0]
    instr = rabbitizer.Instruction(word, 0x04001000 + off, category=instr_category)
    d = instr.disassemble()
    # Highlight exit points
    note = ""
    is_exit = False
    if word == 0x03E00008:  # jr $ra
        note = "  <<< jr $ra (function return)"
        is_exit = True
        exits.append((off, "jr $ra"))
    elif (word >> 26) == 0x02:  # j (unconditional)
        imm26 = word & 0x03FFFFFF
        target = imm26 << 2
        target_off = target - 0x04001000
        note = f"  <<< unconditional j -> IMEM 0x{target_off:04X}"
        if target_off == 0x1088:
            note += " (main-loop continuation)"
            is_exit = True
        elif target_off == 0x1010:
            note += " (main-loop start)"
            is_exit = True
        elif start <= target_off < off:
            note += " (backward branch - loop?)"
        exits.append((off, f"j 0x{target_off:04X}"))
    # Track writes to $17 (DL pointer)
    elif "$17" in d and ("lw" in d or "addiu" in d):
        if "lw $17" in d:
            note = "  <-- LOAD into $17 (potential DL pointer manipulation)"
    print(f"  {0x04001000 + off:08X}: {word:08X}  {d}{note}")
    # Stop on main-loop-continuation jump, or after 200 instructions
    if is_exit and "j 0x1088" in (note or ""):
        print("\n  >>> Reached main-loop continuation (function ends here)")
        break
    if off - start > 0x600:
        print("\n  (stopping after 0x600 bytes scanned)")
        break

print("\n=== All exit points found ===")
for off, kind in exits:
    print(f"  IMEM 0x{off:04X}: {kind}")
