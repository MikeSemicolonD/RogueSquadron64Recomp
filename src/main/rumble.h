#ifndef RS64_RUMBLE_H
#define RS64_RUMBLE_H

#include <cstdint>

#include "input_bindings.h"

// The game runs its own Rumble Pak effects and pulses the motor on/off (PWM) through
// osMotorStart/osMotorStop. motor() records those edges; tick() turns the recent on-fraction
// into a strength and applies the per-effect toggles, damage scaling, and death-spiral sustain.
namespace rs64::rumble {

struct Output {
    float lo = 0.0f;   // low-frequency (heavy) motor, [0,1]
    float hi = 0.0f;   // high-frequency (light) motor, [0,1]
};

// From __osMotorAccess (game SI thread).
void motor(int channel, bool on);

// Call ~60 Hz. rdram = raw host RDRAM (words in host order), null if not up yet.
// in_mission gates the RDRAM-derived layers (effect ids, health, death spiral).
Output tick(const rs64::input::RumbleConfig& cfg, const uint8_t* rdram, bool in_mission);

// Player 0 craft, for the lightbar. Only meaningful in a mission; valid = false when health is unreadable.
struct CraftStatus {
    bool  valid  = false;
    float health = 1.0f;   // health / max health, [0,1]
    bool  spiral = false;  // death spiral in progress
    bool  dead   = false;  // crashed or destroyed
};
CraftStatus craft_status(const uint8_t* rdram);

} // namespace rs64::rumble

#endif
