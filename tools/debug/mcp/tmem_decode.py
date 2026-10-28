"""Decode an RT64 TMEM dump (4 KB, from the MCP dump_tmem method) to RGBA and PNG, following lib/rt64/src/shaders/TextureDecoder.hlsli.

`python tmem_decode.py --self-test` checks the decoder; `python tmem_decode.py <file.tmem>` writes <file>.png using <file.tmem>.json.
"""

import json
import struct
import sys
import zlib

FMT_RGBA, FMT_YUV, FMT_CI, FMT_IA, FMT_I = range(5)
TLUT_RGBA16, TLUT_IA16 = 2, 3


def rgba16(v):
    return (((v >> 11) & 31) * 255 // 31, ((v >> 6) & 31) * 255 // 31, ((v >> 1) & 31) * 255 // 31, 255 if v & 1 else 0)


def ia16(v):
    return (v >> 8, v >> 8, v >> 8, v & 0xFF)


def decode(tmem, meta):
    """RGBA bytes, width, height for the tile described by meta (fmt, siz, line, tmem, palette, tlut, width, height)."""
    fmt, siz = meta["fmt"], meta["siz"]
    width, height = meta["width"], meta["height"]
    base = meta["tmem"] * 8
    stride = max(1, meta["line"]) * 8
    tlut = (meta.get("tlut", 0) >> 14) & 3
    palette = meta.get("palette", 0)
    rgba32 = fmt == FMT_RGBA and siz == 3
    shift = 2 if rgba32 else siz
    mask = 0x7FF if (rgba32 or tlut) else 0xFFF

    def load(rel, odd_row, or_address=0):
        if odd_row:
            row_start = (rel // stride) * stride
            at = base + row_start + (((rel - row_start) // 4) ^ 1) * 4 + (rel & 3)
        else:
            at = base + rel
        return tmem[((at & mask) | or_address) & 0xFFF]

    out = bytearray()
    for y in range(height):
        odd_row = bool(y & 1)
        for x in range(width):
            addr = y * stride + ((x << shift) >> 1)
            v0 = load(addr, odd_row)
            v1 = load(addr + 1, odd_row)
            nibble = (v0 >> (0 if x & 1 else 4)) & 0xF
            if tlut:
                pal = 0x800 + ((palette << 7) + (nibble << 3) if siz == 0 else (v0 << 3))
                value = (tmem[pal & 0xFFF] << 8) | tmem[(pal + 1) & 0xFFF]
                px = rgba16(value) if tlut == TLUT_RGBA16 else ia16(value) if tlut == TLUT_IA16 else (0, 0, 0, 255)
            elif siz == 0:
                if fmt == FMT_IA:
                    i = (nibble >> 1) * 255 // 7
                    px = (i, i, i, 255 if nibble & 1 else 0)
                elif fmt == FMT_CI:
                    i = (palette << 4) | nibble
                    px = (i, i, i, i)
                else:
                    px = (nibble * 17,) * 4
            elif siz == 1:
                if fmt == FMT_IA:
                    px = ((v0 >> 4) * 17,) * 3 + ((v0 & 15) * 17,)
                else:
                    px = (v0,) * 4
            elif siz == 2 or rgba32:
                if rgba32:
                    px = (v0, v1, load(addr, odd_row, 0x800), load(addr + 1, odd_row, 0x800))
                elif fmt == FMT_RGBA:
                    px = rgba16((v0 << 8) | v1)
                elif fmt == FMT_IA:
                    px = ia16((v0 << 8) | v1)
                else:
                    px = (v0, v1, v0, v1)
            else:
                px = (0, 0, 0, 255)
            out += bytes(px)
    return bytes(out), width, height


def png_bytes(rgba, width, height):
    raw = b"".join(b"\x00" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


def write_png(path, rgba, width, height):
    with open(path, "wb") as f:
        f.write(png_bytes(rgba, width, height))


def decode_file(tmem_path, png_path=None):
    with open(tmem_path, "rb") as f:
        tmem = f.read()
    with open(tmem_path + ".json", "r", encoding="utf-8") as f:
        meta = json.load(f)
    rgba, w, h = decode(tmem, meta)
    png_path = png_path or tmem_path.rsplit(".", 1)[0] + ".png"
    write_png(png_path, rgba, w, h)
    return png_path


def self_test():
    # RGBA16 4x2 at TMEM 0, line 1 (8 bytes a row): row 0 = red, green, blue, white; row 1 stored word-swapped.
    red, green, blue, white = 0xF801, 0x07C1, 0x003F, 0xFFFF
    tmem = bytearray(4096)
    row0 = [red, green, blue, white]
    row1 = [white, blue, green, red]
    for i, v in enumerate(row0):
        tmem[i * 2:i * 2 + 2] = v.to_bytes(2, "big")
    stored1 = b"".join(v.to_bytes(2, "big") for v in row1)
    tmem[8:16] = stored1[4:8] + stored1[0:4]
    rgba, w, h = decode(bytes(tmem), {"fmt": 0, "siz": 2, "line": 1, "tmem": 0, "palette": 0, "tlut": 0, "width": 4, "height": 2})
    assert (w, h) == (4, 2)
    px = [tuple(rgba[i * 4:i * 4 + 4]) for i in range(8)]
    assert px[0] == (255, 0, 0, 255) and px[1] == (0, 255, 0, 255) and px[2] == (0, 0, 255, 255) and px[3] == (255, 255, 255, 255), px[:4]
    assert px[4] == (255, 255, 255, 255) and px[7] == (255, 0, 0, 255), px[4:]

    # CI4 4x1, palette 1 (TLUT RGBA16): indices 0, 1, 2, 15 -> palette entries at 0x800 + (1 << 7) + index * 8.
    tmem = bytearray(4096)
    tmem[0:2] = bytes([0x01, 0x2F])
    for index, colour in ((0, red), (1, green), (2, blue), (15, white)):
        at = 0x800 + (1 << 7) + index * 8
        tmem[at:at + 2] = colour.to_bytes(2, "big")
    rgba, _, _ = decode(bytes(tmem), {"fmt": 2, "siz": 0, "line": 1, "tmem": 0, "palette": 1, "tlut": 0x8000, "width": 4, "height": 1})
    assert [tuple(rgba[i * 4:i * 4 + 4]) for i in range(4)] == [(255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255), (255, 255, 255, 255)]

    # I8 8x2: row 1 starts at byte 8 and is word-swapped.
    tmem = bytearray(4096)
    tmem[0:8] = bytes(range(8))
    tmem[8:16] = bytes([20, 21, 22, 23, 16, 17, 18, 19])
    rgba, _, _ = decode(bytes(tmem), {"fmt": 4, "siz": 1, "line": 1, "tmem": 0, "palette": 0, "tlut": 0, "width": 8, "height": 2})
    assert [rgba[(8 + x) * 4] for x in range(8)] == [16, 17, 18, 19, 20, 21, 22, 23]

    # IA8 and IA4: intensity in the high nibble / high 3 bits, alpha in the rest.
    tmem = bytearray(4096)
    tmem[0] = 0xF0
    tmem[1] = 0x0F
    rgba, _, _ = decode(bytes(tmem), {"fmt": 3, "siz": 1, "line": 1, "tmem": 0, "palette": 0, "tlut": 0, "width": 2, "height": 1})
    assert tuple(rgba[0:4]) == (255, 255, 255, 0) and tuple(rgba[4:8]) == (0, 0, 0, 255)

    png = png_bytes(bytes(rgba), 2, 1)
    assert png[:8] == b"\x89PNG\r\n\x1a\n" and b"IEND" in png
    print("tmem_decode self-test OK")


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        self_test()
    elif len(sys.argv) > 1:
        print(decode_file(sys.argv[1]))
