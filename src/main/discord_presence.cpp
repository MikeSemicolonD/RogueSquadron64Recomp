#include "main/discord_presence.h"

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
#include "host_api/host_api.h"
#include "librecomp/mods.hpp"
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

Activity describe(const std::string& st, uint32_t level, uint8_t craft)
{

    Activity a;

    if (st.rfind("mission", 0) == 0) {
        a.details = level < std::size(kLevelNames) ? kLevelNames[level] : "Custom level";

        if (st == "mission.atst") {
            a.state = "AT-ST";

            if (level == kChickenLevel) {
                a.details = "CHICKEN";
            }
        } else if (craft < std::size(kCraftNames)) {
            a.state = kCraftNames[craft];
        }
    } else if (st == "cinematic") {
        a.details = "Watching a cutscene";
    } else if (st == "attract.demo") {
        a.details = "Watching a demo";
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
    static int s_state = -2;
    static uint32_t s_level = ~0u;
    static uint8_t s_craft = 0xFF;
    static bool s_enabled = false;
    static int s_players = 0;
    static std::string s_details;
    static int64_t s_start_time = 0;
    static const std::string kModId = "discord-status";

    const int state = rs64_state_current();
    const uint32_t level = rs64_state_level();
    const uint8_t craft = rdram[0x130B41 ^ 3];
    const bool enabled = recomp::mods::is_mod_enabled(kModId);
    const int players = co_op_players();

    if (state == s_state && level == s_level && craft == s_craft && enabled == s_enabled && players == s_players)
    {
        return;
    }

    s_state = state;
    s_level = level;
    s_craft = craft;
    s_enabled = enabled;
    s_players = players;

    Activity a = describe(rs64_state_current_id(), level, craft);

    a.players = players;

    if (players > 0 && a.state.empty()) {
        a.state = "Co-op";
    }

    // The timer restarts on a new mission or screen type, not on each menu page.
    if (a.details != s_details) {
        s_details = a.details;
        s_start_time = (int64_t)time(nullptr);
    }

    a.start = s_start_time;
    a.enabled = enabled;

    std::call_once(s_start, [] { std::thread(run).detach(); });

    std::lock_guard<std::mutex> lk(g_mtx);

    g_want = a;
    g_dirty = true;
    g_cv.notify_one();
}

}
