#!/usr/bin/env python3
"""Widen the front-end menu (gCurrentMenuData, MenuD) from 8 to 12 rows and write menu_rows.toml ([[patches.instruction]] entries).

MenuD keeps its single fields and the p8 / sc / fB0 arrays where they are, so those three grow into the space after them; type, pos, w74 and fD0 move past +0xF8
(NEW_BASE). The struct then overruns D_800CE830, the snapshot copy, which moves to host RAM (SNAPSHOT). Text group 5 holds one element per row, and its
fixed elements 8-12 (title and hint lines) move to 12-16, so the group is created with 17 elements.

Reads RecompiledFuncs (the instruction comments) for dataflow and the ROM for the original words. Prints every site it could not settle; settle those in
RESOLVE below after reading the asm.
"""
import argparse
import glob
import os
import re
import struct
import sys

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

ROWS = 12
MENUD = 0x800CE730
OLD_SIZE = 0xF8
SNAPSHOT_OLD = 0x800CE830
# RS64_HR_MENU_SNAPSHOT in src/util/host_ram_map.h, and the lui/addiu pairs that address the snapshot.
SNAPSHOT_NEW = 0x80C42800
SNAPSHOT_SITES = [(0x800BBF8C, 0x800BBF90), (0x800BC118, 0x800BC140)]
# Fixed 8-row loops: setupMenuData's pos/sc reset and updateTieInterceptorAnimFlag's label refresh. The --report BOUND-8 list shows every compare with 8.
ROW_LOOP_BOUNDS = [0x800BA168, 0x800BC434]
# Old array base, element size, new base. Arrays not listed keep their offsets.
MOVED = {
    "type": (0x28, 1, 0x188),
    "pos": (0x30, 4, 0xF8),
    "w74": (0x74, 4, 0x128),
    "fD0": (0xD0, 4, 0x158),
}
NEW_SIZE = 0x194
FIELDS = [(0x08, 0x28, "p8"), (0x28, 0x30, "type"), (0x30, 0x50, "pos"), (0x50, 0x54, "titlepos"), (0x54, 0x74, "sc"),
          (0x74, 0x94, "w74"), (0xB0, 0xD0, "fB0"), (0xD0, 0xF0, "fD0")]

TEXT_GROUP = 5
OLD_ELEMS = 13
FIXED_ELEM_SHIFT = 4
# Text functions taking (group, element) and the argument register that holds the element.
ELEM_FUNCS = {0x80061C74: 5, 0x80061F34: 5, 0x80063C3C: 5, 0x80063CFC: 6, 0x80063E54: 5}
GROUP_CREATE = 0x800613C0
SETUP_MENU = 0x800BA0F0
SNAPSHOT_FN = 0x800BBF80
ZMEMCPY = 0x80018EF4

REGS = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
        "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]
CALLER_SAVED = frozenset([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 31])
LOADS = {"lb", "lbu", "lh", "lhu", "lw", "lwu", "lwl", "lwr", "ld", "lwc1", "ldc1"}
STORES = {"sb", "sh", "sw", "swl", "swr", "sd", "swc1", "sdc1"}
BRANCH2 = {"beq", "bne", "beql", "bnel"}
BRANCH1 = {"blez", "bgtz", "bltz", "bgez", "blezl", "bgtzl", "bltzl", "bgezl"}
FPBRANCH = {"bc1t", "bc1f", "bc1tl", "bc1fl"}
LIKELY = {"beql", "bnel", "blezl", "bgtzl", "bltzl", "bgezl", "bc1tl", "bc1fl"}
LINK_BRANCH = {"bal", "bgezal", "bltzal", "bgezall", "bltzall"}
CONDITIONAL = BRANCH2 | BRANCH1 | FPBRANCH
DELAYED = CONDITIONAL | LINK_BRANCH | {"b", "j", "jr", "jal", "jalr"}
NO_WRITE = STORES | DELAYED | {"mult", "multu", "div", "divu", "dmult", "dmultu", "ddiv", "ddivu", "mthi", "mtlo",
                               "mtc1", "dmtc1", "ctc1", "nop", "cache", "sync", "break", "syscall"}
MAX_OFFS = 4
MAX_VISITS = 4000

FUNC_RE = re.compile(r"^RECOMP_FUNC\s+\w+\s+(\w+)\(")
INSN_RE = re.compile(r"^\s*// 0x([0-9A-Fa-f]{8}): (\S+)\s*(.*)$")
LABEL_RE = re.compile(r"^\s*L_([0-9A-Fa-f]{8}):")
SWITCH_RE = re.compile(r"^\s*switch \(jr_addend_([0-9A-Fa-f]{8})")
CASE_RE = re.compile(r"^\s*case \d+: goto L_([0-9A-Fa-f]{8});")
MEM_RE = re.compile(r"^(-?0x[0-9A-Fa-f]+|-?\d+)\((\$\w+)\)$")
SYM_RE = re.compile(r'name = "(\w+)", vram = 0x([0-9A-Fa-f]+), size = 0x([0-9A-Fa-f]+)')

# Sites settled by hand: (vram) -> "skip" (leave the instruction alone) or an int (the new 16-bit immediate).
RESOLVE = {}


def reg(s):
    s = s.strip()

    if not s.startswith("$"):
        return None

    name = s[1:]

    if name == "s8":
        return 30

    return REGS.index(name) if name in REGS else None


def mem(s):
    m = MEM_RE.match(s.strip())

    if not m:
        return 0, None

    return int(m.group(1), 0), reg(m.group(2))


def field_of(off):
    for lo, hi, name in FIELDS:
        if lo <= off < hi:
            return name

    return None


def load_sections():
    secs = {}
    cur = None
    text = open(os.path.join(REPO, "syms", "rogue_squadron.syms.toml"), encoding="utf-8").read()

    for line in text.splitlines():
        m = re.match(r'name = "(\.[\w.]+)"', line.strip())

        if m:
            cur = {"name": m.group(1), "funcs": {}}
            secs[cur["name"]] = cur
            continue

        m = re.match(r"(rom|vram|size) = 0x([0-9A-Fa-f]+)$", line.strip())

        if m and cur is not None and m.group(1) not in cur:
            cur[m.group(1)] = int(m.group(2), 16)
            continue

        m = SYM_RE.search(line)

        if m and cur is not None:
            cur["funcs"][m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))

    return secs


def parse_funcs(paths, wanted):
    funcs = {}

    for path in paths:
        cur = None

        for line in open(path, encoding="utf-8"):
            m = FUNC_RE.match(line)

            if m:
                cur = None

                if m.group(1) in wanted:
                    cur = {"name": m.group(1), "insns": {}, "labels": set()}
                    funcs[cur["name"]] = cur

                continue

            if cur is None:
                continue

            m = INSN_RE.match(line)

            if m:
                vram = int(m.group(1), 16)

                if vram not in cur["insns"]:
                    rest = m.group(3).strip()
                    cur["insns"][vram] = (m.group(2), [a.strip() for a in rest.split(",")] if rest else [])

                continue

            m = LABEL_RE.match(line)

            if m:
                cur["labels"].add(int(m.group(1), 16))
                continue

            m = SWITCH_RE.match(line)

            if m:
                cur["switch"] = int(m.group(1), 16)
                cur.setdefault("tables", {})[cur["switch"]] = set()
                continue

            m = CASE_RE.match(line)

            if m and cur.get("switch"):
                cur["tables"][cur["switch"]].add(int(m.group(1), 16))

    return funcs


class Analysis:
    def __init__(self, menu_funcs, imm):
        self.menu_funcs = menu_funcs
        self.imm = imm
        self.by_vram = {v: n for n, (v, _) in menu_funcs.items()}
        self.field_uses = {}
        self.unknown_mem = set()
        self.elem_consts = {}
        self.elem_var = set()
        self.memcpy = {}
        self.escapes = set()
        self.seeds = {}
        self.group_create = {}
        self.wrappers = set()
        self.mixed = set()

    def note(self, store, key, val):
        store.setdefault(key, set()).add(val)

    def call(self, fn, vram, target, st):
        def get(r):
            return st.get(r, frozenset())

        args = {r: get(r) for r in (4, 5, 6, 7)}
        a0k = {v[1] for v in args[4] if v[0] == "K"}

        if target in ELEM_FUNCS and a0k == {TEXT_GROUP} and len(args[4]) == 1:
            er = ELEM_FUNCS[target]
            ks = [v for v in args[er] if v[0] == "K"]
            others = [v for v in args[er] if v[0] != "K"]

            for _, k, origin in ks:
                if 8 <= k < OLD_ELEMS:
                    self.note(self.elem_consts, origin, (fn, vram, k))

            if others or not args[er]:
                self.elem_var.add((fn, vram, target))

        if target == GROUP_CREATE:
            for _, k, origin in [v for v in args[5] if v[0] == "K"]:
                if k == TEXT_GROUP:
                    self.note(self.group_create, vram, tuple(sorted((v[1], v[2]) for v in args[6] if v[0] == "K")))

        if target == ZMEMCPY and any(v[0] in "MS" for r in (4, 5) for v in args[r]):
            self.note(self.memcpy, vram, tuple(sorted((v[1], v[2]) for v in args[6] if v[0] == "K")))
        elif target in self.by_vram:
            callee = self.by_vram[target]
            seed = {r: frozenset(v for v in vals if v[0] in "MS") for r, vals in args.items()}
            seed = {r: v for r, v in seed.items() if v}

            if seed:
                cur = self.seeds.setdefault(callee, {})

                for r, v in seed.items():
                    cur[r] = cur.get(r, frozenset()) | v

            if a0k == {TEXT_GROUP} and target not in (SETUP_MENU,):
                self.wrappers.add((fn, vram, callee))
        elif target is None or target not in (ZMEMCPY,):
            for r, vals in args.items():
                if any(v[0] in "MS" for v in vals) and target not in ELEM_FUNCS:
                    self.escapes.add((fn, vram, "arg $%s to 0x%s" % (REGS[r], "%08X" % target if target else "jalr")))

        out = {r: v for r, v in st.items() if not (isinstance(r, int) and r in CALLER_SAVED)}

        if target == SETUP_MENU:
            out[2] = frozenset([("M", 0, 0)])
        elif target == SNAPSHOT_FN:
            out[2] = frozenset([("S", 0, 0)])

        return out

    def access(self, fn, vram, op, imm, vals):
        if any(v[0] in "MS" for v in vals) and any(v[0] == "X" for v in vals):
            self.mixed.add((fn, vram, op))

        for v in vals:
            if v[0] not in "MS":
                continue

            if v[1] is None:
                self.unknown_mem.add((fn, vram, op))
                continue

            off = v[1] + imm
            self.note(self.field_uses, (fn, vram), (off, op, bool(v[2])))

    def transfer(self, fn, vram, op, a, st):
        st = dict(st)

        def get(r):
            return st.get(r, frozenset()) if r else frozenset()

        def put(r, vals):
            if not r:
                return

            vals = widen(vals)
            st[r] = vals if vals else frozenset([("X", 0, 0)])

        if op == "lui":
            put(reg(a[0]), {("H", self.imm(vram) & 0xFFFF, 0)})
        elif op in ("addiu", "daddiu", "addi"):
            k = self.imm(vram)
            src = reg(a[1])

            if src == 0:
                put(reg(a[0]), {("K", k, vram)})
            else:
                out = set()

                for v in get(src):
                    if v[0] == "H":
                        addr = ((v[1] << 16) + k) & 0xFFFFFFFF

                        if MENUD <= addr < MENUD + OLD_SIZE:
                            out.add(("M", addr - MENUD, 0))
                        elif SNAPSHOT_OLD <= addr < SNAPSHOT_OLD + OLD_SIZE:
                            out.add(("S", addr - SNAPSHOT_OLD, 0))
                    elif v[0] in "MS":
                        out.add((v[0], None if v[1] is None else v[1] + k, v[2]))

                put(reg(a[0]), out)
        elif op == "ori" and reg(a[1]) == 0:
            put(reg(a[0]), {("K", self.imm(vram) & 0xFFFF, vram)})
        elif op in ("addu", "daddu", "or", "add"):
            rs, rt = reg(a[1]), reg(a[2])
            va, vb = get(rs), get(rt)
            pa = {v for v in va if v[0] in "MS"}
            pb = {v for v in vb if v[0] in "MS"}

            if rt == 0:
                put(reg(a[0]), va)
            elif rs == 0:
                put(reg(a[0]), vb)
            elif pa and not pb:
                put(reg(a[0]), {(v[0], v[1], 1) for v in pa})
            elif pb and not pa:
                put(reg(a[0]), {(v[0], v[1], 1) for v in pb})
            else:
                if pa and pb:
                    self.escapes.add((fn, vram, "two struct pointers added"))

                put(reg(a[0]), None)
        elif op in LOADS or op in STORES:
            base = mem(a[1])[1]
            imm = self.imm(vram)
            bvals = get(base)

            if base == 29:
                if op in STORES:
                    st[("sp", imm)] = get(reg(a[0]))
                else:
                    put(reg(a[0]), st.get(("sp", imm), frozenset()))

                return st

            for v in bvals:
                if v[0] == "H":
                    addr = ((v[1] << 16) + imm) & 0xFFFFFFFF

                    if MENUD <= addr < MENUD + OLD_SIZE:
                        self.note(self.field_uses, (fn, vram), (addr - MENUD, op, False))
                    elif SNAPSHOT_OLD <= addr < SNAPSHOT_OLD + OLD_SIZE:
                        self.escapes.add((fn, vram, "absolute snapshot access"))

            self.access(fn, vram, op, imm, bvals)

            if op in STORES:
                if any(v[0] in "MS" for v in get(reg(a[0]))):
                    self.escapes.add((fn, vram, "pointer stored to memory"))
            else:
                put(reg(a[0]), None)
        elif op in NO_WRITE:
            pass
        elif a:
            put(reg(a[0]), None)

        return st

    def step(self, fn, vram, st):
        insns = fn["insns"]
        op, a = insns[vram]
        delay = insns.get(vram + 4)

        def with_delay(s):
            return self.transfer(fn["name"], vram + 4, delay[0], delay[1], s) if delay else s

        def target(s):
            s = s.strip()
            return int(s[2:], 16) if s.startswith("L_") else int(s, 16)

        if op in CONDITIONAL or op == "b":
            s = self.transfer(fn["name"], vram, op, a, st)
            taken = with_delay(s)
            out = [(target(a[-1]), taken)]

            if op != "b":
                out.append((vram + 8, s if op in LIKELY else taken))

            return out

        if op == "j":
            return [(target(a[0]), with_delay(st))]

        if op == "jal":
            s = with_delay(st)
            return [(vram + 8, self.call(fn["name"], vram, int(a[0], 16), s))]

        if op == "jalr" or op in LINK_BRANCH:
            s = with_delay(st)
            return [(vram + 8, self.call(fn["name"], vram, None, s))]

        if op == "jr":
            s = with_delay(st)

            if reg(a[0]) != 31:
                targets = fn.get("tables", {}).get(vram, fn["labels"])
                return [(lbl, s) for lbl in sorted(targets)]

            return []

        return [(vram + 4, self.transfer(fn["name"], vram, op, a, st))]

    def run(self, fn, seed):
        insns = fn["insns"]

        if not insns:
            return

        delay_slots = {v + 4 for v, (op, _) in insns.items() if op in DELAYED}
        entry = min(insns)
        state_in = {entry: dict(seed)}
        visits = {}
        work = [entry]

        while work:
            v = work.pop()

            if v not in insns or (v in delay_slots and v not in fn["labels"]):
                continue

            visits[v] = visits.get(v, 0) + 1

            if visits[v] > MAX_VISITS:
                raise RuntimeError("dataflow did not converge in %s at 0x%08X" % (fn["name"], v))

            for nv, ns in self.step(fn, v, state_in[v]):
                if nv not in insns:
                    continue

                if nv <= v:
                    ns = {r: frozenset((t[0], t[1], 1) if t[0] in "MS" else t for t in vals) for r, vals in ns.items()}

                old = state_in.get(nv)
                merged = ns if old is None else join(old, ns)

                if old is None or merged != old:
                    state_in[nv] = merged
                    work.append(nv)


def widen(vals):
    if not vals:
        return frozenset()

    ptrs = {v for v in vals if v[0] in "MS"}

    if len({v[1] for v in ptrs}) > MAX_OFFS:
        known = [v[1] for v in ptrs if v[1] is not None]
        tags = {v[0] for v in ptrs}
        ptrs = {(t, min(known) if known and None not in {v[1] for v in ptrs} else None, 1) for t in tags}

    return frozenset(ptrs | {v for v in vals if v[0] not in "MS"})


def join(a, b):
    out = {}

    for r in set(a) | set(b):
        v = widen(a.get(r, frozenset()) | b.get(r, frozenset()))

        if v:
            out[r] = v

    return out


def rom_word(rom, sec, vram):
    off = sec["rom"] + vram - sec["vram"]
    return struct.unpack(">I", rom[off:off + 4])[0]


def with_imm(word, imm):
    return (word & 0xFFFF0000) | (imm & 0xFFFF)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--funcs", default=os.path.join(REPO, "RecompiledFuncs"))
    ap.add_argument("--rom", default=os.path.join(REPO, "build", "Debug", "rogue_squadron.z64"))
    ap.add_argument("--out", default=os.path.join(REPO, "menu_rows.toml"))
    ap.add_argument("--report", action="store_true", help="print the analysis, write nothing")
    args = ap.parse_args(argv)

    secs = load_sections()
    menu = secs[".ovl.menu"]
    funcs = parse_funcs(sorted(glob.glob(os.path.join(args.funcs, "funcs_*.c"))), set(menu["funcs"]))
    rom = open(args.rom, "rb").read()

    # Immediates come from the ROM, not the listing, which shows them after these patches once they are in a regen.
    def imm(vram):
        return struct.unpack(">h", struct.pack(">H", rom_word(rom, menu, vram) & 0xFFFF))[0]

    an = Analysis(menu["funcs"], imm)

    for fn in funcs.values():
        an.run(fn, {})

    done = {}

    while True:
        pending = [(n, s) for n, s in an.seeds.items() if done.get(n) != s and n in funcs]

        if not pending:
            break

        for name, seed in pending:
            done[name] = dict(seed)
            an.run(funcs[name], seed)

    patches = {}
    problems = []

    for (fn, vram), uses in sorted(an.field_uses.items(), key=lambda kv: kv[0][1]):
        deltas = set()

        for off, op, idx in uses:
            name = field_of(off)

            if name in MOVED:
                base, _, new = MOVED[name]
                deltas.add(new - base)
            else:
                deltas.add(0)

        if RESOLVE.get(vram) == "skip":
            continue

        if len(deltas) > 1:
            problems.append("%s 0x%08X: one access reaches fields with different moves %s" % (fn, vram, sorted(uses)))
            continue

        d = deltas.pop()

        if d:
            word = rom_word(rom, menu, vram)
            imm = struct.unpack(">h", struct.pack(">H", word & 0xFFFF))[0]
            patches[(fn, vram)] = (with_imm(word, imm + d), "%s %s" % (sorted({field_of(o) for o, _, _ in uses})[0], "+0x%X" % (imm + d)))

    for origin, uses in sorted(an.elem_consts.items()):
        if RESOLVE.get(origin) == "skip":
            continue

        ks = {k for _, _, k in uses}
        fn = sorted(uses)[0][0]

        if len(ks) != 1:
            problems.append("0x%08X: element constant with several values %s" % (origin, sorted(ks)))
            continue

        k = ks.pop()
        word = rom_word(rom, menu, origin)

        if (word & 0xFFFF) != k or (word >> 26) != 0x09:
            problems.append("%s 0x%08X: element %d origin is not addiu rX, $zero, %d" % (fn, origin, k, k))
            continue

        patches[(fn, origin)] = (with_imm(word, k + FIXED_ELEM_SHIFT), "text element %d -> %d" % (k, k + FIXED_ELEM_SHIFT))

    def func_at(vram):
        return next(n for n, (v, size) in menu["funcs"].items() if v <= vram < v + size)

    def set_imm(vram, expect, new, why):
        word = rom_word(rom, menu, vram)

        if (word & 0xFFFF) != (expect & 0xFFFF):
            problems.append("0x%08X: expected immediate 0x%X, found word %08X" % (vram, expect & 0xFFFF, word))
            return

        patches[(func_at(vram), vram)] = (with_imm(word, new), why)

    for calls in an.memcpy.values():
        for size, origin in {k for args in calls for k in args}:
            set_imm(origin, OLD_SIZE, NEW_SIZE, "copy size 0x%X" % NEW_SIZE)

    for calls in an.group_create.values():
        for count, origin in {k for args in calls for k in args}:
            set_imm(origin, OLD_ELEMS, OLD_ELEMS + FIXED_ELEM_SHIFT, "text group %d elements %d" % (TEXT_GROUP, OLD_ELEMS + FIXED_ELEM_SHIFT))

    for hi, lo in SNAPSHOT_SITES:
        set_imm(hi, (SNAPSHOT_OLD + 0x8000) >> 16,(SNAPSHOT_NEW + 0x8000) >> 16, "snapshot hi")
        set_imm(lo, SNAPSHOT_OLD, SNAPSHOT_NEW, "snapshot lo")

    for vram in ROW_LOOP_BOUNDS:
        set_imm(vram, 8, ROWS, "row loop bound %d" % ROWS)

    bounds = []

    for name in sorted({fn for fn, _ in an.field_uses}):
        for vram, (op, a) in sorted(funcs[name]["insns"].items()):
            if op in ("slti", "sltiu") and int(a[2], 0) == 8:
                bounds.append((name, vram))

    if args.report:
        print("field-access instructions:", len(an.field_uses), "patched:", sum(1 for p in patches.values() if not p[1].startswith("text")))
        print("element constants:", len(an.elem_consts))

        for line in problems:
            print("PROBLEM", line)

        for fn, vram, op in sorted(an.mixed, key=lambda x: x[1]):
            print("MIXED %s 0x%08X %s" % (fn, vram, op))

        for fn, vram, op in sorted(an.unknown_mem, key=lambda x: x[1]):
            print("UNKNOWN-OFFSET %s 0x%08X %s" % (fn, vram, op))

        for fn, vram, why in sorted(an.escapes, key=lambda x: x[1]):
            print("ESCAPE %s 0x%08X %s" % (fn, vram, why))

        for vram, sizes in sorted(an.memcpy.items()):
            print("MEMCPY 0x%08X sizes %s" % (vram, sorted(sizes)))

        for vram, counts in sorted(an.group_create.items()):
            print("GROUP5-CREATE 0x%08X counts %s" % (vram, sorted(counts)))

        for fn, vram, callee in sorted(an.wrappers, key=lambda x: x[1]):
            print("GROUP5-WRAPPER %s 0x%08X -> %s" % (fn, vram, callee))

        for fn, vram, t in sorted(an.elem_var, key=lambda x: x[1]):
            print("ELEM-VAR %s 0x%08X -> 0x%08X" % (fn, vram, t))

        for fn, vram in bounds:
            print("BOUND-8 %s 0x%08X" % (fn, vram))

        for (fn, vram), (word, why) in sorted(patches.items(), key=lambda kv: kv[0][1]):
            print("PATCH %s 0x%08X %08X %s" % (fn, vram, word, why))

        return 1 if problems else 0

    if problems:
        for line in problems:
            print("PROBLEM", line, file=sys.stderr)

        return 1

    lines = [
        "# Front-end menus hold 12 rows instead of 8: gCurrentMenuData's type/pos/w74/fD0 arrays move past +0xF8, its snapshot moves to host RAM,",
        "# and text group 5's fixed elements 8-12 move to 12-16. Generated by tools/re/gen_menu_rows.py; rerun it instead of editing.",
        "",
    ]

    for (fn, vram), (word, why) in sorted(patches.items(), key=lambda kv: kv[0][1]):
        lines += ["# " + why, "[[patches.instruction]]", 'func = "%s"' % fn, "vram = 0x%08X" % vram, "value = 0x%08X" % word, ""]

    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))

    print("wrote %d patches to %s" % (len(patches), args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
