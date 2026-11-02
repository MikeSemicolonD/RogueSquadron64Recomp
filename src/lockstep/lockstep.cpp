#include "lockstep/lockstep_core.h"
#include "game/game_state.h"
#include "util/debug_logs.h"
#include "host_api/host_api.h"
#include "main/main.h"
#include "main/renderdoc_capture.h"
#include "recomp.h"
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
#include <vector>

#ifdef RS64_MCP
#include "main/mcp_bridge.h"
#include "hle/rt64_rs64_mcp.h"
#endif

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
        const bool delay = recomp::dbg::env_int("ROGUESQ_LS_INPUT_DELAY", 0) > 0;
        const Mode won = replay ? Mode::Replay : record ? Mode::Record : delay ? Mode::Delay : Mode::Off;

        if (int(replay) + int(record) + int(delay) > 1)
        {
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

    if (s.missions > 1 && s.mode != Mode::Delay) {
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

        if (running.level != h.level || running.craft != h.craft)
        {
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

    if (!s.active || demo_active(rdram))
    {
        return dt;
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

// Holds the calling game thread until released() or the deadline; true when released.
static bool hold_until(const std::function<bool()>& released, std::chrono::steady_clock::time_point until)
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

#ifdef RS64_MCP

// MCP pause state: the latest frame of each clock (pause points, game thread) and the gfx-task sequence (send_dl, gfx thread).
static std::mutex s_mcp_lock;
static std::map<std::string, uint32_t> s_mcp_clocks;
static uint64_t s_mcp_dl_seq = 0;

static bool mcp_log()
{

    static const bool on = recomp::dbg::env_on("ROGUESQ_LOG_MCP");

    return on;
}

// The clocks when the last display list was sent: the display list after a frame's pause point is that frame's.
static std::map<std::string, uint32_t> s_mcp_dl_clocks;

// One MCP pause target: hold at the first pause point after a display list from `clock` frame >= `frame` (clock "dl" = the gfx-task sequence).
static std::optional<PauseTarget> s_mcp_target;
static bool s_mcp_held = false;
static bool s_mcp_release = false;
static uint64_t s_mcp_hold_gen = 0;
// Bumped when the MCP client disconnects, so a request left waiting by that client can't pause RT64 afterwards.
static uint64_t s_mcp_epoch = 0;
static nlohmann::json s_mcp_picture;

// The MCP press tool's input on controller 1, held until dl_seq reaches until_dl (0 = until release).
struct McpInput { uint16_t mask; float x; float y; uint64_t until_dl; nlohmann::json names; };
static std::optional<McpInput> s_mcp_input;

extern "C" bool rs64_input_name(const char* name, uint16_t* mask, float* sx, float* sy);

// Caller holds s_mcp_lock; drops an input whose frames have run out.
static bool mcp_input_live()
{

    if (s_mcp_input && s_mcp_input->until_dl != 0 && s_mcp_dl_seq >= s_mcp_input->until_dl) {
        s_mcp_input.reset();
    }

    return s_mcp_input.has_value();
}

extern "C" bool rs64_mcp_consume_input(uint16_t* buttons, float* x, float* y)
{

    std::lock_guard<std::mutex> g(s_mcp_lock);

    if (!mcp_input_live())
    {
        return false;
    }

    *buttons = s_mcp_input->mask;
    *x = s_mcp_input->x;
    *y = s_mcp_input->y;

    return true;
}

extern "C" void rs64_mcp_note_dl()
{

    std::lock_guard<std::mutex> g(s_mcp_lock);

    ++s_mcp_dl_seq;
    s_mcp_dl_clocks = s_mcp_clocks;

    if (mcp_log()) fprintf(stderr, "[mcp] dl %llu\n", (unsigned long long)s_mcp_dl_seq);
}

static void mcp_note_clock(const std::string& clock, uint32_t frame)
{

    std::lock_guard<std::mutex> g(s_mcp_lock);

    s_mcp_clocks[clock] = frame;

    if (mcp_log()) fprintf(stderr, "[mcp] point %s %u dl %llu\n", clock.c_str(), frame, (unsigned long long)s_mcp_dl_seq);
}

// Unpauses RT64 and waits for it to apply, so display lists sent after a release are drawn, not dropped.
static void mcp_unpause_rt64()
{

    const uint64_t d0 = RT64::rs64mcp::drainCount();
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);

    RT64::rs64mcp::requestPause(false);
    hold_until([d0]() { return RT64::rs64mcp::drainCount() > d0 && !RT64::rs64mcp::paused(); }, until);
}

// An RDRAM condition checked once per pause point (once a frame): changes (from the value when set), eq, ne, gt, lt; or op "drawn":
// RT64 saw a mesh / sprite key from `key` on screen (rs64mcp draw watch; one or two frames after it first shows).
struct McpWatch { uint32_t addr; uint32_t size; std::string op; uint64_t value; std::string key; };
static std::optional<McpWatch> s_mcp_watch;

static void mcp_clear_watch()
{

    s_mcp_watch.reset();
    RT64::rs64mcp::disarmDrawWatch();
}

extern "C" volatile uint8_t* volatile g_recomp_rdram_for_wp_raw;

// Big-endian read of 1, 2 or 4 bytes at a KSEG0 address (after moved-range relocation); false outside valid RAM.
static bool mcp_read(uint32_t addr, uint32_t size, uint64_t& out)
{

    volatile uint8_t* rdram = g_recomp_rdram_for_wp_raw;
    const uint32_t at = relocated(addr);

    if (!rdram || !rs64::host::ram_ok(at) || !rs64::host::ram_ok(at + size - 1))
    {
        return false;
    }

    out = 0;

    for (uint32_t i = 0; i < size; i++) {
        out = (out << 8) | rdram[(at + i - 0x80000000u) ^ 3];
    }

    return true;
}

static bool mcp_watch_hit(const McpWatch& w, uint64_t& value)
{

    if (!mcp_read(w.addr, w.size, value))
    {
        return false;
    }

    if (w.op == "changes") return value != w.value;
    if (w.op == "eq") return value == w.value;
    if (w.op == "ne") return value != w.value;
    if (w.op == "gt") return value > w.value;

    return value < w.value;
}

static void mcp_pause_point(const std::string& clock)
{

    std::unique_lock<std::mutex> g(s_mcp_lock);

    if (!s_mcp_target && !s_mcp_watch)
    {
        return;
    }

    const auto it = s_mcp_dl_clocks.find(clock);
    const uint32_t clock_frame = (it != s_mcp_dl_clocks.end()) ? it->second : 0;
    nlohmann::json picture = { { "clock", clock }, { "frame", clock_frame }, { "dl_seq", s_mcp_dl_seq }, { "clocks", s_mcp_dl_clocks } };
    bool hit = false;

    if (s_mcp_target)
    {
        const PauseTarget& t = *s_mcp_target;

        hit = (t.clock == "dl") ? (s_mcp_dl_seq >= t.frame) : (pause_clock_matches(t.clock, clock) && it != s_mcp_dl_clocks.end() && it->second >= t.frame);

        if (hit && t.clock == "dl") {
            picture["clock"] = "dl";
            picture["frame"] = (uint32_t)s_mcp_dl_seq;
        }
    }

    if (!hit && s_mcp_watch)
    {
        uint64_t value = 0;
        std::string drawn;

        if (s_mcp_watch->op == "drawn") {
            hit = RT64::rs64mcp::drawWatchHit(drawn);
        }
        else {
            hit = mcp_watch_hit(*s_mcp_watch, value);
        }

        if (hit && !drawn.empty()) {
            picture["watch"] = { { "drawn", drawn } };
        }
        else if (hit) {
            picture["watch"] = { { "addr", s_mcp_watch->addr }, { "value", value }, { "was", s_mcp_watch->value } };
        }
    }

    if (!hit)
    {
        return;
    }

    s_mcp_picture = picture;
    s_mcp_target.reset();
    mcp_clear_watch();
    s_mcp_held = true;
    s_mcp_release = false;
    s_mcp_hold_gen++;
    g.unlock();

    if (mcp_log()) fprintf(stderr, "[mcp] held: %s\n", s_mcp_picture.dump().c_str());

    hold_until([]() {
        std::lock_guard<std::mutex> lk(s_mcp_lock);

        return s_mcp_release || !rs64_mcp_connected();
    }, std::chrono::steady_clock::time_point::max());

    const bool dropped = !rs64_mcp_connected();

    g.lock();
    s_mcp_held = false;
    g.unlock();

    if (dropped) {
        mcp_unpause_rt64();
    }
}

// The client went away: drop its target, release a hold and turn RT64's pause and debug views off.
extern "C" void rs64_mcp_on_disconnect()
{

    std::lock_guard<std::mutex> g(s_mcp_lock);

    s_mcp_target.reset();
    mcp_clear_watch();
    s_mcp_input.reset();
    s_mcp_release = true;
    s_mcp_epoch++;
    RT64::rs64mcp::requestPause(false);
    RT64::rs64mcp::requestReset();
}

static nlohmann::json mcp_frame_info()
{

    std::lock_guard<std::mutex> g(s_mcp_lock);
    nlohmann::json clocks = s_mcp_clocks;

    clocks["dl"] = s_mcp_dl_seq;

    const nlohmann::json input = mcp_input_live() ? nlohmann::json{ { "buttons", s_mcp_input->names }, { "stick", { s_mcp_input->x, s_mcp_input->y } }, { "until_dl", s_mcp_input->until_dl } } : nlohmann::json();

    return { { "state", rs64_state_current_id() }, { "clocks", clocks }, { "held", s_mcp_held }, { "rt64_paused", RT64::rs64mcp::paused() }, { "picture", s_mcp_held ? s_mcp_picture : nlohmann::json() }, { "input", input } };
}

struct McpWait { uint64_t gen; uint64_t epoch; };

// Sets (or clears) the MCP target and, when releasing, lets the held game thread go; returns the hold generation to wait past and the client epoch.
static McpWait mcp_set_target(std::optional<PauseTarget> target, bool release = false)
{

    std::lock_guard<std::mutex> g(s_mcp_lock);

    if (!target) {
        mcp_clear_watch();
    }

    s_mcp_target = std::move(target);
    s_mcp_release = s_mcp_release || release;

    return { s_mcp_hold_gen, s_mcp_epoch };
}

static void mcp_set_watch(const McpWatch& w)
{

    std::lock_guard<std::mutex> g(s_mcp_lock);

    s_mcp_watch = w;
}

// The frozen picture's clock plus `frames`, or an error when nothing is held.
static PauseTarget mcp_step_target(const std::string& clock, uint32_t frames)
{

    std::lock_guard<std::mutex> g(s_mcp_lock);

    if (!s_mcp_held)
    {
        throw std::runtime_error("not paused; use pause_at");
    }

    const auto& clocks = s_mcp_picture["clocks"];

    if (clock == "dl")
    {
        return { clock, (uint32_t)s_mcp_picture["dl_seq"].get<uint64_t>() + frames };
    }

    if (!clocks.contains(clock))
    {
        throw std::runtime_error("clock " + clock + " is not running; frozen clocks: " + clocks.dump());
    }

    return { clock, clocks[clock].get<uint32_t>() + frames };
}

// Pauses RT64 unless the client that asked has gone; under the lock so a disconnect's unpause always lands after it.
static bool mcp_request_rt64_pause(uint64_t epoch)
{

    std::lock_guard<std::mutex> g(s_mcp_lock);

    if (s_mcp_epoch != epoch || !s_mcp_held)
    {
        return false;
    }

    RT64::rs64mcp::requestPause(true);

    return true;
}

// Waits for the game thread to hold on a new target, then pauses RT64 and waits until the frozen frame has been redrawn.
static nlohmann::json mcp_wait_frozen(McpWait w, double timeout_s)
{

    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds((long long)(timeout_s * 1000.0));

    const bool held = hold_until([w]() {
        std::lock_guard<std::mutex> g(s_mcp_lock);

        return (s_mcp_hold_gen > w.gen && s_mcp_held) || s_mcp_epoch != w.epoch;
    }, until);

    if (!held)
    {
        mcp_set_target(std::nullopt);

        throw std::runtime_error("timeout; game still running: " + mcp_frame_info().dump());
    }

    const uint64_t d0 = RT64::rs64mcp::drainCount();

    if (!mcp_request_rt64_pause(w.epoch))
    {
        throw std::runtime_error("client disconnected");
    }

    if (!hold_until([d0]() { return RT64::rs64mcp::drainCount() >= d0 + 2 && RT64::rs64mcp::paused(); }, std::chrono::steady_clock::now() + std::chrono::seconds(5)))
    {
        throw std::runtime_error("RT64 did not pause");
    }

    std::lock_guard<std::mutex> g(s_mcp_lock);

    return { { "picture", s_mcp_picture } };
}

static nlohmann::json mcp_pause_at(const nlohmann::json& p)
{

    if (!p.contains("clock") || !p["clock"].is_string() || !p.contains("frame") || !p["frame"].is_number_unsigned())
    {
        throw std::runtime_error("pause_at needs clock (string) and frame (unsigned)");
    }

    if (mcp_frame_info()["held"].get<bool>())
    {
        throw std::runtime_error("already paused; use step or resume");
    }

    const McpWait w = mcp_set_target(PauseTarget{ p["clock"].get<std::string>(), p["frame"].get<uint32_t>() });

    return mcp_wait_frozen(w, p.value("timeout_s", 120.0));
}

// Pause once an RDRAM value meets a condition, checked every frame (a game state change is pause_at with the state's clock at frame 0).
static nlohmann::json mcp_pause_when(const nlohmann::json& p)
{

    static const char* const kOps[] = { "changes", "eq", "ne", "gt", "lt" };
    McpWatch w{ 0, p.value("size", 4u), p.value("op", std::string("changes")), 0 };

    if (!p.contains("addr") || !p["addr"].is_number_unsigned())
    {
        throw std::runtime_error("addr (unsigned) required");
    }

    w.addr = p["addr"].get<uint32_t>();

    if (w.size != 1 && w.size != 2 && w.size != 4)
    {
        throw std::runtime_error("size must be 1, 2 or 4");
    }

    if (std::find(std::begin(kOps), std::end(kOps), w.op) == std::end(kOps))
    {
        throw std::runtime_error("op must be changes, eq, ne, gt or lt");
    }

    if (w.op != "changes" && !p.contains("value"))
    {
        throw std::runtime_error("value required for op " + w.op);
    }

    if (!mcp_read(w.addr, w.size, w.value))
    {
        throw std::runtime_error("address outside valid RAM");
    }

    if (w.op != "changes") {
        w.value = p["value"].get<uint64_t>();
    }

    if (mcp_frame_info()["held"].get<bool>())
    {
        throw std::runtime_error("already paused; resume first");
    }

    const McpWait wait = mcp_set_target(std::nullopt);

    mcp_set_watch(w);

    return mcp_wait_frozen(wait, p.value("timeout_s", 120.0));
}

// Freeze once a model part or sprite whose key starts with one of `key`'s '|'-separated prefixes is drawn on screen.
static nlohmann::json mcp_pause_when_drawn(const nlohmann::json& p)
{

    if (!p.contains("key") || !p["key"].is_string() || p["key"].get<std::string>().empty())
    {
        throw std::runtime_error("key (mesh or sprite key prefix) required");
    }

    if (mcp_frame_info()["held"].get<bool>())
    {
        throw std::runtime_error("already paused; resume first");
    }

    const McpWait wait = mcp_set_target(std::nullopt);

    RT64::rs64mcp::armDrawWatch(p["key"].get<std::string>());
    mcp_set_watch({ 0, 0, "drawn", 0, p["key"].get<std::string>() });

    return mcp_wait_frozen(wait, p.value("timeout_s", 120.0));
}

static nlohmann::json mcp_step(const nlohmann::json& p)
{

    const PauseTarget target = mcp_step_target(p.value("clock", std::string("dl")), p.value("frames", 1u));

    mcp_unpause_rt64();

    const McpWait w = mcp_set_target(target, true);

    return mcp_wait_frozen(w, p.value("timeout_s", 30.0));
}

static nlohmann::json mcp_resume(const nlohmann::json&)
{

    RT64::rs64mcp::requestReset();
    mcp_unpause_rt64();
    mcp_set_target(std::nullopt, true);

    // The held game thread notices the release on its next poll; wait so an immediate pause_at doesn't see it still held.
    hold_until([]() {
        std::lock_guard<std::mutex> g(s_mcp_lock);

        return !s_mcp_held;
    }, std::chrono::steady_clock::now() + std::chrono::seconds(2));

    return { { "resumed", true } };
}

// Holds buttons (and/or the stick) on controller 1 for `frames` display lists, or until release; combine with step to play exact frames.
static nlohmann::json mcp_press(const nlohmann::json& p)
{

    McpInput in{ 0, 0.0f, 0.0f, 0, nlohmann::json::array() };

    for (const auto& b : p.value("buttons", nlohmann::json::array()))
    {
        const std::string name = b.is_string() ? b.get<std::string>() : std::string();

        if (!rs64_input_name(name.c_str(), &in.mask, &in.x, &in.y))
        {
            throw std::runtime_error("unknown button: " + name + " (start a b z l r dup ddown dleft dright cup cdown cleft cright, or up down left right for the stick)");
        }

        in.names.push_back(name);
    }

    if (p.contains("stick"))
    {
        const auto& st = p["stick"];

        if (!st.is_array() || st.size() != 2 || !st[0].is_number() || !st[1].is_number())
        {
            throw std::runtime_error("stick must be [x, y] in -1..1");
        }

        in.x = std::clamp(st[0].get<float>(), -1.0f, 1.0f);
        in.y = std::clamp(st[1].get<float>(), -1.0f, 1.0f);
    }

    const uint32_t frames = p.value("frames", 0u);
    std::lock_guard<std::mutex> g(s_mcp_lock);

    in.until_dl = (frames > 0) ? s_mcp_dl_seq + frames : 0;
    s_mcp_input = in;

    return { { "buttons", in.names }, { "stick", { in.x, in.y } }, { "until_dl", in.until_dl } };
}

static nlohmann::json mcp_release(const nlohmann::json&)
{

    std::lock_guard<std::mutex> g(s_mcp_lock);

    s_mcp_input.reset();

    return { { "released", true } };
}

extern "C" void rs64_mcp_register_pause_methods()
{

    rs64_mcp_register("pause_when", mcp_pause_when);
    rs64_mcp_register("pause_when_drawn", mcp_pause_when_drawn);
    rs64_mcp_register("press", mcp_press);
    rs64_mcp_register("release", mcp_release);

    rs64_mcp_register("pause_at", mcp_pause_at);
    rs64_mcp_register("step", mcp_step);
    rs64_mcp_register("resume", mcp_resume);
    rs64_mcp_register("frame_info", [](const nlohmann::json&) { return mcp_frame_info(); });
}

#endif

extern "C" void rs64_pause_point(const char* clock, uint32_t frame)
{

    static const char* base = recomp::dbg::env_str("ROGUESQ_PAUSE_FILE");

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

    auto rd = [rdram](uint32_t a) { return ((uint32_t)rdram[(a - 0x80000000u) ^ 3] << 24) | ((uint32_t)rdram[(a + 1 - 0x80000000u) ^ 3] << 16) | ((uint32_t)rdram[(a + 2 - 0x80000000u) ^ 3] << 8) | (uint32_t)rdram[(a + 3 - 0x80000000u) ^ 3]; };
    const uint32_t cut = rd(0x800B1904u);

    if (cut < 0x80000000u || cut >= 0x80800000u)
    {
        return;
    }

    rs64_pause_point("cutscene", rd(0x800B0B28u));
}

extern "C" void rs64_ls_frame_pads(uint8_t* rdram, recomp_context*) {

    State& s = st();

    if (!s.active || demo_active(rdram))
    {
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
