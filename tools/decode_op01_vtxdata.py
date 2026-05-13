"""Decode the 64 KB regions at RAM 0x80700000 and 0x80710000 that
F3DFACTOR5 op 0x01 sources per attribution DL.

The op 0x01 payload was w0=0x01030040 with w1=alternating between
0x80700000 and 0x80710000 each DL. The b3=0x40 (=64) likely encodes
a byte count or vertex count. b1=0x03 may be sub-opcode or destination.

Goal: see what format the data takes — N64 vertex (16B: xyz+flag+texST+rgba),
matrix data (32B: int+frac halves), or some custom Factor 5 packing.
"""

import os
import struct

ROOT = r"E:/Projects/RogueSquadron64Recomp"
A = os.path.join(ROOT, "dumps", "f5_vtxdata.0x00700000.bin")
B = os.path.join(ROOT, "dumps", "f5_vtxdata.0x00710000.bin")

with open(A, "rb") as f:
    buf_a = f.read()
with open(B, "rb") as f:
    buf_b = f.read()

print(f"Buffer A (0x80700000): {len(buf_a)} bytes")
print(f"Buffer B (0x80710000): {len(buf_b)} bytes")
print(f"Identical? {buf_a == buf_b}")

# Quick zero/non-zero analysis
def analyze(buf, label):
    nonzero = sum(1 for b in buf if b != 0)
    print(f"\n=== {label} ===")
    print(f"  Total bytes: {len(buf)}, non-zero: {nonzero} ({nonzero*100//len(buf)}%)")
    # First non-zero run
    first_nz = None
    for i, b in enumerate(buf):
        if b != 0:
            first_nz = i
            break
    print(f"  First non-zero byte at offset 0x{first_nz:04X}" if first_nz is not None else "  All zero!")
    # Last non-zero
    last_nz = None
    for i in range(len(buf) - 1, -1, -1):
        if buf[i] != 0:
            last_nz = i
            break
    print(f"  Last non-zero byte at offset 0x{last_nz:04X}" if last_nz is not None else "")
    # Hex dump of first 256 bytes
    print("  First 256 bytes:")
    for off in range(0, min(256, len(buf)), 16):
        hexstr = " ".join(f"{b:02X}" for b in buf[off:off+16])
        ascstr = "".join(chr(b) if 32 <= b < 127 else "." for b in buf[off:off+16])
        print(f"    {off:04X}: {hexstr}  |{ascstr}|")

analyze(buf_a, "Buffer A (0x80700000)")
analyze(buf_b, "Buffer B (0x80710000)")

# Diff the two
print("\n=== Diff of first 256 bytes ===")
diffs = []
for i in range(min(256, len(buf_a), len(buf_b))):
    if buf_a[i] != buf_b[i]:
        diffs.append(i)
print(f"  First-256 bytes differ at {len(diffs)} positions: {diffs[:32]}")

# Try interpreting as N64 vertex format
# Standard N64 vertex layout (16 bytes):
#   s16 x, s16 y, s16 z
#   s16 flag (often unused)
#   s16 s, s16 t  (texture coords, 10.5 fixed point)
#   u8 r, u8 g, u8 b, u8 a  (color)
# Total: 16 bytes per vertex.
print("\n=== Buffer A as N64 vertex array (first 8 vertices) ===")
for i in range(8):
    off = i * 16
    if off + 16 > len(buf_a):
        break
    x, y, z, flag = struct.unpack(">hhhh", buf_a[off:off+8])
    s, t = struct.unpack(">hh", buf_a[off+8:off+12])
    r, g, b, a = buf_a[off+12], buf_a[off+13], buf_a[off+14], buf_a[off+15]
    print(f"  v{i}: pos=({x:6d},{y:6d},{z:6d}) flag={flag:5d}  st=({s:6d},{t:6d})  rgba=({r:3d},{g:3d},{b:3d},{a:3d})")

# Op 0x01 payload was w0=0x01030040 with b3=0x40=64. If b3 is count of bytes,
# that's 64 bytes = 4 N64 vertices. Or 1 4x4 fixed-point matrix.
# Try interpreting first 64 bytes as a 4x4 matrix (N64 mtx format: int hi + frac lo halves).
print("\n=== Buffer A as 4x4 N64 matrix (first 64 bytes; int + frac halves) ===")
# N64 matrix format: 8 rows of int16[4], then 8 rows of uint16[4] fractions
# Combined value = (int << 16) | frac, interpreted as s15.16 fixed-point
ints = struct.unpack(">16h", buf_a[0:32])
fracs = struct.unpack(">16H", buf_a[32:64])
for row in range(4):
    line = "  "
    for col in range(4):
        idx = row * 4 + col
        combined = (ints[idx] << 16) | fracs[idx]
        # Sign-extend
        if combined & 0x80000000:
            combined -= 1 << 32
        fval = combined / 65536.0
        line += f" {fval:9.4f}"
    print(line)

# Same for buffer B
print("\n=== Buffer B as 4x4 N64 matrix ===")
ints = struct.unpack(">16h", buf_b[0:32])
fracs = struct.unpack(">16H", buf_b[32:64])
for row in range(4):
    line = "  "
    for col in range(4):
        idx = row * 4 + col
        combined = (ints[idx] << 16) | fracs[idx]
        if combined & 0x80000000:
            combined -= 1 << 32
        fval = combined / 65536.0
        line += f" {fval:9.4f}"
    print(line)

# Look for plausible vertex sequences further in
print("\n=== Search for plausible vertex coords in Buffer A ===")
# Heuristic: 16-byte aligned, x,y,z all in range [-1000, 1000]
candidates = 0
for off in range(0, min(8192, len(buf_a) - 16), 16):
    x, y, z = struct.unpack(">hhh", buf_a[off:off+6])
    if -1000 < x < 1000 and -1000 < y < 1000 and -1000 < z < 1000 and (x or y or z):
        if candidates < 16:
            print(f"  off=0x{off:04X}: x={x:5d} y={y:5d} z={z:5d}")
        candidates += 1
print(f"  total plausible-vertex candidates in first 8KB: {candidates}")
