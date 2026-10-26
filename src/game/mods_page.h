#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "game/menu_layout.h"

// The MODS page's list (main menu > MODS): every installed mod in load order, a window of rows over it, and the carry that moves one mod.
namespace rs64::mods_page {

struct Mod {
    std::string id;
    std::string name;
    std::string description;
    bool enabled;
    bool runtime_toggleable;

    // Why the mod could not be opened (librecomp's open error); empty for a loaded mod. Failed mods list last and never move.
    std::string failed = {};
};

// The page's words in the shown language (the game's text, drawn as given).
struct Words {
    std::string on = "ON";
    std::string off = "OFF";
    std::string failed = "FAILED";
    std::string none = "NO MODS INSTALLED";
};

// pos.sel is the mod under the cursor; carrying is the carried mod's index (-1 when none), carry_from where it was picked up.
// visible is the window size the last carry used, so a cancel can bring the mod back into view.
struct List {
    std::vector<Mod> mods;
    rs64::menu::ScrollPos pos{ 0, 0 };
    int carrying = -1;
    int carry_from = -1;
    int visible = 1;
};

constexpr int kNameMax = 16;
constexpr int kDescMax = 200;

inline void fit_window(List& l, int visible)
{

    l.visible = std::max(1, visible);

    if (l.pos.sel < l.pos.top) l.pos.top = l.pos.sel;
    if (l.pos.sel >= l.pos.top + l.visible) l.pos.top = l.pos.sel - l.visible + 1;

    l.pos.top = std::max(0, std::min(l.pos.top, std::max(0, (int)l.mods.size() - l.visible)));
}

// The loaded mods come first; failed ones after them.
inline int loaded_count(const List& l) {
    return (int)std::count_if(l.mods.begin(), l.mods.end(), [](const Mod& m) { return m.failed.empty(); });
}

// Moves the carried mod to index (clamped to the loaded mods), shifting the ones between.
inline void carry_to(List& l, int index, int visible)
{

    if (l.carrying < 0)
    {
        return;
    }

    const int to = std::clamp(index, 0, loaded_count(l) - 1);
    Mod m = std::move(l.mods[l.carrying]);

    l.mods.erase(l.mods.begin() + l.carrying);
    l.mods.insert(l.mods.begin() + to, std::move(m));
    l.carrying = to;
    l.pos.sel = to;
    fit_window(l, visible);
}

inline int carry_begin(List& l)
{

    if (l.mods.empty())
    {
        return -1;
    }

    l.pos.sel = std::clamp(l.pos.sel, 0, (int)l.mods.size() - 1);

    if (!l.mods[l.pos.sel].failed.empty())
    {
        return -1;
    }

    l.carrying = l.carry_from = l.pos.sel;

    return l.carrying;
}

inline void carry_step(List& l, int dir, int visible)
{

    if (l.carrying < 0)
    {
        return;
    }

    const int to = l.carrying + (dir < 0 ? -1 : 1);

    if (to < 0 || to >= loaded_count(l))
    {
        return;
    }

    carry_to(l, to, visible);
}

struct Drop { int from; int to; };

inline Drop carry_drop(List& l)
{

    const Drop d{ l.carry_from, l.carrying };

    l.carrying = l.carry_from = -1;

    return d;
}

inline void carry_cancel(List& l)
{

    if (l.carrying < 0)
    {
        return;
    }

    carry_to(l, l.carry_from, l.visible);
    l.carrying = l.carry_from = -1;
}

struct Window { int first; int count; bool more_above; bool more_below; };

inline Window window(const List& l, int visible)
{

    const int n = (int)l.mods.size();
    const int count = std::max(0, std::min(visible, n - l.pos.top));

    return { l.pos.top, count, l.pos.top > 0, l.pos.top + visible < n };
}

// Menu font text: upper case (it draws capitals only), only the characters it has (A-Z, digits, Ä Ö Ü and some punctuation; others are
// dropped rather than drawn as '?'), spaces collapsed, and cut to max ending in "..." when longer.
inline std::string menu_upper(const std::string& utf8, size_t max)
{

    static const std::string kPunct = " ;:_'/&\",!?-.<>{}";
    std::string s;

    for (size_t i = 0; i < utf8.size();)
    {
        const unsigned char b = (unsigned char)utf8[i];
        const size_t len = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : 4;
        unsigned int cp = len == 1 ? b : len == 2 ? (b & 0x1Fu) << 6 | ((unsigned char)utf8[std::min(i + 1, utf8.size() - 1)] & 0x3Fu) : 0x100;

        i += len;

        if ((cp >= 'a' && cp <= 'z') || (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7)) cp -= 0x20;

        const bool drawn = (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9') || cp == 0xC4 || cp == 0xD6 || cp == 0xDC || (cp < 0x80 && kPunct.find((char)cp) != std::string::npos);

        if (!drawn || (cp == ' ' && (s.empty() || s.back() == ' '))) continue;

        s += (char)cp;
    }

    while (!s.empty() && s.back() == ' ') s.pop_back();

    if (s.size() <= max) return s;

    s.resize(max - 3);

    while (!s.empty() && s.back() == ' ') s.pop_back();

    return s + "...";
}

// The longest start of s that fits max_w (by width, e.g. the font's measure), ending in "..." when cut; empty if not even "..." fits.
template <typename Width>
std::string fit_text(const std::string& s, Width width, int max_w)
{

    if (width(s) <= max_w)
    {
        return s;
    }

    for (size_t n = s.size(); n-- > 0;)
    {
        std::string t = s.substr(0, n);

        while (!t.empty() && t.back() == ' ') t.pop_back();

        t += "...";

        if (width(t) <= max_w) return t;
    }

    return {};
}

// s in up to max_lines lines of whole words that fit max_w, joined by '\n'; the last line takes the rest, cut with "..." (fit_text), and a
// word wider than a line is cut on its own line.
template <typename Width>
std::string wrap_text(const std::string& s, Width width, int max_w, int max_lines)
{

    std::vector<std::string> words;
    size_t at = 0;

    while (at < s.size())
    {
        const size_t end = std::min(s.find(' ', at), s.size());

        if (end > at) words.push_back(s.substr(at, end - at));

        at = end + 1;
    }

    std::string out;
    int lines = 0;

    for (size_t i = 0; i < words.size() && lines < max_lines; ++lines)
    {
        std::string line = words[i++];

        if (lines == max_lines - 1) {
            for (; i < words.size(); ++i) line += " " + words[i];
        } else {
            while (i < words.size() && width(line + " " + words[i]) <= max_w) line += " " + words[i++];
        }

        out += (lines ? "\n" : "") + fit_text(line, width, max_w);
    }

    return out;
}

inline std::string row_text(const Mod& m, const Words& w = {}) {
    return menu_upper(m.name, kNameMax) + "\t" + (!m.failed.empty() ? w.failed : m.enabled ? w.on : w.off);
}

// The cursor mod's description, or why it failed to open.
inline std::string description_text(const List& l, const Words& w)
{

    if (l.mods.empty()) return w.none;

    const Mod& m = l.mods[std::clamp(l.pos.sel, 0, (int)l.mods.size() - 1)];

    return menu_upper(m.failed.empty() ? m.description : m.failed, kDescMax);
}

struct Start {
    std::vector<std::string> order;
    std::vector<bool> enabled;
};

inline Start snapshot(const List& l)
{

    Start s;

    for (const Mod& m : l.mods)
    {
        s.order.push_back(m.id);
        s.enabled.push_back(m.enabled);
    }

    return s;
}

// A new order, or a mod that loads once turned on or off, takes a restart; a runtime-toggleable mod's switch applies at once.
inline bool needs_restart(const Start& s, const List& l)
{

    if (s.order.size() != l.mods.size())
    {
        return true;
    }

    for (size_t i = 0; i < l.mods.size(); ++i)
    {
        if (l.mods[i].id != s.order[i]) return true;
        if (!l.mods[i].runtime_toggleable && l.mods[i].enabled != s.enabled[i]) return true;
    }

    return false;
}

} // namespace rs64::mods_page
