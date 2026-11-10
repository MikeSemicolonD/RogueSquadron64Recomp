#include "main/discord_presence.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include "game/game_state.h"
#include "game/nav_sequencer.h"
#include "game/upstream_compat.h"
#include "host_api/host_api.h"
#include "librecomp/mods.hpp"
#include "rs64_toml_hooks.h"
#include "util/debug_logs.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace rs64::discord {

namespace {

using json = nlohmann::json;

static constexpr const char* kClientId = "1569823955684954132";

// Opcodes of Discord's IPC frames: an 8-byte little-endian header (opcode, length), then JSON.
enum Op : uint32_t { kHandshake = 0, kFrame = 1 };

static constexpr const char* kLevelNames[] = {
    "Ambush at Mos Eisley", "Rendezvous on Barkhesh", "The Search for the Nonnah", "Defection at Corellia",
    "Liberation of Gerrard V", "The Jade Moon", "Imperial Construction Yards", "Assault on Kile II",
    "Rescue on Kessel", "Prisons of Kessel", "Battle Above Taloraan", "Escape from Fest",
    "Blockade on Chandrila", "Raid on Sullust", "Moff Seerdon's Revenge", "The Battle of Calamari",
    "Beggar's Canyon", "The Death Star Trench Run", "The Battle of Hoth"
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

// enum PlayerCraft order (the craft byte gGameSettings+1).
static constexpr const char* kCraftNames[] = {
    "X-wing", "Y-wing", "A-wing", "V-wing", "Snowspeeder", "Millennium Falcon", "TIE Interceptor", "T-16 Skyhopper", "Naboo Starfighter"
};

struct Activity
{
    bool enabled = false;
    std::string details;
    std::string state;
    int64_t start = 0;
    int players = 0;

    bool operator==(const Activity& o) const
    {
        return enabled == o.enabled && details == o.details && state == o.state && start == o.start && players == o.players;
    }
};

std::mutex g_mtx;
std::condition_variable g_cv;
Activity g_want;
bool g_dirty = false;

class Pipe
{
public:
    bool open()
    {

#if defined(_WIN32)

        for (int i = 0; i < 10; ++i) {
            const std::string path = "\\\\.\\pipe\\discord-ipc-" + std::to_string(i);

            h_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);

            if (h_ != INVALID_HANDLE_VALUE) {
                return true;
            }
        }

        return false;

#else

        // shortcut: Flatpak and Snap Discord put the socket in a subfolder (app/com.discordapp.Discord, snap.discord); add those if players ask.
        const char* dir = nullptr;

        for (const char* var : { "XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP" }) {
            dir = recomp::dbg::env_str(var);

            if (dir) {
                break;
            }
        }

        const std::string base = std::string(dir ? dir : "/tmp") + "/discord-ipc-";

        for (int i = 0; i < 10; ++i) {
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
            snprintf(addr.sun_path, sizeof(addr.sun_path), "%s%d", base.c_str(), i);

            if (connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                return true;
            }

            ::close(fd_);
            fd_ = -1;
        }

        return false;

#endif

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

    bool send_frame(uint32_t op, const std::string& body)
    {

        std::string buf(8 + body.size(), '\0');
        const uint32_t len = (uint32_t)body.size();

        memcpy(&buf[0], &op, 4);
        memcpy(&buf[4], &len, 4);
        memcpy(&buf[8], body.data(), body.size());

        return io(&buf[0], buf.size(), true);
    }

    bool read_frame(uint32_t& op, std::string& body)
    {

        uint32_t hdr[2];

        if (!io(hdr, sizeof(hdr), false) || hdr[1] > 0x10000)
        {
            return false;
        }

        op = hdr[0];
        body.assign(hdr[1], '\0');

        return hdr[1] == 0 || io(&body[0], hdr[1], false);
    }

private:
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

// Handshake, then Discord's READY. A CLOSE reply (a bad client id, an old client) is logged once.
bool connect_discord(Pipe& pipe)
{

    if (!pipe.open())
    {
        return false;
    }

    uint32_t op = 0;
    std::string reply;
    const json hello = { { "v", 1 }, { "client_id", kClientId } };

    if (pipe.send_frame(kHandshake, hello.dump()) && pipe.read_frame(op, reply) && op == kFrame) {
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
        args["activity"] = {
            { "details", a.details },
            { "timestamps", { { "start", a.start } } },
            { "assets", { { "large_image", "icon_art" }, { "large_text", "Rogue Squadron 64 Recompiled" } } }
        };

        if (!a.state.empty()) {
            args["activity"]["state"] = a.state;
        }

        // Discord shows the party as "(1 of 2)" after the state line.
        if (a.players > 0) {
            args["activity"]["party"] = { { "id", "rs64-coop-" + std::to_string(process_id()) }, { "size", { a.players, kMaxPlayers } } };
        }
    }

    const json msg = { { "cmd", "SET_ACTIVITY" }, { "args", args }, { "nonce", std::to_string(++s_nonce) } };
    uint32_t op = 0;
    std::string reply;

    if (!pipe.send_frame(kFrame, msg.dump()) || !pipe.read_frame(op, reply) || op != kFrame)
    {
        return false;
    }

    if (reply.find("\"ERROR\"") != std::string::npos) {
        fprintf(stderr, "[discord] SET_ACTIVITY error: %s\n", reply.c_str());
    }

    return true;
}

// Sends each change; with Discord closed, retries every 15 s. Disabling the discord-status mod clears the status but keeps the connection.
void run()
{

    Pipe pipe;
    Activity sent;
    bool have_sent = false;

    for (;;) {
        std::unique_lock<std::mutex> lk(g_mtx);

        g_cv.wait_for(lk, std::chrono::seconds(15), [] { return g_dirty; });
        g_dirty = false;

        const Activity want = g_want;

        lk.unlock();

        if (!pipe.is_open()) {
            have_sent = false;

            if (!want.enabled || !connect_discord(pipe)) {
                continue;
            }
        }

        if (have_sent && want == sent) {
            continue;
        }

        if (!send_activity(pipe, want)) {
            pipe.close();

            std::lock_guard<std::mutex> relk(g_mtx);
            g_dirty = true;

            continue;
        }

        sent = want;
        have_sent = true;
    }
}

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
Activity describe(const uint8_t* rdram, bool hangar)
{

    Activity a;
    const int overlay = g_active_overlay;
    const uint32_t level = rs64_state_level();
    const uint8_t craft = rdram[0x130B41 ^ 3];
    const bool demo = (*reinterpret_cast<const uint32_t*>(rdram + 0x130B50) & 0x20u) != 0;
    const std::string st = rs64_state_current_id();

    if (hangar) {
        a.details = "In the hangar";
        a.state = level_name(level);
    } else if (overlay == kOverlayMission && demo) {
        a.details = "Watching a demo";
    } else if (overlay == kOverlayMission) {
        a.details = level_name(level);

        if (level == kChickenLevel) {
            a.state = "AT-ST";
        } else if (craft < std::size(kCraftNames)) {
            a.state = kCraftNames[craft];
        }
    } else if (overlay == kOverlayCinematic && level < std::size(kLevelNames)) {
        a.details = level_name(level);
        a.state = "Cutscene";
    } else if (overlay == kOverlayCinematic) {
        a.details = "Watching a cutscene";
    } else if (st == "menu.account.level_select") {
        a.details = "Selecting a level";
    } else if (st == "menu.account.craft_select") {
        a.details = "Choosing a craft";
        a.state = level_name(level);
    } else {
        a.details = "In the menus";
    }

    return a;
}

}

void poll(const uint8_t* rdram)
{

#if defined(__ANDROID__)
    return;
#endif

    static std::once_flag s_start;
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

    // The timer runs while the top line stays the same: a level's cutscenes, pauses and co-op changes keep it.
    a.start = (a.details == s_last.details) ? s_last.start : (int64_t)time(nullptr);

    if (a == s_last)
    {
        return;
    }

    s_last = a;

    static const bool s_log = recomp::dbg::env_on("ROGUESQ_LOG_DISCORD");

    if (s_log) {
        fprintf(stderr, "[discord] overlay=%d state=%s players=%d -> \"%s\" / \"%s\" start=%lld\n", (int)g_active_overlay, rs64_state_current_id(), a.players, a.details.c_str(), a.state.c_str(), (long long)a.start);
    }

    std::call_once(s_start, [] { std::thread(run).detach(); });

    std::lock_guard<std::mutex> lk(g_mtx);

    g_want = a;
    g_dirty = true;
    g_cv.notify_one();
}

}
