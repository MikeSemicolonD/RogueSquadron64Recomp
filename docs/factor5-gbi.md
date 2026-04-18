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

## Open questions

1. **Opcode 0x02 handler at IMEM 0x14F0** — needs disassembly to confirm it's a config load / segment setup. Once identified, replace `op02_unknown` with a real handler.
2. **Opcode 0x80 sub-DL call hypothesis** — if `0x80`'s 24-bit field is a raw RDRAM pointer to a child DL, we should implement it as `state->rsp->displayList(ptr, push_return=true)` or similar. The address `0x720108` is plausible RDRAM (game uses 8 MB expansion pak).
3. **Why 0xFF**, `0xFE`, `0xF9`, `0xF8`, `0xF7`, `0xF6`, `0xED`, `0xE9`, `0xE8`, `0xE7`, `0xE6` all resolve in F3DEX — are their semantics actually preserved, or does Factor 5 overload any of them? (Rendering verification required.)

## Diagnostic infrastructure

- `RT64_LOG_PRINTF` is null-guarded in [lib/rt64/src/common/rt64_common.h](../lib/rt64/src/common/rt64_common.h) so logs without an open file no longer CRT-assert.
- DL dispatch trace (stderr) in [lib/rt64/src/hle/rt64_interpreter.cpp](../lib/rt64/src/hle/rt64_interpreter.cpp) — remove after Stage B.
- MSVC CRT asserts are routed to stderr from [src/main/main.cpp](../src/main/main.cpp).

## Next steps

1. Replace the `op02_unknown` no-op with a real handler by disassembling IMEM `0x14F0` (use `rabbitizer` or `mips64-gcc objdump` on the extracted text blob).
2. Prototype `op80_unknown` as a sub-DL call: treat the 24-bit field as a direct RDRAM pointer and push it onto the DL stack.
3. Rebuild, run, check whether anything presents to the swapchain.
