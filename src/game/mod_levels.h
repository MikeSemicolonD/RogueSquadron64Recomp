#pragma once
// Mod levels: a mod's levels.json lists new levels that play as a stock "template" level with their own folder, cutscenes and text.
#include "game/level_rules.h"
#include "json/json.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace rs64levels {

constexpr int kVoiceNone = -1, kMusicTemplate = -1;
// Highest SELECT LEVEL index: the selector's mask is (1 << (max + 1)) - 1, which wraps at 0x1F.
constexpr int kMaxEntry = 0x1E;
constexpr size_t kMaxFolder = 48;

// A medal tier as the game's medal_info record: at most `minutes`, at least the others (accuracy in %).
struct MedalTier {
    float minutes = 0;
    uint16_t kills = 0, accuracy = 0, friendlies = 0, bonus = 0;
};

// A mod level's own objective (pause-list line). Destroy: counter index >= amount; reach: flag index set; survive: seconds pass.
// Protect fails the mission when its flag is set, TimeLimit when the seconds run out; both show complete on success.
enum class Goal { Destroy, Reach, Protect, Survive, TimeLimit };
struct Objective {
    std::string text;
    Goal goal = Goal::Destroy;
    int index = 0;
    int amount = 0;
    float seconds = 0;
    // Called out when it completes (or fails, for Protect / TimeLimit): a stock voice line and a DAT camera to cut to.
    int voice = kVoiceNone;
    std::string camera;
    // The mod's own recording at assets/level/<folder>/<recording>, played instead of voice.
    std::string recording;
    // Read aloud when the pause screen comes to its line (none by default): a stock voice id or a recording in the level folder.
    int pauseVoice = kVoiceNone;
    std::string pauseRecording;
};
constexpr size_t kMaxObjectives = 4;

// voice: kVoiceNone, or a stock voice id played as the SELECT LEVEL briefing. medals: bronze, silver, gold (without them the template's
// thresholds apply). objectives: when present, the template's mission script does not run.
struct ModLevel {
    std::string mod, id, title, description, folder;
    int tmpl = 0;
    int voice = kVoiceNone;
    bool hasMedals = false;
    MedalTier medals[3];
    // Approved craft (getAvailablePlayerCraftFlags bits), or kTemplateCraft for the template's.
    int craftMask = -1;
    // Opening crawl: kCrawlTemplate (the template's, if it has one), kCrawlNone, or its own text then planet pan-down 0-3.
    int crawl = -2;
    std::string crawlChapter, crawlTitle, crawlBody;
    // A crawl file (assets/crawl/<name>/crawl.json) named by "crawl": "<name>"; crawl stays kCrawlTemplate until one is found.
    std::string crawlName;
    std::vector<Objective> objectives;
    // Starting song: a song id from the music cue table (0x8010B1C0), or kMusicTemplate for the template's.
    int music = kMusicTemplate;
};

// Objective progress from the game's 1-based objective counters and flags (counts[i - 1], flags[i - 1]) and the mission's elapsed time.
struct ObjectiveState {
    unsigned done = 0, failed = 0;
    bool success = false, fail = false;
};
inline ObjectiveState evaluateObjectives(const std::vector<Objective>& objs, const int32_t* counts, const uint8_t* flags, float elapsed) {
    ObjectiveState st;
    bool all = true;
    for (size_t i = 0; i < objs.size(); i++) {
        const auto& o = objs[i];
        bool done = false;
        switch (o.goal) {
            case Goal::Destroy: done = counts[o.index - 1] >= o.amount; break;
            case Goal::Reach: done = flags[o.index - 1] != 0; break;
            case Goal::Survive: done = elapsed >= o.seconds; break;
            case Goal::Protect: st.failed |= (flags[o.index - 1] != 0) << i; continue;
            case Goal::TimeLimit: st.failed |= (elapsed >= o.seconds) << i; continue;
        }
        if (done) st.done |= 1u << i;
        all &= done;
    }
    st.fail = st.failed != 0;
    st.success = all && !st.fail;
    if (st.success) st.done = (1u << objs.size()) - 1;
    return st;
}

constexpr int kCrawlTemplate = -2, kCrawlNone = -1;
// The crawl font (starwars50, charset 0x8003B520) has no '"', '&' or '#': a missing glyph draws garbage, so only these are kept.
inline std::string crawlText(const std::string& s) {
    static const std::string kSafe = " \n!?,.:;'/\\-_()";
    std::string out;
    for (char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || kSafe.find(c) != std::string::npos) out += c;
    }
    return out;
}
// One crawl string as the game's (buildModelViewerTextPage): "*}" centred small line, "*{" centred title, then the wrapped body.
inline std::string crawlString(const std::string& chapter, const std::string& title, const std::string& body) {
    std::string s;
    if (!chapter.empty()) s += "*}" + crawlText(chapter) + "\n\n";
    return s + "*{" + crawlText(title) + "\n\n" + crawlText(body);
}
inline std::string crawlString(const ModLevel& l) {
    return crawlString(l.crawlChapter, l.crawlTitle, l.crawlBody);
}

// A camera key: seconds from the crawl's start, position, and rotation in degrees (applied as a DAT rotation: x, then y, then z).
struct CrawlKey {
    float at = 0;
    float pos[3] = {};
    float rot[3] = {};
};

// A crawl file, assets/crawl/<name>/crawl.json. planet: the stock backdrop 0-3 (with ownPlanet, the one a missing own backdrop
// falls back to); replaces: -1, or the stock chapter 0-3 this crawl plays instead of; camera: its keys (empty = the game's camera).
struct Crawl {
    std::string name, folder, chapter, title, body;
    int planet = 0;
    bool ownPlanet = false;
    int replaces = -1;
    std::vector<CrawlKey> camera;
};

// The keyed camera at second s: quadratic ease in/out between keys (as the stock pan, quadraticEaseInOut 0x800662E0), held outside them.
inline bool crawlCameraAt(const std::vector<CrawlKey>& keys, float s, float pos[3], float rot[3]) {
    if (keys.empty()) return false;
    size_t j = 0;
    while (j + 1 < keys.size() && s >= keys[j + 1].at) j++;
    const CrawlKey& a = keys[j];
    const CrawlKey& b = keys[j + 1 < keys.size() ? j + 1 : j];
    const float span = b.at - a.at, k = span > 0 ? std::clamp((s - a.at) / span, 0.0f, 1.0f) : 0.0f;
    const float e = k <= 0.5f ? 2 * k * k : 1 - 2 * (1 - k) * (1 - k);
    for (int i = 0; i < 3; i++) {
        pos[i] = a.pos[i] + (b.pos[i] - a.pos[i]) * e;
        rot[i] = a.rot[i] + (b.rot[i] - a.rot[i]) * e;
    }
    return true;
}

inline std::string crawlString(const Crawl& c) {
    return crawlString(c.chapter, c.title, c.body);
}

// The crawl file that plays for level (the level being played; a mod level plays as its template), or nullptr for the game's own
// or a mod level's inline crawl. Later crawls (later-loaded mods) win.
inline const Crawl* pickCrawl(int level, const ModLevel* modLevel, const std::vector<Crawl>& crawls) {
    if (modLevel && !modLevel->crawlName.empty()) {
        for (auto it = crawls.rbegin(); it != crawls.rend(); ++it) {
            if (it->name == modLevel->crawlName) return &*it;
        }
        return nullptr;
    }
    if (modLevel && modLevel->crawl != kCrawlTemplate) return nullptr;
    const int chapter = level == 0 ? 0 : level == 5 ? 1 : level == 10 ? 2 : level == 15 ? 3 : -1;
    for (auto it = crawls.rbegin(); chapter >= 0 && it != crawls.rend(); ++it) {
        if (it->replaces == chapter) return &*it;
    }
    return nullptr;
}

inline std::optional<Crawl> parseCrawl(const std::string& name, const std::string& text, std::vector<std::string>& errors) {
    const auto doc = nlohmann::json::parse(text, nullptr, false);
    const std::string where = "crawl " + name;
    if (doc.is_discarded() || !doc.is_object()) {
        errors.push_back(where + ": crawl.json is not a JSON object");
        return std::nullopt;
    }
    Crawl c;
    c.name = name;
    c.chapter = rs64rules::field(doc, "chapter", std::string(), where, errors);
    c.title = rs64rules::field(doc, "title", std::string(), where, errors);
    c.body = rs64rules::field(doc, "body", std::string(), where, errors);
    c.ownPlanet = doc.contains("planet") && doc["planet"].is_string() && doc["planet"].get<std::string>() == "own";
    const int planet = c.ownPlanet ? rs64rules::field(doc, "base", 0, where, errors) : rs64rules::field(doc, "planet", 0, where, errors);
    const int replaces = rs64rules::field(doc, "replaces", -1, where, errors);
    c.planet = planet >= 0 && planet <= 3 ? planet : 0;
    if (planet != c.planet) errors.push_back(where + ": the planet is 0-3; 0 used");
    c.replaces = replaces >= -1 && replaces <= 3 ? replaces : -1;
    if (replaces != c.replaces) errors.push_back(where + ": \"replaces\" is a chapter 0-3; ignored");
    if (name.size() > 15) errors.push_back(where + ": the name is longer than 15 characters, so its own files are not found");
    const auto vec3 = [](const nlohmann::json& v, float out[3]) {
        if (!v.is_array() || v.size() != 3) return false;
        for (int i = 0; i < 3; i++) {
            if (!v[i].is_number()) return false;
            out[i] = v[i].get<float>();
        }
        return true;
    };
    const auto& cam = doc.contains("camera") ? doc["camera"] : nlohmann::json();
    for (size_t i = 0; cam.is_array() && i < cam.size(); i++) {
        CrawlKey k;
        const auto& e = cam[i];
        const bool ok = e.is_object() && e.contains("at") && e["at"].is_number() && e.contains("pos") && vec3(e["pos"], k.pos) && (!e.contains("rot") || vec3(e["rot"], k.rot));
        if (!ok) {
            errors.push_back(where + ": camera key " + std::to_string(i + 1) + " needs \"at\" seconds and \"pos\" [x, y, z] (and \"rot\" [x, y, z]); skipped");
            continue;
        }
        k.at = e["at"].get<float>();
        c.camera.push_back(k);
    }
    std::stable_sort(c.camera.begin(), c.camera.end(), [](const CrawlKey& a, const CrawlKey& b) { return a.at < b.at; });
    return c;
}

// The manifest name a crawl's own file is served under, inside the crawl scene's directories (a manifest takes added files only
// under its own top directories): crawl/<name>/backdrop -> backdrop/<name>, crawl/<name>/dat -> frontend/starwars/<name>.
inline std::string crawlServedPath(const std::string& key) {
    const size_t slash = key.find('/', 6);
    if (key.rfind("crawl/", 0) != 0 || slash == std::string::npos) return std::string();
    const std::string name = key.substr(6, slash - 6), file = key.substr(slash + 1);
    return file == "backdrop" ? "backdrop/" + name : file == "dat" ? "frontend/starwars/" + name : std::string();
}

// The name a lookup uses while crawl c plays: its own backdrop (with planet "own") and its own scene DAT when it ships them (have).
inline std::optional<std::string> crawlAsset(const Crawl& c, const std::string& lookedUp, const std::set<std::string>& have) {
    const bool backdrop = c.ownPlanet && lookedUp.rfind("backdrop/planet_", 0) == 0;
    const std::string file = backdrop ? c.folder + "/backdrop" : lookedUp == "frontend/starwars/dat" ? c.folder + "/dat" : std::string();
    if (file.empty() || !have.count(file)) return std::nullopt;
    return crawlServedPath(file);
}

constexpr int kTemplateCraft = -1;
// Craft names in the game's craft order (front_TXT 0x18.., the bits of the available-craft mask).
inline const char* const kCraftNames[] = { "xwing", "ywing", "awing", "vwing", "speeder", "falcon", "tie", "t16", "naboo" };
// The mask for craft names; unknown names are skipped (bad gets them).
inline int craftMaskOf(const std::vector<std::string>& names, std::vector<std::string>* bad = nullptr) {
    int mask = 0;
    for (const auto& n : names) {
        const auto it = std::find_if(std::begin(kCraftNames), std::end(kCraftNames), [&](const char* c) { return n == c; });
        if (it != std::end(kCraftNames)) mask |= 1 << (it - std::begin(kCraftNames));
        else if (bad) bad->push_back(n);
    }
    return mask;
}

// The three 0xC medal_info records (bronze, silver, gold): big-endian f32 minutes, then u16 kills, accuracy, friendlies, bonus (left 0: the host keeps the template's).
inline std::vector<uint8_t> medalRecords(const ModLevel& l) {
    std::vector<uint8_t> out;
    const auto u16 = [&](uint16_t v) {
        out.push_back((uint8_t)(v >> 8));
        out.push_back((uint8_t)v);
    };
    for (const auto& t : l.medals) {
        uint32_t f;
        std::memcpy(&f, &t.minutes, 4);
        for (int s = 24; s >= 0; s -= 8) out.push_back((uint8_t)(f >> s));
        u16(t.kills);
        u16(t.accuracy);
        u16(t.friendlies);
        u16(t.bonus);
    }
    return out;
}

// Best medal per mod level ("mod/id" -> 0 none .. 3 gold), kept apart from the game's medals in its own file.
class MedalStore {
public:
    int best(const std::string& key) const {
        const auto it = best_.find(key);
        return it == best_.end() ? 0 : it->second;
    }
    // True when the medal beats the stored one.
    bool record(const std::string& key, int medal) {
        if (medal <= best(key)) return false;
        best_[key] = medal;
        return true;
    }
    void load(const std::string& text) {
        best_.clear();
        const auto doc = nlohmann::json::parse(text, nullptr, false);
        if (!doc.is_object()) return;
        for (const auto& [k, v] : doc.items()) {
            if (v.is_number_integer()) best_[k] = v.get<int>();
        }
    }
    std::string save() const { return nlohmann::json(best_).dump(1); }

private:
    std::map<std::string, int> best_;
};

// Templates are the playable stock levels 0x00-0x12. Folders are one plain name (assets/level/<folder>/).
inline std::vector<ModLevel> parseModLevels(const std::string& mod, const std::string& text, std::vector<std::string>& errors) {
    std::vector<ModLevel> out;
    const auto doc = nlohmann::json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.contains("levels") || !doc["levels"].is_array()) {
        errors.push_back(mod + ": levels.json is not { \"levels\": [...] }");
        return out;
    }
    for (const auto& e : doc["levels"]) {
        ModLevel l;
        l.mod = mod;
        l.id = e.value("id", std::string());
        l.title = e.value("title", std::string());
        l.description = e.value("description", std::string());
        l.folder = e.value("folder", std::string());
        l.tmpl = e.value("template", -1);
        if (e.contains("voice") && e["voice"].is_number_integer()) {
            l.voice = e["voice"].get<int>();
        }
        if (e.contains("crawl") && e["crawl"].is_boolean() && !e["crawl"].get<bool>()) l.crawl = kCrawlNone;
        if (e.contains("crawl") && e["crawl"].is_string()) l.crawlName = e["crawl"].get<std::string>();
        if (e.contains("crawl") && e["crawl"].is_object()) {
            const auto& c = e["crawl"];
            l.crawlChapter = c.value("chapter", std::string());
            l.crawlTitle = c.value("title", std::string());
            l.crawlBody = c.value("body", std::string());
            const int planet = c.value("planet", 0);
            l.crawl = planet >= 0 && planet <= 3 ? planet : 0;
        }
        if (e.contains("craft") && e["craft"].is_array()) {
            std::vector<std::string> names, bad;
            for (const auto& c : e["craft"]) {
                if (c.is_string()) names.push_back(c.get<std::string>());
            }
            l.craftMask = craftMaskOf(names, &bad);
            for (const auto& b : bad) errors.push_back(mod + ": level '" + l.id + "' unknown craft '" + b + "'");
            if (!l.craftMask) l.craftMask = kTemplateCraft;
        }
        if (e.contains("medals") && e["medals"].is_object()) {
            const auto& m = e["medals"];
            const char* tiers[3] = { "bronze", "silver", "gold" };
            l.hasMedals = true;
            for (int i = 0; i < 3; i++) {
                if (!m.contains(tiers[i]) || !m[tiers[i]].is_object()) {
                    l.hasMedals = false;
                    continue;
                }
                const auto& t = m[tiers[i]];
                l.medals[i] = { t.value("time", 0.0f), t.value("kills", (uint16_t)0), t.value("accuracy", (uint16_t)0), t.value("friendlies", (uint16_t)0) };
            }
            if (!l.hasMedals) errors.push_back(mod + ": level '" + l.id + "' medals need bronze, silver and gold; the template's apply");
        }
        if (e.contains("music") && e["music"].is_number_integer()) {
            const int m = e["music"].get<int>();

            if (m >= 0 && m <= 0xFF) l.music = m;
            else errors.push_back(mod + ": level '" + l.id + "' music must be a song id 0-255; the template's plays");
        }
        if (e.contains("objectives") && e["objectives"].is_array()) {
            static const std::map<std::string, Goal> kGoals = { { "destroy", Goal::Destroy }, { "reach", Goal::Reach }, { "protect", Goal::Protect }, { "survive", Goal::Survive }, { "timelimit", Goal::TimeLimit } };
            const size_t before = errors.size();
            bool completes = false;
            for (const auto& j : e["objectives"]) {
                const auto g = kGoals.find(j.value("goal", std::string()));
                if (g == kGoals.end()) {
                    errors.push_back(mod + ": level '" + l.id + "' objective goal must be destroy, reach, protect, survive or timelimit");
                    continue;
                }
                Objective o{ j.value("text", std::string()), g->second };
                o.index = j.value(o.goal == Goal::Destroy ? "counter" : "flag", 0);
                o.amount = j.value("count", 0);
                o.seconds = j.value("seconds", 0.0f);
                o.voice = j.value("voice", kVoiceNone);
                o.camera = j.value("camera", std::string());
                o.recording = j.value("recording", std::string());
                o.pauseVoice = j.value("pauseVoice", kVoiceNone);
                o.pauseRecording = j.value("pauseRecording", std::string());
                const bool timed = o.goal == Goal::Survive || o.goal == Goal::TimeLimit;
                if (timed ? o.seconds <= 0 : (o.index < 1 || o.index > 128 || (o.goal == Goal::Destroy && o.amount < 1))) {
                    errors.push_back(mod + ": level '" + l.id + "' objective '" + o.text + "' needs " + (timed ? "seconds" : o.goal == Goal::Destroy ? "a counter 1-128 and a count" : "a flag 1-128"));
                    continue;
                }
                completes |= o.goal == Goal::Destroy || o.goal == Goal::Reach || o.goal == Goal::Survive;
                l.objectives.push_back(std::move(o));
            }
            if (errors.size() == before && (l.objectives.size() > kMaxObjectives || !completes)) errors.push_back(mod + ": level '" + l.id + "' needs 1-4 objectives, one of them destroy, reach or survive");
            if (errors.size() != before) l.objectives.clear();
        }
        const bool plain = !l.folder.empty() && l.folder.size() <= kMaxFolder && l.folder.find_first_of("/\\") == std::string::npos && l.folder != "." && l.folder != "..";
        if (l.tmpl < 0 || l.tmpl > 0x12 || !plain || l.id.empty()) {
            errors.push_back(mod + ": level '" + l.id + "' needs a template 0-18 and a plain folder name of at most 48 characters");
            continue;
        }
        out.push_back(std::move(l));
    }
    return out;
}

// The asset a lookup should use while the mod level plays: the template's level file "level/lv_<t>/<f>" -> "level/<folder>/<f>" and its
// "cuts/id<t>_intro|extro" -> "cuts/<folder>_intro|extro", when the mod ships that file (have); empty = keep the template's.
inline std::string remapPath(const std::string& path, const ModLevel& l, const std::set<std::string>& have) {
    const char c = (char)(l.tmpl < 10 ? '0' + l.tmpl : 'a' + l.tmpl - 10);
    const std::string dir = std::string("level/lv_") + c + "/", cut = "cuts/id" + std::to_string(l.tmpl) + "_";
    std::string out;
    if (path.rfind(dir, 0) == 0) {
        out = "level/" + l.folder + "/" + path.substr(dir.size());
    } else if (path == cut + "intro" || path == cut + "extro") {
        out = "cuts/" + l.folder + "_" + path.substr(cut.size());
    }
    return have.count(out) ? out : std::string();
}

// SELECT LEVEL text in its fonts: letters uppercased (no lowercase glyphs) and anything a candidate font lacks dropped (drawing stops
// at the first missing glyph). The set is what normal10, normal11 and starwars50 all have (charsets at 0x8003B494 / B4DC / B520).
inline std::string menuText(const std::string& s) {
    static const std::string kSafe = " \n!()_-/\\,.:'";
    std::string out;
    for (char c : s) {
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || kSafe.find(c) != std::string::npos) out += c;
    }
    return out;
}

// SELECT LEVEL indices for n mod levels after the highest stock entry shown.
struct Entries {
    int base = 0;
    int count = 0;
};
inline Entries layout(int stock_max, size_t n) {
    return { stock_max + 1, (int)std::min<size_t>(n, (size_t)std::max(0, kMaxEntry - stock_max)) };
}

}
