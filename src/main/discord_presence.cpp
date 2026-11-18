#include "main/discord_presence.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include "game/game_state.h"
#include "game/nav_sequencer.h"
#include "game/upstream_compat.h"
#include "host_api/host_api.h"
#include "librecomp/mods.hpp"
#include "rs64_toml_hooks.h"
#include "util/debug_logs.h"
#include "util/rdram_words.h"

#if defined(__ANDROID__)
#include <dlfcn.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace rs64::discord {

namespace {

using json = nlohmann::json;

static constexpr uint64_t kAppId = 1569823955684954132ull;
static constexpr const char* kLargeImage = "icon_art";
static constexpr const char* kAppName = "Rogue Squadron 64 Recompiled";

// Opcodes of Discord's IPC frames: an 8-byte little-endian header (opcode, length), then JSON.
enum Op : uint32_t { kHandshake = 0, kFrame = 1 };

// Discord tries discord-ipc-0 through discord-ipc-9.
static constexpr int kPipeSlots = 10;

static constexpr const char* kLevelNames[] = {
    "Ambush at Mos Eisley", "Rendezvous on Barkhesh", "The Search for the Nonnah", "Defection at Corellia",
    "Liberation of Gerrard V", "The Jade Moon", "Imperial Construction Yards", "Assault on Kile II",
    "Rescue on Kessel", "Prisons of Kessel", "Battle Above Taloraan", "Escape from Fest",
    "Blockade on Chandrila", "Raid on Sullust", "Moff Seerdon's Revenge", "The Battle of Calamari",
    "Beggar's Canyon", "The Death Star Trench Run", "The Battle of Hoth"
};

// enum PlayerCraft order (the craft byte gGameSettings+1).
static constexpr const char* kCraftNames[] = {
    "X-wing", "Y-wing", "A-wing", "V-wing", "Snowspeeder", "Millennium Falcon", "TIE Interceptor", "T-16 Skyhopper", "Naboo Starfighter"
};

static constexpr int kMaxPlayers = 2;

// The multiplayer mod's mp_lobby_state flag (LobbyState in net_core.h).
static constexpr int kLobbyHosting = 1;
static constexpr int kLobbyConnected = 3;

// g_active_overlay ids.
static constexpr int kOverlayMission = 0;
static constexpr int kOverlayCinematic = 2;

// The hangar counts as up while its frame counter moved within this many presents.
static constexpr int kHangarPresents = 30;

// The AT-ST level the CHICKEN passcode starts.
static constexpr uint32_t kChickenLevel = 20;

struct Activity
{
    bool enabled = false;
    std::string details;
    std::string state;
    int64_t start = 0;
    int players = 0;

    bool operator==(const Activity&) const = default;
};

// Set when the app returns to the foreground: the next poll sends the status even though it did not change.
std::atomic<bool> g_resend{ false };

#if defined(__ANDROID__)

// The icon_art asset by URL: the Android SDK sends the activity without the app looked up, so an asset key shows nothing there.
static constexpr const char* kLargeImageUrl = "https://cdn.discordapp.com/app-assets/1569823955684954132/1569825505103061042.png";

using SetFn = void (*)(const Rs64DiscordStatus*);
using PumpFn = void (*)();

std::atomic<SetFn> g_set{ nullptr };
std::atomic<PumpFn> g_pump{ nullptr };

// True while the app is in the background: the status is cleared and nothing is sent.
std::atomic<bool> g_background{ false };

void pump()
{

    if (const PumpFn p = g_pump.load()) {
        p();
    }
}

// The bridge is opened the first time the mod is on; an APK built without the SDK has none and stays silent.
void publish(const Activity& a)
{

    static bool s_tried = false;

    if (a.enabled && !s_tried) {
        s_tried = true;

        if (void* lib = dlopen("libdiscord_bridge.so", RTLD_NOW)) {
            g_pump = reinterpret_cast<PumpFn>(dlsym(lib, "rs64_discord_pump"));
            g_set = reinterpret_cast<SetFn>(dlsym(lib, "rs64_discord_set"));
        }
    }

    const SetFn set = g_set.load();

    if (!set || g_background)
    {
        return;
    }

    if (!a.enabled) {
        set(nullptr);

        return;
    }

    const Rs64DiscordStatus s = { kAppId, kAppName, a.details.c_str(), a.state.c_str(), kLargeImageUrl, kAppName, a.start, a.players, kMaxPlayers };

    set(&s);
}

#else

std::mutex g_mtx;
std::condition_variable g_cv;
Activity g_want;
bool g_dirty = false;

class Pipe
{
public:
    bool open()
    {

        for (int i = 0; i < kPipeSlots; ++i) {
            if (try_open(i)) {
                return true;
            }
        }

        return false;
    }

    void close()
    {

#if defined(_WIN32)

        if (h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
        }

        h_ = INVALID_HANDLE_VALUE;

#else

        if (fd_ >= 0) {
            ::close(fd_);
        }

        fd_ = -1;

#endif

    }

    bool is_open() const
    {

#if defined(_WIN32)
        return h_ != INVALID_HANDLE_VALUE;
#else
        return fd_ >= 0;
#endif

    }

    // One frame out, Discord's reply frame back; false when the pipe broke or the reply is not a frame.
    bool request(uint32_t op, const json& body, std::string& reply)
    {

        const std::string text = body.dump();
        const uint32_t len = (uint32_t)text.size();
        std::string buf(8 + text.size(), '\0');

        memcpy(&buf[0], &op, 4);
        memcpy(&buf[4], &len, 4);
        memcpy(&buf[8], text.data(), text.size());

        uint32_t hdr[2];

        if (!io(&buf[0], buf.size(), true) || !io(hdr, sizeof(hdr), false) || hdr[1] > 0x10000)
        {
            return false;
        }

        reply.assign(hdr[1], '\0');

        if (hdr[1] > 0 && !io(&reply[0], hdr[1], false))
        {
            return false;
        }

        return hdr[0] == kFrame;
    }

private:
    bool try_open(int slot)
    {

#if defined(_WIN32)

        const std::string path = "\\\\.\\pipe\\discord-ipc-" + std::to_string(slot);

        h_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);

        return h_ != INVALID_HANDLE_VALUE;

#else

        // shortcut: Flatpak and Snap Discord put the socket in a subfolder (app/com.discordapp.Discord, snap.discord); add those if players ask.
        static const std::string s_base = [] {

            for (const char* var : { "XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP" }) {
                if (const char* dir = recomp::dbg::env_str(var)) {
                    return std::string(dir) + "/discord-ipc-";
                }
            }

            return std::string("/tmp/discord-ipc-");
        }();

        fd_ = socket(AF_UNIX, SOCK_STREAM, 0);

        if (fd_ < 0)
        {
            return false;
        }

#if defined(SO_NOSIGPIPE)
        int one = 1;
        setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s%d", s_base.c_str(), slot);

        if (connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            return true;
        }

        close();

        return false;

#endif

    }

    bool io(void* data, size_t size, bool write)
    {

        char* p = static_cast<char*>(data);

        while (size > 0) {

#if defined(_WIN32)

            DWORD n = 0;
            const BOOL ok = write ? WriteFile(h_, p, (DWORD)size, &n, nullptr) : ReadFile(h_, p, (DWORD)size, &n, nullptr);

            if (!ok || n == 0)
            {
                return false;
            }

#else

#if defined(MSG_NOSIGNAL)
            const int flags = MSG_NOSIGNAL;
#else
            const int flags = 0;
#endif

            const ssize_t n = write ? send(fd_, p, size, flags) : recv(fd_, p, size, 0);

            if (n <= 0)
            {
                return false;
            }

#endif

            p += n;
            size -= (size_t)n;
        }

        return true;
    }

#if defined(_WIN32)
    HANDLE h_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif

};

int process_id()
{

#if defined(_WIN32)
    return (int)GetCurrentProcessId();
#else
    return (int)getpid();
#endif

}

// Handshake, then Discord's READY. A refusal (a bad client id, an old client) is logged once.
bool connect_discord(Pipe& pipe)
{

    if (!pipe.open())
    {
        return false;
    }

    std::string reply;

    if (pipe.request(kHandshake, { { "v", 1 }, { "client_id", std::to_string(kAppId) } }, reply)) {
        fprintf(stderr, "[discord] connected\n");

        return true;
    }

    static bool s_logged = false;

    if (!s_logged) {
        s_logged = true;
        fprintf(stderr, "[discord] handshake refused: %s\n", reply.c_str());
    }

    pipe.close();

    return false;
}

bool send_activity(Pipe& pipe, const Activity& a)
{

    static int s_nonce = 0;
    json args = { { "pid", process_id() } };

    if (a.enabled) {
        json& act = args["activity"];

        act = {
            { "details", a.details },
            { "timestamps", { { "start", a.start } } },
            { "assets", { { "large_image", kLargeImage }, { "large_text", kAppName } } }
        };

        if (!a.state.empty()) {
            act["state"] = a.state;
        }

        // Discord shows the party as "(1 of 2)" after the state line.
        if (a.players > 0) {
            act["party"] = { { "id", "rs64-coop-" + std::to_string(process_id()) }, { "size", { a.players, kMaxPlayers } } };
        }
    }

    std::string reply;

    if (!pipe.request(kFrame, { { "cmd", "SET_ACTIVITY" }, { "args", args }, { "nonce", std::to_string(++s_nonce) } }, reply))
    {
        return false;
    }

    if (reply.find("\"ERROR\"") != std::string::npos) {
        fprintf(stderr, "[discord] SET_ACTIVITY error: %s\n", reply.c_str());
    }

    return true;
}

// The IPC thread: sleeps until a change, sends it, and with Discord closed retries every 15 s. Disabling the discord-status mod clears the status and disconnects.
void run()
{

    Pipe pipe;
    std::optional<Activity> sent;

    for (;;) {
        std::unique_lock<std::mutex> lk(g_mtx);

        g_cv.wait_for(lk, std::chrono::seconds(15), [] { return g_dirty; });
        g_dirty = false;

        const Activity want = g_want;

        lk.unlock();

        if (!pipe.is_open()) {
            sent.reset();

            if (!want.enabled || !connect_discord(pipe)) {
                continue;
            }
        }

        if (sent == want) {
            continue;
        }

        if (!send_activity(pipe, want)) {
            pipe.close();

            std::lock_guard<std::mutex> relk(g_mtx);
            g_dirty = true;

            continue;
        }

        sent = want;

        // Off: the status is cleared above, then the connection goes too.
        if (!want.enabled) {
            pipe.close();
        }
    }
}

void pump()
{
}

// The thread starts the first time the mod is on, so a game with it off never talks to Discord.
void publish(const Activity& a)
{

    static std::once_flag s_start;

    if (a.enabled) {
        std::call_once(s_start, [] { std::thread(run).detach(); });
    }

    std::lock_guard<std::mutex> lk(g_mtx);

    g_want = a;
    g_dirty = true;
    g_cv.notify_one();
}

#endif

// 0 outside co-op; 1 while hosting with the slot open; 2 once the other player is in.
int co_op_players()
{

    const int lobby = rs64::host::flag("mp_lobby_state");

    if (lobby == kLobbyConnected || rs64::host::flag("ghost_mode") != 0)
    {
        return kMaxPlayers;
    }

    return lobby == kLobbyHosting ? 1 : 0;
}

// A mod level plays as a stock level id, so its own title wins.
const char* level_name(uint32_t level)
{

    if (const char* title = rs64_mod_levels_current_title())
    {
        return title;
    }

    if (level == kChickenLevel)
    {
        return "CHICKEN";
    }

    return level < std::size(kLevelNames) ? kLevelNames[level] : "Custom level";
}

// The loaded overlay decides mission / cutscene / front end: the menu classifier also matches stale menu data inside missions and cutscenes.
// Every screen of a level (crawl, cutscenes, mission) has the level as its top line, so they share one timer.
Activity describe(const uint8_t* rdram, bool hangar)
{

    const int overlay = g_active_overlay;
    const uint32_t level = rs64_state_level();
    const uint8_t craft = rs64::mips::rb(rdram, 0x80130B41u);
    const bool demo = (rs64::mips::rw(rdram, 0x80130B50u) & 0x20u) != 0;
    const std::string_view st = rs64_state_current_id();

    const auto screen = [](std::string details, std::string state = {}) {

        Activity a;

        a.details = std::move(details);
        a.state = std::move(state);

        return a;
    };

    if (rs64_crawl_playing())
    {
        return screen(level_name(level), "Opening crawl");
    }

    if (hangar)
    {
        return screen("In the hangar", level_name(level));
    }

    if (overlay == kOverlayMission && demo)
    {
        return screen("Watching a demo");
    }

    if (overlay == kOverlayMission && level == kChickenLevel)
    {
        return screen(level_name(level), "AT-ST");
    }

    if (overlay == kOverlayMission)
    {
        return screen(level_name(level), craft < std::size(kCraftNames) ? kCraftNames[craft] : "");
    }

    if (overlay == kOverlayCinematic)
    {
        return level < std::size(kLevelNames) ? screen(level_name(level), "Cutscene") : screen("Watching a cutscene");
    }

    if (st == "menu.account.level_select")
    {
        return screen("Selecting a level");
    }

    if (st == "menu.account.craft_select" || st == "menu.account.hangar")
    {
        return screen("Choosing a craft", level_name(level));
    }

    return screen("In the menus");
}

}

#if defined(__ANDROID__)

// The Discord app keeps a status until told otherwise, and a swiped-away game gets no shutdown, so leaving the foreground clears it.
void set_foreground(bool on)
{

    g_background = !on;

    if (on) {
        g_resend = true;

        return;
    }

    if (const SetFn set = g_set.load()) {
        set(nullptr);
        pump();
    }
}

#endif

void poll(const uint8_t* rdram)
{

    pump();

    static Activity s_last;
    static uint32_t s_hangar_count = 0;
    static int s_hangar_age = kHangarPresents;
    static const std::string kModId = "discord-status";

    const uint32_t hangar_count = rs64_nav_hangar_frame_count();

    s_hangar_age = (hangar_count != s_hangar_count) ? 0 : std::min(s_hangar_age + 1, kHangarPresents);
    s_hangar_count = hangar_count;

    Activity a = describe(rdram, s_hangar_age < kHangarPresents);

    a.enabled = recomp::mods::is_mod_enabled(kModId);
    a.players = co_op_players();

    if (a.players > 0 && a.state.empty()) {
        a.state = "Co-op";
    }

    a.start = (a.details == s_last.details) ? s_last.start : (int64_t)time(nullptr);

    if (a == s_last && !g_resend.exchange(false))
    {
        return;
    }

    s_last = a;

    static const bool s_log = recomp::dbg::env_on("ROGUESQ_LOG_DISCORD");

    if (s_log) {
        fprintf(stderr, "[discord] overlay=%d state=%s players=%d -> \"%s\" / \"%s\" start=%lld\n", (int)g_active_overlay, rs64_state_current_id(), a.players, a.details.c_str(), a.state.c_str(), (long long)a.start);
    }

    publish(a);
}

}
