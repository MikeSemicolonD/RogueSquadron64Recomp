// Desktop side of Wi-Fi Direct co-op: the same fixed group as the Android build (MainActivity), so a PC and a phone pair with no router.
// Host: Windows runs a WinRT legacy group owner, Linux a NetworkManager hotspot. Join: when a LAN search finds nobody and the group is in range, the PC joins it as a plain Wi-Fi network and goes back to its own afterwards.
#include "net/net_link.h"

#if RS64_WIFI_DIRECT

#if defined(_WIN32)

#include <winsock2.h>
#include <windows.h>
#include <wlanapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.WiFiDirect.h>
#include <winrt/Windows.Security.Credentials.h>

#else

#include <dlfcn.h>
#include <type_traits>
#include <utility>

#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace rs64::net {

namespace {

constexpr char kSsid[] = "DIRECT-RS-RogueSquadron64";
constexpr int kPollMs = 500;

struct State {
    std::mutex m;
    std::condition_variable cv;
    int mode = 0;
    bool searching = false;
    bool quit = false;
    std::atomic<int> link{0};
    std::thread worker;
};

// Never destroyed: the game leaves through _Exit, and a joinable thread in a static destructor would terminate.
State& st()
{

    static State* s = new State;
    return *s;
}

// Sleeps up to ms; false as soon as the mode moves off `mode` or the game quits.
bool hold(int mode, int ms)
{

    State& s = st();
    std::unique_lock lk(s.m);

    return !s.cv.wait_for(lk, std::chrono::milliseconds(ms), [&s, mode] { return s.quit || s.mode != mode; });
}

bool still_searching()
{

    State& s = st();
    std::lock_guard lk(s.m);

    return s.searching && s.mode == 2;
}

#if defined(_WIN32)

namespace wfd = winrt::Windows::Devices::WiFiDirect;

constexpr wchar_t kSsidW[] = L"DIRECT-RS-RogueSquadron64";
constexpr wchar_t kPass[] = L"rs64coop";
constexpr wchar_t kProfile[] = L"RS64 co-op";
constexpr int kScanPolls = 8;
constexpr int kAddressPolls = 30;

// shortcut: per-user WPA2 profile with the passphrase in clear, like the Android build's fixed group; give each host its own group if that matters.
constexpr wchar_t kProfileXml[] =
    L"<?xml version=\"1.0\"?><WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">"
    L"<name>RS64 co-op</name><SSIDConfig><SSID><name>DIRECT-RS-RogueSquadron64</name></SSID></SSIDConfig>"
    L"<connectionType>ESS</connectionType><connectionMode>manual</connectionMode><MSM><security>"
    L"<authEncryption><authentication>WPA2PSK</authentication><encryption>AES</encryption><useOneX>false</useOneX></authEncryption>"
    L"<sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>rs64coop</keyMaterial></sharedKey>"
    L"</security></MSM></WLANProfile>";

class Group {
public:
    void start()
    {

        if (pub_)
        {
            return;
        }

        try {
            pub_ = wfd::WiFiDirectAdvertisementPublisher();
            wfd::WiFiDirectAdvertisement adv = pub_.Advertisement();
            wfd::WiFiDirectLegacySettings legacy = adv.LegacySettings();
            winrt::Windows::Security::Credentials::PasswordCredential cred;

            adv.IsAutonomousGroupOwnerEnabled(true);
            legacy.IsEnabled(true);
            legacy.Ssid(kSsidW);
            cred.Password(kPass);
            legacy.Passphrase(cred);

            pub_.StatusChanged([](const auto&, const wfd::WiFiDirectAdvertisementPublisherStatusChangedEventArgs& a) {
                fprintf(stderr, "[net] Wi-Fi Direct group status %d (error %d)\n", (int)a.Status(), (int)a.Error());
            });

            // Phones that connect as Wi-Fi Direct peers (not as plain Wi-Fi clients) are accepted here.
            listener_ = wfd::WiFiDirectConnectionListener();
            listener_.ConnectionRequested([this](const auto&, const wfd::WiFiDirectConnectionRequestedEventArgs& a) { accept(a); });
            pub_.Start();

            fprintf(stderr, "[net] Wi-Fi Direct group started\n");
        } catch (const winrt::hresult_error& e) {
            fprintf(stderr, "[net] Wi-Fi Direct group failed: 0x%08X\n", (unsigned)e.code());
            stop();
        }
    }

    void stop()
    {

        if (pub_ && pub_.Status() == wfd::WiFiDirectAdvertisementPublisherStatus::Started) {
            pub_.Stop();
        }

        pub_ = nullptr;
        listener_ = nullptr;

        std::lock_guard lk(m_);

        for (wfd::WiFiDirectDevice& d : peers_) {
            d.Close();
        }

        peers_.clear();
    }

private:
    void accept(const wfd::WiFiDirectConnectionRequestedEventArgs& a)
    {

        try {
            wfd::WiFiDirectConnectionRequest req = a.GetConnectionRequest();
            wfd::WiFiDirectDevice dev = wfd::WiFiDirectDevice::FromIdAsync(req.DeviceInformation().Id()).get();
            std::lock_guard lk(m_);

            peers_.push_back(dev);
            fprintf(stderr, "[net] Wi-Fi Direct peer accepted\n");
        } catch (const winrt::hresult_error& e) {
            fprintf(stderr, "[net] Wi-Fi Direct peer refused: 0x%08X\n", (unsigned)e.code());
        }
    }

    wfd::WiFiDirectAdvertisementPublisher pub_{nullptr};
    wfd::WiFiDirectConnectionListener listener_{nullptr};
    std::mutex m_;
    std::vector<wfd::WiFiDirectDevice> peers_;
};

class Wlan {
public:
    Wlan()
    {

        DWORD version = 0;
        WLAN_INTERFACE_INFO_LIST* list = nullptr;

        if (WlanOpenHandle(2, nullptr, &version, &h_) != ERROR_SUCCESS)
        {
            h_ = nullptr;
            return;
        }

        if (WlanEnumInterfaces(h_, nullptr, &list) == ERROR_SUCCESS && list->dwNumberOfItems > 0) {
            iface_ = list->InterfaceInfo[0].InterfaceGuid;
            ok_ = true;
        }

        if (list) {
            WlanFreeMemory(list);
        }

        // A profile left over from a run that crashed while joined.
        if (ok_) {
            WlanDeleteProfile(h_, &iface_, kProfile, nullptr);
        }
    }

    bool joined() const { return joined_; }

    // Scans for the group and joins it while the lobby still searches; returns the link value to report.
    int join()
    {

        if (!ok_)
        {
            return 0;
        }

        WlanScan(h_, &iface_, nullptr, nullptr, nullptr);
        bool seen = visible();

        for (int i = 0; i < kScanPolls && !seen; ++i) {
            if (!hold(2, kPollMs))
            {
                return 0;
            }

            seen = visible();
        }

        if (!seen) {
            fprintf(stderr, "[net] no Wi-Fi Direct game in range\n");
        }

        if (!seen || !still_searching())
        {
            return 0;
        }

        const std::vector<uint32_t> before = local_ipv4s();
        WLAN_REASON_CODE reason = 0;

        prev_ = current_profile();

        if (WlanSetProfile(h_, &iface_, WLAN_PROFILE_USER, kProfileXml, nullptr, TRUE, nullptr, &reason) != ERROR_SUCCESS)
        {
            fprintf(stderr, "[net] Wi-Fi Direct profile rejected (reason %lu)\n", (unsigned long)reason);
            return 0;
        }

        joined_ = true;
        connect(kProfile);
        fprintf(stderr, "[net] joining the Wi-Fi Direct game\n");

        // Linked once the group's DHCP hands out an address this PC did not have before.
        for (int i = 0; i < kAddressPolls; ++i) {
            if (!hold(2, kPollMs))
            {
                return 1;
            }

            const std::vector<uint32_t> now = local_ipv4s();
            const bool fresh = std::any_of(now.begin(), now.end(), [&before](uint32_t a) { return std::find(before.begin(), before.end(), a) == before.end(); });

            if (fresh)
            {
                fprintf(stderr, "[net] Wi-Fi Direct linked\n");
                return 2;
            }
        }

        fprintf(stderr, "[net] Wi-Fi Direct join timed out\n");
        return 0;
    }

    // Leaves the group and reconnects the network the PC was on.
    void restore()
    {

        if (!joined_)
        {
            return;
        }

        joined_ = false;
        WlanDisconnect(h_, &iface_, nullptr);
        WlanDeleteProfile(h_, &iface_, kProfile, nullptr);

        if (!prev_.empty()) {
            connect(prev_.c_str());
        }

        fprintf(stderr, "[net] left the Wi-Fi Direct game\n");
    }

private:
    bool visible() const
    {

        WLAN_AVAILABLE_NETWORK_LIST* list = nullptr;
        bool found = false;

        if (WlanGetAvailableNetworkList(h_, &iface_, 0, nullptr, &list) != ERROR_SUCCESS)
        {
            return false;
        }

        for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
            const DOT11_SSID& ssid = list->Network[i].dot11Ssid;

            if (ssid.uSSIDLength == sizeof(kSsid) - 1 && memcmp(ssid.ucSSID, kSsid, ssid.uSSIDLength) == 0) {
                found = true;
            }
        }

        WlanFreeMemory(list);
        return found;
    }

    std::wstring current_profile() const
    {

        WLAN_CONNECTION_ATTRIBUTES* cc = nullptr;
        DWORD size = 0;
        std::wstring name;

        if (WlanQueryInterface(h_, &iface_, wlan_intf_opcode_current_connection, nullptr, &size, (void**)&cc, nullptr) != ERROR_SUCCESS)
        {
            return name;
        }

        if (cc->isState == wlan_interface_state_connected) {
            name = cc->strProfileName;
        }

        WlanFreeMemory(cc);
        return name;
    }

    void connect(const wchar_t* profile)
    {

        WLAN_CONNECTION_PARAMETERS p{};

        p.wlanConnectionMode = wlan_connection_mode_profile;
        p.strProfile = profile;
        p.dot11BssType = dot11_BSS_type_infrastructure;
        WlanConnect(h_, &iface_, &p, nullptr);
    }

    HANDLE h_ = nullptr;
    GUID iface_{};
    bool ok_ = false;
    bool joined_ = false;
    std::wstring prev_;
};

#else

// NetworkManager over D-Bus, through libdbus loaded at run time (no build dependency); the Flatpak reaches it with --system-talk-name=org.freedesktop.NetworkManager.
// The hotspot and join connections are volatile: never saved, and NetworkManager deletes them once they deactivate.
constexpr const char* kNm = "org.freedesktop.NetworkManager";
constexpr const char* kNmPath = "/org/freedesktop/NetworkManager";
constexpr const char* kProps = "org.freedesktop.DBus.Properties";
constexpr int kScanPolls = 8;
constexpr int kActivatePolls = 30;
constexpr uint32_t kDeviceWifi = 2;
constexpr uint32_t kDeviceDisconnected = 30;
constexpr uint32_t kActivated = 2;
constexpr uint32_t kDeactivated = 4;

// libdbus's iterator and error structs, sized past their real layouts (an iterator is 72 bytes on 64-bit).
struct DIter {
    alignas(void*) unsigned char b[128];
};

struct DError {
    const char* name;
    const char* message;
    alignas(void*) unsigned char rest[48];
};

struct Dbus {
    void* (*bus_get)(int, DError*);
    void (*error_init)(DError*);
    void (*error_free)(DError*);
    void (*exit_on_disconnect)(void*, unsigned);
    void* (*new_call)(const char*, const char*, const char*, const char*);
    void* (*send)(void*, void*, int, DError*);
    void (*unref)(void*);
    void (*init_append)(void*, DIter*);
    unsigned (*open)(DIter*, int, const char*, DIter*);
    unsigned (*close)(DIter*, DIter*);
    unsigned (*append)(DIter*, int, const void*);
    unsigned (*append_fixed)(DIter*, int, const void*, int);
    unsigned (*init)(void*, DIter*);
    int (*type)(DIter*);
    void (*get)(DIter*, void*);
    void (*recurse)(DIter*, DIter*);
    unsigned (*next)(DIter*);
    void (*get_fixed)(DIter*, void*, int*);
};

// One setting value: 's' string, 'b' boolean, 'y' bytes (from s), 'l' string list.
struct Setting {
    const char* key;
    char type;
    std::string s;
    std::vector<std::string> list;
};

using SettingGroup = std::pair<const char*, std::vector<Setting>>;

std::vector<SettingGroup> wifi_settings(bool host)
{

    std::vector<SettingGroup> g = {
        {"connection", {{"id", 's', host ? "RS64 co-op host" : "RS64 co-op", {}}, {"type", 's', "802-11-wireless", {}}, {"autoconnect", 'b', "", {}}}},
        {"802-11-wireless", {{"ssid", 'y', kSsid, {}}, {"mode", 's', host ? "ap" : "infrastructure", {}}}},
        {"802-11-wireless-security", {{"key-mgmt", 's', "wpa-psk", {}}, {"psk", 's', "rs64coop", {}}}},
    };

    if (host) {
        g[1].second.push_back({"band", 's', "bg", {}});
        g[2].second.push_back({"proto", 'l', "", {"rsn"}});
        g[2].second.push_back({"pairwise", 'l', "", {"ccmp"}});
        g[2].second.push_back({"group", 'l', "", {"ccmp"}});
        g.push_back({"ipv4", {{"method", 's', "shared", {}}}});
    }

    return g;
}

// The system bus and the calls the pairing needs; used from the worker thread only.
class Nm {
public:
    bool open()
    {

        void* lib = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
        DError err;

        if (!lib)
        {
            return false;
        }

        const auto sym = [lib](const char* name, auto& fn) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
            return fn != nullptr;
        };

        const bool all = sym("dbus_bus_get", d_.bus_get) && sym("dbus_error_init", d_.error_init) && sym("dbus_error_free", d_.error_free)
            && sym("dbus_connection_set_exit_on_disconnect", d_.exit_on_disconnect) && sym("dbus_message_new_method_call", d_.new_call)
            && sym("dbus_connection_send_with_reply_and_block", d_.send) && sym("dbus_message_unref", d_.unref)
            && sym("dbus_message_iter_init_append", d_.init_append) && sym("dbus_message_iter_open_container", d_.open)
            && sym("dbus_message_iter_close_container", d_.close) && sym("dbus_message_iter_append_basic", d_.append)
            && sym("dbus_message_iter_append_fixed_array", d_.append_fixed) && sym("dbus_message_iter_init", d_.init)
            && sym("dbus_message_iter_get_arg_type", d_.type) && sym("dbus_message_iter_get_basic", d_.get)
            && sym("dbus_message_iter_recurse", d_.recurse) && sym("dbus_message_iter_next", d_.next)
            && sym("dbus_message_iter_get_fixed_array", d_.get_fixed);

        if (!all)
        {
            return false;
        }

        d_.error_init(&err);
        bus_ = d_.bus_get(1, &err);

        if (!bus_) {
            fprintf(stderr, "[net] no system bus: %s\n", err.message ? err.message : "?");
            d_.error_free(&err);
            return false;
        }

        // libdbus exits the process on a lost bus unless told not to.
        d_.exit_on_disconnect(bus_, 0);
        return true;
    }

    // The Wi-Fi device's object path and state, or "" when there is none.
    std::string wifi_device(uint32_t* state)
    {

        std::string found;

        for (const std::string& dev : paths(kNmPath, kNm, "GetDevices")) {
            if (found.empty() && prop_u(dev, "org.freedesktop.NetworkManager.Device", "DeviceType") == kDeviceWifi) {
                found = dev;
            }
        }

        if (!found.empty() && state) {
            *state = prop_u(found, "org.freedesktop.NetworkManager.Device", "State");
        }

        return found;
    }

    bool ssid_visible(const std::string& dev)
    {

        for (const std::string& ap : paths(dev, "org.freedesktop.NetworkManager.Device.Wireless", "GetAllAccessPoints")) {
            if (prop_bytes(ap, "org.freedesktop.NetworkManager.AccessPoint", "Ssid") == kSsid) {
                return true;
            }
        }

        return false;
    }

    void request_scan(const std::string& dev)
    {

        void* reply = call(dev.c_str(), "org.freedesktop.NetworkManager.Device.Wireless", "RequestScan", [this](DIter* it) { empty_dict(it); }, false);

        if (reply) {
            d_.unref(reply);
        }
    }

    // A volatile connection made and activated on dev; returns its active-connection path, or "".
    std::string add_and_activate(const std::vector<SettingGroup>& settings, const std::string& dev)
    {

        void* reply = call(kNmPath, kNm, "AddAndActivateConnection2", [&](DIter* it) {
            append_settings(it, settings);
            append_path(it, dev.c_str());
            append_path(it, "/");
            append_options(it);
        }, true);

        return second_path(reply);
    }

    void activate(const std::string& conn, const std::string& dev)
    {

        void* reply = call(kNmPath, kNm, "ActivateConnection", [&](DIter* it) {
            append_path(it, conn.c_str());
            append_path(it, dev.c_str());
            append_path(it, "/");
        }, true);

        if (reply) {
            d_.unref(reply);
        }
    }

    void deactivate(const std::string& active)
    {

        void* reply = call(kNmPath, kNm, "DeactivateConnection", [&](DIter* it) { append_path(it, active.c_str()); }, false);

        if (reply) {
            d_.unref(reply);
        }
    }

    uint32_t active_state(const std::string& active) {
        return prop_u(active, "org.freedesktop.NetworkManager.Connection.Active", "State");
    }

    // The saved connection behind the device's active one ("" when it has none), to bring back afterwards.
    std::string current_connection(const std::string& dev)
    {

        const std::string active = prop_path(dev, "org.freedesktop.NetworkManager.Device", "ActiveConnection");

        if (active.empty() || active == "/")
        {
            return {};
        }

        const std::string conn = prop_path(active, "org.freedesktop.NetworkManager.Connection.Active", "Connection");
        return conn == "/" ? std::string{} : conn;
    }

private:
    template <typename Args>
    void* call(const char* path, const char* iface, const char* method, const Args& args, bool log_error)
    {

        void* msg = d_.new_call(kNm, path, iface, method);
        DIter it;
        DError err;

        if (!msg)
        {
            return nullptr;
        }

        d_.init_append(msg, &it);
        args(&it);
        d_.error_init(&err);

        void* reply = d_.send(bus_, msg, 5000, &err);

        d_.unref(msg);

        if (!reply) {
            if (log_error) {
                fprintf(stderr, "[net] NetworkManager %s: %s\n", method, err.message ? err.message : "failed");
            }

            d_.error_free(&err);
        }

        return reply;
    }

    std::vector<std::string> paths(const std::string& path, const char* iface, const char* method)
    {

        void* reply = call(path.c_str(), iface, method, [](DIter*) {}, false);
        std::vector<std::string> out;
        DIter it;
        DIter arr;

        if (!reply)
        {
            return out;
        }

        if (d_.init(reply, &it) && d_.type(&it) == 'a') {
            d_.recurse(&it, &arr);

            while (d_.type(&arr) == 'o') {
                const char* p = nullptr;

                d_.get(&arr, &p);
                out.emplace_back(p ? p : "");
                d_.next(&arr);
            }
        }

        d_.unref(reply);
        return out;
    }

    // Properties.Get: the reply and an iterator on the value inside its variant.
    void* get_prop(const std::string& path, const char* iface, const char* name, DIter* value)
    {

        void* reply = call(path.c_str(), kProps, "Get", [&](DIter* it) {
            append_string(it, iface);
            append_string(it, name);
        }, false);
        DIter it;

        if (!reply)
        {
            return nullptr;
        }

        if (!d_.init(reply, &it) || d_.type(&it) != 'v') {
            d_.unref(reply);
            return nullptr;
        }

        d_.recurse(&it, value);
        return reply;
    }

    uint32_t prop_u(const std::string& path, const char* iface, const char* name)
    {

        DIter v;
        uint32_t out = 0;
        void* reply = get_prop(path, iface, name, &v);

        if (!reply)
        {
            return 0;
        }

        if (d_.type(&v) == 'u') {
            d_.get(&v, &out);
        }

        d_.unref(reply);
        return out;
    }

    std::string prop_path(const std::string& path, const char* iface, const char* name)
    {

        DIter v;
        std::string out;
        void* reply = get_prop(path, iface, name, &v);

        if (!reply)
        {
            return out;
        }

        if (d_.type(&v) == 'o') {
            const char* p = nullptr;

            d_.get(&v, &p);
            out = p ? p : "";
        }

        d_.unref(reply);
        return out;
    }

    std::string prop_bytes(const std::string& path, const char* iface, const char* name)
    {

        DIter v;
        DIter arr;
        std::string out;
        void* reply = get_prop(path, iface, name, &v);

        if (!reply)
        {
            return out;
        }

        if (d_.type(&v) == 'a') {
            const char* bytes = nullptr;
            int n = 0;

            d_.recurse(&v, &arr);
            d_.get_fixed(&arr, &bytes, &n);
            out.assign(bytes ? bytes : "", bytes ? (size_t)n : 0);
        }

        d_.unref(reply);
        return out;
    }

    // AddAndActivateConnection2 answers (o path, o active_connection, a{sv}).
    std::string second_path(void* reply)
    {

        DIter it;
        std::string out;

        if (!reply)
        {
            return out;
        }

        if (d_.init(reply, &it) && d_.next(&it) && d_.type(&it) == 'o') {
            const char* p = nullptr;

            d_.get(&it, &p);
            out = p ? p : "";
        }

        d_.unref(reply);
        return out;
    }

    void append_string(DIter* it, const char* s) {
        d_.append(it, 's', &s);
    }

    void append_path(DIter* it, const char* s) {
        d_.append(it, 'o', &s);
    }

    void empty_dict(DIter* it)
    {

        DIter dict;

        d_.open(it, 'a', "{sv}", &dict);
        d_.close(it, &dict);
    }

    // a{sv} {"persist": "volatile"}: the connection lives in memory only and goes away when it deactivates.
    void append_options(DIter* it)
    {

        DIter dict;
        DIter entry;
        DIter var;
        const char* key = "persist";
        const char* val = "volatile";

        d_.open(it, 'a', "{sv}", &dict);
        d_.open(&dict, 'e', nullptr, &entry);
        d_.append(&entry, 's', &key);
        d_.open(&entry, 'v', "s", &var);
        d_.append(&var, 's', &val);
        d_.close(&entry, &var);
        d_.close(&dict, &entry);
        d_.close(it, &dict);
    }

    // a{sa{sv}}: the connection's setting groups.
    void append_settings(DIter* it, const std::vector<SettingGroup>& groups)
    {

        DIter outer;

        d_.open(it, 'a', "{sa{sv}}", &outer);

        for (const SettingGroup& g : groups) {
            DIter entry;
            DIter inner;

            d_.open(&outer, 'e', nullptr, &entry);
            d_.append(&entry, 's', &g.first);
            d_.open(&entry, 'a', "{sv}", &inner);

            for (const Setting& s : g.second) {
                append_setting(&inner, s);
            }

            d_.close(&entry, &inner);
            d_.close(&outer, &entry);
        }

        d_.close(it, &outer);
    }

    void append_setting(DIter* dict, const Setting& s)
    {

        static const char* const kSig[] = {"s", "b", "ay", "as"};
        const int sig = s.type == 's' ? 0 : s.type == 'b' ? 1 : s.type == 'y' ? 2 : 3;
        DIter entry;
        DIter var;
        DIter arr;

        d_.open(dict, 'e', nullptr, &entry);
        d_.append(&entry, 's', &s.key);
        d_.open(&entry, 'v', kSig[sig], &var);

        if (s.type == 's') {
            const char* v = s.s.c_str();

            d_.append(&var, 's', &v);
        } else if (s.type == 'b') {
            const uint32_t v = 0;

            d_.append(&var, 'b', &v);
        } else if (s.type == 'y') {
            const char* v = s.s.data();

            d_.open(&var, 'a', "y", &arr);
            d_.append_fixed(&arr, 'y', &v, (int)s.s.size());
            d_.close(&var, &arr);
        } else {
            d_.open(&var, 'a', "s", &arr);

            for (const std::string& item : s.list) {
                const char* v = item.c_str();

                d_.append(&arr, 's', &v);
            }

            d_.close(&var, &arr);
        }

        d_.close(&entry, &var);
        d_.close(dict, &entry);
    }

    Dbus d_{};
    void* bus_ = nullptr;
};

Nm* nm()
{

    static Nm* s = [] {

        Nm* n = new Nm;

        if (!n->open()) {
            fprintf(stderr, "[net] no NetworkManager; no Wi-Fi Direct\n");
            delete n;
            return (Nm*)nullptr;
        }

        return n;
    }();

    return s;
}

// Hosting takes the Wi-Fi radio, so a PC already on a Wi-Fi network (where LAN play works) does not start one.
class Group {
public:
    void start()
    {

        uint32_t state = 0;
        std::string dev;

        if (!active_.empty() || !nm())
        {
            return;
        }

        dev = nm()->wifi_device(&state);

        if (dev.empty() || state != kDeviceDisconnected)
        {
            fprintf(stderr, "[net] Wi-Fi is busy or missing; no Wi-Fi Direct game\n");
            return;
        }

        active_ = nm()->add_and_activate(wifi_settings(true), dev);

        if (!active_.empty()) {
            fprintf(stderr, "[net] Wi-Fi Direct game started (hotspot)\n");
        }
    }

    void stop()
    {

        if (active_.empty())
        {
            return;
        }

        nm()->deactivate(active_);
        active_.clear();
        fprintf(stderr, "[net] Wi-Fi Direct hotspot stopped\n");
    }

private:
    std::string active_;
};

class Wlan {
public:
    bool joined() const { return !active_.empty(); }

    // Scans for the group and joins it while the lobby still searches; returns the link value to report.
    int join()
    {

        if (!nm())
        {
            return 0;
        }

        dev_ = nm()->wifi_device(nullptr);

        if (dev_.empty())
        {
            return 0;
        }

        nm()->request_scan(dev_);
        bool seen = nm()->ssid_visible(dev_);

        for (int i = 0; i < kScanPolls && !seen; ++i) {
            if (!hold(2, kPollMs))
            {
                return 0;
            }

            seen = nm()->ssid_visible(dev_);
        }

        if (!seen) {
            fprintf(stderr, "[net] no Wi-Fi Direct game in range\n");
        }

        if (!seen || !still_searching())
        {
            return 0;
        }

        prev_ = nm()->current_connection(dev_);
        active_ = nm()->add_and_activate(wifi_settings(false), dev_);

        if (active_.empty())
        {
            return 0;
        }

        fprintf(stderr, "[net] joining the Wi-Fi Direct game\n");

        // NetworkManager reports the connection activated once it has its address.
        for (int i = 0; i < kActivatePolls; ++i) {
            if (!hold(2, kPollMs))
            {
                return 1;
            }

            const uint32_t state = nm()->active_state(active_);

            if (state == kActivated)
            {
                fprintf(stderr, "[net] Wi-Fi Direct linked\n");
                return 2;
            }

            if (state == kDeactivated || state == 0) {
                break;
            }
        }

        fprintf(stderr, "[net] Wi-Fi Direct join failed\n");
        return 0;
    }

    // Leaves the group and reconnects the network the PC was on.
    void restore()
    {

        if (active_.empty())
        {
            return;
        }

        nm()->deactivate(active_);
        active_.clear();

        if (!prev_.empty()) {
            nm()->activate(prev_, dev_);
        }

        fprintf(stderr, "[net] left the Wi-Fi Direct game\n");
    }

private:
    std::string dev_;
    std::string active_;
    std::string prev_;
};

#endif

void worker()
{

    State& s = st();
    int have_mode = 0;
    bool have_search = false;

#if defined(_WIN32)
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
#endif

    Group group;
    Wlan wlan;

    for (;;) {
        std::unique_lock lk(s.m);

        s.cv.wait(lk, [&] { return s.quit || s.mode != have_mode || s.searching != have_search; });

        const bool quit = s.quit;
        const int mode = quit ? 0 : s.mode;
        const bool search = s.searching;

        lk.unlock();
        have_mode = mode;
        have_search = search;

        if (mode == 1) {
            group.start();
        } else {
            group.stop();
        }

        if (mode == 0) {
            wlan.restore();
            s.link = 0;
        }

        // A search that starts on the group already finds the host over the LAN.
        if (mode == 2 && search && !wlan.joined()) {
            s.link = wlan.join();
        }

        if (quit)
        {
            return;
        }
    }
}

}

int wifi_direct_tick(int mode, bool searching)
{

    State& s = st();

    if (mode == 0 && !s.worker.joinable())
    {
        return 0;
    }

    if (!s.worker.joinable()) {
        s.worker = std::thread(worker);
    }

    std::unique_lock lk(s.m);

    if (mode != s.mode || searching != s.searching) {
        // Held at connecting from the first frame of a search, so the lobby waits for the scan.
        if (mode == 2 && searching && !s.searching && s.link == 0) {
            s.link = 1;
        }

        s.mode = mode;
        s.searching = searching;
        lk.unlock();
        s.cv.notify_all();
    }

    return s.link;
}

void wifi_direct_shutdown()
{

    State& s = st();

    if (!s.worker.joinable())
    {
        return;
    }

    std::unique_lock lk(s.m);

    s.quit = true;
    lk.unlock();
    s.cv.notify_all();
    s.worker.join();
}

}

#endif
