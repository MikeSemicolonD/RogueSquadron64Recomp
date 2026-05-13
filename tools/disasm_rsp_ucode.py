"""Disassemble F3DFACTOR5 RSP ucode + locate opcode 0x02 handler.

Dispatch logic (decoded from IMEM 0x1024-0x1054):
  - srl $2, $19, 30                       # extract top 2 bits of byte 0
  - beq top2bits, 3 -> 0x1074 (RDP buffering for opcodes 0xC0..0xFF)
  - bnez top2bits -> 0x1048               # opcodes 0x40..0xBF go to table 2
  - sra $2, $19, 23                       # delay slot: prepare offset
  - andi $2, $2, 0x1FE                    # mask
  - lhu $2, 0xD6($2)                      # TABLE 1 at DMEM[0xD6] for opcodes 0x00..0x3F
  - jr $2
0x1048:
  - negu $2, $2
  - andi $2, $2, 0x1FE
  - lhu $2, 0x64($2)                      # TABLE 2 at DMEM[0x64] for opcodes 0x40..0xBF
  - jr $2

For opcode X (0x00..0x3F): offset into table1 = X * 2
For opcode X (0x40..0xBF): offset into table2 = (256*2 - X*2) & 0x1FE
"""

import os
import struct
import rabbitizer

ROOT = r"E:/Projects/RogueSquadron64Recomp"
IMEM = os.path.join(ROOT, "dumps", "f5_ucode.imem.bin")
DMEM = os.path.join(ROOT, "dumps", "f5_ucode.dmem.bin")

with open(IMEM, "rb") as f:
    imem = f.read()
with open(DMEM, "rb") as f:
    dmem = f.read()

instr_category = getattr(rabbitizer.InstrCategory, "RSP",
                         rabbitizer.InstrCategory.CPU)

TABLE1_BASE = 0xD6   # DMEM offset for opcodes 0x00..0x3F
TABLE2_BASE = 0x64   # DMEM offset for opcodes 0x40..0xBF

def handler_offset(opcode):
    """Return IMEM offset of the handler for the given opcode byte."""
    top2 = (opcode >> 6) & 3
    if top2 == 0:
        # Table 1: DMEM[0xD6 + (opcode << 1)]
        off = TABLE1_BASE + (opcode << 1)
    elif top2 == 3:
        # RDP buffering, not dispatch
        return None
    else:
        # Table 2: DMEM[0x64 + ((-opcode) << 1) & 0x1FE]
        # In the disassembly: sra $19, 23 gives (opcode << 1) sign-extended,
        # then negu gives a positive offset.
        # For X with bit 7 set:   sra 23 = 0xFFFFFE00 | (X<<1)  -> negu gives 0x200 - (X<<1)
        # For X with bit 7 clear: sra 23 = (X<<1) positive       -> negu gives 0x100000000-(X<<1) trunc to 32b = 0xFFFFFE00+(X<<1)... no wait
        # Actually simpler: $2 = sra $19 23. For top-byte = X:
        #   if X & 0x80:    $2 = 0xFFFFFE00 | (X<<1)
        #   if X & 0x80 == 0:    $2 = (X<<1), positive  (but we know top2 != 0 here)
        # Since we're in path "top2 != 0", X >= 0x40. Subcases:
        #   X in 0x40..0x7F: $2 = (X<<1) in 0x80..0xFE (no sign extend)
        #     negu = -(X<<1) two's complement, then & 0x1FE = (0x200 - (X<<1)) & 0x1FE
        #   X in 0x80..0xBF: $2 = 0xFFFFFE00 | (X<<1)
        #     negu = -((0xFFFFFE00 | (X<<1))) = 0x200 - (X<<1) (in 32-bit modular)
        # Both subcases produce: offset = (0x200 - (X<<1)) & 0x1FE
        off = TABLE2_BASE + ((0x200 - (opcode << 1)) & 0x1FE)
    if off + 2 > len(dmem):
        return None
    # DMEM is big-endian halfwords from the RSP's POV; the dump preserves
    # the XOR-3 byteswap-undone layout when we read from RDRAM, so plain
    # >H struct unpack works.
    return struct.unpack(">H", dmem[off:off+2])[0]

def imem_word(off):
    if off + 4 > len(imem):
        return None
    return struct.unpack(">I", imem[off:off+4])[0]

def disasm_at(imem_off, n=12, prefix=""):
    lines = []
    for i in range(n):
        off = imem_off + i * 4
        if off + 4 > len(imem):
            break
        word = struct.unpack(">I", imem[off : off+4])[0]
        instr = rabbitizer.Instruction(word, 0x04001000 + off,
                                       category=instr_category)
        lines.append(f"{prefix}  {0x04001000 + off:08X}: {word:08X}  {instr.disassemble()}")
    return "\n".join(lines)

# Print handler IMEM offsets for the opcodes that matter
interesting = [0x00, 0x01, 0x02, 0x03, 0x06, 0x80, 0xAF, 0xB0, 0xB2,
               0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBC, 0xBE, 0xBF]
print("=== Handler IMEM offsets (F3DFACTOR5 dispatch) ===")
for op in interesting:
    h = handler_offset(op)
    if h is None:
        print(f"  op 0x{op:02X}: (RDP buffering, no dispatch)")
    else:
        in_range = "OK" if h < len(imem) else "OUT-OF-RANGE"
        print(f"  op 0x{op:02X}: handler IMEM 0x{h:04X}  [{in_range}]")

# Disassemble op 0x02 handler specifically
print("\n=== op 0x02 handler disassembly ===")
h02 = handler_offset(0x02)
if h02 is not None and h02 < len(imem):
    print(disasm_at(h02, 40))

# Compare with op 0x06 (G_DL — a known opcode) to validate dispatch math
print("\n=== op 0x06 (G_DL) handler disassembly (validation) ===")
h06 = handler_offset(0x06)
if h06 is not None and h06 < len(imem):
    print(disasm_at(h06, 12))

# Op 0x00 should be a no-op / return-to-loop
print("\n=== op 0x00 (G_NOOP) handler disassembly ===")
h00 = handler_offset(0x00)
if h00 is not None and h00 < len(imem):
    print(disasm_at(h00, 8))

# Also dump op 0x01 and 0x03 since they're in the same Factor 5 custom family
for op in [0x01, 0x03]:
    print(f"\n=== op 0x{op:02X} handler disassembly ===")
    h = handler_offset(op)
    if h is not None and h < len(imem):
        print(disasm_at(h, 20))
