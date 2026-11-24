#include "lockstep/lockstep_core.h"
#include "lockstep/mcp_pause.h"
#include "util/rdram_words.h"
#include "game/game_state.h"
#include "game/hook_helpers.h"
#include "game/nav_sequencer.h"
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

// Recorded audio-thread answers by mission frame, served in query order within a frame.
struct AudioAnswers {
    std::map<uint32_t, std::vector<uint8_t>> by_frame;
    uint32_t frame = 0xFFFFFFFFu;
    size_t idx = 0;
};

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
    bool head = false;
    int8_t head_x = 0;
    int8_t head_y = 0;

    // Replay with ROGUESQ_LS_RECORD also set: replays one file and writes an upgraded copy (inputs replayed, audio answers live).
    bool rerecord = false;
    bool resume = false;
    AudioAnswers song;
    AudioAnswers speech;

    // The god-mode bit of gGameSettingsFlags (0x80130B4C) is set by the invincibility mod after the session hook, inside initMission.
    uint32_t god = 0;
    long flags0_at = -1;

    // Mission logic rate of this session, and this frame's final dt (rs64_ls_rate_dt) above 30.
    uint32_t hz = 30;
    float dt = 0.0f;
};

constexpr uint32_t kSettingsFlags = 0x80130B4Cu;
constexpr uint32_t kGodModeBit = 0x00800000u;

State& st() {

    static State s;

    return s;
}

// ROGUESQ_LS_SEED for record mode; 0 keeps the game's own RNG state.
uint32_t ls_seed() {

    static const uint32_t seed = []() {

        const char* e = recomp::dbg::env_str("ROGUESQ_LS_SEED");

        if (!e)
        {
            return 0u;
        }

        const uint32_t v = parse_seed(e);

        if (v == 0)
        {
            fprintf(stderr, "[ls] ROGUESQ_LS_SEED=%s is not a nonzero number; keeping the game's own seed\n", e);
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

// ROGUESQ_LS_REPLAY=<a.rec>;<b.rec>;...: one recording per mission, in order, in one launch.
const std::vector<std::string>& playlist()
{

    static const std::vector<std::string> list = [] {

        std::vector<std::string> v;
        const char* e = recomp::dbg::env_str("ROGUESQ_LS_REPLAY");
        const std::string all = e ? e : "";

        for (size_t at = 0; at < all.size();) {
            size_t end = all.find(';', at);

            if (end == std::string::npos) end = all.size();

            if (end > at) v.push_back(all.substr(at, end - at));

            at = end + 1;
        }

        return v;
    }();

    return list;
}

// Mission init leaves parts of gPlayers[0], the life count and the rumble handle counter (handles land in gPlayers+0x1C0) from the previous mission;
// a chained replay restores what the first mission started with.
constexpr uint32_t kPlayer0 = 0x80137DB8u;
constexpr uint32_t kPlayerSize = 0x2A0u;
constexpr uint32_t kNumLives = 0x80130B10u;
constexpr uint32_t kRumbleNextHandle = 0x80110744u;

struct FreshStart {
    bool taken = false;
    std::vector<uint8_t> player;
    uint8_t lives = 0;
    uint32_t rumble_handle = 0;
};

void fresh_start(uint8_t* rdram, bool restore)
{

    static FreshStart s_fresh;

    if (!restore) {
        s_fresh.player.resize(kPlayerSize);

        for (uint32_t i = 0; i < kPlayerSize; ++i) {
            s_fresh.player[i] = rb(rdram, kPlayer0 + i);
        }

        s_fresh.lives = rb(rdram, kNumLives);
        s_fresh.rumble_handle = rw(rdram, kRumbleNextHandle);
        s_fresh.taken = true;

        return;
    }

    if (!s_fresh.taken)
    {
        return;
    }

    // +0x4 and +0xC0 are heap pointers into the loaded level.
    for (uint32_t i = 0; i < kPlayerSize; ++i) {
        if ((i & ~3u) == 0x4 || (i & ~3u) == 0xC0) continue;

        wb(rdram, kPlayer0 + i, s_fresh.player[i]);
    }

    wb(rdram, kNumLives, s_fresh.lives);
    ww(rdram, kRumbleNextHandle, s_fresh.rumble_handle);
}

// ROGUESQ_LS_HEAP_DUMP=<folder>: at each lockstep mission start (and at mission frame ROGUESQ_LS_HEAP_DUMP_AT), the game heap's blocks (initMainHeap's list from 0x8010FF18:
// u16 state 1 free / 2 used / 6 reserved / 8 end, u16 allocation flags, u32 size, data at +0x10) go to <folder>/missionNN_levelL[_fF].heap, with a summary line in the log.
void dump_game_heap(const uint8_t* rdram, int mission, uint32_t level, long frame)
{

    const char* dir = recomp::dbg::env_str("ROGUESQ_LS_HEAP_DUMP");

    if (!dir)
    {
        return;
    }

    char name[80];

    if (frame < 0) {
        snprintf(name, sizeof(name), "mission%02d_level%u.heap", mission, level);
    } else {
        snprintf(name, sizeof(name), "mission%02d_level%u_f%ld.heap", mission, level, frame);
    }

    const std::string path = (std::filesystem::path(dir) / name).string();
    FILE* f = fopen(path.c_str(), "wb");
    uint32_t blk = rw(rdram, 0x8010FF18u);
    uint32_t used = 0;
    uint32_t used_bytes = 0;
    uint32_t free_bytes = 0;
    uint32_t largest_free = 0;

    for (int n = 0; n < 200000 && blk >= 0x80000000u && blk < 0x807FFFF0u; ++n) {
        const uint32_t state = ((uint32_t)rb(rdram, blk) << 8) | rb(rdram, blk + 1);
        const uint32_t tag = ((uint32_t)rb(rdram, blk + 2) << 8) | rb(rdram, blk + 3);
        const uint32_t size = rw(rdram, blk + 4);

        if (state & 8) break;

        if (f) fprintf(f, "%08X %u %04X %08X\n", blk, state, tag, size);

        if (state == 2) {
            used++;
            used_bytes += size;
        } else if (state == 1) {
            free_bytes += size;
            largest_free = std::max(largest_free, size);
        }

        blk += 0x10 + size;
    }

    if (f) fclose(f);

    // The texture pool (getLoadedTextureCount): texture data grows up from 0x8011A7D8, TLUTs down from 0x8011A7D0; loadLevelTextureCache sizes the level's cache from the gap.
    fprintf(stderr, "[heap] mission %d level %u: %u blocks used (%u bytes), %u bytes free, largest free %u; texture pool data %08X tlut %08X -> %s\n", mission, level, used, used_bytes,
            free_bytes, largest_free, rw(rdram, 0x8011A7D8u), rw(rdram, 0x8011A7D0u), path.c_str());
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

void end_session(uint8_t* rdram, const char* why)
{

    State& s = st();

    fprintf(stderr, "[ls] session ended at frame %u: %s\n", s.frame, why);
    rs64::crash::log_suppressed();
    close_files();
    s.active = false;
    static const bool keep_running = recomp::dbg::env_on("ROGUESQ_LS_KEEP_RUNNING");

    if (s.mode != Mode::Replay || keep_running)
    {
        return;
    }

    // A playlist names the next recording's level and craft to the nav chain instead of quitting.
    if (s.missions > 0 && (size_t)s.missions < playlist().size()) {
        const std::string& next = playlist()[(size_t)s.missions];
        SessionHeader h;
        std::vector<FrameInput> frames;
        std::map<uint32_t, std::vector<uint8_t>> song;

        if (load_replay(next.c_str(), &h, &frames, &song))
        {
            // SELECT LEVEL lists the bonus levels from these unlock and passcode words, so the next pilot's are in place before it opens.
            ww(rdram, 0x80130B4Cu, h.flags0);
            ww(rdram, 0x80130B58u, h.cheats0);
            ww(rdram, 0x80130B5Cu, h.cheats1);

            fprintf(stderr, "[ls] playlist: next %s (level %u craft %u)\n", next.c_str(), h.level, h.craft);
            rs64_nav_chain_next((int)h.level, (int)h.craft, true);

            return;
        }
    }

    rs64_menu_request_quit();
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

    // ROGUESQ_LS_EACH_MISSION=1 (smoke tests chaining levels in one launch): every mission starts a new session, rewriting the recording.
    static const bool each = recomp::dbg::env_on("ROGUESQ_LS_EACH_MISSION") || playlist().size() > 1;

    // A live recording keeps going through a retry or the next mission, into <name>_try<N>.rec: one file per mission attempt.
    const bool next_file = s.mode == Mode::Record;

    if (s.missions > 1) {
        if (s.active)
        {
            end_session(rdram, each ? "next mission" : next_file ? "mission restarted, recording the next attempt" : "second initMission (one recording = one mission attempt)");
        }

        if (!each && !next_file) return;

        const int missions = s.missions;

        s = State{};
        s.mode = mode();
        s.missions = missions;
    }

    s.frame = 0;
    const uint32_t seed = ls_seed();

    // The menu's last pad read (previous/new pressed words, 4 ports) stays latched through the mission and depends on load timing, so every session starts it cleared.
    for (uint32_t a = 0x8013A950u; a < 0x8013A970u; a += 4) ww(rdram, a, 0);

    if (s.mode == Mode::Record)
    {
        const std::string path = attempt_path(recomp::dbg::env_str("ROGUESQ_LS_RECORD"), each ? 1 : s.missions);
        // The game's own RNG state is saved and the replay restores it, so a recorded mission plays as randomly as an unrecorded one; ROGUESQ_LS_SEED forces one.
        SessionHeader h = read_session(rdram, (ghost || seed == 0) ? rw(rdram, 0x80003470u) : seed);
        h.build = __DATE__ " " __TIME__;
        h.widescreen = (uint32_t)rs64::video::display_value(rs64::video::Display::Widescreen);
        h.draw_pct = (uint32_t)std::lround(rs64_draw_dist_setting() * 100.0f);
        s.cruise = rs64_throttle_cruise_live();
        memcpy(&h.cruise_bits, &s.cruise, 4);
        h.ship = rs64_ships_flying_id();
        h.hz = ghost ? 30u : (uint32_t)rs64_logic_hz();
        s.hz = h.hz;
        s.rec = fopen(path.c_str(), "wb");

        if (!s.rec) {
            fprintf(stderr, "[ls] cannot write %s\n", path.c_str());

            return;
        }

        const std::string head = format_header(h);

        s.flags0_at = (long)head.find("flags0=0x") + 9;
        fputs(head.c_str(), s.rec);
        fflush(s.rec);

        if (!ghost) {
            s.hash = open_hash(path + ".hash");
            ww(rdram, 0x80003470u, h.seed);
        }

        fprintf(stderr, "[ls] recording level %u craft %u seed %u at %u Hz -> %s%s\n", h.level, h.craft, h.seed, h.hz, path.c_str(), ghost ? " (co-op: input only)" : "");
    } else if (s.mode == Mode::Replay) {
        const size_t entry = (size_t)s.missions - 1;

        if (entry >= playlist().size())
        {
            fprintf(stderr, "[ls] playlist: mission %d has no recording\n", s.missions);
            rs64_menu_request_quit();

            return;
        }

        const char* path = playlist()[entry].c_str();
        SessionHeader h;

        if (!load_replay(path, &h, &s.frames, &s.song.by_frame, &s.speech.by_frame)) {
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

        dump_game_heap(rdram, s.missions, h.level, -1);
        fresh_start(rdram, entry > 0);
        apply_session(rdram, h);

        // Widescreen and DRAW DISTANCE change the object culls; the first entry's were set at launch, before the level loads.
        if (entry > 0 && h.draw_pct != 0)
        {
            using rs64::video::Display;

            if ((uint32_t)rs64::video::display_value(Display::Widescreen) != (h.widescreen ? 1u : 0u)) rs64::video::step_display(Display::Widescreen);

            rs64::video::set_draw_distance((float)h.draw_pct / 100.0f);

            fprintf(stderr, "[ls] replay display: widescreen=%u draw=%.2f (from the recording)\n", h.widescreen, h.draw_pct / 100.0);
        }

        if (!h.ship.empty() && !rs64_ships_fly(h.ship.c_str()))
        {
            fprintf(stderr, "[ls] the recording flew ship %s, which no enabled mod has: its template craft is shown\n", h.ship.c_str());
        }

        s.god = h.flags0 & kGodModeBit;
        s.hz = h.hz ? h.hz : 30u;
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

            // ROGUESQ_LS_RESUME=<frame>: replay only the frames before it, then keep recording live input into the new file (rewind to a checkpoint).
            const long resume = recomp::dbg::env_int("ROGUESQ_LS_RESUME", -1);

            if (resume >= 0 && (size_t)resume <= s.frames.size()) {
                s.frames.resize((size_t)resume);
                for (AudioAnswers* a : {&s.song, &s.speech}) {
                    a->by_frame.erase(a->by_frame.lower_bound((uint32_t)resume), a->by_frame.end());
                }
                s.resume = true;
            }
        }

        s.hash_path = out ? std::string(out) : (rerec ? std::string(rerec) + ".hash" : std::string(path) + ".replay.hash");

        // With a playlist, ROGUESQ_LS_HASH_OUT is a folder holding one <name>.replay.hash per recording.
        if (out && playlist().size() > 1)
        {
            s.hash_path = (std::filesystem::path(out) / (std::filesystem::path(path).stem().string() + ".replay.hash")).string();
        }
        s.hash = open_hash(s.hash_path);

        fprintf(stderr, "[ls] replaying %zu frames, level %u craft %u seed %u at %u Hz%s\n", s.frames.size(), h.level, h.craft, h.seed, s.hz, rerec ? " (re-recording)" : "");
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

    return s.hz > 30 ? dt : 1.0f / 30.0f;
}

// A high-rate session keeps real time while recording: each frame's final dt goes into its line, and a replay steps the same dts (1/hz for lines without one).
extern "C" float rs64_ls_rate_dt(float dt)
{

    State& s = st();

    if (!s.active || s.hz <= 30)
    {
        return dt;
    }

    if (s.mode == Mode::Replay && s.frame < s.frames.size()) {
        const float rec = s.frames[s.frame].dt;

        dt = rec > 0.0f ? rec : 1.0f / (float)s.hz;
    }

    s.dt = dt;

    return dt;
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

    // Clocks reached within a few frames of the newest pause point are the ones running now.
    const auto running = [&](const char* name) {

        const auto it = s_clock_seen.find(name);

        return it != s_clock_seen.end() && newest - it->second.second <= milliseconds(100);
    };

    const auto part = [&](const std::string& name) { return name + "-" + std::to_string(s_clock_seen[name].first); };

    std::string label;

    if (running("cutscene")) {
        label = part("cutscene");
    } else if (running("crawl")) {
        label = part("crawl");
    }

    // ingame counts the mission's frames, so it replaces the mission state; the state is left out under a cutscene or crawl.
    if (running("ingame"))
    {
        return label + (label.empty() ? "" : "_") + part("ingame");
    }

    if (!label.empty())
    {
        return label;
    }

    std::string state;
    steady_clock::time_point latest{};

    for (const auto& [name, seen] : s_clock_seen) {
        if (name != "nav" && seen.second >= latest) {
            state = name;
            latest = seen.second;
        }
    }

    return state.empty() ? label : part(state);
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
    rs64_nav_clock_point();
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

extern "C" void rs64_ls_frame_pads(uint8_t* rdram, recomp_context*) {

    State& s = st();

    if (!s.active || demo_active(rdram))
    {
        return;
    }

    // The "ingame" clock: frames since this recording or replay's mission started (the recording's frame numbers), which restarts with every chained mission (the state clock may not).
    rs64_pause_point("ingame", s.frame);

    if (s.mode == Mode::Replay) {
        ls_renderdoc_point(s);
        ls_pause_point(s);
    }

    if (s.mode == Mode::Replay && s.resume && s.frame >= s.frames.size())
    {
        s.mode = Mode::Record;
        s.rerecord = false;

        fprintf(stderr, "[ls] resumed live recording at frame %u\n", s.frame);
    }

    if (s.mode == Mode::Record) {
        FrameInput f;
        f.frame = s.frame;

        for (int i = 0; i < 6; ++i) {
            f.pad[i] = rb(rdram, 0x80130B88u + i);
        }

        s.throttle_q = quantize_throttle(rs64_throttle_live());
        f.throttle = s.throttle_q;

        float hx = 0.0f;
        float hy = 0.0f;

        f.head = rs64_atst_head_live(&hx, &hy) != 0;
        f.head_x = (int8_t)std::lround(std::clamp(hx, -1.0f, 1.0f) * 127.0f);
        f.head_y = (int8_t)std::lround(std::clamp(hy, -1.0f, 1.0f) * 127.0f);
        s.head = f.head;
        s.head_x = f.head_x;
        s.head_y = f.head_y;
        f.dt = s.hz > 30 ? s.dt : 0.0f;

        if (s.rec) {
            fprintf(s.rec, "%s\n", format_frame(f).c_str());
            fflush(s.rec);
        }
    } else if (s.mode == Mode::Replay) {
        if (s.frame >= s.frames.size()) {
            end_session(rdram, "replay finished");

            return;
        }

        const FrameInput& f = s.frames[s.frame];

        if (f.frame != s.frame) {
            end_session(rdram, "recording frame numbers out of sequence");

            return;
        }

        for (int i = 0; i < 6; ++i) {
            wb(rdram, 0x80130B88u + i, f.pad[i]);
        }

        s.throttle_q = f.throttle;
        s.head = f.head;
        s.head_x = f.head_x;
        s.head_y = f.head_y;

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

    static const long s_heap_at = recomp::dbg::env_int("ROGUESQ_LS_HEAP_DUMP_AT", -1);

    if (s_heap_at >= 0 && (long)s.frame == s_heap_at)
    {
        dump_game_heap(rdram, s.missions, rb(rdram, 0x80130B40u), s_heap_at);
    }

    s.frame++;

    // A recording that ends with the mission (success, fail) has no mission frame after its last one, so stop here.
    if (s.mode == Mode::Replay && !s.resume && s.frame >= s.frames.size()) {
        end_session(rdram, "replay finished");
    }
}

// A recording or replay is running: its mission must play the same whatever the display settings. Off again once it ends (ROGUESQ_LS_KEEP_RUNNING plays on live).
extern "C" int rs64_ls_session(void) {
    return mode() != Mode::Off && st().active;
}

// The running session's logic rate, 0 when none runs.
extern "C" int rs64_ls_hz(void) {
    return rs64_ls_session() ? (int)st().hz : 0;
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

// The AT-ST head aim this frame: the recorded one while a session records or replays, so mouse or MCP aiming replays exactly.
extern "C" int rs64_ls_head(int live, float* x, float* y)
{

    State& s = st();

    if (!s.active || (s.mode != Mode::Record && s.mode != Mode::Replay))
    {
        return live;
    }

    *x = s.head_x / 127.0f;
    *y = s.head_y / 127.0f;

    return s.head ? 1 : 0;
}

extern "C" float rs64_ls_cruise(float live) {

    State& s = st();

    if (s.active && (s.mode == Mode::Record || s.mode == Mode::Replay))
    {
        return s.cruise;
    }

    return live;
}

namespace {
// An audio-thread answer the sim branches on: recorded per query while recording, served back in order while replaying.
uint32_t audio_answer(uint8_t* rdram, uint32_t live, AudioAnswers& a, char kind, int& warned)
{

    State& s = st();

    if (!s.active || s.frame == 0 || demo_active(rdram))
    {
        return live;
    }

    const uint32_t frame = s.frame - 1;

    if (s.mode == Mode::Record || s.rerecord) {
        if (s.rec) {
            fprintf(s.rec, "%s\n", format_song_event(frame, live != 0, kind).c_str());
            fflush(s.rec);
        }

        return live;
    }

    // A recording made before this answer was recorded has none of it: play it live, as those recordings always did.
    if (s.mode != Mode::Replay || (kind == 'P' && a.by_frame.empty()))
    {
        return live;
    }

    if (a.frame != frame) {
        a.frame = frame;
        a.idx = 0;
    }

    const auto it = a.by_frame.find(frame);

    if (it == a.by_frame.end() || a.idx >= it->second.size()) {
        if (warned++ < 5)
        {
            fprintf(stderr, "[ls] %s query on frame %u not in the recording; using the live answer\n", kind == 'P' ? "speech" : "song", frame);
        }

        return live;
    }

    return it->second[a.idx++];
}
}

// tickSongFadeTimer's isSongHandleActive answer comes from the audio thread, so it is recorded per frame and replayed like input.
extern "C" uint32_t rs64_ls_song_active(uint8_t* rdram, uint32_t live) {

    static int s_warned = 0;

    return audio_answer(rdram, live, st().song, 'S', s_warned);
}

// The mission-end responders wait for speech to go idle (isSpeechBusyOrQueued, audio thread), so that answer is a sim input too.
extern "C" uint32_t rs64_ls_speech_idle(uint8_t* rdram, uint32_t live) {

    static int s_warned = 0;

    return audio_answer(rdram, live, st().speech, 'P', s_warned);
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
