"""Dump DMEM contents (BE byte order corrected from LE-u32 file)."""
import struct

with open("factor5_ucode_data.bin","rb") as f:
    data = f.read()

# Build BE-byte mem image
mem = bytearray(len(data))
for i in range(0, len(data), 4):
    mem[i:i+4] = data[i:i+4][::-1]

print("BE-corrected DMEM dump (first 0x100):")
for i in range(0, 0x100, 16):
    hexb = ' '.join(f'{b:02X}' for b in mem[i:i+16])
    asc = ''.join(chr(b) if 32 <= b < 127 else '.' for b in mem[i:i+16])
    print(f"  {i:03X}  {hexb}  {asc}")

print("\nDispatch table 1 (mem 0xD6, opcodes 0x00-0x3F by idx*2):")
for op in range(0x40):
    off = 0xD6 + op * 2
    val = struct.unpack(">H", mem[off:off+2])[0]
    pc = val & 0xFFC
    print(f"  op_{op:02X} idx*2=0x{op*2:02X}  raw=0x{val:04X}  PC=0x{pc:03X}")
