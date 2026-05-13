"""Scan the .data-segment dump at 0x80037000 for vertex-like patterns.
This region contains the state-setup sub-DL (0x80037658-0x800376C8) and
possibly static vertex arrays near it.
"""

import os
import struct

path = r"E:/Projects/RogueSquadron64Recomp/dumps/f5_vtxdata.0x00037000.bin"
with open(path, "rb") as f:
    buf = f.read()
print(f"DUMP: 0x80037000 ({len(buf)} bytes)")

# Density by 1KB blocks
print("\n=== Non-zero density by 1KB block ===")
for off in range(0, len(buf), 1024):
    chunk = buf[off:off+1024]
    nz = sum(1 for b in chunk if b != 0)
    if nz > 50:
        print(f"  @ 0x{off:04X}-0x{off+1024:04X}: {nz}/1024 non-zero")

# Look for plausible vertex regions
print("\n=== Plausible vertex runs (16-byte F3D format) ===")
run_start = None
run_count = 0
runs = []
for off in range(0, len(buf) - 16, 16):
    x, y, z = struct.unpack(">hhh", buf[off:off+6])
    plausible = (
        -2000 < x < 2000 and -2000 < y < 2000 and -2000 < z < 2000
        and (abs(x) + abs(y) + abs(z)) > 5
    )
    if plausible:
        if run_start is None:
            run_start = off
        run_count += 1
    else:
        if run_count >= 4:
            runs.append((run_start, run_count))
        run_start = None
        run_count = 0
if run_count >= 4:
    runs.append((run_start, run_count))

print(f"  Found {len(runs)} runs of 4+ plausible 16-byte vertices")
for start, count in runs[:15]:
    abs_addr = 0x80037000 + start
    print(f"\n  Run @ 0x{abs_addr:08X}: {count} vertices")
    for i in range(min(count, 6)):
        v_off = start + i * 16
        x, y, z, flag = struct.unpack(">hhhh", buf[v_off:v_off+8])
        s, t = struct.unpack(">hh", buf[v_off+8:v_off+12])
        r, g, b, a = buf[v_off+12], buf[v_off+13], buf[v_off+14], buf[v_off+15]
        print(f"    v{i:2d}: pos=({x:5d},{y:5d},{z:5d}) flag={flag:5d}  "
              f"st=({s:5d},{t:5d})  rgba=({r:3d},{g:3d},{b:3d},{a:3d})")

# Also look at what's right at the sub-DL location to compare
print("\n=== Context around sub-DL at 0x80037658 ===")
sub_dl_off = 0x658
for off in range(sub_dl_off - 0x20, sub_dl_off + 0x80, 16):
    abs_addr = 0x80037000 + off
    hexstr = " ".join(f"{b:02X}" for b in buf[off:off+16])
    print(f"  0x{abs_addr:08X}: {hexstr}")
