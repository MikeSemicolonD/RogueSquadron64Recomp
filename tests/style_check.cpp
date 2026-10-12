// Style checks for src/main from AGENTS.md: no comment after code, comment blocks of at most 2 lines, env reads through recomp::dbg::env_*, one statement per line.
// Usage: style_check <repo root> [--update-baseline]. Exits 1 on findings not in tools/style/baseline.txt.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::string strip(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

// First 60 code points, so findings match the baseline's text for non-ASCII lines.
static std::string head60(const std::string& s) {
    size_t i = 0;
    for (int n = 0; i < s.size(); ++i) {
        if (((unsigned char)s[i] & 0xC0) != 0x80 && n++ == 60) break;
    }
    return s.substr(0, i);
}

static bool bare_getenv(const std::string& code) {
    for (size_t p = code.find("getenv("); p != std::string::npos; p = code.find("getenv(", p + 1)) {
        char c = p ? code[p - 1] : ' ';
        if (c != ':' && c != '_' && !std::isalnum((unsigned char)c)) return true;
    }
    return false;
}

static std::vector<std::string> findings(const fs::path& root) {
    static const std::regex side(R"([;{)]\s+//)");
    static const std::regex two(R"(;\s+(if|while)\s*\()");
    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator(root / "src" / "main")) {
        auto ext = e.path().extension();
        if (e.is_regular_file() && (ext == ".cpp" || ext == ".h")) files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    std::vector<std::string> out;
    for (auto& f : files) {
        std::string rel = "src/main/" + f.filename().string();
        std::ifstream in(f, std::ios::binary);
        std::string line, first;
        int run = 0;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::string s = strip(line);
            if (s.rfind("//", 0) == 0) {
                if (++run == 1) first = s;
                if (run == 3) out.push_back(rel + ": comment block over 2 lines: " + head60(first));
                continue;
            }
            run = 0;
            std::string code = line;
            if (std::count(line.begin(), line.end(), '"') % 2 == 0) code = line.substr(0, line.find('"'));
            if (std::regex_search(code, side)) out.push_back(rel + ": comment after code: " + head60(s));
            if (bare_getenv(code) && code.find("env_") == std::string::npos) out.push_back(rel + ": getenv outside env_* helpers: " + head60(s));
            if (std::regex_search(code, two) && s.rfind("for", 0) != 0) out.push_back(rel + ": two statements on one line: " + head60(s));
        }
    }
    // A [[patches.hook]] text is one call into src/main/toml_hooks.c, in one of two forms, so the hook's C stays out of the toml.
    static const std::regex hook_form(R"(^text = '\{ (if \(rs64_th_\w+\(rdram, ctx\)\) return;|rs64_th_\w+\(rdram, ctx\);) \}'$)");
    std::ifstream toml(root / "rogue_squadron.toml", std::ios::binary);
    std::string line;
    while (std::getline(toml, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("text", 0) == 0 && !std::regex_match(line, hook_form)) out.push_back("rogue_squadron.toml: hook text is not a single rs64_th_ call: " + head60(line));
    }
    return out;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: style_check <repo root> [--update-baseline]\n");
        return 2;
    }
    fs::path root = argv[1];
    fs::path baseline = root / "tools" / "style" / "baseline.txt";
    auto v = findings(root);
    std::set<std::string> cur(v.begin(), v.end());
    if (argc > 2 && std::strcmp(argv[2], "--update-baseline") == 0) {
        std::ofstream o(baseline, std::ios::binary);
        for (auto& x : cur) o << x << '\n';
        std::printf("baseline: %zu findings\n", cur.size());
        return 0;
    }
    std::set<std::string> base;
    std::ifstream in(baseline, std::ios::binary);
    for (std::string l; std::getline(in, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        base.insert(l);
    }
    size_t fresh = 0, known = 0;
    for (auto& x : cur) {
        if (base.count(x)) {
            ++known;
        } else {
            std::printf("%s\n", x.c_str());
            ++fresh;
        }
    }
    std::printf("style: %zu new, %zu baselined\n", fresh, known);
    return fresh ? 1 : 0;
}
