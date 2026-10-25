#!/usr/bin/env python3
"""Two-sided Factor 5 display-list diff: walk GOLDEN and CANDIDATE RDRAM dumps with f5_dl_walk.py
--json and compare the normalized record streams, reporting the FIRST divergence (op + chunk ordinal
+ field) plus summary deltas.

Both dumps must be raw 8 MB MIPS-big-endian RDRAM images (the layout every ROGUESQ_DUMP_RDRAM_* writer
and the PJ64 goldens produce). Because the SAME walker runs on both sides, any grammar blind-spot in
the walker cancels out for the seam question "did the game emit the same DL?" — so a divergence here is
a real recomp-vs-hardware DL difference, not a walker artifact.

  dl_diff.py GOLDEN.bin CANDIDATE.bin [addr|task] [--max N] [--context K] [--summary-only]

Exit status: 0 = identical record streams, 1 = divergence, 2 = walk/tool error.
"""
import argparse, json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
WALKER = os.path.join(HERE, 'f5_dl_walk.py')

ap = argparse.ArgumentParser()
ap.add_argument('golden'); ap.add_argument('candidate'); ap.add_argument('addr', default='task', nargs='?')
ap.add_argument('--max', type=int, default=20000)
ap.add_argument('--context', type=int, default=4, help='records of context to show around a divergence')
ap.add_argument('--summary-only', action='store_true')
a = ap.parse_args()

def walk(path):
    if not os.path.isfile(path):
        print('ERROR: no such dump: %s' % path); sys.exit(2)
    r = subprocess.run([sys.executable, WALKER, path, a.addr, '--max', str(a.max), '--json'],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print('ERROR: walker failed on %s:\n%s' % (path, r.stderr.strip())); sys.exit(2)
    try:
        return json.loads(r.stdout)
    except json.JSONDecodeError as e:
        print('ERROR: walker did not emit JSON for %s: %s' % (path, e)); sys.exit(2)

def fmt(rec):
    if rec is None: return '(end of stream)'
    r = dict(rec); i = r.pop('i', '?')
    op = r.pop('op', None); kind = r.pop('kind', '?')
    head = 'op=0x%02X' % op if isinstance(op, int) else 'op=--'
    extra = ' '.join('%s=%s' % (k, v) for k, v in r.items())
    return '#%s %-11s %s %s' % (i, kind, head, extra)

g, c = walk(a.golden), walk(a.candidate)
gs, cs = g['summary'], c['summary']

print('golden    : %s' % a.golden)
print('candidate : %s' % a.candidate)
print('summary   : %-8s %-8s %-8s' % ('', 'golden', 'cand'))
for k in ('steps', 'faces', 'rects', 'unknown', 'chunks', 'end'):
    flag = '' if gs.get(k) == cs.get(k) else '   <-- DIFF'
    print('  %-9s %-8s %-8s%s' % (k, gs.get(k), cs.get(k), flag))

if a.summary_only:
    sys.exit(0 if gs == cs else 1)

gr, cr = g['records'], c['records']
n = min(len(gr), len(cr))
div = None
for i in range(n):
    a_, b_ = dict(gr[i]), dict(cr[i])
    a_.pop('i', None); b_.pop('i', None)
    if a_ != b_:
        div = i; break

if div is None and len(gr) == len(cr):
    print('\nRESULT: identical DL record streams (%d records).' % len(gr))
    sys.exit(0)

if div is None:
    # streams agree up to the shorter one's length but differ in length
    i = n
    print('\nRESULT: streams agree for %d records, then LENGTH differs '
          '(golden=%d, candidate=%d).' % (n, len(gr), len(cr)))
else:
    i = div
    diff_fields = sorted(set(k for k in (set(gr[i]) | set(cr[i])) if k != 'i'
                             and gr[i].get(k) != cr[i].get(k)))
    print('\nRESULT: first divergence at record #%d  (fields: %s)' % (i, ', '.join(diff_fields)))

lo = max(0, i - a.context)
print('--- context (golden | candidate) ---')
for j in range(lo, i):
    print('  %s' % fmt(gr[j]))          # identical up to the divergence
print('>> golden    : %s' % fmt(gr[i] if i < len(gr) else None))
print('>> candidate : %s' % fmt(cr[i] if i < len(cr) else None))
sys.exit(1)
