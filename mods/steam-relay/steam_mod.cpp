// The steam-relay mod: publishes the "mp.transport" service so multiplayer can host and join over Steam's relay network (app id 480), with a 6-digit join code.
// steam_api is loaded at run time from this mod's folder, never linked: a player supplies it, and without it multiplayer stays on ENet.
#include "rs64/host_api.h"
#include "rs64/mp_transport.h"
#include "recomp.h"
#include "steam/steam_api.h"
#include "steam/steam_api_flat.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define MOD_VISIBLE __declspec(dllexport)
#else
#include <dlfcn.h>
#include <sys/stat.h>
#define MOD_VISIBLE __attribute__((visibility("default")))
#endif
#define MOD_EXPORT extern "C" MOD_VISIBLE

extern "C" {
MOD_VISIBLE uint32_t recomp_api_version = 1;
}

namespace {

#if defined(_WIN32)
constexpr const char* kLibName = "steam_api64.dll";
#elif defined(__APPLE__)
constexpr const char* kLibName = "libsteam_api.dylib";
#else
constexpr const char* kLibName = "libsteam_api.so";
#endif

// The accessor names of the pinned SDK (the one this mod was built against): a library from another SDK lacks one of them.
std::string accessor(const char* prefix, const char* version, const char* suffix) {
    const std::string v(version);
    return std::string(prefix) + suffix + v.substr(v.size() - 3);
}

constexpr int kRetryMs = 3000;
constexpr int kPumpMs = 4;

const rs64_host_api* g_api = nullptr;

void log(const std::string& s) {
    if (g_api) {
        g_api->log(("[steam] " + s).c_str());
    }
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The flat API entry points, resolved once from the loaded library.
struct Api {
    decltype(&SteamAPI_InitFlat) InitFlat = nullptr;
    decltype(&SteamAPI_Shutdown) Shutdown = nullptr;
    decltype(&SteamAPI_GetHSteamPipe) GetHSteamPipe = nullptr;
    decltype(&SteamAPI_ManualDispatch_Init) MDInit = nullptr;
    decltype(&SteamAPI_ManualDispatch_RunFrame) MDRunFrame = nullptr;
    decltype(&SteamAPI_ManualDispatch_GetNextCallback) MDNext = nullptr;
    decltype(&SteamAPI_ManualDispatch_FreeLastCallback) MDFree = nullptr;
    decltype(&SteamAPI_ManualDispatch_GetAPICallResult) MDCallResult = nullptr;
    ISteamNetworkingSockets* (*Sockets)() = nullptr;
    ISteamNetworkingUtils* (*Utils)() = nullptr;
    ISteamMatchmaking* (*Matchmaking)() = nullptr;
    decltype(&SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess) InitRelay = nullptr;
    decltype(&SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P) Listen = nullptr;
    decltype(&SteamAPI_ISteamNetworkingSockets_ConnectP2P) Connect = nullptr;
    decltype(&SteamAPI_ISteamNetworkingSockets_AcceptConnection) Accept = nullptr;
    decltype(&SteamAPI_ISteamNetworkingSockets_CloseConnection) Close = nullptr;
    decltype(&SteamAPI_ISteamNetworkingSockets_CloseListenSocket) CloseListen = nullptr;
    decltype(&SteamAPI_ISteamNetworkingSockets_SendMessageToConnection) Send = nullptr;
    decltype(&SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection) Receive = nullptr;
    decltype(&SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection) Flush = nullptr;
    decltype(&SteamAPI_ISteamNetworkingSockets_GetConnectionRealTimeStatus) RealTime = nullptr;
    decltype(&SteamAPI_SteamNetworkingMessage_t_Release) Release = nullptr;
    decltype(&SteamAPI_ISteamMatchmaking_CreateLobby) CreateLobby = nullptr;
    decltype(&SteamAPI_ISteamMatchmaking_SetLobbyData) SetLobbyData = nullptr;
    decltype(&SteamAPI_ISteamMatchmaking_GetLobbyData) GetLobbyData = nullptr;
    decltype(&SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter) StringFilter = nullptr;
    decltype(&SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter) DistanceFilter = nullptr;
    decltype(&SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter) CountFilter = nullptr;
    decltype(&SteamAPI_ISteamMatchmaking_RequestLobbyList) RequestLobbyList = nullptr;
    decltype(&SteamAPI_ISteamMatchmaking_GetLobbyByIndex) LobbyByIndex = nullptr;
    decltype(&SteamAPI_ISteamMatchmaking_JoinLobby) JoinLobby = nullptr;
    decltype(&SteamAPI_ISteamMatchmaking_LeaveLobby) LeaveLobby = nullptr;
    decltype(&SteamAPI_ISteamMatchmaking_GetLobbyOwner) LobbyOwner = nullptr;
    decltype(&SteamAPI_ISteamMatchmaking_SetLobbyJoinable) SetJoinable = nullptr;
};

enum class Load { NotTried, Missing, Blocked, WrongVersion, Loaded };

class Steam {
public:
    // Loads the library (once) and inits Steam (retried every kRetryMs while it fails).
    bool available();
    bool start(int role, const char* peer, uint32_t build);
    void stop();
    void shutdown();
    // Only connected() pumps: multiplayer reads connected() then connect_count() as one snapshot, so a connect must not land between them.
    bool connected() { pump(); return connected_; }
    bool lost() const { return lost_; }
    uint32_t connect_count() const { return connects_; }
    void send(const uint8_t* b, uint32_t n, bool reliable);
    bool flush(int timeout_ms);
    void receive(rs64_mp_recv_fn fn, void* fn_user);
    const char* status() const { return status_.c_str(); }
    const char* host_label() const { return host_label_.c_str(); }
    const char* pending_join();

private:
    bool load();
    void pump();
    void on_callback(const CallbackMsg_t& m);
    void on_call(SteamAPICall_t call, int id);
    void leave_stale(SteamAPICall_t call, int id);
    void on_connection(const SteamNetConnectionStatusChangedCallback_t& c);
    void set_status(const std::string& s);
    void fail(const std::string& why);
    void search(const std::string& code);
    void join_lobby(uint64_t lobby);
    std::string build_key() const;

    Api f_;
    Load load_ = Load::NotTried;
    bool inited_ = false;
    int64_t retry_at_ = 0;
    int64_t pumped_at_ = 0;
    HSteamPipe pipe_ = 0;
    ISteamNetworkingSockets* sockets_ = nullptr;
    ISteamMatchmaking* mm_ = nullptr;

    int role_ = 0;
    uint32_t build_ = 0;
    std::string code_;
    uint64_t lobby_ = 0;
    SteamAPICall_t call_ = k_uAPICallInvalid;
    int call_id_ = 0;
    HSteamListenSocket listen_ = k_HSteamListenSocket_Invalid;
    HSteamNetConnection conn_ = k_HSteamNetConnection_Invalid;
    bool connected_ = false;
    bool lost_ = false;
    bool failed_ = false;
    uint32_t connects_ = 0;
    std::deque<std::vector<uint8_t>> inbox_;
    std::string status_ = "";
    std::string host_label_ = "";
    std::string pending_;
    std::string pending_out_;
    std::mt19937 rng_{std::random_device{}()};
};

Steam g_steam;

std::string own_dir() {
#ifdef _WIN32
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&own_dir, &self);
    wchar_t w[MAX_PATH] = {};
    GetModuleFileNameW(self, w, MAX_PATH);
    char u[MAX_PATH * 3] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof u, nullptr, nullptr);
    std::string p(u);
#else
    Dl_info info{};
    dladdr((void*)&own_dir, &info);
    std::string p(info.dli_fname ? info.dli_fname : "");
#endif
    const size_t cut = p.find_last_of("/\\");
    return cut == std::string::npos ? std::string() : p.substr(0, cut + 1);
}

bool file_exists(const std::string& p) {
#ifdef _WIN32
    wchar_t w[MAX_PATH * 2] = {};
    MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, w, MAX_PATH * 2);
    return GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(p.c_str(), &st) == 0;
#endif
}

void* open_lib(const std::string& path, std::string* err) {
#ifdef _WIN32
    wchar_t w[MAX_PATH * 2] = {};
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w, MAX_PATH * 2);
    HMODULE h = LoadLibraryExW(w, nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (!h) {
        *err = "LoadLibrary error " + std::to_string(GetLastError());
    }
    return (void*)h;
#else
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* e = dlerror();
        *err = e ? e : "dlopen failed";
    }
    return h;
#endif
}

void* sym(void* lib, const char* name) {
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)lib, name);
#else
    return dlsym(lib, name);
#endif
}

void set_env(const char* k, const char* v) {
#ifdef _WIN32
    SetEnvironmentVariableA(k, v);
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

bool Steam::load() {
    if (load_ != Load::NotTried) {
        return load_ == Load::Loaded;
    }
    const std::string path = own_dir() + kLibName;
    if (!file_exists(path)) {
        load_ = Load::Missing;
        status_ = "STEAM API DLL MISSING";
        log(std::string(kLibName) + " not found at " + path + " (copy it from Steamworks SDK " RS64_STEAM_SDK_VERSION "); multiplayer stays on direct connections");
        return false;
    }
    std::string err;
    void* lib = open_lib(path, &err);
    if (!lib) {
        load_ = Load::Blocked;
        status_ = "STEAM API DLL BLOCKED";
        log("cannot load " + path + ": " + err);
        return false;
    }
    std::vector<std::string> missing;
    auto get = [&](auto& slot, const std::string& name) {
        slot = (std::remove_reference_t<decltype(slot)>)sym(lib, name.c_str());
        if (!slot) {
            missing.push_back(name);
        }
    };
    get(f_.InitFlat, "SteamAPI_InitFlat");
    get(f_.Shutdown, "SteamAPI_Shutdown");
    get(f_.GetHSteamPipe, "SteamAPI_GetHSteamPipe");
    get(f_.MDInit, "SteamAPI_ManualDispatch_Init");
    get(f_.MDRunFrame, "SteamAPI_ManualDispatch_RunFrame");
    get(f_.MDNext, "SteamAPI_ManualDispatch_GetNextCallback");
    get(f_.MDFree, "SteamAPI_ManualDispatch_FreeLastCallback");
    get(f_.MDCallResult, "SteamAPI_ManualDispatch_GetAPICallResult");
    get(f_.Sockets, accessor("SteamAPI_SteamNetworkingSockets", STEAMNETWORKINGSOCKETS_INTERFACE_VERSION, "_SteamAPI_v"));
    get(f_.Utils, accessor("SteamAPI_SteamNetworkingUtils", STEAMNETWORKINGUTILS_INTERFACE_VERSION, "_SteamAPI_v"));
    get(f_.Matchmaking, accessor("SteamAPI_SteamMatchmaking", STEAMMATCHMAKING_INTERFACE_VERSION, "_v"));
    get(f_.InitRelay, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess");
    get(f_.Listen, "SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P");
    get(f_.Connect, "SteamAPI_ISteamNetworkingSockets_ConnectP2P");
    get(f_.Accept, "SteamAPI_ISteamNetworkingSockets_AcceptConnection");
    get(f_.Close, "SteamAPI_ISteamNetworkingSockets_CloseConnection");
    get(f_.CloseListen, "SteamAPI_ISteamNetworkingSockets_CloseListenSocket");
    get(f_.Send, "SteamAPI_ISteamNetworkingSockets_SendMessageToConnection");
    get(f_.Receive, "SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection");
    get(f_.Flush, "SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection");
    get(f_.RealTime, "SteamAPI_ISteamNetworkingSockets_GetConnectionRealTimeStatus");
    get(f_.Release, "SteamAPI_SteamNetworkingMessage_t_Release");
    get(f_.CreateLobby, "SteamAPI_ISteamMatchmaking_CreateLobby");
    get(f_.SetLobbyData, "SteamAPI_ISteamMatchmaking_SetLobbyData");
    get(f_.GetLobbyData, "SteamAPI_ISteamMatchmaking_GetLobbyData");
    get(f_.StringFilter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter");
    get(f_.DistanceFilter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter");
    get(f_.CountFilter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter");
    get(f_.RequestLobbyList, "SteamAPI_ISteamMatchmaking_RequestLobbyList");
    get(f_.LobbyByIndex, "SteamAPI_ISteamMatchmaking_GetLobbyByIndex");
    get(f_.JoinLobby, "SteamAPI_ISteamMatchmaking_JoinLobby");
    get(f_.LeaveLobby, "SteamAPI_ISteamMatchmaking_LeaveLobby");
    get(f_.LobbyOwner, "SteamAPI_ISteamMatchmaking_GetLobbyOwner");
    get(f_.SetJoinable, "SteamAPI_ISteamMatchmaking_SetLobbyJoinable");
    if (!missing.empty()) {
        load_ = Load::WrongVersion;
        status_ = "STEAM API DLL WRONG VERSION";
        std::string list;
        for (const auto& m : missing) {
            list += " " + m;
        }
        log(path + " is not from Steamworks SDK " RS64_STEAM_SDK_VERSION "; missing:" + list);
        return false;
    }
    load_ = Load::Loaded;
    log("loaded " + path);
    return true;
}

bool Steam::available() {
    if (inited_) {
        return true;
    }
    if (!load()) {
        return false;
    }
    const int64_t now = now_ms();
    if (now < retry_at_) {
        return false;
    }
    retry_at_ = now + kRetryMs;
#if defined(__linux__)
    // Flatpak / Snap Steam runs sandboxed: a game outside the sandbox cannot reach it.
    if (const char* home = getenv("HOME")) {
        const std::string h(home);
        if (!file_exists(h + "/.steam/sdk64/steamclient.so") && (file_exists(h + "/.var/app/com.valvesoftware.Steam") || file_exists(h + "/snap/steam"))) {
            status_ = "STEAM FLATPAK NOT SUPPORTED";
            return false;
        }
    }
#endif
    const char* appid = getenv("ROGUESQ_STEAM_APPID");
    const std::string id = appid && *appid ? appid : "480";
    set_env("SteamAppId", id.c_str());
    set_env("SteamGameId", id.c_str());
    SteamErrMsg err = {};
#ifdef _WIN32
    // steam_api sets up its own crash reporting: keep the game's handler (it writes dumps/crash-dumps).
    const LPTOP_LEVEL_EXCEPTION_FILTER game_filter = SetUnhandledExceptionFilter(nullptr);
    SetUnhandledExceptionFilter(game_filter);
    const ESteamAPIInitResult r = f_.InitFlat(&err);
    if (SetUnhandledExceptionFilter(game_filter) != game_filter) {
        log("restored the game's crash handler after Steam replaced it");
    }
#else
    const ESteamAPIInitResult r = f_.InitFlat(&err);
#endif
    if (r != k_ESteamAPIInitResult_OK) {
        status_ = r == k_ESteamAPIInitResult_NoSteamClient ? "STEAM IS NOT RUNNING" : r == k_ESteamAPIInitResult_VersionMismatch ? "STEAM API DLL WRONG VERSION" : "STEAM FAILED TO START";
        log("SteamAPI_InitFlat failed (" + std::to_string((int)r) + "): " + err);
        return false;
    }
    f_.MDInit();
    pipe_ = f_.GetHSteamPipe();
    sockets_ = f_.Sockets();
    mm_ = f_.Matchmaking();
    ISteamNetworkingUtils* utils = f_.Utils();
    // InitFlat does not check interface versions; an older Steam client answers NULL for one it lacks.
    if (!sockets_ || !mm_ || !utils) {
        f_.Shutdown();
        status_ = "STEAM API DLL WRONG VERSION";
        log("the Steam client lacks an interface Steamworks SDK " RS64_STEAM_SDK_VERSION " needs (update Steam)");
        return false;
    }
    f_.InitRelay(utils);
    inited_ = true;
    status_ = "STEAM READY";
    log("initialized as app " + id);
    return true;
}

void Steam::set_status(const std::string& s) {
    if (s != status_) {
        status_ = s;
        log("status: " + s);
    }
}

void Steam::fail(const std::string& why) {
    set_status(why);
    lost_ = true;
    failed_ = true;
}

std::string Steam::build_key() const {
    char b[16];
    snprintf(b, sizeof b, "%08X", build_);
    return b;
}

bool Steam::start(int role, const char* peer, uint32_t build) {
    if (!available()) {
        return false;
    }
    stop();
    failed_ = false;
    // A friends-list join that arrived during an earlier lobby is stale now.
    pending_.clear();
    role_ = role;
    build_ = build;
    if (role == RS64_MP_ROLE_HOST) {
        listen_ = f_.Listen(sockets_, 0, 0, nullptr);
        call_ = f_.CreateLobby(mm_, k_ELobbyTypePublic, 2);
        call_id_ = LobbyCreated_t::k_iCallback;
        set_status("CREATING STEAM LOBBY");
        if (listen_ == k_HSteamListenSocket_Invalid || call_ == k_uAPICallInvalid) {
            // Link never calls stop() after a failed start: undo the half that worked here.
            stop();
            set_status("COULD NOT CREATE STEAM LOBBY");
            return false;
        }
        return true;
    }
    const std::string p = peer ? peer : "";
    if (p.rfind("lobby:", 0) == 0) {
        join_lobby(strtoull(p.c_str() + 6, nullptr, 10));
    } else {
        search(p);
    }
    return true;
}

void Steam::search(const std::string& code) {
    code_ = code;
    // App 480 lobbies come from every Spacewar user: match the build key too, so a stranger's "code" never hides ours.
    f_.StringFilter(mm_, "code", code.c_str(), k_ELobbyComparisonEqual);
    f_.StringFilter(mm_, "rs64r", build_key().c_str(), k_ELobbyComparisonEqual);
    f_.DistanceFilter(mm_, k_ELobbyDistanceFilterWorldwide);
    f_.CountFilter(mm_, 1);
    call_ = f_.RequestLobbyList(mm_);
    call_id_ = LobbyMatchList_t::k_iCallback;
    set_status("FINDING GAME " + code);
}

void Steam::join_lobby(uint64_t lobby) {
    call_ = f_.JoinLobby(mm_, lobby);
    call_id_ = LobbyEnter_t::k_iCallback;
    set_status("JOINING STEAM LOBBY");
}

void Steam::stop() {
    if (!inited_) {
        return;
    }
    if (conn_ != k_HSteamNetConnection_Invalid) {
        f_.Close(sockets_, conn_, 0, "stop", true);
    }
    if (listen_ != k_HSteamListenSocket_Invalid) {
        f_.CloseListen(sockets_, listen_);
    }
    if (lobby_) {
        f_.LeaveLobby(mm_, lobby_);
    }
    conn_ = k_HSteamNetConnection_Invalid;
    listen_ = k_HSteamListenSocket_Invalid;
    lobby_ = 0;
    call_ = k_uAPICallInvalid;
    role_ = 0;
    connected_ = false;
    lost_ = false;
    connects_ = 0;
    inbox_.clear();
    host_label_.clear();
    code_.clear();
    // A failure's reason stays on the status line after multiplayer gives up and stops; the next start() clears it.
    if (!failed_) {
        status_ = "STEAM READY";
    }
}

void Steam::shutdown() {
    if (!inited_) {
        return;
    }
    stop();
    f_.Shutdown();
    inited_ = false;
    retry_at_ = 0;
    pending_.clear();
    status_ = "";
    log("shut down");
}

void Steam::pump() {
    if (!inited_) {
        return;
    }
    const int64_t now = now_ms();
    if (now - pumped_at_ < kPumpMs) {
        return;
    }
    pumped_at_ = now;
    f_.MDRunFrame(pipe_);
    CallbackMsg_t m;
    while (f_.MDNext(pipe_, &m)) {
        on_callback(m);
        f_.MDFree(pipe_);
    }
    if (conn_ != k_HSteamNetConnection_Invalid) {
        SteamNetworkingMessage_t* msgs[32];
        int n;
        while ((n = f_.Receive(sockets_, conn_, msgs, 32)) > 0) {
            for (int i = 0; i < n; ++i) {
                const uint8_t* d = (const uint8_t*)msgs[i]->m_pData;
                inbox_.emplace_back(d, d + msgs[i]->m_cbSize);
                f_.Release(msgs[i]);
            }
        }
    }
}

void Steam::on_callback(const CallbackMsg_t& m) {
    if (m.m_iCallback == SteamAPICallCompleted_t::k_iCallback) {
        const auto* c = (const SteamAPICallCompleted_t*)m.m_pubParam;
        if (c->m_hAsyncCall == call_) {
            on_call(c->m_hAsyncCall, c->m_iCallback);
        } else {
            leave_stale(c->m_hAsyncCall, c->m_iCallback);
        }
    } else if (m.m_iCallback == SteamNetConnectionStatusChangedCallback_t::k_iCallback) {
        on_connection(*(const SteamNetConnectionStatusChangedCallback_t*)m.m_pubParam);
    } else if (m.m_iCallback == GameLobbyJoinRequested_t::k_iCallback) {
        const auto* j = (const GameLobbyJoinRequested_t*)m.m_pubParam;
        pending_ = "lobby:" + std::to_string(j->m_steamIDLobby.ConvertToUint64());
        log("friends-list join request for lobby " + pending_.substr(6));
    }
}

// A lobby created or joined by a call that stop() abandoned (STOP HOSTING / CANCEL JOIN while it was in flight): leave it, or it holds one of its two slots.
void Steam::leave_stale(SteamAPICall_t call, int id) {
    bool failed = false;
    if (id == LobbyCreated_t::k_iCallback) {
        LobbyCreated_t r{};
        if (f_.MDCallResult(pipe_, call, &r, sizeof r, id, &failed) && !failed && r.m_eResult == k_EResultOK) {
            f_.LeaveLobby(mm_, r.m_ulSteamIDLobby);
        }
    } else if (id == LobbyEnter_t::k_iCallback) {
        LobbyEnter_t r{};
        if (f_.MDCallResult(pipe_, call, &r, sizeof r, id, &failed) && !failed && r.m_EChatRoomEnterResponse == k_EChatRoomEnterResponseSuccess) {
            f_.LeaveLobby(mm_, r.m_ulSteamIDLobby);
        }
    }
}

void Steam::on_call(SteamAPICall_t call, int id) {
    call_ = k_uAPICallInvalid;
    bool failed = false;
    if (id == LobbyCreated_t::k_iCallback) {
        LobbyCreated_t r{};
        if (!f_.MDCallResult(pipe_, call, &r, sizeof r, id, &failed) || failed || r.m_eResult != k_EResultOK) {
            fail("COULD NOT CREATE STEAM LOBBY");
            return;
        }
        lobby_ = r.m_ulSteamIDLobby;
        char code[8];
        snprintf(code, sizeof code, "%06u", (unsigned)(rng_() % 1000000u));
        code_ = code;
        f_.SetLobbyData(mm_, lobby_, "code", code_.c_str());
        f_.SetLobbyData(mm_, lobby_, "rs64r", build_key().c_str());
        host_label_ = "JOIN CODE " + code_;
        set_status("SHARE THE JOIN CODE");
        log("lobby " + std::to_string(lobby_) + " code " + code_);
    } else if (id == LobbyMatchList_t::k_iCallback) {
        LobbyMatchList_t r{};
        if (!f_.MDCallResult(pipe_, call, &r, sizeof r, id, &failed) || failed || r.m_nLobbiesMatching == 0) {
            fail("NO GAME WITH THAT CODE");
            return;
        }
        join_lobby(f_.LobbyByIndex(mm_, 0));
    } else if (id == LobbyEnter_t::k_iCallback) {
        LobbyEnter_t r{};
        if (!f_.MDCallResult(pipe_, call, &r, sizeof r, id, &failed) || failed || r.m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess) {
            fail("COULD NOT JOIN THAT GAME");
            return;
        }
        lobby_ = r.m_ulSteamIDLobby;
        const char* key = f_.GetLobbyData(mm_, lobby_, "rs64r");
        if (!key || build_key() != key) {
            fail("VERSION MISMATCH");
            log(std::string("lobby build ") + (key ? key : "?") + ", ours " + build_key());
            return;
        }
        SteamNetworkingIdentity host;
        host.SetSteamID64(f_.LobbyOwner(mm_, lobby_));
        conn_ = f_.Connect(sockets_, host, 0, 0, nullptr);
        set_status("CONNECTING OVER STEAM");
    }
}

void Steam::on_connection(const SteamNetConnectionStatusChangedCallback_t& c) {
    const ESteamNetworkingConnectionState st = c.m_info.m_eState;
    if (role_ == RS64_MP_ROLE_HOST && c.m_info.m_hListenSocket == listen_ && st == k_ESteamNetworkingConnectionState_Connecting) {
        if (conn_ == k_HSteamNetConnection_Invalid) {
            conn_ = c.m_hConn;
            f_.Accept(sockets_, conn_);
        } else {
            f_.Close(sockets_, c.m_hConn, 0, "full", false);
        }
        return;
    }
    if (c.m_hConn != conn_) {
        return;
    }
    if (st == k_ESteamNetworkingConnectionState_Connected) {
        connected_ = true;
        lost_ = false;
        ++connects_;
        set_status(role_ == RS64_MP_ROLE_HOST ? "PLAYER JOINED OVER STEAM" : "CONNECTED OVER STEAM");
        if (role_ == RS64_MP_ROLE_HOST && lobby_) {
            f_.SetJoinable(mm_, lobby_, false);
        }
    } else if (st == k_ESteamNetworkingConnectionState_ClosedByPeer || st == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
        log(std::string("connection ended: ") + c.m_info.m_szEndDebug);
        f_.Close(sockets_, conn_, 0, nullptr, false);
        conn_ = k_HSteamNetConnection_Invalid;
        const bool was = connected_;
        connected_ = false;
        if (role_ == RS64_MP_ROLE_HOST) {
            lost_ = was;
            set_status("SHARE THE JOIN CODE");
            if (lobby_) {
                f_.SetJoinable(mm_, lobby_, true);
            }
        } else {
            fail(was ? "STEAM CONNECTION LOST" : "COULD NOT REACH THE HOST");
        }
    }
}

void Steam::send(const uint8_t* b, uint32_t n, bool reliable) {
    pump();
    if (conn_ == k_HSteamNetConnection_Invalid || !connected_) {
        return;
    }
    f_.Send(sockets_, conn_, b, n, reliable ? k_nSteamNetworkingSend_ReliableNoNagle : k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
}

bool Steam::flush(int timeout_ms) {
    if (conn_ == k_HSteamNetConnection_Invalid) {
        return true;
    }
    f_.Flush(sockets_, conn_);
    const int64_t until = now_ms() + timeout_ms;
    do {
        SteamNetConnectionRealTimeStatus_t st{};
        if (f_.RealTime(sockets_, conn_, &st, 0, nullptr) != k_EResultOK || (st.m_cbPendingReliable == 0 && st.m_cbSentUnackedReliable == 0)) {
            return true;
        }
#ifdef _WIN32
        Sleep(1);
#else
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, nullptr);
#endif
    } while (now_ms() < until);
    return false;
}

void Steam::receive(rs64_mp_recv_fn fn, void* fn_user) {
    pump();
    while (!inbox_.empty()) {
        const std::vector<uint8_t>& m = inbox_.front();
        fn(m.data(), (uint32_t)m.size(), fn_user);
        inbox_.pop_front();
    }
}

const char* Steam::pending_join() {
    pump();
    if (pending_.empty()) {
        return nullptr;
    }
    pending_out_.swap(pending_);
    pending_.clear();
    return pending_out_.c_str();
}

Steam& S(void* u) { return *(Steam*)u; }

const rs64_mp_transport kTransport = {
    RS64_MP_TRANSPORT_VERSION,
    (uint32_t)sizeof(rs64_mp_transport),
    "steam",
    &g_steam,
    [](void* u) -> int { return S(u).available() ? 1 : 0; },
    [](void* u, int role, const char* peer, uint32_t build) -> int { return S(u).start(role, peer, build) ? 1 : 0; },
    [](void* u) { S(u).stop(); },
    [](void* u) { S(u).shutdown(); },
    [](void* u) -> int { return S(u).connected() ? 1 : 0; },
    [](void* u) -> int { return S(u).lost() ? 1 : 0; },
    [](void* u) -> uint32_t { return S(u).connect_count(); },
    [](void* u, const uint8_t* b, uint32_t n, int reliable) { S(u).send(b, n, reliable != 0); },
    [](void* u, int ms) -> int { return S(u).flush(ms) ? 1 : 0; },
    [](void* u, rs64_mp_recv_fn fn, void* fu) { S(u).receive(fn, fu); },
    [](void* u) -> const char* { return S(u).status(); },
    [](void* u) -> const char* { return S(u).host_label(); },
    [](void* u) -> const char* { return S(u).pending_join(); },
};

}

// r4 = the host API table, r5 = the version the host implements; r2 = 0 accepts, r3 = the version this mod was built against.
MOD_EXPORT void rs64_mod_init(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    const rs64_host_api* api = (const rs64_host_api*)(uintptr_t)ctx->r4;
    ctx->r3 = RS64_HOST_API_VERSION;
    if ((uint32_t)ctx->r5 < RS64_HOST_API_VERSION || !api || api->size < sizeof(rs64_host_api)) {
        ctx->r2 = 1;
        return;
    }
    g_api = api;
    if (api->add_service(RS64_MP_TRANSPORT_SERVICE, &kTransport) != 0) {
        ctx->r2 = 1;
        return;
    }
    api->log("[steam] relay transport registered; Steam starts when the MULTIPLAYER page opens");
    ctx->r2 = 0;
}
