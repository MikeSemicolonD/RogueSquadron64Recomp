# Debugging the recompile with Visual Studio

The N64Recomp / RSPRecomp toolchain emits plain C, so all of Visual Studio's
normal C/C++ debugger features work on this project unmodified — including
on the recompiled CPU code under `E:\Projects\N64Recomp\RecompiledFuncs\` and
the recompiled RSP graphics ucode under `build/factor5_ucode/`. Reaching for
the debugger before adding more `printf` is almost always faster.

The recompiler keeps the original MIPS PC of every instruction as a comment,
e.g. `// 0x800907B0: div.d $f20, $f0, $f2`, so once you have a host-side
breakpoint you can correlate exactly to the disassembly of the original ROM.

## Setup

1. Build a Debug configuration:
   ```sh
   cmake -B build -T ClangCL
   cmake --build build --config Debug --target RogueSquadron64Recomp
   ```
2. Open `build\RogueSquadron64Recomp.sln` in Visual Studio 2022.
3. In Solution Explorer, right-click `RogueSquadron64Recomp` →
   **Set as Startup Project**.
4. F5 (Debug → Start Debugging) launches the binary under the debugger.

`Ctrl+,` opens "Go to All" — type a filename like `funcs_24.c` to jump to
recompiled CPU code, or `factor5_ucode_recompiled.c` for the RSP graphics
ucode.

## The recompile context

Most recompiled functions take `(uint8_t* rdram, recomp_context* ctx)`.
`ctx` is the MIPS register file plus FP regs, and is the most important
thing to watch when stepping:

```
ctx->r0 .. ctx->r31    — general-purpose MIPS registers (uint32_t)
ctx->f0 .. ctx->f31    — FP registers; .d = double, .f = float, .u64 = raw bits, .u32l = lo word
ctx->lo, ctx->hi       — multiply/divide result registers
```

`rdram` is the 8 MB RAM blob; the helpers `MEM_B / MEM_H / MEM_W / LD`
(byte / halfword / word / doubleword) translate MIPS-style `0x80xxxxxx`
KSEG0 addresses to host bytes inside `rdram` with the right endian swizzle.
`(addr - 0x2250) & 0xFFFFFF` is a useful expression for converting a
MIPS-space address to the physical RDRAM offset for inspection in the
**Memory** window.

## Common patterns

### Catching a specific failure mode without stopping every iteration

The recompile faithfully reproduces tight loops. Setting an unconditional
breakpoint inside one will break thousands of times before the failure
condition appears. Use **conditional breakpoints** instead:

- Right-click the red dot → **Conditions…**
- Tick **Conditional Expression**
- Enter a host-language predicate that's true only at the failure

Examples:

| Failure | Conditional expression |
|---|---|
| `ctx->f2` becomes NaN | `ctx->f2.d != ctx->f2.d` |
| Pointer arithmetic produces a kernel address | `ctx->r17 >= 0x80800000` |
| Specific memory address gets touched | `ctx->r1 - 0x2250 == 0x80128000` |
| Nth iteration only | switch the dropdown from Conditional Expression to **Hit Count**, set "is equal to N" |

Conditional breakpoints are evaluated in the debugger every time the line
executes, so they slow that line down significantly while in use — fine for
a single failure-isolation session, not something to leave on.

### Running to "the next anomalous frame"

Many regressions only manifest after the title fade or N64 logo transition.
Two ways to skip the boring frames:

1. **Disable** (don't delete) any noisy breakpoints during boot, then enable
   them once the logo is on screen via Debug → Windows → Breakpoints
   (`Ctrl+Alt+B`).
2. Add a **tracepoint** somewhere known-good (e.g. an `osViSwapBuffer`
   handler) — right-click red dot → Actions → "Log a message to Output
   window" — to print frame counts without halting. Then set conditional
   breakpoints downstream that reference that frame counter.

### Walking back through a NaN / bad-value

The MIPS recompile does not collapse temporaries the way a high-level
compiler would: every register write is its own line in the C output, so
you can scrub backwards through a few hundred lines of one function and
see exactly which load or arithmetic op produced the value you don't like.
Some helpful debugger features:

- **Make Object ID** on `ctx` (right-click in Watch / Locals → Make Object
  ID, then `Ctrl+H` to refer to it as `$1`). Lets you compare register
  state across calls or threads quickly.
- **Memory window** (Debug → Windows → Memory → Memory 1) — paste
  `rdram + ((ctx->r1 - 0x2250) & 0xFFFFFF)` to see what the surrounding
  RDRAM looks like at the moment of the read.
- **Trace Into Specific Function** (right-click → Step Into Specific) —
  recompiled call sites show as a sequence of register stores then a `func_XXXX(rdram, ctx)` call; this lets you skip the stores and step
  directly into the next recompiled function.

### Recompile bug vs. game logic

A few patterns reliably distinguish a recompile bug from genuine in-game
state:

| Symptom | Likely cause |
|---|---|
| `f2.u64 == 0x7FF8000000000000` (canonical quiet NaN) | A previous FP op produced NaN — game logic divide-by-zero, sqrt of negative, etc. |
| `f2.u64` non-canonical NaN with random low bits | Reading uninitialized RDRAM as a double |
| Address loaded is at a host-stack offset, not RDRAM | `r1` / `r29` being clobbered — recompile dispatch bug |
| Address is sensible but the bytes look wrong | Endian swizzle mismatch — likely a missing `BSWAP_*` macro for that load width |
| Same code path, same RDRAM → different outcome each run | Race with another thread (gfx, audio, scheduler) — check `Threads` window |

### The crash handler

`src\main\main.cpp` installs a Win32 `SetUnhandledExceptionFilter` and a
SIGABRT handler that print symbolicated stack traces using DbgHelp.
Output goes to stderr. If a crash happens **outside the debugger** (e.g. a
release build a tester is running), the printed `rva` values can be matched
back to source via the .pdb in `build/Debug/RogueSquadron64Recomp.pdb`:

```
windbg -z RogueSquadron64Recomp.exe
0:000> ln <module-rva>
```

…or simply load the PDB in VS via File → Open → File on the .pdb, then
**Debug → Windows → Disassembly** with the rva pasted into Address.

## Specific files worth knowing about

| File | What it is |
|---|---|
| `E:\Projects\N64Recomp\RecompiledFuncs\funcs_*.c` | Recompiled CPU code from the rom; thousands of `func_8xxxxxxx` functions. |
| `build\factor5_ucode\factor5_ucode_recompiled.c` | Recompiled Factor 5 graphics RSP ucode. The dispatch loop is at `L_1090`; opcode handlers branch from there. |
| `build\factor5_ucode\factor5_boot_recompiled.c` | Recompiled boot ucode that DMAs the main ucode into IMEM. |
| `src\rsp\dpc_bridge.cpp` | Where DPC_END writes from the ucode become RT64 RDP submissions. |
| `lib\rt64\src\hle\rt64_interpreter.cpp` | RT64's HLE display-list interpreter (currently bypassed for Factor 5 — see README). |

## When printf is still the right tool

A debugger is best for catching a single failure once you know roughly
where to look. Logging is still better for:

- Following a high-rate event you can't stop on without losing context
  (RDP submissions, VI retraces).
- Validating that a code path is reached at all.
- Comparing across threads when stepping would change the timing.

The codebase has rate-limited `fprintf(stderr, "[name] …")` patterns gated
by `static int n=0; if (++n<=N || (n%K)==0) { … }` already — re-enable a
specific category by searching for `if(false) fprintf(stderr, "[name]"`
and flipping `if(false)` → `if(true)`. Categories include `[trace]`,
`[diag-*]`, `[ck]`, `[capture]`, `[ucode]`, `[mqdrain]`, `[apply]`,
`[cycle0]`, `[cycle1]`. Prefer redirecting stderr to a file
(`> log.txt 2>&1`) — Windows console I/O is synchronous and orders of
magnitude slower than file I/O, and at high event rates a console-bound
process will appear to hang as the message-pump starves.
