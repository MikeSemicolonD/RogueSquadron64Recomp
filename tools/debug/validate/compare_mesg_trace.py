#!/usr/bin/env python3
"""Compare per-frame message order: PJ64 hw trace (ra symbolized) vs recomp trace (host caller column).
Usage: compare_mesg_trace.py hw.csv recomp.csv [--frames 60-70] [--skip SI,mutex]
Events are normalized to "event queue function" (offsets dropped); dispatch/enqyield/start rows ignored.
"""
import sys, os, argparse, collections
sys.path.insert(0, os.path.dirname(__file__))
from rdram_golden_diff import load_symbols, sym_at
ap = argparse.ArgumentParser()
ap.add_argument('hw'); ap.add_argument('recomp')
ap.add_argument('--frames', default='60-70'); ap.add_argument('--overlay', default='cinematic')
ap.add_argument('--symbols', default=os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..', '..', '..', 'rogue_squadron64', 'symbol_files')))
ap.add_argument('--noise', default='siServiceThread,wakeSerialThreadOnSi,__osSiGetAccess,__osSiRelAccess,osContStartQuery,osContStartReadData,factor5MutexAcquire,factor5MutexRelease,rs_malloc,rs_free,pollControllerInputs,__osDevMgrMain,piDmaWorker,sendServiceMessage,recvServiceMessage,yieldThreadRet1,serviceVoicePoolFrame,runAiAudioStreamPlaybackLoop,func_8008ED70,factor5QueueBlock,factor5QueueSignal')
a = ap.parse_args()
lo, hi = map(int, a.frames.split('-'))
noise = set(a.noise.split(','))
allsyms = load_symbols([a.symbols])
keep = {'main_overlay', 'libultra', 'zlib', a.overlay + '_overlay'}
syms = [s for s in allsyms if s[4] in keep]
IGN = {'dispatch', 'enqyield', 'start'}
def norm_hw(path):
    out = collections.defaultdict(list)
    for line in open(path):
        p = line.strip().split(',')
        if len(p) < 6 or p[1] in IGN: continue
        f = int(p[0])
        if f < lo or f > hi: continue
        s = sym_at(syms, int(p[5], 16)); fn = s[0] if s else '?'
        if fn in noise: continue
        out[f].append('%s %s %s' % (p[1], p[3], fn))
    return out
def norm_rc(path):
    out = collections.defaultdict(list)
    for line in open(path):
        p = line.strip().split(',')
        if len(p) < 9 or p[1] in IGN or p[1] == 'recvret': continue
        f = int(p[0])
        if f < lo or f > hi: continue
        fn = p[8].split('+')[0]
        if fn in noise: continue
        out[f].append('%s %s %s' % (p[1], p[3], fn))
    return out
hw, rc = norm_hw(a.hw), norm_rc(a.recomp)
for f in range(lo, hi + 1):
    h, r = hw.get(f, []), rc.get(f, [])
    ch, cr = collections.Counter(h), collections.Counter(r)
    print('== frame %d: hw %d events, recomp %d events ==' % (f, len(h), len(r)))
    for k in sorted(set(ch) | set(cr)):
        if ch[k] != cr[k]: print('   %-60s hw=%d recomp=%d' % (k, ch[k], cr[k]))
