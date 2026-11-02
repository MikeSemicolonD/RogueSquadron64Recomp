// cmake --build build --config Debug --target input_bindings_test
#define SDL_MAIN_HANDLED
#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif
#include "../src/input/input_bindings.h"
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace rs64::input;

static bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

static Source key(int sc) { return Source{ SourceKind::Key, sc, 0 }; }

static bool same(const Source& p, const Source& q) {
    return p.kind == q.kind && p.code == q.code && p.dir == q.dir && p.dev == q.dev;
}

static bool holds(const Bindings& b, Target t, const Source& s)
{

    for (const Source& x : b.targets[(int)t]) {
        if (same(x, s)) return true;
    }

    return false;
}

static bool font_safe(const std::string& s)
{

    for (char c : s) {
        if (!(std::isupper((unsigned char)c) || std::isdigit((unsigned char)c) || std::strchr(" ;:_'/&\",!?-<>{}.", c))) return false;
    }

    return !s.empty();
}

// Rebind-menu logic: device classes, rows, swap.
static void controls_menu_checks()
{

    // device_of_classifies_sources
    assert(device_of(key(SDL_SCANCODE_W)) == Device::Keyboard);
    assert(device_of(Source{ SourceKind::MouseButton, 1, 0 }) == Device::Keyboard);
    assert(device_of(Source{ SourceKind::MouseAxis, 0, 1 }) == Device::Keyboard);
    assert(device_of(Source{ SourceKind::PadAxis, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1 }) == Device::Gamepad);
    assert(device_of(Source{ SourceKind::JoyHat, 0, SDL_HAT_UP, 0 }) == Device::Joystick);

    // rows_for_keyboard_skip_start_throttle_roll
    const std::vector<ControlRow> kr = control_rows(Device::Keyboard);
    assert(!kr.empty() && kr[0].target == Target::A && std::strcmp(kr[0].action, "THRUST") == 0);

    for (const ControlRow& r : kr) {
        assert(r.target != Target::Start && r.target != Target::Throttle && r.target != Target::RollLeft && r.target != Target::RollRight);
        assert(font_safe(r.action));
    }

    // rows_for_joystick_include_throttle_and_roll
    bool thr = false, roll = false;

    for (const ControlRow& r : control_rows(Device::Joystick)) {
        thr = thr || r.target == Target::Throttle;
        roll = roll || r.target == Target::RollLeft;
        assert(r.target != Target::Start);
    }

    assert(thr && roll);

    // rebind_keeps_other_sources
    Bindings b = default_bindings();
    assert(b.targets[(int)Target::A][0].code == SDL_SCANCODE_W);
    assert(rebind(b, Target::A, Device::Keyboard, key(SDL_SCANCODE_G)) == Target::Count);
    assert(b.targets[(int)Target::A][0].code == SDL_SCANCODE_G);
    assert(holds(b, Target::A, key(SDL_SCANCODE_RETURN)) && !holds(b, Target::A, key(SDL_SCANCODE_W)));
    assert(b.targets[(int)Target::A][shown_source(b, Target::A, Device::Keyboard)].code == SDL_SCANCODE_G);
    assert(holds(b, Target::A, Source{ SourceKind::PadButton, SDL_CONTROLLER_BUTTON_A, 0 }));

    // rebind_swaps_matched_source_in_place
    b = default_bindings();
    const size_t b_space = 0;
    assert(same(b.targets[(int)Target::B][b_space], key(SDL_SCANCODE_SPACE)));
    assert(rebind(b, Target::A, Device::Keyboard, key(SDL_SCANCODE_SPACE)) == Target::B);
    assert(same(b.targets[(int)Target::A][0], key(SDL_SCANCODE_SPACE)));
    assert(same(b.targets[(int)Target::B][b_space], key(SDL_SCANCODE_W)));
    assert(holds(b, Target::B, key(SDL_SCANCODE_BACKSPACE)) && holds(b, Target::B, Source{ SourceKind::MouseButton, SDL_BUTTON_LEFT, 0 }));
    assert(holds(b, Target::A, key(SDL_SCANCODE_RETURN)));

    // rebind_swap_with_second_source
    b = default_bindings();
    assert(rebind(b, Target::B, Device::Keyboard, key(SDL_SCANCODE_RETURN)) == Target::Count);
    assert(same(b.targets[(int)Target::B][0], key(SDL_SCANCODE_RETURN)));
    assert(!holds(b, Target::A, key(SDL_SCANCODE_RETURN)) && !holds(b, Target::A, key(SDL_SCANCODE_SPACE)));
    assert(same(b.targets[(int)Target::A][0], key(SDL_SCANCODE_W)));

    // rebind_same_source_is_noop
    b = default_bindings();
    const size_t n_a = b.targets[(int)Target::A].size();
    assert(rebind(b, Target::A, Device::Keyboard, key(SDL_SCANCODE_W)) == Target::Count);
    assert(b.targets[(int)Target::A].size() == n_a && same(b.targets[(int)Target::A][0], key(SDL_SCANCODE_W)));

    // rebind_hidden_source_is_taken_not_swapped: B's mouse-left is not its shown keyboard binding (Space is), so B just loses it.
    b = default_bindings();
    const Source lmb{ SourceKind::MouseButton, SDL_BUTTON_LEFT, 0 };
    assert(rebind(b, Target::A, Device::Keyboard, lmb) == Target::Count);
    assert(same(b.targets[(int)Target::A][0], lmb));
    assert(!holds(b, Target::B, lmb) && !holds(b, Target::B, key(SDL_SCANCODE_W)));
    assert(same(b.targets[(int)Target::B][shown_source(b, Target::B, Device::Keyboard)], key(SDL_SCANCODE_SPACE)));

    // A target with no source of the device gets the captured one appended; the giver loses it.
    b = default_bindings();
    assert(shown_source(b, Target::LookUp, Device::Keyboard) == -1);
    assert(rebind(b, Target::LookUp, Device::Keyboard, key(SDL_SCANCODE_Q)) == Target::Count);
    assert(holds(b, Target::LookUp, key(SDL_SCANCODE_Q)) && !holds(b, Target::CUp, key(SDL_SCANCODE_Q)));

    // default_device_bindings restores one device only.
    b = default_bindings();
    rebind(b, Target::A, Device::Keyboard, key(SDL_SCANCODE_G));
    rebind(b, Target::A, Device::Gamepad, Source{ SourceKind::PadButton, SDL_CONTROLLER_BUTTON_Y, 0 });
    b = default_device_bindings(b, Device::Keyboard);
    assert(same(b.targets[(int)Target::A][shown_source(b, Target::A, Device::Keyboard)], key(SDL_SCANCODE_W)));
    assert(b.targets[(int)Target::A][shown_source(b, Target::A, Device::Gamepad)].code == SDL_CONTROLLER_BUTTON_Y);

    // menu_source_name_is_font_safe
    assert(menu_source_name(key(SDL_SCANCODE_LSHIFT)) == "L SHIFT");
    assert(menu_source_name(Source{ SourceKind::PadButton, SDL_CONTROLLER_BUTTON_A, 0 }) == "PAD A");
    assert(font_safe(menu_source_name(Source{ SourceKind::PadAxis, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1 })));
    assert(menu_source_name(Source{ SourceKind::MouseButton, 1, 0 }) == "MOUSE 1");
    assert(menu_source_name(Source{ SourceKind::JoyAxis, 2, -1, 0 }) == "J1 AX 2 <");
    assert(menu_source_name(Source{ SourceKind::JoyButton, 3, 0, 0 }) == "J1 BTN 3");
    assert(menu_source_name(Source{ SourceKind::PadAxis, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1 }) == "LT");
    assert(menu_source_name(Source{ SourceKind::PadAxis, SDL_CONTROLLER_AXIS_RIGHTX, -1 }) == "RS X <");
    assert(menu_source_name(Source{}) == "--");
    assert(font_safe(menu_source_name(key(SDL_SCANCODE_KP_PLUS))));

    // Punctuation keys get names: ';' '/' '"' '&' '_' are scroll arrows, the period and blank in the menu font.
    assert(menu_source_name(key(SDL_SCANCODE_SEMICOLON)) == "SEMICOLON");
    assert(menu_source_name(key(SDL_SCANCODE_SLASH)) == "SLASH");
    assert(menu_source_name(key(SDL_SCANCODE_LEFTBRACKET)) == "L BRACKET");
    assert(menu_source_name(key(SDL_SCANCODE_GRAVE)) == "GRAVE");

    // menu_buttons_fixed_wins: a held menu key's own button replaces what its binding adds (Return bound to fire must not also back out).
    assert(menu_buttons(0x4000, 0x8000) == 0x8000);
    assert(menu_buttons(0x4000 | 0x0020, 0x8000) == (0x8000 | 0x0020));
    assert(menu_buttons(0x4000, 0) == 0x4000);

    // capture_filter_keyboard_takes_mouse_buttons_not_motion
    const CaptureFilter kf{ false, Device::Keyboard };
    assert(capture_accepts(kf, key(SDL_SCANCODE_G)));
    assert(capture_accepts(kf, Source{ SourceKind::MouseButton, 2, 0 }));
    assert(!capture_accepts(kf, Source{ SourceKind::MouseAxis, 0, 1 }));
    assert(!capture_accepts(kf, Source{ SourceKind::PadButton, SDL_CONTROLLER_BUTTON_A, 0 }));

    // capture_filter_gamepad_takes_pad_axis
    const CaptureFilter gf{ false, Device::Gamepad };
    assert(capture_accepts(gf, Source{ SourceKind::PadAxis, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 1 }));
    assert(capture_accepts(gf, Source{ SourceKind::PadButton, SDL_CONTROLLER_BUTTON_B, 0 }));
    assert(!capture_accepts(gf, key(SDL_SCANCODE_G)));

    // capture_filter_joystick_rejects_keys
    const CaptureFilter jf{ false, Device::Joystick };
    assert(!capture_accepts(jf, key(SDL_SCANCODE_G)));
    assert(capture_accepts(jf, Source{ SourceKind::JoyAxis, 2, -1, 1 }));

    // The F6 window takes anything but mouse motion.
    const CaptureFilter af{};
    assert(capture_accepts(af, key(SDL_SCANCODE_G)) && capture_accepts(af, Source{ SourceKind::JoyHat, 0, 1, 0 }));
    assert(!capture_accepts(af, Source{ SourceKind::MouseAxis, 1, -1 }));

    // menu_fixed_mask_keyboard: confirm/back/start/arrows act in menus whatever the bindings say.
    uint8_t keys[SDL_NUM_SCANCODES] = {0};
    RawState st;
    st.keys = keys;
    st.keys_len = SDL_NUM_SCANCODES;
    assert(menu_fixed_mask(st) == 0);
    keys[SDL_SCANCODE_RETURN] = 1;
    keys[SDL_SCANCODE_BACKSPACE] = 1;
    assert(menu_fixed_mask(st) == (0x8000 | 0x4000));
    keys[SDL_SCANCODE_RETURN] = 0;
    keys[SDL_SCANCODE_BACKSPACE] = 0;
    keys[SDL_SCANCODE_ESCAPE] = 1;
    keys[SDL_SCANCODE_UP] = 1;
    assert(menu_fixed_mask(st) == (0x4000 | 0x0800));
    assert(menu_fixed_mask(st, true) == (0x1000 | 0x0800));
}

// menu_fixed_mask_pad: pad A/B/Start and the D-pad.
static void menu_fixed_pad_checks()
{

    const int vi = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, SDL_CONTROLLER_BUTTON_MAX, 0);
    assert(vi >= 0 && SDL_IsGameController(vi));
    SDL_GameController* pad = SDL_GameControllerOpen(vi);
    SDL_Joystick* pj = SDL_GameControllerGetJoystick(pad);
    assert(pad);

    RawState st;
    st.pad = pad;
    SDL_JoystickSetVirtualButton(pj, SDL_CONTROLLER_BUTTON_B, 1);
    SDL_JoystickSetVirtualButton(pj, SDL_CONTROLLER_BUTTON_START, 1);
    SDL_GameControllerUpdate();
    assert(menu_fixed_mask(st) == (0x4000 | 0x1000));
    SDL_JoystickSetVirtualButton(pj, SDL_CONTROLLER_BUTTON_B, 0);
    SDL_JoystickSetVirtualButton(pj, SDL_CONTROLLER_BUTTON_START, 0);
    SDL_JoystickSetVirtualButton(pj, SDL_CONTROLLER_BUTTON_A, 1);
    SDL_JoystickSetVirtualButton(pj, SDL_CONTROLLER_BUTTON_DPAD_DOWN, 1);
    SDL_GameControllerUpdate();
    assert(menu_fixed_mask(st) == (0x8000 | 0x0400));

    // pad_deadzone_is_tunable: a stick at 40% steers at the default dead zone and not at 0.5.
    Bindings b = default_bindings();
    uint8_t keys[SDL_NUM_SCANCODES] = {0};
    uint16_t btn = 0;
    float x = 0, y = 0;
    st.keys = keys;
    st.keys_len = SDL_NUM_SCANCODES;
    SDL_JoystickSetVirtualAxis(pj, SDL_CONTROLLER_AXIS_LEFTX, (int16_t)(0.4f * 32767));
    SDL_GameControllerUpdate();
    resolve(b, st, &btn, &x, &y);
    assert(x > 0.0f);
    b.pad_deadzone = 0.5f;
    resolve(b, st, &btn, &x, &y);
    assert(near(x, 0.0f));

    Bindings r;
    assert(save_bindings(b, "pad_deadzone_test.json") && load_bindings(r, "pad_deadzone_test.json"));
    remove("pad_deadzone_test.json");
    assert(near(r.pad_deadzone, 0.5f));

    SDL_GameControllerClose(pad);
    SDL_JoystickDetachVirtual(vi);
}

int main() {

#ifdef _WIN32
    // A failed assert prints and exits instead of opening the Debug CRT abort dialog.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif

    controls_menu_checks();
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    menu_fixed_pad_checks();
    // Virtual 4-axis, 10-button, 1-hat stick standing in for a flight stick.
    int vidx = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_FLIGHT_STICK, 4, 10, 1);
    assert(vidx >= 0);
    SDL_Joystick* j = SDL_JoystickOpen(vidx);
    assert(j);

    uint8_t keys[SDL_NUM_SCANCODES] = {0};
    RawState st;
    st.keys = keys;
    st.keys_len = SDL_NUM_SCANCODES;

    Bindings b;   // no default keyboard binds: start from an empty profile
    b.keyboard_enabled = false;
    uint16_t btn = 0;
    float x = 0, y = 0;

    // No keyboard, pad, or joystick: inactive.
    assert(!resolve(b, st, &btn, &x, &y));

    bool added = false;
    int dev = find_or_add_joy_device(b, "guid-a", 0, "Test Stick", &added);
    assert(dev == 0 && added);
    assert(find_or_add_joy_device(b, "guid-a", 0, "Test Stick", &added) == 0 && !added);
    assert(find_or_add_joy_device(b, "guid-a", 1, "Test Stick", &added) == 1 && added);
    b.joy_devices.pop_back();

    add_joystick_defaults(b, dev, 4, 10, 1);
    SDL_Joystick* joys[1] = { j };
    st.joys = joys;
    st.joys_len = 1;

    // Four axes: X/Y steer, throttle left unbound.
    assert(b.targets[(int)Target::StickRight].size() == 1);
    assert(b.targets[(int)Target::A].size() == 1 && b.targets[(int)Target::A][0].kind == SourceKind::JoyButton);

    // Full right deflection maps to stick_range (80), not 127.
    SDL_JoystickSetVirtualAxis(j, 0, 32767);
    SDL_JoystickUpdate();
    assert(resolve(b, st, &btn, &x, &y));
    assert(near(x, 80.0f / 127.0f));
    b.stick_range = 127.0f;
    resolve(b, st, &btn, &x, &y);
    assert(near(x, 1.0f));
    b.stick_range = 80.0f;

    // Inversion flips the axis.
    b.joy_devices[0].invert_axes = 1u;
    resolve(b, st, &btn, &x, &y);
    assert(near(x, -80.0f / 127.0f));
    b.joy_devices[0].invert_axes = 0;

    // Rudder (axis 2) sums with stick X in the same direction and clamps.
    b.targets[(int)Target::StickRight].push_back(Source{ SourceKind::JoyAxis, 2, +1, 0 });
    SDL_JoystickSetVirtualAxis(j, 0, 16384);
    SDL_JoystickSetVirtualAxis(j, 2, 16384);
    SDL_JoystickUpdate();
    resolve(b, st, &btn, &x, &y);
    float one = (0.5f - 0.05f) / 0.95f * (80.0f / 127.0f);
    assert(near(x, 2.0f * one));
    SDL_JoystickSetVirtualAxis(j, 0, 0);
    SDL_JoystickSetVirtualAxis(j, 2, 0);

    // Buttons: button 0 = B (fire).
    SDL_JoystickSetVirtualButton(j, 0, 1);
    SDL_JoystickUpdate();
    resolve(b, st, &btn, &x, &y);
    assert(btn & 0x4000);
    SDL_JoystickSetVirtualButton(j, 0, 0);

    // Hat up = look: holds C-Up and points the stick up, overriding steering.
    SDL_JoystickSetVirtualAxis(j, 0, 32767);
    SDL_JoystickSetVirtualHat(j, 0, SDL_HAT_UP);
    SDL_JoystickUpdate();
    resolve(b, st, &btn, &x, &y);
    assert((btn & 0x0008) && near(x, 0.0f) && near(y, 1.0f));
    SDL_JoystickSetVirtualHat(j, 0, SDL_HAT_CENTERED);
    SDL_JoystickSetVirtualAxis(j, 0, 0);
    SDL_JoystickUpdate();

    // Roll on a twist axis (3): does nothing on its own; with the Roll button (button 6 -> R) held it takes over stick X. Pitch stays on the stick.
    {
        Bindings rb = b;
        rb.targets[(int)Target::RollRight].push_back(Source{ SourceKind::JoyAxis, 3, +1, 0 });
        rb.targets[(int)Target::RollLeft].push_back(Source{ SourceKind::JoyAxis, 3, -1, 0 });
        SDL_JoystickSetVirtualAxis(j, 3, -32768);
        SDL_JoystickSetVirtualAxis(j, 1, -32768);
        SDL_JoystickUpdate();
        resolve(rb, st, &btn, &x, &y);
        assert(!(btn & 0x0010) && near(x, 0.0f) && near(y, 80.0f / 127.0f));
        SDL_JoystickSetVirtualButton(j, 6, 1);
        SDL_JoystickUpdate();
        resolve(rb, st, &btn, &x, &y);
        assert((btn & 0x0010) && near(x, -80.0f / 127.0f) && near(y, 80.0f / 127.0f));
        SDL_JoystickSetVirtualAxis(j, 3, 3000);
        SDL_JoystickUpdate();
        resolve(rb, st, &btn, &x, &y);
        assert((btn & 0x0010) && near(x, 0.0f));
        SDL_JoystickSetVirtualAxis(j, 3, 32767);
        SDL_JoystickSetVirtualHat(j, 0, SDL_HAT_LEFT);
        SDL_JoystickUpdate();
        resolve(rb, st, &btn, &x, &y);
        assert((btn & 0x0008) && near(x, -1.0f));
        SDL_JoystickSetVirtualButton(j, 6, 0);
        SDL_JoystickSetVirtualHat(j, 0, SDL_HAT_CENTERED);
        SDL_JoystickSetVirtualAxis(j, 3, 0);
        SDL_JoystickSetVirtualAxis(j, 1, 0);
        SDL_JoystickUpdate();
    }

    // Joystick toggle off: joystick sources read nothing.
    b.joystick_enabled = false;
    SDL_JoystickSetVirtualButton(j, 0, 1);
    SDL_JoystickUpdate();
    assert(!resolve(b, st, &btn, &x, &y));
    b.joystick_enabled = true;
    SDL_JoystickSetVirtualButton(j, 0, 0);
    SDL_JoystickUpdate();

    // Three-axis stick: axis 2 is a positional throttle. Forward (negative) = 1, back = 0, and it presses no buttons.
    Bindings t;
    t.keyboard_enabled = false;
    int tdev = find_or_add_joy_device(t, "guid-b", 0, "Classic Stick", &added);
    add_joystick_defaults(t, tdev, 3, 4, 0);
    assert(throttle_position(t, st) >= 0.0f);
    SDL_JoystickSetVirtualAxis(j, 2, -32768);
    SDL_JoystickUpdate();
    assert(near(throttle_position(t, st), 1.0f));
    resolve(t, st, &btn, &x, &y);
    assert(!(btn & 0xA000));
    SDL_JoystickSetVirtualAxis(j, 2, 32767);
    SDL_JoystickUpdate();
    assert(near(throttle_position(t, st), 0.0f));
    SDL_JoystickSetVirtualAxis(j, 2, 0);
    SDL_JoystickUpdate();
    assert(near(throttle_position(t, st), 0.5f));
    t.joystick_enabled = false;
    assert(throttle_position(t, st) < 0.0f);
    assert(throttle_position(b, st) < 0.0f);

    // Speed mapping: back = min, cruise point = cruise, forward = max (X-wing 2.25 / 2.625 / 3.75).
    assert(near(throttle_speed(0.0f, 0.5f, 2.25f, 2.625f, 3.75f), 2.25f));
    assert(near(throttle_speed(0.5f, 0.5f, 2.25f, 2.625f, 3.75f), 2.625f));
    assert(near(throttle_speed(1.0f, 0.5f, 2.25f, 2.625f, 3.75f), 3.75f));
    assert(near(throttle_speed(0.75f, 0.5f, 2.25f, 2.625f, 3.75f), 3.1875f));
    assert(near(throttle_speed(0.5f, 0.5f, 2.0f, 9.0f, 4.0f), 3.0f));

    // Saitek X52 (7 axes): throttle on axis 2, twist on axis 5 as roll.
    {
        Bindings x;
        int xd = find_or_add_joy_device(x, "guid-x52", 0, "X52 H.O.T.A.S.", &added);
        add_joystick_defaults(x, xd, 7, 34, 1);
        assert(x.targets[(int)Target::Throttle].size() == 1 && x.targets[(int)Target::Throttle][0].code == 2);
        assert(x.targets[(int)Target::RollRight].size() == 1 && x.targets[(int)Target::RollRight][0].code == 5);
        assert(x.targets[(int)Target::StickRight].size() == 1 && x.targets[(int)Target::StickRight][0].code == 0);
    }

    // A device named "throttle" binds only its first axis, to Throttle.
    Bindings th;
    int thd = find_or_add_joy_device(th, "guid-c", 0, "Thrustmaster TWCS Throttle", &added);
    add_joystick_defaults(th, thd, 5, 14, 1);
    assert(th.targets[(int)Target::Throttle].size() == 1 && th.targets[(int)Target::Throttle][0].code == 0);
    assert(th.targets[(int)Target::A].empty() && th.targets[(int)Target::StickLeft].empty() && th.targets[(int)Target::B].empty());

    // Gamepad/joystick range doesn't change keyboard or mouse steering.
    Bindings kb = default_bindings();
    keys[SDL_SCANCODE_RIGHT] = 1;
    st.joys = nullptr;
    st.joys_len = 0;
    resolve(kb, st, &btn, &x, &y);
    assert(near(x, 1.0f));
    keys[SDL_SCANCODE_RIGHT] = 0;

    // JSON round trip keeps devices, joystick binds, rumble, and ranges.
    b.rumble.strength = 0.5f;
    b.rumble.weapons = false;
    b.joy_devices[0].deadzone = 0.1f;
    b.joy_devices[0].invert_axes = 4u;
    const char* path = "input_bindings_test.json";
    assert(save_bindings(b, path));
    Bindings r;
    assert(load_bindings(r, path));
    remove(path);
    assert(r.joy_devices.size() == 1 && r.joy_devices[0].guid == "guid-a" && r.joy_devices[0].name == "Test Stick");
    assert(near(r.joy_devices[0].deadzone, 0.1f) && r.joy_devices[0].invert_axes == 4u);
    assert(near(r.rumble.strength, 0.5f) && !r.rumble.weapons && r.rumble.hit);
    assert(!r.keyboard_enabled && near(r.stick_range, 80.0f) && near(r.throttle_cruise, 0.5f));
    for (int ti = 0; ti < (int)Target::Count; ++ti) {
        assert(r.targets[ti].size() == b.targets[ti].size());
        for (size_t k = 0; k < r.targets[ti].size(); ++k) {
            const Source& p = r.targets[ti][k];
            const Source& q = b.targets[ti][k];
            assert(p.kind == q.kind && p.code == q.code && p.dir == q.dir && p.dev == q.dev);
        }
    }
    assert(source_label(r.targets[(int)Target::LookUp][0]) == "J1 hat0 up");

    SDL_JoystickClose(j);
    SDL_JoystickDetachVirtual(vidx);
    SDL_Quit();
    printf("input_bindings_test OK\n");
    return 0;
}
