// In-mission pause menu: GAME SETTINGS and mod pages built from the menu button config (menu_config.cpp) into host-RAM PauseMenuStuff arrays.

#include "game/menus/menu_config_internal.h"
#include "game/menus/menu_config.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "os_compat.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif
#include "json/json.hpp"
#include "librecomp/mods.hpp"
#include "recomp.h"   // recomp_context, recomp_func_t (for mod-provided @exports)
#include "librecomp/addresses.hpp"
#include "rs64_toml_hooks.h"
#include "game/lang/lang.h"
#include "game/menus/menu_layout.h"
#include "game/menus/mods_page.h"
#include "video/video_config.h"
#include "main/main.h"   // rs64_menu_request_quit, rs64_toggle_fullscreen, rs64_get_fullscreen
#include "input/input_host.h"
#include "input/touch_config.h"
#include "util/mips_call.h"
#include "util/host_ram_map.h"
#include "host_api/host_api.h"
#include "util/debug_logs.h"

using namespace rs64::menucfg;

// Pause menu (in-mission): GAME SETTINGS (id 1) and each mod page (ids 4+) are host-RAM PauseMenuStuff arrays, rebuilt each paused frame; the root takes one button.
// Mod entries carry a nextMenu sentinel (0xF010 + button index) for the render/confirm hooks; links (flag 0x20) carry the target id, so their text is keyed by entry address.

namespace {

// Any valid textId; the render hook overrides the text.
constexpr uint16_t PAUSE_TEXTID    = 0x0002;
constexpr uint16_t PAUSE_MARK_BASE = 0xF010;
constexpr uint16_t PAUSE_BACK_TID  = 0x0039;
constexpr uint32_t PAUSE_ROOT      = 0x109DD4;
constexpr uint32_t PAUSE_GS        = 0x109E24;
constexpr uint32_t PAUSE_GS_SPACER = 0x109E54;
constexpr uint32_t PAUSE_HOST      = RS64_HR_PAUSE_MENU - 0x80000000u;
constexpr uint32_t PAUSE_HOST_END  = RS64_HR_PAUSE_MENU_END - 0x80000000u;
constexpr uint32_t PAUSE_STRIDE    = 0x80;
constexpr uint8_t  PAUSE_GS_ID     = 1;
constexpr uint8_t  PAUSE_FIRST_PAGE = 4;
constexpr int      PAUSE_MAX_LINES = 16;
constexpr int      PAUSE_MAX_GLYPHS = 0x12C;

struct PauseBinding {
    BType type;
    std::string key, label, label_on, label_off;
    recomp_func_t* action_fn = nullptr, * toggle_get_fn = nullptr, * toggle_set_fn = nullptr;
    int smin = 0, smax = 100, sstep = 10, bar_width = 10;
    std::string bar_filled = "|", bar_empty = ".";
};

std::unordered_map<uint16_t, PauseBinding> s_pause_marks;

// Native pause sliders (the AUDIO page's widget, pauseMenuAudioSettings 0x80109E68): title and endpoint rows carry nextMenu PAUSE_SLIDER_MARK + k,
// the two bar rows textId PAUSE_BAR_TID + k (valid text ids past the game's 0-2 channels), so the load/store/draw hooks find slider k and its channel byte.
constexpr uint16_t PAUSE_SLIDER_MARK = 0xF800;
constexpr uint16_t PAUSE_BAR_TID     = 3;
constexpr int      PAUSE_SLIDER_TOP  = 0x7F;
constexpr uint32_t SPEECH_BYTE       = 0x130B62;

struct PauseSlider {
    int channel = 0, smin = 0, smax = 100;
    std::string key, off_label, on_label;
    recomp_func_t* get_fn = nullptr, * set_fn = nullptr;
};

std::vector<PauseSlider> s_pause_sliders;
bool s_pause_seeded = false;
int s_pause_speech = -1;

const PauseSlider* pause_slider(uint16_t key, uint16_t base) {
    return key >= base && key - base < (int)s_pause_sliders.size() ? &s_pause_sliders[key - base] : nullptr;
}
std::unordered_map<uint32_t, std::string> s_pause_text;
std::unordered_map<uint8_t, uint8_t> s_pause_parent_of;
bool s_pause_built = false;
uint8_t s_back_from = 0;
int16_t s_back_cursor = 0;
bool s_back_wait = false;

uint8_t pause_page_id(const std::string& page)
{

    const uint8_t id = page_id(page);

    return id == 0xFF ? 0xFF : (uint8_t)(PAUSE_FIRST_PAGE + (id - FIRST_PAGE_ID));
}

const std::string* pause_page_name(uint8_t id)
{
    return id >= PAUSE_FIRST_PAGE ? page_name((uint8_t)(FIRST_PAGE_ID + (id - PAUSE_FIRST_PAGE))) : nullptr;
}

// The root's rebuilt array takes the last slot; GAME SETTINGS the first, mod pages the ones between.
constexpr uint8_t  PAUSE_ROOT_ID   = 0;
constexpr uint32_t PAUSE_ROOT_HOST = PAUSE_HOST_END - PAUSE_STRIDE;

uint32_t pause_array_off(uint8_t id)
{

    if (id == PAUSE_ROOT_ID) return PAUSE_ROOT_HOST;

    return id == PAUSE_GS_ID ? PAUSE_HOST : PAUSE_HOST + PAUSE_STRIDE * (1u + (id - PAUSE_FIRST_PAGE));
}

// The pause menus the port rebuilds into host RAM: the root, GAME SETTINGS and mod pages (not the game's audio settings or abort confirm).
bool pause_rebuilt(uint8_t id)
{
    return id == PAUSE_ROOT_ID || id == PAUSE_GS_ID || pause_page_name(id);
}

// One PauseMenuStuff array under construction, capped at the menu's 16 lines and 0x12C glyphs.
struct PauseWriter {
    uint8_t* rdram;
    uint32_t off;
    uint32_t limit;
    int lines = 0;
    int glyphs = 0;

    // Entries kept free for the ones that must follow (a page's spacer and BACK); reserve_lines counts only the drawn ones (-1: all of them).
    int reserve = 0;
    int reserve_lines = -1;

    int line_reserve() const {
        return reserve_lines >= 0 ? reserve_lines : reserve;
    }

    bool add(uint16_t flags, uint16_t tid, uint16_t next, const std::string& text)
    {

        const bool drawn = (flags & 0x1000) == 0;

        if ((drawn && lines + 1 + line_reserve() > PAUSE_MAX_LINES) || glyphs + (int)text.size() > PAUSE_MAX_GLYPHS) return false;
        if (off + 6u * (1u + (uint32_t)reserve) + 2u > limit) return false;

        wr_h(rdram, off + 0, (int16_t)flags);
        wr_h(rdram, off + 2, (int16_t)tid);
        wr_h(rdram, off + 4, (int16_t)next);

        if (!text.empty()) s_pause_text[0x80000000u | off] = text;

        off += 6;
        lines += drawn ? 1 : 0;
        glyphs += (int)text.size();

        return true;
    }

    void end() { wr_h(rdram, off, (int16_t)0xFFFF); }
};

// The visible buttons of a pause menu or page, in JSON order with `index` applied.
std::vector<const Button*> pause_lines(uint8_t* rdram, const std::string& menu)
{

    std::vector<const Button*> lines;

    for (const auto& b : config().buttons)
        if (b.menu == menu && button_visible(rdram, b, true)) lines.push_back(&b);

    for (const auto& b : config().buttons)
        if (b.menu == menu && b.has_index && button_visible(rdram, b, true))
        {
            const size_t from = (size_t)(std::find(lines.begin(), lines.end(), &b) - lines.begin());

            rs64::menu::move_to_index(lines, from, b.index);
        }

    return lines;
}

void add_pause_button(PauseWriter& w, const Button& b, uint8_t self)
{

    if (b.type == BType::Submenu)
    {
        const uint8_t id = pause_page_id(b.page);

        if (id == 0xFF || pause_array_off(id) + PAUSE_STRIDE > PAUSE_ROOT_HOST) return;

        s_pause_parent_of[id] = self;
        w.add(0x4021, PAUSE_TEXTID, id, b.label);

        return;
    }

    const uint16_t mark = (uint16_t)(PAUSE_MARK_BASE + (&b - config().buttons.data()));
    BehaviorFns fns = resolve_behavior(b.mod_id, b.type, b.behavior);

    // A slider is the native four-row widget (title, endpoints, two bar halves) while a channel byte is free and the rows fit, else a text bar.
    const int k = (int)s_pause_sliders.size();
    const bool fits = w.lines + 4 + w.line_reserve() <= PAUSE_MAX_LINES && w.off + 6u * (4u + (uint32_t)w.reserve) + 2u <= w.limit;

    if (b.type == BType::Slider && b.confirm.empty() && SND_CH_FIRST + k <= SND_CH_LAST && fits)
    {
        PauseSlider ps{ SND_CH_FIRST + k, b.smin, b.smax, b.behavior, b.label_off.empty() ? "OFF" : b.label_off,
                        b.label_on.empty() ? "MAX" : b.label_on, fns.toggle_get, fns.toggle_set };

        if (!s_pause_seeded) wr_b(w.rdram, VOL_BYTES + ps.channel, (uint8_t)byte_from_value(slider_value(w.rdram, b.behavior, fns.toggle_get), b.smin, b.smax, PAUSE_SLIDER_TOP));

        s_pause_sliders.push_back(std::move(ps));
        w.add(0x4201, PAUSE_TEXTID, (uint16_t)(PAUSE_SLIDER_MARK + k), b.label);
        w.add(0x4400, PAUSE_TEXTID, (uint16_t)(PAUSE_SLIDER_MARK + k), std::string{});
        w.add(0x4800, (uint16_t)(PAUSE_BAR_TID + k), 0, std::string{});
        w.add(0x4800, (uint16_t)(PAUSE_BAR_TID + k), 1, std::string{});

        return;
    }

    if (!w.add((uint16_t)b.flags, PAUSE_TEXTID, mark, std::string{})) return;

    s_pause_marks[mark] = { b.type, b.behavior, b.label, b.label_on, b.label_off,
                            fns.action, fns.toggle_get, fns.toggle_set,
                            b.smin, b.smax, b.sstep, b.bar_width, b.bar_filled, b.bar_empty };
}

// The selectable index (HUD+0xD68) of the first link (flag 0x20) in an array whose nextMenu is `next`, as the game's own scan finds it, or -1.
int pause_cursor_of(uint8_t* rdram, uint32_t arr, uint16_t next)
{

    int sel = 0;

    for (uint32_t e = arr; (uint16_t)rd_h(rdram, e) != 0xFFFF; e += 6)
    {
        if (((uint16_t)rd_h(rdram, e) & 1) == 0) continue;
        if (((uint16_t)rd_h(rdram, e) & 0x20) && (uint16_t)rd_h(rdram, e + 4) == next) return sel;

        ++sel;
    }

    return -1;
}

// The selectable entry at index `sel` as flags << 32 | textId << 16 | nextMenu (native toggles share nextMenu values with links, so all three identify it), or 0.
uint64_t pause_entry_at(uint8_t* rdram, uint32_t arr, int sel)
{

    for (uint32_t e = arr; (uint16_t)rd_h(rdram, e) != 0xFFFF; e += 6)
    {
        if (((uint16_t)rd_h(rdram, e) & 1) == 0) continue;
        if (sel-- == 0) return (uint64_t)(uint16_t)rd_h(rdram, e) << 32 | (uint64_t)(uint16_t)rd_h(rdram, e + 2) << 16 | (uint16_t)rd_h(rdram, e + 4);
    }

    return 0;
}

int pause_selectable_count(uint8_t* rdram, uint32_t arr)
{

    int n = 0;

    for (uint32_t e = arr; (uint16_t)rd_h(rdram, e) != 0xFFFF; e += 6)
        if ((uint16_t)rd_h(rdram, e) & 1) ++n;

    return n;
}

// The selectable index of the first entry equal to `id` (pause_entry_at's form), or -1.
int pause_index_of(uint8_t* rdram, uint32_t arr, uint64_t id)
{

    int sel = 0;

    for (uint32_t e = arr; (uint16_t)rd_h(rdram, e) != 0xFFFF; e += 6)
    {
        if (((uint16_t)rd_h(rdram, e) & 1) == 0) continue;
        if (pause_entry_at(rdram, e, 0) == id) return sel;

        ++sel;
    }

    return -1;
}

uint32_t pause_array_for(uint8_t id)
{

    if (s_pause_built && pause_rebuilt(id)) return pause_array_off(id);

    switch (id) {
        case 0:  return PAUSE_ROOT;
        case 1:  return PAUSE_GS;
        case 2:  return 0x109E68;
        default: return 0x109EEC;
    }
}

void build_pause_page(uint8_t* rdram, const std::string& page, uint8_t id)
{

    std::string title = page;

    for (const auto& b : config().buttons)
        if (b.page == page && b.menu != page) {
            title = b.title.empty() ? b.label : b.title;
            break;
        }

    PauseWriter w{ rdram, pause_array_off(id), pause_array_off(id) + PAUSE_STRIDE };
    const auto it = s_pause_parent_of.find(id);
    const uint8_t parent = it != s_pause_parent_of.end() ? it->second : PAUSE_GS_ID;

    w.add(0x2000, PAUSE_TEXTID, 0, title);
    w.add(0x1000, 0x0002, 0, std::string{});

    // The spacer before BACK is not drawn, so only BACK takes a line.
    w.reserve = 2;
    w.reserve_lines = 1;

    for (const Button* b : pause_lines(rdram, page)) add_pause_button(w, *b, id);

    w.reserve = 0;
    w.reserve_lines = -1;

    w.add(0x1000, 0x0001, 0, std::string{});
    w.add(0x4021, PAUSE_BACK_TID, parent, std::string{});
    w.end();
}

}

// RESET TO DEFAULTS on the CONTROLS page: the page's input settings back to their defaults, and every native slider bar showing them re-seeded.
void rs64::menucfg::reset_controls()
{

    const rs64::input::Bindings d;

    input_set([&d](rs64::input::Bindings& b) {
        b.mouse_sensitivity = d.mouse_sensitivity;
        b.mouse_smoothing = d.mouse_smoothing;
        b.pad_deadzone = d.pad_deadzone;
        b.rumble.enabled = d.rumble.enabled;

        for (auto& j : b.joy_devices) j.deadzone = rs64::input::JoyDevice{}.deadzone;
    });

    s_pause_seeded = false;

    if (!s_rdram) return;

    for (auto& ss : s_snd_sliders)
    {
        ss.last_byte = byte_from_value(slider_value(s_rdram, ss.key, ss.get_fn), ss.smin, ss.smax);
        wr_b(s_rdram, VOL_BYTES + ss.channel, (uint8_t)ss.last_byte);
    }
}

namespace {

// A pause menu's natives (the game's array at src) with the mod's buttons for `menu` merged in as on the front end (hide, replace, before / after, order, index),
// written to its host array. A button without placement goes where the native spacer was (above BACK), or at the end when there is none.
void merge_pause_menu(uint8_t* rdram, uint32_t src, uint32_t spacer, const std::string& menu, uint8_t self)
{

    PauseWriter w{ rdram, pause_array_off(self), pause_array_off(self) + PAUSE_STRIDE };

    struct Row { double order; uint32_t e; const Button* b; };

    std::vector<Row> rows;
    double spacer_order = -1;
    int k = 0;

    // The entries above the first selectable one (title, gap, the root's objectives) stay on top.
    for (uint32_t e = src; (uint16_t)rd_h(rdram, e) != 0xFFFF; e += 6, ++k)
    {
        const uint16_t flags = (uint16_t)rd_h(rdram, e);

        if (e == spacer) spacer_order = k * 10.0;
        else if (rows.empty() && !(flags & 1)) w.add(flags, (uint16_t)rd_h(rdram, e + 2), (uint16_t)rd_h(rdram, e + 4), std::string{});
        else rows.push_back({ k * 10.0, e, nullptr });
    }

    if (spacer_order < 0) spacer_order = k * 10.0;

    auto native_row = [&](const std::string& name) {

        const int tid = rs64::menu::pause_native_alias(name);

        return std::find_if(rows.begin(), rows.end(), [&](const Row& r) { return tid >= 0 && !r.b && (uint16_t)rd_h(rdram, r.e + 2) == tid; });
    };

    if (auto h = config().hides.find(menu); h != config().hides.end())
    {
        for (const auto& name : h->second)
            if (auto r = native_row(name); r != rows.end()) rows.erase(r);
    }

    double tail = spacer_order;

    for (const auto& b : config().buttons)
    {
        if (b.menu != menu || !button_visible(rdram, b, true)) continue;

        auto anchor = [&](const std::string& name, double& out) {

            if (auto r = native_row(name); r != rows.end()) return out = r->order, true;

            for (const Row& r : rows)
                if (r.b && r.b->id == name) return out = r.order, true;

            return false;
        };

        double ord = 0;
        bool have = false;

        if (auto r = b.replace.empty() ? rows.end() : native_row(b.replace); r != rows.end())
        {
            ord = r->order;
            have = true;
            rows.erase(r);
        }

        if (!have && !b.after.empty() && anchor(b.after, ord)) { ord += 5; have = true; }
        if (!have && !b.before.empty() && anchor(b.before, ord)) { ord -= 5; have = true; }
        if (!have && b.has_order) { ord = b.order; have = true; }
        if (!have) ord = (tail += 0.01);

        rows.push_back({ ord, 0, &b });
    }

    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.order < b.order; });

    for (const auto& b : config().buttons)
    {
        if (b.menu != menu || !b.has_index) continue;

        const auto it = std::find_if(rows.begin(), rows.end(), [&](const Row& r) { return r.b == &b; });

        if (it != rows.end()) rs64::menu::move_to_index(rows, (size_t)(it - rows.begin()), b.index);
    }

    for (size_t i = 0; i < rows.size(); ++i)
    {
        if (!rows[i].b)
        {
            w.add((uint16_t)rd_h(rdram, rows[i].e), (uint16_t)rd_h(rdram, rows[i].e + 2), (uint16_t)rd_h(rdram, rows[i].e + 4), std::string{});
            continue;
        }

        // Room is kept for the natives still to come (BACK at least).
        w.reserve = (int)std::count_if(rows.begin() + (long)i + 1, rows.end(), [](const Row& r) { return !r.b; });
        add_pause_button(w, *rows[i].b, self);
        w.reserve = 0;
    }

    w.end();
}
} // namespace

extern "C" void rs64_pause_install(uint8_t* rdram, recomp_context* ctx)
{

    static const bool off = env_disabled("ROGUESQ_NO_PAUSE_BUTTONS");

    if (off) return;

    s_rdram = rdram;

    // Only while the pause menu is OPEN (D_8010CA20 == 5) — touching these arrays
    // during level-load/briefing starves the audio scheduler.
    if (rd_w(rdram, 0x8010CA20u - 0x80000000u) != 5)
    {
        s_pause_built = false;
        s_in_pause = false;
        s_pause_seeded = false;

        return;
    }

    const unsigned hud = (unsigned)ctx->r4;
    const uint32_t h = hud - 0x80000000u;

    // The game's Back reads B held (0x800C1B64 mode 4), so after a page's Back the next one waits for B to be released.
    if (s_back_wait) s_back_wait = rs64::mips::mips_call(rdram, ctx, 0x800C1B64u, { (uint32_t)rd_w(rdram, h + 0x234), 4u }).v0 & 0xFF;

    // The entry under the cursor, to keep it there when a list changes (a requires condition flipped).
    const uint8_t cur = rd_b(rdram, h + 0xD6A);
    const int cur_sel = rd_h(rdram, h + 0xD68);
    const bool ours = s_pause_built && pause_rebuilt(cur);
    const uint64_t cur_id = ours ? pause_entry_at(rdram, pause_array_off(cur), cur_sel) : 0;

    s_pause_marks.clear();
    s_pause_text.clear();
    s_pause_sliders.clear();

    // The root (objectives, CONTINUE ...) and GAME SETTINGS, each with the mod's buttons merged in.
    merge_pause_menu(rdram, PAUSE_ROOT, 0, "pause", PAUSE_ROOT_ID);
    merge_pause_menu(rdram, PAUSE_GS, PAUSE_GS_SPACER, "pause_game_settings", PAUSE_GS_ID);

    // Pages, built after their openers so each knows its parent; a page opened only from another page is built after it.
    std::vector<uint8_t> built;

    for (bool grew = true; grew; )
    {
        grew = false;

        for (const auto& [id, parent] : std::unordered_map<uint8_t, uint8_t>(s_pause_parent_of))
        {
            const std::string* p = pause_page_name(id);

            if (!p || std::find(built.begin(), built.end(), id) != built.end()) continue;

            build_pause_page(rdram, *p, id);
            built.push_back(id);
            grew = true;
        }
    }

    s_pause_built = true;
    s_in_pause = true;
    s_pause_seeded = true;

    if (cur_id != 0 && pause_entry_at(rdram, pause_array_off(cur), cur_sel) != cur_id)
    {
        const int sel = pause_index_of(rdram, pause_array_off(cur), cur_id);

        // Gone from the list: the cursor stays in range (on the last selectable entry at most).
        const int last = pause_selectable_count(rdram, pause_array_off(cur)) - 1;

        wr_h(rdram, h + 0xD68, (int16_t)(sel >= 0 ? sel : std::min(cur_sel, std::max(last, 0))));
    }
}

// The four places the game picks the array for a pause menu id (HUD+0xD6A, or a link's nextMenu): ours for the root, GAME SETTINGS and mod pages.
extern "C" unsigned rs64_pause_array_for(uint8_t* rdram, unsigned id, unsigned native)
{

    if (!s_pause_built || id > 0xFF || !pause_rebuilt((uint8_t)id)) return native;

    return 0x80000000u | pause_array_for((uint8_t)id);
}

// B in a pause submenu (initOrUpdatePauseScreenDim 0x800C23EC / 0x800C240C): the game always returns to the root; a mod page returns to the menu that links to it,
// with the cursor on that link.
extern "C" void rs64_pause_back_begin(uint8_t* rdram, unsigned hud)
{

    const uint32_t h = hud - 0x80000000u;

    s_back_from = rd_b(rdram, h + 0xD6A);
    s_back_cursor = rd_h(rdram, h + 0xD68);
}

extern "C" void rs64_pause_back_end(uint8_t* rdram, unsigned hud)
{

    const uint8_t from = s_back_from;
    const uint32_t h = hud - 0x80000000u;

    s_back_from = 0;

    // B still held from the previous page's Back: stay where we are.
    if (s_back_wait)
    {
        wr_b(rdram, h + 0xD6A, from);
        wr_h(rdram, h + 0xD68, s_back_cursor);

        return;
    }

    if (!s_pause_built || !pause_page_name(from)) return;

    const auto it = s_pause_parent_of.find(from);
    const uint8_t parent = it != s_pause_parent_of.end() ? it->second : PAUSE_GS_ID;
    const int sel = pause_cursor_of(rdram, pause_array_for(parent), from);

    wr_b(rdram, h + 0xD6A, parent);
    wr_h(rdram, h + 0xD68, (int16_t)(sel >= 0 ? sel : 0));
    s_back_wait = true;
}

// Render hook: the label vaddr for a pause sentinel or one of our link/title entries (by address), or 0 if not ours.
extern "C" unsigned rs64_pause_label_for(uint8_t* rdram, unsigned marker, unsigned entry)
{

    auto ti = s_pause_text.find(entry);

    if (ti != s_pause_text.end()) return alloc_str(rdram, ti->second);

    auto it = s_pause_marks.find((uint16_t)marker);

    if (it == s_pause_marks.end()) return 0;

    const PauseBinding& b = it->second;
    std::string text;

    if (b.type == BType::Toggle)
        text = toggle_text(rdram, b.key, b.toggle_get_fn, b.label, b.label_on, b.label_off);
    else if (b.type == BType::Slider)
        text = slider_bar_text(b.label, slider_value(rdram, b.key, b.toggle_get_fn),
                               b.smin, b.smax, b.bar_width, b.bar_filled, b.bar_empty);
    else
        text = b.label;

    return alloc_str(rdram, text);
}

// Confirm hook: run the behavior for a pause sentinel.
extern "C" void rs64_pause_confirm(uint8_t* rdram, unsigned marker) {

    auto it = s_pause_marks.find((uint16_t)marker);

    if (it == s_pause_marks.end()) return;

    const PauseBinding& b = it->second;

    if (b.type == BType::Toggle)
    {
        if (b.toggle_set_fn) call_export(rdram, b.toggle_set_fn); else toggle_flip(b.key);
    } else if (b.type == BType::Slider) {
        int v = slider_value(rdram, b.key, b.toggle_get_fn);
        slider_apply(rdram, b.key, b.toggle_set_fn, confirmed_slider_value((int)(uint16_t)marker, v, b.smin, b.smax, b.sstep, b.label,
                                                                           b.bar_width, b.bar_filled, b.bar_empty));
    } else {
        if (b.action_fn) call_export(rdram, b.action_fn); else fire_action(b.key);
    }
}

// Pause slider edit, initOrUpdatePauseScreenDim: the value byte is loaded at 0x800C2660 ($s2 = the title row, $v0 = the byte its nextMenu picked: a mod
// row falls into the speech branch) and stored to speech (+0x22) before 0x800C27CC. A mod slider loads and keeps its own byte; speech is put back.
extern "C" void rs64_pause_slider_load(uint8_t* rdram, recomp_context* ctx)
{

    const PauseSlider* ps = pause_slider((uint16_t)rd_h(rdram, (uint32_t)ctx->r18 - 0x80000000u + 4), PAUSE_SLIDER_MARK);

    if (!ps) return;

    s_pause_speech = (int)ctx->r2 & 0xFF;
    ctx->r2 = rd_b(rdram, VOL_BYTES + ps->channel);
}

extern "C" void rs64_pause_slider_store(uint8_t* rdram, recomp_context* ctx)
{

    const PauseSlider* ps = pause_slider((uint16_t)rd_h(rdram, (uint32_t)ctx->r18 - 0x80000000u + 4), PAUSE_SLIDER_MARK);

    if (!ps || s_pause_speech < 0) return;

    const int byte = rd_b(rdram, SPEECH_BYTE);

    wr_b(rdram, VOL_BYTES + ps->channel, (uint8_t)byte);
    wr_b(rdram, SPEECH_BYTE, (uint8_t)s_pause_speech);
    s_pause_speech = -1;
    slider_apply(rdram, ps->key, ps->set_fn, value_from_byte(byte, ps->smin, ps->smax, PAUSE_SLIDER_TOP));
}

// Pause slider draw, runPauseMenuStateMachine ($s4 = the row's textId field): a mod bar row shows its own byte (0x800C3418, $s2),
// and a mod endpoint row its own end labels in place of OFF/MAX (0x800C3314: $s0 and $v0 hold the two strings).
extern "C" void rs64_pause_slider_bar(uint8_t* rdram, recomp_context* ctx)
{

    const PauseSlider* ps = pause_slider((uint16_t)rd_h(rdram, (uint32_t)ctx->r20 - 0x80000000u), PAUSE_BAR_TID);

    if (ps) ctx->r18 = rd_b(rdram, VOL_BYTES + ps->channel);
}

extern "C" void rs64_pause_slider_ends(uint8_t* rdram, recomp_context* ctx)
{

    const PauseSlider* ps = pause_slider((uint16_t)rd_h(rdram, (uint32_t)ctx->r20 - 0x80000000u + 2), PAUSE_SLIDER_MARK);

    if (!ps) return;

    ctx->r16 = alloc_str(rdram, ps->off_label);
    ctx->r2 = alloc_str(rdram, ps->on_label);
}
