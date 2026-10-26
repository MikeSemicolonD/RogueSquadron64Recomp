#include "game/level_rules.h"
#include "check.h"
#include <cstdio>

using namespace rs64rules;

namespace
{

std::string param(const Action& a, const std::string& key)
{

    for (const auto& p : a.params)
    {
        if (p.key == key) return p.value;
    }

    return {};
}

const auto kAll = [](const std::string&) { return true; };
const auto kNever = [](const Rule&) { return false; };

}

int main()
{

    // parses_whens_and_actions: every "when" string, its fields and the action list with params as strings
    {
        std::vector<std::string> errors;
        const auto rules = parseRules(R"({ "version": 1, "rules": [
            { "when": "enter", "object": "bridge", "who": "player", "actions": [ { "do": "spawn", "kind": "TIE fighter", "count": 3, "path": "p" } ] },
            { "when": "leave", "object": "bridge" }, { "when": "pathPoint", "object": "route", "value": 3 },
            { "when": "destroyed", "object": "shuttle" }, { "when": "healthBelow", "object": "shuttle", "value": 50 },
            { "when": "start" }, { "when": "after", "seconds": 10, "every": true }, { "when": "objectiveDone", "value": 1 },
            { "when": "objectiveFailed", "value": 2 }, { "when": "flagIs", "value": 120 }, { "when": "counterAtLeast", "value": 121, "count": 5 },
            { "when": "custom", "custom": "mod.event", "object": "x" } ] })", errors);

        CHECK(errors.empty() && rules.size() == 12);

        const When kinds[] = { When::Enter, When::Leave, When::PathPoint, When::Destroyed, When::HealthBelow, When::Start, When::After,
            When::ObjectiveDone, When::ObjectiveFailed, When::FlagIs, When::CounterAtLeast, When::Custom };

        for (size_t i = 0; i < rules.size(); i++) CHECK(rules[i].when == kinds[i]);

        CHECK(rules[0].object == "bridge" && rules[0].who == "player" && rules[0].actions.size() == 1);
        CHECK(rules[0].actions[0].kind == "spawn" && param(rules[0].actions[0], "kind") == "TIE fighter");
        CHECK(param(rules[0].actions[0], "count") == "3" && param(rules[0].actions[0], "path") == "p");
        CHECK(rules[10].count == 5);
        CHECK(rules[2].value == 3 && rules[6].seconds == 10.0f && rules[6].every && !rules[0].every && rules[11].custom == "mod.event");
    }

    // rejects_bad_documents: no throw; a bad document gives no rules, an unknown "when" skips that rule; one error each
    {
        std::vector<std::string> errors;

        CHECK(parseRules("not json", errors).empty() && errors.size() == 1);
        CHECK(parseRules(R"({ "version": 2, "rules": [ { "when": "start" } ] })", errors).empty() && errors.size() == 2);

        const auto rules = parseRules(R"({ "version": 1, "rules": [ { "when": "teleport" }, { "when": "start" } ] })", errors);

        CHECK(rules.size() == 1 && rules[0].when == When::Start && errors.size() == 3);
    }

    // wrong_types_do_not_throw: a hand-written file with mistyped fields loads; numeric strings count as numbers, other mistypes
    // fall back to the defaults with one error each
    {
        std::vector<std::string> errors;
        const auto rules = parseRules(R"({ "version": "1", "rules": [
            { "when": "healthBelow", "object": "s", "value": "50", "every": "yes", "actions": [ { "do": 7 }, { "do": "win" } ] },
            { "when": "after", "seconds": "ten", "object": 3, "actions": { "do": "fail" } },
            { "when": 5 } ] })", errors);

        CHECK(rules.size() == 2);
        CHECK(rules[0].value == 50 && !rules[0].every && rules[0].actions.size() == 1 && rules[0].actions[0].kind == "win");
        CHECK(rules[1].seconds == 0.0f && rules[1].object.empty() && rules[1].actions.empty());
        CHECK(errors.size() == 6);
    }

    // action_params_as_text_or_numbers: strings unquoted, numbers given as strings read as numbers, a missing key the fallback
    {
        std::vector<std::string> errors;
        const auto rules = parseRules(R"({ "version": 1, "rules": [ { "when": "start", "actions": [
            { "do": "voice", "id": "627", "file": "brief", "seconds": 2.5 } ] } ] })", errors);
        const Action& a = rules[0].actions[0];

        CHECK(paramText(a, "file") == "brief" && paramText(a, "id") == "627" && paramText(a, "none").empty());
        CHECK(paramNumber(a, "id", -1) == 627 && paramNumber(a, "seconds", 0) == 2 && paramNumber(a, "none", -1) == -1 && paramNumber(a, "file", -1) == -1);
    }

    // matches_events_once_or_every: who filters, Leave is not Enter, once-rules fire once, same-event rules in file order
    {
        std::vector<std::string> errors;
        Runner r;

        r.reset(parseRules(R"({ "version": 1, "rules": [
            { "when": "enter", "object": "bridge", "who": "player", "actions": [ { "do": "voice", "id": 1 } ] },
            { "when": "enter", "object": "bridge", "every": true, "actions": [ { "do": "voice", "id": 2 } ] } ] })", errors), kAll);

        r.event({ When::Enter, "bridge", "other" });
        auto a = r.take();
        CHECK(a.size() == 1 && param(a[0], "id") == "2");

        r.event({ When::Leave, "bridge", "player" });
        CHECK(r.take().empty());

        // An event that does not know its activator (area triggers) matches any "who".
        Runner u;

        u.reset(parseRules(R"({ "version": 1, "rules": [ { "when": "enter", "object": "b", "who": "player", "actions": [ { "do": "win" } ] } ] })", errors), kAll);
        u.event({ When::Enter, "b", "" });
        CHECK(u.take().size() == 1);

        r.event({ When::Enter, "bridge", "player" });
        a = r.take();
        CHECK(a.size() == 2 && param(a[0], "id") == "1" && param(a[1], "id") == "2");

        r.event({ When::Enter, "bridge", "player" });
        a = r.take();
        CHECK(a.size() == 1 && param(a[0], "id") == "2");
    }

    // waits_hold_later_actions: a wait holds the rest of its rule for that much game time
    {
        std::vector<std::string> errors;
        Runner r;

        r.reset(parseRules(R"({ "version": 1, "rules": [ { "when": "destroyed", "object": "s", "actions": [
            { "do": "voice", "id": 1 }, { "do": "wait", "seconds": 2 }, { "do": "voice", "id": 2 } ] } ] })", errors), kAll);
        r.event({ When::Destroyed, "s" });

        auto a = r.take();
        CHECK(a.size() == 1 && param(a[0], "id") == "1");

        r.tick(1.0f, 1.0f, kNever);
        CHECK(r.take().empty());

        r.tick(1.5f, 2.5f, kNever);
        a = r.take();
        CHECK(a.size() == 1 && param(a[0], "id") == "2");
    }

    // start_and_after: start on the first tick; after once its seconds have passed, once
    {
        std::vector<std::string> errors;
        Runner r;

        r.reset(parseRules(R"({ "version": 1, "rules": [ { "when": "start", "actions": [ { "do": "win" } ] },
            { "when": "after", "seconds": 10, "actions": [ { "do": "fail" } ] } ] })", errors), kAll);
        r.tick(0.1f, 0.1f, kNever);

        auto a = r.take();
        CHECK(a.size() == 1 && a[0].kind == "win");

        r.tick(5.0f, 5.1f, kNever);
        CHECK(r.take().empty());

        r.tick(5.0f, 10.1f, kNever);
        a = r.take();
        CHECK(a.size() == 1 && a[0].kind == "fail");

        r.tick(5.0f, 15.1f, kNever);
        CHECK(r.take().empty());
    }

    // reset_drops_pending: a wait from the previous run never comes out after a reset
    {
        std::vector<std::string> errors;
        Runner r;
        const auto rules = parseRules(R"({ "version": 1, "rules": [ { "when": "start", "actions": [
            { "do": "wait", "seconds": 1 }, { "do": "win" } ] } ] })", errors);

        r.reset(rules, kAll);
        r.tick(0.1f, 0.1f, kNever);
        r.reset({}, kAll);
        r.tick(5.0f, 5.0f, kNever);
        CHECK(r.take().empty());
    }

    // conditions_polled_once: a polled "when" fires the first tick its condition holds, and a once-rule not again
    {
        std::vector<std::string> errors;
        Runner r;

        r.reset(parseRules(R"({ "version": 1, "rules": [ { "when": "healthBelow", "object": "s", "value": 50, "actions": [ { "do": "win" } ] } ] })", errors), kAll);

        bool low = false;
        const auto cond = [&](const Rule&) { return low; };

        r.tick(0.1f, 0.1f, cond);
        CHECK(r.take().empty());

        low = true;
        r.tick(0.1f, 0.2f, cond);
        CHECK(r.take().size() == 1);

        r.tick(0.1f, 0.3f, cond);
        CHECK(r.take().empty());
    }

    // missing_objects_skip_one_rule: a rule naming an object the level lacks is dropped with an error; the others still fire
    {
        std::vector<std::string> errors;
        Runner r;
        const auto dropped = r.reset(parseRules(R"({ "version": 1, "rules": [
            { "when": "destroyed", "object": "gone", "actions": [ { "do": "fail" } ] },
            { "when": "destroyed", "object": "here", "actions": [ { "do": "win" } ] } ] })", errors),
            [](const std::string& n) { return n == "here"; });

        CHECK(dropped.size() == 1);

        r.event({ When::Destroyed, "gone" });
        r.event({ When::Destroyed, "here" });

        const auto a = r.take();
        CHECK(a.size() == 1 && a[0].kind == "win");
    }

    // custom_events_by_name: a mod's custom when matches its name, and its object only when the rule names one
    {
        std::vector<std::string> errors;
        Runner r;

        r.reset(parseRules(R"({ "version": 1, "rules": [
            { "when": "custom", "custom": "mod.tripped", "every": true, "actions": [ { "do": "win" } ] },
            { "when": "custom", "custom": "mod.tripped", "object": "gate", "every": true, "actions": [ { "do": "fail" } ] } ] })", errors), kAll);

        r.event({ When::Custom, "wall", "", "mod.tripped" });
        auto a = r.take();
        CHECK(a.size() == 1 && a[0].kind == "win");

        r.event({ When::Custom, "gate", "", "mod.tripped" });
        CHECK(r.take().size() == 2);

        r.event({ When::Custom, "gate", "", "mod.other" });
        CHECK(r.take().empty());
    }

    // silent_mort_stream: "MORT", 50 frames a second at 8 kHz, a word count with the header, then only silence runs (1 + 4 bits n-1)
    {
        const auto m = silentMort(3);
        const auto be32 = [&](size_t o) { return (uint32_t)m[o] << 24 | (uint32_t)m[o + 1] << 16 | (uint32_t)m[o + 2] << 8 | m[o + 3]; };

        CHECK(m.size() % 4 == 0 && be32(0) == 0x4D4F5254u && (be32(4) >> 16) == 150 && (be32(4) & 0xFFFF) == 8000 && be32(8) * 4 == m.size());

        size_t bit = 96, frames = 0;
        const auto read = [&](int n) {

            uint32_t v = 0;

            for (int i = 0; i < n; i++, bit++) v |= ((be32((bit >> 5) * 4) >> (bit & 31)) & 1u) << i;

            return v;
        };

        while (frames < 150)
        {
            CHECK(read(1) == 1);
            frames += read(4) + 1;
        }

        CHECK(frames == 150);
    }

    // rules_folder: the mod level's folder, else the stock level's lv_<base-36 id>
    CHECK(rulesFolder("ice", 1) == "ice" && rulesFolder("", 1) == "lv_1" && rulesFolder("", 10) == "lv_a");

    std::printf("level_rules_test: ok\n");

    return 0;
}
