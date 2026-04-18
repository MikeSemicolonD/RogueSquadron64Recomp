# LLE Spike Report: Factor5 RSP Microcode via RSPRecomp

## Question

Can the N64Recomp RSPRecomp tool ingest Rogue Squadron's Factor5 custom graphics
RSP microcode (ROM offset `0x25610`, size `0x1FA0`) and produce recompiled C
that we can drive from a `M_GFXTASK` dispatch into RT64's LLE path?

## Setup

Config: `e:/Projects/N64Recomp/rogue_squadron_gfx.toml`

```toml
text_offset          = 0x25610
text_size            = 0x1FA0
text_address         = 0x04001000
rom_file_path        = "Star Wars - Rogue Squadron (USA).n64"
output_file_path     = "rsp/gfxMain.cpp"
output_function_name = "gfxMain"
```

ROM offset confirmed from splat yaml [roguesquadron.yaml:126](../../rogue_squadron64/roguesquadron.yaml#L126)
(`main/gspF3DEXMainText`).

Run: `./build_new/Release/RSPRecomp.exe rogue_squadron_gfx.toml`

## Result

**Failed.** Output file `rsp/gfxMain.cpp` was created but is zero bytes —
RSPRecomp opened the stream, encountered an unrecoverable error in
`process_instruction`, and terminated via an uncaught `std::runtime_error`
before writing any function body.

Stderr (complete):
```
Unhandled instruction: INVALID
Unhandled instruction: INVALID
Unhandled mfc0: 8
```

## Root causes

### 1. Graphics-only COP0 registers not handled

[rsp_recomp.cpp:124-140](../../N64Recomp/RSPRecomp/src/rsp_recomp.cpp#L124-L140)
handles only the COP0 registers needed by *audio* ucode — `SP_STATUS`,
`SP_DMA_FULL`, `SP_DMA_BUSY`, `SP_SEMAPHORE`, and `DPC_STATUS`. The default
branch throws. The code comment at line 135 is explicit:
> `// Good enough for the microcodes that would be recompiled (i.e. non-graphics ones)`

Factor5 ucode reads a wide set of COP0 registers. Histogram of `mfc0 rd`
values across the 0x1FA0 blob (33 occurrences):

| rd  | count | meaning                               | handled? |
|-----|-------|---------------------------------------|----------|
| 0   | 10    | SP_MEM_ADDR                           | no (read not in switch) |
| 1   | 7     | SP_DRAM_ADDR                          | no |
| 2   | 4     | SP_RD_LEN                             | no |
| 3   | 4     | SP_WR_LEN                             | no |
| 4   | 1     | SP_STATUS                             | **yes** |
| 6   | 1     | SP_DMA_BUSY                           | **yes** |
| 7   | 2     | SP_SEMAPHORE                          | **yes** |
| 8   | 1     | DPC_START  ← **throw site**           | no |
| 21  | 1     | (>= 16; rabbitizer did not mask)      | no |
| 29  | 1     | (>= 16)                               | no |
| 31  | 1     | (>= 16)                               | no |

The first encountered unknown (`rd = 8`, DPC_START) throws and kills the run.
Even patching past it, at least seven additional rd values need support.

### 2. Zero `mtc0` writes in the blob

Static scan found **0** `mtc0` instructions in the text region. Graphics ucode
must write RDP command buffer addresses (DPC_START / DPC_END), SP DMA regs,
etc. The absence of any `mtc0` in the 0x1FA0-byte region is strong evidence
that this text segment is the *bootstrap/dispatch* overlay only — the actual
draw/DMA code lives in **further overlays** streamed into IMEM at runtime.
Factor5 ucode is known to be overlay-heavy; this is consistent with that.

### 3. `INVALID` opcodes

Two `Unhandled instruction: INVALID` messages precede the throw. These are
not fatal in Release (the `assert(false)` is compiled out and the function
returns false), but they mean rabbitizer rejected two instruction words.
Candidates: (a) data words mixed into the text region, (b) Factor5-specific
opcode extensions, or (c) instructions after the real end of the text where
padding happens to decode as garbage. Without suppressing the later throw we
can't see a full list.

## Implications for real LLE integration

Running RSPRecomp "as is" over the ucode is not viable. To get to the point
where we could hand a recompiled `gfxMain` to the runtime, the following
work is needed — in rough order:

1. **Extend RSPRecomp COP0 coverage** — `expected_c0_reg_value` and
   `c0_reg_write_action` need every SP/DPC register the ucode touches. For
   SP_MEM_ADDR / SP_DRAM_ADDR reads, we need to return the context's tracked
   DMA pointers (not constants). For DPC reads, the tool must model a pending
   RDP command buffer pointer (DPC_START / DPC_END / DPC_CURRENT) rather than
   return 0.
2. **Identify and declare Factor5 overlays** — the Factor5 ucode streams
   code overlays from DRAM into IMEM during a task. The tool already supports
   this via `RSPRecompilerOverlayConfig` + `overlay_slots`, but we have no
   map of where those overlays live in ROM or which IMEM slots they target.
   Discovering that requires either (a) disassembling Rogue Squadron's boot
   DMA path, (b) cross-referencing published Factor5 ucode work in the
   Indy64 / Naboo decomp projects, or (c) instrumenting the game at runtime
   to log DMA addresses hitting IMEM.
3. **RDP command capture** — even with a clean recompile, the RSP ucode
   typically emits RDP commands via `DPC_START`/`DPC_END` pointing into a
   ring buffer in RDRAM. RT64's LLE path (`processDisplayLists(..., false)`)
   reads that buffer. The recompiled C needs a way to expose the current
   DPC_END to the host after the task returns, or to write directly into the
   OSTask's `output_buff` range. Needs a new runtime contract.
4. **`INVALID` opcode triage** — only actionable after the mfc0 throw is
   fixed. Likely requires rabbitizer updates or explicit data-region splits
   in the config.

## Recommendation

**LLE via RSPRecomp is a multi-week project, not a quick wire-up.** The tool
was explicitly scoped to audio ucode and needs meaningful extension before
it can handle graphics — and that's *before* we tackle Factor5's overlay
structure, which is the real hard problem.

Before committing to that path, consider the cheaper alternatives:

- **Teach RT64 to recognize the Factor5 GBI.** RT64's HLE database is
  extensible. If Indy64/Naboo decomp communities have already reverse
  engineered the Factor5 display-list format, adding it as a GBI profile in
  [rt64_interpreter.cpp](../lib/rt64/src/hle/rt64_interpreter.cpp) is far
  less work than full LLE.
- **Ship an "audio only" build first.** Stubbing graphics at `send_dl` and
  getting audio + CPU simulation running end-to-end would prove out the rest
  of the runtime plumbing while the GBI/LLE question is investigated.

If we still want to pursue LLE, the right next spike is **mapping Factor5's
overlay structure** — that's the gating unknown and it's independent of
RSPRecomp tool work.

## Artifacts

- Config: [e:/Projects/N64Recomp/rogue_squadron_gfx.toml](../../N64Recomp/rogue_squadron_gfx.toml)
- Run log: [build/rsp_spike.log](../build/rsp_spike.log) (3 stderr lines, no stdout)
- Empty output: `e:/Projects/N64Recomp/rsp/gfxMain.cpp` (0 bytes)
- No changes to runtime, CMake, or dispatch code.
