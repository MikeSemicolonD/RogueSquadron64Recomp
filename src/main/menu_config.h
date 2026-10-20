#pragma once
#include <cstdint>

// Host entry points for the custom front-end menu entries, called from the
// [[patches.hook]] blocks in rogue_squadron.toml. Behavior is configured by
// roguesq_menu.json next to the executable (see menu_config.cpp for the schema).

extern "C" {
// Append the custom entries to gCurrentMenuData when the MAIN_MENU is (re)built.
void rs64_menu_install_main(uint8_t* rdram);
// Handle a confirm press on a custom MAIN_MENU entry.
void rs64_menu_confirm_main(uint8_t* rdram);
uint32_t rs64_menu_confirms();
// Whether a mod page (a list menu the state classifier does not name) is on screen; safe from any thread.
int rs64_menu_page_shown(void);
// Volume channel byte of a mod Sound Settings slider at entry `slot`; -1 for a native entry.
int rs64_menu_slider_channel(int slot);
// 1 while a Sound Settings volume bar is open for editing (the highlight must stay on it).
int rs64_menu_slider_editing(void);
// Hover moved a highlight: the game thread plays the menu's cursor sound (pause = 1 for the pause menu).
void rs64_menu_hover_sound(int pause);
// A tap/click at fraction f across a menu entry's text (front-end slot, or pause record nextMenu); a text slider's next confirm uses it.
void rs64_menu_slider_click(int key, float f);
}
