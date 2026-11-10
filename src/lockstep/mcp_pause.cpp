#include "lockstep/mcp_pause.h"
#include "lockstep/lockstep_core.h"
#include "game/game_state.h"
#include "host_api/host_api.h"
#include "main/mcp_bridge.h"
#include "hle/rt64_rs64_mcp.h"
#include "util/debug_logs.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

using namespace rs64::ls;

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

void mcp_note_clock(const std::string& clock, uint32_t frame)
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

void mcp_pause_point(const std::string& clock)
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

    // A parent clock (mission) reads the child state the picture is in (mission.atst).
    const std::string picture_clock = s_mcp_picture["clock"].get<std::string>();
    const std::string key = (!clocks.contains(clock) && pause_clock_matches(clock, picture_clock)) ? picture_clock : clock;

    if (!clocks.contains(key))
    {
        throw std::runtime_error("clock " + clock + " is not running; frozen clocks: " + clocks.dump());
    }

    return { clock, clocks[key].get<uint32_t>() + frames };
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
