#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#include "game/menus/menu_layout.h"

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

    std::string version = {};
    std::vector<std::string> authors = {};
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

// Moves mod k one place earlier (dir < 0) or later in the load order; returns its new index (k when it cannot move).
inline int move_mod(List& l, int k, int dir)
{

    if (k < 0 || k >= (int)l.mods.size() || l.carrying >= 0)
    {
        return k;
    }

    l.pos.sel = k;

    if (carry_begin(l) < 0)
    {
        return k;
    }

    carry_step(l, dir, l.visible);

    return carry_drop(l).to;
}

// A number option one step up (dir > 0) or down, kept in [min, max]; a schema without a step moves a tenth of the range.
inline double step_number(double v, double min, double max, double step, int dir)
{

    const double s = step > 0.0 ? step : (max - min) / 10.0;

    return std::clamp(v + (dir < 0 ? -s : s), min, max);
}

// A number option's value as shown, to its precision (digits after the point).
inline std::string number_text(double v, int precision)
{

    char buf[32];

    std::snprintf(buf, sizeof buf, "%.*f", std::clamp(precision, 0, 6), v);

    return buf;
}

// An RGBA8 image (w x h) scaled to cover out_w x out_h (each pixel the average of the source pixels it covers, the middle kept) as RGBA5551 tiles
// of tw x th: tiles row by row, each tile's pixels row by row.
inline std::vector<uint16_t> rgba5551_tiles(const uint8_t* px, int w, int h, int out_w, int out_h, int tw, int th)
{

    std::vector<uint16_t> out;

    if (!px || w <= 0 || h <= 0)
    {
        return out;
    }

    const double s = std::max((double)out_w / w, (double)out_h / h);
    const double x0 = (w - out_w / s) / 2.0, y0 = (h - out_h / s) / 2.0;

    for (int ty = 0; ty < out_h / th; ++ty)
    {
        for (int tx = 0; tx < out_w / tw; ++tx)
        {
            for (int y = 0; y < th; ++y)
            {
                for (int x = 0; x < tw; ++x)
                {
                    const int ox = tx * tw + x, oy = ty * th + y;
                    const int sx0 = std::clamp((int)(x0 + ox / s), 0, w - 1), sy0 = std::clamp((int)(y0 + oy / s), 0, h - 1);
                    const int sx1 = std::clamp((int)std::ceil(x0 + (ox + 1) / s), sx0 + 1, w), sy1 = std::clamp((int)std::ceil(y0 + (oy + 1) / s), sy0 + 1, h);
                    uint32_t sum[4] = {};

                    for (int yy = sy0; yy < sy1; ++yy)
                    {
                        for (int xx = sx0; xx < sx1; ++xx)
                        {
                            for (int c = 0; c < 4; ++c) sum[c] += px[((size_t)yy * w + xx) * 4 + c];
                        }
                    }

                    const uint32_t n = (uint32_t)((sx1 - sx0) * (sy1 - sy0));
                    const auto ch = [&](int c) { return (sum[c] + n / 2) / n; };

                    out.push_back((uint16_t)((ch(0) >> 3) << 11 | (ch(1) >> 3) << 6 | (ch(2) >> 3) << 1 | (ch(3) >= 128 ? 1 : 0)));
                }
            }
        }
    }

    return out;
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

// A source line this long or longer was hard-wrapped prose: the next line continues its paragraph. Shorter ones (a list of names) stay lines.
constexpr size_t kReflowMin = 60;

// A text file (CREDITS, LICENSE) as display lines: paragraphs upper-cased (menu_upper), kept to the characters keep() allows, wrapped to max_w,
// with a run of blank lines kept to one and none at either end.
template <typename Width, typename Keep>
std::vector<std::string> text_lines(const std::string& raw, Width width, int max_w, Keep keep)
{

    std::vector<std::string> paras;
    size_t prev_len = 0;

    for (size_t at = 0; at <= raw.size();)
    {
        const size_t end = std::min(raw.find('\n', at), raw.size());
        std::string line = raw.substr(at, end - at);

        at = end + 1;
        std::replace(line.begin(), line.end(), '\t', ' ');

        const bool blank = line.find_first_not_of(" \r") == std::string::npos;

        if (blank) {
            paras.push_back("");
        } else if (prev_len >= kReflowMin && !paras.empty() && !paras.back().empty()) {
            paras.back() += " " + line;
        } else {
            paras.push_back(line);
        }

        prev_len = blank ? 0 : line.size();
    }

    std::vector<std::string> out;

    for (const std::string& para : paras)
    {
        std::string t;

        for (const char c : menu_upper(para, para.size() + 3))
        {
            if (c == ' ' || keep(c)) t += c;
        }

        if (t.empty())
        {
            if (!out.empty() && !out.back().empty()) out.push_back("");

            continue;
        }

        const std::string w = wrap_text(t, width, max_w, (int)t.size());

        for (size_t p = 0; p <= w.size();)
        {
            const size_t q = std::min(w.find('\n', p), w.size());

            out.push_back(w.substr(p, q - p));
            p = q + 1;
        }
    }

    while (!out.empty() && out.back().empty()) out.pop_back();

    return out;
}

inline std::string row_text(const Mod& m, const Words& w = {}) {
    return menu_upper(m.name, kNameMax) + "\t" + (!m.failed.empty() ? w.failed : m.enabled ? w.on : w.off);
}

// The mod page's line above its description: "V1.2.0 - BY A, B", either part left out when the mod gives none.
inline std::string info_line(const Mod& m, const std::string& by)
{

    std::string names;

    for (const std::string& a : m.authors)
    {
        names += (names.empty() ? "" : ", ") + a;
    }

    std::string s = m.version.empty() ? "" : "V" + m.version;

    if (!names.empty()) s += (s.empty() ? "" : " - ") + by + " " + names;

    return menu_upper(s, kDescMax);
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
