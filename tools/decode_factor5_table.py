"""Decode Factor5 ucode dispatch table at DMEM 0xD6 (op<0x40) and 0x64 (op>=0x40)."""
import struct, sys

with open("factor5_ucode_data.bin", "rb") as f:
    data = f.read()

# Table at 0xD6: 64 halfwords for opcodes 0x00..0x3F (indexed by op*2)
print("Dispatch table 1 (DMEM 0xD6, opcodes 0x00..0x3F):")
for op in range(0x40):
    off = 0xD6 + op * 2
    addr = struct.unpack(">H", data[off:off+2])[0]
    if addr != 0:
        print(f"  op_{op:02X} -> 0x{addr:04X}")

print()
print("Dispatch table 2 (DMEM 0x64, opcodes 0xC0..0xFF reversed?):")
for op in range(0x40):
    off = 0x64 + op * 2
    addr = struct.unpack(">H", data[off:off+2])[0]
    if addr != 0:
        print(f"  entry[{op:02X}] (off 0x{off:03X}) -> 0x{addr:04X}")
