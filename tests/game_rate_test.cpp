#include "game/game_rate.h"
#include "check.h"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace rs64::rate;

static int count_ticks(State& s, const std::vector<float>& dts) {

    int n = 0;

    for (float dt : dts) {
        update(s, dt, true);
        n += s.tick ? 1 : 0;
    }

    return n;
}

int main() {

    const float k60 = 1.0f / 60.0f;
    const float k30 = 1.0f / 30.0f;

    // off_is_identity
    State off;
    update(off, k60, false);
    CHECK(off.scale == 1.0f);
    CHECK(off.tick);

    // sixty_ticks_every_other_frame
    State s60;
    const bool want[6] = { false, true, false, true, false, true };

    for (bool w : want) {
        update(s60, k60, true);
        CHECK(s60.tick == w);
        CHECK(std::fabs(s60.scale - 0.5f) < 1e-6f);
    }

    // thirty_on_matches_off
    State s30;

    for (int i = 0; i < 5; ++i) {
        update(s30, k30, true);
        CHECK(s30.tick);
        CHECK(std::fabs(s30.scale - 1.0f) < 1e-6f);
    }

    // uneven_dt_ticks
    State un;
    const std::vector<float> dts = { k60, 1.0f / 40.0f, k60, k30 };
    float sum = 0.0f;

    for (float dt : dts) {
        sum += dt;
    }

    CHECK(count_ticks(un, dts) == (int)std::floor(sum * 30.0f + 1e-4f));

    // zero_dt
    State z;
    update(z, 0.0f, true);
    CHECK(z.scale == 0.25f);
    CHECK(!z.tick);

    // toggle_resets_accumulator
    State t;
    update(t, k60, true);
    update(t, k60, false);
    update(t, k60, true);
    CHECK(!t.tick);

    // drag_half_step
    CHECK(std::fabs(drag(0.9f, 0.5f) * drag(0.9f, 0.5f) - 0.9f) < 1e-6f);
    CHECK(drag(0.9f, 1.0f) == 0.9f);
    CHECK(drag(0.0f, 0.5f) == 0.0f);
    CHECK(drag(-0.5f, 0.5f) == -0.5f);

    std::printf("game_rate_test: ok\n");

    return 0;
}
