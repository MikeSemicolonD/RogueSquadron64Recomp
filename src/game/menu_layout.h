#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
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

} // namespace rs64::menu
