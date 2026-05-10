"""Post-process RSPRecomp output for the Factor 5 graphics ucode.

Three fixups, all idempotent. Pass either the main ucode file or the boot
file — fixups self-gate by detecting which file is being processed.

1. Replace `goto L_XXXX;` referring to undefined labels with a clean exit.
   RSPRecomp emits these for relative branches whose targets fall outside
   the recompiled function's PC range. Factor 5's main ucode has one such
   case: a `bgez $2, +0xC` near end-of-IMEM in a perspective-divide block
   whose target wraps past 0x04002000.

1b. Silence the boot ucode's expected `jr $7=0x1080` printf (boot file only).
    factor5_boot ends by DMA'ing the main ucode to IMEM[0x1080] then doing
    `jr $7` to hand off. RSPRecomp can't model the inter-ucode jump, so it
    falls through to do_indirect_jump's default and prints a 4-line register
    dump every task. The runner already treats UnhandledJumpTarget as
    expected (see factor5_gfx_runner in src/main/main.cpp), so we gate the
    printf to only fire on *unexpected* targets.

2. Inject a per-task iteration cap at L_1090 (main ucode only).
   RSPRecomp lowers `mfc0 $X, SP_STATUS` to literal `rN = 0;` since the RSP
   coprocessor 0 isn't modeled. That makes the dispatch loop's halt-bit
   check (line in L_1090: `if (r2 != 0) goto L_11A8;`) dead — r2 is always
   0. The only natural exit (return Broke at L_10E8) is unreachable.
   Without a synthetic halt the recompile spins forever until it hits an
   unknown jump-target and returns UnhandledJumpTarget. The 16384-iter
   cap is high enough to handle any real DL chunk we've observed (cinematic
   tasks ~6/sec process << 16k commands each) and bounds the runaway case.

Usage: python fixup_factor5_ucode.py <recompiled_c_file>
"""
import sys
import re
from pathlib import Path

if len(sys.argv) != 2:
    print(f"usage: {sys.argv[0]} <recompiled_c_file>", file=sys.stderr)
    sys.exit(1)

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
original = text

# ---------------------------------------------------------------------------
# Fixup 1: undefined goto targets → clean exit
# ---------------------------------------------------------------------------
defined_labels = set(re.findall(r"^([A-Za-z_][A-Za-z_0-9]*):\s*$", text, flags=re.MULTILINE))
goto_targets = set(re.findall(r"goto\s+([A-Za-z_][A-Za-z_0-9]*);", text))
undefined = sorted(goto_targets - defined_labels)

for label in undefined:
    pattern = re.compile(r"(\s*)goto\s+" + re.escape(label) + r";")
    replacement = (
        r"\1// goto " + label + " — out-of-IMEM target, never taken in practice;\n"
        r"\1return RspExitReason::ImemOverrun;"
    )
    text = pattern.sub(replacement, text)

# ---------------------------------------------------------------------------
# Fixup 1b: silence boot's expected `jr $7=0x1080` exit
# ---------------------------------------------------------------------------
# factor5_boot ends by DMA'ing the main ucode to IMEM[0x1080] and then doing
# `jr $7` where r7 = 0x1080 to hand off to it. RSPRecomp can't model that
# inter-ucode jump, so it falls through to the do_indirect_jump default and
# prints a 4-line "Unhandled jump target" register dump. The runner already
# treats UnhandledJumpTarget from boot as expected (see factor5_gfx_runner in
# src/main/main.cpp), so the printf is pure spam — and at ~3 tasks/sec it
# floods stderr enough to starve the console pump (the same class of issue
# documented in project_factor5_lle_breakthrough.md).
#
# Gate the printf so it only fires for *unexpected* jump targets.
SILENCE_MARKER = "/* fixup: silence expected boot->main JR */"
if "factor5_boot(" in text and SILENCE_MARKER not in text:
    boot_printf_pattern = re.compile(
        r'(\n\s*)printf\("Unhandled jump target 0x%04X in microcode factor5_boot,',
        re.MULTILINE)
    if boot_printf_pattern.search(text):
        text = boot_printf_pattern.sub(
            r"\1if (jump_target != 0x1080) {  " + SILENCE_MARKER + r"\1"
            r'    printf("Unhandled jump target 0x%04X in microcode factor5_boot,',
            text, count=1)
        # Close the if-block right before `return RspExitReason::UnhandledJumpTarget;`.
        text = text.replace(
            "    return RspExitReason::UnhandledJumpTarget;\n}\n",
            "    }\n    return RspExitReason::UnhandledJumpTarget;\n}\n",
            1)

# ---------------------------------------------------------------------------
# Fixup 2: inject iter cap at L_1090 (main ucode only)
# ---------------------------------------------------------------------------
ITER_MARKER = "/* fixup: iter cap */"
R18_MARKER  = "/* fixup: r18 = 0x100 (ucode-state base in DMEM) */"
is_main_ucode = "factor5_ucode(" in text and "factor5_boot(" not in text
if is_main_ucode and ITER_MARKER not in text:
    # Insert a counter + r18 init at function entry (right after the local
    # var decls). Pattern: locals block ends with `RSP rsp{};`. Insert after.
    #
    # r18 init: Factor 5's main ucode treats $18 as the ucode-state base
    # address (DMEM 0x100), set by L_1DB0's bootstrap routine. RSPRecomp
    # doesn't preserve register state across the boot→main split (each
    # function starts with $r0..$r31 = 0). L_1DB0 normally bootstraps $18
    # via an indirect-jump dispatch, but if that dispatch never fires (or
    # fires only for the first task), $18 stays at 0 and reads from
    # DMEM[0x30] etc. land on unrelated static data.
    entry_pattern = re.compile(r"(\n    RSP rsp\{\};\n)")
    if entry_pattern.search(text):
        text = entry_pattern.sub(
            r"\1    int rs64_iter = 0;  " + ITER_MARKER + "\n"
            r"    r18 = 0x100;  " + R18_MARKER + "\n",
            text, count=1)

    # Insert the cap check right after the L_1090 label line.
    # Format: `L_1090:` followed by a comment then the actual instruction.
    # Also log first hit per process via a static counter so we know if the
    # cap is actually firing and at what task index.
    l1090_pattern = re.compile(r"(L_1090:\n)")
    if l1090_pattern.search(text):
        replacement = (
            "L_1090:\n"
            "    if (++rs64_iter > 16384) {  " + ITER_MARKER + "\n"
            "        static int rs64_cap_fires = 0;\n"
            "        if (++rs64_cap_fires <= 4 || (rs64_cap_fires & 63) == 0) {\n"
            '            fprintf(stderr, "[ucode-cap] fired #%d, last r17=0x%X\\n", rs64_cap_fires, r17);\n'
            "            fflush(stderr);\n"
            "        }\n"
            "        return RspExitReason::Broke;\n"
            "    }\n"
        )
        text = l1090_pattern.sub(lambda m: replacement, text, count=1)

# ---------------------------------------------------------------------------

if text == original:
    sys.exit(0)

path.write_text(text, encoding="utf-8")
notes = []
if undefined:
    notes.append(f"replaced {len(undefined)} undefined goto target(s): {', '.join(undefined)}")
if ITER_MARKER in text and ITER_MARKER not in original:
    notes.append("injected iter cap at L_1090")
if SILENCE_MARKER in text and SILENCE_MARKER not in original:
    notes.append("silenced boot->main JR printf")

# Make sure <cstdio> is available for the iter-cap fprintf.
if "#include <cstdio>" not in text:
    text = text.replace('#include "librecomp/rsp.hpp"',
                        '#include <cstdio>\n#include "librecomp/rsp.hpp"', 1)
    path.write_text(text, encoding="utf-8")
print(f"fixup_factor5_ucode: {path} — " + "; ".join(notes), file=sys.stderr)
