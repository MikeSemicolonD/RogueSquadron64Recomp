#!/usr/bin/env python3
"""Perception-free walk of the MusyX synth voice state in an 8 MB big-endian RDRAM dump.

The audio analogue of f5_dl_walk.py. Where the DL walker follows chunk links, this reads MusyX's
control block at the FIXED global 0x80149700 (so no data_ptr search is needed) and walks the voice
descriptor slots the CPU builds for the RSP synth. Field offsets come from the decomp
(rogue_squadron64 symbol_files) + src/main/main.cpp probes:

  control block @ 0x80149700:
    +0x00 u16  synthVoiceCount        (musyxBuildVoiceCommandList)
    +0x02 u8   byteCnt
    +0x08 u32  descBuf[0]  ] voice descriptor buffers (musyxMixActiveVoices writes these);
    +0x0C u32  descBuf[1]  ] up to 20 slots, stride 0x88, keyOn/active flag @ slot+0x22
    +0x50 u32  cmdListHead (0x80149750)
    +0x58 u32  outBuf[0]
    +0x5C u32  outBuf[1]
    +0x60 u16  outBufVoiceCount (0x80149760)
    +0x64 u32  ctrl[-0x689C] +0x68 ctrl[-0x6898] +0x6C ctrl[-0x6894] +0x70 ctrl[-0x6890]

  voice slot (stride 0x88): +0x04 u16 pitch/period (musyxSetVoicePitchBend, default 0x1000),
    +0x22 u8 keyOn/active, sampPtr/loop/base loaded by musyxInitVoiceFromSample (offset located
    empirically -- text mode labels every 0x80xxxxxx/0xB0xxxxxx word in a slot as a ptr candidate).

--json emits a normalized record stream (descBuf/outBuf/ctrl pointers -> first-seen ordinals) so a
recomp dump and a PJ64 golden compare equal despite different allocation addresses. Consumed by
audio_diff.py.
Usage: audio_cmd_walk.py dump.bin [--json] [--slots N] [--stride 0x88]"""
import sys, struct, argparse, json

CTRL = 0x149700          # physical addr of the 0x80149700 control block
VOICE_STRIDE = 0x88
MAX_SLOTS = 20

ap = argparse.ArgumentParser()
ap.add_argument('dump')
ap.add_argument('--json', action='store_true')
ap.add_argument('--slots', type=int, default=MAX_SLOTS)
ap.add_argument('--stride', type=lambda x: int(x, 0), default=VOICE_STRIDE)
a = ap.parse_args()
d = open(a.dump, 'rb').read()
if len(d) < 0x800000:
    sys.exit('dump too small (%d bytes); expected 8 MB RDRAM' % len(d))

def W(p):                                   # big-endian word at physical addr (dump is rdram[i^3])
    p &= 0xFFFFFF
    return struct.unpack('>I', d[p:p+4])[0]
def H(p):
    p &= 0xFFFFFF
    return struct.unpack('>H', d[p:p+2])[0]
def B(p):
    return d[(p & 0xFFFFFF) ^ 3]
def phys(ptr):
    return ptr & 0xFFFFFF
def valid(ptr):
    return (ptr >> 24) in (0x80, 0xB0) and 0 < phys(ptr) < 0x800000

records = []; ords = {}
def ordn(ptr):
    p = phys(ptr)
    if p not in ords: ords[p] = len(ords)
    return ords[p]
def rec(**kw):
    kw['i'] = len(records); records.append(kw)
def out(s):
    if not a.json: print(s)

vcount = H(CTRL + 0x00); bcnt = B(CTRL + 0x02)
descbuf = [W(CTRL + 0x08), W(CTRL + 0x0C)]
cmdhead = W(CTRL + 0x50)
outbuf = [W(CTRL + 0x58), W(CTRL + 0x5C)]
obvc = H(CTRL + 0x60)
ctrl = [W(CTRL + 0x64), W(CTRL + 0x68), W(CTRL + 0x6C), W(CTRL + 0x70)]

out('== MusyX control block @ 0x80149700 ==')
out('  synthVoiceCount=%d byteCnt=%d outBufVoiceCount=%d' % (vcount, bcnt, obvc))
out('  descBuf[0]=0x%08X descBuf[1]=0x%08X  cmdListHead=0x%08X' % (descbuf[0], descbuf[1], cmdhead))
out('  outBuf[0]=0x%08X outBuf[1]=0x%08X' % (outbuf[0], outbuf[1]))
out('  ctrl[-0x689C..-0x6890]=%s' % ' '.join('0x%08X' % c for c in ctrl))
rec(kind='ctrl', synthVoiceCount=vcount, byteCnt=bcnt, outBufVoiceCount=obvc,
    descBuf=[ordn(p) if valid(p) else None for p in descbuf],
    cmdListHead=ordn(cmdhead) if valid(cmdhead) else None,
    outBuf=[ordn(p) if valid(p) else None for p in outbuf],
    ctrl=[ordn(c) if valid(c) else (c & 0xFFFFFFFF) for c in ctrl])

def ptr_candidates(base):                   # words in the slot that look like RAM/cart pointers
    cands = []
    for off in range(0, a.stride, 4):
        w = W(base + off)
        if valid(w):
            cands.append((off, w))
    return cands

total_active = 0
for bi, bp in enumerate(descbuf):
    if not valid(bp):
        continue
    out('\n== descBuf[%d] @ 0x%08X (stride 0x%X) ==' % (bi, bp, a.stride))
    for s in range(a.slots):
        base = phys(bp) + s * a.stride
        if base + a.stride > 0x800000:
            break
        keyOn = B(base + 0x22)
        pitch = H(base + 0x04)
        nz = sum(1 for k in range(a.stride) if B(base + k))
        if not nz:
            continue                        # empty slot
        total_active += 1
        cands = ptr_candidates(base)
        candstr = ' '.join('+0x%02X=0x%08X' % (o, w) for o, w in cands)
        hexrow = ' '.join('%02X' % B(base + k) for k in range(a.stride))
        # Full word-by-word classification of the whole slot: each word is either a pointer
        # (compared by allocation-invariant ordinal) or a raw scalar (rate/root/adsr/flags,
        # compared by value). This is what lets audio_diff catch a wrong RATE at ANY offset.
        words = []
        for off in range(0, a.stride, 4):
            w = W(base + off)
            words.append(['p', off, ordn(w)] if valid(w) else ['v', off, w])
        out('  slot %2d: keyOn=%d pitch=0x%04X nz=%d/%d  ptrs[%s]' %
            (s, keyOn, pitch, nz, a.stride, candstr))
        if not a.json:
            out('          hex: %s' % hexrow)
        rec(kind='voice', descBuf=bi, slot=s, keyOn=keyOn, pitch=pitch, nz=nz,
            ptrs=[[o, ordn(w)] for o, w in cands], words=words)

out('\n== summary: active voice slots=%d  synthVoiceCount=%d ==' % (total_active, vcount))
rec(kind='summary', activeSlots=total_active, synthVoiceCount=vcount)

if a.json:
    json.dump({'records': records, 'ords': len(ords)}, sys.stdout, indent=0)
    print()
