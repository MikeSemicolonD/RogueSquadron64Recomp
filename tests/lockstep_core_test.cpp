#include "../src/main/lockstep_core.h"
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

static void test_r4_relocates_with_its_own_delta() {
    CHECK(relocated(0x80138930u) == 0x80B58930u);
    CHECK(relocated(0x80138D0Fu) == 0x80B58D0Fu);
    CHECK(relocated(0x8013892Fu) == 0x8013892Fu);
    CHECK(relocated(0x80138D10u) == 0x80138D10u);
    std::vector<uint8_t> ram(kRdramSpan, 0);
    poison_moved(ram.data());
    auto at = [&](uint32_t a) { return ram[(a - 0x80000000u) ^ 3]; };
    CHECK(at(0x80138930u) == kRelocPoison);
    CHECK(at(0x80138D0Fu) == kRelocPoison);
    CHECK(at(0x80138D10u) == 0);
}

static void test_relocated_maps_only_moved_ranges() {
    CHECK(relocated(0x80138058u) == 0x80B38058u);
    CHECK(relocated(0x80138060u) == 0x80B38060u);
    CHECK(relocated(0x80138837u) == 0x80B38837u);
    CHECK(relocated(0x80138838u) == 0x80138838u);
    CHECK(relocated(0x80138E5Bu) == 0x80138E5Bu);
    CHECK(relocated(0x80138E90u) == 0x80B38E90u);
    CHECK(relocated(0x8013901Fu) == 0x80B3901Fu);
    CHECK(relocated(0x80139020u) == 0x80139020u);
    CHECK(relocated(0x80137DB8u) == 0x80137DB8u);
}

static void test_poison_covers_only_moved_ranges() {
    std::vector<uint8_t> ram(kRdramSpan, 0);
    poison_moved(ram.data());
    CHECK(first_unpoisoned(ram.data()) == 0);
    auto at = [&](uint32_t a) { return ram[(a - 0x80000000u) ^ 3]; };
    CHECK(at(0x80138057u) == 0);
    CHECK(at(0x80138058u) == kRelocPoison);
    CHECK(at(0x80138837u) == kRelocPoison);
    CHECK(at(0x80138838u) == 0);
    CHECK(at(0x80138E5Bu) == 0);
    CHECK(at(0x80138E5Cu) == kRelocPoison);
    CHECK(at(0x8013901Fu) == kRelocPoison);
    CHECK(at(0x80139020u) == 0);
}

static void test_first_unpoisoned_reports_lowest_write() {
    std::vector<uint8_t> ram(kRdramSpan, 0);
    poison_moved(ram.data());
    ram[(0x80138E90u - 0x80000000u) ^ 3] = 0;
    ram[(0x80138061u - 0x80000000u) ^ 3] = 0;
    CHECK(first_unpoisoned(ram.data()) == 0x80138061u);
    ram[(0x80138061u - 0x80000000u) ^ 3] = kRelocPoison;
    CHECK(first_unpoisoned(ram.data()) == 0x80138E90u);
}

static void test_hash_reads_relocated_objective_counts() {
    std::vector<uint8_t> ram(kRdramSpan, 0);
    const FrameHash a = hash_frame(ram.data());
    ram[(0x80B38060u - 0x80000000u) ^ 3] = 1;
    const FrameHash b = hash_frame(ram.data());
    CHECK(a.combined != b.combined);
    ram[(0x80138060u - 0x80000000u) ^ 3] = 1;
    const FrameHash c = hash_frame(ram.data());
    CHECK(b.combined == c.combined);
}

static void test_pad2_bytes_masks_start_and_marks_connected() {
    FrameInput f;
    f.pad[0] = 0x90;
    f.pad[1] = 0x20;
    f.pad[2] = 0x05;
    f.pad[3] = 0xFB;
    f.pad[4] = 0;
    f.pad[5] = 0x33;
    uint8_t out[6];
    pad2_bytes(&f, out);
    const uint8_t want[6] = {0x80, 0x20, 0x05, 0xFB, 1, 0};
    CHECK(memcmp(out, want, 6) == 0);
}

static void test_shared_life_outcomes() {
    CHECK(shared_life(3, false) == LifeOutcome::Spend);
    CHECK(shared_life(2, false) == LifeOutcome::Spend);
    CHECK(shared_life(1, false) == LifeOutcome::Out);
    CHECK(shared_life(0, false) == LifeOutcome::GameOver);
    CHECK(shared_life(0, true) == LifeOutcome::Ignore);
    CHECK(shared_life(2, true) == LifeOutcome::Ignore);
}

static void test_spectate_pad_keeps_only_start() {
    uint8_t pad[6] = {0xFF, 0xFF, 0x50, 0xB0, 1, 0};
    spectate_pad(pad, 0, 0x2000);
    CHECK(pad[0] == 0x10 && pad[1] == 0 && pad[2] == 0 && pad[3] == 0 && pad[4] == 1 && pad[5] == 0);
    uint8_t idle[6] = {0x80, 0x00, 0x00, 0x00, 1, 0};
    spectate_pad(idle, 0, 0x2000);
    CHECK(idle[0] == 0);
    // The survivor's fire button (Z = 0x2000 here) comes through; their other buttons don't.
    uint8_t fire[6] = {0x00, 0x00, 0x00, 0x00, 1, 0};
    spectate_pad(fire, 0x2000 | 0x8000 | 0x0004, 0x2000);
    CHECK(fire[0] == 0x20 && fire[1] == 0);
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

static PuppetState at_x(float x, float vx) {
    PuppetState s;
    s.pos = {x, 0.0f, 0.0f};
    s.fwd = {0.0f, 0.0f, 1.0f};
    s.down = {0.0f, 1.0f, 0.0f};
    s.vel = {vx, 0.0f, 0.0f};
    return s;
}

static void test_interp_puppet_midpoint_and_extrapolation() {
    Puppet p;
    p.set_interp(0.1);
    // Sent at 0 and 0.1 s, each arriving 0.05 s later: offset 0.05.
    p.on_state(at_x(0.0f, 10.0f), 0.05, 0.0);
    p.on_state(at_x(1.0f, 10.0f), 0.15, 0.1);
    // now 0.2 -> render time 0.2 - 0.05 - 0.1 = 0.05: halfway.
    CHECK(std::fabs(p.sample(0.2).pos.x - 0.5f) < 1e-4f);
    // Past the newest sample: extrapolated with its velocity, capped at the coast time.
    CHECK(std::fabs(p.sample(0.45).pos.x - 3.0f) < 1e-3f);
    CHECK(std::fabs(p.sample(5.0).pos.x - (1.0f + 10.0f * (float)kPuppetCoastS)) < 1e-3f);
    // Before the oldest: the oldest.
    CHECK(std::fabs(p.sample(0.1).pos.x - 0.0f) < 1e-4f);
}

static void test_interp_puppet_ignores_jitter() {
    // Constant motion (1 unit/s) sent at 30 Hz, arriving 50 ms plus 0..40 ms of jitter; sampled at 60 Hz with a 100 ms buffer.
    Puppet p;
    p.set_interp(0.1);
    uint32_t rng = 12345;
    auto jitter = [&rng]() { rng = rng * 1103515245u + 12345u; return (double)((rng >> 16) % 41) / 1000.0; };
    std::vector<std::pair<double, double>> arrivals;
    for (int i = 0; i < 90; ++i) {
        const double sent = i / 30.0;
        arrivals.push_back({sent + 0.05 + jitter(), sent});
    }
    std::sort(arrivals.begin(), arrivals.end());
    size_t next = 0;
    float last = -1.0f;
    for (int f = 0; f < 170; ++f) {
        const double now = 0.3 + f / 60.0;
        while (next < arrivals.size() && arrivals[next].first <= now) {
            p.on_state(at_x((float)arrivals[next].second, 1.0f), arrivals[next].first, arrivals[next].second);
            ++next;
        }
        const float x = p.sample(now).pos.x;
        // Rendered at now - 0.05 (the smallest offset seen) - 0.1: the true position then, and never moving backwards.
        CHECK(x >= last - 1e-5f);
        if (now > 0.6 && now < 2.9) {
            CHECK(std::fabs(x - (float)(now - 0.05 - 0.1)) < 0.02f);
        }
        last = x;
    }
}

static void test_hud_message() {
    // (this player out, peer left, peer down, shared lives)
    CHECK(std::string(hud_message(false, false, false, 2)) == "");
    CHECK(std::string(hud_message(true, false, true, 0)) == "SPECTATING");
    CHECK(std::string(hud_message(false, true, false, 2)) == "WINGMATE LEFT");
    CHECK(std::string(hud_message(false, false, true, 1)) == "WINGMATE DOWN");
    CHECK(std::string(hud_message(false, false, true, 0)) == "WINGMATE OUT");
    CHECK(std::string(hud_message(true, true, false, 0)) == "SPECTATING");
}

static void test_browse_report() {
    // State 4 is both "confirmed, exiting" (result still running, 0xFFFFFF) and "backed out" (result 0xFFFFFE): only the first may make the client confirm.
    CHECK(browse_report(4, 0xFFFFFFu) == 4);
    CHECK(browse_report(4, 0xFFFFFEu) == 2);
    CHECK(browse_report(3, 0xFFFFFFu) == 3);
    CHECK(browse_report(2, 0xFFFFFEu) == 2);
}

static void test_craft_barrier() {
    CraftBarrier b;
    CHECK(!b.released());
    b.pick(1);
    CHECK(!b.released());
    b.on_remote(0);
    CHECK(b.released());
    // back_out_clears_ready: the other player backing out re-arms the wait, and a new pick is needed.
    b.on_remote(-1);
    CHECK(!b.released());
    b.on_remote(2);
    CHECK(b.released());
    b.back();
    CHECK(!b.released() && b.local == -1);
}

static void test_puppet_model() {
    CHECK(puppet_model(0) == 0);
    CHECK(puppet_model(1) == 1);
    CHECK(puppet_model(5) == 5);
    CHECK(puppet_model(7) == 7);
    CHECK(puppet_model(6) == -1);
    CHECK(puppet_model(8) == -1);
    CHECK(puppet_model(-1) == -1);
    CHECK(std::string(wingman_model_name(1)) == "wmywng" && std::string(wingman_model_name(7)) == "t16");
    CHECK(wingman_model_name(8) == nullptr && wingman_model_name(-1) == nullptr);
}

static void test_pad2_bytes_idle_when_recording_ends() {
    uint8_t out[6] = {1, 2, 3, 4, 5, 6};
    pad2_bytes(nullptr, out);
    const uint8_t want[6] = {0, 0, 0, 0, 1, 0};
    CHECK(memcmp(out, want, 6) == 0);
}

static float dot3(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// World Y points down: level flight along +Z has body-down = +Y and right = down x fwd = +X.
static ImposterCraft level_craft() {
    ImposterCraft c;
    c.pos = {10.0f, 50.0f, 20.0f};
    c.fwd = {0.0f, 0.0f, 1.0f};
    c.down = {0.0f, 1.0f, 0.0f};
    return c;
}

static const float kDt = 1.0f / 30.0f;

static ImposterCraft imposter_fly_once(ImposterCraft c, const ImposterPad& in) {
    imposter_fly(c, in, ImposterWorld{}, kDt);
    return c;
}

static void test_poly_trig_matches_small_angles() {
    const float xs[] = {0.0f, 0.01f, -0.05f, 0.1f, 0.2f};
    for (float x : xs) {
        CHECK(std::fabs(poly_sin(x) - std::sin(x)) < 1e-5f);
        CHECK(std::fabs(poly_cos(x) - std::cos(x)) < 1e-5f);
    }
}

static void test_asin_a_matches_asin() {
    const float xs[] = {0.0f, 0.3f, -0.6f, 0.7f, 0.8f, -0.95f, 0.999f};
    for (float x : xs) {
        CHECK(std::fabs(asin_a(x) - std::asin(x)) < 2e-3f);
    }
    CHECK(asin_a(1.0f) == 1.5707964f);
    CHECK(asin_a(-1.0f) == -1.5707964f);
}

static void test_imposter_cruises_straight_with_no_input() {
    ImposterCraft c = level_craft();
    for (int i = 0; i < 30; ++i) {
        imposter_fly(c, ImposterPad{}, ImposterWorld{}, kDt);
    }
    CHECK(std::fabs(c.speed - 2.625f) < 1e-6f);
    CHECK(std::fabs(c.pos.z - (20.0f + 2.625f)) < 1e-3f);
    CHECK(std::fabs(c.pos.x - 10.0f) < 1e-4f);
    CHECK(std::fabs(c.pos.y - 50.0f) < 1e-4f);
}

static void test_imposter_stick_dead_zone() {
    ImposterPad in;
    in.stick_x = 7;
    in.stick_y = -7;
    ImposterCraft c = imposter_fly_once(level_craft(), in);
    CHECK(c.fwd.x == 0.0f && c.fwd.y == 0.0f && c.fwd.z == 1.0f);
}

static void test_imposter_full_right_stick_yaws_and_rolls() {
    ImposterPad in;
    in.stick_x = 75;
    const ImposterCraft c = imposter_fly_once(level_craft(), in);
    const float heading = std::atan2(c.fwd.x, c.fwd.z) * 57.29578f;
    CHECK(std::fabs(heading - 2.7648f) < 0.02f);
    const Vec3 right = {c.down.y * c.fwd.z - c.down.z * c.fwd.y, c.down.z * c.fwd.x - c.down.x * c.fwd.z, c.down.x * c.fwd.y - c.down.y * c.fwd.x};
    CHECK(std::fabs(std::fabs(std::asin(right.y) * 57.29578f) - 2.268f) < 0.05f);
}

static void test_imposter_stick_up_pitches_nose_down() {
    ImposterPad in;
    in.stick_y = 80;
    const ImposterCraft c = imposter_fly_once(level_craft(), in);
    CHECK(std::fabs(c.fwd.y - std::sin(2.0736f / 57.29578f)) < 1e-3f);
    ImposterWorld inv;
    inv.settings = 0x3 | 0x4;
    ImposterCraft d = level_craft();
    imposter_fly(d, in, inv, kDt);
    CHECK(d.fwd.y < 0.0f);
}

static void test_imposter_hold_rotation_button() {
    ImposterPad in;
    in.stick_x = 75;
    in.stick_y = 80;
    in.freeze = true;
    const ImposterCraft c = imposter_fly_once(level_craft(), in);
    CHECK(c.fwd.x == 0.0f && c.fwd.y == 0.0f && c.fwd.z == 1.0f);
}

static void test_imposter_auto_rolls_level() {
    ImposterCraft c = level_craft();
    const float s = 0.5f;
    const float k = 0.8660254f;
    c.down = {s, k, 0.0f};
    for (int i = 0; i < 120; ++i) {
        imposter_fly(c, ImposterPad{}, ImposterWorld{}, kDt);
    }
    const Vec3 right = {c.down.y * c.fwd.z - c.down.z * c.fwd.y, c.down.z * c.fwd.x - c.down.x * c.fwd.z, c.down.x * c.fwd.y - c.down.y * c.fwd.x};
    CHECK(std::fabs(right.y) < 0.05f);
}

static void test_imposter_brake_and_boost() {
    ImposterPad brake;
    brake.brake = true;
    ImposterCraft c = level_craft();
    for (int i = 0; i < 10; ++i) {
        imposter_fly(c, brake, ImposterWorld{}, kDt);
    }
    CHECK(std::fabs(c.speed - 2.25f) < 1e-6f);
    imposter_fly(c, ImposterPad{}, ImposterWorld{}, kDt);
    CHECK(std::fabs(c.speed - 2.4375f) < 1e-6f);
    ImposterPad boost;
    boost.boost = true;
    ImposterCraft d = level_craft();
    for (int i = 0; i < 300; ++i) {
        imposter_fly(d, boost, ImposterWorld{}, kDt);
        CHECK(d.speed <= 3.75f);
    }
    CHECK(d.speed == 3.75f);
}

static void test_imposter_sfoils_close_for_speed_and_stiffer_turns() {
    ImposterCraft c = level_craft();
    ImposterPad toggle;
    toggle.foil = true;
    imposter_fly(c, toggle, ImposterWorld{}, kDt);
    CHECK(c.foil_t > 0.0f && !c.foil_closed);
    for (int i = 0; i < 40; ++i) {
        imposter_fly(c, ImposterPad{}, ImposterWorld{}, kDt);
    }
    CHECK(c.foil_closed && c.foil_t == 1.0f);
    CHECK(std::fabs(c.speed - 5.625f) < 1e-5f);
    ImposterPad right;
    right.stick_x = 75;
    const ImposterCraft turned = imposter_fly_once(c, right);
    const float heading = std::atan2(turned.fwd.x, turned.fwd.z) * 57.29578f;
    CHECK(std::fabs(heading - 2.7648f * (1.28f * 1.28f) / (1.6f * 1.6f)) < 0.02f);
}

static void test_imposter_brake_opens_sfoils() {
    ImposterCraft c = level_craft();
    c.foil_t = 1.0f;
    c.foil_closed = true;
    c.speed = 5.625f;
    c.target = 5.625f;
    ImposterPad brake;
    brake.brake = true;
    imposter_fly(c, brake, ImposterWorld{}, kDt);
    CHECK(c.foil_t < 1.0f && c.foil_closed);
    for (int i = 0; i < 40; ++i) {
        imposter_fly(c, ImposterPad{}, ImposterWorld{}, kDt);
    }
    CHECK(!c.foil_closed && c.foil_t == 0.0f);
    CHECK(c.speed <= 3.75f);
}

static void test_imposter_frame_stays_orthonormal() {
    ImposterCraft c = level_craft();
    for (int i = 0; i < 3766; ++i) {
        ImposterPad in;
        in.stick_x = (int8_t)((i * 37) % 151 - 75);
        in.stick_y = (int8_t)((i * 53) % 161 - 80);
        imposter_fly(c, in, ImposterWorld{}, kDt);
    }
    CHECK(std::fabs(dot3(c.fwd, c.fwd) - 1.0f) < 1e-4f);
    CHECK(std::fabs(dot3(c.down, c.down) - 1.0f) < 1e-4f);
    CHECK(std::fabs(dot3(c.fwd, c.down)) < 1e-4f);
}

static void test_imposter_stays_above_ground() {
    ImposterPad dive;
    dive.stick_y = 80;
    ImposterWorld w;
    w.ground_y = 52.0f;
    ImposterCraft c = level_craft();
    for (int i = 0; i < 200; ++i) {
        imposter_fly(c, dive, w, kDt);
        CHECK(c.pos.y <= 52.0f - 0.02f + 1e-4f);
    }
}

static void test_imposter_fire_cooldown() {
    uint16_t cd = 0;
    CHECK(imposter_fire(true, &cd));
    CHECK(cd == 6);
    int shots = 1;
    for (int i = 0; i < 12; ++i) {
        shots += imposter_fire(true, &cd) ? 1 : 0;
    }
    CHECK(shots == 2);
    uint16_t idle = 3;
    CHECK(!imposter_fire(false, &idle));
    CHECK(idle == 2);
}

static void test_imposter_record_layout() {
    std::vector<uint8_t> ram(kRdramSpan, 0xCC);
    build_imposter_record(ram.data(), Vec3{1.0f, 2.0f, 3.0f});
    auto b = [&](uint32_t a) { return ram[(a - 0x80000000u) ^ 3]; };
    auto w = [&](uint32_t a) { return (uint32_t)b(a) << 24 | (uint32_t)b(a + 1) << 16 | (uint32_t)b(a + 2) << 8 | b(a + 3); };
    CHECK(b(kImposterRec + 0x0) == 0x00 && b(kImposterRec + 0x1) == 0x27);
    CHECK(b(kImposterRec + 0x6) == 0xFF && b(kImposterRec + 0x7) == 0xFF);
    CHECK(w(kImposterRec + 0x0C) == 0x800A9028u);
    CHECK(w(kImposterRec + 0x10) == 0x3F800000u);
    CHECK(w(kImposterRec + 0x18) == 0x40400000u);
    CHECK(w(kImposterRec + 0x28) == 0x3C656042u);
    CHECK(w(kImposterRec + 0x30) == 0x3C656042u);
    CHECK(b(kImposterRec + 0x34) == 0xFF && b(kImposterRec + 0x35) == 0xFF);
    CHECK(w(kImposterRec + 0x54) == 0x80000000u);
    CHECK(w(kImposterRec + 0x6C) == 1000u);
    CHECK(w(kImposterRec + 0x70) == 1000u);
    CHECK(w(kImposterRec + 0x74) == 1000u);
    CHECK(w(kImposterRec + 0x78) == 0u);
    CHECK(w(kImposterRec + 0x88) == 0u);
    CHECK(w(kImposterRec + 0x90) == 7u);
    CHECK(w(kImposterRec + 0x94) == 0xF0u);
    CHECK(w(kImposterRec + 0xAC) == 0u);
    CHECK(b(kImposterRec + 0xF0) == '-' && b(kImposterRec + 0xF1) == 0);
    CHECK(b(kImposterRec + 0xFF) == 0);
    CHECK(w(kImposterRec + 0xEC) == 0x40280000u);
    CHECK(w(kImposterRec + 0xF4) == 0x40280000u);
    CHECK(w(kImposterRec + 0xF8) == 0u);
    CHECK(b(kImposterRec + 0x100) == 0xCC);
}

static void test_clear_imposter_record_zeroes_only_the_record() {
    std::vector<uint8_t> ram(kRdramSpan, 0xCC);
    clear_imposter_record(ram.data());
    for (uint32_t i = 0; i < 0x100; ++i) {
        CHECK(ram[(kImposterRec + i - 0x80000000u) ^ 3] == 0);
    }
    CHECK(ram[(kImposterRec + 0x100 - 0x80000000u) ^ 3] == 0xCC);
    CHECK(ram[(kImposterRec - 1 - 0x80000000u) ^ 3] == 0xCC);
}

static void test_imposter_spawn_pos_is_unfused() {
    volatile float rx = 0.1f, rz = 0.3f, one_five = 1.5f;
    const float mx = rx * one_five;
    const float mz = rz * one_five;
    const Vec3 p = imposter_spawn_pos(Vec3{1.0f, 2.0f, 3.0f}, Vec3{0.1f, 0.7f, 0.3f});
    CHECK(p.x == 1.0f + mx);
    CHECK(p.y == 2.0f);
    CHECK(p.z == 3.0f + mz);
}

static void put_be16(std::vector<uint8_t>& ram, uint32_t a, uint16_t v);

static void test_npc_pool_room() {
    std::vector<uint8_t> ram(kRdramSpan, 0);
    put_be32(ram, 0x80130BC8u, 300);
    put_be16(ram, 0x80130BB4u, 0x128);
    CHECK(npc_pool_room(ram.data()));
    put_be16(ram, 0x80130BB4u, 0xFFFF);
    CHECK(!npc_pool_room(ram.data()));
    put_be16(ram, 0x80130BB4u, 0x128);
    put_be32(ram, 0x80130BC8u, 0xFFFFFFFFu);
    CHECK(!npc_pool_room(ram.data()));
}

static void test_imposter_spawn_ok_rejects_failed_spawns() {
    CHECK(imposter_spawn_ok(75, 0x8021E418u, 0x8035833Cu, 0x80300000u));
    CHECK(!imposter_spawn_ok(0xFFFF, 0x8021E418u, 0x8035833Cu, 0x80300000u));
    // The slot table has 0x800 entries (a busy level hands out slots like 0x128 and 0x201); 0xFFFF is the failure value.
    CHECK(imposter_spawn_ok(0x201, 0x8021E418u, 0x8035833Cu, 0x80300000u));
    CHECK(imposter_spawn_ok(kNpcSlots - 1, 0x8021E418u, 0x8035833Cu, 0x80300000u));
    CHECK(!imposter_spawn_ok(kNpcSlots, 0x8021E418u, 0x8035833Cu, 0x80300000u));
    CHECK(!imposter_spawn_ok(75, 0, 0x8035833Cu, 0x80300000u));
    CHECK(!imposter_spawn_ok(75, 0x8021E418u, 0, 0x80300000u));
    CHECK(!imposter_spawn_ok(75, 0x8021E418u, 0x8035833Cu, 0));
    CHECK(!imposter_spawn_ok(75, 0x80800000u, 0x8035833Cu, 0x80300000u));
    CHECK(!imposter_spawn_ok(75, 0x8021E418u, 0x7FFFFFF0u, 0x80300000u));
}

static float len3(const Vec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

static PuppetState puppet_at(float x, float vx) {
    PuppetState s;
    s.pos = {x, 0, 0};
    s.fwd = {0, 0, 1};
    s.down = {0, 1, 0};
    s.vel = {vx, 0, 0};
    return s;
}

static void test_puppet_holds_reset_pose_without_target() {
    Puppet p;
    p.reset(puppet_at(5, 0));
    CHECK(!p.has_target());
    CHECK(p.step(1.0, 1.0f / 30).pos.x == 5.0f);
}

static void test_puppet_extrapolates_and_caps_coast() {
    Puppet p;
    p.reset(puppet_at(0, 0));
    p.on_state(puppet_at(0, 10), 0.0);
    PuppetState s;
    for (int i = 1; i <= 90; ++i) s = p.step(i / 30.0, 1.0f / 30);
    CHECK(std::fabs(s.pos.x - 5.0f) < 0.1f);
}

static void test_puppet_blends_small_errors_and_snaps_big_ones() {
    Puppet p;
    p.reset(puppet_at(0, 0));
    p.on_state(puppet_at(2, 0), 0.0);
    const float first = p.step(1.0 / 30, 1.0f / 30).pos.x;
    CHECK(first > 0.0f && first < 2.0f);
    PuppetState s;
    for (int i = 2; i <= 30; ++i) s = p.step(i / 30.0, 1.0f / 30);
    CHECK(std::fabs(s.pos.x - 2.0f) < 0.01f);
    p.on_state(puppet_at(100, 0), 1.0);
    CHECK(p.step(1.0 + 1.0 / 30, 1.0f / 30).pos.x == 100.0f);
}

static void test_skip_share_sends_local_skips_but_not_its_own_echo() {
    SkipShare k;
    CHECK(!k.on_local(false, 0.0));
    CHECK(k.on_local(true, 0.0));
    // A skip this side made by pressing START for the other player is not sent back.
    k.on_remote(1.0);
    CHECK(k.press(1.0, true));
    CHECK(!k.on_local(true, 1.2));
    CHECK(!k.press(1.3, false));
    CHECK(k.on_local(true, 1.0 + SkipShare::kEchoS + 0.5));
}

static void test_cutscene_skip_detection() {
    // Running cutscene (gate 100 of end 590) that stops: skipped. One that reaches its end: not.
    CHECK(cutscene_skipped(true, 100, 590, false));
    CHECK(!cutscene_skipped(true, 575, 590, false));
    CHECK(!cutscene_skipped(true, 100, 590, true));
    CHECK(!cutscene_skipped(false, 100, 590, false));
}

static void test_skip_share_presses_start_until_the_cinematic_ends() {
    SkipShare k;
    CHECK(!k.press(0.0, true));
    k.on_remote(1.0);
    // Not in a cutscene yet (still loading): no press, but the skip waits.
    CHECK(!k.press(1.5, false));
    // The cutscene starts: START pulses (pressed, then released, then pressed again) so the game sees fresh presses.
    CHECK(k.press(2.0, true));
    CHECK(!k.press(2.0 + SkipShare::kPulseS * 1.5, true));
    CHECK(k.press(2.0 + SkipShare::kPulseS * 2.5, true));
    // The cutscene ended: the skip is used up and a later cutscene plays.
    CHECK(!k.press(3.0, false));
    CHECK(!k.press(4.0, true));
}

static void test_skip_share_ends_with_its_scene() {
    // A skip that pressed through one scene (the hangar launch) does not carry into the next (the crawl).
    SkipShare k;
    k.on_remote(0.0);
    CHECK(k.press(0.5, true));
    k.end_scene();
    CHECK(!k.press(0.6, true));
    // A skip still waiting for its scene (not pressed yet) survives a scene change.
    SkipShare w;
    w.on_remote(0.0);
    w.end_scene();
    CHECK(w.press(0.5, true));
}

static void test_skip_share_expires() {
    SkipShare k;
    k.on_remote(0.0);
    CHECK(!k.press(SkipShare::kWaitS + 0.1, true));
}

static void test_puppet_axes_stay_orthonormal_through_a_reversal() {
    Puppet p;
    p.reset(puppet_at(0, 0));
    PuppetState t = puppet_at(0, 0);
    t.fwd = {0, 0, -1};
    p.on_state(t, 0.0);
    PuppetState s;
    for (int i = 1; i <= 60; ++i) {
        s = p.step(i / 30.0, 1.0f / 30);
        CHECK(std::fabs(len3(s.fwd) - 1.0f) < 1e-3f && std::fabs(len3(s.down) - 1.0f) < 1e-3f);
        CHECK(std::fabs(s.fwd.x * s.down.x + s.fwd.y * s.down.y + s.fwd.z * s.down.z) < 1e-3f);
    }
    CHECK(s.fwd.z < -0.999f);
}

static void test_puppet_turns_smoothly_for_small_heading_changes() {
    Puppet p;
    p.reset(puppet_at(0, 0));
    PuppetState t = puppet_at(0, 0);
    t.fwd = {0.2588190f, 0, 0.9659258f};
    p.on_state(t, 0.0);
    const PuppetState s = p.step(1.0 / 30, 1.0f / 30);
    CHECK(s.fwd.x > 0.01f && s.fwd.x < 0.25f);
}

static void put_str(std::vector<uint8_t>& ram, uint32_t a, const char* s) {
    for (size_t i = 0; i <= strlen(s); ++i) put_u8(ram, a + (uint32_t)i, (uint8_t)s[i]);
}

static void put_be16(std::vector<uint8_t>& ram, uint32_t a, uint16_t v) {
    put_u8(ram, a, (uint8_t)(v >> 8));
    put_u8(ram, a + 1, (uint8_t)v);
}

static void test_puppet_snap_jumps_to_target() {
    Puppet p;
    p.reset(puppet_at(0, 0));
    p.on_state(puppet_at(5, 0), 0.0);
    p.snap();
    CHECK(p.step(0.0, 1.0f / 30).pos.x == 5.0f);
}

static void test_mesh_loaded_walks_the_name_hash() {
    std::vector<uint8_t> ram(kRdramSpan, 0);
    for (uint32_t b = 0; b < 25; ++b) put_be16(ram, 0x801394B0u + b * 2, 0xFFFF);
    // "wmywng" sums to 681, bucket 681 % 25 = 6; chain: entry 3 ("abc") -> entry 5 ("wmywng").
    put_be16(ram, 0x801394B0u + 6 * 2, 3);
    put_be16(ram, 0x80139020u + 3 * 12, 5);
    put_be32(ram, 0x80139020u + 3 * 12 + 4, 0x80200000u);
    put_str(ram, 0x80200000u, "abc");
    put_be16(ram, 0x80139020u + 5 * 12, 0xFFFF);
    put_be32(ram, 0x80139020u + 5 * 12 + 4, 0x80200010u);
    put_str(ram, 0x80200010u, "wmywng");
    CHECK(mesh_loaded(ram.data(), "wmywng"));
    CHECK(!mesh_loaded(ram.data(), "wmxwng"));
    CHECK(!mesh_loaded(ram.data(), "wmywn"));
    CHECK(wingman_model(ram.data()) == 1);
    for (uint32_t b = 0; b < 25; ++b) put_be16(ram, 0x801394B0u + b * 2, 0xFFFF);
    CHECK(wingman_model(ram.data()) == -1);
}

static void test_hash_imposter_part_only_when_enabled() {
    CHECK(hash_part_names(true).back() == "imposter");
    CHECK(hash_part_names(true).size() == hash_part_names(false).size() + 1);
    std::vector<uint8_t> ram(kRdramSpan, 0);
    const FrameHash a = hash_frame(ram.data(), true);
    ram[(kImposterRec + 0x10 - 0x80000000u) ^ 3] = 1;
    CHECK(hash_frame(ram.data(), true).combined != a.combined);
    CHECK(hash_frame(ram.data(), false).combined == hash_frame(std::vector<uint8_t>(kRdramSpan, 0).data(), false).combined);
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

static void test_team_triggers_first_enter_last_exit() {
    TeamTriggers t;
    CHECK(t.on_edge(0, 5, true));
    CHECK(!t.on_edge(1, 5, true));
    CHECK(!t.on_edge(0, 5, false));
    CHECK(t.on_edge(1, 5, false));
    CHECK(t.on_edge(1, 5, true));
}

static void test_team_triggers_ignore_repeats_and_strays() {
    TeamTriggers t;
    CHECK(!t.on_edge(1, 7, false));
    CHECK(t.on_edge(1, 7, true));
    CHECK(!t.on_edge(1, 7, true));
    CHECK(t.on_edge(0, 8, true));
    CHECK(t.on_edge(1, 7, false));
}

static void test_team_triggers_forget_and_reset() {
    TeamTriggers t;
    CHECK(t.on_edge(1, 3, true));
    t.forget(1);
    CHECK(t.on_edge(0, 3, true));
    t.reset();
    CHECK(t.on_edge(1, 3, true));
}

int main() {
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
    test_relocated_maps_only_moved_ranges();
    test_r4_relocates_with_its_own_delta();
    test_poison_covers_only_moved_ranges();
    test_first_unpoisoned_reports_lowest_write();
    test_hash_reads_relocated_objective_counts();
    test_poly_trig_matches_small_angles();
    test_asin_a_matches_asin();
    test_imposter_cruises_straight_with_no_input();
    test_imposter_stick_dead_zone();
    test_imposter_full_right_stick_yaws_and_rolls();
    test_imposter_stick_up_pitches_nose_down();
    test_imposter_hold_rotation_button();
    test_imposter_auto_rolls_level();
    test_imposter_brake_and_boost();
    test_imposter_sfoils_close_for_speed_and_stiffer_turns();
    test_imposter_brake_opens_sfoils();
    test_imposter_frame_stays_orthonormal();
    test_imposter_stays_above_ground();
    test_imposter_fire_cooldown();
    test_imposter_record_layout();
    test_hash_imposter_part_only_when_enabled();
    test_clear_imposter_record_zeroes_only_the_record();
    test_imposter_spawn_pos_is_unfused();
    test_imposter_spawn_ok_rejects_failed_spawns();
    test_npc_pool_room();
    test_puppet_holds_reset_pose_without_target();
    test_puppet_extrapolates_and_caps_coast();
    test_puppet_blends_small_errors_and_snaps_big_ones();
    test_puppet_axes_stay_orthonormal_through_a_reversal();
    test_puppet_turns_smoothly_for_small_heading_changes();
    test_skip_share_sends_local_skips_but_not_its_own_echo();
    test_cutscene_skip_detection();
    test_skip_share_presses_start_until_the_cinematic_ends();
    test_skip_share_ends_with_its_scene();
    test_skip_share_expires();
    test_mesh_loaded_walks_the_name_hash();
    test_puppet_snap_jumps_to_target();
    test_pad2_bytes_masks_start_and_marks_connected();
    test_pad2_bytes_idle_when_recording_ends();
    test_shared_life_outcomes();
    test_spectate_pad_keeps_only_start();
    test_follow_step();
    test_craft_barrier();
    test_browse_report();
    test_hud_message();
    test_interp_puppet_midpoint_and_extrapolation();
    test_interp_puppet_ignores_jitter();
    test_puppet_model();
    test_team_triggers_first_enter_last_exit();
    test_team_triggers_ignore_repeats_and_strays();
    test_team_triggers_forget_and_reset();
    printf("lockstep_core_test OK\n");
    return 0;
}
