#pragma once
// Level rules: "when this happens, do that" for a level, from a mod's assets/level/<folder>/rules.txt (or the older rules.json). No game access here: the host
// feeds events and ticks, and executes the actions take() returns.
#include "json/json.hpp"
#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace rs64rules {

enum class When { Enter, Leave, PathPoint, Destroyed, HealthBelow, Start, After, ObjectiveDone, ObjectiveFailed, FlagIs, CounterAtLeast, Custom, True };

struct Cond;

using CondPtr = std::shared_ptr<const Cond>;

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
    // rules.txt only: the "if" (or a when true: rule's condition), and the header's line.
    CondPtr cond;
    int line = 0;
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
    return w == When::HealthBelow || w == When::FlagIs || w == When::CounterAtLeast || w == When::True;
}

// ---- Conditions: "destroyed(gate) and not objective_done(2) or kills >= 3", read-only checks the runner answers ----
enum class Check { Destroyed, Entered, Health, ObjectiveDone, ObjectiveFailed, Time, Flag, Counter, Variable };
enum class Op { None, Lt, Le, Gt, Ge, Eq, Ne };

// name: an object or variable; index: an objective, flag or counter.
struct Atom {
    Check check = Check::Variable;
    std::string name;
    int index = 0;
};

struct Cond {
    enum class Kind { And, Or, Not, Test };

    Kind kind = Kind::Test;
    std::vector<std::shared_ptr<const Cond>> parts;
    Atom atom;
    Op op = Op::None;
    double rhs = 0;
};

constexpr int kMaxDepth = 16;
constexpr int kMaxChecksPerCond = 64;

class CondParser {
public:
    explicit CondParser(const std::string& text) : s_(text) {}

    CondPtr parse(std::string& error)
    {

        auto c = parseOr(0);

        skip();

        if (c && pos_ < s_.size()) fail("unexpected \"" + rest() + "\"");

        if (!err_.empty())
        {
            error = err_;
            return nullptr;
        }

        return c;
    }

private:
    const std::string& s_;
    size_t pos_ = 0;
    int checks_ = 0;
    std::string err_;

    void fail(const std::string& e)
    {
        if (err_.empty()) err_ = e;
    }

    void skip()
    {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t')) pos_++;
    }

    static bool wordChar(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    }

    std::string rest() const
    {
        return s_.substr(pos_, 24);
    }

    std::string peekWord()
    {

        skip();

        size_t e = pos_;

        while (e < s_.size() && wordChar(s_[e])) e++;

        return s_.substr(pos_, e - pos_);
    }

    bool takeWord(const char* w)
    {

        if (peekWord() != w) return false;

        pos_ += std::char_traits<char>::length(w);

        return true;
    }

    bool takeChar(char c)
    {

        skip();

        if (pos_ >= s_.size() || s_[pos_] != c) return false;

        pos_++;

        return true;
    }

    CondPtr join(Cond::Kind kind, std::vector<CondPtr> parts)
    {

        if (parts.size() == 1) return parts[0];

        auto c = std::make_shared<Cond>();

        c->kind = kind;
        c->parts = std::move(parts);

        return c;
    }

    CondPtr parseOr(int depth)
    {

        std::vector<CondPtr> parts = { parseAnd(depth) };

        while (err_.empty() && takeWord("or")) parts.push_back(parseAnd(depth));

        return err_.empty() ? join(Cond::Kind::Or, std::move(parts)) : nullptr;
    }

    CondPtr parseAnd(int depth)
    {

        std::vector<CondPtr> parts = { parseUnary(depth) };

        while (err_.empty() && takeWord("and")) parts.push_back(parseUnary(depth));

        return err_.empty() ? join(Cond::Kind::And, std::move(parts)) : nullptr;
    }

    CondPtr parseUnary(int depth)
    {

        if (depth > kMaxDepth)
        {
            fail("nested more than " + std::to_string(kMaxDepth) + " deep");
            return nullptr;
        }

        if (takeWord("not"))
        {
            auto c = std::make_shared<Cond>();

            c->kind = Cond::Kind::Not;
            c->parts = { parseUnary(depth + 1) };

            return err_.empty() ? c : nullptr;
        }

        if (takeChar('('))
        {
            auto c = parseOr(depth + 1);

            if (err_.empty() && !takeChar(')')) fail("a \"(\" is never closed");

            return err_.empty() ? c : nullptr;
        }

        return parseTest();
    }

    // A number, or true / false as 1 / 0.
    bool number(double& out)
    {

        if (takeWord("true"))
        {
            out = 1;
            return true;
        }

        if (takeWord("false"))
        {
            out = 0;
            return true;
        }

        skip();

        const size_t start = pos_;

        if (pos_ < s_.size() && s_[pos_] == '-') pos_++;

        const size_t digits = pos_;

        while (pos_ < s_.size() && ((s_[pos_] >= '0' && s_[pos_] <= '9') || s_[pos_] == '.')) pos_++;

        if (pos_ == digits)
        {
            pos_ = start;
            return false;
        }

        out = std::strtod(s_.substr(start, pos_ - start).c_str(), nullptr);

        return true;
    }

    Op op()
    {

        skip();

        static const std::pair<const char*, Op> kOps[] = { { "<=", Op::Le }, { ">=", Op::Ge }, { "==", Op::Eq }, { "!=", Op::Ne }, { "<", Op::Lt }, { ">", Op::Gt } };

        for (const auto& [text, o] : kOps)
        {
            if (s_.compare(pos_, std::char_traits<char>::length(text), text) == 0)
            {
                pos_ += std::char_traits<char>::length(text);
                return o;
            }
        }

        return Op::None;
    }

    // The text inside a check's parentheses: a name (to the ")"), or a whole number in lo-hi.
    bool inner(Atom& a, int lo, int hi)
    {

        if (!takeChar('('))
        {
            fail("expected \"(\" after the check");
            return false;
        }

        // Stock names can hold parentheses (CIV_Hut_(NEW)_01), so the name ends at the matching ")".
        size_t close = pos_;

        for (int open = 1; close < s_.size(); close++)
        {
            if (s_[close] == '(') open++;
            else if (s_[close] == ')' && --open == 0) break;
        }

        if (close >= s_.size())
        {
            fail("a \"(\" is never closed");
            return false;
        }

        std::string text = s_.substr(pos_, close - pos_);

        pos_ = close + 1;

        while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.erase(text.begin());

        while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.pop_back();

        if (text.empty())
        {
            fail("nothing inside \"()\"");
            return false;
        }

        if (hi == 0)
        {
            a.name = text;
            return true;
        }

        char* end = nullptr;
        const long n = std::strtol(text.c_str(), &end, 10);

        if (*end != '\0' || n < lo || n > hi)
        {
            fail("\"" + text + "\" is not a number from " + std::to_string(lo) + " to " + std::to_string(hi));
            return false;
        }

        a.index = (int)n;

        return true;
    }

    CondPtr parseTest()
    {

        const std::string w = peekWord();

        if (w.empty() || (w[0] >= '0' && w[0] <= '9'))
        {
            fail(pos_ < s_.size() ? "expected a check at \"" + rest() + "\"" : "the condition ends too soon");
            return nullptr;
        }

        static const std::map<std::string, std::pair<Check, int>> kChecks = {
            { "destroyed", { Check::Destroyed, 0 } }, { "entered", { Check::Entered, 0 } }, { "health", { Check::Health, 0 } },
            { "objective_done", { Check::ObjectiveDone, 4 } }, { "objective_failed", { Check::ObjectiveFailed, 4 } },
            { "flag", { Check::Flag, 128 } }, { "counter", { Check::Counter, 128 } },
        };

        static const char* const kReserved[] = { "and", "or", "not", "true", "false" };

        for (const char* r : kReserved)
        {
            if (w == r)
            {
                fail("\"" + w + "\" is not a check");
                return nullptr;
            }
        }

        pos_ += w.size();

        if (++checks_ > kMaxChecksPerCond)
        {
            fail("more than " + std::to_string(kMaxChecksPerCond) + " checks");
            return nullptr;
        }

        auto c = std::make_shared<Cond>();
        const auto found = kChecks.find(w);

        if (w == "time") c->atom.check = Check::Time;
        else if (found == kChecks.end()) c->atom = { Check::Variable, w, 0 };
        else
        {
            c->atom.check = found->second.first;

            if (!inner(c->atom, 1, found->second.second)) return nullptr;
        }

        c->op = op();

        const bool needsOp = c->atom.check == Check::Health || c->atom.check == Check::Counter || c->atom.check == Check::Time;

        if (c->op == Op::None && needsOp)
        {
            fail("\"" + w + "\" needs a comparison, like " + w + (w == "time" ? "" : "(...)") + " > 10");
            return nullptr;
        }

        if (c->op != Op::None && !number(c->rhs))
        {
            fail("expected a number after the comparison");
            return nullptr;
        }

        return c;
    }
};

inline CondPtr parseCondition(const std::string& text, std::string& error)
{
    return CondParser(text).parse(error);
}

inline bool evalCondition(const CondPtr& c, const std::function<double(const Atom&)>& value)
{

    if (!c) return true;

    switch (c->kind)
    {
        case Cond::Kind::Not: return !evalCondition(c->parts[0], value);
        case Cond::Kind::And:
            for (const auto& p : c->parts)
            {
                if (!evalCondition(p, value)) return false;
            }

            return true;
        case Cond::Kind::Or:
            for (const auto& p : c->parts)
            {
                if (evalCondition(p, value)) return true;
            }

            return false;
        case Cond::Kind::Test: break;
    }

    const double v = value(c->atom);

    switch (c->op)
    {
        case Op::None: return v != 0;
        case Op::Lt: return v < c->rhs;
        case Op::Le: return v <= c->rhs;
        case Op::Gt: return v > c->rhs;
        case Op::Ge: return v >= c->rhs;
        case Op::Eq: return v == c->rhs;
        case Op::Ne: return v != c->rhs;
    }

    return false;
}

struct CondRefs {
    std::vector<std::string> objects, variables;
    bool objectives = false;
    int checks = 0;
};

inline void condRefsInto(const CondPtr& c, CondRefs& out)
{

    if (!c) return;

    for (const auto& p : c->parts) condRefsInto(p, out);

    if (c->kind != Cond::Kind::Test) return;

    const auto add = [](std::vector<std::string>& list, const std::string& n) {

        if (std::find(list.begin(), list.end(), n) == list.end()) list.push_back(n);
    };

    const Check k = c->atom.check;

    out.checks++;

    if (k == Check::Destroyed || k == Check::Entered || k == Check::Health) add(out.objects, c->atom.name);
    else if (k == Check::Variable) add(out.variables, c->atom.name);
    else if (k == Check::ObjectiveDone || k == Check::ObjectiveFailed) out.objectives = true;
}

inline CondRefs condRefs(const CondPtr& c)
{

    CondRefs out;

    condRefsInto(c, out);

    return out;
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

// ---- rules.txt: "when <event> [if <condition>] [every time]", one action a line, "end". See docs/guides/modding.md "Level rules". ----
constexpr size_t kMaxFileBytes = 256 * 1024;
constexpr int kMaxRules = 512;
constexpr size_t kMaxLine = 1024;
constexpr int kMaxChecksPerFile = 2048;
constexpr int kMaxVariables = 256;

namespace script {

inline std::string trim(const std::string& s)
{

    const size_t a = s.find_first_not_of(' ');

    return a == std::string::npos ? std::string() : s.substr(a, s.find_last_not_of(' ') - a + 1);
}

inline std::vector<std::string> words(const std::string& s)
{

    std::vector<std::string> out;

    for (size_t i = 0; i < s.size();)
    {
        const size_t e = s.find(' ', i);
        const size_t end = e == std::string::npos ? s.size() : e;

        if (end > i) out.push_back(s.substr(i, end - i));

        i = end + 1;
    }

    return out;
}

// The text after the first n words.
inline std::string after(const std::string& s, int n)
{

    size_t i = 0;

    for (int k = 0; k < n && i < s.size(); k++)
    {
        i = s.find_first_not_of(' ', i);
        i = i == std::string::npos ? s.size() : s.find(' ', i);
        i = i == std::string::npos ? s.size() : i;
    }

    return trim(s.substr(std::min(i, s.size())));
}

inline bool wholeNumber(const std::string& s, int lo, int hi, int& out)
{

    char* end = nullptr;
    const long n = std::strtol(s.c_str(), &end, 10);

    if (s.empty() || *end != '\0' || n < lo || n > hi) return false;

    out = (int)n;

    return true;
}

inline bool seconds(const std::string& s, float& out)
{

    char* end = nullptr;
    const double d = std::strtod(s.c_str(), &end);

    if (s.empty() || *end != '\0' || !(d >= 0) || d > 86400) return false;

    out = (float)d;

    return true;
}

inline bool variableName(const std::string& s)
{

    static const char* const kReserved[] = { "and", "or", "not", "time", "true", "false" };

    if (s.empty() || (s[0] >= '0' && s[0] <= '9')) return false;

    for (const char c : s)
    {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }

    return std::none_of(std::begin(kReserved), std::end(kReserved), [&](const char* r) { return s == r; });
}

inline bool endsWith(const std::string& s, const std::string& tail)
{
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

inline bool header(const std::string& line)
{

    const auto w = words(line);

    return !w.empty() && (w[0] == "when" || w[0] == "every");
}

inline bool parseHeader(std::string t, Rule& r, std::string& err)
{

    bool everyTime = false;

    if (endsWith(t, " every time"))
    {
        everyTime = true;
        t = trim(t.substr(0, t.size() - 11));
    }

    if (t.rfind("when true:", 0) == 0)
    {
        const std::string c = trim(t.substr(10));
        const auto cw = words(c);

        if (std::find(cw.begin(), cw.end(), "if") != cw.end())
        {
            err = "\"when true:\" is its own condition; it takes no \"if\"";
            return false;
        }

        r.when = When::True;
        r.every = everyTime;
        r.cond = parseCondition(c, err);

        return r.cond != nullptr;
    }

    std::string head = t;
    const size_t ifAt = t.find(" if ");

    if (ifAt != std::string::npos)
    {
        head = trim(t.substr(0, ifAt));
        r.cond = parseCondition(trim(t.substr(ifAt + 4)), err);

        if (!r.cond) return false;
    }

    const auto w = words(head);
    const std::string kind = w.size() > 1 ? w[1] : "";
    const std::string rest = after(head, 2);

    r.every = everyTime;

    if (w[0] == "every")
    {
        if (everyTime) err = "\"every N seconds\" already repeats; drop \"every time\"";
        else if (w.size() != 3 || w[2] != "seconds" || !seconds(w[1], r.seconds) || r.seconds <= 0) err = "expected \"every <seconds> seconds\"";

        r.when = When::After;
        r.every = true;

        return err.empty();
    }

    static const std::map<std::string, When> kNamed = {
        { "enter", When::Enter }, { "leave", When::Leave }, { "point", When::PathPoint }, { "destroyed", When::Destroyed },
    };

    const auto named = kNamed.find(kind);

    if (named != kNamed.end())
    {
        r.when = named->second;
        r.object = rest;

        if (rest.empty()) err = "\"when " + kind + "\" needs a name";
    }
    else if (kind == "health")
    {
        const size_t below = rest.rfind(" below ");

        r.when = When::HealthBelow;
        r.object = below == std::string::npos ? "" : trim(rest.substr(0, below));

        if (r.object.empty() || !wholeNumber(trim(rest.substr(below + 7)), 0, 100, r.value)) err = "expected \"when health <unit> below <percent>\"";
    }
    else if (kind == "start" && w.size() == 2) r.when = When::Start;
    else if (kind == "after")
    {
        r.when = When::After;

        if (w.size() != 3 || !seconds(w[2], r.seconds)) err = "expected \"when after <seconds>\"";
    }
    else if (kind == "objective")
    {
        const bool done = w.size() == 4 && w[3] == "done";

        r.when = done ? When::ObjectiveDone : When::ObjectiveFailed;

        if (w.size() != 4 || !(done || w[3] == "failed") || !wholeNumber(w[2], 1, 4, r.value)) err = "expected \"when objective <1-4> done\" or \"failed\"";
    }
    else if (kind == "flag")
    {
        r.when = When::FlagIs;

        if (w.size() != 3 || !wholeNumber(w[2], 1, 128, r.value)) err = "expected \"when flag <1-128>\"";
    }
    else if (kind == "counter")
    {
        r.when = When::CounterAtLeast;

        if (w.size() != 5 || w[3] != "reaches" || !wholeNumber(w[2], 1, 128, r.value) || !wholeNumber(w[4], -2147483647, 2147483647, r.count))
        {
            err = "expected \"when counter <1-128> reaches <count>\"";
        }
    }
    else if (kind == "custom")
    {
        const size_t forAt = rest.find(" for ");

        r.when = When::Custom;
        r.custom = forAt == std::string::npos ? rest : trim(rest.substr(0, forAt));
        r.object = forAt == std::string::npos ? "" : trim(rest.substr(forAt + 5));

        if (r.custom.empty() || r.custom.find(' ') != std::string::npos) err = "expected \"when custom <event> [for <object>]\"";
    }
    else err = "\"" + (kind.empty() ? w[0] : kind) + "\" is not something a rule can wait for";

    return err.empty();
}

// A mod action or spawn: "<name> key=value ...", each value running to the next " key=" or the end of the line.
inline bool parseGeneric(const std::string& name, const std::string& rest, Action& a, std::string& err)
{

    std::vector<size_t> keys;

    for (size_t i = 0; i < rest.size(); i++)
    {
        if (i > 0 && rest[i - 1] != ' ') continue;

        size_t e = i;

        while (e < rest.size() && ((rest[e] >= 'a' && rest[e] <= 'z') || (rest[e] >= 'A' && rest[e] <= 'Z') || (rest[e] >= '0' && rest[e] <= '9') || rest[e] == '_')) e++;

        if (e > i && e < rest.size() && rest[e] == '=') keys.push_back(i);
    }

    const bool nameOk = std::all_of(name.begin(), name.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.'; });

    if (!nameOk || (!rest.empty() && (keys.empty() || keys[0] != 0)))
    {
        err = "\"" + name + "\" is not an action";
        return false;
    }

    a.kind = name;

    for (size_t k = 0; k < keys.size(); k++)
    {
        const size_t eq = rest.find('=', keys[k]);
        const size_t end = k + 1 < keys.size() ? keys[k + 1] : rest.size();

        a.params.push_back({ rest.substr(keys[k], eq - keys[k]), trim(rest.substr(eq + 1, end - eq - 1)) });
    }

    return true;
}

inline bool parseAction(const std::string& line, Action& a, std::string& err)
{

    const auto w = words(line);
    const std::string& k = w[0];
    const std::string rest = after(line, 1);
    int n = 0;
    int v = 0;
    float s = 0;

    const auto one = [&](const char* kind, const char* key, bool ok) {

        if (!ok) err = "expected \"" + k + " <" + key + ">\"";

        a = { kind, { { key, rest } } };

        return ok;
    };

    if (k == "voice") return one("voice", "id", w.size() == 2 && wholeNumber(w[1], 0, 65535, n));

    if (k == "music") return one("music", "song", w.size() == 2 && wholeNumber(w[1], 0, 255, n));

    if (k == "wait") return one("wait", "seconds", w.size() == 2 && seconds(w[1], s));

    if (k == "recording") return one("recording", "file", !rest.empty());

    if (k == "camera") return one("camera", "camera", !rest.empty());

    if (k == "destroy") return one("destroy", "unit", !rest.empty());

    if ((k == "win" || k == "fail") && w.size() == 1)
    {
        a = { k, {} };
        return true;
    }

    if ((k == "complete" || k == "fail") && w.size() == 3 && w[1] == "objective" && wholeNumber(w[2], 1, 4, n))
    {
        a = { k == "complete" ? "objectiveDone" : "objectiveFail", { { "objective", w[2] } } };
        return true;
    }

    if (k == "complete" || k == "fail")
    {
        err = "expected \"" + k + " objective <1-4>\"";
        return false;
    }

    if (k == "message")
    {
        a = { "message", { { "seconds", w.size() > 1 ? w[1] : "" }, { "text", after(line, 2) } } };

        if (w.size() < 3 || !wholeNumber(w[1], 1, 10, n)) err = "expected \"message <1-10 seconds> <text>\"";

        return err.empty();
    }

    if (k == "flag" || k == "counter")
    {
        const char* amount = k == "flag" ? "value" : "add";

        a = { k, { { k, w.size() > 1 ? w[1] : "" }, { amount, w.size() > 2 ? w[2] : "1" } } };

        if (w.size() < 2 || w.size() > 3 || !wholeNumber(w[1], 1, 128, n) || (w.size() == 3 && !wholeNumber(w[2], -2147483647, 2147483647, v)))
        {
            err = "expected \"" + k + " <1-128> [" + amount + "]\"";
        }

        return err.empty();
    }

    if (k == "set" || k == "add")
    {
        const std::string value = w.size() == 3 ? (w[2] == "true" ? "1" : (w[2] == "false" ? "0" : w[2])) : "";
        const bool ok = w.size() == 3 && variableName(w[1]) && wholeNumber(value, -2147483647, 2147483647, v) && (k == "set" || value == w[2]);

        a = { k, { { "variable", w.size() > 1 ? w[1] : "" }, { "value", value } } };

        if (!ok) err = k == "set" ? "expected \"set <variable> <number, true or false>\"" : "expected \"add <variable> <number>\"";

        return ok;
    }

    return parseGeneric(k, rest, a, err);
}

}

inline std::vector<Rule> parseScript(const std::string& input, std::vector<std::string>& errors)
{

    std::vector<Rule> out;

    const auto report = [&](int line, const std::string& e) { errors.push_back("line " + std::to_string(line) + ": " + e); };

    if (input.size() > kMaxFileBytes)
    {
        report(1, "the file is over 256 KB; not read");
        return out;
    }

    std::string text = input.compare(0, 3, "\xEF\xBB\xBF") == 0 ? input.substr(3) : input;
    std::vector<std::string> variables;
    int checks = 0;
    bool open = false;
    bool bad = false;
    bool skipping = false;
    Rule cur;

    // A finished rule joins the list unless it is bad or would pass a cap. Returns false once the rule cap is reached.
    const auto close = [&]() {

        open = false;

        if (bad) return true;

        auto refs = condRefs(cur.cond);
        std::vector<std::string> fresh;

        for (const auto& a : cur.actions)
        {
            if (a.kind == "set" || a.kind == "add") refs.variables.push_back(paramOf(a, "variable"));
        }

        for (const auto& v : refs.variables)
        {
            if (std::find(variables.begin(), variables.end(), v) == variables.end() && std::find(fresh.begin(), fresh.end(), v) == fresh.end()) fresh.push_back(v);
        }

        if (variables.size() + fresh.size() > (size_t)kMaxVariables)
        {
            report(cur.line, "more than " + std::to_string(kMaxVariables) + " variables in this level's rules");
            return true;
        }

        if (checks + refs.checks > kMaxChecksPerFile)
        {
            report(cur.line, "more than " + std::to_string(kMaxChecksPerFile) + " checks in this level's rules");
            return true;
        }

        if ((int)out.size() >= kMaxRules)
        {
            report(cur.line, "more than " + std::to_string(kMaxRules) + " rules; the rest are not read");
            return false;
        }

        variables.insert(variables.end(), fresh.begin(), fresh.end());
        checks += refs.checks;
        out.push_back(std::move(cur));

        return true;
    };

    int n = 0;

    for (size_t at = 0; at < text.size(); )
    {
        const size_t nl = std::min(text.find('\n', at), text.size());
        std::string line = text.substr(at, nl - at);

        at = nl + 1;
        n++;

        std::replace(line.begin(), line.end(), '\t', ' ');

        if (!line.empty() && line.back() == '\r') line.pop_back();

        line = script::trim(line);

        if (line.empty() || line[0] == '#') continue;

        if (line.size() > kMaxLine)
        {
            report(n, "the line is over " + std::to_string(kMaxLine) + " characters");
            bad = bad || open;
            skipping = !open;
            continue;
        }

        std::string err;

        if (script::header(line))
        {
            if (open) report(cur.line, "no \"end\" for this rule");

            cur = Rule{};
            cur.line = n;
            open = true;
            skipping = false;
            bad = !script::parseHeader(line, cur, err);

            if (bad) report(n, err);
        }
        else if (line == "end")
        {
            if (open)
            {
                if (!close()) return out;
            }
            else if (!skipping) report(n, "\"end\" without a rule");

            skipping = false;
        }
        else if (open)
        {
            Action a;

            if (!bad && !script::parseAction(line, a, err))
            {
                report(n, err);
                bad = true;
            }

            cur.actions.push_back(std::move(a));
        }
        else if (!skipping)
        {
            report(n, "expected \"when\" or \"every\" to start a rule");
            skipping = true;
        }
    }

    if (open) report(cur.line, "no \"end\" for this rule");

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

// Levels whose model package another level also loads, from assets/level/<folder>/packages.json: { "version": 1, "levels": ["lv_2"] }.
// Only stock level folders pass, since a name becomes an asset path.
inline std::vector<std::string> parsePackages(const std::string& text, std::vector<std::string>& errors)
{

    std::vector<std::string> out;
    const auto doc = nlohmann::json::parse(text, nullptr, false);

    if (doc.is_discarded() || !doc.is_object() || !doc.contains("levels") || !doc["levels"].is_array())
    {
        errors.push_back("packages.json is not { \"version\": 1, \"levels\": [...] }");
        return out;
    }

    for (const auto& level : doc["levels"])
    {
        const std::string name = level.is_string() ? level.get<std::string>() : "";
        const bool stock = name.size() == 4 && name.rfind("lv_", 0) == 0 && ((name[3] >= '0' && name[3] <= '9') || (name[3] >= 'a' && name[3] <= 'z'));

        if (stock)
        {
            out.push_back(name);
        }
        else
        {
            errors.push_back("packages.json names \"" + name + "\", which is not a stock level folder");
        }
    }

    return out;
}

// The rules folder of a level: the mod level's folder, else the stock level's (lv_ + base-36 id).
inline std::string rulesFolder(const std::string& modFolder, int level)
{

    if (!modFolder.empty()) return modFolder;

    return std::string("lv_") + (char)(level < 10 ? '0' + level : 'a' + level - 10);
}

// What the runner asks the game each tick. health is a percentage; an unset function reads as 0 / false.
struct Facts {
    std::function<int(const std::string&)> health;
    std::function<bool(int)> flag;
    std::function<int32_t(int)> counter;
};

class Runner {
public:
    // Drops, with one message each: rules naming an object known() rejects (in the when or the condition), objective rules on a
    // level without its own objectives, and rules reading a variable no rule sets.
    std::vector<std::string> reset(std::vector<Rule> rules, const std::function<bool(const std::string&)>& known, bool objectives = true)
    {

        std::vector<std::string> dropped;
        std::vector<std::string> written;

        for (const auto& r : rules)
        {
            for (const auto& a : r.actions)
            {
                if (a.kind == "set" || a.kind == "add") written.push_back(paramOf(a, "variable"));
            }
        }

        rules_.clear();

        for (auto& r : rules)
        {
            const auto refs = condRefs(r.cond);
            const auto unknown = std::find_if(refs.objects.begin(), refs.objects.end(), [&](const std::string& n) { return !known(n); });
            const auto unset = std::find_if(refs.variables.begin(), refs.variables.end(), [&](const std::string& v) { return std::find(written.begin(), written.end(), v) == written.end(); });
            const bool usesObjectives = refs.objectives || r.when == When::ObjectiveDone || r.when == When::ObjectiveFailed
                || std::any_of(r.actions.begin(), r.actions.end(), [](const Action& a) { return a.kind == "objectiveDone" || a.kind == "objectiveFail"; });
            const std::string where = r.line ? "line " + std::to_string(r.line) + ": " : "";

            if (namesObject(r.when) && !known(r.object)) dropped.push_back(where + "rule on \"" + r.object + "\": no such object in the level");
            else if (unknown != refs.objects.end()) dropped.push_back(where + "\"" + *unknown + "\": no such object in the level");
            else if (!objectives && usesObjectives) dropped.push_back(where + "uses objectives, but this level has none of its own");
            else if (unset != refs.variables.end()) dropped.push_back(where + "no rule sets \"" + *unset + "\"");
            else rules_.push_back(std::move(r));
        }

        fired_.assign(rules_.size(), false);
        held_.assign(rules_.size(), false);
        nextAt_.assign(rules_.size(), 0.0f);
        pending_.clear();
        queued_.clear();
        out_.clear();
        vars_.clear();
        destroyed_.clear();
        entered_.clear();
        objDone_ = 0;
        objFailed_ = 0;
        started_ = false;

        return dropped;
    }

    // Remembered for the conditions, and matched against the rules on the next tick.
    void event(const Event& e)
    {

        const auto remember = [&](std::vector<std::string>& list) {

            if (std::find(list.begin(), list.end(), e.object) == list.end()) list.push_back(e.object);
        };

        if (e.when == When::Destroyed) remember(destroyed_);

        if (e.when == When::Enter) remember(entered_);

        if (e.when == When::ObjectiveDone && e.value >= 1 && e.value <= 32) objDone_ |= 1u << (e.value - 1);

        if (e.when == When::ObjectiveFailed && e.value >= 1 && e.value <= 32) objFailed_ |= 1u << (e.value - 1);

        queued_.push_back(e);
    }

    // dt and elapsed are game time; facts answers the game reads.
    void tick(float dt, float elapsed, const Facts& facts)
    {

        std::map<std::string, int> health;

        const auto value = [&](const Atom& a) -> double {

            const auto has = [](const std::vector<std::string>& list, const std::string& n) { return std::find(list.begin(), list.end(), n) != list.end(); };

            switch (a.check)
            {
                case Check::Destroyed: return has(destroyed_, a.name);
                case Check::Entered: return has(entered_, a.name);
                case Check::Health:
                    if (has(destroyed_, a.name)) return 0;

                    if (!health.count(a.name)) health[a.name] = facts.health ? facts.health(a.name) : 0;

                    return health[a.name];
                case Check::ObjectiveDone: return (objDone_ >> (a.index - 1)) & 1u;
                case Check::ObjectiveFailed: return (objFailed_ >> (a.index - 1)) & 1u;
                case Check::Time: return elapsed;
                case Check::Flag: return facts.flag && facts.flag(a.index);
                case Check::Counter: return facts.counter ? facts.counter(a.index) : 0;
                case Check::Variable: return variable(a.name);
            }

            return 0;
        };

        const auto holds = [&](const Rule& r) { return evalCondition(r.cond, value); };
        const auto events = std::move(queued_);

        queued_.clear();

        for (const auto& e : events)
        {
            for (size_t i = 0; i < rules_.size(); i++)
            {
                if (matches(rules_[i], e) && holds(rules_[i])) fire(i);
            }
        }

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

            if (r.when == When::Start && !started_ && holds(r)) fire(i);

            const float at = nextAt_[i] > 0 ? nextAt_[i] : r.seconds;

            // A once-only rule whose "if" fails keeps waiting; a repeating one skips that interval.
            if (r.when == When::After && elapsed >= at && (r.every || !fired_[i]))
            {
                const bool ok = holds(r);

                if (r.every || ok) nextAt_[i] = at + (r.seconds > 0 ? r.seconds : 1e9f);

                if (ok) fire(i);
            }

            if (!polled(r.when)) continue;

            bool now = r.when == When::True;

            if (r.when == When::HealthBelow) now = value({ Check::Health, r.object, 0 }) < r.value;
            else if (r.when == When::FlagIs) now = value({ Check::Flag, "", r.value }) != 0;
            else if (r.when == When::CounterAtLeast) now = value({ Check::Counter, "", r.value }) >= r.count;

            now = now && holds(r);

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

    int32_t variable(const std::string& name) const
    {

        const auto it = vars_.find(name);

        return it == vars_.end() ? 0 : it->second;
    }

private:
    struct Pending {
        size_t rule, next;
        float left;
    };

    static bool matches(const Rule& r, const Event& e)
    {

        if (r.when != e.when || polled(r.when) || r.when == When::Start || r.when == When::After) return false;

        // A custom when without an object matches its event from any object.
        const bool anyObject = r.when == When::Custom && r.object.empty();

        if ((!anyObject && r.object != e.object) || (!r.who.empty() && !e.who.empty() && r.who != e.who) || (r.when == When::Custom && r.custom != e.custom)) return false;

        const bool byValue = r.when == When::ObjectiveDone || r.when == When::ObjectiveFailed;

        return !byValue || r.value == e.value;
    }

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
            const auto& a = acts[k];

            if (a.kind == "wait")
            {
                pending_.push_back({ i, k + 1, std::strtof(paramOf(a, "seconds").c_str(), nullptr) });
                return;
            }

            if (a.kind == "set" || a.kind == "add")
            {
                const int64_t base = a.kind == "add" ? variable(paramOf(a, "variable")) : 0;
                const int64_t sum = base + paramNumber(a, "value", 0);

                vars_[paramOf(a, "variable")] = (int32_t)std::clamp<int64_t>(sum, INT32_MIN, INT32_MAX);
                continue;
            }

            out_.push_back(a);
        }
    }

    std::vector<Rule> rules_;
    std::vector<bool> fired_, held_;
    std::vector<float> nextAt_;
    std::vector<Pending> pending_;
    std::vector<Event> queued_;
    std::vector<Action> out_;
    std::map<std::string, int32_t> vars_;
    std::vector<std::string> destroyed_, entered_;
    unsigned objDone_ = 0, objFailed_ = 0;
    bool started_ = false;
};

}
