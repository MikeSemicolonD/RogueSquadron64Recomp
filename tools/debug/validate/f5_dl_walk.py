#!/usr/bin/env python3
"""Offline walk of a Factor 5 display list with the ucode's own rules (IMEM 0x1088..0x12F8, see
rt64_gbi_f3dfactor5.cpp): chunks are 0x108 bytes fetched whole, executed from +8; at +0x108 or on
B5/0x12 the walk continues in the chunk named by the current chunk's first word; 06 = call w1,
07 = branch w1, B8 = return / end. Lengths: 03 = 24, BD/BE/14 = 16, `05 05` = 40, textured BF/13/B4
= 32, untextured = 16.

--json emits a normalized, diff-friendly record stream (one record per flow op / primitive) with
absolute chunk addresses replaced by first-seen ordinals, so the same logical DL from two different
dumps (recomp vs PJ64 golden, different allocation addresses) compares equal. Consumed by dl_diff.py.
Usage: f5_dl_walk.py dump.bin [addr|task] [--max N] [--verbose] [--tail N] [--json]"""
import sys, struct, argparse, json
ap = argparse.ArgumentParser()
ap.add_argument('dump'); ap.add_argument('addr', default='task', nargs='?')
ap.add_argument('--max', type=int, default=20000); ap.add_argument('--verbose', action='store_true')
ap.add_argument('--tail', type=int, default=24)
ap.add_argument('--json', action='store_true', help='emit normalized diff-friendly records as JSON')
ap.add_argument('--b4', choices=('auto', '16', '32'), default='auto',
                help="0xB4 stride rule: auto=32 if w0&2 else 16 (hardware); 32=live op_b4_quad's unconditional rule (for A/B)")
ap.add_argument('--tex', action='store_true',
                help="decode per-face UVs (raw + texel via the 03 82 scale) and bound texture state; prints a "
                     "UV/material summary (with --json, adds st/idx/tex fields to face records).")
ap.add_argument('--legacy-uv', action='store_true', help="texel = raw/256 (ignore the 03 82 texcoord scale)")
a = ap.parse_args()
d = open(a.dump, 'rb').read()
def W(x): return struct.unpack('>I', d[x:x+4])[0]
def s16(v):                       # texcoord halfwords are signed 16-bit
    v &= 0xFFFF
    return v - 0x10000 if (v & 0x8000) else v
# 03 82 = texcoord scale (16.16, s then t). The ucode multiplies each raw per-face UV by it to get
# S10.5; the game sends (W-1)/128 per material, so raw UVs are 4.12 normalized (0x1000 = one tile).
tc_scale = [0, 0]
def tc_apply(raw, scale):
    v = (raw * scale) >> 16
    return max(-32768, min(32767, v))
start = (W(0x377C8 + 0x30) if a.addr == 'task' else int(a.addr, 16)) & 0xFFFFFF
KNOWN = set([0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
             0x11, 0x12, 0x13, 0x14, 0x80, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA,
             0xBB, 0xBC, 0xBD, 0xBE, 0xBF] + list(range(0xE4, 0x100)))
def valid_chunk(p): return (p >> 24) == 0x80 and (p & 0xFFFFFF) != 0 and (p & 0xFFFFFF) + 0x108 <= 0x800000
def linked_next(base):
    nxt = W(base)
    if not valid_chunk(nxt): return None
    if W((nxt & 0xFFFFFF) + 4) != (0x80000000 | base): return None   # target's prev must point back (allocated list)
    return nxt
stack = []          # (return pc, caller chunk base)
base = start        # chunk being executed at this level
pc = start + 8
steps = 0; hist = []; unknown = 0; first_unknown = None; faces = 0; rects = 0; ended = False
# --json: normalize absolute chunk offsets to first-seen ordinals so two dumps of the same logical DL
# (different allocation addresses) produce identical record streams; real structural differences remain.
records = []; chunk_ords = {}
# --tex: running RDP texture state so each textured face can be tagged with the material it draws with,
# and an aggregate of emitted UVs (raw + texel after the 03 82 scale) to expose flip/swap at a glance.
tex = {'timg': None, 'fmt': None, 'siz': None, 'line': None, 'tmem': None, 'pal': None,
       'tile': None, 'w': None, 'h': None, 'sc': None, 'tc': None,
       'cms': None, 'cmt': None, 'masks': None, 'maskt': None,   # wrap modes + mask (wrap period = 2^mask)
       'tlut': None, 'uls': None, 'ult': None}                    # palette src; tile-space origin (10.2)
tex_faces = []                                    # per-face UV+state snapshots (for the summary)
def snapshot_tex(): return {k: tex[k] for k in tex}
def decode_face_uv(pc, op, w0, w1):
    """Return (idx[], st_raw[[s,t]...], texel[[s,t]...]) for a textured face at pc. Mirrors
    op_bf_tri / op_b4_quad / op_13_quad: st words at +16,+20,+24(,+28); s=hi16, t=lo16 (signed 8.8)."""
    quad = op in (0xB4, 0x13)
    if quad:
        idx = [((w1 >> 16) & 0xFF) // 5, ((w1 >> 8) & 0xFF) // 5, (w1 & 0xFF) // 5, (w1 >> 24) // 5]   # ucode: bytes 1,2,3,0
        words = [W(pc + 16), W(pc + 20), W(pc + 24), W(pc + 28)]
    else:
        idx = [((w1 >> 16) & 0xFF) // 5, ((w1 >> 8) & 0xFF) // 5, (w1 & 0xFF) // 5]
        words = [W(pc + 16), W(pc + 20), W(pc + 24)]
    st_raw = [[s16(x >> 16), s16(x & 0xFFFF)] for x in words]   # raw signed 16-bit
    if a.legacy_uv:
        texel = [[s / 256.0, t / 256.0] for s, t in st_raw]     # old reading: raw as 8.8 texels
    else:
        texel = [[tc_apply(s, tc_scale[0]) / 32.0, tc_apply(t, tc_scale[1]) / 32.0] for s, t in st_raw]
    return idx, st_raw, texel
def cord(off):
    off &= 0xFFFFFF
    if off not in chunk_ords: chunk_ords[off] = len(chunk_ords)
    return chunk_ords[off]
def rec(**kw):
    kw['i'] = len(records); records.append(kw)
def out(line):
    if not a.json: print(line)
def flow(line):
    hist.append(line)
    out('%s  depth=%d' % (line, len(stack)))
def enter(chunk):
    global base, pc
    base = chunk; pc = chunk + 8
while steps < a.max and not ended:
    if pc + 8 > 0x800000:
        out('OOB pc %08X' % pc); rec(kind='oob', depth=len(stack), chunk=cord(base)); break
    if pc >= base + 0x108:
        nxt = linked_next(base); steps += 1
        if nxt is not None:
            flow('%08X: (chunk end) -> next %08X' % (0x80000000 + pc, nxt))
            rec(kind='chunk_next', depth=len(stack), chunk=cord(base), to=cord(nxt))
            enter(nxt & 0xFFFFFF); continue
        nxt = W(base)
        flow('%08X: (chunk end, no next %08X)' % (0x80000000 + pc, nxt))
        rec(kind='chunk_end', depth=len(stack), chunk=cord(base))
        if stack: pc, base = stack.pop(); continue
        ended = True; break
    w0, w1 = W(pc), W(pc + 4); op = w0 >> 24; ln = 8; note = ''
    if op == 0x06 and (w0 & 0x00FEFFFF) == 0:
        t = w1 & 0xFFFFFF; steps += 1
        if ((w0 >> 16) & 1) == 0:
            stack.append((pc + 8, base)); flow('%08X: %08X %08X call' % (0x80000000 + pc, w0, w1))
            rec(kind='call', op=op, depth=len(stack), chunk=cord(base), to=cord(t))
        else:
            flow('%08X: %08X %08X branch' % (0x80000000 + pc, w0, w1))
            rec(kind='branch', op=op, depth=len(stack), chunk=cord(base), to=cord(t))
        enter(t); continue
    elif op == 0x07:
        t = w1 & 0xFFFFFF; steps += 1
        flow('%08X: %08X %08X branch' % (0x80000000 + pc, w0, w1))
        bad = (t == 0 or t + 0x108 > 0x800000)
        rec(kind='branch07', op=op, depth=len(stack), chunk=cord(base), to=('bad' if bad else cord(t)))
        if bad: out('   bad target'); ended = True; break
        enter(t); continue
    elif op in (0xB5, 0x12):
        nxt = linked_next(base); steps += 1
        flow('%08X: %08X %08X next -> %s' % (0x80000000 + pc, w0, w1, ('%08X' % nxt) if nxt is not None else 'unlinked %08X' % W(base)))
        rec(kind='next', op=op, depth=len(stack), chunk=cord(base), to=(cord(nxt) if nxt is not None else 'unlinked'))
        if nxt is not None: enter(nxt & 0xFFFFFF); continue
        if stack: pc, base = stack.pop(); continue
        ended = True; break
    elif op in (0xB8, 0x0F):
        steps += 1; flow('%08X: %08X %08X ret' % (0x80000000 + pc, w0, w1))
        rec(kind='ret', op=op, depth=len(stack), chunk=cord(base))
        if stack: pc, base = stack.pop(); continue
        ended = True; break
    elif op == 0x05: ln = 40 if ((w0 >> 16) & 0xFF) == 5 else 8
    elif op in (0xBD, 0x0A): ln = 24              # sprite record: w2 fog color, w3 half-extent, w4 tex extent
    elif op in (0xBE, 0x14, 0x09): ln = 16
    elif op == 0x03:
        ln = 24
        if ((w0 >> 16) & 0xFF) == 0x82:
            hi, lo = W(pc + 8), W(pc + 16)
            tc_scale[0] = s16(hi >> 16) * 65536 + (lo >> 16)
            tc_scale[1] = s16(hi & 0xFFFF) * 65536 + (lo & 0xFFFF)
    elif op in (0xBF, 0x08, 0x13): ln = 32 if (w0 & 2) else 16
    elif op == 0xB4: ln = 32 if a.b4 == '32' else (16 if a.b4 == '16' else (32 if (w0 & 2) else 16))
    elif op not in KNOWN:
        unknown += 1; note = 'UNKNOWN'
        if first_unknown is None: first_unknown = (steps, pc)
    if a.tex:                                     # track RDP texture state as it flows
        if op == 0xFD:                            # SETTIMG: bound texture image
            tex['timg'] = w1 & 0xFFFFFF; tex['fmt'] = (w0 >> 21) & 7; tex['siz'] = (w0 >> 19) & 3
        elif op == 0xF5:                          # SETTILE: fmt/siz/line/tmem/palette + wrap/mask
            tex['tile'] = (w1 >> 24) & 7; tex['fmt'] = (w0 >> 21) & 7; tex['siz'] = (w0 >> 19) & 3
            tex['line'] = (w0 >> 9) & 0x1FF; tex['tmem'] = w0 & 0x1FF; tex['pal'] = (w1 >> 20) & 0xF
            tex['cmt'] = (w1 >> 18) & 3; tex['maskt'] = (w1 >> 14) & 0xF
            tex['cms'] = (w1 >> 8) & 3;  tex['masks'] = (w1 >> 4) & 0xF
        elif op == 0xF2:                          # SETTILESIZE: uls/ult/lrs/lrt (10.2) -> W/H texels
            uls, ult = (w0 >> 12) & 0xFFF, w0 & 0xFFF
            lrs, lrt = (w1 >> 12) & 0xFFF, w1 & 0xFFF
            tex['w'] = ((lrs - uls) >> 2) + 1; tex['h'] = ((lrt - ult) >> 2) + 1
            tex['uls'] = uls; tex['ult'] = ult   # tile-space origin (10.2); nonzero = UV offset
        elif op == 0xF0:                          # LOADTLUT: current texture image = palette source
            tex['tlut'] = tex['timg']
        elif op == 0xBB:                          # G_TEXTURE: s/t scale (stream sends 0 -> full)
            tex['sc'] = (w1 >> 16) & 0xFFFF; tex['tc'] = w1 & 0xFFFF
    if a.tex and op in (0xF0, 0xF3, 0xF4):        # emit load records so the palette/pixel sources are visible
        tile = (w1 >> 24) & 7
        if op == 0xF0:                            # loadTLUT: src = current image; ncols from lrs
            rec(kind='load', sub='tlut', op=op, chunk=cord(base),
                src=('0x%06X' % (tex['timg'] or 0)), tile=tile, ncols=(((w1 >> 12) & 0xFFF) >> 2) + 1)
        else:                                     # loadBlock/loadTile: src = pixel image
            rec(kind='load', sub=('block' if op == 0xF3 else 'tile'), op=op, chunk=cord(base),
                src=('0x%06X' % (tex['timg'] or 0)), tile=tile,
                uls=(w0 >> 12) & 0xFFF, ult=w0 & 0xFFF, lrs=(w1 >> 12) & 0xFFF, dxt=w1 & 0xFFF)
    if op == 0xFD:                                # SETTIMG (bound texture image)
        rec(kind='settimg', op=op, chunk=cord(base), addr='0x%06X' % (w1 & 0xFFFFFF), fmt=(w0 >> 21) & 7, siz=(w0 >> 19) & 3)
    if op == 0xF5:                                # SETTILE (which tile, fmt/siz/tmem)
        rec(kind='settile', op=op, chunk=cord(base), tile=(w1 >> 24) & 7, fmt=(w0 >> 21) & 7, siz=(w0 >> 19) & 3, line=(w0 >> 9) & 0x1FF, tmem=w0 & 0x1FF)
    if op == 0xFC:                                # SETCOMBINE
        rec(kind='combine', op=op, chunk=cord(base), w0='0x%08X' % w0, w1='0x%08X' % w1)
    if op in (0xB9, 0xBA):                        # SETOTHERMODE_L / _H
        rec(kind='othermode', op=op, chunk=cord(base), w0='0x%08X' % w0, w1='0x%08X' % w1)
    if op == 0xBD:                                # billboard sprite
        rec(kind='bd', op=op, chunk=cord(base), w0='0x%08X' % w0, w1='0x%08X' % w1,
            fog='0x%08X' % W(pc + 8), extent='0x%08X' % W(pc + 12), tex='0x%08X' % W(pc + 16))
    if op == 0x05 and ((w0 >> 16) & 0xFF) == 5:   # terrain tile: 05 05 02=flat quad, 05 05 00=heightfield grid
        sub = (w0 >> 8) & 0xFF
        payload = ['0x%08X' % W(pc + 8 + i * 4) for i in range(8)]   # words 2..9 of the 40-byte record
        rec(kind='terrain', op=op, depth=len(stack), chunk=cord(base),
            form=('flat' if (sub & 2) else 'grid'), w0=('0x%08X' % w0), w1=('0x%08X' % w1), payload=payload)
    if op in (0xBF, 0xB4, 0x13, 0x08):
        faces += 1
        r = dict(kind='face', op=op, depth=len(stack), chunk=cord(base),
                 verts=(3 if op in (0xBF, 0x08) else 4), tex=bool(w0 & 2), stride=ln)
        if a.tex and (w0 & 2):
            idx, st_raw, texel = decode_face_uv(pc, op, w0, w1)
            r['idx'] = idx; r['st'] = st_raw; r['texel'] = texel; r['mat'] = snapshot_tex()
            tex_faces.append(r)
        rec(**r)
    elif op in (0xE4, 0xE5):
        rects += 1; rec(kind='rect', op=op, depth=len(stack), chunk=cord(base), stride=ln)
    elif op not in KNOWN:
        rec(kind='unknown', op=op, depth=len(stack), chunk=cord(base))
    else:
        rec(kind='state', op=op, depth=len(stack), chunk=cord(base), stride=ln)
    hist.append('%08X: %08X %08X %s' % (0x80000000 + pc, w0, w1, note))
    if a.verbose: out(hist[-1] + ('  depth=%d' % len(stack)))
    steps += 1; pc += ln
def tex_summary():
    """Aggregate emitted UVs per bound material. Flip/swap tells: if t-texel clusters near H (and
    away from 0) the runtime baked v=1-rawV; an s-range that tracks H (not W) means s/t are swapped."""
    if not tex_faces: return {'textured_faces': 0}
    from collections import defaultdict
    buckets = defaultdict(lambda: {'faces': 0, 's': [1e9, -1e9], 't': [1e9, -1e9]})
    gs = [1e9, -1e9]; gt = [1e9, -1e9]
    for f in tex_faces:
        m = f['mat']
        key = (m['timg'], m['fmt'], m['siz'], m['w'], m['h'], m['cms'], m['cmt'], m['masks'], m['maskt'])
        b = buckets[key]; b['faces'] += 1
        for s, t in f['texel']:
            b['s'][0] = min(b['s'][0], s); b['s'][1] = max(b['s'][1], s)
            b['t'][0] = min(b['t'][0], t); b['t'][1] = max(b['t'][1], t)
            gs[0] = min(gs[0], s); gs[1] = max(gs[1], s); gt[0] = min(gt[0], t); gt[1] = max(gt[1], t)
    cmname = {0: 'wrap', 1: 'mirror', 2: 'clamp', 3: 'clmir', None: '?'}
    mats = []
    for (timg, fmt, siz, w, h, cms, cmt, masks, maskt), b in sorted(buckets.items(), key=lambda kv: -kv[1]['faces']):
        mats.append({'timg': ('0x%06X' % timg) if timg is not None else None,
                     'fmt': fmt, 'siz': siz, 'w': w, 'h': h, 'faces': b['faces'],
                     'cms': cmname[cms], 'cmt': cmname[cmt], 'masks': masks, 'maskt': maskt,
                     's_texel': [round(b['s'][0], 2), round(b['s'][1], 2)],
                     't_texel': [round(b['t'][0], 2), round(b['t'][1], 2)]})
    return {'textured_faces': len(tex_faces),
            's_texel_range': [round(gs[0], 2), round(gs[1], 2)],
            't_texel_range': [round(gt[0], 2), round(gt[1], 2)], 'materials': mats}
if a.json:
    summ = {'steps': steps, 'faces': faces, 'rects': rects, 'unknown': unknown,
            'chunks': len(chunk_ords), 'end': 'clean' if ended else 'runaway'}
    if a.tex: summ['tex'] = tex_summary()
    print(json.dumps({'summary': summ, 'records': records}))
else:
    print('steps=%d faces=%d rects=%d unknown=%d first_unknown=%s end=%s' % (
        steps, faces, rects, unknown, ('step %d @%08X' % (first_unknown[0], 0x80000000 + first_unknown[1])) if first_unknown else None,
        'clean' if ended else 'RUNAWAY (max steps)'))
    if first_unknown:
        lo = max(0, first_unknown[0] - a.tail)
        print('--- history before first unknown ---'); print('\n'.join(hist[lo:first_unknown[0] + 4]))
    if a.tex:
        s = tex_summary()
        print('--- UV / material summary ---')
        print('textured faces=%d  s_texel=%s  t_texel=%s' % (
            s['textured_faces'], s.get('s_texel_range'), s.get('t_texel_range')))
        for m in s.get('materials', []):
            print('  timg=%s fmt=%s siz=%s tile=%sx%s cms=%s/%d cmt=%s/%d faces=%d  s=%s t=%s' % (
                m['timg'], m['fmt'], m['siz'], m['w'], m['h'],
                m['cms'], m['masks'], m['cmt'], m['maskt'], m['faces'], m['s_texel'], m['t_texel']))
        print('read: texels outside [0,tile] need cms/cmt=wrap|mirror with mask=log2(dim); '
              'clamp there => smear. t near H not 0 => baked v-flip; s tracking H => s/t swap.')
