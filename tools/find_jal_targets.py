"""Find all jal/j instructions in F3DFACTOR5 IMEM that target a specific
IMEM address, to trace call sites for the DMA helper at 0x11B0.
"""

import os
import struct
import sys
import rabbitizer

ROOT = r"E:/Projects/RogueSquadron64Recomp"
with open(os.path.join(ROOT, "dumps", "f5_ucode.imem.bin"), "rb") as f:
    imem = f.read()

instr_category = getattr(rabbitizer.InstrCategory, "RSP",
                         rabbitizer.InstrCategory.CPU)

# Targets to find callers of. NOTE: IMEM offset is the address from the start
# of the IMEM dump (which corresponds to RSP address 0x04001000). So an
# IMEM offset of 0x1B0 = RSP address 0x040011B0.
TARGETS = [
    (0x01B0, "DMA helper (RAM->DMEM, $2=ram, $3=dmem, $4=len)"),
    (0x0190, "DMA helper (DMEM->RAM)"),
    (0x014C, "DL fetch DMA (loads next DL chunk into DMEM[0x170])"),
    (0x01E4, "func_40011E4 (entry setup #2)"),
    (0x025C, "func_400125C (entry setup #1)"),
    (0x02F0, "func_40012F0"),
    (0x0308, "func_4001308"),
    (0x0324, "func_4001324"),
    (0x0088, "ucode_loop_continue (post-state-write)"),
]

for target_off, label in TARGETS:
    target_rsp_addr = 0x04001000 + target_off
    target_jal_imm = (target_rsp_addr & 0x0FFFFFFF) >> 2

    print(f"\n=== Callers of IMEM 0x{target_off:04X} ({label}) ===")
    hits = []
    for off in range(0, len(imem), 4):
        word = struct.unpack(">I", imem[off:off+4])[0]
        op = (word >> 26) & 0x3F
        if op not in (0x02, 0x03):  # j, jal
            continue
        imm26 = word & 0x03FFFFFF
        if imm26 == target_jal_imm:
            instr_kind = "jal" if op == 0x03 else "j"
            hits.append((off, word, instr_kind))

    if not hits:
        print("  (no callers found)")
    else:
        print(f"  Found {len(hits)} call sites:")
        for off, word, kind in hits:
            instr = rabbitizer.Instruction(word, 0x04001000 + off, category=instr_category)
            # Context: 3 instructions before + this one (delay slot)
            print(f"\n  Call from 0x{0x04001000 + off:08X} ({kind}):")
            for k in range(-3, 2):
                coff = off + k * 4
                if 0 <= coff < len(imem):
                    cw = struct.unpack(">I", imem[coff:coff+4])[0]
                    ci = rabbitizer.Instruction(cw, 0x04001000 + coff, category=instr_category)
                    mark = " <-- call" if k == 0 else " <-- delay" if k == 1 else ""
                    print(f"    {0x04001000 + coff:08X}: {cw:08X}  {ci.disassemble()}{mark}")
