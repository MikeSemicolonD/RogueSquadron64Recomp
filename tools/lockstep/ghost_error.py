"""Ghost co-op puppet accuracy: compares each instance's puppet track with the other instance's own track at the same moment.

Usage: python tools/lockstep/ghost_error.py <host ghost.csv> <client ghost.csv> [--p95 1.0] [--settle-ms 3000]
Both CSVs come from ROGUESQ_GHOST_TRACE on one machine, so their steady-clock ms columns share a timeline.
Exits 1 when a direction has no samples or its p95 error exceeds --p95.
"""
import argparse
import bisect
import csv
import math
import sys


def load(path):
    rows = []
    with open(path, newline="") as f:
        for r in csv.reader(f):
            if not r or r[0] == "ms":
                continue
            try:
                rows.append([float(x) for x in r])
            except ValueError:
                continue
    return rows


def at(truth, times, t):
    i = bisect.bisect_left(times, t)
    if i <= 0 or i >= len(truth):
        return None
    a, b = truth[i - 1], truth[i]
    u = (t - a[0]) / (b[0] - a[0]) if b[0] > a[0] else 0.0
    return [a[k] + (b[k] - a[k]) * u for k in (1, 2, 3)]


def smoothness(track):
    """p99 of |p[i+1] - 2 p[i] + p[i-1]| (units per frame squared): corrections and snaps show up as spikes a real flight path does not have."""
    d = []
    for i in range(1, len(track) - 1):
        a, b, c = track[i - 1], track[i], track[i + 1]
        d.append(math.dist([c[k] - 2 * b[k] + a[k] for k in (1, 2, 3)], [0.0, 0.0, 0.0]))
    if not d:
        return 0.0
    d.sort()
    return d[int(0.99 * (len(d) - 1))]


def error(truth, puppet, settle_ms, delay_ms=0.0):
    times = [r[0] for r in truth]
    shown = [r for r in puppet if not any(math.isnan(x) for x in r[4:7])]
    if not shown:
        return 0, 0.0, 0.0, 0.0
    t0 = shown[0][0] + settle_ms
    errs = []
    for r in shown:
        if r[0] < t0:
            continue
        p = at(truth, times, r[0] - delay_ms)
        if p is None:
            continue
        errs.append(math.dist(p, r[4:7]))
    if not errs:
        return 0, 0.0, 0.0, 0.0
    errs.sort()
    return len(errs), sum(errs) / len(errs), errs[int(0.95 * (len(errs) - 1))], errs[-1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("client")
    ap.add_argument("--p95", type=float, default=1.0)
    ap.add_argument("--settle-ms", type=float, default=3000)
    # Path error: the puppet against where the other player was delay-ms earlier (an interpolating puppet is meant to trail by about latency + its buffer).
    ap.add_argument("--delay-ms", type=float, default=None)
    # Also report the delay (0..800 ms, 10 ms steps) with the lowest mean path error: how far the puppet actually trails.
    ap.add_argument("--fit", action="store_true")
    a = ap.parse_args()
    h, c = load(a.host), load(a.client)
    ok = True
    for name, truth, pup in (("host->client", h, c), ("client->host", c, h)):
        n, mean, p95, mx = error(truth, pup, a.settle_ms)
        shown = [[r[0], r[4], r[5], r[6]] for r in pup if not any(math.isnan(x) for x in r[4:7])]
        own = [[r[0], r[1], r[2], r[3]] for r in truth]
        line = f"{name}: n={n} mean={mean:.3f} p95={p95:.3f} max={mx:.3f} smooth puppet={smoothness(shown):.4f} truth={smoothness(own):.4f}"
        if a.delay_ms is not None:
            _, pmean, pp95, _ = error(truth, pup, a.settle_ms, a.delay_ms)
            line += f" path@{a.delay_ms:.0f}ms mean={pmean:.3f} p95={pp95:.3f}"
        if a.fit:
            best = min((error(truth, pup, a.settle_ms, d)[1], d) for d in range(0, 801, 10))
            _, _, fp95, _ = error(truth, pup, a.settle_ms, best[1])
            line += f" fit@{best[1]}ms mean={best[0]:.3f} p95={fp95:.3f}"
        print(line)
        ok = ok and n > 0 and p95 <= a.p95
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
