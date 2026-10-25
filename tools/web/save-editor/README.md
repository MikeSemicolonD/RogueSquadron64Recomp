# Rogue Squadron save editor

`rs64_save_editor.html` — a self-contained, offline HTML tool for reading and editing
*Star Wars: Rogue Squadron* (N64) EEPROM save data. No build step, no dependencies:
open the file in any browser.

## What it edits

Works on any raw EEPROM image — the recomp's on-disk `Eep4k` save (512 bytes) or a cart
dump made with the Pico dumper (`gamepak.c`). Load a file, edit, export a corrected copy.

The recompilation stores its save as `saves/<game-id>.bin` (see
[librecomp `pi.cpp`](../../lib/N64ModernRuntime/librecomp/src/pi.cpp)): a raw 512-byte linear
EEPROM image, no header, no byte-swap — byte-for-byte what this editor reads and writes. So you
can load that `.bin` directly and drop the exported copy back in (with the game closed). Export
preserves the loaded filename, so an edited `<game-id>.bin` is a drop-in replacement.

- **Pilots** — 3 account slots: name, current level, per-level medals
  (none/bronze/silver/gold, 19 levels), pre-selected profile, active flag, and per-pilot
  weapon upgrades (the `account +0x0C` unlock word, mask `0x7FFE00`).
- **Elite Rogues** — the 10-entry high-score board with derived score and rank.
- **Settings** — music/SFX/speech volume, controller and language bytes, active-slot mask.
- **Unlocks** — the cheat-code flags (`cheatCodeFlags`, body `+0x08`): ships (Millennium
  Falcon, TIE, Naboo, AT-ST), bonus levels (Tatooine/Trench/Hoth), "all weapon & shield
  powerups", unlimited lives, all-missions, altitude radar, menu unlocks, and a mutually
  exclusive Luke-skin selector. Bit map confirmed in the sister decomp's
  `docs/cheat_codes/cheat_codes.md` (runtime word `0x80130B58` = `D_80130B40.cheatCodeFlags[0]`).
- **Raw / Hex** — annotated hex dump (header / body copy 0 / body copy 1 / padding),
  arbitrary byte poke, manual checksum recompute, and copy-0 → copy-1 mirror.

Structured edits mirror both body copies and recompute all checksums automatically, so the
exported file passes the game's `initSaveData` sanity check. The editor only exports edits of
a loaded save; the example data it opens with can't be exported.

The confirmed weapon-upgrade bits are the seeker toggles (`0x800`/`0x2000`/`0x4000`, per
`func_800C6728`); the remaining "advanced" toggles are inferred (shown dashed) and not yet
verified in-game.

## Format (verified against the game code)

512-byte image: `0x00` header (`0x20`) + two `save_data_body` copies (`0xC8`) at `0x20`
and `0xE8`, then zero padding. Each record holds two zlib `adler32` checksums (`+0x00` over
the `0xB0` body, `+0x04` over the `0x10` unk08 block). The header is a fixed constant block
(`"GAME"`/`"GSYS"`/`F5F5F5F5`/`AAAAAAAA`/`55555555` + `(0xB0<<16)|2`) with its own two
adler32s. All multi-byte fields are big-endian.

Sources: [docs/data-structures.md](../../docs/data-structures.md) "Save data", and the sister
decomp's `func_80006338` (header build), `func_80006798` (body checksums), `func_8006EB48`
(medal bit layout).

## Export in the sandbox vs. locally

The extension dropdown next to **Export** picks the output type (`.bin` for the recomp,
`.eep`/`.srm`/`.sra`/`.raw` otherwise); loading a file selects its extension automatically. The
bytes are identical regardless — only the name changes.

Opened locally, export downloads the raw image directly. In the claude.ai Artifact sandbox,
raw binary isn't an allowed download type, so it delivers a `.zip` containing the `.eep` via
the `downloads` capability (unzip to get the save); if that's unavailable it falls back to a
base64 blob you can copy out of the Raw tab.
