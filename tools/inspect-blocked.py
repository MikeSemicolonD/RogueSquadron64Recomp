"""For each thread in the dump that's parked in moodycamel::Semaphore::wait
(i.e. blocked on osRecvMesg via wait_for_resumed), walk the stack to find
the recompiled MIPS entry function. Report counts.

Usage:  python tools/inspect-blocked.py [crash_*.dmp]
"""
import sys
from pathlib import Path
from minidump.minidumpfile import MinidumpFile

if len(sys.argv) < 2:
    dumps = sorted(Path("dumps/crash-dumps").glob("crash_*.dmp"), key=lambda p: p.stat().st_mtime, reverse=True)
    if not dumps:
        print("No crash_*.dmp found.")
        sys.exit(1)
    path = str(dumps[0])
else:
    path = sys.argv[1]

print(f"Reading {path}")
md = MinidumpFile.parse(path)

# Find the main exe module
exe_module = None
for m in md.modules.modules:
    name = m.name.split("\\")[-1].split("/")[-1].lower()
    if "roguesquadron" in name and name.endswith(".exe"):
        exe_module = m
        break

if not exe_module:
    print("Couldn't find the main exe module.")
    sys.exit(1)

exe_lo = exe_module.baseaddress
exe_hi = exe_lo + exe_module.size

# For each thread, scan the top of its stack memory for return addresses
# pointing into our exe. Counting frames that fall within the exe gives
# us a rough "depth into our code" — and the addresses themselves are
# RVAs we can resolve via VS later.

# Build memory range index: each MemoryRange64 has start_address and size,
# raw bytes accessible via minidump's read_physical or similar.

# Read top of each thread's stack: first ~8 KB
print(f"\nThreads with frames pointing into RogueSquadron64Recomp.exe (exe range 0x{exe_lo:X}..0x{exe_hi:X}):\n")

reader = md.get_reader().get_buffered_reader()

for t in md.threads.threads:
    ctx = t.ContextObject
    if ctx is None:
        continue
    rsp = getattr(ctx, 'Rsp', 0)
    if rsp == 0:
        continue
    # Read first 4 KB of stack
    try:
        reader.move(rsp)
        stack_bytes = reader.read(4096)
    except Exception:
        continue

    # Walk 8-byte words, look for ones pointing into the exe
    frames_in_exe = []
    for i in range(0, len(stack_bytes) - 7, 8):
        v = int.from_bytes(stack_bytes[i:i+8], "little")
        if exe_lo <= v < exe_hi:
            rva = v - exe_lo
            if not frames_in_exe or frames_in_exe[-1] != rva:
                frames_in_exe.append(rva)
            if len(frames_in_exe) >= 10:
                break

    if frames_in_exe:
        rip = getattr(ctx, 'Rip', 0)
        rip_rva = rip - exe_lo if exe_lo <= rip < exe_hi else None
        rip_str = f"+0x{rip_rva:X} (in_exe)" if rip_rva is not None else f"0x{rip:X} (kernel/other)"
        print(f"TID {t.ThreadId:>6}  RIP {rip_str}")
        print("  stack RVAs (likely return addresses, top-down):")
        for r in frames_in_exe[:6]:
            print(f"    +0x{r:X}")
