#!/usr/bin/env python3
"""Diff a recomp RDRAM dump against an emulator (PJ64) golden dump.

Both inputs are raw 8 MB images in MIPS big-endian byte order:
  golden    = Project64 memory dump (e.g. dumps/MainMenu/output.bin)
  candidate = ROGUESQ_DUMP_RDRAM_ON_SCREEN=menu output (dumps/rdram_screen_N.bin)

Reports (1) known engine globals field-by-field, (2) differing 4 KB pages grouped
by region with the decomp symbols that fall inside them, (3) .text/.rodata
differences, which mean code/overlay corruption rather than game state.

  python tools/debug/validate/rdram_golden_diff.py dumps/MainMenu/output.bin dumps/rdram_screen_1.bin
  ... [--symbols E:/Projects/rogue_squadron64/symbol_files] [--top 40] [--page 4096]
      [--focus 0x80130B40:0x30] [--json out.json]
"""
import argparse
import glob
import json
import os
import re
import sys

RDRAM = 0x800000

# (name, address, size) from docs/reference/data-structures.md. Extend freely.
KNOWN_GLOBALS = [
    ("gMissionState",        0x80130B10, 0x28),
    ("gGameSettings",        0x80130B40, 0x30),
    ("cheatFlags",           0x80130B58, 0x08),
    ("gNpcSlotList..free",   0x80130BB0, 0x1C),
    ("gCurrentMenuData",     0x800CE730, 0xF8),
    ("gSaveDataBody",        0x8013A5C0, 0xB0),
    ("cineStatePtr",         0x800B0934, 0x08),
    ("gateCtr",              0x800B0B28, 0x04),
    ("gCurrentCutsceneFile", 0x800B1904, 0x04),
    ("viRetraceCount",       0x8011A890, 0x04),
    ("matCachePoolHeader",   0x80128EF4, 0x18),
    ("texTablePtrs",         0x80128EFC, 0x10),
    ("gActiveSlots",         0x80139560, 0x10),
    ("voiceSlotsPtr",        0x801496F8, 0x04),
    ("songSlotsPtr",         0x80149A00, 0x04),
]

# Coarse address classes (KSEG0 offsets). Framebuffers and heap are expected to
# differ between runs; main code and rodata are not. All three overlays (mission
# 0x665A0, menu 0x283F0, cinematic) load at 0x800A5130, so the overlay window only
# compares meaningfully when both images have the same overlay resident.
REGIONS = [
    ("main text",      0x000000, 0x03B000),
    ("main rodata",    0x03B000, 0x0A5130),
    ("overlay window", 0x0A5130, 0x10B6D0),
    ("main bss",       0x10B6D0, 0x160000),
    ("heap",           0x160000, 0x400000),
    ("framebuffers",   0x400000, 0x800000),
]
CODE_REGIONS = ('main text', 'main rodata')

SYM_RE = re.compile(r'^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*0x([0-9A-Fa-f]+)\s*;\s*(?://\s*(.*))?$')


def load_symbols(dirs):
    syms = []
    for d in dirs:
        for f in glob.glob(os.path.join(d, '*.txt')):
            if 'ignored' in os.path.basename(f):
                continue
            seg = os.path.basename(f)[:-4]
            for line in open(f, encoding='utf-8', errors='replace'):
                m = SYM_RE.match(line)
                if not m:
                    continue
                addr = int(m.group(2), 16)
                if not (0x80000000 <= addr < 0x80000000 + RDRAM):
                    continue
                meta = m.group(3) or ''
                sm = re.search(r'size:0x([0-9A-Fa-f]+)', meta)
                size = int(sm.group(1), 16) if sm else 0
                tm = re.search(r'type:(\S+)', meta)
                syms.append((addr, size, m.group(1), tm.group(1) if tm else '', seg))
    syms.sort()
    return syms


def sym_at(syms, addr):
    """Nearest symbol at or before addr: (name, offset, size, type, segment)."""
    lo, hi = 0, len(syms)
    while lo < hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= addr:
            lo = mid + 1
        else:
            hi = mid
    if lo == 0:
        return None
    a, size, name, typ, seg = syms[lo - 1]
    return (name, addr - a, size, typ, seg)


def read_image(path):
    d = open(path, 'rb').read()
    if len(d) != RDRAM:
        sys.exit(f"{path}: expected {RDRAM} bytes, got {len(d)}")
    # A BE image contains readable game strings; a word-swapped one does not.
    if d.find(b'LucasArts') < 0 and d.find(b'ROGUE') < 0:
        print(f"warning: {path}: no readable game strings; byte order may be wrong", file=sys.stderr)
    return d


OVERLAYS = [  # (name, ROM offset, size) from docs/reference/game-architecture.md; all load at 0x800A5130
    ("mission",   0x000A5D30, 0x000665A0),
    ("menu",      0x0010C2D0, 0x000283F0),
    ("cinematic", 0x00137580, 0x0000B810),
]
OVERLAY_VA = 0x800A5130


def detect_overlay(img, rom):
    """Name the overlay resident in the overlay window by matching its first 4 KB against the ROM."""
    if not rom:
        return 'unknown (no ROM)'
    win = img[OVERLAY_VA - 0x80000000: OVERLAY_VA - 0x80000000 + 0x1000]
    best = None
    for name, rom_off, size in OVERLAYS:
        src = rom[rom_off: rom_off + 0x1000]
        match = sum(1 for a, b in zip(win, src) if a == b)
        if best is None or match > best[1]:
            best = (name, match)
    return f"{best[0]} ({best[1] * 100 // 0x1000}% of first 4 KB matches ROM)"


def region_of(off):
    for name, lo, hi in REGIONS:
        if lo <= off < hi:
            return name
    return '?'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('golden')
    ap.add_argument('candidate')
    ap.add_argument('--symbols', action='append', default=[],
                    help='decomp symbol_files dir (repeatable); default ../rogue_squadron64/symbol_files if present')
    ap.add_argument('--top', type=int, default=40, help='differing pages to list')
    ap.add_argument('--page', type=int, default=4096)
    ap.add_argument('--focus', action='append', default=[], help='ADDR:SIZE (hex) extra field-level compare')
    ap.add_argument('--json', help='write machine-readable summary')
    ap.add_argument('--rom', help='ROM for overlay detection (default: repo-root rogue_squadron.z64)')
    a = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    if not a.symbols:
        cand = os.path.normpath(os.path.join(here, '..', '..', '..', '..', 'rogue_squadron64', 'symbol_files'))
        if os.path.isdir(cand):
            a.symbols = [cand]
    syms = load_symbols(a.symbols)
    g = read_image(a.golden)
    c = read_image(a.candidate)

    rom_path = a.rom or os.path.normpath(os.path.join(here, '..', '..', '..', 'rogue_squadron.z64'))
    rom = open(rom_path, 'rb').read() if os.path.isfile(rom_path) else None
    ov_g, ov_c = detect_overlay(g, rom), detect_overlay(c, rom)
    print(f"golden    resident overlay: {ov_g}")
    print(f"candidate resident overlay: {ov_c}")
    print("(globals in the overlay window only compare when both hold the same overlay)\n")

    out = {'golden': a.golden, 'candidate': a.candidate, 'globals': [], 'pages': [], 'regions': {},
           'overlay': {'golden': ov_g, 'candidate': ov_c}}

    # 1. Known globals, word by word.
    print("== Known globals (golden | candidate) ==")
    focus = list(KNOWN_GLOBALS)
    for f in a.focus:
        ad, sz = f.split(':')
        focus.append((f"focus_{ad}", int(ad, 16), int(sz, 16)))
    for name, addr, size in focus:
        off = addr - 0x80000000
        gb, cb = g[off:off + size], c[off:off + size]
        same = gb == cb
        out['globals'].append({'name': name, 'addr': addr, 'size': size, 'same': same})
        print(f"{'  ' if same else '!!'} {name:22s} @{addr:08X} {'same' if same else 'DIFF'}")
        if not same:
            for i in range(0, size, 4):
                gw, cw = gb[i:i + 4], cb[i:i + 4]
                if gw != cw:
                    print(f"       +{i:02X}: {gw.hex()} | {cw.hex()}")

    # 2. Page-level diff by region.
    print(f"\n== Differing {a.page}-byte pages by region ==")
    pages = []
    zero = bytes(a.page)
    # per region: [pages, bytes, golden-zero/cand-nonzero, golden-nonzero/cand-zero, both-nonzero]
    reg_tot = {r[0]: [0, 0, 0, 0, 0] for r in REGIONS}
    for off in range(0, RDRAM, a.page):
        gp, cp = g[off:off + a.page], c[off:off + a.page]
        if gp == cp:
            continue
        nd = sum(1 for x, y in zip(gp, cp) if x != y)
        pages.append((nd, off))
        r = reg_tot[region_of(off)]
        r[0] += 1
        r[1] += nd
        if gp == zero:
            r[2] += 1
        elif cp == zero:
            r[3] += 1
        else:
            r[4] += 1
    print("  region          pages differ      bytes   g=0/c!=0  g!=0/c=0  both!=0")
    for name, lo, hi in REGIONS:
        n, b, gz, cz, nz = reg_tot[name]
        tot = (hi - lo) // a.page
        print(f"  {name:14s} {n:5d}/{tot:5d}  {b:9d}   {gz:7d}   {cz:7d}   {nz:7d}")
        out['regions'][name] = {'pages': n, 'bytes': b, 'golden_zero': gz, 'cand_zero': cz, 'both_nonzero': nz}

    # 3. Worst pages outside framebuffers, with the symbols inside each.
    print(f"\n== Top {a.top} differing pages outside framebuffers (bytes, addr, region, symbols) ==")
    shown = 0
    for nd, off in sorted(pages, reverse=True):
        if region_of(off) == 'framebuffers':
            continue
        addr = 0x80000000 + off
        inside = [s for s in syms if addr <= s[0] < addr + a.page]
        names = ', '.join(s[2] for s in inside[:6]) + (' ...' if len(inside) > 6 else '')
        near = sym_at(syms, addr)
        if not inside and near:
            names = f"(in {near[0]}+0x{near[1]:X})"
        print(f"  {nd:5d}  {addr:08X}  {region_of(off):13s} {names}")
        out['pages'].append({'addr': addr, 'bytes': nd, 'region': region_of(off), 'symbols': [s[2] for s in inside]})
        shown += 1
        if shown >= a.top:
            break

    # 4. Code/rodata integrity: any diff here is corruption or a different overlay set.
    text_pages = [p for p in pages if region_of(p[1]) in CODE_REGIONS]
    print(f"\n== Code/rodata: {len(text_pages)} differing pages ==")
    for nd, off in sorted(text_pages, key=lambda p: p[1])[:20]:
        addr = 0x80000000 + off
        first = next(i for i in range(a.page) if g[off + i] != c[off + i])
        near = sym_at(syms, addr + first)
        where = f"{near[0]}+0x{near[1]:X}" if near else '?'
        print(f"  {addr + first:08X} ({where}): {g[off + first:off + first + 8].hex()} | "
              f"{c[off + first:off + first + 8].hex()}  [{nd} bytes in page]")

    if a.json:
        json.dump(out, open(a.json, 'w'), indent=1)
        print(f"\nwrote {a.json}")


if __name__ == '__main__':
    main()
