// The MODS page (main menu > MODS): every installed mod in load order, ON/OFF on the right, A carries a mod to a new place.
#include "game/menus/menu_config.h"
#include "game/menus/menu_config_internal.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
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

constexpr uint32_t kModsPadB = 0x00004000u;

// Rows sit below the page title, a little smaller than the menu text so a 16-letter name clears the ON/OFF column.
constexpr int kModsTitleY = 2;

// The description sits under the title, the rows below it.
constexpr int kModsRowsY = kModsTopY + 54;
constexpr float kModsRowScale = 0.8f;

// The description is white text in the front end's small font (hatten13, text group 8), in the slot the volume slider uses for OFF (the front
// end draws it on every menu page; SELECT GAME and the slider lay their own text in it): up to kModsDescLines lines of kModsDescWidth font pixels.
constexpr int kModsDescLines = 3, kModsDescWidth = 340, kModsDescGap = 5;
constexpr uint16_t kModsTabStop = 250, kModsEndStop = 340;

// A click or tap this far across a mod row (or further) lands on its ON/OFF part.
constexpr float kModsToggleFrom = 0.6f;

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
    bool tabs = false;
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

// The mod rows on screen (0 off the page), and drag steps from the SDL thread (mods_drag_event): { op, slot }, applied by the next pass.
std::atomic<int> s_mods_rows_shown{0};
std::mutex s_mods_drag_mtx;
std::vector<std::pair<int, int>> s_mods_drag_ops;
uint32_t s_mods_edge_ms = 0;
constexpr uint32_t kModsEdgeStepMs = 150;

// The order and on/off states when the game started: a page that differs from them shows RESTART REQUIRED.
bool s_mods_start_taken = false;
mp::Start s_mods_start;

bool mods_on_screen(uint8_t* rdram) {
    return s_mods.shown && rd_b(rdram, GCMD_OFF + 0x04) == page_id(kModsPage);
}

void mods_mark_all() {

    s_mods.dirty = (1u << MAX_ENTRIES) - 1;
    s_mods.desc_dirty = true;
}

std::string mods_slot_text(int i)
{

    const mp::Window w = mp::window(s_mods.list, s_mods.rows);
    const int k = w.first + i;
    std::string t = mp::row_text(s_mods.list.mods[k], s_mods.words);

    // The carried mod sits between an up and a down triangle ('^' and '#', ensure_menu_period) while it moves.
    if (k == s_mods.list.carrying)
    {
        t.insert(t.find('\t'), " #");
        t = "^ " + t;
    }

    t += kCtlRowEnd;

    const bool more_above = i == 0 && w.more_above;
    const bool more_below = i == s_mods.rows - 1 && w.more_below;

    if (more_above || more_below) t.back() = more_above ? '^' : '#';

    return t;
}

// The description wrapped to the description lines, keeping only the characters hatten13 has.
std::string mods_desc_text(uint8_t* rdram)
{

    std::string t;

    for (const char c : mp::description_text(s_mods.list, s_mods.words))
    {
        if (c == ' ' || font_glyph(rdram, kModsDescFont, c) >= 0) t += c;
    }

    auto width = [rdram](const std::string& x) { return font_width(rdram, kModsDescFont, x, kModsDescGap); };

    return mp::wrap_text(t, width, kModsDescWidth, kModsDescLines);
}

// The laid-out description element while it is shown, else 0 (mods_frame keeps its alpha up while set).
uint32_t s_mods_desc_elem = 0;

// Lays the description out in its text group slot (setFontTextSlot 0x80063CFC, x -0x100 centres each line) in white at y; empty text, as on leaving
// the page, stops it being drawn. A page's note label uses the same slot.
void mods_draw_desc(uint8_t* rdram, recomp_context* ctx, const std::string& text, int y)
{

    static uint32_t s_str = 0, s_white = 0;

    if (!s_str)
    {
        void* p = recomp::alloc(rdram, 260);
        void* c = recomp::alloc(rdram, 4);

        if (!p || !c) return;

        s_str = (uint32_t)((uint8_t*)p - rdram) + 0x80000000u;
        s_white = (uint32_t)((uint8_t*)c - rdram) + 0x80000000u;
        wr_w(rdram, s_white - 0x80000000u, (int32_t)0xFFFFFFFFu);
    }

    const size_t n = std::min<size_t>(text.size(), 259);

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

int mods_install(uint8_t* rdram, uint8_t parent)
{

    for (int i = 0; i < MAX_ENTRIES; ++i) s_slot[i] = {};

    s_mods = ModsScreen{};
    s_mods.shown = true;

    // The rows are one entry each: a shared-row layout left from another page must not steer this one (page_row_nav).
    s_page_rows = false;

    std::unique_lock<std::mutex> lk(s_mods_drag_mtx);

    s_mods_drag_ops.clear();
    lk.unlock();

    s_mods.words = mods_words();

    for (const auto& d : recomp::mods::get_all_mod_details("rs64"))
    {
        s_mods.list.mods.push_back({ d.mod_id, d.display_name.empty() ? d.mod_id : d.display_name, d.description,
                                     recomp::mods::is_mod_enabled(d.mod_id), d.runtime_toggleable });
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

    const int count = s_mods.back_slot + 1;
    const int16_t base = rd_h(rdram, GCMD_OFF + 0x96);
    const int32_t sc = 0x3F800000;
    int32_t row_sc = 0;

    std::memcpy(&row_sc, &kModsRowScale, 4);

    // Slot i at absolute y: the game places entry i at list_base + 36 i - 18 count + offset.
    auto offset = [&](int i, int y) { return (int16_t)(y - base - 36 * i + 18 * count); };

    // The game titles sit at y 10 on an 8-entry page and move 18 per entry fewer; MODS sits 8 higher, at any entry count. A mod's own title override still wins.
    wr_w(rdram, GCMD_OFF + 0x00, (int32_t)alloc_str(rdram, mods_text("title", "MODS", "MODS", "MODS")));
    wr_h(rdram, GCMD_OFF + 0x52, (int16_t)(kModsTitleY + 18 * (count - MAX_ENTRIES)));
    apply_title(rdram, kModsPage);

    for (int i = 0; i < s_mods.rows; ++i)
    {
        s_mods.bufs[i] = live_buffer(rdram, "mods#" + std::to_string(i));

        if (!s_mods.bufs[i]) return -1;

        write_live(rdram, s_mods.bufs[i], mods_slot_text(i));
        write_slot(rdram, i, Slot{ (int32_t)s_mods.bufs[i], SUBTYPE_HOST, 0, (int16_t)kCtlX, offset(i, kModsRowsY + kCtlPitch * i), row_sc });
    }

    int y = kModsRowsY + kCtlPitch * kModsRows + 2 * kCtlBackGap;

    if (kModsFolder)
    {
        const std::string folder = mods_text("folder", "OPEN MODS FOLDER", "MOD-ORDNER \303\226FFNEN", "OUVRIR LE DOSSIER DES MODS");

        write_slot(rdram, s_mods.folder_slot, Slot{ (int32_t)alloc_str(rdram, folder), SUBTYPE_HOST, 0, 0, offset(s_mods.folder_slot, y), sc });
        y += kCtlPitch + 2 * kCtlBackGap;
    }

    s_mods.back_buf = live_buffer(rdram, "mods#back");

    if (!s_mods.back_buf) return -1;

    write_live(rdram, s_mods.back_buf, mods_back_text());
    write_slot(rdram, s_mods.back_slot, Slot{ (int32_t)s_mods.back_buf, SUBTYPE_SUBMENU, parent, 0, offset(s_mods.back_slot, y), sc });
    s_slot[s_mods.back_slot].sound = kSfxBack;
    wr_b(rdram, GCMD_OFF + 0x95, (uint8_t)count);
    wr_b(rdram, GCMD_OFF + 0x94, 0);
    wr_b(rdram, GCMD_OFF + 0x05, parent);

    fprintf(stderr, "[mods] page: %zu mods, %d rows\n", s_mods.list.mods.size(), s_mods.rows);

    return count;
}

// The cursor as one position in [mods..., folder, back]: up / down step it, wrapping.
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

void mods_toggle(int k)
{

    mp::Mod& m = s_mods.list.mods[k];

    if (!m.failed.empty())
    {
        return;
    }

    m.enabled = !m.enabled;
    recomp::mods::enable_mod(m.id, m.enabled);
    mods_mark_all();

    fprintf(stderr, "[mods] %s %s\n", m.id.c_str(), m.enabled ? "on" : "off");
}

// Drag steps: 0 picks up the row's mod, 1 carries it to a row (-1 / 99: past the top / bottom row, one scroll step per kModsEdgeStepMs),
// 2 drops it (its new place saved), 3 puts it back.
void mods_drop(mp::List& l)
{

    const mp::Drop d = mp::carry_drop(l);

    if (d.from != d.to) recomp::mods::set_mod_index("rs64", l.mods[d.to].id, (size_t)d.to);

    fprintf(stderr, "[mods] moved %s from %d to %d\n", l.mods[d.to].id.c_str(), d.from, d.to);
}

void mods_apply_drags(uint8_t* rdram)
{

    std::unique_lock<std::mutex> lk(s_mods_drag_mtx);
    const std::vector<std::pair<int, int>> ops = std::exchange(s_mods_drag_ops, {});

    lk.unlock();

    mp::List& l = s_mods.list;

    for (const auto& [op, slot] : ops)
    {
        if (op == 0 && slot >= 0 && slot < s_mods.rows && l.carrying < 0) {
            l.pos.sel = l.pos.top + slot;
            mp::carry_begin(l);
        } else if (op == 1 && l.carrying >= 0) {
            const bool edge = slot < 0 || slot >= s_mods.rows;

            if (!edge) mp::carry_to(l, l.pos.top + slot, s_mods.rows);
            else if (SDL_GetTicks() - s_mods_edge_ms >= kModsEdgeStepMs) {
                s_mods_edge_ms = SDL_GetTicks();
                mp::carry_step(l, slot < 0 ? -1 : 1, s_mods.rows);
            }
        } else if (op == 2 && l.carrying >= 0) {
            mods_drop(l);
        } else if (op == 3) {
            mp::carry_cancel(l);
        } else {
            continue;
        }

        mods_set_cursor(rdram, l.pos.sel);
        mods_mark_all();
    }
}

// Each front-end pass before the menu reads the pad: tab stops on entry / exit, up / down through the list (or carrying a mod), left / right
// flipping ON/OFF, and B putting a carried mod back.
void mods_pass(uint8_t* rdram, recomp_context* ctx)
{

    if (!mods_on_screen(rdram))
    {
        if (s_mods.tabs && !controls_on_screen(rdram)) rs64::mips::mips_call(rdram, ctx, 0x80063EC0u, {5u});

        if (s_mods.shown) mods_draw_desc(rdram, ctx, "");

        s_mods.shown = false;
        s_mods.tabs = false;
        s_mods_rows_shown.store(0);

        return;
    }

    s_mods_rows_shown.store(s_mods.rows);
    mods_apply_drags(rdram);

    if (!s_mods.tabs)
    {
        static uint32_t s_stops = 0;

        if (!s_stops) {
            void* p = recomp::alloc(rdram, 4);

            if (!p) return;

            s_stops = (uint32_t)((uint8_t*)p - rdram) + 0x80000000u;
        }

        wr_h(rdram, s_stops - 0x80000000u, (int16_t)kModsTabStop);
        wr_h(rdram, s_stops - 0x80000000u + 2, (int16_t)kModsEndStop);
        rs64::mips::mips_call(rdram, ctx, 0x80063E98u, {5u, s_stops, 2u});
        s_mods.tabs = true;
        mods_mark_all();
    }

    uint32_t p = (uint32_t)rd_w(rdram, PAD_PRESSED);
    mp::List& l = s_mods.list;

    // Mouse wheel notches step the cursor (or carry a mod) like up / down.
    const int wheel = s_ctl_wheel.exchange(0);

    if (wheel) p |= wheel > 0 ? kNavUp : kNavDown;

    if (l.carrying >= 0 && (p & kModsPadB))
    {
        mp::carry_cancel(l);
        mods_set_cursor(rdram, l.pos.sel);
        mods_mark_all();
        wr_w(rdram, PAD_PRESSED, (int32_t)(p & ~kModsPadB));

        return;
    }

    if (!(p & (kNavUp | kNavDown | kNavLeft | kNavRight))) return;

    wr_w(rdram, PAD_PRESSED, (int32_t)(p & ~(kNavUp | kNavDown | kNavLeft | kNavRight)));

    const int n = (int)l.mods.size(), total = n + (kModsFolder ? 2 : 1), v = mods_cursor(rdram);

    if (l.carrying >= 0)
    {
        if (p & (kNavUp | kNavDown)) mp::carry_step(l, (p & kNavUp) ? -1 : 1, s_mods.rows);

        mods_set_cursor(rdram, l.pos.sel);
        mods_mark_all();
    } else if (!(p & (kNavUp | kNavDown))) {
        if (v < n) mods_toggle(v);
    } else {
        mods_set_cursor(rdram, (v + ((p & kNavUp) ? total - 1 : 1)) % total);
        s_mods.desc_dirty = true;
    }

    rs64::mips::mips_call(rdram, ctx, 0x80087EE8u, {0x17u, 0x7Fu, 0x40u});
}

// A on the page: a mod row picks the mod up (or drops the carried one; the new place is saved), or flips ON/OFF when the click or tap
// landed on that part; OPEN MODS FOLDER opens it. BACK is the game's own.
bool mods_confirm(uint8_t* rdram, int e)
{

    if (!mods_on_screen(rdram)) return false;

    mp::List& l = s_mods.list;

    if (e == s_mods.folder_slot)
    {
        fire_action("open_mods_folder");

        return true;
    }

    if (e < 0 || e >= s_mods.rows) return false;

    const int k = std::min(l.pos.top + e, (int)l.mods.size() - 1);
    const bool fresh = SDL_GetTicks() - s_click_ms.load() < 1000;

    // A click or tap: on ON/OFF it flips it; on the name it does nothing (mouse and touch carry by dragging, mods_drag_event), so only
    // A on a controller or keyboard picks a mod up.
    if (l.carrying < 0 && fresh && s_click_key.load() == e)
    {
        s_click_key.store(-1);

        if (s_click_frac.load() >= kModsToggleFrom) mods_toggle(k);

        return true;
    }

    if (l.carrying >= 0)
    {
        mods_drop(l);
    } else {
        l.pos.sel = k;
        mp::carry_begin(l);
    }

    mods_mark_all();

    return true;
}

// Each front-end frame: rewrite changed rows and the description line, relaid out in place.
void mods_frame(uint8_t* rdram, recomp_context* ctx)
{

    if (!mods_on_screen(rdram)) return;

    // A mouse or touch hover moves the highlight without a pass: the description follows it.
    const int slot = rd_b(rdram, GCMD_OFF + 0x94);

    if (slot != s_mods.last_slot)
    {
        s_mods.last_slot = slot;

        if (slot < s_mods.rows && s_mods.list.carrying < 0) s_mods.list.pos.sel = s_mods.list.pos.top + slot;

        s_mods.desc_dirty = true;
    }

    // The description belongs to the mod under the cursor: OPEN MODS FOLDER and BACK show none (an empty list still says so).
    if (s_mods.desc_dirty)
    {
        s_mods.desc_dirty = false;
        mods_draw_desc(rdram, ctx, slot < s_mods.rows || s_mods.list.mods.empty() ? mods_desc_text(rdram) : std::string{});
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

using namespace rs64::menucfg;

// The MODS page for mods_drag_event (input_host.cpp), any thread: whether a front-end entry is a mod row (its slot, else -1), where a
// row's ON/OFF part starts, and a drag step to queue (mods_apply_drags).
extern "C" int rs64_mods_page_row(int entry) {
    return entry >= 0 && entry < s_mods_rows_shown.load() ? entry : -1;
}

extern "C" float rs64_mods_toggle_from(void) {
    return kModsToggleFrom;
}

extern "C" void rs64_mods_drag(int op, int entry)
{

    std::lock_guard<std::mutex> lk(s_mods_drag_mtx);

    s_mods_drag_ops.emplace_back(op, entry);
}
