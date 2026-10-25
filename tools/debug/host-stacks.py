"""Symbolize host-thread stacks from an external minidump.

For each host thread: scan the thread's stack memory for return addresses that
land in the exe and symbolize them via the exe's PDB with llvm-symbolizer (a
naive but effective pseudo-backtrace). Prints only threads whose stack carries
frames of interest so the cooperative "current" thread (viRetrace, MAIN, etc.)
is easy to spot.

CRITICAL: the dump's exe must match build/Debug/RogueSquadron64Recomp.{exe,pdb}.
If the game was rebuilt after the dump was taken, the RVAs resolve to the WRONG
functions. This script compares the PE TimeDateStamp of the dump's module vs the
on-disk exe and REFUSES to symbolize on a mismatch (capture a fresh dump with the
current build instead).

Usage: python tools/debug/host-stacks.py [dump.dmp] [--force]
"""
import sys, subprocess, struct, bisect
from pathlib import Path
from minidump.minidumpfile import MinidumpFile

LLVM = r"C:\Program Files\LLVM\bin\llvm-symbolizer.exe"
EXE = r"E:\Projects\RogueSquadron64Recomp\build\Debug\RogueSquadron64Recomp.exe"
# functions worth flagging on a stack (substring match, post-symbolization)
INTEREST = ("waitForPostSwapAck", "viRetrace", "musyx", "SynthFrame", "submitGfx",
            "spTaskScheduler", "AiAudio", "drainAudio", "present", "Present",
            "osRecvMesg", "osSendMesg", "wait_for_resumed", "run_next_thread")


def pe_timestamp(fp):
    with open(fp, "rb") as f:
        d = f.read(0x400)
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    ib = struct.unpack_from("<Q", d, pe + 24 + 24)[0]
    ts = struct.unpack_from("<I", d, pe + 8)[0]
    return ts, ib


def main():
    force = "--force" in sys.argv
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    dumps = sorted(Path("dumps/crash-dumps").glob("*_external.dmp"),
                   key=lambda p: p.stat().st_mtime, reverse=True)
    path = args[0] if args else str(dumps[0])
    md = MinidumpFile.parse(path)
    reader = md.get_reader()
    exe = next(m for m in md.modules.modules
               if m.name.lower().endswith("roguesquadron64recomp.exe"))
    base, size = exe.baseaddress, exe.size
    cur_ts, image_base = pe_timestamp(EXE)
    dump_ts = getattr(exe, "timestamp", 0)
    print(f"dump {path}")
    print(f"  exe base 0x{base:X} size 0x{size:X}")
    print(f"  dump module TimeDateStamp 0x{dump_ts:X}  vs current exe 0x{cur_ts:X}")
    if dump_ts != cur_ts:
        print("  *** BINARY MISMATCH: current build differs from the dump's exe.")
        print("  *** Symbols would be WRONG. Capture a fresh dump with the current")
        print("  *** build (do not rebuild between repro and dump), or pass --force.")
        if not force:
            return 1
    print()

    def sym(rvas):
        if not rvas:
            return {}
        inp = "".join(f"0x{image_base + r:X}\n" for r in rvas)
        out = subprocess.run([LLVM, "--obj=" + EXE, "-f", "-C"],
                             input=inp, capture_output=True, text=True).stdout
        blocks = out.strip().split("\n\n")
        res = {}
        for r, b in zip(rvas, blocks):
            lines = [x for x in b.splitlines() if x.strip()]
            res[r] = lines[0] if lines else "??"
        return res

    # segment map for bounded stack reads
    S = sorted((s.start_virtual_address, s.size) for s in md.memory_segments_64.memory_segments)
    starts = [x[0] for x in S]

    def read_stack(rsp, want=0x6000):
        i = bisect.bisect_right(starts, rsp) - 1
        if i < 0 or not (S[i][0] <= rsp < S[i][0] + S[i][1]):
            return b""
        n = min(want, S[i][0] + S[i][1] - rsp)
        try:
            return reader.read(rsp, n)
        except Exception:
            return b""

    for t in md.threads.threads:
        rsp = t.ContextObject.Rsp
        data = read_stack(rsp)
        seen, uniq = set(), []
        for off in range(0, len(data) - 8, 8):
            v = struct.unpack_from("<Q", data, off)[0]
            if base <= v < base + size and (v - base) not in seen:
                seen.add(v - base); uniq.append(v - base)
        if not uniq:
            continue
        s = sym(uniq[:28])
        flagged = [nm for r, nm in s.items() if any(k in nm for k in INTEREST)]
        marker = "  <<< " + ", ".join(sorted(set(flagged))[:4]) if flagged else ""
        print(f"=== TID {t.ThreadId} ({len(uniq)} exe frames){marker} ===")
        for r in uniq[:20]:
            print(f"    +0x{r:X}  {s.get(r,'??')}")
        print()


if __name__ == "__main__":
    sys.exit(main())
