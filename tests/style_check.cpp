// Style checks for src/, mods/multiplayer-native/src and relay-server/src from AGENTS.md (plus the mod's base-header allowlist): no comment after code, comment blocks of at most 2 lines, a blank line before a comment that follows a struct or class member, env reads through recomp::dbg::env_*, one statement per line. The web JS gets the first and last plus its own (docs/engine/web-js-style.md).
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

static const std::regex kSideComment(R"([;{)]\s+//)");
static const std::regex kTwoStatements(R"(;\s+(if|while)\s*\()");
static const std::string kMpDir = "mods/multiplayer-native/src";

static std::string strip(const std::string& s) {

    size_t b = 0;
    size_t e = s.size();

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

static void drop_cr(std::string& line) {

    if (!line.empty() && line.back() == '\r') line.pop_back();
}

static bool bare_getenv(const std::string& code)
{

    for (size_t p = code.find("getenv("); p != std::string::npos; p = code.find("getenv(", p + 1))
    {
        const char c = p ? code[p - 1] : ' ';

        if (c != ':' && c != '_' && !std::isalnum((unsigned char)c))
        {
            return true;
        }
    }

    return false;
}

// One C++ file: the comment rules, the mod's header allowlist, env reads and one statement per line.
static void check_cpp_file(const fs::path& root, const std::string& dir, const fs::path& f, std::vector<std::string>& out)
{

    static const std::regex include_re(R"re(^\s*#\s*include\s+"([^"]+)")re");
    static const std::regex char_lit(R"('(\\.|[^'\\])')");
    static const std::regex type_head(R"(^(template\s*<.*>\s*)?(struct|class)\s+[\w:]+[^;(]*$)");

    // Base-game headers the multiplayer mod may include; anything else it needs goes through the host API (mods/multiplayer-native/CMakeLists.txt lists the same files).
    static const std::set<std::string> mp_allowed = {"lockstep/lockstep_core.h", "util/debug_logs.h", "util/rdram_words.h", "util/settings_file.h", "recomp.h", "rs64/host_api.h", "rs64/mp_transport.h", "rs64/mp_channel.h", "rs64/camera_shots.h", "rs64/ships.h", "json/json.hpp", "relay_protocol.h"};
    const std::string rel = fs::relative(f, root).generic_string();
    std::ifstream in(f, std::ios::binary);
    std::string line;
    std::string first;
    std::string prev;
    int run = 0;

    // Open braces, 's' for a struct or class body and 'o' for any other block, so a comment can be told to sit among members.
    std::vector<char> braces;
    bool pending_type = false;

    while (std::getline(in, line))
    {
        drop_cr(line);

        const std::string s = strip(line);

        if (s.rfind("//", 0) == 0)
        {
            if (++run == 1) first = s;

            if (run == 3)
            {
                out.push_back(rel + ": comment block over 2 lines: " + head60(first));
            }

            // A comment under a member declaration starts a new group, so a blank line goes above it.
            const bool after_member = (run == 1) && !braces.empty() && (braces.back() == 's') && !prev.empty() && (prev.back() == ';');

            if (after_member)
            {
                out.push_back(rel + ": no blank line before a comment after a member: " + head60(s));
            }

            prev = s;
            continue;
        }

        run = 0;
        prev = s;

        const std::string braced = std::regex_replace(line.substr(0, std::min(line.find("//"), line.find('"'))), char_lit, "''");
        const bool type_line = std::regex_search(strip(braced), type_head);
        bool first_brace = true;

        for (char ch : braced) {
            if (ch == '{') {
                braces.push_back(((type_line || pending_type) && first_brace) ? 's' : 'o');
                first_brace = false;
            } else if (ch == '}' && !braces.empty()) {
                braces.pop_back();
            }
        }

        if (!strip(braced).empty()) pending_type = type_line && first_brace;

        std::smatch inc;
        const bool foreign = (dir == kMpDir) && std::regex_search(line, inc, include_re) && !mp_allowed.count(inc[1]) && !fs::exists(root / dir / inc[1].str());

        if (foreign)
        {
            out.push_back(rel + ": includes a base-game header outside the allowed list: " + inc[1].str());
        }

        std::string code = line;

        if (std::count(line.begin(), line.end(), '"') % 2 == 0) code = line.substr(0, line.find('"'));

        if (std::regex_search(code, kSideComment))
        {
            out.push_back(rel + ": comment after code: " + head60(s));
        }

        if (bare_getenv(code) && code.find("env_") == std::string::npos)
        {
            out.push_back(rel + ": getenv outside env_* helpers: " + head60(s));
        }

        if (std::regex_search(code, kTwoStatements) && s.rfind("for", 0) != 0)
        {
            out.push_back(rel + ": two statements on one line: " + head60(s));
        }
    }
}

// One JS line with its strings and regex literals blanked, so only code is checked.
static std::string js_code(const std::string& line)
{

    std::string code;
    char q = 0;

    for (size_t i = 0; i < line.size(); ++i)
    {
        const char c = line[i];

        if (q) {
            if (c == '\\') ++i;
            else if (c == q) q = 0;

            continue;
        }

        const bool regex_start = (c == '/') && (i + 1 < line.size()) && (line[i + 1] != '/') && (line[i + 1] != '*') &&
            (code.empty() || std::strchr("(,=:[!&|?;{}", strip(code).empty() ? '(' : strip(code).back()));

        if (c == '\'' || c == '"' || c == '`') q = c;
        else if (regex_start) q = '/';
        else code += c;
    }

    return code;
}

// One web JS file (docs/engine/web-js-style.md): no comment after code, one statement per line, brace-less bodies on their own line, one declaration per statement.
static void check_js_file(const fs::path& root, const fs::path& f, std::vector<std::string>& out)
{

    static const std::regex header(R"(^(else\s+)?(if|while|for)\s*\()");
    static const std::regex declaration(R"(^(export\s+)?(const|let|var)\s)");
    const std::string rel = fs::relative(f, root).generic_string();
    std::ifstream in(f, std::ios::binary);
    std::string line;

    while (std::getline(in, line))
    {
        drop_cr(line);

        const std::string s = strip(line);

        if (s.rfind("//", 0) == 0) continue;

        const std::string code = js_code(line);

        if (std::regex_search(code, kSideComment))
        {
            out.push_back(rel + ": comment after code: " + head60(s));
        }

        if (std::regex_search(code, kTwoStatements) && s.rfind("for", 0) != 0)
        {
            out.push_back(rel + ": two statements on one line: " + head60(s));
        }

        std::string c = strip(code);

        if (c.rfind("} ", 0) == 0) c = strip(c.substr(1));

        // A brace-less if / else / loop body goes on the next line.
        std::smatch hm;

        if (std::regex_search(c, hm, header))
        {
            int depth = 0;
            size_t i = hm.length(0) - 1;

            for (; i < c.size(); ++i) {
                if (c[i] == '(') ++depth;
                else if (c[i] == ')' && --depth == 0) break;
            }

            const std::string rest = i < c.size() ? strip(c.substr(i + 1)) : "";

            if (!rest.empty() && rest[0] != '{' && rest != ";")
            {
                out.push_back(rel + ": body on the if / loop line: " + head60(s));
            }
        }
        else if (c.rfind("else ", 0) == 0 && c.rfind("else if", 0) != 0 && strip(c.substr(4))[0] != '{')
        {
            out.push_back(rel + ": body on the else line: " + head60(s));
        }

        // One declaration per const / let / var statement.
        if (!std::regex_search(c, declaration)) continue;

        int depth = 0;

        for (char ch : c) {
            if (ch == '(' || ch == '[' || ch == '{') ++depth;
            else if (ch == ')' || ch == ']' || ch == '}') --depth;
            else if (ch == ';' && depth == 0) break;
            else if (ch == ',' && depth == 0) {
                out.push_back(rel + ": several declarations in one statement: " + head60(s));
                break;
            }
        }
    }
}

static std::vector<std::string> findings(const fs::path& root)
{

    std::vector<std::pair<std::string, fs::path>> files;
    std::vector<std::string> out;

    for (const std::string& dir : {std::string("src"), kMpDir, std::string("relay-server/src")}) {
        for (auto& e : fs::recursive_directory_iterator(root / dir)) {
            const auto ext = e.path().extension();

            if (e.is_regular_file() && (ext == ".cpp" || ext == ".h")) files.push_back({dir, e.path()});
        }
    }

    files.push_back({"tests", root / "tests" / "style_check.cpp"});
    std::sort(files.begin(), files.end());

    for (auto& [dir, f] : files) {
        check_cpp_file(root, dir, f, out);
    }

    // Web JS (tools/web): the generated and vendored files are skipped.
    std::vector<fs::path> js;

    for (auto it = fs::recursive_directory_iterator(root / "tools/web"); it != fs::recursive_directory_iterator(); ++it)
    {
        if (it->is_directory() && it->path().filename() == "node_modules") {
            it.disable_recursion_pending();
            continue;
        }

        const auto ext = it->path().extension();
        const auto name = it->path().filename();
        const bool skipped = (name == "voice_cats.js") || (name == "coi-serviceworker.js") || (name == "rs64tint.js");

        if (it->is_regular_file() && (ext == ".js" || ext == ".mjs") && !skipped) js.push_back(it->path());
    }

    std::sort(js.begin(), js.end());

    for (auto& f : js) {
        check_js_file(root, f, out);
    }

    // A [[patches.hook]] text is one call into src/game/toml_hooks.c, in one of two forms, so the hook's C stays out of the toml.
    static const std::regex hook_form(R"(^text = '\{ (if \(rs64_th_\w+\(rdram, ctx\)\) return;|rs64_th_\w+\(rdram, ctx\);) \}'$)");
    std::ifstream toml(root / "rogue_squadron.toml", std::ios::binary);
    std::string line;

    while (std::getline(toml, line))
    {
        drop_cr(line);

        if (line.rfind("text", 0) == 0 && !std::regex_match(line, hook_form))
        {
            out.push_back("rogue_squadron.toml: hook text is not a single rs64_th_ call: " + head60(line));
        }
    }

    return out;
}

int main(int argc, char** argv) {

    if (argc < 2) {
        std::fprintf(stderr, "usage: style_check <repo root> [--update-baseline]\n");
        return 2;
    }

    const fs::path root = argv[1];
    const fs::path baseline = root / "tools" / "style" / "baseline.txt";
    const auto v = findings(root);
    const std::set<std::string> cur(v.begin(), v.end());

    if (argc > 2 && std::strcmp(argv[2], "--update-baseline") == 0) {
        std::ofstream o(baseline, std::ios::binary);

        for (auto& x : cur) o << x << '\n';

        std::printf("baseline: %zu findings\n", cur.size());
        return 0;
    }

    std::set<std::string> base;
    std::ifstream in(baseline, std::ios::binary);

    for (std::string l; std::getline(in, l);) {
        drop_cr(l);
        base.insert(l);
    }

    size_t fresh = 0;
    size_t known = 0;

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
