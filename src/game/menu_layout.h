#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rs64::menu {

// JSON text is UTF-8; the game's fonts index Latin-1 bytes (the menu font has A-Z, digits, some punctuation and Ä Ö Ü). Characters past U+00FF become '?'.
inline std::string to_latin1(const std::string& s)
{

    std::string out;

    for (size_t i = 0; i < s.size(); ++i)
    {
        const uint8_t c = (uint8_t)s[i];

        if (c < 0x80) {
            out += (char)c;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            const uint32_t cp = ((c & 0x1Fu) << 6) | ((uint8_t)s[++i] & 0x3Fu);
            out += cp <= 0xFF ? (char)cp : '?';
        } else {
            while (i + 1 < s.size() && ((uint8_t)s[i + 1] & 0xC0) == 0x80) ++i;
            out += '?';
        }
    }

    return out;
}

// A button's `index`: 0-based, negative counts from the end, clamped to the list.
inline int resolve_index(int index, int count)
{

    if (count <= 0) return 0;

    const int i = index < 0 ? count + index : index;

    return std::clamp(i, 0, count - 1);
}

template <class T>
void move_to_index(std::vector<T>& v, size_t from, int index)
{

    if (from >= v.size()) return;

    const int to = resolve_index(index, (int)v.size());
    T item = std::move(v[from]);

    v.erase(v.begin() + from);
    v.insert(v.begin() + to, std::move(item));
}

// A front-end native entry by alias, matched on the built slot's sub-type (and param when match_param): ordering anchors, replace and hide.
struct NativeAlias { uint8_t sub; int32_t param; bool match_param; };

struct NativeAliasRow { const char* menu; const char* name; NativeAlias alias; };

// Captured from the built menus (sub, param); OPTIONS' spacer and GAME SETTINGS' spacer have no alias.
inline constexpr NativeAliasRow kNativeAliases[] = {
    { "main_menu", "start", { 1, 1, true } },
    { "main_menu", "options", { 1, 2, true } },
    { "options", "biographies", { 1, 8, true } },
    { "options", "elite_rogues", { 1, 4, true } },
    { "options", "passcodes", { 1, 7, true } },
    { "options", "game_settings", { 1, 3, true } },
    { "options", "sound_settings", { 1, 6, true } },
    { "options", "controller_settings", { 1, 5, true } },
    { "options", "back", { 25, 0, false } },
    { "game_settings", "auto_roll", { 5, 0, true } },
    { "game_settings", "auto_level", { 5, 1, true } },
    { "game_settings", "free_camera", { 5, 2, true } },
    { "game_settings", "crosshairs", { 5, 3, true } },
    { "game_settings", "high_resolution", { 5, 4, true } },
    { "game_settings", "restore_defaults", { 14, 0, false } },
    { "game_settings", "back", { 25, 0, false } },
};

inline const NativeAlias* native_alias(std::string_view menu, std::string_view name)
{

    for (const NativeAliasRow& r : kNativeAliases)
        if (menu == r.menu && name == r.name) return &r.alias;

    return nullptr;
}

// Pause GAME SETTINGS natives (pauseMenuGameSettings 0x80109E24) by alias: their text id, or -1.
inline int pause_native_alias(std::string_view name)
{

    constexpr std::pair<const char*, int> kRows[] = { { "auto_roll", 0x25 }, { "auto_level", 0x26 }, { "free_camera", 0x28 }, { "crosshairs", 0x27 },
                                                      { "displays", 0x2B }, { "cockpit", 0x2A }, { "back", 0x39 } };

    for (const auto& [n, tid] : kRows)
        if (name == n) return tid;

    return -1;
}

// A list longer than the menu's rows, shown `visible` at a time from `top`; sel == total is the BACK entry below it.
struct ScrollPos { int sel; int top; };

inline ScrollPos scroll_step(ScrollPos p, int dir, int total, int visible)
{

    if (dir > 0) {
        p.sel = p.sel >= total ? 0 : p.sel + 1;
    } else {
        p.sel = p.sel == 0 ? total : p.sel - 1;
    }

    if (p.sel == total) return p;

    if (p.sel < p.top) p.top = p.sel;
    if (p.sel >= p.top + visible) p.top = p.sel - visible + 1;

    return p;
}

} // namespace rs64::menu
