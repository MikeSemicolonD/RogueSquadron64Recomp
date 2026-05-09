"""
Generate a full funcs.h from the syms toml. Modern clang errors on the
'previous implicit declaration here' / 'conflicting types' pair that arises
when N64Recomp's terse funcs.h leaves declarations to be inferred from
calls. We work around by emitting an explicit declaration for every
function in the syms toml before any call site sees it.

Usage:
    python tools/gen_funcs_h.py <syms.toml> <output_funcs.h>

Default invocation re-generates E:/Projects/N64Recomp/RecompiledFuncs/funcs.h
from syms/rogue_squadron.syms.toml.
"""
import re
import sys
from pathlib import Path

def parse_syms(path):
    """Yield function names from N64Recomp's syms toml output."""
    text = Path(path).read_text(encoding='utf-8')
    # The syms toml format inside [[section]] blocks contains lines like
    #     { name = "func_80007D74", vram = 0x80007D74, size = 0xE8 },
    # We just want the names.
    for m in re.finditer(r'name\s*=\s*"([^"]+)"', text):
        name = m.group(1)
        # Skip the section names (they sit on their own line outside the
        # functions = [...] array). Heuristic: section names start with '.'
        # or have no func_/__ prefix and no underscores in unusual places.
        if name.startswith('.'):
            continue
        yield name

def main():
    args = sys.argv[1:]
    if len(args) >= 2:
        syms_path = args[0]
        out_path = args[1]
    else:
        repo = Path(__file__).resolve().parent.parent
        syms_path = repo / 'syms' / 'rogue_squadron.syms.toml'
        out_path = Path('E:/Projects/N64Recomp/RecompiledFuncs/funcs.h')

    names = sorted(set(parse_syms(syms_path)))

    out = ['#ifndef __FUNCS_H__\n', '#define __FUNCS_H__\n', '\n',
           '#include "recomp.h"\n', '\n',
           '#ifdef __cplusplus\n', 'extern "C" {\n', '#endif\n', '\n']
    for n in names:
        out.append(f'void {n}(uint8_t* rdram, recomp_context* ctx);\n')
    out += ['\n', '#ifdef __cplusplus\n', '}\n', '#endif\n', '\n',
            '#endif\n']
    Path(out_path).write_text(''.join(out), encoding='utf-8')
    print(f'Wrote {len(names)} declarations to {out_path}')

if __name__ == '__main__':
    main()
