# Factor 5 GBI (Rogue Squadron)

## Registration

- ROM text offset `0x25610`, size `0x1FA0`, hash `0xC8B0316823094FD2`
- ROM data offset `0x39900`, size `0x100`, hash `0xF2150F53524F1EFA`
- RT64 profile: `GBI_F3DFACTOR5` in [lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp](../lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp), inherits from `GBI_F3DEX`.
- The real data segment extends to ~`0x3C0` bytes; only the first `0x100` are hashed for identification.

## Opcode map observed on boot

Non-F3DEX opcodes seen during the first 15 s of boot display lists:

| Opcode | w0 pattern           | w1 pattern    | Count | Current handler | Notes |
|-------:|----------------------|---------------|------:|-----------------|-------|
| 0x80   | `0x8072XXXX`         | `0x00000000`  |    39 | `op80_unknown` (no-op) | 24-bit arg in w0. **Disproved:** not a sub-DL call — treating it as such infinite-loops and hangs after ~200 DLs. Likely a state/register set or param load where the 24 bits are an ID/value. |
| 0x02   | `0x028001C0` (const) | `0x01FF0000`  |    78 | `op02_unknown` (no-op) | Constant payload — likely a fixed setup/config (DMA range, clipping, or segment base). |

All other opcodes in the stream (0x01, 0x03, 0x06, 0xB8, 0xB9, 0xBA, 0xBC, 0xE6-0xED, 0xF6-0xFF) are standard F3D/F3DEX and dispatch via the inherited map.

## RSP dispatch mechanism (preliminary)

Main command loop at IMEM `0x04001010`:

```
04001014  LW   r19, 0(r17)         ; r19 = w0
04001018  LW   r20, 4(r17)         ; r20 = w1
04001024  SRL  r2,  r19, 30        ; top 2 bits of opcode
0400102c  BEQ  r2,  3, +17         ; opcodes 0xC0-0xFF → alt path
04001030  ADDIU r17, r17, 8        ; advance DL ptr
04001034  BNE  r2,  r0, +4         ; opcodes 0x40-0xBF → SUBU path
04001038  SRA  r2,  r19, 23        ; r2 = (signed w0) >> 23

; Fallthrough (opcodes 0x00-0x3F):
0400103c  ANDI r2, r2, 0x01fe
04001040  LHU  r2, 0x00d6(r2)      ; table A at DMEM[0xD6], indexed by (op*2)
04001044  JR   r2

; BNE-taken (opcodes 0x40-0xBF):
04001048  SUBU r2, r0, r2          ; r2 = -r2
0400104c  ANDI r2, r2, 0x01fe
04001050  LHU  r2, 0x0064(r2)      ; table B at DMEM[0x64], indexed by (-op*2) & 0x1FE
04001054  JR   r2

; BEQ-taken (opcodes 0xC0-0xFF):
04001128  ...                      ; RDP/state path — likely the standard F3DEX tail
```

So there are (at least) two dispatch tables:
- **Table A** at DMEM `0xD6`, covering opcodes `0x00-0x3F`. Example: opcode 0x02 → halfword at data[0xDA] = `0x14F0` → handler at IMEM `0x14F0`.
- **Table B** at DMEM `0x64`, covering opcodes `0x40-0xBF` via sign-magnitude indexing. For opcode 0x80 the computed index lands at data[0x164], which is past the 0x100-byte "registration" window but within the full `~0x3C0`-byte data segment.

## Confirmed Factor5 opcodes (from runtime probing — May 2026)

| Opcode | Factor5 behavior | Handler in our tree | Confidence |
|-------:|-------------------|---------------------|-----------|
| `0xB5` | Chunk/DL terminator (= F3DEX `G_ENDDL`). Each 0x108-byte chunk ends with `0xB5` at offset 0x100. Sub-DL at `0x007239B8` is a single `0xB5` command — only makes sense as "do nothing, return". | `op_B5_endDl` → `state->popReturnAddress()` | **Confirmed** — fix unblocked credits scene from infinite-DL hang |
| `0xE4` | LLE TEXRECT (16-byte: command + one RDPHALF follow-up packing uls/ult/dsdx/dtdy together). F3DEX HLE expected 24 bytes which consumed the *next* TEXRECT as garbage data. | `texrectLLE` | **Confirmed** — fix made credit text glyphs render with correct UVs |
| `0xE5` | LLE TEXRECTFLIP — same format change as `0xE4` | `texrectFlipLLE` | **Confirmed** by inference |
| `0xFF` | `G_SETCIMG`, but Factor5 sometimes emits with bogus payload (`w1=0`). Real calls always have non-zero w1. | `setColorImage_filtered` (filters bogus form) | **Confirmed** |

## Unknown opcode family — `0xXX` where bit pattern is `00xxxx10`

Observed during post-credits scene playback:

| Opcode | Frequency (approx, per 1M-cmd broken-loop sample) | Notes |
|-------:|--------------------------------------------------:|-------|
| `0x02` | 80                                                | Constant payload: `w0=0x028001C0 w1=0x01FF0000`. Only 80 hits per task — likely a one-shot setup command. |
| `0x12` | 2,430                                             | Plausibly G_VTX variant |
| `0x16` | 9                                                 | Rare |
| `0x1E` | 18                                                | Rare |
| `0x22` | 26                                                | Rare |
| `0x26` | 4,864                                             | ~2× tri count — possibly tri2 / quad |
| `0x2A` | 12,147                                            | Most frequent — possibly tri1 |
| `0x2E` | 12,149                                            | Most frequent — possibly tri1 variant |
| `0x32` | 0 in sample                                       |  |
| `0x36` | 2,431                                             | Plausibly G_VTX variant (matches 0x12 count) |
| `0x3A` | 2,427                                             | Plausibly G_VTX variant (matches 0x12 count) |

**Pattern**: opcode bits `[6:2]` form a 5-bit operand (0–31); bits `[1:0]` always `0b10`. Payload is always `w0 = w1 = (op << 24) | 0x003400` — the constant `0x003400` does NOT vary per call. This means the **operand is encoded in the opcode byte itself**, not in the data. Without ucode disassembly we can't know what it operates on.

Tested experimentally: routing `0x2A`, `0x2E` to F3DEX's `tri1` and `0x22`, `0x26` to `tri2` decoded vertex indices to mostly-degenerate values (e.g., (0, 26, 0)) — confirms the operand is NOT in the data payload using F3DEX's vertex-index encoding.

## Crash-class summary (post-credits scenes)

The N64 logo / X-wing intro sequence hits multiple unimplemented paths. These are now safety-netted, not fixed:

1. **Different DL chunks with no recognized terminator** — chunks at `0x0074xxxx` march past the end of RDRAM. Caught by 5M-iter safety limit in [rt64_interpreter.cpp](../lib/rt64/src/hle/rt64_interpreter.cpp).
2. **Tasks with unrecognized ucode** — `getGBIForUCode` returns null. Caught by null-`hleGBI` skip in `processDisplayLists` and a mid-task null guard in the main loop.
3. **`G_MOVEMEM` with idx outside F3D set** — was `assert(false)`, now logs and skips ([rt64_gbi_f3d.cpp](../lib/rt64/src/gbi/rt64_gbi_f3d.cpp)).
4. **Unimplemented framebuffer readback formats** (4-bit I, RGBA8, IA8, etc.) — was `assert(...)`, now logs and returns `0` ([rt64_native_target.cpp](../lib/rt64/src/render/rt64_native_target.cpp)).
5. **STL bounds checks ("vector subscript out of range")** — `_CrtSetReportHook` returns 1 to suppress the abort.

## Open questions

1. **Opcode 0x02 handler at IMEM 0x14F0** — needs disassembly to confirm it's a config load / segment setup. Once identified, replace `op02_unknown` with a real handler.
2. **The `XX10` opcode family** — likely vertex/triangle/state commands with operand in the opcode byte. Reverse-engineering needs the Factor5 ucode binary.
3. **Whether `0xFE`, `0xF9`, `0xF8`, `0xF7`, `0xF6`, `0xED`, `0xE9`, `0xE8`, `0xE7`, `0xE6` semantics are preserved** in Factor5 — needs rendering verification per opcode.

## Diagnostic infrastructure

- `RT64_LOG_PRINTF` is null-guarded in [lib/rt64/src/common/rt64_common.h](../lib/rt64/src/common/rt64_common.h) so logs without an open file no longer CRT-assert.
- DL dispatch trace (stderr) in [lib/rt64/src/hle/rt64_interpreter.cpp](../lib/rt64/src/hle/rt64_interpreter.cpp) — remove after Stage B.
- MSVC CRT asserts are routed to stderr from [src/main/main.cpp](../src/main/main.cpp).

## Current status (end of session)

- Stage A: **done.** GBI is registered, `getGBIForUCode` succeeds, no identification assert.
- Stage B: **done for opcode coverage.** All opcodes in the stream have handlers (standard F3DEX or explicit no-ops). Zero unknown-opcode log entries over 5000+ DL commands / 39 render frames. DL loop runs stably without asserts or crashes. Game threads run healthily (validated via mq/thread trace).
- Stage C (visible rendering): **blocked — scope escalation.**

## Why nothing renders (the blocker)

Over 5000 DL commands across 39 frame loops, observed opcode frequencies are:

```
273x 0xBC (MOVEWORD)    156x 0xE7 (RDPPIPESYNC)
234x 0xBA (SETOTHERMODE_H)  117x 0xB8 (ENDDL)
 78x each of: 0x06 (DL), 0x02 (Factor5), 0x00 (NOOP), 0xF6 (FILLRECT),
              0xF7 (SETFILLCOLOR), 0xFF (SETCIMG), 0xED (SETSCISSOR),
              0xB9 (SETOTHERMODE_L)
 39x each of: 0x01 (MTX), 0x03 (MOVEMEM), 0x80 (Factor5), 0xB6/B7 (GEOMETRYMODE),
              0xE6/E8/E9 (RDP sync), 0xF8 (FOGCOLOR), 0xF9 (BLENDCOLOR), 0xFE (SETZIMG)
```

**Zero G_VTX (0x04), zero G_TRI1 (0xBF), zero G_TRI2 (0xB1).** Every frame is clear-rect + state-setup only, no geometry submitted through any standard F3DEX opcode. The game is literally drawing nothing but filled rectangles, 39 frames deep.

Combined with the disassembly of opcode `0x02`'s handler at IMEM `0x14F0` (a 40+-instruction loop iterating up to 512 times, writing to RSP COP2 vector registers via `mtc2 ... $v3[e]`), this means:

> **Factor 5's custom ucode bundles the entire vertex / transform / triangle pipeline into opcode `0x02`.** It doesn't use F3DEX's G_VTX/G_TRI* at all.

So opcode 0x02 is not a configuration command we can safely no-op — it's *the* geometry command. Stubbing it drops all 3D drawing, which is exactly what we observe.

## Path forward — two realistic options

Both are significant multi-week work; choose based on tooling preference:

1. **Full HLE reimplementation of opcode 0x02.** Reverse-engineer the full RSP handler at IMEM `0x14F0` plus the helpers it calls (`func_4001F14`, `func_4001F60`, ...). The handler reads from DMEM pointed to by `k1` and `gp`, runs vector math through COP2, and produces vertex/triangle output. We'd implement the equivalent transform + draw path in `GBI_F3DFACTOR5::op02`, emitting into RT64's existing vertex/triangle buffers. Cost: several weeks of RSP+vector-math reverse engineering; no unique tooling beyond `rabbitizer` + the existing RT64 rendering API.

2. **Extend RSPRecomp to handle graphics ucode, then go LLE.** The earlier LLE spike ([docs/lle-spike-report.md](lle-spike-report.md)) failed because RSPRecomp chokes on graphics-specific `mfc0` reads of DPC registers and on a handful of INVALID opcodes. Adding those handlers to RSPRecomp is a more concentrated codebase but unlocks every custom ucode game, not just Rogue Squadron. Cost: similar multi-week, but the payoff compounds.

The GBI-level plan (identification + opcode map) is effectively closed — no more progress is possible at that layer.

## Diagnostic infrastructure left in place

- DL dispatch trace (stderr), capped at 5000 entries — [rt64_interpreter.cpp:172-200](../lib/rt64/src/hle/rt64_interpreter.cpp#L172-L200). Remove when no longer needed.
- Null-guard on `RT64_LOG_PRINTF` — [rt64_common.h:49-50](../lib/rt64/src/common/rt64_common.h#L49-L50). Keep.
- Enhanced `osStartThread` / `osCreateThread` logging — [ultra_translation.cpp:16-33](../lib/N64ModernRuntime/librecomp/src/ultra_translation.cpp#L16-L33). Keep (cheap, useful).
- MSVC CRT asserts routed to stderr — [main.cpp](../src/main/main.cpp). Keep.
