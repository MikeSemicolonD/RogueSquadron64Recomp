"""Emulate the Factor5 dispatch loop for op_01 to find the actual handler PC."""
import struct, sys

def be16(b, off):
    return struct.unpack(">H", b[off:off+2])[0]

def le16(b, off):
    return struct.unpack("<H", b[off:off+2])[0]

# Read data binary as if BE-formatted in DMEM (RSP's view)
with open("factor5_ucode_data.bin", "rb") as f:
    dmem_be = f.read()

# Alternate hypothesis: file is LE-u32, so RSP-view BE bytes are file bytes XOR-3 within word
def make_be_from_le_u32(b):
    out = bytearray(b)
    for i in range(0, len(b), 4):
        out[i:i+4] = b[i:i+4][::-1]
    return bytes(out)

dmem_swapped = make_be_from_le_u32(dmem_be)

def emulate_dispatch(w0, dmem):
    # mfc0 + lw + ... + andi 0x80 + bnez exit ...
    # srl $2, $19, 30
    top2 = (w0 >> 30) & 3
    # beq $2, 3 -> takes some branch (not table-lookup path)
    if top2 == 3:
        return ("0xC0+ branch (not a table lookup)", None)
    # bnez $2 -> table 2 path (offset 0x64) for opcodes 0x40..0xBF
    if top2 != 0:
        # path through negu
        sra_val = ((w0 >> 23) if w0 < 0x80000000 else
                   ((w0 >> 23) | (~0xFFFFFFFF >> 23 << 23) | 0xFFFFFE00)) & 0xFFFFFFFF
        # python: w0 >> 23 with sign extension
        sra_val = w0 >> 23
        if w0 & 0x80000000:
            sra_val = sra_val | 0xFFFFFE00  # sign-fill bits 31..9
            sra_val &= 0xFFFFFFFF
        negu = ((-sra_val) & 0xFFFFFFFF)
        idx = negu & 0x1FE
        target = be16(dmem, 0x64 + idx)
        return (f"table2 idx 0x{idx:X}", target)
    else:
        # top 2 bits = 0, opcode < 0x40, table 1 at 0xD6
        sra_val = w0 >> 23   # python int, opcode < 0x80000000 here so no sign issue
        idx = sra_val & 0x1FE
        target = be16(dmem, 0xD6 + idx)
        return (f"table1 idx 0x{idx:X}", target)

for op in [0x00, 0x01, 0x02, 0x05, 0x06, 0x14, 0x80, 0xB8, 0xBC, 0xE4, 0xFF]:
    w0 = (op << 24) | 0x000000  # arbitrary low bits
    label, t = emulate_dispatch(w0, dmem_be)
    label2, t2 = emulate_dispatch(w0, dmem_swapped)
    print(f"op 0x{op:02X}: {label}")
    print(f"  BE-file:   target=0x{t:04X}  masked-PC=0x{t&0xFFC:03X}")
    if t2 is not None:
        print(f"  swap-file: target=0x{t2:04X}  masked-PC=0x{t2&0xFFC:03X}")
