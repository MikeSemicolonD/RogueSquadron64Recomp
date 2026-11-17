// The controls mod's CONTROLLER SETTINGS screen: a scrolling list of rebindable actions.
#include "game/menus/menu_config.h"
#include "game/menus/menu_config_internal.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
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
#include "main/main.h"
#include "input/input_host.h"
#include "util/mips_call.h"

namespace rs64::menucfg {

namespace ri = rs64::input;

constexpr const char* kControlsMod = "controls-menu";
constexpr const char* kControlsPage = "controls";

// Visible list rows, then RESET TO DEFAULTS and BACK in the last two slots.
constexpr int kCtlRows = PAGE_ROWS - 2;
constexpr int kCtlResetSlot = kCtlRows, kCtlBackSlot = kCtlRows + 1;
constexpr int kCtlTopY = -158;
constexpr uint32_t kCtlFlashMs = 2000, kCtlQuietMs = 250, kCtlHoldMs = 2000;
constexpr const char* kCtlReset = "RESET TO DEFAULTS";

// Two binding columns after the action. Each column is as wide as its widest text (text group 5's units, which its tab stops share); rows draw
// at kCtlMaxScale or smaller to fit kCtlMaxWidth, down to kCtlMinScale, past which binding names are cut with "...".
constexpr int kCtlColumns = 2;
constexpr uint32_t kCtlGroup = 5;
// A menu unit draws about 1.18 window pixels wide; 480 units reach the scrollbar this list moves out to (kBindingsBarGameX in touch_host.cpp).
constexpr int kCtlColGap = 14, kCtlMaxWidth = 480;
constexpr float kCtlMaxScale = 0.85f, kCtlMinScale = 0.6f;

struct ControlsScreen {
    bool shown = false;
    bool tabs = false;
    ri::Device dev = ri::Device::Keyboard;
    std::vector<ri::ControlRow> rows;
    rs64::menu::ScrollPos pos{ 0, 0 };
    uint32_t bufs[kCtlRows] = {};
    uint32_t dirty = 0;
    int capturing = -1;
    int col = 0;
    int back_from = 0;
    uint32_t quiet_until = 0;
    int flash_row = -1;
    std::string flash;
    uint32_t flash_until = 0;

    // A on a row waits: let go within kCtlHoldMs to rebind, hold it to clear.
    int hold_row = -1;
    uint32_t hold_until = 0;

    // The fitted layout: tab stops (both columns and the row end), the rows' scale, the widest a binding name may be (0 = any).
    uint16_t stops[kCtlColumns + 1] = { 200, 350, 500 };
    float scale = 1.0f;
    int name_max = 0;
};

ControlsScreen s_ctl;

// The tab stops' RDRAM copy, which text group 5 reads through a pointer each layout.
uint32_t s_ctl_stops = 0;

// Mouse wheel notches from the SDL thread (up positive), taken by the next menu pass as up/down steps.
std::atomic<int> s_ctl_wheel{0};

bool controls_enabled() {
    return recomp::mods::is_mod_enabled(kControlsMod);
}

// List rows (the actions); pos.sel == ctl_total() is RESET TO DEFAULTS and ctl_total() + 1 is BACK, below the list.
int ctl_total() {
    return (int)s_ctl.rows.size();
}

int ctl_slot_of(int sel) {
    return sel < ctl_total() ? sel - s_ctl.pos.top : sel == ctl_total() ? kCtlResetSlot : kCtlBackSlot;
}

std::string ctl_action_name(ri::Target t)
{

    for (const auto& r : s_ctl.rows) {
        if (r.target == t) return r.action;
    }

    return ri::target_label(t);
}

// The row's binding in column c for the screen's device, "--" when it has none. Caller holds g_bindings_mtx.
std::string ctl_binding_name(const ri::ControlRow& row, int c)
{

    const int k = ri::nth_source(rs64::app::g_bindings, row.target, s_ctl.dev, c);

    return ri::menu_source_name(k >= 0 ? rs64::app::g_bindings.targets[(int)row.target][k] : ri::Source{});
}

// Lays the columns out for the device's rows: each as wide as its widest name (in braces, as when selected), then the row scale that keeps it
// within kCtlMaxWidth; below kCtlMinScale, names are capped instead. The stops reach the game through s_ctl_stops.
void ctl_fit(uint8_t* rdram)
{

    std::lock_guard<std::mutex> lk(rs64::app::g_bindings_mtx);
    const auto width = [rdram](const std::string& s) { return text_advance(rdram, kCtlGroup, s); };
    int action = 0;
    int names[kCtlColumns] = {};

    for (const ri::ControlRow& row : s_ctl.rows)
    {
        action = std::max(action, width(row.action));

        for (int c = 0; c < kCtlColumns; ++c) {
            names[c] = std::max(names[c], width("{" + ctl_binding_name(row, c) + "}"));
        }
    }

    const int end_blank = width("`");
    const int fixed = action + kCtlColumns * kCtlColGap + kCtlColGap + end_blank;

    s_ctl.scale = std::min(kCtlMaxScale, float(kCtlMaxWidth) / float(fixed + names[0] + names[1]));
    s_ctl.name_max = 0;

    if (s_ctl.scale < kCtlMinScale)
    {
        s_ctl.scale = kCtlMinScale;
        s_ctl.name_max = std::max((int(float(kCtlMaxWidth) / kCtlMinScale) - fixed) / kCtlColumns, width("{W...}"));

        for (int& n : names) n = std::min(n, s_ctl.name_max);
    }

    s_ctl.stops[0] = (uint16_t)(action + kCtlColGap);
    s_ctl.stops[1] = (uint16_t)(s_ctl.stops[0] + names[0] + kCtlColGap);
    s_ctl.stops[2] = (uint16_t)(s_ctl.stops[1] + names[1] + kCtlColGap);

    for (int i = 0; s_ctl_stops && i <= kCtlColumns; ++i) {
        wr_h(rdram, s_ctl_stops - 0x80000000u + 2 * i, (int16_t)s_ctl.stops[i]);
    }
}

std::string ctl_row_text(uint8_t* rdram, int r)
{

    if (r >= (int)s_ctl.rows.size()) return kCtlReset;

    const ri::ControlRow& row = s_ctl.rows[r];

    if (r == s_ctl.flash_row && SDL_GetTicks() < s_ctl.flash_until) return s_ctl.flash;

    // A whole-row prompt, so the binding columns only have to fit binding names.
    if (r == s_ctl.capturing) return std::string(row.action) + ": " + (s_ctl.dev == ri::Device::Keyboard ? "PRESS A KEY" : "PRESS A BUTTON");

    std::lock_guard<std::mutex> lk(rs64::app::g_bindings_mtx);
    const auto width = [rdram](const std::string& s) { return text_advance(rdram, kCtlGroup, s); };
    std::string text = row.action;

    for (int c = 0; c < kCtlColumns; ++c)
    {
        // The selected binding is in braces; a name too long for its column is cut.
        const bool selected = r == s_ctl.pos.sel && c == s_ctl.col;
        std::string name = ctl_binding_name(row, c);

        if (s_ctl.name_max > 0) name = rs64::mods_page::fit_text(name, width, s_ctl.name_max - width("{}"));

        text += "\t" + (selected ? "{" + name + "}" : name);
    }

    return text + kCtlRowEnd;
}

// The controller model's mesh (loadControllerModel); the controls mod's keyboard and gamepad models load beside it, swapped in on entry.
constexpr uint32_t kControllerMesh = 0x000CF0ECu;
uint32_t s_ctl_mesh_orig = 0;

// By Device; VR has no model of its own and shows the N64 controller.
uint32_t s_ctl_mesh[4] = {};

void controls_show_model(uint8_t* rdram, bool on)
{

    if (!s_ctl_mesh_orig) return;

    const uint32_t m = on ? s_ctl_mesh[(int)s_ctl.dev] : 0;

    wr_w(rdram, kControllerMesh, (int32_t)(m ? m : s_ctl_mesh_orig));
}

// Visible slot i: the top row's end blank becomes '^' with rows above it, the bottom row's '#' with rows below (the scroll arrows ensure_menu_period draws).
std::string ctl_slot_text(uint8_t* rdram, int i)
{

    std::string t = ctl_row_text(rdram, s_ctl.pos.top + i);
    const bool more_above = i == 0 && s_ctl.pos.top > 0;
    const bool more_below = i == kCtlRows - 1 && s_ctl.pos.top + kCtlRows < ctl_total();

    if (!t.empty() && t.back() == '`' && (more_above || more_below)) t.back() = more_above ? '^' : '#';

    return t;
}

// Entry offset for a row drawn at absolute y: the game places entry i at list_base + 36*i - 18*count + offset.
int16_t ctl_offset_y(int16_t base, int i, int y) {
    return (int16_t)(y - base - 36 * i + 18 * PAGE_ROWS);
}

// setupMenuData tail on CONTROLLER SETTINGS (natively just BACK): the list rows (live buffers the frame hook relabels), RESET TO DEFAULTS and BACK.
int controls_install(uint8_t* rdram)
{

    Slot back = read_slot(rdram, 0);
    const int16_t base = rd_h(rdram, GCMD_OFF + 0x96);
    constexpr int32_t kUnitScale = 0x3F800000;

    s_ctl = ControlsScreen{};
    s_ctl.shown = true;
    s_ctl.dev = rs64::app::last_device();
    s_ctl.rows = ri::control_rows(s_ctl.dev);
    ctl_fit(rdram);

    int32_t scale_bits = 0;

    std::memcpy(&scale_bits, &s_ctl.scale, 4);

    for (int i = 0; i < kCtlRows; ++i)
    {
        s_ctl.bufs[i] = live_buffer(rdram, "controls#" + std::to_string(i));

        if (!s_ctl.bufs[i]) return -1;

        write_live(rdram, s_ctl.bufs[i], ctl_slot_text(rdram, i));
        write_slot(rdram, i, Slot{ (int32_t)s_ctl.bufs[i], SUBTYPE_HOST, 0, (int16_t)kCtlX, ctl_offset_y(base, i, kCtlTopY + kCtlPitch * i), scale_bits });
        s_slot[i] = {};
    }

    const int reset_y = kCtlTopY + kCtlPitch * kCtlRows + kCtlBackGap;

    write_slot(rdram, kCtlResetSlot, Slot{ (int32_t)alloc_str(rdram, kCtlReset), SUBTYPE_HOST, 0, 0, ctl_offset_y(base, kCtlResetSlot, reset_y), kUnitScale });
    s_slot[kCtlResetSlot] = {};
    back.y = ctl_offset_y(base, kCtlBackSlot, reset_y + kCtlPitch + kCtlBackGap);
    write_slot(rdram, kCtlBackSlot, back);
    s_slot[kCtlBackSlot] = {};
    wr_b(rdram, GCMD_OFF + 0x95, (uint8_t)PAGE_ROWS);
    wr_b(rdram, GCMD_OFF + 0x94, 0);
    controls_show_model(rdram, true);

    // Opened from the mod's CONTROLS page (BINDINGS), so BACK and B return there rather than to OPTIONS.
    if (const uint8_t page = page_id(kControlsPage); page != 0xFF) wr_b(rdram, GCMD_OFF + 0x05, page);

    fprintf(stderr, "[controls] list for device %d, %d rows\n", (int)s_ctl.dev, ctl_total());

    return PAGE_ROWS;
}

bool controls_on_screen(uint8_t* rdram) {
    return s_ctl.shown && rd_b(rdram, GCMD_OFF + 0x04) == MENU_CONTROLLER_SETTINGS;
}

bool controls_view(uint8_t* rdram, ListView* v)
{

    if (!controls_on_screen(rdram)) return false;

    *v = { ctl_total(), kCtlRows, s_ctl.pos.top };

    return true;
}

// A scrollbar drag: the rows move under the highlight, which keeps its place on screen.
void controls_scroll_to(uint8_t* rdram, int top)
{

    top = std::clamp(top, 0, std::max(0, ctl_total() - kCtlRows));

    if (top == s_ctl.pos.top) return;

    const int slot = rd_b(rdram, GCMD_OFF + 0x94);

    s_ctl.pos.top = top;

    if (slot < kCtlRows) s_ctl.pos.sel = top + slot;

    s_ctl.dirty = (1u << kCtlRows) - 1;
}

constexpr uint32_t kCtlAllRows = (1u << kCtlRows) - 1;

// After a binding changes: lay the columns out again and give the rows the new scale (the frame's relayout picks both up).
void ctl_refit(uint8_t* rdram)
{

    const float before = s_ctl.scale;

    ctl_fit(rdram);

    if (s_ctl.scale != before)
    {
        int32_t bits = 0;

        std::memcpy(&bits, &s_ctl.scale, 4);

        for (int i = 0; i < kCtlRows; ++i)
        {
            Slot s = read_slot(rdram, i);

            s.scaler = bits;
            write_slot(rdram, i, s);
        }
    }

    s_ctl.dirty = kCtlAllRows;
}

void ctl_flash(int r, const std::string& text)
{

    s_ctl.flash_row = r;
    s_ctl.flash = text;
    s_ctl.flash_until = SDL_GetTicks() + kCtlFlashMs;
}

// A on a list row: wait to see whether A is held (clear) or let go (rebind); on RESET TO DEFAULTS, reset the device's bindings.
bool controls_confirm(uint8_t* rdram, int e)
{

    if (!controls_on_screen(rdram) || e > kCtlResetSlot || s_ctl.capturing >= 0 || s_ctl.hold_row >= 0) return false;

    if (e == kCtlResetSlot)
    {
        rs64::app::reset_device_bindings(s_ctl.dev);
        ctl_refit(rdram);

        return true;
    }

    s_ctl.hold_row = s_ctl.pos.top + e;
    s_ctl.hold_until = SDL_GetTicks() + kCtlHoldMs;
    s_ctl.flash_row = -1;

    return true;
}

// A held on a row: let go early to rebind the selected column, or hold to clear it.
void controls_poll_hold(uint8_t* rdram)
{

    wr_w(rdram, PAD_PRESSED, 0);

    const int r = s_ctl.hold_row;

    if (rs64::app::n64_buttons() & N64_A_BUTTON)
    {
        if (SDL_GetTicks() < s_ctl.hold_until) return;

        std::unique_lock<std::mutex> lk(rs64::app::g_bindings_mtx);

        ri::clear_binding(rs64::app::g_bindings, s_ctl.rows[r].target, s_ctl.dev, s_ctl.col);
        ri::save_bindings(rs64::app::g_bindings, ri::default_config_path());
        lk.unlock();

        s_ctl.hold_row = -1;
        s_ctl.quiet_until = SDL_GetTicks() + kCtlQuietMs;
        ctl_flash(r, "CLEARED");
        ctl_refit(rdram);

        return;
    }

    s_ctl.hold_row = -1;

    if (!rs64::app::capture_device_present(s_ctl.dev)) return;

    s_ctl.capturing = r;
    rs64::app::capture_begin({ false, s_ctl.dev, true });
    s_ctl.dirty = kCtlAllRows;
}

// The capture: the menu sees no presses until it ends; a taken input swaps with the binding that held it, Delete or Backspace clears.
void controls_poll_capture(uint8_t* rdram)
{

    wr_w(rdram, PAD_PRESSED, 0);

    ri::Source got;
    const int c = rs64::app::capture_poll(&got);

    if (c == 0) return;

    // The input just bound is still held, and the game would read it as a fresh press of its new N64 button (A = confirm restarts the capture).
    const int r = s_ctl.capturing;
    s_ctl.capturing = -1;
    s_ctl.quiet_until = SDL_GetTicks() + kCtlQuietMs;
    s_ctl.dirty = kCtlAllRows;

    if (c < 0) return;

    std::unique_lock<std::mutex> lk(rs64::app::g_bindings_mtx);
    ri::Target gave = ri::Target::Count;

    if (c == 2) {
        ri::clear_binding(rs64::app::g_bindings, s_ctl.rows[r].target, s_ctl.dev, s_ctl.col);
    } else {
        gave = ri::rebind(rs64::app::g_bindings, s_ctl.rows[r].target, s_ctl.dev, got, s_ctl.col);
    }

    ri::save_bindings(rs64::app::g_bindings, ri::default_config_path());
    lk.unlock();
    ctl_refit(rdram);

    if (c == 2) {
        ctl_flash(r, "CLEARED");
    } else if (gave != ri::Target::Count) {
        ctl_flash(r, "SWAPPED WITH " + ctl_action_name(gave));
    }
}

// Each front-end pass before the menu reads the pad: tab stops on entry/exit, the capture, and up/down scrolling the window (left/right would cycle presets).
void controls_pass(uint8_t* rdram, recomp_context* ctx)
{

    if (!controls_on_screen(rdram))
    {
        if (s_ctl.tabs) rs64::mips::mips_call(rdram, ctx, 0x80063EC0u, {5u});

        if (s_ctl.capturing >= 0) rs64::app::capture_end();

        if (s_ctl.shown) controls_show_model(rdram, false);

        // The MODS, mod text and LANGUAGE pages read the wheel too (their passes run after this one).
        if (!mods_on_screen(rdram) && !mod_text_on_screen(rdram) && !langs_on_screen(rdram))
        {
            s_ctl_wheel.store(0);
            s_list_scroll_req.store(-1);
        }

        s_ctl.shown = false;
        s_ctl.tabs = false;
        s_ctl.capturing = -1;
        s_ctl.hold_row = -1;

        return;
    }

    if (!s_ctl.tabs)
    {
        if (!s_ctl_stops) {
            void* p = recomp::alloc(rdram, 2 * (kCtlColumns + 1));

            if (!p) return;

            s_ctl_stops = (uint32_t)((uint8_t*)p - rdram) + 0x80000000u;
        }

        ctl_fit(rdram);
        rs64::mips::mips_call(rdram, ctx, 0x80063E98u, {kCtlGroup, s_ctl_stops, (uint32_t)(kCtlColumns + 1)});
        s_ctl.tabs = true;
        s_ctl.dirty = kCtlAllRows;
    }

    // The braces follow the highlight, which a mouse or touch hover moves too.
    const int hover = rd_b(rdram, GCMD_OFF + 0x94);

    if (hover < kCtlRows && s_ctl.pos.top + hover != s_ctl.pos.sel && s_ctl.pos.top + hover < ctl_total())
    {
        s_ctl.pos.sel = s_ctl.pos.top + hover;
        s_ctl.dirty = kCtlAllRows;
    }

    if (s_ctl.hold_row >= 0)
    {
        controls_poll_hold(rdram);

        return;
    }

    if (s_ctl.flash_row >= 0 && SDL_GetTicks() >= s_ctl.flash_until)
    {
        s_ctl.flash_row = -1;
        s_ctl.dirty = (1u << kCtlRows) - 1;
    }

    const int want_top = s_list_scroll_req.exchange(-1);

    if (want_top >= 0 && s_ctl.capturing < 0) controls_scroll_to(rdram, want_top);

    // Wheel notches while waiting for an input are dropped.
    if (s_ctl.capturing >= 0)
    {
        s_ctl_wheel.store(0);
        controls_poll_capture(rdram);

        return;
    }

    if (SDL_GetTicks() < s_ctl.quiet_until)
    {
        wr_w(rdram, PAD_PRESSED, 0);

        return;
    }

    uint32_t p = (uint32_t)rd_w(rdram, PAD_PRESSED);
    const int wheel = s_ctl_wheel.exchange(0);

    if (wheel) p |= wheel > 0 ? kNavUp : kNavDown;

    if (!(p & (kNavUp | kNavDown | kNavLeft | kNavRight))) return;

    wr_w(rdram, PAD_PRESSED, (int32_t)(p & ~(kNavUp | kNavDown | kNavLeft | kNavRight)));

    // A mouse or touch hover may have moved the highlight since the last step.
    const int slot = rd_b(rdram, GCMD_OFF + 0x94);
    const int n = ctl_total(), reset = n, back = n + 1;
    int& sel = s_ctl.pos.sel;

    sel = slot == kCtlResetSlot ? reset : slot >= kCtlBackSlot ? back : std::min(s_ctl.pos.top + slot, n - 1);

    // Left/right switch the binding column on a list row, and from RESET TO DEFAULTS or BACK return to the row left from (the window keeps its place).
    if (!(p & (kNavUp | kNavDown))) {
        if (sel < n) {
            s_ctl.col = (s_ctl.col + 1) % kCtlColumns;
            s_ctl.dirty = kCtlAllRows;
            rs64::mips::mips_call(rdram, ctx, 0x80087EE8u, {0x17u, 0x7Fu, 0x40u});

            return;
        }

        sel = std::clamp(s_ctl.back_from, s_ctl.pos.top, s_ctl.pos.top + kCtlRows - 1);
    } else if (p & kNavDown) {
        // The list stops at its ends: wrapping would jump a long list back to the top.
        if (sel == back) return;

        if (sel >= n - 1) {
            s_ctl.back_from = std::min(sel, n - 1);
            ++sel;
        } else {
            s_ctl.pos = rs64::menu::scroll_step(s_ctl.pos, 1, n, kCtlRows);
        }
    } else {
        if (sel == 0) return;

        if (sel == back) sel = reset;
        else if (sel == reset) s_ctl.pos = { n - 1, std::max(0, n - kCtlRows) };
        else s_ctl.pos = rs64::menu::scroll_step(s_ctl.pos, -1, n, kCtlRows);
    }

    // The braces move with the highlight.
    s_ctl.dirty = kCtlAllRows;

    wr_b(rdram, GCMD_OFF + 0x94, (uint8_t)ctl_slot_of(s_ctl.pos.sel));
    rs64::mips::mips_call(rdram, ctx, 0x80087EE8u, {0x17u, 0x7Fu, 0x40u});
}

// The front-end photo behind every menu (element *0x800CF15C, its draw object at +0xC): with flag 2 in +0x14 its prim colour +0x2C tints the
// texture (drawSubtitleText 0x80016010), so the list's page sets half grey each frame and gives the shared object back on leaving.
constexpr uint32_t kBgElem = 0x000CF15Cu;
constexpr int32_t kBgDim = 0x808080FF;
uint32_t s_bg_obj = 0;
int32_t s_bg_flags = 0, s_bg_rgba = 0;

void controls_dim_background(uint8_t* rdram, bool on)
{

    const uint32_t elem = (uint32_t)rd_w(rdram, kBgElem);
    const uint32_t obj = elem >= 0x80000000u && elem < 0x80800000u ? (uint32_t)rd_w(rdram, elem - 0x80000000u + 0x0C) : 0;
    const bool valid = obj >= 0x80000000u && obj < 0x80800000u;

    if (!on || !valid)
    {
        if (s_bg_obj && s_bg_obj == obj) {
            wr_w(rdram, obj - 0x80000000u + 0x14, s_bg_flags);
            wr_w(rdram, obj - 0x80000000u + 0x2C, s_bg_rgba);
        }

        s_bg_obj = 0;

        return;
    }

    const uint32_t o = obj - 0x80000000u;

    if (s_bg_obj != obj) {
        s_bg_obj = obj;
        s_bg_flags = rd_w(rdram, o + 0x14);
        s_bg_rgba = rd_w(rdram, o + 0x2C);
    }

    wr_w(rdram, o + 0x14, s_bg_flags | 2);
    wr_w(rdram, o + 0x2C, kBgDim);
}

// Each front-end frame: dim the photo behind the list, rewrite changed rows and relay them out in place.
void controls_frame(uint8_t* rdram, recomp_context* ctx)
{

    controls_dim_background(rdram, controls_on_screen(rdram));

    if (!controls_on_screen(rdram) || !s_ctl.dirty) return;

    for (int i = 0; i < kCtlRows; ++i)
    {
        if (s_ctl.dirty & (1u << i)) write_live(rdram, s_ctl.bufs[i], ctl_slot_text(rdram, i));
    }

    rs64::mips::mips_call(rdram, ctx, 0x800C4C8Cu, {GCMD, s_ctl.dirty, 0u});
    s_ctl.dirty = 0;
}
} // namespace rs64::menucfg

using namespace rs64::menucfg;

// Mission start (initAllPlayersHudTracking copies D_80109AD0[preset] per player): with the controls mod on, every preset slot points at Luke's
// table, so gameplay matches the list while the saved preset byte stays the player's. Replays keep their recorded preset.
extern "C" int rs64_ls_replaying(void);

// loadControllerModel, after the N64 controller is set up (0x800C4BD8): the keyboard and gamepad models load the same way
// (load_hmt_and_hob, walkMeshdef0List by object name, 0x8005955C), from the mod's frontend/controller/<name> assets.
extern "C" void rs64_controls_load_models(uint8_t* rdram, recomp_context* ctx)
{

    s_ctl_mesh_orig = (uint32_t)rd_w(rdram, kControllerMesh);
    std::fill(std::begin(s_ctl_mesh), std::end(s_ctl_mesh), 0u);
    mods_image_load(rdram, ctx);

    if (!controls_enabled()) return;

    for (const auto& [dev, name] : { std::pair{ ri::Device::Keyboard, "keyboard" }, std::pair{ ri::Device::Gamepad, "gamepad" }, std::pair{ ri::Device::Joystick, "joystick" } })
    {
        rs64::mips::mips_call(rdram, ctx, 0x8005645Cu, {alloc_str(rdram, std::string("frontend/controller/") + name), 0u, 0u, 0u});

        const uint32_t mesh = rs64::mips::mips_call(rdram, ctx, 0x80056EB0u, {alloc_str(rdram, name)}).v0;

        if (!mesh) {
            fprintf(stderr, "[controls] %s model not found\n", name);
            continue;
        }

        rs64::mips::mips_call(rdram, ctx, 0x8005955Cu, {mesh, 0u, 0x01060000u, 0u});
        s_ctl_mesh[(int)dev] = mesh;
    }

    fprintf(stderr, "[controls] models: controller %08X keyboard %08X gamepad %08X joystick %08X\n", s_ctl_mesh_orig, s_ctl_mesh[0], s_ctl_mesh[1], s_ctl_mesh[2]);
}

extern "C" void rs64_controls_wheel(int steps) {
    s_ctl_wheel.fetch_add(steps);
}

extern "C" int rs64_controls_menu_active(void) {
    return controls_enabled() ? 1 : 0;
}

extern "C" void rs64_controls_force_luke(uint8_t* rdram)
{

    constexpr uint32_t kMissionPresets = 0x00109AD0u, kLuke = 0x8009DE18u;

    if (!controls_enabled() || rs64_ls_replaying()) return;

    static bool s_logged = false;

    if (!s_logged)
    {
        s_logged = true;

        fprintf(stderr, "[controls] mission presets %08X %08X %08X %08X -> Luke %08X\n", (uint32_t)rd_w(rdram, kMissionPresets), (uint32_t)rd_w(rdram, kMissionPresets + 4),
                (uint32_t)rd_w(rdram, kMissionPresets + 8), (uint32_t)rd_w(rdram, kMissionPresets + 12), kLuke);
    }

    for (uint32_t k = 0; k < 4; ++k) wr_w(rdram, kMissionPresets + 4 * k, (int32_t)kLuke);
}
