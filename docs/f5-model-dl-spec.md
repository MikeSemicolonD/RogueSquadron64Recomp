# F5 Model Display-List Grammar

How the game's CPU compiles models into the F3DFACTOR5 display-list stream
(§1–6, from the recompiled functions in `RecompiledFuncs/`) and how the RSP
ucode consumes it (§7, from the IMEM listing). §7 is authoritative for
per-opcode semantics; the earlier sections carry the CPU-side emit detail and
some claims not independently re-checked (flagged inline).

## 1. Pipeline architecture

The game does not hand models to the RSP. The CPU walks the scene graph and
compiles every visible model into DL commands each frame:

```
traverseSceneGraphRecursive (funcs_4.c:16459)
  → processSceneNode (funcs_4.c:15315)             node matrix + visibility
    → processMeshdef1ForLod (funcs_4.c:14963)      LOD selection
      → submitSceneNodeRender (funcs_4.c:1820)     per-node prolog: lights, RDP state, materials
        → renderLitMeshFaceGroup (funcs_4.c:9893)    lit geometry → DL
        → renderFlatMeshFaceGroup (funcs_4.c:4555)   flat geometry → DL
        → renderUnlitMeshFaces (funcs_4.c:6488)      unlit geometry → DL
          → buildOrientedFaceGeometry (funcs_4.c:7715)  oriented/billboard quads → DL
          → resetVertexCacheSlot (funcs_4.c:7143)       RSP vertex-cache slot management
          → emitMaterialTexturedDL / Alt (funcs_3.c:18359 / 19547)  per-material texture DL
          → appendRdpStateDl (funcs_3.c:17896)          RDP state block
          → emitAllocatedDLCommand (funcs_3.c:2557) / reserveAndEmitDLEntry (funcs_3.c:14608)
          → findAndUnlinkSmallestEntry (funcs_6.c:13573) chunk allocator
```

The renderers iterate staged per-material face lists (linked records built
during scene-graph processing), not raw HOB faces; raw HOB data is upstream.

## 2. Emit primitives + chunk discipline

- `emitAllocatedDLCommand(a0=cursor, a1)` / `reserveAndEmitDLEntry(a0=cursor, a1)` (funcs_3.c:2557 / 14608): bounds-check the cursor against the chunk ceiling (`0x80116x` globals: head `+0x63B0`, cursor `+0x63FC`, ceiling `+0x63D4`); on overflow they write `w0=0xB5000000 w1=0` (chunk-link placeholder), pull a new chunk via `findAndUnlinkSmallestEntry`, and back-patch the link.
- Command words are written at the call sites via `MEM_W(0x0,cursor)=w0; MEM_W(0x4,cursor)=w1`.
- Stream framing: `0xE700` pipe-syncs, `0xB500` chunk links, `0xB800` end.

## 3. Command grammar (CPU-emit side)

### 0x04 — vertex batch load

```
w0 = 0x04<<24 | (n << 10) | (((n*8 + 0xF) & 0xFFF0) - 1)     n = vertex count
w1 = RDRAM pointer to packed 8-byte vertices
```
G_VTX shape: count in bits 10+, (DMA length-1) low, 8-byte vertices. Emitted
at renderLitMeshFaceGroup funcs_4.c:10870-10883 and buildOrientedFaceGeometry
funcs_4.c:8300-8323. RSP cache slot stride 0x50; slot counter global `0x8012xxxx-0x5BB0`.

### 0x01 — matrix load
- renderLitMeshFaceGroup emits `w0=0x01020040, w1=0x80037780` (funcs_4.c:11787-11800): b1=0x02 (LOAD param), w1 = static data-segment block at 0x80037780 (content not yet dumped).
- The runtime camera matrices (w1=0x80700000 proj / 0x80710000 modelview) are not emitted by submitSceneNodeRender or the facegroup renderers; their emitter (likely setupCameraMatrices funcs_4.c:20538 or the per-frame header builder) is unpinned.

### 0xBF — triangle, 0x13 — quad
- Selection: staged-face `flags & 0x3 == 0` → 0xBF triangle, else 0x13 quad (funcs_4.c:10829, 11175-11207).
- Index encoding: w1 packs vertex-cache byte offsets (slot×0x50): `((w1>>13)&0x7F8) ((w1>>5)&0x7F8) ((w1<<3)&0x7F8)`.
- `w0 & 0x2` → a 16-byte inline texcoord block follows (emission site for the inline block not isolated).

### 0xB4 — oriented quad

```
w0 = 0xB4000006
then: 4× u16 vertex-cache slot values (from sp+0x5E/0x58/0x5A/0x5C), constant 0x10001000,
      and additional words — total length ≥16B.
```
funcs_4.c:9782-9854, in buildOrientedFaceGeometry (billboard/oriented faces; also
from renderLitMeshFaceGroup:12099 for regular models). Per §7 the length is 32 B
when textured.

### 0xBD — billboard sprite
```
w0 = 0xBD00xxxx  (| ((count*5+idx)<<8))
w1 = RGBA — face color (face+0x2C), or 0xFFFFFFFF on the white-sprite branch (face flag 0x200 clear)
then: second color word (from $s4), then sprite width/height as unsigned fixed point
```
renderLitMeshFaceGroup 0x800137A4.. (white-sprite branch 0x8001387C..). The `f22` multiply and
`f20` compare at 0x8001382C convert the face's float width/height (face+0x20/+0x24) to unsigned
fixed point (`f20` = 2^31 for the trunc.w.s split); it is not lighting. The game does no
per-face lighting here: model colours come from static prelit vertex colours (meshdef1 +0x4C),
and the level's directional light (0x80136E20) is transformed per node but never read for
shading. The F5 GBI module overrides 0xBD (it would otherwise fall through to F3DEX
`popMatrix` and corrupt RT64's matrix stack under native matrix loading).

### 0xBE — merged state word
Emitted unconditionally at the end of appendRdpStateDl: `w0=0xBE000000, w1=t0`
where t0 accumulates mode bits from tables at 0x8003775C / 0x8003776C indexed by
node mode bits (funcs_3.c:18200-18208). Overridden by the F5 module (F3DEX would
read w1 as `cullDl` vertex indices and cull/terminate real DLs).

### 0xFA — G_SETPRIMCOLOR (standard)
`w0=0xFA008000; w1 = face RGBA (staged-face flags&0x200) else 0xFFFFFFFF`
buildOrientedFaceGeometry funcs_4.c:9229-9277.

### appendRdpStateDl block (funcs_3.c:17896-18310)
Args: a0=cursor, a1=node mode flags (node+0x2C), a2/a3=out bytes. Sequence:
1. `0xE700` pipesync (17904-17906)
2. conditional `0xB900 0201`-style othermode_L with float-derived fog/alpha value (17964-18034, on a1&0x8000000)
3. conditional `0xB700` (set geometry mode, w1=t1) / `0xB600` (clear, w1=t2) (18162-18193)
4. always `0xBE00` w1=t0 merged state (18200-18208).
Mode-bit → state tables: 0x8003775C (idx=(a1>>20)&7), 0x8003776C (idx=(a1>>23)&7).

### Material / texture bind
- `emitMaterialTexturedDL` (funcs_3.c:18359) + Alt (19547): a0=cursor, a1=texture index, a2=flags (Alt forces |0x1), a3=material ptr. Early-outs on table-entry flags bit0 clear or a2&0x8.
- Index remap: reads u16 at entry+0x2 of the HMT→global remap table at 0x8011A444, passes it to `findOrCreateMaterial`.
- W/H come from the global texture table D_80128F08 via getTextureLUTFieldAt8/AtA (funcs_6.c:14542-14593): base 0x80128F08, offset idx*0x24, reads +0x08 (W) and +0x0A (H).
- The top level emits bracket commands only (0xE7 sync, 0xFC combine, 0xBB texture; Alt ends 0xB9000002). Render-mode cache: skip if mode byte equals cached (`0x8012xxxx-0x56F8`).

**Cached per-material sub-DLs via G_DL.** findOrCreateMaterial (funcs_6.c:14274-14541;
a0=cursor, a1=texture index u16, a2=mode bits):
- Material cache: bucket table D_80128F00[texIdx] → linked material nodes (0x18-stride matpool at D_80128EF4). Node fields: +0x0 next, +0x8 dirty u16, +0xC mode byte, +0xD aux, +0xE compare, +0x10 mode word, +0x14 cached material DL address.
- Cache hit → emits `w0=0x06000000, w1=node->0x14 - 8` (funcs_6.c:14501-14510): texture binds enter the stream as G_DL sub-DL calls into cached per-material DLs; w0 passes the F5 GBI's strict-G_DL filter, so the HLE follows them.
- Cache miss → allocOrEvictMaterialNode (funcs_6.c, 0x8002236C) → emitMaterialRenderStateDL (funcs_6.c:~12172) builds the snippet; node+0x14 = its address; ends with 0xB800 + 0xB500 framing. Chunk overflow handled inline (0xB500 link + allocateDisplayListBuffer 0x80007D74).

**Cached material-DL contents.** loadTextureTile is called twice (funcs_6.c:12232,
12264) — the SETTIMG/SETTILE/LOADBLOCK emission; plus othermode-H
`0xBA00/w1=0x1001|0x1402` + othermode-L `0xB900/w1=0x031D|0x0002` and combiner/mode
words from data-segment tables (emitMaterialRenderModeDL funcs_7.c:26-338; Alt
339-615). decodeRdpFormatFlags (funcs_6.c:4030) maps a 6-value format index (bits
0-5, the Factor 5 image subtype enum 0..5) → RDP fmt/siz.

**loadTextureTile (funcs_6.c:7827).** Per-format switch on (entry flags & 0x3F)-1
bounded 0..5: each case emits the standard libultra block-load sequence — 0xE6/0xE8/0xE7
syncs, 0xFD SETTIMG (w1=data@entry+0x10 or TLUT D_80128EFC[entry+0x0E]), 0xF5 SETTILE,
0xF3 LOADBLOCK, 0xF0 LOADTLUT (CI only), 0xF2 SETTILESIZE; W/H from entry+0x08/+0x0A
(mip-shifted by a2). Cached material DLs thus contain only standard RDP ops the GBI
already interprets.

### 0x14 — secondary vertex batch (unverified)
renderLitMeshFaceGroup's secondary pass (flag 0x21 variant, funcs_4.c:11598-12174)
appears to emit `0x14` batches where the primary used 0x04, plus an
`0x01020040`-prefixed end-of-material marker (11793). Per §7, 0x14 is a 16-byte
state command.

### 0x02 — per-vertex color staging
Per §7: DMA `(w0&0xFFFF)+1` bytes of RGBA from w1 into DMEM `0xB70`. None of the
five extracted model-render functions emit it; likely the particle/explosion path
or another emitter.

## 4. submitSceneNodeRender prolog (funcs_4.c:1820-4554)
- Args: a0=scene node, a1=render-pass index, a2=DL cursor, a3=render-pass array.
- Sequence: transformSceneLights(node, pass, cursor, &flags) (:1976) → appendRdpStateDl (:2207) → chunk reservations (:2223, :2239) → per-material emitMaterialTexturedDL (:2320, on material-ID change; lit-vs-flat by material flag bit 2, :3714-3716) → facegroup renderers.
- Node mode word at node+0x2C (bit 0x8 lighting; 0x4000/0xC000 transform/cull). Pass lists hang off 0x80138D18-area globals (0x63B0/0x63FC).
- `transformSceneLights` (0x8000EF88) is misnamed: it clears a 32-word table and runs the vertex-cache helper over the node's render items. Lights are transformed by `transformLightByType` (0x8000DCFC) from `traverseSceneGraphRecursive`; nothing reads the result for shading.

## 7. Ucode truth (from the IMEM listing)

Source: `dumps/f5_ucode.imem.bin` (byte 0 = ucode image byte 0 = IMEM `0x1080`; the
ucode's own constants and the DMEM dispatch table use real IMEM addresses, so
listing address = real - 0x80). Overlays live in the RDRAM ucode image (`0x80024A10`),
table at DMEM `0x76..0xAA`.

**Fetch model (real `0x1088..0x12F8`).** One 0x108-byte chunk is DMA'd into DMEM `0x170`
and executed from `+8` (the header's next/prev pointers are never executed; op `0x80`
dispatches to IMEM 0). When the cursor reaches `+0x108`, or on `B5`/`0x12`, the ucode
fetches the chunk named by the current chunk's FIRST WORD and continues at `+8` (`B5`'s
own w1 is never read). `06` pushes `{chunk, cursor}` and fetches w1; `07` fetches w1
without pushing; `B8` pops, or ends the task when the stack is empty. The allocated chunk
list is doubly linked: a valid next chunk's second word points back to the current chunk
(the HLE and `tools/validate/f5_dl_walk.py` require this; a stale call into a recycled
chunk would otherwise walk the free list).

| op | handler (real) | meaning | length |
|---|---|---|---|
| `01` | `0x1484` | DMA 64 B matrix from w1: byte1 bit0 = 0 modelview (DMEM `0x5D0`, then MVP = MV x P into `0x610`), 1 = projection (`0x590`) | 8 |
| `02` | `0x14F0` | DMA `(w0&0xFFFF)+1` bytes from w1 into `0xB70` = per-vertex RGBA colors | 8 |
| `03` | `0x14D4` | byte1 selects a DMEM slot (table `0x46`); the next 16 B are stored inline. `03 80` = viewport (vscale x,y,z,pad / vtrans x,y,z,pad, 2-bit fixed); `03 82` = texcoord scale (4 hi + 4 lo halfwords, 16.16 s,t,s,t; DMEM 0x140) multiplied into every per-face UV | 24 |
| `04` | `0x15B4` | DMA `(w0&0x3FF)+1` bytes from w1 into `0x280`; `(w0>>10)&0x3F` 8-byte vertices (x,y,z int16, pad) | 8 |
| `05` | `0x15AC` | load overlay 0xC: `05 05 02 ..` = flat terrain tile (below); `05 05 00 ..` = heightfield tile (overlays 0x14/0x18; HLE `f5_tile_grid`) | 40 |
| `BF` / `08` | `0x146C` | triangle: w1 bytes 1..3 = vertex slot (byte/5 = index), word2 bytes = per-vertex color offsets into `0xB70`, word3 = flags; `w0&2` = textured, 16 B of raw (s,t) halfwords follow, multiplied by the `03 82` texcoord scale (raw 0x1000 = one tile) | 32 / 16 |
| `B4` / `13` | `0x1484`-0x80 | quad: as `BF` plus the 4th vertex from w1 byte0; vertex order = bytes 1,2,3,0 (colors + UVs in that order); tris (v0,v1,v2) (v0,v2,v3) | 32 / 16 |
| `14`, `BD`, `BE` | `0x12EC`, `0x15A4`, `0x12E4` | 16-byte state commands (`BD` also loads overlay 0x2C) | 16 |
| `06`, `07`, `B5`, `B8` | `0x12A0`, `0x12B8`, `0x10E0`, `0x12C4` | call, branch, next-chunk, return | 8 |
| `E4`/`E5` | top2 = 3 path | texrect copied to the RDP buffer | 16 |
| `08..12` | shared table | aliases of `BF..B5` | as above |

**Flat tile (`05 05 02 xx`, overlays 0xC then 0x24).** Word pairs after the command:
w1 = (h0,h1), word2 = (h2,h3) corner heights; word3..word6 = corner RGBA; word7.lo =
texcoord span (S10.5, stored into the vertex as-is by overlay 0x24; grid overlay 0x10
steps it per sample, t decreasing along z); word8 = (x, y>>4); word9 = (z, size). Corners
v0=(x,y+h0,z) v1=(x+size,y+h1,z) v2=(x,y+h2,z+size) v3=(x+size,y+h3,z+size), UVs
v0 (0,s) v1 (s,s) v2 (0,0) v3 (s,0), transformed by the current MVP.

**Heightfield tile (`05 05 00 xx`).** DMAs `(word4.hi+0x16)&0xFF0` height bytes from
word2 into DMEM `0x4E0` and `(word4.lo+0x13)&0xFF0` color bytes from word3 into `0x380`;
byte1 = samples per row, byte3 = LOD shift, byte2>>4 = second shift. Overlay 0x14
subdivides/averages rows, overlay 0x18 emits the grid. The HLE (`f5_tile_grid`) reads the
5x5 signed heights and RGBA colors and bilinear-subdivides them (`ROGUESQ_F5_TERRAIN_SUB`).

**Conventions.** NDC is y-down (the ucode adds `ndc*vscale` with no negation); the HLE
hands RT64 a negative `vscale.y`. Matrices are standard N64 split int/frac, row-vector
convention.
