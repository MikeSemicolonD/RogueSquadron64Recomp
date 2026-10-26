"""Trace F3DFACTOR5 RSP entry-point initialization to identify DMA-loaded
data structures (matrix stack, vertex buffer, dispatch table location).

The entry sequence is:
  0x1000: jal func_400125C   ; first setup
  0x1004: ori $5, $zero, 0x0
  0x1008: jal func_40011E4   ; second setup
  0x100C: nop
  0x1010: ... main loop ...
"""

import os
import struct
import rabbitizer

ROOT = r"E:/Projects/RogueSquadron64Recomp"
with open(os.path.join(ROOT, "dumps", "f5_ucode.imem.bin"), "rb") as f:
    imem = f.read()
with open(os.path.join(ROOT, "dumps", "f5_ucode.dmem.bin"), "rb") as f:
    dmem = f.read()

instr_category = getattr(rabbitizer.InstrCategory, "RSP",
                         rabbitizer.InstrCategory.CPU)

# COP0 register name table for RSP
COP0_NAMES = {
    0: "SP_MEM_ADDR (dmem)",
    1: "SP_DRAM_ADDR",
    2: "SP_RD_LEN (dma rdram->dmem)",
    3: "SP_WR_LEN (dma dmem->rdram)",
    4: "SP_STATUS",
    5: "SP_DMA_FULL",
    6: "SP_DMA_BUSY",
    7: "SP_SEMAPHORE",
    8: "DPC_START",
    9: "DPC_END",
    10:"DPC_CURRENT",
    11:"DPC_STATUS",
    12:"DPC_CLOCK",
    13:"DPC_BUFBUSY",
    14:"DPC_PIPEBUSY",
    15:"DPC_TMEM",
}

def disasm_range(start, end_or_count, label=""):
    print(f"\n=== {label} ===")
    end = end_or_count
    if end_or_count < 0x1000:  # treat as instruction count
        end = start + end_or_count * 4
    for off in range(start, end, 4):
        if off + 4 > len(imem):
            break
        word = struct.unpack(">I", imem[off:off+4])[0]
        instr = rabbitizer.Instruction(word, 0x04001000 + off,
                                       category=instr_category)
        disasm = instr.disassemble()
        ann = ""
        if "mtc0" in disasm:
            cop0_dst = (word >> 11) & 0x1F
            ann = "  ; " + COP0_NAMES.get(cop0_dst, f"cop0_{cop0_dst}")
        elif "mfc0" in disasm:
            cop0_src = (word >> 11) & 0x1F
            ann = "  ; from " + COP0_NAMES.get(cop0_src, f"cop0_{cop0_src}")
        print(f"  {0x04001000 + off:08X}: {word:08X}  {disasm}{ann}")

# func_400125C: first setup function
# 0x400125C - 0x04001000 = 0x025C
disasm_range(0x025C, 30, "func_400125C (entry setup #1)")

# func_40011E4: second setup function
# 0x40011E4 - 0x04001000 = 0x01E4
disasm_range(0x01E4, 30, "func_40011E4 (entry setup #2)")

# Also check entry-point itself
disasm_range(0x0000, 16, "IMEM entry point (first 16 instructions)")
