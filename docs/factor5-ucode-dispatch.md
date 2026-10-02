# Factor5 RSP ucode — dispatch + handler map

Working notes on the dispatch table and handler addresses. Per-opcode meaning is
authoritative in [f5-model-dl-spec.md §7](f5-model-dl-spec.md).

## Files

- `dumps/f5_ucode.imem.bin` (8 KB IMEM image, BE; offset 0 = IMEM `0x04001000`, matches ROM `0x25610`)
- `dumps/f5_ucode.dmem.bin` (4 KB DMEM image, BE; starts with the ROM data segment at `0x39900`)

Read by `tools/disasm_rsp_entry.py`, `tools/disasm_op01_area.py`, `tools/decode_dmem_vtx_area.py` and siblings.

## Dispatch loop (IMEM 0x010 - 0x05C)

```
04001010  mfc0  $2, SP_STATUS
04001014  lw    $19, 0x0($17)   ; w0
04001018  lw    $20, 0x4($17)   ; w1
0400101C  andi  $2, 0x80        ; halt bit
04001020  bnez  $2, exit
04001024  srl   $2, $19, 30     ; top 2 bits of opcode byte
04001028  ori   $3, 0x3
0400102C  beq   $2, $3, ...     ; opcodes >= 0xC0 go a different path
04001030  addiu $17, $17, 0x8   ; (delay) advance DL ptr
04001034  bnez  $2, alt         ; opcodes 0x40..0xBF go alt path
04001038  sra   $2, $19, 23     ; (delay) idx = (w0 >> 23) & 0x1FE
0400103C  andi  $2, $2, 0x1FE
04001040  lhu   $2, 0xD6($2)    ; opcodes 0x00..0x3F: table 1 at DMEM 0xD6
04001044  jr    $2
04001048  negu  $2, $2          ; (delay slot — also alt path entry)
0400104C  andi  $2, 0x1FE
04001050  lhu   $2, 0x64($2)    ; opcodes 0x40..0xBF: table 2 at DMEM 0x64
04001054  jr    $2
```

**Opcode → IMEM PC mapping:**

`idx = ((w0 sra 23) & 0x1FE)` → byte offset into table.
For op `O`, idx = `(O & ~0x80) << 1` (bit 7 collapsed by andi/negu split).

## Table 1 (DMEM 0xD6, opcodes 0x00..0x3F)

| op | idx | raw | PC | notes |
|---|---|---|---|---|
| 00 | 00 | 0x10D8 | 0x0D8 | DPC sync stub / fall-through |
| 01 | 02 | 0x1504 | 0x504 | vertex pipeline outer-loop tail |
| 02 | 04 | 0x14F0 | 0x4F0 | vertex pipeline inner-loop tail |
| **03** | **06** | **0x14D4** | **0x4D4** | **vertex pipeline inner-loop entry — actually used (~2.5K/frame)** |
| 04 | 08 | 0x15B4 | 0x5B4 | clip-space matrix mul + vch/vcl clip |
| 05 | 0A | 0x15AC | 0x5AC | clip-space mat-mul mid-loop |
| 06 | 0C | 0x12A0 | 0x2A0 | (RMW-then-jump-to-fetch helper) |
| 07 | 0E | 0x12B8 | 0x2B8 | |
| 08 | 10 | 0x146C | 0x46C | |
| 09 | 12 | 0x12E4 | 0x2E4 | |
| 0A | 14 | 0x15A4 | 0x5A4 | |
| 0B | 16 | 0x1390 | 0x390 | |
| 0C | 18 | 0x1370 | 0x370 | |
| 0D | 1A | 0x12FC | 0x2FC | |
| 0E | 1C | 0x1304 | 0x304 | |
| 0F | 1E | 0x12C4 | 0x2C4 | |
| 10 | 20 | 0x1350 | 0x350 | |
| 11 | 22 | 0x135C | 0x35C | |
| 12 | 24 | 0x10E0 | 0x0E0 | |
| 13 | 26 | 0x1484 | 0x484 | |
| 14 | 28 | 0x12EC | 0x2EC | |

Entries 0x15..0x3F are mostly 0x0000 (= no-ops, re-enter dispatch).

## Observed game opcode frequency (one frame-100 histogram)

| op | count | RDP/F3D meaning (where standard) |
|---|---|---|
| 0x00 | 11185 | NOOP |
| 0xE7 | 7688 | F3D PIPESYNC |
| 0xBA | 5342 | SETOTHERMODE_H |
| 0xF5 | 5045 | SETTILE |
| 0xE4 | 4288 | TEXRECT |
| 0xB9 | 2955 | SETOTHERMODE_L |
| 0xB8 | 2691 | ENDDL |
| 0x06 | 2672 | DL (call sub-DL) |
| 0xE6 | 2548 | RDPLOADSYNC |
| 0xFD | 2531 | SETTIMG |
| **0x03** | **2531** | **Factor5 vertex op (PC 0x4D4)** |
| 0xFC | 2514 | SETCOMBINE |
| 0xF3 | 2514 | LOADBLOCK |
| 0xF2 | 2514 | SETTILESIZE |
| 0xBB | 2514 | TEXTURE |
| 0xB5 | 1033 | Factor5 (no-op marker) |
| 0x22 | 791 | Factor5 |
| 0x80 | 680 | Factor5 |

**Key finding:** `op_03` (not op_01) is the actual high-volume Factor5 vertex
opcode. The earlier note's claim that "op_01 is the full 3D pipeline" had two
errors: wrong opcode number (it's op_03 we observe, with 0x05B4 being where
op_04 lands), and wrong description of the layout (each op enters the pipeline
at a different *stage*, not a separate function).

## Pipeline structure

**Outer block (IMEM 0x4C0 - 0x520):** 4×4 matrix-by-vertex multiply.
- `$8`, `$9`, `$10` are DMEM offsets (input vtx, input mtx, output vtx)
- Inner loop (0x4C8..0x4FC, 4 iters): 4-column matrix mul with vmadl/vmadm/vmadn/vmadh chain — full 32-bit-precision dot product
- Outer loop (0x4C0..0x518, processes batches): stores result via sqv to `$10`, increments and loops

**Clip-and-project block (IMEM 0x540 - 0x640+):**
- Loads 4×4 matrix into v8..v15 (hi/lo split for 32-bit precision)
- For each vertex: clip-matrix multiply (0x5AC..0x5CC), perspective divide (0x5D4..0x5D8), clip-flag computation (vch/vcl twice at 0x5DC..0x5EC), screen-space transform (0x608..0x624), clip-flag pack (0x5F0..0x600, 0x60C..0x628)
- Stores clip flags at `0x24($9)` and probably more

The per-opcode semantics decoded from this table are superseded by the
IMEM-listing grammar in [f5-model-dl-spec.md §7](f5-model-dl-spec.md), whose
addresses are the real IMEM values (this doc's handler PCs are offset).
