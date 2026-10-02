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
// Whether a mod page (a list menu the state classifier does not name) is on screen; safe from any thread.
int rs64_menu_page_shown(void);
}
