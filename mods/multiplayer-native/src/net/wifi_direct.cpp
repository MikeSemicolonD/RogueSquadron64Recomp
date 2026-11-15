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

constexpr char kHostCon[] = "'RS64 co-op host'";
constexpr char kJoinCon[] = "'RS64 co-op'";
constexpr char kSecurity[] = " ssid DIRECT-RS-RogueSquadron64 wifi-sec.key-mgmt wpa-psk wifi-sec.psk rs64coop";

// Runs a shell command; returns its exit status and, when asked, its output (stderr included, so a refusal's reason reaches the log).
int run(const std::string& cmd, std::string* out = nullptr)
{

    FILE* p = popen((cmd + " 2>&1").c_str(), "r");
    char buf[256];
    std::string text;

    if (!p)
    {
        return -1;
    }

    while (fgets(buf, sizeof buf, p)) {
        text += buf;
    }

    if (out) {
        *out = text;
    }

    return pclose(p);
}

// run() that logs nmcli's own message when the command fails.
bool run_logged(const std::string& cmd)
{

    std::string out;
    const bool ok = run(cmd, &out) == 0;

    if (!ok) {
        fprintf(stderr, "[net] %s: %s", cmd.c_str(), out.empty() ? "failed\n" : out.c_str());
    }

    return ok;
}

std::string shell_quoted(const std::string& s)
{

    std::string q = "'";

    for (char c : s) {
        q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }

    return q + "'";
}

bool has_line(const std::string& text, const std::string& line)
{

    return ("\n" + text + "\n").find("\n" + line + "\n") != std::string::npos;
}

// Hosting takes the Wi-Fi radio, so a PC already on a Wi-Fi network (where LAN play works) does not start one.
class Group {
public:
    void start()
    {

        std::string devices;

        if (on_)
        {
            return;
        }

        run("nmcli -t -f TYPE,STATE device", &devices);

        if (("\n" + devices).find("\nwifi:disconnected") == std::string::npos)
        {
            fprintf(stderr, "[net] Wi-Fi is busy or missing; no Wi-Fi Direct game\n");
            return;
        }

        const std::string add = std::string("nmcli con add type wifi ifname '*' autoconnect no con-name ") + kHostCon + kSecurity
            + " mode ap wifi.band bg ipv4.method shared wifi-sec.proto rsn wifi-sec.pairwise ccmp wifi-sec.group ccmp";

        on_ = run_logged(add);

        if (on_ && run_logged(std::string("nmcli --wait 15 con up ") + kHostCon)) {
            fprintf(stderr, "[net] Wi-Fi Direct game started (hotspot)\n");
        } else {
            fprintf(stderr, "[net] Wi-Fi Direct hotspot failed\n");
        }
    }

    void stop()
    {

        if (!on_)
        {
            return;
        }

        on_ = false;
        run(std::string("nmcli con delete ") + kHostCon);
        fprintf(stderr, "[net] Wi-Fi Direct hotspot stopped\n");
    }

private:
    bool on_ = false;
};

class Wlan {
public:
    // Connections left over from a run that crashed while hosting or joined.
    Wlan()
    {

        ok_ = run("nmcli -v") == 0;

        if (ok_) {
            run(std::string("nmcli con delete ") + kHostCon);
            run(std::string("nmcli con delete ") + kJoinCon);
        } else {
            fprintf(stderr, "[net] no NetworkManager; no Wi-Fi Direct\n");
        }
    }

    bool joined() const { return joined_; }

    // shortcut: nmcli blocks up to its own timeouts (scan, 15 s connect), so a quit during a join waits for it; move to NetworkManager's D-Bus API if that matters.
    int join()
    {

        std::string list;

        if (!ok_)
        {
            return 0;
        }

        run("nmcli -t -f SSID device wifi list --rescan yes", &list);

        const bool seen = has_line(list, kSsid);

        if (!seen) {
            fprintf(stderr, "[net] no Wi-Fi Direct game in range\n");
        }

        if (!seen || !still_searching())
        {
            return 0;
        }

        prev_ = active_wifi();

        if (!run_logged(std::string("nmcli con add type wifi ifname '*' autoconnect no con-name ") + kJoinCon + kSecurity))
        {
            return 0;
        }

        joined_ = true;
        fprintf(stderr, "[net] joining the Wi-Fi Direct game\n");

        if (!run_logged(std::string("nmcli --wait 15 con up ") + kJoinCon))
        {
            fprintf(stderr, "[net] Wi-Fi Direct join timed out\n");
            return 0;
        }

        fprintf(stderr, "[net] Wi-Fi Direct linked\n");
        return 2;
    }

    void restore()
    {

        if (!joined_)
        {
            return;
        }

        joined_ = false;
        run(std::string("nmcli con delete ") + kJoinCon);

        if (!prev_.empty()) {
            run("nmcli --wait 0 con up id " + shell_quoted(prev_));
        }

        fprintf(stderr, "[net] left the Wi-Fi Direct game\n");
    }

private:
    // The active Wi-Fi connection's name; nmcli -t escapes ':' and '\' in it.
    static std::string active_wifi()
    {

        constexpr char kType[] = ":802-11-wireless";
        std::string text;
        size_t start = 0;

        run("nmcli -t -f NAME,TYPE con show --active", &text);

        while (start < text.size()) {
            size_t end = text.find('\n', start);

            if (end == std::string::npos) {
                end = text.size();
            }

            const std::string line = text.substr(start, end - start);
            const size_t n = sizeof(kType) - 1;

            start = end + 1;

            if (line.size() > n && line.compare(line.size() - n, n, kType) == 0)
            {
                std::string name;

                for (size_t i = 0; i < line.size() - n; ++i) {
                    if (line[i] == '\\' && i + 1 < line.size() - n) {
                        ++i;
                    }

                    name += line[i];
                }

                return name;
            }
        }

        return {};
    }

    bool ok_ = false;
    bool joined_ = false;
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
