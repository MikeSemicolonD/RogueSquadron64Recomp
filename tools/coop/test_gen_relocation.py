import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from gen_relocation import parse_funcs, analyze_func, IDX, LOOP


def src(lines, name="f"):
    body = []
    for l in lines:
        body.append(l if l.startswith("L_") else "    // " + l)
    return "RECOMP_FUNC void %s(uint8_t* rdram, recomp_context* ctx) {\n%s\n}\n" % (name, "\n".join(body))


def uses_by_vram(lines, name="f"):
    fn = parse_funcs(src(lines, name))[0]
    return {s.vram: u for s, u in analyze_func(fn).items()}


def addrs(u):
    return sorted({a for _, a, _ in u if a is not None})


def test_parse_dedupes_delay_slots():
    text = src([
        "0x80001000: beq         $a0, $zero, L_80001010",
        "0x80001004: addiu       $v0, $zero, 0x4",
        "0x80001004: addiu       $v0, $zero, 0x4",
        "0x80001008: jr          $ra",
        "0x8000100C: nop",
        "L_80001010:",
        "0x80001010: jr          $ra",
        "0x80001014: nop",
    ])
    fn = parse_funcs(text)[0]
    assert fn.name == "f"
    assert sorted(fn.insns) == [0x80001000, 0x80001004, 0x80001008, 0x8000100C, 0x80001010, 0x80001014]
    assert fn.labels == {0x80001010}
    assert fn.insns[0x80001004].args == ["$v0", "$zero", "0x4"]


def test_direct_load_records_address():
    u = uses_by_vram([
        "0x80001000: lui         $v0, 0x8014",
        "0x80001004: lw          $v0, -0x7FA0($v0)",
        "0x80001008: jr          $ra",
        "0x8000100C: nop",
    ])
    assert addrs(u[0x80001000]) == [0x80138060]


def test_addiu_then_offset():
    u = uses_by_vram([
        "0x80001000: lui         $a0, 0x8014",
        "0x80001004: addiu       $a0, $a0, -0x7D98",
        "0x80001008: lwc1        $f0, 0x10($a0)",
        "0x8000100C: jr          $ra",
        "0x80001010: nop",
    ])
    assert addrs(u[0x80001000]) == [0x80138278]


def test_ori_forms_address():
    u = uses_by_vram([
        "0x80001000: lui         $v0, 0x8013",
        "0x80001004: ori         $v0, $v0, 0x8060",
        "0x80001008: lw          $v1, 0x0($v0)",
        "0x8000100C: jr          $ra",
        "0x80001010: nop",
    ])
    assert addrs(u[0x80001000]) == [0x80138060]


def test_indexed_add_keeps_base_and_flags_it():
    u = uses_by_vram([
        "0x80001000: lui         $v0, 0x8014",
        "0x80001004: addu        $v0, $v0, $t0",
        "0x80001008: lw          $v1, -0x7FA0($v0)",
        "0x8000100C: jr          $ra",
        "0x80001010: nop",
    ])
    assert ("mem", 0x80138060, IDX) in u[0x80001000]


def test_lui_shared_across_branch():
    u = uses_by_vram([
        "0x80001000: lui         $s0, 0x8014",
        "0x80001004: beq         $a0, $zero, L_80001014",
        "0x80001008: nop",
        "0x8000100C: lw          $v1, -0x7FA0($s0)",
        "0x80001010: nop",
        "L_80001014:",
        "0x80001014: sw          $v1, -0x7F9C($s0)",
        "0x80001018: jr          $ra",
        "0x8000101C: nop",
    ])
    assert addrs(u[0x80001000]) == [0x80138060, 0x80138064]


def test_two_luis_reaching_one_use():
    u = uses_by_vram([
        "0x80001000: beq         $a0, $zero, L_80001010",
        "0x80001004: lui         $t0, 0x8014",
        "0x80001008: lui         $t0, 0x8014",
        "0x8000100C: nop",
        "L_80001010:",
        "0x80001010: lw          $v1, -0x7FA0($t0)",
        "0x80001014: jr          $ra",
        "0x80001018: nop",
    ])
    assert addrs(u[0x80001004]) == [0x80138060]
    assert addrs(u[0x80001008]) == [0x80138060]


def test_call_clobbers_caller_saved_not_saved():
    u = uses_by_vram([
        "0x80001000: lui         $v0, 0x8014",
        "0x80001004: lui         $s0, 0x8014",
        "0x80001008: jal         0x80002000",
        "0x8000100C: nop",
        "0x80001010: lw          $v1, -0x7FA0($v0)",
        "0x80001014: lw          $v1, -0x7FA0($s0)",
        "0x80001018: jr          $ra",
        "0x8000101C: nop",
    ])
    assert u[0x80001000] == set()
    assert addrs(u[0x80001004]) == [0x80138060]


def test_delay_slot_reaches_target():
    u = uses_by_vram([
        "0x80001000: beq         $a0, $zero, L_80001010",
        "0x80001004: lui         $t0, 0x8014",
        "0x80001008: jr          $ra",
        "0x8000100C: nop",
        "L_80001010:",
        "0x80001010: lw          $v1, -0x7FA0($t0)",
        "0x80001014: jr          $ra",
        "0x80001018: nop",
    ])
    assert addrs(u[0x80001004]) == [0x80138060]


def test_likely_delay_slot_only_on_taken():
    u = uses_by_vram([
        "0x80001000: lui         $t0, 0x8014",
        "0x80001004: addiu       $t0, $t0, -0x7760",
        "0x80001008: beql        $a0, $zero, L_80001018",
        "0x8000100C: lui         $t0, 0x8014",
        "0x80001010: lw          $v1, 0x0($t0)",
        "0x80001014: nop",
        "L_80001018:",
        "0x80001018: lw          $v1, -0x7FA0($t0)",
        "0x8000101C: jr          $ra",
        "0x80001020: nop",
    ])
    assert addrs(u[0x8000100C]) == [0x80138060]
    assert addrs(u[0x80001000]) == [0x80130900, 0x801388A0]


def test_jump_table_jr_reaches_labels():
    u = uses_by_vram([
        "0x80001000: lui         $s0, 0x8014",
        "0x80001004: jr          $v0",
        "0x80001008: nop",
        "L_8000100C:",
        "0x8000100C: lw          $v1, -0x7FA0($s0)",
        "0x80001010: jr          $ra",
        "0x80001014: nop",
    ])
    assert addrs(u[0x80001000]) == [0x80138060]


def test_escape_records_pointer_value():
    u = uses_by_vram([
        "0x80001000: lui         $a0, 0x8014",
        "0x80001004: jal         0x80002000",
        "0x80001008: addiu       $a0, $a0, -0x7FA8",
        "0x8000100C: jr          $ra",
        "0x80001010: nop",
    ])
    assert ("escape", 0x80138058, 0) in u[0x80001000]


def test_untracked_hi_is_ignored():
    u = uses_by_vram([
        "0x80001000: lui         $v0, 0x8012",
        "0x80001004: lw          $v1, 0x10($v0)",
        "0x80001008: jr          $ra",
        "0x8000100C: nop",
    ])
    assert u == {}


def test_loop_increment_terminates():
    u = uses_by_vram([
        "0x80001000: lui         $v0, 0x8014",
        "0x80001004: addiu       $v0, $v0, -0x7D98",
        "L_80001008:",
        "0x80001008: sw          $zero, 0x0($v0)",
        "0x8000100C: addiu       $v0, $v0, 0x5D0",
        "0x80001010: bne         $v0, $a0, L_80001008",
        "0x80001014: nop",
        "0x80001018: jr          $ra",
        "0x8000101C: nop",
    ])
    assert 0x80138268 in addrs(u[0x80001000])


def test_back_edge_marks_values_looped():
    u = uses_by_vram([
        "0x80001000: lui         $s0, 0x8014",
        "0x80001004: addiu       $s0, $s0, -0x8248",
        "L_80001008:",
        "0x80001008: sw          $zero, 0x10($s0)",
        "0x8000100C: addiu       $s0, $s0, 0x2A0",
        "0x80001010: bne         $s0, $a0, L_80001008",
        "0x80001014: nop",
    ])
    assert ("mem", 0x80137DC8, 0) in u[0x80001000]
    assert ("mem", 0x80138068, LOOP) in u[0x80001000]


import re
from gen_relocation import (Site, classify, lui_value, parse_resolve, parse_existing, build_patches,
                            emit_toml, MOVED)

ROOT = pathlib.Path(__file__).resolve().parents[2]


def all_uses(lines, name="f"):
    return analyze_func(parse_funcs(src(lines, name))[0])


def cls_of(lines):
    u = all_uses(lines)
    assert len(u) == 1
    return classify(next(iter(u.values())))


def test_load_in_moved_range_is_move():
    assert cls_of(["0x80001000: lui         $v0, 0x8014", "0x80001004: lw          $v1, -0x7FA0($v0)"]) == "move"


def test_load_at_range_end_is_keep():
    assert cls_of(["0x80001000: lui         $v0, 0x8014", "0x80001004: lw          $v1, -0x77C8($v0)"]) == "keep"


def test_mixed_site_is_error():
    lines = ["0x80001000: lui         $v0, 0x8014",
             "0x80001004: lw          $v1, -0x7FA0($v0)",
             "0x80001008: lw          $a1, -0x7760($v0)"]
    assert cls_of(lines) == "mixed"
    res = build_patches(all_uses(lines), {}, set())
    assert res.patches == [] and any("mixed" in e for e in res.errors)


def test_edge_pointer_escape_is_ambiguous():
    lines = ["0x80001000: lui         $a0, 0x8014",
             "0x80001004: jal         0x80002000",
             "0x80001008: addiu       $a0, $a0, -0x7FA8"]
    assert cls_of(lines) == "ambiguous"
    res = build_patches(all_uses(lines), {}, set())
    assert res.patches == [] and any("resolve" in e for e in res.errors)


def test_resolve_manifest_settles_ambiguous():
    lines = ["0x80001000: lui         $a0, 0x8014",
             "0x80001004: jal         0x80002000",
             "0x80001008: addiu       $a0, $a0, -0x7FA8"]
    resolve = parse_resolve("f 0x80001000 move  # object at 0x80138058\n\n# comment\n")
    res = build_patches(all_uses(lines), resolve, set())
    assert res.errors == []
    assert res.patches == [("f", 0x80001000, 0x3C0480B4)]


def test_stale_resolve_entry_is_error():
    lines = ["0x80001000: lui         $v0, 0x8014", "0x80001004: lw          $v1, -0x7FA0($v0)"]
    res = build_patches(all_uses(lines), {("f", 0x80001000): "keep", ("g", 0x80009000): "move"}, set())
    assert any("classified move" in e for e in res.errors)
    assert any("matches no lui site" in e for e in res.errors)


def test_bad_resolve_line_is_error():
    try:
        parse_resolve("f 0x80001000 maybe\n")
    except ValueError:
        return
    assert False, "expected ValueError"


def test_patched_lui_is_normalized():
    lines = ["0x80001000: lui         $v0, 0x80B4", "0x80001004: lw          $v1, -0x7FA0($v0)"]
    res = build_patches(all_uses(lines), {}, set())
    assert res.errors == []
    assert res.patches == [("f", 0x80001000, 0x3C0280B4)]


def test_collision_with_existing_patch_is_error():
    lines = ["0x80001000: lui         $v0, 0x8014", "0x80001004: lw          $v1, -0x7FA0($v0)"]
    existing = parse_existing('[[patches.instruction]]\nfunc = "f"\nvram = 0x80001000\nvalue = 0x00000000\n')
    assert existing == {("f", 0x80001000)}
    res = build_patches(all_uses(lines), {}, existing)
    assert res.patches == [] and any("already patched" in e for e in res.errors)


def test_emit_value_encoding():
    assert lui_value(Site("f", 0, 2, 0x8014, False)) == 0x3C0280B4
    assert lui_value(Site("f", 0, 1, 0x8013, False)) == 0x3C0180B3
    assert lui_value(Site("f", 0, 31, 0x8014, True)) == 0x3C1F80B4


def test_overlay_functions_keyed_by_name():
    text = src(["0x800A6000: lui         $v0, 0x8014", "0x800A6004: lw          $v1, -0x7FA0($v0)"], "ovlA") + \
           src(["0x800A6000: lui         $v0, 0x8014", "0x800A6004: lw          $v1, -0x7760($v0)"], "ovlB")
    uses = {}
    for fn in parse_funcs(text):
        uses.update(analyze_func(fn))
    res = build_patches(uses, {}, set())
    assert res.errors == []
    assert res.patches == [("ovlA", 0x800A6000, 0x3C0280B4)]


def test_unused_site_is_counted_not_patched():
    lines = ["0x80001000: lui         $v0, 0x8014",
             "0x80001004: lui         $s1, 0x8014",
             "0x80001008: sll         $t0, $s1, 2"]
    res = build_patches(all_uses(lines), {}, set())
    assert res.patches == []
    assert res.counts.get("unused") == 1
    assert len(res.errors) == 1 and "0x80001004" in res.errors[0] and "unclassified" in res.errors[0]


def test_emit_toml_format():
    text = emit_toml([("f", 0x80001000, 0x3C0280B4)])
    assert text.startswith("# Generated by tools/coop/gen_relocation.py.")
    assert '[[patches.instruction]]\nfunc = "f"\nvram = 0x80001000\nvalue = 0x3C0280B4\n' in text


def test_ranges_match_cpp_table():
    header = (ROOT / "src" / "main" / "lockstep_core.h").read_text()
    line = next(l for l in header.splitlines() if "kMovedRanges[]" in l)
    triples = [(int(a, 16), int(b, 16), int(d, 16)) for a, b, d in re.findall(r"\{(0x[0-9A-Fa-f]+)u,\s*(0x[0-9A-Fa-f]+)u,\s*(0x[0-9A-Fa-f]+)u\}", line)]
    assert triples == [(lo, hi, delta_hi << 16) for _, lo, hi, delta_hi in MOVED]


def test_r4_site_moves_with_its_own_delta():
    lines = ["0x80001000: lui         $v0, 0x8014", "0x80001004: lw          $v1, -0x76D0($v0)"]
    res = build_patches(all_uses(lines), {}, set())
    assert res.errors == []
    assert res.patches == [("f", 0x80001000, 0x3C0280B6)]


def test_r4_patched_lui_is_normalized():
    lines = ["0x80001000: lui         $v0, 0x80B6", "0x80001004: lw          $v1, -0x76D0($v0)"]
    res = build_patches(all_uses(lines), {}, set())
    assert res.errors == []
    assert res.patches == [("f", 0x80001000, 0x3C0280B6)]


def test_resolve_move_with_range_name_sets_delta():
    site = Site("f", 0x80001000, 2, 0x8014, False)
    uses = {site: {("mem", None, IDX | LOOP)}}
    res = build_patches(uses, parse_resolve("f 0x80001000 move R4  # widened walk of 0x80138930\n"), set())
    assert res.errors == []
    assert res.patches == [("f", 0x80001000, 0x3C0280B6)]
    res = build_patches(uses, parse_resolve("f 0x80001000 move\n"), set())
    assert any("no single moved range" in e for e in res.errors)
    expect = False
    try:
        parse_resolve("f 0x80001000 move R9\n")
    except ValueError:
        expect = True
    assert expect


def test_site_spanning_two_deltas_is_mixed():
    lines = ["0x80001000: lui         $v0, 0x8014",
             "0x80001004: lw          $v1, -0x7FA0($v0)",
             "0x80001008: lw          $a1, -0x76D0($v0)"]
    assert cls_of(lines) == "mixed"


def test_loop_iterations_do_not_classify():
    lines = ["0x80001000: lui         $s0, 0x8014",
             "0x80001004: addiu       $s0, $s0, -0x8248",
             "L_80001008:",
             "0x80001008: sw          $zero, 0x10($s0)",
             "0x8000100C: addiu       $s0, $s0, 0x2A0",
             "0x80001010: bne         $s0, $a0, L_80001008",
             "0x80001014: nop"]
    assert cls_of(lines) == "keep"


def test_raw_lui_escape_is_ignored():
    lines = ["0x80001000: lui         $v0, 0x8014",
             "0x80001004: lw          $v1, -0x7FA0($v0)",
             "0x80001008: jr          $ra",
             "0x8000100C: nop"]
    assert cls_of(lines) == "move"


def test_indexed_edge_escape_is_not_ambiguous():
    lines = ["0x80001000: lui         $t0, 0x8014",
             "0x80001004: addiu       $t0, $t0, -0x6FE0",
             "0x80001008: addu        $a0, $t0, $a1",
             "0x8000100C: jal         0x80002000",
             "0x80001010: nop"]
    assert cls_of(lines) == "keep"


def test_edge_escape_beside_other_uses_is_dropped_with_note():
    lines = ["0x80001000: lui         $s0, 0x8014",
             "0x80001004: lw          $v1, -0x7FA0($s0)",
             "0x80001008: addiu       $a0, $s0, -0x77C8",
             "0x8000100C: jal         0x80002000",
             "0x80001010: nop"]
    res = build_patches(all_uses(lines), {}, set())
    assert res.errors == []
    assert res.patches == [("f", 0x80001000, 0x3C1080B4)]
    assert len(res.notes) == 1 and "range-edge pointer ignored" in res.notes[0]


def test_edge_rule_settles_ambiguous():
    lines = ["0x80001000: lui         $a0, 0x8014",
             "0x80001004: jal         0x80002000",
             "0x80001008: addiu       $a0, $a0, -0x7FA8"]
    resolve = parse_resolve("edge 0x80138058 move  # object at the start of R1\n")
    res = build_patches(all_uses(lines), resolve, set())
    assert res.errors == []
    assert res.patches == [("f", 0x80001000, 0x3C0480B4)]


def test_unused_edge_rule_is_error():
    lines = ["0x80001000: lui         $v0, 0x8014", "0x80001004: lw          $v1, -0x7FA0($v0)"]
    res = build_patches(all_uses(lines), parse_resolve("edge 0x80139020 keep\n"), set())
    assert any("matches no lui site" in e for e in res.errors)


from gen_relocation import verify_applied


def test_unclassified_is_error_unless_resolved():
    site = Site("f", 0x80001000, 2, 0x8014, False)
    uses = {site: {("mem", None, IDX | LOOP)}}
    res = build_patches(uses, {}, set())
    assert any("unclassified" in e for e in res.errors)
    res = build_patches(uses, {("f", 0x80001000): "keep"}, set())
    assert res.errors == [] and res.patches == []


def test_verify_applied_counts_patched_sites():
    toml_text = emit_toml([("f", 0x80001000, 0x3C0280B4)])
    patched = src(["0x80001000: lui         $v0, 0x80B4", "0x80001000: lui         $v0, 0x80B4"])
    unpatched = src(["0x80001000: lui         $v0, 0x8014"])
    assert verify_applied([patched], toml_text) == (1, 1)
    assert verify_applied([unpatched], toml_text) == (0, 1)


if __name__ == "__main__":
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    failed = 0
    for fn in fns:
        try:
            fn()
            print("PASS", fn.__name__)
        except Exception as e:
            failed += 1
            print("FAIL", fn.__name__, "->", repr(e))
    print(f"{len(fns)-failed}/{len(fns)} passed")
    sys.exit(1 if failed else 0)
