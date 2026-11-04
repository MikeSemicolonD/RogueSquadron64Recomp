#pragma once
// Mod asset overrides (asset_overrides.cpp): files under <mod>/assets/ appended past the ROM end, as mod levels and level rules look them up.
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace rs64::assets {

struct Override { uint32_t rom_off, size; };

// assets/from.json: { "<new path>": "<existing asset it copies>", ... }, the same as one <new path>.from file per entry.
inline constexpr const char* kFromMap = "from.json";

// The appended copy of the mod file at assets/<path>, or nullptr.
const Override* find(const std::string& path);

// Cart offset of the silent voice stream `seconds` long (1-10) that level-rule messages play.
uint32_t silence_rom(int seconds);

std::vector<std::pair<std::string, std::string>> read_from_map(const std::filesystem::path& assets);

}
