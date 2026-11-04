// Touch and mouse input on the menus: the touch engine's game context, menu taps, slider and MODS-page drags, and mouse hover/clicks.

#include "input/input_host.h"
#include "game/menus/menu_config.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <tuple>
#include "os_compat.h"
#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "librecomp/game.hpp"
#include "input/rumble.h"
#include "input/touch_config.h"
#include "game/game_state.h"
#include "util/debug_logs.h"
#include "util/rdram_words.h"
#include "host_api/host_api.h"
#include "rs64_toml_hooks.h"
#include "main/main.h"
#include "game/upstream_compat.h"
#include "game/hook_helpers.h"
#include "game/nav_sequencer.h"
#include "video/rt64_render_context.h"
#include "video/video_config.h"
#include "main/crash.h"
#include "main/ui_panels.h"
#ifdef __ANDROID__
#include "main/android_host.h"
#endif

using recomp::dbg::env_on;
using recomp::dbg::env_int;

extern "C" volatile uint8_t* volatile g_recomp_rdram_for_wp_raw;
namespace RT64 {
    extern std::atomic<float> presentShiftY;
}

extern SDL_Window* g_sdl_window;

namespace rs64::app {


// Touch engine: finger/sensor events arrive on the SDL pump thread, polls on the game thread.
rs64::touch::Engine g_touch;
std::mutex g_touch_mtx;
static rs64::touch::MenuSnapshot g_touch_menu_at_down{};


// Set by the TOUCH LAYOUT menu action on the game thread; the next touch poll opens the editor.
static std::atomic<bool> g_touch_edit_request{false};

extern "C" void rs64_touch_layout_request(void) { g_touch_edit_request.store(true); }

uint8_t* touch_rdram() {
    return (uint8_t*)g_recomp_rdram_for_wp_raw;
}

static bool touch_state_is(const char* want);

rs64::touch::Context touch_context()
{

    uint8_t* r = touch_rdram();

    // A mod page is a list menu, though its title in host RAM leaves the classifier at "unknown".
    if (g_active_overlay == 1 && rs64_menu_page_shown())
    {
        return rs64::touch::Context::ListMenu;
    }

    // The account menu (select game / level / craft) is carousel-like on every screen; its classifier sub-states are unreliable.
    if (r && g_active_overlay == 1 && rs64::touch::account_screen(r) != rs64::touch::AccountScreen::NotAccount)
    {
        return rs64::touch::Context::Carousel;
    }

    // A menu whose title lives on the mod heap (mod YES/NO confirms, relabeled menus) classifies as "unknown"; resident menu entries make it a list.
    if (r && g_active_overlay == 1 && touch_state_is("unknown") && !rs64_state_in_cinematic())
    {
        float w, h;
        window_size(&w, &h);
        rs64::touch::MenuSnapshot snap{};
        std::vector<rs64::touch::Box> boxes;

        if (rs64::touch::read_menu(r, &snap, &boxes, w, h) && !boxes.empty())
        {
            return rs64::touch::Context::ListMenu;
        }
    }

    return rs64::touch::classify(g_active_overlay, rs64_state_current_id(), r && rs64::touch::read_paused(r), r && rs64::touch::read_demo(r));
}

rs64::touch::AccountScreen touch_account_screen() {

    uint8_t* r = touch_rdram();

    if (!r || g_active_overlay != 1)
    {
        return rs64::touch::AccountScreen::NotAccount;
    }

    return rs64::touch::account_screen(r);
}

static bool touch_state_is(const char* want) {

    const char* id = rs64_state_current_id();

    return id && std::strcmp(id, want) == 0;
}

// A volume bar (Sound Settings or a mod page slider) is open, opened by any input: the highlight must not move until it closes.
static bool sound_bar_editing()
{
    return rs64_menu_slider_editing() != 0;
}

// Tap-to-select only where the resident menu data is what's on screen: list menus, Passcodes' ENTER CODE/BACK, SELECT GAME's ERASE GAME.
bool touch_menu_hits_allowed()
{

    return touch_context() == rs64::touch::Context::ListMenu || touch_state_is("menu.passcodes") ||
           touch_account_screen() == rs64::touch::AccountScreen::SelectGame;
}

// SOUND SETTINGS volume bar being edited (opened by a touch): touches on the bar set the value from the finger's x.
struct TouchSlider {
    bool active = false;
    int entry = -1;
    uint32_t opened_ms = 0;
    int64_t finger = -1;
    int last_value = -1;

    // A touch off the bar just closed it: that tap only closes the game's slider edit.
    uint32_t close_ms = 0;
};

static TouchSlider g_touch_slider;

void window_size(float* w, float* h) {

    int iw = 0, ih = 0;

    if (g_sdl_window) {
        SDL_GetWindowSize(g_sdl_window, &iw, &ih);
    }

    *w = (float)iw;
    *h = (float)ih;
}

// MODS page: pressing a mod's name and dragging carries the mod (the game thread applies each step); a press that never moved stays a tap.
struct ModsDrag {
    int64_t finger = -1;
    int slot = -1;
    float y = 0.0f;
    bool carrying = false;
};

static ModsDrag g_mods_drag;

static bool mods_drag_event(rs64::touch::FingerEvent ev, int64_t finger, float x, float y)
{

    using rs64::touch::FingerEvent;
    ModsDrag& d = g_mods_drag;
    uint8_t* r = touch_rdram();
    float w, h;
    window_size(&w, &h);
    rs64::touch::MenuSnapshot snap{};
    std::vector<rs64::touch::Box> boxes;

    if (!r || !rs64::touch::read_menu(r, &snap, &boxes, w, h))
    {
        d = ModsDrag{};

        return false;
    }

    if (ev == FingerEvent::Down)
    {
        d = ModsDrag{};

        for (const rs64::touch::Box& b : boxes)
        {
            const bool on_name = b.x1 > b.x0 && (x - b.x0) / (b.x1 - b.x0) < rs64_mods_toggle_from();

            if (x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1 && on_name && rs64_mods_page_row(b.entry) >= 0) {
                d.finger = finger;
                d.slot = b.entry;
                d.y = y;
            }
        }

        return false;
    }

    if (d.finger != finger)
    {
        return false;
    }

    if (ev == FingerEvent::Up)
    {
        const bool carried = d.carrying;

        d = ModsDrag{};

        if (carried) rs64_mods_drag(2, -1);

        return carried;
    }

    if (!d.carrying)
    {
        if (std::fabs(y - d.y) < 0.02f) return false;

        d.carrying = true;
        rs64_mods_drag(0, d.slot);
    }

    // The row under the point, or above the first row (-1) / below the last (99), which scroll the list.
    int to = -2;
    float top = 2.0f, bottom = -1.0f;

    for (const rs64::touch::Box& b : boxes)
    {
        if (rs64_mods_page_row(b.entry) < 0) continue;

        top = std::min(top, b.y0);
        bottom = std::max(bottom, b.y1);

        if (y >= b.y0 && y <= b.y1) to = b.entry;
    }

    if (to == -2) to = y < top ? -1 : y > bottom ? 99 : -2;

    if (to != -2) rs64_mods_drag(1, to);

    return true;
}

// Returns true if the finger event was on the open volume bar. The game steps 1 per held frame in its slider edit
// mode and applies the mixer itself, so write one step short of the target and inject one frame toward it.
static bool touch_slider_event(rs64::touch::FingerEvent ev, int64_t finger, float x, float y)
{

    using rs64::touch::FingerEvent;
    TouchSlider& s = g_touch_slider;
    uint8_t* r = touch_rdram();
    float w, h;
    window_size(&w, &h);

    // The bar is open only while the game edits it (a tap's open takes a moment to reach the game); a stale one would turn
    // the next touch into a stick nudge, which steps the highlight.
    if (s.active && !sound_bar_editing() && SDL_GetTicks() - s.opened_ms > 300)
    {
        s.active = false;
    }

    // A bar opened from the keyboard or pad takes touches too.
    if (!s.active && r && ev == FingerEvent::Down && sound_bar_editing()) {
        rs64::touch::MenuSnapshot snap{};
        std::vector<rs64::touch::Box> boxes;

        if (rs64::touch::read_menu(r, &snap, &boxes, w, h)) {
            s = TouchSlider{};
            s.active = true;
            s.entry = snap.current;
        }
    }

    rs64::touch::SliderBar bar{};

    if (!s.active || !r || !rs64::touch::sound_slider(r, s.entry, w, h, &bar))
    {
        s.active = false;

        return false;
    }

    if (const int ch = rs64_menu_slider_channel(s.entry); ch >= 0) {
        bar.channel = ch;
    }

    if (ev == FingerEvent::Down) {
        if (y < bar.y0 || y > bar.y1)
        {
            // Off the bar: this touch closes the slider (an overlay B does it itself; a tap sends only A, see touch_menu_tap).
            s.active = false;
            s.close_ms = SDL_GetTicks();

            return false;
        }

        s.finger = finger;
        s.last_value = -1;
    }

    if (finger != s.finger)
    {
        return false;
    }

    if (ev == FingerEvent::Up) {
        s.finger = -1;

        return true;
    }

    // The bar fades in for about 1/3 s after opening and ignores input meanwhile.
    if (SDL_GetTicks() - s.opened_ms < 400)
    {
        return true;
    }

    const int target = rs64::touch::slider_value_at(bar, x);

    if (target != s.last_value) {
        s.last_value = target;

        if (target > 0) {
            rs64::touch::write_volume(r, bar.channel, (uint8_t)(target - 1));
            g_touch.inject(0, 1.0f, 0.0f, 1);
        } else {
            rs64::touch::write_volume(r, bar.channel, 1);
            g_touch.inject(0, -1.0f, 0.0f, 1);
        }
    }

    return true;
}

// Menu/carousel tap handling (called by the engine with g_touch_mtx held): per-screen targets, then menu entry hit-test.
rs64::touch::TapAction touch_menu_tap(float x, float y)
{

    using rs64::touch::TapAction;
    float w, h;
    window_size(&w, &h);
    uint8_t* r = touch_rdram();

    if (!r)
    {
        return TapAction::None;
    }

    // A tap on the picture slid up above the keyboard lands where that spot is drawn unshifted.
    y += RT64::presentShiftY.load();

    if (touch_context() == rs64::touch::Context::PauseMenu)
    {
        const int k = rs64::touch::pause_hover(r, x, y, w, h);

        if (k < 0)
        {
            return TapAction::None;
        }

        // A mod text slider on this row takes the value under the point (its line's drawn text extent).
        std::vector<rs64::touch::PauseLine> lines;
        float x0 = 1.0f, x1 = 0.0f;

        if (rs64::touch::read_pause_lines(r, w, h, &lines)) {
            for (const rs64::touch::PauseLine& l : lines) {
                if (l.selectable == k && !l.bar) {
                    x0 = std::min(x0, l.x0);
                    x1 = std::max(x1, l.x1);
                }
            }
        }

        const int next = rs64::touch::pause_entry_next(r, rs64::touch::pause_submenu(r), k);

        if (next >= 0 && x1 > x0) {
            rs64_menu_slider_click(next, (x - x0) / (x1 - x0));
        }

        return TapAction::Confirm;
    }

    // PASSCODES: the engine steps on side touches itself; a tap on the wheel's center enters the letter.
    if (touch_context() == rs64::touch::Context::Wheel && rs64::touch::on_wheel(y))
    {
        return TapAction::Confirm;
    }

    const rs64::touch::AccountScreen acct = touch_account_screen();

    if (acct == rs64::touch::AccountScreen::SelectGame) {
        const TapAction slot = rs64::touch::select_game_tap(x, y, w, h);

        if (slot != TapAction::None)
        {
            return slot;
        }
    } else if (acct == rs64::touch::AccountScreen::Levels) {
        return rs64::touch::level_select_tap(x, y);
    } else if (acct == rs64::touch::AccountScreen::Craft) {
        return rs64::touch::craft_select_tap(x, y);
    } else if (touch_context() == rs64::touch::Context::Carousel) {
        const TapAction arrow = rs64::touch::carousel_arrow_tap(x, y);

        if (arrow != TapAction::None)
        {
            return arrow;
        }
    }

    // The first tap after leaving an open volume bar only closes the game's slider edit (A), without moving the highlight.
    if (sound_bar_editing() || (g_touch_slider.close_ms != 0 && SDL_GetTicks() - g_touch_slider.close_ms < 600))
    {
        g_touch_slider.close_ms = 0;

        return TapAction::Confirm;
    }

    g_touch_slider.close_ms = 0;
    g_touch_slider.active = false;

    if (!touch_menu_hits_allowed() || !rs64::touch::tap_select(r, g_touch_menu_at_down, x, y, w, h))
    {
        return TapAction::None;
    }

    // Confirming a volume entry opens its bar; remember it so touches on the bar set the value.
    rs64::touch::MenuSnapshot snap{};
    std::vector<rs64::touch::Box> boxes;
    rs64::touch::SliderBar bar{};
    const bool menu_ok = rs64::touch::read_menu(r, &snap, &boxes, w, h);

    for (const rs64::touch::Box& b : boxes) {
        if (menu_ok && b.entry == snap.current && b.x1 > b.x0) {
            rs64_menu_slider_click(b.entry, (x - b.x0) / (b.x1 - b.x0));
        }
    }

    if (menu_ok && rs64::touch::sound_slider(r, snap.current, w, h, &bar)) {
        g_touch_slider.active = true;
        g_touch_slider.entry = snap.current;
        g_touch_slider.opened_ms = SDL_GetTicks();
        g_touch_slider.finger = -1;
    }

    return TapAction::Confirm;
}

// Pause volume bars: a touch on a bar highlights its row and sets the value from the finger's x while it drags.
// The pause slider steps by a frame-time-scaled amount, so write one short and inject one frame of right (lands within 1).
struct TouchPauseSlider {
    int64_t finger = -1;
    int channel = -1;
    int row = -1;
    int last_value = -1;
};

static TouchPauseSlider g_touch_pause_slider;

static bool touch_pause_slider_event(rs64::touch::FingerEvent ev, int64_t finger, float x, float y) {

    using rs64::touch::FingerEvent;
    TouchPauseSlider& s = g_touch_pause_slider;
    uint8_t* r = touch_rdram();

    if (!r || touch_context() != rs64::touch::Context::PauseMenu) {
        s.finger = -1;

        return false;
    }

    float w, h;
    window_size(&w, &h);
    std::vector<rs64::touch::PauseLine> lines;

    if (!rs64::touch::read_pause_lines(r, w, h, &lines))
    {
        return false;
    }

    const rs64::touch::PauseLine* bar = nullptr;

    if (ev == FingerEvent::Down) {
        for (const rs64::touch::PauseLine& l : lines)
        {
            if (l.bar && l.channel >= 0 && x >= l.x0 - 0.03f && x <= l.x1 + 0.03f && y >= l.y0 - 0.02f && y <= l.y1 + 0.02f)
            {
                bar = &l;
            }
        }

        if (!bar)
        {
            return false;
        }

        s = { finger, bar->channel, bar->selectable, -1 };
        rs64::touch::pause_set_entry(r, s.row);
    } else {
        if (finger != s.finger)
        {
            return false;
        }

        if (ev == FingerEvent::Up) {
            s.finger = -1;

            return true;
        }

        for (const rs64::touch::PauseLine& l : lines) {
            if (l.bar && l.channel == s.channel) {
                bar = &l;
            }
        }

        if (!bar)
        {
            return true;
        }
    }

    const int target = rs64::touch::pause_slider_value(*bar, x);

    if (target != s.last_value)
    {
        s.last_value = target;
        rs64::touch::write_volume(r, s.channel, (uint8_t)(target > 0 ? target - 1 : 0));
        g_touch.inject(0, 1.0f, 0.0f, 1);
    }

    return true;
}

// Horizontal wheel notches (+ right): an open volume bar or a highlighted pause bar steps a tenth of its range, and SELECT LEVEL,
// the hangar and the carousels step left/right.
void menu_wheel_x(int steps)
{

    uint8_t* r = touch_rdram();

    if (!r || steps == 0 || g_show_controls.load() || rs64_rt64_inspector_open())
    {
        return;
    }

    std::lock_guard<std::mutex> lk(g_touch_mtx);
    float w, h;
    window_size(&w, &h);
    int channel = -1, top = 128;

    if (sound_bar_editing()) {
        rs64::touch::MenuSnapshot snap{};
        std::vector<rs64::touch::Box> boxes;
        rs64::touch::SliderBar bar{};

        if (rs64::touch::read_menu(r, &snap, &boxes, w, h) && rs64::touch::sound_slider(r, snap.current, w, h, &bar)) {
            const int ch = rs64_menu_slider_channel(snap.current);

            channel = ch >= 0 ? ch : bar.channel;
        }
    } else if (touch_context() == rs64::touch::Context::PauseMenu) {
        std::vector<rs64::touch::PauseLine> lines;
        const int k = rs64::touch::pause_entry(r);

        if (k >= 0 && rs64::touch::read_pause_lines(r, w, h, &lines)) {
            for (const rs64::touch::PauseLine& l : lines) {
                if (l.bar && l.channel >= 0 && l.selectable == k) channel = l.channel;
            }
        }

        top = 127;
    }

    if (channel >= 0) {
        const int target = std::clamp((int)rs64::touch::read_volume(r, channel) + steps * ((top + 9) / 10), 0, top);

        // As touch_slider_event: one step short of the target plus one frame toward it, so the game applies the mixer.
        rs64::touch::write_volume(r, channel, (uint8_t)(target > 0 ? target - 1 : 1));
        g_touch.inject(0, target > 0 ? 1.0f : -1.0f, 0.0f, 1);

        return;
    }

    const rs64::touch::AccountScreen acct = touch_account_screen();

    if (acct == rs64::touch::AccountScreen::Levels || acct == rs64::touch::AccountScreen::Craft || touch_context() == rs64::touch::Context::Carousel) {
        for (int i = 0; i < std::abs(steps); ++i) {
            g_touch.inject(0, steps > 0 ? 1.0f : -1.0f, 0.0f, 3);
        }
    }
}

// Esc in the pause menu (bound to Start, which confirms the highlighted row): a submenu goes back with B and the caller drops the key;
// the root highlights its first row (resume) for the Start to confirm, so Esc never picks the row the mouse hovers.
bool pause_escape()
{

    uint8_t* r = touch_rdram();

    if (!r || touch_context() != rs64::touch::Context::PauseMenu)
    {
        return false;
    }

    std::lock_guard<std::mutex> lk(g_touch_mtx);
    const int menu = rs64::touch::pause_submenu(r);

    if (menu == 0) {
        rs64::touch::pause_set_entry(r, 0);
    } else if (menu > 0) {
        g_touch.inject(rs64::touch::B, 0.0f, 0.0f, 3);
    }

    return menu > 0;
}

// Menus where the cursor is freed from steering capture and clicks go through the touch menu layer.
bool mouse_menu_context(rs64::touch::Context ctx)
{

    using rs64::touch::Context;

    return ctx == Context::ListMenu || ctx == Context::Carousel || ctx == Context::PauseMenu || ctx == Context::Wheel;
}

// Free cursor in a menu: hover highlights, left click acts as a tap, right click is B. In a cutscene a left click is START (skip), captured or not.
void menu_mouse_event(const SDL_Event& e)
{

    const uint32_t which = (e.type == SDL_MOUSEMOTION) ? e.motion.which : e.button.which;

    if (which == SDL_TOUCH_MOUSEID || g_show_controls.load() || rs64_rt64_inspector_open() || rs64::touch::shared_config().enabled)
    {
        return;
    }

    // SDL still sends motion over an unfocused window.
    if (e.type == SDL_MOUSEMOTION && !SDL_GetKeyboardFocus() && which != kMcpMouseId)
    {
        return;
    }

    const rs64::touch::Context ctx = touch_context();

    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT && (ctx == rs64::touch::Context::Cutscene || rs64_state_in_cinematic()))
    {
        std::lock_guard<std::mutex> lk(g_touch_mtx);
        g_touch.inject(rs64::touch::START, 0.0f, 0.0f, 3);

        return;
    }

    if (g_mouse_capture.load())
    {
        return;
    }

    float w, h;
    window_size(&w, &h);
    uint8_t* r = touch_rdram();

    if (!r || !mouse_menu_context(ctx) || w <= 0.0f || h <= 0.0f)
    {
        return;
    }

    std::lock_guard<std::mutex> lk(g_touch_mtx);

    // Clicks use the last motion position, the point hover tested (an injected button event can carry a stale one).
    static float s_mx = -1.0f, s_my = -1.0f;

    if (e.type == SDL_MOUSEMOTION) {
        s_mx = e.motion.x / w;
        s_my = e.motion.y / h;
    }

    const float fx = (s_mx >= 0.0f) ? s_mx : e.button.x / w;
    const float fy = (s_my >= 0.0f) ? s_my : e.button.y / h;

    // Volume bars: the left button acts as a finger, so a click sets the value and a drag slides it.
    if (e.type == SDL_MOUSEMOTION || e.button.button == SDL_BUTTON_LEFT)
    {
        using rs64::touch::FingerEvent;
        constexpr int64_t kMouseFinger = -2;
        const FingerEvent ev = (e.type == SDL_MOUSEBUTTONDOWN) ? FingerEvent::Down : (e.type == SDL_MOUSEBUTTONUP) ? FingerEvent::Up : FingerEvent::Move;

        if (touch_slider_event(ev, kMouseFinger, fx, fy) || touch_pause_slider_event(ev, kMouseFinger, fx, fy) || mods_drag_event(ev, kMouseFinger, fx, fy))
        {
            return;
        }
    }

    rs64::touch::MenuSnapshot snap{};
    std::vector<rs64::touch::Box> boxes;

    if (e.type == SDL_MOUSEMOTION)
    {
        // Highlight only when the cursor enters a different entry, so a bump doesn't undo a keyboard/pad choice.
        static int64_t s_hovered = -1;
        const float x = fx, y = fy;
        int64_t hovered = -1;
        std::vector<rs64::touch::PauseLine> lines;

        if (ctx == rs64::touch::Context::PauseMenu)
        {
            if (rs64::touch::read_pause_lines(r, w, h, &lines))
            {
                for (const rs64::touch::PauseRow& row : rs64::touch::pause_rows(lines)) {
                    if (x >= row.x0 && x <= row.x1 && y >= row.y0 && y <= row.y1)
                    {
                        hovered = ((int64_t)rs64::touch::pause_submenu(r) << 8) | row.selectable;
                    }
                }
            }
        } else if (touch_menu_hits_allowed() && rs64::touch::read_menu(r, &snap, &boxes, w, h))
        {
            for (const rs64::touch::Box& b : boxes) {
                if (x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1)
                {
                    hovered = ((int64_t)snap.menu_ptr << 16) | ((int64_t)snap.menu_id << 8) | b.entry;
                }
            }
        }

        if (hovered != s_hovered && hovered >= 0 && !sound_bar_editing())
        {
            const bool pause = ctx == rs64::touch::Context::PauseMenu;
            const bool moved = pause ? rs64::touch::pause_hover(r, x, y, w, h) >= 0 : rs64::touch::tap_select(r, snap, x, y, w, h);

            if (moved) {
                rs64_menu_hover_sound(pause ? 1 : 0);
            }
        }

        // While a bar is open nothing counts as hovered, so the first move after it closes highlights the entry under the cursor.
        s_hovered = sound_bar_editing() ? -1 : hovered;
    } else if (e.button.button == SDL_BUTTON_LEFT && e.type == SDL_MOUSEBUTTONDOWN) {
        rs64::touch::read_menu(r, &g_touch_menu_at_down, &boxes, w, h);
    } else if (e.button.button == SDL_BUTTON_LEFT && e.type == SDL_MOUSEBUTTONUP) {
        g_touch.tap(ctx, fx, fy);
    } else if (e.button.button == SDL_BUTTON_RIGHT && e.type == SDL_MOUSEBUTTONDOWN) {
        g_touch.inject(rs64::touch::B, 0.0f, 0.0f, 3);
    }
}

void touch_event(const SDL_Event& e)
{

    using rs64::touch::FingerEvent;
    const FingerEvent ev = (e.type == SDL_FINGERDOWN) ? FingerEvent::Down : (e.type == SDL_FINGERUP) ? FingerEvent::Up : FingerEvent::Move;
    std::lock_guard<std::mutex> lk(g_touch_mtx);

    if (g_touch.editing())
    {
        g_touch.finger((int64_t)e.tfinger.fingerId, ev, e.tfinger.x, e.tfinger.y, e.tfinger.timestamp);

        return;
    }

    if (touch_slider_event(ev, (int64_t)e.tfinger.fingerId, e.tfinger.x, e.tfinger.y))
    {
        return;
    }

    if (touch_pause_slider_event(ev, (int64_t)e.tfinger.fingerId, e.tfinger.x, e.tfinger.y))
    {
        return;
    }

    if (mods_drag_event(ev, (int64_t)e.tfinger.fingerId, e.tfinger.x, e.tfinger.y))
    {
        return;
    }

    if (ev == FingerEvent::Down) {
        std::vector<rs64::touch::Box> unused;
        float w, h;
        window_size(&w, &h);

        if (uint8_t* r = touch_rdram()) {
            rs64::touch::read_menu(r, &g_touch_menu_at_down, &unused, w, h);
        }
    }

    g_touch.finger((int64_t)e.tfinger.fingerId, ev, e.tfinger.x, e.tfinger.y, e.tfinger.timestamp);
}

void touch_slider_cancel() {
    g_touch_slider.active = false;
}

bool take_touch_edit_request() {
    return g_touch_edit_request.exchange(false);
}

}
