"""Trace what happens AFTER the vertex-transform shared body at 0x1504 to find
where op 0x02 emits its triangle commands and confirm the ordering vs the
fillRect-clears that follow it in the DL.

The shared body at 0x1504 ends somewhere after the jal func_4001F60 chain
(perspective divide). Trace forward to see if it:
  - Writes directly to the RDP output buffer at $16 (the RDP DMA cursor)
  - Schedules deferred work via a separate buffer
  - Calls back into the main dispatch loop at 0x1088 (post-state-write entry)
"""

import os
import struct
import rabbitizer

ROOT = r"E:/Projects/RogueSquadron64Recomp"
with open(os.path.join(ROOT, "dumps", "f5_ucode.imem.bin"), "rb") as f:
    imem = f.read()

instr_category = getattr(rabbitizer.InstrCategory, "RSP",
                         rabbitizer.InstrCategory.CPU)

def disasm_range(start, end_or_count, label=""):
    print(f"\n=== {label} ===")
    end = end_or_count
    if end_or_count < 0x1000:
        end = start + end_or_count * 4
    for off in range(start, end, 4):
        if off + 4 > len(imem):
            break
        word = struct.unpack(">I", imem[off:off+4])[0]
        instr = rabbitizer.Instruction(word, 0x04001000 + off,
                                       category=instr_category)
        d = instr.disassemble()
        # Highlight stores to $16 (RDP DMA cursor) or jumps back to dispatch
        ann = ""
        if "sw" in d and "$16" in d:
            ann = "  ; *** STORE to RDP output buffer ($16 cursor) ***"
        elif "$16" in d and "addiu" in d:
            ann = "  ; advance RDP cursor"
        elif d.startswith("j  "):
            ann = "  ; *** unconditional jump (may be loop continue) ***"
        print(f"  {0x04001000 + off:08X}: {word:08X}  {d}{ann}")

# The shared body starts at 0x1504. The op 0x02 prefix at 0x14F0 falls into it.
# func_4001F14 (vertex transform) is called from 0x1518.
# func_4001F60 (perspective divide) is called from somewhere later.
# Then the result is emitted to the RDP buffer.
#
# Trace 0x1518 onwards to find where transformed vertices land.
disasm_range(0x1518, 60, "shared body after jal func_4001F14 (op 0x02 loop body)")

# The full func_4001F60 + tail to see where it returns/emits
disasm_range(0x1F60, 60, "func_4001F60 (perspective divide + emit?)")

# Look at func_400179C (called from func_4001F60 at 0x1F90)
disasm_range(0x079C, 40, "func_400179C")
