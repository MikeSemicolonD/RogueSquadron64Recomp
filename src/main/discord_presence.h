#pragma once
// Discord Rich Presence through the Discord client's local IPC socket while the discord-status mod is enabled: the mission and craft being flown, cleared when the game closes.
#include <cstdint>

namespace rs64::discord {

// Each present, after the game-state poll; starts the IPC thread on first use. A no-op on Android.
void poll(const uint8_t* rdram);

}
