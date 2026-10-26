# rizin queries against the game ELF

`rzq.py` wraps [rizin](https://github.com/rizinorg/rizin) (installed via
`winget install Rizin.Rizin`, plus `pip install rzpipe`) to answer the questions
the grep-based tools in `tools/re/rename/` can't: register-relative data accesses,
call graphs, and string references, all resolved against the named symbols.

Input is the symbolized ELF the sister decomp repo builds
(`E:/Projects/rogue_squadron64/build/roguesquadron.elf`, from `tools/make_elf.py`).
Rebuild that ELF after adding names to `symbol_files/` or rizin won't see them.

```
python tools/re/rz/rzq.py xrefs 0x800B0B28            # who reads/writes a global
python tools/re/rz/rzq.py xrefs setupCameraMatrices   # callers
python tools/re/rz/rzq.py callees processSceneNode    # calls + named globals touched
python tools/re/rz/rzq.py disasm cinematicLoopBody
python tools/re/rz/rzq.py strrefs "LucasArts"         # strings + who references them
python tools/re/rz/rzq.py funcs "^sym.runMenu"
python tools/re/rz/rzq.py raw "pd 20 @ 0x80015548"    # any rizin command
```

## Overlays

The mission, menu and cinematic overlays all load at 0x800A5130, so only one
can be mapped at a time. The ELF order puts cinematic on top; pass
`--overlay mission` or `--overlay menu` to prioritize another (`omp <map id>`
in rizin terms). Main-segment code (< 0x800A5130) is unaffected.

## Analysis cache

Full analysis (`aaa`) takes ~25 s. `--project <file.rzdb>` saves it on first
run and reloads it afterwards (~10 s). Use one project file per overlay.

## Not available

The Ghidra decompiler plugin (rz-ghidra) ships source-only for Windows; the
recomp's own `RecompiledFuncs/funcs_*.c` and the sister repo's m2c output remain
the decompilation sources.
