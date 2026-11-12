#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif
#include "ultramodern/input.hpp"
#include "input/input_bindings.h"
#include "input/touch_input.h"
#include "input/touch_menu.h"

// Host input: SDL gamepads, raw joysticks, keyboard/mouse, touch and sensors, polled into N64 controller state. Shared with the ImGui panels and main().
namespace rs64::app {
    // Raw joysticks (flight sticks, HOTAS throttles, pedals): every joystick SDL doesn't treat as a
    // gamepad. g_joy_by_dev maps Bindings::joy_devices to the open handle. Both guarded by g_bindings_mtx.
    struct OpenJoy {
        SDL_Joystick*  joy = nullptr;
        SDL_JoystickID id = -1;
        std::string    guid;
        int            ordinal = 0;
        // Force-feedback sticks without SDL_JoystickRumble.
        SDL_Haptic*    haptic = nullptr;
    };

    extern SDL_GameController* controller;
    extern std::vector<SDL_GameController*> g_pads;
    extern rs64::input::Bindings g_bindings;
    // UI thread mutates, game thread reads.
    extern std::mutex g_bindings_mtx;
    extern std::atomic<bool> g_show_controls;
    extern std::atomic<bool> g_show_fps;
    extern std::atomic<bool> g_mouse_capture;
    // F8 with the fake VR head on: mouse motion turns the head instead of steering.
    extern std::atomic<bool> g_vr_mouse_look;
    // Mouse motion in pixels since the last call, while g_vr_mouse_look is on.
    void take_vr_mouse_look(float* dx, float* dy);
    extern std::vector<OpenJoy> g_open_joys;
    extern std::vector<SDL_Joystick*> g_joy_by_dev;
    extern std::atomic<float> g_throttle;
    extern std::atomic<bool> g_report_rumble_pak;
    extern bool g_fullscreen_applied;
    extern rs64::touch::Engine g_touch;
    extern std::mutex g_touch_mtx;

    // Set when a pad connects so the next lightbar tick (rumble.cpp) re-sends the current color.
    extern std::atomic<bool> g_lightbar_resend;

    void poll_input();
    bool get_n64_input(int controller_num, uint16_t* buttons, float* x, float* y);
    void set_rumble(int controller_num, bool on);
    ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num);
    void rumble_thread_main();
    int joy_dev_locked(const OpenJoy& oj);
    void rebind_open_joysticks_locked();

    // Rebind capture shared by the F6 window and the in-game Controls screen. capture_poll: 1 = *out captured,
    // 0 = waiting, -1 = cancelled (Esc, or pad Start for a one-device capture). Call without holding g_bindings_mtx.
    void capture_begin(const rs64::input::CaptureFilter& f);
    int capture_poll(rs64::input::Source* out);
    void capture_end();
    rs64::input::Device last_device();

    // Whether device d has anything open to capture from (a touch-only phone has no gamepad to answer "press a button").
    bool capture_device_present(rs64::input::Device d);

    // Device d's sources back to their defaults (joysticks: each open stick's own), saved.
    void reset_device_bindings(rs64::input::Device d);

    uint8_t* touch_rdram();
    rs64::touch::Context touch_context();
    rs64::touch::AccountScreen touch_account_screen();
    bool touch_menu_hits_allowed();
    rs64::touch::TapAction touch_menu_tap(float x, float y);
    bool mouse_menu_context(rs64::touch::Context ctx);
    void window_size(float* w, float* h);

    // A scrolling list page's scrollbar in normalized window coords: the track beside its rows and the thumb over the rows shown.
    struct ListBar { float x0, x1, y0, y1, t0, t1; int total, visible; };

    bool list_scrollbar(float w, float h, ListBar* out);

    // touch_host.cpp, called from poll_input: finger and mouse events on the menus, and the touch state poll_input resets.
    void touch_event(const SDL_Event& e);
    void menu_mouse_event(const SDL_Event& e);
    void menu_wheel_x(int steps);
    bool pause_escape();
    // Mouse id of events the MCP bridge pushes; the menus take them without window focus.
    constexpr uint32_t kMcpMouseId = 0x4D435000u;
    void touch_slider_cancel();
    bool take_touch_edit_request();
}
