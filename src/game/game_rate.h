#pragma once
// Mission frame rate core: a frame's dt becomes a step scale (1 at 30 fps) and a 30 Hz tick for game logic that counts frames.
#include <algorithm>
#include <cmath>

namespace rs64::rate {

struct State {
    float acc = 0.0f;
    float scale = 1.0f;
    bool tick = true;
    bool was_on = false;
};

// Off: every frame is a full 30 Hz step. On: scale = dt in 30 Hz steps, tick fires once per accumulated step.
inline void update(State& s, float dt, bool on)
{

    if (!on) {
        s = State{};
        return;
    }

    if (!s.was_on) {
        s.acc = 0.0f;
    }

    const float steps = dt * 30.0f;

    s.was_on = true;
    s.scale = std::clamp(steps, 0.25f, 3.0f);
    s.acc = std::min(s.acc + steps, 2.0f);
    s.tick = s.acc >= 1.0f - 1e-4f;

    if (s.tick) {
        s.acc -= 1.0f;
    }
}

// A per-frame multiplier k applied over `scale` 30 Hz steps.
inline float drag(float k, float scale) {
    return k > 0.0f ? std::pow(k, scale) : k;
}
}
