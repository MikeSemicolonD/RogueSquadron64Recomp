"""Brute-force scan of captured RAM dumps for vertex-like patterns.

Vertex data heuristic: blocks of s16 triplets (x, y, z) or quads (x, y, z, w)
where all values are in a plausible coordinate range. For attribution-glyph
geometry the range should be modest (letters are small) — maybe [-200, +200]
in normalized coords or [-2000, +2000] if pre-projection.

Also try interpretation as F3D vertex format (16 bytes):
  s16 x, y, z;  u16 flag;  s16 s, t;  u8 r, g, b, a;

Report regions with high vertex-density.
"""

import os
import struct

ROOT = r"E:/Projects/RogueSquadron64Recomp"

DUMPS = [
    ("0x80700000", os.path.join(ROOT, "dumps", "f5_vtxdata.0x00700000.bin")),
    ("0x80710000", os.path.join(ROOT, "dumps", "f5_vtxdata.0x00710000.bin")),
]

def is_plausible_coord(v, max_abs=2000):
    """s16 value v is a plausible vertex coord."""
    return -max_abs <= v <= max_abs and v != 0

def scan_as_vertex16(buf, label):
    """Scan as 16-byte F3D vertex array. Report runs of plausible vertices."""
    print(f"\n=== {label}: 16-byte F3D vertex scan ===")
    run_start = None
    run_count = 0
    best_runs = []
    for off in range(0, len(buf) - 16, 16):
        x, y, z = struct.unpack(">hhh", buf[off:off+6])
        # All three nonzero AND in plausible range → likely a vertex
        plausible = (
            -2000 < x < 2000 and -2000 < y < 2000 and -2000 < z < 2000
            and (abs(x) + abs(y) + abs(z)) > 5  # at least some magnitude
        )
        if plausible:
            if run_start is None:
                run_start = off
            run_count += 1
        else:
            if run_count >= 4:  # at least 4 consecutive plausible vertices
                best_runs.append((run_start, run_count))
            run_start = None
            run_count = 0
    if run_count >= 4:
        best_runs.append((run_start, run_count))
    print(f"  Found {len(best_runs)} runs of 4+ plausible 16-byte vertices:")
    for start, count in best_runs[:10]:
        print(f"    @ 0x{start:04X}: {count} vertices")
        for i in range(min(count, 4)):
            v_off = start + i * 16
            x, y, z, flag = struct.unpack(">hhhh", buf[v_off:v_off+8])
            print(f"      v{i}: ({x:5d},{y:5d},{z:5d}) flag={flag}")

def scan_as_short_triples(buf, label):
    """Scan for runs of (x, y, z) s16 triples — 6 bytes per vertex."""
    print(f"\n=== {label}: 6-byte s16 triple scan ===")
    run_start = None
    run_count = 0
    best_runs = []
    for off in range(0, len(buf) - 6, 6):
        x, y, z = struct.unpack(">hhh", buf[off:off+6])
        plausible = (
            -1000 < x < 1000 and -1000 < y < 1000 and -1000 < z < 1000
            and (abs(x) + abs(y) + abs(z)) > 3
        )
        if plausible:
            if run_start is None:
                run_start = off
            run_count += 1
        else:
            if run_count >= 6:  # 6+ consecutive
                best_runs.append((run_start, run_count))
            run_start = None
            run_count = 0
    if run_count >= 6:
        best_runs.append((run_start, run_count))
    print(f"  Found {len(best_runs)} runs of 6+ plausible 6-byte triples:")
    for start, count in best_runs[:10]:
        print(f"    @ 0x{start:04X}: {count} triples")

def general_density(buf, label):
    """Find any region with notable non-zero density (could be data)."""
    print(f"\n=== {label}: non-zero density by 1KB block ===")
    for off in range(0, len(buf), 1024):
        chunk = buf[off:off+1024]
        nz = sum(1 for b in chunk if b != 0)
        if nz > 8:
            print(f"  @ 0x{off:04X}: {nz}/1024 non-zero bytes")
            # Show the non-zero offsets in this block
            for i, b in enumerate(chunk):
                if b != 0:
                    abs_off = off + i
                    if abs_off < off + 64:  # only first few
                        # Read 16 bytes context
                        ctx = buf[abs_off:abs_off+16]
                        hex_ctx = " ".join(f"{x:02X}" for x in ctx)
                        print(f"    nz @ 0x{abs_off:04X}: {hex_ctx}")
                        break

for label, path in DUMPS:
    if not os.path.exists(path):
        print(f"{label}: file missing: {path}")
        continue
    with open(path, "rb") as f:
        buf = f.read()
    print(f"\n{'='*60}")
    print(f"DUMP: {label} ({len(buf)} bytes)")
    print('='*60)
    general_density(buf, label)
    scan_as_vertex16(buf, label)
    scan_as_short_triples(buf, label)
