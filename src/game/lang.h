#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "game/lang_core.h"

namespace rs64::lang {

// Reads lang/language.json (+ front/game/voice.json) from each enabled mod, once.
void load_all();

const Language* find(const std::string& code);

// Built-ins first (en, de, fr), then mod languages in mod load order.
std::vector<std::string> codes();

// The setting's mod language, or nullptr for a built-in or unset language.
const Language* active();

// At least one language mod is enabled (fonts and the texture table are extended only then).
bool installed();

// Points every extended font's map bytes 0x80-0xFE at the code page glyphs, or back at the font's own characters when no mod language is active.
void apply_maps(uint8_t* rdram);

// The runtime code page that encode() fills.
CodePage& code_page();

// Writes the glyphs of these code page bytes (0x80-0xFE) into every loaded extended font from the active language's glyph packs; a missing glyph is blank.
void write_slots(uint8_t* rdram, const std::vector<uint8_t>& slots);

// UTF-8 to the game's one-byte text in the active language's code page; slots it newly takes get their glyphs at the next flush().
std::string encode_text(const std::string& utf8);
void flush(uint8_t* rdram);

// The game's (or a mod label's English) text for a translated string as shown, so tools that find menu entries by label work in any language; `shown` itself if unknown.
void note_original(const std::string& shown, const std::string& original);
std::string original(const std::string& shown);

} // namespace rs64::lang
