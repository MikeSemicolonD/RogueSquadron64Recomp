#include "game/mod_content/level_environment.h"
#include "check.h"

using namespace rs64env;

int main()
{

    std::vector<std::string> errors;

    const Environment empty = parseEnvironment("{}", errors);

    CHECK(errors.empty());
    CHECK(empty.fogStart < 0.0f);
    CHECK(empty.fogReach == FogReach::Auto);
    CHECK(!empty.fogColor.set);
    CHECK(!empty.clouds);

    const Environment full = parseEnvironment(R"({ "fog": { "start": 0.3, "reach": "stock", "color": [10, 20, 30] },
        "clouds": { "wind": [1.5, -2], "altitude": -40, "tint": [200, 210, 220] } })", errors);

    CHECK(errors.empty());
    CHECK(full.fogStart == 0.3f);
    CHECK(full.fogReach == FogReach::Stock);
    CHECK(full.fogColor.set && full.fogColor.rgb[2] == 30);
    CHECK(full.clouds);
    CHECK(full.windX == 1.5f && full.windZ == -2.0f);
    CHECK(full.hasAltitude && full.altitude == -40.0f);
    CHECK(full.cloudTint.set && full.cloudTint.rgb[0] == 200);

    const Environment plainClouds = parseEnvironment(R"({ "clouds": {} })", errors);

    CHECK(errors.empty());
    CHECK(plainClouds.clouds && !plainClouds.hasAltitude && plainClouds.windX == 0.0f);

    parseEnvironment(R"({ "fog": { "start": 1.5, "reach": "sideways", "color": [300, 0, 0] } })", errors);
    CHECK(errors.size() == 3);

    errors.clear();
    parseEnvironment("not json", errors);
    CHECK(errors.size() == 1);

    errors.clear();

    const Environment bounded = parseEnvironment(R"({ "bounds": [0, 30, 12.5, 4] })", errors);

    CHECK(errors.empty());
    CHECK(bounded.hasBounds && bounded.bounds[1] == 30.0f && bounded.bounds[2] == 12.5f);
    CHECK(!empty.hasBounds);

    parseEnvironment(R"({ "bounds": [1, 2, 3] })", errors);
    parseEnvironment(R"({ "bounds": [1, -2, 3, 4] })", errors);
    CHECK(errors.size() == 2);

    // hmp lighting block at 0x30: a point light, a flagged sun, then the sun (Construction Yards' colour, its mission sun normalized).
    std::vector<uint8_t> hmp(0x30 + 0xC + 3 * 0x20, 0);

    const auto put32 = [&hmp](size_t o, uint32_t v) {

        for (int k = 0; k < 4; k++)
        {
            hmp[o + k] = (uint8_t)(v >> (24 - 8 * k));
        }
    };

    const auto putf = [&put32](size_t o, float f) {
        uint32_t bits;

        std::memcpy(&bits, &f, 4);
        put32(o, bits);
    };

    put32(0x20, 0x30);
    put32(0x30, 3);
    hmp[0x30 + 0x18] = 1;
    hmp[0x50 + 0x1B] = 1;
    putf(0x50 + 0xC, 255.0f);
    putf(0x70 + 0xC, 210.0f);
    putf(0x70 + 0x10, 125.0f);
    putf(0x70 + 0x14, 73.0f);

    float sun[3];

    CHECK(hmpSunColor(hmp.data(), hmp.size(), sun));
    CHECK(std::fabs(sun[0] - 0.8236f) < 0.002f && std::fabs(sun[1] - 0.4898f) < 0.002f && std::fabs(sun[2] - 0.2861f) < 0.002f);
    CHECK(!hmpSunColor(hmp.data(), 0x80, sun));
    CHECK(!hmpSunColor(nullptr, 0, sun));

    return 0;
}
