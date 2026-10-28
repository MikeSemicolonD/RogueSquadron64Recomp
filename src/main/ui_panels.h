#pragma once

#include <string>

// ImGui panels drawn over the game: the controls/rebind window (F6), the mods page, the touch overlay and RT64's dev inspector.
namespace rs64::app {
    void draw_dev_ui();
    // True while an ImGui window takes the keyboard (set each ImGui frame), so typing there doesn't also drive the game.
    bool imgui_wants_keyboard();
    // A bottom-right message for `seconds`, from any thread; one with the same key replaces the last. Says why the game freezes during a capture or dump.
    void notify(const char* key, const std::string& text, float seconds);
}
