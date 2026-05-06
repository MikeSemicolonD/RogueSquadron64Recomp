"""Quick triage of a minidump: list threads, mark which ones are in known
'idle' patterns, print top RIP for each, flag anything suspicious. Doesn't
need symbols — works on raw addresses; correlate with our recompiled .pdb
via VS or dumpbin if a thread looks suspicious."""
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

# Find the main exe module so we can compute RVAs for each thread's RIP
exe_module = None
for m in md.modules.modules:
    name = m.name.split("\\")[-1].split("/")[-1].lower()
    if "roguesquadron" in name and name.endswith(".exe"):
        exe_module = m
        break

if exe_module:
    print(f"\nMain module: {exe_module.name}")
    print(f"  base=0x{exe_module.baseaddress:016X}  size=0x{exe_module.size:X}")

print(f"\nTotal threads: {len(md.threads.threads)}")
print(f"{'TID':<8} {'RIP':<18} {'RVA':<14} {'StackSP':<18} {'Notes'}")
print("-" * 100)
for t in md.threads.threads:
    ctx = t.ContextObject if hasattr(t, 'ContextObject') else None
    if ctx is None or not hasattr(ctx, 'Rip'):
        # Try the alternate field
        rip = getattr(t, 'rip', 0)
    else:
        rip = ctx.Rip
    sp = getattr(ctx, 'Rsp', 0) if ctx else 0
    rva = ""
    note = ""
    if exe_module and exe_module.baseaddress <= rip < exe_module.baseaddress + exe_module.size:
        rva_v = rip - exe_module.baseaddress
        rva = f"+0x{rva_v:X}"
        note = "in_exe"
    else:
        # Try other modules
        for m in md.modules.modules:
            if m.baseaddress <= rip < m.baseaddress + m.size:
                modname = m.name.split("\\")[-1].split("/")[-1]
                rva = f"{modname}+0x{rip - m.baseaddress:X}"
                if "ntdll" in modname.lower() or "kernelbase" in modname.lower():
                    note = "kernel_wait"
                else:
                    note = ""
                break
    print(f"{t.ThreadId:<8} 0x{rip:016X} {rva:<14} 0x{sp:016X} {note}")
