#include "../src/lockstep/lockstep_core.h"
#include <algorithm>
#include "check.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>

using namespace rs64::ls;

static void test_header_roundtrip() {
    SessionHeader h;
    h.seed = 7; h.level = 5; h.craft = 2; h.difficulty = 1; h.flags0 = 0x00012203u;
    h.controller = 3; h.expansion = 1; h.cheats0 = 0x40u; h.cheats1 = 0; h.naboo = 0xDEADBEEFu; h.build = "test build";
    h.secondary = 9; h.cruise_bits = 0x3F000000u;
    for (size_t i = 0; i < h.pilot.size(); ++i) {
        h.pilot[i] = (uint8_t)(i * 7 + 1);
    }
    const std::string text = format_header(h);
    CHECK(text.rfind("rs64-ls v2\n", 0) == 0);
    CHECK(text.size() >= 4 && text.compare(text.size() - 4, 4, "---\n") == 0);
    SessionHeader g;
    CHECK(parse_header(text, &g));
    CHECK(g.seed == 7 && g.level == 5 && g.craft == 2 && g.difficulty == 1 && g.flags0 == 0x00012203u);
    CHECK(g.controller == 3 && g.expansion == 1 && g.cheats0 == 0x40u && g.naboo == 0xDEADBEEFu && g.build == "test build");
    CHECK(g.secondary == 9 && g.cruise_bits == 0x3F000000u && g.pilot == h.pilot);
}

static void test_header_rejects_bad_magic() {
    SessionHeader g;
    CHECK(!parse_header("rs64-input v4\n---\n", &g));
    CHECK(!parse_header("rs64-ls v2\nseed=1\n", &g));
    CHECK(!parse_header("rs64-ls v1\nseed=1\n---\n", &g));
}

static void test_frame_roundtrip() {
    FrameInput f;
    f.frame = 1234;
    const uint8_t pad[6] = {0x80, 0x01, 0x7F, 0x81, 0x00, 0x05};
    memcpy(f.pad, pad, 6);
    f.throttle = 200;
    const std::string line = format_frame(f);
    CHECK(line == "1234 80017F810005 200");
    FrameInput g;
    CHECK(parse_frame((line + "\n").c_str(), &g));
    CHECK(g.frame == 1234 && memcmp(g.pad, pad, 6) == 0 && g.throttle == 200);
}

static void test_parse_frame_rejects_partial() {
    FrameInput g;
    CHECK(!parse_frame("1234 80017F81\n", &g));
    CHECK(!parse_frame("1234 80017F810005\n", &g));
    CHECK(!parse_frame("", &g));
    CHECK(!parse_frame("abc 80017F810005 1\n", &g));
}

static void test_parse_frame_requires_complete_line() {
    FrameInput g;
    CHECK(!parse_frame("1234 80017F810005 12", &g));
    CHECK(parse_frame("1234 80017F810005 127\n", &g));
    CHECK(g.throttle == 127);
}

static void test_song_event_roundtrip() {
    CHECK(format_song_event(2940, false) == "S 2940 0");
    CHECK(format_song_event(7, true) == "S 7 1");
    uint32_t frame = 0;
    bool active = true;
    CHECK(parse_song_event("S 2940 0\n", &frame, &active));
    CHECK(frame == 2940 && !active);
    CHECK(parse_song_event("S 7 1\n", &frame, &active));
    CHECK(frame == 7 && active);
    CHECK(!parse_song_event("S 7 1", &frame, &active));
    CHECK(!parse_song_event("S 7 2\n", &frame, &active));
    CHECK(!parse_song_event("7 000000000100 255\n", &frame, &active));
    FrameInput f;
    CHECK(!parse_frame("S 7 1\n", &f));
}

static void test_parsers_accept_crlf() {
    SessionHeader h;
    h.seed = 3;
    std::string text = format_header(h);
    std::string crlf;
    for (char c : text) {
        if (c == '\n') {
            crlf += '\r';
        }
        crlf += c;
    }
    SessionHeader g;
    CHECK(parse_header(crlf, &g));
    CHECK(g.seed == 3);
    FrameInput f;
    CHECK(parse_frame("12 80017F810005 127\r\n", &f));
    CHECK(f.frame == 12 && f.throttle == 127);
    CHECK(!parse_frame("12 80017F810005 12\r", &f));
    uint32_t frame = 0;
    bool active = false;
    CHECK(parse_song_event("S 9 1\r\n", &frame, &active));
    CHECK(frame == 9 && active);
}

static void test_throttle_quantization() {
    CHECK(quantize_throttle(-1.0f) == 0xFF);
    CHECK(quantize_throttle(0.0f) == 0);
    CHECK(quantize_throttle(1.0f) == 254);
    CHECK(quantize_throttle(2.0f) == 254);
    CHECK(quantize_throttle(0.5f) == 127);
    CHECK(dequantize_throttle(0xFF) < 0.0f);
    CHECK(dequantize_throttle(254) == 1.0f);
    CHECK(quantize_throttle(dequantize_throttle(127)) == 127);
}

static void test_delay_line() {
    DelayLine d(2);
    uint8_t p[6];
    for (int i = 1; i <= 4; ++i) {
        memset(p, i, 6);
        d.push_pop(p);
        const uint8_t expect = (i <= 2) ? 0 : (uint8_t)(i - 2);
        for (int k = 0; k < 6; ++k) {
            CHECK(p[k] == expect);
        }
    }
}

#include <vector>

static void put_be32(std::vector<uint8_t>& ram, uint32_t kseg0, uint32_t v) {
    const uint32_t off = kseg0 - 0x80000000u;
    for (int i = 0; i < 4; ++i) {
        ram[(off + i) ^ 3] = (uint8_t)(v >> (24 - 8 * i));
    }
}

static void test_fnv1a_known_value() {
    CHECK(fnv1a((const uint8_t*)"a", 1) == 0xaf63dc4c8601ec8cull);
}

static void test_hash_is_stable_and_region_sensitive() {
    std::vector<uint8_t> ram(kRdramSpan, 0);
    const FrameHash a = hash_frame(ram.data());
    const FrameHash b = hash_frame(ram.data());
    CHECK(a.combined == b.combined);
    CHECK(a.parts.size() == hash_part_names().size());
    put_be32(ram, 0x80003470, 0x12345678u);
    const FrameHash c = hash_frame(ram.data());
    CHECK(c.parts[0] != a.parts[0]);
    for (size_t i = 1; i < c.parts.size(); ++i) {
        CHECK(c.parts[i] == a.parts[i]);
    }
    CHECK(c.combined != a.combined);
}

static void test_hash_ignores_language_byte() {
    std::vector<uint8_t> ram(kRdramSpan, 0);
    const FrameHash a = hash_frame(ram.data());
    ram[(0x130B46u) ^ 3] = 2;
    const FrameHash b = hash_frame(ram.data());
    CHECK(a.combined == b.combined);
}

static void test_hash_ignores_player_text_id() {
    std::vector<uint8_t> ram(kRdramSpan, 0);
    const FrameHash a = hash_frame(ram.data());
    ram[(0x137DB8u + 0x215) ^ 3] = 0x46;
    for (uint32_t off = 0x240; off < 0x250; ++off) {
        ram[(0x137DB8u + off) ^ 3] = 0x46;
    }
    const FrameHash b = hash_frame(ram.data());
    CHECK(a.combined == b.combined);
    ram[(0x137DB8u + 0x23F) ^ 3] = 0x01;
    const FrameHash c = hash_frame(ram.data());
    CHECK(c.combined != a.combined);
    ram[(0x137DB8u + 0x23F) ^ 3] = 0x00;
    ram[(0x137DB8u + 0x250) ^ 3] = 0x01;
    const FrameHash d = hash_frame(ram.data());
    CHECK(d.combined != a.combined);
}

static void test_npc_walk() {
    std::vector<uint8_t> ram(kRdramSpan, 0);
    const uint32_t table = 0x80200000u;
    const uint32_t ctx = 0x80210000u;
    const uint32_t pos = 0x80220000u;
    put_be32(ram, 0x80130BB0, table);
    put_be32(ram, table + 5 * 8, ctx);
    put_be32(ram, ctx + 0, 0x800B5434u);
    put_be32(ram, ctx + 8, pos);
    const FrameHash a = hash_frame(ram.data());
    put_be32(ram, pos + 4, 0x3F800000u);
    const FrameHash b = hash_frame(ram.data());
    const size_t npcs = hash_part_names().size() - 1;
    CHECK(a.parts[npcs] != b.parts[npcs]);
    put_be32(ram, ctx + 8, 0x12345678u);
    const FrameHash c = hash_frame(ram.data());
    CHECK(c.parts[npcs] != b.parts[npcs]);
}

static void test_hash_line_format() {
    FrameHash h;
    h.combined = 0x1ull;
    h.parts = {0x2ull, 0x3ull};
    CHECK(format_hash_line(9, h) == "9 0000000000000001 0000000000000002 0000000000000003");
    CHECK(format_hash_legend().rfind("# frame combined rng mission settings", 0) == 0);
}

static void put_u8(std::vector<uint8_t>& ram, uint32_t kseg0, uint8_t v) {
    ram[(kseg0 - 0x80000000u) ^ 3] = v;
}

static void test_read_session_uses_settings_level() {
    std::vector<uint8_t> ram(kRdramSpan, 0);
    put_u8(ram, 0x80130B40, 7);
    put_be32(ram, 0x80130B70, 3);
    put_u8(ram, 0x80130B41, 2);
    put_u8(ram, 0x80130B43, 9);
    put_u8(ram, 0x80130B11, 5);
    put_u8(ram, 0x80130B2F, 0x0A);
    const SessionHeader h = read_session(ram.data(), 1);
    CHECK(h.level == 7 && h.craft == 2 && h.secondary == 9);
    CHECK(h.pilot[0x01] == 5 && h.pilot[0x1F] == 0x0A);
}

static void test_apply_session_restores_pilot_state() {
    std::vector<uint8_t> a(kRdramSpan, 0);
    put_u8(a, 0x80130B40, 7);
    put_u8(a, 0x80130B41, 2);
    put_u8(a, 0x80130B42, 1);
    put_u8(a, 0x80130B43, 9);
    put_u8(a, 0x80130B45, 3);
    put_u8(a, 0x80130B63, 1);
    put_be32(a, 0x80130B4C, 0x00019D9Bu);
    put_be32(a, 0x80130B58, 0x40u);
    put_be32(a, 0x80130B68, 0xDEADBEEFu);
    for (uint32_t i = 0; i < 0x30; ++i) {
        put_u8(a, 0x80130B10 + i, (uint8_t)(i + 1));
    }
    const SessionHeader h = read_session(a.data(), 1);
    std::vector<uint8_t> b(kRdramSpan, 0);
    put_u8(b, 0x80130B40, 7);
    put_u8(b, 0x80130B41, 2);
    apply_session(b.data(), h);
    const SessionHeader g = read_session(b.data(), 1);
    CHECK(g.difficulty == 1 && g.secondary == 9 && g.controller == 3 && g.expansion == 1);
    CHECK(g.flags0 == 0x00019D9Bu && g.cheats0 == 0x40u && g.naboo == 0xDEADBEEFu);
    CHECK(g.pilot == h.pilot);
}

static void test_dump_regions() {
    std::vector<uint8_t> ram(kRdramSpan, 0);
    put_be32(ram, 0x80003470, 0xA1B2C3D4u);
    const std::string d = dump_regions(ram.data());
    CHECK(d.rfind("rng A1B2C3D4\n", 0) == 0);
    CHECK(d.find("\nplayer0 ") != std::string::npos);
    CHECK(d.find("npcs") == std::string::npos);
}






static void test_pause_targets() {
    // ROGUESQ_PAUSE_AT: clock[:frame], comma separated; no frame means 0.
    const auto t = parse_pause_targets("menu.options, crawl:2595,cutscene:120,,mission:3");
    CHECK(t.size() == 4);
    CHECK(t[0].clock == "menu.options" && t[0].frame == 0);
    CHECK(t[1].clock == "crawl" && t[1].frame == 2595);
    CHECK(t[2].clock == "cutscene" && t[2].frame == 120 && t[3].clock == "mission" && t[3].frame == 3);
    CHECK(parse_pause_targets("").empty());

    // A clock matches its own name and, at a dot, its sub-states.
    CHECK(pause_clock_matches("menu", "menu.options") && pause_clock_matches("menu.options", "menu.options"));
    CHECK(!pause_clock_matches("menu.opt", "menu.options") && !pause_clock_matches("menu.options", "menu") && !pause_clock_matches("crawl", "cutscene"));
}

static void test_follow_step() {
    // Mirroring the host live: (cursor, state, host cursor, host state, host picked). States: 2 browse, 3 briefing.
    // Browse: move toward the host's cursor; open the briefing once the host has it open (or picked).
    CHECK(follow_step(0, 2, 2, 2, false) == kFollowRight);
    CHECK(follow_step(3, 2, 1, 2, false) == kFollowLeft);
    CHECK(follow_step(2, 2, 2, 2, false) == kFollowNone);
    CHECK(follow_step(2, 2, 2, 3, false) == kFollowConfirm);
    CHECK(follow_step(2, 2, 2, 2, true) == kFollowConfirm);
    // Briefing: confirm once the host picked; close it when the host closed theirs or is on another level.
    CHECK(follow_step(2, 3, 2, 3, false) == kFollowNone);
    CHECK(follow_step(2, 3, 2, 3, true) == kFollowConfirm);
    CHECK(follow_step(2, 3, 2, 2, false) == kFollowBack);
    CHECK(follow_step(1, 3, 2, 3, false) == kFollowBack);
    // Anything else (fly-in, exiting): wait.
    CHECK(follow_step(2, 4, 2, 3, true) == kFollowNone);
    CHECK(follow_step(0, 1, 2, 2, false) == kFollowNone);
}

static void test_cutscene_skip_detection() {
    // Running cutscene (gate 100 of end 590) that stops: skipped. One that reaches its end: not.
    CHECK(cutscene_skipped(true, 100, 590, false));
    CHECK(!cutscene_skipped(true, 575, 590, false));
    CHECK(!cutscene_skipped(true, 100, 590, true));
    CHECK(!cutscene_skipped(false, 100, 590, false));
}

static void test_hash_extra_part_appended() {
    const std::vector<HashPart> extra = {{"mod", [](const uint8_t* r, void*) { return (uint64_t)r[(0x80B40010u - 0x80000000u) ^ 3]; }, nullptr}};
    CHECK(hash_part_names(extra).back() == "mod");
    CHECK(hash_part_names(extra).size() == hash_part_names().size() + 1);
    std::vector<uint8_t> ram(kRdramSpan, 0);
    const FrameHash a = hash_frame(ram.data(), extra);
    CHECK(a.parts.size() == hash_part_names(extra).size());
    ram[(0x80B40010u - 0x80000000u) ^ 3] = 1;
    CHECK(hash_frame(ram.data(), extra).combined != a.combined);
    CHECK(hash_frame(ram.data()).combined == hash_frame(std::vector<uint8_t>(kRdramSpan, 0).data()).combined);
}

static void test_env_parsers() {
    CHECK(parse_seed("0x1234") == 0x1234u);
    CHECK(parse_seed("77") == 77u);
    CHECK(parse_seed("0") == 0u);
    CHECK(parse_seed("abc") == 0u);
    CHECK(parse_seed("12abc") == 0u);
    CHECK(parse_seed("") == 0u);
    CHECK(parse_seed(nullptr) == 0u);
    CHECK(clamp_delay(0) == 1);
    CHECK(clamp_delay(-5) == 1);
    CHECK(clamp_delay(61) == 60);
    CHECK(clamp_delay(3) == 3);
    CHECK(clamp_delay(60) == 60);
}

static void test_moved_ranges_default_to_none() {
    CHECK(moved_ranges().empty() && relocated(0x80138060u) == 0x80138060u);
    set_moved_ranges({{0x80100000u, 0x80100010u, 0x00A00000u}});
    CHECK(relocated(0x8010000Fu) == 0x80B0000Fu && relocated(0x80100010u) == 0x80100010u);
    set_moved_ranges({});
    CHECK(relocated(0x8010000Fu) == 0x8010000Fu);
}

int main() {
    test_moved_ranges_default_to_none();
#ifdef _WIN32
    // Report failed checks on stderr instead of the Debug CRT's modal dialog.
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    test_env_parsers();
    test_header_roundtrip();
    test_header_rejects_bad_magic();
    test_frame_roundtrip();
    test_parse_frame_rejects_partial();
    test_parse_frame_requires_complete_line();
    test_song_event_roundtrip();
    test_parsers_accept_crlf();
    test_throttle_quantization();
    test_delay_line();
    test_fnv1a_known_value();
    test_hash_is_stable_and_region_sensitive();
    test_hash_ignores_language_byte();
    test_hash_ignores_player_text_id();
    test_npc_walk();
    test_hash_line_format();
    test_dump_regions();
    test_read_session_uses_settings_level();
    test_apply_session_restores_pilot_state();
    test_hash_extra_part_appended();
    test_cutscene_skip_detection();
    test_follow_step();
    test_pause_targets();
    printf("lockstep_core_test OK\n");
    return 0;
}
