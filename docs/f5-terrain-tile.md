# Factor 5 ucode: op 0x05 grid terrain tile, and how its LOD seams work

Sources: the overlay listings ovl0C/10/14/18/1C.asm (runtime IMEM 0x1DB0+). Overlay 0x20 was read from imem_full.asm
image offset 0x1AF8 and runs at IMEM 0x12A0 (DMEM overlay table entry 0x20 = size 0x2157, so dest index 2 means 0x78, i.e. 0x12A0).
Overlay 0x28 is image 0x1D38 and runs at 0x1DB0. Game side: `renderFlatMeshFaceGroup` (0x8001108C, builds the record) and
`buildGridCellMeshForFace` (0x8004317C, decides the LOD, neighbour bytes and weights).
All ucode addresses below are runtime IMEM addresses. The notation "ovlXX@addr" means that overlay's listing.

Entry: the dispatcher does `lw $19,0($17); lw $20,4($17); addiu $17,$17,8` (0x1094/0x1098/0x10B0), and op 05 is `j 0x1268; ori $5,0xC`
(0x15AC). So inside every overlay `$17 = rec + 8`. No overlay changes $17 until ovl28@0x1DB0 does `addiu $17,$17,0x20`, which makes
the record 40 bytes. The loader (0x1268..0x129C) clobbers only $2..$5. **$10 = M carries across overlays.**

## (a) Record layout (40 bytes, offsets from record start)

| off | size | meaning | ucode reader | game writer (0x80011714..0x80011880; t = tile struct) |
|---|---|---|---|---|
| +0 | u8 | 0x05 opcode | dispatcher | const |
| +1 | u8 | **N** = source samples per row (5) | ovl0C@1E14 `lbu -7` | t+0x20 low byte |
| +2 | u8 | flags: bit1 = flat form; bit0 = depth-sort direction; bits 7..4 = **L** = color LOD shift | ovl0C@1DB0 (&2), @1E1C (>>4); ovl1C@1DB0 (&1) | `(t+0x15 & 0xF)<<4 \| (t+0x2E & 1)` |
| +3 | u8 | **s** = this tile's LOD = height stride shift. All four seam tests compare against this byte | ovl0C@1E18, ovl14@1DF8, ovl18@1DF0, ovl1C@1E44 | t+0x10 |
| +4 | u8 | nb **x-min** edge (column i=0) | ovl18@1DEC (`lbu -4($17+$29)`, $29=0); ovl20@12A0 | t+0x11 |
| +5 | u8 | nb **x-max** edge (column i=M-1) | ovl18@1DEC ($29=1); ovl20@12DC | t+0x12 |
| +6 | u8 | nb **z-min** edge (row j=0) | ovl14@1DF4 (`lbu -2($17+$29)`, $29=0); ovl1C@1E50 | t+0x13 |
| +7 | u8 | nb **z-max** edge (row j=M-1) | ovl14@1DF4 ($29=1); ovl1C@1E8C | t+0x14 |
| +8 | u32 | height pointer (low 3 bits = DMEM misalignment) | ovl0C@1DC0/1DF4 | t+0x24 |
| +0xC | u32 | color pointer | ovl0C@1DDC/1DF8 | t+0x28 |
| +0x10 | u16 | height DMA bytes = N*N | ovl0C@1DBC | (N*N)<<16 |
| +0x12 | u16 | color DMA bytes = Mc*Mc*4, Mc = ((N-1)>>L)+1 | ovl0C@1DD8 | 0x800117A4..C4 |
| **+0x14** | u16 | **interior weight** | ovl10@1DB0 | t+0x16 |
| +0x16 | u16 | weight, x-min edge | ovl18@1DE8 (`lhu 0xE($17+2*$29)`) | t+0x18 |
| +0x18 | u16 | weight, x-max edge | ovl18@1DE8 | t+0x1A |
| +0x1A | u16 | weight, z-min edge | ovl14@1DF0 (`lhu 0x12($17+2*$29)`) | t+0x1C |
| +0x1C | u16 | weight, z-max edge | ovl14@1DF0 | t+0x1E |
| +0x1E | u16 | texcoord step per sample | ovl10@1F44 | `((texdim-1) << (s+5)) / (N-1)` |
| +0x20 | s16 | x | ovl10@1EB8 | t+0x8 |
| +0x22 | s16 | y>>4 (the ucode shifts it left by 4) | ovl10@1EB4/1EC0 | t+0xA |
| +0x24 | s16 | z | ovl10@1EBC | t+0xC |
| +0x26 | u16 | size = spacing between vertices = base << s | ovl10@1EB0 | `t+0x22 << s` |

Corrections to our HLE (f5_tile_grid):
- The edge mapping L/R/T/B = +4/+5/+6/+7, with weights at +0x16/+0x18/+0x1A/+0x1C, is **correct**. Here L means x-min (column 0), R means x-max, T means z-min (row 0) and B means z-max.
- The comparison is against byte3 `s`. That is correct too.
- **Missing: +0x14 is the interior morph weight** (rec[2].w1 >> 16). We ignore it.
- **Missing: the "equal" branch.** When nb == s and w != 0 the ucode still morphs that edge (see (c)). Our `continue` skips it.
  **This is the hole:** the coarse side of an LOD boundary always takes this branch.
- The weights are 0.16 fractions: w = trunc(f * 65535.0). The constant 65535.0 is at 0x8003A510 (0x80043414..0x80043468).

## (b) Overlay order for a grid tile (`05 05 00 xx`)

1. **0x0C** @0x1DB0: DMAs heights to DMEM 0x4E0 and colors to 0x380, then builds the M*M height pool at 0xCB4 (s16 each, height = byte<<4)
   and the M*M color pool at 0xB70 (RGBA8). It computes M = ((N-1)>>s)+1 (1E44..1E4C). Then `j 0x1268, $5=0x14` (1F5C).
2. **0x14** @0x1DB0: seams on **rows j=0 and j=M-1** (the z-min and z-max edges). Then loads 0x18 (1F0C).
3. **0x18** @0x1DB0: seams on **columns i=0 and i=M-1** (the x-min and x-max edges). Then loads 0x10 (1F5C).
4. **0x10** @0x1DB0: interior morph (1DB0..1EAC), then builds the M*M vertices at DMEM 0x280 (1EB0..1F2C). It transforms them with the resident
   routine `j 0x15D0` (returns to 0x1F40; output is 0x28-byte vertices at DMEM 0x670), then writes colors and texcoords (1F40..1FB8). Then loads 0x1C.
5. **0x1C** @0x1DB0: depth-sorts the 2(M-1)^2 cell triangles (key = mean of the three +0x1C screen-z values, times DMEM 0x74 = 0x5555).
   It adds z-edge crack slivers (1E44..1ECC), then loads 0x20.
6. **0x20** @0x12A0: adds x-edge slivers (12A0..131C), then walks the sorted list and emits each triangle through 0x17F0 (1394..13F0). Then loads 0x28.
7. **0x28** @0x1DB0: `$17 += 0x20`, reloads the resident 0x12A0 code (image+0x220, 0x330 bytes), and returns to 0x10D8.

Flat form (byte2 bit1): 0x0C@1F64..1FD0 builds 4 corners (heights = halfwords at rec+4, +6, +8, +0xA) and loads 0x24. It has no seam logic.

## (c) Exact per-vertex arithmetic

Notation: `H[j][i]` is the height pool 0xCB4 + 2*(j*M+i), and `C[j][i][4]` is the color pool 0xB70 + 4*(j*M+i). j is the row and runs along z;
i is the column and runs along x (see (f)). Every operation below is applied identically to the height and to each of the 4 color bytes.
Color bytes are loaded with lbv into the low byte of each lane, so they act as values 0..255. Wrapping and clamping never matter in practice.

RSP fixed-point primitives (the accumulator is 48-bit; a signed value multiplied by an unsigned 16-bit value):
```c
// MEAN(a,b): vaddc + vmudm by v31[5]=0x8000
static int MEAN(int a, int b) { return floor_div((a + b) * 0x8000, 65536); }   // = (a+b)>>1, arithmetic
// LERP4(a,b,f): vmudm a*(0x10000-f) ; vmadm b*f  -> acc = a*(65536-f) + b*f  (exact 32-bit)
// MORPH(acc, h, w): ovl14@1F9C..1FB0 / ovl18@1FA0..1FB4
static int MORPH(int32_t acc, int h, unsigned w) {        // w in 1..0xFFFF, (1-w) = (0x10000-w)&0xFFFF
    int32_t hi = acc >> 16;             // vmadm result (clamped s16)
    uint32_t lo = acc & 0xFFFF;         // vsar e=10: accumulator low
    int64_t a2 = ((int64_t)lo * w >> 16) + (int64_t)hi * w;   // vmudl then vmadm
    a2 += (int64_t)h * ((0x10000 - w) & 0xFFFF);              // vmadm
    return clamp_s16(a2 >> 16);
}
```

### Edge pass, identical for each of the 4 edges
ovl14 handles rows j=0 then j=M-1: `$29=0/1`, base `$27 = 0xCB4 + $29*(M-1)*M*2` (1DBC..1DE0, 1F00). The stride along the edge is 1 vertex.
ovl18 handles columns i=0 then i=M-1: base `0xCB4 + $29*(M-1)*2` (1DC0..1DD8, 1F50). The stride along the edge is M vertices.
`E[k]` below is the k-th vertex along the edge, k = 0..M-1. Corners (k=0, k=M-1) are never written.
```c
nb = rec[4 + edgeIndex];   w = rec_u16[weightOff];   s = rec[3];
if (nb != s) {                                    // ovl14@1DFC..1E2C / ovl18@1DF4..1E64  ("differs")
    for (k = 1; k != M; k += 2)                   // odd k -> midpoint of its neighbours (pre-morph values)
        E[k] = MEAN(E[k-1], E[k+1]);              // subroutine ovl14@1F14 / inline ovl18@1E1C..1E5C
    if (w != 0)                                   // ovl14@1E34 / ovl18@1E6C
        for (k = 1; k < M-1; ++k) {               // ovl14@1E48..1E98 / ovl18@1E8C..1EF4
            if ((k & 3) == 0) continue;
            f  = (k & 3) << 14;                   // 0x4000/0x8000/0xC000
            k0 = k & ~3;                          // 4-sample span: E[k0]..E[k0+4]
            E[k] = MORPH(E[k0]*(0x10000-f) + E[k0+4]*f, E[k], w);   // ovl14@1F60 / ovl18@1F64
        }
} else if (w != 0) {                              // ovl14@1EA8..1EF4 / ovl18@1F00..1F44  ("equal")
    for (k = 1; k != M; k += 2)                   // f fixed at 0x8000 (vmov v3[2],v3[3] <- 0x8000)
        E[k] = MORPH(E[k-1]*0x8000 + E[k+1]*0x8000, E[k], w);       // odd k morphs toward the midpoint
}
```
Notes:
- For M=5, the span `k0..k0+4` in "differs" is the whole edge, so the target is the corner-to-corner line.
- For M=3 in "differs" with w != 0 the ucode reads E[4], which lies **outside the edge** (the next row, or past the pool). The game never sends that (see (d)).
- Loop `k != M, k += 2` never terminates for even M (M=2 at s=2). The game never sends a grid tile with s=2 (see (d)).

### Interior pass (ovl10@1DB0..1EAC), after all edges
```c
w = rec_u16[0x14];  if (w == 0 || M < 3) skip;             // 1DB0..1DBC
half = w >> 1;  inv = (0x10000 - w) & 0xFFFF;              // 1DD4..1DE4
for (j = 1; j <= M-2; ++j) for (i = 1; i <= M-2; ++i) {    // row-major, in place
    if (!(i & 1) && !(j & 1)) continue;                    // 1DF8..1E04
    dj = (j & 1), di = (i & 1);                            // pair = (j-dj,i-di) and (j+dj,i+di)
    // odd col only -> left/right; odd row only -> up/down; both odd -> MAIN diagonal (-M-1, +M+1)
    a = H[j-dj][i-di];  b = H[j+dj][i+di];
    H[j][i] = clamp_s16( ((a + b) * half + H[j][i] * inv) >> 16 );   // vmudm/vmadm 1E78..1E80
}
```
The pair members are always (even, even) points or edge points, so the in-place order does not matter. Interior points next to an
edge read the already-morphed edge vertex (for example (1,2) uses (0,2)). **This means an HLE must run edges before the interior.**

### Colors from the parser (ovl0C@1E88..1F58) (not a seam effect)
With `d = L - s`: `step = d>=0 ? 0x100>>d : 0x100<<-d` and `mask = d>=0 ? (1<<d)-1 : 0` (1E5C..1E84).
Color index = `floor(j*step/256)*Mc + floor(i*step/256)`. If `(i&mask)||(j&mask)`, the color is
`MEAN(C[idx], C[idx + (i&mask?1:0) + (j&mask?Mc:0)])` (the diagonal case again uses the main diagonal). Otherwise it is a straight copy.
Heights are `(s8)src[j*(N<<s) + i*(1<<s)] << 4` (1F28..1F48).

## (d) Do both sides of a shared edge match?

**Yes, exactly, as long as both sides run both ucode branches and the game's pairing (below) is honoured.** The ucode alone guarantees
nothing: the pairing comes from the CPU in `buildGridCellMeshForFace` (0x8004317C):

1. LOD per tile, from squared distance d (node+8) against thresholds at 0x8009DE94 = {256, 49, 30.25, 16, 6.25} (0x800431CC..0x80043328):
   - d >= 49: LOD 2, f = 0.
   - 30.25 <= d < 49: LOD 1, f = (d-30.25)/(49-30.25).
   - 16 <= d < 30.25: LOD 1, f = 0.
   - 6.25 <= d < 16: LOD 0, f = (d-6.25)/(16-6.25).
   - d < 6.25: LOD 0, f = 0.

   f is the morph fraction toward the next coarser LOD.
2. Init (0x800433E4..0x80043478): t+0x10..0x14 = LOD, so all four nb bytes = own LOD. **All five weights (interior + 4 edges) = trunc(f*65535)**.
   LOD 2 gets all weights 0.
3. Promotion (0x80043528..0x80043674): a LOD-0 tile touching any LOD-2 tile becomes LOD 1 with all bytes 1 and all weights 0. So neighbours differ by at most 1.
4. Edge pairing (0x8004367C..0x80043768), done once per shared edge (row-1 neighbour and col-1 neighbour, via the grid 0x80132DC0):
   - **If the neighbour is finer, it copies this tile's edge byte and weight.** Otherwise this tile copies the neighbour's.
     So the **finer tile always takes the coarser tile's byte (= coarser LOD, so != its own s, so it takes the "differs" branch) and the coarser tile's morph weight**.
   - The coarser tile keeps byte == s, so it takes the "equal" branch with the same weight.
   - Between equal-LOD tiles the lower-row/lower-column tile adopts the other's byte and weight. Both take "equal" with one weight.
5. Record emit (0x80011550..0x800115B0): a LOD-2 tile has bytes 2,2,2,2 and weights 0, so it is always sent as the flat `05 05 02` form.
   Hence a grid tile never has s=2 (no M=2 hang). A M=3 tile's "differs" edge always borders LOD 2 with w=0 (no E[4] over-read), and becomes a straight line.

Check for fine (M=5, k=0..4) against coarse (M=3, k'=0..2) with the same w:
- Coarse k'=1 is `MORPH(E0*0x8000+E4*0x8000, E2, w)`. Fine k=2 is `MORPH(E0*0x8000+E4*0x8000, E2, w)`: **bit-identical**. Corners are untouched on both sides.
- Fine k=1 is `w*(3E0+E4)/4 + (1-w)*(E0+E2)/2`, which equals the midpoint of fine E0 and the morphed E2, up to floor rounding. k=3 works the same way.
  So these are T-junctions on the coarse segment, and the ucode covers the rounding gap with **sliver triangles** (below).
- Colors go through the same operations, so they match too.

The weight that governs a shared edge is the **coarser** tile's own morph fraction. When the LODs are equal, it is the row-1 or col-1 tile's fraction.

Crack slivers (only when nb != s). Vertex slot `v(i,j) = 0x670 + (i*M+j)*0x28`. Emitted as extra depth-sorted triangles
(key 0x80 | n, with slot triples stored at DMEM 0xB00+n, 1ED4..1F08):
- z-min: ovl1C@1E50..1E88. z-max: ovl1C@1E8C..1ECC. x-min: ovl20@12A0..12D8. x-max: ovl20@12DC..131C.
- M=3: one per edge, (end, end, mid).
- M=5: two per edge, ((0,2,1),(2,4,3)).
- **Ucode delay-slot bug:** on the M=5 (s==0) path of ovl1C, the first sliver of the z-min and z-max edges uses a stale register.
  - z-min gets (v(0,0), v(1,1), v(1,0)) instead of (v(0,0), v(2,0), v(1,0)). The correct $3=+0x190 sits in the other branch's delay slot at 1E70.
  - z-max gets (v(0,2), v(1,4), v(2,4)) instead of (v(0,4), v(1,4), v(2,4)). The correct $2=+0xA0 sits in the delay slot at 1EAC.
  - Hardware therefore leaves those two T-junction spans unfilled. Our HLE should emit the intended slivers, or snap the fine vertices
    exactly onto the coarse segment.

## (e) Interior effect of the weights

Only rec+0x14 (the tile's own f) moves interior points. It pulls every odd-row/odd-column interior sample toward the mean of its
horizontal, vertical or main-diagonal neighbours (ovl10@1DB0..1EAC). At w = 0xFFFF a 5x5 tile becomes the 3x3 tile's triangulated
surface, and a 3x3 tile becomes the flat quad (diagonal (0,0)-(2,2) = flat v0-v3). That is a geomorph. The edge weights do not
touch the interior directly; interior points next to an edge see the morphed edge values through the pass order.

## (f) Vertex emission layout

- M = ((N-1)>>s)+1. With N=5: s=0 gives M=5, s=1 gives M=3. The M=5 limit is also hard-coded, because the triangle key packs i,j into 2 bits each (ovl20@1394..13BC).
- There is **no subdivision**: M*M vertices, spacing = rec+0x26 in both x and z, so the tile spans (M-1)*size.
- Build order (ovl10@1ED8..1F2C) is column-major. Outer loop over i (x); inner loop over j (z):
  `vtx[i*M+j] = (x + i*size, (y16<<4) + H[j][i], z + j*size)`. Vertices sit at DMEM 0x280 + 8n and transform to 0x670 + 0x28n.
- Colors (ovl10@1F84..1F98): the RGB bytes of C[j][i] are written to vertex +0x10..0x12. Byte +0x13 keeps the transform's value (UNKNOWN meaning, likely fog/alpha).
- Texcoords: s = i*step, t = (M-1-j)*step, with step = rec+0x1E (1F54..1FAC).
- Per cell (i,j) in 0..M-2 there are two triangles, (v(i,j), v(i+1,j), v(i+1,j+1)) and (v(i,j), v(i+1,j+1), v(i,j+1)) (ovl20@1394..13B8).
  The diagonal is (i,j)-(i+1,j+1), matching the interior morph diagonal. Our f5_emit_terrain_strips uses the same diagonal.
- UNKNOWN: the internals of the resident transform 0x15D0, and the exact meaning of vertex +0x1C (it is used as the depth-sort key).

## HLE fix summary

1. Order: rows, then columns, then interior.
2. Add the "equal" edge branch.
3. Add the rec+0x14 interior morph.
4. Keep "differs" as is (identical for M=5; for M=3, only the midpoint step).
5. Apply every step to colors as well.
6. Emit the slivers, or snap the fine vertices onto the coarse line.

Separately, our time-based `f5_terrain_blend` keys its signature on rec[0].w1. A finer tile changes signature when it adopts a neighbour
byte, but the coarser tile does not, so the two can blend out of step. The ucode's own weights are already continuous, so this blend is redundant for grid tiles.
