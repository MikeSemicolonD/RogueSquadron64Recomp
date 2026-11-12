#include "lockstep/lockstep_core.h"
#include "lockstep/mcp_pause.h"
#include "util/rdram_words.h"
#include "game/game_state.h"
#include "game/hook_helpers.h"
#include "util/debug_logs.h"
#include "host_api/host_api.h"
#include "main/main.h"
#include "main/crash.h"
#include "main/renderdoc_capture.h"
#include "recomp.h"
#include "rs64_toml_hooks.h"
#include "video/video_config.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>
#include <utility>

namespace {

using namespace rs64::ls;
using rs64::mips::rb;
using rs64::mips::rw;
using rs64::mips::wb;
using rs64::mips::ww;

enum class Mode { Off, Record, Replay };

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
    uint8_t throttle_q = 0xFF;
    float cruise = 0.0f;

    // Replay with ROGUESQ_LS_RECORD also set: replays one file and writes an upgraded copy (inputs replayed, audio answers live).
    bool rerecord = false;
    std::map<uint32_t, std::vector<uint8_t>> song;
    uint32_t song_frame = 0xFFFFFFFFu;
    size_t song_idx = 0;

    // The god-mode bit of gGameSettingsFlags (0x80130B4C) is set by the invincibility mod after the session hook, inside initMission.
    uint32_t god = 0;
    long flags0_at = -1;
};

constexpr uint32_t kSettingsFlags = 0x80130B4Cu;
constexpr uint32_t kGodModeBit = 0x00800000u;

State& st() {

    static State s;

    return s;
}

uint32_t ls_seed() {

    static const uint32_t seed = []() {

        const char* e = recomp::dbg::env_str("ROGUESQ_LS_SEED");

        if (!e)
        {
            return 1u;
        }

        const uint32_t v = parse_seed(e);

        if (v == 0)
        {
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

        return replay ? Mode::Replay : record ? Mode::Record : Mode::Off;
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
bool dump_frame(uint32_t frame)
{

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

void end_session(const char* why)
{

    State& s = st();

    fprintf(stderr, "[ls] session ended at frame %u: %s\n", s.frame, why);
    rs64::crash::log_suppressed();
    close_files();
    s.active = false;
    static const bool keep_running = recomp::dbg::env_on("ROGUESQ_LS_KEEP_RUNNING");

    if (s.mode == Mode::Replay && !keep_running) {
        rs64_menu_request_quit();
    }
}

FILE* open_hash(const std::string& path) {

    FILE* f = fopen(path.c_str(), "wb");

    if (f)
    {
        fprintf(f, "%s\n", format_hash_legend(rs64::host::hash_parts()).c_str());
        fflush(f);
    } else {
        fprintf(stderr, "[ls] cannot write %s\n", path.c_str());
    }

    if (recomp::dbg::env_str("ROGUESQ_LS_DUMP_FRAMES")) {
        st().dump = fopen((path + ".dump").c_str(), "wb");
    }

    return f;
}
}

extern "C" void rs64_ls_on_mission_init(uint8_t* rdram, recomp_context*)
{

    State& s = st();
    s.mode = mode();

    if (s.mode == Mode::Off || demo_active(rdram))
    {
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

    if (s.missions > 1) {
        if (s.active)
        {
            end_session("second initMission (one recording = one mission attempt)");
        }

        return;
    }

    s.frame = 0;
    const uint32_t seed = ls_seed();

    // The menu's last pad read (previous/new pressed words, 4 ports) stays latched through the mission and depends on load timing, so every session starts it cleared.
    for (uint32_t a = 0x8013A950u; a < 0x8013A970u; a += 4) ww(rdram, a, 0);

    if (s.mode == Mode::Record)
    {
        const char* path = recomp::dbg::env_str("ROGUESQ_LS_RECORD");
        SessionHeader h = read_session(rdram, ghost ? rw(rdram, 0x80003470u) : seed);
        h.build = __DATE__ " " __TIME__;
        h.widescreen = (uint32_t)rs64::video::display_value(rs64::video::Display::Widescreen);
        h.draw_pct = (uint32_t)std::lround(rs64_draw_dist_setting() * 100.0f);
        s.cruise = rs64_throttle_cruise_live();
        memcpy(&h.cruise_bits, &s.cruise, 4);
        s.rec = fopen(path, "wb");

        if (!s.rec) {
            fprintf(stderr, "[ls] cannot write %s\n", path);

            return;
        }

        const std::string head = format_header(h);

        s.flags0_at = (long)head.find("flags0=0x") + 9;
        fputs(head.c_str(), s.rec);
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

        if (running.level != h.level || running.craft != h.craft)
        {
            fprintf(stderr, "[ls] replay mismatch: running level %u craft %u, recording is level %u craft %u\n", running.level, running.craft, h.level, h.craft);
            rs64_menu_request_quit();

            return;
        }

        apply_session(rdram, h);
        s.god = h.flags0 & kGodModeBit;
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
    }

    s.active = true;
}

extern "C" float rs64_ls_frame_dt(uint8_t* rdram, float dt) {

    State& s = st();

    if (!s.active || demo_active(rdram))
    {
        return dt;
    }

    uint32_t flags = rw(rdram, kSettingsFlags);

    if (s.mode == Mode::Replay) {
        ww(rdram, kSettingsFlags, (flags & ~kGodModeBit) | s.god);
    } else if (s.mode == Mode::Record && s.rec && s.flags0_at > 0) {
        // A recording plays mortal even with the invincibility mod on, unless ROGUESQ_LS_GOD=1 asks for an invincible one.
        static const bool s_god = recomp::dbg::env_on("ROGUESQ_LS_GOD");

        if (!s_god && (flags & kGodModeBit))
        {
            flags &= ~kGodModeBit;
            ww(rdram, kSettingsFlags, flags);

            fprintf(stderr, "[ls] invincibility off for this recording (ROGUESQ_LS_GOD=1 keeps it)\n");
        }

        fflush(s.rec);
        fseek(s.rec, s.flags0_at, SEEK_SET);
        fprintf(s.rec, "%08X", flags);
        fseek(s.rec, 0, SEEK_END);
        s.flags0_at = -1;
    }

    return 1.0f / 30.0f;
}

// ROGUESQ_LS_PAUSE_AT=<f>[,<f>...]: a replay holds before frame f, writing <hash log>.paused, until <hash log>.resume appears (30 s cap), so
// tools/debug/recordings/capture-frames.ps1 can screenshot an exact frame at any ROGUESQ_SPEED. Off by default: it blocks the game thread.
static std::vector<uint32_t> env_frame_list(const char* name) {

    std::vector<uint32_t> v;
    const char* e = recomp::dbg::env_str(name);

    for (const char* p = e; p && *p;) {
        v.push_back((uint32_t)strtoul(p, const_cast<char**>(&p), 10));

        while (*p == ',' || *p == ' ') ++p;
    }

    std::sort(v.begin(), v.end());

    return v;
}

static const std::vector<uint32_t>& ls_pause_frames()
{

    static const std::vector<uint32_t> frames = env_frame_list("ROGUESQ_LS_PAUSE_AT");

    return frames;
}

// With RenderDoc loaded, a pause frame is captured from ROGUESQ_RENDERDOC_LEAD frames before it (RT64 presents a frame or two behind the game, and a held
// game sends no new display lists), over ROGUESQ_RENDERDOC_FRAMES presents.
static void ls_renderdoc_point(const State& s)
{

    if (!rs64_renderdoc_active())
    {
        return;
    }

    static const uint32_t lead = (uint32_t)recomp::dbg::env_int("ROGUESQ_RENDERDOC_LEAD", 2);
    static const uint32_t presents = (uint32_t)recomp::dbg::env_int("ROGUESQ_RENDERDOC_FRAMES", 3);
    const auto& frames = ls_pause_frames();

    if (std::find(frames.begin(), frames.end(), s.frame + lead) != frames.end())
    {
        rs64_renderdoc_trigger(presents);
    }
}

bool hold_until(const std::function<bool()>& released, std::chrono::steady_clock::time_point until)
{

    bool done = false;

    while (!(done = released()) && std::chrono::steady_clock::now() < until)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    return done;
}

// Writes <base>.paused (the frame) and holds the calling game thread until <base>.resume names that frame (30 s cap).
static void hold_for_capture(const std::string& base, uint32_t frame, const char* tag)
{

    const std::string paused = base + ".paused";
    const std::string resume = base + ".resume";

    if (FILE* f = fopen(paused.c_str(), "wb")) {
        fprintf(f, "%u\n", frame);
        fclose(f);
    }

    std::error_code ec;
    const auto start = std::chrono::steady_clock::now();
    const auto until = start + std::chrono::seconds(30);

    // The resume file names the frame it releases, so a leftover from an earlier pause cannot release this one.
    auto resume_frame = [&resume]() -> long {

        FILE* f = fopen(resume.c_str(), "rb");

        if (!f)
        {
            return -1;
        }

        long v = -1;

        if (fscanf(f, "%ld", &v) != 1) {
            v = -1;
        }

        fclose(f);

        return v;
    };

    const bool resumed = hold_until([&]() { return resume_frame() == (long)frame; }, until);

    while (std::filesystem::exists(resume, ec) && !std::filesystem::remove(resume, ec) && std::chrono::steady_clock::now() < until)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::filesystem::remove(paused, ec);
    const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

    fprintf(stderr, "[%s] paused at frame %u: %s after %lld ms\n", tag, frame, resumed ? "resumed" : "timed out", ms);
}

static void ls_pause_point(const State& s)
{

    const auto& frames = ls_pause_frames();

    if (frames.empty() || s.hash_path.empty() || std::find(frames.begin(), frames.end(), s.frame) == frames.end())
    {
        return;
    }

    hold_for_capture(s.hash_path, s.frame, "ls");
}

// ROGUESQ_PAUSE_AT=<clock>[:<f>][,...] with ROGUESQ_PAUSE_FILE=<base>: hold the game thread once a clock reaches frame f, by the replay pause's handshake.
// Clocks: a game state id (frames since it was entered; menu covers menu.*), cutscene (the file's frames) or crawl (its clock at 30 a second).
static std::vector<PauseTarget>& pause_targets()
{

    static const char* spec = recomp::dbg::env_str("ROGUESQ_PAUSE_AT");
    static std::vector<PauseTarget> v = parse_pause_targets(spec ? spec : "");

    return v;
}

// The latest frame of every clock and when it was reached, so a capture can be named by game time instead of RenderDoc's present count.
static std::mutex s_clock_mtx;
static std::map<std::string, std::pair<uint32_t, std::chrono::steady_clock::time_point>> s_clock_seen;

static void note_clock(const char* clock, uint32_t frame)
{

    std::lock_guard<std::mutex> lock(s_clock_mtx);

    s_clock_seen[clock] = { frame, std::chrono::steady_clock::now() };
}

std::string rs64_clock_label()
{

    using namespace std::chrono;

    std::lock_guard<std::mutex> lock(s_clock_mtx);
    steady_clock::time_point newest{};

    for (const auto& [name, seen] : s_clock_seen) {
        newest = std::max(newest, seen.second);
    }

    std::string label;

    // Clocks reached within a few frames of the newest pause point are the ones running now.
    for (const auto& [name, seen] : s_clock_seen) {
        if (newest - seen.second > milliseconds(100)) {
            continue;
        }

        label += (label.empty() ? "" : "_") + name + "-" + std::to_string(seen.first);
    }

    return label;
}

extern "C" void rs64_pause_point(const char* clock, uint32_t frame)
{

    static const char* base = recomp::dbg::env_str("ROGUESQ_PAUSE_FILE");

    note_clock(clock, frame);

#ifdef RS64_MCP

    mcp_note_clock(clock, frame);
    mcp_pause_point(clock);

#endif

    if (!base || !*base || pause_targets().empty())
    {
        return;
    }

    const std::string c = clock;

    // Targets fire in the order listed, so a later one can wait for an earlier state (attract.demo:30,cutscene:580 = the intro after a demo).
    for (PauseTarget& t : pause_targets())
    {
        if (t.done) continue;

        if (!pause_clock_matches(t.clock, c) || frame < t.frame) return;

        t.done = true;

        if (frame != t.frame) fprintf(stderr, "[pause] %s frame %u reached at %u\n", t.clock.c_str(), t.frame, frame);

        hold_for_capture(base, t.frame, c.c_str());
    }
}

// Set by the menu and mission hooks' tick, cleared by the next frame submit.
static bool s_state_hooked = false;

static void state_tick()
{

    static std::string last;
    static uint32_t frames = 0;
    const std::string id = rs64_state_current_id();

    frames = id == last ? frames + 1 : 0;
    last = id;
    rs64_pause_point(id.c_str(), frames);
}

// The game state's clock: frames since the classifier last changed state, from the per-frame menu and mission hooks.
extern "C" void rs64_pause_state_point()
{

    s_state_hooked = true;
    state_tick();
}

// Every screen's frame submit (submitGfxFrame): ticks the state clock on screens no menu or mission hook ticked since the last submit, such as the craft-select hangar.
extern "C" void rs64_pause_frame_point()
{

    if (s_state_hooked)
    {
        s_state_hooked = false;
        return;
    }

    state_tick();
}

// The cutscene clock: gateCtr 0x800B0B28, the file's own frames as the RS64 Explorer timeline counts them, while a cutscene is loaded.
extern "C" void rs64_cine_pause_point(uint8_t* rdram)
{

    const uint32_t cut = rw(rdram, 0x800B1904u);

    if (cut < 0x80000000u || cut >= 0x80800000u)
    {
        return;
    }

    rs64_pause_point("cutscene", rw(rdram, 0x800B0B28u));
}

// ROGUESQ_INPUT_RECORD=<file> records controller 0 from the first input poll after launch (menus included); ROGUESQ_INPUT_REPLAY=<file> plays it back from the same point.
// Lines after the header: "<ms> <vi> <poll> <buttons hex> <x> <y>". v4 (written now) has one line per poll; replay serves the recorded sample nearest the current wall-clock ms (within half a poll). v3 (older) was written on change and replays by wall-clock ms with the stick blended between close samples.
namespace {

struct InputRec { uint32_t ms, vi, poll; uint16_t buttons; float x, y; };

}

bool rs64::ls::session_input(int controller_num, uint16_t* buttons, float* x, float* y, LiveInput live)
{

    static const char* s_rec_path = recomp::dbg::env_str("ROGUESQ_INPUT_RECORD");
    static const char* s_play_path = recomp::dbg::env_str("ROGUESQ_INPUT_REPLAY");
    static bool s_anchored = false;
    static uint32_t s_poll = 0;
    static unsigned s_vi0 = 0;
    static std::chrono::steady_clock::time_point s_t0;

    if ((controller_num == 0) && (s_rec_path || s_play_path)) {
        if (!s_anchored)
        {
            s_anchored = true;
            s_poll = 0;
            s_vi0 = g_vi_tick;
            s_t0 = std::chrono::steady_clock::now();

            fprintf(stderr, "[input-rec] first poll: %s\n", s_play_path ? "replaying" : "recording");
        }
    }

    const uint32_t vi = s_anchored ? (uint32_t)(g_vi_tick - s_vi0) : 0u;
    const uint32_t ms = s_anchored ? (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s_t0).count() : 0u;

    if ((controller_num == 0) && s_play_path && s_anchored)
    {
        static std::vector<InputRec> s_recs;
        static size_t s_idx = 0;
        static bool s_loaded = false;
        static int s_version = 0;

        if (!s_loaded)
        {
            s_loaded = true;

            if (FILE* f = fopen(s_play_path, "r"))
            {
                char header[32] = {};

                if ((fscanf(f, "%31s v%d", header, &s_version) == 2) && (s_version >= 3))
                {
                    InputRec r{};
                    unsigned b = 0;

                    while (fscanf(f, "%u %u %u %x %f %f", &r.ms, &r.vi, &r.poll, &b, &r.x, &r.y) == 6)
                    {
                        r.buttons = (uint16_t)b;
                        s_recs.push_back(r);
                    }
                }

                fclose(f);
            }

            fprintf(stderr, "[input-rec] loaded %zu entries (v%d) from %s\n", s_recs.size(), s_version, s_play_path);
        }

        if ((s_version >= 4) && !s_recs.empty())
        {
            // Every poll was recorded with its time, so serve the recorded sample nearest to now: at most half a poll off, and no lag builds up when the game polls at a slightly different rate.
            auto it = std::lower_bound(s_recs.begin() + (ptrdiff_t)s_idx, s_recs.end(), ms, [](const InputRec& r, uint32_t t) { return r.ms < t; });
            size_t j = (it == s_recs.end()) ? (s_recs.size() - 1) : (size_t)(it - s_recs.begin());

            if ((j > 0) && ((it == s_recs.end()) || ((s_recs[j].ms - ms) > (ms - s_recs[j - 1].ms))))
            {
                j--;
            }

            s_idx = std::max(s_idx, j);
        }
        else {
            while ((s_idx + 1 < s_recs.size()) && (s_recs[s_idx + 1].ms <= ms)) {
                s_idx++;
            }
        }

        // Close the game once the recording has played out (the last entry is written when the recorded session ended).
        static bool s_quit = false;

        if (!s_quit && !s_recs.empty() && (ms > s_recs.back().ms + 500u))
        {
            s_quit = true;

            fprintf(stderr, "[input-rec] replay finished at ms=%u, closing\n", ms);
            rs64_menu_request_quit();
        }

        const bool have = !s_recs.empty() && ((s_version >= 4) || (s_recs[s_idx].ms <= ms));
        *buttons = have ? s_recs[s_idx].buttons : 0;
        *x = have ? s_recs[s_idx].x : 0.0f;
        *y = have ? s_recs[s_idx].y : 0.0f;

        // v3 only: blend the stick between samples of continuous motion (mouse steering) so a poll that lands a few ms off still reads nearly the recorded value; held values and sudden changes still step.
        if (have && (s_version < 4) && (s_idx + 1 < s_recs.size())) {
            const InputRec& a = s_recs[s_idx];
            const InputRec& b = s_recs[s_idx + 1];

            if ((b.ms > a.ms) && (b.ms - a.ms <= 50u)) {
                const float t = (float)(ms - a.ms) / (float)(b.ms - a.ms);
                *x = a.x + (b.x - a.x) * t;
                *y = a.y + (b.y - a.y) * t;
            }
        }

        // Drift check: our VI/poll counts at this time vs the recorded ones.
        static uint32_t s_last_log = 0;

        if (have && (ms >= s_last_log + 10000))
        {
            s_last_log = ms;

            fprintf(stderr, "[input-rec] ms=%u vi=%u recorded_vi=%u poll=%u recorded_poll=%u\n", ms, vi, s_recs[s_idx].vi, s_poll, s_recs[s_idx].poll);
        }

        s_poll++;

        return true;
    }

    const bool ok = live(controller_num, buttons, x, y);

    if ((controller_num == 0) && s_rec_path && s_anchored)
    {
        static FILE* s_file = [](const char* p) { FILE* f = fopen(p, "w"); if (f) fprintf(f, "rs64-input v4\n"); return f; }(s_rec_path);

        if (s_file)
        {
            fprintf(s_file, "%u %u %u %04X %.9g %.9g\n", ms, vi, s_poll, *buttons, *x, *y);
            fflush(s_file);
        }

        s_poll++;
    }

    return ok;
}

extern "C" void rs64_ls_frame_pads(uint8_t* rdram, recomp_context*) {

    State& s = st();

    if (!s.active || demo_active(rdram))
    {
        return;
    }

    // The "ls" clock: frames since this session's mission started, which restarts with every chained mission (the state clock may not).
    rs64_pause_point("ls", s.frame);

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
    }
}

// The same hook point after every mod's handler (registered last), so a mod's input writes this frame (the multiplayer mod's player 2) are in the hash.
extern "C" void rs64_ls_frame_hash(uint8_t* rdram, recomp_context*)
{

    State& s = st();

    if (!s.active || demo_active(rdram))
    {
        return;
    }

    // Hashed next to the input write, so a killed run leaves the two files at most one line apart.
    if (s.hash)
    {
        fprintf(s.hash, "%s\n", format_hash_line(s.frame, hash_frame(rdram, rs64::host::hash_parts())).c_str());
        fflush(s.hash);
    }

    if (s.dump && dump_frame(s.frame))
    {
        fprintf(s.dump, "frame %u\n%s", s.frame, dump_regions(rdram).c_str());
        fflush(s.dump);
    }

    s.frame++;

    // A recording that ends with the mission (success, fail) has no mission frame after its last one, so stop here.
    if (s.mode == Mode::Replay && s.frame >= s.frames.size()) {
        end_session("replay finished");
    }
}

extern "C" int rs64_ls_session(void) {
    return mode() != Mode::Off;
}

extern "C" int rs64_ls_replaying(void) {
    return st().mode == Mode::Replay ? 1 : 0;
}

extern "C" float rs64_ls_throttle(float live) {

    State& s = st();

    if (s.active && (s.mode == Mode::Record || s.mode == Mode::Replay))
    {
        return dequantize_throttle(s.throttle_q);
    }

    return live;
}

extern "C" float rs64_ls_cruise(float live) {

    State& s = st();

    if (s.active && (s.mode == Mode::Record || s.mode == Mode::Replay))
    {
        return s.cruise;
    }

    return live;
}

// tickSongFadeTimer's isSongHandleActive answer comes from the audio thread, so it is recorded per frame and replayed like input.
extern "C" uint32_t rs64_ls_song_active(uint8_t* rdram, uint32_t live) {

    State& s = st();

    if (!s.active || s.frame == 0 || demo_active(rdram))
    {
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

    if (s.mode != Mode::Replay)
    {
        return live;
    }

    if (s.song_frame != frame) {
        s.song_frame = frame;
        s.song_idx = 0;
    }

    const auto it = s.song.find(frame);

    if (it == s.song.end() || s.song_idx >= it->second.size()) {
        static int s_warned = 0;

        if (s_warned++ < 5)
        {
            fprintf(stderr, "[ls] song query on frame %u not in the recording; using the live answer\n", frame);
        }

        return live;
    }

    return it->second[s.song_idx++];
}

// ROGUESQ_RELOC_GUARD=1: poison the moved per-player ranges at their old addresses and report any write there.
extern "C" void rs64_reloc_guard_tick(uint8_t* rdram) {

    static const bool s_on = recomp::dbg::env_on("ROGUESQ_RELOC_GUARD");

    if (!s_on || !rdram)
    {
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

    if (a == 0)
    {
        return;
    }

    if (s_reports < 20)
    {
        ++s_reports;

        fprintf(stderr, "[reloc] write to old address %08X at present #%u (new home %08X)\n", a, s_presents, relocated(a));
        fflush(stderr);
    }

    poison_moved(rdram);
}
