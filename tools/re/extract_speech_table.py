"""Extract speech sample table + English text mapping from the Rogue Squadron 64 ROM.

Stage 1 (this script):
  - Walk the data blob (ROM 0x144340 ...).
  - Find the speech asset (MORT-encoded sample container) and the Voice text file.
  - Parse the speech.raw header table -> list of (voiceId, offset, freq, byte_size).
  - Read voiceIdtoTextIdMap from the boot rodata.
  - Decrypt Voice text strings (XOR chain, prev=0xF5).
  - Emit CSV: voice_id, mort_offset, freq, byte_size, text_id, english.

Stage 2 (TODO): MORT codec decoder -> per-sample WAV.

Usage: python tools/re/extract_speech_table.py [--rom rs64.n64.us.1.0.z64] [--out speech_table.csv] [--dump-assets <dir>]
"""

from __future__ import annotations

import argparse
import csv
import struct
import sys
import zlib
from pathlib import Path

# ROM layout
SEGMENT_OFFSET = 0x144340
SEGMENT_HEADER_SIZE = 0x20
NUM_SEGMENTS = 2  # "data" and "dbg_data"
DATA_OFFSET_BASE = SEGMENT_OFFSET + SEGMENT_HEADER_SIZE * NUM_SEGMENTS  # 0x144380

# voiceIdtoTextIdMap location in RAM (from symbol_files/main_overlay.txt)
VOICE_MAP_VRAM = 0x8009FFE0
VOICE_MAP_BYTES = 0x5E8  # u16 * 0x2F4
ENTRYPOINT_VRAM = 0x80000400
ENTRYPOINT_ROM = 0x1000  # standard N64 boot offset; voice map ROM = vram - 0x80000400 + 0x1000


# ----- structures -----

def read_be32(buf: bytes, off: int) -> int:
    return struct.unpack_from(">I", buf, off)[0]


def read_be16(buf: bytes, off: int) -> int:
    return struct.unpack_from(">H", buf, off)[0]


def read_cstr(buf: bytes, off: int, maxlen: int) -> str:
    end = buf.find(b"\0", off, off + maxlen)
    if end < 0:
        end = off + maxlen
    raw = buf[off:end]
    # Asset names are mostly ASCII; map any high bytes to '?' for safe display.
    return "".join(chr(c) if 0x20 <= c < 0x7F else "?" for c in raw)


# ----- data blob walk -----

def walk_data_blob(rom: bytes):
    """Yields (segment_name, path, manifest_entry_dict).

    Per docs/data_blob: each segment has ONE block. block_size from the block
    header is the offset (relative to block start) to where the manifest lives.
    Manifest entries with flags==0x80 are directories — the next
    `directory_size / 0x20` entries are inside that directory.
    """
    for seg_idx in range(NUM_SEGMENTS):
        seg_off = SEGMENT_OFFSET + seg_idx * SEGMENT_HEADER_SIZE
        seg_name = read_cstr(rom, seg_off, 16)
        data_offset_rel = read_be32(rom, seg_off + 0x1C)
        seg_base = DATA_OFFSET_BASE + data_offset_rel  # ROM offset of this segment's block

        block_size = read_be32(rom, seg_base + 0x00)
        manifest_size = read_be16(rom, seg_base + 0x06)
        manifest_off_abs = seg_base + block_size
        num_entries = manifest_size // 0x20

        # Read manifest into a flat list, then yield with directory paths assembled.
        entries = []
        for i in range(num_entries):
            ent_off = manifest_off_abs + i * 0x20
            ent = {
                "asset_offset": read_be32(rom, ent_off + 0x00) + seg_base,  # to ROM-absolute
                "decompressed_size": read_be32(rom, ent_off + 0x04),
                "compressed_size": read_be32(rom, ent_off + 0x08),
                "flags": rom[ent_off + 0x0C],
                "unk0D": rom[ent_off + 0x0D],
                "directory_size": read_be16(rom, ent_off + 0x0E),
                "asset_name": read_cstr(rom, ent_off + 0x10, 16),
            }
            entries.append(ent)

        # Walk with directory stack so paths get assembled.
        dir_stack = []  # list of (path_prefix, remaining_count)
        i = 0
        while i < num_entries:
            ent = entries[i]
            # pop dirs we've exhausted
            while dir_stack and dir_stack[-1][1] == 0:
                dir_stack.pop()
            path_prefix = "/".join(d[0] for d in dir_stack)
            full_path = (path_prefix + "/" if path_prefix else "") + ent["asset_name"]

            if ent["flags"] == 0x80:
                # directory: next directory_size/32 entries (including THIS one) are the dir contents
                child_count = ent["directory_size"] // 0x20
                # decrement parent dir counter for this slot
                if dir_stack:
                    dir_stack[-1] = (dir_stack[-1][0], dir_stack[-1][1] - 1)
                # push new dir, count excludes the header entry itself
                dir_stack.append((ent["asset_name"], child_count - 1))
                i += 1
                continue

            # leaf asset
            yield seg_name, full_path, ent
            if dir_stack:
                dir_stack[-1] = (dir_stack[-1][0], dir_stack[-1][1] - 1)
            i += 1


def load_asset_bytes(rom: bytes, ent: dict) -> bytes:
    off = ent["asset_offset"]
    if ent["compressed_size"] != 0xFFFFFFFF:
        # Compressed: zlib stream. Extractor docs say "compressed sizes are oversized by 10".
        comp_size = ent["compressed_size"] - 10
        raw = rom[off:off + comp_size]
        try:
            return zlib.decompress(raw)
        except zlib.error as exc:
            print(f"  warn: zlib decompress failed for {ent['asset_name']}: {exc}", file=sys.stderr)
            return b""
    else:
        return rom[off:off + ent["decompressed_size"]]


# ----- voice-map (boot rodata) -----

def load_voice_map(rom: bytes) -> list[int]:
    rom_off = VOICE_MAP_VRAM - ENTRYPOINT_VRAM + ENTRYPOINT_ROM
    entries = VOICE_MAP_BYTES // 2
    return list(struct.unpack_from(f">{entries}H", rom, rom_off))


# ----- Voice TXT file -----

def decrypt_text(buf: bytes) -> bytes:
    out = bytearray(len(buf))
    prev = 0xF5
    for i, b in enumerate(buf):
        c = b ^ prev
        out[i] = c
        prev ^= c
    return bytes(out)


def parse_text_file(data: bytes) -> list[str]:
    """Returns a list of decrypted English strings indexed by textId.

    Header (big-endian):
      u16 language_count
      u16 string_count
      u32 language_offset[language_count]
      ...
    Within a language: u16 string_offsets[string_count] (rel to language_offset).
    Each string is null-terminated, XOR-encrypted with the prev=0xF5 chain.
    English is language 0.
    """
    if len(data) < 0x1C:
        return []
    lang_count = read_be16(data, 0x00)
    string_count = read_be16(data, 0x02)
    if lang_count == 0 or lang_count > 6 or string_count == 0 or string_count > 0x4000:
        return []
    en_lang_off = read_be32(data, 0x04)  # English = first language
    # string_offsets are u16 relative to en_lang_off
    strings: list[str] = []
    for i in range(string_count):
        rel = read_be16(data, en_lang_off + i * 2)
        s_off = en_lang_off + rel
        # find encrypted-null terminator: must decrypt incrementally
        # Easier: decrypt a generous slice then find \0
        # Strings tend to be < 512 bytes; cap at 1024.
        chunk = data[s_off:s_off + 1024]
        dec = decrypt_text(chunk)
        end = dec.find(b"\0")
        if end < 0:
            end = len(dec)
        try:
            strings.append(dec[:end].decode("ascii"))
        except UnicodeDecodeError:
            strings.append(dec[:end].decode("ascii", errors="replace"))
    return strings


# ----- speech.raw (MORT table only) -----

def parse_speech_header(data: bytes) -> list[dict]:
    """Returns list of {sample_idx, offset, type_byte, mort_flag, unk05, freq, byte_size}.

    sample_idx is the position in speech.raw's offset table. The MORT magic is
    just "MORT" (4 bytes); byte 4 of the header is a flag (0x00 or 0x01) — not
    a null terminator as earlier docs assumed.
    """
    if len(data) < 4:
        return []
    n = read_be32(data, 0)
    if n == 0 or n > 0x10000:
        return []
    out = []
    for i in range(n):
        word = read_be32(data, 4 + i * 4)
        type_byte = (word >> 24) & 0xFF
        offset = word & 0xFFFFFF
        if offset + 12 > len(data):
            continue
        if data[offset:offset + 4] != b"MORT":
            continue
        mort_flag = data[offset + 4]
        unk05 = data[offset + 5]
        freq = read_be16(data, offset + 6)
        word_count = read_be32(data, offset + 8)
        out.append({
            "sample_idx": i,
            "offset": offset,
            "type_byte": type_byte,
            "mort_flag": mort_flag,
            "unk05": unk05,
            "freq": freq,
            "byte_size": word_count * 4,
        })
    return out


# ----- main -----

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--rom", default="rs64.n64.us.1.0.z64")
    ap.add_argument("--out", default="speech_table.csv")
    ap.add_argument("--dump-assets", default=None, help="optional dir to extract all assets")
    ap.add_argument("--list-assets", action="store_true", help="list all assets and exit")
    args = ap.parse_args()

    rom_path = Path(args.rom)
    if not rom_path.exists():
        print(f"rom not found: {rom_path}", file=sys.stderr)
        return 2
    rom = rom_path.read_bytes()
    print(f"[+] rom: {rom_path} ({len(rom)} bytes)")

    # Walk data blob, optionally dump assets, find speech + voice text
    speech_data = None
    voice_text_data = None
    asset_count = 0
    if args.dump_assets:
        out_dir = Path(args.dump_assets)
        out_dir.mkdir(parents=True, exist_ok=True)

    for seg_name, full_path, ent in walk_data_blob(rom):
        asset_count += 1
        name = ent["asset_name"]
        if args.list_assets:
            print(f"  {seg_name:8s}  off=0x{ent['asset_offset']:08x}  decomp={ent['decompressed_size']:8d}  comp=0x{ent['compressed_size']:08x}  flags=0x{ent['flags']:02x}  {seg_name}/{full_path}")
        lname = full_path.lower()
        if lname.endswith("speech") and speech_data is None:
            speech_data = load_asset_bytes(rom, ent)
            print(f"[+] found speech asset: path={full_path!r} bytes={len(speech_data)}")
        if lname.endswith("voice_txt") and voice_text_data is None:
            voice_text_data = load_asset_bytes(rom, ent)
            print(f"[+] found voice_TXT asset: path={full_path!r} bytes={len(voice_text_data)}")
        if args.dump_assets:
            data = load_asset_bytes(rom, ent)
            safe = full_path.replace("/", "_")
            (out_dir / f"{seg_name}.{safe}.bin").write_bytes(data)

    print(f"[+] total assets seen: {asset_count}")

    if args.list_assets:
        return 0

    if speech_data is None:
        print("[!] speech asset not found by name match. Dump with --dump-assets and inspect.", file=sys.stderr)
        return 3

    samples = parse_speech_header(speech_data)
    print(f"[+] mort samples parsed: {len(samples)}")

    voice_map = load_voice_map(rom)
    print(f"[+] voice map entries: {len(voice_map)}  (first 8: {voice_map[:8]})")

    voice_strings: list[str] = []
    if voice_text_data:
        voice_strings = parse_text_file(voice_text_data)
        print(f"[+] voice_TXT parsed: {len(voice_strings)} strings; sample: {voice_strings[:3] if voice_strings else 'NONE'}")
    else:
        print("[?] voice_TXT not found - english column will be blank")

    # Build reverse lookup: sample_idx -> [list of game voiceIds that play it]
    reverse_vm: dict[int, list[int]] = {}
    for gvid, pos in enumerate(voice_map):
        reverse_vm.setdefault(pos, []).append(gvid)

    # Emit main CSV + "needs listening" worksheet for samples without useful subtitle.
    out_path = Path(args.out)
    needs_path = out_path.with_name(out_path.stem + "_needs_listen.csv")

    def needs_listen(english: str) -> bool:
        t = english.strip()
        # Empty / space-only → no subtitle attached. Always needs listening.
        if not t:
            return True
        # Short utterances that look like placeholder labels rather than real
        # subtitle text. "Raowr!" / "Romf." style Wookiee noises end up here —
        # the user may want to confirm those by ear too.
        return len(t) <= 10

    with out_path.open("w", newline="", encoding="utf-8") as fp, \
         needs_path.open("w", newline="", encoding="utf-8") as nfp:
        w = csv.writer(fp)
        nw = csv.writer(nfp)
        w.writerow(["sample_idx", "mort_offset", "mort_flag", "freq", "byte_size",
                    "english", "game_voice_ids", "needs_listen", "wav_filename"])
        nw.writerow(["sample_idx", "freq", "byte_size", "english_or_blank",
                     "game_voice_ids", "wav_filename", "user_notes"])
        for s in samples:
            idx = s["sample_idx"]
            english = voice_strings[idx] if voice_strings and idx < len(voice_strings) else ""
            gvids = reverse_vm.get(idx, [])
            gvid_str = " ".join(str(g) for g in gvids) if gvids else ""
            wav_name = f"sample_{idx:04d}_{s['freq']}hz.wav"
            needs = needs_listen(english)
            w.writerow([idx, f"0x{s['offset']:06x}", f"0x{s['mort_flag']:02x}",
                        s["freq"], s["byte_size"], english, gvid_str,
                        "Y" if needs else "", wav_name])
            if needs:
                nw.writerow([idx, s["freq"], s["byte_size"], english, gvid_str, wav_name, ""])
    print(f"[+] wrote {out_path}")
    print(f"[+] wrote {needs_path}  (samples needing identification)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
