// Post-processes RSPRecomp output for the shared boot and MusyX ucodes, in place and idempotently.
// Usage: fixup_factor5_ucode <recompiled.c>...
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

static bool ident_char(char c) {
    return std::isalnum((unsigned char)c) || c == '_';
}

static std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string l; std::getline(in, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
    }
    return lines;
}

static std::string join_lines(const std::vector<std::string>& lines) {
    std::string out;
    for (auto& l : lines) out += l + "\n";
    return out;
}

static std::string indent_of(const std::string& l) {
    return l.substr(0, l.find_first_not_of(" \t"));
}

static bool insert_after(std::vector<std::string>& lines, const std::string& anchor, const std::vector<std::string>& add) {
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i] == anchor) {
            lines.insert(lines.begin() + i + 1, add.begin(), add.end());
            return true;
        }
    }
    return false;
}

static std::string fixup(const std::string& original) {
    std::vector<std::string> lines = split_lines(original);

    // RSPRecomp emits gotos to labels outside the recompiled range for branches past the end of IMEM; they are never taken.
    std::set<std::string> labels;
    for (auto& l : lines) {
        size_t n = 0;
        while (n < l.size() && ident_char(l[n])) ++n;
        if (n && !std::isdigit((unsigned char)l[0]) && n < l.size() && l[n] == ':' && l.find_first_not_of(" \t", n + 1) == std::string::npos) labels.insert(l.substr(0, n));
    }
    for (auto& l : lines) {
        for (size_t p = l.find("goto "); p != std::string::npos; p = l.find("goto ", p + 1)) {
            if (p && ident_char(l[p - 1])) continue;
            size_t b = l.find_first_not_of(' ', p + 5), e = b;
            while (e < l.size() && ident_char(l[e])) ++e;
            if (e == b || e >= l.size() || l[e] != ';' || labels.count(l.substr(b, e - b))) continue;
            l.replace(p, e + 1 - p, "return RspExitReason::ImemOverrun;");
        }
    }

    // The boot ucode's `jr $7` to 0x1080 hands off to the next ucode; RSPRecomp can't follow it, so keep its register dump for unexpected targets only.
    const std::string silence = "/* fixup: silence expected boot->main JR */";
    if (original.find("factor5_boot(") != std::string::npos && original.find(silence) == std::string::npos) {
        for (size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].find("printf(\"Unhandled jump target 0x%04X in microcode factor5_boot,") == std::string::npos) continue;
            std::string ind = indent_of(lines[i]);
            lines[i] = "    " + lines[i];
            lines.insert(lines.begin() + i, ind + "if (jump_target != 0x1080) {  " + silence);
            for (size_t j = i + 2; j + 1 < lines.size(); ++j) {
                if (lines[j] == "    return RspExitReason::UnhandledJumpTarget;" && lines[j + 1] == "}") {
                    lines.insert(lines.begin() + j, "    }");
                    break;
                }
            }
            break;
        }
    }

    // MusyX's top-level `jr $ra` has r31 = 0, which the dispatch maps back to the ucode entry; treat it as task complete and cap any other runaway.
    const std::string cap = "/* fixup: musyx runaway cap */";
    bool musyx = original.find("musyx_audio(") != std::string::npos;
    if (musyx && original.find(cap) == std::string::npos) {
        insert_after(lines, "    RSP rsp{};", {"    long rs64_iter = 0;  " + cap});
        insert_after(lines, "do_indirect_jump:", {
            "    if (jump_target == 0) {  " + cap,
            "        return RspExitReason::Broke;",
            "    }",
            "    if (++rs64_iter > (1L<<20)) {  " + cap,
            "        static int rs64_musyx_cap = 0;",
            "        if (++rs64_musyx_cap <= 4) {",
            "            fprintf(stderr, \"[musyx-cap] fired #%d, last jump_target=0x%04X\\n\", rs64_musyx_cap, jump_target);",
            "            fflush(stderr);",
            "        }",
            "        return RspExitReason::Broke;",
            "    }",
        });
    }

    // The per-voice mix loop at L_13EC never exits when r29 is 0 (no active voice); a sample end of 0 is never a real voice.
    const std::string unblock = "/* no-voice unblock";
    if (musyx && original.find(unblock) == std::string::npos) {
        for (auto& l : lines) {
            if (l.substr(l.find_first_not_of(" \t") == std::string::npos ? 0 : l.find_first_not_of(" \t")) == "goto L_13EC;") {
                l = indent_of(l) + "if (r29 != 0) goto L_13EC;  " + unblock + ": r29==0 (null voice) exits instead of spinning */";
                break;
            }
        }
    }

    std::string text = join_lines(lines);
    if (text.find("#include <cstdio>") == std::string::npos) text = "#include <cstdio>\n" + text;
    return text;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: fixup_factor5_ucode <recompiled.c>...\n");
        return 2;
    }
    for (int i = 1; i < argc; ++i) {
        std::ifstream in(argv[i], std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "fixup_factor5_ucode: cannot read %s\n", argv[i]);
            return 1;
        }
        std::string original((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        std::string text = fixup(original);
        if (text == original) continue;
        std::ofstream out(argv[i], std::ios::binary);
        out << text;
        if (!out) {
            std::fprintf(stderr, "fixup_factor5_ucode: cannot write %s\n", argv[i]);
            return 1;
        }
        std::fprintf(stderr, "fixup_factor5_ucode: fixed %s\n", argv[i]);
    }
    return 0;
}
