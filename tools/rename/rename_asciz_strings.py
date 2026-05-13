"""Bulk-rename `D_HHHHHHHH = ...; // type:asciz` entries in each overlay's
symbol_file using the actual asciz contents from splat's .rodata.s output.

For each unnamed D_ asciz entry:
  - Look up the literal string from the rodata sources.
  - Convert to a camel-case identifier ("imp_stuff/foo_bar" -> "ImpStuffFooBar").
  - Drop a leading '+' or '-' (cheat-code prefix marker).
  - Skip if too short (<3 chars), too long (>40), or non-printable.
  - Prefix per overlay: strMiss / strMenu / strCin / (main_overlay: bare "str").
  - Skip if the chosen name is already used anywhere (collision guard).

Operates in-place on the symbol files. Designed to be idempotent — already-
renamed entries are skipped because they no longer start with `D_`.

Usage:
    python tools/rename/rename_asciz_strings.py
"""
import re, glob, os

SYM_DIR = os.environ.get(
    "RSR_SYM_DIR",
    "E:/Projects/rogue_squadron64/symbol_files",
)
RODATA_ROOT = os.environ.get(
    "RSR_ASM_DATA_DIR",
    "E:/Projects/rogue_squadron64/asm/data",
)

asciz_re = re.compile(r'^\s+/\* [\w]+ ([0-9A-F]{8}) \*/\s+\.asciz "(.*)"$')
ALLOWED_CHARS = re.compile(r"^[\x20-\x7E]+$")  # printable ASCII


def to_camel(s):
    s = s.lstrip("+").lstrip("-")
    s = re.sub(r"[^A-Za-z0-9]+", " ", s).strip()
    parts = s.split()
    return "".join(p[:1].upper() + p[1:] for p in parts if p)


# 1. Names already in use across all symbol files (collision guard)
all_used = set()
for sf in glob.glob(os.path.join(SYM_DIR, "*.txt")):
    if "ignored" in sf:
        continue
    with open(sf, "r", encoding="utf-8") as f:
        for line in f:
            m = re.match(r"\s*([A-Za-z_][A-Za-z0-9_]*)\s*=", line)
            if m:
                all_used.add(m.group(1))

# 2. Index all asciz strings from rodata
strings = {}
for sf in glob.glob(os.path.join(RODATA_ROOT, "**/*.rodata.s"), recursive=True):
    with open(sf, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = asciz_re.match(line)
            if m:
                strings[m.group(1)] = m.group(2)

# 3. Per-overlay rewrite
OVERLAYS = [
    ("main_overlay", ""),
    ("mission_overlay", "Miss"),
    ("menu_overlay", "Menu"),
    ("cinematic_overlay", "Cin"),
]

for ov_name, prefix in OVERLAYS:
    sym_path = os.path.join(SYM_DIR, f"{ov_name}.txt")
    if not os.path.exists(sym_path):
        continue
    with open(sym_path, "r", encoding="utf-8") as f:
        lines = f.readlines()
    new_lines = []
    renamed = 0
    for line in lines:
        m = re.match(r"^(D_)([0-9A-F]{8})(\s*=\s*0x[0-9A-Fa-f]+;\s*//\s*type:asciz.*)$", line)
        if m:
            addr = m.group(2)
            s = strings.get(addr)
            if s and 3 <= len(s) <= 40 and ALLOWED_CHARS.match(s):
                camel = to_camel(s)
                if camel and 3 <= len(camel) <= 24:
                    new_name = f"str{prefix}{camel}"
                    if new_name not in all_used:
                        all_used.add(new_name)
                        rest = m.group(3).split(";", 1)[1]
                        new_line = f"{new_name:30s}= 0x{addr};{rest} - {s!r}\n"
                        if not new_line.endswith("\n"):
                            new_line += "\n"
                        new_lines.append(new_line)
                        renamed += 1
                        continue
        new_lines.append(line)
    with open(sym_path, "w", encoding="utf-8") as f:
        f.writelines(new_lines)
    print(f"{ov_name}: renamed {renamed} asciz entries")
