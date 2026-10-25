#!/usr/bin/env python3
"""Two-sided diff of MusyX synth voice state, the audio analogue of dl_diff.py.

Runs audio_cmd_walk.py --json on a GOLDEN (PJ64) and a CANDIDATE (recomp) RDRAM dump at the same
game-state landmark (e.g. cine_frame120 vs a recomp cine_iter-120 capture) and reports the FIRST
structural divergence in the MusyX control block / voice slots, plus summary deltas. Same walker
both sides, so walker blind-spots cancel for the "is the CPU building the same voice state hardware
does?" question.

The historical silence bug is a LOGIC-layer divergence: hardware voices carry a live sample-cursor
pointer (voice+0x28) and keyOn=3; the broken recomp had null cursors / keyOn=0. This diff pins that.
Pointers are compared as first-seen ordinals (allocation-invariant), so a matching structure with
different absolute addresses compares EQUAL; a null-vs-present or keyOn mismatch does NOT.

Exit 1 on divergence. Usage: audio_diff.py GOLDEN CAND [--slots N]"""
import sys, os, json, subprocess, argparse

HERE = os.path.dirname(os.path.abspath(__file__))
WALK = os.path.join(HERE, 'audio_cmd_walk.py')

ap = argparse.ArgumentParser()
ap.add_argument('golden'); ap.add_argument('cand')
ap.add_argument('--slots', type=int, default=20)
a = ap.parse_args()

def walk(path):
    r = subprocess.run([sys.executable, WALK, path, '--json', '--slots', str(a.slots)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit('walker failed on %s:\n%s' % (path, r.stderr))
    return json.loads(r.stdout)['records']

G = walk(a.golden); C = walk(a.cand)

def key(rec):                               # identity of a record independent of allocation
    if rec['kind'] == 'ctrl': return ('ctrl',)
    if rec['kind'] == 'voice': return ('voice', rec['descBuf'], rec['slot'])
    if rec['kind'] == 'summary': return ('summary',)
    return (rec['kind'], rec['i'])
GI = {key(r): r for r in G}
CI = {key(r): r for r in C}

def fields(r):                              # comparable fields (drop volatile ordinal *values*; keep
    r = dict(r); r.pop('i', None)           # presence/nullness + scalar fields like keyOn/pitch)
    return r

print('== MusyX voice-state diff ==')
print('  golden: %s' % a.golden)
print('  cand:   %s' % a.cand)

diverged = False
# 1) control block
gc = GI.get(('ctrl',)); cc = CI.get(('ctrl',))
if gc and cc:
    for f in ('synthVoiceCount', 'byteCnt', 'outBufVoiceCount'):
        if gc.get(f) != cc.get(f):
            print('  CTRL DIVERGE %-18s golden=%s cand=%s' % (f, gc.get(f), cc.get(f)))
            diverged = True

# 2) voice slots, in golden order
gvoices = [r for r in G if r['kind'] == 'voice']
for gr in gvoices:
    k = key(gr); cr = CI.get(k)
    if cr is None:
        print('  VOICE MISSING in cand: descBuf%d slot%d (golden keyOn=%d pitch=0x%04X)'
              % (gr['descBuf'], gr['slot'], gr['keyOn'], gr['pitch']))
        diverged = True
        break
    # keyOn / pitch scalars
    if gr['keyOn'] != cr['keyOn'] or gr['pitch'] != cr['pitch']:
        print('  VOICE DIVERGE descBuf%d slot%d: keyOn %d->%d  pitch 0x%04X->0x%04X'
              % (gr['descBuf'], gr['slot'], gr['keyOn'], cr['keyOn'], gr['pitch'], cr['pitch']))
        diverged = True
        break
    # pointer PRESENCE at the same offsets (nullness is what the silence bug is about)
    goff = {o for o, _ in gr['ptrs']}; coff = {o for o, _ in cr['ptrs']}
    if goff != coff:
        miss = sorted(goff - coff); extra = sorted(coff - goff)
        print('  VOICE PTR-SET DIVERGE descBuf%d slot%d: golden-only offsets=%s cand-only=%s'
              % (gr['descBuf'], gr['slot'],
                 ['+0x%02X' % o for o in miss], ['+0x%02X' % o for o in extra]))
        print('     (a golden ptr absent in cand = an unpopulated voice field — the silence signature)')
        diverged = True
        break
    # full param-region scalar words (rate/root/adsr live here; a wrong RATE = the screech signature).
    # ptr-typed words compared as ordinal (alloc-invariant); scalar words compared by raw value.
    gw = {o: (t, v) for t, o, v in gr.get('words', [])}
    cw = {o: (t, v) for t, o, v in cr.get('words', [])}
    wdiff = []
    for o in sorted(gw):
        if o not in cw or gw[o][0] != cw[o][0] or (gw[o][0] == 'v' and gw[o][1] != cw[o][1]):
            wdiff.append((o, gw[o], cw.get(o)))
    if wdiff:
        print('  VOICE FIELD DIVERGE descBuf%d slot%d (keyOn/pitch/ptrs match; scalar fields differ):'
              % (gr['descBuf'], gr['slot']))
        for o, g, c in wdiff[:8]:
            gs = ('ptr#%d' % g[1]) if g[0] == 'p' else ('0x%08X' % g[1])
            cs = 'absent' if c is None else (('ptr#%d' % c[1]) if c[0] == 'p' else ('0x%08X' % c[1]))
            print('     +0x%02X: golden=%s cand=%s' % (o, gs, cs))
        diverged = True
        break

# 3) summary deltas
gs = GI.get(('summary',)); cs = CI.get(('summary',))
if gs and cs:
    print('  summary: activeSlots golden=%d cand=%d | synthVoiceCount golden=%d cand=%d'
          % (gs['activeSlots'], cs['activeSlots'], gs['synthVoiceCount'], cs['synthVoiceCount']))

if diverged:
    print('RESULT: DIVERGED')
    sys.exit(1)
print('RESULT: voice state structurally IDENTICAL (keyOn/pitch/ptr-presence match on all golden voices)')
