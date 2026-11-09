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

    return 0;
}
