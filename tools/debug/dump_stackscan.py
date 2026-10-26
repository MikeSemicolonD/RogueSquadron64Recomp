"""Scan every thread's stack in a minidump for return addresses inside the main exe,
then symbolize them via llvm-symbolizer --relative-address. Names which code each
blocked thread came from when RIP sits in ntdll kernel waits (inspect-dump.py shows
only RIP). Usage: python tools/debug/dump_stackscan.py [dump.dmp]"""
import sys, struct, itertools, subprocess
from pathlib import Path
from minidump.minidumpfile import MinidumpFile

path = sys.argv[1] if len(sys.argv) > 1 else str(sorted(Path("dumps/crash-dumps").glob("crash_*.dmp"), key=lambda p: p.stat().st_mtime)[-1])
md = MinidumpFile.parse(path)
exe_base = exe_size = None
for m in md.modules.modules:
    n = m.name.lower()
    if "roguesquadron" in n and n.endswith(".exe"):
        exe_base, exe_size = m.baseaddress, m.size
buff = md.get_reader().get_buffered_reader()
results = []
for t in md.threads.threads:
    sp = t.ContextObject.Rsp if t.ContextObject else None
    if not sp: continue
    try:
        buff.move(sp); data = buff.read(0x800)
    except Exception:
        continue
    rvas = []
    for off in range(0, len(data) - 8, 8):
        q = struct.unpack_from("<Q", data, off)[0]
        if exe_base <= q < exe_base + exe_size:
            rvas.append(q - exe_base)
        if len(rvas) >= 6: break
    if rvas: results.append((t.ThreadId, rvas))
allr = sorted(set(itertools.chain.from_iterable(r for _, r in results)))
out = subprocess.run(["llvm-symbolizer", "--relative-address", "-e", "build/Debug/RogueSquadron64Recomp.exe"] + [hex(r) for r in allr], capture_output=True, text=True).stdout
syms = dict(zip(allr, (b.split("\n")[0] for b in out.strip().split("\n\n"))))
for tid, rvas in results:
    print(f"TID {tid}: " + " <- ".join(syms.get(r, hex(r)) for r in rvas[:5]))
