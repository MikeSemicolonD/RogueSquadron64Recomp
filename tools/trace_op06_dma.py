"""Scan op 0x06's handler (IMEM 0x12A0 onwards) for ANY DMA-related
activity: mtc0 instructions, uses of $20 (w1 = RAM source per dispatch),
loads from high RAM addresses, etc.

Also follow ALL jal targets reachable from op 0x06's handler to see
where the actual data movement happens.
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
    0: "SP_MEM_ADDR (DMEM dst)", 1: "SP_DRAM_ADDR (RAM src)",
    2: "SP_RD_LEN (RAM->DMEM)", 3: "SP_WR_LEN (DMEM->RAM)",
    4: "SP_STATUS", 5: "SP_DMA_FULL", 6: "SP_DMA_BUSY",
    7: "SP_SEMAPHORE",
    8: "DPC_START", 9: "DPC_END", 10: "DPC_CURRENT", 11: "DPC_STATUS",
}

def jal_target(word):
    op = (word >> 26) & 0x3F
    if op != 0x03:  # jal
        return None
    imm26 = word & 0x03FFFFFF
    return ((imm26 << 2) & 0x0FFFFFFF) - 0x1000  # back to IMEM offset

def j_target(word):
    op = (word >> 26) & 0x3F
    if op != 0x02:  # j
        return None
    imm26 = word & 0x03FFFFFF
    return ((imm26 << 2) & 0x0FFFFFFF) - 0x1000

# Walk op 0x06's handler tree (DFS through jal/j targets)
def walk_function(start, max_steps=300, seen=None):
    if seen is None:
        seen = set()
    if start in seen or start < 0 or start >= len(imem):
        return seen
    seen.add(start)
    off = start
    steps = 0
    while off < len(imem) and steps < max_steps:
        word = struct.unpack(">I", imem[off:off+4])[0]
        instr = rabbitizer.Instruction(word, 0x04001000 + off, category=instr_category)
        d = instr.disassemble()

        # Found something interesting
        op = (word >> 26) & 0x3F
        if op == 0x10 and (word >> 21) & 0x1F == 0x04:
            # mtc0
            rd = (word >> 11) & 0x1F
            rt = (word >> 16) & 0x1F
            print(f"  [DMA-RELATED] {0x04001000 + off:08X}: {word:08X}  "
                  f"mtc0 $r{rt}, {COP0_NAMES.get(rd, f'cop0_{rd}')}")
        elif "$20" in d:
            # Uses $20 (w1 of current command — RAM address!)
            print(f"  [USES $20]   {0x04001000 + off:08X}: {word:08X}  {d}")

        # End-of-function markers
        if word == 0x03E00008:  # jr $ra
            break
        if op == 0x02:  # j (unconditional, exits function)
            target = j_target(word)
            if target is not None and target not in seen and 0 <= target < len(imem):
                # Follow this j to a new region
                if target != off + 4:  # ignore "j .+4" (which is just continuation)
                    pass  # fall through to next instruction handling
            # Execute delay slot
            if off + 4 < len(imem):
                ds_word = struct.unpack(">I", imem[off+4:off+8])[0]
                ds_instr = rabbitizer.Instruction(ds_word, 0x04001000 + off + 4, category=instr_category)
                if "mtc0" in ds_instr.disassemble() or "$20" in ds_instr.disassemble():
                    print(f"  [DS]         {0x04001000 + off + 4:08X}: {ds_word:08X}  {ds_instr.disassemble()}")
            break
        if op == 0x03:  # jal — recurse
            target = jal_target(word)
            if target is not None and 0 <= target < len(imem) and target not in seen:
                print(f"  -> following jal to IMEM 0x{target:04X}")
                walk_function(target, max_steps, seen)

        off += 4
        steps += 1
    return seen

print("=== Walking op 0x06's handler tree (IMEM 0x12A0) ===\n")
print("Looking for any mtc0 (DMA), uses of $20 (RAM source), or follow-jal:\n")
walk_function(0x12A0)
