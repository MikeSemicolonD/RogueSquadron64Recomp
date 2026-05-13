"""Find all RSP DMA setup instructions in F3DFACTOR5 IMEM.

Standard RSP DMA programming:
  mtc0 $R, $c0   # SP_MEM_ADDR    (DMEM/IMEM offset, bit 12 = IMEM)
  mtc0 $R, $c1   # SP_DRAM_ADDR   (RDRAM physical address)
  mtc0 $R, $c2   # SP_RD_LEN      (length, kicks RAM->DMEM DMA)
  mtc0 $R, $c3   # SP_WR_LEN      (length, kicks DMEM->RAM DMA)

Encoding: opcode=COP0 (0x10), sub=MTC0 (0x04 in rs field), rd in bits[15:11]
  word = (0x10 << 26) | (0x04 << 21) | (rt << 16) | (rd << 11)
"""

import os
import struct
import rabbitizer

ROOT = r"E:/Projects/RogueSquadron64Recomp"
with open(os.path.join(ROOT, "dumps", "f5_ucode.imem.bin"), "rb") as f:
    imem = f.read()

instr_category = getattr(rabbitizer.InstrCategory, "RSP",
                         rabbitizer.InstrCategory.CPU)

COP0_NAMES = {
    0: "SP_MEM_ADDR (DMEM offset)",
    1: "SP_DRAM_ADDR (RAM address)",
    2: "SP_RD_LEN (RAM->DMEM kick)",
    3: "SP_WR_LEN (DMEM->RAM kick)",
    4: "SP_STATUS",
    7: "SP_SEMAPHORE",
    8: "DPC_START",
    9: "DPC_END",
    11: "DPC_STATUS",
}

# Find all mtc0 instructions
print("All mtc0 (write-to-cop0) instructions in IMEM:")
print()
for off in range(0, len(imem), 4):
    word = struct.unpack(">I", imem[off:off+4])[0]
    op = (word >> 26) & 0x3F
    if op != 0x10:  # COP0
        continue
    rs = (word >> 21) & 0x1F
    if rs != 0x04:  # MTC0
        continue
    rt = (word >> 16) & 0x1F
    rd = (word >> 11) & 0x1F
    cop0_name = COP0_NAMES.get(rd, f"cop0_{rd}")
    instr = rabbitizer.Instruction(word, 0x04001000 + off, category=instr_category)
    # Print a window of context: 4 instructions before + this one
    print(f"  IMEM 0x{off:04X}  ({0x04001000 + off:08X}): mtc0 $r{rt}, {cop0_name}")
    print(f"    Context (this -4 .. this):")
    for k in range(-4, 1):
        coff = off + k * 4
        if 0 <= coff < len(imem):
            cw = struct.unpack(">I", imem[coff:coff+4])[0]
            ci = rabbitizer.Instruction(cw, 0x04001000 + coff, category=instr_category)
            mark = " <-- mtc0" if k == 0 else ""
            print(f"      {0x04001000 + coff:08X}: {cw:08X}  {ci.disassemble()}{mark}")
    print()
