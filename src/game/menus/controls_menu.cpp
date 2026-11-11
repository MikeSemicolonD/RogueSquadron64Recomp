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
constexpr float kCtlScale = 1.0f;
constexpr int kCtlTopY = -158;
constexpr uint16_t kCtlTabStop = 200, kCtlEndStop = 350;
constexpr uint32_t kCtlFlashMs = 2000, kCtlQuietMs = 250;
constexpr const char* kCtlReset = "RESET TO DEFAULTS";

struct ControlsScreen {
    bool shown = false;
    bool tabs = false;
    ri::Device dev = ri::Device::Keyboard;
    std::vector<ri::ControlRow> rows;
    rs64::menu::ScrollPos pos{ 0, 0 };
    uint32_t bufs[kCtlRows] = {};
    uint32_t dirty = 0;
    int capturing = -1;
    int back_from = 0;
    uint32_t quiet_until = 0;
    int flash_row = -1;
    std::string flash;
    uint32_t flash_until = 0;
};

ControlsScreen s_ctl;

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

std::string ctl_row_text(int r)
{

    if (r >= (int)s_ctl.rows.size()) return kCtlReset;

    const ri::ControlRow& row = s_ctl.rows[r];

    if (r == s_ctl.flash_row && SDL_GetTicks() < s_ctl.flash_until) return s_ctl.flash;

    // A whole-row prompt, so the binding column only has to fit binding names.
    if (r == s_ctl.capturing) return std::string(row.action) + ": " + (s_ctl.dev == ri::Device::Keyboard ? "PRESS A KEY" : "PRESS A BUTTON");

    std::lock_guard<std::mutex> lk(rs64::app::g_bindings_mtx);
    const int k = ri::shown_source(rs64::app::g_bindings, row.target, s_ctl.dev);
    const ri::Source none{};

    return std::string(row.action) + "\t" + ri::menu_source_name(k >= 0 ? rs64::app::g_bindings.targets[(int)row.target][k] : none) + kCtlRowEnd;
}

// The controller model's mesh (loadControllerModel); the controls mod's keyboard and gamepad models load beside it, swapped in on entry.
constexpr uint32_t kControllerMesh = 0x000CF0ECu;
uint32_t s_ctl_mesh_orig = 0;
uint32_t s_ctl_mesh[3] = {};

void controls_show_model(uint8_t* rdram, bool on)
{

    if (!s_ctl_mesh_orig) return;

    const uint32_t m = on ? s_ctl_mesh[(int)s_ctl.dev] : 0;

    wr_w(rdram, kControllerMesh, (int32_t)(m ? m : s_ctl_mesh_orig));
}

// Visible slot i: the top row's end blank becomes '^' with rows above it, the bottom row's '#' with rows below (the scroll arrows ensure_menu_period draws).
std::string ctl_slot_text(int i)
{

    std::string t = ctl_row_text(s_ctl.pos.top + i);
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
    int32_t scale_bits = 0;

    std::memcpy(&scale_bits, &kCtlScale, 4);

    s_ctl = ControlsScreen{};
    s_ctl.shown = true;
    s_ctl.dev = rs64::app::last_device();
    s_ctl.rows = ri::control_rows(s_ctl.dev);

    for (int i = 0; i < kCtlRows; ++i)
    {
        s_ctl.bufs[i] = live_buffer(rdram, "controls#" + std::to_string(i));

        if (!s_ctl.bufs[i]) return -1;

        write_live(rdram, s_ctl.bufs[i], ctl_slot_text(i));
        write_slot(rdram, i, Slot{ (int32_t)s_ctl.bufs[i], SUBTYPE_HOST, 0, (int16_t)kCtlX, ctl_offset_y(base, i, kCtlTopY + kCtlPitch * i), scale_bits });
        s_slot[i] = {};
    }

    const int reset_y = kCtlTopY + kCtlPitch * kCtlRows + kCtlBackGap;

    write_slot(rdram, kCtlResetSlot, Slot{ (int32_t)alloc_str(rdram, kCtlReset), SUBTYPE_HOST, 0, 0, ctl_offset_y(base, kCtlResetSlot, reset_y), scale_bits });
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

// A on a list row: start a capture for the screen's device, or reset that device's bindings.
bool controls_confirm(uint8_t* rdram, int e)
{

    if (!controls_on_screen(rdram) || e > kCtlResetSlot || s_ctl.capturing >= 0) return false;

    const int r = s_ctl.pos.top + e;

    if (e == kCtlResetSlot)
    {
        rs64::app::reset_device_bindings(s_ctl.dev);
        s_ctl.dirty = (1u << kCtlRows) - 1;

        return true;
    }

    if (!rs64::app::capture_device_present(s_ctl.dev)) return true;

    s_ctl.capturing = r;
    s_ctl.flash_row = -1;
    rs64::app::capture_begin({ false, s_ctl.dev });
    s_ctl.dirty |= 1u << e;

    return true;
}

// The capture: the menu sees no presses until it ends; a taken input swaps with the action that held it.
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
    s_ctl.dirty = (1u << kCtlRows) - 1;

    if (c < 0) return;

    std::unique_lock<std::mutex> lk(rs64::app::g_bindings_mtx);
    const ri::Target gave = ri::rebind(rs64::app::g_bindings, s_ctl.rows[r].target, s_ctl.dev, got);

    ri::save_bindings(rs64::app::g_bindings, ri::default_config_path());
    lk.unlock();

    if (gave == ri::Target::Count) return;

    s_ctl.flash_row = r;
    s_ctl.flash = "SWAPPED WITH " + ctl_action_name(gave);
    s_ctl.flash_until = SDL_GetTicks() + kCtlFlashMs;
}

// Each front-end pass before the menu reads the pad: tab stops on entry/exit, the capture, and up/down scrolling the window (left/right would cycle presets).
void controls_pass(uint8_t* rdram, recomp_context* ctx)
{

    if (!controls_on_screen(rdram))
    {
        if (s_ctl.tabs) rs64::mips::mips_call(rdram, ctx, 0x80063EC0u, {5u});

        if (s_ctl.capturing >= 0) rs64::app::capture_end();

        if (s_ctl.shown) controls_show_model(rdram, false);

        // The MODS and LANGUAGE pages read the wheel too (their passes run after this one).
        if (!mods_on_screen(rdram) && !langs_on_screen(rdram)) s_ctl_wheel.store(0);

        s_ctl.shown = false;
        s_ctl.tabs = false;
        s_ctl.capturing = -1;

        return;
    }

    if (!s_ctl.tabs)
    {
        static uint32_t s_stops = 0;

        if (!s_stops) {
            void* p = recomp::alloc(rdram, 4);

            if (!p) return;

            s_stops = (uint32_t)((uint8_t*)p - rdram) + 0x80000000u;
        }

        wr_h(rdram, s_stops - 0x80000000u, (int16_t)kCtlTabStop);
        wr_h(rdram, s_stops - 0x80000000u + 2, (int16_t)kCtlEndStop);
        rs64::mips::mips_call(rdram, ctx, 0x80063E98u, {5u, s_stops, 2u});
        s_ctl.tabs = true;
        s_ctl.dirty = (1u << kCtlRows) - 1;
    }

    if (s_ctl.flash_row >= 0 && SDL_GetTicks() >= s_ctl.flash_until)
    {
        s_ctl.flash_row = -1;
        s_ctl.dirty = (1u << kCtlRows) - 1;
    }

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

    const int top = s_ctl.pos.top;

    // Left/right leave the list for RESET TO DEFAULTS and return to the row left from (the window keeps its place).
    if (!(p & (kNavUp | kNavDown))) {
        if (sel < n) {
            s_ctl.back_from = sel;
            sel = reset;
        } else {
            sel = std::clamp(s_ctl.back_from, s_ctl.pos.top, s_ctl.pos.top + kCtlRows - 1);
        }
    } else if (p & kNavDown) {
        if (sel == back) s_ctl.pos = { 0, 0 };
        else if (sel >= n - 1) ++sel;
        else s_ctl.pos = rs64::menu::scroll_step(s_ctl.pos, 1, n, kCtlRows);
    } else {
        if (sel == 0) sel = back;
        else if (sel == back) sel = reset;
        else if (sel == reset) s_ctl.pos = { n - 1, std::max(0, n - kCtlRows) };
        else s_ctl.pos = rs64::menu::scroll_step(s_ctl.pos, -1, n, kCtlRows);
    }

    if (s_ctl.pos.top != top) s_ctl.dirty = (1u << kCtlRows) - 1;

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
        if (s_ctl.dirty & (1u << i)) write_live(rdram, s_ctl.bufs[i], ctl_slot_text(i));
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
