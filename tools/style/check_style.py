"""Style checks for src/main from AGENTS.md: no comment after code, comment blocks of at most 2 lines, env reads through recomp::dbg::env_*, one statement per line.

Usage: python tools/style/check_style.py [--update-baseline]. Exits 1 on findings not in tools/style/baseline.txt.
"""
import pathlib, re, sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
BASELINE = ROOT / 'tools' / 'style' / 'baseline.txt'
FILES = sorted(list((ROOT / 'src' / 'main').glob('*.cpp')) + list((ROOT / 'src' / 'main').glob('*.h')))
SIDE = re.compile(r'[;{)]\s+//')
GETENV = re.compile(r'(?<![:\w])getenv\(')
TWO = re.compile(r';\s+(if|while)\s*\(')

def findings():
    out = []
    for f in FILES:
        rel = f.relative_to(ROOT).as_posix()
        run = 0
        for n, line in enumerate(f.read_text(encoding='utf-8', errors='replace').splitlines(), 1):
            s = line.strip()
            if s.startswith('//'):
                run += 1
                if run == 1:
                    first = s
                if run == 3:
                    out.append(f'{rel}: comment block over 2 lines: {first[:60]}')
                continue
            run = 0
            code = line.split('"')[0] if line.count('"') % 2 == 0 else line
            if SIDE.search(code):
                out.append(f'{rel}: comment after code: {s[:60]}')
            if GETENV.search(code) and 'env_' not in code:
                out.append(f'{rel}: getenv outside env_* helpers: {s[:60]}')
            if TWO.search(code) and not s.startswith('for'):
                out.append(f'{rel}: two statements on one line: {s[:60]}')
    return out

def main():
    cur = findings()
    if '--update-baseline' in sys.argv:
        with open(BASELINE, 'w', encoding='utf-8', newline='\n') as fh:
            fh.write('\n'.join(sorted(set(cur))) + '\n')
        print(f'baseline: {len(set(cur))} findings')
        return 0
    base = set(BASELINE.read_text(encoding='utf-8').splitlines()) if BASELINE.exists() else set()
    new = sorted(set(cur) - base)
    for x in new:
        print(x)
    print(f'style: {len(new)} new, {len(set(cur) & base)} baselined')
    return 1 if new else 0

if __name__ == '__main__':
    sys.exit(main())
