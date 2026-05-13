"""Find callers of a given recompiled-C function.

Usage:
    python tools/rename/find_callers.py <target_name>
    python tools/rename/find_callers.py func_80056350
    python tools/rename/find_callers.py initMission

Prints one caller name per line, alphabetically.

Override the recompiled-C directory with RSR_FUNCS_DIR if needed.
"""
import os, re, glob, sys

FUNCS_DIR = os.environ.get(
    "RSR_FUNCS_DIR",
    "E:/Projects/N64Recomp/RecompiledFuncs",
)

if len(sys.argv) < 2:
    print("Usage: find_callers.py <target_function_name>", file=sys.stderr)
    sys.exit(1)

target = sys.argv[1]
func_def = re.compile(r"^RECOMP_FUNC void (\w+)\(uint8_t\* rdram, recomp_context\* ctx\) \{", re.M)
call_re = re.compile(rf"\s+{re.escape(target)}\(rdram, ctx\);")

callers = set()
for cfile in sorted(glob.glob(os.path.join(FUNCS_DIR, "funcs_*.c"))):
    with open(cfile, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    matches = list(func_def.finditer(text))
    for i, m in enumerate(matches):
        start = m.end()
        end = matches[i+1].start() if i+1 < len(matches) else len(text)
        body = text[start:end]
        if call_re.search(body):
            callers.add(m.group(1))

for c in sorted(callers):
    print(c)
