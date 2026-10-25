#!/usr/bin/env python3
"""Walk a Factor 5 task list (same chunk rules as f5_dl_walk.py) while tracking the matrices the
HLE would load (01 03 = projection, 01 02 = modelview), the vertex batches (04) and faces
(BF/B4/13), and print where each face group lands in NDC. Used to compare the recomp's transform
against the hardware framebuffer rendered from the same dump (scratchpad fb_extract.py).
Usage: f5_dl_ndc.py dump.bin [--min-faces N]"""
import sys, struct, argparse
ap = argparse.ArgumentParser(); ap.add_argument('dump'); ap.add_argument('--min-faces', type=int, default=1)
ap.add_argument('--max', type=int, default=60000)
a = ap.parse_args()
d = open(a.dump, 'rb').read()
def W(x): return struct.unpack('>I', d[x:x+4])[0]
def H(x): return struct.unpack('>h', d[x:x+2])[0]
def U(x): return struct.unpack('>H', d[x:x+2])[0]
def mtx(x): return [[H(x + 2*(4*r+c)) + U(x + 32 + 2*(4*r+c)) / 65536.0 for c in range(4)] for r in range(4)]
def mul(A, B): return [[sum(A[r][k]*B[k][c] for k in range(4)) for c in range(4)] for r in range(4)]
def xf(v, M): return [sum(v[k]*M[k][c] for k in range(4)) for c in range(4)]
def valid_chunk(p): return (p >> 24) == 0x80 and (p & 0xFFFFFF) != 0 and (p & 0xFFFFFF) + 0x108 <= 0x800000
start = W(0x377C8 + 0x30) & 0xFFFFFF
stack = []; base = start; pc = start + 8; steps = 0
proj = None; mv = None; mvaddr = 0; projaddr = 0; verts = []; vaddr = 0
groups = []   # (mvaddr, projaddr, vaddr, nverts, faces, ndc list)
cur = None
def flush():
    global cur
    if cur and cur['faces'] >= a.min_faces: groups.append(cur)
    cur = None
def enter(chunk):
    global base, pc
    base = chunk; pc = chunk + 8
def face(idx, tag):
    global cur
    if proj is None or mv is None or not verts: return
    if cur is None: cur = dict(mv=mvaddr, proj=projaddr, v=vaddr, n=len(verts), faces=0, pts=[], tag=tag)
    M = mul(mv, proj)
    for i in idx:
        if i >= len(verts): continue
        v = verts[i]; c = xf([v[0], v[1], v[2], 1.0], M); w = c[3] if abs(c[3]) > 1e-6 else 1e-6
        cur['pts'].append((c[0]/w, c[1]/w, c[2]/w, w))
    cur['faces'] += 1
while steps < a.max:
    if pc + 8 > 0x800000: break
    if pc >= base + 0x108:
        nxt = W(base); steps += 1
        if valid_chunk(nxt): enter(nxt & 0xFFFFFF); continue
        if stack: pc, base = stack.pop(); continue
        break
    w0, w1 = W(pc), W(pc + 4); op = w0 >> 24; ln = 8
    if op == 0x06 and (w0 & 0x00FEFFFF) == 0:
        t = w1 & 0xFFFFFF; steps += 1
        if ((w0 >> 16) & 1) == 0: stack.append((pc + 8, base))
        enter(t); continue
    elif op == 0x07:
        t = w1 & 0xFFFFFF; steps += 1
        if t == 0 or t + 0x108 > 0x800000: break
        enter(t); continue
    elif op in (0xB5, 0x12):
        nxt = W(base); steps += 1
        if valid_chunk(nxt): enter(nxt & 0xFFFFFF); continue
        if stack: pc, base = stack.pop(); continue
        break
    elif op in (0xB8, 0x0F):
        steps += 1
        if stack: pc, base = stack.pop(); continue
        break
    elif op == 0x01:
        addr = w1 & 0xFFFFFF; b1 = (w0 >> 16) & 0xFF
        if addr + 64 <= 0x800000:
            m = mtx(addr)
            if b1 == 3 or (m[3][3] == 0 and m[2][3] != 0): proj = m; projaddr = w1
            else: mv = m; mvaddr = w1
            flush()
    elif op == 0x04:
        n = (w0 >> 10) & 0x3F; addr = w1 & 0xFFFFFF
        if addr + 8*n <= 0x800000:
            verts = [(H(addr + 8*i), H(addr + 8*i + 2), H(addr + 8*i + 4)) for i in range(n)]; vaddr = w1
        flush()
    elif op == 0x05: ln = 40 if ((w0 >> 16) & 0xFF) == 5 else 8
    elif op in (0xBE, 0xBD, 0x14, 0x09, 0x0A): ln = 16
    elif op == 0x03: ln = 24
    elif op in (0xBF, 0x08):
        face([((w1 >> 16) & 0xFF) // 5, ((w1 >> 8) & 0xFF) // 5, (w1 & 0xFF) // 5], 'tri'); ln = 32 if (w0 & 2) else 16
    elif op in (0xB4, 0x13):
        face([(w1 >> 24) // 5, ((w1 >> 16) & 0xFF) // 5, ((w1 >> 8) & 0xFF) // 5, (w1 & 0xFF) // 5], 'quad'); ln = 32 if (w0 & 2) else 16
    steps += 1; pc += ln
flush()
print('groups=%d (steps %d)' % (len(groups), steps))
for g in groups:
    p = g['pts']
    if not p: continue
    xs = [q[0] for q in p]; ys = [q[1] for q in p]; ws = [q[3] for q in p]
    print('  mv=%08X proj=%08X verts=%08X n=%2d %s faces=%3d  ndc x[%6.2f,%6.2f] y[%6.2f,%6.2f] w[%7.0f,%7.0f]' % (
        g['mv'], g['proj'], g['v'], g['n'], g['tag'], g['faces'], min(xs), max(xs), min(ys), max(ys), min(ws), max(ws)))
