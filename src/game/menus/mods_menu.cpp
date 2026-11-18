// The MODS page (main menu > MODS): every installed mod in load order with its ON/OFF state; a row opens that mod's page, which turns it on or off
// and moves it in the load order.
#include "game/menus/menu_config.h"
#include "game/menus/menu_config_internal.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif

#include "librecomp/mods.hpp"
#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "rs64_toml_hooks.h"
#include "game/menus/menu_layout.h"
#include "game/menus/mods_page.h"
#include "util/mips_call.h"
#include "stb/stb_image.h"

namespace rs64::menucfg {

namespace mp = rs64::mods_page;

// Slots: the mod rows, OPEN MODS FOLDER (desktop), BACK, then the description line (text-only entries go after the selectable ones).
#ifdef __ANDROID__
constexpr int kModsRows = 6;
constexpr bool kModsFolder = false;
#else
constexpr int kModsRows = 5;
constexpr bool kModsFolder = true;
#endif

// Rows sit below the page title, a little smaller than the menu text so a 16-letter name clears the ON/OFF column.
constexpr int kModsTitleY = 2;

// The description sits under the title, the rows below it.
constexpr int kModsRowsY = kModsTopY + 54;
constexpr float kModsRowScale = 0.8f;

// The description is white text in the front end's small font (hatten13, text group 8), in the slot the volume slider uses for OFF (the front
// end draws it on every menu page; SELECT GAME and the slider lay their own text in it): up to kModsDescLines lines of kModsDescWidth font pixels.
constexpr int kModsDescLines = 3, kModsDescWidth = 340, kModsDescGap = 5;
constexpr uint16_t kModsTabStop = 250, kModsEndStop = 340;

// A mod's page and its SETTINGS have wider rows than the list (a 14-letter name, "< 12 OF 14 >" or a 9-letter value between arrows); the game
// centres each row by its end stop, so they still line up. A tap on a value left of kSetValueSplit across the row steps it down, right of it up.
constexpr uint16_t kSetTabStop = 260, kSetEndStop = 450;
constexpr float kSetValueSplit = 0.76f;

// A mod's page: its info line and description take up to kModDescLines lines, ENABLED and PRIORITY sit under them, then BACK.
constexpr int kModDescLines = 6;
constexpr int kModRowsY = kModsTopY + 54 + 17 * (kModDescLines - kModsDescLines);
constexpr int kModEnabledSlot = 0, kModPrioritySlot = 1, kModRows = 2;

// The mod's own files a row opens as a text page, when it has them: CREDITS, then LICENSE.
constexpr const char* kModTextFiles[2] = { "CREDITS.txt", "LICENSE.txt" };

// The text page shows kTextLines lines of the file in the description slot from kModsTopY. Where those lines land on the 320x240 screen
// (measured): the first line's top and the pitch, for the scrollbar's track.
constexpr int kTextLines = 13;
constexpr float kTextTop240 = 47.0f, kTextPitch240 = 9.4f;


mp::Words mods_words()
{

    mp::Words w;

    w.on = mods_text("on", "ON", "AN", "OUI");
    w.off = mods_text("off", "OFF", "AUS", "NON");
    w.failed = mods_text("failed", "FAILED", "FEHLER", "ERREUR");
    w.none = mods_text("none", "NO MODS INSTALLED", "KEINE MODS INSTALLIERT", "AUCUN MOD INSTALLE");

    return w;
}

struct ModsScreen {
    bool shown = false;
    mp::List list;
    int rows = 0;
    int folder_slot = -1;
    int back_slot = -1;
    uint32_t bufs[kModsRows] = {};
    uint32_t back_buf = 0;
    uint32_t dirty = 0;
    bool desc_dirty = true;
    int last_slot = -1;
    bool restart_shown = false;
    mp::Words words;
};

ModsScreen s_mods;

// The mod page on screen: which mod (by id, as PRIORITY moves it), its rows' buffers and what to rewrite, the CREDITS / LICENSE slots
// (-1 without the file) and the slot to highlight when it opens (the text row it was left by).
struct ModScreen {
    bool shown = false;
    std::string id;
    uint32_t bufs[kModRows] = {};
    uint32_t back_buf = 0;
    int back_slot = kModRows;
    int settings_slot = -1;
    int text_slot[2] = { -1, -1 };
    int return_slot = 0;
    uint32_t dirty = 0;
    bool desc_dirty = true;
};

ModScreen s_modp;

// A mod's CREDITS or LICENSE page: its file as display lines and the first one shown.
struct TextScreen {
    bool shown = false;
    int which = 0;
    std::vector<std::string> lines;
    int top = 0;
    bool dirty = true;
};

TextScreen s_text;

// The SETTINGS page (below): on screen, its pad pass, A on a row, and leaving it.
bool mod_settings_on_screen(uint8_t* rdram);
void mod_settings_pass(uint8_t* rdram, recomp_context* ctx);
bool mod_settings_confirm(int e);
void mod_settings_hide();

// The value column's tab stop now set (0 when none); each page sets its own when it opens.
uint32_t s_mods_tab = 0;

// The mod whose page was opened and the MODS window then, so BACK returns to the same rows with that mod highlighted.
std::string s_mods_return;
int s_mods_return_top = 0;

// The order and on/off states when the game started: a page that differs from them shows RESTART REQUIRED.
bool s_mods_start_taken = false;
mp::Start s_mods_start;

bool mods_on_screen(uint8_t* rdram) {
    return s_mods.shown && rd_b(rdram, GCMD_OFF + 0x04) == page_id(kModsPage);
}

bool mod_page_on_screen(uint8_t* rdram) {
    return s_modp.shown && rd_b(rdram, GCMD_OFF + 0x04) == page_id(kModPage);
}

bool mod_text_on_screen(uint8_t* rdram) {
    return s_text.shown && rd_b(rdram, GCMD_OFF + 0x04) == page_id(kModTextPage);
}

// The MODS list, or a mod's text page with its lines' area.
bool mods_view(uint8_t* rdram, ListView* v)
{

    if (mod_text_on_screen(rdram))
    {
        const int shown = std::min(kTextLines, (int)s_text.lines.size());

        *v = { (int)s_text.lines.size(), kTextLines, s_text.top, (int)kTextTop240, (int)(kTextTop240 + kTextPitch240 * shown) };

        return true;
    }

    if (!mods_on_screen(rdram)) return false;

    *v = { (int)s_mods.list.mods.size(), s_mods.rows, s_mods.list.pos.top };

    return true;
}

// A mod file's text, empty when the mod has none.
std::string mod_file_text(const std::string& id, const char* file)
{

    bool exists = false;
    const std::vector<char> raw = recomp::mods::read_mod_file(id, file, exists);

    return exists ? std::string(raw.begin(), raw.end()) : std::string{};
}

std::string mod_text_title(int which) {
    return which == 0 ? mods_text("credits", "CREDITS", "MITWIRKENDE", "CREDITS") : mods_text("license", "LICENSE", "LIZENZ", "LICENCE");
}

void mods_mark_all() {

    s_mods.dirty = (1u << PAGE_ROWS) - 1;
    s_mods.desc_dirty = true;
}

// The open mod's index in the list, or -1.
int mod_page_index()
{

    const auto& v = s_mods.list.mods;
    const auto it = std::find_if(v.begin(), v.end(), [](const mp::Mod& m) { return m.id == s_modp.id; });

    return it == v.end() ? -1 : (int)(it - v.begin());
}

std::string mods_slot_text(int i)
{

    const mp::Window w = mp::window(s_mods.list, s_mods.rows);
    std::string t = mp::row_text(s_mods.list.mods[w.first + i], s_mods.words) + kCtlRowEnd;
    const bool more_above = i == 0 && w.more_above;
    const bool more_below = i == s_mods.rows - 1 && w.more_below;

    if (more_above || more_below) t.back() = more_above ? '^' : '#';

    return t;
}

// Text wrapped to the description lines, keeping only the characters hatten13 has.
std::string mods_wrap_desc(uint8_t* rdram, const std::string& text, int lines)
{

    std::string t;

    for (const char c : text)
    {
        if (c == ' ' || font_glyph(rdram, kModsDescFont, c) >= 0) t += c;
    }

    auto width = [rdram](const std::string& x) { return font_width(rdram, kModsDescFont, x, kModsDescGap); };

    return mp::wrap_text(t, width, kModsDescWidth, lines);
}

// A mod page's text: the version and authors line, then the whole description (or why the mod failed to open).
std::string mod_page_desc_text(uint8_t* rdram, const mp::Mod& m)
{

    const std::string info = mods_wrap_desc(rdram, mp::info_line(m, mods_text("by", "BY", "VON", "PAR")), 1);
    const std::string body = mods_wrap_desc(rdram, mp::menu_upper(m.failed.empty() ? m.description : m.failed, 600), kModDescLines - (info.empty() ? 0 : 1));

    if (info.empty() || body.empty()) return info + body;

    return info + "\n" + body;
}

// The laid-out description element while it is shown, else 0 (mods_frame keeps its alpha up while set).
uint32_t s_mods_desc_elem = 0;

// Lays the description out in its text group slot (setFontTextSlot 0x80063CFC, x -0x100 centres each line) in white at y; empty text, as on leaving
// the page, stops it being drawn. A page's note label uses the same slot.
void mods_draw_desc(uint8_t* rdram, recomp_context* ctx, const std::string& text, int y)
{

    constexpr size_t kDescBuf = 640;
    static uint32_t s_str = 0, s_white = 0;

    if (!s_str)
    {
        void* p = recomp::alloc(rdram, kDescBuf);
        void* c = recomp::alloc(rdram, 4);

        if (!p || !c) return;

        s_str = (uint32_t)((uint8_t*)p - rdram) + 0x80000000u;
        s_white = (uint32_t)((uint8_t*)c - rdram) + 0x80000000u;
        wr_w(rdram, s_white - 0x80000000u, (int32_t)0xFFFFFFFFu);
    }

    const size_t n = std::min<size_t>(text.size(), kDescBuf - 1);

    for (size_t i = 0; i <= n; ++i) rdram[(s_str - 0x80000000u + (uint32_t)i) ^ 3] = i < n ? (uint8_t)text[i] : 0;

    rs64::mips::mips_call(rdram, ctx, 0x80063CFCu, {kModsDescFont, s_str, kModsDescFontSlot, (uint32_t)-0x100}, {(uint32_t)y});

    rs64::mips::mips_call(rdram, ctx, 0x80063E54u, {kModsDescFont, kModsDescFontSlot, s_white});
    rs64::mips::mips_call(rdram, ctx, 0x80061F34u, {kModsDescFont, kModsDescFontSlot, 0x3F800000u});

    s_mods_desc_elem = text.empty() ? 0u : rs64::mips::mips_call(rdram, ctx, 0x80063C3Cu, {kModsDescFont, kModsDescFontSlot}).v0;
}

// The BACK line: BACK, then " - RESTART REQUIRED" once the order or a mod that loads once changed. It is an entry, so it updates in place
// (the game draws the title as it was when the page opened).
std::string mods_back_text()
{

    std::string t = back_text();

    if (s_mods.restart_shown) t += " - " + mods_text("restart", "RESTART REQUIRED", "NEUSTART ERFORDERLICH", "REDEMARRAGE REQUIS");

    return t;
}

// Slot i at absolute y: the game places entry i at list_base + 36 i - 18 count + offset.
int16_t mods_offset(int16_t base, int count, int i, int y) {
    return (int16_t)(y - base - 36 * i + 18 * count);
}

int mods_install(uint8_t* rdram, uint8_t parent)
{

    for (int i = 0; i < MAX_ENTRIES; ++i) s_slot[i] = {};

    s_mods = ModsScreen{};
    s_mods.shown = true;
    s_modp.shown = false;
    s_mods_tab = 0;

    // The rows are one entry each: a shared-row layout left from another page must not steer this one (page_row_nav).
    s_page_rows = false;

    s_mods.words = mods_words();

    for (const auto& d : recomp::mods::get_all_mod_details("rs64"))
    {
        mp::Mod m{ d.mod_id, d.display_name.empty() ? d.mod_id : d.display_name, d.description, recomp::mods::is_mod_enabled(d.mod_id), d.runtime_toggleable };

        m.version = d.version.to_string();
        m.authors = d.authors;
        s_mods.list.mods.push_back(std::move(m));
    }

    // Mods the start-up scan could not open list last, by file name, with the reason as their description.
    for (const auto& e : recomp::mods::get_mod_open_errors())
    {
        const std::string file = e.mod_path.filename().string();
        const std::string why = recomp::mods::error_to_string(e.error) + (e.error_param.empty() ? "" : ": " + e.error_param);

        s_mods.list.mods.push_back({ file, file, "", false, false, why });
    }

    if (!s_mods_start_taken)
    {
        s_mods_start = mp::snapshot(s_mods.list);
        s_mods_start_taken = true;
    }

    s_mods.restart_shown = mp::needs_restart(s_mods_start, s_mods.list);

    s_mods.rows = std::min(kModsRows, (int)s_mods.list.mods.size());
    s_mods.folder_slot = kModsFolder ? s_mods.rows : -1;
    s_mods.back_slot = s_mods.rows + (kModsFolder ? 1 : 0);

    // Back from a mod's page: the same rows, that mod highlighted (PRIORITY may have moved it).
    int cursor = 0;

    if (!s_mods_return.empty())
    {
        mp::List& l = s_mods.list;
        const auto it = std::find_if(l.mods.begin(), l.mods.end(), [](const mp::Mod& m) { return m.id == s_mods_return; });

        if (it != l.mods.end())
        {
            l.pos = { (int)(it - l.mods.begin()), s_mods_return_top };
            mp::fit_window(l, s_mods.rows);
            cursor = l.pos.sel - l.pos.top;
        }

        s_mods_return.clear();
    }

    const int count = s_mods.back_slot + 1;
    const int16_t base = rd_h(rdram, GCMD_OFF + 0x96);
    const int32_t sc = 0x3F800000;
    int32_t row_sc = 0;

    std::memcpy(&row_sc, &kModsRowScale, 4);

    // The game titles sit at y 10 on an 8-entry page and move 18 per entry fewer; MODS sits 8 higher, at any entry count. A mod's own title override still wins.
    wr_w(rdram, GCMD_OFF + 0x00, (int32_t)alloc_str(rdram, mods_text("title", "MODS", "MODS", "MODS")));
    wr_h(rdram, GCMD_OFF + 0x52, (int16_t)(kModsTitleY + 18 * (count - PAGE_ROWS)));
    apply_title(rdram, kModsPage);

    const uint8_t mod_page = page_id(kModPage);

    for (int i = 0; i < s_mods.rows; ++i)
    {
        s_mods.bufs[i] = live_buffer(rdram, "mods#" + std::to_string(i));

        if (!s_mods.bufs[i]) return -1;

        write_live(rdram, s_mods.bufs[i], mods_slot_text(i));
        write_slot(rdram, i, Slot{ (int32_t)s_mods.bufs[i], SUBTYPE_SUBMENU, mod_page, (int16_t)kCtlX, mods_offset(base, count, i, kModsRowsY + kCtlPitch * i), row_sc });
    }

    int y = kModsRowsY + kCtlPitch * kModsRows + 2 * kCtlBackGap;

    if (kModsFolder)
    {
        const std::string folder = mods_text("folder", "OPEN MODS FOLDER", "MOD-ORDNER \303\226FFNEN", "OUVRIR LE DOSSIER DES MODS");

        write_slot(rdram, s_mods.folder_slot, Slot{ (int32_t)alloc_str(rdram, folder), SUBTYPE_HOST, 0, 0, mods_offset(base, count, s_mods.folder_slot, y), sc });
        y += kCtlPitch + 2 * kCtlBackGap;
    }

    s_mods.back_buf = live_buffer(rdram, "mods#back");

    if (!s_mods.back_buf) return -1;

    write_live(rdram, s_mods.back_buf, mods_back_text());
    write_slot(rdram, s_mods.back_slot, Slot{ (int32_t)s_mods.back_buf, SUBTYPE_SUBMENU, parent, 0, mods_offset(base, count, s_mods.back_slot, y), sc });
    s_slot[s_mods.back_slot].sound = kSfxBack;
    wr_b(rdram, GCMD_OFF + 0x95, (uint8_t)count);
    wr_b(rdram, GCMD_OFF + 0x94, (uint8_t)cursor);
    wr_b(rdram, GCMD_OFF + 0x05, parent);

    fprintf(stderr, "[mods] page: %zu mods, %d rows\n", s_mods.list.mods.size(), s_mods.rows);

    return count;
}

std::string mod_page_row_text(int slot, const mp::Mod& m, int k)
{

    if (slot == kModEnabledSlot)
    {
        const std::string& v = !m.failed.empty() ? s_mods.words.failed : m.enabled ? s_mods.words.on : s_mods.words.off;

        return mods_text("enabled", "ENABLED", "AKTIV", "ACTIF") + "\t" + v + kCtlRowEnd;
    }

    // The menu font has no slash.
    const std::string of = " " + mods_text("of", "OF", "VON", "SUR") + " ";
    const std::string v = m.failed.empty() ? "< " + std::to_string(k + 1) + of + std::to_string(mp::loaded_count(s_mods.list)) + " >" : "-";

    return mods_text("priority", "PRIORITY", "PRIORIT\303\204T", "PRIORITE") + "\t" + v + kCtlRowEnd;
}

// The open mod's page: its full name as the title, ENABLED and PRIORITY, CREDITS and LICENSE when it has them, then BACK to the MODS list.
int mod_page_install(uint8_t* rdram, uint8_t parent)
{

    for (int i = 0; i < MAX_ENTRIES; ++i) s_slot[i] = {};

    s_page_rows = false;
    s_text.shown = false;

    const int k = mod_page_index();

    if (k < 0) return -1;

    const mp::Mod& m = s_mods.list.mods[k];
    int next = kModRows;

    s_modp.settings_slot = recomp::mods::get_mod_config_schema(m.id).options.empty() ? -1 : next++;

    for (int w = 0; w < 2; ++w)
    {
        s_modp.text_slot[w] = mod_file_text(m.id, kModTextFiles[w]).empty() ? -1 : next++;
    }

    s_modp.back_slot = next;

    const int count = s_modp.back_slot + 1;
    const int16_t base = rd_h(rdram, GCMD_OFF + 0x96);
    const uint8_t text_page = page_id(kModTextPage);
    int32_t row_sc = 0;

    std::memcpy(&row_sc, &kModsRowScale, 4);

    s_modp.shown = true;
    s_modp.dirty = 0;
    s_modp.desc_dirty = true;
    s_mods_tab = 0;

    wr_w(rdram, GCMD_OFF + 0x00, (int32_t)alloc_str(rdram, mp::menu_upper(m.name, 24)));
    wr_h(rdram, GCMD_OFF + 0x52, (int16_t)(kModsTitleY + 18 * (count - PAGE_ROWS)));

    for (int i = 0; i < kModRows; ++i)
    {
        s_modp.bufs[i] = live_buffer(rdram, "mod#" + std::to_string(i));

        if (!s_modp.bufs[i]) return -1;

        write_live(rdram, s_modp.bufs[i], mod_page_row_text(i, m, k));
        write_slot(rdram, i, Slot{ (int32_t)s_modp.bufs[i], SUBTYPE_HOST, 0, (int16_t)kCtlX, mods_offset(base, count, i, kModRowsY + kCtlPitch * i), row_sc });
    }

    int y = kModRowsY + kCtlPitch * kModRows + 2 * kCtlBackGap;

    if (s_modp.settings_slot >= 0)
    {
        const std::string label = mods_text("settings", "SETTINGS", "EINSTELLUNGEN", "REGLAGES");

        write_slot(rdram, s_modp.settings_slot, Slot{ (int32_t)alloc_str(rdram, label), SUBTYPE_SUBMENU, page_id(kModSettingsPage), 0, mods_offset(base, count, s_modp.settings_slot, y), 0x3F800000 });
        y += kCtlPitch + kCtlBackGap;
    }

    for (int w = 0; w < 2; ++w)
    {
        const int slot = s_modp.text_slot[w];

        if (slot < 0) continue;

        write_slot(rdram, slot, Slot{ (int32_t)alloc_str(rdram, mod_text_title(w)), SUBTYPE_SUBMENU, text_page, 0, mods_offset(base, count, slot, y), 0x3F800000 });
        y += kCtlPitch + kCtlBackGap;
    }

    s_modp.back_buf = live_buffer(rdram, "mod#back");

    if (!s_modp.back_buf) return -1;

    const int back = s_modp.back_slot;

    s_mods.restart_shown = mp::needs_restart(s_mods_start, s_mods.list);
    write_live(rdram, s_modp.back_buf, mods_back_text());
    write_slot(rdram, back, Slot{ (int32_t)s_modp.back_buf, SUBTYPE_SUBMENU, parent, 0, mods_offset(base, count, back, y + (back > kModRows ? kCtlBackGap : 0)), 0x3F800000 });
    s_slot[back].sound = kSfxBack;
    wr_b(rdram, GCMD_OFF + 0x95, (uint8_t)count);
    wr_b(rdram, GCMD_OFF + 0x94, (uint8_t)std::min(s_modp.return_slot, back));
    wr_b(rdram, GCMD_OFF + 0x05, parent);
    s_modp.return_slot = 0;

    fprintf(stderr, "[mods] mod page: %s\n", m.id.c_str());

    return count;
}

// The cursor as one position in [mods..., folder, back].
int mods_cursor(uint8_t* rdram)
{

    const int slot = rd_b(rdram, GCMD_OFF + 0x94), n = (int)s_mods.list.mods.size();

    if (slot < s_mods.rows) return std::min(s_mods.list.pos.top + slot, n - 1);
    if (slot == s_mods.folder_slot) return n;

    return n + (kModsFolder ? 1 : 0);
}

void mods_set_cursor(uint8_t* rdram, int v)
{

    const int n = (int)s_mods.list.mods.size(), top = s_mods.list.pos.top;
    int slot = s_mods.back_slot;

    if (v < n)
    {
        s_mods.list.pos.sel = v;
        mp::fit_window(s_mods.list, s_mods.rows);
        slot = v - s_mods.list.pos.top;
    } else if (kModsFolder && v == n) {
        slot = s_mods.folder_slot;
    }

    if (s_mods.list.pos.top != top) mods_mark_all();

    wr_b(rdram, GCMD_OFF + 0x94, (uint8_t)slot);
}

// A scrollbar drag: the rows move under the highlight, which keeps its place on screen.
void mods_scroll_to(uint8_t* rdram, int top)
{

    mp::List& l = s_mods.list;

    top = std::clamp(top, 0, std::max(0, (int)l.mods.size() - s_mods.rows));

    if (top == l.pos.top) return;

    const int slot = rd_b(rdram, GCMD_OFF + 0x94);

    l.pos.top = top;

    if (slot < s_mods.rows) l.pos.sel = top + slot;

    mods_mark_all();
}

// ENABLED flips the open mod (dir unused); PRIORITY moves it one place earlier (dir < 0) or later, saving the new order. False when nothing changed.
bool mod_page_change(int slot, int dir)
{

    const int k = mod_page_index();

    if (k < 0 || !s_mods.list.mods[k].failed.empty()) return false;

    mp::Mod& m = s_mods.list.mods[k];

    if (slot == kModEnabledSlot)
    {
        m.enabled = !m.enabled;
        recomp::mods::enable_mod(m.id, m.enabled);

        fprintf(stderr, "[mods] %s %s\n", m.id.c_str(), m.enabled ? "on" : "off");
    } else if (slot == kModPrioritySlot) {
        const int to = mp::move_mod(s_mods.list, k, dir);

        if (to == k) return false;

        recomp::mods::set_mod_index("rs64", s_modp.id, (size_t)to);

        fprintf(stderr, "[mods] moved %s from %d to %d\n", s_modp.id.c_str(), k, to);
    } else {
        return false;
    }

    s_modp.dirty |= (1u << kModRows) - 1;

    return true;
}

// The tab stops the page's rows use: the value column at `tab`, the row's end at `end`.
void mods_set_tabs(uint8_t* rdram, recomp_context* ctx, uint16_t tab, uint16_t end = kModsEndStop)
{

    static uint32_t s_stops = 0;
    const uint32_t key = tab | (uint32_t)end << 16;

    if (s_mods_tab == key) return;

    if (!s_stops) {
        void* p = recomp::alloc(rdram, 4);

        if (!p) return;

        s_stops = (uint32_t)((uint8_t*)p - rdram) + 0x80000000u;
    }

    wr_h(rdram, s_stops - 0x80000000u, (int16_t)tab);
    wr_h(rdram, s_stops - 0x80000000u + 2, (int16_t)end);
    rs64::mips::mips_call(rdram, ctx, 0x80063E98u, {5u, s_stops, 2u});
    s_mods_tab = key;
    mods_mark_all();
    s_modp.dirty |= (1u << kModRows) - 1;
}

// The mod page's pad pass: left / right flip ENABLED or move the mod in PRIORITY; up / down are the game's.
void mod_page_pass(uint8_t* rdram, recomp_context* ctx)
{

    const uint32_t p = (uint32_t)rd_w(rdram, PAD_PRESSED);

    if (!(p & (kNavLeft | kNavRight))) return;

    wr_w(rdram, PAD_PRESSED, (int32_t)(p & ~(kNavLeft | kNavRight)));

    if (mod_page_change(rd_b(rdram, GCMD_OFF + 0x94), (p & kNavLeft) ? -1 : 1)) rs64::mips::mips_call(rdram, ctx, 0x80087EE8u, {0x17u, 0x7Fu, 0x40u});
}

// The text page's first line: clamped so the last page of lines fills the area.
void mod_text_scroll_to(int top)
{

    top = std::clamp(top, 0, std::max(0, (int)s_text.lines.size() - kTextLines));

    if (top == s_text.top) return;

    s_text.top = top;
    s_text.dirty = true;
}

// The text page's pad pass: up / down (and the wheel) scroll a line, stopping at the ends; the scrollbar sets the first line.
void mod_text_pass(uint8_t* rdram)
{

    const int want_top = s_list_scroll_req.exchange(-1);

    if (want_top >= 0) mod_text_scroll_to(want_top);

    uint32_t p = (uint32_t)rd_w(rdram, PAD_PRESSED);
    const int wheel = s_ctl_wheel.exchange(0);

    if (wheel) p |= wheel > 0 ? kNavUp : kNavDown;

    if (!(p & (kNavUp | kNavDown))) return;

    wr_w(rdram, PAD_PRESSED, (int32_t)(p & ~(kNavUp | kNavDown)));
    mod_text_scroll_to(s_text.top + ((p & kNavUp) ? -1 : 1));
}

// Each front-end pass before the menu reads the pad: tab stops on entry / exit, the scrollbar, and up / down through the list, stopping at its ends.
void mods_pass(uint8_t* rdram, recomp_context* ctx)
{

    const bool mod_page = mod_page_on_screen(rdram);
    const bool text_page = mod_text_on_screen(rdram);
    const bool settings_page = mod_settings_on_screen(rdram);

    if (!mods_on_screen(rdram) && !mod_page && !text_page && !settings_page)
    {
        if (s_mods_tab && !controls_on_screen(rdram)) rs64::mips::mips_call(rdram, ctx, 0x80063EC0u, {5u});

        if (s_mods.shown || s_modp.shown || s_text.shown) mods_draw_desc(rdram, ctx, "");

        s_mods.shown = false;
        s_mods_tab = 0;
        s_modp.shown = false;
        s_text.shown = false;
        mod_settings_hide();

        return;
    }

    if (text_page)
    {
        mod_text_pass(rdram);

        return;
    }

    if (settings_page)
    {
        mods_set_tabs(rdram, ctx, kSetTabStop, kSetEndStop);
        mod_settings_pass(rdram, ctx);

        return;
    }

    if (mod_page) {
        mods_set_tabs(rdram, ctx, kSetTabStop, kSetEndStop);
    } else {
        mods_set_tabs(rdram, ctx, kModsTabStop);
    }

    if (mod_page)
    {
        mod_page_pass(rdram, ctx);

        return;
    }

    const int want_top = s_list_scroll_req.exchange(-1);

    if (want_top >= 0) mods_scroll_to(rdram, want_top);

    uint32_t p = (uint32_t)rd_w(rdram, PAD_PRESSED);

    // Mouse wheel notches step the cursor like up / down.
    const int wheel = s_ctl_wheel.exchange(0);

    if (wheel) p |= wheel > 0 ? kNavUp : kNavDown;

    if (!(p & (kNavUp | kNavDown | kNavLeft | kNavRight))) return;

    wr_w(rdram, PAD_PRESSED, (int32_t)(p & ~(kNavUp | kNavDown | kNavLeft | kNavRight)));

    if (!(p & (kNavUp | kNavDown))) return;

    const int total = (int)s_mods.list.mods.size() + (kModsFolder ? 2 : 1);
    const int to = mods_cursor(rdram) + ((p & kNavUp) ? -1 : 1);
    // The list stops at its ends: wrapping would jump a long list back to the top.
    if (to < 0 || to >= total) return;

    mods_set_cursor(rdram, to);
    s_mods.desc_dirty = true;
    rs64::mips::mips_call(rdram, ctx, 0x80087EE8u, {0x17u, 0x7Fu, 0x40u});
}

// A on the MODS page: a mod row opens its page (the row's own transition; this notes which mod), OPEN MODS FOLDER opens it. On the mod page,
// ENABLED flips and a tap on PRIORITY's value moves the mod toward the side tapped. BACK is the game's own.
bool mods_confirm(uint8_t* rdram, int e)
{

    if (mod_settings_on_screen(rdram)) return mod_settings_confirm(e);

    if (mod_page_on_screen(rdram))
    {
        if (e == kModEnabledSlot)
        {
            mod_page_change(e, 0);

            return true;
        }

        // SETTINGS, CREDITS, LICENSE: the row's own transition opens the page; this notes which file, and the row to come back to.
        if (e == s_modp.settings_slot)
        {
            s_modp.return_slot = e;

            return true;
        }

        for (int w = 0; w < 2; ++w)
        {
            if (e != s_modp.text_slot[w]) continue;

            s_text.which = w;
            s_modp.return_slot = e;

            return true;
        }

        const bool tapped = SDL_GetTicks() - s_click_ms.load() < 1000 && s_click_key.load() == e;

        if (e != kModPrioritySlot) return false;

        const float f = s_click_frac.load();

        if (tapped && f >= (float)kSetTabStop / kSetEndStop)
        {
            s_click_key.store(-1);
            mod_page_change(e, f < kSetValueSplit ? -1 : 1);
        }

        return true;
    }

    if (!mods_on_screen(rdram)) return false;

    if (e == s_mods.folder_slot)
    {
        fire_action("open_mods_folder");

        return true;
    }

    if (e < 0 || e >= s_mods.rows || s_mods.list.mods.empty()) return false;

    const int k = std::min(s_mods.list.pos.top + e, (int)s_mods.list.mods.size() - 1);

    s_modp.id = s_mods.list.mods[k].id;
    s_modp.return_slot = 0;
    s_mods_return = s_modp.id;
    s_mods_return_top = s_mods.list.pos.top;

    return true;
}

// ---- A mod's image (thumb.png) on its page ----

// A model drawn as CONTROLLER SETTINGS draws its controller (0x800B9994 submits *0x800CF0EC, called as at 0x800B7F30): a 2x3 grid of squares on
// 64x32 RGBA16 textures modimg_0..5 (src/game/menus/mod_image), into which each mod's thumb.png is written.
constexpr uint32_t kMenuModel = 0x000CF0ECu, kMenuModelScene = 0x800CF090u, kMenuModelArg = 0x000CF074u;
constexpr int kImgCols = 2, kImgRows = 3, kImgTileW = 64, kImgTileH = 32;

uint32_t s_img_mesh = 0;
std::string s_img_id;
bool s_img_has = false;

// The texture record table (*0x80128F08, *0x80128EF0 records of 0x24: name at +0x14, pixels at +0x10).
uint32_t mod_image_texture(uint8_t* rdram, const std::string& name)
{

    const uint32_t table = (uint32_t)rd_w(rdram, 0x00128F08u) - 0x80000000u;
    const int count = (uint16_t)rd_h(rdram, 0x00128EF0u);

    for (int r = 0; r < count; ++r)
    {
        const uint32_t rec = table + (uint32_t)r * 0x24u;
        bool same = true;

        for (size_t i = 0; i <= name.size() && same; ++i) same = rd_b(rdram, rec + 0x14 + (uint32_t)i) == (i < name.size() ? (uint8_t)name[i] : 0);

        if (same) return (uint32_t)rd_w(rdram, rec + 0x10) - 0x80000000u;
    }

    return 0;
}

// The tiles' pixels from the mod's thumb.png (each texture stores odd rows with their 4-byte words swapped, as the game's loader leaves them);
// false when the mod has no image or the model's textures are not loaded.
bool mod_image_write(uint8_t* rdram, const std::string& id)
{

    const std::vector<char>& png = recomp::mods::get_mod_thumbnail(id);
    int w = 0, h = 0;
    stbi_uc* px = png.empty() ? nullptr : stbi_load_from_memory((const stbi_uc*)png.data(), (int)png.size(), &w, &h, nullptr, 4);

    if (!px) return false;

    const std::vector<uint16_t> tiles = mp::rgba5551_tiles(px, w, h, kImgCols * kImgTileW, kImgRows * kImgTileH, kImgTileW, kImgTileH);

    stbi_image_free(px);

    for (int t = 0; t < kImgCols * kImgRows; ++t)
    {
        const uint32_t data = mod_image_texture(rdram, "modimg_" + std::to_string(t));

        if (!data) return false;

        for (int y = 0; y < kImgTileH; ++y)
        {
            for (int x = 0; x < kImgTileW; ++x)
            {
                const uint32_t at = (uint32_t)(y * kImgTileW + x) * 2 ^ (y & 1 ? 4u : 0u);

                wr_h(rdram, data + at, (int16_t)tiles[((size_t)t * kImgTileH + y) * kImgTileW + x]);
            }
        }
    }

    return true;
}

// loadControllerModel (rs64_controls_load_models): the image model loads beside the controller, as the controls mod's models do.
void mods_image_load(uint8_t* rdram, recomp_context* ctx)
{

    s_img_mesh = 0;
    s_img_id.clear();
    rs64::mips::mips_call(rdram, ctx, 0x8005645Cu, {alloc_str(rdram, "frontend/controller/mod_image"), 0u, 0u, 0u});

    const uint32_t mesh = rs64::mips::mips_call(rdram, ctx, 0x80056EB0u, {alloc_str(rdram, "mod_image")}).v0;

    if (!mesh)
    {
        fprintf(stderr, "[mods] image model not found\n");

        return;
    }

    rs64::mips::mips_call(rdram, ctx, 0x8005955Cu, {mesh, 0u, 0x01060000u, 0u});
    s_img_mesh = mesh;
}

// Each front-end frame on a mod page with an image: the model is submitted in the controller's place.
void mod_image_frame(uint8_t* rdram, recomp_context* ctx)
{

    if (!s_img_mesh) return;

    if (s_img_id != s_modp.id)
    {
        s_img_id = s_modp.id;
        s_img_has = mod_image_write(rdram, s_modp.id);
    }

    if (!s_img_has) return;

    const float shown = 1.0f;
    uint32_t shown_bits = 0;
    const int32_t keep = rd_w(rdram, kMenuModel);

    std::memcpy(&shown_bits, &shown, 4);
    wr_w(rdram, kMenuModel, (int32_t)s_img_mesh);
    rs64::mips::mips_call(rdram, ctx, 0x800B9994u, {kMenuModelScene, shown_bits, (uint32_t)rd_w(rdram, kMenuModelArg), 0u});
    wr_w(rdram, kMenuModel, keep);
}

// Each front-end frame on the mod page: its description, and the rows and BACK line that changed.
void mod_page_frame(uint8_t* rdram, recomp_context* ctx)
{

    const int k = mod_page_index();

    if (k < 0) return;

    if (s_modp.desc_dirty)
    {
        s_modp.desc_dirty = false;
        mods_draw_desc(rdram, ctx, mod_page_desc_text(rdram, s_mods.list.mods[k]));
    }

    if (s_mods_desc_elem) rs64::mips::mips_call(rdram, ctx, 0x80061C74u, {kModsDescFont, kModsDescFontSlot, 0xFEu});

    if (!s_modp.dirty) return;

    for (int i = 0; i < kModRows; ++i)
    {
        if (s_modp.dirty & (1u << i)) write_live(rdram, s_modp.bufs[i], mod_page_row_text(i, s_mods.list.mods[k], k));
    }

    const bool restart = mp::needs_restart(s_mods_start, s_mods.list);

    if (restart != s_mods.restart_shown)
    {
        s_mods.restart_shown = restart;
        s_modp.dirty |= 1u << s_modp.back_slot;
        write_live(rdram, s_modp.back_buf, mods_back_text());
    }

    rs64::mips::mips_call(rdram, ctx, 0x800C4C8Cu, {GCMD, s_modp.dirty, 0u});
    s_modp.dirty = 0;
}

// The open mod's CREDITS or LICENSE: the file's lines in the description slot under the title, then BACK to the mod page.
int mod_text_install(uint8_t* rdram, uint8_t parent)
{

    for (int i = 0; i < MAX_ENTRIES; ++i) s_slot[i] = {};

    s_page_rows = false;

    const int which = s_text.which;
    auto keep = [rdram](char c) { return font_glyph(rdram, kModsDescFont, c) >= 0; };
    auto width = [rdram](const std::string& x) { return font_width(rdram, kModsDescFont, x, kModsDescGap); };

    s_text = TextScreen{};
    s_text.which = which;
    s_text.lines = mp::text_lines(mod_file_text(s_modp.id, kModTextFiles[which]), width, kModsDescWidth, keep);
    s_text.shown = true;
    s_modp.shown = false;

    const int16_t base = rd_h(rdram, GCMD_OFF + 0x96);
    const int back_y = kModsTopY + 17 * kTextLines + 2 * kCtlBackGap;

    wr_w(rdram, GCMD_OFF + 0x00, (int32_t)alloc_str(rdram, mod_text_title(which)));
    wr_h(rdram, GCMD_OFF + 0x52, (int16_t)(kModsTitleY + 18 * (1 - PAGE_ROWS)));
    write_slot(rdram, 0, Slot{ (int32_t)alloc_str(rdram, back_text()), SUBTYPE_SUBMENU, parent, 0, mods_offset(base, 1, 0, back_y), 0x3F800000 });
    s_slot[0].sound = kSfxBack;
    wr_b(rdram, GCMD_OFF + 0x95, 1);
    wr_b(rdram, GCMD_OFF + 0x94, 0);
    wr_b(rdram, GCMD_OFF + 0x05, parent);

    fprintf(stderr, "[mods] %s of %s: %zu lines\n", kModTextFiles[which], s_modp.id.c_str(), s_text.lines.size());

    return 1;
}

// Each front-end frame on the text page: the lines from top, rewritten when it scrolls.
void mod_text_frame(uint8_t* rdram, recomp_context* ctx)
{

    if (s_text.dirty)
    {
        std::string t;

        for (int i = s_text.top; i < std::min(s_text.top + kTextLines, (int)s_text.lines.size()); ++i)
        {
            t += (i == s_text.top ? "" : "\n") + s_text.lines[i];
        }

        s_text.dirty = false;
        mods_draw_desc(rdram, ctx, t);
    }

    if (s_mods_desc_elem) rs64::mips::mips_call(rdram, ctx, 0x80061C74u, {kModsDescFont, kModsDescFontSlot, 0xFEu});
}

// ---- A mod's SETTINGS: a row per config option (name, value), the highlighted option's description above them ----

// shortcut: a mod with more options than kSetRows shows the first ones only; scroll them like the MODS rows if a mod needs more.
constexpr int kSetRows = 6;

constexpr size_t kSetNameMax = 14, kSetValueMax = 9;

struct SettingsScreen {
    bool shown = false;
    int rows = 0;
    uint32_t bufs[kSetRows] = {};
    uint32_t dirty = 0;
    int last_slot = -1;
};

SettingsScreen s_set;

bool mod_settings_on_screen(uint8_t* rdram) {
    return s_set.shown && rd_b(rdram, GCMD_OFF + 0x04) == page_id(kModSettingsPage);
}

void mod_settings_hide() {
    s_set.shown = false;
}

const std::vector<recomp::mods::ConfigOption>& mod_settings_options() {
    return recomp::mods::get_mod_config_schema(s_modp.id).options;
}

// An option's value as its row shows it: a choice or number between step arrows, text as it is (it cannot be edited here).
std::string mod_setting_value(const recomp::mods::ConfigOption& o)
{

    using namespace recomp::mods;
    const ConfigValueVariant v = get_mod_config_value(s_modp.id, o.id);

    if (const auto* e = std::get_if<ConfigOptionEnum>(&o.variant))
    {
        const uint32_t* u = std::get_if<uint32_t>(&v);
        const uint32_t i = u ? *u : e->default_value;

        return "< " + mp::menu_upper(i < e->options.size() ? e->options[i] : "", kSetValueMax) + " >";
    }

    if (const auto* n = std::get_if<ConfigOptionNumber>(&o.variant))
    {
        const double* d = std::get_if<double>(&v);

        return "< " + mp::number_text(d ? *d : n->default_value, n->precision) + " >";
    }

    if (const auto* s = std::get_if<ConfigOptionString>(&o.variant))
    {
        const std::string* t = std::get_if<std::string>(&v);

        return mp::menu_upper(t ? *t : s->default_value, kSetValueMax + 4);
    }

    return {};
}

std::string mod_setting_row_text(int i)
{

    const recomp::mods::ConfigOption& o = mod_settings_options()[i];

    return mp::menu_upper(o.name, kSetNameMax) + "\t" + mod_setting_value(o) + kCtlRowEnd;
}

// Option i one step up (dir > 0) or down, stopping at the ends, saved; false when it cannot move (an end, or a text option).
bool mod_setting_step(int i, int dir)
{

    using namespace recomp::mods;

    if (i < 0 || i >= s_set.rows) return false;

    const ConfigOption& o = mod_settings_options()[i];
    const ConfigValueVariant v = get_mod_config_value(s_modp.id, o.id);

    if (const auto* e = std::get_if<ConfigOptionEnum>(&o.variant))
    {
        const uint32_t* u = std::get_if<uint32_t>(&v);
        const int cur = (int)(u ? *u : e->default_value);
        const int to = std::clamp(cur + (dir < 0 ? -1 : 1), 0, (int)e->options.size() - 1);

        if (to == cur) return false;

        set_mod_config_value(s_modp.id, o.id, (uint32_t)to);
    } else if (const auto* n = std::get_if<ConfigOptionNumber>(&o.variant)) {
        const double* d = std::get_if<double>(&v);
        const double cur = d ? *d : n->default_value;
        const double to = mp::step_number(cur, n->min, n->max, n->step, dir);

        if (to == cur) return false;

        set_mod_config_value(s_modp.id, o.id, to);
    } else {
        return false;
    }

    s_set.dirty |= 1u << i;

    fprintf(stderr, "[mods] %s %s: %s\n", s_modp.id.c_str(), o.id.c_str(), mod_setting_value(o).c_str());

    return true;
}

// The open mod's SETTINGS: its options under their descriptions, then BACK to the mod page.
int mod_settings_install(uint8_t* rdram, uint8_t parent)
{

    for (int i = 0; i < MAX_ENTRIES; ++i) s_slot[i] = {};

    s_page_rows = false;
    s_modp.shown = false;
    s_set = SettingsScreen{};
    s_set.rows = std::min(kSetRows, (int)mod_settings_options().size());
    s_set.shown = true;
    s_mods_tab = 0;

    const int count = s_set.rows + 1;
    const int16_t base = rd_h(rdram, GCMD_OFF + 0x96);
    int32_t row_sc = 0;

    std::memcpy(&row_sc, &kModsRowScale, 4);

    wr_w(rdram, GCMD_OFF + 0x00, (int32_t)alloc_str(rdram, mods_text("settings", "SETTINGS", "EINSTELLUNGEN", "REGLAGES")));
    wr_h(rdram, GCMD_OFF + 0x52, (int16_t)(kModsTitleY + 18 * (count - PAGE_ROWS)));

    for (int i = 0; i < s_set.rows; ++i)
    {
        s_set.bufs[i] = live_buffer(rdram, "modset#" + std::to_string(i));

        if (!s_set.bufs[i]) return -1;

        write_live(rdram, s_set.bufs[i], mod_setting_row_text(i));
        write_slot(rdram, i, Slot{ (int32_t)s_set.bufs[i], SUBTYPE_HOST, 0, (int16_t)kCtlX, mods_offset(base, count, i, kModsRowsY + kCtlPitch * i), row_sc });
    }

    const int back_y = kModsRowsY + kCtlPitch * s_set.rows + 2 * kCtlBackGap;

    write_slot(rdram, s_set.rows, Slot{ (int32_t)alloc_str(rdram, back_text()), SUBTYPE_SUBMENU, parent, 0, mods_offset(base, count, s_set.rows, back_y), 0x3F800000 });
    s_slot[s_set.rows].sound = kSfxBack;
    wr_b(rdram, GCMD_OFF + 0x95, (uint8_t)count);
    wr_b(rdram, GCMD_OFF + 0x94, 0);
    wr_b(rdram, GCMD_OFF + 0x05, parent);

    fprintf(stderr, "[mods] settings of %s: %d options\n", s_modp.id.c_str(), s_set.rows);

    return count;
}

// The SETTINGS pad pass: left / right step the highlighted option.
void mod_settings_pass(uint8_t* rdram, recomp_context* ctx)
{

    const uint32_t p = (uint32_t)rd_w(rdram, PAD_PRESSED);

    if (!(p & (kNavLeft | kNavRight))) return;

    wr_w(rdram, PAD_PRESSED, (int32_t)(p & ~(kNavLeft | kNavRight)));

    if (mod_setting_step(rd_b(rdram, GCMD_OFF + 0x94), (p & kNavLeft) ? -1 : 1)) rs64::mips::mips_call(rdram, ctx, 0x80087EE8u, {0x17u, 0x7Fu, 0x40u});
}

// A on an option steps it up; a tap on its value steps it toward the side tapped.
bool mod_settings_confirm(int e)
{

    if (e < 0 || e >= s_set.rows) return false;

    const bool tapped = SDL_GetTicks() - s_click_ms.load() < 1000 && s_click_key.load() == e;
    const float f = s_click_frac.load();

    if (!tapped)
    {
        mod_setting_step(e, 1);

        return true;
    }

    s_click_key.store(-1);

    if (f >= (float)kSetTabStop / kSetEndStop) mod_setting_step(e, f < kSetValueSplit ? -1 : 1);

    return true;
}

// Each front-end frame on SETTINGS: the highlighted option's description, and the rows that changed.
void mod_settings_frame(uint8_t* rdram, recomp_context* ctx)
{

    const int slot = rd_b(rdram, GCMD_OFF + 0x94);

    if (slot != s_set.last_slot)
    {
        // The first frame relays the rows out on the page's tab stops (set by the pass before it).
        if (s_set.last_slot < 0) s_set.dirty |= (1u << s_set.rows) - 1;

        s_set.last_slot = slot;

        const std::string desc = slot < s_set.rows ? mp::menu_upper(mod_settings_options()[slot].description, mp::kDescMax) : std::string{};

        mods_draw_desc(rdram, ctx, mods_wrap_desc(rdram, desc, kModsDescLines));
    }

    if (s_mods_desc_elem) rs64::mips::mips_call(rdram, ctx, 0x80061C74u, {kModsDescFont, kModsDescFontSlot, 0xFEu});

    if (!s_set.dirty) return;

    for (int i = 0; i < s_set.rows; ++i)
    {
        if (s_set.dirty & (1u << i)) write_live(rdram, s_set.bufs[i], mod_setting_row_text(i));
    }

    rs64::mips::mips_call(rdram, ctx, 0x800C4C8Cu, {GCMD, s_set.dirty, 0u});
    s_set.dirty = 0;
}

// Each front-end frame: rewrite changed rows and the description line, relaid out in place.
void mods_frame(uint8_t* rdram, recomp_context* ctx)
{

    if (mod_settings_on_screen(rdram))
    {
        mod_settings_frame(rdram, ctx);

        return;
    }

    if (mod_text_on_screen(rdram))
    {
        mod_text_frame(rdram, ctx);

        return;
    }

    if (mod_page_on_screen(rdram))
    {
        mod_image_frame(rdram, ctx);
        mod_page_frame(rdram, ctx);

        return;
    }

    if (!mods_on_screen(rdram)) return;

    // A mouse or touch hover moves the highlight without a pass: the description follows it.
    const int slot = rd_b(rdram, GCMD_OFF + 0x94);

    if (slot != s_mods.last_slot)
    {
        s_mods.last_slot = slot;

        if (slot < s_mods.rows) s_mods.list.pos.sel = s_mods.list.pos.top + slot;

        s_mods.desc_dirty = true;
    }

    // The description belongs to the mod under the cursor: OPEN MODS FOLDER and BACK show none (an empty list still says so).
    if (s_mods.desc_dirty)
    {
        s_mods.desc_dirty = false;
        mods_draw_desc(rdram, ctx, slot < s_mods.rows || s_mods.list.mods.empty() ? mods_wrap_desc(rdram, mp::description_text(s_mods.list, s_mods.words), kModsDescLines) : std::string{});
    }

    // The slot's alpha, set each frame as the volume slider does its OFF / MAX labels (0xFF would swap the slot's glyph set instead).
    if (s_mods_desc_elem) rs64::mips::mips_call(rdram, ctx, 0x80061C74u, {kModsDescFont, kModsDescFontSlot, 0xFEu});

    if (!s_mods.dirty) return;

    for (int i = 0; i < s_mods.rows; ++i)
    {
        if (s_mods.dirty & (1u << i)) write_live(rdram, s_mods.bufs[i], mods_slot_text(i));
    }

    const bool restart = mp::needs_restart(s_mods_start, s_mods.list);

    if (restart != s_mods.restart_shown)
    {
        s_mods.restart_shown = restart;
        s_mods.dirty |= 1u << s_mods.back_slot;
        write_live(rdram, s_mods.back_buf, mods_back_text());
    }

    rs64::mips::mips_call(rdram, ctx, 0x800C4C8Cu, {GCMD, s_mods.dirty, 0u});
    s_mods.dirty = 0;
}
} // namespace rs64::menucfg
