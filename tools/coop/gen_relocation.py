#!/usr/bin/env python3
"""Find the lui sites that address the moved per-player ranges and emit coop_relocation.toml."""
import argparse
import glob
import os
import re
import sys
from dataclasses import dataclass

DELTA_HI = 0x00A0
# (name, lo, hi, lui delta). R4 has its own delta so its index-1 entry clears camera node[1]; patched immediates stay distinct per delta.
MOVED = [
    ("R1", 0x80138058, 0x80138268, 0x00A0),
    ("R2", 0x80138268, 0x80138838, 0x00A0),
    ("R4", 0x80138930, 0x80138D10, 0x00A2),
    ("R3", 0x80138E5C, 0x80139020, 0x00A0),
]
# Pointer values here may be "end of the previous array" or "start of the next object".
EDGES = {0x80138058, 0x80138838, 0x80138930, 0x80138D10, 0x80138E5C, 0x80139020}
TRACKED_HI = {0x8013, 0x8014}
PATCHED_HI = {hi + d: hi for hi in TRACKED_HI for d in {m[3] for m in MOVED}}

REGS = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
        "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]
CALLER_SAVED = frozenset([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 31])
ARG_REGS = (4, 5, 6, 7)
LOADS = {"lb", "lbu", "lh", "lhu", "lw", "lwu", "lwl", "lwr", "ld", "ldl", "ldr", "ll", "lld", "lwc1", "ldc1"}
STORES = {"sb", "sh", "sw", "swl", "swr", "sd", "sdl", "sdr", "sc", "scd", "swc1", "sdc1"}
BRANCH2 = {"beq", "bne", "beql", "bnel"}
BRANCH1 = {"blez", "bgtz", "bltz", "bgez", "blezl", "bgtzl", "bltzl", "bgezl"}
FPBRANCH = {"bc1t", "bc1f", "bc1tl", "bc1fl"}
LIKELY = {"beql", "bnel", "blezl", "bgtzl", "bltzl", "bgezl", "bc1tl", "bc1fl"}
LINK_BRANCH = {"bal", "bgezal", "bltzal", "bgezall", "bltzall"}
CONDITIONAL = BRANCH2 | BRANCH1 | FPBRANCH
DELAYED = CONDITIONAL | LINK_BRANCH | {"b", "j", "jr", "jal", "jalr"}
NO_WRITE = STORES | DELAYED | {"mult", "multu", "div", "divu", "dmult", "dmultu", "ddiv", "ddivu", "mthi", "mtlo",
                               "mtc1", "dmtc1", "ctc1", "nop", "cache", "sync", "break", "syscall",
                               "teq", "tne", "tge", "tlt", "tgeu", "tltu"}
MAX_OFFS = 4
# Value flags: IDX = a register index was added; LOOP = the value came around a loop back edge.
IDX = 1
LOOP = 2
MAX_VISITS = 2000

FUNC_RE = re.compile(r"^RECOMP_FUNC\s+\w+\s+(\w+)\(")
INSN_RE = re.compile(r"^\s*// 0x([0-9A-Fa-f]{8}): (\S+)\s*(.*)$")
LABEL_RE = re.compile(r"^\s*L_([0-9A-Fa-f]{8}):")
MEM_RE = re.compile(r"^(-?0x[0-9A-Fa-f]+|-?\d+)\((\$\w+)\)$")


@dataclass
class Insn:
    vram: int
    op: str
    args: list


@dataclass
class Func:
    name: str
    insns: dict
    labels: set


@dataclass(frozen=True)
class Site:
    func: str
    vram: int
    rt: int
    hi: int
    patched: bool


def parse_funcs(text):
    funcs = []
    cur = None
    for line in text.splitlines():
        m = FUNC_RE.match(line)
        if m:
            cur = Func(m.group(1), {}, set())
            funcs.append(cur)
            continue
        if cur is None:
            continue
        m = INSN_RE.match(line)
        if m:
            vram = int(m.group(1), 16)
            if vram not in cur.insns:
                rest = m.group(3).strip()
                args = [a.strip() for a in rest.split(",")] if rest else []
                cur.insns[vram] = Insn(vram, m.group(2), args)
            continue
        m = LABEL_RE.match(line)
        if m:
            cur.labels.add(int(m.group(1), 16))
    return funcs


def _reg(s):
    s = s.strip()
    if not s.startswith("$"):
        return None
    name = s[1:]
    if name in REGS:
        return REGS.index(name)
    if name == "s8":
        return 30
    return None


def _mem(s):
    m = MEM_RE.match(s.strip())
    if not m:
        return 0, None
    return int(m.group(1), 0), _reg(m.group(2))


def _target(s):
    s = s.strip()
    if s.startswith("L_"):
        return int(s[2:], 16)
    return int(s, 16)


def _use(uses, kind, vals, extra=0):
    for site, off, idx in vals:
        addr = None if off is None else ((site.hi << 16) + off + extra) & 0xFFFFFFFF
        uses.setdefault(site, set()).add((kind, addr, idx))


def _widen(vals):
    by_site = {}
    for v in vals:
        by_site.setdefault(v[0], set()).add(v)
    out = set()
    for site, vs in by_site.items():
        if len({o for _, o, _ in vs}) > MAX_OFFS:
            out.add((site, None, IDX | (LOOP if any(f & LOOP for _, _, f in vs) else 0)))
        else:
            out |= vs
    return frozenset(out)


def _join(a, b):
    out = {}
    for r in set(a) | set(b):
        v = _widen(a.get(r, frozenset()) | b.get(r, frozenset()))
        if v:
            out[r] = v
    return out


def _transfer(fn, insn, st, uses):
    op, a = insn.op, insn.args
    st = dict(st)

    def get(r):
        return st.get(r, frozenset()) if r else frozenset()

    def put(r, vals):
        if not r:
            return
        if vals:
            st[r] = _widen(vals)
        else:
            st.pop(r, None)

    if op == "lui":
        rt, hi = _reg(a[0]), int(a[1], 0) & 0xFFFF
        orig = PATCHED_HI.get(hi, hi)
        if orig in TRACKED_HI and rt:
            site = Site(fn.name, insn.vram, rt, orig, hi != orig)
            uses.setdefault(site, set())
            put(rt, {(site, 0, 0)})
        else:
            put(rt, None)
    elif op in ("addiu", "addi", "daddiu", "daddi"):
        k = int(a[2], 0)
        put(_reg(a[0]), {(s, None if o is None else o + k, i) for s, o, i in get(_reg(a[1]))})
    elif op == "ori":
        k = int(a[2], 0) & 0xFFFF
        src_vals = get(_reg(a[1]))
        _use(uses, "escape", {v for v in src_vals if v[1] != 0 or v[2] & IDX})
        put(_reg(a[0]), {(s, k, i) for s, o, i in src_vals if o == 0 and not i & IDX})
    elif op in ("addu", "add", "daddu", "dadd", "or"):
        rs, rt = _reg(a[1]), _reg(a[2])
        va, vb = get(rs), get(rt)
        if va and vb:
            _use(uses, "escape", va | vb)
            put(_reg(a[0]), None)
        elif va or vb:
            other = rt if va else rs
            vals = va or vb
            put(_reg(a[0]), vals if not other else {(s, o, i | IDX) for s, o, i in vals})
        else:
            put(_reg(a[0]), None)
    elif op in ("subu", "sub", "dsubu", "dsub"):
        rs, rt = _reg(a[1]), _reg(a[2])
        va, vb = get(rs), get(rt)
        if va and not vb:
            put(_reg(a[0]), {(s, o, i | IDX) for s, o, i in va} if rt else va)
        else:
            _use(uses, "escape", va | vb)
            put(_reg(a[0]), None)
    elif op in LOADS or op in STORES:
        off, base = _mem(a[1])
        _use(uses, "mem", get(base), off)
        if op in STORES:
            _use(uses, "escape", get(_reg(a[0])))
        else:
            put(_reg(a[0]), None)
    else:
        if op in NO_WRITE:
            srcs = [get(_reg(x)) for x in a]
        else:
            srcs = [get(_reg(x)) for x in a[1:]]
        _use(uses, "escape", frozenset().union(*srcs) if srcs else frozenset())
        if op not in NO_WRITE and a:
            put(_reg(a[0]), None)
    return st


def _call(st, uses):
    for r in ARG_REGS:
        _use(uses, "escape", st.get(r, frozenset()))
    return {r: v for r, v in st.items() if r not in CALLER_SAVED}


def _step(fn, vram, st, uses):
    insn = fn.insns[vram]
    op = insn.op
    delay = fn.insns.get(vram + 4)

    def with_delay(s):
        return _transfer(fn, delay, s, uses) if delay else s

    if op in CONDITIONAL or op == "b":
        s = _transfer(fn, insn, st, uses)
        taken = with_delay(s)
        out = [(_target(insn.args[-1]), taken)]
        if op != "b":
            out.append((vram + 8, s if op in LIKELY else taken))
        return out
    if op == "j":
        return [(_target(insn.args[0]), with_delay(st))]
    if op in LINK_BRANCH or op in ("jal", "jalr"):
        s = _call(with_delay(_transfer(fn, insn, st, uses)), uses)
        return [(vram + 8, s)]
    if op == "jr":
        s = with_delay(st)
        r = _reg(insn.args[0])
        if r == 31:
            _use(uses, "escape", s.get(2, frozenset()) | s.get(3, frozenset()))
            return []
        _use(uses, "escape", s.get(r, frozenset()))
        return [(l, s) for l in sorted(fn.labels)]
    return [(vram + 4, _transfer(fn, insn, st, uses))]


def analyze_func(fn):
    uses = {}
    if not fn.insns:
        return uses
    delay_slots = {v + 4 for v, i in fn.insns.items() if i.op in DELAYED}
    entry = min(fn.insns)
    state_in = {entry: {}}
    visits = {}
    work = [entry]
    while work:
        v = work.pop()
        if v not in fn.insns or (v in delay_slots and v not in fn.labels):
            continue
        visits[v] = visits.get(v, 0) + 1
        if visits[v] > MAX_VISITS:
            raise RuntimeError("dataflow did not converge in %s at 0x%08X" % (fn.name, v))
        for nv, ns in _step(fn, v, state_in[v], uses):
            if nv not in fn.insns:
                continue
            if nv <= v:
                ns = {r: frozenset((s, o, f | LOOP) for s, o, f in vals) for r, vals in ns.items()}
            old = state_in.get(nv)
            merged = ns if old is None else _join(old, ns)
            if old is None or merged != old:
                state_in[nv] = merged
                work.append(nv)
    return uses


EXISTING_RE = re.compile(r'\[\[patches\.instruction\]\]\s*func\s*=\s*"([^"]+)"\s*vram\s*=\s*(0x[0-9A-Fa-f]+)')
HEADER = "# Generated by tools/coop/gen_relocation.py. Moves the per-player neighbour ranges R1-R3 by +0x00A00000 and R4 by +0x00A20000; do not edit by hand."


@dataclass
class Result:
    patches: list
    errors: list
    counts: dict
    unclassified: list
    notes: list


def region_of(addr):
    for name, lo, hi, _ in MOVED:
        if lo <= addr < hi:
            return name
    return None


def delta_of(addr):
    for _, lo, hi, delta in MOVED:
        if lo <= addr < hi:
            return delta
    return None


def site_delta(uses):
    """The one lui delta every moved use agrees on (range-edge pointers count for the range they start), or None."""
    deltas = {delta_of(a) for _, a, _ in _known(uses)} - {None}
    return deltas.pop() if len(deltas) == 1 else None


RAW = {hi << 16 for hi in TRACKED_HI}


def _known(uses):
    live = [(k, a, f) for k, a, f in uses if a is not None and not (k == "escape" and a in RAW and not f & IDX)]
    first = [u for u in live if not u[2] & LOOP]
    return first or live


def _is_edge(use):
    k, a, f = use
    return k == "escape" and a in EDGES and not f & IDX


def classify(uses):
    if not uses:
        return "unused"
    known = _known(uses)
    if not known:
        return "unclassified"
    rest = [u for u in known if not _is_edge(u)]
    if not rest:
        return "ambiguous"
    moved = {region_of(a) is not None for _, a, _ in rest}
    if moved == {True}:
        return "move" if len({delta_of(a) for _, a, _ in rest}) == 1 else "mixed"
    if moved == {False}:
        return "keep"
    return "mixed"


def edge_dropped(uses):
    known = _known(uses)
    return any(_is_edge(u) for u in known) and not all(_is_edge(u) for u in known)


def lui_value(site, delta=DELTA_HI):
    return 0x3C000000 | (site.rt << 16) | (site.hi + delta)


def parse_resolve(text):
    out = {}
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        names = {m[0] for m in MOVED}
        ok = len(parts) == 3 and parts[2] in ("move", "keep")
        ok = ok or (len(parts) == 4 and parts[2] == "move" and parts[3] in names)
        if not ok:
            raise ValueError("resolve line %d: expected '<func>|edge 0x<addr> move [R1..R4]|keep': %r" % (n, raw))
        out[(parts[0], int(parts[1], 16))] = "move:" + parts[3] if len(parts) == 4 else parts[2]
    return out


def parse_existing(toml_text):
    return {(f, int(v, 16)) for f, v in EXISTING_RE.findall(toml_text)}


def _desc(site):
    return "%s:0x%08X (lui $%s, 0x%04X)" % (site.func, site.vram, REGS[site.rt], site.hi)


def _fmt(uses):
    return ", ".join("%s@%s%s%s" % (k, "?" if a is None else "%08X" % a, "+idx" if i & IDX else "", "+loop" if i & LOOP else "")
                     for k, a, i in sorted(uses, key=lambda u: (u[0], u[1] or 0, u[2])))


def build_patches(all_uses, resolve, existing):
    res = Result([], [], {}, [], [])
    seen = set()
    for site in sorted(all_uses, key=lambda s: (s.vram, s.func)):
        uses = all_uses[site]
        cls = classify(uses)
        forced_delta = None
        key = (site.func, site.vram)
        if key in resolve:
            seen.add(key)
            if cls not in ("ambiguous", "unclassified"):
                res.errors.append("%s: resolve entry for a site classified %s" % (_desc(site), cls))
                continue
            cls = resolve[key]
            if cls.startswith("move:"):
                forced_delta = next(m[3] for m in MOVED if m[0] == cls[5:])
                cls = "move"
        elif cls == "ambiguous":
            edges = {a for k, a, f in _known(uses) if _is_edge((k, a, f))}
            ekey = ("edge", edges.pop()) if len(edges) == 1 else None
            if ekey in resolve:
                seen.add(ekey)
                cls = resolve[ekey]
        res.counts[cls] = res.counts.get(cls, 0) + 1
        if cls in ("move", "keep") and key not in resolve and edge_dropped(uses):
            res.notes.append("%s: %s; range-edge pointer ignored: %s" % (_desc(site), cls, _fmt(uses)))
        if cls == "mixed":
            res.errors.append("%s: mixed uses %s" % (_desc(site), _fmt(uses)))
        elif cls == "ambiguous":
            res.errors.append("%s: pointer at a range edge, add a resolve entry: %s" % (_desc(site), _fmt(uses)))
        elif cls == "unclassified":
            res.unclassified.append(site)
            res.errors.append("%s: unclassified (all uses widened), add a resolve entry: %s" % (_desc(site), _fmt(uses)))
        elif cls == "move":
            delta = forced_delta if forced_delta is not None else site_delta(uses)
            if key in existing:
                res.errors.append("%s: already patched in rogue_squadron.toml" % _desc(site))
            elif delta is None:
                res.errors.append("%s: move, but its uses name no single moved range: %s" % (_desc(site), _fmt(uses)))
            else:
                res.patches.append((site.func, site.vram, lui_value(site, delta)))
    for key in sorted(set(resolve) - seen):
        res.errors.append("%s 0x%08X: resolve entry matches no lui site" % key)
    return res


def emit_toml(patches):
    out = [HEADER, ""]
    for func, vram, value in patches:
        out += ["[[patches.instruction]]", 'func = "%s"' % func, "vram = 0x%08X" % vram, "value = 0x%08X" % value, ""]
    return "\n".join(out)


PATCHED_LUI_RE = re.compile(r"// 0x([0-9A-Fa-f]{8}): lui +\$\w+, 0x80B[3-6]\s*$", re.M)


def verify_applied(func_texts, toml_text):
    """Return (patched lui sites found in the recompiled output, patches listed in toml_text)."""
    found = set()
    for text in func_texts:
        for fn in re.split(r"(?m)^RECOMP_FUNC\s+\w+\s+", text)[1:]:
            name = fn.split("(", 1)[0]
            found |= {(name, int(v, 16)) for v in PATCHED_LUI_RE.findall(fn)}
    return len(found), len(parse_existing(toml_text))


def _read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--funcs", default="RecompiledFuncs")
    ap.add_argument("--resolve", default=os.path.join("tools", "coop", "relocation_resolve.txt"))
    ap.add_argument("--existing", default="rogue_squadron.toml")
    ap.add_argument("--out", default=os.path.join("tools", "coop", "coop_relocation.toml"))
    ap.add_argument("--verify", action="store_true", help="check that every patch in --out is applied in --funcs")
    args = ap.parse_args(argv)

    paths = sorted(glob.glob(os.path.join(args.funcs, "funcs_*.c")))
    if not paths:
        print("no funcs_*.c under %s" % args.funcs, file=sys.stderr)
        return 1
    if args.verify:
        found, listed = verify_applied([_read(p) for p in paths], _read(args.out))
        print("relocation: %d of %d lui patches applied" % (found, listed))
        return 0 if found == listed else 1
    all_uses = {}
    for path in paths:
        for fn in parse_funcs(_read(path)):
            all_uses.update(analyze_func(fn))
    resolve = parse_resolve(_read(args.resolve)) if os.path.exists(args.resolve) else {}
    res = build_patches(all_uses, resolve, parse_existing(_read(args.existing)))

    print("lui sites: %d" % len(all_uses))
    for cls in sorted(res.counts):
        print("  %-12s %d" % (cls, res.counts[cls]))
    patched = {(p[0], p[1]) for p in res.patches}
    per_range = {}
    for site, uses in all_uses.items():
        if (site.func, site.vram) not in patched:
            continue
        for _, a, _ in uses:
            name = region_of(a) if a is not None else None
            if name:
                per_range[name] = per_range.get(name, 0) + 1
    print("moved uses per range: %s" % ", ".join("%s=%d" % kv for kv in sorted(per_range.items())))
    for n in res.notes:
        print("note " + n)
    for site in res.unclassified:
        print("unclassified %s: %s" % (_desc(site), _fmt(all_uses[site])))
    for e in res.errors:
        print("ERROR " + e)
    if res.errors:
        print("%d error(s); %s not written" % (len(res.errors), args.out))
        return 1
    tmp = args.out + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(emit_toml(res.patches))
    os.replace(tmp, args.out)
    print("wrote %d patches to %s" % (len(res.patches), args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
