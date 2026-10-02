#include "../lib/rt64/src/hle/rt64_rs64_crosshair.h"
#include "check.h"
#include <cstdio>

using namespace rs64xhair;

int main() {
    // Outer ring: 32x32 texels at dsdx 0x280 / dtdy 0x224 = 51.2 x 59.6 px.
    const float ow = drawnSize(32, 0x280), oh = drawnSize(32, 0x224);
    const float iw = drawnSize(16, 0x280), ih = drawnSize(16, 0x224);
    CHECK(ow > 51.1f && ow < 51.3f);
    CHECK(oh > 59.4f && oh < 59.8f);
    CHECK(drawnSize(32, 0) == 0.0f);

    const Rect clip{ 0.0f, 0.0f, 640.0f, 480.0f };

    // Samples measured on level 6 (texrect corners vs the HUD element position).
    const Element outer{ -233.9f, -144.2f, 3 };
    CHECK(matches(outer, Rect{ 22.0f, 79.75f, 73.0f, 139.25f }, ow, oh, clip));
    CHECK(matches(Element{ -253.9f, -161.5f, 3 }, Rect{ 2.0f, 62.25f, 27.5f, 92.0f }, iw, ih, clip));

    // A ring partly off the left edge: the upper-left clamps to the clip edge and only the lower-right matches.
    CHECK(matches(Element{ -275.5f, -146.6f, 3 }, Rect{ 0.0f, 77.25f, 31.5f, 136.75f }, ow, oh, clip));
    // The same clamp at the right edge.
    CHECK(matches(Element{ 360.0f, -144.2f, 3 }, Rect{ 616.0f, 79.75f, 640.0f, 139.25f }, ow, oh, clip));

    // Hidden elements and other HUD rects never match.
    CHECK(!matches(Element{ -233.9f, -144.2f, 0 }, Rect{ 22.0f, 79.75f, 73.0f, 139.25f }, ow, oh, clip));
    CHECK(!matches(outer, Rect{ 433.0f, 20.0f, 545.0f, 52.0f }, ow, oh, clip));
    CHECK(!matches(outer, Rect{ 30.0f, 79.75f, 81.0f, 139.25f }, ow, oh, clip));
    // A shorter rect away from the clip edge is not a clamped ring.
    CHECK(!matches(outer, Rect{ 22.0f, 79.75f, 50.0f, 139.25f }, ow, oh, clip));
    CHECK(!matches(outer, Rect{ 30.0f, 90.0f, 60.0f, 120.0f }, ow, oh, clip));

    // ringOf picks the element whose size and position fit.
    const Element els[2] = { outer, Element{ -197.9f, -144.2f, 3 } };
    const TexSize outerTex{ 32, 32 }, innerTex{ 16, 16 };
    CHECK(ringOf(els, Rect{ 22.0f, 79.75f, 73.0f, 139.25f }, outerTex, 0x280, 0x224, clip) == 0);
    CHECK(ringOf(els, Rect{ 58.0f, 79.75f, 83.5f, 109.5f }, innerTex, 0x280, 0x224, clip) == 1);
    CHECK(ringOf(els, Rect{ 22.0f, 79.75f, 73.0f, 139.25f }, innerTex, 0x280, 0x224, clip) == -1);
    CHECK(ringOf(els, Rect{ 58.0f, 79.75f, 83.5f, 109.5f }, outerTex, 0x280, 0x224, clip) == -1);
    CHECK(ringOf(els, Rect{ 22.0f, 79.75f, 73.0f, 139.25f }, outerTex, 0, 0x224, clip) == -1);

    std::printf("crosshair_match_test: OK\n");
    return 0;
}
