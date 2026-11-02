"""Loader + validator for state_model.toml (the canonical game-state descriptor).

Works on any interpreter used to build: stdlib `tomllib` (3.11+) or the `tomli`
backport (3.9/3.10). The CMake build Python (3.11) and the repo tools Python (3.9)
each have exactly one of these.
"""
try:
    import tomllib as _toml
except ModuleNotFoundError:
    import tomli as _toml
from dataclasses import dataclass, field
from typing import Optional

VALID_OPS = {"eq", "ne", "range", "flag"}


@dataclass
class Predicate:
    addr: int
    mask: int
    op: str
    value: int
    value_hi: Optional[int] = None


@dataclass
class Force:
    slot: str
    params: dict


@dataclass
class State:
    id: str
    name: str
    parent: str
    classify: list
    force: Optional[Force] = None
    reachable_from: list = field(default_factory=list)
    focus: list = field(default_factory=list)


class Model:
    def __init__(self, states):
        self.states = states
        self.by_id = {s.id: s for s in states}

    def depth(self, sid):
        d, cur = 0, self.by_id[sid]
        while cur.parent:
            d += 1
            cur = self.by_id.get(cur.parent)
            if cur is None:
                break
        return d

    def children_of(self, sid):
        return [s for s in self.states if s.parent == sid]


def load_model(path):
    with open(path, "rb") as f:
        raw = _toml.load(f)
    states = []
    for e in raw.get("state", []):
        preds = [Predicate(p["addr"], p["mask"], p["op"], p["value"], p.get("value_hi"))
                 for p in e.get("classify", [])]
        force = None
        if "force" in e:
            force = Force(e["force"]["slot"], dict(e["force"].get("params", {})))
        states.append(State(e["id"], e["name"], e.get("parent", ""), preds, force,
                            list(e.get("reachable_from", [])), list(e.get("focus", []))))
    return Model(states)


def _rd_be(buf, kseg0):
    o = kseg0 - 0x80000000
    if o < 0 or o + 4 > len(buf):
        return 0
    return int.from_bytes(buf[o:o + 4], "big")


def _pred_holds(buf, p):
    """Mirror of the host classifier (game_state.cpp) over a guest-order (big-endian) buffer."""
    v = _rd_be(buf, p.addr) & p.mask
    pv = p.value & p.mask
    if p.op == "eq":
        return v == pv
    if p.op == "ne":
        return v != pv
    if p.op == "flag":
        return v == pv
    if p.op == "range":
        return (p.value & p.mask) <= v <= ((p.value_hi or 0) & p.mask)
    return False


def classify(model, buf):
    """Return the deepest-matching state id for a guest-order RDRAM buffer, or 'unknown'."""
    best, best_depth = None, -1
    for s in model.states:
        if s.classify and all(_pred_holds(buf, p) for p in s.classify):
            d = model.depth(s.id)
            if d > best_depth:
                best, best_depth = s.id, d
    return best or "unknown"


def validate(model):
    errs = []
    seen = set()
    for s in model.states:
        if s.id in seen:
            errs.append(f"duplicate id: {s.id}")
        seen.add(s.id)
        if s.parent and s.parent not in model.by_id:
            errs.append(f"state {s.id}: parent '{s.parent}' does not exist")
        for p in s.classify:
            if p.op not in VALID_OPS:
                errs.append(f"state {s.id}: bad op '{p.op}'")
            if p.op == "range" and p.value_hi is None:
                errs.append(f"state {s.id}: range predicate missing value_hi")
    errs += _overlap_errors(model)
    return errs


# --- sibling overlap (symbolic; no brute-force iteration over 32-bit ranges) ---

def _interval(p):
    """Masked-value interval [lo, hi] a predicate admits, or None if not expressible
    as a single interval (ne: complement of a point -> treat as 'anything', which is
    conservative: cannot prove disjoint)."""
    m = p.mask
    v = p.value & m
    if p.op in ("eq", "flag"):
        return (v, v)
    if p.op == "range":
        return (p.value & m, (p.value_hi if p.value_hi is not None else p.value) & m)
    return None  # ne


def _key_disjoint(a_preds, b_preds):
    """True if, on this shared (addr,mask) key, a and b can never both hold."""
    def excludes(eqs, nes):
        return any(e.op == "eq" and n.op == "ne" and (e.value & e.mask) == (n.value & n.mask) for e in eqs for n in nes)
    if excludes(a_preds, b_preds) or excludes(b_preds, a_preds):
        return True
    a_int = [i for i in (_interval(p) for p in a_preds) if i is not None]
    b_int = [i for i in (_interval(p) for p in b_preds) if i is not None]
    if not a_int or not b_int:
        return False  # an ne (or unexpressible) predicate present -> cannot prove disjoint
    # a holds only within the intersection of its own intervals; same for b.
    a_lo = max(lo for lo, _ in a_int)
    a_hi = min(hi for _, hi in a_int)
    b_lo = max(lo for lo, _ in b_int)
    b_hi = min(hi for _, hi in b_int)
    if a_lo > a_hi or b_lo > b_hi:
        return True  # one side self-contradictory -> never holds -> trivially disjoint
    return a_hi < b_lo or b_hi < a_lo


def _by_key(preds):
    m = {}
    for p in preds:
        m.setdefault((p.addr, p.mask), []).append(p)
    return m


def _confusable(a, b):
    """Two sibling predicate sets are confusable when they test at least one common
    (addr,mask) field and are non-disjoint on every shared field, so no single tested
    field distinguishes them. Siblings keyed on entirely different fields are a design
    choice (resolved by deepest-first + order), not a lint error."""
    ma, mb = _by_key(a), _by_key(b)
    shared = set(ma) & set(mb)
    if not shared:
        return False
    return all(not _key_disjoint(ma[key], mb[key]) for key in shared)


def _overlap_errors(model):
    errs = []
    by_parent = {}
    for s in model.states:
        by_parent.setdefault(s.parent, []).append(s)
    for sibs in by_parent.values():
        for i in range(len(sibs)):
            for j in range(i + 1, len(sibs)):
                if _confusable(sibs[i].classify, sibs[j].classify):
                    errs.append(f"ambiguous siblings: '{sibs[i].id}' and '{sibs[j].id}' can both match")
    return errs
