import textwrap
import pathlib
import tempfile
from state_model import load_model, validate


def _fresh_tmp():
    return pathlib.Path(tempfile.mkdtemp())


def _write(tmp_path, body):
    p = tmp_path / "m.toml"
    p.write_text(textwrap.dedent(body))
    return str(p)


def test_loads_states_and_predicates(tmp_path=None):
    tmp_path = tmp_path or _fresh_tmp()
    m = load_model(_write(tmp_path, '''
        [[state]]
        id = "menu"
        name = "Menu"
        parent = ""
        classify = [ { addr = 0x80130B14, mask = 0xFF000000, op = "eq", value = 0x05000000 } ]
    '''))
    assert m.by_id["menu"].classify[0].addr == 0x80130B14
    assert m.by_id["menu"].classify[0].op == "eq"


def test_depth_follows_parent(tmp_path=None):
    tmp_path = tmp_path or _fresh_tmp()
    m = load_model(_write(tmp_path, '''
        [[state]]
        id = "menu"
        name = "Menu"
        parent = ""
        classify = [ { addr = 0x80130B14, mask = 0xFF000000, op = "eq", value = 0x05000000 } ]
        [[state]]
        id = "menu.level_select"
        name = "Level Select"
        parent = "menu"
        classify = [ { addr = 0x80130B60, mask = 0xFF000000, op = "eq", value = 0x04000000 } ]
    '''))
    assert m.depth("menu") == 0
    assert m.depth("menu.level_select") == 1


def test_validate_flags_bad_op(tmp_path=None):
    tmp_path = tmp_path or _fresh_tmp()
    m = load_model(_write(tmp_path, '''
        [[state]]
        id = "x"
        name = "X"
        parent = ""
        classify = [ { addr = 0x80130B14, mask = 0xFF000000, op = "bogus", value = 5 } ]
    '''))
    errs = validate(m)
    assert any("op" in e and "x" in e for e in errs)


def test_validate_flags_missing_parent(tmp_path=None):
    tmp_path = tmp_path or _fresh_tmp()
    m = load_model(_write(tmp_path, '''
        [[state]]
        id = "child"
        name = "Child"
        parent = "ghost"
        classify = [ { addr = 0x80130B14, mask = 0xFF000000, op = "eq", value = 1 } ]
    '''))
    assert any("ghost" in e for e in validate(m))


def test_validate_flags_duplicate_id(tmp_path=None):
    tmp_path = tmp_path or _fresh_tmp()
    m = load_model(_write(tmp_path, '''
        [[state]]
        id = "dup"
        name = "A"
        parent = ""
        classify = [ { addr = 1, mask = 0xFF, op = "eq", value = 1 } ]
        [[state]]
        id = "dup"
        name = "B"
        parent = ""
        classify = [ { addr = 1, mask = 0xFF, op = "eq", value = 2 } ]
    '''))
    assert any("dup" in e for e in validate(m))


def test_disjoint_siblings_are_not_flagged(tmp_path=None):
    tmp_path = tmp_path or _fresh_tmp()
    m = load_model(_write(tmp_path, '''
        [[state]]
        id = "a"
        name = "A"
        parent = ""
        classify = [ { addr = 0x80130B14, mask = 0xFF000000, op = "eq", value = 0x05000000 } ]
        [[state]]
        id = "b"
        name = "B"
        parent = ""
        classify = [ { addr = 0x80130B14, mask = 0xFF000000, op = "eq", value = 0x06000000 } ]
    '''))
    assert validate(m) == []


def test_overlapping_range_siblings_are_flagged(tmp_path=None):
    tmp_path = tmp_path or _fresh_tmp()
    m = load_model(_write(tmp_path, '''
        [[state]]
        id = "a"
        name = "A"
        parent = ""
        classify = [ { addr = 0x80130B14, mask = 0xFF000000, op = "range", value = 0x06000000, value_hi = 0x08000000 } ]
        [[state]]
        id = "b"
        name = "B"
        parent = ""
        classify = [ { addr = 0x80130B14, mask = 0xFF000000, op = "eq", value = 0x07000000 } ]
    '''))
    errs = validate(m)
    assert any("a" in e and "b" in e for e in errs)


def test_codegen_emits_table_and_counts(tmp_path=None):
    tmp_path = tmp_path or _fresh_tmp()
    from gen_state_table import gen_state_table
    m = load_model(_write(tmp_path, '''
        [[state]]
        id = "menu"
        name = "Menu"
        parent = ""
        classify = [ { addr = 0x80130B14, mask = 0xFF000000, op = "eq", value = 0x05000000 } ]
        force = { slot = "menu_screen", params = { screen = 4 } }
    '''))
    c = gen_state_table(m)
    assert "#define RS_STATE_COUNT 1" in c
    assert "0x80130B14" in c
    assert "RS_SLOT_MENU_SCREEN" in c
    assert '"menu"' in c


def test_codegen_emits_reachable(tmp_path=None):
    tmp_path = tmp_path or _fresh_tmp()
    from gen_state_table import gen_state_table
    m = load_model(_write(tmp_path, '''
        [[state]]
        id = "menu"
        name = "Menu"
        parent = ""
        classify = [ { addr = 1, mask = 0xFF, op = "eq", value = 5 } ]
        [[state]]
        id = "mission"
        name = "Mission"
        parent = ""
        classify = [ { addr = 2, mask = 0xFF, op = "eq", value = 3 } ]
        force = { slot = "boot_level", params = { level = 0, craft = -1 } }
        reachable_from = ["menu"]
    '''))
    c = gen_state_table(m)
    assert '"menu"' in c
    assert "g_state_reachable" in c


def _blank_rdram():
    return bytearray(0x800000)


def _set_u8(buf, kseg0, val):
    buf[kseg0 - 0x80000000] = val & 0xFF


def _set_u32(buf, kseg0, val):
    buf[kseg0 - 0x80000000: kseg0 - 0x80000000 + 4] = int(val & 0xFFFFFFFF).to_bytes(4, "big")


def test_classify_real_descriptor_states():
    from state_model import classify
    root = pathlib.Path(__file__).resolve().parents[2]
    m = load_model(str(root / "tools" / "state" / "state_model.toml"))

    # in-mission: numMissionObjectives != 0, demo bit clear
    b = _blank_rdram()
    _set_u8(b, 0x80130B17, 4)
    _set_u32(b, 0x80130B50, 0x0000001B)   # bit 0x20 clear
    assert classify(m, b) == "mission"

    # attract demo: same objectives, demo bit set
    b = _blank_rdram()
    _set_u8(b, 0x80130B17, 4)
    _set_u32(b, 0x80130B50, 0x0000003B)   # bit 0x20 set
    assert classify(m, b) == "attract.demo"

    # cinematic: valid cineStatePtr
    b = _blank_rdram()
    _set_u32(b, 0x800B0934, 0x800D9780)
    assert classify(m, b) == "cinematic"

    # menu main page: valid gCurrentMenuData, menu id 0
    b = _blank_rdram()
    _set_u32(b, 0x800CE730, 0x800A5FD4)
    _set_u8(b, 0x800CE734, 0)
    assert classify(m, b) == "menu.main"

    # menu options page: menu id 2 (child beats parent)
    b = _blank_rdram()
    _set_u32(b, 0x800CE730, 0x800A5FD4)
    _set_u8(b, 0x800CE734, 2)
    assert classify(m, b) == "menu.options"

    # menu resident but an unmapped id -> parent "menu"
    b = _blank_rdram()
    _set_u32(b, 0x800CE730, 0x800A5FD4)
    _set_u8(b, 0x800CE734, 99)
    assert classify(m, b) == "menu"

    # boot: nothing set -> unknown (uncalibrated)
    assert classify(m, _blank_rdram()) == "unknown"


def test_real_descriptor_is_valid():
    root = pathlib.Path(__file__).resolve().parents[2]
    m = load_model(str(root / "tools" / "state" / "state_model.toml"))
    assert validate(m) == []


def test_real_descriptor_codegen(tmp_path=None):
    from gen_state_table import gen_state_table
    root = pathlib.Path(__file__).resolve().parents[2]
    m = load_model(str(root / "tools" / "state" / "state_model.toml"))
    c = gen_state_table(m)
    assert "#define RS_STATE_COUNT" in c
    assert len(m.states) >= 3


if __name__ == "__main__":
    import sys
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
