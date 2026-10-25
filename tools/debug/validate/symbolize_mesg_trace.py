#!/usr/bin/env python3
"""Annotate a mesg_trace CSV (frame,event,tid,queue,flags,ra) with the function at ra.
Usage: symbolize_mesg_trace.py trace.csv [--overlay cinematic] [--frame N] [--no-dispatch]
"""
import sys, os, argparse
sys.path.insert(0, os.path.dirname(__file__))
from rdram_golden_diff import load_symbols, sym_at

ap = argparse.ArgumentParser()
ap.add_argument('trace'); ap.add_argument('--overlay', default='cinematic')
ap.add_argument('--frame', type=int); ap.add_argument('--no-dispatch', action='store_true')
ap.add_argument('--symbols', default=os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..', '..', '..', 'rogue_squadron64', 'symbol_files')))
a = ap.parse_args()
allsyms = load_symbols([a.symbols])
keep = {'main_overlay', 'libultra', 'zlib', a.overlay + '_overlay'}
syms = [s for s in allsyms if s[4] in keep]
def name(ra):
    if ra == 0: return '-'
    s = sym_at(syms, ra)
    return '%s+0x%x' % (s[0], s[1]) if s else '?'
for line in open(a.trace):
    p = line.strip().split(',')
    if len(p) < 6: continue
    if a.frame is not None and int(p[0]) != a.frame: continue
    if a.no_dispatch and p[1] == 'dispatch': continue
    print('%s,%s,%s,%s,%s,%s,%s' % (p[0], p[1], p[2], p[3], p[4], p[5], name(int(p[5], 16))))
