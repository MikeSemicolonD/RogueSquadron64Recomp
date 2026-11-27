"""Compare two lockstep hash logs (ROGUESQ_LS_* harness) and report the first divergent frame."""
import sys
from dataclasses import dataclass, field


@dataclass
class Result:
    status: str
    frames_compared: int = 0
    first_frame: int = -1
    regions: list = field(default_factory=list)
    detail: str = ""


def _load(path):
    with open(path, "r", encoding="utf-8") as f:
        lines = f.read().splitlines()
    if not lines or not lines[0].startswith("# frame combined"):
        raise ValueError(f"{path}: missing legend")
    names = lines[0].split()[3:]
    rows = []
    for ln in lines[1:]:
        parts = ln.split()
        if len(parts) != len(names) + 2:
            break
        rows.append((int(parts[0]), parts[1], parts[2:]))
    return names, rows


def compare(baseline_path, replay_path):
    try:
        names_a, rows_a = _load(baseline_path)
        names_b, rows_b = _load(replay_path)
    except (OSError, ValueError) as e:
        return Result("unusable", detail=str(e))
    # A co-op host's log adds regions (the imposter) to the solo baseline's: then only the shared regions are compared, column by column, since the combined hash covers them all.
    shared = [n for n in names_a if n in names_b]
    if shared != names_a:
        return Result("unusable", detail=f"region sets differ: {names_a} vs {names_b}")
    if not rows_a or not rows_b:
        return Result("unusable", detail=f"no frames: {len(rows_a)} vs {len(rows_b)}")
    cols_a = [names_a.index(n) for n in shared]
    cols_b = [names_b.index(n) for n in shared]
    n = min(len(rows_a), len(rows_b))
    for i in range(n):
        fa, ca, pa = rows_a[i]
        fb, cb, pb = rows_b[i]
        if fa != fb:
            return Result("unusable", i, detail=f"frame numbering differs at row {i}: {fa} vs {fb}")
        if names_a == names_b and ca == cb:
            continue
        regions = [shared[k] for k in range(len(shared)) if pa[cols_a[k]] != pb[cols_b[k]]]
        if regions:
            return Result("diverged", i, fa, regions)
    # A run killed mid-frame can leave one file a single trailing line ahead.
    if abs(len(rows_a) - len(rows_b)) > 1:
        return Result("diverged", n, n, ["length"], f"{len(rows_a)} vs {len(rows_b)} frames")
    if len(rows_a) != len(rows_b):
        return Result("identical", n, detail="one trailing frame ignored")
    return Result("identical", n)


def main(argv):
    if len(argv) != 3:
        print("usage: compare_hashes.py <baseline.hash> <replay.hash>")
        return 2
    r = compare(argv[1], argv[2])
    if r.status == "identical":
        print(f"IDENTICAL {r.frames_compared} frames {r.detail}".rstrip())
        return 0
    if r.status == "diverged":
        print(f"DIVERGED at frame {r.first_frame} regions={','.join(r.regions)} {r.detail}".rstrip())
        return 1
    print(f"UNUSABLE {r.detail}")
    return 1 if r.detail.startswith("no frames") else 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
