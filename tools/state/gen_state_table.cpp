// Generates src/main/state_table.inl from tools/state/state_model.toml after validating it (the same checks as validate() in state_model.py).
// Usage: gen_state_table <state_model.toml> <out.inl>
#include <algorithm>
#include <cstdarg>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <toml++/toml.hpp>

struct Pred {
    int64_t addr, mask, value;
    std::string op;
    std::optional<int64_t> value_hi;
};

struct State {
    std::string id, name, parent, force_slot;
    std::vector<Pred> classify;
    std::map<std::string, int64_t> force_params;
    std::vector<std::string> reachable_from;
};

static std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

static std::string cstr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"') o += '\\';
        o += c;
    }
    return o + "\"";
}

static std::vector<State> load(const char* path) {
    toml::table root = toml::parse_file(path);
    std::vector<State> out;
    const toml::array* arr = root["state"].as_array();
    if (!arr) return out;
    for (const toml::node& n : *arr) {
        const toml::table& e = *n.as_table();
        State s;
        s.id = e["id"].value<std::string>().value();
        s.name = e["name"].value<std::string>().value();
        s.parent = e["parent"].value_or<std::string>("");
        if (const toml::array* c = e["classify"].as_array()) {
            for (const toml::node& pn : *c) {
                const toml::table& p = *pn.as_table();
                s.classify.push_back({p["addr"].value<int64_t>().value(), p["mask"].value<int64_t>().value(),
                                      p["value"].value<int64_t>().value(), p["op"].value<std::string>().value(),
                                      p["value_hi"].value<int64_t>()});
            }
        }
        if (const toml::table* f = e["force"].as_table()) {
            s.force_slot = (*f)["slot"].value<std::string>().value();
            if (const toml::table* params = (*f)["params"].as_table()) {
                for (auto&& [k, v] : *params) s.force_params[std::string(k.str())] = v.value<int64_t>().value();
            }
        }
        if (const toml::array* r = e["reachable_from"].as_array()) {
            for (const toml::node& rn : *r) s.reachable_from.push_back(rn.value<std::string>().value());
        }
        out.push_back(std::move(s));
    }
    return out;
}

static int depth(const std::vector<State>& states, const std::map<std::string, size_t>& idx, const State& s) {
    int d = 0;
    const State* cur = &s;
    while (!cur->parent.empty()) {
        ++d;
        auto it = idx.find(cur->parent);
        if (it == idx.end()) break;
        cur = &states[it->second];
    }
    return d;
}

// Masked-value interval a predicate admits; ne has none, so it can never prove two siblings disjoint.
static std::optional<std::pair<int64_t, int64_t>> interval(const Pred& p) {
    int64_t v = p.value & p.mask;
    if (p.op == "eq" || p.op == "flag") return std::make_pair(v, v);
    if (p.op == "range") return std::make_pair(v, p.value_hi.value_or(p.value) & p.mask);
    return std::nullopt;
}

static bool key_disjoint(const std::vector<Pred>& a, const std::vector<Pred>& b) {
    auto bounds = [](const std::vector<Pred>& ps, int64_t& lo, int64_t& hi) {
        bool any = false;
        lo = INT64_MIN;
        hi = INT64_MAX;
        for (const Pred& p : ps) {
            if (auto i = interval(p)) {
                any = true;
                lo = std::max(lo, i->first);
                hi = std::min(hi, i->second);
            }
        }
        return any;
    };
    int64_t alo, ahi, blo, bhi;
    if (!bounds(a, alo, ahi) || !bounds(b, blo, bhi)) return false;
    if (alo > ahi || blo > bhi) return true;
    return ahi < blo || bhi < alo;
}

using Keyed = std::map<std::pair<int64_t, int64_t>, std::vector<Pred>>;

static Keyed by_key(const std::vector<Pred>& ps) {
    Keyed m;
    for (const Pred& p : ps) m[{p.addr, p.mask}].push_back(p);
    return m;
}

// Siblings are confusable when they share a tested field and no shared field tells them apart.
static bool confusable(const std::vector<Pred>& a, const std::vector<Pred>& b) {
    Keyed ma = by_key(a), mb = by_key(b);
    bool shared = false;
    for (auto& [key, preds] : ma) {
        auto it = mb.find(key);
        if (it == mb.end()) continue;
        shared = true;
        if (key_disjoint(preds, it->second)) return false;
    }
    return shared;
}

static std::vector<std::string> validate(const std::vector<State>& states, const std::map<std::string, size_t>& idx) {
    std::vector<std::string> errs;
    std::set<std::string> seen;
    for (const State& s : states) {
        if (!seen.insert(s.id).second) errs.push_back("duplicate id: " + s.id);
        if (!s.parent.empty() && !idx.count(s.parent)) errs.push_back("state " + s.id + ": parent '" + s.parent + "' does not exist");
        for (const Pred& p : s.classify) {
            if (p.op != "eq" && p.op != "ne" && p.op != "range" && p.op != "flag") errs.push_back("state " + s.id + ": bad op '" + p.op + "'");
            if (p.op == "range" && !p.value_hi) errs.push_back("state " + s.id + ": range predicate missing value_hi");
        }
        if (!s.force_slot.empty() && s.force_slot != "boot_level" && s.force_slot != "menu_screen" && s.force_slot != "demo") errs.push_back("state " + s.id + ": bad force slot '" + s.force_slot + "'");
    }
    std::vector<std::string> parents;
    std::map<std::string, std::vector<const State*>> by_parent;
    for (const State& s : states) {
        if (!by_parent.count(s.parent)) parents.push_back(s.parent);
        by_parent[s.parent].push_back(&s);
    }
    for (const std::string& par : parents) {
        auto& sibs = by_parent[par];
        for (size_t i = 0; i < sibs.size(); ++i) {
            for (size_t j = i + 1; j < sibs.size(); ++j) {
                if (confusable(sibs[i]->classify, sibs[j]->classify)) errs.push_back("ambiguous siblings: '" + sibs[i]->id + "' and '" + sibs[j]->id + "' can both match");
            }
        }
    }
    return errs;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: gen_state_table <state_model.toml> <out.inl>\n");
        return 2;
    }
    std::vector<State> states;
    try {
        states = load(argv[1]);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: %s\n", argv[1], e.what());
        return 1;
    }
    std::map<std::string, size_t> idx;
    for (size_t i = 0; i < states.size(); ++i) idx.emplace(states[i].id, i);
    auto errs = validate(states, idx);
    if (!errs.empty()) {
        std::fprintf(stderr, "descriptor invalid:\n");
        for (auto& e : errs) std::fprintf(stderr, "  %s\n", e.c_str());
        return 1;
    }

    static const std::map<std::string, std::pair<const char*, std::vector<std::string>>> slots = {
        {"boot_level", {"RS_SLOT_BOOT_LEVEL", {"level", "craft"}}},
        {"menu_screen", {"RS_SLOT_MENU_SCREEN", {"screen"}}},
        {"demo", {"RS_SLOT_DEMO", {"demo"}}},
    };
    static const std::map<std::string, const char*> ops = {{"eq", "RS_OP_EQ"}, {"ne", "RS_OP_NE"}, {"range", "RS_OP_RANGE"}, {"flag", "RS_OP_FLAG"}};
    std::vector<std::string> preds, fps, rf, rows;
    for (const State& s : states) {
        size_t pred_off = preds.size();
        for (const Pred& p : s.classify) {
            preds.push_back(fmt("  { 0x%08Xu, 0x%08Xu, %s, 0x%08Xu, 0x%08Xu },", (uint32_t)p.addr, (uint32_t)p.mask, ops.at(p.op),
                                (uint32_t)p.value, (uint32_t)p.value_hi.value_or(0)));
        }
        size_t fp_off = fps.size();
        const char* slot = "RS_SLOT_NONE";
        if (!s.force_slot.empty()) {
            auto& sl = slots.at(s.force_slot);
            slot = sl.first;
            for (auto& key : sl.second) {
                auto it = s.force_params.find(key);
                fps.push_back(fmt("  %lld,", (long long)(it == s.force_params.end() ? -1 : it->second)));
            }
        }
        size_t rf_off = rf.size();
        for (auto& r : s.reachable_from) rf.push_back("  " + cstr(r) + ",");
        auto pit = idx.find(s.parent);
        long parent = pit == idx.end() ? -1 : (long)pit->second;
        rows.push_back(fmt("  { %s, %s, %ld, %d, %zu, %zu, %s, %zu, %zu, %zu, %zu },", cstr(s.id).c_str(), cstr(s.name).c_str(), parent,
                           depth(states, idx, s), pred_off, s.classify.size(), slot, fp_off, fps.size() - fp_off, rf_off,
                           s.reachable_from.size()));
    }
    auto join = [](const std::vector<std::string>& v, const char* empty) {
        if (v.empty()) return std::string(empty);
        std::string o;
        for (size_t i = 0; i < v.size(); ++i) o += (i ? "\n" : "") + v[i];
        return o;
    };
    std::string text = "// GENERATED by tools/state/gen_state_table.cpp from state_model.toml. Do not edit.\n\n";
    text += "static const RsPredicate g_state_predicates[] = {\n" + join(preds, "  {0,0,0,0,0},") + "\n};\n\n";
    text += "static const int32_t g_state_force_params[] = {\n" + join(fps, "  0,") + "\n};\n\n";
    text += "static const char* const g_state_reachable[] = {\n" + join(rf, "  0,") + "\n};\n\n";
    text += "static const RsState g_state_table[] = {\n" + join(rows, "") + "\n};\n\n";
    text += fmt("#define RS_STATE_COUNT %zu\n", states.size());
    std::ofstream o(argv[2], std::ios::binary);
    o << text;
    if (!o) {
        std::fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    std::printf("wrote %s (%zu states)\n", argv[2], states.size());
    return 0;
}
