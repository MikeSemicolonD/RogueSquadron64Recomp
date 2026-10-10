#!/usr/bin/env python3
"""Build ../rogue_squadron64/build/roguesquadron.elf when the public decomp lags the names rogue_squadron.toml uses.

Function bounds come from syms/rogue_squadron.syms.toml, names from the decomp where the vram matches, and RENAMES for the rest (each pinned to an overlay by the instruction its hook or patch expects at that vram).
Needs rogue_squadron.z64 in the decomp's root: python tools/make_elf_from_syms.py
"""
import os, sys, tomllib, importlib.util
from pathlib import Path

RECOMP = Path(__file__).resolve().parent.parent
DECOMP = RECOMP.parent / "rogue_squadron64"
DECOMP_ELF = "build/roguesquadron.decomp.elf"

spec = importlib.util.spec_from_file_location("mk", DECOMP / "tools/make_elf.py")
mk = importlib.util.module_from_spec(spec); spec.loader.exec_module(mk)

# (section, name) -> vram of a vram inside the target function
RENAMES = {
    ".ovl.mission": {
        "computeOrientedTransformFromQuat": 0x800B10DC, "findClosestRadarContactAndSetSweep": 0x800C55E0,
        "findNearestSceneObjectToPoint": 0x800AFD4C, "loadPlayerStartPosition": 0x800AFF8C,
        "onPlayerCraftGroundCrash": 0x800B02DC, "updatePlayerCraftPhysics": 0x800B2E48,
        "updatePlayerDeathSpiral": 0x800B0D14, "updateProbeDroidHopState": 0x800CAB38,
        "updateXwingFlightControls": 0x800B48EC, "updateAwingFlightControls": 0x800B5D54,
        "updateYwingFlightControls": 0x800B7984, "updateVwingFlightControls": 0x800B9280,
        "updateFalconFlightControls": 0x800BA8F4, "updateTieInterceptorFlightControls": 0x800BC190,
        "updateT16FlightControls": 0x800BD148, "updateNabooStarfighterFlightControls": 0x800BDEB0,
        "updateSnowSpeederFlightControls": 0x800ABDA0,
        "runPauseMenuStateMachine": 0x800C3550, "initOrUpdatePauseScreenDim": 0x800C2458,
        "tickSongFadeTimer": 0x800EE82C, "requestSpeechResponseMode2": 0x800FBB08,
        "requestSpeechResponseMode1": 0x800FBB64, "endMissionCleanup": 0x800FB9E4,
        "requestMissionTransitionMode": 0x800FB190, "setHudEnableBit4": 0x800C7738,
        "setHudEnableBit8": 0x800C776C, "drawRadarIcon": 0x800C5E84, "renderRadarMinimap": 0x800C6290,
    },
    ".ovl.menu": {
        "runMenuModelViewerScreen": 0x800C29F8, "tickFormatMessageWorker": 0x800B380C,
        "runMenuShipSelectScreen": 0x800C14A8, "setupMenuData": 0x800BAF70,
        "menuControllerInput": 0x800B51B8, "menuOverlayInit": 0x800C593C,
        "updateMenuPerFrame": 0x800BB394, "initCraftSelectScreen": 0x800AF0D0,
        "tickCraftSelectScreen": 0x800AF3EC, "hangarVehicleDisplayNpcHandler": 0x800C6D90, "runHangarSelectionFrame": 0x800AADD8,
    },
    ".ovl.cinematic": {"cinematicLoopBody": 0x800A6024, "playCutsceneObjectImpactSound": 0x800AEFAC,
                       "updateCinematicCameraStateMachine": 0x800ADA48},
}

def read_elf_syms(path):
    """(name, vram, size, type, shndx) for every symbol in a make_elf.py ELF."""
    import struct
    d = open(path, "rb").read()
    sh = struct.unpack(">I", d[32:36])[0]; n = struct.unpack(">H", d[48:50])[0]
    secs = [struct.unpack(">IIIIIIIIII", d[sh + i * 40: sh + i * 40 + 40]) for i in range(n)]
    st = next(x for x in secs if x[1] == 2); strt = secs[st[6]]
    out = []
    for o in range(st[4] + 16, st[4] + st[5], 16):
        nm, v, sz, info, _, shn = struct.unpack(">IIIBBH", d[o:o + 16])
        out.append((d[strt[4] + nm: d.index(b"\0", strt[4] + nm)].decode(), v, sz, info & 15, shn))
    return out

def main():
    syms = tomllib.load(open(RECOMP / "syms/rogue_squadron.syms.toml", "rb"))
    secidx = {s[3]: i + 1 for i, s in enumerate(mk.SECTIONS)}
    # syms.toml already contains the config's manual_funcs; N64Recomp rejects them as duplicates.
    cfg = tomllib.load(open(RECOMP / "rogue_squadron.toml", "rb"))
    manual = {m["name"] for m in cfg["input"].get("manual_funcs", [])}
    # Name functions after the decomp where one starts at the same vram; keep its data symbols.
    os.chdir(DECOMP)
    out = mk.OUT
    mk.OUT = DECOMP_ELF
    mk.main()  # plain decomp ELF, for its names and data symbols
    mk.OUT = out
    old = read_elf_syms(DECOMP / DECOMP_ELF)
    referenced = set(config_vrams(cfg))
    old_func = {(shn, v): n for n, v, sz, ty, shn in old if ty == mk.STT_FUNC}
    resolved = [(n, v, False, sz, shn) for n, v, sz, ty, shn in old if ty != mk.STT_FUNC]
    for sec in syms["section"]:
        shndx = secidx[sec["name"]]
        _, base, size, _ = mk.SECTIONS[shndx - 1]
        funcs = sec.get("functions", [])
        for new, v in RENAMES.get(sec["name"], {}).items():
            hit = [f for f in funcs if f["vram"] <= v < f["vram"] + f["size"]]
            if len(hit) != 1: sys.exit(f"{new}: {len(hit)} candidates at {v:#x} in {sec['name']}")
            print(f"  {sec['name']:15} {hit[0]['name']:28} -> {new}")
            hit[0]["name"] = new
        for f in funcs:
            # make_elf.py hardcodes the entry symbols (func_80000400/func_8000040C).
            if sec["name"] != ".entry" and f["name"] not in manual and base <= f["vram"] < base + size:
                name = f["name"]
                if name.startswith("func_") and name not in referenced:
                    name = old_func.get((shndx, f["vram"]), name)
                resolved.append((name, f["vram"], True, f["size"], shndx))
    # RSP microcode blobs live in .text; the config stubs them, which needs a function symbol.
    for n, v, sz, ty, shn in old:
        if n in referenced and ty != mk.STT_FUNC and n not in {r[0] for r in resolved if r[2]}:
            resolved = [r for r in resolved if r[0] != n]
            resolved.append((n, v, True, sz, shn))
    resolve_missing(cfg, old, syms, resolved)
    funcs = {r[0] for r in resolved if r[2]}
    resolved = [r for r in resolved if r[2] or r[0] not in funcs]
    names = [r[0] for r in resolved]
    dup = {n for n in names if names.count(n) > 1}
    if dup: sys.exit(f"duplicate names: {sorted(dup)[:10]}")
    resolved.sort(key=lambda r: (r[4], r[1]))
    write(resolved)

def config_vrams(cfg):
    """name -> set of vrams the config patches inside it, and every name it references."""
    out = {}
    for kind in ("hook", "instruction"):
        for h in cfg["patches"].get(kind, []):
            v = h.get("before_vram", h.get("vram"))
            out.setdefault(h["func"], set()).update([v] if v else [])
    for f in cfg["input"].get("patch_files", []):
        pt = tomllib.load(open(RECOMP / f, "rb"))
        for h in pt.get("patches", {}).get("instruction", []) + pt.get("patches", {}).get("hook", []):
            v = h.get("vram", h.get("before_vram"))
            out.setdefault(h["func"], set()).update([v] if v else [])
    for n in cfg["patches"].get("stubs", []):
        out.setdefault(n, set())
    for x in cfg["input"].get("function_sizes", []):
        out.setdefault(x["name"], set())
    return out

def resolve_missing(cfg, old, syms, resolved):
    """Name the syms.toml functions the config references but neither source names."""
    have = {r[0] for r in resolved if r[2]}
    idx = {id(r): i for i, r in enumerate(resolved)}
    funcs = [i for i, r in enumerate(resolved) if r[2]]
    def containing(shn, v):
        return [i for i in funcs if resolved[i][4] == shn and resolved[i][1] <= v < resolved[i][1] + resolved[i][3]]
    old_by_name = {n: (v, shn) for n, v, sz, ty, shn in old if ty == mk.STT_FUNC}
    unresolved = []
    for name, vrams in sorted(config_vrams(cfg).items()):
        if name in have:
            continue
        hit = []
        if name in old_by_name:
            v, shn = old_by_name[name]
            hit = containing(shn, v)
        if not hit and vrams:
            cands = set()
            for shn in range(2, len(mk.SECTIONS) + 1):
                sets = [set(containing(shn, v)) for v in vrams]
                common = set.intersection(*sets) if sets else set()
                cands |= {i for i in common if resolved[i][0].startswith("func_")}
            hit = sorted(cands)
        if len(hit) == 1 and resolved[hit[0]][0].startswith("func_"):
            r = resolved[hit[0]]
            resolved[hit[0]] = (name,) + r[1:]
            have.add(name)
        elif name in old_by_name and not hit:
            v, shn = old_by_name[name]  # no syms.toml function there: keep the decomp's
            sz = next(sz for n, vv, sz, ty, s in old if n == name)
            resolved.append((name, v, True, sz, shn)); have.add(name)
        else:
            unresolved.append((name, [hex(v) for v in sorted(vrams)],
                               [(mk.SECTIONS[resolved[i][4]-1][3], resolved[i][0]) for i in hit]))
    for u in unresolved:
        print("UNRESOLVED", *u)

def write(resolved):
    # Reuse make_elf's writer by temporarily swapping in our symbol list.
    def src(i):
        return mk.SECTIONS[i - 1][3] if 0 < i <= len(mk.SECTIONS) else ""
    mk.parse_symbols = lambda files: [(n, v, f, s, src(i)) for n, v, f, s, i in resolved]
    mk.parse_asm_labels = lambda *a, **k: []
    orig = mk.vram_to_shndx
    mk.vram_to_shndx = lambda vram, src: next((i + 1 for i, s in enumerate(mk.SECTIONS) if s[3] == src), None) or orig(vram, src) or (mk.SHN_ABS if src == "" else 0)
    mk.main()

if __name__ == "__main__":
    main()
