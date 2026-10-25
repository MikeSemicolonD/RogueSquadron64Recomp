#!/usr/bin/env python3
"""Free display-list chunk count from an RDRAM dump, as countDisplayListChunks (0x80007CB8) sees it:
walk the free list at 0x801163B0 (first word = next), 0x108 bytes per chunk. spawnOrientedNpc and the
other budget-gated effect spawners require >= 0x4E20 bytes (76 chunks); PJ64 goldens sit at 900+.
Usage: spawn_budget.py dump.bin [dump2.bin ...]"""
import struct, sys

GATE = 0x4E20
for path in sys.argv[1:]:
    d = open(path, 'rb').read()
    W = lambda a: struct.unpack('>I', d[a:a + 4])[0]
    p = W(0x1163B0) & 0xFFFFFF
    n = 0
    while p and p + 4 <= len(d) and n < 100000:
        n += 1
        p = W(p) & 0xFFFFFF
    print(f"{path}: free DL chunks={n} bytes={n * 0x108} gate(>=0x{GATE:X})={'pass' if n * 0x108 >= GATE else 'FAIL'}")
