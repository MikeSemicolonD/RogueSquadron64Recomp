import pathlib
import tempfile
from compare_hashes import compare, main

LEGEND = "# frame combined rng mission npcs\n"


def _write(lines):
    d = pathlib.Path(tempfile.mkdtemp())
    p = d / "h.hash"
    p.write_text(LEGEND + "".join(l + "\n" for l in lines))
    return str(p)


def test_identical():
    a = _write(["0 01 0A 0B 0C", "1 02 0A 0B 0D"])
    b = _write(["0 01 0A 0B 0C", "1 02 0A 0B 0D"])
    r = compare(a, b)
    assert r.status == "identical" and r.frames_compared == 2


def test_first_divergence_names_regions():
    a = _write(["0 01 0A 0B 0C", "1 02 0A 0B 0D", "2 03 0A 0B 0E"])
    b = _write(["0 01 0A 0B 0C", "1 FF 0A 0F 0E", "2 FE 0A 0F 0E"])
    r = compare(a, b)
    assert r.status == "diverged" and r.first_frame == 1
    assert r.regions == ["mission", "npcs"]


def test_length_mismatch_is_divergence():
    a = _write(["0 01 0A 0B 0C", "1 02 0A 0B 0D", "2 03 0A 0B 0E"])
    b = _write(["0 01 0A 0B 0C"])
    r = compare(a, b)
    assert r.status == "diverged" and r.first_frame == 1 and r.regions == ["length"]


def test_one_trailing_frame_is_tolerated():
    a = _write(["0 01 0A 0B 0C", "1 02 0A 0B 0D"])
    b = _write(["0 01 0A 0B 0C"])
    r = compare(a, b)
    assert r.status == "identical" and r.frames_compared == 1 and "trailing" in r.detail


def test_legend_mismatch_is_unusable():
    a = _write(["0 01 0A 0B 0C"])
    d = pathlib.Path(tempfile.mkdtemp())
    b = d / "other.hash"
    b.write_text("# frame combined rng\n0 01 0A\n")
    r = compare(a, str(b))
    assert r.status == "unusable"


def test_missing_file_is_unusable():
    a = _write(["0 01 0A 0B 0C"])
    r = compare(a, a + ".nope")
    assert r.status == "unusable"


def test_zero_frames_is_unusable_with_exit_1():
    a = _write(["0 01 0A 0B 0C"])
    empty = _write([])
    for pair in ((a, empty), (empty, a), (empty, empty)):
        r = compare(*pair)
        assert r.status == "unusable" and "no frames" in r.detail
        assert main(["compare_hashes.py", *pair]) == 1


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
