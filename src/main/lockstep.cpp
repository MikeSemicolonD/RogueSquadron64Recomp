#include "lockstep_core.h"
#include "debug_logs.h"
#include "host_api.h"
#include "main.h"
#include "renderdoc_capture.h"
#include "recomp.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>
#include <utility>
#include <vector>

namespace {

using namespace rs64::ls;

enum class Mode { Off, Record, Replay, Delay };

struct State {
    Mode mode = Mode::Off;
    bool active = false;
    int missions = 0;
    uint32_t frame = 0;
    FILE* rec = nullptr;
    FILE* hash = nullptr;
    std::string hash_path;
    FILE* dump = nullptr;
    std::vector<FrameInput> frames;
    std::unique_ptr<DelayLine> delay;
    uint8_t throttle_q = 0xFF;
    float cruise = 0.0f;
    // Replay with ROGUESQ_LS_RECORD also set: replays one file and writes an upgraded copy (inputs replayed, audio answers live).
    bool rerecord = false;
    std::map<uint32_t, std::vector<uint8_t>> song;
    uint32_t song_frame = 0xFFFFFFFFu;
    size_t song_idx = 0;
    // ROGUESQ_LS_PAD2_REPLAY: port 1 flies a recorded port-0 run from mission frame 0.
    bool pad2 = false;
    uint32_t pad2_frame = 0;
    std::vector<FrameInput> pad2_frames;
};

State& st() {
    static State s;
    return s;
}

uint8_t rb(const uint8_t* rdram, uint32_t a) {
    return rdram[(a - 0x80000000u) ^ 3];
}

void wb(uint8_t* rdram, uint32_t a, uint8_t v) {
    rdram[(a - 0x80000000u) ^ 3] = v;
}

uint32_t rw(const uint8_t* rdram, uint32_t a) {
    uint32_t v;
    memcpy(&v, rdram + (a - 0x80000000u), 4);
    return v;
}

void ww(uint8_t* rdram, uint32_t a, uint32_t v) {
    memcpy(rdram + (a - 0x80000000u), &v, 4);
}

// Attract demos run runInMissionFrame with their own recorded dt and seed; initMission uses the same test.
bool demo_active(const uint8_t* rdram) {
    return (rw(rdram, 0x80130B50u) & 0x60u) != 0;
}

uint32_t ls_seed() {
    static const uint32_t seed = []() {
        const char* e = recomp::dbg::env_str("ROGUESQ_LS_SEED");
        if (!e) {
            return 1u;
        }
        const uint32_t v = parse_seed(e);
        if (v == 0) {
            fprintf(stderr, "[ls] ROGUESQ_LS_SEED=%s is not a nonzero number; keeping seed 1\n", e);
            return 1u;
        }
        return v;
    }();
    return seed;
}

Mode mode() {
    static const Mode m = []() {
        const bool replay = recomp::dbg::env_str("ROGUESQ_LS_REPLAY") != nullptr;
        // RECORD together with REPLAY is the re-record mode, not a conflict.
        const bool record = recomp::dbg::env_str("ROGUESQ_LS_RECORD") != nullptr && !replay;
        const bool delay = recomp::dbg::env_int("ROGUESQ_LS_INPUT_DELAY", 0) > 0;
        const Mode won = replay ? Mode::Replay : record ? Mode::Record : delay ? Mode::Delay : Mode::Off;
        if (int(replay) + int(record) + int(delay) > 1) {
            static const char* const names[] = {"off", "record", "replay", "delay"};
            fprintf(stderr, "[ls] several lockstep modes set; using %s\n", names[(int)won]);
        }
        return won;
    }();
    return m;
}

void close_files() {
    State& s = st();
    if (s.rec) {
        fclose(s.rec);
        s.rec = nullptr;
    }
    if (s.hash) {
        fclose(s.hash);
        s.hash = nullptr;
    }
    if (s.dump) {
        fclose(s.dump);
        s.dump = nullptr;
    }
}

// ROGUESQ_LS_DUMP_FRAMES=lo-hi: raw region bytes for those frames, next to the hash log.
bool dump_frame(uint32_t frame) {
    static const std::pair<long, long> range = []() {
        const char* e = recomp::dbg::env_str("ROGUESQ_LS_DUMP_FRAMES");
        long lo = -1;
        long hi = -1;
        if (e && sscanf(e, "%ld-%ld", &lo, &hi) < 2) {
            hi = lo;
        }
        return std::make_pair(lo, hi);
    }();
    return range.first >= 0 && (long)frame >= range.first && (long)frame <= range.second;
}

void end_session(const char* why) {
    State& s = st();
    fprintf(stderr, "[ls] session ended at frame %u: %s\n", s.frame, why);
    close_files();
    s.active = false;
    static const bool keep_running = recomp::dbg::env_on("ROGUESQ_LS_KEEP_RUNNING");
    if (s.mode == Mode::Replay && !keep_running) {
        rs64_menu_request_quit();
    }
}

FILE* open_hash(const std::string& path) {
    FILE* f = fopen(path.c_str(), "wb");
    if (f) {
        fprintf(f, "%s\n", format_hash_legend(rs64::host::flag("coop_imposter") != 0).c_str());
        fflush(f);
    } else {
        fprintf(stderr, "[ls] cannot write %s\n", path.c_str());
    }
    if (recomp::dbg::env_str("ROGUESQ_LS_DUMP_FRAMES")) {
        st().dump = fopen((path + ".dump").c_str(), "wb");
    }
    return f;
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

void pad2_on_mission_init(uint8_t* rdram) {
    State& s = st();
    s.pad2 = false;
    if (rs64::host::flag("ghost_mode")) {
        return;
    }
    const char* path = recomp::dbg::env_str("ROGUESQ_LS_PAD2_REPLAY");
    if (!path || !rs64::host::flag("coop_imposter") || demo_active(rdram)) {
        return;
    }
    SessionHeader h;
    std::map<uint32_t, std::vector<uint8_t>> song;
    s.pad2_frames.clear();
    if (!load_replay(path, &h, &s.pad2_frames, &song)) {
        return;
    }
    const SessionHeader running = read_session(rdram, 0);
    if (running.level != h.level || running.craft != h.craft) {
        fprintf(stderr, "[coop] player 2 disabled: %s is level %u craft %u, running level %u craft %u\n", path, h.level, h.craft, running.level, running.craft);
        return;
    }
    s.pad2 = true;
    s.pad2_frame = 0;
    fprintf(stderr, "[coop] player 2 replays %zu frames from %s\n", s.pad2_frames.size(), path);
}

}

extern "C" void rs64_ls_on_mission_init(uint8_t* rdram, recomp_context*) {
    State& s = st();
    s.mode = mode();
    pad2_on_mission_init(rdram);
    if (s.mode == Mode::Off || demo_active(rdram)) {
        return;
    }
    // Ghost co-op keeps only a recording of this player's input (for run-mp.ps1 -ClientPad): no reseed and no hashes, since the two worlds differ by design.
    const bool ghost = rs64::host::flag("ghost_mode") != 0;
    if (ghost && s.mode != Mode::Record) {
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            fprintf(stderr, "[ls] session skipped: ghost co-op is on\n");
        }
        s.mode = Mode::Off;
        return;
    }
    s.missions++;
    if (s.missions > 1 && s.mode != Mode::Delay) {
        if (s.active) {
            end_session("second initMission (one recording = one mission attempt)");
        }
        return;
    }
    s.frame = 0;
    const uint32_t seed = ls_seed();
    if (s.mode == Mode::Record) {
        const char* path = recomp::dbg::env_str("ROGUESQ_LS_RECORD");
        SessionHeader h = read_session(rdram, ghost ? rw(rdram, 0x80003470u) : seed);
        h.build = __DATE__ " " __TIME__;
        s.cruise = rs64_throttle_cruise_live();
        memcpy(&h.cruise_bits, &s.cruise, 4);
        s.rec = fopen(path, "wb");
        if (!s.rec) {
            fprintf(stderr, "[ls] cannot write %s\n", path);
            return;
        }
        fputs(format_header(h).c_str(), s.rec);
        fflush(s.rec);
        if (!ghost) {
            s.hash = open_hash(std::string(path) + ".hash");
            ww(rdram, 0x80003470u, h.seed);
        }
        fprintf(stderr, "[ls] recording level %u craft %u seed %u -> %s%s\n", h.level, h.craft, h.seed, path, ghost ? " (co-op: input only)" : "");
    } else if (s.mode == Mode::Replay) {
        const char* path = recomp::dbg::env_str("ROGUESQ_LS_REPLAY");
        SessionHeader h;
        if (!load_replay(path, &h, &s.frames, &s.song)) {
            rs64_menu_request_quit();
            return;
        }
        const SessionHeader running = read_session(rdram, 0);
        if (running.level != h.level || running.craft != h.craft) {
            fprintf(stderr, "[ls] replay mismatch: running level %u craft %u, recording is level %u craft %u\n", running.level, running.craft, h.level, h.craft);
            rs64_menu_request_quit();
            return;
        }
        apply_session(rdram, h);
        memcpy(&s.cruise, &h.cruise_bits, 4);
        ww(rdram, 0x80003470u, h.seed);
        const char* out = recomp::dbg::env_str("ROGUESQ_LS_HASH_OUT");
        const char* rerec = recomp::dbg::env_str("ROGUESQ_LS_RECORD");
        if (rerec) {
            s.rec = fopen(rerec, "wb");
            if (!s.rec) {
                fprintf(stderr, "[ls] cannot write %s\n", rerec);
                rs64_menu_request_quit();
                return;
            }
            fputs(format_header(h).c_str(), s.rec);
            fflush(s.rec);
            s.rerecord = true;
        }
        s.hash_path = out ? std::string(out) : (rerec ? std::string(rerec) + ".hash" : std::string(path) + ".replay.hash");
        s.hash = open_hash(s.hash_path);
        fprintf(stderr, "[ls] replaying %zu frames, level %u craft %u seed %u%s\n", s.frames.size(), h.level, h.craft, h.seed, rerec ? " (re-recording)" : "");
    } else {
        const int delay = clamp_delay(recomp::dbg::env_int("ROGUESQ_LS_INPUT_DELAY", 0));
        s.delay = std::make_unique<DelayLine>(delay);
        ww(rdram, 0x80003470u, seed);
        fprintf(stderr, "[ls] input delay %d frames, fixed dt\n", delay);
    }
    s.active = true;
}

extern "C" float rs64_ls_frame_dt(uint8_t* rdram, float dt) {
    State& s = st();
    if (!s.active || demo_active(rdram)) {
        return dt;
    }
    return 1.0f / 30.0f;
}

// ROGUESQ_LS_PAUSE_AT=<f>[,<f>...]: a replay holds before frame f, writing <hash log>.paused, until <hash log>.resume appears (30 s cap), so
// tools/recordings/capture-frames.ps1 can screenshot an exact frame at any ROGUESQ_SPEED. Off by default: it blocks the game thread.
static const std::vector<uint32_t>& ls_pause_frames() {
    static const std::vector<uint32_t> frames = [] {
        std::vector<uint32_t> v;
        const char* e = recomp::dbg::env_str("ROGUESQ_LS_PAUSE_AT");
        for (const char* p = e; p && *p;) {
            v.push_back((uint32_t)strtoul(p, const_cast<char**>(&p), 10));
            while (*p == ',' || *p == ' ') ++p;
        }
        return v;
    }();
    return frames;
}

// With RenderDoc loaded, a pause frame is captured from ROGUESQ_RENDERDOC_LEAD frames before it (RT64 presents a frame or two behind the game, and a held
// game sends no new display lists), over ROGUESQ_RENDERDOC_FRAMES presents.
static void ls_renderdoc_point(const State& s) {
    if (!rs64_renderdoc_active()) {
        return;
    }
    static const uint32_t lead = (uint32_t)recomp::dbg::env_int("ROGUESQ_RENDERDOC_LEAD", 2);
    static const uint32_t presents = (uint32_t)recomp::dbg::env_int("ROGUESQ_RENDERDOC_FRAMES", 3);
    const auto& frames = ls_pause_frames();
    if (std::find(frames.begin(), frames.end(), s.frame + lead) != frames.end()) {
        rs64_renderdoc_trigger(presents);
    }
}

static void ls_pause_point(const State& s) {
    const auto& frames = ls_pause_frames();
    if (frames.empty() || s.hash_path.empty() || std::find(frames.begin(), frames.end(), s.frame) == frames.end()) {
        return;
    }
    const std::string paused = s.hash_path + ".paused";
    const std::string resume = s.hash_path + ".resume";
    if (FILE* f = fopen(paused.c_str(), "wb")) {
        fprintf(f, "%u\n", s.frame);
        fclose(f);
    }
    std::error_code ec;
    const auto start = std::chrono::steady_clock::now();
    const auto until = start + std::chrono::seconds(30);
    // The resume file names the frame it releases, so a leftover from an earlier pause cannot release this one.
    auto resume_frame = [&resume]() -> long {
        FILE* f = fopen(resume.c_str(), "rb");
        if (!f) {
            return -1;
        }
        long v = -1;
        if (fscanf(f, "%ld", &v) != 1) {
            v = -1;
        }
        fclose(f);
        return v;
    };
    bool resumed = false;
    while (!(resumed = (resume_frame() == (long)s.frame)) && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    while (std::filesystem::exists(resume, ec) && !std::filesystem::remove(resume, ec) && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::filesystem::remove(paused, ec);
    const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    fprintf(stderr, "[ls] paused at frame %u: %s after %lld ms\n", s.frame, resumed ? "resumed" : "timed out", ms);
}

extern "C" void rs64_ls_frame_pads(uint8_t* rdram, recomp_context*) {
    State& s = st();
    if (s.pad2 && !demo_active(rdram)) {
        uint8_t pad[6];
        pad2_bytes(s.pad2_frame < s.pad2_frames.size() ? &s.pad2_frames[s.pad2_frame] : nullptr, pad);
        for (int i = 0; i < 6; ++i) wb(rdram, 0x80130B8Eu + i, pad[i]);
        if (s.pad2_frame == 0 || s.pad2_frame == 30) {
            auto f32 = [rdram](uint32_t a) { uint32_t b = rw(rdram, a); float f; memcpy(&f, &b, 4); return f; };
            const uint32_t blk = rw(rdram, 0x80B400E0u);
            fprintf(stderr, "[coop] frame %u: p1 pos=%.1f,%.1f,%.1f  imposter pos=%.1f,%.1f,%.1f\n", s.pad2_frame,
                f32(0x80137DC0u), f32(0x80137DC4u), f32(0x80137DC8u),
                blk ? f32(blk) : 0.0f, blk ? f32(blk + 4) : 0.0f, blk ? f32(blk + 8) : 0.0f);
        }
        s.pad2_frame++;
    }
    if (!s.active || demo_active(rdram)) {
        return;
    }
    if (s.mode == Mode::Replay) {
        ls_renderdoc_point(s);
        ls_pause_point(s);
    }
    if (s.mode == Mode::Record) {
        FrameInput f;
        f.frame = s.frame;
        for (int i = 0; i < 6; ++i) {
            f.pad[i] = rb(rdram, 0x80130B88u + i);
        }
        s.throttle_q = quantize_throttle(rs64_throttle_live());
        f.throttle = s.throttle_q;
        if (s.rec) {
            fprintf(s.rec, "%s\n", format_frame(f).c_str());
            fflush(s.rec);
        }
    } else if (s.mode == Mode::Replay) {
        if (s.frame >= s.frames.size()) {
            end_session("replay finished");
            return;
        }
        const FrameInput& f = s.frames[s.frame];
        if (f.frame != s.frame) {
            end_session("recording frame numbers out of sequence");
            return;
        }
        for (int i = 0; i < 6; ++i) {
            wb(rdram, 0x80130B88u + i, f.pad[i]);
        }
        s.throttle_q = f.throttle;
        if (s.rerecord && s.rec) {
            fprintf(s.rec, "%s\n", format_frame(f).c_str());
            fflush(s.rec);
        }
    } else {
        uint8_t pad[6];
        for (int i = 0; i < 6; ++i) {
            pad[i] = rb(rdram, 0x80130B88u + i);
        }
        s.delay->push_pop(pad);
        for (int i = 0; i < 6; ++i) {
            wb(rdram, 0x80130B88u + i, pad[i]);
        }
    }
    // Hashed next to the input write, so a killed run leaves the two files at most one line apart.
    if (s.hash) {
        fprintf(s.hash, "%s\n", format_hash_line(s.frame, hash_frame(rdram, rs64::host::flag("coop_imposter") != 0)).c_str());
        fflush(s.hash);
    }
    if (s.dump && dump_frame(s.frame)) {
        fprintf(s.dump, "frame %u\n%s", s.frame, dump_regions(rdram).c_str());
        fflush(s.dump);
    }
    s.frame++;
    // A recording that ends with the mission (success, fail) has no mission frame after its last one, so stop here.
    if (s.mode == Mode::Replay && s.frame >= s.frames.size()) {
        end_session("replay finished");
    }
}

extern "C" float rs64_ls_throttle(float live) {
    State& s = st();
    if (s.active && (s.mode == Mode::Record || s.mode == Mode::Replay)) {
        return dequantize_throttle(s.throttle_q);
    }
    return live;
}

extern "C" float rs64_ls_cruise(float live) {
    State& s = st();
    if (s.active && (s.mode == Mode::Record || s.mode == Mode::Replay)) {
        return s.cruise;
    }
    return live;
}

// tickSongFadeTimer's isSongHandleActive answer comes from the audio thread, so it is recorded per frame and replayed like input.
extern "C" uint32_t rs64_ls_song_active(uint8_t* rdram, uint32_t live) {
    State& s = st();
    if (!s.active || s.frame == 0 || demo_active(rdram)) {
        return live;
    }
    const uint32_t frame = s.frame - 1;
    if (s.mode == Mode::Record || s.rerecord) {
        if (s.rec) {
            fprintf(s.rec, "%s\n", format_song_event(frame, live != 0).c_str());
            fflush(s.rec);
        }
        return live;
    }
    if (s.mode != Mode::Replay) {
        return live;
    }
    if (s.song_frame != frame) {
        s.song_frame = frame;
        s.song_idx = 0;
    }
    const auto it = s.song.find(frame);
    if (it == s.song.end() || s.song_idx >= it->second.size()) {
        static int s_warned = 0;
        if (s_warned++ < 5) {
            fprintf(stderr, "[ls] song query on frame %u not in the recording; using the live answer\n", frame);
        }
        return live;
    }
    return it->second[s.song_idx++];
}

// ROGUESQ_RELOC_GUARD=1: poison the moved per-player ranges at their old addresses and report any write there.
extern "C" void rs64_reloc_guard_tick(uint8_t* rdram) {
    static const bool s_on = recomp::dbg::env_on("ROGUESQ_RELOC_GUARD");
    if (!s_on || !rdram) {
        return;
    }
    static bool s_armed = false;
    static uint32_t s_presents = 0;
    static int s_reports = 0;
    ++s_presents;
    if (!s_armed) {
        s_armed = true;
        poison_moved(rdram);
        fprintf(stderr, "[reloc] guard armed at present #%u\n", s_presents);
        return;
    }
    const uint32_t a = first_unpoisoned(rdram);
    if (a == 0) {
        return;
    }
    if (s_reports < 20) {
        ++s_reports;
        fprintf(stderr, "[reloc] write to old address %08X at present #%u (new home %08X)\n", a, s_presents, relocated(a));
        fflush(stderr);
    }
    poison_moved(rdram);
}
