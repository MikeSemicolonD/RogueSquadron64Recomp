"""Disassemble Factor5 main ucode (after boot DMA) — covers full 0x1FA0 RDRAM
segment. The boot ucode DMAs the main code starting at IMEM 0x80, so:

  file_offset 0     → IMEM 0x80 (assuming text_address=0x04001080)
  file_offset 0x470 → IMEM 0x4F0  (op_02 handler tail, per docs/factor5-ucode-dispatch.md)
  file_offset 0x454 → IMEM 0x4D4  (op_03 handler entry)

But docs/factor5-ucode-dispatch.md shows dispatch loop at 0x04001010. That's
IMEM 0x10, which is BELOW where main code is supposed to start (0x80). So
either (a) the doc was wrong about the loop's address, or (b) there's a
larger ucode load. This script outputs disasm at multiple candidate base
addresses so we can identify the right one by looking at where the dispatch
loop's `lhu $2, 0xD6($2); jr $2` pattern actually occurs.
"""
import sys
import struct
import rabbitizer

PATH = sys.argv[1] if len(sys.argv) > 1 else "factor5_ucode_text.bin"
BASE = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x04001080
START_OFF = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x0
END_OFF = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x1FA0

with open(PATH, "rb") as f:
    text = f.read()

# Note: Factor 5 ucode is BE-stored in RDRAM. Each 4-byte chunk represents one
# RSP instruction in big-endian byte order.
n = (END_OFF - START_OFF) // 4
words = struct.unpack(f">{n}I", text[START_OFF:START_OFF + n*4])

print(f"; {PATH}: {len(text)} bytes; disasm offset {START_OFF:#x}..{END_OFF:#x}; base PC {BASE:#x}")
for i, w in enumerate(words):
    pc = BASE + START_OFF + i*4
    insn = rabbitizer.Instruction(w, vram=pc, category=rabbitizer.InstrCategory.RSP)
    s = insn.disassemble(extraLJust=12).strip()
    print(f"{pc:08X}  {w:08X}  {s}")
