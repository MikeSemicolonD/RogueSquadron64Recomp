#include "../lib/rt64/src/hle/rt64_rs64_hud.h"
#include "check.h"
#include <cmath>
#include <cstdio>

std::atomic<float> g_rs64_hud_placement{ 0.0f };

using namespace rs64hud;

int main() {
    // Elements measured on Corellia (x = upper-left from the screen centre, 512-wide view).
    CHECK(classify(0x8037602Cu, -224.0f, 0x02400003u, 512.0f) == Left);
    CHECK(classify(0x8037611Cu, 121.6f, 0x02400013u, 512.0f) == Right);
    CHECK(classify(0x8037614Cu, 177.0f, 0x02400003u, 512.0f) == Right);
    CHECK(classify(0x8037614Cu, 40.0f, 0x02400003u, 512.0f) == None);
    // Crosshair rings stay where the aim puts them, even far off centre (level 6 samples).
    CHECK(classify(0x8010CA58u, -233.9f, 0x3u, 512.0f) == None);
    CHECK(classify(0x8010CA88u, 360.0f, 0x3u, 512.0f) == None);
    CHECK(classify(0x8010CCD0u, -275.5f, 0x3u, 512.0f) == None);
    // Text never moves, wherever it starts: the left-aligned subtitle and pause menu lines.
    CHECK(classify(0x8019C760u, -218.0f, 0x32u, 512.0f) == None);
    CHECK(classify(0x80376224u, -127.0f, 0x32u, 512.0f) == None);

    // The wipe panel spans the 0..447 scissor; a tall HUD piece does not.
    CHECK(spansHeight(0, 448 * 4, 0, 447 * 4));
    CHECK(!spansHeight(16 * 4, 428 * 4, 0, 447 * 4));

    // 16:9 over the 512x448 view's 1.45 source aspect: the full shift is half the extra width.
    const float a = (16.0f / 9.0f) / 1.45f;
    CHECK(std::fabs(shiftNative(512.0f, 1.0f, a) - 256.0f * (a - 1.0f)) < 1e-4f);
    CHECK(std::fabs(shiftNative(512.0f, 0.5f, a) - 128.0f * (a - 1.0f)) < 1e-4f);
    CHECK(shiftNative(512.0f, 0.0f, a) == 0.0f);
    CHECK(shiftNative(512.0f, 1.0f, 1.0f) == 0.0f);
    CHECK(shiftNative(512.0f, 1.0f, 0.8f) == 0.0f);

    CHECK(markerW0(Right) == 0x60000002u);
    CHECK((markerW0(Left) >> 24) == kMarkerOpcode);

    std::printf("hud_placement_test: OK\n");
    return 0;
}
