#include "game/mod_levels.h"
#include <set>
#include "check.h"
#include <cstdio>

using namespace rs64levels;

int main() {
    // parses_levels_json
    {
        std::vector<std::string> errors;
        const auto lv = parseModLevels("my-mod", R"({ "levels": [
            { "id": "dune_sea", "title": "DUNE SEA", "description": "Hold the line.", "template": 0, "folder": "dune_sea" },
            { "id": "ice", "title": "ICE", "template": 3, "folder": "ice_world", "voice": 640 } ] })", errors);
        CHECK(errors.empty());
        CHECK(lv.size() == 2);
        CHECK(lv[0].mod == "my-mod" && lv[0].id == "dune_sea" && lv[0].title == "DUNE SEA" && lv[0].description == "Hold the line.");
        CHECK(lv[0].tmpl == 0 && lv[0].folder == "dune_sea" && lv[0].voice == kVoiceNone);
        CHECK(lv[1].tmpl == 3 && lv[1].voice == 640 && lv[1].description.empty());
    }
    // rejects_bad_templates_and_folders: only playable stock levels; folders are one plain name
    {
        std::vector<std::string> errors;
        const auto lv = parseModLevels("m", R"({ "levels": [
            { "id": "a", "title": "A", "template": 19, "folder": "a" },
            { "id": "b", "title": "B", "template": 0, "folder": "../b" },
            { "id": "c", "title": "C", "template": 0, "folder": "x/y" },
            { "id": "d", "title": "D", "template": 0, "folder": "" },
            { "id": "e", "title": "E", "template": 1, "folder": "e" },
            { "id": "f", "title": "F", "template": 1, "folder": "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" } ] })", errors);
        CHECK(lv.size() == 1 && lv[0].id == "e");
        CHECK(errors.size() == 5);
        CHECK(parseModLevels("m", "not json", errors).empty());
    }
    // remaps_template_files_the_mod_ships: a lookup of the template's level file or intro/extro goes to the mod level's own copy, if it has one
    {
        ModLevel l;
        l.folder = "dune_sea";
        l.tmpl = 0;
        const std::set<std::string> have = { "level/dune_sea/dat", "level/dune_sea/hmp", "cuts/dune_sea_intro" };
        CHECK(remapPath("level/lv_0/dat", l, have) == "level/dune_sea/dat");
        CHECK(remapPath("level/lv_0/gvm", l, have).empty());
        CHECK(remapPath("cuts/id0_intro", l, have) == "cuts/dune_sea_intro");
        CHECK(remapPath("cuts/id0_extro", l, have).empty());
        CHECK(remapPath("level/lv_1/dat", l, have).empty());
        CHECK(remapPath("cuts/id10_intro", l, have).empty());
        l.tmpl = 0x0A;
        CHECK(remapPath("level/lv_a/dat", l, { "level/dune_sea/dat" }) == "level/dune_sea/dat");
        CHECK(remapPath("cuts/id10_intro", l, { "cuts/dune_sea_intro" }) == "cuts/dune_sea_intro");
    }
    // layout_follows_stock_and_caps_at_0x1e: mod entries start after the highest stock entry; the selector mask allows index 0x1E at most
    {
        CHECK(layout(0x0F, 3).base == 0x10 && layout(0x0F, 3).count == 3);
        CHECK(layout(0x12, 20).base == 0x13 && layout(0x12, 20).count == 0x1E - 0x12);
        CHECK(layout(0x05, 0).count == 0);
        CHECK(layout(0x1E, 2).count == 0);
    }
    // medal_thresholds_pack_like_medal_info: three 0xC records (bronze, silver, gold), big-endian f32 minutes, u16 kills/accuracy/friendlies; bonus is not the mod's (the template's secret pickups)
    {
        std::vector<std::string> errors;
        const auto lv = parseModLevels("m", R"({ "levels": [ { "id": "a", "title": "A", "template": 0, "folder": "a", "medals": {
            "bronze": { "time": 12.5, "kills": 10, "accuracy": 20, "friendlies": 1, "bonus": 0 },
            "silver": { "time": 9, "kills": 20, "accuracy": 35, "friendlies": 2, "bonus": 0 },
            "gold":   { "time": 6, "kills": 30, "accuracy": 50, "friendlies": 3, "bonus": 1 } } },
            { "id": "b", "title": "B", "template": 0, "folder": "b", "medals": { "gold": { "time": 1 } } } ] })", errors);
        CHECK(errors.size() == 1 && lv.size() == 2);
        CHECK(lv[0].hasMedals && !lv[1].hasMedals);
        const auto r = medalRecords(lv[0]);
        CHECK(r.size() == 0x24);
        CHECK(r[0] == 0x41 && r[1] == 0x48 && r[2] == 0 && r[3] == 0);
        CHECK(r[4] == 0 && r[5] == 10 && r[6] == 0 && r[7] == 20 && r[8] == 0 && r[9] == 1 && r[10] == 0 && r[11] == 0);
        CHECK(r[0x18] == 0x40 && r[0x19] == 0xC0 && r[0x1D] == 30 && r[0x1F] == 50 && r[0x21] == 3 && r[0x23] == 0);
    }
    // approved_craft_is_a_mask_in_craft_order: X-wing, Y-wing, A-wing, V-wing, speeder, Falcon, TIE interceptor, T-16, Naboo starfighter
    {
        std::vector<std::string> errors;
        const auto lv = parseModLevels("m", R"({ "levels": [
            { "id": "a", "title": "A", "template": 0, "folder": "a", "craft": ["xwing", "awing", "falcon"] },
            { "id": "b", "title": "B", "template": 0, "folder": "b" },
            { "id": "c", "title": "C", "template": 0, "folder": "c", "craft": ["xwing", "starship"] } ] })", errors);
        CHECK(lv.size() == 3 && errors.size() == 1);
        CHECK(lv[0].craftMask == ((1 << 0) | (1 << 2) | (1 << 5)));
        CHECK(lv[1].craftMask == kTemplateCraft);
        CHECK(lv[2].craftMask == 1);
        CHECK(craftMaskOf({ "ywing", "vwing", "speeder", "tie", "t16", "naboo" }) == ((1 << 1) | (1 << 3) | (1 << 4) | (1 << 6) | (1 << 7) | (1 << 8)));
    }
    // medal_store_keeps_the_best_per_mod_level: a separate record per mod + level id, kept when a mod is disabled
    {
        MedalStore s;
        CHECK(s.best("m/a") == 0);
        CHECK(s.record("m/a", 2) && s.best("m/a") == 2);
        CHECK(!s.record("m/a", 1) && s.best("m/a") == 2);
        CHECK(s.record("m/a", 3));
        MedalStore t;
        t.load(s.save() + "");
        CHECK(t.best("m/a") == 3 && t.best("other/x") == 0);
        t.load("garbage");
        CHECK(t.best("m/a") == 0);
    }
    // opening_crawl_is_the_template_s_none_or_its_own: one crawl string ("*}" chapter, "*{" title, body) in the starwars50 font's characters
    {
        std::vector<std::string> errors;
        const auto lv = parseModLevels("m", R"({ "levels": [
            { "id": "a", "title": "A", "template": 0, "folder": "a" },
            { "id": "b", "title": "B", "template": 0, "folder": "b", "crawl": false },
            { "id": "c", "title": "C", "template": 1, "folder": "c", "crawl": { "chapter": "Chapter V", "title": "The \"Dune\" Sea", "body": "Rebels & spies.\nGo!", "planet": 2 } },
            { "id": "d", "title": "D", "template": 1, "folder": "d", "crawl": { "title": "X", "planet": 9 } } ] })", errors);
        CHECK(errors.empty() && lv.size() == 4);
        CHECK(lv[0].crawl == kCrawlTemplate && lv[1].crawl == kCrawlNone && lv[2].crawl == 2 && lv[3].crawl == 0);
        CHECK(crawlString(lv[2]) == "*}Chapter V\n\n*{The Dune Sea\n\nRebels  spies.\nGo!");
        CHECK(crawlString(lv[3]) == "*{X\n\n");
    }
    // objectives_parse_and_reject: at most 4, at least one goal that completes the mission, valid counter/flag indices
    {
        std::vector<std::string> errors;
        const auto lv = parseModLevels("m", R"({ "levels": [
            { "id": "a", "title": "A", "template": 1, "folder": "a", "objectives": [
                { "text": "Destroy the walkers", "goal": "destroy", "counter": 80, "count": 6 },
                { "text": "Reach the base", "goal": "reach", "flag": 90 },
                { "text": "Protect the shuttle", "goal": "protect", "flag": 91 },
                { "text": "Hurry", "goal": "timelimit", "seconds": 300 } ] },
            { "id": "b", "title": "B", "template": 1, "folder": "b", "objectives": [ { "text": "Keep it alive", "goal": "protect", "flag": 5 } ] },
            { "id": "c", "title": "C", "template": 1, "folder": "c", "objectives": [ { "text": "X", "goal": "destroy", "counter": 0, "count": 1 }, { "text": "Y", "goal": "fly", "seconds": 1 } ] } ] })", errors);
        CHECK(lv.size() == 3 && errors.size() == 3);
        CHECK(lv[0].objectives.size() == 4 && lv[0].objectives[0].goal == Goal::Destroy && lv[0].objectives[0].index == 80 && lv[0].objectives[0].amount == 6);
        CHECK(lv[0].objectives[3].goal == Goal::TimeLimit && lv[0].objectives[3].seconds == 300.0f);
        CHECK(lv[1].objectives.empty() && lv[2].objectives.empty());
    }
    // objective_callouts: a voice line (stock id, -1 = none) and a DAT camera to cut to when it completes (or fails, for protect / timelimit)
    {
        std::vector<std::string> errors;
        const auto l = parseModLevels("m", R"({ "levels": [ { "id": "a", "title": "A", "template": 1, "folder": "a", "objectives": [
            { "text": "D", "goal": "destroy", "counter": 3, "count": 1, "voice": 93, "camera": "Camera_01", "recording": "obj_a1" },
            { "text": "P", "goal": "protect", "flag": 5, "voice": 80, "pauseVoice": 704, "pauseRecording": "pause_b2" },
            { "text": "R", "goal": "reach", "flag": 4 } ] } ] })", errors)[0];
        CHECK(errors.empty() && l.objectives.size() == 3);
        CHECK(l.objectives[0].voice == 93 && l.objectives[0].camera == "Camera_01" && l.objectives[0].recording == "obj_a1");
        CHECK(l.objectives[1].voice == 80 && l.objectives[1].camera.empty());
        // The pause screen reads each line aloud: its own voice (stock id or recording), none by default.
        CHECK(l.objectives[1].pauseVoice == 704 && l.objectives[1].pauseRecording == "pause_b2" && l.objectives[0].pauseVoice == kVoiceNone);
        CHECK(l.objectives[2].voice == kVoiceNone);
        std::vector<int32_t> counts(128);
        std::vector<uint8_t> flags(128);
        flags[4] = 1;
        const auto st = evaluateObjectives(l.objectives, counts.data(), flags.data(), 1.0f);
        CHECK(st.fail && st.failed == 2 && st.done == 0);
    }
    // objectives_evaluate: lines go green as they complete; success when every completing goal is done, fail on a protect flag or the time limit
    {
        std::vector<std::string> errors;
        const auto l = parseModLevels("m", R"({ "levels": [ { "id": "a", "title": "A", "template": 1, "folder": "a", "objectives": [
            { "text": "D", "goal": "destroy", "counter": 3, "count": 2 }, { "text": "R", "goal": "reach", "flag": 4 },
            { "text": "P", "goal": "protect", "flag": 5 }, { "text": "T", "goal": "timelimit", "seconds": 60 } ] } ] })", errors)[0];
        std::vector<int32_t> counts(128);
        std::vector<uint8_t> flags(128);
        auto st = evaluateObjectives(l.objectives, counts.data(), flags.data(), 10.0f);
        CHECK(st.done == 0 && !st.success && !st.fail);
        counts[2] = 2;
        st = evaluateObjectives(l.objectives, counts.data(), flags.data(), 20.0f);
        CHECK(st.done == 1 && !st.success);
        flags[3] = 1;
        st = evaluateObjectives(l.objectives, counts.data(), flags.data(), 30.0f);
        CHECK(st.success && st.done == 0xF && !st.fail);
        flags[4] = 1;
        CHECK(evaluateObjectives(l.objectives, counts.data(), flags.data(), 30.0f).fail);
        flags[4] = 0;
        counts[2] = 0;
        CHECK(evaluateObjectives(l.objectives, counts.data(), flags.data(), 61.0f).fail);
        const auto s = parseModLevels("m", R"({ "levels": [ { "id": "s", "title": "S", "template": 1, "folder": "s", "objectives": [ { "text": "Hold", "goal": "survive", "seconds": 90 } ] } ] })", errors)[0];
        CHECK(!evaluateObjectives(s.objectives, counts.data(), flags.data(), 89.0f).success);
        CHECK(evaluateObjectives(s.objectives, counts.data(), flags.data(), 90.0f).success);
    }
    // starting_song: a song id from the game's music cue table (0-255), the template's when left out or out of range
    {
        std::vector<std::string> errors;
        const auto lv = parseModLevels("m", R"({ "levels": [
            { "id": "a", "title": "A", "template": 1, "folder": "a", "music": 23 },
            { "id": "b", "title": "B", "template": 1, "folder": "b" },
            { "id": "c", "title": "C", "template": 1, "folder": "c", "music": 300 } ] })", errors);
        CHECK(lv.size() == 3 && errors.size() == 1);
        CHECK(lv[0].music == 23 && lv[1].music == kMusicTemplate && lv[2].music == kMusicTemplate);
    }
    // menu_text_is_uppercase: the SELECT LEVEL fonts have no lowercase letters (stock titles and descriptions are all caps)
    CHECK(menuText("Strike the city before dawn.\nGo.") == "STRIKE THE CITY BEFORE DAWN.\nGO.");
    // menu_text_drops_what_the_fonts_lack: only characters every SELECT LEVEL candidate font has (normal10/11, starwars50) survive
    CHECK(menuText("Who's there? 3 TIEs & 1 AT-ST (east/west): go!") == "WHO'S THERE 3 TIES  1 AT-ST (EAST/WEST): GO!");
    CHECK(menuText("Caf\xc3\xa9 #1") == "CAF 1");
    std::printf("mod_levels_test: ok\n");
    return 0;
}
