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
    extern std::atomic<bool> g_mouse_capture;
    extern std::vector<OpenJoy> g_open_joys;
    extern std::vector<SDL_Joystick*> g_joy_by_dev;
    extern std::atomic<float> g_throttle;
    extern std::atomic<bool> g_report_rumble_pak;
    extern bool g_fullscreen_applied;
    extern rs64::touch::Engine g_touch;
    extern std::mutex g_touch_mtx;

    void poll_input();
    bool get_n64_input(int controller_num, uint16_t* buttons, float* x, float* y);
    void set_rumble(int controller_num, bool on);
    ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num);
    void rumble_thread_main();
    int joy_dev_locked(const OpenJoy& oj);
    void rebind_open_joysticks_locked();

    uint8_t* touch_rdram();
    rs64::touch::Context touch_context();
    rs64::touch::AccountScreen touch_account_screen();
    bool touch_menu_hits_allowed();
    rs64::touch::TapAction touch_menu_tap(float x, float y);
    bool mouse_menu_context(rs64::touch::Context ctx);
    void window_size(float* w, float* h);
}
