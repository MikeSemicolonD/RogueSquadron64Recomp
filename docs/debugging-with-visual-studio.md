# Debugging the recompile with Visual Studio

The N64Recomp / RSPRecomp toolchain emits plain C, so all of Visual Studio's
normal C/C++ debugger features work on this project unmodified — including
on the recompiled CPU code under `RecompiledFuncs\` and the recompiled RSP
audio ucode under `build/factor5_ucode/`. Reaching for the debugger before
adding more `printf` is almost always faster.

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
recompiled CPU code, or `musyx_audio_recompiled.c` for the RSP audio
synth.

## The recompile context

Most recompiled functions take `(uint8_t* rdram, recomp_context* ctx)`.
`ctx` is the MIPS register file plus FP regs, and is the most important
thing to watch when stepping:

```
ctx->r0 .. ctx->r31    — general-purpose MIPS registers (uint32_t)
ctx->f0 .. ctx->f31    — FP registers; .d = double, .f = float, .u64 = raw bits, .u32l = lo word
ctx->lo, ctx->hi       — multiply/divide result registers
```

`rdram` is the RAM blob, based at KSEG0 `0x80000000`; the helpers `MEM_B / MEM_H / MEM_W / LD`
(byte / halfword / word / doubleword, in `lib/N64ModernRuntime/N64Recomp/include/recomp.h`)
translate MIPS-style `0x80xxxxxx` addresses to host bytes inside `rdram`. RDRAM is stored as
native little-endian 32-bit words, so a word lives at `rdram + (addr - 0x80000000)`, a halfword
at `rdram + ((addr ^ 2) - 0x80000000)` and a byte at `rdram + ((addr ^ 3) - 0x80000000)`. GPRs are
64-bit and sign-extended, so compare them as `(uint32_t)ctx->rN`.

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
| Pointer arithmetic produces a kernel address | `(uint32_t)ctx->r17 >= 0x80800000` |
| Specific memory address gets touched (`sw $x, 0x10($a0)`) | `(uint32_t)(ctx->r4 + 0x10) == 0x80128000` |
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
  `rdram + ((uint32_t)ctx->r4 - 0x80000000)` to see what the surrounding
  RDRAM looks like at the moment of the read.
- **Trace Into Specific Function** (right-click → Step Into Specific) —
  recompiled call sites show as a sequence of register stores then a `func_XXXX(rdram, ctx)` call; this lets you skip the stores and step
  directly into the next recompiled function.

### When data breakpoints don't work — in-source `[mem-watch]` instrumentation

VS hardware data breakpoints have only 4 slots and sometimes don't fire on
writes that go through MEM_W / SD store macros (memory address changes
across runs from VirtualAlloc, possible cache-line / TLB interactions).
A reliable alternative is to add a tiny watcher *inside* the recompile
itself.

Add at the top of a function whose entry you want to sample:

```c
{
    static uint64_t prev = 0;
    uint64_t cur = *(uint64_t*)(rdram + 0x3DDB0);   // your suspect address
    if (cur != prev) {
        fprintf(stderr, "[mem-watch] funcname entry: 0x3DDB0 was 0x%016llX, now 0x%016llX\n",
            (unsigned long long)prev, (unsigned long long)cur);
        fflush(stderr);
        prev = cur;
    }
}
```

Place these at multiple points in a call chain (the chronic-crasher
function plus its callers) — the value transitions get bracketed between
the function whose watcher last logged "good" and the one that first
logged "bad", narrowing the corrupter to a small region of code.

For finding the offending function via an indirect dispatch loop (e.g.
`LOOKUP_FUNC(ctx->r2)(rdram, ctx);`), use a *one-shot* check after the
call so it doesn't disturb timing on every iteration:

```c
{
    uint32_t f = (uint32_t)ctx->r2;
    LOOKUP_FUNC(ctx->r2)(rdram, ctx);
    static int logged = 0;
    if (!logged && *(uint64_t*)(rdram + 0x3DDB0) == 0xFFFFFFFFFFFFFFFFULL) {
        logged = 1;
        fprintf(stderr, "[mem-watch] FIRST corruption observed after dispatching func 0x%08X\n", f);
        fflush(stderr);
    }
}
```

Heavier `before/after` watchers (read both before *and* after each
dispatch) can shift threading timing enough to mask race-condition bugs —
prefer the lightest possible probe.

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
| `RecompiledFuncs\funcs_*.c` | Recompiled CPU code from the ROM; thousands of `func_8xxxxxxx` functions (in-repo, gitignored, regenerated). |
| `build\factor5_ucode\musyx_audio_recompiled.c` | Recompiled MusyX audio synth RSP ucode. |
| `build\factor5_ucode\factor5_boot_recompiled.c` | Recompiled boot ucode that DMAs the audio ucode data into place. |
| `lib\rt64\src\hle\rt64_interpreter.cpp` | RT64's HLE display-list interpreter loop. |
| `lib\rt64\src\gbi\rt64_gbi.cpp`, `lib\rt64\src\hle\rt64_rsp.cpp` | Ucode identification selects `GBIUCode::F3DFACTOR5` (`GBI_F3DFACTOR5::setup`); `rt64_rsp.cpp` applies its F5-specific RSP state. |
| `lib\rt64\src\gbi\rt64_gbi_f3dfactor5.cpp` | The Factor 5 opcode handlers. |

## When printf is still the right tool

A debugger is best for catching a single failure once you know roughly
where to look. Logging is still better for:

- Following a high-rate event you can't stop on without losing context
  (RDP submissions, VI retraces).
- Validating that a code path is reached at all.
- Comparing across threads when stepping would change the timing.

Trace categories are off by default and enabled with `ROGUESQ_LOG_*`
environment variables; the catalog is [debug-trace-env-vars.md](debug-trace-env-vars.md).
New probes use the rate-limited `static int n=0; if (++n<=N || (n%K)==0) { … }`
pattern. Prefer redirecting stderr to a file
(`> log.txt 2>&1`) — Windows console I/O is synchronous and orders of
magnitude slower than file I/O, and at high event rates a console-bound
process will appear to hang as the message-pump starves.
