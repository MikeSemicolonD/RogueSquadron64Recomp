# Debugging with Visual Studio

N64Recomp and RSPRecomp turn the game into plain C, so Visual Studio's normal debugger works on all of it: the recompiled game code in `RecompiledFuncs\`, the recompiled audio microcode in `build\factor5_ucode\`, the runtime and RT64. Breakpoints, stepping, watches and the memory window all behave as they do in any C++ project. Reaching for the debugger before adding more `printf` calls is usually faster.

Every recompiled instruction keeps its original MIPS address and disassembly as a comment, for example `// 0x800907B0: div.d $f20, $f0, $f2`, so any line you stop on maps straight back to the original ROM code.

This page covers setup, how to read recompiled code, breakpoint techniques, crashes and hangs, and when logging is the better tool.

## Setup

1. Build the Debug configuration. This assumes you have done the first-time steps in the [README](../README.md#building) (decomp ELF, `RecompiledFuncs/`, microcode); after that, the recompiler defaults to the one built inside `build/`:

   ```sh
   cmake -B build -T ClangCL
   cmake --build build --config Debug --target RogueSquadron64Recomp
   ```

2. Open `build\RogueSquadron64Recomp.sln` in Visual Studio 2022.
3. In Solution Explorer, right-click `RogueSquadron64Recomp` and pick **Set as Startup Project**.
4. Press F5 to start it under the debugger.

`Ctrl+,` (Go to All) finds files quickly: type `funcs_24.c` for recompiled game code or `musyx_audio_recompiled.c` for the audio synth.

## Reading recompiled code

A recompiled function looks like `void func_80012345(uint8_t* rdram, recomp_context* ctx)`. The two arguments are what you will watch most:

- **`ctx`** is the MIPS CPU state:

  | Field | Contents |
  |---|---|
  | `ctx->r0` .. `ctx->r31` | General-purpose registers. They are 64-bit and sign-extended, so compare them as `(uint32_t)ctx->rN`. |
  | `ctx->f0` .. `ctx->f31` | Floating-point registers. `.d` = double, `.f` = float, `.u64` = raw bits, `.u32l` = low word. |
  | `ctx->lo`, `ctx->hi` | Multiply/divide results. |

  MIPS register names map to numbers as usual: `a0`-`a3` are `r4`-`r7`, `v0` is `r2`, `sp` is `r29`, `ra` is `r31`.

- **`rdram`** is the N64's RAM. A game address `0x80xxxxxx` lives at `rdram + (addr - 0x80000000)`, but RAM is stored as little-endian 32-bit words, so smaller values are swizzled:

  | Size | Host location | Macro |
  |---|---|---|
  | word (32-bit) | `rdram + (addr - 0x80000000)` | `MEM_W` |
  | halfword | `rdram + ((addr ^ 2) - 0x80000000)` | `MEM_H` |
  | byte | `rdram + ((addr ^ 3) - 0x80000000)` | `MEM_B` |

  The macros are in `lib/N64ModernRuntime/N64Recomp/include/recomp.h`.

The game's function names come from the decomp. To find what calls a function, or what it calls, use `tools/rz/rzq.py` ([tools/rz/README.md](../tools/rz/README.md)).

## Breakpoint techniques

### Conditional breakpoints

Game code runs tight loops, so an ordinary breakpoint inside one can stop thousands of times before the interesting case. Make it conditional instead: right-click the breakpoint, pick **Conditions…**, and enter an expression that is true only when things go wrong.

| To stop when... | Condition |
|---|---|
| `f2` becomes NaN | `ctx->f2.d != ctx->f2.d` |
| a pointer leaves the game's 8 MB | `(uint32_t)ctx->r17 >= 0x80800000` |
| a store hits one address (`sw $x, 0x10($a0)`) | `(uint32_t)(ctx->r4 + 0x10) == 0x80128000` |
| the Nth time only | change the dropdown to **Hit Count**, "is equal to N" |

Conditions are evaluated every time the line runs, which slows it down a lot. Use them for one investigation and then remove them.

### Skipping past boot

Many bugs only show after the logos or in a particular mission.

- **Disable** (don't delete) noisy breakpoints during boot and re-enable them later from Debug > Windows > Breakpoints (`Ctrl+Alt+B`).
- Use a **tracepoint** (right-click a breakpoint, Actions, "Log a message to Output window") to print a counter without stopping, then condition later breakpoints on that counter.
- Get to the right screen without a controller: `ROGUESQ_BOOT_TARGET=level:<id>` drives the menus to a mission. See [debug-trace-env-vars.md](debug-trace-env-vars.md).

### Tracing a bad value backwards

Recompiled code doesn't fold temporaries the way a normal compiler does: every register write is its own line. You can step backwards through a function and see exactly which load or arithmetic op produced a bad value. Useful tools:

- **Make Object ID** on `ctx` (right-click it in Watch or Locals) lets you refer to it as `$1` and compare register state across calls or threads.
- **Memory window** (Debug > Windows > Memory > Memory 1): enter `rdram + ((uint32_t)ctx->r4 - 0x80000000)` to see the RAM around a pointer.
- **Step Into Specific** (right-click a line): call sites are a run of register stores followed by `func_XXXX(rdram, ctx)`; this jumps straight into the call.

### Recompiler bug or game logic?

| Symptom | Likely cause |
|---|---|
| `f2.u64 == 0x7FF8000000000000` (the standard quiet NaN) | An earlier float op produced NaN: a game-logic divide by zero, square root of a negative, and so on. |
| A NaN with random-looking low bits | Uninitialized RAM read as a double. |
| An address that points into the host stack, not RDRAM | `r1` or `r29` clobbered: a recompiler bug. |
| A sensible address, but the bytes look wrong | Byte-order mismatch for that load size. |
| Same code, same RAM, different result each run | A race with another thread (graphics, audio, scheduler). Check the Threads window. |

If a recompiled function hangs or crashes on data that decodes fine offline, suspect the recompiler's translation of a rare instruction (the `bltzal` / `bgezal` link-branches caused this once) before digging into the game system.

## Finding who changed a value

Visual Studio's data breakpoints have only four slots, and RDRAM moves between runs, so they are awkward here. The game has two built-in alternatives, both described in [debug-trace-env-vars.md](debug-trace-env-vars.md):

- `ROGUESQ_WATCH_ADDRS=0x80128000,...` logs every change to up to 16 addresses, once per frame.
- `ROGUESQ_DATA_BP=0x80128000` (Windows) sets a hardware breakpoint on one RAM word on every thread and logs each access with the recompiled function and source line.

If you need a finer check, add a probe at the top of a suspect function in `RecompiledFuncs\funcs_*.c`. Those files are regenerated, so this is only for diagnosis; a real fix goes in `rogue_squadron.toml` (see [AGENTS.md](../AGENTS.md#overriding-recompiled-functions)).

```c
{
    static uint64_t prev = 0;
    uint64_t cur = *(uint64_t*)(rdram + 0x3DDB0);
    if (cur != prev) {
        fprintf(stderr, "[mem-watch] funcname: 0x3DDB0 0x%016llX -> 0x%016llX\n",
            (unsigned long long)prev, (unsigned long long)cur);
        fflush(stderr);
        prev = cur;
    }
}
```

Put a few along a call chain: the bad write happened between the last probe that saw a good value and the first that saw a bad one. For an indirect dispatch loop (`LOOKUP_FUNC(ctx->r2)(rdram, ctx);`), check once after the call so the probe doesn't change timing on every iteration:

```c
{
    uint32_t f = (uint32_t)ctx->r2;
    LOOKUP_FUNC(ctx->r2)(rdram, ctx);
    static int logged = 0;
    if (!logged && *(uint64_t*)(rdram + 0x3DDB0) == 0xFFFFFFFFFFFFFFFFULL) {
        logged = 1;
        fprintf(stderr, "[mem-watch] first corruption after func 0x%08X\n", f);
        fflush(stderr);
    }
}
```

Keep probes as light as possible. Heavier ones (checking before and after every call) can shift thread timing enough to hide a race.

## Crashes and hangs

Debug crashes on the x64 build. The 32-bit build has no 4 GB guard region around emulated memory, so a bad game pointer wraps into the 512 MB RDRAM window instead of crashing where it goes wrong.

**Crash.** The crash handlers in `src\main\crash.cpp` print a stack trace to stderr and write a full minidump to `dumps\crash-dumps\crash_<date>_<time>.dmp`. Open it in Visual Studio (File > Open > File, then **Debug with Native Only**). It needs the matching exe and PDB from `build\Debug\`. `python tools/inspect-dump.py` lists the dump's threads and marks the few running game code (`in_exe`); those are the ones worth opening.

**Hang.** While the game is frozen, run `pwsh tools/dump-game.ps1` from another shell to capture it (F12 also writes a dump, but not if the window has stopped responding). Then:

- `python tools/reconstruct-freeze.py` rebuilds the game's threads and message queues from RAM and shows which thread is waiting on what.
- `python tools/host-stacks.py` prints symbolized host stacks (Debug build only; it refuses a dump from a different exe).

[AGENTS.md](../AGENTS.md#when-investigating-a-new-crash) lists the common crash signatures and what they usually mean.

## Files worth knowing

| File | What it is |
|---|---|
| `RecompiledFuncs\funcs_*.c` | Recompiled game code, thousands of `func_8xxxxxxx` and named functions. Generated locally and gitignored. |
| `build\factor5_ucode\musyx_audio_recompiled.c` | Recompiled MusyX audio synth microcode. |
| `build\factor5_ucode\factor5_boot_recompiled.c` | Recompiled boot microcode that loads the audio microcode's data. |
| `src\game\toml_hooks.c` | The bodies of the hooks `rogue_squadron.toml` inserts into game functions. |
| `lib\rt64\src\hle\rt64_interpreter.cpp` | RT64's display-list interpreter loop. |
| `lib\rt64\src\gbi\rt64_gbi_f3dfactor5.cpp` | The Factor 5 display-list opcode handlers. |
| `lib\rt64\src\gbi\rt64_gbi.cpp`, `lib\rt64\src\hle\rt64_rsp.cpp` | Microcode identification (selects `F3DFACTOR5`) and its RSP state. |

## When logging is the better tool

The debugger is best for catching one failure once you know roughly where it is. Logging is better for:

- following high-rate events you can't stop on without losing context (display-list submissions, VI retraces),
- checking that a code path runs at all,
- comparing threads when stepping would change the timing.

Logging categories are off by default and enabled with `ROGUESQ_LOG_*` environment variables; [debug-trace-env-vars.md](debug-trace-env-vars.md) lists them all. Rate-limit new probes (`static int n=0; if (++n<=10 || (n%50)==0) { ... }`) and send stderr to a file, not the console: Windows console output is synchronous and slow enough at high rates to make the game look hung.
