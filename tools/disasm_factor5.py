"""Disassemble Factor5 RSP ucode using rabbitizer."""
import sys
import struct
import rabbitizer

TEXT_ADDR = 0x04001000  # RSP IMEM base
TEXT_PATH = sys.argv[1] if len(sys.argv) > 1 else "factor5_ucode_text.bin"

with open(TEXT_PATH, "rb") as f:
    text = f.read()

assert len(text) == 0x1000, f"unexpected size {len(text)}"
words = struct.unpack(">1024I", text)

# Decode all words as RSP instructions
print(f"; Factor5 ucode disassembly ({len(words)} insns)")
print(f"; Loaded to RSP IMEM at 0x{TEXT_ADDR:08X}")
print()

for i, w in enumerate(words):
    pc = TEXT_ADDR + i * 4
    insn = rabbitizer.Instruction(w, vram=pc, category=rabbitizer.InstrCategory.RSP)
    s = insn.disassemble(extraLJust=12).strip()
    print(f"{pc:08X}  {w:08X}  {s}")
