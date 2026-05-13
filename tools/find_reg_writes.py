"""Disassemble the F3DFACTOR5 RSP IMEM and find all writes to specific
registers so we can trace how $27 (vertex base) and $28 (matrix base)
are set up before op 0x02's loop body runs.
"""

import os
import struct
import rabbitizer

ROOT = r"E:/Projects/RogueSquadron64Recomp"
with open(os.path.join(ROOT, "dumps", "f5_ucode.imem.bin"), "rb") as f:
    imem = f.read()

instr_category = getattr(rabbitizer.InstrCategory, "RSP",
                         rabbitizer.InstrCategory.CPU)

# MIPS instruction analysis: find all writes to register N.
# Reg-write instructions in MIPS:
#   R-type (op=0):  rd field at bits [15:11]. Most R-type instructions write rd.
#                   Exceptions: jr, jalr_no_link (write nothing visible), syscall/break, mult/div (write hi/lo), sw etc.
#   I-type:         rt field at bits [20:16] for: addiu, andi, ori, xori, slti, lui, lw, lh, lb, etc.
#                   Branches (beq/bne/blez/bgtz) write nothing.
#   J-type:         no register write (except jal -> $ra)

def writes_to(word, target_reg):
    """Return True if this instruction writes to target_reg."""
    op = (word >> 26) & 0x3F
    rs = (word >> 21) & 0x1F
    rt = (word >> 16) & 0x1F
    rd = (word >> 11) & 0x1F
    func = word & 0x3F

    if op == 0:
        # R-type
        # Check func to filter out non-writing ones
        # jr (0x08): no write, jalr (0x09): writes rd (default $ra=31), syscall/break/sync/mfhi/mflo: special
        if func == 0x08:  # jr
            return False
        if func == 0x0C:  # syscall
            return False
        if func == 0x0D:  # break
            return False
        if func == 0x18 or func == 0x19:  # mult/multu
            return False  # writes hi/lo
        if func == 0x1A or func == 0x1B:  # div/divu
            return False
        return rd == target_reg
    elif op in (0x01, 0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17):
        # Branches / regimm: no rt write
        return False
    elif op in (0x02, 0x03):
        # j, jal — jal writes $ra=31
        if op == 0x03:
            return target_reg == 31
        return False
    elif op in (0x28, 0x29, 0x2A, 0x2B, 0x2E, 0x2F):
        # Stores: no register write
        return False
    elif op == 0x12:  # COP2
        # Vector ops — most write to $vN, not to general regs.
        # Some mfc2 writes to rt.
        sub = (word >> 21) & 0x1F
        if sub == 0:  # MFC2
            return rt == target_reg
        return False
    elif op == 0x10:  # COP0
        sub = (word >> 21) & 0x1F
        if sub == 0:  # MFC0
            return rt == target_reg
        return False
    else:
        # Most I-type: addiu, andi, ori, xori, slti, sltiu, lw, lh, lb, lhu, lbu, lui
        # All write rt
        return rt == target_reg

def disasm(off):
    word = struct.unpack(">I", imem[off:off+4])[0]
    instr = rabbitizer.Instruction(word, 0x04001000 + off, category=instr_category)
    return instr.disassemble()

TARGETS = [27, 28, 29, 10, 11]
TARGET_NAMES = {27: "s11/$27", 28: "s12/$28", 29: "sp/$29",
                10: "t2/$10", 11: "t3/$11"}

print("Scanning IMEM for writes to key op 0x02 registers...\n")
for target in TARGETS:
    print(f"\n=== Writes to ${target} ({TARGET_NAMES[target]}) ===")
    hits = []
    for off in range(0, len(imem), 4):
        word = struct.unpack(">I", imem[off:off+4])[0]
        if writes_to(word, target):
            hits.append((off, word, disasm(off)))
    print(f"  Found {len(hits)} writes:")
    for off, word, d in hits:
        print(f"    {0x04001000 + off:08X}: {word:08X}  {d}")
