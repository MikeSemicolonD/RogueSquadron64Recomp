#!/usr/bin/env python3
"""Decode the Factor 5 render rodata tables from the built ELF.

Reads `llvm-objdump -s` over roguesquadron.elf, builds a VRAM->bytes map, then
decodes the material render-state tables that the recomp reads at runtime but
does NOT inline (they live in rodata, fetched via MEM_W):

  - combiner table   @0x80037860  (512 x 8B {w0,w1} G_SETCOMBINE pairs)
  - other-mode LUT   @0x80038860  (128 x 1B, indexed by assembled mode & 0x7F)
  - fmt/siz switch jump tables @0x80000990 / 0x800009A8 (code addrs, not data)
  - cubic-spline basis coeffs  @0x800186F4 (5 floats; Catmull-Rom 0.5..2.5)

Usage: python tools/re/render/decode_rodata_tables.py [path/to/roguesquadron.elf]
Read-only. Prints a report to stdout.
"""
import subprocess, sys, struct, os

ELF = sys.argv[1] if len(sys.argv) > 1 else r"E:/Projects/rogue_squadron64/build/roguesquadron.elf"
OBJDUMP = r"C:/Program Files/LLVM/bin/llvm-objdump"

def load_mem(elf):
    """Return dict {vram_word_addr: 4 bytes big-endian} from objdump -s."""
    out = subprocess.run([OBJDUMP, "-s", elf], capture_output=True, text=True).stdout
    mem = {}
    for line in out.splitlines():
        line = line.strip()
        parts = line.split()
        # lines look like: "80037860 fc127e24 ffffffff fc127e24 fffff9fc  ..~$..."
        if len(parts) < 2: continue
        try:
            base = int(parts[0], 16)
        except ValueError:
            continue
        if base < 0x80000000 or base > 0x80800000: continue
        col = 0
        for w in parts[1:]:
            if len(w) == 8 and all(c in "0123456789abcdef" for c in w):
                mem[base + col*4] = bytes.fromhex(w)
                col += 1
            else:
                break
    return mem

def w32(mem, addr):
    b = mem.get(addr)
    return struct.unpack(">I", b)[0] if b else None

# --- RDP color-combiner mux names (libultra gbi.h) ---
A = ["COMBINED","TEX0","TEX1","PRIM","SHADE","ENV","1","NOISE"]+["0"]*8        # 4-bit a
B = ["COMBINED","TEX0","TEX1","PRIM","SHADE","ENV","CENTER","K4"]+["0"]*8       # 4-bit b
C = ["COMBINED","TEX0","TEX1","PRIM","SHADE","ENV","SCALE","COMB_A","TEX0_A",   # 5-bit c
     "TEX1_A","PRIM_A","SHADE_A","ENV_A","LOD_FRAC","PRIM_LOD","K5"]+["0"]*16
D = ["COMBINED","TEX0","TEX1","PRIM","SHADE","ENV","1","0"]                     # 3-bit d
AA = ["COMBINED","TEX0","TEX1","PRIM","SHADE","ENV","1","0"]                    # 3-bit alpha a/b/d
AC = ["LOD_FRAC","TEX0","TEX1","PRIM","SHADE","ENV","PRIM_LOD","0"]             # 3-bit alpha c

def decode_combine(w0, w1):
    a0=(w0>>20)&0xF; c0=(w0>>15)&0x1F; Aa0=(w0>>12)&0x7; Ac0=(w0>>9)&0x7
    a1=(w0>>5)&0xF;  c1=w0&0x1F
    b0=(w1>>28)&0xF; b1=(w1>>24)&0xF; Aa1=(w1>>21)&0x7; Ac1=(w1>>18)&0x7
    d0=(w1>>15)&0x7; Ab0=(w1>>12)&0x7; Ad0=(w1>>9)&0x7
    d1=(w1>>6)&0x7;  Ab1=(w1>>3)&0x7;  Ad1=w1&0x7
    c = f"c0:({A[a0]}-{B[b0]})*{C[c0]}+{D[d0]}  a0:({AA[Aa0]}-{AA[Ab0]})*{AC[Ac0]}+{AA[Ad0]}"
    c2= f"c1:({A[a1]}-{B[b1]})*{C[c1]}+{D[d1]}  a1:({AA[Aa1]}-{AA[Ab1]})*{AC[Ac1]}+{AA[Ad1]}"
    return c + " | " + c2

def main():
    if not os.path.exists(ELF):
        print(f"ELF not found: {ELF}"); return
    mem = load_mem(ELF)
    print(f"# loaded {len(mem)} words from {ELF}\n")

    print("## Combiner table @0x80037860 (distinct non-default recipes)")
    print("idx = assembled material-mode bits; entry = gsDPSetCombine(w0,w1)\n")
    seen = {}
    default = (0xfcffffff, 0xffffffff)
    for i in range(512):
        a = 0x80037860 + i*8
        w0, w1 = w32(mem,a), w32(mem,a+4)
        if w0 is None: break
        if (w0,w1) == default: continue
        key = (w0,w1)
        seen.setdefault(key, []).append(i)
    print(f"{'(default fcffffff/ffffffff used by all other slots)'}\n")
    for (w0,w1), idxs in sorted(seen.items(), key=lambda kv: kv[1][0]):
        ids = ",".join(f"0x{i:x}" for i in idxs[:12]) + (" ..." if len(idxs)>12 else "")
        print(f"[{ids}]  {w0:08x} {w1:08x}")
        print(f"    {decode_combine(w0,w1)}")
    print(f"\n# {len(seen)} distinct non-default combiner recipes\n")

    print("## Other-mode LUT @0x80038860 (128 bytes, index = mode & 0x7F)")
    row = []
    for i in range(128):
        b = mem.get(0x80038860 + (i & ~3))
        if b is None: continue
        row.append(b[i & 3])
    print(" ".join(f"{v:02x}" for v in row[:128]) if row else "(not found)")
    print()

    print("## fmt/siz switch jump tables (CODE addresses, not data nibbles)")
    for name, base, n in [("fmt @0x80000990",0x80000990,6),("siz @0x800009A8",0x800009A8,6)]:
        vals = [w32(mem, base+i*4) for i in range(n)]
        print(f"  {name}: " + " ".join(f"{v:08x}" if v else "----" for v in vals))
    print("  (the actual G_IM_FMT/G_IM_SIZ literals are emitted in loadTextureTile's case arms)\n")

    print("## Cubic-spline basis coeffs @0x800186F4 (expect 0.5,1.0,1.5,2.0,2.5)")
    fl = []
    for i in range(5):
        w = w32(mem, 0x800186F4 + i*4)
        fl.append(struct.unpack(">f", struct.pack(">I", w))[0] if w else None)
    print("  " + ", ".join(f"{x:g}" for x in fl if x is not None))

if __name__ == "__main__":
    main()
