# Factor 5 GBI (Rogue Squadron)

Canonical opcode map for the game's F3DFACTOR5 graphics ucode, with observed
w0/w1 patterns and the interpretations that runtime probing disproved. The DL
grammar itself (chunk fetch, per-opcode semantics from the IMEM listing) is in
[f5-model-dl-spec.md §7](f5-model-dl-spec.md).

## Registration

- ROM text offset `0x25610`, size `0x1FA0`, hash `0xC8B0316823094FD2`.
- ROM data offset `0x39900`, size `0x100`, hash `0xF2150F53524F1EFA`.
- RT64 profile: `GBI_F3DFACTOR5` in [lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp](../lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp), inherits `GBI_F3DEX`.
- The real data segment extends to ~`0x3C0` bytes; only the first `0x100` are hashed for identification.

## Factor 5-specific opcodes

| Opcode | Standard meaning | Factor 5 behavior | Handler |
|-------:|------------------|-------------------|---------|
| `0x04` | G_VTX | Vertex batch: `(w0>>10)&0x3F` 8-byte vertices (x,y,z int16, pad) DMA'd from w1 | native |
| `0x02` | G_RDPHALF_2 | DMA `(w0&0xFFFF)+1` bytes of per-vertex RGBA into DMEM `0xB70` | `op02` |
| `0x03` | — | State: byte1 selects a DMEM slot. `03 80` = viewport; `03 82` = texcoord scale | native |
| `0xBF` / `0x08` | G_TRI1 | Triangle; `w0&2` = textured (16 B texcoords follow) | native |
| `0xB4` / `0x13` | G_QUAD | Quad (4th vertex from w1 byte0) → two triangles | native |
| `0xB5` | G_QUAD | Chunk/DL terminator at chunk offset `0x100`; returns to parent DL | `op_B5_endDl` |
| `0xE4` | G_TEXRECT | LLE format (16 bytes), not HLE 24-byte; the handler consumes both words (the S/T/DsDx/DtDy word dispatched as a command whenever S >= 0x100, e.g. 0x0D = SETOTHERMODE_H forcing copy mode) | `texrectLLE_guarded` |
| `0xE5` | G_TEXRECTFLIP | LLE format | `texrectFlipLLE` |
| `0xFF` | G_SETCIMG | Sometimes emitted with bogus payload (w1=0, fmt>4, OOB addr); rejected | `setColorImage_filtered` |
| `0x80` | unused | Chunk metadata header (next-chunk pointer in 24-bit w0); walked as a no-op | `op80_unknown` |
| `0x05` | — | Terrain record (40 B): `05 05 02` flat tile, `05 05 00` heightfield | `op05` |

Standard F3D/F3DEX opcodes (0x01, 0x06, 0xB8, 0xB9, 0xBA, 0xBC, 0xE6-0xED, 0xF6-0xFF) dispatch through the inherited map.

## RSP dispatch mechanism

Main command loop at IMEM `0x04001010`:

```
04001014  LW   r19, 0(r17)         ; r19 = w0
04001018  LW   r20, 4(r17)         ; r20 = w1
04001024  SRL  r2,  r19, 30        ; top 2 bits of opcode
0400102c  BEQ  r2,  3, +17         ; opcodes 0xC0-0xFF → alt path
04001030  ADDIU r17, r17, 8        ; advance DL ptr
04001034  BNE  r2,  r0, +4         ; opcodes 0x40-0xBF → SUBU path
04001038  SRA  r2,  r19, 23        ; r2 = (signed w0) >> 23

; opcodes 0x00-0x3F:
0400103c  ANDI r2, r2, 0x01fe
04001040  LHU  r2, 0x00d6(r2)      ; table A at DMEM 0xD6, indexed by (op*2)
04001044  JR   r2

; opcodes 0x40-0xBF:
04001048  SUBU r2, r0, r2          ; r2 = -r2
0400104c  ANDI r2, r2, 0x01fe
04001050  LHU  r2, 0x0064(r2)      ; table B at DMEM 0x64, indexed by (-op*2) & 0x1FE
04001054  JR   r2
```

Two dispatch tables: Table A at DMEM `0xD6` (opcodes `0x00-0x3F`), Table B at
DMEM `0x64` (opcodes `0x40-0xBF`, sign-magnitude indexing). Opcode 0x80 lands at
data[0x164], past the 0x100 registration window but within the full ~`0x3C0`-byte
segment.

## Geometry mode (0xB6 clear / 0xB7 set) cull bits

From the tri routine (IMEM 0x1770 onward; `f5_ucode.imem.bin` offset = IMEM
address minus 0x1080): the cull test masks the sign-extended screen-space cross
product with `geometryMode << 18` and rejects when bit 31 is set, so only 0x2000
(G_CULL_BACK) culls, on positive cross (y-down screen space). 0x1000 never reaches
the test; the emitter reads it separately to skip the texcoord 1/w premultiply, so
it is a texcoord-perspective flag, not G_CULL_FRONT. Both bits set = back-face
cull. Honored when `ROGUESQ_F5_CULL` is on (default).

## Disproven interpretations

- **0x80 as a sub-DL call** — treating its 24-bit w0 as a call target infinite-loops and hangs after ~200 DLs. It is a chunk-metadata / param header.
- **0x02 as "the whole vertex/transform/triangle pipeline"** — an early reading (from a boot phase that emitted only clear-rects) concluded Factor 5 bundled all geometry into 0x02 and never used G_VTX/G_TRI. Wrong: 0x04 is the vertex batch, 0xBF/0xB4 are the triangle/quad, and 0x02 is per-vertex RGBA color staging.
- Routing `0x22`/`0x26`/`0x2A`/`0x2E` through F3DEX tri1/tri2 decodes degenerate vertex indices — the operand is encoded in the opcode byte, not the F3DEX-style data payload.

## Crash classes safety-netted (post-credits / intro scenes)

1. Chunks with no recognized terminator march past RDRAM — caught by the iteration cap in [rt64_interpreter.cpp](../lib/rt64/src/hle/rt64_interpreter.cpp).
2. Unrecognized ucode (`getGBIForUCode` returns null) — caught by null-`hleGBI` skip + mid-task null guard.
3. `G_MOVEMEM` with idx outside F3D set — was `assert(false)`, now logs and skips ([rt64_gbi_f3d.cpp](../lib/rt64/src/gbi/rt64_gbi_f3d.cpp)).
4. Unimplemented framebuffer readback formats — was assert, now logs and returns 0 ([rt64_native_target.cpp](../lib/rt64/src/render/rt64_native_target.cpp)).
5. STL bounds checks — `_CrtSetReportHook` returns 1 to suppress the abort.
