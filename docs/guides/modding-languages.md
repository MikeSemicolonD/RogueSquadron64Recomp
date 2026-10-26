# Language mods

A language mod adds a language to the game: its menu text, mission text and subtitles, drawn with glyphs the mod brings. Any script works, including ones the cartridge never had (Japanese, Greek, Cyrillic, ...). The game's own English, German and French stay as they are.

[mods/lang-template](../../mods/lang-template) is a working starting point: it relabels three menu entries and draws a ★ from its glyph pack. [mods/lang-ja](../../mods/lang-ja) is a complete one: a Japanese translation of the menus, mission text, subtitles and opening crawls, with packs for every font.

## Folder layout

A language mod is a folder mod (see [modding.md](modding.md) for where mods go):

```
my-language/
  mod.json                  the usual mod manifest
  lang/language.json        { "code": "ja", "name": "NIHONGO", "rtl": false }
  lang/front.json           front-end text (menus, briefings, SELECT LEVEL)
  lang/game.json            in-mission text (pause menu, HUD messages)
  lang/voice.json           subtitles for voice lines
  lang/menu.json            optional: menu mod labels by text key (adding-menus-and-buttons.md#languages)
  lang/glyphs/<font>.bin    one glyph pack per game font
```

- `code` names the language: a lower-case ISO 639-1 language code (`ja`, `ko`, `zh`, `ru`, `el`, ...), not an ISO 3166 country code (Japanese is `ja`, not `jp`; Korean `ko`, not `kr`; Chinese `zh`, not `cn`). For a regional variant add the region as a BCP 47 tag (`pt-BR`, `zh-TW`). Menu mods key their label maps on the same code, and `--language` takes it. The loader doesn't check the standard: it accepts any non-empty code, so a wrong one (`jp`) loads but won't match other mods' label maps. `en`, `de` and `fr` are the game's own and can't be reused.
- `name` is shown on the LANGUAGE page. That page draws in the game's Latin font until the language is active, so a name in another script shows as the code there; a romanized name (`NIHONGO`) always shows.
- `rtl: true` marks a right-to-left language (see [Right-to-left](#right-to-left)).
- One mod holds one language.

## Text files

Each text file is a JSON object from text id to UTF-8 text. Ids are decimal without leading zeros (`"163"`) or hex (`"0xA3"`). An id the file leaves out keeps the game's English text, so a translation can grow a few strings at a time:

```json
{ "162": "スタート", "163": "オプション", "164": "もどる" }
```

To find an id, map a range of ids to their own numbers and read them off the screen, e.g. `"160": "160"`, `"161": "161"`, ... Some ids on the main menu and OPTIONS page: 162 START, 163 OPTIONS, 164 BACK, 172 BIOGRAPHIES, 175 GAME SETTINGS.

ASCII characters (0x20-0x7E) always draw with the game's own glyphs, and a pack can't replace them; the menu font has only capitals, digits and some punctuation, so lower-case Latin text shows blanks there. Every other character needs a glyph in the pack of the font that shows it.

## Glyph packs

A pack holds the glyphs one game font is missing, at that font's cell size. The game draws only the top rows of each cell (the drawn height), so keep ink inside them.

| Font | Used for | Cell (w x h) | Drawn rows |
|---|---|---|---|
| `italic35` | front-end menus | 22 x 35 | 33 |
| `normal10` | other text | 10 x 14 | 14 |
| `normal11` | other text | 8 x 15 | 15 |
| `subtitle8` | subtitles | 6 x 12 | 10 |
| `hatten13` | other text | 10 x 20 | 15 |
| `subtitle12` | subtitles | 12 x 12 | 10 |
| `subtitle20` | subtitles | 10 x 20 | 15 |

`credits` and `starwars50` take no packs.

The RS64 Explorer's *Fonts* tab builds them. Import a language mod by picking its folder (its text, packs and license files come in), open a font, and draw its glyphs from a `.ttf` or `.otf` file, either for that font or for every font at once: one glyph for every character the text uses. The project remembers the file, so importing the text again after translating more draws the new characters too. Each glyph is fitted to the font's drawn height, and the tab lists the characters the file has no glyph for (it never fills them from another font file). Click a glyph to touch it up pixel by pixel or change its advance; *Export mod* writes the packs to `lang/glyphs/`.

File format (all numbers big-endian), for writing packs another way:

| Bytes | Field |
|---|---|
| 4 | `RSG1` |
| 2, 2 | cell width (even), cell height: must match the font |
| 4 | glyph count |
| per glyph: 4 | Unicode code point |
| 1 | advance in pixels |
| w x h / 2 | 4-bit intensity, row by row from the top, two pixels per byte (high nibble = left), 0 = clear, 15 = solid |

A pack with the wrong cell size or length is skipped with a `[lang]` line in the log. A character missing from a font's pack (or a font with no pack) uses the same character from the language's other packs, scaled from the one whose drawn height is nearest; a character no pack has draws as an outlined box.

Draw from fonts whose license allows redistribution (for example the SIL Open Font License or the M+ font license); the system fonts that come with Windows or macOS don't. Ship each font's license text in `lang/glyphs/` beside the packs drawn from it, with a short note saying which pack came from which font.

Fit each glyph to both the cell width and the drawn height. For the large menu font an outline font at a bold weight reads best; for the 6-12 px cells a pixel font made for that size (PixelMplus 10, Misaki 8x8) reads far better than a shrunk outline font.

## Limits

- One screen can show at most 127 different non-ASCII characters at once. Characters borrow 127 slots and the least recently used one is reused, so a language with thousands of characters works as long as each screen stays under the limit. Labels of menu-mod buttons are encoded when the menu is built, so after 127 newer characters (a long briefing, say) a label can show wrong glyphs until the menu is rebuilt.
- The LANGUAGE page lists up to 7 languages (the game's three plus four mods).
- The text box sizes are the game's: long translations can run off a button or wrap where English did not.

## Right-to-left

With `rtl: true` each line is reversed for display, keeping runs of ASCII letters and digits left to right. There is no shaping: write Arabic and similar scripts in their presentation forms (U+FB50-U+FEFF) in the text files, with a glyph for each form.

## Trying it

- Pick the language in OPTIONS > LANGUAGE, or run with `--language <code>` (that run only; the saved choice is unchanged).
- Menu mods can label buttons for the language with a label map entry for its code (see [adding-menus-and-buttons.md](adding-menus-and-buttons.md#languages)).
- Removing the mod returns the game to its own languages; a saved choice of the removed language is ignored and the game uses its own language setting.
