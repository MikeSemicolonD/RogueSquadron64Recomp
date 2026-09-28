// cmake --build build --config Debug --target input_bindings_test
#define SDL_MAIN_HANDLED
#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif
#include "../src/main/input_bindings.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace rs64::input;

static bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

int main() {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_JOYSTICK) != 0) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
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

    // Roll on a twist axis (3): holds R and takes over stick X; pitch stays on the stick.
    {
        Bindings rb = b;
        rb.targets[(int)Target::RollRight].push_back(Source{ SourceKind::JoyAxis, 3, +1, 0 });
        rb.targets[(int)Target::RollLeft].push_back(Source{ SourceKind::JoyAxis, 3, -1, 0 });
        SDL_JoystickSetVirtualAxis(j, 3, -32768);
        SDL_JoystickSetVirtualAxis(j, 1, -32768);
        SDL_JoystickUpdate();
        resolve(rb, st, &btn, &x, &y);
        assert((btn & 0x0010) && near(x, -80.0f / 127.0f) && near(y, 80.0f / 127.0f));
        SDL_JoystickSetVirtualAxis(j, 3, 3000);
        SDL_JoystickUpdate();
        resolve(rb, st, &btn, &x, &y);
        assert(!(btn & 0x0010));
        SDL_JoystickSetVirtualAxis(j, 3, 32767);
        SDL_JoystickSetVirtualHat(j, 0, SDL_HAT_LEFT);
        SDL_JoystickUpdate();
        resolve(rb, st, &btn, &x, &y);
        assert(!(btn & 0x0010) && (btn & 0x0008) && near(x, -1.0f));
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
