#include "input/input_host.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include "os_compat.h"
#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "librecomp/game.hpp"
#include "input/rumble.h"
#include "input/touch_config.h"
#include "game/game_state.h"
#include "util/debug_logs.h"
#include "host_api/host_api.h"
#include "main/main.h"
#include "game/upstream_compat.h"
#include "game/hook_helpers.h"
#include "game/nav_sequencer.h"
#include "video/rt64_render_context.h"
#include "video/video_config.h"
#include "main/crash.h"
#ifdef __ANDROID__
#include "main/android_host.h"
#endif

using recomp::dbg::env_on;
using recomp::dbg::env_int;

// N64 button bitmasks (from libultra PR/controller.h)
#define N64_A_BUTTON     0x8000
#define N64_B_BUTTON     0x4000
#define N64_Z_TRIG       0x2000
#define N64_START_BUTTON 0x1000
#define N64_U_JPAD       0x0800
#define N64_D_JPAD       0x0400
#define N64_L_JPAD       0x0200
#define N64_R_JPAD       0x0100
#define N64_L_TRIG       0x0020
#define N64_R_TRIG       0x0010
#define N64_U_CBUTTONS   0x0008
#define N64_D_CBUTTONS   0x0004
#define N64_L_CBUTTONS   0x0002
#define N64_R_CBUTTONS   0x0001

extern "C" volatile uint8_t* volatile g_recomp_rdram_for_wp_raw;
namespace RT64 {
    extern std::atomic<float> presentShiftY;
}

// Fullscreen: the roguesq_video.json setting, set/toggled from any thread (menu, UI); applied on the main thread
// in poll_input via SDL_SetWindowFullscreen (RT64 resizes its swapchain off the resulting resize event).
SDL_Window* g_sdl_window = nullptr;

// The refresh rates of the display the window is on, for the menu's TARGET FPS choices.
static void publish_refresh_rates()
{

    const int disp = SDL_GetWindowDisplayIndex(g_sdl_window);
    const int n = disp >= 0 ? SDL_GetNumDisplayModes(disp) : 0;
    std::vector<int> hz;

    for (int i = 0; i < n; ++i)
    {
        SDL_DisplayMode m{};

        if (SDL_GetDisplayMode(disp, i, &m) == 0 && m.refresh_rate > 0) hz.push_back(m.refresh_rate);
    }

    rs64::video::set_refresh_rates(std::move(hz));
}

namespace rs64::app {
SDL_GameController* controller = nullptr;

// Every open gamepad; `controller` is whichever one last produced input (Steam Input / DS4Windows can expose an idle virtual pad alongside the real one).
std::vector<SDL_GameController*> g_pads;

// Set when a pad connects so the next lightbar tick re-sends the current color.
static std::atomic<bool> g_lightbar_resend{true};

// ROGUESQ_FAKE_CONTROLLER=1 — report a connected controller and feed neutral
// input even with no physical gamepad attached. Lets headless/automated runs
// pass the game's "NO CONTROLLER" gate and reach the cinematic/menu for
// screenshot + DL-dump iteration (no controller = oracle is the user otherwise).
// ROGUESQ_AUTO_START=<ms>: once past the gate, pulse START every <ms> to advance
// through prompts (0/unset = never). Self-test aid only; default OFF.
static bool fake_controller_enabled() {

    static bool s = env_on("ROGUESQ_FAKE_CONTROLLER");

    return s;
}

// ROGUESQ_INPUT_SEQ="name:holdMs:afterMs,..." — a scripted virtual-controller
// timeline injected at get_n64_input (no window focus needed, unlike SendInput).
// ROGUESQ_INPUT_DELAY=<ms> waits before the first step (default 12000, to clear boot).
// Names: start a b z l r  dup ddown dleft dright  cup cdown cleft cright
//        up down left right (analog stick)  wait (neutral hold).
// Used for headless front-end navigation + state calibration. Default OFF.
struct InputStep { uint16_t mask; float sx, sy; uint32_t hold, after; };

static bool scripted_input(uint16_t* buttons, float* x, float* y)
{

    static int s_init = -1;
    static std::vector<InputStep> s_steps;
    static uint32_t s_delay = 12000;
    static uint32_t s_t0 = 0;            // wall-clock at first eligible call
    static bool s_done_logged = false;

    if (s_init < 0) {
        s_init = 0;
        const char* seq = recomp::dbg::env_str("ROGUESQ_INPUT_SEQ");

        if (seq && *seq) {
            s_init = 1;
            s_delay = (uint32_t)env_int("ROGUESQ_INPUT_DELAY", 12000);

            auto tok = [](const std::string& n) -> InputStep {

                InputStep s{0, 0.f, 0.f, 90, 400};
                std::string name = n;
                size_t c1 = name.find(':');
                std::string hold, after;

                if (c1 != std::string::npos)
                {
                    size_t c2 = name.find(':', c1 + 1);
                    hold = name.substr(c1 + 1, (c2 == std::string::npos ? std::string::npos : c2 - c1 - 1));

                    if (c2 != std::string::npos) after = name.substr(c2 + 1);

                    name = name.substr(0, c1);
                }

                if (!hold.empty()) s.hold = (uint32_t)std::atoi(hold.c_str());
                if (!after.empty()) s.after = (uint32_t)std::atoi(after.c_str());

                if      (name == "start") s.mask = N64_START_BUTTON;
                else if (name == "a")     s.mask = N64_A_BUTTON;
                else if (name == "b")     s.mask = N64_B_BUTTON;
                else if (name == "z")     s.mask = N64_Z_TRIG;
                else if (name == "l")     s.mask = N64_L_TRIG;
                else if (name == "r")     s.mask = N64_R_TRIG;
                else if (name == "dup")   s.mask = N64_U_JPAD;
                else if (name == "ddown") s.mask = N64_D_JPAD;
                else if (name == "dleft") s.mask = N64_L_JPAD;
                else if (name == "dright")s.mask = N64_R_JPAD;
                else if (name == "cup")   s.mask = N64_U_CBUTTONS;
                else if (name == "cdown") s.mask = N64_D_CBUTTONS;
                else if (name == "cleft") s.mask = N64_L_CBUTTONS;
                else if (name == "cright")s.mask = N64_R_CBUTTONS;
                else if (name == "up")    s.sy = 1.0f;
                else if (name == "down")  s.sy = -1.0f;
                else if (name == "left")  s.sx = -1.0f;
                else if (name == "right") s.sx = 1.0f;

                // "wait" / unknown -> neutral hold
                return s;
            };

            std::string all(seq), cur;

            for (size_t i = 0; i <= all.size(); ++i)
            {
                if (i == all.size() || all[i] == ',') { if (!cur.empty()) s_steps.push_back(tok(cur)); cur.clear(); }
                else cur.push_back(all[i]);
            }
        }
    }

    if (s_init != 1) return false;

    uint32_t now = SDL_GetTicks();

    if (s_t0 == 0) s_t0 = now;

    uint32_t elapsed = now - s_t0;

    if (elapsed < s_delay) { *buttons = 0; *x = 0.f; *y = 0.f; return true; }

    uint32_t t = elapsed - s_delay;
    uint32_t acc = 0;

    for (const InputStep& st : s_steps) {
        uint32_t span = st.hold + st.after;

        if (t < acc + span) {
            bool holding = (t - acc) < st.hold;
            *buttons = holding ? st.mask : 0;
            *x = holding ? st.sx : 0.f;
            *y = holding ? st.sy : 0.f;

            return true;
        }

        acc += span;
    }

    if (!s_done_logged) { s_done_logged = true; fprintf(stderr, "[input-seq] sequence complete at t=%ums\n", elapsed); fflush(stderr); }

    *buttons = 0;
    *x = 0.f;
    *y = 0.f;

    return true;
}

// Requested by the custom main-menu QUIT entry (menuControllerInput hook).
// Pushes SDL_QUIT so the event loop runs the graceful shutdown path.
extern "C" void rs64_menu_request_quit(void) {

    SDL_Event q;
    q.type = SDL_QUIT;
    SDL_PushEvent(&q);
}

// Active bindings + mouse-steering runtime state. poll_input and get_n64_input
// run on the same game thread and in a paired sequence per controller read, so
// the mouse accumulators are plain game-thread statics. g_mouse_capture is read
// from update_gfx (main thread) to toggle relative-mouse mode, so it is atomic.
rs64::input::Bindings g_bindings = rs64::input::default_bindings();
std::mutex            g_bindings_mtx;
std::atomic<bool>     g_show_controls{false};
std::atomic<bool>     g_mouse_capture{false};
static float                 g_mouse_ax = 0.0f;   // accumulated relative motion this frame
static float                 g_mouse_ay = 0.0f;
static uint32_t              g_mouse_btn = 0;

std::vector<OpenJoy>       g_open_joys;
std::vector<SDL_Joystick*> g_joy_by_dev;

// Throttle lever position [0,1] from the last input poll, -1 when no throttle is bound or connected.
std::atomic<float> g_throttle{-1.0f};

// Lever position the throttle hook would use without the lockstep harness; ROGUESQ_THROTTLE=<0..1> forces one (headless tests).
extern "C" float rs64_throttle_live(void)
{

    static const float s_forced = []() { const char* e = recomp::dbg::env_str("ROGUESQ_THROTTLE"); return e ? (float)std::atof(e) : -1.0f; }();

    return s_forced >= 0.0f ? s_forced : g_throttle.load();
}

extern "C" float rs64_throttle_cruise_live(void) {

    std::lock_guard<std::mutex> lk(g_bindings_mtx);

    return g_bindings.throttle_cruise;
}

// Per-craft speed hooks in rogue_squadron.toml call this just before the craft's current speed steps toward its
// target. Addresses are f32 game constants (base_addr 0 = unscaled, cruise_addr 0 = none); a nonzero f32 at
// skip_addr (a scripted boost) keeps the game's target.
extern "C" float rs64_throttle_hook(uint8_t* rdram, float target, uint32_t base_addr, uint32_t floor_addr, uint32_t cruise_addr, uint32_t cap_addr, uint32_t skip_addr)
{

    const float p = rs64_ls_throttle(rs64_throttle_live());

    if (p < 0.0f) return target;

    auto f32 = [rdram](uint32_t addr) { float f; memcpy(&f, rdram + (addr - 0x80000000u), 4); return f; };

    if (skip_addr && f32(skip_addr) != 0.0f) return target;

    const float base = base_addr ? f32(base_addr) : 1.0f;
    const float min_speed = base * f32(floor_addr);
    const float max_speed = base * f32(cap_addr);
    const float cruise_speed = cruise_addr ? base * f32(cruise_addr) : -1.0f;

    if (!(max_speed > min_speed)) return target;

    const float cruise_at = rs64_ls_cruise(rs64_throttle_cruise_live());
    const float speed = rs64::input::throttle_speed(p, cruise_at, min_speed, cruise_speed, max_speed);

    if (recomp::dbg::log_throttle())
    {
        static int n = 0;

        if ((++n % 60) == 1) fprintf(stderr, "[throttle] pos=%.2f range=%.3f/%.3f/%.3f game=%.3f -> %.3f\n", p, min_speed, cruise_speed, max_speed, target, speed);
    }

    return speed;
}

// Rumble Pak reported to the game (all 13 built-in effects). Set from rumble.enabled at startup or when it is
// turned on; never cleared, since the game only probes for the pak at mission start. Turning rumble off mutes the host output.
std::atomic<bool> g_report_rumble_pak{false};

static void rebuild_joy_map_locked() {

    g_joy_by_dev.assign(g_bindings.joy_devices.size(), nullptr);

    for (const OpenJoy& oj : g_open_joys) {
        for (size_t d = 0; d < g_bindings.joy_devices.size(); ++d)
        {
            if (g_bindings.joy_devices[d].guid == oj.guid && g_bindings.joy_devices[d].ordinal == oj.ordinal)
            {
                g_joy_by_dev[d] = oj.joy;
            }
        }
    }
}

static void open_raw_joystick(int device_index)
{

    if (SDL_IsGameController(device_index)) return;

    SDL_Joystick* j = SDL_JoystickOpen(device_index);

    if (!j) return;

    std::lock_guard<std::mutex> lk(g_bindings_mtx);
    OpenJoy oj;
    oj.joy = j;
    oj.id = SDL_JoystickInstanceID(j);

    for (const OpenJoy& o : g_open_joys) {
        if (o.id == oj.id) return;
    }

    char guid[64] = {};
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(j), guid, sizeof(guid));
    oj.guid = guid;

    for (const OpenJoy& o : g_open_joys) {
        if (o.guid == oj.guid) oj.ordinal++;
    }

    const char* name = SDL_JoystickName(j);
    const int axes = SDL_JoystickNumAxes(j), buttons = SDL_JoystickNumButtons(j), hats = SDL_JoystickNumHats(j);
    bool added = false;
    const int dev = rs64::input::find_or_add_joy_device(g_bindings, oj.guid, oj.ordinal, name ? name : "", &added);

    if (added && dev >= 0)
    {
        rs64::input::add_joystick_defaults(g_bindings, dev, axes, buttons, hats);
        rs64::input::save_bindings(g_bindings, rs64::input::default_config_path());
    }

    if (!SDL_JoystickHasRumble(j) && SDL_JoystickIsHaptic(j) == SDL_TRUE)
    {
        SDL_Haptic* h = SDL_HapticOpenFromJoystick(j);

        if (h && SDL_HapticRumbleSupported(h) == SDL_TRUE && SDL_HapticRumbleInit(h) == 0)
        {
            oj.haptic = h;
        } else if (h) {
            SDL_HapticClose(h);
        }
    }

    g_open_joys.push_back(oj);
    rebuild_joy_map_locked();

    fprintf(stderr, "[input] joystick J%d \"%s\" axes=%d buttons=%d hats=%d rumble=%s%s\n", dev + 1, name ? name : "?",
            axes, buttons, hats, SDL_JoystickHasRumble(j) ? "yes" : (oj.haptic ? "haptic" : "no"), added ? " (new: PC-style defaults bound)" : "");
    fflush(stderr);
}

static void close_raw_joystick(SDL_JoystickID id) {

    std::lock_guard<std::mutex> lk(g_bindings_mtx);

    for (size_t i = 0; i < g_open_joys.size(); ++i) {
        if (g_open_joys[i].id != id) continue;
        if (g_open_joys[i].haptic) SDL_HapticClose(g_open_joys[i].haptic);

        SDL_JoystickClose(g_open_joys[i].joy);
        g_open_joys.erase(g_open_joys.begin() + (ptrdiff_t)i);
        rebuild_joy_map_locked();

        fprintf(stderr, "[input] joystick removed\n");
        fflush(stderr);

        return;
    }
}

static const char* gamepad_type_name(SDL_GameControllerType t) {

    switch (t)
    {
        case SDL_CONTROLLER_TYPE_XBOX360:                      return "Xbox 360/XInput";
        case SDL_CONTROLLER_TYPE_XBOXONE:                      return "Xbox One";
        case SDL_CONTROLLER_TYPE_PS3:                          return "PS3";
        case SDL_CONTROLLER_TYPE_PS4:                          return "PS4";
        case SDL_CONTROLLER_TYPE_PS5:                          return "PS5";
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:          return "Switch Pro";
        case SDL_CONTROLLER_TYPE_VIRTUAL:                      return "virtual";
        case SDL_CONTROLLER_TYPE_GOOGLE_STADIA:                return "Stadia";
        case SDL_CONTROLLER_TYPE_AMAZON_LUNA:                  return "Luna";
        case SDL_CONTROLLER_TYPE_NVIDIA_SHIELD:                return "Shield";
        default:                                               return "unknown";
    }
}

static void open_gamepad(int device_index)
{

    SDL_GameController* pad = SDL_GameControllerOpen(device_index);

    if (!pad)
    {
        fprintf(stderr, "[input] gamepad %d open failed: %s\n", device_index, SDL_GetError());
        fflush(stderr);

        return;
    }

    std::lock_guard<std::mutex> lk(g_bindings_mtx);

    for (SDL_GameController* p : g_pads) {
        if (p == pad) {
            SDL_GameControllerClose(pad);

            return;
        }
    }

    g_pads.push_back(pad);

    if (!controller) controller = pad;

    g_lightbar_resend.store(true);
    char guid[64] = {};
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(SDL_GameControllerGetJoystick(pad)), guid, sizeof(guid));
    const char* name = SDL_GameControllerName(pad);

    fprintf(stderr, "[input] gamepad \"%s\" type=%s vid=%04x pid=%04x guid=%s%s\n", name ? name : "?",
            gamepad_type_name(SDL_GameControllerGetType(pad)), SDL_GameControllerGetVendor(pad), SDL_GameControllerGetProduct(pad), guid,
            controller == pad ? " (active)" : "");
    fflush(stderr);
}

static void close_gamepad(SDL_JoystickID id) {

    std::lock_guard<std::mutex> lk(g_bindings_mtx);

    for (size_t i = 0; i < g_pads.size(); ++i)
    {
        if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pads[i])) != id) continue;
        if (controller == g_pads[i]) controller = nullptr;

        SDL_GameControllerClose(g_pads[i]);
        g_pads.erase(g_pads.begin() + (ptrdiff_t)i);

        if (!controller && !g_pads.empty()) controller = g_pads.front();

        fprintf(stderr, "[input] gamepad removed (%zu left)\n", g_pads.size());
        fflush(stderr);

        return;
    }
}

static bool gamepad_has_input(SDL_GameController* pad)
{

    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b)
        if (SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)b)) return true;

    for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; ++a)
        if (std::abs((int)SDL_GameControllerGetAxis(pad, (SDL_GameControllerAxis)a)) > 16000) return true;

    return false;
}

static void select_active_gamepad()
{

    if (g_pads.size() < 2 || (controller && gamepad_has_input(controller))) return;

    for (SDL_GameController* p : g_pads)
    {
        if (p == controller || !gamepad_has_input(p)) continue;

        std::lock_guard<std::mutex> lk(g_bindings_mtx);
        controller = p;
        const char* name = SDL_GameControllerName(p);

        fprintf(stderr, "[input] active gamepad -> \"%s\"\n", name ? name : "?");
        fflush(stderr);

        return;
    }
}

struct LightRgb { float r, g, b; };

static LightRgb light_rgb(uint32_t c)
{
    return { ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f };
}

static LightRgb light_mix(LightRgb a, LightRgb b, float t)
{
    return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
}

// DualShock 4 / DualSense lightbar: state colors crossfade; in a mission the color follows health, pulses when critical,
// flashes on hits, and strobes through the death spiral. Pads without an LED (XInput, Steam/DS4Windows virtual pads) are skipped.
static void lightbar_tick(const rs64::input::LightbarConfig& cfg, const char* st, float dt)
{

    static LightRgb s_cur{};
    static bool     s_have = false;
    static float    s_last_health = -1.0f, s_flash = 0.0f, s_phase = 0.0f;
    static uint32_t s_sent = 0xFFFFFFFFu;
    static auto     s_sent_t = std::chrono::steady_clock::now();

    if (!cfg.enabled) {
        s_sent = 0xFFFFFFFFu;

        return;
    }

    s_phase = std::fmod(s_phase + dt, 60.0f);
    constexpr float TWO_PI = 6.2831853f;
    const bool in_menu = st && std::strncmp(st, "menu", 4) == 0;
    const bool in_mission = st && std::strcmp(st, "mission") == 0;

    LightRgb target = light_rgb(in_menu ? cfg.menu : cfg.cinematic);
    float bright = 1.0f;
    bool snap = false;

    if (in_mission)
    {
        const rs64::rumble::CraftStatus cs = rs64::rumble::craft_status((const uint8_t*)g_recomp_rdram_for_wp_raw);
        target = light_rgb(cfg.mission);

        if (cs.valid && cs.dead)
        {
            target = light_rgb(cfg.dead);
        } else if (cs.valid && cs.spiral) {
            target = light_rgb(cfg.death);
            bright = 0.15f + 0.85f * (0.5f + 0.5f * std::cos(s_phase * TWO_PI * 5.0f));
            snap = true;
        } else if (cs.valid && cfg.health) {
            const float h = cs.health;
            target = h >= 0.5f ? light_mix(light_rgb(cfg.damaged), light_rgb(cfg.mission), (h - 0.5f) * 2.0f)
                               : light_mix(light_rgb(cfg.critical), light_rgb(cfg.damaged), h * 2.0f);

            if (h < 0.25f) bright = 0.45f + 0.55f * (0.5f + 0.5f * std::cos(s_phase * TWO_PI * 1.4f));
        }

        if (cs.valid && cfg.hit_flash && !cs.dead && s_last_health >= 0.0f && cs.health < s_last_health - 0.005f) s_flash = 1.0f;

        s_last_health = cs.valid ? cs.health : -1.0f;
    } else {
        s_last_health = -1.0f;
        s_flash = 0.0f;
    }

    const float a = (snap || !s_have) ? 1.0f : 1.0f - std::exp(-dt / 0.12f);
    s_cur = light_mix(s_cur, target, a);
    s_have = true;
    LightRgb out = { s_cur.r * bright, s_cur.g * bright, s_cur.b * bright };

    if (s_flash > 0.0f) out = light_mix(out, light_rgb(cfg.hit), s_flash);

    s_flash = std::max(0.0f, s_flash - dt / 0.18f);

    auto q = [](float v) { return (uint32_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
    const uint32_t packed = (q(out.r) << 16) | (q(out.g) << 8) | q(out.b);
    const auto now = std::chrono::steady_clock::now();
    const bool resend = g_lightbar_resend.exchange(false);

    if (!resend && (packed == s_sent || now - s_sent_t < std::chrono::milliseconds(33))) return;

    s_sent = packed;
    s_sent_t = now;
    std::lock_guard<std::mutex> lk(g_bindings_mtx);

    for (SDL_GameController* p : g_pads)
    {
        if (SDL_GameControllerHasLED(p)) SDL_GameControllerSetLED(p, (Uint8)(packed >> 16), (Uint8)(packed >> 8), (Uint8)packed);
    }
}

// Converts the game's Rumble Pak motor pulses (plus the damage/death-spiral layers) into device rumble at ~60 Hz, and drives the lightbar.
void rumble_thread_main() {

    bool prev_any = false;
    float haptic_last = 0.0f;
    auto haptic_t = std::chrono::steady_clock::now();
    auto tick_t = haptic_t;

    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        rs64::input::RumbleConfig cfg;
        rs64::input::LightbarConfig light;
        std::unique_lock<std::mutex> cfg_lk(g_bindings_mtx);
        cfg = g_bindings.rumble;
        light = g_bindings.lightbar;
        cfg_lk.unlock();
        const char* st = rs64_state_current_id();
        const auto tick_now = std::chrono::steady_clock::now();
        lightbar_tick(light, st, std::chrono::duration<float>(tick_now - tick_t).count());
        tick_t = tick_now;
        const bool in_mission = st && std::strcmp(st, "mission") == 0;
        const rs64::rumble::Output out = rs64::rumble::tick(cfg, (const uint8_t*)g_recomp_rdram_for_wp_raw, in_mission);
        const bool any = out.lo > 0.0f || out.hi > 0.0f;

        if (!any && !prev_any) continue;

        prev_any = any;
        const Uint16 lo = (Uint16)(out.lo * 65535.0f), hi = (Uint16)(out.hi * 65535.0f);
        const Uint32 ms = any ? 100 : 0;
        const float mag = std::max(out.lo, out.hi);
        const auto now = std::chrono::steady_clock::now();
        const bool haptic_due = !any || std::fabs(mag - haptic_last) > 0.05f || now - haptic_t > std::chrono::milliseconds(100);
        std::lock_guard<std::mutex> lk(g_bindings_mtx);

        if (controller) SDL_GameControllerRumble(controller, lo, hi, ms);

        for (const OpenJoy& oj : g_open_joys) {
            if (!g_bindings.joystick_enabled) break;

            if (!oj.haptic) {
                SDL_JoystickRumble(oj.joy, lo, hi, ms);
                continue;
            }

            if (!haptic_due) continue;

            if (any) SDL_HapticRumblePlay(oj.haptic, mag, 150);
            else SDL_HapticRumbleStop(oj.haptic);
        }

        if (haptic_due) {
            haptic_last = any ? mag : 0.0f;
            haptic_t = now;
        }
    }
}
bool                  g_fullscreen_applied = false;

extern "C" void rs64_set_fullscreen(int on)   { rs64::video::set_fullscreen(on != 0); }
extern "C" int  rs64_get_fullscreen(void)     { return rs64::video::fullscreen() ? 1 : 0; }
extern "C" void rs64_toggle_fullscreen(void)  { rs64::video::set_fullscreen(!rs64::video::fullscreen()); }

// Touch engine: finger/sensor events arrive on the SDL pump thread, polls on the game thread.
rs64::touch::Engine g_touch;
std::mutex g_touch_mtx;
static rs64::touch::MenuSnapshot g_touch_menu_at_down{};
static SDL_Sensor* g_accel_sensor = nullptr;
static SDL_Sensor* g_gyro_sensor = nullptr;

// Set by the TOUCH LAYOUT menu action on the game thread; the next touch poll opens the editor.
static std::atomic<bool> g_touch_edit_request{false};

extern "C" void rs64_touch_layout_request(void) { g_touch_edit_request.store(true); }

uint8_t* touch_rdram() {
    return (uint8_t*)g_recomp_rdram_for_wp_raw;
}

extern "C" int rs64_menu_page_shown(void);
extern "C" int rs64_menu_slider_channel(int slot);
extern "C" void rs64_menu_slider_click(int key, float f);
extern "C" int rs64_menu_slider_editing(void);
extern "C" void rs64_menu_hover_sound(int pause);

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

// A Sound Settings volume bar is open (opened by any input): the highlight must not move until it closes.
static bool sound_bar_editing()
{
    return rs64_menu_slider_editing() && touch_state_is("menu.sound_settings");
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
    uint32_t close_ms = 0;   // a touch off the bar just closed it: that tap only closes the game's slider edit
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

    if (!s.active || !r || !touch_state_is("menu.sound_settings") || !rs64::touch::sound_slider(r, s.entry, w, h, &bar))
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

// Menus where the cursor is freed from steering capture and clicks go through the touch menu layer.
bool mouse_menu_context(rs64::touch::Context ctx)
{

    using rs64::touch::Context;

    return ctx == Context::ListMenu || ctx == Context::Carousel || ctx == Context::PauseMenu || ctx == Context::Wheel;
}

// Free cursor in a menu: hover highlights, left click acts as a tap, right click is B. In a cutscene a left click is START (skip), captured or not.
static void menu_mouse_event(const SDL_Event& e)
{

    const uint32_t which = (e.type == SDL_MOUSEMOTION) ? e.motion.which : e.button.which;

    if (which == SDL_TOUCH_MOUSEID || g_show_controls.load() || rs64_rt64_inspector_open() || rs64::touch::shared_config().enabled)
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

        if (touch_slider_event(ev, kMouseFinger, fx, fy) || touch_pause_slider_event(ev, kMouseFinger, fx, fy))
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

        s_hovered = hovered;
    } else if (e.button.button == SDL_BUTTON_LEFT && e.type == SDL_MOUSEBUTTONDOWN) {
        rs64::touch::read_menu(r, &g_touch_menu_at_down, &boxes, w, h);
    } else if (e.button.button == SDL_BUTTON_LEFT && e.type == SDL_MOUSEBUTTONUP) {
        g_touch.tap(ctx, fx, fy);
    } else if (e.button.button == SDL_BUTTON_RIGHT && e.type == SDL_MOUSEBUTTONDOWN) {
        g_touch.inject(rs64::touch::B, 0.0f, 0.0f, 3);
    }
}

static void touch_event(const SDL_Event& e)
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

// Tilt steering: screen roll from gravity, low-passed so hand shake and linear acceleration don't jitter the turn.
static void accel_event(const SDL_Event& e)
{

    static float s_roll = 0.0f;
    const bool flipped = SDL_GetDisplayOrientation(0) == SDL_ORIENTATION_LANDSCAPE_FLIPPED;
    float roll = 0.0f;
    const bool valid = rs64::touch::tilt_angles(e.sensor.data[0], e.sensor.data[1], e.sensor.data[2], flipped, &roll);
    s_roll += (roll - s_roll) * 0.25f;
    std::lock_guard<std::mutex> lk(g_touch_mtx);
    g_touch.tilt(s_roll, valid);
}

// Gyro rates for the steering assist and mouse-like up/down. Device axes are portrait-natural: the screen normal is device z (+ = counter-clockwise, so negate
// for clockwise), and tipping the top edge away turns about screen-right (device -y, flipped on the other landscape side).
static void gyro_event(const SDL_Event& e)
{

    const bool flipped = SDL_GetDisplayOrientation(0) == SDL_ORIENTATION_LANDSCAPE_FLIPPED;
    const float s = flipped ? -1.0f : 1.0f;
    std::lock_guard<std::mutex> lk(g_touch_mtx);
    g_touch.gyro_rate(-e.sensor.data[2], s * e.sensor.data[1]);
}

static SDL_Sensor* open_sensor(SDL_SensorType type) {

    for (int i = 0; i < SDL_NumSensors(); ++i) {
        if (SDL_SensorGetDeviceType(i) == type)
        {
            return SDL_SensorOpen(i);
        }
    }

    return nullptr;
}

// Opens the accelerometer (tilt) and gyroscope (rate assist) while the GYRO STEERING toggle is on, closes them when off.
static void update_gyro_sensor() {

    const bool want = rs64::touch::gyro_enabled();

    if (want && !g_accel_sensor) {
        static bool s_warned = false;
        g_accel_sensor = open_sensor(SDL_SENSOR_ACCEL);
        g_gyro_sensor = open_sensor(SDL_SENSOR_GYRO);

        if ((!g_accel_sensor || !g_gyro_sensor) && !s_warned)
        {
            s_warned = true;

            fprintf(stderr, "[touch] accelerometer=%s gyroscope=%s\n", g_accel_sensor ? "yes" : "no", g_gyro_sensor ? "yes" : "no");
        }
    } else if (!want && g_accel_sensor) {
        SDL_SensorClose(g_accel_sensor);
        g_accel_sensor = nullptr;

        if (g_gyro_sensor) {
            SDL_SensorClose(g_gyro_sensor);
            g_gyro_sensor = nullptr;
        }
    }

    std::lock_guard<std::mutex> lk(g_touch_mtx);
    rs64::touch::Config tc = g_touch.config();
    tc.gyro = want && g_accel_sensor;
    g_touch.set_config(tc);

    if (!g_gyro_sensor) {
        g_touch.gyro_rate(0.0f, 0.0f);
    }
}

static void apply_fullscreen_if_requested() {

#ifndef __ANDROID__

    const bool want = rs64::video::fullscreen();

    if (want != g_fullscreen_applied && g_sdl_window)
    {
        g_fullscreen_applied = want;
        SDL_SetWindowFullscreen(g_sdl_window, want ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    }

#endif
}

// While the lobby address is typed on a phone, slide the picture up just enough that the highlighted entry clears the on-screen keyboard.
static void update_keyboard_shift() {

#ifdef __ANDROID__

    float shift = 0.0f;
    uint8_t* r = touch_rdram();

    if (r && rs64::host::flag("text_entry")) {
        const float ime = rs64::android::ime_fraction();
        float w, h;
        window_size(&w, &h);
        rs64::touch::MenuSnapshot snap{};
        std::vector<rs64::touch::Box> boxes;

        if (ime > 0.0f && rs64::touch::read_menu(r, &snap, &boxes, w, h)) {
            for (const rs64::touch::Box& b : boxes) {
                if (b.entry == snap.current) {
                    shift = std::max(0.0f, b.y1 - (1.0f - ime - 0.03f));
                }
            }
        }
    }

    RT64::presentShiftY.store(shift);

#endif
}

void poll_input()
{

    apply_fullscreen_if_requested();
    update_keyboard_shift();
    static bool s_scanned = false;

    if (!s_scanned) {
        s_scanned = true;
        const int n = SDL_NumJoysticks();

        fprintf(stderr, "[input] %d device(s) at first poll\n", n);
        fflush(stderr);

        for (int i = 0; i < n; ++i) {
            if (SDL_IsGameController(i)) open_gamepad(i);
            else open_raw_joystick(i);
        }
    }

    // Any physical input hides the touch overlay until the next touch.
    auto note_physical_input = [] {

        std::lock_guard<std::mutex> lk(g_touch_mtx);
        g_touch.physical_input();
    };

    static bool s_rates_known = false;

    if (!s_rates_known && g_sdl_window) {
        publish_refresh_rates();
        s_rates_known = true;
    }

    SDL_Event e;

    while (SDL_PollEvent(&e)) {
        switch (e.type)
        {
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED) publish_refresh_rates();
            break;
        case SDL_FINGERDOWN:
        case SDL_FINGERMOTION:
        case SDL_FINGERUP:
            touch_event(e);
            break;
        case SDL_MOUSEMOTION:
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            menu_mouse_event(e);
            break;
        case SDL_SENSORUPDATE:
            if (g_gyro_sensor && e.sensor.which == SDL_SensorGetInstanceID(g_gyro_sensor))
            {
                gyro_event(e);
            } else if (g_accel_sensor && e.sensor.which == SDL_SensorGetInstanceID(g_accel_sensor))
            {
                accel_event(e);
            }
            break;

        // Text entry (the lobby address): typed text and Backspace/Enter go to a key handler that takes them, not the game.
        case SDL_TEXTINPUT:
            rs64::host::run_key_handlers(e.text.text, 0);
            break;
        case SDL_KEYDOWN: {
            const SDL_Keycode sym = e.key.keysym.sym;

            if ((sym == SDLK_BACKSPACE || sym == SDLK_RETURN || sym == SDLK_KP_ENTER) && rs64::host::run_key_handlers(nullptr, sym == SDLK_BACKSPACE ? '\b' : '\r'))
            {
                break;
            }

            // Android's system back gesture/button arrives as AC_BACK and means B.
            if (e.key.repeat == 0 && e.key.keysym.scancode == SDL_SCANCODE_AC_BACK) {
                std::lock_guard<std::mutex> lk(g_touch_mtx);
                g_touch_slider.active = false;
                g_touch.back_pressed();
                break;
            }

            note_physical_input();

            // F6 toggles the controls/rebind window. Mouse-steering capture is automatic (focus-driven, in update_gfx).
            if (e.key.repeat == 0)
            {
                if (e.key.keysym.scancode == SDL_SCANCODE_RETURN && (e.key.keysym.mod & KMOD_ALT))
                {
                    rs64_toggle_fullscreen();
                } else if (e.key.keysym.scancode == SDL_SCANCODE_F6) {
                    g_show_controls.store(!g_show_controls.load());
                } else if (e.key.keysym.scancode == SDL_SCANCODE_F5) {
                    // Toggle Factor 5's built-in frame-profiler/debug-text HUD.
                    // Gate byte at virtual 0x80038CE0 (retail leaves it 0); the
                    // emitter early-returns when zero. ^3 = N64 byte order.
                    if (uint8_t* rd = (uint8_t*)g_recomp_rdram_for_wp_raw) {
                        uint8_t& gate = rd[0x38CE0 ^ 3];
                        gate = gate ? 0 : 1;

                        fprintf(stderr, "[F5] profiler HUD gate 0x80038CE0 -> %u\n", gate);
                        fflush(stderr);
                    }
                } else if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                    // Esc closes the controls window; otherwise it falls through to
                    // the game (bound to Start/pause).
                    if (g_show_controls.load()) g_show_controls.store(false);
                }
            }

            if (sym == SDLK_F12) {
                fprintf(stderr, "[F12] manual minidump requested\n");
                fflush(stderr);
                rs64::crash::write_minidump_safe(nullptr);
            }

            // Diagnostic: F1-F4 reaching PollEvent means SDL delivers them; RT64's filter (which runs first) may have consumed them already.
            if (sym == SDLK_F1 || sym == SDLK_F2 || sym == SDLK_F3 || sym == SDLK_F4)
            {
                fprintf(stderr, "[input] SDL_KEYDOWN sym=%d (F%d) reached PollEvent\n",
                        (int)sym, (int)(sym - SDLK_F1 + 1));
                fflush(stderr);
            }
            break;
        }
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_JOYBUTTONDOWN:
            note_physical_input();
            break;
        case SDL_CONTROLLERAXISMOTION:
            if (std::abs(e.caxis.value) > 16000) {
                note_physical_input();
            }
            break;
        case SDL_QUIT:
            fprintf(stderr, "[main] SDL_QUIT received, exiting\n");
            fflush(stderr);
            rs64::host::run_quit_handlers();

            // _Exit, not exit: the recomp game threads and RT64 are still live, so
            // running the C++/atexit static-destructor table here throws (-> terminate,
            // the Release crash-on-close) or deadlocks (the hang). Terminate now; the OS
            // reclaims SDL/GPU/process resources. Matches the crash handlers' _Exit.
            _Exit(EXIT_SUCCESS);
        case SDL_CONTROLLERDEVICEADDED:
            open_gamepad(e.cdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            close_gamepad(e.cdevice.which);
            break;
        case SDL_JOYDEVICEADDED:
            open_raw_joystick(e.jdevice.which);
            break;
        case SDL_JOYDEVICEREMOVED:
            close_raw_joystick(e.jdevice.which);
            break;
        default:
            break;
        }
    }

    select_active_gamepad();
    update_gyro_sensor();

    // Drain SDL's relative-motion accumulator once per poll. Read from the
    // internal state (updated before RT64's event filter), so capture is robust
    // in dev mode. Only feed the resolver while capture is engaged.
    int rdx = 0, rdy = 0;
    uint32_t rbtn = SDL_GetRelativeMouseState(&rdx, &rdy);

    if (g_mouse_capture.load()) {
        g_mouse_ax += (float)rdx;
        g_mouse_ay += (float)rdy;
    } else {
        g_mouse_ax = g_mouse_ay = 0.0f;
    }

    g_mouse_btn = rs64::touch::shared_config().enabled ? 0 : rbtn;

    // ROGUESQ_PROFILER_DUMP=1: sample the 7 F5-HUD timing slots (microseconds)
    // so each bar can be labelled. Addresses are the absolute lw sources in
    // drawFrameProfilerBars; words are stored host-order in the raw RDRAM buf.
    static int s_prof_dump = -1;

    if (s_prof_dump < 0) s_prof_dump = env_on("ROGUESQ_PROFILER_DUMP");

    if (s_prof_dump) {
        if (const uint8_t* rd = (const uint8_t*)g_recomp_rdram_for_wp_raw)
        {
            auto W = [rd](uint32_t va) { return *reinterpret_cast<const uint32_t*>(rd + (va & 0x7FFFFF)); };
            static int n = 0;
            ++n;

            if (n % 15 == 0)
            {
                fprintf(stderr, "[prof] yel=%u blu=%u red=%u mag=%u wht=%u grn=%u cyn=%u\n",
                        W(0x8011A914), W(0x80128EE0), W(0x80128EDC),
                        W(0x8011A920), W(0x8011DC54), W(0x8011DC4C), W(0x8011A918));
                fflush(stderr);
            }
        }
    }
}

// BOOT_TARGET: while the FrontEnd attract-title is up, the present hook sets this
// so we inject the native START title-skip. Pulsed (~120ms on / off) because the
// game's skip reads NEW button presses -- a held START would register only once.
static inline uint16_t boot_start_pulse() {

    if (!g_boot_pulse_start) return 0;

    return ((SDL_GetTicks() % 240u) < 120u) ? N64_START_BUTTON : 0;
}

static rs64::touch::Pad poll_touch()
{

    float w, h;
    window_size(&w, &h);
    const rs64::touch::Context ctx = touch_context();
    float px0 = 0.0f, px1 = 1.0f;
    rs64::touch::picture_extent(touch_rdram(), g_active_overlay, w, h, &px0, &px1);
    std::lock_guard<std::mutex> lk(g_touch_mtx);
    g_touch.set_screen(w, h);
    g_touch.set_picture(px0, px1);

    if (g_touch_edit_request.exchange(false) && g_touch.config().enabled) {
        g_touch_slider.active = false;
        g_touch.begin_edit();
    }

    if (g_touch.take_layout_saved()) {
        rs64::touch::Config& shared = rs64::touch::shared_config();
        shared.layout = g_touch.config().layout;
        shared.stick_split = g_touch.config().stick_split;
        rs64::touch::save_config(shared, rs64::touch::config_path());
    }

    return g_touch.poll(ctx, SDL_GetTicks());
}

static bool get_n64_input_live(int controller_num, uint16_t* buttons, float* x, float* y)
{

    if (controller_num != 0) {
        *buttons = 0;
        *x = 0.0f;
        *y = 0.0f;

        return false;
    }

    // While the controls window is open, suppress gameplay input so rebinding
    // or navigating the UI doesn't also drive the ship. Still report connected.
    if (g_show_controls.load()) {
        *buttons = 0;
        *x = 0.0f;
        *y = 0.0f;

        return true;
    }

    // Headless automation overrides resolved input and must run BEFORE resolve(): keyboard is
    // enabled by default, so resolve() reports "active" even with no key pressed, which would
    // otherwise skip the fake-controller branch and swallow injected input.
    // The BOOT_TARGET menu driver also drives with a real controller connected (it only reports input while its sequence runs).
    uint16_t nav_buttons = 0;
    float nav_x = 0.f, nav_y = 0.f;

    if (rs64_nav_consume(&nav_buttons, &nav_x, &nav_y)) {
        *buttons = nav_buttons;
        *x = nav_x;
        *y = nav_y;

        return true;
    }

    if (fake_controller_enabled()) {
        uint16_t sb = 0;
        float sx = 0.f, sy = 0.f;

        if (scripted_input(&sb, &sx, &sy)) {
            *buttons = sb;
            *x = sx;
            *y = sy;

            return true;
        }
    }

    // Resolve keyboard + gamepad + mouse through the bindings profile.
    rs64::input::RawState st;
    st.keys = SDL_GetKeyboardState(&st.keys_len);
    st.pad = controller;
    st.mouse_active = g_mouse_capture.load();

    // Fire buttons only while steering; middle (Start) works in menus too.
    st.mouse_buttons = st.mouse_active ? g_mouse_btn : (g_mouse_btn & SDL_BUTTON_MMASK);
    float raw_dx = st.mouse_active ? g_mouse_ax : 0.0f;
    float raw_dy = st.mouse_active ? g_mouse_ay : 0.0f;
    g_mouse_ax = g_mouse_ay = 0.0f;  // consume this frame's accumulated motion

    uint16_t btn = 0;
    bool active;
    std::unique_lock<std::mutex> bind_lk(g_bindings_mtx);

    // Frame-rate-aware exponential low-pass on the mouse velocity so fast
    // flicks ramp in and stop-motion eases back to center, instead of the raw
    // per-frame delta snapping the stick to full/zero each frame.
    static uint32_t s_last_ms = 0;
    static float s_sm_dx = 0.0f, s_sm_dy = 0.0f;
    uint32_t smooth_now = SDL_GetTicks();
    float smooth_dt = s_last_ms ? (float)(smooth_now - s_last_ms) : 16.0f;
    s_last_ms = smooth_now;
    float tau = g_bindings.mouse_smoothing * 100.0f;  // smoothing amount -> ms time constant
    float alpha = tau > 0.0f ? smooth_dt / (smooth_dt + tau) : 1.0f;
    s_sm_dx += (raw_dx - s_sm_dx) * alpha;
    s_sm_dy += (raw_dy - s_sm_dy) * alpha;
    st.mouse_dx = s_sm_dx;
    st.mouse_dy = s_sm_dy;
    st.joys = g_joy_by_dev.data();
    st.joys_len = (int)g_joy_by_dev.size();
    active = rs64::input::resolve(g_bindings, st, &btn, x, y);
    g_throttle.store(rs64::input::throttle_position(g_bindings, st));
    bind_lk.unlock();

    if (!active)
    {
        // Touch counts as a connected controller (no "NO CONTROLLER" gate on a phone).
        if (g_touch.config().enabled) {
            rs64::touch::Pad tp = poll_touch();
            *buttons = tp.buttons | boot_start_pulse();
            *x = tp.x;
            *y = tp.y;

            return true;
        }

        // No keyboard and no gamepad: keep the fake-controller path so headless
        // runs still clear the "NO CONTROLLER" gate.
        if (fake_controller_enabled())
        {
            // nav/scripted injection already handled above (before resolve).
            static int s_auto = -1;

            if (s_auto < 0) s_auto = env_int("ROGUESQ_AUTO_START", 0);

            uint16_t fb = 0;

            if (s_auto > 0 && (SDL_GetTicks() % (uint32_t)s_auto) < 120u) fb |= N64_START_BUTTON;

            fb |= boot_start_pulse();
            *buttons = fb;
            *x = 0.0f;
            *y = 0.0f;

            return true;
        }

        *buttons = 0;
        *x = 0.0f;
        *y = 0.0f;

        return false;
    }

    static int s_auto = -1;

    if (s_auto < 0) s_auto = env_int("ROGUESQ_AUTO_START", 0);
    if (s_auto > 0 && (SDL_GetTicks() % (uint32_t)s_auto) < 120u) btn |= N64_START_BUTTON;

    btn |= boot_start_pulse();
    rs64::touch::merge(poll_touch(), &btn, x, y);
    *buttons = btn;

    return true;
}

// ROGUESQ_INPUT_RECORD=<file> records controller 0 from the first input poll after launch (menus included); ROGUESQ_INPUT_REPLAY=<file> plays it back from the same point.
// Lines after the header: "<ms> <vi> <poll> <buttons hex> <x> <y>". v4 (written now) has one line per poll; replay serves the recorded sample nearest the current wall-clock ms (within half a poll).
// v3 (older) was written on change and replays by wall-clock ms with the stick blended between close samples.
struct InputRec { uint32_t ms, vi, poll; uint16_t buttons; float x, y; };

bool get_n64_input(int controller_num, uint16_t* buttons, float* x, float* y)
{

    static const char* s_rec_path = recomp::dbg::env_str("ROGUESQ_INPUT_RECORD");
    static const char* s_play_path = recomp::dbg::env_str("ROGUESQ_INPUT_REPLAY");
    static bool s_anchored = false;
    static uint32_t s_poll = 0;
    static unsigned s_vi0 = 0;
    static std::chrono::steady_clock::time_point s_t0;

    if ((controller_num == 0) && (s_rec_path || s_play_path)) {
        if (!s_anchored)
        {
            s_anchored = true;
            s_poll = 0;
            s_vi0 = g_vi_tick;
            s_t0 = std::chrono::steady_clock::now();

            fprintf(stderr, "[input-rec] first poll: %s\n", s_play_path ? "replaying" : "recording");
        }
    }

    const uint32_t vi = s_anchored ? (uint32_t)(g_vi_tick - s_vi0) : 0u;
    const uint32_t ms = s_anchored ? (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s_t0).count() : 0u;

    if ((controller_num == 0) && s_play_path && s_anchored)
    {
        static std::vector<InputRec> s_recs;
        static size_t s_idx = 0;
        static bool s_loaded = false;
        static int s_version = 0;

        if (!s_loaded)
        {
            s_loaded = true;

            if (FILE* f = fopen(s_play_path, "r"))
            {
                char header[32] = {};

                if ((fscanf(f, "%31s v%d", header, &s_version) == 2) && (s_version >= 3))
                {
                    InputRec r{};
                    unsigned b = 0;

                    while (fscanf(f, "%u %u %u %x %f %f", &r.ms, &r.vi, &r.poll, &b, &r.x, &r.y) == 6)
                    {
                        r.buttons = (uint16_t)b;
                        s_recs.push_back(r);
                    }
                }

                fclose(f);
            }

            fprintf(stderr, "[input-rec] loaded %zu entries (v%d) from %s\n", s_recs.size(), s_version, s_play_path);
        }

        if ((s_version >= 4) && !s_recs.empty())
        {
            // Every poll was recorded with its time, so serve the recorded sample nearest to now: at most half a poll off, and no lag builds up when the game polls at a slightly different rate.
            auto it = std::lower_bound(s_recs.begin() + (ptrdiff_t)s_idx, s_recs.end(), ms, [](const InputRec& r, uint32_t t) { return r.ms < t; });
            size_t j = (it == s_recs.end()) ? (s_recs.size() - 1) : (size_t)(it - s_recs.begin());

            if ((j > 0) && ((it == s_recs.end()) || ((s_recs[j].ms - ms) > (ms - s_recs[j - 1].ms))))
            {
                j--;
            }

            s_idx = std::max(s_idx, j);
        }
        else {
            while ((s_idx + 1 < s_recs.size()) && (s_recs[s_idx + 1].ms <= ms)) {
                s_idx++;
            }
        }

        // Close the game once the recording has played out (the last entry is written when the recorded session ended).
        static bool s_quit = false;

        if (!s_quit && !s_recs.empty() && (ms > s_recs.back().ms + 500u))
        {
            s_quit = true;

            fprintf(stderr, "[input-rec] replay finished at ms=%u, closing\n", ms);
            rs64_menu_request_quit();
        }

        const bool have = !s_recs.empty() && ((s_version >= 4) || (s_recs[s_idx].ms <= ms));
        *buttons = have ? s_recs[s_idx].buttons : 0;
        *x = have ? s_recs[s_idx].x : 0.0f;
        *y = have ? s_recs[s_idx].y : 0.0f;

        // v3 only: blend the stick between samples of continuous motion (mouse steering) so a poll that lands a few ms off still reads nearly the recorded value; held values and sudden changes still step.
        if (have && (s_version < 4) && (s_idx + 1 < s_recs.size())) {
            const InputRec& a = s_recs[s_idx];
            const InputRec& b = s_recs[s_idx + 1];

            if ((b.ms > a.ms) && (b.ms - a.ms <= 50u)) {
                const float t = (float)(ms - a.ms) / (float)(b.ms - a.ms);
                *x = a.x + (b.x - a.x) * t;
                *y = a.y + (b.y - a.y) * t;
            }
        }

        // Drift check: our VI/poll counts at this time vs the recorded ones.
        static uint32_t s_last_log = 0;

        if (have && (ms >= s_last_log + 10000))
        {
            s_last_log = ms;

            fprintf(stderr, "[input-rec] ms=%u vi=%u recorded_vi=%u poll=%u recorded_poll=%u\n", ms, vi, s_recs[s_idx].vi, s_poll, s_recs[s_idx].poll);
        }

        s_poll++;

        return true;
    }

    const bool ok = get_n64_input_live(controller_num, buttons, x, y);

    if (controller_num == 0) {
        rs64::host::run_input_filters(0, buttons, x, y);
    }

    if ((controller_num == 0) && s_rec_path && s_anchored)
    {
        static FILE* s_file = [](const char* p) { FILE* f = fopen(p, "w"); if (f) fprintf(f, "rs64-input v4\n"); return f; }(s_rec_path);

        if (s_file)
        {
            fprintf(s_file, "%u %u %u %04X %.9g %.9g\n", ms, vi, s_poll, *buttons, *x, *y);
            fflush(s_file);
        }

        s_poll++;
    }

    return ok;
}

void set_rumble(int controller_num, bool on) {
    rs64::rumble::motor(controller_num, on);
}

ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num)
{

    std::unique_lock<std::mutex> lk(g_bindings_mtx);
    const bool any_joy = g_bindings.joystick_enabled && !g_open_joys.empty();
    lk.unlock();

    if (controller_num == 0 && (controller || any_joy || g_bindings.keyboard_enabled || fake_controller_enabled()))
    {
        const auto pak = g_report_rumble_pak.load() ? ultramodern::input::Pak::RumblePak : ultramodern::input::Pak::None;

        return { ultramodern::input::Device::Controller, pak };
    }

    return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
}

// Controls / rebind window. Runs on the RT64 graphics thread via the ImGui hook
// (SetRenderHookImgui), between NewFrame and Render. All g_bindings mutation is
// under g_bindings_mtx since get_n64_input reads it on the game thread. Only
// active in developer mode (the inspector frame); F6 toggles visibility.
// Index into g_bindings.joy_devices for an open joystick, or -1.
int joy_dev_locked(const OpenJoy& oj) {

    for (size_t d = 0; d < g_bindings.joy_devices.size(); ++d)
    {
        if (g_bindings.joy_devices[d].guid == oj.guid && g_bindings.joy_devices[d].ordinal == oj.ordinal) return (int)d;
    }

    return -1;
}

// After a profile reset, re-register connected joysticks so they get the PC-style defaults again.
void rebind_open_joysticks_locked() {

    for (const OpenJoy& oj : g_open_joys)
    {
        bool added = false;
        const char* name = SDL_JoystickName(oj.joy);
        const int dev = rs64::input::find_or_add_joy_device(g_bindings, oj.guid, oj.ordinal, name ? name : "", &added);

        if (added && dev >= 0)
        {
            rs64::input::add_joystick_defaults(g_bindings, dev, SDL_JoystickNumAxes(oj.joy), SDL_JoystickNumButtons(oj.joy), SDL_JoystickNumHats(oj.joy));
        }
    }

    rebuild_joy_map_locked();
}
}
