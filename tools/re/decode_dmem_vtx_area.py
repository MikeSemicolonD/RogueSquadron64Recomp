"""Check DMEM[0x01C0..] (where op 0x02's payload byte 2-3 = 0x01C0 points)
for vertex data — this is the candidate vertex source per the op 0x02
analysis. DMEM dump is 2 KB total, captured at task entry (before any DL
processing); the vertex region might be pre-baked into the ucode_data
segment OR populated by other ops we haven't decoded yet.
"""

import os
import struct

ROOT = r"E:/Projects/RogueSquadron64Recomp"
with open(os.path.join(ROOT, "dumps", "f5_ucode.dmem.bin"), "rb") as f:
    dmem = f.read()

print(f"DMEM size: {len(dmem)} bytes")

# Hex dump around 0x01C0
print("\n=== DMEM 0x0180..0x0280 (around offset 0x01C0) ===")
start = 0x0180
end = 0x0280
for off in range(start, min(end, len(dmem)), 16):
    hexstr = " ".join(f"{b:02X}" for b in dmem[off:off+16])
    ascstr = "".join(chr(b) if 32 <= b < 127 else "." for b in dmem[off:off+16])
    marker = "  <- 0x01C0" if off == 0x01C0 else ""
    print(f"  {off:04X}: {hexstr}  |{ascstr}|{marker}")

# Try interpreting DMEM[0x01C0..0x01C0+128*4] as vertex coords
# (loop count = 128, each iter likely processes 1 vertex)
print("\n=== DMEM[0x01C0..] as 16-byte N64 vertex format (first 16 vertices) ===")
for i in range(16):
    off = 0x01C0 + i * 16
    if off + 16 > len(dmem):
        break
    x, y, z, flag = struct.unpack(">hhhh", dmem[off:off+8])
    s, t = struct.unpack(">hh", dmem[off+8:off+12])
    r, g, b, a = dmem[off+12], dmem[off+13], dmem[off+14], dmem[off+15]
    print(f"  v{i:3d} @0x{off:04X}: pos=({x:6d},{y:6d},{z:6d}) flag={flag:5d}  st=({s:6d},{t:6d})  rgba=({r:3d},{g:3d},{b:3d},{a:3d})")

# Also check 8-byte vertex format (just xyz + rgba packed)
print("\n=== DMEM[0x01C0..] as compact 8-byte format (first 24 entries) ===")
for i in range(24):
    off = 0x01C0 + i * 8
    if off + 8 > len(dmem):
        break
    x, y, z, w = struct.unpack(">hhhh", dmem[off:off+8])
    print(f"  e{i:3d} @0x{off:04X}: ({x:6d},{y:6d},{z:6d},{w:6d})")

# Look at the entire 2KB DMEM for non-zero regions
print("\n=== Non-zero region map of DMEM (32-byte rows) ===")
for off in range(0, len(dmem), 32):
    chunk = dmem[off:off+32]
    nz = sum(1 for b in chunk if b != 0)
    if nz > 0:
        hexstr = " ".join(f"{b:02X}" for b in chunk[:32])
        if nz < 32:
            label = f"{nz}/32"
        else:
            label = "ALL"
        print(f"  {off:04X}: [{label} nz] {hexstr}")
