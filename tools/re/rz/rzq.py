#!/usr/bin/env python3
"""Query the game ELF with rizin: xrefs, callers/callees, disassembly, string refs.

Uses the symbolized ELF from the sister decomp repo (build/roguesquadron.elf), so
named functions show up as sym.<name>.  The three overlays share VRAM 0x800A5130;
pick which one is mapped on top with --overlay (default: cinematic, the ELF order).

Examples:
  python tools/re/rz/rzq.py xrefs 0x800B0B28            # who reads/writes gateCtr
  python tools/re/rz/rzq.py xrefs setupCameraMatrices   # callers of a function
  python tools/re/rz/rzq.py callees cinematicLoopBody   # what a function calls/touches
  python tools/re/rz/rzq.py disasm processSceneNode
  python tools/re/rz/rzq.py strrefs "Nintendo"          # strings matching + their xrefs
  python tools/re/rz/rzq.py funcs "^menu"               # list analysed funcs by regex
  python tools/re/rz/rzq.py raw "pd 20 @ 0x80015548"    # any rizin command
"""
import argparse, json, os, re, shutil, sys

DEFAULT_ELF = os.path.normpath(os.path.join(os.path.dirname(__file__),
    "..", "..", "..", "..", "rogue_squadron64", "build", "roguesquadron.elf"))
RIZIN_BIN = r"C:\Program Files\Rizin\bin"
OVERLAY_MAP = {"mission": 4, "menu": 5, "cinematic": 6}


def open_rz(elf, overlay, project):
    if not shutil.which("rizin") and os.path.isdir(RIZIN_BIN):
        os.environ["PATH"] = RIZIN_BIN + os.pathsep + os.environ.get("PATH", "")
    import rzpipe
    rz = rzpipe.open(elf, flags=["-2", "-e", "scr.color=0", "-e", "scr.interactive=false"])
    if overlay != "cinematic":
        rz.cmd(f"omp {OVERLAY_MAP[overlay]}")
    if project and os.path.exists(project):
        rz.cmd(f"Po {project}")
    else:
        rz.cmd("aaa")
        if project:
            rz.cmd(f"Ps {project}")
    return rz


def resolve(rz, target):
    """Accept 0xADDR or a symbol name; return numeric address or None."""
    if re.fullmatch(r"0x[0-9a-fA-F]+", target):
        return int(target, 16)
    flags = {f["name"]: f["offset"] for f in rz.cmdj("flj") or []}
    for cand in (f"sym.{target}", target, f"fcn.{target}"):
        if cand in flags:
            return flags[cand]
    return None


def fn_name_at(rz, addr):
    j = rz.cmdj(f"afij @ {addr:#x}")
    return j[0]["name"] if j else "?"


def cmd_xrefs(rz, a):
    addr = resolve(rz, a.target)
    if addr is None:
        sys.exit(f"unknown symbol {a.target}")
    refs = rz.cmdj(f"axtj @ {addr:#x}") or []
    print(f"xrefs to {a.target} ({addr:#x}): {len(refs)}")
    for r in refs:
        op = rz.cmd(f"pi 1 @ {r['from']:#x}").strip()
        print(f"  {fn_name_at(rz, r['from']):40s} {r['from']:#x} [{r['type']}] {op}")


def cmd_callees(rz, a):
    """Calls plus loads/stores that land exactly on a named symbol; internal branches skipped."""
    addr = resolve(rz, a.target)
    if addr is None:
        sys.exit(f"unknown symbol {a.target}")
    refs = rz.cmdj(f"afxj @ {addr:#x}") or []
    seen = set()
    rows = []
    for r in refs:
        key = (r["type"], r["to"])
        if key in seen or r["type"] == "CODE":
            continue
        seen.add(key)
        name = rz.cmd(f"fd @ {r['to']:#x}").strip()
        if r["type"] == "CALL" or (name.startswith("sym.") and " + " not in name):
            rows.append((r["type"], r["to"], name))
    print(f"refs from {a.target} ({addr:#x}): {len(rows)} (of {len(refs)} raw)")
    for t, to, name in sorted(rows):
        print(f"  [{t:4s}] {to:#x} {name}")


def cmd_disasm(rz, a):
    addr = resolve(rz, a.target)
    if addr is None:
        sys.exit(f"unknown symbol {a.target}")
    print(rz.cmd(f"pdf @ {addr:#x}"))


def cmd_strrefs(rz, a):
    pat = re.compile(a.pattern)
    hits = [s for s in (rz.cmdj("izzj") or []) if pat.search(s.get("string", ""))]
    for s in hits:
        print(f"{s['vaddr']:#x} {s['string'][:80]!r}")
        for r in rz.cmdj(f"axtj @ {s['vaddr']:#x}") or []:
            op = rz.cmd(f"pi 1 @ {r['from']:#x}").strip()
            print(f"    {fn_name_at(rz, r['from']):40s} {r['from']:#x} {op}")


def cmd_funcs(rz, a):
    pat = re.compile(a.pattern) if a.pattern else None
    for f in sorted(rz.cmdj("aflj") or [], key=lambda f: f["offset"]):
        if pat and not pat.search(f["name"]):
            continue
        print(f"{f['offset']:#x} {f['size']:6d} {f['name']}")


def cmd_raw(rz, a):
    print(rz.cmd(a.command))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--elf", default=DEFAULT_ELF)
    p.add_argument("--overlay", choices=OVERLAY_MAP, default="cinematic",
                   help="which overlay is mapped on top of 0x800A5130")
    p.add_argument("--project", help="rizin project file to cache analysis (saved on first run)")
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("xrefs"); s.add_argument("target"); s.set_defaults(fn=cmd_xrefs)
    s = sub.add_parser("callees"); s.add_argument("target"); s.set_defaults(fn=cmd_callees)
    s = sub.add_parser("disasm"); s.add_argument("target"); s.set_defaults(fn=cmd_disasm)
    s = sub.add_parser("strrefs"); s.add_argument("pattern"); s.set_defaults(fn=cmd_strrefs)
    s = sub.add_parser("funcs"); s.add_argument("pattern", nargs="?"); s.set_defaults(fn=cmd_funcs)
    s = sub.add_parser("raw"); s.add_argument("command"); s.set_defaults(fn=cmd_raw)
    a = p.parse_args()
    if not os.path.exists(a.elf):
        sys.exit(f"ELF not found: {a.elf} (run tools/make_elf.py in rogue_squadron64)")
    rz = open_rz(a.elf, a.overlay, a.project)
    try:
        a.fn(rz, a)
    finally:
        rz.quit()


if __name__ == "__main__":
    main()
