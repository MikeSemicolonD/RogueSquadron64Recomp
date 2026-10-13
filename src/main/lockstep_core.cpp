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

std::vector<std::string> hash_part_names(const std::vector<HashPart>& extra) {
    std::vector<std::string> names = {"rng", "mission", "settings", "stats", "mission_ctl", "obj_bools", "obj_counts", "player0", "buttons", "npcs"};
    for (const HashPart& p : extra) {
        names.push_back(p.name);
    }
    return names;
}

FrameHash hash_frame(const uint8_t* rdram, const std::vector<HashPart>& extra) {
    FrameHash out;
    for (const auto& spans : region_spans()) {
        out.parts.push_back(hash_spans(rdram, spans));
    }
    out.parts.push_back(hash_npcs(rdram));
    for (const HashPart& p : extra) {
        out.parts.push_back(p.fn(rdram, p.user));
    }
    uint64_t c = 0xcbf29ce484222325ull;
    for (uint64_t p : out.parts) {
        c = mix32(mix32(c, (uint32_t)(p >> 32)), (uint32_t)p);
    }
    out.combined = c;
    return out;
}

std::string format_hash_legend(const std::vector<HashPart>& extra) {
    std::string s = "# frame combined";
    for (const std::string& n : hash_part_names(extra)) {
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

static std::vector<MovedRange>& moved_ranges_store() {
    static std::vector<MovedRange> r;
    return r;
}

void set_moved_ranges(const std::vector<MovedRange>& ranges) { moved_ranges_store() = ranges; }

const std::vector<MovedRange>& moved_ranges() { return moved_ranges_store(); }

uint32_t relocated(uint32_t kseg0) {
    for (const MovedRange& r : moved_ranges()) {
        if (kseg0 >= r.lo && kseg0 < r.hi) {
            return kseg0 + r.delta;
        }
    }
    return kseg0;
}

void poison_moved(uint8_t* rdram) {
    for (const MovedRange& r : moved_ranges()) {
        for (uint32_t a = r.lo; a < r.hi; ++a) {
            rdram[(a - 0x80000000u) ^ 3] = kRelocPoison;
        }
    }
}

uint32_t first_unpoisoned(const uint8_t* rdram) {
    for (const MovedRange& r : moved_ranges()) {
        for (uint32_t a = r.lo; a < r.hi; ++a) {
            if (rdram[(a - 0x80000000u) ^ 3] != kRelocPoison) {
                return a;
            }
        }
    }
    return 0;
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

bool cutscene_skipped(bool was_running, uint32_t gate, uint32_t end_frame, bool running) {
    return was_running && !running && gate + 30u < end_frame;
}


bool load_replay(const char* path, SessionHeader* h, std::vector<FrameInput>* frames, std::map<uint32_t, std::vector<uint8_t>>* song) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[ls] cannot read %s\n", path);
        return false;
    }
    std::string head;
    char line[256];
    bool in_frames = false;
    while (fgets(line, sizeof(line), f)) {
        if (!in_frames) {
            head += line;
            if (strncmp(line, "---", 3) == 0) {
                in_frames = true;
            }
            continue;
        }
        FrameInput fi;
        uint32_t song_frame = 0;
        bool song_active = false;
        if (parse_frame(line, &fi)) {
            frames->push_back(fi);
        } else if (parse_song_event(line, &song_frame, &song_active)) {
            (*song)[song_frame].push_back(song_active ? 1 : 0);
        } else {
            break;
        }
    }
    fclose(f);
    if (!parse_header(head, h)) {
        fprintf(stderr, "[ls] %s: bad header\n", path);
        return false;
    }
    return true;
}

bool demo_active(const uint8_t* rdram) {
    return (rd32(rdram, 0x80130B50u) & 0x60u) != 0;
}

}
