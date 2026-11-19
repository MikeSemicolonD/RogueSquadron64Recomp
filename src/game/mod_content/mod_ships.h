#pragma once
// Custom ships: a mod's ships.json adds hangar ships. Each flies as a stock "template" craft (its handling, weapons and sounds) with its
// own model, placed in the hangar scene by the mod, and its own name.
#include "game/mod_content/mod_levels.h"
#include "json/json.hpp"
#include <algorithm>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace rs64ships {

// The hangar's object array (*0x800CDA5C) has 56 slots and fills 48, so eight ships fit; its bay -> craft table runs in host RAM.
constexpr size_t kMaxShips = 8;
constexpr size_t kIdMax = 9;

// The special button. Toggle (the S-foils): a press engages it over `seconds`, a press or the brake releases it; speed multiplies the
// craft's speed. Timed (the boost): engaged for `seconds`, then a `cooldown`; speed multiplies its top speed. guns false locks the guns.
enum class SpecialMode { None, Toggle, Timed };

struct Special {
    SpecialMode mode = SpecialMode::None;
    float speed = 1.0f;
    float seconds = 1.0f;
    float cooldown = 0.0f;
    bool guns = true;
    bool custom = false;
};

struct Ship {
    std::string mod, id, name;
    int tmpl = 0;
    // The camera stop: positions along the hangar's two camera routes (the 0x800CC414 / 0x800CC430 entries of a stock stop).
    float stop[2] = { 0, 0 };
    // Craft behaviours from any craft ("sfoils" X-wing, "boost" V-wing, "aim" A-wing); unlisted = the template's own.
    std::set<std::string> behaviours;
    bool ownBehaviours = false;
    // ships.json "special": the button's own settings, in place of the sfoils / boost presets.
    Special special;
};

constexpr const char* kBehaviours[] = { "sfoils", "boost", "aim" };
constexpr int kBehaviourCraft[] = { 0, 3, 2 };

inline bool hasBehaviour(const Ship& s, const std::string& b)
{

    if (s.ownBehaviours) return s.behaviours.count(b) > 0;

    for (size_t i = 0; i < std::size(kBehaviours); i++)
    {
        if (b == kBehaviours[i]) return s.tmpl == kBehaviourCraft[i];
    }

    return false;
}

// The special button a ship flies with: its own "special", else the X-wing's S-foils (1 s, 1.5x speed, guns locked) or the V-wing's boost
// (6 s at 4/3 of top speed, 6 s cooldown capped at 2/3) when it has that behaviour, else none.
inline Special specialOf(const Ship& s)
{

    if (s.special.custom) return s.special;

    if (hasBehaviour(s, "sfoils")) return { SpecialMode::Toggle, 1.5f, 1.0f, 0.0f, false, false };

    if (hasBehaviour(s, "boost")) return { SpecialMode::Timed, 6.0f / 4.5f, 6.0f, 6.0f, true, false };

    return {};
}

// The game splits a trailing digit off a model path, so pl_crafts/ship1 would load as pl_crafts/ship.
inline bool validId(const std::string& id)
{

    if (id.empty() || id.size() > kIdMax || (id.back() >= '0' && id.back() <= '9')) return false;

    return std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

inline int craftId(const std::string& name)
{

    const auto it = std::find_if(std::begin(rs64levels::kCraftNames), std::end(rs64levels::kCraftNames), [&](const char* c) { return name == c; });

    return it == std::end(rs64levels::kCraftNames) ? -1 : (int)(it - std::begin(rs64levels::kCraftNames));
}

// { "ships": [ { "id", "name", "template", "stop": [u, v], "behaviours": [...] } ] }; bad entries are reported and left out.
inline std::vector<Ship> parseShips(const std::string& mod, const std::string& text, std::vector<std::string>& errors)
{

    std::vector<Ship> out;
    const auto j = nlohmann::json::parse(text, nullptr, false);

    if (!j.is_object() || !j.contains("ships") || !j["ships"].is_array())
    {
        errors.push_back(mod + ": ships.json is not { \"ships\": [...] }");

        return out;
    }

    for (const auto& e : j["ships"])
    {
        Ship s;

        s.mod = mod;
        s.id = rs64rules::field(e, "id", std::string(), mod + ": ships.json", errors);

        const std::string where = mod + ": ship \"" + s.id + "\"";

        s.name = rs64rules::field(e, "name", std::string(), where, errors);
        s.tmpl = craftId(rs64rules::field(e, "template", std::string(), where, errors));

        // The TIE (its own hangar reveal, its model under imp_stuff/) and the T-16 (no hangar bay) can't be templates.
        if (s.tmpl == 6 || s.tmpl == 7) s.tmpl = -1;

        const auto stop = e.is_object() && e.contains("stop") ? e["stop"] : nlohmann::json::array();
        const bool stopOk = stop.is_array() && stop.size() == 2 && stop[0].is_number() && stop[1].is_number();

        if (!validId(s.id) || s.name.empty() || s.tmpl < 0 || !stopOk)
        {
            errors.push_back(mod + ": ship \"" + s.id + "\" needs an id (1-9 of a-z 0-9 _), a name, a template craft (xwing ywing awing vwing speeder falcon naboo) and a stop [u, v]; skipped");
            continue;
        }

        s.stop[0] = stop[0].get<float>();
        s.stop[1] = stop[1].get<float>();

        const auto beh = e.contains("behaviours") ? e["behaviours"] : nlohmann::json();

        if (beh.is_array())
        {
            s.ownBehaviours = true;

            for (const auto& b : beh)
            {
                const std::string name = b.is_string() ? b.get<std::string>() : std::string();
                const bool known = std::find_if(std::begin(kBehaviours), std::end(kBehaviours), [&](const char* k) { return name == k; }) != std::end(kBehaviours);

                if (known)
                {
                    s.behaviours.insert(name);
                    continue;
                }

                errors.push_back(mod + ": ship \"" + s.id + "\" has an unknown behaviour \"" + name + "\" (sfoils boost aim); ignored");
            }

            if (s.behaviours.count("sfoils") && s.behaviours.erase("boost"))
            {
                errors.push_back(mod + ": ship \"" + s.id + "\" can't have both sfoils and boost (they share a button); boost dropped");
            }
        }

        // "special": { "mode": "toggle" | "timed" | "none", "speed", "seconds", "cooldown", "guns" }, each clamped to a sane range.
        if (e.contains("special") && e["special"].is_object())
        {
            const auto& sp = e["special"];
            const std::string mode = rs64rules::field(sp, "mode", std::string("toggle"), where, errors);

            s.special.custom = true;
            s.special.mode = mode == "timed" ? SpecialMode::Timed : (mode == "none" ? SpecialMode::None : SpecialMode::Toggle);

            if (mode != "toggle" && mode != "timed" && mode != "none") errors.push_back(where + ": special mode \"" + mode + "\" is toggle, timed or none; toggle used");

            const bool timed = s.special.mode == SpecialMode::Timed;

            s.special.speed = std::clamp(rs64rules::field(sp, "speed", timed ? 6.0f / 4.5f : 1.5f, where, errors), 0.25f, 3.0f);
            s.special.seconds = std::clamp(rs64rules::field(sp, "seconds", timed ? 6.0f : 1.0f, where, errors), 0.1f, 60.0f);
            s.special.cooldown = std::clamp(rs64rules::field(sp, "cooldown", timed ? 6.0f : 0.0f, where, errors), 0.0f, 120.0f);
            s.special.guns = rs64rules::field(sp, "guns", timed, where, errors);
        }

        out.push_back(std::move(s));
    }

    return out;
}

// A camera stop and its position u along the camera's route (camroute_0).
struct StopAt {
    int stop;
    float u;
};

// The stop a turn reaches from `from`: the stops in order of u round the route (the stock turning order: dir +1 is the stock right
// step's way), skipping those skip() rejects. -1 when `from` is not among them (the TIE's own reveal, stop 7).
template <class Skip>
inline int nextStop(int from, int dir, std::vector<StopAt> stops, Skip skip)
{

    std::sort(stops.begin(), stops.end(), [](const StopAt& a, const StopAt& b) { return a.u < b.u; });

    const auto it = std::find_if(stops.begin(), stops.end(), [&](const StopAt& s) { return s.stop == from; });

    if (it == stops.end()) return -1;

    const int n = (int)stops.size();
    const int i = (int)(it - stops.begin());

    for (int step = 1; step < n; step++)
    {
        const int s = stops[((i + dir * step) % n + n) % n].stop;

        if (!skip(s)) return s;
    }

    return from;
}

// The mission's per-craft file names (tables 0x8010B3A0 exterior, 0x8010A0FC cockpit, 0x80109CB0 HUD icon) in craft order; a ship's are
// <id>, c_<id> and ov_<id>.
constexpr const char* kExterior[] = { "xwing", "ywing", "awing", "vwing", "snowspeeder", "falcon", "tieinter", "t16", "koelsch" };
constexpr const char* kCockpit[] = { "c_xwg", "c_ywg", "c_awg", "c_vwg", "c_spdr", "c_fal", "c_tie", "c_t16", "c_kolsch" };
constexpr const char* kIcon[] = { "ov_xwing", "ov_ywing", "ov_awing", "ov_vwing", "ov_spdr", "ov_falcon", "ov_intercept", "ov_t16", "ov_koelsch" };

// Craft id -> the per-craft model setup type dispatchOverlaySetupByParentAssetName (0x80053C04) picks by the model's name.
constexpr int kSetupType[] = { 2, 3, 0, 1, 4, 5, 6, 7, 8 };

inline std::string firstToOne(const std::string& s) {
    return s.empty() ? s : "1" + s.substr(1);
}

// While the ship flies: a template file the mod has the ship's own of -> the ship's (empty = keep). Files are first looked up with a "1"
// first letter; with only the plain file, that lookup goes to the ship's missing "1" name so the game falls back to its plain one.
inline std::string shipAsset(const std::string& path, const Ship& s, const std::set<std::string>& have)
{

    const size_t slash = path.rfind('/');
    const std::string dir = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
    const std::string leaf = path.substr(dir.size());
    const std::string own[] = { s.id, "c_" + s.id, "ov_" + s.id };
    const char* tmpl[] = { kExterior[s.tmpl], kCockpit[s.tmpl], kIcon[s.tmpl] };

    for (int i = 0; i < 3; i++)
    {
        const std::string t = tmpl[i];

        for (const bool one : { false, true })
        {
            const std::string base = one ? firstToOne(t) : t;

            if (leaf.compare(0, base.size(), base) != 0) continue;

            const std::string rest = leaf.substr(base.size());

            if (!rest.empty() && rest[0] != '_') continue;

            const std::string plain = dir + own[i] + rest;
            const std::string oneName = dir + firstToOne(own[i]) + rest;

            if (!one) return have.count(plain) ? plain : std::string();

            return have.count(oneName) || have.count(plain) ? oneName : std::string();
        }
    }

    return {};
}

}
