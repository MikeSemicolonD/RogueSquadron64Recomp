#include "input/input_host.h"
#include "lockstep/lockstep_core.h"
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

// The class of the last physical input; a touch-only player counts as a gamepad.
static std::atomic<rs64::input::Device> s_last_device{rs64::input::Device::Keyboard};

rs64::input::Device last_device() {
    return s_last_device.load();
}

// Esc pressed in a pause submenu went to the game as B (pause_escape); the held key drives no binding until released.
static std::atomic<bool> s_esc_dropped{false};

// Every open gamepad; `controller` is whichever one last produced input (Steam Input / DS4Windows can expose an idle virtual pad alongside the real one).
std::vector<SDL_GameController*> g_pads;

std::atomic<bool> g_lightbar_resend{true};

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

#ifdef RS64_MCP
// The MCP press tool's held input (src/lockstep/lockstep.cpp); false when none is active.
extern "C" bool rs64_mcp_consume_input(uint16_t* buttons, float* x, float* y);
#endif

// A controller input name (ROGUESQ_INPUT_SEQ and the MCP press tool): ORs its button into mask or sets the stick; false when unknown.
extern "C" bool rs64_input_name(const char* name, uint16_t* mask, float* sx, float* sy)
{

    static const std::pair<const char*, uint16_t> kButtons[] = {
        { "start", N64_START_BUTTON }, { "a", N64_A_BUTTON }, { "b", N64_B_BUTTON }, { "z", N64_Z_TRIG }, { "l", N64_L_TRIG }, { "r", N64_R_TRIG },
        { "dup", N64_U_JPAD }, { "ddown", N64_D_JPAD }, { "dleft", N64_L_JPAD }, { "dright", N64_R_JPAD },
        { "cup", N64_U_CBUTTONS }, { "cdown", N64_D_CBUTTONS }, { "cleft", N64_L_CBUTTONS }, { "cright", N64_R_CBUTTONS },
    };
    static const std::tuple<const char*, float, float> kStick[] = { { "up", 0.f, 1.f }, { "down", 0.f, -1.f }, { "left", -1.f, 0.f }, { "right", 1.f, 0.f } };

    for (const auto& b : kButtons) {
        if (std::strcmp(name, b.first) == 0) {
            *mask |= b.second;

            return true;
        }
    }

    for (const auto& st : kStick) {
        if (std::strcmp(name, std::get<0>(st)) == 0) {
            *sx = std::get<1>(st);
            *sy = std::get<2>(st);

            return true;
        }
    }

    return false;
}

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

                rs64_input_name(name.c_str(), &s.mask, &s.sx, &s.sy);

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
std::atomic<bool>     g_show_fps{false};
std::atomic<bool>     g_mouse_capture{false};
static float                 g_mouse_ax = 0.0f;   // accumulated relative motion this frame
static float                 g_mouse_ay = 0.0f;
static uint32_t              g_mouse_btn = 0;

// AT-ST head aim from the mouse, as a held stick position (-1..1); about 200 px across the full range at the default sensitivity.
constexpr float kAtStHeadPerPixel = 0.1f;
static float g_atst_head_x = 0.0f;
static float g_atst_head_y = 0.0f;
static bool g_atst_head_on = false;

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

// ROGUESQ_INPUT_CLAIM=<slot>,<file> (split-screen test pair): no input until a device is pressed, then only that device, and its id goes to <file>.<slot>.
// Slot 2 claims nothing until <file>.1 exists and never claims that device. Pads are read in the background, so only the keyboard player needs focus.
struct InputClaim { bool on = false; int slot = 0; std::string base, owned, other; };

static InputClaim& input_claim()
{

    static InputClaim s = [] {

        InputClaim c;
        const char* v = recomp::dbg::env_str("ROGUESQ_INPUT_CLAIM");
        const char* comma = v ? strchr(v, ',') : nullptr;

        if (!comma) return c;

        c.on = true;
        c.slot = atoi(v);
        c.base = comma + 1;
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

        return c;
    }();

    return s;
}

static constexpr const char* kClaimKeyboard = "keyboard";

static std::string joy_claim_id(SDL_Joystick* j)
{

    const char* path = SDL_JoystickPath(j);
    char guid[64] = {};

    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(j), guid, sizeof(guid));

    return path && *path ? path : guid;
}

// Buttons and hats only: a HOTAS throttle rests off-centre and would claim on its own.
static bool joystick_has_press(SDL_Joystick* j)
{

    for (int b = 0; b < SDL_JoystickNumButtons(j); ++b)
        if (SDL_JoystickGetButton(j, b)) return true;

    for (int h = 0; h < SDL_JoystickNumHats(j); ++h)
        if (SDL_JoystickGetHat(j, h) != SDL_HAT_CENTERED) return true;

    return false;
}

static std::string pressed_claim_id(const rs64::input::RawState& st, const std::string& skip)
{

    // A key claims only on its down edge, so one already reading as held never does.
    static uint8_t s_prev[SDL_NUM_SCANCODES];
    static bool s_seeded = false;
    const int nk = st.keys ? std::min(st.keys_len, (int)SDL_NUM_SCANCODES) : 0;
    int pressed = -1;

    for (int k = 0; s_seeded && k < nk; ++k)
        if (st.keys[k] && !s_prev[k] && pressed < 0) pressed = k;

    if (nk > 0) std::memcpy(s_prev, st.keys, nk);

    s_seeded = true;

    if (pressed >= 0 && skip != kClaimKeyboard)
    {
        fprintf(stderr, "[input] claim key %s\n", SDL_GetScancodeName((SDL_Scancode)pressed));

        return kClaimKeyboard;
    }

    // Devices claim on a press edge too; a device's first poll only seeds its state, so one already held never claims.
    static std::map<std::string, bool> s_held;
    std::string found;

    const auto edge = [&](const std::string& id, bool now) {

        const auto it = s_held.find(id);
        const bool was = it == s_held.end() ? now : it->second;

        s_held[id] = now;

        if (now && !was && id != skip && found.empty()) found = id;
    };

    for (SDL_GameController* p : g_pads)
        edge(joy_claim_id(SDL_GameControllerGetJoystick(p)), gamepad_has_input(p));

    for (const OpenJoy& oj : g_open_joys)
        edge(joy_claim_id(oj.joy), joystick_has_press(oj.joy));

    return found;
}

// Leaves only the claimed device in st, claiming the first one pressed. Called with g_bindings_mtx held.
static void apply_input_claim(rs64::input::RawState& st)
{

    InputClaim& c = input_claim();

    if (c.owned.empty() && !(c.slot == 2 && c.other.empty()))
    {
        c.owned = pressed_claim_id(st, c.other);

        if (!c.owned.empty())
        {
            const std::string file = c.base + "." + std::to_string(c.slot);

            if (FILE* f = fopen(file.c_str(), "w"))
            {
                fputs(c.owned.c_str(), f);
                fclose(f);
            }

            fprintf(stderr, "[input] player %d claimed %s\n", c.slot, c.owned.c_str());
            fflush(stderr);
        }
    } else if (c.owned.empty()) {
        const std::string file = c.base + ".1";

        if (FILE* f = fopen(file.c_str(), "r"))
        {
            char buf[512] = {};

            if (fgets(buf, sizeof(buf), f)) c.other = buf;

            fclose(f);
        }
    }

    static const uint8_t s_none[SDL_NUM_SCANCODES] = {};
    static std::vector<SDL_Joystick*> s_joys;

    if (c.owned != kClaimKeyboard)
    {
        st.keys = s_none;
        st.keys_len = SDL_NUM_SCANCODES;
        st.mouse_active = false;
        st.mouse_buttons = 0;
        st.mouse_dx = st.mouse_dy = 0.0f;
    }

    controller = nullptr;

    for (SDL_GameController* p : g_pads)
        if (joy_claim_id(SDL_GameControllerGetJoystick(p)) == c.owned) controller = p;

    st.pad = controller;
    s_joys.assign(st.joys, st.joys + st.joys_len);

    for (SDL_Joystick*& j : s_joys)
        if (j && joy_claim_id(j) != c.owned) j = nullptr;

    st.joys = s_joys.data();
}

static void select_active_gamepad()
{

    if (input_claim().on || g_pads.size() < 2 || (controller && gamepad_has_input(controller))) return;

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

bool                  g_fullscreen_applied = false;

extern "C" void rs64_set_fullscreen(int on)   { rs64::video::set_fullscreen(on != 0); }
extern "C" int  rs64_get_fullscreen(void)     { return rs64::video::fullscreen() ? 1 : 0; }
extern "C" void rs64_toggle_fullscreen(void)  { rs64::video::set_fullscreen(!rs64::video::fullscreen()); }

static SDL_Sensor* g_accel_sensor = nullptr;
static SDL_Sensor* g_gyro_sensor = nullptr;

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
            if (e.type == SDL_FINGERDOWN) s_last_device.store(rs64::input::Device::Gamepad);

            touch_event(e);
            break;
        case SDL_MOUSEMOTION:
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            if (e.type == SDL_MOUSEBUTTONDOWN) s_last_device.store(rs64::input::Device::Keyboard);

            menu_mouse_event(e);
            break;
        case SDL_MOUSEWHEEL:
            rs64_controls_wheel(e.wheel.y);
            menu_wheel_x(e.wheel.x);
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

        // Text entry (the lobby address, the ENTER NAME / PASSCODES letter wheels): typed text and Backspace/Enter go to a handler that takes them, not the game.
        case SDL_TEXTINPUT:
            if (!rs64::host::run_key_handlers(e.text.text, 0)) rs64_menu_type(e.text.text, 0);
            break;
        case SDL_KEYDOWN: {
            const SDL_Keycode sym = e.key.keysym.sym;

            if (sym == SDLK_BACKSPACE || sym == SDLK_RETURN || sym == SDLK_KP_ENTER)
            {
                const int key = sym == SDLK_BACKSPACE ? '\b' : '\r';

                if (rs64::host::run_key_handlers(nullptr, key) || rs64_menu_type(nullptr, key)) break;
            }

            // Android's system back gesture/button arrives as AC_BACK and means B.
            if (e.key.repeat == 0 && e.key.keysym.scancode == SDL_SCANCODE_AC_BACK) {
                std::lock_guard<std::mutex> lk(g_touch_mtx);
                touch_slider_cancel();
                g_touch.back_pressed();
                break;
            }

            note_physical_input();
            s_last_device.store(rs64::input::Device::Keyboard);

            // F6 toggles the controls/rebind window. Mouse-steering capture is automatic (focus-driven, in update_gfx).
            if (e.key.repeat == 0)
            {
                switch (e.key.keysym.scancode) {
                    case SDL_SCANCODE_RETURN:
                        if (e.key.keysym.mod & KMOD_ALT) rs64_toggle_fullscreen();

                        break;
                    case SDL_SCANCODE_F6:
                        g_show_controls.store(!g_show_controls.load());
                        break;
                    case SDL_SCANCODE_F7:
                        g_show_fps.store(!g_show_fps.load());
                        break;
                    case SDL_SCANCODE_F5: {
                        // Factor 5's frame-profiler/debug-text HUD: gate byte 0x80038CE0 (retail leaves it 0). ^3 = N64 byte order.
                        if (uint8_t* rd = (uint8_t*)g_recomp_rdram_for_wp_raw) {
                            uint8_t& gate = rd[0x38CE0 ^ 3];
                            gate = gate ? 0 : 1;

                            fprintf(stderr, "[F5] profiler HUD gate 0x80038CE0 -> %u\n", gate);
                            fflush(stderr);
                        }
                        break;
                    }
                    case SDL_SCANCODE_F9: {
                        const rs64lights::DebugView& view = rs64lights::stepDebugView((e.key.keysym.mod & KMOD_SHIFT) ? -1 : 1);

                        rs64::app::notify("rtdebug", std::string("RT debug view: ") + view.name + "  (F9 next, Shift+F9 previous)", 3.0f);
                        break;
                    }
                    case SDL_SCANCODE_ESCAPE:
                        // Esc closes the controls window; otherwise it falls through to the game (bound to Start/pause).
                        if (g_show_controls.load()) {
                            g_show_controls.store(false);
                        } else if (pause_escape()) {
                            s_esc_dropped.store(true);
                        }
                        break;
                    default:
                        break;
                }
            }

            if (sym == SDLK_F12) {
                fprintf(stderr, "[F12] manual minidump requested\n");
                fflush(stderr);
                rs64::app::notify("dump", "Saving a memory dump (F12). The game may freeze until it is written.", 60.0f);

                // A moment's delay lets the notice draw before the dump stalls the process.
                std::thread([] {

                    std::this_thread::sleep_for(std::chrono::milliseconds(250));

                    if (rs64::crash::write_minidump_safe(nullptr)) {
                        rs64::app::notify("dump", std::string("Memory dump saved: ") + rs64::crash::last_minidump_path(), 6.0f);
                    } else {
                        rs64::app::notify("dump", "Memory dump failed (see the log).", 6.0f);
                    }
                }).detach();
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

            // SDL sends joystick events for gamepads too.
            if (e.type == SDL_CONTROLLERBUTTONDOWN) s_last_device.store(rs64::input::Device::Gamepad);
            else if (!SDL_GameControllerFromInstanceID(e.jbutton.which)) s_last_device.store(rs64::input::Device::Joystick);

            break;
        case SDL_JOYHATMOTION:
            if (e.jhat.value && !SDL_GameControllerFromInstanceID(e.jhat.which)) s_last_device.store(rs64::input::Device::Joystick);

            break;
        case SDL_CONTROLLERAXISMOTION:
            if (std::abs(e.caxis.value) > 16000) {
                note_physical_input();
                s_last_device.store(rs64::input::Device::Gamepad);
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

// The overlay's last polled N64 buttons, read by the rebind capture.
static std::atomic<uint16_t> g_touch_buttons{0};

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

    if (take_touch_edit_request() && g_touch.config().enabled) {
        touch_slider_cancel();
        g_touch.begin_edit();
    }

    if (g_touch.take_layout_saved()) {
        rs64::touch::Config& shared = rs64::touch::shared_config();
        shared.layout = g_touch.config().layout;
        shared.stick_split = g_touch.config().stick_split;
        rs64::touch::save_config(shared, rs64::touch::config_path());
    }

    const rs64::touch::Pad pad = g_touch.poll(ctx, SDL_GetTicks());

    g_touch_buttons.store(pad.buttons);

    return pad;
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

#ifdef RS64_MCP

    if (rs64_mcp_consume_input(&nav_buttons, &nav_x, &nav_y)) {
        *buttons = nav_buttons;
        *x = nav_x;
        *y = nav_y;

        return true;
    }

#endif

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

    // While a letter wheel takes typing, keys that type (letters, digits, Space, punctuation, Backspace) drive no binding; arrows and Esc still do, and Enter does except on PASSCODES, where it submits the code.
    static uint8_t s_typing_keys[SDL_NUM_SCANCODES];
    const int typing = st.keys ? rs64_menu_typing() : 0;

    if (typing) {
        const int n = std::min(st.keys_len, (int)SDL_NUM_SCANCODES);

        std::memcpy(s_typing_keys, st.keys, n);
        std::memset(s_typing_keys + SDL_SCANCODE_A, 0, SDL_SCANCODE_0 - SDL_SCANCODE_A + 1);
        std::memset(s_typing_keys + SDL_SCANCODE_SPACE, 0, SDL_SCANCODE_SLASH - SDL_SCANCODE_SPACE + 1);
        s_typing_keys[SDL_SCANCODE_BACKSPACE] = 0;

        if (typing == 2) {
            s_typing_keys[SDL_SCANCODE_RETURN] = 0;
            s_typing_keys[SDL_SCANCODE_KP_ENTER] = 0;
        }

        st.keys = s_typing_keys;
        st.keys_len = n;
    }

    // An Esc that pause_escape turned into B drives no binding until it is released.
    static uint8_t s_esc_keys[SDL_NUM_SCANCODES];

    if (st.keys && s_esc_dropped.load()) {
        const int n = std::min(st.keys_len, (int)SDL_NUM_SCANCODES);

        if (n > SDL_SCANCODE_ESCAPE && st.keys[SDL_SCANCODE_ESCAPE]) {
            std::memcpy(s_esc_keys, st.keys, n);
            s_esc_keys[SDL_SCANCODE_ESCAPE] = 0;
            st.keys = s_esc_keys;
            st.keys_len = n;
        } else {
            s_esc_dropped.store(false);
        }
    }

    // While the F1 inspector is open (as for the mouse in menu_mouse_event) or an ImGui window takes the keyboard, keys drive no binding.
    static const uint8_t s_no_keys[SDL_NUM_SCANCODES] = {};

    if (rs64_rt64_inspector_open() || rs64::app::imgui_wants_keyboard()) {
        st.keys = s_no_keys;
        st.keys_len = SDL_NUM_SCANCODES;
    }

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

    // AT-ST (CHICKEN): the walker reads stick position, which a rate-style mouse can't hold, so the mouse aims the head instead (rs64_atst_head).
    const bool atst = st.mouse_active && rs64::host::flag("atst_active");

    if (atst) {
        const float k = g_bindings.mouse_sensitivity * kAtStHeadPerPixel;
        const float dx = g_bindings.mouse_invert_x ? -raw_dx : raw_dx;
        const float dy = g_bindings.mouse_invert_y ? -raw_dy : raw_dy;

        g_atst_head_x = std::clamp(g_atst_head_x + dx * k, -1.0f, 1.0f);
        g_atst_head_y = std::clamp(g_atst_head_y - dy * k, -1.0f, 1.0f);
        raw_dx = raw_dy = 0.0f;
    } else {
        g_atst_head_x = 0.0f;
        g_atst_head_y = 0.0f;
    }

    g_atst_head_on = atst;

    // Frame-rate-aware exponential low-pass on the mouse velocity so fast
    // flicks ramp in and stop-motion eases back to center, instead of the raw
    // per-frame delta snapping the stick to full/zero each frame.
    static uint32_t s_last_ms = 0;
    static float s_sm_dx = 0.0f, s_sm_dy = 0.0f;
    uint32_t smooth_now = SDL_GetTicks();
    float smooth_dt = s_last_ms ? (float)(smooth_now - s_last_ms) : 16.0f;
    s_last_ms = smooth_now;

    // Pixels since the last poll grow with the frame time and the game scales turning by it too, so convert to pixels per 30 fps frame; the floor guards a double poll.
    const float per_frame = (1000.0f / 30.0f) / std::max(smooth_dt, 1000.0f / 60.0f);

    raw_dx *= per_frame;
    raw_dy *= per_frame;

    float tau = g_bindings.mouse_smoothing * 100.0f;  // smoothing amount -> ms time constant
    float alpha = tau > 0.0f ? smooth_dt / (smooth_dt + tau) : 1.0f;
    s_sm_dx += (raw_dx - s_sm_dx) * alpha;
    s_sm_dy += (raw_dy - s_sm_dy) * alpha;
    st.mouse_dx = s_sm_dx;
    st.mouse_dy = s_sm_dy;
    st.joys = g_joy_by_dev.data();
    st.joys_len = (int)g_joy_by_dev.size();

    if (input_claim().on) apply_input_claim(st);

    active = rs64::input::resolve(g_bindings, st, &btn, x, y);
    g_throttle.store(rs64::input::throttle_position(g_bindings, st));
    bind_lk.unlock();

    // Rebinding flight actions never takes away menu confirm/back/navigation.
    const rs64::touch::Context menu_ctx = touch_context();

    if (mouse_menu_context(menu_ctx)) btn = rs64::input::menu_buttons(btn, rs64::input::menu_fixed_mask(st, menu_ctx == rs64::touch::Context::PauseMenu));

    // The layout's THRUST (+0x18) and BRAKES1 (+0x12) masks (D_8010BE18) walk forward / back; BRAKES1 is dropped since the walker would stop to aim the head with the stick.
    if (atst && g_recomp_rdram_for_wp_raw)
    {
        const uint8_t* rd = (const uint8_t*)g_recomp_rdram_for_wp_raw;
        const uint16_t thrust = rs64::mips::rh(rd, 0x8010BE30u);
        const uint16_t brake = rs64::mips::rh(rd, 0x8010BE2Au);

        if (btn & thrust) *y = 1.0f;

        if (btn & brake)
        {
            *y = -1.0f;
            btn &= (uint16_t)~brake;
        }
    }

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

// Head angles for updatePlayerAtStBehavior 0x800D1AE0 in the game's own scale (yaw = x * 80, pitch = y * -30); 0 while the mouse isn't aiming.
extern "C" int rs64_atst_head(float* yaw, float* pitch)
{

    if (!g_atst_head_on) return 0;

    *yaw = g_atst_head_x * 80.0f;
    *pitch = g_atst_head_y * -30.0f;

    return 1;
}

bool get_n64_input(int controller_num, uint16_t* buttons, float* x, float* y)
{

    return rs64::ls::session_input(controller_num, buttons, x, y, [](int c, uint16_t* b, float* sx, float* sy) {
        const bool ok = get_n64_input_live(c, b, sx, sy);

        if (c == 0) {
            rs64::host::run_input_filters(0, b, sx, sy);
        }

        return ok;
    });
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

namespace {

// One rebind capture at a time (the F6 window or the in-game Controls screen): inputs pressed or moved since capture_begin.
struct CaptureState {
    rs64::input::CaptureFilter filter;
    bool active = false;
    uint8_t keys[SDL_NUM_SCANCODES] = {0};
    uint32_t pad = 0, mouse = 0;
    uint16_t touch = 0;
    float pad_axes[SDL_CONTROLLER_AXIS_MAX] = {0};
    struct JoySnap { SDL_JoystickID id; std::vector<float> axes; std::vector<uint8_t> buttons, hats; };
    std::vector<JoySnap> joys;
};

CaptureState s_cap;

// The F6 window (graphics thread) and the Controls screen (game thread) share the capture; taken before g_bindings_mtx.
std::mutex s_cap_mtx;

uint32_t pad_buttons_now()
{

    uint32_t m = 0;

    if (!controller) return 0;

    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b) {
        if (SDL_GameControllerGetButton(controller, (SDL_GameControllerButton)b)) m |= 1u << b;
    }

    return m;
}

} // namespace

void capture_begin(const rs64::input::CaptureFilter& f)
{

    std::lock_guard<std::mutex> cap_lk(s_cap_mtx);
    int nk = 0;
    const uint8_t* ks = SDL_GetKeyboardState(&nk);

    s_cap = CaptureState{};
    s_cap.filter = f;
    s_cap.active = true;
    memcpy(s_cap.keys, ks, std::min(nk, (int)SDL_NUM_SCANCODES));
    s_cap.pad = pad_buttons_now();
    s_cap.mouse = SDL_GetMouseState(nullptr, nullptr);
    s_cap.touch = g_touch_buttons.load();

    for (int a = 0; controller && a < SDL_CONTROLLER_AXIS_MAX; ++a) s_cap.pad_axes[a] = SDL_GameControllerGetAxis(controller, (SDL_GameControllerAxis)a) / 32767.0f;

    std::lock_guard<std::mutex> lk(g_bindings_mtx);

    for (const OpenJoy& oj : g_open_joys)
    {
        CaptureState::JoySnap s{ oj.id, {}, {}, {} };

        for (int a = 0; a < SDL_JoystickNumAxes(oj.joy); ++a) s.axes.push_back(SDL_JoystickGetAxis(oj.joy, a) / 32767.0f);
        for (int b = 0; b < SDL_JoystickNumButtons(oj.joy); ++b) s.buttons.push_back(SDL_JoystickGetButton(oj.joy, b));
        for (int h = 0; h < SDL_JoystickNumHats(oj.joy); ++h) s.hats.push_back(SDL_JoystickGetHat(oj.joy, h));

        s_cap.joys.push_back(std::move(s));
    }
}

int capture_poll(rs64::input::Source* out)
{

    namespace ri = rs64::input;

    std::lock_guard<std::mutex> cap_lk(s_cap_mtx);

    if (!s_cap.active) return -1;

    int nk = 0;
    const uint8_t* ks = SDL_GetKeyboardState(&nk);
    const uint32_t pad = pad_buttons_now();
    const bool start_cancels = !s_cap.filter.any && (pad & ~s_cap.pad & (1u << SDL_CONTROLLER_BUTTON_START));
    const uint16_t touch = g_touch_buttons.load();
    const bool touch_cancels = touch & ~s_cap.touch & (rs64::touch::B | rs64::touch::START);

    s_cap.touch = touch;

    if (ks[SDL_SCANCODE_ESCAPE] || start_cancels || touch_cancels)
    {
        s_cap.active = false;

        return -1;
    }

    ri::Source got;
    bool have = false;
    auto offer = [&](const ri::Source& s) {

        if (!have && ri::capture_accepts(s_cap.filter, s)) {
            got = s;
            have = true;
        }
    };

    for (int sc = 0; sc < nk && sc < SDL_NUM_SCANCODES; ++sc)
    {
        if (ks[sc] && !s_cap.keys[sc]) offer({ ri::SourceKind::Key, sc, 0 });

        s_cap.keys[sc] = ks[sc];
    }

    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b)
    {
        if ((pad >> b & 1) && !(s_cap.pad >> b & 1)) offer({ ri::SourceKind::PadButton, b, 0 });
    }

    s_cap.pad = pad;

    // A trigger or stick half moved past half travel from where it rested when the capture began.
    for (int a = 0; controller && a < SDL_CONTROLLER_AXIS_MAX; ++a)
    {
        const float delta = SDL_GameControllerGetAxis(controller, (SDL_GameControllerAxis)a) / 32767.0f - s_cap.pad_axes[a];

        if (std::fabs(delta) >= 0.5f) offer({ ri::SourceKind::PadAxis, a, (int8_t)(delta > 0.0f ? 1 : -1) });
    }

    // With touch on the pointer is a finger (SDL_HINT_MOUSE_TOUCH_EVENTS, compositor mouse emulation), so its buttons never bind, as in poll_input.
    const uint32_t mb = rs64::touch::shared_config().enabled ? 0 : SDL_GetMouseState(nullptr, nullptr);

    for (int b = 1; b <= 5; ++b)
    {
        if ((mb & SDL_BUTTON(b)) && !(s_cap.mouse & SDL_BUTTON(b))) offer({ ri::SourceKind::MouseButton, b, 0 });
    }

    s_cap.mouse = mb;

    std::unique_lock<std::mutex> lk(g_bindings_mtx);

    for (const OpenJoy& oj : g_open_joys)
    {
        const int dev = joy_dev_locked(oj);
        const CaptureState::JoySnap* s = nullptr;

        for (const auto& js : s_cap.joys) {
            if (js.id == oj.id) s = &js;
        }

        if (dev < 0 || !s) continue;

        for (int b = 0; b < (int)s->buttons.size(); ++b)
        {
            if (SDL_JoystickGetButton(oj.joy, b) && !s->buttons[b]) offer({ ri::SourceKind::JoyButton, b, 0, (int8_t)dev });
        }

        for (int h = 0; h < (int)s->hats.size(); ++h)
        {
            const uint8_t now_bits = SDL_JoystickGetHat(oj.joy, h) & (uint8_t)~s->hats[h];

            for (int bit = 0; bit < 4; ++bit)
            {
                if (now_bits & (1u << bit)) offer({ ri::SourceKind::JoyHat, h, (int8_t)(1 << bit), (int8_t)dev });
            }
        }

        for (int a = 0; a < (int)s->axes.size(); ++a)
        {
            const float delta = SDL_JoystickGetAxis(oj.joy, a) / 32767.0f - s->axes[a];

            if (std::fabs(delta) < 0.5f) continue;

            int dir = delta > 0.0f ? 1 : -1;

            if (a < 32 && ((g_bindings.joy_devices[dev].invert_axes >> a) & 1u)) dir = -dir;

            offer({ ri::SourceKind::JoyAxis, a, (int8_t)dir, (int8_t)dev });
        }
    }

    lk.unlock();

    if (!have) return 0;

    *out = got;
    s_cap.active = false;

    return 1;
}

void capture_end()
{

    std::lock_guard<std::mutex> cap_lk(s_cap_mtx);

    s_cap.active = false;
}

bool capture_device_present(rs64::input::Device d)
{

    std::lock_guard<std::mutex> lk(g_bindings_mtx);

    switch (d)
    {
        case rs64::input::Device::Gamepad:  return controller != nullptr;
        case rs64::input::Device::Joystick: return !g_open_joys.empty();
        default:                            return true;
    }
}

void reset_device_bindings(rs64::input::Device d)
{

    std::lock_guard<std::mutex> lk(g_bindings_mtx);

    g_bindings = rs64::input::default_device_bindings(g_bindings, d);

    // Joystick defaults are per device (default_bindings has none), so each open stick gets its own again.
    if (d == rs64::input::Device::Joystick)
    {
        for (const OpenJoy& oj : g_open_joys)
        {
            const int dev = joy_dev_locked(oj);

            if (dev >= 0) rs64::input::add_joystick_defaults(g_bindings, dev, SDL_JoystickNumAxes(oj.joy), SDL_JoystickNumButtons(oj.joy), SDL_JoystickNumHats(oj.joy));
        }
    }

    rs64::input::save_bindings(g_bindings, rs64::input::default_config_path());
}
}
