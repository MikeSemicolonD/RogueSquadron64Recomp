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

    // A ring past the left edge is moved whole to the edge.
    CHECK(matches(Element{ -275.5f, -146.6f, 3 }, Rect{ 0.0f, 77.25f, 51.25f, 136.75f }, ow, oh, clip));
    // The same at the right edge, and when it is entirely outside the frame.
    CHECK(matches(Element{ 360.0f, -144.2f, 3 }, Rect{ 588.75f, 79.75f, 640.0f, 139.25f }, ow, oh, clip));
    CHECK(matches(Element{ 500.0f, -144.2f, 3 }, Rect{ 588.75f, 79.75f, 640.0f, 139.25f }, ow, oh, clip));

    // A moved ring is known by its tile, wherever the element's position has got to by the time it is drawn.
    const Element tiles[kElementCount] = { outer, Element{ 900.0f, -900.0f, 3 }, Element{ 0.0f, 0.0f, 0 } };
    CHECK(ringOfTile(tiles, TexSize{ 32, 32 }) == 0);
    CHECK(ringOfTile(tiles, TexSize{ 16, 16 }) == 1);
    CHECK(ringOfTile(tiles, TexSize{ 32, 20 }) == -1);
    CHECK(ringOfTile(tiles, TexSize{ 64, 64 }) == -1);

    // Hidden elements and other HUD rects never match.
    CHECK(!matches(Element{ -233.9f, -144.2f, 0 }, Rect{ 22.0f, 79.75f, 73.0f, 139.25f }, ow, oh, clip));
    CHECK(!matches(outer, Rect{ 433.0f, 20.0f, 545.0f, 52.0f }, ow, oh, clip));
    CHECK(!matches(outer, Rect{ 30.0f, 79.75f, 81.0f, 139.25f }, ow, oh, clip));
    // A shorter rect away from the clip edge is not a clamped ring.
    CHECK(!matches(outer, Rect{ 22.0f, 79.75f, 50.0f, 139.25f }, ow, oh, clip));
    CHECK(!matches(outer, Rect{ 30.0f, 90.0f, 60.0f, 120.0f }, ow, oh, clip));

    // ringOf picks the element whose size and position fit.
    const Element els[kElementCount] = { outer, Element{ -197.9f, -144.2f, 3 }, Element{ 0.0f, 0.0f, 0 } };
    const TexSize outerTex{ 32, 32 }, innerTex{ 16, 16 };
    CHECK(ringOf(els, Rect{ 22.0f, 79.75f, 73.0f, 139.25f }, outerTex, 0x280, 0x224, clip) == 0);
    CHECK(ringOf(els, Rect{ 58.0f, 79.75f, 83.5f, 109.5f }, innerTex, 0x280, 0x224, clip) == 1);
    CHECK(ringOf(els, Rect{ 22.0f, 79.75f, 73.0f, 139.25f }, innerTex, 0x280, 0x224, clip) == -1);
    CHECK(ringOf(els, Rect{ 58.0f, 79.75f, 83.5f, 109.5f }, outerTex, 0x280, 0x224, clip) == -1);
    CHECK(ringOf(els, Rect{ 22.0f, 79.75f, 73.0f, 139.25f }, outerTex, 0, 0x224, clip) == -1);

    // Y-wing bomb sight (level 13): a 32x20 tile drawn 32 x 37.25 px, and moved up to the bottom edge.
    const Element all[kElementCount] = { outer, Element{ -197.9f, -144.2f, 3 }, Element{ -3.12f, 21.17f, 3 } };
    const TexSize bombTex{ 32, 20 };
    CHECK(ringOf(all, Rect{ 252.75f, 245.0f, 284.75f, 282.25f }, bombTex, 0x280, 0x224, clip) == 2);
    const Element clamped[kElementCount] = { outer, Element{ -197.9f, -144.2f, 3 }, Element{ -34.17f, 206.84f, 3 } };
    CHECK(ringOf(clamped, Rect{ 221.75f, 410.75f, 253.75f, 448.0f }, bombTex, 0x280, 0x224, Rect{ 0.0f, 0.0f, 640.0f, 448.0f }) == 2);
    // Hidden bomb sight: no match.
    const Element hidden[kElementCount] = { outer, Element{ -197.9f, -144.2f, 3 }, Element{ -3.12f, 21.17f, 0 } };
    CHECK(ringOf(hidden, Rect{ 252.75f, 245.0f, 284.75f, 282.25f }, bombTex, 0x280, 0x224, clip) == -1);

    // Results-screen iris: a 64x64 tile whose corner sits on a stale bomb-sight element at the screen centre.
    const Element stale[kElementCount] = { outer, Element{ -197.9f, -144.2f, 3 }, Element{ -0.5f, -0.5f, 3 } };
    CHECK(ringOf(stale, Rect{ 255.5f, 223.5f, 295.5f, 273.5f }, TexSize{ 64, 64 }, 1638, 1311, clip) == -1);

    std::printf("crosshair_match_test: OK\n");
    return 0;
}
