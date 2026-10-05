#include "lockstep_core.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace rs64::ls {

static const char* kMagic = "rs64-ls v2";

std::string format_header(const SessionHeader& h) {
    char pilot[2 * 0x30 + 1];
    for (size_t i = 0; i < h.pilot.size(); ++i) {
        snprintf(pilot + i * 2, 3, "%02X", h.pilot[i]);
    }
    char buf[768];
    snprintf(buf, sizeof(buf),
        "%s\nseed=%u\nlevel=%u\ncraft=%u\ndifficulty=%u\nflags0=0x%08X\ncontroller=%u\nexpansion=%u\n"
        "cheats0=0x%08X\ncheats1=0x%08X\nnaboo=0x%08X\nsecondary=%u\ncruise=0x%08X\npilot=%s\nbuild=%s\n---\n",
        kMagic, h.seed, h.level, h.craft, h.difficulty, h.flags0, h.controller, h.expansion,
        h.cheats0, h.cheats1, h.naboo, h.secondary, h.cruise_bits, pilot, h.build.c_str());
    return buf;
}

static bool parse_hex_bytes(const std::string& hex, uint8_t* out, size_t n) {
    if (hex.size() != n * 2) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        char byte[3] = {hex[i * 2], hex[i * 2 + 1], 0};
        char* end = nullptr;
        out[i] = (uint8_t)std::strtoul(byte, &end, 16);
        if (*end != 0) {
            return false;
        }
    }
    return true;
}

bool parse_header(const std::string& text, SessionHeader* out) {
    std::istringstream in(text);
    std::string line;
    auto next_line = [&in, &line]() {
        if (!std::getline(in, line)) {
            return false;
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        return true;
    };
    if (!next_line() || line != kMagic) {
        return false;
    }
    SessionHeader h;
    while (next_line()) {
        if (line == "---") {
            *out = h;
            return true;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        const uint32_t v = (uint32_t)std::strtoul(val.c_str(), nullptr, 0);
        if (key == "seed") h.seed = v;
        else if (key == "level") h.level = v;
        else if (key == "craft") h.craft = v;
        else if (key == "difficulty") h.difficulty = v;
        else if (key == "flags0") h.flags0 = v;
        else if (key == "controller") h.controller = v;
        else if (key == "expansion") h.expansion = v;
        else if (key == "cheats0") h.cheats0 = v;
        else if (key == "cheats1") h.cheats1 = v;
        else if (key == "naboo") h.naboo = v;
        else if (key == "secondary") h.secondary = v;
        else if (key == "cruise") h.cruise_bits = v;
        else if (key == "build") h.build = val;
        else if (key == "pilot" && !parse_hex_bytes(val, h.pilot.data(), h.pilot.size())) return false;
    }
    return false;
}

std::string format_frame(const FrameInput& f) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%u %02X%02X%02X%02X%02X%02X %u", f.frame,
        f.pad[0], f.pad[1], f.pad[2], f.pad[3], f.pad[4], f.pad[5], (unsigned)f.throttle);
    return buf;
}

// Recordings written in text mode on Windows end lines with "\r\n"; parse them like "\n".
static std::string normalize_eol(const char* line) {
    std::string s = line ? line : "";
    if (s.size() >= 2 && s[s.size() - 2] == '\r' && s.back() == '\n') {
        s.erase(s.size() - 2, 1);
    }
    return s;
}

bool parse_frame(const char* raw, FrameInput* out) {
    const std::string norm = normalize_eol(raw);
    const char* line = raw ? norm.c_str() : nullptr;
    unsigned frame = 0;
    char hex[13] = {};
    unsigned thr = 0;
    char tail = 0;
    // The writer ends every frame with a newline; a line without one was cut off mid-write.
    if (!line || sscanf(line, "%u %12s %u%c", &frame, hex, &thr, &tail) != 4 || tail != '\n') {
        return false;
    }
    if (strlen(hex) != 12 || thr > 0xFF) {
        return false;
    }
    FrameInput f;
    f.frame = frame;
    for (int i = 0; i < 6; ++i) {
        char byte[3] = {hex[i * 2], hex[i * 2 + 1], 0};
        char* end = nullptr;
        f.pad[i] = (uint8_t)std::strtoul(byte, &end, 16);
        if (*end != 0) {
            return false;
        }
    }
    f.throttle = (uint8_t)thr;
    *out = f;
    return true;
}

std::string format_song_event(uint32_t frame, bool active) {
    char buf[32];
    snprintf(buf, sizeof(buf), "S %u %u", frame, active ? 1u : 0u);
    return buf;
}

bool parse_song_event(const char* raw, uint32_t* frame, bool* active) {
    const std::string norm = normalize_eol(raw);
    const char* line = raw ? norm.c_str() : nullptr;
    unsigned f = 0;
    unsigned v = 0;
    char tail = 0;
    if (!line || sscanf(line, "S %u %u%c", &f, &v, &tail) != 3 || tail != '\n' || v > 1) {
        return false;
    }
    *frame = f;
    *active = v != 0;
    return true;
}

uint32_t parse_seed(const char* s) {
    if (!s || !*s) {
        return 0;
    }
    char* end = nullptr;
    const unsigned long v = std::strtoul(s, &end, 0);
    if (*end != '\0') {
        return 0;
    }
    return (uint32_t)v;
}

int clamp_delay(long frames) {
    return (int)(frames < 1 ? 1 : (frames > 60 ? 60 : frames));
}

uint8_t quantize_throttle(float p) {
    if (!(p >= 0.0f)) {
        return 0xFF;
    }
    const float c = p > 1.0f ? 1.0f : p;
    return (uint8_t)std::lround(c * 254.0f);
}

float dequantize_throttle(uint8_t q) {
    return (q == 0xFF) ? -1.0f : (float)q / 254.0f;
}

DelayLine::DelayLine(int frames) : ring_((size_t)(frames > 0 ? frames : 1) * 6, 0), frames_((size_t)(frames > 0 ? frames : 1)) {}

void DelayLine::push_pop(uint8_t pad[6]) {
    uint8_t out[6];
    memcpy(out, &ring_[idx_ * 6], 6);
    memcpy(&ring_[idx_ * 6], pad, 6);
    idx_ = (idx_ + 1) % frames_;
    memcpy(pad, out, 6);
}

uint64_t fnv1a(const uint8_t* data, size_t n, uint64_t h) {
    for (size_t i = 0; i < n; ++i) {
        h ^= data[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

namespace {

struct Span { uint32_t addr; uint32_t size; };

bool in_rdram(uint32_t kseg0, uint32_t size) {
    return kseg0 >= 0x80000000u && (uint64_t)(kseg0 - 0x80000000u) + size <= kRdramSize;
}

uint8_t rd8(const uint8_t* rdram, uint32_t kseg0) {
    return rdram[(kseg0 - 0x80000000u) ^ 3];
}

uint32_t rd32(const uint8_t* rdram, uint32_t kseg0) {
    return ((uint32_t)rd8(rdram, kseg0) << 24) | ((uint32_t)rd8(rdram, kseg0 + 1) << 16) |
           ((uint32_t)rd8(rdram, kseg0 + 2) << 8) | (uint32_t)rd8(rdram, kseg0 + 3);
}

uint64_t hash_spans(const uint8_t* rdram, const std::vector<Span>& spans) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const Span& s : spans) {
        for (uint32_t i = 0; i < s.size; ++i) {
            const uint8_t b = rd8(rdram, s.addr + i);
            h = fnv1a(&b, 1, h);
        }
    }
    return h;
}

uint64_t mix32(uint64_t h, uint32_t v) {
    const uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    return fnv1a(b, 4, h);
}

uint64_t hash_npcs(const uint8_t* rdram) {
    uint64_t h = 0xcbf29ce484222325ull;
    const uint32_t table = rd32(rdram, 0x80130BB0u);
    if (!in_rdram(table, 0x800 * 8)) {
        return h;
    }
    for (uint32_t i = 0; i < 0x800; ++i) {
        const uint32_t ctx = rd32(rdram, table + i * 8);
        if (!in_rdram(ctx, 0x3C)) {
            continue;
        }
        h = mix32(h, i);
        h = mix32(h, rd32(rdram, ctx + 0x0));
        for (uint32_t k = 0x14; k < 0x1A; ++k) {
            const uint8_t b = rd8(rdram, ctx + k);
            h = fnv1a(&b, 1, h);
        }
        const uint32_t pos = rd32(rdram, ctx + 0x8);
        if (in_rdram(pos, 12)) {
            for (uint32_t k = 0; k < 12; ++k) {
                const uint8_t b = rd8(rdram, pos + k);
                h = fnv1a(&b, 1, h);
            }
        } else {
            h = mix32(h, 0xFFFFFFFFu);
        }
    }
    return h;
}

const std::vector<std::vector<Span>>& region_spans() {
    static const std::vector<std::vector<Span>> k = {
        {{0x80003470u, 0x4}},
        {{0x80130B10u, 0x30}},
        {{0x80130B40u, 0x6}, {0x80130B4Cu, 0x4}, {0x80130B58u, 0x8}, {0x80130B63u, 0x1}, {0x80130B68u, 0x4}},
        {{0x80130B78u, 0x38}},
        {{0x8010C9E0u, 0x50}},
        {{0x801388A0u, 0x80}},
        {{relocated(0x80138060u), 0x200}},
        // Skips gPlayers[0]+0x215 (text pending flag) and the text-element block +0x240..+0x24F (ids from counter 0x800CC850), set on the format-message worker's timing.
        {{0x80137DB8u, 0x215}, {0x80137FCEu, 0x2A}, {0x80138008u, 0x50}},
        {{0x8013A950u, 0x20}},
    };
    return k;
}

}

namespace {

void wr8(uint8_t* rdram, uint32_t kseg0, uint8_t v) {
    rdram[(kseg0 - 0x80000000u) ^ 3] = v;
}

void wr32(uint8_t* rdram, uint32_t kseg0, uint32_t v) {
    for (uint32_t i = 0; i < 4; ++i) {
        wr8(rdram, kseg0 + i, (uint8_t)(v >> (24 - 8 * i)));
    }
}

}

SessionHeader read_session(const uint8_t* rdram, uint32_t seed) {
    SessionHeader h;
    h.seed = seed;
    h.level = rd8(rdram, 0x80130B40u);
    h.craft = rd8(rdram, 0x80130B41u);
    h.difficulty = rd8(rdram, 0x80130B42u);
    h.secondary = rd8(rdram, 0x80130B43u);
    h.controller = rd8(rdram, 0x80130B45u);
    h.flags0 = rd32(rdram, 0x80130B4Cu);
    h.cheats0 = rd32(rdram, 0x80130B58u);
    h.cheats1 = rd32(rdram, 0x80130B5Cu);
    h.expansion = rd8(rdram, 0x80130B63u);
    h.naboo = rd32(rdram, 0x80130B68u);
    for (uint32_t i = 0; i < h.pilot.size(); ++i) {
        h.pilot[i] = rd8(rdram, 0x80130B10u + i);
    }
    return h;
}

void apply_session(uint8_t* rdram, const SessionHeader& h) {
    wr8(rdram, 0x80130B42u, (uint8_t)h.difficulty);
    wr8(rdram, 0x80130B43u, (uint8_t)h.secondary);
    wr8(rdram, 0x80130B45u, (uint8_t)h.controller);
    wr32(rdram, 0x80130B4Cu, h.flags0);
    wr32(rdram, 0x80130B58u, h.cheats0);
    wr32(rdram, 0x80130B5Cu, h.cheats1);
    wr8(rdram, 0x80130B63u, (uint8_t)h.expansion);
    wr32(rdram, 0x80130B68u, h.naboo);
    for (uint32_t i = 0; i < h.pilot.size(); ++i) {
        wr8(rdram, 0x80130B10u + i, h.pilot[i]);
    }
}

static uint64_t hash_imposter(const uint8_t* rdram) {
    uint64_t h = hash_spans(rdram, {{kImposterRec, 0x100}});
    const uint32_t blk = rd32(rdram, kImposterRec + 0xE0);
    if (in_rdram(blk, 0x30)) {
        for (uint32_t i = 0; i < 0x30; ++i) {
            const uint8_t b = rd8(rdram, blk + i);
            h = fnv1a(&b, 1, h);
        }
    }
    return h;
}

std::vector<std::string> hash_part_names(bool imposter) {
    std::vector<std::string> names = {"rng", "mission", "settings", "stats", "mission_ctl", "obj_bools", "obj_counts", "player0", "buttons", "npcs"};
    if (imposter) {
        names.push_back("imposter");
    }
    return names;
}

FrameHash hash_frame(const uint8_t* rdram, bool imposter) {
    FrameHash out;
    for (const auto& spans : region_spans()) {
        out.parts.push_back(hash_spans(rdram, spans));
    }
    out.parts.push_back(hash_npcs(rdram));
    if (imposter) {
        out.parts.push_back(hash_imposter(rdram));
    }
    uint64_t c = 0xcbf29ce484222325ull;
    for (uint64_t p : out.parts) {
        c = mix32(mix32(c, (uint32_t)(p >> 32)), (uint32_t)p);
    }
    out.combined = c;
    return out;
}

std::string format_hash_legend(bool imposter) {
    std::string s = "# frame combined";
    for (const std::string& n : hash_part_names(imposter)) {
        s += " " + n;
    }
    return s;
}

std::string dump_regions(const uint8_t* rdram) {
    const std::vector<std::string> names = hash_part_names();
    const auto& regions = region_spans();
    std::string out;
    char hex[3];
    for (size_t r = 0; r < regions.size(); ++r) {
        out += names[r];
        out += ' ';
        for (const Span& s : regions[r]) {
            for (uint32_t i = 0; i < s.size; ++i) {
                snprintf(hex, sizeof(hex), "%02X", rd8(rdram, s.addr + i));
                out += hex;
            }
        }
        out += '\n';
    }
    return out;
}

uint32_t relocated(uint32_t kseg0) {
    for (const MovedRange& r : kMovedRanges) {
        if (kseg0 >= r.lo && kseg0 < r.hi) {
            return kseg0 + r.delta;
        }
    }
    return kseg0;
}

void poison_moved(uint8_t* rdram) {
    for (const MovedRange& r : kMovedRanges) {
        for (uint32_t a = r.lo; a < r.hi; ++a) {
            rdram[(a - 0x80000000u) ^ 3] = kRelocPoison;
        }
    }
}

uint32_t first_unpoisoned(const uint8_t* rdram) {
    for (const MovedRange& r : kMovedRanges) {
        for (uint32_t a = r.lo; a < r.hi; ++a) {
            if (rdram[(a - 0x80000000u) ^ 3] != kRelocPoison) {
                return a;
            }
        }
    }
    return 0;
}

void pad2_bytes(const FrameInput* f, uint8_t out[6]) {
    for (int i = 0; i < 6; ++i) {
        out[i] = f ? f->pad[i] : 0;
    }
    out[0] &= (uint8_t)~0x10;
    out[4] = 1;
    out[5] = 0;
}

std::string format_hash_line(uint32_t frame, const FrameHash& h) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%u %016llX", frame, (unsigned long long)h.combined);
    std::string s = buf;
    for (uint64_t p : h.parts) {
        snprintf(buf, sizeof(buf), " %016llX", (unsigned long long)p);
        s += buf;
    }
    return s;
}


float poly_sin(float x) {
    const float x2 = x * x;
    return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f)));
}

float poly_cos(float x) {
    const float x2 = x * x;
    return 1.0f - x2 / 2.0f * (1.0f - x2 / 12.0f * (1.0f - x2 / 30.0f));
}

static Vec3 v_add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static Vec3 v_scale(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static float v_dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 v_cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static Vec3 v_norm(Vec3 a) {
    const float len = std::sqrt(v_dot(a, a));
    return len > 0.0f ? v_scale(a, 1.0f / len) : a;
}

static float fabs_f(float v) {
    return v < 0.0f ? -v : v;
}

static float clamp1(float v) {
    return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
}

// Normalised only when long enough, like the game's normIfLen.
static Vec3 v_norm_if_len(Vec3 a) {
    const float len = std::sqrt(v_dot(a, a));
    return len >= 1e-4f ? v_scale(a, 1.0f / len) : a;
}

float asin_a(float x) {
    if (x > 0.99999899f) {
        return 1.5707964f;
    }
    if (x < -0.99999899f) {
        return -1.5707964f;
    }
    const bool small = fabs_f(x) < 0.70710677f;
    float p = x;
    if (!small) {
        p = std::sqrt(1.0f - x * x);
        if (x < 0.0f) {
            p = -p;
        }
    }
    if (!(fabs_f(p) < 0.001f)) {
        const float p2 = p * p;
        const float p3 = p * p2;
        const float p5 = p3 * p2;
        const float p7 = p5 * p2;
        const float p9 = p7 * p2;
        p = (((p + p3 * 0.16666667f) + p5 * 0.075f) + p7 * 0.044642858f) + p9 * 0.047445997f;
    }
    if (small) {
        return p;
    }
    return x < 0.0f ? -1.5707964f - p : 1.5707964f - p;
}

// tickPlayerHudIndicators' normalisation (clamp, 0.015 dead zone), then the physics dead zone (0.1) and squaring unless linear.
static float stick_axis(int8_t raw, float scale, bool linear) {
    float v = clamp1((float)raw / scale);
    if (fabs_f(v) < 0.015f) {
        v = 0.0f;
    }
    float d = 0.0f;
    if (v > 0.1f) {
        d = v - 0.1f;
    } else if (v < -0.1f) {
        d = v + 0.1f;
    }
    return linear ? d : d * fabs_f(d);
}

static float bank_deg(Vec3 right, Vec3 fwd) {
    const Vec3 level_up = v_norm(v_cross(fwd, v_norm(v_cross(Vec3{0.0f, 1.0f, 0.0f}, fwd))));
    return (-asin_a(v_dot(right, level_up)) * 180.0f) / 3.1415927f;
}

// orient = orient * E(yaw, pitch, roll) (0x800AA570), a body-frame rotation; columns of orient are right, down, fwd.
static void rotate_body(ImposterCraft& c, float yaw_deg, float pitch_deg, float roll_deg) {
    const float k = 0.017453292f;
    const float ca = poly_cos(yaw_deg * k);
    const float sa = poly_sin(yaw_deg * k);
    const float cb = poly_cos(pitch_deg * k);
    const float sb = poly_sin(pitch_deg * k);
    const float cc = poly_cos(roll_deg * k);
    const float sc = poly_sin(roll_deg * k);
    const float e01 = ca * sc + (sa * cc) * sb;
    const float e02 = -sa * cb;
    const float e11 = cc * cb;
    const float e12 = sb;
    const float e21 = sa * sc - (ca * cc) * sb;
    const float e22 = ca * cb;
    const Vec3 r = v_cross(c.down, c.fwd);
    const Vec3 d = c.down;
    const Vec3 f = c.fwd;
    const Vec3 nd = v_add(v_add(v_scale(r, e01), v_scale(d, e11)), v_scale(f, e21));
    const Vec3 nf = v_add(v_add(v_scale(r, e02), v_scale(d, e12)), v_scale(f, e22));
    c.fwd = v_norm(nf);
    c.down = v_norm(v_add(nd, v_scale(c.fwd, -v_dot(nd, c.fwd))));
}

void imposter_fly(ImposterCraft& c, const ImposterPad& in, const ImposterWorld& w, float dt) {
    // Speed (updateXwingFlightControls): only the target ramps, and speed takes it every frame. sc is last frame's S-foil speed scale.
    const float sc = c.foil_t * 0.5f + 1.0f;
    c.boost_hold = in.boost ? c.boost_hold + dt : 0.0f;
    float tgt = c.target;
    if (in.brake && !in.boost) {
        tgt -= 0.1875f;
    }
    if (tgt < sc * 2.25f) {
        tgt = sc * 2.25f;
    }
    tgt += c.boost_hold * 0.1875f;
    if (!in.brake && !in.boost && c.speed < 2.625f) {
        tgt += 0.1875f;
        if (tgt > sc * 2.625f) {
            tgt = sc * 2.625f;
        }
    }
    if (c.foil_t != 0.0f) {
        tgt = sc * 3.75f;
    }
    if (tgt > sc * 3.75f) {
        tgt = sc * 3.75f;
    }
    c.target = tgt;
    c.speed = tgt;
    // S-foils: the toggle (or, when closed, the brake) starts a one-second move; it finishes on its own.
    if (!c.foil_closed) {
        if (c.foil_t != 0.0f || in.foil) {
            c.foil_t += dt;
            if (c.foil_t > 1.0f) {
                c.foil_t = 1.0f;
                c.foil_closed = true;
            }
        }
    } else if (c.foil_t != 1.0f || in.foil || in.brake) {
        c.foil_t -= dt;
        if (c.foil_t < 0.0f) {
            c.foil_t = 0.0f;
            c.foil_closed = false;
        }
    }
    const float handling = c.foil_closed ? 1.28f : 1.6f;

    // Rotation (updatePlayerCraftPhysics, normal control mode, X-wing constants).
    const bool linear = (w.settings & 0x40) != 0;
    const float x = stick_axis(in.stick_x, 75.0f, linear);
    const float y = stick_axis(in.stick_y, 80.0f, linear);
    const Vec3 right = v_cross(c.down, c.fwd);
    const bool fp = right.y < 0.0f;
    float roll = -((-(x * handling) * -30.0f) * 1.75f);
    float yaw = -(((x * handling) * 40.0f) * handling);
    float bank = bank_deg(right, c.fwd);
    if (fabs_f(bank) >= 60.0f) {
        yaw *= 0.5f;
    }
    float turn_pitch = fabs_f(yaw) * dt * (fabs_f(x) * 2.5f * (fabs_f(bank) / 60.0f));
    if ((yaw < 0.0f) == fp) {
        yaw *= 0.5f;
        roll *= 1.5f;
    }
    if (fp ? x > 0.0f : x < 0.0f) {
        turn_pitch = 0.0f;
    }
    float pitch = (-30.0f * handling) * (-(y * handling));
    if (w.settings & 0x4) {
        pitch = -pitch;
    }
    float d8 = yaw * dt;
    float dc = pitch * dt;
    float e0 = roll * dt;
    const Vec3 flat = v_norm_if_len(Vec3{c.fwd.x, 0.0f, c.fwd.z});
    const float p_abs = 90.0f - asin_a(v_dot(flat, c.fwd)) * 57.2958f;
    const bool centred = x == 0.0f && y == 0.0f;
    if ((w.settings & 0x1) && centred) {
        if (p_abs > 60.0f && (c.fwd.y < 0.0f || w.level == 10)) {
            c.level_latch = true;
        }
        if (p_abs < 0.5f) {
            c.level_latch = false;
        }
    } else {
        c.level_latch = false;
    }
    bool auto_roll = centred;
    if (p_abs >= 75.0f) {
        auto_roll = false;
    }
    if (p_abs >= 60.0f) {
        d8 = 0.0f;
    }
    if (p_abs >= 45.0f) {
        turn_pitch *= 1.0f - (p_abs - 45.0f) / 45.0f;
    }
    dc -= turn_pitch;
    if (in.freeze) {
        d8 = 0.0f;
        dc = 0.0f;
        e0 = 0.0f;
    }
    if (c.down.y < 0.0f) {
        bank = fp ? 180.0f - bank : -180.0f - bank;
    }
    if (auto_roll && fabs_f(bank) > 0.1f) {
        e0 = (-bank * 2.0f) * dt;
    }
    if (c.level_latch) {
        dc = (c.fwd.y < 0.0f ? p_abs * 0.75f : -p_abs * 0.75f) * dt;
    }
    if (fabs_f(d8) < 0.1f) {
        d8 = 0.0f;
    }
    if (fabs_f(dc) < 0.1f) {
        dc = 0.0f;
    }
    if (fabs_f(e0) < 0.1f) {
        e0 = 0.0f;
    }
    rotate_body(c, d8, dc, e0);

    c.pos = v_add(c.pos, v_scale(c.fwd, c.speed * dt));
    // World Y points down: the ground is a larger y than the craft, the ceiling a smaller one.
    if (c.pos.y > w.ground_y - 0.02f) {
        c.pos.y = w.ground_y - 0.02f;
    }
    if (c.pos.y < w.ceiling_y) {
        c.pos.y = w.ceiling_y;
    }
}

bool imposter_fire(bool fire, uint16_t* cooldown) {
    if (*cooldown > 0) {
        --*cooldown;
        return false;
    }
    if (fire) {
        *cooldown = 6;
        return true;
    }
    return false;
}

void clear_imposter_record(uint8_t* rdram) {
    for (uint32_t i = 0; i < 0x100; ++i) {
        wr8(rdram, kImposterRec + i, 0);
    }
}

Vec3 imposter_spawn_pos(const Vec3& p1, const Vec3& right) {
    return Vec3{p1.x + right.x * 1.5f, p1.y, p1.z + right.z * 1.5f};
}

bool imposter_spawn_ok(uint32_t slot, uint32_t npc, uint32_t blk, uint32_t mesh) {
    auto in_ram = [](uint32_t a) { return a >= 0x80000000u && a < 0x80000000u + (uint32_t)kRdramSize; };
    return slot < kNpcSlots && in_ram(npc) && in_ram(blk) && in_ram(mesh);
}

static uint16_t rd16(const uint8_t* rdram, uint32_t a) {
    return (uint16_t)((rd8(rdram, a) << 8) | rd8(rdram, a + 1));
}

bool npc_pool_room(const uint8_t* rdram) {
    return (int32_t)rd32(rdram, 0x80130BC8u) >= 0 && rd16(rdram, 0x80130BB4u) != 0xFFFFu;
}

bool mesh_loaded(const uint8_t* rdram, const char* name) {
    uint32_t sum = 0;
    for (const char* c = name; *c; ++c) {
        sum += (uint8_t)*c;
    }
    uint16_t idx = rd16(rdram, 0x801394B0u + (sum % 25u) * 2);
    for (int hops = 0; idx != 0xFFFFu && hops < 1024; ++hops) {
        const uint32_t entry = 0x80139020u + idx * 12u;
        if (!in_rdram(entry, 12)) {
            return false;
        }
        const uint32_t str = rd32(rdram, entry + 4);
        if (!in_rdram(str, 16)) {
            return false;
        }
        size_t i = 0;
        while (i < 16 && name[i] && rd8(rdram, str + (uint32_t)i) == (uint8_t)name[i]) {
            ++i;
        }
        if (i == 16 || (name[i] == 0 && rd8(rdram, str + (uint32_t)i) == 0)) {
            return true;
        }
        idx = rd16(rdram, entry);
    }
    return false;
}

const char* wingman_model_name(int model) {
    static const char* const kModels[8] = {"wmxwng", "wmywng", "wmawng", "wmvwng", "wmspdr", "wmfalc", "tie", "t16"};
    return (model >= 0 && model < 8) ? kModels[model] : nullptr;
}

int wingman_model(const uint8_t* rdram) {
    for (int i = 0; i < 8; ++i) {
        if (mesh_loaded(rdram, wingman_model_name(i))) {
            return i;
        }
    }
    return -1;
}

int follow_step(int cursor, int state, int target, int target_state, bool picked) {
    if (state == 3) {
        if (cursor != target || (!picked && target_state == 2)) {
            return kFollowBack;
        }
        return picked ? kFollowConfirm : kFollowNone;
    }
    if (state != 2) {
        return kFollowNone;
    }
    if (cursor != target) {
        return cursor < target ? kFollowRight : kFollowLeft;
    }
    return (picked || target_state == 3) ? kFollowConfirm : kFollowNone;
}

const char* hud_message(bool out, bool peer_left, bool peer_down, int lives) {
    if (out) {
        return "SPECTATING";
    }
    if (peer_left) {
        return "WINGMATE LEFT";
    }
    if (peer_down) {
        return lives == 0 ? "WINGMATE OUT" : "WINGMATE DOWN";
    }
    return "";
}

int browse_report(int state, uint32_t result) {
    return (state == 4 && result == 0xFFFFFEu) ? 2 : state;
}

bool TeamTriggers::on_edge(int player, uint16_t event, bool enter) {
    const uint8_t bit = (uint8_t)(1u << (player & 1));
    uint8_t& in = inside_[event];
    if (enter) {
        if (in & bit) {
            return false;
        }
        const bool first = in == 0;
        in |= bit;
        return first;
    }
    if (!(in & bit)) {
        return false;
    }
    in &= (uint8_t)~bit;
    return in == 0;
}

void TeamTriggers::forget(int player) {
    const uint8_t bit = (uint8_t)(1u << (player & 1));
    for (auto& e : inside_) {
        e.second &= (uint8_t)~bit;
    }
}

int puppet_model(int craft) {
    return ((craft >= 0 && craft <= 5) || craft == 7) ? craft : -1;
}

LifeOutcome shared_life(uint8_t pool, bool player_out) {
    if (player_out) {
        return LifeOutcome::Ignore;
    }
    if (pool >= 2) {
        return LifeOutcome::Spend;
    }
    return pool == 1 ? LifeOutcome::Out : LifeOutcome::GameOver;
}

void spectate_pad(uint8_t pad[6], uint16_t survivor_held, uint16_t fire_mask) {
    const uint16_t fire = survivor_held & fire_mask;
    pad[0] = (uint8_t)((pad[0] & 0x10) | (fire >> 8));
    pad[1] = (uint8_t)fire;
    pad[2] = 0;
    pad[3] = 0;
}

void SkipShare::on_remote(double now_s) {
    got_s_ = now_s;
    pressing_since_s_ = -1.0;
}

bool SkipShare::press(double now_s, bool cinematic) {
    if (got_s_ < 0.0) {
        return false;
    }
    if (pressing_since_s_ >= 0.0 && !cinematic) {
        got_s_ = -1.0;
        pressing_since_s_ = -1.0;
        return false;
    }
    if (now_s - got_s_ > kWaitS) {
        got_s_ = -1.0;
        pressing_since_s_ = -1.0;
        return false;
    }
    if (!cinematic) {
        return false;
    }
    if (pressing_since_s_ < 0.0) {
        pressing_since_s_ = now_s;
    }
    last_press_s_ = now_s;
    return std::fmod(now_s - pressing_since_s_, 2.0 * kPulseS) < kPulseS;
}

bool cutscene_skipped(bool was_running, uint32_t gate, uint32_t end_frame, bool running) {
    return was_running && !running && gate + 30u < end_frame;
}

void Puppet::reset(const PuppetState& at) {
    shown_ = at;
    target_ = at;
    has_target_ = false;
}

void Puppet::on_state(const PuppetState& s, double now_s, double sent_s) {
    target_ = s;
    received_s_ = now_s;
    has_target_ = true;
    if (interp_s_ <= 0.0 || sent_s < 0.0) {
        return;
    }
    // A sender restart (new mission) or reordering: the timeline starts over.
    if (!buf_.empty() && sent_s <= buf_.back().sent) {
        if (sent_s < buf_.back().sent - 1.0) {
            buf_.clear();
            offsets_.clear();
        } else {
            return;
        }
    }
    buf_.push_back({sent_s, s});
    while (buf_.size() > 2 && buf_.front().sent < sent_s - 1.5) {
        buf_.pop_front();
    }
    offsets_.push_back({now_s, now_s - sent_s});
    while (offsets_.size() > 1 && offsets_.front().first < now_s - 4.0) {
        offsets_.pop_front();
    }
    offset_ = offsets_.front().second;
    for (const auto& o : offsets_) {
        offset_ = o.second < offset_ ? o.second : offset_;
    }
}

static PuppetState lerp_state(const PuppetState& a, const PuppetState& b, float u) {
    PuppetState r;
    r.pos = v_add(a.pos, v_scale(v_add(b.pos, v_scale(a.pos, -1.0f)), u));
    r.vel = v_add(a.vel, v_scale(v_add(b.vel, v_scale(a.vel, -1.0f)), u));
    Vec3 f = v_add(a.fwd, v_scale(v_add(b.fwd, v_scale(a.fwd, -1.0f)), u));
    Vec3 d = v_add(a.down, v_scale(v_add(b.down, v_scale(a.down, -1.0f)), u));
    f = v_dot(f, f) < 1e-8f ? b.fwd : v_norm(f);
    d = v_add(d, v_scale(f, -v_dot(d, f)));
    r.fwd = f;
    r.down = v_dot(d, d) < 1e-8f ? b.down : v_norm(d);
    return r;
}

PuppetState Puppet::sample(double now_s) const {
    if (buf_.empty()) {
        return target_;
    }
    const double t = now_s - offset_ - interp_s_;
    if (t <= buf_.front().sent) {
        return buf_.front().s;
    }
    if (t >= buf_.back().sent) {
        double ahead = t - buf_.back().sent;
        ahead = ahead > kPuppetCoastS ? kPuppetCoastS : ahead;
        PuppetState r = buf_.back().s;
        r.pos = v_add(r.pos, v_scale(r.vel, (float)ahead));
        return r;
    }
    for (size_t i = 1; i < buf_.size(); ++i) {
        if (buf_[i].sent >= t) {
            const Sample& a = buf_[i - 1];
            const Sample& b = buf_[i];
            const double span = b.sent - a.sent;
            return lerp_state(a.s, b.s, span > 0.0 ? (float)((t - a.sent) / span) : 1.0f);
        }
    }
    return buf_.back().s;
}

PuppetState Puppet::step(double now_s, float dt) {
    if (!has_target_) {
        return shown_;
    }
    if (interp_s_ > 0.0 && !buf_.empty()) {
        // The interpolated track is already smooth; a light blend only covers the step from extrapolating back to real samples.
        const PuppetState raw = sample(now_s);
        const Vec3 err = v_add(raw.pos, v_scale(shown_.pos, -1.0f));
        float k = (float)(dt / 0.05);
        k = k > 1.0f ? 1.0f : (k < 0.0f ? 0.0f : k);
        const bool snap = v_dot(err, err) > kPuppetSnap * kPuppetSnap || v_dot(shown_.fwd, raw.fwd) < 0.0f;
        shown_.pos = snap ? raw.pos : v_add(shown_.pos, v_scale(err, k));
        shown_.fwd = raw.fwd;
        shown_.down = raw.down;
        shown_.vel = raw.vel;
        return shown_;
    }
    double age = now_s - received_s_;
    age = age < 0.0 ? 0.0 : (age > kPuppetCoastS ? kPuppetCoastS : age);
    const Vec3 goal = v_add(target_.pos, v_scale(target_.vel, (float)age));
    const Vec3 err = v_add(goal, v_scale(shown_.pos, -1.0f));
    float k = (float)(dt / kPuppetBlendS);
    k = k > 1.0f ? 1.0f : (k < 0.0f ? 0.0f : k);
    const float k_pos = v_dot(err, err) > kPuppetSnap * kPuppetSnap ? 1.0f : k;
    shown_.pos = v_add(shown_.pos, v_scale(err, k_pos));
    // Streamed turns move a few degrees per update; a heading more than 90 degrees off is a respawn or teleport, so it snaps (a straight blend would stall at the reversal).
    const float k_rot = v_dot(shown_.fwd, target_.fwd) < 0.0f ? 1.0f : k;
    Vec3 f = v_add(shown_.fwd, v_scale(v_add(target_.fwd, v_scale(shown_.fwd, -1.0f)), k_rot));
    Vec3 d = v_add(shown_.down, v_scale(v_add(target_.down, v_scale(shown_.down, -1.0f)), k_rot));
    if (v_dot(f, f) < 1e-8f) {
        f = target_.fwd;
    }
    f = v_norm(f);
    d = v_add(d, v_scale(f, -v_dot(d, f)));
    if (v_dot(d, d) < 1e-8f) {
        d = v_add(target_.down, v_scale(f, -v_dot(target_.down, f)));
    }
    shown_.fwd = f;
    shown_.down = v_norm(d);
    shown_.vel = target_.vel;
    return shown_;
}

void build_imposter_record(uint8_t* rdram, const Vec3& pos) {
    auto wf = [rdram](uint32_t a, float f) { uint32_t b; memcpy(&b, &f, 4); wr32(rdram, a, b); };
    clear_imposter_record(rdram);
    const uint32_t r = kImposterRec;
    wr8(rdram, r + 0x1, 0x27);
    wr8(rdram, r + 0x6, 0xFF);
    wr8(rdram, r + 0x7, 0xFF);
    wr32(rdram, r + 0x0C, 0x800A9028u);
    wf(r + 0x10, pos.x);
    wf(r + 0x14, pos.y);
    wf(r + 0x18, pos.z);
    wf(r + 0x28, 0.014f);
    wf(r + 0x2C, 0.014f);
    wf(r + 0x30, 0.014f);
    wr8(rdram, r + 0x34, 0xFF);
    wr8(rdram, r + 0x35, 0xFF);
    wr32(rdram, r + 0x54, 0x80000000u);
    // Health per difficulty (npcHealthTableIndex 0x80137CE4 selects the entry).
    wr32(rdram, r + 0x6C, 1000u);
    wr32(rdram, r + 0x70, 1000u);
    wr32(rdram, r + 0x74, 1000u);
    wr32(rdram, r + 0x90, 7u);
    wr32(rdram, r + 0x94, 0xF0u);
    // Private state (the handler reads nothing past +0xDC): S-foil t +0xE8, speed +0xEC, fire cooldown +0xF2, target +0xF4, boost hold +0xF8, level latch +0xFC, S-foils closed +0xFD.
    wf(r + 0xEC, 2.625f);
    wf(r + 0xF4, 2.625f);
    wr8(rdram, r + 0xF0, '-');
}

}
