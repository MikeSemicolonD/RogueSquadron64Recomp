#pragma once
// Level rules: "when this happens, do that" for a level, from a mod's assets/level/<folder>/rules.json. No game access here: the host
// feeds events and ticks, and executes the actions take() returns.
#include "json/json.hpp"
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

namespace rs64rules {

enum class When { Enter, Leave, PathPoint, Destroyed, HealthBelow, Start, After, ObjectiveDone, ObjectiveFailed, FlagIs, CounterAtLeast, Custom };

struct Param {
    std::string key, value;
};

struct Action {
    std::string kind;
    std::vector<Param> params;
};

// object: the area, path event (a path point, named by the explorer) or unit; who: the activator ("" anyone, "player"; an event that
// cannot tell matches any); value: health %, objective, flag or counter.
struct Rule {
    When when = When::Start;
    std::string object, who, custom;
    int value = 0;
    // counterAtLeast: the count the counter must reach.
    int count = 0;
    float seconds = 0;
    bool every = false;
    std::vector<Action> actions;
};

struct Event {
    When when = When::Start;
    std::string object, who, custom;
    int value = 0;
};

constexpr int kVersion = 1;

inline const std::map<std::string, When>& whenNames()
{

    static const std::map<std::string, When> k = {
        { "enter", When::Enter }, { "leave", When::Leave }, { "pathPoint", When::PathPoint }, { "destroyed", When::Destroyed },
        { "healthBelow", When::HealthBelow }, { "start", When::Start }, { "after", When::After }, { "objectiveDone", When::ObjectiveDone },
        { "objectiveFailed", When::ObjectiveFailed }, { "flagIs", When::FlagIs }, { "counterAtLeast", When::CounterAtLeast }, { "custom", When::Custom },
    };

    return k;
}

inline std::string paramOf(const Action& a, const std::string& key)
{

    for (const auto& p : a.params)
    {
        if (p.key == key) return p.value;
    }

    return {};
}

// A parameter as text: params are kept as JSON text, so a string comes back quoted and is unquoted here.
inline std::string paramText(const Action& a, const std::string& key)
{

    std::string v = paramOf(a, key);

    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);

    return v;
}

// A whole-number parameter, given as a number or a numeric string; fallback when missing or not a number.
inline int paramNumber(const Action& a, const std::string& key, int fallback)
{

    const std::string v = paramText(a, key);
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);

    return v.empty() || !end || *end != '\0' ? fallback : (int)d;
}

// Whens that name a level object, so a rule whose object the level lacks can be dropped.
inline bool namesObject(When w)
{
    return w == When::Enter || w == When::Leave || w == When::PathPoint || w == When::Destroyed || w == When::HealthBelow;
}

// Polled each tick rather than raised by a game event.
inline bool polled(When w)
{
    return w == When::HealthBelow || w == When::FlagIs || w == When::CounterAtLeast;
}

// A field read from a hand-written file: the expected type, a number given as a numeric string, or the fallback with one error
// (nlohmann's value() would throw on a mistyped field).
template <class T>
inline T field(const nlohmann::json& e, const char* key, T fallback, const std::string& where, std::vector<std::string>& errors)
{

    if (!e.is_object() || !e.contains(key)) return fallback;

    const auto& v = e[key];

    if constexpr (std::is_same_v<T, std::string>)
    {
        if (v.is_string()) return v.get<std::string>();
    }
    else if constexpr (std::is_same_v<T, bool>)
    {
        if (v.is_boolean()) return v.get<bool>();
    }
    else
    {
        if (v.is_number()) return v.get<T>();

        if (v.is_string())
        {
            const std::string s = v.get<std::string>();
            char* end = nullptr;
            const double d = std::strtod(s.c_str(), &end);

            if (!s.empty() && end && *end == '\0') return (T)d;
        }
    }

    errors.push_back(where + ": \"" + key + "\" has the wrong type; ignored");

    return fallback;
}

inline std::vector<Rule> parseRules(const std::string& text, std::vector<std::string>& errors)
{

    std::vector<Rule> out;
    const auto doc = nlohmann::json::parse(text, nullptr, false);

    if (doc.is_discarded() || !doc.is_object() || !doc.contains("rules") || !doc["rules"].is_array())
    {
        errors.push_back("rules.json is not { \"version\": 1, \"rules\": [...] }");
        return out;
    }

    const int version = field(doc, "version", kVersion, "rules.json", errors);

    if (version != kVersion)
    {
        errors.push_back("rules.json version " + std::to_string(version) + " is newer than this build reads");
        return out;
    }

    for (size_t i = 0; i < doc["rules"].size(); i++)
    {
        const auto& e = doc["rules"][i];
        const std::string where = "rule " + std::to_string(i + 1);
        const auto w = e.is_object() && e.contains("when") && e["when"].is_string() ? whenNames().find(e["when"].get<std::string>()) : whenNames().end();

        if (w == whenNames().end())
        {
            errors.push_back(where + ": unknown \"when\"");
            continue;
        }

        Rule r;
        r.when = w->second;
        r.object = field(e, "object", std::string(), where, errors);
        r.who = field(e, "who", std::string(), where, errors);
        r.custom = field(e, "custom", std::string(), where, errors);
        r.value = field(e, "value", 0, where, errors);
        r.count = field(e, "count", 0, where, errors);
        r.seconds = field(e, "seconds", 0.0f, where, errors);
        r.every = field(e, "every", false, where, errors);

        const auto& acts = e.contains("actions") ? e["actions"] : nlohmann::json::array();

        if (!acts.is_array())
        {
            errors.push_back(where + ": \"actions\" is not a list; ignored");
        }

        for (const auto& a : acts.is_array() ? acts : nlohmann::json::array())
        {
            if (!a.is_object() || !a.contains("do") || !a["do"].is_string())
            {
                errors.push_back(where + ": an action without a \"do\" name; skipped");
                continue;
            }

            Action act{ a["do"].get<std::string>() };

            for (const auto& [k, v] : a.items())
            {
                if (k != "do") act.params.push_back({ k, v.is_string() ? v.get<std::string>() : v.dump() });
            }

            r.actions.push_back(std::move(act));
        }

        out.push_back(std::move(r));
    }

    return out;
}

// A MORT voice stream of seconds of silence (8 kHz, 50 frames a second), what a message plays so its subtitle stays up that long.
// Header "MORT", u16 frames, u16 rate, u32 words with the header; then silence runs (bit 1, 4 bits n-1), packed LSB-first in BE words.
inline std::vector<uint8_t> silentMort(int seconds)
{

    const uint32_t frames = (uint32_t)seconds * 50u;
    std::vector<uint32_t> words = { 0x4D4F5254u, frames << 16 | 8000u, 0 };
    uint32_t bit = 96;

    const auto put = [&](uint32_t v, int n) {

        for (int i = 0; i < n; i++, bit++)
        {
            if ((bit >> 5) >= words.size()) words.push_back(0);

            if ((v >> i) & 1u) words[bit >> 5] |= 1u << (bit & 31);
        }
    };

    for (uint32_t left = frames; left > 0;)
    {
        const uint32_t n = left < 16 ? left : 16;

        put(1, 1);
        put(n - 1, 4);
        left -= n;
    }

    // The player reads ahead past the frame count; an uncounted silence run keeps that silent.
    put(1, 1);
    put(15, 4);
    words[2] = (uint32_t)words.size();

    std::vector<uint8_t> out;

    for (const uint32_t w : words)
    {
        for (int s = 24; s >= 0; s -= 8) out.push_back((uint8_t)(w >> s));
    }

    return out;
}

// The rules folder of a level: the mod level's folder, else the stock level's (lv_ + base-36 id).
inline std::string rulesFolder(const std::string& modFolder, int level)
{

    if (!modFolder.empty()) return modFolder;

    return std::string("lv_") + (char)(level < 10 ? '0' + level : 'a' + level - 10);
}

class Runner {
public:
    // Rules naming an object known() rejects are dropped; returns one message each.
    std::vector<std::string> reset(std::vector<Rule> rules, const std::function<bool(const std::string&)>& known)
    {

        std::vector<std::string> dropped;

        rules_.clear();

        for (auto& r : rules)
        {
            if (namesObject(r.when) && !known(r.object)) dropped.push_back("rule on \"" + r.object + "\": no such object in the level");
            else rules_.push_back(std::move(r));
        }

        fired_.assign(rules_.size(), false);
        held_.assign(rules_.size(), false);
        nextAt_.assign(rules_.size(), 0.0f);
        pending_.clear();
        out_.clear();
        started_ = false;

        return dropped;
    }

    void event(const Event& e)
    {

        for (size_t i = 0; i < rules_.size(); i++)
        {
            const auto& r = rules_[i];

            if (r.when != e.when || polled(r.when) || r.when == When::Start || r.when == When::After) continue;

            // A custom when without an object matches its event from any object.
            const bool anyObject = r.when == When::Custom && r.object.empty();

            if ((!anyObject && r.object != e.object) || (!r.who.empty() && !e.who.empty() && r.who != e.who) || (r.when == When::Custom && r.custom != e.custom)) continue;

            const bool byValue = r.when == When::ObjectiveDone || r.when == When::ObjectiveFailed;

            if (byValue && r.value != e.value) continue;

            fire(i);
        }
    }

    // dt and elapsed are game time; condition answers the polled whens.
    void tick(float dt, float elapsed, const std::function<bool(const Rule&)>& condition)
    {

        auto due = std::move(pending_);

        pending_.clear();

        for (auto& p : due)
        {
            p.left -= dt;

            if (p.left <= 0) run(p.rule, p.next);
            else pending_.push_back(p);
        }

        for (size_t i = 0; i < rules_.size(); i++)
        {
            const auto& r = rules_[i];

            if (r.when == When::Start && !started_) fire(i);

            if (r.when == When::After && elapsed >= (nextAt_[i] > 0 ? nextAt_[i] : r.seconds) && (r.every || !fired_[i]))
            {
                nextAt_[i] = (nextAt_[i] > 0 ? nextAt_[i] : r.seconds) + (r.seconds > 0 ? r.seconds : 1e9f);
                fire(i);
            }

            if (!polled(r.when)) continue;

            const bool now = condition(r);

            // Every-time polled rules fire when the condition turns true again, not on each tick it holds.
            if (now && !held_[i]) fire(i);

            held_[i] = now;
        }

        started_ = true;
    }

    std::vector<Action> take()
    {
        return std::move(out_);
    }

private:
    struct Pending {
        size_t rule, next;
        float left;
    };

    void fire(size_t i)
    {

        if (fired_[i] && !rules_[i].every) return;

        fired_[i] = true;
        run(i, 0);
    }

    void run(size_t i, size_t from)
    {

        const auto& acts = rules_[i].actions;

        for (size_t k = from; k < acts.size(); k++)
        {
            if (acts[k].kind == "wait")
            {
                pending_.push_back({ i, k + 1, std::strtof(paramOf(acts[k], "seconds").c_str(), nullptr) });
                return;
            }

            out_.push_back(acts[k]);
        }
    }

    std::vector<Rule> rules_;
    std::vector<bool> fired_, held_;
    std::vector<float> nextAt_;
    std::vector<Pending> pending_;
    std::vector<Action> out_;
    bool started_ = false;
};

}
