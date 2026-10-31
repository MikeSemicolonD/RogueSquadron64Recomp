"""Copy a Star Wars: Rogue Squadron (USA 1.0) ROM to where the build reads it, as big-endian .z64.

Accepts .z64, .v64 (byte-swapped) or .n64 (little-endian). Usage: python tools/setup/import_rom.py <rom>
"""
import hashlib
import sys
from pathlib import Path

RECOMP = Path(__file__).resolve().parents[2]
SHA1 = "ed42eed1ee2db646ff7ef94ba8c5421d164a4f0d"

# N64Recomp and make_elf_from_syms.py read the decomp's copy, RSPRecomp (rsp/*.toml) the repo root's.
DESTS = [RECOMP.parent / "rogue_squadron64" / "rogue_squadron.z64", RECOMP / "rogue_squadron.z64"]


def to_z64(data):
    magic = bytes(data[:4])

    if magic == b"\x37\x80\x40\x12":
        data[0::2], data[1::2] = data[1::2], data[0::2]
    elif magic == b"\x40\x12\x37\x80":
        data[0::4], data[1::4], data[2::4], data[3::4] = data[3::4], data[2::4], data[1::4], data[0::4]

    return data


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)

    src = Path(sys.argv[1].strip().strip("'\"")).expanduser()

    if not src.is_file():
        sys.exit(f"No file at {src}")

    data = to_z64(bytearray(src.read_bytes()))

    if hashlib.sha1(data).hexdigest() != SHA1:
        sys.exit(f"{src} is not the USA 1.0 ROM of Star Wars: Rogue Squadron (expected SHA-1 {SHA1} as .z64). USA Rev 1, European and Japanese ROMs are not supported.")

    for dest in DESTS:
        if not dest.parent.is_dir():
            sys.exit(f"{dest.parent} does not exist: clone the decomp there first.")

        dest.write_bytes(data)
        print(f"Wrote {dest}")


if __name__ == "__main__":
    main()
