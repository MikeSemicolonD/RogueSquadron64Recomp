"""Deep-dive into F3DFACTOR5 op 0x02 handler logic.

Trace through:
  - op 0x02 entry at IMEM 0x14F0
  - common body at 0x1504 (shared with op 0x01 and 0x03)
  - inner jal targets (func_4001F14, func_4001F60)

Look for:
  - DMA operations (mtc0 to SP_DMA_* registers)
  - COP2 vector setup
  - jumps to common code paths
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

def disasm_range(start, end, label=""):
    print(f"\n=== {label} (IMEM 0x{start:04X} .. 0x{end:04X}) ===")
    for off in range(start, end, 4):
        if off + 4 > len(imem):
            break
        word = struct.unpack(">I", imem[off:off+4])[0]
        instr = rabbitizer.Instruction(word, 0x04001000 + off,
                                       category=instr_category)
        disasm = instr.disassemble()
        # Annotate DMA-related ops
        ann = ""
        if "$c" in disasm or "$0" in disasm or "mtc0" in disasm:
            if "mtc0" in disasm:
                # Decode which control register is being written
                cop0_dst = (word >> 11) & 0x1F
                cop0_names = {
                    0: "SP_MEM_ADDR (DMA cache addr)",
                    1: "SP_DRAM_ADDR (DMA dram addr)",
                    2: "SP_RD_LEN  (DMA RDRAM->DMEM/IMEM)",
                    3: "SP_WR_LEN  (DMA DMEM/IMEM->RDRAM)",
                    4: "SP_STATUS",
                    5: "SP_DMA_FULL",
                    6: "SP_DMA_BUSY",
                    7: "SP_SEMAPHORE",
                    8: "DPC_START",
                    9: "DPC_END",
                    10:"DPC_CURRENT",
                    11:"DPC_STATUS",
                }
                ann = "  ; " + cop0_names.get(cop0_dst, f"cop0_{cop0_dst}")
        print(f"  {0x04001000 + off:08X}: {word:08X}  {disasm}{ann}")

# Op 0x02 entry + first few instructions
disasm_range(0x14F0, 0x1510, "op 0x02 prefix")
# Op 0x01 entry + few instructions
disasm_range(0x1504, 0x1520, "op 0x01 prefix (shared body start)")
# Op 0x03 entry
disasm_range(0x14D4, 0x14F0, "op 0x03 prefix")

# The inner jals - func_4001F14 and func_4001F60
disasm_range(0x0F14, 0x0F60, "func_4001F14 (called from shared body)")
disasm_range(0x0F60, 0x0FC0, "func_4001F60 (called from shared body)")

# What's at offset 0x1088 (mentioned in entry-point loop)
disasm_range(0x1088, 0x10D8, "ucode_loop_continue (post-state-write)")

# What's at offset 0x12A0 (op 0x06 G_DL handler) for comparison
disasm_range(0x12A0, 0x12E4, "op 0x06 (G_DL) handler — known DMA-loading path")
