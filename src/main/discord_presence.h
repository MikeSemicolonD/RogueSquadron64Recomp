#pragma once
// Discord Rich Presence while the discord-status mod is enabled: the mission and craft being flown, cleared when the game closes.
// Desktop talks to the Discord client's local IPC socket. Android loads libdiscord_bridge.so (discord_bridge.cpp), the only binary linked with the closed Discord Social SDK.
#include <cstdint>

namespace rs64::discord {

// Each present, after the game-state poll; starts the IPC thread or loads the bridge the first time the mod is on.
void poll(const uint8_t* rdram);

}

// What the game hands libdiscord_bridge.so: rs64_discord_set(status), or rs64_discord_set(nullptr) to clear; rs64_discord_pump() every present.
struct Rs64DiscordStatus
{
    uint64_t app_id;
    const char* name;
    const char* details;
    const char* state;
    const char* large_image;
    const char* large_text;
    int64_t start;
    int players;
    int max_players;
};
