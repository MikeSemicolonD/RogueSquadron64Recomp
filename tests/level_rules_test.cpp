#include "game/mod_content/level_rules.h"
#include "check.h"
#include <cstdio>
#include <fstream>
#include <map>

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
const Facts kNone{};

// Loads a script with every object known; returns what reset dropped.
std::vector<std::string> load(Runner& r, const std::string& text, bool objectives = true)
{

    std::vector<std::string> errors;
    auto dropped = r.reset(parseScript(text, errors), kAll, objectives);

    CHECK(errors.empty());

    return dropped;
}

// The actions a tick produced, as "kind" or "kind:id".
std::vector<std::string> fired(Runner& r)
{

    std::vector<std::string> out;

    for (const auto& a : r.take())
    {
        out.push_back(param(a, "id").empty() ? a.kind : a.kind + ":" + param(a, "id"));
    }

    return out;
}

using Fired = std::vector<std::string>;

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
        r.tick(0.0f, 0.0f, kNone);
        auto a = r.take();
        CHECK(a.size() == 1 && param(a[0], "id") == "2");

        r.event({ When::Leave, "bridge", "player" });
        r.tick(0.0f, 0.0f, kNone);
        CHECK(r.take().empty());

        // An event that does not know its activator (area triggers) matches any "who".
        Runner u;

        u.reset(parseRules(R"({ "version": 1, "rules": [ { "when": "enter", "object": "b", "who": "player", "actions": [ { "do": "win" } ] } ] })", errors), kAll);
        u.event({ When::Enter, "b", "" });
        u.tick(0.0f, 0.0f, kNone);
        CHECK(u.take().size() == 1);

        r.event({ When::Enter, "bridge", "player" });
        r.tick(0.0f, 0.0f, kNone);
        a = r.take();
        CHECK(a.size() == 2 && param(a[0], "id") == "1" && param(a[1], "id") == "2");

        r.event({ When::Enter, "bridge", "player" });
        r.tick(0.0f, 0.0f, kNone);
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
        r.tick(0.0f, 0.0f, kNone);

        auto a = r.take();
        CHECK(a.size() == 1 && param(a[0], "id") == "1");

        r.tick(1.0f, 1.0f, kNone);
        CHECK(r.take().empty());

        r.tick(1.5f, 2.5f, kNone);
        a = r.take();
        CHECK(a.size() == 1 && param(a[0], "id") == "2");
    }

    // start_and_after: start on the first tick; after once its seconds have passed, once
    {
        std::vector<std::string> errors;
        Runner r;

        r.reset(parseRules(R"({ "version": 1, "rules": [ { "when": "start", "actions": [ { "do": "win" } ] },
            { "when": "after", "seconds": 10, "actions": [ { "do": "fail" } ] } ] })", errors), kAll);
        r.tick(0.1f, 0.1f, kNone);

        auto a = r.take();
        CHECK(a.size() == 1 && a[0].kind == "win");

        r.tick(5.0f, 5.1f, kNone);
        CHECK(r.take().empty());

        r.tick(5.0f, 10.1f, kNone);
        a = r.take();
        CHECK(a.size() == 1 && a[0].kind == "fail");

        r.tick(5.0f, 15.1f, kNone);
        CHECK(r.take().empty());
    }

    // reset_drops_pending: a wait from the previous run never comes out after a reset
    {
        std::vector<std::string> errors;
        Runner r;
        const auto rules = parseRules(R"({ "version": 1, "rules": [ { "when": "start", "actions": [
            { "do": "wait", "seconds": 1 }, { "do": "win" } ] } ] })", errors);

        r.reset(rules, kAll);
        r.tick(0.1f, 0.1f, kNone);
        r.reset({}, kAll);
        r.tick(5.0f, 5.0f, kNone);
        CHECK(r.take().empty());
    }

    // conditions_polled_once: a polled "when" fires the first tick its condition holds, and a once-rule not again
    {
        std::vector<std::string> errors;
        Runner r;

        r.reset(parseRules(R"({ "version": 1, "rules": [ { "when": "healthBelow", "object": "s", "value": 50, "actions": [ { "do": "win" } ] } ] })", errors), kAll);

        bool low = false;
        Facts f;

        f.health = [&](const std::string& n) { return n == "s" && low ? 40 : 100; };

        r.tick(0.1f, 0.1f, f);
        CHECK(r.take().empty());

        low = true;
        r.tick(0.1f, 0.2f, f);
        CHECK(r.take().size() == 1);

        r.tick(0.1f, 0.3f, f);
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
        r.tick(0.0f, 0.0f, kNone);

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
        r.tick(0.0f, 0.0f, kNone);
        auto a = r.take();
        CHECK(a.size() == 1 && a[0].kind == "win");

        r.event({ When::Custom, "gate", "", "mod.tripped" });
        r.tick(0.0f, 0.0f, kNone);
        CHECK(r.take().size() == 2);

        r.event({ When::Custom, "gate", "", "mod.other" });
        r.tick(0.0f, 0.0f, kNone);
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

    // packages.json: stock level folders only
    {
        std::vector<std::string> errors;

        CHECK(parsePackages(R"({ "version": 1, "levels": ["lv_2", "lv_h"] })", errors) == std::vector<std::string>({ "lv_2", "lv_h" }) && errors.empty());
        CHECK(parsePackages(R"({ "levels": ["../x", "lv_22", 5, "lv_a"] })", errors) == std::vector<std::string>({ "lv_a" }) && errors.size() == 3);

        errors.clear();
        CHECK(parsePackages("not json", errors).empty() && errors.size() == 1);
    }

    // condition_parses_and_evaluates: precedence not > and > or, parentheses, every check form, true/false literals
    {
        std::string err;
        const auto c = parseCondition("destroyed(gate) and not objective_done(2) or kills >= 3", err);

        CHECK(c && err.empty());

        std::map<std::string, double> v = { { "gate", 1 }, { "kills", 0 } };
        const auto value = [&](const Atom& a) { return a.check == Check::ObjectiveDone ? (a.index == 2 ? 0.0 : 1.0) : v[a.name]; };

        CHECK(evalCondition(c, value));
        v["gate"] = 0;
        CHECK(!evalCondition(c, value));
        v["kills"] = 3;
        CHECK(evalCondition(c, value));

        CHECK(parseCondition("health(shield gen) < 50 and entered(bridge_zone) and time > 120", err));
        CHECK(parseCondition("flag(5) and counter(3) != 2 and (bridge_held == true or not done)", err));

        const auto refs = condRefs(parseCondition("destroyed(a) and entered(b) and x and objective_failed(1)", err));

        CHECK(refs.objects == std::vector<std::string>({ "a", "b" }) && refs.variables == std::vector<std::string>({ "x" }) && refs.objectives && refs.checks == 4);

        const auto spaced = condRefs(parseCondition("health( shield gen ) < 50", err));

        CHECK(spaced.objects == std::vector<std::string>({ "shield gen" }));

        // Stock names can hold parentheses (CIV_Hut_(NEW)_01): a name reads to its matching ")".
        const auto paren = condRefs(parseCondition("destroyed(CIV_Hut_(NEW)_01) and entered(AT_PT_Spline_(Combat))", err));

        CHECK(paren.objects == std::vector<std::string>({ "CIV_Hut_(NEW)_01", "AT_PT_Spline_(Combat)" }));
    }

    // condition_rejects_bad_text: one error each, never a throw, never a partial tree
    {
        std::string err;

        for (const char* bad : { "", "and", "destroyed(", "kills >=", "x y", "health(a)", "time", "flag(0)", "flag(129)", "true", "not", "(a", "a)", "and = 1", "destroyed(a) and", "objective_done(5)", "destroyed()" })
        {
            err.clear();
            CHECK(!parseCondition(bad, err) && !err.empty());
        }

        CHECK(!parseCondition(std::string(17, '(') + "a" + std::string(17, ')'), err));
        CHECK(parseCondition(std::string(15, '(') + "a" + std::string(15, ')'), err));

        std::string many = "a";

        for (int i = 0; i < 64; i++)
        {
            many += " and a";
        }

        CHECK(!parseCondition(many, err));
    }

    // script_shared_cases: the game accepts exactly the rule counts and reports exactly the error lines the shared file lists
    {
        std::ifstream in(RS64_RULES_CASES, std::ios::binary);
        const auto cases = nlohmann::json::parse(in);

        CHECK(cases.size() >= 8);

        for (const auto& c : cases)
        {
            std::vector<std::string> errors;
            const auto rules = parseScript(c["text"].get<std::string>(), errors);
            std::vector<int> lines;

            for (const auto& e : errors)
            {
                lines.push_back(std::atoi(e.c_str() + 5));
            }

            if (rules.size() != c["rules"].get<size_t>() || lines != c["errorLines"].get<std::vector<int>>())
            {
                std::fprintf(stderr, "case %s: %zu rules\n", c["name"].get<std::string>().c_str(), rules.size());

                for (const auto& e : errors)
                {
                    std::fprintf(stderr, "  %s\n", e.c_str());
                }
            }

            CHECK(rules.size() == c["rules"].get<size_t>());
            CHECK(lines == c["errorLines"].get<std::vector<int>>());
        }
    }

    // script_fields: the parsed structs carry the values the text gives
    {
        std::vector<std::string> errors;
        const auto r = parseScript("when health shield gen below 40 if bridge_held every time\n  message 6 Hold the bridge\n  set bridge_held false\n  add kills -1\nend\n", errors);

        CHECK(errors.empty() && r.size() == 1 && r[0].when == When::HealthBelow && r[0].object == "shield gen" && r[0].value == 40 && r[0].every && r[0].cond && r[0].line == 1);
        CHECK(param(r[0].actions[0], "seconds") == "6" && param(r[0].actions[0], "text") == "Hold the bridge");
        CHECK(r[0].actions[1].kind == "set" && param(r[0].actions[1], "variable") == "bridge_held" && param(r[0].actions[1], "value") == "0");
        CHECK(r[0].actions[2].kind == "add" && param(r[0].actions[2], "value") == "-1");

        const auto t = parseScript("every 5 seconds\nend\nwhen true: kills >= 3\nend\nwhen custom mod.tripped for gate\n  shake seconds=2 text=big boom\n  complete objective 2\nend\n", errors);

        CHECK(t.size() == 3 && t[0].when == When::After && t[0].every && t[0].seconds == 5.0f);
        CHECK(t[1].when == When::True && t[1].cond && !t[1].every);
        CHECK(t[2].when == When::Custom && t[2].custom == "mod.tripped" && t[2].object == "gate");
        CHECK(t[2].actions[0].kind == "shake" && param(t[2].actions[0], "seconds") == "2" && param(t[2].actions[0], "text") == "big boom");
        CHECK(t[2].actions[1].kind == "objectiveDone" && param(t[2].actions[1], "objective") == "2");
    }

    // script_caps: over-cap input stops with an error rather than a changed meaning
    {
        std::vector<std::string> errors;
        std::string big;

        for (int i = 0; i < 600; i++)
        {
            big += "when start\n  win\nend\n";
        }

        CHECK(parseScript(big, errors).size() == (size_t)kMaxRules && !errors.empty());

        errors.clear();
        CHECK(parseScript("when start\n  message 3 " + std::string(1100, 'x') + "\nend\nwhen start\n  win\nend\n", errors).size() == 1 && errors.size() == 1);

        errors.clear();
        CHECK(parseScript(std::string(kMaxFileBytes + 1, ' '), errors).empty() && errors.size() == 1);

        std::string vars = "when start\n";

        for (int i = 0; i < kMaxVariables + 1; i++)
        {
            vars += "  set v" + std::to_string(i) + " 1\n";
        }

        errors.clear();
        CHECK(parseScript(vars + "end\n", errors).empty() && errors.size() == 1);
    }

    // if_failure_keeps_rule_armed: a once-only rule whose "if" fails still fires on a later event once it holds
    {
        Runner r;

        load(r, "when enter a if x\n  voice 1\nend\nwhen enter b\n  set x true\nend\n");
        r.event({ When::Enter, "a" });
        r.tick(0.0f, 0.0f, kNone);
        CHECK(fired(r).empty());

        r.event({ When::Enter, "b" });
        r.tick(0.0f, 0.0f, kNone);
        CHECK(fired(r).empty() && r.variable("x") == 1);

        r.event({ When::Enter, "a" });
        r.tick(0.0f, 0.0f, kNone);
        CHECK(fired(r) == Fired({ "voice:1" }));
    }

    // polled_rule_uses_combined_rise: "when flag 5 if x every time" fires each time flag-and-x turns true, not each frame it holds
    {
        Runner r;
        Facts f;

        f.flag = [](int i) { return i == 5; };
        load(r, "when flag 5 if x every time\n  voice 1\nend\nwhen enter b every time\n  set x true\nend\nwhen leave b every time\n  set x false\nend\n");

        r.tick(0.0f, 0.0f, f);
        CHECK(fired(r).empty());

        r.event({ When::Enter, "b" });
        r.tick(0.0f, 0.0f, f);
        CHECK(fired(r) == Fired({ "voice:1" }));

        r.tick(0.0f, 0.0f, f);
        CHECK(fired(r).empty());

        r.event({ When::Leave, "b" });
        r.tick(0.0f, 0.0f, f);
        r.event({ When::Enter, "b" });
        r.tick(0.0f, 0.0f, f);
        CHECK(fired(r) == Fired({ "voice:1" }));
    }

    // after_once_waits_for_condition: "when after 10 if x" fires at the first tick past 10 s where x holds
    {
        Runner r;

        load(r, "when after 10 if x\n  voice 1\nend\nwhen enter b\n  set x true\nend\n");
        r.tick(5.0f, 5.0f, kNone);
        r.tick(5.0f, 10.0f, kNone);
        CHECK(fired(r).empty());

        r.event({ When::Enter, "b" });
        r.tick(5.0f, 15.0f, kNone);
        CHECK(fired(r) == Fired({ "voice:1" }));

        r.tick(5.0f, 20.0f, kNone);
        CHECK(fired(r).empty());
    }

    // every_skips_failed_interval: "every 10 seconds if x" with x false at 10 s and true by 20 s fires at 20 s and 30 s only
    {
        Runner r;

        load(r, "every 10 seconds if x\n  voice 1\nend\nwhen enter b\n  set x true\nend\n");
        r.tick(10.0f, 10.0f, kNone);
        CHECK(fired(r).empty());

        r.event({ When::Enter, "b" });
        r.tick(5.0f, 15.0f, kNone);
        CHECK(fired(r).empty());

        r.tick(5.0f, 20.0f, kNone);
        CHECK(fired(r) == Fired({ "voice:1" }));

        r.tick(10.0f, 30.0f, kNone);
        CHECK(fired(r) == Fired({ "voice:1" }));
    }

    // start_checked_once: "when start if x" with x false at the start never fires
    {
        Runner r;

        load(r, "when start if x\n  voice 1\nend\nwhen enter b\n  set x true\nend\n");
        r.tick(0.0f, 0.0f, kNone);
        r.event({ When::Enter, "b" });
        r.tick(0.0f, 0.0f, kNone);
        r.tick(0.0f, 0.0f, kNone);
        CHECK(fired(r).empty());
    }

    // when_true_fires_at_start: a condition that holds at the start fires on the first tick, once
    {
        Runner r;

        load(r, "when true: time >= 0\n  voice 1\nend\n");
        r.tick(0.033f, 0.033f, kNone);
        CHECK(fired(r) == Fired({ "voice:1" }));

        r.tick(0.033f, 0.066f, kNone);
        CHECK(fired(r).empty());
    }

    // variables_set_add_saturate: set/add stay inside the runner, saturate, and an action after a wait sees its value only then
    {
        Runner r;

        load(r, "when start\n  set k 2147483647\n  add k 5\n  wait 1\n  add k -2147483642\n  voice 1\nend\nwhen true: k == 5\n  voice 2\nend\n");
        r.tick(0.1f, 0.1f, kNone);
        CHECK(fired(r).empty() && r.variable("k") == 2147483647);

        r.tick(1.0f, 1.1f, kNone);
        CHECK(fired(r) == Fired({ "voice:1", "voice:2" }) && r.variable("k") == 5);
        CHECK(r.variable("never") == 0);
    }

    // unknown_variable_drops_rule: a rule reading a variable no rule sets is dropped; the rest load
    {
        Runner r;
        const auto dropped = load(r, "when true: kils > 1\n  win\nend\nwhen start\n  set kills 1\nend\nwhen true: kills > 0\n  voice 1\nend\n");

        CHECK(dropped.size() == 1 && dropped[0].find("kils") != std::string::npos);

        r.tick(0.1f, 0.1f, kNone);
        CHECK(fired(r) == Fired({ "voice:1" }));
    }

    // objectives_dropped_without_own: objective whens, checks and actions drop on a level without its own objectives
    {
        Runner r;
        const auto dropped = load(r, "when objective 1 done\n  win\nend\nwhen start if objective_done(1)\n  win\nend\nwhen start\n  complete objective 1\nend\nwhen start\n  voice 1\nend\n", false);

        CHECK(dropped.size() == 3);

        r.tick(0.1f, 0.1f, kNone);
        CHECK(fired(r) == Fired({ "voice:1" }));
    }

    // health_of_destroyed_is_zero: once a unit's destroyed event is seen its health reads 0, whatever the game reports
    {
        Runner r;
        Facts f;

        f.health = [](const std::string&) { return 100; };
        load(r, "when true: health(s) < 1\n  voice 1\nend\nwhen health s below 50\n  voice 2\nend\n");
        r.tick(0.1f, 0.1f, f);
        CHECK(fired(r).empty());

        r.event({ When::Destroyed, "s" });
        r.tick(0.1f, 0.2f, f);
        CHECK(fired(r) == Fired({ "voice:1", "voice:2" }));
    }

    // condition_objects_checked: a rule whose condition names an object the level lacks is dropped
    {
        Runner r;
        std::vector<std::string> errors;
        const auto dropped = r.reset(parseScript("when start if destroyed(gone)\n  win\nend\nwhen start\n  voice 1\nend\n", errors), [](const std::string& n) { return n == "here"; });

        CHECK(dropped.size() == 1);
    }

    std::printf("level_rules_test: ok\n");

    return 0;
}
