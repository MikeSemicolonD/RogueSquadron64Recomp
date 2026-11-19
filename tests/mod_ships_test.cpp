#include "game/mod_content/mod_ships.h"
#include "check.h"
#include <cstdio>

using namespace rs64ships;

int main() {
    // parses_ships_json
    {
        std::vector<std::string> errors;
        const auto s = parseShips("my-mod", R"({ "ships": [
            { "id": "redwing", "name": "RED WING", "template": "xwing", "stop": [0.86, 0.85] },
            { "id": "bluefin", "name": "BLUEFIN", "template": "awing", "stop": [0.1, 0.2] } ] })", errors);
        CHECK(errors.empty());
        CHECK(s.size() == 2);
        CHECK(s[0].mod == "my-mod" && s[0].id == "redwing" && s[0].name == "RED WING" && s[0].tmpl == 0);
        CHECK(s[0].stop[0] > 0.859f && s[0].stop[0] < 0.861f && s[0].stop[1] > 0.849f && s[0].stop[1] < 0.851f);
        CHECK(s[1].tmpl == 2);
    }
    // rejects_bad_entries: ids are short plain names, the template a craft name, the stop two numbers
    {
        std::vector<std::string> errors;
        const auto s = parseShips("m", R"({ "ships": [
            { "id": "toolongname", "name": "A", "template": "xwing", "stop": [0, 0] },
            { "id": "Bad", "name": "A", "template": "xwing", "stop": [0, 0] },
            { "id": "ok", "name": "", "template": "xwing", "stop": [0, 0] },
            { "id": "ok", "name": "A", "template": "zwing", "stop": [0, 0] },
            { "id": "ok", "name": "A", "template": "xwing", "stop": [0] },
            { "id": "ok", "name": "A", "template": "tie", "stop": [0, 0] },
            { "id": "ok", "name": "A", "template": "t16", "stop": [0, 0] },
            { "id": "ship_1", "name": "A", "template": "xwing", "stop": [0, 0] },
            { "id": "ship2", "name": "A", "template": "xwing", "stop": [0, 0] },
            { "id": "good", "name": "A", "template": "naboo", "stop": [0.5, 0.5] } ] })", errors);
        CHECK(errors.size() == 9);
        CHECK(s.size() == 1 && s[0].id == "good" && s[0].tmpl == 8);
        CHECK(parseShips("m", "[]", errors).empty());
    }
    // behaviours: unlisted = the template's own, a list replaces them, unknown names are reported
    {
        std::vector<std::string> errors;
        const auto s = parseShips("m", R"({ "ships": [
            { "id": "a", "name": "A", "template": "awing", "stop": [0, 0] },
            { "id": "b", "name": "B", "template": "ywing", "stop": [0, 0], "behaviours": ["aim", "sfoils", "warp"] },
            { "id": "c", "name": "C", "template": "xwing", "stop": [0, 0], "behaviours": [] },
            { "id": "d", "name": "D", "template": "vwing", "stop": [0, 0], "behaviours": ["boost", "sfoils"] } ] })", errors);
        CHECK(s.size() == 4 && errors.size() == 2);
        CHECK(hasBehaviour(s[3], "sfoils") && !hasBehaviour(s[3], "boost"));
        CHECK(hasBehaviour(s[0], "aim") && !hasBehaviour(s[0], "sfoils") && !hasBehaviour(s[0], "boost"));
        CHECK(hasBehaviour(s[1], "aim") && hasBehaviour(s[1], "sfoils") && !hasBehaviour(s[1], "boost") && !hasBehaviour(s[1], "warp"));
        CHECK(!hasBehaviour(s[2], "sfoils") && !hasBehaviour(s[2], "aim"));
    }
    // special: the presets from behaviours, a custom block in their place, values clamped
    {
        std::vector<std::string> errors;
        const auto s = parseShips("m", R"({ "ships": [
            { "id": "a", "name": "A", "template": "xwing", "stop": [0, 0] },
            { "id": "b", "name": "B", "template": "vwing", "stop": [0, 0] },
            { "id": "c", "name": "C", "template": "ywing", "stop": [0, 0] },
            { "id": "d", "name": "D", "template": "xwing", "stop": [0, 0], "special": { "mode": "timed", "speed": 9, "seconds": 3, "cooldown": 2, "guns": false } },
            { "id": "e", "name": "E", "template": "awing", "stop": [0, 0], "special": { "mode": "warp" } } ] })", errors);
        CHECK(s.size() == 5 && errors.size() == 1);
        const auto a = specialOf(s[0]);
        const auto b = specialOf(s[1]);
        const auto d = specialOf(s[3]);
        const auto e = specialOf(s[4]);
        CHECK(a.mode == SpecialMode::Toggle && !a.guns && !a.custom && a.speed > 1.49f && a.speed < 1.51f);
        CHECK(b.mode == SpecialMode::Timed && b.guns && b.seconds == 6.0f && b.cooldown == 6.0f);
        CHECK(specialOf(s[2]).mode == SpecialMode::None);
        CHECK(d.custom && d.mode == SpecialMode::Timed && d.speed == 3.0f && d.seconds == 3.0f && d.cooldown == 2.0f && !d.guns);
        CHECK(e.custom && e.mode == SpecialMode::Toggle && !e.guns);
    }
    // turning_order: by u round the route, as the stock stops (0..6 in their u order wrap 2 -> 3), ships among them, locked ones skipped
    {
        const std::vector<StopAt> stock = { { 0, 0.828f }, { 1, 0.8968f }, { 2, 0.9953f }, { 3, 0.1377f }, { 4, 0.3001f }, { 5, 0.5486f }, { 6, 0.696f } };
        const auto none = [](int) { return false; };

        for (int s = 0; s <= 6; s++) CHECK(nextStop(s, 1, stock, none) == (s + 1) % 7 && nextStop(s, -1, stock, none) == (s + 6) % 7);

        auto with = stock;
        with.push_back({ 8, 0.8625f });
        with.push_back({ 9, 0.08f });
        CHECK(nextStop(0, 1, with, none) == 8 && nextStop(8, 1, with, none) == 1 && nextStop(8, -1, with, none) == 0);
        CHECK(nextStop(2, 1, with, none) == 9 && nextStop(9, 1, with, none) == 3 && nextStop(3, -1, with, none) == 9);
        CHECK(nextStop(8, 1, with, [](int s) { return s == 1; }) == 2);
        CHECK(nextStop(7, 1, with, none) == -1);
    }

    // ship_assets: the template's mission files -> the ship's own where the mod ships them, "1" names fall back to the plain file
    {
        Ship s;
        s.id = "redwing";
        s.tmpl = 0;
        const std::set<std::string> have = { "pl_crafts/redwing_HOB", "pl_crafts/redwing_HMT", "pl_crafts/c_redwing_HOB", "ov/1v_redwing" };
        CHECK(shipAsset("pl_crafts/xwing_HOB", s, have) == "pl_crafts/redwing_HOB");
        CHECK(shipAsset("pl_crafts/1wing_HMT", s, have) == "pl_crafts/1edwing_HMT");
        CHECK(shipAsset("pl_crafts/c_xwg_HOB", s, have) == "pl_crafts/c_redwing_HOB");
        CHECK(shipAsset("pl_crafts/c_xwg_HMT", s, have).empty());
        CHECK(shipAsset("ov/1v_xwing", s, have) == "ov/1v_redwing");
        CHECK(shipAsset("ov/ov_xwing", s, have).empty());
        CHECK(shipAsset("pl_crafts/xwingx_HOB", s, have).empty() && shipAsset("pl_crafts/ywing_HOB", s, have).empty());
        CHECK(shipAsset("level/lv_1/dat", s, have).empty());
    }

    std::printf("mod_ships_test: ok\n");
    return 0;
}
